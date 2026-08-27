"""Adds a second injection point: the finished frame, at present.

Two honest trade-offs rather than one compromise.

Before frame generation, the model sees a proxy of the upscaler's linear HDR output, because the game's
own tonemapper has not run yet. The proxy is a Reinhard approximation, not what the game would have
produced, so the answer has to be applied conservatively. It costs one run per rendered frame and
generated frames inherit the result.

At present, the frame has already been through the game's tonemapper. It is exactly the sort of picture
the model was trained on -- so there is no colour codec here at all, and the answer is used as it comes.
That is the better image, and it is what the ReShade build was doing. It costs a run per presented
frame, so roughly double with frame generation, and each generated frame is enhanced independently.

The guides have to be cloned unconditionally for this path. By the time present comes round the game has
long since moved on, and the buffers it handed NGX during the upscale are not promised to still hold
this frame -- or to still exist.
"""

import io

PATH = "C:/Games_Temp/OptiScaler/OptiScaler/dlssnr/DlssNr_Dx12.cpp"
text = io.open(PATH, encoding="utf-8").read()

# --- state for the present path --------------------------------------------------------------------

old = """    // Only created when a game hands over typeless guides, which most do and Cyberpunk does not."""
new = """    // Cloned unconditionally when running at present, and only for typeless formats otherwise."""
assert old in text
text = text.replace(old, new, 1)

old = """    unsigned int width = 0;
    unsigned int height = 0;
    bool reset = true;"""
new = """    unsigned int width = 0;
    unsigned int height = 0;
    bool reset = true;

    // The present path records its own work: the overlay's command list only runs when the menu is
    // open, so it cannot be borrowed for something that has to happen every frame.
    ID3D12CommandAllocator* presentAllocators[kPresentAllocators] = {};
    ID3D12GraphicsCommandList* presentList = nullptr;

    // Dimensions of the guides as the upscaler handed them over, kept for the present path, which runs
    // long after that call has returned.
    unsigned int guideWidth = 0;
    unsigned int guideHeight = 0;
    bool guidesReady = false;"""
assert old in text
text = text.replace(old, new, 1)

text = text.replace(
    "struct NrState\n{",
    "// One per back buffer, so an allocator is never reset while its frame is still in flight.\nconstexpr unsigned int kPresentAllocators = 3;\n\nstruct NrState\n{",
    1,
)

# --- the present path itself -----------------------------------------------------------------------

PRESENT = '''
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
    return true;
}
'''

anchor = "} // namespace\n\nnamespace DlssNr"
assert anchor in text
text = text.replace(anchor, PRESENT + anchor, 1)

# --- public entry point ------------------------------------------------------------------------------

ENTRY = '''
void EvaluateAtPresent(ID3D12CommandQueue* queue, ID3D12Resource* backBuffer, unsigned int backBufferIndex)
{
    const Config& cfg = *Config::Instance();

    if (!cfg.DlssNrEnabled.value_or_default() || g_nr.failed || queue == nullptr || backBuffer == nullptr)
        return;

    if (cfg.DlssNrInjectPoint.value_or_default() != INJECT_PRESENT)
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

    if (!EnsureForwarder() || !EnsureCapabilityParams(device) || !EnsurePresentList(device))
    {
        g_nr.failed = true;
        LOG_ERROR("DLSS-NR unavailable: {}", g_nr.reason);
        device->Release();
        return;
    }

    if (g_nr.feature != nullptr && (g_nr.width != width || g_nr.height != height))
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
    }

    if (g_nr.output == nullptr)
    {
        g_nr.output = CreateScratch(device, scratchFormat, width, height);
        g_nr.colorCopy = CreateScratch(device, scratchFormat, width, height);
    }

    if (g_nr.output == nullptr || g_nr.colorCopy == nullptr)
    {
        g_nr.failed = true;
        g_nr.reason = "the staging textures could not be created";
        device->Release();
        return;
    }

    ID3D12CommandAllocator* allocator = g_nr.presentAllocators[backBufferIndex % kPresentAllocators];

    if (FAILED(allocator->Reset()) || FAILED(g_nr.presentList->Reset(allocator, nullptr)))
    {
        device->Release();
        return;
    }

    ID3D12GraphicsCommandList* cmdList = g_nr.presentList;

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
                        device, cmdList, g_nr.capabilityParams, width, height,
                        (int) cfg.DlssNrPreset.value_or_default());

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
        LOG_INFO("DLSS-NR running on the finished frame at {}x{}, guides {}x{}", width, height,
                 g_nr.guideWidth, g_nr.guideHeight);
    }

    // The frame is already display-referred here -- it has been through the game's own tonemapper --
    // so it goes to the model exactly as it is. No encode, no white point, nothing to invert.
    Barrier(cmdList, backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(cmdList, g_nr.colorCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->CopyResource(g_nr.colorCopy, backBuffer);
    Barrier(cmdList, g_nr.colorCopy, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    Barrier(cmdList, g_nr.depthClone, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cmdList, g_nr.motionClone, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    const int result = g_nr.evaluate(
        cmdList, g_nr.feature, g_nr.capabilityParams, g_nr.colorCopy, g_nr.depthClone, g_nr.motionClone,
        g_nr.output, width, height, g_nr.guideWidth, g_nr.guideHeight, 1, g_nr.reset ? 1 : 0,
        cfg.DlssNrIntensity.value_or_default(), (int) cfg.DlssNrStyle.value_or_default(),
        cfg.DlssNrLocalStructure.value_or_default(), cfg.DlssNrLocalTone.value_or_default(),
        cfg.DlssNrSkinStructure.value_or_default(), cfg.DlssNrAutoMask.value_or_default() ? 1 : 0);

    g_nr.reset = false;

    Barrier(cmdList, g_nr.depthClone, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_DEST);
    Barrier(cmdList, g_nr.motionClone, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_DEST);

    if (result == NVSDK_NGX_Result_Success)
    {
        Barrier(cmdList, g_nr.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cmdList, backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        cmdList->CopyResource(backBuffer, g_nr.output);
        Barrier(cmdList, g_nr.output, D3D12_RESOURCE_STATE_COPY_SOURCE,
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

    Barrier(cmdList, g_nr.colorCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if (SUCCEEDED(cmdList->Close()))
    {
        ID3D12CommandList* lists[] = { cmdList };
        queue->ExecuteCommandLists(1, lists);
    }

    device->Release();
}
'''

anchor = "bool IsRunning() { return g_nr.feature != nullptr && !g_nr.failed; }"
assert anchor in text
text = text.replace(anchor, ENTRY.strip() + "\n\n" + anchor, 1)

# --- the upscale path defers to it -------------------------------------------------------------------

old = """    if (!EnsureForwarder() || !EnsureCapabilityParams(device))
    {
        g_nr.failed = true;
        LOG_ERROR("DLSS-NR unavailable: {}", g_nr.reason);
        device->Release();
        return;
    }"""
new = """    // When the model runs on the finished frame instead, this call exists only to take a copy of the
    // guides while they are still valid and still describe this frame.
    if (cfg.DlssNrInjectPoint.value_or_default() == INJECT_PRESENT)
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
    }"""
assert old in text
text = text.replace(old, new, 1)

# --- teardown ------------------------------------------------------------------------------------------

old = """    g_codec.destroy();"""
new = """    if (g_nr.presentList != nullptr)
    {
        g_nr.presentList->Release();
        g_nr.presentList = nullptr;
    }

    for (unsigned int i = 0; i < kPresentAllocators; ++i)
    {
        if (g_nr.presentAllocators[i] != nullptr)
        {
            g_nr.presentAllocators[i]->Release();
            g_nr.presentAllocators[i] = nullptr;
        }
    }

    g_codec.destroy();"""
assert old in text
text = text.replace(old, new, 1)

io.open(PATH, "w", encoding="utf-8").write(text)
print("module patched")

# --- header ---------------------------------------------------------------------------------------------

PATH = "C:/Games_Temp/OptiScaler/OptiScaler/dlssnr/DlssNr_Dx12.h"
text = io.open(PATH, encoding="utf-8").read()
text = text.replace(
    "namespace DlssNr\n{",
    """namespace DlssNr
{
// Where the model runs. Two genuine trade-offs rather than one compromise: before frame generation it
// costs one run per rendered frame and generated frames inherit the result, but the game's tonemapper
// has not run yet so it works on a proxy; on the finished frame it sees exactly the sort of picture it
// was trained on, at a run per presented frame.
constexpr unsigned int INJECT_BEFORE_FG = 0;
constexpr unsigned int INJECT_PRESENT = 1;""",
    1,
)
text = text.replace(
    "// Whether the model is loaded and running, for the overlay.",
    """// Runs the model over the finished frame, on a command list of its own, and submits it. Called every
// present; does nothing unless that inject point is selected.
void EvaluateAtPresent(ID3D12CommandQueue* queue, ID3D12Resource* backBuffer, unsigned int backBufferIndex);

// Whether the model is loaded and running, for the overlay.""",
    1,
)
io.open(PATH, "w", encoding="utf-8").write(text)
print("header patched")
