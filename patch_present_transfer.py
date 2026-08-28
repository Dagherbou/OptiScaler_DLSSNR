"""Gives the finished-frame path the same difference transfer the other one has.

Until now it copied the model's answer over the frame wholesale, so there was no dial: the model's
output either replaced the frame or the pass was off. The other inject point applies the answer as the
difference from what the model was shown, which makes strength zero a bit-exact bypass and lets the edit
be scaled -- including down, which is the right control for "I want some of this, not all of it".

No colour conversion is involved here: the frame is already display-referred, so the codec runs in
passthrough and the transfer is the only thing it does. That also brings the debug views to this path,
which is where they are most useful, since this is the picture the player actually sees.

The back buffer cannot be written directly by a compute shader -- a swapchain buffer is not created for
unordered access -- so the resolve writes to a scratch texture that is then copied over it.
"""

import io

PATH = "C:/Games_Temp/OptiScaler/OptiScaler/dlssnr/DlssNr_Dx12.cpp"
text = io.open(PATH, encoding="utf-8").read()

# --- the resolve needs its own destination in this path too -------------------------------------------

old = """    if (g_nr.output == nullptr)
    {
        g_nr.output = CreateScratch(device, scratchFormat, width, height);
        g_nr.colorCopy = CreateScratch(device, scratchFormat, width, height);
    }

    if (g_nr.output == nullptr || g_nr.colorCopy == nullptr)"""
new = """    if (g_nr.output == nullptr)
    {
        g_nr.output = CreateScratch(device, scratchFormat, width, height);
        g_nr.colorCopy = CreateScratch(device, scratchFormat, width, height);
        // The resolve cannot write the back buffer directly: a swapchain buffer is not created for
        // unordered access. It writes here and this is copied over the frame.
        g_nr.hdrCopy = CreateScratch(device, scratchFormat, width, height);
    }

    if (g_nr.output == nullptr || g_nr.colorCopy == nullptr || g_nr.hdrCopy == nullptr)"""
assert old in text
text = text.replace(old, new, 1)

old = """        if (g_nr.colorCopy != nullptr)
        {
            g_nr.colorCopy->Release();
            g_nr.colorCopy = nullptr;
        }
    }

    if (g_nr.output == nullptr)
    {
        g_nr.output = CreateScratch(device, scratchFormat, width, height);"""
new = """        if (g_nr.colorCopy != nullptr)
        {
            g_nr.colorCopy->Release();
            g_nr.colorCopy = nullptr;
        }

        if (g_nr.hdrCopy != nullptr)
        {
            g_nr.hdrCopy->Release();
            g_nr.hdrCopy = nullptr;
        }
    }

    if (g_nr.output == nullptr)
    {
        g_nr.output = CreateScratch(device, scratchFormat, width, height);"""
assert old in text
text = text.replace(old, new, 1)

# --- the codec has to exist here ------------------------------------------------------------------------

old = """    if (!EnsureForwarder() || !EnsureCapabilityParams(device) || !EnsurePresentList(device))"""
new = """    if (!EnsureForwarder() || !EnsureCapabilityParams(device) || !EnsurePresentList(device) ||
        !g_codec.ensure(device))"""
assert old in text
text = text.replace(old, new, 1)

# --- write the answer back as a difference ---------------------------------------------------------------

old = """    if (result == NVSDK_NGX_Result_Success)
    {
        Barrier(cmdList, g_nr.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cmdList, backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        cmdList->CopyResource(backBuffer, g_nr.output);
        Barrier(cmdList, g_nr.output, D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Barrier(cmdList, backBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    }"""
new = """    if (result == NVSDK_NGX_Result_Success)
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

        Barrier(cmdList, g_nr.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        g_codec.dispatch(cmdList, resolveParams, g_nr.colorCopy, g_nr.output, g_nr.colorCopy,
                         g_nr.hdrCopy, nullptr);
        Barrier(cmdList, g_nr.output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        Barrier(cmdList, g_nr.hdrCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cmdList, backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        cmdList->CopyResource(backBuffer, g_nr.hdrCopy);
        Barrier(cmdList, g_nr.hdrCopy, D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Barrier(cmdList, backBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    }"""
assert old in text
text = text.replace(old, new, 1)

io.open(PATH, "w", encoding="utf-8").write(text)
print("present path patched")

# --- the highlight rolloff only makes sense where a curve was applied --------------------------------------

PATH = "C:/Games_Temp/OptiScaler/OptiScaler/dlssnr/DlssNr_Codec.h"
text = io.open(PATH, encoding="utf-8").read()

old = """    // Near white the proxy has lost the information the model would need, and anything it invents there
    // is guesswork on a light source. Fade it out rather than trust it.
    float proxyLuma = dot(proxy, kLuma);
    applied *= 1.0 - smoothstep(0.85, 1.0, proxyLuma);"""
new = """    // Near white a compressed proxy has lost the information the model would need, and scaling its
    // answer back up amplifies whatever it invents there. That only applies where a curve was used: with
    // nothing compressed there is nothing to amplify, and rolling off would just discard detail in every
    // bright part of an ordinary frame.
    if (gPassthrough == 0)
    {
        float proxyLuma = dot(proxy, kLuma);
        applied *= 1.0 - smoothstep(0.85, 1.0, proxyLuma);
    }"""
assert old in text
text = text.replace(old, new, 1)

io.open(PATH, "w", encoding="utf-8").write(text)
print("codec patched")
