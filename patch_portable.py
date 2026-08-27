"""Makes the DLSS-NR pass survive games other than the one it was written against.

Three things stood in the way of dropping this build into any DLSS title:

  * The guides were passed straight to the model. A depth buffer is very often declared typeless, and a
    typeless resource cannot be viewed -- NGX builds its own views and has nothing to tell it which
    format to use. Cyberpunk happens to hand over a typed one. Most games do not.

  * The snippet and the forwarder were only looked for beside OptiScaler. A user dropping this into a
    game folder may well put them beside the executable instead.

  * The forwarder was not in the build output, so the package was not actually drop-in.
"""

import io

PATH = "C:/Games_Temp/OptiScaler/OptiScaler/dlssnr/DlssNr_Dx12.cpp"
text = io.open(PATH, encoding="utf-8").read()

# --- typed clones for the guides ------------------------------------------------------------------

HELPERS = '''// A typeless resource cannot be viewed, and NGX builds its own views with nothing to tell it which
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

'''

anchor = "// The upscaler's own names differ between super resolution and ray reconstruction"
assert anchor in text
text = text.replace(anchor, HELPERS + anchor, 1)

# Clone handles on the state struct.
text = text.replace(
    "    ID3D12Resource* colorCopy = nullptr;\n    ID3D12Resource* output = nullptr;",
    """    ID3D12Resource* colorCopy = nullptr;
    ID3D12Resource* output = nullptr;

    // Only created when a game hands over typeless guides, which most do and Cyberpunk does not.
    ID3D12Resource* depthClone = nullptr;
    ID3D12Resource* motionClone = nullptr;""",
    1,
)

# Use readable guides in the evaluate call.
old_eval = """    const int result = g_nr.evaluate(
        cmdList, g_nr.feature, g_nr.capabilityParams, g_nr.colorCopy, depth, motion, g_nr.output, width,"""
new_eval = """    ID3D12Resource* depthIn = ReadableGuide(device, cmdList, depth, &g_nr.depthClone);
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
        cmdList, g_nr.feature, g_nr.capabilityParams, g_nr.colorCopy, depthIn, motionIn, g_nr.output, width,"""
assert old_eval in text
text = text.replace(old_eval, new_eval, 1)

# Put the clones back where the next frame's copy expects them.
old_tail = "    // Leave the staging copy as the next frame expects to find it."
new_tail = """    // Put any guide clones back where the next frame's copy expects to find them.
    if (g_nr.depthClone != nullptr)
        Barrier(cmdList, g_nr.depthClone, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);

    if (g_nr.motionClone != nullptr)
        Barrier(cmdList, g_nr.motionClone, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);

    // Leave the staging copy as the next frame expects to find it."""
assert old_tail in text
text = text.replace(old_tail, new_tail, 1)

# --- look beside the executable too ----------------------------------------------------------------

old_fwd = """    const auto path = g_dllDir / L"nvngx.dll_dlssnr.dll";
    g_nr.forwarder = LoadLibraryW(path.wstring().c_str());

    if (g_nr.forwarder == nullptr)
    {
        LOG_ERROR("nvngx.dll_dlssnr.dll not found beside OptiScaler at {}", path.string());
        g_nr.reason = "nvngx.dll_dlssnr.dll is missing";
        return false;
    }"""
new_fwd = """    // Beside OptiScaler first, then beside the executable: someone dropping this into a game folder may
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

    const auto path = found.value() / L"nvngx.dll_dlssnr.dll";
    g_nr.forwarder = LoadLibraryW(path.wstring().c_str());

    if (g_nr.forwarder == nullptr)
    {
        LOG_ERROR("nvngx.dll_dlssnr.dll found at {} but would not load, error {}", path.string(),
                  GetLastError());
        g_nr.reason = "nvngx.dll_dlssnr.dll would not load";
        return false;
    }"""
assert old_fwd in text
text = text.replace(old_fwd, new_fwd, 1)

old_snip = """        const auto snippet = Util::FindFilePath(g_dllDir, "nvngx_dlssnr.dll");

        if (!snippet.has_value())"""
new_snip = """        auto snippet = Util::FindFilePath(g_dllDir, "nvngx_dlssnr.dll");

        if (!snippet.has_value())
            snippet = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx_dlssnr.dll");

        if (!snippet.has_value())"""
assert old_snip in text
text = text.replace(old_snip, new_snip, 1)

text = text.replace(
    """            g_nr.reason = "nvngx_dlssnr.dll was not found";""",
    """            g_nr.reason = "nvngx_dlssnr.dll was not found beside OptiScaler or the game";""",
    1,
)

# Release the clones on shutdown.
text = text.replace(
    """    if (g_nr.colorCopy != nullptr)
    {
        g_nr.colorCopy->Release();
        g_nr.colorCopy = nullptr;
    }

    g_codec.destroy();""",
    """    if (g_nr.colorCopy != nullptr)
    {
        g_nr.colorCopy->Release();
        g_nr.colorCopy = nullptr;
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

    g_codec.destroy();""",
    1,
)

io.open(PATH, "w", encoding="utf-8").write(text)
print("module patched")
