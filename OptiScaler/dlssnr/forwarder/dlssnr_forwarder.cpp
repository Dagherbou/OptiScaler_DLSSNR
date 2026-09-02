// DLSS Neural Rendering calls, isolated in a module the snippet will accept as a caller.
//
// The snippet resolves the module owning its return address and requires that module's path to contain
// "nvngx.dll" (the driver core is _nvngx.dll), rejecting anything else with FAIL_PlatformError before it
// inspects a single argument. Neither a ReShade add-on nor OptiScaler is named anything like that, so the
// calls are made from here instead and reached through the exports below.
//
// The parameter block is the core's capability block rather than a fresh one: it carries the snippet and
// preset callbacks a feature expects at create time. The core exports no Set/Get helpers (they are
// static-library inlines), so it is driven through its vtable. NVSDK_NGX_Parameter declares eight Set
// overloads then eight Get overloads, in this order: ULL, float, double, uint, int, ID3D11Resource*,
// ID3D12Resource*, void*.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>

namespace {

// Slot indices confirmed by round-tripping values through the live block: a setter at N is read back by
// the getter at N+8. The unsigned setter is slot 3 (a feature create driven through it succeeds) and the
// resource getter answers at slot 8, so resources are written through slot 0 -- the 64-bit setter, which
// is what a resource handle is. Writing them through the typed D3D12 setter left them unset.
constexpr int VT_SET_ULL = 0;
// Where the float setter actually lives. The public header declares it at slot 1, and this block --
// the driver's own, not the header's implementation -- does not keep a float there: every float written
// to slot 1 reads back as FAIL_UnsupportedParameter while every uint lands. The host discovers the real
// slot by round-tripping a value and sets it here before anything else is written.
int g_floatSlot = 1;
constexpr int VT_SET_UINT = 3;

using PFN_SetULL = void(__thiscall *)(void *, const char *, unsigned long long);
using PFN_SetFloat = void(__thiscall *)(void *, const char *, float);
using PFN_SetUInt = void(__thiscall *)(void *, const char *, unsigned int);

void setUInt(void *params, const char *name, unsigned int v) {
    void **vt = *reinterpret_cast<void ***>(params);
    reinterpret_cast<PFN_SetUInt>(vt[VT_SET_UINT])(params, name, v);
}

void setFloat(void *params, const char *name, float v) {
    void **vt = *reinterpret_cast<void ***>(params);
    reinterpret_cast<PFN_SetFloat>(vt[g_floatSlot])(params, name, v);
}

void setResourcePtr(void *params, const char *name, void *v) {
    void **vt = *reinterpret_cast<void ***>(params);
    reinterpret_cast<PFN_SetULL>(vt[VT_SET_ULL])(params, name, (unsigned long long) v);
}

void setResource(void *params, const char *name, ID3D12Resource *v) {
    void **vt = *reinterpret_cast<void ***>(params);
    reinterpret_cast<PFN_SetULL>(vt[VT_SET_ULL])(params, name, (unsigned long long) v);
}

using PFN_NrInitExt = int(__cdecl *)(unsigned long long, const wchar_t *, ID3D12Device *, int,
                                     const void *);
using PFN_NrCreate = int(__cdecl *)(ID3D12GraphicsCommandList *, int, const void *, void **);
using PFN_NrEvaluate = int(__cdecl *)(ID3D12GraphicsCommandList *, const void *, const void *, void *);
using PFN_NrRelease = int(__cdecl *)(void *);

struct Snippet {
    HMODULE module = nullptr;
    PFN_NrInitExt init = nullptr;
    PFN_NrCreate create = nullptr;
    PFN_NrEvaluate evaluate = nullptr;
    PFN_NrRelease release = nullptr;
    bool initialised = false;

};

Snippet g_snip;

bool loadSnippet(const wchar_t *path) {
    if (g_snip.module) {
        return g_snip.create != nullptr;
    }
    g_snip.module = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_snip.module) {
        return false;
    }
    g_snip.init = (PFN_NrInitExt) GetProcAddress(g_snip.module, "NVSDK_NGX_D3D12_Init_Ext");
    g_snip.create = (PFN_NrCreate) GetProcAddress(g_snip.module, "NVSDK_NGX_D3D12_CreateFeature");
    g_snip.evaluate = (PFN_NrEvaluate) GetProcAddress(g_snip.module, "NVSDK_NGX_D3D12_EvaluateFeature");
    g_snip.release = (PFN_NrRelease) GetProcAddress(g_snip.module, "NVSDK_NGX_D3D12_ReleaseFeature");


    return g_snip.create != nullptr && g_snip.evaluate != nullptr;
}


} // namespace

extern "C" {

// Called once, after the host has worked out which slot this block keeps floats in.
__declspec(dllexport) void dlssnr_call_set_float_slot(int slot) {
    if (slot >= 0 && slot < 8) {
        g_floatSlot = slot;
    }
}

// Writes a float through an arbitrary slot, so the host can find the right one by testing.
__declspec(dllexport) void dlssnr_call_probe_float(void *params, const char *name, float value,
                                                   int slot) {
    if (!params || slot < 0 || slot >= 8) {
        return;
    }
    void **vt = *reinterpret_cast<void ***>(params);
    reinterpret_cast<PFN_SetFloat>(vt[slot])(params, name, value);
}

// Last init and create results, so the add-on can log why a feature never appeared.
__declspec(dllexport) int dlssnr_call_last_init = 0;
__declspec(dllexport) int dlssnr_call_last_create = 0;

// ---------------------------------------------------------------------------------------------
// Vulkan.
//
// The model ships a complete native Vulkan surface -- fourteen entry points against D3D12's ten --
// so a Vulkan game does not need the D3D12 bridge the pass currently reaches it through. It needs
// this: the same caller gate, because the snippet checks its caller's module path whichever API is
// being used, and only this DLL has a name that satisfies it.
//
// Handles are passed as void*. Every Vulkan handle this touches -- instance, physical device,
// device, command buffer -- is a dispatchable handle, which is a pointer on every 64-bit platform,
// and taking them opaquely keeps the forwarder free of a Vulkan dependency it would otherwise carry
// only to name types it never dereferences.
//
// Resources are the caller's problem for the same reason: NGX wants a pointer to an
// NVSDK_NGX_Resource_VK, the host builds it, and this passes the pointer through the same 64-bit
// parameter setter the D3D12 path uses for ID3D12Resource*.
// ---------------------------------------------------------------------------------------------

using PFN_NrVkInitExt = int(__cdecl *)(unsigned long long, const wchar_t *, void *, void *, void *,
                                       const void *, int);
using PFN_NrVkCreate = int(__cdecl *)(void *, int, const void *, void **);
using PFN_NrVkEvaluate = int(__cdecl *)(void *, const void *, const void *, void *);

struct VkSnippet {
    HMODULE module = nullptr;
    PFN_NrVkInitExt init = nullptr;
    PFN_NrVkCreate create = nullptr;
    PFN_NrVkEvaluate evaluate = nullptr;
    PFN_NrRelease release = nullptr;
    bool initialised = false;
};

VkSnippet g_vk;

bool loadVkSnippet(const wchar_t *path) {
    if (g_vk.module) {
        return g_vk.create != nullptr;
    }

    g_vk.module = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);

    if (!g_vk.module) {
        return false;
    }

    g_vk.init = (PFN_NrVkInitExt) GetProcAddress(g_vk.module, "NVSDK_NGX_VULKAN_Init_Ext");
    g_vk.create = (PFN_NrVkCreate) GetProcAddress(g_vk.module, "NVSDK_NGX_VULKAN_CreateFeature");
    g_vk.evaluate = (PFN_NrVkEvaluate) GetProcAddress(g_vk.module, "NVSDK_NGX_VULKAN_EvaluateFeature");
    g_vk.release = (PFN_NrRelease) GetProcAddress(g_vk.module, "NVSDK_NGX_VULKAN_ReleaseFeature");

    return g_vk.create != nullptr && g_vk.evaluate != nullptr;
}

// ---------------------------------------------------------------------------------------------
// The model's own scaling ratio, asked for the way NVIDIA asks for it.
//
// The snippet publishes callbacks into a parameter block through PopulateParameters_Impl -- the same
// mechanism DLSS uses for DLSSOptimalSettingsCallback. The strings in nvngx_dlssnr.dll spell the
// contract out in order: DLSSNRComputeScalingRatioCallback, ComputeScalingRatioCommon,
// PerfQualityValue, then the two failures "missing PerfQualityValue for DLSSNR scaling ratio
// computation" and "unsupported PerfQualityValue %u ...", then DLSSNR.ScalingRatio itself.
//
// So: populate a block, read the callback out of it, set PerfQualityValue, call it, read the ratio
// back. Read-only -- it creates no feature and changes no feature state, which is exactly why it is
// worth doing before anything is built on the answer.
//
// It lives here rather than in the host because PopulateParameters_Impl is the snippet's own export
// and the snippet resolves its caller's module path.
// ---------------------------------------------------------------------------------------------

using PFN_NrPopulate = int(__cdecl *)(void *);
using PFN_NrRatioCallback = int(__cdecl *)(void *);

// Getters mirror setters eight slots up -- the same rule that put the resource getter at 8 opposite
// the 64-bit setter at 0. A pointer published into the block is written as a 64-bit value, so it comes
// back through slot 8; a float comes back through the discovered float slot plus eight.
constexpr int VT_GET_ULL = VT_SET_ULL + 8;

using PFN_GetULL = int(__thiscall *)(void *, const char *, unsigned long long *);
using PFN_GetFloat = int(__thiscall *)(void *, const char *, float *);

__declspec(dllexport) int dlssnr_last_ratio_result = 0;
__declspec(dllexport) int dlssnr_last_ratio_stage = 0;

// Asks the model what resolution it wants for a given quality level.
//
// perfQuality is NVSDK_NGX_PerfQuality_Value: 0 MaxPerf, 1 Balanced, 2 MaxQuality, 3 UltraPerformance,
// 4 UltraQuality, 5 DLAA. Returns 1 and writes outRatio on success. 0 means the callback was never
// published, so the mechanism is not live in this snippet; -1 means it was published and refused, which
// for this callback means the quality value is not one it supports. dlssnr_last_ratio_stage says how
// far it got, so a zero can be told apart from a snippet that would not load at all.
__declspec(dllexport) int dlssnr_query_scaling_ratio(const wchar_t *snippetPath, void *capabilityParams,
                                                     unsigned int perfQuality, float *outRatio) {
    dlssnr_last_ratio_stage = 0;

    if (!loadSnippet(snippetPath) || !capabilityParams || !outRatio) {
        return 0;
    }

    dlssnr_last_ratio_stage = 1;

    // Publishing is what puts the callback in the block. Harmless if it has already happened.
    auto populate = (PFN_NrPopulate) GetProcAddress(g_snip.module, "NVSDK_NGX_D3D12_PopulateParameters_Impl");

    if (populate != nullptr) {
        volatile int populated = populate(capabilityParams);
        (void) populated;
        dlssnr_last_ratio_stage = 2;
    }

    void **vt = *reinterpret_cast<void ***>(capabilityParams);
    unsigned long long raw = 0;

    if (reinterpret_cast<PFN_GetULL>(vt[VT_GET_ULL])(capabilityParams, "DLSSNRComputeScalingRatioCallback",
                                                     &raw) != 1 ||
        raw == 0) {
        return 0;
    }

    dlssnr_last_ratio_stage = 3;

    setUInt(capabilityParams, "PerfQualityValue", perfQuality);

    // Cleared first so a callback that writes nothing cannot be mistaken for one that wrote zero.
    setFloat(capabilityParams, "DLSSNR.ScalingRatio", -1.0f);

    // Assigned rather than returned, for the same reason every other call through this module is.
    volatile int result = reinterpret_cast<PFN_NrRatioCallback>((void *) raw)(capabilityParams);
    dlssnr_last_ratio_result = (int) result;

    if (result != 1) {
        return -1;
    }

    dlssnr_last_ratio_stage = 4;

    float ratio = -1.0f;

    if (reinterpret_cast<PFN_GetFloat>(vt[g_floatSlot + 8])(capabilityParams, "DLSSNR.ScalingRatio",
                                                            &ratio) != 1) {
        return -1;
    }

    dlssnr_last_ratio_stage = 5;
    *outRatio = ratio;
    return 1;
}

// ---------------------------------------------------------------------------------------------
// Native Direct3D 11.
//
// The claim this tests is one nobody ever tested. "The model refuses to run on DX11, it answers
// FeatureNotSupported" has been in the notes and the release text for a long time, and this forwarder
// resolves no D3D11 entry point at all -- so whatever returned FeatureNotSupported, it was not the
// snippet's own D3D11 path, because that path has never been called.
//
// The binary says it should exist. nvngx_dlssnr.dll exports ten D3D11 entry points, implemented in
// source/features/dlssnr/ngx_d3d11.cpp, and its CreateFeature, EvaluateFeature and ReleaseFeature go
// through the same CreateFeatureCommon / EvaluateFeatureCommon / ReleaseFeatureCommon in
// ngx_templates.h that the D3D12 path uses. That is a real implementation sharing the same core, not
// a stub.
//
// If it works, a D3D11 game does not need the bridge -- which is what currently forces those games to
// give up DLSS as their upscaler, the biggest caveat in the whole feature.
//
// Probe first, exactly as the Vulkan path did: resolve the entry points and report which answer,
// before anything is created.
// ---------------------------------------------------------------------------------------------

// The _Ext variant puts the version BEFORE the feature info. That is the entire difference between
// NVSDK_NGX_D3D11_Init and NVSDK_NGX_D3D11_Init_Ext, and getting it backwards is not a wrong answer,
// it is a fault: NGX would take the version as a pointer and dereference 0x15 to read PathListInfo.
// The correct order is in this repo already, at proxies/NVNGX_Proxy.h.
using PFN_NrD3D11Init = int(__cdecl *)(unsigned long long, const wchar_t *, void *, int, const void *);
using PFN_NrD3D11Create = int(__cdecl *)(void *, int, const void *, void **);
using PFN_NrD3D11Evaluate = int(__cdecl *)(void *, const void *, const void *, void *);

struct D3D11Snippet {
    HMODULE module = nullptr;
    PFN_NrD3D11Init init = nullptr;
    PFN_NrD3D11Create create = nullptr;
    PFN_NrD3D11Evaluate evaluate = nullptr;
    PFN_NrRelease release = nullptr;
    bool initialised = false;
};

D3D11Snippet g_d3d11;

bool loadD3D11Snippet(const wchar_t *path) {
    if (g_d3d11.module) {
        return g_d3d11.create != nullptr;
    }

    // Loading the same path twice does not give two modules -- Windows returns the same HMODULE with
    // the reference count raised. So this would share one snippet's global NGX core with the D3D12
    // path: one device pointer, one set of caches, two independent "initialised" flags over the top.
    // Initialising it for D3D11 could then leave the D3D12 core holding a D3D11 device, which is a
    // device removal rather than a wrong answer.
    //
    // A probe must not be able to break the thing it is probing, so it takes its own copy under a
    // different name and the two cores never meet. If the copy cannot be made, the probe declines
    // rather than falling back to the shared module.
    wchar_t probeCopy[MAX_PATH] = {};
    wchar_t probeDir[MAX_PATH] = {};

    if (GetTempPathW(MAX_PATH, probeDir) == 0) {
        return false;
    }

    wcscpy_s(probeCopy, probeDir);
    wcscat_s(probeCopy, L"nvngx.dll_dlssnr_d3d11probe.dll");

    if (!CopyFileW(path, probeCopy, FALSE)) {
        return false;
    }

    g_d3d11.module = LoadLibraryExW(probeCopy, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);

    if (!g_d3d11.module) {
        return false;
    }

    g_d3d11.init = (PFN_NrD3D11Init) GetProcAddress(g_d3d11.module, "NVSDK_NGX_D3D11_Init_Ext");
    g_d3d11.create = (PFN_NrD3D11Create) GetProcAddress(g_d3d11.module, "NVSDK_NGX_D3D11_CreateFeature");
    g_d3d11.evaluate = (PFN_NrD3D11Evaluate) GetProcAddress(g_d3d11.module, "NVSDK_NGX_D3D11_EvaluateFeature");
    g_d3d11.release = (PFN_NrRelease) GetProcAddress(g_d3d11.module, "NVSDK_NGX_D3D11_ReleaseFeature");

    return g_d3d11.create != nullptr && g_d3d11.evaluate != nullptr;
}

__declspec(dllexport) int dlssnr_d3d11_last_init = 0;
__declspec(dllexport) int dlssnr_d3d11_last_create = 0;

// Which entry points resolved, as a bit field: init 1, create 2, evaluate 4, release 8. Fifteen means
// the model's D3D11 surface is entirely reachable. Answered without creating anything.
__declspec(dllexport) int dlssnr_d3d11_probe(const wchar_t *snippetPath) {
    loadD3D11Snippet(snippetPath);

    int bits = 0;
    bits |= g_d3d11.init != nullptr ? 1 : 0;
    bits |= g_d3d11.create != nullptr ? 2 : 0;
    bits |= g_d3d11.evaluate != nullptr ? 4 : 0;
    bits |= g_d3d11.release != nullptr ? 8 : 0;

    return bits;
}

// Initialises NGX on a D3D11 device. This is the call that decides the question: if the snippet
// wanted nothing to do with D3D11 it would refuse here, before any feature exists.
__declspec(dllexport) int dlssnr_d3d11_init(const wchar_t *snippetPath, const wchar_t *dataPath,
                                            void *device, int sdkVersion) {
    if (!loadD3D11Snippet(snippetPath) || g_d3d11.init == nullptr || device == nullptr) {
        return -1;
    }

    if (g_d3d11.initialised) {
        return 1;
    }

    // Assigned rather than returned: a tail call becomes a jmp and the snippet resolves its caller
    // past this module, which the gate rejects before reading an argument.
    volatile int result = g_d3d11.init(0x24480451ull, dataPath, device, sdkVersion, nullptr);

    dlssnr_d3d11_last_init = (int) result;
    g_d3d11.initialised = result == 1;

    return (int) result;
}

// Creates the feature on a D3D11 device context. Tuning is set here for the same reason it is on the
// other two paths: the model reads it once, when it builds the feature.
__declspec(dllexport) void *dlssnr_d3d11_create(void *deviceContext, void *capabilityParams,
                                                unsigned int width, unsigned int height, int preset,
                                                float intensity, int style, float localStructure,
                                                float localTone, float skinStructure, int useAutoMask,
                                                int uiCorrection) {
    if (g_d3d11.create == nullptr || deviceContext == nullptr || capabilityParams == nullptr) {
        return nullptr;
    }

    setUInt(capabilityParams, "DLSSNR.Enabled", 1);
    setUInt(capabilityParams, "DLSSNR.Width", width);
    setUInt(capabilityParams, "DLSSNR.Height", height);
    setUInt(capabilityParams, "CreationNodeMask", 1);
    setUInt(capabilityParams, "VisibilityNodeMask", 1);
    setUInt(capabilityParams, "DLSSNR.Hint.Render.Preset", (unsigned int) preset);

    setFloat(capabilityParams, "DLSSNR.Intensity", intensity);
    setUInt(capabilityParams, "DLSSNR.Style", (unsigned int) style);
    setFloat(capabilityParams, "DLSSNR.LocalStructureStrength", localStructure);
    setFloat(capabilityParams, "DLSSNR.LocalToneStrength", localTone);
    setFloat(capabilityParams, "DLSSNR.SkinStructureStrength", skinStructure);
    setUInt(capabilityParams, "DLSSNR.UseAutoMask", (unsigned int) useAutoMask);
    setUInt(capabilityParams, "DLSSNR.UICorrection", (unsigned int) uiCorrection);

    void *feature = nullptr;
    volatile int result = g_d3d11.create(deviceContext, 18, capabilityParams, &feature);

    dlssnr_d3d11_last_create = (int) result;

    return result == 1 ? feature : nullptr;
}

__declspec(dllexport) void dlssnr_d3d11_release(void *feature) {
    if (g_d3d11.release != nullptr && feature != nullptr) {
        volatile int ignored = g_d3d11.release(feature);
        (void) ignored;
    }
}

__declspec(dllexport) int dlssnr_vk_last_init = 0;
__declspec(dllexport) int dlssnr_vk_last_create = 0;

// Which of the four entry points resolved, as a bit field: init 1, create 2, evaluate 4, release 8.
// 15 means the model's Vulkan surface is entirely reachable from here. Answered without creating
// anything, so the host can decide whether the native path exists before committing to it.
__declspec(dllexport) int dlssnr_vk_probe(const wchar_t *snippetPath) {
    loadVkSnippet(snippetPath);

    int bits = 0;
    bits |= g_vk.init != nullptr ? 1 : 0;
    bits |= g_vk.create != nullptr ? 2 : 0;
    bits |= g_vk.evaluate != nullptr ? 4 : 0;
    bits |= g_vk.release != nullptr ? 8 : 0;

    return bits;
}

__declspec(dllexport) int dlssnr_vk_init(const wchar_t *snippetPath, const wchar_t *dataPath,
                                         void *instance, void *physicalDevice, void *device,
                                         int sdkVersion) {
    if (!loadVkSnippet(snippetPath) || g_vk.init == nullptr) {
        return -1;
    }

    if (g_vk.initialised) {
        return 1;
    }

    // Assigned rather than returned directly. A tail call becomes a jmp, and the snippet resolves its
    // caller from the return address -- so tail calling hands it whoever called this instead of this
    // module, and the caller gate rejects it before a single argument is read.
    volatile int result = g_vk.init(0x0, dataPath, instance, physicalDevice, device, nullptr, sdkVersion);

    dlssnr_vk_last_init = (int) result;
    g_vk.initialised = result == 1;

    return (int) result;
}

// Creates the feature on a Vulkan command buffer. Same contract as the D3D12 one: initialisation work
// is recorded into the buffer, so the handle has to outlive its execution.
// Creates the feature on a Vulkan command buffer. Same contract as the D3D12 one: initialisation work
// is recorded into the buffer, so the handle has to outlive its execution.
//
// The tuning is set here and not at evaluate, for the same reason it is on the D3D12 path: the model
// reads these once, when it builds the feature, and anything set only at evaluate is ignored. That was
// why none of these controls did anything for a long time.
__declspec(dllexport) void *dlssnr_vk_create(void *cmdBuffer, void *capabilityParams, unsigned int width,
                                             unsigned int height, int preset, float intensity, int style,
                                             float localStructure, float localTone, float skinStructure,
                                             int useAutoMask, int uiCorrection) {
    if (g_vk.create == nullptr || cmdBuffer == nullptr || capabilityParams == nullptr) {
        return nullptr;
    }

    setUInt(capabilityParams, "DLSSNR.Enabled", 1);
    setUInt(capabilityParams, "DLSSNR.Width", width);
    setUInt(capabilityParams, "DLSSNR.Height", height);
    setUInt(capabilityParams, "CreationNodeMask", 1);
    setUInt(capabilityParams, "VisibilityNodeMask", 1);

    // Written unconditionally, including zero. The block belongs to the driver and outlives the
    // feature, so skipping the write for "default" leaves the last chosen preset sitting in it.
    setUInt(capabilityParams, "DLSSNR.Hint.Render.Preset", (unsigned int) preset);

    setFloat(capabilityParams, "DLSSNR.Intensity", intensity);
    setUInt(capabilityParams, "DLSSNR.Style", (unsigned int) style);
    setFloat(capabilityParams, "DLSSNR.LocalStructureStrength", localStructure);
    setFloat(capabilityParams, "DLSSNR.LocalToneStrength", localTone);
    setFloat(capabilityParams, "DLSSNR.SkinStructureStrength", skinStructure);
    setUInt(capabilityParams, "DLSSNR.UseAutoMask", (unsigned int) useAutoMask);
    setUInt(capabilityParams, "DLSSNR.UICorrection", (unsigned int) uiCorrection);

    void *feature = nullptr;
    volatile int result = g_vk.create(cmdBuffer, 18, capabilityParams, &feature);

    dlssnr_vk_last_create = (int) result;

    return result == 1 ? feature : nullptr;
}

// Evaluates on Vulkan. Same parameter block as the D3D12 path, filled the same way and in the same
// order, because it is the same feature -- only the four resources differ, and only in that each is a
// pointer to an NVSDK_NGX_Resource_VK the host built rather than an ID3D12Resource. Both go through
// the block's 64-bit setter, so the snippet sees the same shape either way.
//
// Filling it here rather than in the host keeps the two APIs from drifting: a parameter added to one
// evaluate and forgotten in the other would be a bug that only appears on one backend.
__declspec(dllexport) int dlssnr_vk_evaluate(void *cmdBuffer, void *feature, void *capabilityParams,
                                             void *color, void *depth, void *motion, void *output,
                                             unsigned int width, unsigned int height,
                                             unsigned int guideWidth, unsigned int guideHeight,
                                             int depthInverted, int reset, float intensity, int style,
                                             float localStructure, float localTone, float skinStructure,
                                             int useAutoMask, float mvScaleX, float mvScaleY) {
    if (g_vk.evaluate == nullptr || feature == nullptr || capabilityParams == nullptr) {
        return -1;
    }

    setResourcePtr(capabilityParams, "DLSSNR.Color", color);
    setResourcePtr(capabilityParams, "DLSSNR.Depth", depth);
    setResourcePtr(capabilityParams, "DLSSNR.MVec", motion);
    setResourcePtr(capabilityParams, "DLSSNR.Output", output);

    // The block is shared with the game's own DLSS, which overwrites these between frames, so every
    // value the feature reads is set again here rather than relying on what create left behind.
    setUInt(capabilityParams, "DLSSNR.Enabled", 1);
    setUInt(capabilityParams, "DLSSNR.Width", width);
    setUInt(capabilityParams, "DLSSNR.Height", height);
    setUInt(capabilityParams, "DLSSNR.DepthInverted", (unsigned int) depthInverted);
    setUInt(capabilityParams, "DLSSNR.Reset", (unsigned int) reset);

    setUInt(capabilityParams, "DLSSNR.ColorSubrectBaseX", 0);
    setUInt(capabilityParams, "DLSSNR.ColorSubrectBaseY", 0);
    setUInt(capabilityParams, "DLSSNR.ColorSubrectWidth", width);
    setUInt(capabilityParams, "DLSSNR.ColorSubrectHeight", height);
    setUInt(capabilityParams, "DLSSNR.OutputSubrectBaseX", 0);
    setUInt(capabilityParams, "DLSSNR.OutputSubrectBaseY", 0);
    setUInt(capabilityParams, "DLSSNR.OutputSubrectWidth", width);
    setUInt(capabilityParams, "DLSSNR.OutputSubrectHeight", height);
    setUInt(capabilityParams, "DLSSNR.DepthSubrectBaseX", 0);
    setUInt(capabilityParams, "DLSSNR.DepthSubrectBaseY", 0);
    setUInt(capabilityParams, "DLSSNR.DepthSubrectWidth", guideWidth);
    setUInt(capabilityParams, "DLSSNR.DepthSubrectHeight", guideHeight);
    setUInt(capabilityParams, "DLSSNR.MVecSubrectBaseX", 0);
    setUInt(capabilityParams, "DLSSNR.MVecSubrectBaseY", 0);
    setUInt(capabilityParams, "DLSSNR.MVecSubrectWidth", guideWidth);
    setUInt(capabilityParams, "DLSSNR.MVecSubrectHeight", guideHeight);

    // The game's own encoding, passed through rather than derived. Deriving it from the resolutions
    // came out as exactly 1.0 at native, which told the model almost nothing had moved.
    setFloat(capabilityParams, "DLSSNR.MVecScaleX", mvScaleX);
    setFloat(capabilityParams, "DLSSNR.MVecScaleY", mvScaleY);

    setFloat(capabilityParams, "DLSSNR.Intensity", intensity);
    setUInt(capabilityParams, "DLSSNR.Style", (unsigned int) style);
    setFloat(capabilityParams, "DLSSNR.LocalStructureStrength", localStructure);
    setFloat(capabilityParams, "DLSSNR.LocalToneStrength", localTone);
    setFloat(capabilityParams, "DLSSNR.SkinStructureStrength", skinStructure);
    setUInt(capabilityParams, "DLSSNR.UseAutoMask", (unsigned int) useAutoMask);

    // Assigned rather than returned. A tail call becomes a jmp and the snippet would resolve its
    // caller past this module, which the gate rejects.
    volatile int result = g_vk.evaluate(cmdBuffer, feature, capabilityParams, nullptr);

    return (int) result;
}

__declspec(dllexport) void dlssnr_vk_release(void *feature) {
    if (g_vk.release != nullptr && feature != nullptr) {
        volatile int ignored = g_vk.release(feature);
        (void) ignored;
    }
}

// Creates a persistent Neural Rendering feature. The handle records initialisation work into cmd, so it
// must outlive that command list's execution; releasing it early loses the device.
__declspec(dllexport) void *dlssnr_call_create(const wchar_t *snippetPath, const wchar_t *dataPath,
                                               ID3D12Device *device, ID3D12GraphicsCommandList *cmd,
                                               void *capabilityParams, unsigned int width,
                                               unsigned int height, int preset, float intensity,
                                               int style, float localStructure, float localTone,
                                               float skinStructure, int useAutoMask,
                                               int uiCorrection) {
    if (!loadSnippet(snippetPath) || !capabilityParams) {
        return nullptr;
    }
    if (!g_snip.initialised && g_snip.init) {
        // OptiScaler's own generic application id, the one it already hands DLSS when a game's id is
        // not wanted. What was here before was 0x4350324B -- "CP2K" -- so every game that ever loaded
        // this announced itself to the driver as Cyberpunk 2077.
        dlssnr_call_last_init = g_snip.init(0x24480451ull, dataPath, device, 0x0000015, capabilityParams);
        g_snip.initialised = (dlssnr_call_last_init == 1);
        if (!g_snip.initialised) {
            return nullptr;
        }
    }
    setUInt(capabilityParams, "DLSSNR.Enabled", 1);
    setUInt(capabilityParams, "DLSSNR.Width", width);
    setUInt(capabilityParams, "DLSSNR.Height", height);
    setUInt(capabilityParams, "CreationNodeMask", 1);
    setUInt(capabilityParams, "VisibilityNodeMask", 1);
    // Written unconditionally, including zero. This block belongs to the driver and outlives the
    // feature, so skipping the write for "default" left whichever preset was chosen last still sitting
    // in it -- and going back to default did nothing at all.
    setUInt(capabilityParams, "DLSSNR.Hint.Render.Preset", (unsigned int) preset);

    // The tuning has to be here rather than at evaluate. Everything this sets before create takes
    // effect; everything set only at evaluate is ignored, which is why none of these controls did
    // anything for a long time. The model reads them once, when it builds the feature.
    setFloat(capabilityParams, "DLSSNR.Intensity", intensity);
    setUInt(capabilityParams, "DLSSNR.Style", (unsigned int) style);
    setFloat(capabilityParams, "DLSSNR.LocalStructureStrength", localStructure);
    setFloat(capabilityParams, "DLSSNR.LocalToneStrength", localTone);
    setFloat(capabilityParams, "DLSSNR.SkinStructureStrength", skinStructure);
    setUInt(capabilityParams, "DLSSNR.UseAutoMask", (unsigned int) useAutoMask);
    setUInt(capabilityParams, "DLSSNR.UICorrection", (unsigned int) uiCorrection);
    void *handle = nullptr;
    dlssnr_call_last_create = g_snip.create(cmd, 18, capabilityParams, &handle);
    return handle;
}

// Colour and output are display resolution; depth and motion come from the game's own DLSS evaluation and
// may be render resolution, so each resource carries its own subrect and motion scales by the ratio.
__declspec(dllexport) int dlssnr_call_evaluate(ID3D12GraphicsCommandList *cmd, void *feature,
                                               void *capabilityParams, ID3D12Resource *color,
                                               ID3D12Resource *depth, ID3D12Resource *motion,
                                               ID3D12Resource *output, unsigned int width,
                                               unsigned int height, unsigned int guideWidth,
                                               unsigned int guideHeight, int depthInverted, int reset,
                                               float intensity, int style, float localStructure,
                                               float localTone, float skinStructure, int useAutoMask,
                                               float mvScaleX, float mvScaleY) {
    if (!feature || !capabilityParams || !g_snip.evaluate) {
        return 0;
    }
    setResource(capabilityParams, "DLSSNR.Color", color);
    setResource(capabilityParams, "DLSSNR.Depth", depth);
    setResource(capabilityParams, "DLSSNR.MVec", motion);
    setResource(capabilityParams, "DLSSNR.Output", output);

    // The block is shared with the game's own DLSS, which overwrites these between frames, so every
    // value the feature reads is set again here rather than relying on what create left behind.
    setUInt(capabilityParams, "DLSSNR.Enabled", 1);
    setUInt(capabilityParams, "DLSSNR.Width", width);
    setUInt(capabilityParams, "DLSSNR.Height", height);
    setUInt(capabilityParams, "DLSSNR.DepthInverted", (unsigned int) depthInverted);
    setUInt(capabilityParams, "DLSSNR.Reset", (unsigned int) reset);

    setUInt(capabilityParams, "DLSSNR.ColorSubrectBaseX", 0);
    setUInt(capabilityParams, "DLSSNR.ColorSubrectBaseY", 0);
    setUInt(capabilityParams, "DLSSNR.ColorSubrectWidth", width);
    setUInt(capabilityParams, "DLSSNR.ColorSubrectHeight", height);
    setUInt(capabilityParams, "DLSSNR.OutputSubrectBaseX", 0);
    setUInt(capabilityParams, "DLSSNR.OutputSubrectBaseY", 0);
    setUInt(capabilityParams, "DLSSNR.OutputSubrectWidth", width);
    setUInt(capabilityParams, "DLSSNR.OutputSubrectHeight", height);
    setUInt(capabilityParams, "DLSSNR.DepthSubrectBaseX", 0);
    setUInt(capabilityParams, "DLSSNR.DepthSubrectBaseY", 0);
    setUInt(capabilityParams, "DLSSNR.DepthSubrectWidth", guideWidth);
    setUInt(capabilityParams, "DLSSNR.DepthSubrectHeight", guideHeight);
    setUInt(capabilityParams, "DLSSNR.MVecSubrectBaseX", 0);
    setUInt(capabilityParams, "DLSSNR.MVecSubrectBaseY", 0);
    setUInt(capabilityParams, "DLSSNR.MVecSubrectWidth", guideWidth);
    setUInt(capabilityParams, "DLSSNR.MVecSubrectHeight", guideHeight);

    // The game's own encoding, passed through. Deriving this from the resolutions was a guess, and at
    // native resolution it came out as exactly 1.0 -- so a game using normalised vectors was telling
    // the model almost nothing had moved.
    setFloat(capabilityParams, "DLSSNR.MVecScaleX", mvScaleX);
    setFloat(capabilityParams, "DLSSNR.MVecScaleY", mvScaleY);

    setFloat(capabilityParams, "DLSSNR.Intensity", intensity);
    setUInt(capabilityParams, "DLSSNR.Style", (unsigned int) style);
    setFloat(capabilityParams, "DLSSNR.LocalStructureStrength", localStructure);
    setFloat(capabilityParams, "DLSSNR.LocalToneStrength", localTone);
    setFloat(capabilityParams, "DLSSNR.SkinStructureStrength", skinStructure);
    setUInt(capabilityParams, "DLSSNR.UseAutoMask", (unsigned int) useAutoMask);

    // The result must not be returned directly. `return f(...)` is a tail call, and the compiler emits a
    // jmp rather than a call, which leaves this module's frame behind: the snippet then resolves its
    // caller to whoever called us and rejects it. Keeping the value in a volatile forces a real call and
    // a return through this module, which is the whole reason this file exists.
    volatile int result = g_snip.evaluate(cmd, feature, capabilityParams, nullptr);
    return result;
}

// Inputs NVIDIA's own Streamline plugin sets that the positional exports predate: the model's global
// tone strength (read at create), and the interface as the game draws it -- its layer, its alpha, and
// the composited back buffer -- which is what the model's UI correction was designed around. Called
// before create and before every evaluate; absent resources are written as null, because the block
// outlives everything and a stale pointer is a freed resource.
__declspec(dllexport) void dlssnr_call_set_extras(void *capabilityParams, float globalTone,
                                                  ID3D12Resource *ui, ID3D12Resource *uiAlpha,
                                                  ID3D12Resource *backbuffer, unsigned int uiWidth,
                                                  unsigned int uiHeight, unsigned int bbWidth,
                                                  unsigned int bbHeight) {
    if (!capabilityParams) {
        return;
    }
    // DLSSNR.GlobalToneStrength is deliberately not written. The string does not appear anywhere in
    // nvngx_dlssnr.dll -- the model's own vocabulary is 61 DLSSNR.* names and this is not one of them,
    // so every write went into a map nothing reads. It exists in Streamline's sl.dlss_nr.dll, which is
    // presumably where it was copied from. Left as a parameter here so the signature does not churn.
    (void) globalTone;

    setResource(capabilityParams, "DLSSNR.UI", ui);
    setResource(capabilityParams, "DLSSNR.UIAlpha", uiAlpha);
    setResource(capabilityParams, "DLSSNR.Backbuffer", backbuffer);
    setUInt(capabilityParams, "DLSSNR.UISubrectBaseX", 0);
    setUInt(capabilityParams, "DLSSNR.UISubrectBaseY", 0);
    setUInt(capabilityParams, "DLSSNR.UISubrectWidth", uiWidth);
    setUInt(capabilityParams, "DLSSNR.UISubrectHeight", uiHeight);
    setUInt(capabilityParams, "DLSSNR.UIAlphaSubrectBaseX", 0);
    setUInt(capabilityParams, "DLSSNR.UIAlphaSubrectBaseY", 0);
    setUInt(capabilityParams, "DLSSNR.UIAlphaSubrectWidth", uiWidth);
    setUInt(capabilityParams, "DLSSNR.UIAlphaSubrectHeight", uiHeight);
    setUInt(capabilityParams, "DLSSNR.BackbufferSubrectBaseX", 0);
    setUInt(capabilityParams, "DLSSNR.BackbufferSubrectBaseY", 0);
    setUInt(capabilityParams, "DLSSNR.BackbufferSubrectWidth", bbWidth);
    setUInt(capabilityParams, "DLSSNR.BackbufferSubrectHeight", bbHeight);
}

__declspec(dllexport) void dlssnr_call_release(void *feature) {
    if (feature && g_snip.release) {
        volatile int result = g_snip.release(feature); // not a tail call, for the reason above
        (void) result;
    }
}

} // extern "C"
