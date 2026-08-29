#include "pch.h"

#include "DlssNr.h"

#if OPTI_DLSSNR

#include "DlssNr_Codec.h"

#include <Config.h>
#include <State.h>
#include <Util.h>

#include <upscalers/dlss/DLSSFeature_Dx12.h>
#include <shaders/output_scaling/OS_Dx12.h>

#include <memory>
#include <vector>

namespace DlssNr::Split
{

// The device the seam was created on, handed over on every entry.
static ID3D12Device* g_splitDevice = nullptr;

// --- The split pipeline -----------------------------------------------------------------------------
//
// Ray Reconstruction as a pure denoiser, Neural Rendering at a controllable resolution, and one
// enlargement at the end. The stream stays linear HDR throughout, so every feature keeps the flags the
// game asked for.
//
// Supersampling is not a switch of its own: the split reads the Output Scaling Ratio the user already
// tuned, and supersamples its enlargement whenever that ratio is above one. Output Scaling's own Enable
// must stay off -- it owns the same feature geometry and the two cannot both steer it.
//
// Two arrangements, chosen by one checkbox:
//
//   RR 1:1 (default)   RR denoises at render size, the model runs there too, and an internal Super
//                      Resolution feature does the enlargement -- to the supersampled size when the
//                      ratio asks, with OptiScaler's own downscaler carrying it back. Cheapest: the
//                      expensive models both run at their smallest size.
//
//   RR included        RR itself upscales to the supersampled size, the model works on that image (its
//                      cost governed by the Model resolution dropdown), and only the downscale remains.
//                      The conventional Output Scaling look with the model in the chain -- and RR's
//                      cost rising with the square of the ratio, which is the price of it.

struct SplitState
{
    unsigned int displayWidth = 0;  // what the game originally asked its RR feature to output
    unsigned int displayHeight = 0;
    ID3D12Resource* intermediate = nullptr; // render-sized: denoised, then enhanced (RR 1:1 mode)
    ID3D12Resource* oversized = nullptr;    // above display size: the supersampled working image
    std::unique_ptr<IFeature_Dx12> sr;      // the enlargement (RR 1:1 mode only)
    std::unique_ptr<OS_Dx12> downscaler;    // oversized -> display, OptiScaler's own filtering
    int downscalerKind = -1;                // the Downscaler choice it was built with

    // The enlargement was created this frame: its NGX creation commands are recorded in the game's
    // command list but not yet executed, and NGX requires them executed before the first evaluate.
    // Evaluating in the same list is a dice-roll that sometimes deadlocks the GPU (both session
    // crashes died on exactly the creation frame). Skip one frame instead, like OptiScaler's own
    // recreation counter does.
    bool srJustCreated = false;

    // The enlargement is created on this private queue, executed and fenced to completion before
    // anything else happens. Recording NGX's creation into the game's already-loaded command list --
    // even without evaluating -- was the remaining dice-roll: three sessions died on exactly that
    // frame. A few milliseconds of synchronous wait, once per creation, inside a hitch that exists
    // anyway.
    ID3D12CommandQueue* createQueue = nullptr;
    ID3D12CommandAllocator* createAlloc = nullptr;
    ID3D12GraphicsCommandList* createList = nullptr;
    ID3D12Fence* createFence = nullptr;
    UINT64 createFenceValue = 0;
    HANDLE createEvent = nullptr;
    unsigned int srTargetWidth = 0;         // what the enlargement was built to produce
    int srBuiltPreset = -1;                 // the render preset it was built with
    bool failed = false;


    // Geometry-change control, so nothing can ever loop recreations. The pair doubles as the steering
    // target while a recreation is mid-flight and the feature does not exist to ask.
    unsigned int lastDesiredWidth = 0;
    unsigned int lastDesiredHeight = 0;
    int armTries = 0;

    // The split only ever restores geometry it changed itself, exactly once. Without this, a feature
    // legitimately resized by something else -- conventional Output Scaling above all -- read as
    // "wrong" forever, and the restore fought it in an endless recreation loop that shredded the frame.
    bool geometryOwned = false;
    bool restorePending = false;

    // Rapid toggling must not thrash recreations: the wanted-state has to hold still briefly before the
    // machinery moves. Steering of in-flight transitions is unaffected.
    bool lastWant = false;
    int stableFrames = 0;

    // The game's own quality-mode declaration, captured with the display size. A feature built at a
    // ratio that contradicts the block's declared quality mode is created fine and then refused at
    // evaluate (0xbad00000) -- so every geometry the split asks for carries a matching declaration,
    // and the game's own is restored with its geometry.
    unsigned int origPerfQuality = 0xffffffff;

    // The driver refused the supersampled enlargement once this session: run at display size instead
    // of latching the whole split off. Cleared on a toggle edge or Retry.
    bool supersampleRefused = false;
};

static SplitState SplitDx12;

static void SplitClearFailure()
{
    SplitDx12.failed = false;
    SplitDx12.armTries = 0;
    SplitDx12.supersampleRefused = false;
    DlssNr::SetSplitStatus("");
}

static const bool g_splitRetryRegistered = [] {
    DlssNr::g_splitRetryHook = &SplitClearFailure;
    return true;
}();

// Retired on a live change: still referenced by command lists submitted over the last frames, so each
// entry is released a number of evaluates later. A list, so rapid changes queue rather than forcing an
// early free -- releasing under the GPU is the mistake this project has paid for repeatedly.
struct SplitRetired
{
    ID3D12Resource* resource = nullptr;
    std::unique_ptr<IFeature_Dx12> feature;
    std::unique_ptr<OS_Dx12> shader;
    int framesLeft = 32;
};

static std::vector<SplitRetired> SplitParkedList;

static void SplitParkResource(ID3D12Resource*& res)
{
    if (res == nullptr)
        return;

    SplitRetired r;
    r.resource = res;
    res = nullptr;
    SplitParkedList.push_back(std::move(r));
}

static bool SplitWanted()
{
    const Config& cfg = *Config::Instance();
    return cfg.DlssNrEnabled.value_or_default() && cfg.DlssNrSplitPipeline.value_or_default() &&
           !SplitDx12.failed;
}

// Whether the user wants supersampling: Output Scaling's own Enable, as saved -- the runtime value is
// forced off while the split runs, because the split does the supersampling itself.
static bool SplitOsIntent()
{
    return Config::Instance()->OutputScalingEnabled.value_for_config().value_or(false);
}

// The split absorbs Output Scaling while it runs: the runtime flag goes off, so no feature geometry can
// be steered by two owners, and the split supersamples at the Ratio itself. Given back the moment the
// split stands down, so conventional Output Scaling resumes.
static void SplitAbsorbOs()
{
    if (SplitOsIntent() && Config::Instance()->OutputScalingEnabled.value_or_default())
    {
        Config::Instance()->OutputScalingEnabled.set_volatile_value(false);
        LOG_INFO("DLSS-NR split: absorbing Output Scaling -- the split supersamples in its place");
    }
}

static void SplitRestoreOs()
{
    if (SplitOsIntent() && !Config::Instance()->OutputScalingEnabled.value_or_default())
    {
        Config::Instance()->OutputScalingEnabled.set_volatile_value(true);
        LOG_INFO("DLSS-NR split: returning Output Scaling to its own machinery");
    }
}

// The supersample ratio in force: the Output Scaling Ratio, when the user has Output Scaling on.
static float SplitRatio()
{
    if (!SplitOsIntent() || SplitDx12.supersampleRefused)
        return 1.0f;

    float mult = Config::Instance()->OutputScalingMultiplier.value_or_default();

    if (mult > 3.0f)
        mult = 3.0f;

    return mult > 1.05f ? mult : 1.0f;
}

// The quality mode that honestly describes an upscale from renderW to targetW. The thresholds sit
// between the modes' nominal ratios (Quality 1.5x, Balanced 1.72x, Performance 2x, Ultra
// Performance 3x).
static unsigned int SplitPerfQuality(unsigned int renderW, unsigned int targetW)
{
    // A feature that does not upscale must say so: a 1:1 enlargement declared as an upscale mode is
    // created fine and then dies in the driver (the RE Engine crash, mechanically).
    if (targetW <= renderW + renderW / 20)
        return NVSDK_NGX_PerfQuality_Value_DLAA;

    const float r = renderW == 0 ? 1.0f : (float) targetW / (float) renderW;

    if (r >= 2.5f)
        return NVSDK_NGX_PerfQuality_Value_UltraPerformance;

    if (r >= 1.85f)
        return NVSDK_NGX_PerfQuality_Value_MaxPerf;

    if (r >= 1.6f)
        return NVSDK_NGX_PerfQuality_Value_Balanced;

    return NVSDK_NGX_PerfQuality_Value_MaxQuality;
}

// What the Ray Reconstruction feature's output size should be under the current settings.
static void SplitDesiredTarget(unsigned int renderW, unsigned int renderH, unsigned int* outW,
                               unsigned int* outH)
{
    const float mult = SplitRatio();

    if (Config::Instance()->DlssNrSplitIncludeRR.value_or_default() && mult > 1.0f &&
        SplitDx12.displayWidth != 0)
    {
        // Include-RR can run at its own ratio: most of the reconstruction sharpness arrives well
        // below the full Output Scaling ratio, and RR's cost rises with the ratio squared.
        float rrMult = Config::Instance()->DlssNrSplitIncludeRRRatio.value_or_default();
        rrMult = rrMult > 1.05f ? (rrMult > 3.0f ? 3.0f : rrMult) : mult;

        *outW = (unsigned int) (SplitDx12.displayWidth * rrMult + 0.5f);
        *outH = (unsigned int) (SplitDx12.displayHeight * rrMult + 0.5f);
        return;
    }

    *outW = renderW;
    *outH = renderH;
}

// Frees what live changes parked, entry by entry, once enough evaluates have passed that nothing in
// flight can still reference each one.
static void SplitTickParked()
{
    for (size_t i = 0; i < SplitParkedList.size();)
    {
        if (--SplitParkedList[i].framesLeft > 0)
        {
            ++i;
            continue;
        }

        if (SplitParkedList[i].resource != nullptr)
            SplitParkedList[i].resource->Release();

        SplitParkedList.erase(SplitParkedList.begin() + i);
    }
}

// Parks the enlargement stage for deferred release.
static void SplitParkEnlargement()
{
    if (SplitDx12.sr != nullptr)
    {
        SplitRetired r;
        r.feature = std::move(SplitDx12.sr);
        SplitParkedList.push_back(std::move(r));
    }

    if (SplitDx12.downscaler != nullptr)
    {
        SplitRetired r;
        r.shader = std::move(SplitDx12.downscaler);
        SplitParkedList.push_back(std::move(r));
    }

    SplitParkResource(SplitDx12.oversized);
    SplitDx12.srTargetWidth = 0;
}

// Applies the toggles while the game runs, by re-creating the Ray Reconstruction feature at whatever
// geometry the settings currently call for. The split itself never trusts this function: it operates
// only when the feature's observed geometry matches the desired one, so every transition frame falls
// through to the conventional path.
void ManageTransition(uint32_t handleId, NVSDK_NGX_Parameter* params, const FeatureView& view,
                      ID3D12Device* device)
{
    g_splitDevice = device;
    SplitTickParked();

    if (!view.found)
        return;

    const bool want = SplitWanted();
    State& state = State::Instance();

    if (want)
        SplitAbsorbOs();
    else
        SplitRestoreOs();

    // The moment the split stops serving -- unchecked, model disabled, or failed -- the model's normal
    // inject point must see that. The native-1:1 serve never disarms, so without this the flag it
    // raised stayed up forever and the finished-frame path was muted for the rest of the session.
    if (!want)
    {
        DlssNr::SetSplitActive(false);

        if (SplitDx12.lastWant)
            DlssNr::SetSplitStatus("");
    }

    // A recreation is mid-flight -- the old feature may already be destroyed, and ChangeFeature's first
    // phase stamps the block's output size back to the old feature's. Keep the block aimed at the
    // destination every frame until the new feature exists, or the parse reads the stamped size and the
    // recreation reproduces exactly what it was meant to replace.
    if (view.changeBackendCounter != 0 || view.feature == nullptr)
    {
        // Steer only transitions that are ours: the split's own arm, or its one restore. Anything else
        // in flight -- Output Scaling's recreations included -- is none of our business.
        if (want && SplitDx12.geometryOwned && SplitDx12.lastDesiredWidth != 0)
        {
            params->Set(NVSDK_NGX_Parameter_OutWidth, SplitDx12.lastDesiredWidth);
            params->Set(NVSDK_NGX_Parameter_OutHeight, SplitDx12.lastDesiredHeight);

            unsigned int rw = 0;
            params->Get(NVSDK_NGX_Parameter_Width, &rw);

            if (rw != 0 && SplitDx12.displayWidth != 0 &&
                SplitDx12.lastDesiredWidth > SplitDx12.displayWidth)
                params->Set(NVSDK_NGX_Parameter_PerfQualityValue,
                            SplitPerfQuality(rw, SplitDx12.lastDesiredWidth));
        }
        else if (SplitDx12.restorePending && SplitDx12.displayWidth != 0)
        {
            params->Set(NVSDK_NGX_Parameter_OutWidth, SplitDx12.displayWidth);
            params->Set(NVSDK_NGX_Parameter_OutHeight, SplitDx12.displayHeight);

            if (SplitDx12.origPerfQuality != 0xffffffff)
                params->Set(NVSDK_NGX_Parameter_PerfQualityValue, SplitDx12.origPerfQuality);
        }

        return;
    }

    // A completed transition ends any pending restore, whatever geometry resulted -- one attempt only.
    SplitDx12.restorePending = false;

    // Debounce: act on a changed wanted-state only once it has held still. A fast enable/disable run
    // otherwise burns the whole retry budget on transitions that were each individually succeeding.
    if (want != SplitDx12.lastWant)
    {
        SplitDx12.lastWant = want;
        SplitDx12.stableFrames = 0;
        SplitDx12.supersampleRefused = false;
        return;
    }

    ++SplitDx12.stableFrames;

    IFeature_Dx12* f = view.feature;

    unsigned int w = 0, h = 0, ow = 0, oh = 0;
    params->Get(NVSDK_NGX_Parameter_Width, &w);
    params->Get(NVSDK_NGX_Parameter_Height, &h);
    params->Get(NVSDK_NGX_Parameter_OutWidth, &ow);
    params->Get(NVSDK_NGX_Parameter_OutHeight, &oh);

    // The display size the game originally asked for, captured exactly once. This block is also
    // written by us -- the internal SR's oversized target goes through it -- and treating later, larger
    // values as the game's own compounded the supersample every frame until the device hung.
    if (SplitDx12.displayWidth == 0 && w != 0 && ow > w)
    {
        SplitDx12.displayWidth = ow;
        SplitDx12.displayHeight = oh;
        params->Get(NVSDK_NGX_Parameter_PerfQualityValue, &SplitDx12.origPerfQuality);
    }

    unsigned int desiredW = 0, desiredH = 0;
    SplitDesiredTarget(f->RenderWidth(), f->RenderHeight(), &desiredW, &desiredH);

    const bool matches = f->TargetWidth() == desiredW && f->TargetHeight() == desiredH;

    // A different geometry is now desired: the retry budget starts over.
    if (desiredW != SplitDx12.lastDesiredWidth || desiredH != SplitDx12.lastDesiredHeight)
    {
        SplitDx12.lastDesiredWidth = desiredW;
        SplitDx12.lastDesiredHeight = desiredH;
        SplitDx12.armTries = 0;
    }

    // The feature is where the settings want it: the retry budget is refunded, so only consecutive
    // failures ever exhaust it.
    if (want && matches)
        SplitDx12.armTries = 0;

    const bool settled = SplitDx12.stableFrames >= 30;

    // A Ray Reconstruction feature that does not upscale (RE Engine runs RR 1:1 and upscales in a
    // separate Super Resolution feature) already IS the split arrangement -- rearranging it built a
    // pointless 1:1 enlargement against the game's real output and crashed. Nothing to do here.
    // A natively 1:1 feature is not rearranged -- but it IS served: the split's evaluate runs the
    // model on it and adds the internal enlargement, which is the real accumulator the edit needs.
    if (want && f->TargetWidth() <= f->RenderWidth() + f->RenderWidth() / 20 && !SplitDx12.geometryOwned)
        return;

    if (want && !matches && settled)
    {
        // If recreations complete and the feature still does not match, something else owns its
        // geometry. Give up loudly rather than re-creating every frame, which is a device-killing storm.
        if (SplitDx12.armTries >= 3)
        {
            if (!SplitDx12.failed)
            {
                SplitDx12.failed = true;
                DlssNr::SetSplitStatus("failed: the feature will not hold the requested size (see the log)");
                LOG_ERROR("DLSS-NR split: three recreations did not reach {}x{}; giving up", desiredW,
                          desiredH);
            }

            return;
        }

        if (w == 0 || (SplitDx12.displayWidth == 0 && (ow == 0 || ow <= w)))
        {
            static bool saidNothing = false;

            if (!saidNothing)
            {
                saidNothing = true;
                LOG_INFO("DLSS-NR split: nothing to split at {}x{} -> {}x{}; needs a render scale below "
                         "native",
                         w, h, ow, oh);
            }

            DlssNr::SetSplitStatus("waiting: needs a render scale below native (set DLSS Quality or lower)");
            return;
        }

        ++SplitDx12.armTries;
        params->Set(NVSDK_NGX_Parameter_OutWidth, desiredW);
        params->Set(NVSDK_NGX_Parameter_OutHeight, desiredH);

        // A feature must declare the quality mode it actually is, or the driver creates it and then
        // refuses every evaluate: an oversized target its upscale mode, a 1:1 target DLAA.
        params->Set(NVSDK_NGX_Parameter_PerfQualityValue, SplitPerfQuality(f->RenderWidth(), desiredW));
        state.newBackend = view.rayReconstruction ? Upscaler::DLSSD : Upscaler::DLSS;
        state.changeBackend[handleId] = true;
        SplitDx12.geometryOwned = true;

        LOG_INFO("DLSS-NR split: re-creating Ray Reconstruction {}x{} -> {}x{} in place",
                 f->RenderWidth(), f->RenderHeight(), desiredW, desiredH);
        DlssNr::SetSplitStatus("re-creating Ray Reconstruction...");
        return;
    }

    if (!want && settled && SplitDx12.geometryOwned && SplitDx12.displayWidth != 0 &&
        (f->TargetWidth() != SplitDx12.displayWidth || f->TargetHeight() != SplitDx12.displayHeight))
    {
        params->Set(NVSDK_NGX_Parameter_OutWidth, SplitDx12.displayWidth);
        params->Set(NVSDK_NGX_Parameter_OutHeight, SplitDx12.displayHeight);

        if (SplitDx12.origPerfQuality != 0xffffffff)
            params->Set(NVSDK_NGX_Parameter_PerfQualityValue, SplitDx12.origPerfQuality);

        state.newBackend = view.rayReconstruction ? Upscaler::DLSSD : Upscaler::DLSS;
        state.changeBackend[handleId] = true;
        SplitDx12.geometryOwned = false;
        SplitDx12.restorePending = true;

        SplitParkResource(SplitDx12.intermediate);
        SplitParkEnlargement();
        DlssNr::SetSplitActive(false);
        DlssNr::SetSplitStatus("");

        LOG_INFO("DLSS-NR split: returning Ray Reconstruction to {}x{} -> {}x{} in place",
                 f->RenderWidth(), f->RenderHeight(), SplitDx12.displayWidth, SplitDx12.displayHeight);
        return;
    }
}

// Clamps the game's Ray Reconstruction feature at creation, for launches with the split already on.
void OnCreate(NVSDK_NGX_Feature featureId, NVSDK_NGX_Parameter* params)
{
    if ((featureId != NVSDK_NGX_Feature_RayReconstruction && featureId != NVSDK_NGX_Feature_SuperSampling) ||
        !SplitWanted() || params == nullptr)
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

    unsigned int desiredW = 0, desiredH = 0;
    SplitDesiredTarget(w, h, &desiredW, &desiredH);
    params->Set(NVSDK_NGX_Parameter_OutWidth, desiredW);
    params->Set(NVSDK_NGX_Parameter_OutHeight, desiredH);
    SplitDx12.lastDesiredWidth = desiredW;
    SplitDx12.lastDesiredHeight = desiredH;
    SplitDx12.geometryOwned = true;

    LOG_INFO("DLSS-NR split: Ray Reconstruction created {}x{} -> {}x{}; display {}x{} will be reached "
             "at the end of the chain",
             w, h, desiredW, desiredH, ow, oh);
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

// Makes sure the oversized working image exists at the given size, parking a mismatched one.
static bool SplitEnsureOversized(unsigned int w, unsigned int h, DXGI_FORMAT format)
{
    if (SplitDx12.oversized != nullptr &&
        ((unsigned int) SplitDx12.oversized->GetDesc().Width != w ||
         SplitDx12.oversized->GetDesc().Height != h))
        SplitParkEnlargement();

    if (SplitDx12.oversized == nullptr)
        SplitDx12.oversized = SplitScratch(g_splitDevice, format, w, h);

    return SplitDx12.oversized != nullptr;
}

static void SplitBarrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* res,
                         D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    cmdList->ResourceBarrier(1, &b);
}

// The final downscale, through OptiScaler's own filter so the look matches Output Scaling's.
static bool SplitDownscale(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* from, ID3D12Resource* to)
{
    // The downscaler's pipeline is baked at construction, but its dispatch reads the Downscaler
    // dropdown live -- a stale instance runs one scaler's shader on another's constants. Rebuild when
    // the user's choice changes; the old instance is parked, since its last dispatch may be in flight.
    const int downscalerKind = (int) Config::Instance()->OutputScalingDownscaler.value_or_default();

    if (SplitDx12.downscaler != nullptr && downscalerKind != SplitDx12.downscalerKind)
    {
        SplitRetired r;
        r.shader = std::move(SplitDx12.downscaler);
        SplitParkedList.push_back(std::move(r));
    }

    if (SplitDx12.downscaler == nullptr)
    {
        SplitDx12.downscaler = std::make_unique<OS_Dx12>("DLSS-NR Split Downscale", g_splitDevice, false);
        SplitDx12.downscalerKind = downscalerKind;
    }

    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = from;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    cmdList->ResourceBarrier(1, &b);

    const bool ok = SplitDx12.downscaler->Dispatch(cmdList, from, to);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    cmdList->ResourceBarrier(1, &b);

    return ok;
}

// The private creation kit, built once. Failure leaves it absent and the caller falls back.
static bool SplitEnsureCreationKit()
{
    if (SplitDx12.createQueue != nullptr)
        return true;

    D3D12_COMMAND_QUEUE_DESC qd {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    if (FAILED(g_splitDevice->CreateCommandQueue(&qd, IID_PPV_ARGS(&SplitDx12.createQueue))))
        return false;

    if (FAILED(g_splitDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                   IID_PPV_ARGS(&SplitDx12.createAlloc))) ||
        FAILED(g_splitDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, SplitDx12.createAlloc,
                                              nullptr, IID_PPV_ARGS(&SplitDx12.createList))) ||
        FAILED(SplitDx12.createList->Close()) ||
        FAILED(g_splitDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&SplitDx12.createFence))))
    {
        if (SplitDx12.createQueue != nullptr) { SplitDx12.createQueue->Release(); SplitDx12.createQueue = nullptr; }
        if (SplitDx12.createAlloc != nullptr) { SplitDx12.createAlloc->Release(); SplitDx12.createAlloc = nullptr; }
        if (SplitDx12.createList != nullptr) { SplitDx12.createList->Release(); SplitDx12.createList = nullptr; }
        return false;
    }

    SplitDx12.createEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return SplitDx12.createEvent != nullptr;
}

// The per-frame orchestration. Returns true when it handled the evaluate, with the result in outResult.
bool EvaluateRR(ID3D12GraphicsCommandList* cmdList, const NVSDK_NGX_Handle* handle,
                NVSDK_NGX_Parameter* params, PFN_NVSDK_NGX_ProgressCallback callback,
                const FeatureView& view, ID3D12Device* device, EvaluateFn evaluate,
                NVSDK_NGX_Result* outResult)
{
    g_splitDevice = device;

    if (!SplitWanted())
        return false;

    unsigned int renderW = 0, renderH = 0;
    bool nativeOneToOne = false;

    // Observable state only: operate when the feature's geometry matches what the settings call for and
    // no recreation is in flight. Every transition frame falls through to the conventional path.
    {
        if (!view.found || view.feature == nullptr || view.changeBackendCounter != 0)
            return false;

        IFeature_Dx12* f = view.feature;
        renderW = f->RenderWidth();
        renderH = f->RenderHeight();

        // A feature that is natively 1:1 -- DLAA, or an engine that upscales elsewhere -- already IS
        // the split arrangement. No geometry is touched; the model runs on it and the internal
        // enlargement (a 1:1 DLAA pass, or a supersample when Output Scaling asks) follows. That
        // enlargement is a real upscaler accumulator, which is where the edit's detail actually gets
        // its temporal stability.
        nativeOneToOne = !SplitDx12.geometryOwned && f->TargetWidth() == renderW &&
                         f->TargetHeight() == renderH;

        if (!nativeOneToOne)
        {
            if (SplitDx12.displayWidth == 0 || !SplitDx12.geometryOwned)
                return false;

            unsigned int desiredW = 0, desiredH = 0;
            SplitDesiredTarget(renderW, renderH, &desiredW, &desiredH);

            if (f->TargetWidth() != desiredW || f->TargetHeight() != desiredH)
                return false;
        }
    }

    ID3D12Resource* gameOutput = nullptr;
    ID3D12Resource* gameColor = nullptr;
    params->Get(NVSDK_NGX_Parameter_Output, &gameOutput);
    params->Get(NVSDK_NGX_Parameter_Color, &gameColor);

    if (gameOutput == nullptr || g_splitDevice == nullptr)
        return false;

    const DXGI_FORMAT workFormat = codec::TypedFormat(gameOutput->GetDesc().Format);
    const float mult = SplitRatio();
    const unsigned int dispW = nativeOneToOne ? renderW : SplitDx12.displayWidth;
    const unsigned int dispH = nativeOneToOne ? renderH : SplitDx12.displayHeight;
    const bool includeRR = !nativeOneToOne &&
                           Config::Instance()->DlssNrSplitIncludeRR.value_or_default() && mult > 1.0f;

    char status[160];

    if (includeRR)
    {
        // RR itself upscales to the supersampled size; the model works on that image; only the
        // downscale remains. The conventional Output Scaling look with the model in the chain. The
        // target comes from the same computation the manager armed with, so the two stay in lockstep.
        unsigned int targetW = 0, targetH = 0;
        SplitDesiredTarget(renderW, renderH, &targetW, &targetH);

        if (!SplitEnsureOversized(targetW, targetH, workFormat))
        {
            LOG_ERROR("DLSS-NR split: the supersample target could not be created; falling back");
            SplitDx12.failed = true;
            DlssNr::SetSplitActive(false);
            DlssNr::SetSplitStatus("failed; conventional path running (see the log)");
            return false;
        }

        params->Set(NVSDK_NGX_Parameter_Output, SplitDx12.oversized);
        const NVSDK_NGX_Result rrResult = evaluate(cmdList, handle, params, callback);

        if (rrResult != NVSDK_NGX_Result_Success)
        {
            // The feature matched and was stable, so this is a genuine refusal of the supersampled
            // arrangement. Drop Include RR (runtime only -- the saved checkbox survives) and let the
            // manager re-arm the plain split, rather than failing every frame from here on.
            params->Set(NVSDK_NGX_Parameter_Output, gameOutput);
            LOG_ERROR("DLSS-NR split: Ray Reconstruction refused to run at {}x{} -> {}x{}; dropping "
                      "Include RR",
                      renderW, renderH, targetW, targetH);
            Config::Instance()->DlssNrSplitIncludeRR.set_volatile_value(false);
            DlssNr::SetSplitStatus("include-RR refused at this ratio; running the split without it");
            *outResult = rrResult;
            return true;
        }

        DlssNr::SetSplitActive(true);
        DlssNr::EvaluateAfterUpscale(cmdList, params, true);

        const bool ok = SplitDownscale(cmdList, SplitDx12.oversized, gameOutput);
        params->Set(NVSDK_NGX_Parameter_Output, gameOutput);

        if (!ok)
        {
            LOG_ERROR("DLSS-NR split: the downscale failed; falling back");
            SplitDx12.failed = true;
            DlssNr::SetSplitActive(false);
            DlssNr::SetSplitStatus("failed; conventional path running (see the log)");
            *outResult = NVSDK_NGX_Result_Fail;
            return true;
        }

        params->Set(NVSDK_NGX_Parameter_OutWidth, dispW);
        params->Set(NVSDK_NGX_Parameter_OutHeight, dispH);

        std::snprintf(status, sizeof(status),
                      "running: RR supersampled x%.2f -> NR -> downscale (RR included)",
                      (float) targetW / (float) SplitDx12.displayWidth);
        DlssNr::SetSplitStatus(status);
        *outResult = NVSDK_NGX_Result_Success;
        return true;
    }

    // The enlargement: to the supersampled size when the ratio asks, straight to display otherwise.
    const bool supersample = mult > 1.0f;
    auto targetW = supersample ? (unsigned int) (dispW * mult + 0.5f) : dispW;
    auto targetH = supersample ? (unsigned int) (dispH * mult + 0.5f) : dispH;

    // DLSS refuses ratios beyond 4x its input, so the target is capped there.
    targetW = targetW > renderW * 4 ? renderW * 4 : targetW;
    targetH = targetH > renderH * 4 ? renderH * 4 : targetH;

    // When that target is the size the frame already is, the enlargement changes no geometry: it is a
    // second temporal filter running over the model's work, and a temporal filter's job is to remove
    // high-frequency variation that disagrees with its history -- which is precisely what the model
    // just synthesised. That is where the split's detail went. So it is skipped: the model runs in
    // place on the game's own output, and the pass earns its place again the moment supersampling or
    // a real upscale gives it something to do.
    if (targetW == renderW && targetH == renderH)
    {
        const NVSDK_NGX_Result plainResult = evaluate(cmdList, handle, params, callback);

        if (plainResult != NVSDK_NGX_Result_Success)
        {
            *outResult = plainResult;
            return true;
        }

        SplitParkEnlargement();
        DlssNr::SetSplitActive(true);
        DlssNr::EvaluateAfterUpscale(cmdList, params, true);
        DlssNr::SetSplitStatus("running: NR at 1:1, before the interface -- no enlargement needed "
                               "(Output Scaling adds a supersampled one)");
        *outResult = NVSDK_NGX_Result_Success;
        return true;
    }

    // RR 1:1: denoise at render size, enhance there, enlarge once.
    if (SplitDx12.intermediate == nullptr)
    {
        SplitDx12.intermediate = SplitScratch(g_splitDevice, workFormat, renderW, renderH);

        if (SplitDx12.intermediate == nullptr)
        {
            LOG_ERROR("DLSS-NR split: the intermediate could not be created; falling back");
            SplitDx12.failed = true;
            DlssNr::SetSplitActive(false);
            DlssNr::SetSplitStatus("failed; conventional path running (see the log)");
            return false;
        }
    }

    params->Set(NVSDK_NGX_Parameter_Output, SplitDx12.intermediate);
    const NVSDK_NGX_Result rrResult = evaluate(cmdList, handle, params, callback);

    if (rrResult != NVSDK_NGX_Result_Success)
    {
        params->Set(NVSDK_NGX_Parameter_Output, gameOutput);
        *outResult = rrResult;
        return true;
    }

    DlssNr::SetSplitActive(true);
    DlssNr::EvaluateAfterUpscale(cmdList, params, true);

    const int srPresetWanted = (int) Config::Instance()->DlssNrSplitSrPreset.value_or_default();

    if (SplitDx12.sr != nullptr &&
        (SplitDx12.srTargetWidth != targetW || SplitDx12.srBuiltPreset != srPresetWanted))
    {
        LOG_INFO("DLSS-NR split: rebuilding the enlargement for {}x{} (preset {})", targetW, targetH,
                 srPresetWanted);
        SplitParkEnlargement();
    }

    if (SplitDx12.sr == nullptr)
    {
        params->Set(NVSDK_NGX_Parameter_OutWidth, targetW);
        params->Set(NVSDK_NGX_Parameter_OutHeight, targetH);

        // The enlargement parses the game's block, whose quality mode describes the game's own ratio.
        // Built supersampled, it must declare what it actually is or the driver refuses it at evaluate.
        params->Set(NVSDK_NGX_Parameter_PerfQualityValue, SplitPerfQuality(renderW, targetW));

        // The chosen render preset for the enlargement, written across every mode slot so the one the
        // declared quality maps to is covered; the game's own hints are restored right after creation.
        // (OptiScaler's global Render Presets Override, when on, wins inside the feature's init.)
        unsigned int origHintQ = 0, origHintB = 0, origHintP = 0, origHintUP = 0, origHintDLAA = 0;

        if (srPresetWanted != 0)
        {
            params->Get(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, &origHintQ);
            params->Get(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, &origHintB);
            params->Get(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, &origHintP);
            params->Get(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, &origHintUP);
            params->Get(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, &origHintDLAA);
            params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, (unsigned int) srPresetWanted);
            params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, (unsigned int) srPresetWanted);
            params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, (unsigned int) srPresetWanted);
            params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
                        (unsigned int) srPresetWanted);
            params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, (unsigned int) srPresetWanted);
        }

        auto sr = std::make_unique<DLSSFeatureDx12>(IFeature::GetNextHandleId(), params);
        bool srInited = false;

        // The creation is recorded on the private list, executed on the private queue and waited to
        // completion here -- never mixed into the game's list, which is where three sessions died.
        if (SplitEnsureCreationKit() && SUCCEEDED(SplitDx12.createAlloc->Reset()) &&
            SUCCEEDED(SplitDx12.createList->Reset(SplitDx12.createAlloc, nullptr)))
        {
            srInited = sr->Init(g_splitDevice, SplitDx12.createList, params) && sr->IsInited();

            if (SUCCEEDED(SplitDx12.createList->Close()))
            {
                ID3D12CommandList* createLists[] = { SplitDx12.createList };
                SplitDx12.createQueue->ExecuteCommandLists(1, createLists);
                ++SplitDx12.createFenceValue;

                if (SUCCEEDED(SplitDx12.createQueue->Signal(SplitDx12.createFence,
                                                            SplitDx12.createFenceValue)) &&
                    SplitDx12.createFence->GetCompletedValue() < SplitDx12.createFenceValue)
                {
                    SplitDx12.createFence->SetEventOnCompletion(SplitDx12.createFenceValue,
                                                                SplitDx12.createEvent);
                    WaitForSingleObject(SplitDx12.createEvent, 2000);
                }
            }
            else
            {
                srInited = false;
            }
        }

        if (srPresetWanted != 0)
        {
            params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, origHintQ);
            params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, origHintB);
            params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, origHintP);
            params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, origHintUP);
            params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, origHintDLAA);
        }

        if (SplitDx12.origPerfQuality != 0xffffffff)
            params->Set(NVSDK_NGX_Parameter_PerfQualityValue, SplitDx12.origPerfQuality);

        if (!srInited)
        {
            params->Set(NVSDK_NGX_Parameter_Output, gameOutput);
            params->Set(NVSDK_NGX_Parameter_OutWidth, dispW);
            params->Set(NVSDK_NGX_Parameter_OutHeight, dispH);

            if (supersample && !SplitDx12.supersampleRefused)
            {
                // The supersampled enlargement was refused; run at display size instead of dying.
                SplitDx12.supersampleRefused = true;
                LOG_ERROR("DLSS-NR split: the supersampled enlargement would not initialise; dropping "
                          "to display size");
                DlssNr::SetSplitStatus("supersample refused here; enlargement at display size");
                *outResult = rrResult;
                return true;
            }

            LOG_ERROR("DLSS-NR split: the internal Super Resolution feature would not initialise; "
                      "falling back");
            SplitDx12.failed = true;
            DlssNr::SetSplitActive(false);
            DlssNr::SetSplitStatus("failed; conventional path running (see the log)");
            *outResult = rrResult;
            return true;
        }

        SplitDx12.sr = std::move(sr);
        SplitDx12.srTargetWidth = targetW;
        SplitDx12.srBuiltPreset = srPresetWanted;
        SplitDx12.srJustCreated = true;
        LOG_INFO("DLSS-NR split: internal Super Resolution running {}x{} -> {}x{}{}", renderW, renderH,
                 targetW, targetH, supersample ? " (supersampled)" : "");
    }

    if (SplitDx12.srJustCreated)
    {
        // The creation commands go through the game's own submit first; the first evaluate happens
        // next frame. One frame keeps the previous image -- invisible inside a toggle's hitch.
        SplitDx12.srJustCreated = false;

        params->Set(NVSDK_NGX_Parameter_Output, gameOutput);
        params->Set(NVSDK_NGX_Parameter_OutWidth, dispW);
        params->Set(NVSDK_NGX_Parameter_OutHeight, dispH);
        DlssNr::SetSplitStatus("arming the enlargement...");
        *outResult = NVSDK_NGX_Result_Success;
        return true;
    }

    const bool useOversized = supersample && SplitEnsureOversized(targetW, targetH, workFormat);

    params->Set(NVSDK_NGX_Parameter_Color, SplitDx12.intermediate);
    params->Set(NVSDK_NGX_Parameter_Output, useOversized ? SplitDx12.oversized : gameOutput);
    params->Set(NVSDK_NGX_Parameter_OutWidth, targetW);
    params->Set(NVSDK_NGX_Parameter_OutHeight, targetH);

    // The internal SR consumes the intermediate as an input, and NGX requires inputs shader-readable.
    // The model's pass leaves it in UNORDERED_ACCESS, and an input in the wrong state is undefined
    // reads -- which upscales to garbage without a single error anywhere.
    SplitBarrier(cmdList, SplitDx12.intermediate, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // The feature was created with IsHDR and no AutoExposure, so it requires an exposure texture. The
    // game supplies one under its Ray Reconstruction name; hand it over under the name SR reads, or the
    // result is black -- the community guide's exposure trap, in its two-feature form.
    ID3D12Resource* rrExposure = nullptr;
    params->Get("DLSSD.ExposureTexture", &rrExposure);

    if (rrExposure != nullptr)
        params->Set("ExposureTexture", rrExposure);

    // The game's sharpness must not leak into our SR -- it belongs to the game's own arrangement. The
    // enlargement sharpens only by the user's explicit amount (0 = off, the default; runs via RCAS).
    float gameSharpness = 0.0f;
    params->Get(NVSDK_NGX_Parameter_Sharpness, &gameSharpness);
    params->Set(NVSDK_NGX_Parameter_Sharpness,
                Config::Instance()->DlssNrSplitSrSharpness.value_or_default());

    // The game's jitter describes where its raw samples sat. What the enlargement is handed is not
    // raw: it has been through the denoiser, and its samples sit at pixel centres. Passing the game's
    // offsets tells DLSS to accumulate as if the image were displaced by a fraction of a pixel that
    // changes every frame -- a per-frame misregistration, which reads as softness and shimmer in
    // exactly the detail the model added.
    float gameJitterX = 0.0f;
    float gameJitterY = 0.0f;
    params->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &gameJitterX);
    params->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &gameJitterY);
    params->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.0f);
    params->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.0f);

    bool srOk = SplitDx12.sr->Evaluate(cmdList, params);

    SplitBarrier(cmdList, SplitDx12.intermediate, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    params->Set(NVSDK_NGX_Parameter_Sharpness, gameSharpness);
    params->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, gameJitterX);
    params->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, gameJitterY);

    if (srOk && useOversized)
        srOk = SplitDownscale(cmdList, SplitDx12.oversized, gameOutput);

    // The block goes back exactly as the game filled it -- including the output size, which we borrowed
    // for the oversized target and whose pollution once compounded the supersample until the device hung.
    if (gameColor != nullptr)
        params->Set(NVSDK_NGX_Parameter_Color, gameColor);

    params->Set(NVSDK_NGX_Parameter_Output, gameOutput);
    params->Set(NVSDK_NGX_Parameter_OutWidth, dispW);
    params->Set(NVSDK_NGX_Parameter_OutHeight, dispH);

    if (!srOk && supersample && !SplitDx12.supersampleRefused)
    {
        // The driver refused the supersampled enlargement at evaluate. Run at display size from the
        // next frame instead of latching the whole split off; the enlargement is rebuilt for the new
        // target by the srTargetWidth check above.
        SplitDx12.supersampleRefused = true;
        LOG_ERROR("DLSS-NR split: the supersampled enlargement refused to run; dropping to display "
                  "size");
        DlssNr::SetSplitStatus("supersample refused here; enlargement at display size");
    }
    else if (!srOk)
    {
        LOG_ERROR("DLSS-NR split: the enlargement failed; falling back");
        SplitDx12.failed = true;
        DlssNr::SetSplitActive(false);
        DlssNr::SetSplitStatus("failed; conventional path running (see the log)");
    }
    else if (useOversized)
    {
        std::snprintf(status, sizeof(status),
                      "running: RR 1:1 -> NR -> SR x%.2f supersampled -> downscale", mult);
        DlssNr::SetSplitStatus(status);
    }
    else
    {
        DlssNr::SetSplitStatus(nativeOneToOne ? "running: NR -> internal DLAA (native 1:1)"
                                              : "running: RR 1:1 -> NR -> internal SR");
    }

    *outResult = srOk ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_Fail;
    return true;
}

} // namespace DlssNr::Split

#endif // OPTI_DLSSNR
