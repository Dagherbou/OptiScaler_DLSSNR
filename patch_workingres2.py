"""Runs the model at the chosen fraction of the frame's resolution on the finished-frame path.

The frame is copied at full resolution, shrunk into a second texture for the model, and the model's
answer is enlarged during the resolve and added to the full-resolution copy. So the picture is never
reduced -- only the contribution is -- and at full scale nothing is shrunk or enlarged at all.
"""

import io

PATH = "C:/Games_Temp/OptiScaler/OptiScaler/dlssnr/DlssNr_Dx12.cpp"
text = io.open(PATH, encoding="utf-8").read()

# --- a texture for the reduced frame -------------------------------------------------------------------

old = """    // Cloned unconditionally when running at present, and only for typeless formats otherwise."""
new = """    // The frame shrunk for the model, when it is working below full resolution.
    ID3D12Resource* colorSmall = nullptr;
    unsigned int workWidth = 0;
    unsigned int workHeight = 0;

    // Cloned unconditionally when running at present, and only for typeless formats otherwise."""
assert old in text
text = text.replace(old, new, 1)

# --- size the model's own resources to the working resolution ------------------------------------------

old = """    if (g_nr.feature != nullptr && (g_nr.width != width || g_nr.height != height))
    {
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
    }"""
new = """    // What the model works at. The frame is never reduced; only this is.
    float scale = cfg.DlssNrWorkingScale.value_or_default();
    scale = scale < 0.25f ? 0.25f : (scale > 1.0f ? 1.0f : scale);
    const auto workWidth = (unsigned int) (width * scale + 0.5f);
    const auto workHeight = (unsigned int) (height * scale + 0.5f);
    const bool reduced = workWidth != width || workHeight != height;

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
    }"""
assert old in text
text = text.replace(old, new, 1)

old = """    if (g_nr.output == nullptr)
    {
        g_nr.output = CreateScratch(device, scratchFormat, width, height);
        g_nr.colorCopy = CreateScratch(device, scratchFormat, width, height);
        // The resolve cannot write the back buffer directly: a swapchain buffer is not created for
        // unordered access. It writes here and this is copied over the frame.
        g_nr.hdrCopy = CreateScratch(device, scratchFormat, width, height);
    }"""
new = """    if (g_nr.output == nullptr)
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
    }"""
assert old in text
text = text.replace(old, new, 1)

# --- create the feature at the working size ---------------------------------------------------------------

old = """            g_nr.create(snippet->wstring().c_str(), State::Instance().NVNGX_ApplicationDataPath.c_str(),
                        device, cmdList, g_nr.capabilityParams, width, height, (int) cfg.DlssNrPreset.value_or_default(),"""
new = """            g_nr.create(snippet->wstring().c_str(), State::Instance().NVNGX_ApplicationDataPath.c_str(),
                        device, cmdList, g_nr.capabilityParams, workWidth, workHeight,
                        (int) cfg.DlssNrPreset.value_or_default(),"""
assert old in text
text = text.replace(old, new, 1)

# --- shrink the frame, run the model on it, enlarge the edit ------------------------------------------------

old = """    Barrier(cmdList, g_nr.colorCopy, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    Barrier(cmdList, g_nr.depthClone, D3D12_RESOURCE_STATE_COPY_DEST,"""
new = """    Barrier(cmdList, g_nr.colorCopy, D3D12_RESOURCE_STATE_COPY_DEST,
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

    Barrier(cmdList, g_nr.depthClone, D3D12_RESOURCE_STATE_COPY_DEST,"""
assert old in text
text = text.replace(old, new, 1)

old = """    const int result = g_nr.evaluate(
        cmdList, g_nr.feature, g_nr.capabilityParams, g_nr.colorCopy, g_nr.depthClone, g_nr.motionClone,
        g_nr.output, width, height, g_nr.guideWidth, g_nr.guideHeight, g_nr.guideDepthInverted ? 1 : 0,"""
new = """    const int result = g_nr.evaluate(
        cmdList, g_nr.feature, g_nr.capabilityParams, modelInput, g_nr.depthClone, g_nr.motionClone,
        g_nr.output, workWidth, workHeight, g_nr.guideWidth, g_nr.guideHeight,
        g_nr.guideDepthInverted ? 1 : 0,"""
assert old in text
text = text.replace(old, new, 1)

# The resolve reads the proxy at whatever size the model saw, and the frame at full size.
old = """        g_codec.dispatch(cmdList, resolveParams, g_nr.colorCopy, g_nr.output, g_nr.colorCopy,
                         g_nr.hdrCopy, nullptr);"""
new = """        g_codec.dispatch(cmdList, resolveParams, modelInput, g_nr.output, g_nr.colorCopy, g_nr.hdrCopy,
                         nullptr);"""
assert old in text
text = text.replace(old, new, 1)

# Put the reduced copy back where the next frame expects it.
old = """    Barrier(cmdList, g_nr.colorCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if (g_gpuTime != nullptr)
        g_gpuTime->End(cmdList);"""
new = """    Barrier(cmdList, g_nr.colorCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if (reduced && g_nr.colorSmall != nullptr)
        Barrier(cmdList, g_nr.colorSmall, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if (g_gpuTime != nullptr)
        g_gpuTime->End(cmdList);"""
assert old in text
text = text.replace(old, new, 1)

# --- release it -----------------------------------------------------------------------------------------

old = """    if (g_nr.hdrCopy != nullptr)
    {
        g_nr.hdrCopy->Release();
        g_nr.hdrCopy = nullptr;
    }"""
new = """    if (g_nr.hdrCopy != nullptr)
    {
        g_nr.hdrCopy->Release();
        g_nr.hdrCopy = nullptr;
    }

    if (g_nr.colorSmall != nullptr)
    {
        g_nr.colorSmall->Release();
        g_nr.colorSmall = nullptr;
    }"""
assert old in text
text = text.replace(old, new, 1)

io.open(PATH, "w", encoding="utf-8").write(text)
print("finished-frame path runs at the working resolution")
