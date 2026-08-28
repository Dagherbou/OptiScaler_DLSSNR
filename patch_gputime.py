"""Reports what Neural Rendering actually costs, in the breakdown the upscaler already uses.

Every strength control so far makes the model do the same work and then blends the result away, so
"is it worth it" has been a matter of opinion. It is measured now, and it appears under Extra shaders
rather than with the upscaler, because it is not part of upscaler time -- it runs after it, and on the
finished-frame path it runs at present, on its own command list.

The whole pass is timed, not just the model: the staging copies and the resolve are part of what it
costs, and hiding them would flatter the number.
"""

import io

# --- the module keeps a timer and reports its last reading ---------------------------------------------

PATH = "C:/Games_Temp/OptiScaler/OptiScaler/dlssnr/DlssNr_Dx12.cpp"
text = io.open(PATH, encoding="utf-8").read()

text = text.replace('#include <proxies/NVNGX_Proxy.h>',
                    '#include <proxies/NVNGX_Proxy.h>\n#include <gpu_time/GpuTime_Dx12.h>', 1)

old = """NrState g_nr;
codec::Codec g_codec;"""
new = """NrState g_nr;
codec::Codec g_codec;

// What the pass costs on the GPU, for the breakdown in the overlay.
std::unique_ptr<GpuTime_Dx12> g_gpuTime;
std::optional<double> g_lastGpuTime;"""
assert old in text
text = text.replace(old, new, 1)

# --- time the finished-frame pass -----------------------------------------------------------------------

old = """    ID3D12GraphicsCommandList* cmdList = g_nr.presentList;
"""
new = """    ID3D12GraphicsCommandList* cmdList = g_nr.presentList;

    if (g_gpuTime == nullptr)
        g_gpuTime = std::make_unique<GpuTime_Dx12>(device);
"""
assert old in text
text = text.replace(old, new, 1)

old = """    // The frame is already display-referred here -- it has been through the game's own tonemapper --
    // so it goes to the model exactly as it is. No encode, no white point, nothing to invert.
    Barrier(cmdList, backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);"""
new = """    // Timed across the whole pass: the staging copies and the resolve are part of what this costs, and
    // timing only the model would flatter the number.
    if (g_gpuTime != nullptr)
        g_gpuTime->Start(cmdList);

    // The frame is already display-referred here -- it has been through the game's own tonemapper --
    // so it goes to the model exactly as it is. No encode, no white point, nothing to invert.
    Barrier(cmdList, backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);"""
assert old in text
text = text.replace(old, new, 1)

old = """    if (SUCCEEDED(cmdList->Close()))
    {
        ID3D12CommandList* lists[] = { cmdList };
        queue->ExecuteCommandLists(1, lists);"""
new = """    if (g_gpuTime != nullptr)
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
        }"""
assert old in text
text = text.replace(old, new, 1)

# --- time the before-frame-generation pass ---------------------------------------------------------------

old = """    codec::Params encodeParams {};
    encodeParams.mode = codec::MODE_ENCODE;"""
new = """    if (g_gpuTime == nullptr)
        g_gpuTime = std::make_unique<GpuTime_Dx12>(device);

    if (g_gpuTime != nullptr)
        g_gpuTime->Start(cmdList);

    codec::Params encodeParams {};
    encodeParams.mode = codec::MODE_ENCODE;"""
assert old in text
text = text.replace(old, new, 1)

old = """    Barrier(cmdList, g_nr.hdrCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Put any guide clones back where the next frame's copy expects to find them."""
new = """    Barrier(cmdList, g_nr.hdrCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
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

    // Put any guide clones back where the next frame's copy expects to find them."""
assert old in text
text = text.replace(old, new, 1)

# --- expose it, and let go of the timer on shutdown --------------------------------------------------------

old = """float CurrentWhitePoint() { return g_autoWhitePointSettled ? g_autoWhitePoint : 0.0f; }"""
new = """float CurrentWhitePoint() { return g_autoWhitePointSettled ? g_autoWhitePoint : 0.0f; }

std::optional<double> LastGpuTime() { return g_lastGpuTime; }"""
assert old in text
text = text.replace(old, new, 1)

old = """    g_codec.destroy();"""
new = """    g_gpuTime.reset();
    g_lastGpuTime.reset();

    g_codec.destroy();"""
assert old in text
text = text.replace(old, new, 1)

io.open(PATH, "w", encoding="utf-8").write(text)
print("module timed")

# --- header --------------------------------------------------------------------------------------------

PATH = "C:/Games_Temp/OptiScaler/OptiScaler/dlssnr/DlssNr_Dx12.h"
text = io.open(PATH, encoding="utf-8").read()
old = """float CurrentWhitePoint();"""
new = """float CurrentWhitePoint();

// What the pass last cost on the GPU, in milliseconds, or nothing if it has not been measured yet.
std::optional<double> LastGpuTime();"""
assert old in text
text = text.replace(old, new, 1)
io.open(PATH, "w", encoding="utf-8").write(text)
print("header updated")
