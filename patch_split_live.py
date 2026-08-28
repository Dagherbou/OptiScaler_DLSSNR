"""Makes the split pipeline a live toggle, both directions.

OptiScaler already recreates features in place -- it is how backend switching works -- so the restart
requirement was never structural. Flipping the toggle now re-creates the game's Ray Reconstruction
feature at the other geometry (1:1 or the original render-to-display) through that same machinery, with
a brief hitch and nothing else.

The split no longer trusts a flag to know whether it is on. It operates only when the feature is
observably 1:1 and no recreation is in flight, so every transition frame falls through to the
conventional path automatically, and a recreation that fails leaves the conventional path running
rather than a half-armed split.

Our own resources cannot be freed the moment the toggle goes off -- command lists submitted over the
last frames still reference them -- so they are parked and released a number of evaluates later.
"""

import io

ROOT = "C:/Games_Temp/OptiScaler/OptiScaler/"

p = ROOT + "inputs/NVNGX_DLSS_Dx12.cpp"
t = io.open(p, encoding="utf-8").read()

# --- state gains the parking lot --------------------------------------------------------------------

old = """struct SplitState
{
    unsigned int displayWidth = 0;  // what the game originally asked its RR feature to output
    unsigned int displayHeight = 0;
    ID3D12Resource* intermediate = nullptr; // render-sized: denoised, then enhanced
    std::unique_ptr<IFeature_Dx12> sr;      // the one enlargement
    bool armed = false;                     // the create-time clamp happened
    bool failed = false;
};

static SplitState SplitDx12;"""
assert old in t
new = """struct SplitState
{
    unsigned int displayWidth = 0;  // what the game originally asked its RR feature to output
    unsigned int displayHeight = 0;
    ID3D12Resource* intermediate = nullptr; // render-sized: denoised, then enhanced
    std::unique_ptr<IFeature_Dx12> sr;      // the one enlargement
    bool armed = false;                     // the create-time clamp happened
    bool failed = false;

    // Retired on a live switch-off: still referenced by command lists submitted over the last frames,
    // so they are released a number of evaluates later rather than on the spot.
    ID3D12Resource* parkedIntermediate = nullptr;
    std::unique_ptr<IFeature_Dx12> parkedSr;
    int parkedCountdown = 0;
};

static SplitState SplitDx12;

// Frees what a live switch-off parked, once enough evaluates have passed that nothing in flight can
// still reference it.
static void SplitTickParked()
{
    if (SplitDx12.parkedCountdown <= 0)
        return;

    if (--SplitDx12.parkedCountdown > 0)
        return;

    if (SplitDx12.parkedIntermediate != nullptr)
    {
        SplitDx12.parkedIntermediate->Release();
        SplitDx12.parkedIntermediate = nullptr;
    }

    SplitDx12.parkedSr.reset();
}

// Applies the toggle while the game runs. OptiScaler recreates features in place for backend switching;
// the same machinery re-creates Ray Reconstruction at the other geometry. The split itself never trusts
// this function: it operates only when the feature is observably 1:1, so every transition frame simply
// falls through to the conventional path.
static void SplitManageTransition(uint32_t handleId, NVSDK_NGX_Parameter* params)
{
    SplitTickParked();

    auto it = Dx12Contexts.find(handleId);

    if (it == Dx12Contexts.end() || it->second.feature == nullptr)
        return;

    IFeature_Dx12* f = it->second.feature.get();
    const bool oneToOne = f->TargetWidth() == f->RenderWidth() && f->TargetHeight() == f->RenderHeight();
    const bool want = SplitWanted();
    State& state = State::Instance();

    // A recreation is mid-flight: keep the block's output size aimed at where it is going, in case the
    // game rewrites it every frame, and otherwise stay out of the way.
    if (it->second.changeBackendCounter != 0)
    {
        if (want && SplitDx12.displayWidth != 0)
        {
            unsigned int w = 0, h = 0;
            params->Get(NVSDK_NGX_Parameter_Width, &w);
            params->Get(NVSDK_NGX_Parameter_Height, &h);

            if (w != 0)
            {
                params->Set(NVSDK_NGX_Parameter_OutWidth, w);
                params->Set(NVSDK_NGX_Parameter_OutHeight, h);
            }
        }
        else if (!want && SplitDx12.displayWidth != 0)
        {
            params->Set(NVSDK_NGX_Parameter_OutWidth, SplitDx12.displayWidth);
            params->Set(NVSDK_NGX_Parameter_OutHeight, SplitDx12.displayHeight);
        }

        return;
    }

    if (want && !oneToOne)
    {
        unsigned int w = 0, h = 0, ow = 0, oh = 0;
        params->Get(NVSDK_NGX_Parameter_Width, &w);
        params->Get(NVSDK_NGX_Parameter_Height, &h);
        params->Get(NVSDK_NGX_Parameter_OutWidth, &ow);
        params->Get(NVSDK_NGX_Parameter_OutHeight, &oh);

        if (w == 0 || ow <= w)
        {
            static bool saidNothing = false;

            if (!saidNothing)
            {
                saidNothing = true;
                LOG_INFO("DLSS-NR split: nothing to split at {}x{} -> {}x{}; needs a render scale below "
                         "native",
                         w, h, ow, oh);
            }

            return;
        }

        SplitDx12.displayWidth = ow;
        SplitDx12.displayHeight = oh;
        params->Set(NVSDK_NGX_Parameter_OutWidth, w);
        params->Set(NVSDK_NGX_Parameter_OutHeight, h);
        state.newBackend = Upscaler::DLSSD;
        state.changeBackend[handleId] = true;

        LOG_INFO("DLSS-NR split: re-creating Ray Reconstruction 1:1 at {}x{} in place", w, h);
        return;
    }

    if (!want && oneToOne && SplitDx12.displayWidth != 0)
    {
        params->Set(NVSDK_NGX_Parameter_OutWidth, SplitDx12.displayWidth);
        params->Set(NVSDK_NGX_Parameter_OutHeight, SplitDx12.displayHeight);
        state.newBackend = Upscaler::DLSSD;
        state.changeBackend[handleId] = true;

        // Ours go to the parking lot, not the floor: lists submitted over the last frames still
        // reference them.
        if (SplitDx12.intermediate != nullptr || SplitDx12.sr != nullptr)
        {
            SplitDx12.parkedIntermediate = SplitDx12.intermediate;
            SplitDx12.intermediate = nullptr;
            SplitDx12.parkedSr = std::move(SplitDx12.sr);
            SplitDx12.parkedCountdown = 16;
        }

        DlssNr::SetSplitActive(false);

        LOG_INFO("DLSS-NR split: returning Ray Reconstruction to {}x{} -> {}x{} in place",
                 f->RenderWidth(), f->RenderHeight(), SplitDx12.displayWidth, SplitDx12.displayHeight);
        return;
    }
}"""
t = t.replace(old, new, 1)

# --- the operate gate becomes observational ---------------------------------------------------------

old = """    if (!SplitDx12.armed || !SplitWanted())
        return false;

    ID3D12Resource* gameOutput = nullptr;"""
assert old in t
new = """    if (!SplitWanted() || SplitDx12.displayWidth == 0)
        return false;

    // Observable state only: operate when the feature really is 1:1 and no recreation is in flight.
    // Every transition frame falls through to the conventional path by construction.
    {
        auto it = Dx12Contexts.find(handle->Id);

        if (it == Dx12Contexts.end() || it->second.feature == nullptr ||
            it->second.changeBackendCounter != 0)
            return false;

        IFeature_Dx12* f = it->second.feature.get();

        if (f->TargetWidth() != f->RenderWidth() || f->TargetHeight() != f->RenderHeight())
            return false;
    }

    ID3D12Resource* gameOutput = nullptr;"""
t = t.replace(old, new, 1)

# --- the transition manager runs every RR evaluate ---------------------------------------------------

old = """    // The split pipeline: denoise 1:1, enhance at render resolution, enlarge once.
    if (feature == NVSDK_NGX_Feature_RayReconstruction)
    {
        NVSDK_NGX_Result splitResult = NVSDK_NGX_Result_Success;"""
assert old in t
new = """    // The split pipeline: denoise 1:1, enhance at render resolution, enlarge once. The toggle applies
    // live -- the feature is re-created in place at the other geometry.
    if (feature == NVSDK_NGX_Feature_RayReconstruction)
    {
        SplitManageTransition(handleId, InParameters);

        NVSDK_NGX_Result splitResult = NVSDK_NGX_Result_Success;"""
t = t.replace(old, new, 1)

io.open(p, "w", encoding="utf-8").write(t)
print("seam: live transitions")

# --- menu and config wording ------------------------------------------------------------------------

NL = chr(92) + "n"
p = ROOT + "menu/menu_common.cpp"
t = io.open(p, encoding="utf-8").read()
old = '''        if (ImGui::Checkbox("Split pipeline: RR 1:1 + NR + internal SR (restart)", &split))'''
assert old in t
t = t.replace(old, '''        if (ImGui::Checkbox("Split pipeline: RR 1:1 + NR + internal SR", &split))''', 1)

old = ('''                       "@@Ray Reconstruction titles only, needs a render scale below native (Quality or"
                       "@lower -- at DLAA there is nothing to enlarge), and takes effect on restart:"
                       "@the 1:1 clamp happens when the game creates its feature. Falls back to the"
                       "@conventional path, with a line in the log, if any stage refuses.");''').replace("@", NL)
assert old in t
new = ('''                       "@@Ray Reconstruction titles only, and needs a render scale below native (Quality"
                       "@or lower -- at DLAA there is nothing to enlarge). Applies live, both ways: the"
                       "@feature is re-created in place, which costs a brief hitch. Falls back to the"
                       "@conventional path, with a line in the log, if any stage refuses.");''').replace("@", NL)
t = t.replace(old, new, 1)
io.open(p, "w", encoding="utf-8").write(t)
print("menu updated")

p = ROOT + "Config.h"
t = io.open(p, encoding="utf-8").read()
old = """    // The split pipeline: Ray Reconstruction runs 1:1 as a pure denoiser, the model runs at render
    // resolution, and an internal second Super Resolution feature does the one enlargement. Needs a
    // restart (the clamp happens when the game creates its feature) and a render scale below native to
    // gain anything.
    CustomOptional<bool> DlssNrSplitPipeline { false };"""
assert old in t
new = """    // The split pipeline: Ray Reconstruction runs 1:1 as a pure denoiser, the model runs at render
    // resolution, and an internal second Super Resolution feature does the one enlargement. Applies
    // live -- the feature is re-created in place -- and needs a render scale below native to gain
    // anything.
    CustomOptional<bool> DlssNrSplitPipeline { false };"""
t = t.replace(old, new, 1)
io.open(p, "w", encoding="utf-8").write(t)
print("config comment updated")
