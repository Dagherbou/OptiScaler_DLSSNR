import io

# ============ 1. Coring: a noise floor on the edit, in the resolve shader ============
p = 'OptiScaler/dlssnr/DlssNr_Codec.h'
t = io.open(p, encoding='utf-8').read()

old = """    float gStability;    // how much of the history survives each frame; 0 is off
    uint  gPad;
};"""
assert old in t
t = t.replace(old, """    float gStability;    // how much of the history survives each frame; 0 is off
    float gNoiseFloor;   // edits below this are squashed toward zero; 0 is off
};""", 1)

old = """    float3 edit = model - proxy;
"""
assert old in t
t = t.replace(old, """    float3 edit = model - proxy;

    // Coring. The churn the model re-decides every frame is small-amplitude and unstructured, while
    // the detail worth keeping -- occlusion, contact shadows, synthesised texture -- is larger and
    // structured. Edits below the floor are squashed toward zero, edits above it pass untouched, and
    // the ramp between the two keeps the transition invisible. At 0 this does nothing at all.
    if (gNoiseFloor > 0.0)
    {
        float editSize = max(abs(edit.r), max(abs(edit.g), abs(edit.b)));
        edit *= smoothstep(gNoiseFloor * 0.5, gNoiseFloor * 1.5, editSize);
    }
""", 1)

old = """    float stability;
    unsigned int pad;
};"""
assert old in t
t = t.replace(old, """    float stability;
    float noiseFloor;
};""", 1)

io.open(p, 'w', encoding='utf-8').write(t)
print('codec coring in')

# ============ 2. Both resolve sites feed it ============
p = 'OptiScaler/dlssnr/DlssNr_Dx12.cpp'
t = io.open(p, encoding='utf-8').read()

old = "        resolveParams.maxRatio = cfg.DlssNrMaxRatio.value_or_default();"
assert t.count(old) == 2
t = t.replace(old, old + """
        resolveParams.noiseFloor = cfg.DlssNrNoiseFloor.value_or_default();""")

io.open(p, 'w', encoding='utf-8').write(t)
print('resolve sites in')

# ============ 3. Config ============
p = 'OptiScaler/Config.h'
t = io.open(p, encoding='utf-8').read()

old = "    CustomOptional<float> DlssNrSplitSrSharpness { 0.0f };"
assert old in t
t = t.replace(old, old + """

    // Coring: edits smaller than this are squashed toward zero in the resolve -- the measured
    // per-frame churn is small and unstructured, real detail is larger. 0 is off and bit-identical.
    CustomOptional<float> DlssNrNoiseFloor { 0.0f };

    // Include-RR's own supersample ratio. 0 follows Output Scaling's Ratio; anything above 1.05 runs
    // RR at this ratio instead -- most of the reconstruction sharpness for far less of RR's cost.
    CustomOptional<float> DlssNrSplitIncludeRRRatio { 0.0f };""", 1)

io.open(p, 'w', encoding='utf-8').write(t)

p = 'OptiScaler/Config.cpp'
t = io.open(p, encoding='utf-8').read()

old = '            DlssNrSplitSrSharpness.set_from_config(readFloat("DlssNr", "SplitSrSharpness"));'
assert old in t
t = t.replace(old, old + """
            DlssNrNoiseFloor.set_from_config(readFloat("DlssNr", "NoiseFloor"));
            DlssNrSplitIncludeRRRatio.set_from_config(readFloat("DlssNr", "SplitIncludeRRRatio"));""", 1)

old = """    ini.SetValue("DlssNr", "SplitSrSharpness",
                 GetFloatValue(Instance()->DlssNrSplitSrSharpness.value_for_config()).c_str());"""
assert old in t
t = t.replace(old, old + """
    ini.SetValue("DlssNr", "NoiseFloor", GetFloatValue(Instance()->DlssNrNoiseFloor.value_for_config()).c_str());
    ini.SetValue("DlssNr", "SplitIncludeRRRatio",
                 GetFloatValue(Instance()->DlssNrSplitIncludeRRRatio.value_for_config()).c_str());""", 1)

io.open(p, 'w', encoding='utf-8').write(t)
print('config in')

# ============ 4. Seam: Include-RR runs at its own ratio ============
p = 'OptiScaler/inputs/NVNGX_DLSS_Dx12.cpp'
t = io.open(p, encoding='utf-8').read()

old = """    if (Config::Instance()->DlssNrSplitIncludeRR.value_or_default() && mult > 1.0f &&
        SplitDx12.displayWidth != 0)
    {
        *outW = (unsigned int) (SplitDx12.displayWidth * mult + 0.5f);
        *outH = (unsigned int) (SplitDx12.displayHeight * mult + 0.5f);
        return;
    }"""
assert old in t
t = t.replace(old, """    if (Config::Instance()->DlssNrSplitIncludeRR.value_or_default() && mult > 1.0f &&
        SplitDx12.displayWidth != 0)
    {
        // Include-RR can run at its own ratio: most of the reconstruction sharpness arrives well
        // below the full Output Scaling ratio, and RR's cost rises with the ratio squared.
        float rrMult = Config::Instance()->DlssNrSplitIncludeRRRatio.value_or_default();
        rrMult = rrMult > 1.05f ? (rrMult > 3.0f ? 3.0f : rrMult) : mult;

        *outW = (unsigned int) (SplitDx12.displayWidth * rrMult + 0.5f);
        *outH = (unsigned int) (SplitDx12.displayHeight * rrMult + 0.5f);
        return;
    }""", 1)

old = """        // RR itself upscales to the supersampled size; the model works on that image; only the
        // downscale remains. The conventional Output Scaling look with the model in the chain.
        // DLSS refuses ratios beyond 4x its input, so the target is capped there.
        auto targetW = (unsigned int) (SplitDx12.displayWidth * mult + 0.5f);
        auto targetH = (unsigned int) (SplitDx12.displayHeight * mult + 0.5f);
        targetW = targetW > renderW * 4 ? renderW * 4 : targetW;
        targetH = targetH > renderH * 4 ? renderH * 4 : targetH;"""
assert old in t
t = t.replace(old, """        // RR itself upscales to the supersampled size; the model works on that image; only the
        // downscale remains. The conventional Output Scaling look with the model in the chain. The
        // target comes from the same computation the manager armed with, so the two stay in lockstep.
        unsigned int targetW = 0, targetH = 0;
        SplitDesiredTarget(renderW, renderH, &targetW, &targetH);""", 1)

old = """        std::snprintf(status, sizeof(status),
                      "running: RR supersampled x%.2f -> NR -> downscale (RR included)", mult);"""
assert old in t
t = t.replace(old, """        std::snprintf(status, sizeof(status),
                      "running: RR supersampled x%.2f -> NR -> downscale (RR included)",
                      (float) targetW / (float) SplitDx12.displayWidth);""", 1)

io.open(p, 'w', encoding='utf-8').write(t)
print('rr ratio in')

# ============ 5. Menu: coring slider, RR-ratio slider, clearer enlargement tooltip ============
p = 'OptiScaler/menu/menu_common.cpp'
t = io.open(p, encoding='utf-8').read()

old = """        float editStability = config->DlssNrEditStability.value_or_default();"""
assert old in t
t = t.replace(old, """        float noiseFloor = config->DlssNrNoiseFloor.value_or_default();
        if (ImGui::SliderFloat("Noise floor (coring)", &noiseFloor, 0.0f, 0.05f, "%.3f"))
            config->DlssNrNoiseFloor = noiseFloor;

        ShowHelpMarker("Squashes edits smaller than this toward zero before they land. The wobble is"
                       "\\nthe model re-deciding a small, unstructured fraction of its edit every frame;"
                       "\\nreal detail -- occlusion, contact shadows, synthesised texture -- is larger"
                       "\\nand passes untouched. The cheapest stabiliser: no history, no ghosting."
                       "\\n\\n0 is off and bit-identical. Raise it until the shimmer stops -- around"
                       "\\n0.01 to 0.02 -- and stop there: higher starts to eat the faintest real"
                       "\\ndetail, gentle ambient occlusion first.");

        float editStability = config->DlssNrEditStability.value_or_default();""", 1)

old = """                           "\\n\\nApplies live; the feature is re-created in place.");
"""
assert old in t
t = t.replace(old, """                           "\\n\\nApplies live; the feature is re-created in place.");

            if (includeRR)
            {
                const float rrRatioSet = config->DlssNrSplitIncludeRRRatio.value_or_default();
                const bool followsOs = rrRatioSet <= 1.05f;
                float rrRatioShown = followsOs ? ratio : rrRatioSet;

                if (ImGui::SliderFloat("Include-RR ratio", &rrRatioShown, 1.0f, 3.0f,
                                       followsOs ? "%.2f (following OS Ratio)" : "%.2f"))
                    config->DlssNrSplitIncludeRRRatio = rrRatioShown;

                ShowHelpMarker("The ratio RR itself upscales to when included in the supersample. By"
                               "\\ndefault it follows Output Scaling's Ratio -- but RR's cost rises with"
                               "\\nthe square of this, and most of the reconstruction sharpness arrives"
                               "\\nwell below the full ratio. 1.25 buys most of the sharpness for about"
                               "\\n1.5x RR's normal cost instead of 4x at ratio 2."
                               "\\n\\nApplies live; the feature is re-created in place.");

                if (!followsOs)
                {
                    if (ImGui::SmallButton("Follow OS Ratio"))
                        config->DlssNrSplitIncludeRRRatio = 0.0f;
                }
            }
""", 1)

old = """                ShowHelpMarker("The model preset of the split's internal Super Resolution feature."
                               "\\nThe transformer presets (J, K) hallucinate detail best from an"
                               "\\nalready-resolved image, which is exactly what the enlargement is fed."
                               "\\n\\nApplies live; the enlargement is re-created in place. If the global"
                               "\\nRender Presets Override is on, it wins over this.");"""
assert old in t
t = t.replace(old, """                ShowHelpMarker("The split's chain: Ray Reconstruction denoises 1:1 at render size, the"
                               "\\nmodel draws its detail there, and an internal Super Resolution feature"
                               "\\nenlarges that finished image to the output. This preset picks the SR"
                               "\\nmodel for that last step -- everything the model drew passes through"
                               "\\nit, so it decides how sharp that detail lands."
                               "\\n\\nSR presets are their own scale: E here is unrelated to RR's presets"
                               "\\nor the model's. E is the proven classic; J and K (transformer) are"
                               "\\nstrongest at extracting detail from an already-resolved image."
                               "\\n\\nApplies live; the enlargement is re-created in place. If the global"
                               "\\nRender Presets Override is on, it wins over this.");""", 1)

io.open(p, 'w', encoding='utf-8').write(t)
print('menu in')
