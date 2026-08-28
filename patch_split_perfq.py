import io

p = 'OptiScaler/inputs/NVNGX_DLSS_Dx12.cpp'
t = io.open(p, encoding='utf-8').read()

# --- state ---
old = """    bool lastWant = false;
    int stableFrames = 0;"""
assert old in t
t = t.replace(old, old + """

    // The game's own quality-mode declaration, captured with the display size. A feature built at a
    // ratio that contradicts the block's declared quality mode is created fine and then refused at
    // evaluate (0xbad00000) -- so every geometry the split asks for carries a matching declaration,
    // and the game's own is restored with its geometry.
    unsigned int origPerfQuality = 0xffffffff;

    // The driver refused the supersampled enlargement once this session: run at display size instead
    // of latching the whole split off. Cleared on a toggle edge or Retry.
    bool supersampleRefused = false;""", 1)

# --- SplitRatio honours the refusal ---
old = """static float SplitRatio()
{
    if (!SplitOsIntent())
        return 1.0f;"""
assert old in t
t = t.replace(old, """static float SplitRatio()
{
    if (!SplitOsIntent() || SplitDx12.supersampleRefused)
        return 1.0f;""", 1)

# --- helper ---
old = "// What the Ray Reconstruction feature's output size should be under the current settings."
assert old in t
t = t.replace(old, """// The quality mode that honestly describes an upscale from renderW to targetW. The thresholds sit
// between the modes' nominal ratios (Quality 1.5x, Balanced 1.72x, Performance 2x, Ultra
// Performance 3x).
static unsigned int SplitPerfQuality(unsigned int renderW, unsigned int targetW)
{
    const float r = renderW == 0 ? 1.0f : (float) targetW / (float) renderW;

    if (r >= 2.5f)
        return NVSDK_NGX_PerfQuality_Value_UltraPerformance;

    if (r >= 1.85f)
        return NVSDK_NGX_PerfQuality_Value_MaxPerf;

    if (r >= 1.6f)
        return NVSDK_NGX_PerfQuality_Value_Balanced;

    return NVSDK_NGX_PerfQuality_Value_MaxQuality;
}

""" + old, 1)

# --- mid-flight steering: shadow the quality mode ---
old = """        if (want && SplitDx12.geometryOwned && SplitDx12.lastDesiredWidth != 0)
        {
            params->Set(NVSDK_NGX_Parameter_OutWidth, SplitDx12.lastDesiredWidth);
            params->Set(NVSDK_NGX_Parameter_OutHeight, SplitDx12.lastDesiredHeight);
        }
        else if (SplitDx12.restorePending && SplitDx12.displayWidth != 0)
        {
            params->Set(NVSDK_NGX_Parameter_OutWidth, SplitDx12.displayWidth);
            params->Set(NVSDK_NGX_Parameter_OutHeight, SplitDx12.displayHeight);
        }"""
assert old in t
t = t.replace(old, """        if (want && SplitDx12.geometryOwned && SplitDx12.lastDesiredWidth != 0)
        {
            params->Set(NVSDK_NGX_Parameter_OutWidth, SplitDx12.lastDesiredWidth);
            params->Set(NVSDK_NGX_Parameter_OutHeight, SplitDx12.lastDesiredHeight);

            unsigned int rw = 0;
            params->Get(NVSDK_NGX_Parameter_Width, &rw);

            if (rw != 0 && SplitDx12.displayWidth != 0 &&
                SplitDx12.lastDesiredWidth > SplitDx12.displayWidth)
                params->Set(NVSDK_NGX_Parameter_PerfQualityValue,
                            SplitPerfQuality(rw, SplitDx12.lastDesiredWidth));
        }
        else if (SplitDx12.restorePending && SplitDx12.displayWidth != 0)
        {
            params->Set(NVSDK_NGX_Parameter_OutWidth, SplitDx12.displayWidth);
            params->Set(NVSDK_NGX_Parameter_OutHeight, SplitDx12.displayHeight);

            if (SplitDx12.origPerfQuality != 0xffffffff)
                params->Set(NVSDK_NGX_Parameter_PerfQualityValue, SplitDx12.origPerfQuality);
        }""", 1)

# --- toggle edge clears the refusal so a fresh attempt is possible ---
old = """    if (want != SplitDx12.lastWant)
    {
        SplitDx12.lastWant = want;
        SplitDx12.stableFrames = 0;
        return;
    }"""
assert old in t
t = t.replace(old, """    if (want != SplitDx12.lastWant)
    {
        SplitDx12.lastWant = want;
        SplitDx12.stableFrames = 0;
        SplitDx12.supersampleRefused = false;
        return;
    }""", 1)

# --- capture the game's quality mode with the display size ---
old = """    if (SplitDx12.displayWidth == 0 && w != 0 && ow > w)
    {
        SplitDx12.displayWidth = ow;
        SplitDx12.displayHeight = oh;
    }"""
assert old in t
t = t.replace(old, """    if (SplitDx12.displayWidth == 0 && w != 0 && ow > w)
    {
        SplitDx12.displayWidth = ow;
        SplitDx12.displayHeight = oh;
        params->Get(NVSDK_NGX_Parameter_PerfQualityValue, &SplitDx12.origPerfQuality);
    }""", 1)

# --- arm: declare the quality mode the new geometry actually is ---
old = """        ++SplitDx12.armTries;
        params->Set(NVSDK_NGX_Parameter_OutWidth, desiredW);
        params->Set(NVSDK_NGX_Parameter_OutHeight, desiredH);"""
assert old in t
t = t.replace(old, """        ++SplitDx12.armTries;
        params->Set(NVSDK_NGX_Parameter_OutWidth, desiredW);
        params->Set(NVSDK_NGX_Parameter_OutHeight, desiredH);

        // A feature built oversized must declare the quality mode it actually is, or the driver
        // creates it and then refuses every evaluate. The 1:1 arrangement keeps the game's own value.
        if (desiredW > SplitDx12.displayWidth && SplitDx12.displayWidth != 0)
            params->Set(NVSDK_NGX_Parameter_PerfQualityValue,
                        SplitPerfQuality(f->RenderWidth(), desiredW));
        else if (SplitDx12.origPerfQuality != 0xffffffff)
            params->Set(NVSDK_NGX_Parameter_PerfQualityValue, SplitDx12.origPerfQuality);""", 1)

# --- disarm: give the declaration back with the geometry ---
old = """        params->Set(NVSDK_NGX_Parameter_OutWidth, SplitDx12.displayWidth);
        params->Set(NVSDK_NGX_Parameter_OutHeight, SplitDx12.displayHeight);
        state.newBackend = Upscaler::DLSSD;
        state.changeBackend[handleId] = true;
        SplitDx12.geometryOwned = false;
        SplitDx12.restorePending = true;"""
assert old in t
t = t.replace(old, """        params->Set(NVSDK_NGX_Parameter_OutWidth, SplitDx12.displayWidth);
        params->Set(NVSDK_NGX_Parameter_OutHeight, SplitDx12.displayHeight);

        if (SplitDx12.origPerfQuality != 0xffffffff)
            params->Set(NVSDK_NGX_Parameter_PerfQualityValue, SplitDx12.origPerfQuality);

        state.newBackend = Upscaler::DLSSD;
        state.changeBackend[handleId] = true;
        SplitDx12.geometryOwned = false;
        SplitDx12.restorePending = true;""", 1)

# --- mode B evaluate refusal: drop Include RR instead of toasting forever ---
old = """        params->Set(NVSDK_NGX_Parameter_Output, SplitDx12.oversized);
        const NVSDK_NGX_Result rrResult = TryEvaluateOptiFeature(cmdList, handle, params, callback);

        if (rrResult != NVSDK_NGX_Result_Success)
        {
            params->Set(NVSDK_NGX_Parameter_Output, gameOutput);
            *outResult = rrResult;
            return true;
        }"""
assert old in t
t = t.replace(old, """        params->Set(NVSDK_NGX_Parameter_Output, SplitDx12.oversized);
        const NVSDK_NGX_Result rrResult = TryEvaluateOptiFeature(cmdList, handle, params, callback);

        if (rrResult != NVSDK_NGX_Result_Success)
        {
            // The feature matched and was stable, so this is a genuine refusal of the supersampled
            // arrangement. Drop Include RR (runtime only -- the saved checkbox survives) and let the
            // manager re-arm the plain split, rather than failing every frame from here on.
            params->Set(NVSDK_NGX_Parameter_Output, gameOutput);
            LOG_ERROR("DLSS-NR split: Ray Reconstruction refused to run at {}x{} -> {}x{}; dropping "
                      "Include RR",
                      renderW, renderH, targetW, targetH);
            Config::Instance()->DlssNrSplitIncludeRR.set_volatile_value(false);
            DlssNr::SetSplitStatus("include-RR refused at this ratio; running the split without it");
            *outResult = rrResult;
            return true;
        }""", 1)

# --- internal SR creation: declare its own ratio, then hand the block back ---
old = """    if (SplitDx12.sr == nullptr)
    {
        params->Set(NVSDK_NGX_Parameter_OutWidth, targetW);
        params->Set(NVSDK_NGX_Parameter_OutHeight, targetH);

        auto sr = std::make_unique<DLSSFeatureDx12>(IFeature::GetNextHandleId(), params);

        if (!sr->Init(D3D12Device, cmdList, params) || !sr->IsInited())
        {
            LOG_ERROR("DLSS-NR split: the internal Super Resolution feature would not initialise; "
                      "falling back");
            SplitDx12.failed = true;
            DlssNr::SetSplitActive(false);
            DlssNr::SetSplitStatus("failed; conventional path running (see the log)");
            params->Set(NVSDK_NGX_Parameter_Output, gameOutput);
            params->Set(NVSDK_NGX_Parameter_OutWidth, SplitDx12.displayWidth);
            params->Set(NVSDK_NGX_Parameter_OutHeight, SplitDx12.displayHeight);
            *outResult = rrResult;
            return true;
        }"""
assert old in t
t = t.replace(old, """    if (SplitDx12.sr == nullptr)
    {
        params->Set(NVSDK_NGX_Parameter_OutWidth, targetW);
        params->Set(NVSDK_NGX_Parameter_OutHeight, targetH);

        // The enlargement parses the game's block, whose quality mode describes the game's own ratio.
        // Built supersampled, it must declare what it actually is or the driver refuses it at evaluate.
        params->Set(NVSDK_NGX_Parameter_PerfQualityValue, SplitPerfQuality(renderW, targetW));

        auto sr = std::make_unique<DLSSFeatureDx12>(IFeature::GetNextHandleId(), params);
        const bool srInited = sr->Init(D3D12Device, cmdList, params) && sr->IsInited();

        if (SplitDx12.origPerfQuality != 0xffffffff)
            params->Set(NVSDK_NGX_Parameter_PerfQualityValue, SplitDx12.origPerfQuality);

        if (!srInited)
        {
            params->Set(NVSDK_NGX_Parameter_Output, gameOutput);
            params->Set(NVSDK_NGX_Parameter_OutWidth, SplitDx12.displayWidth);
            params->Set(NVSDK_NGX_Parameter_OutHeight, SplitDx12.displayHeight);

            if (supersample && !SplitDx12.supersampleRefused)
            {
                // The supersampled enlargement was refused; run at display size instead of dying.
                SplitDx12.supersampleRefused = true;
                LOG_ERROR("DLSS-NR split: the supersampled enlargement would not initialise; dropping "
                          "to display size");
                DlssNr::SetSplitStatus("supersample refused here; enlargement at display size");
                *outResult = rrResult;
                return true;
            }

            LOG_ERROR("DLSS-NR split: the internal Super Resolution feature would not initialise; "
                      "falling back");
            SplitDx12.failed = true;
            DlssNr::SetSplitActive(false);
            DlssNr::SetSplitStatus("failed; conventional path running (see the log)");
            *outResult = rrResult;
            return true;
        }""", 1)

io.open(p, 'w', encoding='utf-8').write(t)
print('perfQ steering in')
