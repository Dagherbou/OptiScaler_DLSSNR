"""Replaces the DLSS-NR menu panel with one built around what the model's parameters actually mean.

The first version guessed at ranges. Skin structure is not a 0..1 strength -- -1 means follow local
structure, and that is its default. The transfer and colour strengths are new and go past 1.0 on
purpose: if a slider appears to do nothing, exaggerating it is how you find out whether it does.
"""

import io

NL = "\\n"

SECTION = '''void MenuCommon::RenderDlssNrSettings(RenderMenuContext& ctx)
{
    auto config = ctx.config;

    // DLSS Neural Rendering -----------------------------
    ImGui::Spacing();
    if (auto ch = ScopedCollapsingHeader("DLSS Neural Rendering"); ch.IsHeaderOpen())
    {
        ScopedIndent indent {};
        ImGui::Spacing();

        bool enabled = config->DlssNrEnabled.value_or_default();
        if (ImGui::Checkbox("Enable Neural Rendering", &enabled))
            config->DlssNrEnabled = enabled;

        ShowHelpMarker("Synthesises detail in the upscaler's output, before frame generation sees it."
                       "@@Requires nvngx_dlssnr.dll and nvngx.dll_dlssnr.dll beside OptiScaler."
                       "@Undocumented and driven directly, so none of this is officially supported.");

        if (!DlssNr::IsRunning())
        {
            const char* reason = DlssNr::FailureReason();

            if (reason[0] != 0)
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "Off for this session: %s.", reason);
            else if (enabled)
                ImGui::TextUnformatted("Waiting for the upscaler to run.");
        }
        else
        {
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "Running.");
        }

        ImGui::Spacing();
        ImGui::PushItemWidth(220.0f * ctx.menuResScale);

        ImGui::SeparatorText("How much of it lands");

        float transfer = config->DlssNrTransferStrength.value_or_default();
        if (ImGui::SliderFloat("Detail strength", &transfer, 0.0f, 4.0f, "%.2f"))
            config->DlssNrTransferStrength = transfer;

        ShowHelpMarker("How much of the model's luminance edit reaches the frame."
                       "@@0 gives back exactly what the upscaler produced -- the encode and decode are"
                       "@exact inverses, so this is a true bypass, not an approximation of one."
                       "@@Above 1 exaggerates the edit. If nothing here seems to do anything, push this"
                       "@to 4 and watch: that answers whether the model is contributing at all.");

        float colour = config->DlssNrColourStrength.value_or_default();
        if (ImGui::SliderFloat("Colour strength", &colour, 0.0f, 4.0f, "%.2f"))
            config->DlssNrColourStrength = colour;

        ShowHelpMarker("The same, for the colour part of the edit, which is separated out because it is"
                       "@usually the part you do not want. Detail synthesis is a luminance change; a"
                       "@colour shift is mostly the model drifting.");

        ImGui::SeparatorText("Model");

        int preset = (int) config->DlssNrPreset.value_or_default();
        if (ImGui::SliderInt("Model preset", &preset, 0, 8))
            config->DlssNrPreset = (uint32_t) preset;

        ShowHelpMarker("0 leaves the choice to the model."
                       "@@Undocumented, and not the same scale as the super resolution or ray"
                       "@reconstruction presets -- the same number means something different here."
                       "@@Baked in when the model is created, so it takes effect on the next restart,"
                       "@unlike everything else in this panel.");

        int style = (int) config->DlssNrStyle.value_or_default();
        if (ImGui::SliderInt("Style", &style, 0, 8))
            config->DlssNrStyle = (uint32_t) style;

        ShowHelpMarker("Also undocumented. Not every value appears to be a distinct model -- several"
                       "@look identical in practice. The range is what the parameter accepts, not a"
                       "@promise that all nine differ.");

        float intensity = config->DlssNrIntensity.value_or_default();
        if (ImGui::SliderFloat("Intensity", &intensity, 0.0f, 1.0f, "%.2f"))
            config->DlssNrIntensity = intensity;

        ShowHelpMarker("The model's own strength control, applied inside it. Distinct from detail"
                       "@strength above, which scales the result afterwards.");

        float localStructure = config->DlssNrLocalStructure.value_or_default();
        if (ImGui::SliderFloat("Local structure", &localStructure, 0.0f, 1.0f, "%.2f"))
            config->DlssNrLocalStructure = localStructure;

        float localTone = config->DlssNrLocalTone.value_or_default();
        if (ImGui::SliderFloat("Local tone", &localTone, 0.0f, 1.0f, "%.2f"))
            config->DlssNrLocalTone = localTone;

        float skin = config->DlssNrSkinStructure.value_or_default();
        if (ImGui::SliderFloat("Skin structure", &skin, -1.0f, 1.0f, "%.2f"))
            config->DlssNrSkinStructure = skin;

        ShowHelpMarker("-1 means follow local structure, and is the model's own default -- it is not a"
                       "@strength of zero. 0 and above set skin independently of the rest of the frame.");

        bool autoMask = config->DlssNrAutoMask.value_or_default();
        if (ImGui::Checkbox("Auto skin mask", &autoMask))
            config->DlssNrAutoMask = autoMask;

        ShowHelpMarker("Lets the model find skin itself rather than treating the frame uniformly.");

        ImGui::SeparatorText("Colour");

        bool autoWhite = config->DlssNrAutoWhitePoint.value_or_default();
        if (ImGui::Checkbox("Automatic white point", &autoWhite))
            config->DlssNrAutoWhitePoint = autoWhite;

        ShowHelpMarker("The upscaler's output is linear HDR with an open-ended range; the model was"
                       "@trained on finished, sRGB-encoded frames. The white point is what maps one to"
                       "@the other, and it belongs to the game's exposure."
                       "@@Measured frame means in Cyberpunk alone have ranged from 0.065 in gameplay to"
                       "@185 in another scene, so no fixed number can serve. This measures the frame and"
                       "@follows it."
                       "@@Changing it cannot shift the finished image: the encode and resolve use the"
                       "@same value and are exact inverses. It only moves what the model is shown.");

        if (autoWhite)
        {
            const float current = DlssNr::CurrentWhitePoint();

            if (current > 0.0f)
                ImGui::Text("Measured: %.3f", current);
            else
                ImGui::TextUnformatted("Measuring...");
        }
        else
        {
            float whitePoint = config->DlssNrWhitePoint.value_or_default();
            if (ImGui::SliderFloat("White point", &whitePoint, 0.01f, 300.0f, "%.2f",
                                   ImGuiSliderFlags_Logarithmic))
                config->DlssNrWhitePoint = whitePoint;

            ShowHelpMarker("Too low and highlights flatten out before the model sees them; too high and"
                           "@it works on a dark, featureless frame.");
        }

        ImGui::SeparatorText("Inspect");

        static const char* debugNames[] = { "Off", "Proxy (what the model sees)", "Model output (raw)",
                                            "Difference (amplified)" };
        int debugView = (int) config->DlssNrDebugView.value_or_default();
        if (ImGui::Combo("Debug view", &debugView, debugNames, IM_ARRAYSIZE(debugNames)))
            config->DlssNrDebugView = (uint32_t) debugView;

        ShowHelpMarker("Proxy is the picture handed to the model -- if that looks wrong, the white point"
                       "@is wrong and nothing downstream can be judged."
                       "@@Difference shows what the model actually changed, amplified twenty times and"
                       "@centred on grey. A flat grey frame there means it is doing nothing.");

        ImGui::PopItemWidth();
    }
}

void MenuCommon::RenderMagnifierSettings(RenderMenuContext& ctx)'''

SECTION = SECTION.replace("@", NL)

path = "C:/Games_Temp/OptiScaler/OptiScaler/menu/menu_common.cpp"
text = io.open(path, encoding="utf-8").read()

start = text.index("void MenuCommon::RenderDlssNrSettings(RenderMenuContext& ctx)")
end = text.index("void MenuCommon::RenderMagnifierSettings(RenderMenuContext& ctx)")

text = text[:start] + SECTION + text[end + len("void MenuCommon::RenderMagnifierSettings(RenderMenuContext& ctx)"):]

io.open(path, "w", encoding="utf-8").write(text)
print("menu patched")
