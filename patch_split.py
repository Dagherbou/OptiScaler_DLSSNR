"""Stage two: the split pipeline. Ray Reconstruction as a pure 1:1 denoiser, Neural Rendering at render
resolution, and one enlargement at the end done by a second, internal DLSS Super Resolution feature.

The community guide verified the arrangement end to end and measured it: at a 66% render scale the
denoise gets cheaper (cost tracks output pixels), the model runs on less than half the pixels, and the
one enlargement is done by a temporal upscaler instead of a stretch. Its version had to strip IsHDR from
the second feature and juggle an identity exposure, because its engine tone-mapped before the upscale.
Ours does not: Neural Rendering is applied as a delta, the stream never leaves linear HDR, and both
features keep the flags the game asked for.

Mechanics, all at the NGX seam:

  create   When the game creates its Ray Reconstruction feature, its output size is quietly clamped to
           the render size. OptiScaler builds the same feature it always would -- just 1:1.

  evaluate The game's output resource is swapped for a render-sized scratch; RR denoises into it, the
           model enhances it in place, and the internal SR feature carries it to display size in the
           game's own output resource. The game gets exactly what it asked for.

Restart-bound by nature (the clamp happens at creation), meaningful below native resolution, and off by
default.
"""

import io

ROOT = "C:/Games_Temp/OptiScaler/OptiScaler/"

# --- config -----------------------------------------------------------------------------------------

p = ROOT + "Config.h"
t = io.open(p, encoding="utf-8").read()
old = "    CustomOptional<float> DlssNrEditStability { 0.0f };"
assert old in t
t = t.replace(old, old + """

    // The split pipeline: Ray Reconstruction runs 1:1 as a pure denoiser, the model runs at render
    // resolution, and an internal second Super Resolution feature does the one enlargement. Needs a
    // restart (the clamp happens when the game creates its feature) and a render scale below native to
    // gain anything.
    CustomOptional<bool> DlssNrSplitPipeline { false };""", 1)
io.open(p, "w", encoding="utf-8").write(t)

p = ROOT + "Config.cpp"
t = io.open(p, encoding="utf-8").read()
old = '            DlssNrEditStability.set_from_config(readFloat("DlssNr", "EditStability"));'
assert old in t
t = t.replace(old, old + '\n            DlssNrSplitPipeline.set_from_config(readBool("DlssNr", "SplitPipeline"));', 1)
old = '    ini.SetValue("DlssNr", "EditStability", GetFloatValue(Instance()->DlssNrEditStability.value_for_config()).c_str());'
assert old in t
t = t.replace(old, old + '\n    ini.SetValue("DlssNr", "SplitPipeline", GetBoolValue(Instance()->DlssNrSplitPipeline.value_for_config()).c_str());', 1)
io.open(p, "w", encoding="utf-8").write(t)
print("config added")

# --- our module: a force flag for the in-place pass, and a split-active latch ------------------------

p = ROOT + "dlssnr/DlssNr_Dx12.h"
t = io.open(p, encoding="utf-8").read()
old = "void EvaluateAfterUpscale(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params);"
assert old in t
t = t.replace(old, """void EvaluateAfterUpscale(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params,
                          bool forceInPlace = false);

// The split pipeline runs the model itself; the present-time pass stands down while it is active.
void SetSplitActive(bool active);""", 1)
io.open(p, "w", encoding="utf-8").write(t)

p = ROOT + "dlssnr/DlssNr_Dx12.cpp"
t = io.open(p, encoding="utf-8").read()

old = "void EvaluateAfterUpscale(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params)\n{"
assert old in t
t = t.replace(old, """bool g_splitActive = false;

void SetSplitActive(bool active) { g_splitActive = active; }

void EvaluateAfterUpscale(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params,
                          bool forceInPlace)
{""", 1)

old = """    // When the model runs on the finished frame instead, this call exists only to take a copy of the
    // guides while they are still valid and still describe this frame.
    if (cfg.DlssNrInjectPoint.value_or_default() == INJECT_PRESENT)"""
assert old in t
t = t.replace(old, """    // When the model runs on the finished frame instead, this call exists only to take a copy of the
    // guides while they are still valid and still describe this frame. The split pipeline overrides the
    // choice: it calls for the in-place pass on its own intermediate, whatever the dropdown says.
    if (!forceInPlace && cfg.DlssNrInjectPoint.value_or_default() == INJECT_PRESENT)""", 1)

old = """    if (cfg.DlssNrInjectPoint.value_or_default() != INJECT_PRESENT)
        return;

    // Nothing to work from until the upscaler has run at least once and left its guides behind."""
assert old in t
t = t.replace(old, """    if (cfg.DlssNrInjectPoint.value_or_default() != INJECT_PRESENT)
        return;

    // The split pipeline already ran the model this frame, on its own intermediate.
    if (g_splitActive)
        return;

    // Nothing to work from until the upscaler has run at least once and left its guides behind.""", 1)

io.open(p, "w", encoding="utf-8").write(t)
print("module: force flag and latch added")

# --- the seam: create-time clamp and the evaluate orchestration --------------------------------------

p = ROOT + "inputs/NVNGX_DLSS_Dx12.cpp"
t = io.open(p, encoding="utf-8").read()

t = t.replace('#include "dlssnr/DlssNr_Dx12.h"',
              '#include "dlssnr/DlssNr_Dx12.h"\n#include <upscalers/dlss/DLSSFeature_Dx12.h>', 1)

SPLIT = '''
// --- The split pipeline -----------------------------------------------------------------------------
//
// Ray Reconstruction as a pure 1:1 denoiser, Neural Rendering at render resolution, and one enlargement
// at the end done by an internal DLSS Super Resolution feature. The stream stays linear HDR throughout,
// so both features keep the flags the game asked for -- no exposure tricks, no colour-space compromise.

struct SplitState
{
    unsigned int displayWidth = 0;  // what the game originally asked its RR feature to output
    unsigned int displayHeight = 0;
    ID3D12Resource* intermediate = nullptr; // render-sized: denoised, then enhanced
    std::unique_ptr<IFeature_Dx12> sr;      // the one enlargement
    bool armed = false;                     // the create-time clamp happened
    bool failed = false;
};

static SplitState SplitDx12;

static bool SplitWanted()
{
    const Config& cfg = *Config::Instance();
    return cfg.DlssNrEnabled.value_or_default() && cfg.DlssNrSplitPipeline.value_or_default() &&
           !SplitDx12.failed;
}

// Clamps the game's Ray Reconstruction feature to 1:1 at creation. OptiScaler then builds the same
// feature it always would -- just as a pure denoiser.
static void SplitOnCreate(NVSDK_NGX_Feature featureId, NVSDK_NGX_Parameter* params)
{
    if (featureId != NVSDK_NGX_Feature_RayReconstruction || !SplitWanted() || params == nullptr)
        return;

    unsigned int w = 0, h = 0, ow = 0, oh = 0;
    params->Get(NVSDK_NGX_Parameter_Width, &w);
    params->Get(NVSDK_NGX_Parameter_Height, &h);
    params->Get(NVSDK_NGX_Parameter_OutWidth, &ow);
    params->Get(NVSDK_NGX_Parameter_OutHeight, &oh);

    if (w == 0 || h == 0 || ow <= w || oh <= h)
    {
        LOG_INFO("DLSS-NR split: nothing to split ({}x{} -> {}x{}); running conventionally", w, h, ow, oh);
        return;
    }

    SplitDx12.displayWidth = ow;
    SplitDx12.displayHeight = oh;
    params->Set(NVSDK_NGX_Parameter_OutWidth, w);
    params->Set(NVSDK_NGX_Parameter_OutHeight, h);
    SplitDx12.armed = true;

    LOG_INFO("DLSS-NR split: Ray Reconstruction clamped to {}x{} 1:1; display {}x{} will be reached by "
             "an internal Super Resolution feature",
             w, h, ow, oh);
}

static ID3D12Resource* SplitScratch(ID3D12Device* device, DXGI_FORMAT format, unsigned int w,
                                    unsigned int h)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ID3D12Resource* res = nullptr;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&res));
    return res;
}

// The per-frame orchestration. Returns true when it handled the evaluate, with the result in outResult.
static bool SplitEvaluateRR(ID3D12GraphicsCommandList* cmdList, const NVSDK_NGX_Handle* handle,
                            NVSDK_NGX_Parameter* params, PFN_NVSDK_NGX_ProgressCallback callback,
                            NVSDK_NGX_Result* outResult)
{
    if (!SplitDx12.armed || !SplitWanted())
        return false;

    ID3D12Resource* gameOutput = nullptr;
    ID3D12Resource* gameColor = nullptr;
    params->Get(NVSDK_NGX_Parameter_Output, &gameOutput);
    params->Get(NVSDK_NGX_Parameter_Color, &gameColor);

    unsigned int w = 0, h = 0;
    params->Get(NVSDK_NGX_Parameter_Width, &w);
    params->Get(NVSDK_NGX_Parameter_Height, &h);

    if (gameOutput == nullptr || w == 0 || h == 0 || D3D12Device == nullptr)
        return false;

    if (SplitDx12.intermediate == nullptr)
    {
        // Same family as the game's own output, so nothing downstream has to convert.
        SplitDx12.intermediate =
            SplitScratch(D3D12Device, codec::TypedFormat(gameOutput->GetDesc().Format), w, h);

        if (SplitDx12.intermediate == nullptr)
        {
            LOG_ERROR("DLSS-NR split: the intermediate could not be created; falling back");
            SplitDx12.failed = true;
            DlssNr::SetSplitActive(false);
            return false;
        }
    }

    // 1. Ray Reconstruction denoises 1:1 into the intermediate.
    params->Set(NVSDK_NGX_Parameter_Output, SplitDx12.intermediate);
    const NVSDK_NGX_Result rrResult = TryEvaluateOptiFeature(cmdList, handle, params, callback);

    if (rrResult != NVSDK_NGX_Result_Success)
    {
        params->Set(NVSDK_NGX_Parameter_Output, gameOutput);
        *outResult = rrResult;
        return true;
    }

    // 2. The model enhances the denoised frame in place, at render resolution, as a delta -- the
    //    intermediate stays linear HDR.
    DlssNr::SetSplitActive(true);
    DlssNr::EvaluateAfterUpscale(cmdList, params, true);

    // 3. The one enlargement: an internal Super Resolution feature, created with the game's own flags
    //    -- IsHDR included, since the stream never left linear HDR.
    if (SplitDx12.sr == nullptr)
    {
        params->Set(NVSDK_NGX_Parameter_OutWidth, SplitDx12.displayWidth);
        params->Set(NVSDK_NGX_Parameter_OutHeight, SplitDx12.displayHeight);

        auto sr = std::make_unique<DLSSFeatureDx12>(IFeature::GetNextHandleId(), params);

        if (!sr->Init(D3D12Device, cmdList, params) || !sr->IsInited())
        {
            LOG_ERROR("DLSS-NR split: the internal Super Resolution feature would not initialise; "
                      "falling back");
            SplitDx12.failed = true;
            DlssNr::SetSplitActive(false);
            params->Set(NVSDK_NGX_Parameter_Output, gameOutput);
            params->Set(NVSDK_NGX_Parameter_OutWidth, w);
            params->Set(NVSDK_NGX_Parameter_OutHeight, h);
            *outResult = rrResult;
            return true;
        }

        SplitDx12.sr = std::move(sr);
        LOG_INFO("DLSS-NR split: internal Super Resolution running {}x{} -> {}x{}", w, h,
                 SplitDx12.displayWidth, SplitDx12.displayHeight);
    }

    params->Set(NVSDK_NGX_Parameter_Color, SplitDx12.intermediate);
    params->Set(NVSDK_NGX_Parameter_Output, gameOutput);
    params->Set(NVSDK_NGX_Parameter_OutWidth, SplitDx12.displayWidth);
    params->Set(NVSDK_NGX_Parameter_OutHeight, SplitDx12.displayHeight);

    const bool srOk = SplitDx12.sr->Evaluate(cmdList, params);

    // The block goes back the way the game filled it.
    if (gameColor != nullptr)
        params->Set(NVSDK_NGX_Parameter_Color, gameColor);

    params->Set(NVSDK_NGX_Parameter_Output, gameOutput);

    if (!srOk)
    {
        LOG_ERROR("DLSS-NR split: the internal Super Resolution feature failed; falling back");
        SplitDx12.failed = true;
        DlssNr::SetSplitActive(false);
    }

    *outResult = srOk ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_Fail;
    return true;
}

'''

anchor = "static NVSDK_NGX_Result TryCreateOptiFeature(ID3D12GraphicsCommandList* InCmdList, NVSDK_NGX_Feature InFeatureID,"
assert anchor in t
# The split orchestration needs TryEvaluateOptiFeature, which is defined later; declare it first.
decl = """static NVSDK_NGX_Result TryEvaluateOptiFeature(ID3D12GraphicsCommandList* InCmdList,
                                               const NVSDK_NGX_Handle* InFeatureHandle,
                                               NVSDK_NGX_Parameter* InParameters,
                                               PFN_NVSDK_NGX_ProgressCallback InCallback);

"""
t = t.replace(anchor, decl + SPLIT + anchor, 1)

# Create seam: clamp before, done inside NVSDK_NGX_D3D12_CreateFeature just ahead of TryCreateOptiFeature.
old = """    auto tryResult = TryCreateOptiFeature(InCmdList, InFeatureID, InParameters, OutHandle);

    if (tryResult == NVSDK_NGX_Result_Success)
        HandleToFeature[(*OutHandle)->Id] = InFeatureID;"""
assert old in t
new = """    SplitOnCreate(InFeatureID, InParameters);

    auto tryResult = TryCreateOptiFeature(InCmdList, InFeatureID, InParameters, OutHandle);

    if (tryResult == NVSDK_NGX_Result_Success)
        HandleToFeature[(*OutHandle)->Id] = InFeatureID;"""
t = t.replace(old, new, 1)

# Evaluate seam: the split takes the whole call when active.
old = """    // OptiScaler internal handling
    const NVSDK_NGX_Result optiResult = TryEvaluateOptiFeature(InCmdList, InFeatureHandle, InParameters, InCallback);"""
assert old in t
new = """    // The split pipeline: denoise 1:1, enhance at render resolution, enlarge once.
    if (feature == NVSDK_NGX_Feature_RayReconstruction)
    {
        NVSDK_NGX_Result splitResult = NVSDK_NGX_Result_Success;

        if (SplitEvaluateRR(InCmdList, InFeatureHandle, InParameters, InCallback, &splitResult))
            return splitResult;
    }

    // OptiScaler internal handling
    const NVSDK_NGX_Result optiResult = TryEvaluateOptiFeature(InCmdList, InFeatureHandle, InParameters, InCallback);"""
t = t.replace(old, new, 1)

io.open(p, "w", encoding="utf-8").write(t)
print("seam patched")

# --- menu -------------------------------------------------------------------------------------------

NL = chr(92) + "n"
p = ROOT + "menu/menu_common.cpp"
t = io.open(p, encoding="utf-8").read()
old = '''        ImGui::SeparatorText("Cost");'''
assert old in t
new = ('''        bool split = config->DlssNrSplitPipeline.value_or_default();
        if (ImGui::Checkbox("Split pipeline: RR 1:1 + NR + internal SR (restart)", &split))
            config->DlssNrSplitPipeline = split;

        ShowHelpMarker("Ray Reconstruction runs 1:1 as a pure denoiser, the model runs at render"
                       "@resolution, and one enlargement at the end is done by an internal DLSS Super"
                       "@Resolution feature -- a temporal upscaler with the full G-buffer, not a"
                       "@stretch. Everything stays linear HDR, so both features keep the game's own"
                       "@flags."
                       "@@Denoise gets cheaper, the model runs on fewer pixels, and its detail rides"
                       "@through the upscaler's accumulation. The published measurements at a 66%"
                       "@render scale: about 4.5 + 6 + 3.5 ms against roughly 17 conventionally."
                       "@@Ray Reconstruction titles only, needs a render scale below native (Quality or"
                       "@lower -- at DLAA there is nothing to enlarge), and takes effect on restart:"
                       "@the 1:1 clamp happens when the game creates its feature. Falls back to the"
                       "@conventional path, with a line in the log, if any stage refuses.");

        ImGui::SeparatorText("Cost");''').replace("@", NL)
t = t.replace(old, new, 1)
io.open(p, "w", encoding="utf-8").write(t)
print("menu added")
