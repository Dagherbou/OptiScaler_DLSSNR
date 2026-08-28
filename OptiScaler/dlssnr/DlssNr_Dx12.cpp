#include "pch.h"

#include "DlssNr_Dx12.h"

#include "DlssNr_Codec.h"
#include "DlssNr_Probe.h"
#include "DlssNr_Capture.h"

#include <Config.h>
#include <State.h>
#include <Util.h>

#include <proxies/NVNGX_Proxy.h>
#include <gpu_time/GpuTime_Dx12.h>

namespace
{
// Everything the model is reached through. The snippet refuses callers whose module path does not
// contain "nvngx.dll", so the calls are made from a small library named for exactly that reason and
// shipped beside OptiScaler; see nvngx.dll_dlssnr.dll.
using PFN_NrCreate = void*(__cdecl*) (const wchar_t*, const wchar_t*, ID3D12Device*,
                                      ID3D12GraphicsCommandList*, void*, unsigned int, unsigned int, int,
                                      float, int, float, float, float, int, int);
using PFN_NrEvaluate = int(__cdecl*) (ID3D12GraphicsCommandList*, void*, void*, ID3D12Resource*,
                                      ID3D12Resource*, ID3D12Resource*, ID3D12Resource*, unsigned int,
                                      unsigned int, unsigned int, unsigned int, int, int, float, int,
                                      float, float, float, int, float, float);
using PFN_NrRelease = void(__cdecl*) (void*);
using PFN_NrSetFloatSlot = void(__cdecl*) (int);
using PFN_NrProbeFloat = void(__cdecl*) (void*, const char*, float, int);

// One per back buffer, so an allocator is never reset while its frame is still in flight.
constexpr unsigned int kPresentAllocators = 3;

struct NrState
{
    HMODULE forwarder = nullptr;
    PFN_NrCreate create = nullptr;
    PFN_NrEvaluate evaluate = nullptr;
    PFN_NrRelease release = nullptr;
    PFN_NrSetFloatSlot setFloatSlot = nullptr;
    PFN_NrProbeFloat probeFloat = nullptr;
    bool floatSlotKnown = false;
    int* lastInit = nullptr;
    int* lastCreate = nullptr;

    NVSDK_NGX_Parameter* capabilityParams = nullptr;
    void* feature = nullptr;

    // The model cannot read and write one resource, so the frame is staged through these.
    ID3D12Resource* colorCopy = nullptr;
    ID3D12Resource* output = nullptr;

    // The frame as the upscaler wrote it. The resolve adds the model's edit to this rather than
    // reconstructing it by inverting the tone curve, which is what turned every light in the frame into
    // a string of coloured cells.
    ID3D12Resource* hdrCopy = nullptr;

    // The accumulated edit, double-buffered: one read while the other is written. Always float, since
    // an edit is signed and the frame's own format generally is not.
    ID3D12Resource* editHistory[2] = {};
    unsigned int editIndex = 0;
    bool editWarm = false;

    // The frame shrunk for the model, when it is working below full resolution.
    ID3D12Resource* colorSmall = nullptr;
    unsigned int workWidth = 0;
    unsigned int workHeight = 0;

    // Cloned unconditionally when running at present, and only for typeless formats otherwise.
    ID3D12Resource* depthClone = nullptr;
    ID3D12Resource* motionClone = nullptr;

    unsigned int width = 0;
    unsigned int height = 0;
    bool reset = true;

    // The present path records its own work: the overlay's command list only runs when the menu is
    // open, so it cannot be borrowed for something that has to happen every frame.
    ID3D12CommandAllocator* presentAllocators[kPresentAllocators] = {};
    ID3D12GraphicsCommandList* presentList = nullptr;

    // An allocator cannot be reset while the GPU is still reading the commands recorded into it, and
    // there is nothing else here to serialise against -- this list is submitted independently of the
    // game's own work.
    ID3D12Fence* presentFence = nullptr;
    HANDLE presentFenceEvent = nullptr;
    unsigned long long presentFenceValues[kPresentAllocators] = {};
    unsigned long long presentFenceNext = 0;

    // Dimensions of the guides as the upscaler handed them over, kept for the present path, which runs
    // long after that call has returned.
    unsigned int guideWidth = 0;
    unsigned int guideHeight = 0;
    bool guidesReady = false;

    // How the game encodes its guides, as the game itself reports it. Captured with the guides, since
    // the finished-frame path runs long after the upscaler's call has returned.
    bool guideDepthInverted = false;
    float guideMvScaleX = 1.0f;
    float guideMvScaleY = 1.0f;

    // The values the live feature was created with, and when a difference from them was first seen.
    unsigned int builtPreset = 0;
    float builtIntensity = 0.0f;
    unsigned int builtStyle = 0;
    float builtLocalStructure = 0.0f;
    float builtLocalTone = 0.0f;
    float builtSkinStructure = 0.0f;
    bool builtAutoMask = false;
    bool builtUiCorrection = false;
    unsigned long long settledAt = 0;

    // Once something fails there is no recovering it mid-session, and retrying every frame turns a
    // failure into a crash. It stays off and says why.
    bool failed = false;
    const char* reason = "";
};

NrState g_nr;
codec::Codec g_codec;

// What the pass costs on the GPU, for the breakdown in the overlay.
std::unique_ptr<GpuTime_Dx12> g_gpuTime;
std::optional<double> g_lastGpuTime;

// Exposure measurement, and the white point derived from it.
probe::FrameReducer g_reducer;
probe::BlockReader g_reader;

// Writes matched before/after frames on request, so comparisons stop depending on video.
capture::FrameCapture g_capture;

// One capture happens on its own each session, so there is always a fresh sample without anyone having
// to remember to ask. Started after the scene has had a moment to settle: the first frames after a
// feature is built carry its reset, and are not representative of anything.
constexpr unsigned long long kAutoCaptureAfterFrames = 180;
bool g_autoCaptureDone = false;

// Cleared once per run, so a session's captures are its own and nothing accumulates across launches.
void ClearCaptureDirectory()
{
    static bool cleared = false;

    if (cleared)
        return;

    cleared = true;

    std::error_code ec;
    const auto dir = Util::DllPath().remove_filename() / "dlssnr-capture";

    if (std::filesystem::exists(dir, ec))
    {
        std::filesystem::remove_all(dir, ec);

        if (ec)
            LOG_WARN("DLSS-NR could not clear {}: {}", dir.string(), ec.message());
    }
}
float g_autoWhitePoint = 2.0f;
bool g_autoWhitePointSettled = false;
unsigned long long g_frames = 0;

// The encoded mean is aimed here. Mid-grey rather than anything brighter: the model has to see both the
// shadow detail it might lift and the highlights it must not blow out.
constexpr float kTargetEncodedMean = 0.45f;

// How fast the derived value follows the scene. Readings arrive a few times a second, and an exposure
// that lunges at every cut is worse than one that arrives a moment late.
constexpr float kWhitePointBlend = 0.25f;

// Recomputes the white point from a measured mean. Inverting the encode for the white point that puts
// that mean at the target gives wp = mean * (1 - t^g) / t^g.
float WhitePointForMean(float meanLuma)
{
    const float encoded = powf(kTargetEncodedMean, 2.2f);
    const float ratio = encoded / (1.0f - encoded);
    const float wp = meanLuma / ratio;
    // A black frame between scenes would otherwise drive this to zero and divide the next frame by it.
    return wp < 0.01f ? 0.01f : (wp > 10000.0f ? 10000.0f : wp);
}

std::filesystem::path g_dllDir;

// Loads the forwarder that owns the calls into the snippet.
bool EnsureForwarder()
{
    if (g_nr.forwarder != nullptr)
        return g_nr.create != nullptr;

    if (g_dllDir.empty())
        g_dllDir = Util::DllPath().remove_filename();

    // Beside OptiScaler first, then beside the executable: someone dropping this into a game folder may
    // reasonably put it in either place.
    auto found = Util::FindFilePath(g_dllDir, "nvngx.dll_dlssnr.dll");

    if (!found.has_value())
        found = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx.dll_dlssnr.dll");

    if (!found.has_value())
    {
        LOG_ERROR("nvngx.dll_dlssnr.dll not found beside OptiScaler ({}) or the game executable",
                  g_dllDir.string());
        g_nr.reason = "nvngx.dll_dlssnr.dll is missing";
        return false;
    }

    // FindFilePath hands back the file itself, not the directory holding it.
    const auto path = found.value();
    g_nr.forwarder = LoadLibraryW(path.wstring().c_str());

    if (g_nr.forwarder == nullptr)
    {
        LOG_ERROR("nvngx.dll_dlssnr.dll found at {} but would not load, error {}", path.string(),
                  GetLastError());
        g_nr.reason = "nvngx.dll_dlssnr.dll would not load";
        return false;
    }

    g_nr.create = (PFN_NrCreate) GetProcAddress(g_nr.forwarder, "dlssnr_call_create");
    g_nr.evaluate = (PFN_NrEvaluate) GetProcAddress(g_nr.forwarder, "dlssnr_call_evaluate");
    g_nr.release = (PFN_NrRelease) GetProcAddress(g_nr.forwarder, "dlssnr_call_release");
    g_nr.setFloatSlot = (PFN_NrSetFloatSlot) GetProcAddress(g_nr.forwarder, "dlssnr_call_set_float_slot");
    g_nr.probeFloat = (PFN_NrProbeFloat) GetProcAddress(g_nr.forwarder, "dlssnr_call_probe_float");
    g_nr.lastInit = (int*) GetProcAddress(g_nr.forwarder, "dlssnr_call_last_init");
    g_nr.lastCreate = (int*) GetProcAddress(g_nr.forwarder, "dlssnr_call_last_create");

    if (g_nr.create == nullptr || g_nr.evaluate == nullptr)
    {
        g_nr.reason = "the forwarder is missing its exports";
        return false;
    }

    LOG_INFO("DLSS-NR forwarder loaded from {}", path.string());
    return true;
}

// The model needs the driver core's own capability block: it carries the snippet and preset callbacks a
// feature expects at create time, which a freshly allocated block does not have.
void DiscoverFloatSlot(NVSDK_NGX_Parameter* params);

bool EnsureCapabilityParams(ID3D12Device* device)
{
    if (g_nr.capabilityParams != nullptr)
        return true;

    if (!NVNGXProxy::IsDx12Inited() && !NVNGXProxy::InitDx12(device))
    {
        g_nr.reason = "the NGX core would not initialise";
        return false;
    }

    if (NVNGXProxy::D3D12_GetCapabilityParameters() == nullptr)
    {
        g_nr.reason = "the NGX core has no capability parameters";
        return false;
    }

    if (NVNGXProxy::D3D12_GetCapabilityParameters()(&g_nr.capabilityParams) != NVSDK_NGX_Result_Success ||
        g_nr.capabilityParams == nullptr)
    {
        g_nr.capabilityParams = nullptr;
        g_nr.reason = "the NGX core refused its capability parameters";
        return false;
    }

    // Before anything is written to it, work out where this block keeps floats.
    DiscoverFloatSlot(g_nr.capabilityParams);
    return true;
}

// Works out which vtable slot this parameter block keeps floats in, by writing a known value through
// each candidate and asking for it back through the header's typed getter. Only a slot that returns the
// value it was given is accepted.
//
// Slot 1 is where the public header declares the float overload, so it is tried first and wins wherever
// that assumption holds. It does not hold for the driver's own block: every float written there reads
// back as FAIL_UnsupportedParameter while every uint lands, which is why intensity, local structure,
// local tone and skin structure never did anything.
void DiscoverFloatSlot(NVSDK_NGX_Parameter* params)
{
    if (g_nr.floatSlotKnown || params == nullptr || g_nr.probeFloat == nullptr ||
        g_nr.setFloatSlot == nullptr)
        return;

    g_nr.floatSlotKnown = true;

    static const char* kProbeKey = "DLSSNR.OptiScalerFloatProbe";
    static const int kCandidates[] = { 1, 2, 5, 6, 7, 4, 3, 0 };
    const float expected = 0.375f; // exact in binary, so the round trip is exact or it is wrong

    for (int slot : kCandidates)
    {
        float readBack = 0.0f;
        g_nr.probeFloat(params, kProbeKey, expected, slot);

        if (params->Get(kProbeKey, &readBack) == NVSDK_NGX_Result_Success && readBack == expected)
        {
            g_nr.setFloatSlot(slot);
            LOG_INFO("DLSS-NR float parameters go through vtable slot {}", slot);
            return;
        }
    }

    LOG_ERROR("DLSS-NR could not find the float setter: intensity, local structure, local tone and skin "
              "structure will have no effect. The uint parameters still apply.");
}

// Switching inject points changes the surface format underneath the scratch set: the finished frame
// works in the swapchain's format, the pre-frame-generation path in the upscaler's. A stale set either
// clamps linear HDR into an 8-bit texture -- wrong brightness until something forces a rebuild -- or
// hands CopyResource mismatched formats, which fails silently and makes the whole pass appear to do
// nothing. So the set is torn down whenever the format it was built for is not the format needed now.
void ReleaseSurfacesIfFormatChanged(DXGI_FORMAT needed)
{
    if (g_nr.output == nullptr || g_nr.output->GetDesc().Format == needed)
        return;

    LOG_INFO("DLSS-NR rebuilding surfaces: format {} -> {} (inject point changed)",
             (int) g_nr.output->GetDesc().Format, (int) needed);

    if (g_nr.feature != nullptr && g_nr.release != nullptr)
    {
        g_nr.release(g_nr.feature);
        g_nr.feature = nullptr;
    }

    for (ID3D12Resource** r : { &g_nr.output, &g_nr.colorCopy, &g_nr.hdrCopy, &g_nr.colorSmall })
    {
        if (*r != nullptr)
        {
            (*r)->Release();
            *r = nullptr;
        }
    }

    g_nr.reset = true;
}

ID3D12Resource* CreateScratch(ID3D12Device* device, DXGI_FORMAT format, unsigned int width,
                              unsigned int height)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // The model writes its result, so the destination has to be a UAV.
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ID3D12Resource* res = nullptr;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&res));
    return res;
}

void Barrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* res, D3D12_RESOURCE_STATES from,
             D3D12_RESOURCE_STATES to)
{
    if (from == to)
        return;

    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    cmdList->ResourceBarrier(1, &b);
}

// A typeless resource cannot be viewed, and NGX builds its own views with nothing to tell it which
// format to use. Depth is very often declared typeless, so the typed member of the same family is
// substituted; CopyResource accepts that as a destination for the typeless original.
DXGI_FORMAT TypedGuideFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
        return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R24G8_TYPELESS:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R32G32_TYPELESS:
        return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS:
        return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return f;
    }
}

bool IsTypeless(DXGI_FORMAT f) { return TypedGuideFormat(f) != f; }

// Creates a typed twin of a guide buffer, matching everything but the format.
ID3D12Resource* CreateGuideClone(ID3D12Device* device, ID3D12Resource* source)
{
    D3D12_RESOURCE_DESC desc = source->GetDesc();
    desc.Format = TypedGuideFormat(desc.Format);
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    ID3D12Resource* res = nullptr;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                    nullptr, IID_PPV_ARGS(&res));
    return res;
}

// Hands back something the model can actually read: the guide itself when it is typed, or a typed copy
// of it when it is not. NGX requires its inputs in NON_PIXEL_SHADER_RESOURCE at evaluate time, which is
// a documented contract rather than a guess about any one game's frame graph, so that is the state
// transitioned away from and back to here.
ID3D12Resource* ReadableGuide(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
                              ID3D12Resource* source, ID3D12Resource** clone)
{
    if (source == nullptr || !IsTypeless(source->GetDesc().Format))
        return source;

    if (*clone == nullptr)
    {
        *clone = CreateGuideClone(device, source);

        if (*clone == nullptr)
            return nullptr;

        LOG_INFO("DLSS-NR cloned a typeless guide as format {}",
                 (int) TypedGuideFormat(source->GetDesc().Format));
    }

    Barrier(cmdList, source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyResource(*clone, source);
    Barrier(cmdList, source, D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cmdList, *clone, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return *clone;
}

// The upscaler's own names differ between super resolution and ray reconstruction, and only one set is
// present on any given block.
ID3D12Resource* GetResource(NVSDK_NGX_Parameter* params, const char* a, const char* b)
{
    ID3D12Resource* res = nullptr;

    if (params->Get(a, &res) == NVSDK_NGX_Result_Success && res != nullptr)
        return res;

    res = nullptr;

    if (params->Get(b, &res) == NVSDK_NGX_Result_Success)
        return res;

    return nullptr;
}

// Forces a clone even of a typed guide. At present time the game's own buffers are not promised to hold
// this frame any more, and in one case were freed outright across a save transition.
ID3D12Resource* CloneGuideAlways(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
                                 ID3D12Resource* source, ID3D12Resource** clone)
{
    if (source == nullptr)
        return nullptr;

    if (*clone == nullptr)
    {
        *clone = CreateGuideClone(device, source);

        if (*clone == nullptr)
            return nullptr;
    }

    Barrier(cmdList, source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyResource(*clone, source);
    Barrier(cmdList, source, D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return *clone;
}

// A change has to hold still before it is acted on: a slider being dragged reports a new value every
// frame, and each one would otherwise mean a new model.
constexpr unsigned long long kSettleFrames = 30;

bool TuningMatchesFeature(const Config& cfg)
{
    return g_nr.builtPreset == cfg.DlssNrPreset.value_or_default() &&
           g_nr.builtIntensity == cfg.DlssNrIntensity.value_or_default() &&
           g_nr.builtStyle == cfg.DlssNrStyle.value_or_default() &&
           g_nr.builtLocalStructure == cfg.DlssNrLocalStructure.value_or_default() &&
           g_nr.builtLocalTone == cfg.DlssNrLocalTone.value_or_default() &&
           g_nr.builtSkinStructure == cfg.DlssNrSkinStructure.value_or_default() &&
           g_nr.builtAutoMask == cfg.DlssNrAutoMask.value_or_default() &&
           g_nr.builtUiCorrection == cfg.DlssNrUiCorrection.value_or_default();
}

void RecordBuiltTuning(const Config& cfg)
{
    g_nr.builtPreset = cfg.DlssNrPreset.value_or_default();
    g_nr.builtIntensity = cfg.DlssNrIntensity.value_or_default();
    g_nr.builtStyle = cfg.DlssNrStyle.value_or_default();
    g_nr.builtLocalStructure = cfg.DlssNrLocalStructure.value_or_default();
    g_nr.builtLocalTone = cfg.DlssNrLocalTone.value_or_default();
    g_nr.builtSkinStructure = cfg.DlssNrSkinStructure.value_or_default();
    g_nr.builtAutoMask = cfg.DlssNrAutoMask.value_or_default();
    g_nr.builtUiCorrection = cfg.DlssNrUiCorrection.value_or_default();
}

// Waits for every list this has submitted. Releasing the feature before that is what took the game down
// each of the previous times, and this is the first place with the means to avoid it.
void WaitForAllSubmitted()
{
    if (g_nr.presentFence == nullptr || g_nr.presentFenceNext == 0)
        return;

    if (g_nr.presentFence->GetCompletedValue() >= g_nr.presentFenceNext)
        return;

    if (SUCCEEDED(g_nr.presentFence->SetEventOnCompletion(g_nr.presentFenceNext, g_nr.presentFenceEvent)))
        WaitForSingleObject(g_nr.presentFenceEvent, 1000);
}

void WaitForAllSubmitted();

bool EnsurePresentList(ID3D12Device* device)
{
    if (g_nr.presentList != nullptr)
        return true;

    for (unsigned int i = 0; i < kPresentAllocators; ++i)
    {
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&g_nr.presentAllocators[i]))))
            return false;
    }

    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_nr.presentAllocators[0],
                                         nullptr, IID_PPV_ARGS(&g_nr.presentList))))
        return false;

    g_nr.presentList->Close();

    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_nr.presentFence))))
        return false;

    g_nr.presentFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if (g_nr.presentFenceEvent == nullptr)
        return false;

    return true;
}

// Blocks until the work last recorded into this allocator has finished. In the steady state the GPU is
// already well past it and this returns immediately.
void WaitForAllocator(unsigned int index)
{
    const unsigned long long target = g_nr.presentFenceValues[index];

    if (target == 0 || g_nr.presentFence->GetCompletedValue() >= target)
        return;

    if (SUCCEEDED(g_nr.presentFence->SetEventOnCompletion(target, g_nr.presentFenceEvent)))
        WaitForSingleObject(g_nr.presentFenceEvent, 1000);
}
} // namespace

namespace DlssNr
{
bool g_splitActive = false;

void SetSplitActive(bool active) { g_splitActive = active; }

void EvaluateAfterUpscale(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params,
                          bool forceInPlace)
{
    const Config& cfg = *Config::Instance();

    if (!cfg.DlssNrEnabled.value_or_default() || g_nr.failed || cmdList == nullptr || params == nullptr)
        return;

    ID3D12Resource* target = GetResource(params, NVSDK_NGX_Parameter_Output, "DLSSD.Output");
    ID3D12Resource* depth = GetResource(params, NVSDK_NGX_Parameter_Depth, "DLSSD.Depth");
    ID3D12Resource* motion = GetResource(params, NVSDK_NGX_Parameter_MotionVectors, "DLSSD.MotionVectors");

    // Without all three there is nothing to run on. This is not a failure -- some evaluates legitimately
    // carry none of it -- so it stays quiet and tries again next frame.
    if (target == nullptr || depth == nullptr || motion == nullptr)
        return;

    ID3D12Device* device = nullptr;

    if (FAILED(target->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
        return;

    const D3D12_RESOURCE_DESC desc = target->GetDesc();
    const auto width = (unsigned int) desc.Width;
    const auto height = desc.Height;

    // Depth and motion vectors are the upscaler's inputs and so are at render resolution, while colour
    // and output are at display resolution. The model takes that as a subrect per resource rather than
    // needing them resampled, which is why nothing here rescales anything.
    unsigned int guideWidth = 0;
    unsigned int guideHeight = 0;
    params->Get(NVSDK_NGX_Parameter_Width, &guideWidth);
    params->Get(NVSDK_NGX_Parameter_Height, &guideHeight);

    if (guideWidth == 0 || guideHeight == 0)
    {
        guideWidth = width;
        guideHeight = height;
    }

    // The game states its depth convention in the flags it created its own feature with, so there is no
    // reason to assume one.
    unsigned int createFlags = 0;
    params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &createFlags);
    g_nr.guideDepthInverted = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;

    // And it states how its motion vectors are encoded. Inventing a resolution ratio here meant handing
    // the model vectors it could not interpret.
    float mvScaleX = 1.0f;
    float mvScaleY = 1.0f;

    if (params->Get(NVSDK_NGX_Parameter_MV_Scale_X, &mvScaleX) != NVSDK_NGX_Result_Success)
        mvScaleX = 1.0f;

    if (params->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &mvScaleY) != NVSDK_NGX_Result_Success)
        mvScaleY = 1.0f;

    // Two factors, and both are needed. The game's own scale turns its vectors into render pixels --
    // Cyberpunk reports 1920 x 1080, so its vectors are normalised. The upscale ratio then carries
    // render pixels onto a display-resolution image. They coincide only at native resolution, which is
    // exactly where this was first tested.
    const float upscaleX = guideWidth != 0 ? (float) width / (float) guideWidth : 1.0f;
    const float upscaleY = guideHeight != 0 ? (float) height / (float) guideHeight : 1.0f;
    g_nr.guideMvScaleX = mvScaleX * upscaleX;
    g_nr.guideMvScaleY = mvScaleY * upscaleY;

    static bool reportedGuides = false;

    if (!reportedGuides)
    {
        reportedGuides = true;
        LOG_INFO("DLSS-NR guides: depth {}, motion vector scale {} x {} (the game says {} x {}, times "
                 "the {}x{} upscale ratio)",
                 g_nr.guideDepthInverted ? "inverted" : "not inverted", g_nr.guideMvScaleX,
                 g_nr.guideMvScaleY, mvScaleX, mvScaleY, upscaleX, upscaleY);
    }

    // When the model runs on the finished frame instead, this call exists only to take a copy of the
    // guides while they are still valid and still describe this frame. The split pipeline overrides the
    // choice: it calls for the in-place pass on its own intermediate, whatever the dropdown says.
    if (!forceInPlace && cfg.DlssNrInjectPoint.value_or_default() == INJECT_PRESENT)
    {
        if (CloneGuideAlways(device, cmdList, depth, &g_nr.depthClone) != nullptr &&
            CloneGuideAlways(device, cmdList, motion, &g_nr.motionClone) != nullptr)
        {
            g_nr.guideWidth = guideWidth;
            g_nr.guideHeight = guideHeight;
            g_nr.guidesReady = true;
        }

        device->Release();
        return;
    }

    if (!EnsureForwarder() || !EnsureCapabilityParams(device))
    {
        g_nr.failed = true;
        LOG_ERROR("DLSS-NR unavailable: {}", g_nr.reason);
        device->Release();
        return;
    }

    // What the model works at. The frame and its edit stay full resolution; only the model's input and
    // answer shrink, and the resolve enlarges the answer while compositing.
    float workScale = cfg.DlssNrWorkingScale.value_or_default();
    workScale = workScale < 0.25f ? 0.25f : (workScale > 1.0f ? 1.0f : workScale);
    const auto workWidth = (unsigned int) (width * workScale + 0.5f);
    const auto workHeight = (unsigned int) (height * workScale + 0.5f);
    const bool reduced = workWidth != width || workHeight != height;

    // The surfaces being replaced may last have been used by the present path, whose command lists this
    // pass did not record -- wait on its fence before touching them.
    if (g_nr.output != nullptr && g_nr.output->GetDesc().Format != desc.Format)
        WaitForAllSubmitted();

    ReleaseSurfacesIfFormatChanged(desc.Format);

    if (g_nr.feature != nullptr &&
        (g_nr.width != width || g_nr.height != height || g_nr.workWidth != workWidth ||
         g_nr.workHeight != workHeight))
    {
        // A resolution change invalidates the model and both scratch textures. Nothing is released
        // underneath the GPU here because this runs on the same command list the upscaler just used, and
        // the previous frame's work has long since retired by the time a resolution actually changes.
        g_nr.release(g_nr.feature);
        g_nr.feature = nullptr;

        if (g_nr.output != nullptr)
        {
            g_nr.output->Release();
            g_nr.output = nullptr;
        }

        if (g_nr.colorCopy != nullptr)
        {
            g_nr.colorCopy->Release();
            g_nr.colorCopy = nullptr;
        }

        if (g_nr.hdrCopy != nullptr)
        {
            g_nr.hdrCopy->Release();
            g_nr.hdrCopy = nullptr;
        }

        if (g_nr.colorSmall != nullptr)
        {
            g_nr.colorSmall->Release();
            g_nr.colorSmall = nullptr;
        }
    }

    if (g_nr.output == nullptr)
    {
        g_nr.output = CreateScratch(device, desc.Format, workWidth, workHeight);
        g_nr.colorCopy = CreateScratch(device, desc.Format, width, height);
        g_nr.hdrCopy = CreateScratch(device, desc.Format, width, height);
        g_nr.workWidth = workWidth;
        g_nr.workHeight = workHeight;
    }

    if (reduced && g_nr.colorSmall == nullptr)
        g_nr.colorSmall = CreateScratch(device, desc.Format, workWidth, workHeight);

    if (g_nr.feature == nullptr && g_nr.output != nullptr && g_nr.colorCopy != nullptr &&
        g_nr.hdrCopy != nullptr)
    {
        auto snippet = Util::FindFilePath(g_dllDir, "nvngx_dlssnr.dll");

        if (!snippet.has_value())
            snippet = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx_dlssnr.dll");

        if (!snippet.has_value())
        {
            g_nr.failed = true;
            g_nr.reason = "nvngx_dlssnr.dll was not found beside OptiScaler or the game";
            LOG_ERROR("DLSS-NR unavailable: {}", g_nr.reason);
            device->Release();
            return;
        }

        g_nr.feature =
            g_nr.create(snippet->wstring().c_str(), State::Instance().NVNGX_ApplicationDataPath.c_str(),
                        device, cmdList, g_nr.capabilityParams, workWidth, workHeight,
                        (int) cfg.DlssNrPreset.value_or_default(),
                        cfg.DlssNrIntensity.value_or_default(), (int) cfg.DlssNrStyle.value_or_default(),
                        cfg.DlssNrLocalStructure.value_or_default(), cfg.DlssNrLocalTone.value_or_default(),
                        cfg.DlssNrSkinStructure.value_or_default(),
                        cfg.DlssNrAutoMask.value_or_default() ? 1 : 0,
                        cfg.DlssNrUiCorrection.value_or_default() ? 1 : 0);

        if (g_nr.feature == nullptr)
        {
            g_nr.failed = true;
            g_nr.reason = "the model would not initialise";
            LOG_ERROR("DLSS-NR create failed: init 0x{:X}, create 0x{:X}",
                      g_nr.lastInit != nullptr ? *g_nr.lastInit : 0,
                      g_nr.lastCreate != nullptr ? *g_nr.lastCreate : 0);
            device->Release();
            return;
        }

        g_nr.width = width;
        g_nr.height = height;
        g_nr.reset = true;
        RecordBuiltTuning(cfg);
        LOG_INFO("DLSS-NR running at {}x{}, guides {}x{} (preset {}, intensity {}, style {})", width,
                 height, guideWidth, guideHeight, g_nr.builtPreset, g_nr.builtIntensity, g_nr.builtStyle);
    }

    if (g_nr.feature == nullptr)
    {
        device->Release();
        return;
    }

    // The upscaler has just written this, so it is a UAV. The model needs it readable.
    // Whether the buffer the upscaler just wrote is linear HDR or an already tone-mapped picture is not
    // something to assume: the game says so, in the flags it created its own DLSS feature with. Running
    // the colour transform over a frame that has already been through a tonemapper is pure damage, and
    // skipping it on one that has not leaves the model reading ordinary values as enormously bright.
    unsigned int dlssFlags = 0;
    params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &dlssFlags);
    const bool isHdrBuffer = (dlssFlags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) != 0;

    static bool reportedHdr = false;

    if (!reportedHdr)
    {
        reportedHdr = true;
        LOG_INFO("DLSS-NR: the game's DLSS buffer is {} (create flags 0x{:X}), so the colour transform is {}",
                 isHdrBuffer ? "linear HDR" : "already tone-mapped", dlssFlags,
                 isHdrBuffer ? "on" : "off");
    }

    const bool haveCodec = g_codec.ensure(device);

    if (!haveCodec)
    {
        g_nr.failed = true;
        g_nr.reason = "the colour codec would not compile";
        LOG_ERROR("DLSS-NR unavailable: {}", g_nr.reason);
        device->Release();
        return;
    }

    // What the upscaler produces is linear HDR with an open-ended range; the model was trained on
    // finished, sRGB-encoded frames. The white point is what maps one to the other, and it is a property
    // of the game's exposure rather than a number worth asking anyone to guess: measured means of 0.065,
    // 1.8 and 185 have all been seen in this one game.
    ++g_frames;
    const bool autoWhite = cfg.DlssNrAutoWhitePoint.value_or_default();

    if (autoWhite)
    {
        const probe::Stats stats = g_reader.collect();

        if (stats.valid && stats.meanLuma > 0.0f)
        {
            const float target = WhitePointForMean(stats.meanLuma);

            if (!g_autoWhitePointSettled)
            {
                // Nothing to ease away from on the first reading, and easing in from a wrong default is
                // just a slow wrong answer.
                g_autoWhitePoint = target;
                g_autoWhitePointSettled = true;
                LOG_INFO("DLSS-NR white point settled at {:.3f} (frame mean {:.4f})", g_autoWhitePoint,
                         stats.meanLuma);
            }
            else
            {
                g_autoWhitePoint += (target - g_autoWhitePoint) * kWhitePointBlend;
            }
        }
    }

    const float whitePoint = autoWhite && g_autoWhitePointSettled
                                 ? g_autoWhitePoint
                                 : cfg.DlssNrWhitePoint.value_or_default();

    if (g_gpuTime == nullptr)
        g_gpuTime = std::make_unique<GpuTime_Dx12>(device);

    if (g_gpuTime != nullptr)
        g_gpuTime->Start(cmdList);

    codec::Params encodeParams {};
    encodeParams.mode = codec::MODE_ENCODE;
    // A frame that is already display-referred is handed over untouched: the encode becomes a copy and
    // the resolve adds the model's edit back at full scale.
    encodeParams.passthrough = isHdrBuffer ? 0u : 1u;
    encodeParams.whitePoint = whitePoint;
    encodeParams.width = width;
    encodeParams.height = height;

    Barrier(cmdList, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g_codec.dispatch(cmdList, encodeParams, target, nullptr, nullptr, g_nr.colorCopy, g_nr.hdrCopy);

    // Measuring here, while the frame is already readable, costs one dispatch every so often and no
    // extra barriers. Twice a second is far more often than an exposure meaningfully moves.
    if (autoWhite && (g_frames % 30 == 0) && g_reducer.ensure(device))
    {
        ID3D12Resource* reducedFrame = g_reducer.dispatch(cmdList, target, width, height);
        g_reader.capture(cmdList, reducedFrame, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    Barrier(cmdList, target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // The transitions double as the wait for the encode's writes.
    Barrier(cmdList, g_nr.colorCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cmdList, g_nr.hdrCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // Below full resolution the model is shown a filtered shrink of the proxy; the edit it returns is
    // enlarged during the resolve while the frame underneath stays full size and untouched.
    ID3D12Resource* modelInput = g_nr.colorCopy;

    if (reduced && g_nr.colorSmall != nullptr)
    {
        codec::Params down {};
        down.mode = codec::MODE_DOWNSAMPLE;
        down.width = workWidth;
        down.height = workHeight;
        g_codec.dispatch(cmdList, down, g_nr.colorCopy, nullptr, nullptr, g_nr.colorSmall, nullptr);
        Barrier(cmdList, g_nr.colorSmall, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        modelInput = g_nr.colorSmall;
    }

    ID3D12Resource* depthIn = ReadableGuide(device, cmdList, depth, &g_nr.depthClone);
    ID3D12Resource* motionIn = ReadableGuide(device, cmdList, motion, &g_nr.motionClone);

    if (depthIn == nullptr || motionIn == nullptr)
    {
        g_nr.failed = true;
        g_nr.reason = "the game's depth or motion vectors could not be made readable";
        LOG_ERROR("DLSS-NR unavailable: {}", g_nr.reason);
        device->Release();
        return;
    }

    // The vectors were scaled to full-frame pixels; the image the model reprojects is the working size.
    const float mvToWork = width != 0 ? (float) workWidth / (float) width : 1.0f;

    const int result = g_nr.evaluate(
        cmdList, g_nr.feature, g_nr.capabilityParams, modelInput, depthIn, motionIn, g_nr.output,
        workWidth, workHeight, guideWidth, guideHeight, g_nr.guideDepthInverted ? 1 : 0,
        g_nr.reset ? 1 : 0, cfg.DlssNrIntensity.value_or_default(),
        (int) cfg.DlssNrStyle.value_or_default(), cfg.DlssNrLocalStructure.value_or_default(),
        cfg.DlssNrLocalTone.value_or_default(), cfg.DlssNrSkinStructure.value_or_default(),
        cfg.DlssNrAutoMask.value_or_default() ? 1 : 0, g_nr.guideMvScaleX * mvToWork,
        g_nr.guideMvScaleY * mvToWork);

    g_nr.reset = false;

    // Once, a few seconds in, so it lands after the values have been written at least once.
    static bool tuningReported = false;

    if (!tuningReported && g_frames > 240)
    {
        tuningReported = true;

        auto report = [](const char* name)
        {
            float value = 0.0f;
            const NVSDK_NGX_Result r = g_nr.capabilityParams->Get(name, &value);
            LOG_INFO("DLSS-NR readback {} -> {} (result 0x{:X})", name, value, (uint32_t) r);
        };

        report("DLSSNR.Intensity");
        report("DLSSNR.LocalStructureStrength");
        report("DLSSNR.LocalToneStrength");
        report("DLSSNR.SkinStructureStrength");

        unsigned int style = 0;
        const NVSDK_NGX_Result styleResult = g_nr.capabilityParams->Get("DLSSNR.Style", &style);
        LOG_INFO("DLSS-NR readback DLSSNR.Style -> {} (result 0x{:X})", style, (uint32_t) styleResult);

        // The preset is the last control whose arrival has never been checked, and three of them look
        // identical in play. Either it is not landing or the presets really are alike.
        unsigned int preset = 0;
        const NVSDK_NGX_Result presetResult =
            g_nr.capabilityParams->Get("DLSSNR.Hint.Render.Preset", &preset);
        LOG_INFO("DLSS-NR readback DLSSNR.Hint.Render.Preset -> {} (result 0x{:X}, we wrote {})", preset,
                 (uint32_t) presetResult, cfg.DlssNrPreset.value_or_default());

        LOG_INFO("DLSS-NR wrote intensity {}, local structure {}, local tone {}, skin {}, style {}",
                 cfg.DlssNrIntensity.value_or_default(), cfg.DlssNrLocalStructure.value_or_default(),
                 cfg.DlssNrLocalTone.value_or_default(), cfg.DlssNrSkinStructure.value_or_default(),
                 cfg.DlssNrStyle.value_or_default());
    }

    if (result == NVSDK_NGX_Result_Success)
    {
        // Resolve takes the difference between what the model returned and what it was shown, and adds
        // that back to the frame. At strength zero the result is what the upscaler produced, exactly, and
        // anything the model left alone is untouched rather than round-tripped through the curve.
        codec::Params resolveParams {};
        resolveParams.mode = codec::MODE_RESOLVE;
        resolveParams.whitePoint = whitePoint;
        resolveParams.width = width;
        resolveParams.height = height;
        resolveParams.transferStrength = cfg.DlssNrTransferStrength.value_or_default();
        resolveParams.colourStrength = cfg.DlssNrColourStrength.value_or_default();
        resolveParams.debugView = cfg.DlssNrDebugView.value_or_default();
        resolveParams.maxRatio = cfg.DlssNrMaxRatio.value_or_default();
        resolveParams.passthrough = isHdrBuffer ? 0u : 1u;

        // The accumulator: this frame's edit blended with its own reprojected history, carried to where
        // each surface is now by the same motion vectors the model was given. The model re-decides a
        // measured fraction of its edit every frame even on a static scene; the consistent part -- the
        // detail -- survives this average and the re-randomised part cancels.
        const float stability = cfg.DlssNrEditStability.value_or_default();
        ID3D12Resource* historyIn = nullptr;
        ID3D12Resource* historyOut = nullptr;

        if (stability > 0.0f && motionIn != nullptr)
        {
            if (g_nr.editHistory[0] != nullptr &&
                ((unsigned int) g_nr.editHistory[0]->GetDesc().Width != width ||
                 g_nr.editHistory[0]->GetDesc().Height != height))
            {
                for (auto& h : g_nr.editHistory)
                {
                    h->Release();
                    h = nullptr;
                }

                g_nr.editWarm = false;
            }

            if (g_nr.editHistory[0] == nullptr)
            {
                g_nr.editHistory[0] = CreateScratch(device, DXGI_FORMAT_R16G16B16A16_FLOAT, width, height);
                g_nr.editHistory[1] = CreateScratch(device, DXGI_FORMAT_R16G16B16A16_FLOAT, width, height);
                g_nr.editWarm = false;
            }

            if (g_nr.editHistory[0] != nullptr && g_nr.editHistory[1] != nullptr)
            {
                historyIn = g_nr.editHistory[g_nr.editIndex];
                historyOut = g_nr.editHistory[1 - g_nr.editIndex];

                // 2 restarts the history: the first frame after a build or reset has nothing behind it.
                resolveParams.accumulate = g_nr.editWarm ? 1u : 2u;
                resolveParams.stability = stability > 0.95f ? 0.95f : stability;
                resolveParams.mvScaleX = g_nr.guideMvScaleX;
                resolveParams.mvScaleY = g_nr.guideMvScaleY;
                resolveParams.guideWidth = guideWidth;
                resolveParams.guideHeight = guideHeight;

                Barrier(cmdList, historyIn, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
        }

        Barrier(cmdList, g_nr.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        g_codec.dispatch(cmdList, resolveParams, modelInput, g_nr.output, g_nr.hdrCopy, target,
                         historyOut, motionIn, historyIn);
        Barrier(cmdList, g_nr.output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        if (historyIn != nullptr)
        {
            Barrier(cmdList, historyIn, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            g_nr.editIndex = 1 - g_nr.editIndex;
            g_nr.editWarm = true;
        }
    }
    else
    {
        g_nr.failed = true;
        g_nr.reason = "the model refused to run";
        LOG_ERROR("DLSS-NR evaluate returned 0x{:X}, disabling for this session", (uint32_t) result);
    }

    Barrier(cmdList, g_nr.hdrCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if (g_gpuTime != nullptr)
    {
        g_gpuTime->End(cmdList);

        // This path records into the game's own list, so there is no queue of ours to read from. The
        // one the upscaler was invoked on serves.
        if (State::Instance().currentCommandQueue != nullptr)
        {
            if (auto ms = g_gpuTime->ReadGpuTime((ID3D12CommandQueue*) State::Instance().currentCommandQueue);
                ms.has_value())
                g_lastGpuTime = ms;
        }
    }

    // Put any guide clones back where the next frame's copy expects to find them.
    if (g_nr.depthClone != nullptr)
        Barrier(cmdList, g_nr.depthClone, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);

    if (g_nr.motionClone != nullptr)
        Barrier(cmdList, g_nr.motionClone, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);

    if (reduced && g_nr.colorSmall != nullptr)
        Barrier(cmdList, g_nr.colorSmall, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Leave the staging copy as the next frame expects to find it.
    Barrier(cmdList, g_nr.colorCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    device->Release();
}

void EvaluateAtPresent(ID3D12CommandQueue* queue, ID3D12Resource* backBuffer, unsigned int backBufferIndex)
{
    const Config& cfg = *Config::Instance();

    if (!cfg.DlssNrEnabled.value_or_default() || g_nr.failed || queue == nullptr || backBuffer == nullptr)
        return;

    if (cfg.DlssNrInjectPoint.value_or_default() != INJECT_PRESENT)
        return;

    // The split pipeline already ran the model this frame, on its own intermediate.
    if (g_splitActive)
        return;

    // Nothing to work from until the upscaler has run at least once and left its guides behind.
    if (!g_nr.guidesReady || g_nr.depthClone == nullptr || g_nr.motionClone == nullptr)
        return;

    ID3D12Device* device = nullptr;

    if (FAILED(backBuffer->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
        return;

    const D3D12_RESOURCE_DESC desc = backBuffer->GetDesc();
    const auto width = (unsigned int) desc.Width;
    const auto height = desc.Height;

    // A swapchain buffer is not generally usable as a shader resource, so the frame is staged through
    // textures this owns. The scratch format drops any sRGB view, which cannot be bound as a typed UAV;
    // the bits are the same and the model wants them exactly as they are.
    const DXGI_FORMAT scratchFormat = codec::TypedFormat(desc.Format);

    if (!EnsureForwarder() || !EnsureCapabilityParams(device) || !EnsurePresentList(device) ||
        !g_codec.ensure(device))
    {
        g_nr.failed = true;
        LOG_ERROR("DLSS-NR unavailable: {}", g_nr.reason);
        device->Release();
        return;
    }

    // What the model works at. The frame is never reduced; only this is.
    float scale = cfg.DlssNrWorkingScale.value_or_default();
    scale = scale < 0.25f ? 0.25f : (scale > 1.0f ? 1.0f : scale);
    const auto workWidth = (unsigned int) (width * scale + 0.5f);
    const auto workHeight = (unsigned int) (height * scale + 0.5f);
    const bool reduced = workWidth != width || workHeight != height;

    if (g_nr.output != nullptr && g_nr.output->GetDesc().Format != scratchFormat)
        WaitForAllSubmitted();

    ReleaseSurfacesIfFormatChanged(scratchFormat);

    if (g_nr.feature != nullptr &&
        (g_nr.width != width || g_nr.height != height || g_nr.workWidth != workWidth ||
         g_nr.workHeight != workHeight))
    {
        WaitForAllSubmitted();
        g_nr.release(g_nr.feature);
        g_nr.feature = nullptr;

        if (g_nr.output != nullptr)
        {
            g_nr.output->Release();
            g_nr.output = nullptr;
        }

        if (g_nr.colorCopy != nullptr)
        {
            g_nr.colorCopy->Release();
            g_nr.colorCopy = nullptr;
        }

        if (g_nr.hdrCopy != nullptr)
        {
            g_nr.hdrCopy->Release();
            g_nr.hdrCopy = nullptr;
        }

        if (g_nr.colorSmall != nullptr)
        {
            g_nr.colorSmall->Release();
            g_nr.colorSmall = nullptr;
        }
    }

    if (g_nr.output == nullptr)
    {
        // The model's own images are the working size; the frame's copies stay full size.
        g_nr.output = CreateScratch(device, scratchFormat, workWidth, workHeight);
        g_nr.colorCopy = CreateScratch(device, scratchFormat, width, height);
        // The resolve cannot write the back buffer directly: a swapchain buffer is not created for
        // unordered access. It writes here and this is copied over the frame.
        g_nr.hdrCopy = CreateScratch(device, scratchFormat, width, height);

        if (reduced)
            g_nr.colorSmall = CreateScratch(device, scratchFormat, workWidth, workHeight);

        g_nr.workWidth = workWidth;
        g_nr.workHeight = workHeight;
    }

    if (g_nr.output == nullptr || g_nr.colorCopy == nullptr || g_nr.hdrCopy == nullptr)
    {
        g_nr.failed = true;
        g_nr.reason = "the staging textures could not be created";
        device->Release();
        return;
    }

    // A tuning change means a new model, since the values are only read when one is built.
    if (g_nr.feature != nullptr && !TuningMatchesFeature(cfg))
    {
        if (g_nr.settledAt == 0)
            g_nr.settledAt = g_frames;

        if (g_frames - g_nr.settledAt >= kSettleFrames)
        {
            WaitForAllSubmitted();
            g_nr.release(g_nr.feature);
            g_nr.feature = nullptr;
            g_nr.settledAt = 0;
            LOG_INFO("DLSS-NR rebuilding for changed tuning");
        }
    }
    else
    {
        g_nr.settledAt = 0;
    }

    ++g_frames;

    const unsigned int slot = backBufferIndex % kPresentAllocators;
    ID3D12CommandAllocator* allocator = g_nr.presentAllocators[slot];

    // Waits for the previous run of this pass, not merely for this allocator's own last use. One set of
    // scratch textures and one model feature are shared across every frame, so letting three run at once
    // meant one frame's evaluate could still be reading the staging copy while the next overwrote it --
    // and the feature carries temporal history, which is not something to run three copies of.
    WaitForAllSubmitted();

    if (FAILED(allocator->Reset()) || FAILED(g_nr.presentList->Reset(allocator, nullptr)))
    {
        device->Release();
        return;
    }

    ID3D12GraphicsCommandList* cmdList = g_nr.presentList;

    if (g_gpuTime == nullptr)
        g_gpuTime = std::make_unique<GpuTime_Dx12>(device);

    if (g_nr.feature == nullptr)
    {
        auto snippet = Util::FindFilePath(g_dllDir, "nvngx_dlssnr.dll");

        if (!snippet.has_value())
            snippet = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx_dlssnr.dll");

        if (!snippet.has_value())
        {
            g_nr.failed = true;
            g_nr.reason = "nvngx_dlssnr.dll was not found beside OptiScaler or the game";
            LOG_ERROR("DLSS-NR unavailable: {}", g_nr.reason);
            cmdList->Close();
            device->Release();
            return;
        }

        g_nr.feature =
            g_nr.create(snippet->wstring().c_str(), State::Instance().NVNGX_ApplicationDataPath.c_str(),
                        device, cmdList, g_nr.capabilityParams, workWidth, workHeight,
                        (int) cfg.DlssNrPreset.value_or_default(),
                        cfg.DlssNrIntensity.value_or_default(), (int) cfg.DlssNrStyle.value_or_default(),
                        cfg.DlssNrLocalStructure.value_or_default(), cfg.DlssNrLocalTone.value_or_default(),
                        cfg.DlssNrSkinStructure.value_or_default(),
                        cfg.DlssNrAutoMask.value_or_default() ? 1 : 0,
                        cfg.DlssNrUiCorrection.value_or_default() ? 1 : 0);

        if (g_nr.feature == nullptr)
        {
            g_nr.failed = true;
            g_nr.reason = "the model would not initialise";
            LOG_ERROR("DLSS-NR create failed at present: init 0x{:X}, create 0x{:X}",
                      g_nr.lastInit != nullptr ? *g_nr.lastInit : 0,
                      g_nr.lastCreate != nullptr ? *g_nr.lastCreate : 0);
            cmdList->Close();
            device->Release();
            return;
        }

        g_nr.width = width;
        g_nr.height = height;
        g_nr.reset = true;
        RecordBuiltTuning(cfg);

        {
            unsigned int presetBack = 0;
            const NVSDK_NGX_Result r =
                g_nr.capabilityParams->Get("DLSSNR.Hint.Render.Preset", &presetBack);
            LOG_INFO("DLSS-NR readback DLSSNR.Hint.Render.Preset -> {} (result 0x{:X}, we wrote {})",
                     presetBack, (uint32_t) r, cfg.DlssNrPreset.value_or_default());
        }

        LOG_INFO("DLSS-NR running on the finished frame at {}x{}, guides {}x{} (preset {}, intensity {}, "
                 "style {}, local structure {}, local tone {}, skin {}, ui correction {})",
                 width, height, g_nr.guideWidth, g_nr.guideHeight, g_nr.builtPreset, g_nr.builtIntensity,
                 g_nr.builtStyle, g_nr.builtLocalStructure, g_nr.builtLocalTone, g_nr.builtSkinStructure,
                 cfg.DlssNrUiCorrection.value_or_default() ? 1 : 0);
    }

    // Timed across the whole pass: the staging copies and the resolve are part of what this costs, and
    // timing only the model would flatter the number.
    if (g_gpuTime != nullptr)
        g_gpuTime->Start(cmdList);

    // The frame is already display-referred here -- it has been through the game's own tonemapper --
    // so it goes to the model exactly as it is. No encode, no white point, nothing to invert.
    Barrier(cmdList, backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(cmdList, g_nr.colorCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->CopyResource(g_nr.colorCopy, backBuffer);
    Barrier(cmdList, g_nr.colorCopy, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // Shrink the frame for the model, when it is working below full resolution. The copy above stays at
    // full size and is what the edit is finally added to.
    ID3D12Resource* modelInput = g_nr.colorCopy;

    if (reduced && g_nr.colorSmall != nullptr)
    {
        codec::Params down {};
        down.mode = codec::MODE_DOWNSAMPLE;
        down.width = workWidth;
        down.height = workHeight;
        g_codec.dispatch(cmdList, down, g_nr.colorCopy, nullptr, nullptr, g_nr.colorSmall, nullptr);
        Barrier(cmdList, g_nr.colorSmall, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        modelInput = g_nr.colorSmall;
    }

    Barrier(cmdList, g_nr.depthClone, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cmdList, g_nr.motionClone, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // The vectors were scaled to full-frame pixels; the image the model reprojects is the working size.
    const float mvToWork = width != 0 ? (float) workWidth / (float) width : 1.0f;

    const int result = g_nr.evaluate(
        cmdList, g_nr.feature, g_nr.capabilityParams, modelInput, g_nr.depthClone, g_nr.motionClone,
        g_nr.output, workWidth, workHeight, g_nr.guideWidth, g_nr.guideHeight,
        g_nr.guideDepthInverted ? 1 : 0,
        g_nr.reset ? 1 : 0,
        cfg.DlssNrIntensity.value_or_default(), (int) cfg.DlssNrStyle.value_or_default(),
        cfg.DlssNrLocalStructure.value_or_default(), cfg.DlssNrLocalTone.value_or_default(),
        cfg.DlssNrSkinStructure.value_or_default(), cfg.DlssNrAutoMask.value_or_default() ? 1 : 0,
        g_nr.guideMvScaleX * mvToWork, g_nr.guideMvScaleY * mvToWork);

    g_nr.reset = false;

    // The motion clone stays readable through the resolve: the accumulator reprojects the edit's
    // history with it, in the same dispatch that applies the edit.
    Barrier(cmdList, g_nr.depthClone, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_DEST);

    if (result == NVSDK_NGX_Result_Success)
    {
        // The frame the model was shown and the frame as it was are the same thing here, because nothing
        // was converted on the way in. So the resolve adds the model's edit to the frame rather than
        // replacing it: at strength zero the result is the original, bit for bit.
        codec::Params resolveParams {};
        resolveParams.mode = codec::MODE_RESOLVE;
        resolveParams.passthrough = 1;
        resolveParams.whitePoint = 1.0f;
        resolveParams.width = width;
        resolveParams.height = height;
        resolveParams.transferStrength = cfg.DlssNrTransferStrength.value_or_default();
        resolveParams.colourStrength = cfg.DlssNrColourStrength.value_or_default();
        resolveParams.debugView = cfg.DlssNrDebugView.value_or_default();
        resolveParams.maxRatio = cfg.DlssNrMaxRatio.value_or_default();

        // The same accumulator the before-frame-generation path has: the edit blended with its own
        // reprojected history. With frame generation, generated frames share a rendered frame's motion
        // vectors, which is close enough for an edit this small.
        const float stability = cfg.DlssNrEditStability.value_or_default();
        ID3D12Resource* historyIn = nullptr;
        ID3D12Resource* historyOut = nullptr;

        if (stability > 0.0f && g_nr.motionClone != nullptr)
        {
            if (g_nr.editHistory[0] != nullptr &&
                ((unsigned int) g_nr.editHistory[0]->GetDesc().Width != width ||
                 g_nr.editHistory[0]->GetDesc().Height != height))
            {
                for (auto& h : g_nr.editHistory)
                {
                    h->Release();
                    h = nullptr;
                }

                g_nr.editWarm = false;
            }

            if (g_nr.editHistory[0] == nullptr)
            {
                g_nr.editHistory[0] = CreateScratch(device, DXGI_FORMAT_R16G16B16A16_FLOAT, width, height);
                g_nr.editHistory[1] = CreateScratch(device, DXGI_FORMAT_R16G16B16A16_FLOAT, width, height);
                g_nr.editWarm = false;
            }

            if (g_nr.editHistory[0] != nullptr && g_nr.editHistory[1] != nullptr)
            {
                historyIn = g_nr.editHistory[g_nr.editIndex];
                historyOut = g_nr.editHistory[1 - g_nr.editIndex];

                resolveParams.accumulate = g_nr.editWarm ? 1u : 2u;
                resolveParams.stability = stability > 0.95f ? 0.95f : stability;
                resolveParams.mvScaleX = g_nr.guideMvScaleX;
                resolveParams.mvScaleY = g_nr.guideMvScaleY;
                resolveParams.guideWidth = g_nr.guideWidth;
                resolveParams.guideHeight = g_nr.guideHeight;

                Barrier(cmdList, historyIn, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
        }

        Barrier(cmdList, g_nr.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        g_codec.dispatch(cmdList, resolveParams, modelInput, g_nr.output, g_nr.colorCopy, g_nr.hdrCopy,
                         historyOut, g_nr.motionClone, historyIn);
        Barrier(cmdList, g_nr.output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        if (historyIn != nullptr)
        {
            Barrier(cmdList, historyIn, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            g_nr.editIndex = 1 - g_nr.editIndex;
            g_nr.editWarm = true;
        }

        Barrier(cmdList, g_nr.hdrCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COPY_SOURCE);

        if (!g_autoCaptureDone && cfg.DlssNrAutoCapture.value_or_default() &&
            g_frames > kAutoCaptureAfterFrames)
        {
            g_autoCaptureDone = true;
            ClearCaptureDirectory();
            g_capture.request(capture::kMaxFrames);
        }

        // The frame as the upscaler produced it, and the same frame after the edit. Both are here, this
        // instant, for the same frame -- which is the whole point.
        if (g_capture.isActive())
            g_capture.record(cmdList, device, g_nr.colorCopy,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, g_nr.hdrCopy,
                             D3D12_RESOURCE_STATE_COPY_SOURCE);

        Barrier(cmdList, backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        cmdList->CopyResource(backBuffer, g_nr.hdrCopy);
        Barrier(cmdList, g_nr.hdrCopy, D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Barrier(cmdList, backBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    }
    else
    {
        Barrier(cmdList, backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        g_nr.failed = true;
        g_nr.reason = "the model refused to run";
        LOG_ERROR("DLSS-NR evaluate at present returned 0x{:X}, disabling for this session",
                  (uint32_t) result);
    }

    Barrier(cmdList, g_nr.motionClone, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_DEST);

    Barrier(cmdList, g_nr.colorCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if (reduced && g_nr.colorSmall != nullptr)
        Barrier(cmdList, g_nr.colorSmall, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if (g_gpuTime != nullptr)
        g_gpuTime->End(cmdList);

    if (SUCCEEDED(cmdList->Close()))
    {
        ID3D12CommandList* lists[] = { cmdList };
        queue->ExecuteCommandLists(1, lists);

        // Read after submitting: the result is from an earlier frame, which is what the upscaler's own
        // timings do too.
        if (g_gpuTime != nullptr)
        {
            if (auto ms = g_gpuTime->ReadGpuTime(queue); ms.has_value())
                g_lastGpuTime = ms;
        }

        // Recorded against this allocator, so the next pass round the ring knows what to wait for.
        ++g_nr.presentFenceNext;

        if (SUCCEEDED(queue->Signal(g_nr.presentFence, g_nr.presentFenceNext)))
            g_nr.presentFenceValues[slot] = g_nr.presentFenceNext;

        if (g_capture.readyToWrite())
        {
            WaitForAllSubmitted();
            const auto dir = Util::DllPath().remove_filename() / "dlssnr-capture";
            const auto written = g_capture.write(dir);

            if (!written.empty())
                LOG_INFO("DLSS-NR wrote matched before/after frames to {}", written);
        }
    }

    device->Release();
}

bool IsRunning() { return g_nr.feature != nullptr && !g_nr.failed; }

const char* FailureReason() { return g_nr.failed ? g_nr.reason : ""; }

float CurrentWhitePoint() { return g_autoWhitePointSettled ? g_autoWhitePoint : 0.0f; }

std::optional<double> LastGpuTime() { return g_lastGpuTime; }

void RequestCapture(unsigned int frames)
{
    ClearCaptureDirectory();
    g_capture.request(frames);
}

bool CaptureInProgress() { return g_capture.isActive(); }

void Shutdown()
{
    if (g_nr.feature != nullptr && g_nr.release != nullptr)
        g_nr.release(g_nr.feature);

    g_nr.feature = nullptr;

    if (g_nr.output != nullptr)
    {
        g_nr.output->Release();
        g_nr.output = nullptr;
    }

    if (g_nr.colorCopy != nullptr)
    {
        g_nr.colorCopy->Release();
        g_nr.colorCopy = nullptr;
    }

    if (g_nr.hdrCopy != nullptr)
    {
        g_nr.hdrCopy->Release();
        g_nr.hdrCopy = nullptr;
    }

    if (g_nr.colorSmall != nullptr)
    {
        g_nr.colorSmall->Release();
        g_nr.colorSmall = nullptr;
    }

    for (auto& h : g_nr.editHistory)
    {
        if (h != nullptr)
        {
            h->Release();
            h = nullptr;
        }
    }

    if (g_nr.depthClone != nullptr)
    {
        g_nr.depthClone->Release();
        g_nr.depthClone = nullptr;
    }

    if (g_nr.motionClone != nullptr)
    {
        g_nr.motionClone->Release();
        g_nr.motionClone = nullptr;
    }

    if (g_nr.presentList != nullptr)
    {
        g_nr.presentList->Release();
        g_nr.presentList = nullptr;
    }

    if (g_nr.presentFence != nullptr)
    {
        g_nr.presentFence->Release();
        g_nr.presentFence = nullptr;
    }

    if (g_nr.presentFenceEvent != nullptr)
    {
        CloseHandle(g_nr.presentFenceEvent);
        g_nr.presentFenceEvent = nullptr;
    }

    for (unsigned int i = 0; i < kPresentAllocators; ++i)
    {
        if (g_nr.presentAllocators[i] != nullptr)
        {
            g_nr.presentAllocators[i]->Release();
            g_nr.presentAllocators[i] = nullptr;
        }
    }

    g_capture.release();
    g_gpuTime.reset();
    g_lastGpuTime.reset();

    g_codec.destroy();
    g_reducer.destroy();
    g_reader.destroy();
}
} // namespace DlssNr
