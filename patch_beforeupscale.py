"""Adds a third inject point: the upscaler's input, before it runs.

The model runs at render resolution on the image DLSS is about to consume, and its edit is resolved back
into linear HDR before DLSS ever sees it. DLSS then enlarges and temporally reconstructs a frame that
already carries the synthesised detail.

Three things follow, and they are the whole reason to try it:

  * The model works at render resolution, so at a real render scale it costs a fraction of the
    display-resolution pass -- the published guide measured 13.2 ms falling to 6.0 ms at 66% scale.

  * The guides finally describe the exact image the model is looking at. At every other inject point the
    colour has been through post-processing the depth and motion vectors know nothing about.

  * The detail passes through DLSS's own temporal accumulation, which is the only mechanism available
    that could stabilise it rather than merely reduce it. This is the bet; it may also fail -- feeding an
    upscaler's history detail that changes each frame could destabilise instead. Nobody has tried it.

DLSS keeps receiving linear HDR, so IsHDR stays true and nothing about its feature changes -- unlike the
guide's chapter 9 reordering, which must strip the flag and therefore loses Ray Reconstruction.

Ray Reconstruction is still excluded here, but for a content reason rather than a flag one: its input is
the noisy radiance the denoiser has not yet consumed, and a detail synthesiser fed noise synthesises
noise. Detected and refused with a log line rather than silently producing garbage.
"""

import io

ROOT = "C:/Games_Temp/OptiScaler/OptiScaler/"

# --- header -----------------------------------------------------------------------------------------

p = ROOT + "dlssnr/DlssNr_Dx12.h"
t = io.open(p, encoding="utf-8").read()

old = """constexpr unsigned int INJECT_BEFORE_FG = 0;
constexpr unsigned int INJECT_PRESENT = 1;"""
new = """constexpr unsigned int INJECT_BEFORE_FG = 0;
constexpr unsigned int INJECT_PRESENT = 1;
// The upscaler's input, before it runs: the model works at render resolution and DLSS temporally
// reconstructs a frame that already carries its detail. Plain SR/DLAA only -- Ray Reconstruction's
// input is pre-denoise noise, which is detected and refused.
constexpr unsigned int INJECT_BEFORE_UPSCALE = 2;"""
assert old in t
t = t.replace(old, new, 1)

old = """// Whether the model is loaded and running, for the overlay."""
new = """// Runs the model on the upscaler's input, before the upscaler does. Called ahead of the real evaluate;
// a no-op unless that inject point is selected.
void EvaluateBeforeUpscale(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params);

// Whether the model is loaded and running, for the overlay."""
assert old in t
t = t.replace(old, new, 1)
io.open(p, "w", encoding="utf-8").write(t)
print("header patched")

# --- module -----------------------------------------------------------------------------------------

p = ROOT + "dlssnr/DlssNr_Dx12.cpp"
t = io.open(p, encoding="utf-8").read()

# EvaluateAfterUpscale does nothing in this mode; the work happened before the upscaler ran.
old = """    // When the model runs on the finished frame instead, this call exists only to take a copy of the
    // guides while they are still valid and still describe this frame.
    if (cfg.DlssNrInjectPoint.value_or_default() == INJECT_PRESENT)"""
new = """    // When the model ran before the upscaler, everything already happened earlier in this same call.
    if (cfg.DlssNrInjectPoint.value_or_default() == INJECT_BEFORE_UPSCALE)
    {
        device->Release();
        return;
    }

    // When the model runs on the finished frame instead, this call exists only to take a copy of the
    // guides while they are still valid and still describe this frame.
    if (cfg.DlssNrInjectPoint.value_or_default() == INJECT_PRESENT)"""
assert old in t
t = t.replace(old, new, 1)

# The new inject point itself, defined just before EvaluateAtPresent.
FUNC = '''
void EvaluateBeforeUpscale(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params)
{
    const Config& cfg = *Config::Instance();

    if (!cfg.DlssNrEnabled.value_or_default() || g_nr.failed || cmdList == nullptr || params == nullptr)
        return;

    if (cfg.DlssNrInjectPoint.value_or_default() != INJECT_BEFORE_UPSCALE)
        return;

    // Ray Reconstruction's input is the noisy radiance its denoiser has not yet consumed. A detail
    // synthesiser fed noise synthesises noise, so this point only exists ahead of plain SR / DLAA.
    ID3D12Resource* rrColor = nullptr;
    params->Get("DLSSD.Color", &rrColor);

    if (rrColor != nullptr)
    {
        static bool warnedRr = false;

        if (!warnedRr)
        {
            warnedRr = true;
            LOG_WARN("DLSS-NR: the before-upscaler inject point cannot run with Ray Reconstruction -- "
                     "its input is pre-denoise noise. Use the finished frame or before frame "
                     "generation instead.");
        }

        return;
    }

    ID3D12Resource* color = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motion = nullptr;
    params->Get(NVSDK_NGX_Parameter_Color, &color);
    params->Get(NVSDK_NGX_Parameter_Depth, &depth);
    params->Get(NVSDK_NGX_Parameter_MotionVectors, &motion);

    if (color == nullptr || depth == nullptr || motion == nullptr)
        return;

    // The render-resolution subrect, which is what the model works over. The resources themselves may
    // be display-sized with the valid region at the origin.
    unsigned int width = 0;
    unsigned int height = 0;
    params->Get(NVSDK_NGX_Parameter_Width, &width);
    params->Get(NVSDK_NGX_Parameter_Height, &height);

    const D3D12_RESOURCE_DESC colorDesc = color->GetDesc();

    if (width == 0 || height == 0)
    {
        width = (unsigned int) colorDesc.Width;
        height = colorDesc.Height;
    }

    ID3D12Device* device = nullptr;

    if (FAILED(color->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
        return;

    if (!EnsureForwarder() || !EnsureCapabilityParams(device) || !g_codec.ensure(device))
    {
        g_nr.failed = true;
        LOG_ERROR("DLSS-NR unavailable: {}", g_nr.reason);
        device->Release();
        return;
    }

    // Whether the input is linear HDR or already display-referred, from the game's own creation flags.
    unsigned int createFlags = 0;
    params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &createFlags);
    const bool isHdrBuffer = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) != 0;
    const bool depthInverted = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;

    // The game's own motion vector encoding, with no upscale ratio: the image the model sees here is
    // render resolution, the same space the vectors are in once the game's scale is applied.
    float mvScaleX = 1.0f;
    float mvScaleY = 1.0f;

    if (params->Get(NVSDK_NGX_Parameter_MV_Scale_X, &mvScaleX) != NVSDK_NGX_Result_Success)
        mvScaleX = 1.0f;

    if (params->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &mvScaleY) != NVSDK_NGX_Result_Success)
        mvScaleY = 1.0f;

    if (g_nr.feature != nullptr && (g_nr.width != width || g_nr.height != height))
    {
        // A resolution change: the previous frames' work has long since retired by the time one happens.
        g_nr.release(g_nr.feature);
        g_nr.feature = nullptr;

        if (g_nr.output != nullptr) { g_nr.output->Release(); g_nr.output = nullptr; }
        if (g_nr.colorCopy != nullptr) { g_nr.colorCopy->Release(); g_nr.colorCopy = nullptr; }
        if (g_nr.hdrCopy != nullptr) { g_nr.hdrCopy->Release(); g_nr.hdrCopy = nullptr; }
        if (g_nr.colorSmall != nullptr) { g_nr.colorSmall->Release(); g_nr.colorSmall = nullptr; }
    }

    const DXGI_FORMAT scratchFormat = codec::TypedFormat(colorDesc.Format);

    if (g_nr.output == nullptr)
    {
        g_nr.output = CreateScratch(device, scratchFormat, width, height);     // the model's answer
        g_nr.colorCopy = CreateScratch(device, scratchFormat, width, height);  // the proxy it is shown
        g_nr.hdrCopy = CreateScratch(device, scratchFormat, width, height);    // the input, untouched
        g_nr.colorSmall = CreateScratch(device, scratchFormat, width, height); // the resolve's output
    }

    if (g_nr.output == nullptr || g_nr.colorCopy == nullptr || g_nr.hdrCopy == nullptr ||
        g_nr.colorSmall == nullptr)
    {
        g_nr.failed = true;
        g_nr.reason = "the staging textures could not be created";
        device->Release();
        return;
    }

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
            device->Release();
            return;
        }

        g_nr.feature =
            g_nr.create(snippet->wstring().c_str(), State::Instance().NVNGX_ApplicationDataPath.c_str(),
                        device, cmdList, g_nr.capabilityParams, width, height,
                        (int) cfg.DlssNrPreset.value_or_default(), cfg.DlssNrIntensity.value_or_default(),
                        (int) cfg.DlssNrStyle.value_or_default(),
                        cfg.DlssNrLocalStructure.value_or_default(),
                        cfg.DlssNrLocalTone.value_or_default(),
                        cfg.DlssNrSkinStructure.value_or_default(),
                        cfg.DlssNrAutoMask.value_or_default() ? 1 : 0,
                        cfg.DlssNrUiCorrection.value_or_default() ? 1 : 0);

        if (g_nr.feature == nullptr)
        {
            g_nr.failed = true;
            g_nr.reason = "the model would not initialise";
            LOG_ERROR("DLSS-NR create failed before the upscaler: init 0x{:X}, create 0x{:X}",
                      g_nr.lastInit != nullptr ? *g_nr.lastInit : 0,
                      g_nr.lastCreate != nullptr ? *g_nr.lastCreate : 0);
            device->Release();
            return;
        }

        g_nr.width = width;
        g_nr.height = height;
        g_nr.reset = true;
        RecordBuiltTuning(cfg);
        LOG_INFO("DLSS-NR running on the upscaler's input at {}x{} ({}; the {} pass and its edit feed "
                 "DLSS's own temporal reconstruction)",
                 width, height, isHdrBuffer ? "linear HDR, transformed" : "display-referred, untouched",
                 isHdrBuffer ? "encoded" : "direct");
    }

    ++g_frames;

    if (g_gpuTime == nullptr)
        g_gpuTime = std::make_unique<GpuTime_Dx12>(device);

    if (g_gpuTime != nullptr)
        g_gpuTime->Start(cmdList);

    // White point from the frame itself, exactly as the other linear-HDR path derives it.
    const bool autoWhite = cfg.DlssNrAutoWhitePoint.value_or_default();

    if (autoWhite)
    {
        const probe::Stats stats = g_reader.collect();

        if (stats.valid && stats.meanLuma > 0.0f)
        {
            const float target = WhitePointForMean(stats.meanLuma);

            if (!g_autoWhitePointSettled)
            {
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

    const float whitePoint = autoWhite && g_autoWhitePointSettled ? g_autoWhitePoint
                                                                  : cfg.DlssNrWhitePoint.value_or_default();

    // Encode reads the game's input directly -- it is already shader-readable at evaluate time -- and
    // produces both the proxy the model is shown and an untouched copy in one dispatch.
    codec::Params encodeParams {};
    encodeParams.mode = codec::MODE_ENCODE;
    encodeParams.passthrough = isHdrBuffer ? 0u : 1u;
    encodeParams.whitePoint = whitePoint;
    encodeParams.width = width;
    encodeParams.height = height;
    g_codec.dispatch(cmdList, encodeParams, color, nullptr, nullptr, g_nr.colorCopy, g_nr.hdrCopy);

    Barrier(cmdList, g_nr.colorCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cmdList, g_nr.hdrCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    if (autoWhite && (g_frames % 300 == 0) && g_reducer.ensure(device))
    {
        ID3D12Resource* reducedFrame = g_reducer.dispatch(cmdList, g_nr.hdrCopy, width, height);
        g_reader.capture(cmdList, reducedFrame, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    // The guides describe exactly this image: same resolution, same frame, nothing post-processed in
    // between. That is not true at any other inject point.
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

    const int result = g_nr.evaluate(
        cmdList, g_nr.feature, g_nr.capabilityParams, g_nr.colorCopy, depthIn, motionIn, g_nr.output,
        width, height, width, height, depthInverted ? 1 : 0, g_nr.reset ? 1 : 0,
        cfg.DlssNrIntensity.value_or_default(), (int) cfg.DlssNrStyle.value_or_default(),
        cfg.DlssNrLocalStructure.value_or_default(), cfg.DlssNrLocalTone.value_or_default(),
        cfg.DlssNrSkinStructure.value_or_default(), cfg.DlssNrAutoMask.value_or_default() ? 1 : 0,
        mvScaleX, mvScaleY);

    g_nr.reset = false;

    if (result == NVSDK_NGX_Result_Success)
    {
        // The edit is folded back into the untouched input, so what DLSS receives is the same linear
        // HDR it was always going to get, plus the model's contribution and nothing else.
        codec::Params resolveParams {};
        resolveParams.mode = codec::MODE_RESOLVE;
        resolveParams.passthrough = isHdrBuffer ? 0u : 1u;
        resolveParams.whitePoint = whitePoint;
        resolveParams.width = width;
        resolveParams.height = height;
        resolveParams.transferStrength = cfg.DlssNrTransferStrength.value_or_default();
        resolveParams.colourStrength = cfg.DlssNrColourStrength.value_or_default();
        resolveParams.debugView = cfg.DlssNrDebugView.value_or_default();
        resolveParams.maxRatio = cfg.DlssNrMaxRatio.value_or_default();

        Barrier(cmdList, g_nr.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        g_codec.dispatch(cmdList, resolveParams, g_nr.colorCopy, g_nr.output, g_nr.hdrCopy,
                         g_nr.colorSmall, nullptr);
        Barrier(cmdList, g_nr.output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        // Copied back into the upscaler's own input, restoring the state its evaluate expects.
        Barrier(cmdList, g_nr.colorSmall, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cmdList, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);

        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = color;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = g_nr.colorSmall;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;

        D3D12_BOX box = {};
        box.right = width;
        box.bottom = height;
        box.back = 1;
        cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);

        Barrier(cmdList, g_nr.colorSmall, D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Barrier(cmdList, color, D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    else
    {
        g_nr.failed = true;
        g_nr.reason = "the model refused to run";
        LOG_ERROR("DLSS-NR evaluate before the upscaler returned 0x{:X}, disabling for this session",
                  (uint32_t) result);
    }

    // Everything back where the next frame expects to find it.
    if (g_nr.depthClone != nullptr)
        Barrier(cmdList, g_nr.depthClone, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);

    if (g_nr.motionClone != nullptr)
        Barrier(cmdList, g_nr.motionClone, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);

    Barrier(cmdList, g_nr.colorCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(cmdList, g_nr.hdrCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if (g_gpuTime != nullptr)
    {
        g_gpuTime->End(cmdList);

        if (State::Instance().currentCommandQueue != nullptr)
        {
            if (auto ms = g_gpuTime->ReadGpuTime((ID3D12CommandQueue*) State::Instance().currentCommandQueue);
                ms.has_value())
                g_lastGpuTime = ms;
        }
    }

    device->Release();
}

'''

anchor = "void EvaluateAtPresent(ID3D12CommandQueue* queue, ID3D12Resource* backBuffer, unsigned int backBufferIndex)"
assert anchor in t
t = t.replace(anchor, FUNC.strip() + "\n\n" + anchor, 1)

io.open(p, "w", encoding="utf-8").write(t)
print("module patched")

# --- hook: run it before both evaluates --------------------------------------------------------------

p = ROOT + "inputs/NVNGX_DLSS_Dx12.cpp"
t = io.open(p, encoding="utf-8").read()

old = """            LOG_DEBUG("Passthrough to native DLSS EvaluateFeature for handle {}", handleId);
            NVSDK_NGX_Result result ="""
new = """            LOG_DEBUG("Passthrough to native DLSS EvaluateFeature for handle {}", handleId);

            // The before-upscaler inject point enhances the input this evaluate is about to consume.
            if (feature != NVSDK_NGX_Feature_FrameGeneration)
                DlssNr::EvaluateBeforeUpscale(InCmdList, InParameters);

            NVSDK_NGX_Result result ="""
assert old in t
t = t.replace(old, new, 1)

old = """    // OptiScaler internal handling
    const NVSDK_NGX_Result optiResult = TryEvaluateOptiFeature(InCmdList, InFeatureHandle, InParameters, InCallback);"""
new = """    // The before-upscaler inject point enhances the input this evaluate is about to consume.
    if (feature != NVSDK_NGX_Feature_FrameGeneration)
        DlssNr::EvaluateBeforeUpscale(InCmdList, InParameters);

    // OptiScaler internal handling
    const NVSDK_NGX_Result optiResult = TryEvaluateOptiFeature(InCmdList, InFeatureHandle, InParameters, InCallback);"""
assert old in t
t = t.replace(old, new, 1)

io.open(p, "w", encoding="utf-8").write(t)
print("hook patched")
