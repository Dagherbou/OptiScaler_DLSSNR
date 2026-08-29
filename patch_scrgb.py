import io

p = 'OptiScaler/dlssnr/DlssNr_Dx12.cpp'
t = io.open(p, encoding='utf-8').read()

# --- state ---
old = "    ID3D12Resource* colorSmall = nullptr;"
assert old in t
t = t.replace(old, old + """
    ID3D12Resource* presentProxy = nullptr; // scRGB finished frame: the encoded picture the model sees""", 1)

old = """probe::FrameReducer g_reducer;
probe::BlockReader g_reader;"""
assert old in t
t = t.replace(old, old + """

// The finished-frame path measures its own white point: an scRGB backbuffer lives in display-referred
// linear (1.0 = paper white sits well above it), a different world from the game's internal buffer.
probe::FrameReducer g_presentReducer;
probe::BlockReader g_presentReader;""", 1)

old = """float g_autoWhitePoint = 2.0f;
bool g_autoWhitePointSettled = false;"""
assert old in t
t = t.replace(old, old + """
float g_presentWhite = 3.0f;
bool g_presentWhiteSettled = false;""", 1)

# --- format-change release list ---
old = "    for (ID3D12Resource** r : { &g_nr.output, &g_nr.colorCopy, &g_nr.hdrCopy, &g_nr.colorSmall })"
assert old in t
t = t.replace(old,
    "    for (ID3D12Resource** r :\n"
    "         { &g_nr.output, &g_nr.colorCopy, &g_nr.hdrCopy, &g_nr.colorSmall, &g_nr.presentProxy })", 1)

# --- shutdown release ---
old = """    if (g_nr.colorSmall != nullptr)
    {
        g_nr.colorSmall->Release();
        g_nr.colorSmall = nullptr;
    }
"""
assert old in t
t = t.replace(old, old + """
    if (g_nr.presentProxy != nullptr)
    {
        g_nr.presentProxy->Release();
        g_nr.presentProxy = nullptr;
    }
""", 1)

# --- the staging comment + copy block, followed by the scRGB encode ---
old = """    // The frame is already display-referred here -- it has been through the game's own tonemapper --
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
"""
assert old in t
new = """    // An SDR frame is already in the model's world -- finished and display-referred -- and goes over
    // exactly as it is. An scRGB HDR frame is not: its linear values run far past 1.0 at every bright
    // light, and the model, trained on finished tone-mapped pictures, reads a blazing lamp as an error
    // to calm down. That is the muted-highlights complaint. The same encode the render path uses maps
    // it into the model's world; the resolve maps the edit back, exactly inverted.
    Barrier(cmdList, backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(cmdList, g_nr.colorCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->CopyResource(g_nr.colorCopy, backBuffer);
    Barrier(cmdList, g_nr.colorCopy, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    const bool scRGB = backBuffer->GetDesc().Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    float presentWhite = 1.0f;
    bool hdrEncoded = false;

    // Shrink the frame for the model, when it is working below full resolution. The copy above stays at
    // full size and is what the edit is finally added to.
    ID3D12Resource* modelInput = g_nr.colorCopy;

    if (scRGB)
    {
        // The latest white point reading, eased like the render path's.
        const probe::Stats stats = g_presentReader.collect();

        if (stats.valid && stats.meanLuma > 0.0f)
        {
            const float targetWhite = WhitePointForMean(stats.meanLuma);

            if (!g_presentWhiteSettled)
            {
                g_presentWhite = targetWhite;
                g_presentWhiteSettled = true;
                LOG_INFO("DLSS-NR finished-frame white point settled at {:.3f} (frame mean {:.4f})",
                         g_presentWhite, stats.meanLuma);
            }
            else
            {
                g_presentWhite += (targetWhite - g_presentWhite) * kWhitePointBlend;
            }
        }

        if (g_nr.presentProxy != nullptr &&
            ((unsigned int) g_nr.presentProxy->GetDesc().Width != width ||
             g_nr.presentProxy->GetDesc().Height != height))
        {
            g_nr.presentProxy->Release();
            g_nr.presentProxy = nullptr;
        }

        if (g_nr.presentProxy == nullptr)
            g_nr.presentProxy = CreateScratch(device, backBuffer->GetDesc().Format, width, height);
    }

    if (scRGB && g_nr.presentProxy != nullptr)
    {
        presentWhite = g_presentWhite * cfg.DlssNrWhitePointScale.value_or_default();
        hdrEncoded = true;

        codec::Params encodeParams {};
        encodeParams.mode = codec::MODE_ENCODE;
        encodeParams.passthrough = 0;
        encodeParams.whitePoint = presentWhite;
        encodeParams.width = width;
        encodeParams.height = height;

        // colorCopy is the source and stays the untouched original the resolve reads; the keep output
        // is pointed at hdrCopy, which the resolve overwrites as its target later anyway.
        g_codec.dispatch(cmdList, encodeParams, g_nr.colorCopy, nullptr, nullptr, g_nr.presentProxy,
                         g_nr.hdrCopy);

        D3D12_RESOURCE_BARRIER keepHazard {};
        keepHazard.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        keepHazard.UAV.pResource = g_nr.hdrCopy;
        cmdList->ResourceBarrier(1, &keepHazard);

        // Measure for the white point on the render path's cadence.
        if ((g_frames % 30 == 0) && g_presentReducer.ensure(device))
        {
            ID3D12Resource* reducedFrame =
                g_presentReducer.dispatch(cmdList, g_nr.colorCopy, width, height);
            g_presentReader.capture(cmdList, reducedFrame, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        Barrier(cmdList, g_nr.presentProxy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        modelInput = g_nr.presentProxy;
    }
"""
t = t.replace(old, new, 1)

# --- the downsample reads the (possibly encoded) model input, not the raw copy ---
old = """        g_codec.dispatch(cmdList, down, g_nr.colorCopy, nullptr, nullptr, g_nr.colorSmall, nullptr);"""
assert old in t
t = t.replace(old, """        g_codec.dispatch(cmdList, down, modelInput, nullptr, nullptr, g_nr.colorSmall, nullptr);""", 1)

# --- the resolve decodes when the encode ran ---
old = """        // The frame the model was shown and the frame as it was are the same thing here, because nothing
        // was converted on the way in. So the resolve adds the model's edit to the frame rather than
        // replacing it: at strength zero the result is the original, bit for bit.
        codec::Params resolveParams {};
        resolveParams.mode = codec::MODE_RESOLVE;
        resolveParams.passthrough = 1;
        resolveParams.whitePoint = 1.0f;"""
assert old in t
t = t.replace(old, """        // For SDR the frame the model was shown and the frame as it was are the same thing, and the
        // resolve adds the edit at full scale. For an encoded scRGB frame the resolve decodes with the
        // same white point the encode used -- exact inverses, so at strength zero the result is the
        // original, bit for bit, in both worlds.
        codec::Params resolveParams {};
        resolveParams.mode = codec::MODE_RESOLVE;
        resolveParams.passthrough = hdrEncoded ? 0u : 1u;
        resolveParams.whitePoint = hdrEncoded ? presentWhite : 1.0f;"""
t = t.replace(old, new='', count=0) if False else t

io.open(p, 'w', encoding='utf-8').write(t)
print('scRGB finished-frame support in')
