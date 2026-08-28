"""The compounding-supersample crash, and the Output Scaling relationship the user actually asked for.

The crash first. The split writes its oversized target size into the game's parameter block for the
internal SR and never restored it; the next frame's capture mistook that value for the game's display
size; multiplied by the ratio again, every frame -- 2880, 4320, 6480, 9720 -- until DLSS refused the
ratio and the disarm 'restored' Ray Reconstruction to a polluted 6480x3645 and hung the device. Four
fixes: the display size is captured exactly once; the block's output size is restored after every
handled path; the target is capped at DLSS's own 4x ratio limit; and the parking lot is a list with
per-entry countdowns so rapid changes queue instead of forcing an early free.

Then the redesign: Output Scaling's Enable is the user's supersampling intent, and the split absorbs it.
Split on + OS on: the runtime Output Scaling flag is quietly forced off -- so no feature geometry fight
can exist -- and the split supersamples at the OS Ratio itself. Split off or failed: the flag is given
back and conventional Output Scaling resumes. OS on means supersampled, with or without the split, which
is what the user said it should mean.
"""

import io

ROOT = "C:/Games_Temp/OptiScaler/OptiScaler/"
p = ROOT + "inputs/NVNGX_DLSS_Dx12.cpp"
t = io.open(p, encoding="utf-8").read()

# --- 1. capture the display size exactly once --------------------------------------------------------

old = """    // The display size the game originally asked for, captured whenever the block shows it.
    if (w != 0 && ow > w && ow != SplitDx12.displayWidth &&
        (SplitDx12.displayWidth == 0 || ow > SplitDx12.displayWidth))
    {
        SplitDx12.displayWidth = ow;
        SplitDx12.displayHeight = oh;
    }"""
assert old in t, "capture"
new = """    // The display size the game originally asked for, captured exactly once. This block is also
    // written by us -- the internal SR's oversized target goes through it -- and treating later, larger
    // values as the game's own compounded the supersample every frame until the device hung.
    if (SplitDx12.displayWidth == 0 && w != 0 && ow > w)
    {
        SplitDx12.displayWidth = ow;
        SplitDx12.displayHeight = oh;
    }"""
t = t.replace(old, new, 1)

# --- 2. the parking lot becomes a list ---------------------------------------------------------------

old = """    // Retired on a live change: still referenced by command lists submitted over the last frames, so
    // they are released a number of evaluates later rather than on the spot.
    ID3D12Resource* parkedIntermediate = nullptr;
    ID3D12Resource* parkedOversized = nullptr;
    std::unique_ptr<IFeature_Dx12> parkedSr;
    std::unique_ptr<OS_Dx12> parkedDownscaler;
    int parkedCountdown = 0;
"""
assert old in t, "parked fields"
t = t.replace(old, "", 1)

old = "static SplitState SplitDx12;\n"
assert old in t
new = """static SplitState SplitDx12;

// Retired on a live change: still referenced by command lists submitted over the last frames, so each
// entry is released a number of evaluates later. A list, so rapid changes queue rather than forcing an
// early free -- releasing under the GPU is the mistake this project has paid for repeatedly.
struct SplitRetired
{
    ID3D12Resource* resource = nullptr;
    std::unique_ptr<IFeature_Dx12> feature;
    std::unique_ptr<OS_Dx12> shader;
    int framesLeft = 16;
};

static std::vector<SplitRetired> SplitParkedList;

static void SplitParkResource(ID3D12Resource*& res)
{
    if (res == nullptr)
        return;

    SplitRetired r;
    r.resource = res;
    res = nullptr;
    SplitParkedList.push_back(std::move(r));
}
"""
t = t.replace(old, new, 1)

old = """// Frees what a live change parked, once enough evaluates have passed that nothing in flight can still
// reference it.
static void SplitTickParked()
{
    if (SplitDx12.parkedCountdown <= 0)
        return;

    if (--SplitDx12.parkedCountdown > 0)
        return;

    if (SplitDx12.parkedIntermediate != nullptr)
    {
        SplitDx12.parkedIntermediate->Release();
        SplitDx12.parkedIntermediate = nullptr;
    }

    if (SplitDx12.parkedOversized != nullptr)
    {
        SplitDx12.parkedOversized->Release();
        SplitDx12.parkedOversized = nullptr;
    }

    SplitDx12.parkedSr.reset();
    SplitDx12.parkedDownscaler.reset();
}

// Parks the enlargement stage for deferred release. If the parking lot is still occupied from a very
// recent change, the older tenants have had their frames and are let go now.
static void SplitParkEnlargement()
{
    if (SplitDx12.parkedOversized != nullptr)
        SplitDx12.parkedOversized->Release();

    SplitDx12.parkedSr.reset();
    SplitDx12.parkedDownscaler.reset();
    SplitDx12.parkedOversized = nullptr;

    if (SplitDx12.sr != nullptr)
        SplitDx12.parkedSr = std::move(SplitDx12.sr);

    if (SplitDx12.oversized != nullptr)
    {
        SplitDx12.parkedOversized = SplitDx12.oversized;
        SplitDx12.oversized = nullptr;
    }

    if (SplitDx12.downscaler != nullptr)
        SplitDx12.parkedDownscaler = std::move(SplitDx12.downscaler);

    SplitDx12.srTargetWidth = 0;
    SplitDx12.parkedCountdown = 16;
}"""
assert old in t, "park functions"
new = """// Frees what live changes parked, entry by entry, once enough evaluates have passed that nothing in
// flight can still reference each one.
static void SplitTickParked()
{
    for (size_t i = 0; i < SplitParkedList.size();)
    {
        if (--SplitParkedList[i].framesLeft > 0)
        {
            ++i;
            continue;
        }

        if (SplitParkedList[i].resource != nullptr)
            SplitParkedList[i].resource->Release();

        SplitParkedList.erase(SplitParkedList.begin() + i);
    }
}

// Parks the enlargement stage for deferred release.
static void SplitParkEnlargement()
{
    if (SplitDx12.sr != nullptr)
    {
        SplitRetired r;
        r.feature = std::move(SplitDx12.sr);
        SplitParkedList.push_back(std::move(r));
    }

    if (SplitDx12.downscaler != nullptr)
    {
        SplitRetired r;
        r.shader = std::move(SplitDx12.downscaler);
        SplitParkedList.push_back(std::move(r));
    }

    SplitParkResource(SplitDx12.oversized);
    SplitDx12.srTargetWidth = 0;
}"""
t = t.replace(old, new, 1)

old = """        if (SplitDx12.intermediate != nullptr)
        {
            SplitDx12.parkedIntermediate = SplitDx12.intermediate;
            SplitDx12.intermediate = nullptr;
        }

        SplitParkEnlargement();"""
assert old in t, "disarm parking"
t = t.replace(old, """        SplitParkResource(SplitDx12.intermediate);
        SplitParkEnlargement();""", 1)

# --- 3. Output Scaling absorbed rather than excluded ------------------------------------------------

old = """static bool SplitWanted()
{
    const Config& cfg = *Config::Instance();

    // Output Scaling's Enable owns the same feature geometry; the split reads its Ratio instead and
    // does the supersampling itself.
    if (cfg.OutputScalingEnabled.value_or_default())
        return false;

    return cfg.DlssNrEnabled.value_or_default() && cfg.DlssNrSplitPipeline.value_or_default() &&
           !SplitDx12.failed;
}

// The supersample ratio in force: the user's Output Scaling Ratio, when above one.
static float SplitRatio()
{
    float mult = Config::Instance()->OutputScalingMultiplier.value_or_default();

    if (mult > 3.0f)
        mult = 3.0f;

    return mult > 1.05f ? mult : 1.0f;
}"""
assert old in t, "wanted/ratio"
new = """static bool SplitWanted()
{
    const Config& cfg = *Config::Instance();
    return cfg.DlssNrEnabled.value_or_default() && cfg.DlssNrSplitPipeline.value_or_default() &&
           !SplitDx12.failed;
}

// Whether the user wants supersampling: Output Scaling's own Enable, as saved -- the runtime value is
// forced off while the split runs, because the split does the supersampling itself.
static bool SplitOsIntent()
{
    return Config::Instance()->OutputScalingEnabled.value_for_config().value_or(false);
}

// The split absorbs Output Scaling while it runs: the runtime flag goes off, so no feature geometry can
// be steered by two owners, and the split supersamples at the Ratio itself. Given back the moment the
// split stands down, so conventional Output Scaling resumes.
static void SplitAbsorbOs()
{
    if (SplitOsIntent() && Config::Instance()->OutputScalingEnabled.value_or_default())
    {
        Config::Instance()->OutputScalingEnabled.set_volatile_value(false);
        LOG_INFO("DLSS-NR split: absorbing Output Scaling -- the split supersamples in its place");
    }
}

static void SplitRestoreOs()
{
    if (SplitOsIntent() && !Config::Instance()->OutputScalingEnabled.value_or_default())
    {
        Config::Instance()->OutputScalingEnabled.set_volatile_value(true);
        LOG_INFO("DLSS-NR split: returning Output Scaling to its own machinery");
    }
}

// The supersample ratio in force: the Output Scaling Ratio, when the user has Output Scaling on.
static float SplitRatio()
{
    if (!SplitOsIntent())
        return 1.0f;

    float mult = Config::Instance()->OutputScalingMultiplier.value_or_default();

    if (mult > 3.0f)
        mult = 3.0f;

    return mult > 1.05f ? mult : 1.0f;
}"""
t = t.replace(old, new, 1)

# Absorb on the way in, restore on every way out.
old = """    const bool want = SplitWanted();
    State& state = State::Instance();

    // A recreation is mid-flight"""
assert old in t, "manage head"
new = """    const bool want = SplitWanted();
    State& state = State::Instance();

    if (want)
        SplitAbsorbOs();
    else
        SplitRestoreOs();

    // A recreation is mid-flight"""
t = t.replace(old, new, 1)

old = """    if (!want && Config::Instance()->DlssNrSplitPipeline.value_or_default() &&
        Config::Instance()->OutputScalingEnabled.value_or_default())
        DlssNr::SetSplitStatus("waiting: turn Output Scaling's Enable off -- the split reads its Ratio "
                               "and supersamples itself");

"""
assert old in t, "old OS warning"
t = t.replace(old, "", 1)

# --- 4. restores and the 4x cap ---------------------------------------------------------------------

old = """        const auto targetW = (unsigned int) (SplitDx12.displayWidth * mult + 0.5f);
        const auto targetH = (unsigned int) (SplitDx12.displayHeight * mult + 0.5f);

        if (!SplitEnsureOversized(targetW, targetH, workFormat))"""
assert old in t, "mode B target"
new = """        // DLSS refuses ratios beyond 4x its input, so the target is capped there.
        auto targetW = (unsigned int) (SplitDx12.displayWidth * mult + 0.5f);
        auto targetH = (unsigned int) (SplitDx12.displayHeight * mult + 0.5f);
        targetW = targetW > renderW * 4 ? renderW * 4 : targetW;
        targetH = targetH > renderH * 4 ? renderH * 4 : targetH;

        if (!SplitEnsureOversized(targetW, targetH, workFormat))"""
t = t.replace(old, new, 1)

old = """        std::snprintf(status, sizeof(status),
                      "running: RR supersampled x%.2f -> NR -> downscale (RR included)", mult);"""
assert old in t, "mode B end"
new = """        params->Set(NVSDK_NGX_Parameter_OutWidth, SplitDx12.displayWidth);
        params->Set(NVSDK_NGX_Parameter_OutHeight, SplitDx12.displayHeight);

        std::snprintf(status, sizeof(status),
                      "running: RR supersampled x%.2f -> NR -> downscale (RR included)", mult);"""
t = t.replace(old, new, 1)

old = """    const bool supersample = mult > 1.0f;
    const auto targetW =
        supersample ? (unsigned int) (SplitDx12.displayWidth * mult + 0.5f) : SplitDx12.displayWidth;
    const auto targetH =
        supersample ? (unsigned int) (SplitDx12.displayHeight * mult + 0.5f) : SplitDx12.displayHeight;"""
assert old in t, "mode A target"
new = """    const bool supersample = mult > 1.0f;
    auto targetW =
        supersample ? (unsigned int) (SplitDx12.displayWidth * mult + 0.5f) : SplitDx12.displayWidth;
    auto targetH =
        supersample ? (unsigned int) (SplitDx12.displayHeight * mult + 0.5f) : SplitDx12.displayHeight;

    // DLSS refuses ratios beyond 4x its input, so the target is capped there.
    targetW = targetW > renderW * 4 ? renderW * 4 : targetW;
    targetH = targetH > renderH * 4 ? renderH * 4 : targetH;"""
t = t.replace(old, new, 1)

old = """            params->Set(NVSDK_NGX_Parameter_Output, gameOutput);
            params->Set(NVSDK_NGX_Parameter_OutWidth, renderW);
            params->Set(NVSDK_NGX_Parameter_OutHeight, renderH);
            *outResult = rrResult;
            return true;
        }"""
assert old in t, "sr fail restore"
new = """            params->Set(NVSDK_NGX_Parameter_Output, gameOutput);
            params->Set(NVSDK_NGX_Parameter_OutWidth, SplitDx12.displayWidth);
            params->Set(NVSDK_NGX_Parameter_OutHeight, SplitDx12.displayHeight);
            *outResult = rrResult;
            return true;
        }"""
t = t.replace(old, new, 1)

old = """    if (gameColor != nullptr)
        params->Set(NVSDK_NGX_Parameter_Color, gameColor);

    params->Set(NVSDK_NGX_Parameter_Output, gameOutput);

    if (!srOk)"""
assert old in t, "mode A end restore"
new = """    // The block goes back exactly as the game filled it -- including the output size, which we borrowed
    // for the oversized target and whose pollution once compounded the supersample until the device hung.
    if (gameColor != nullptr)
        params->Set(NVSDK_NGX_Parameter_Color, gameColor);

    params->Set(NVSDK_NGX_Parameter_Output, gameOutput);
    params->Set(NVSDK_NGX_Parameter_OutWidth, SplitDx12.displayWidth);
    params->Set(NVSDK_NGX_Parameter_OutHeight, SplitDx12.displayHeight);

    if (!srOk)"""
t = t.replace(old, new, 1)

io.open(p, "w", encoding="utf-8").write(t)
print("seam patched")

# --- menu: the OS relationship as the user defined it -------------------------------------------------

NL = chr(92) + "n"
p = ROOT + "menu/menu_common.cpp"
t = io.open(p, encoding="utf-8").read()

old_start = t.index("""            // Supersampling is automatic, from the Output Scaling Ratio; the text says what is in force.""")
old_end = t.index("""            bool includeRR = config->DlssNrSplitIncludeRR.value_or_default();""")
new_block = """            // Output Scaling's Enable is the supersampling intent; the split absorbs it while running.
            const float ratio = config->OutputScalingMultiplier.value_or_default();
            const bool osIntent = config->OutputScalingEnabled.value_for_config().value_or(false);

            if (osIntent && ratio > 1.05f)
                ImGui::TextDisabled("Output Scaling absorbed: the split supersamples x%.2f itself.", ratio);
            else if (osIntent)
                ImGui::TextDisabled("Output Scaling absorbed, but its Ratio is 1.0 -- nothing to supersample.");
            else
                ImGui::TextDisabled("No supersampling: enable Output Scaling (and its Ratio) to add it.");

"""
t = t[:old_start] + new_block + t[old_end:]
io.open(p, "w", encoding="utf-8").write(t)
print("menu updated")
