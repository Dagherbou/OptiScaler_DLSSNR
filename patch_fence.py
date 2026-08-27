"""Waits for a command allocator's previous submission before resetting it.

The finished-frame path hung the GPU: DXGI_ERROR_DEVICE_HUNG, a few seconds in. It reset one of three
allocators every present and never waited for the work already recorded on it, which is undefined the
moment the ring wraps -- and with frame generation presenting about twice per rendered frame, it wraps
almost immediately. The overlay gets away with the same shape only because it records nothing unless the
menu is open.

A fence per allocator fixes it properly. The wait costs nothing unless the pass is genuinely more than
three presents ahead of the GPU, in which case waiting is the correct thing to do.
"""

import io

PATH = "C:/Games_Temp/OptiScaler/OptiScaler/dlssnr/DlssNr_Dx12.cpp"
text = io.open(PATH, encoding="utf-8").read()

old = """    ID3D12CommandAllocator* presentAllocators[kPresentAllocators] = {};
    ID3D12GraphicsCommandList* presentList = nullptr;"""
new = """    ID3D12CommandAllocator* presentAllocators[kPresentAllocators] = {};
    ID3D12GraphicsCommandList* presentList = nullptr;

    // An allocator cannot be reset while the GPU is still reading the commands recorded into it, and
    // there is nothing else here to serialise against -- this list is submitted independently of the
    // game's own work.
    ID3D12Fence* presentFence = nullptr;
    HANDLE presentFenceEvent = nullptr;
    unsigned long long presentFenceValues[kPresentAllocators] = {};
    unsigned long long presentFenceNext = 0;"""
assert old in text
text = text.replace(old, new, 1)

old = """    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_nr.presentAllocators[0],
                                         nullptr, IID_PPV_ARGS(&g_nr.presentList))))
        return false;

    g_nr.presentList->Close();
    return true;
}"""
new = """    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_nr.presentAllocators[0],
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
}"""
assert old in text
text = text.replace(old, new, 1)

old = """    ID3D12CommandAllocator* allocator = g_nr.presentAllocators[backBufferIndex % kPresentAllocators];

    if (FAILED(allocator->Reset()) || FAILED(g_nr.presentList->Reset(allocator, nullptr)))
    {
        device->Release();
        return;
    }"""
new = """    const unsigned int slot = backBufferIndex % kPresentAllocators;
    ID3D12CommandAllocator* allocator = g_nr.presentAllocators[slot];

    WaitForAllocator(slot);

    if (FAILED(allocator->Reset()) || FAILED(g_nr.presentList->Reset(allocator, nullptr)))
    {
        device->Release();
        return;
    }"""
assert old in text
text = text.replace(old, new, 1)

old = """    if (SUCCEEDED(cmdList->Close()))
    {
        ID3D12CommandList* lists[] = { cmdList };
        queue->ExecuteCommandLists(1, lists);
    }"""
new = """    if (SUCCEEDED(cmdList->Close()))
    {
        ID3D12CommandList* lists[] = { cmdList };
        queue->ExecuteCommandLists(1, lists);

        // Recorded against this allocator, so the next pass round the ring knows what to wait for.
        ++g_nr.presentFenceNext;

        if (SUCCEEDED(queue->Signal(g_nr.presentFence, g_nr.presentFenceNext)))
            g_nr.presentFenceValues[slot] = g_nr.presentFenceNext;
    }"""
assert old in text
text = text.replace(old, new, 1)

old = """    if (g_nr.presentList != nullptr)
    {
        g_nr.presentList->Release();
        g_nr.presentList = nullptr;
    }"""
new = """    if (g_nr.presentList != nullptr)
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
    }"""
assert old in text
text = text.replace(old, new, 1)

io.open(PATH, "w", encoding="utf-8").write(text)
print("fence added")
