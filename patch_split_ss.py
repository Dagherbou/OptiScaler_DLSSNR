"""Supersamples the split pipeline's enlargement, for the Output Scaling look.

Conventional Output Scaling cannot run with the split -- it owns the same geometry -- but the split's
enlargement is our own feature, so the same idea is built in directly: the internal Super Resolution
feature renders above display resolution and OptiScaler's own downscaler carries it to the display size,
using the Output Scaling Ratio the user already tuned. Quality-scale rendering with DLAA-like sharpness,
with Ray Reconstruction still 1:1 in front.

A bonus falls out for free: the model's edit is carried up with the frame and averaged back down, which
attenuates its per-frame noise spatially -- the graininess complained of at reduced model resolutions --
with no temporal cost at all.
"""

import io

ROOT = "C:/Games_Temp/OptiScaler/OptiScaler/"

# --- config -----------------------------------------------------------------------------------------

p = ROOT + "Config.h"
t = io.open(p, encoding="utf-8").read()
old = "    CustomOptional<bool> DlssNrSplitPipeline { false };"
assert old in t
t = t.replace(old, old + """

    // The split's enlargement renders above display resolution and is downscaled back, using the
    // Output Scaling Ratio. The Output Scaling look, composed with the split.
    CustomOptional<bool> DlssNrSplitSupersample { false };""", 1)
io.open(p, "w", encoding="utf-8").write(t)

p = ROOT + "Config.cpp"
t = io.open(p, encoding="utf-8").read()
old = '            DlssNrSplitPipeline.set_from_config(readBool("DlssNr", "SplitPipeline"));'
assert old in t
t = t.replace(old, old + '\n            DlssNrSplitSupersample.set_from_config(readBool("DlssNr", "SplitSupersample"));', 1)
old = '    ini.SetValue("DlssNr", "SplitPipeline", GetBoolValue(Instance()->DlssNrSplitPipeline.value_for_config()).c_str());'
assert old in t
t = t.replace(old, old + '\n    ini.SetValue("DlssNr", "SplitSupersample", GetBoolValue(Instance()->DlssNrSplitSupersample.value_for_config()).c_str());', 1)
io.open(p, "w", encoding="utf-8").write(t)
print("config added")

# --- seam -------------------------------------------------------------------------------------------

p = ROOT + "inputs/NVNGX_DLSS_Dx12.cpp"
t = io.open(p, encoding="utf-8").read()

t = t.replace('#include <dlssnr/DlssNr_Codec.h>',
              '#include <dlssnr/DlssNr_Codec.h>\n#include <shaders/output_scaling/OS_Dx12.h>', 1)

old = """    // Retired on a live switch-off: still referenced by command lists submitted over the last frames,
    // so they are released a number of evaluates later rather than on the spot.
    ID3D12Resource* parkedIntermediate = nullptr;
    std::unique_ptr<IFeature_Dx12> parkedSr;
    int parkedCountdown = 0;
};"""
assert old in t
new = """    // The supersampled enlargement: the internal SR renders here, above display size, and OptiScaler's
    // own downscaler carries it to the game's output.
    ID3D12Resource* oversized = nullptr;
    std::unique_ptr<OS_Dx12> downscaler;
    bool srSupersampled = false;

    // Retired on a live switch-off: still referenced by command lists submitted over the last frames,
    // so they are released a number of evaluates later rather than on the spot.
    ID3D12Resource* parkedIntermediate = nullptr;
    ID3D12Resource* parkedOversized = nullptr;
    std::unique_ptr<IFeature_Dx12> parkedSr;
    std::unique_ptr<OS_Dx12> parkedDownscaler;
    int parkedCountdown = 0;
};"""
t = t.replace(old, new, 1)

old = """    if (SplitDx12.parkedIntermediate != nullptr)
    {
        SplitDx12.parkedIntermediate->Release();
        SplitDx12.parkedIntermediate = nullptr;
    }

    SplitDx12.parkedSr.reset();
}"""
assert old in t
new = """    if (SplitDx12.parkedIntermediate != nullptr)
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

// Parks the enlargement stage for deferred release, keeping whatever was already parked ticking.
static void SplitParkEnlargement()
{
    if (SplitDx12.sr != nullptr)
        SplitDx12.parkedSr = std::move(SplitDx12.sr);

    if (SplitDx12.oversized != nullptr)
    {
        SplitDx12.parkedOversized = SplitDx12.oversized;
        SplitDx12.oversized = nullptr;
    }

    if (SplitDx12.downscaler != nullptr)
        SplitDx12.parkedDownscaler = std::move(SplitDx12.downscaler);

    SplitDx12.parkedCountdown = 16;
}"""
t = t.replace(old, new, 1)

# Switch-off parks the enlargement pieces too.
old = """        if (SplitDx12.intermediate != nullptr || SplitDx12.sr != nullptr)
        {
            SplitDx12.parkedIntermediate = SplitDx12.intermediate;
            SplitDx12.intermediate = nullptr;
            SplitDx12.parkedSr = std::move(SplitDx12.sr);
            SplitDx12.parkedCountdown = 16;
        }"""
assert old in t
new = """        if (SplitDx12.intermediate != nullptr)
        {
            SplitDx12.parkedIntermediate = SplitDx12.intermediate;
            SplitDx12.intermediate = nullptr;
        }

        SplitParkEnlargement();"""
t = t.replace(old, new, 1)

# The enlargement itself: supersampled when asked, and rebuilt live when the choice changes.
old = """    // 3. The one enlargement: an internal Super Resolution feature, created with the game's own flags
    //    -- IsHDR included, since the stream never left linear HDR.
    if (SplitDx12.sr == nullptr)
    {
        params->Set(NVSDK_NGX_Parameter_OutWidth, SplitDx12.displayWidth);
        params->Set(NVSDK_NGX_Parameter_OutHeight, SplitDx12.displayHeight);"""
assert old in t
new = """    // 3. The one enlargement: an internal Super Resolution feature, created with the game's own flags
    //    -- IsHDR included, since the stream never left linear HDR. Supersampled when asked: it renders
    //    above display size and OptiScaler's own downscaler carries it to the game's output, which is
    //    the Output Scaling look with Ray Reconstruction still 1:1 in front.
    const Config& splitCfg = *Config::Instance();
    const bool supersample = splitCfg.DlssNrSplitSupersample.value_or_default();

    float ssMult = splitCfg.OutputScalingMultiplier.value_or_default();
    ssMult = ssMult < 1.1f ? 1.5f : (ssMult > 3.0f ? 3.0f : ssMult);

    const unsigned int targetW =
        supersample ? (unsigned int) (SplitDx12.displayWidth * ssMult + 0.5f) : SplitDx12.displayWidth;
    const unsigned int targetH =
        supersample ? (unsigned int) (SplitDx12.displayHeight * ssMult + 0.5f) : SplitDx12.displayHeight;

    // The choice is baked into the feature; changing it swaps the enlargement in place.
    if (SplitDx12.sr != nullptr && SplitDx12.srSupersampled != supersample)
    {
        LOG_INFO("DLSS-NR split: rebuilding the enlargement ({}supersampled)", supersample ? "" : "not ");
        SplitParkEnlargement();
    }

    if (SplitDx12.sr == nullptr)
    {
        params->Set(NVSDK_NGX_Parameter_OutWidth, targetW);
        params->Set(NVSDK_NGX_Parameter_OutHeight, targetH);"""
t = t.replace(old, new, 1)

old = """        SplitDx12.sr = std::move(sr);
        LOG_INFO("DLSS-NR split: internal Super Resolution running {}x{} -> {}x{}", w, h,
                 SplitDx12.displayWidth, SplitDx12.displayHeight);
    }

    params->Set(NVSDK_NGX_Parameter_Color, SplitDx12.intermediate);
    params->Set(NVSDK_NGX_Parameter_Output, gameOutput);
    params->Set(NVSDK_NGX_Parameter_OutWidth, SplitDx12.displayWidth);
    params->Set(NVSDK_NGX_Parameter_OutHeight, SplitDx12.displayHeight);

    const bool srOk = SplitDx12.sr->Evaluate(cmdList, params);"""
assert old in t
new = """        SplitDx12.sr = std::move(sr);
        SplitDx12.srSupersampled = supersample;
        LOG_INFO("DLSS-NR split: internal Super Resolution running {}x{} -> {}x{}{}", w, h, targetW,
                 targetH, supersample ? " (supersampled)" : "");
    }

    if (supersample && SplitDx12.oversized == nullptr)
    {
        SplitDx12.oversized =
            SplitScratch(D3D12Device, codec::TypedFormat(gameOutput->GetDesc().Format), targetW, targetH);

        if (SplitDx12.oversized == nullptr)
        {
            LOG_ERROR("DLSS-NR split: the supersample target could not be created; falling back to the "
                      "plain enlargement");
            Config::Instance()->DlssNrSplitSupersample.set_volatile_value(false);
        }
    }

    const bool useOversized = supersample && SplitDx12.oversized != nullptr;

    params->Set(NVSDK_NGX_Parameter_Color, SplitDx12.intermediate);
    params->Set(NVSDK_NGX_Parameter_Output, useOversized ? SplitDx12.oversized : gameOutput);
    params->Set(NVSDK_NGX_Parameter_OutWidth, targetW);
    params->Set(NVSDK_NGX_Parameter_OutHeight, targetH);

    bool srOk = SplitDx12.sr->Evaluate(cmdList, params);

    if (srOk && useOversized)
    {
        // OptiScaler's own downscaler, so the filtering matches the Output Scaling look it imitates.
        if (SplitDx12.downscaler == nullptr)
            SplitDx12.downscaler = std::make_unique<OS_Dx12>("DLSS-NR Split Downscale", D3D12Device, false);

        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = SplitDx12.oversized;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        cmdList->ResourceBarrier(1, &b);

        srOk = SplitDx12.downscaler->Dispatch(cmdList, SplitDx12.oversized, gameOutput);

        b.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        cmdList->ResourceBarrier(1, &b);
    }"""
t = t.replace(old, new, 1)

# The status names the arrangement.
old = """    if (srOk)
        DlssNr::SetSplitStatus("running: RR 1:1 -> NR -> internal SR");"""
assert old in t
new = """    if (srOk)
        DlssNr::SetSplitStatus(useOversized ? "running: RR 1:1 -> NR -> SR supersampled -> downscale"
                                            : "running: RR 1:1 -> NR -> internal SR");"""
t = t.replace(old, new, 1)

io.open(p, "w", encoding="utf-8").write(t)
print("seam patched")

# --- menu -------------------------------------------------------------------------------------------

NL = chr(92) + "n"
p = ROOT + "menu/menu_common.cpp"
t = io.open(p, encoding="utf-8").read()
old = '''        ImGui::SeparatorText("Cost");'''
assert old in t
new = ('''        if (split)
        {
            bool splitSs = config->DlssNrSplitSupersample.value_or_default();
            if (ImGui::Checkbox("Supersample the enlargement (Output Scaling look)", &splitSs))
                config->DlssNrSplitSupersample = splitSs;

            ShowHelpMarker("The internal Super Resolution renders above display size -- by the Output"
                           "@Scaling Ratio -- and OptiScaler's own downscaler carries it back. Quality"
                           "@rendering with DLAA-like sharpness, and Ray Reconstruction still 1:1 in"
                           "@front."
                           "@@A bonus falls out for free: the model's edit is averaged in the downscale,"
                           "@which attenuates its per-frame grain spatially with no ghosting."
                           "@@Costs the enlargement times the ratio squared. Applies live -- the"
                           "@enlargement is swapped in place.");
        }

        ImGui::SeparatorText("Cost");''').replace("@", NL)
t = t.replace(old, new, 1)
io.open(p, "w", encoding="utf-8").write(t)
print("menu added")
