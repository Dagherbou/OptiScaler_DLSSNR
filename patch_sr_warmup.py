import io

p = 'OptiScaler/inputs/NVNGX_DLSS_Dx12.cpp'
t = io.open(p, encoding='utf-8').read()

# --- state ---
old = """    int downscalerKind = -1;                // the Downscaler choice it was built with"""
assert old in t
t = t.replace(old, old + """

    // The enlargement was created this frame: its NGX creation commands are recorded in the game's
    // command list but not yet executed, and NGX requires them executed before the first evaluate.
    // Evaluating in the same list is a dice-roll that sometimes deadlocks the GPU (both session
    // crashes died on exactly the creation frame). Skip one frame instead, like OptiScaler's own
    // recreation counter does.
    bool srJustCreated = false;""", 1)

# --- creation marks the warmup; the evaluate stands down for exactly that frame ---
old = """        SplitDx12.sr = std::move(sr);
        SplitDx12.srTargetWidth = targetW;
        LOG_INFO("DLSS-NR split: internal Super Resolution running {}x{} -> {}x{}{}", renderW, renderH,
                 targetW, targetH, supersample ? " (supersampled)" : "");
    }"""
assert old in t
new = """        SplitDx12.sr = std::move(sr);
        SplitDx12.srTargetWidth = targetW;
        SplitDx12.srJustCreated = true;
        LOG_INFO("DLSS-NR split: internal Super Resolution running {}x{} -> {}x{}{}", renderW, renderH,
                 targetW, targetH, supersample ? " (supersampled)" : "");
    }

    if (SplitDx12.srJustCreated)
    {
        // The creation commands go through the game's own submit first; the first evaluate happens
        // next frame. One frame keeps the previous image -- invisible inside a toggle's hitch.
        SplitDx12.srJustCreated = false;

        params->Set(NVSDK_NGX_Parameter_Output, gameOutput);
        params->Set(NVSDK_NGX_Parameter_OutWidth, SplitDx12.displayWidth);
        params->Set(NVSDK_NGX_Parameter_OutHeight, SplitDx12.displayHeight);
        DlssNr::SetSplitStatus("arming the enlargement...");
        *outResult = NVSDK_NGX_Result_Success;
        return true;
    }"""
t = t.replace(old, new, 1)

io.open(p, 'w', encoding='utf-8').write(t)
print('sr warmup frame in')
