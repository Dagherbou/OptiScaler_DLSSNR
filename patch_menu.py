"""Adds the DLSS Neural Rendering panel to OptiScaler's menu.

Written as a file rather than run inline because the section is full of backslash escapes, and passing
those through a shell heredoc silently turns them into real newlines inside string literals.
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
                       "@Undocumented and driven directly, so nothing here is officially supported.");

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

        int preset = (int) config->DlssNrPreset.value_or_default();
        if (ImGui::SliderInt("Model preset", &preset, 0, 8))
            config->DlssNrPreset = (uint32_t) preset;

        ShowHelpMarker("0 leaves the choice to the model."
                       "@@The Neural Rendering presets are undocumented, and they are not the same scale"
                       "@as the super resolution or ray reconstruction ones -- the same number means"
                       "@something different here. Takes effect when the upscaler is next created.");

        float intensity = config->DlssNrIntensity.value_or_default();
        if (ImGui::SliderFloat("Intensity", &intensity, 0.0f, 1.0f, "%.2f"))
            config->DlssNrIntensity = intensity;

        int style = (int) config->DlssNrStyle.value_or_default();
        if (ImGui::SliderInt("Style", &style, 0, 8))
            config->DlssNrStyle = (uint32_t) style;

        ShowHelpMarker("Also undocumented. This range is a guess at what the model accepts.");

        float localStructure = config->DlssNrLocalStructure.value_or_default();
        if (ImGui::SliderFloat("Local structure", &localStructure, 0.0f, 1.0f, "%.2f"))
            config->DlssNrLocalStructure = localStructure;

        float localTone = config->DlssNrLocalTone.value_or_default();
        if (ImGui::SliderFloat("Local tone", &localTone, 0.0f, 1.0f, "%.2f"))
            config->DlssNrLocalTone = localTone;

        float skinStructure = config->DlssNrSkinStructure.value_or_default();
        if (ImGui::SliderFloat("Skin structure", &skinStructure, 0.0f, 1.0f, "%.2f"))
            config->DlssNrSkinStructure = skinStructure;

        bool autoMask = config->DlssNrAutoMask.value_or_default();
        if (ImGui::Checkbox("Auto mask", &autoMask))
            config->DlssNrAutoMask = autoMask;

        ShowHelpMarker("Lets the model decide where to apply enhancement.");

        ImGui::Spacing();

        bool tone = config->DlssNrToneTransform.value_or_default();
        if (ImGui::Checkbox("Tone transform", &tone))
            config->DlssNrToneTransform = tone;

        ShowHelpMarker("The upscaler's output is linear HDR; the model was trained on finished,"
                       "@display-referred frames. This encodes the frame before the model and applies"
                       "@the exact inverse after, so anything the model leaves alone comes back"
                       "@unchanged.@@Turning it off feeds the model raw HDR, which does not merely"
                       "@shift colour -- it makes the frame unusable.");

        float whitePoint = config->DlssNrWhitePoint.value_or_default();
        if (ImGui::SliderFloat("White point", &whitePoint, 0.5f, 20.0f, "%.1f"))
            config->DlssNrWhitePoint = whitePoint;

        ShowHelpMarker("Linear value the encode maps to display white, which depends on the game's"
                       "@exposure. Too low and highlights flatten out before the model sees them; too"
                       "@high and it works on a dark, featureless frame.");

        ImGui::PopItemWidth();
    }
}

void MenuCommon::RenderMagnifierSettings(RenderMenuContext& ctx)'''

# '@' stands in for an escaped newline so this file carries no backslashes of its own.
SECTION = SECTION.replace("@", NL)

root = "C:/Games_Temp/OptiScaler/OptiScaler/"

path = root + "menu/menu_common.h"
text = io.open(path, encoding="utf-8").read()
decl = "    static void RenderMagnifierSettings(RenderMenuContext& ctx);"
assert decl in text, "magnifier declaration not found"
if "RenderDlssNrSettings" not in text:
    text = text.replace(decl, "    static void RenderDlssNrSettings(RenderMenuContext& ctx);\n" + decl, 1)
    io.open(path, "w", encoding="utf-8").write(text)

path = root + "menu/menu_common.cpp"
text = io.open(path, encoding="utf-8").read()
anchor = "void MenuCommon::RenderMagnifierSettings(RenderMenuContext& ctx)"
assert anchor in text, "magnifier definition not found"
assert "RenderDlssNrSettings" not in text, "already patched"

text = text.replace(anchor, SECTION, 1)

call = "        RenderMagnifierSettings(ctx);"
assert call in text, "magnifier call not found"
text = text.replace(call, "        RenderDlssNrSettings(ctx);\n" + call, 1)

inc = '#include "menu_common.h"'
assert inc in text
text = text.replace(inc, inc + "\n\n#include <dlssnr/DlssNr_Dx12.h>", 1)

io.open(path, "w", encoding="utf-8").write(text)
print("patched")
