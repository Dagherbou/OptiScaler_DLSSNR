import io

# ============ 1. The crash: never evaluate the model in the submission that created it ============
p = 'OptiScaler/dlssnr/DlssNr_Dx12.cpp'
t = io.open(p, encoding='utf-8').read()

# Before-FG / split path: the creation frame returns after recording the creation; the game's own
# submit executes it, and the first evaluate happens on the next call.
old = """        g_nr.width = width;
        g_nr.height = height;
        g_nr.reset = true;
        RecordBuiltTuning(cfg);
        LOG_INFO("DLSS-NR running at {}x{}, guides {}x{} (preset {}, intensity {}, style {})", width,
                 height, guideWidth, guideHeight, g_nr.builtPreset, g_nr.builtIntensity, g_nr.builtStyle);
    }"""
assert old in t
new = """        g_nr.width = width;
        g_nr.height = height;
        g_nr.reset = true;
        RecordBuiltTuning(cfg);
        LOG_INFO("DLSS-NR running at {}x{}, guides {}x{} (preset {}, intensity {}, style {})", width,
                 height, guideWidth, guideHeight, g_nr.builtPreset, g_nr.builtIntensity, g_nr.builtStyle);

        // Creating and evaluating a feature in the same command list is the dice-roll that hung the
        // GPU (every crash died on a creation frame). The creation goes through the game's own submit
        // first; the first evaluate happens next frame. One frame without the model is invisible.
        device->Release();
        return;
    }"""
t = t.replace(old, new, 1)

# Present path: same rule, but this path owns its command list -- the creation commands still have to
# be submitted and fenced before returning, or the ring allocator's bookkeeping breaks.
old = """                 cfg.DlssNrUiCorrection.value_or_default() ? 1 : 0);
    }

    // Timed across the whole pass: the staging copies and the resolve are part of what this costs, and
    // timing only the model would flatter the number."""
assert old in t
new = """                 cfg.DlssNrUiCorrection.value_or_default() ? 1 : 0);

        // Same dice-roll as the render-path creation: the creation commands are submitted and fenced
        // on their own, and the first evaluate happens on the next pass.
        if (SUCCEEDED(cmdList->Close()))
        {
            ID3D12CommandList* lists[] = { cmdList };
            queue->ExecuteCommandLists(1, lists);
            ++g_nr.presentFenceNext;

            if (SUCCEEDED(queue->Signal(g_nr.presentFence, g_nr.presentFenceNext)))
                g_nr.presentFenceValues[slot] = g_nr.presentFenceNext;
        }

        device->Release();
        return;
    }

    // Timed across the whole pass: the staging copies and the resolve are part of what this costs, and
    // timing only the model would flatter the number."""
t = t.replace(old, new, 1)

io.open(p, 'w', encoding='utf-8').write(t)
print('NR creation warmup in')

# ============ 2. Config: internal enlargement preset + sharpening ============
p = 'OptiScaler/Config.h'
t = io.open(p, encoding='utf-8').read()

old = "    CustomOptional<bool> DlssNrSplitIncludeRR { false };"
assert old in t
t = t.replace(old, old + """

    // The internal enlargement's model preset (0 = driver default; otherwise the NGX render-preset
    // number: 5 = E, 6 = F, 10 = J, 11 = K) and its sharpening (0 = off; needs RCAS enabled).
    CustomOptional<uint32_t> DlssNrSplitSrPreset { 0 };
    CustomOptional<float> DlssNrSplitSrSharpness { 0.0f };""", 1)

io.open(p, 'w', encoding='utf-8').write(t)

p = 'OptiScaler/Config.cpp'
t = io.open(p, encoding='utf-8').read()

old = '            DlssNrSplitIncludeRR.set_from_config(readBool("DlssNr", "SplitIncludeRR"));'
assert old in t
t = t.replace(old, old + """
            DlssNrSplitSrPreset.set_from_config(readUInt("DlssNr", "SplitSrPreset"));
            DlssNrSplitSrSharpness.set_from_config(readFloat("DlssNr", "SplitSrSharpness"));""", 1)

old = '    ini.SetValue("DlssNr", "SplitIncludeRR", GetBoolValue(Instance()->DlssNrSplitIncludeRR.value_for_config()).c_str());'
assert old in t
t = t.replace(old, old + """
    ini.SetValue("DlssNr", "SplitSrPreset", GetIntValue(Instance()->DlssNrSplitSrPreset.value_for_config()).c_str());
    ini.SetValue("DlssNr", "SplitSrSharpness",
                 GetFloatValue(Instance()->DlssNrSplitSrSharpness.value_for_config()).c_str());""", 1)

io.open(p, 'w', encoding='utf-8').write(t)
print('config entries in')

# ============ 3. Seam: build the enlargement with the chosen preset, rebuild when it changes ============
p = 'OptiScaler/inputs/NVNGX_DLSS_Dx12.cpp'
t = io.open(p, encoding='utf-8').read()

old = "    unsigned int srTargetWidth = 0;         // what the enlargement was built to produce"
assert old in t
t = t.replace(old, old + """
    int srBuiltPreset = -1;                 // the render preset it was built with""", 1)

old = """    if (SplitDx12.sr != nullptr && SplitDx12.srTargetWidth != targetW)
    {
        LOG_INFO("DLSS-NR split: rebuilding the enlargement for {}x{}", targetW, targetH);
        SplitParkEnlargement();
    }"""
assert old in t
t = t.replace(old, """    const int srPresetWanted = (int) Config::Instance()->DlssNrSplitSrPreset.value_or_default();

    if (SplitDx12.sr != nullptr &&
        (SplitDx12.srTargetWidth != targetW || SplitDx12.srBuiltPreset != srPresetWanted))
    {
        LOG_INFO("DLSS-NR split: rebuilding the enlargement for {}x{} (preset {})", targetW, targetH,
                 srPresetWanted);
        SplitParkEnlargement();
    }""", 1)

# Preset hints are create-time reads of the game's block, like the quality mode: write before the
# creation parses, restore right after, so the game's own values survive.
old = """        // The enlargement parses the game's block, whose quality mode describes the game's own ratio.
        // Built supersampled, it must declare what it actually is or the driver refuses it at evaluate.
        params->Set(NVSDK_NGX_Parameter_PerfQualityValue, SplitPerfQuality(renderW, targetW));

        auto sr = std::make_unique<DLSSFeatureDx12>(IFeature::GetNextHandleId(), params);
        const bool srInited = sr->Init(D3D12Device, cmdList, params) && sr->IsInited();

        if (SplitDx12.origPerfQuality != 0xffffffff)
            params->Set(NVSDK_NGX_Parameter_PerfQualityValue, SplitDx12.origPerfQuality);"""
assert old in t
new = """        // The enlargement parses the game's block, whose quality mode describes the game's own ratio.
        // Built supersampled, it must declare what it actually is or the driver refuses it at evaluate.
        params->Set(NVSDK_NGX_Parameter_PerfQualityValue, SplitPerfQuality(renderW, targetW));

        // The chosen render preset for the enlargement, written across every mode slot so the one the
        // declared quality maps to is covered; the game's own hints are restored right after creation.
        // (OptiScaler's global Render Presets Override, when on, wins inside the feature's init.)
        unsigned int origHintQ = 0, origHintB = 0, origHintP = 0, origHintUP = 0, origHintDLAA = 0;

        if (srPresetWanted != 0)
        {
            params->Get(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, &origHintQ);
            params->Get(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, &origHintB);
            params->Get(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, &origHintP);
            params->Get(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, &origHintUP);
            params->Get(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, &origHintDLAA);
            params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, (unsigned int) srPresetWanted);
            params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, (unsigned int) srPresetWanted);
            params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, (unsigned int) srPresetWanted);
            params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
                        (unsigned int) srPresetWanted);
            params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, (unsigned int) srPresetWanted);
        }

        auto sr = std::make_unique<DLSSFeatureDx12>(IFeature::GetNextHandleId(), params);
        const bool srInited = sr->Init(D3D12Device, cmdList, params) && sr->IsInited();

        if (srPresetWanted != 0)
        {
            params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, origHintQ);
            params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, origHintB);
            params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, origHintP);
            params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, origHintUP);
            params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, origHintDLAA);
        }

        if (SplitDx12.origPerfQuality != 0xffffffff)
            params->Set(NVSDK_NGX_Parameter_PerfQualityValue, SplitDx12.origPerfQuality);"""
t = t.replace(old, new, 1)

old = """        SplitDx12.sr = std::move(sr);
        SplitDx12.srTargetWidth = targetW;
        SplitDx12.srJustCreated = true;"""
assert old in t
t = t.replace(old, """        SplitDx12.sr = std::move(sr);
        SplitDx12.srTargetWidth = targetW;
        SplitDx12.srBuiltPreset = srPresetWanted;
        SplitDx12.srJustCreated = true;""", 1)

# Sharpening: the user's chosen amount instead of a hard zero. Zero stays the default, so the
# no-RCAS-on-our-motion-vectors reasoning still holds unless the user asks for it.
old = """    // And the game's sharpness would switch on RCAS inside our SR, against motion vectors it does not
    // understand at this geometry. The enlargement is an enlargement, nothing more.
    float gameSharpness = 0.0f;
    params->Get(NVSDK_NGX_Parameter_Sharpness, &gameSharpness);
    params->Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);"""
assert old in t
t = t.replace(old, """    // The game's sharpness must not leak into our SR -- it belongs to the game's own arrangement. The
    // enlargement sharpens only by the user's explicit amount (0 = off, the default; runs via RCAS).
    float gameSharpness = 0.0f;
    params->Get(NVSDK_NGX_Parameter_Sharpness, &gameSharpness);
    params->Set(NVSDK_NGX_Parameter_Sharpness,
                Config::Instance()->DlssNrSplitSrSharpness.value_or_default());""", 1)

io.open(p, 'w', encoding='utf-8').write(t)
print('seam preset/sharpness in')

# ============ 4. Menu ============
p = 'OptiScaler/menu/menu_common.cpp'
t = io.open(p, encoding='utf-8').read()

old = """                           "\\nconventional Output Scaling look with the model in the chain -- and RR's"
                           "\\ncost rises with the square of the ratio, which is the price."
                           "\\n\\nApplies live; the feature is re-created in place.");
        }"""
assert old in t
new = """                           "\\nconventional Output Scaling look with the model in the chain -- and RR's"
                           "\\ncost rises with the square of the ratio, which is the price."
                           "\\n\\nApplies live; the feature is re-created in place.");

            {
                static const int srPresetValues[] = { 0, 5, 6, 10, 11 };
                static const char* srPresetNames =
                    "Driver default\\0Preset E (classic, stable)\\0Preset F (classic)\\0"
                    "Preset J (transformer)\\0Preset K (transformer, latest)\\0";
                const int srPresetNow = (int) config->DlssNrSplitSrPreset.value_or_default();
                int srPresetIndex = 0;

                for (int i = 0; i < 5; ++i)
                {
                    if (srPresetValues[i] == srPresetNow)
                        srPresetIndex = i;
                }

                if (ImGui::Combo("Enlargement preset", &srPresetIndex, srPresetNames))
                    config->DlssNrSplitSrPreset = (uint32_t) srPresetValues[srPresetIndex];

                ShowHelpMarker("The model preset of the split's internal Super Resolution feature."
                               "\\nThe transformer presets (J, K) hallucinate detail best from an"
                               "\\nalready-resolved image, which is exactly what the enlargement is fed."
                               "\\n\\nApplies live; the enlargement is re-created in place. If the global"
                               "\\nRender Presets Override is on, it wins over this.");

                float srSharp = config->DlssNrSplitSrSharpness.value_or_default();

                if (ImGui::SliderFloat("Enlargement sharpening", &srSharp, 0.0f, 1.0f, "%.2f"))
                    config->DlssNrSplitSrSharpness = srSharp;

                ShowHelpMarker("Sharpening applied by the enlargement (via RCAS -- needs RCAS enabled"
                               "\\nabove). 0 is off, and the default: the supersample downscale already"
                               "\\nrestores some crispness on its own.");
            }
        }"""
t = t.replace(old, new, 1)

io.open(p, 'w', encoding='utf-8').write(t)
print('menu controls in')
