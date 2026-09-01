#include "pch.h"

#include "DlssNr.h"

#include <Config.h>

#include <imgui/imgui.h>

#include <cctype>
#include <string>

namespace DlssNr
{

// A whole standalone overlay -- its own window, its own colours, independent of the rest of
// OptiScaler's shared menu chrome (title bar, graphs, bottom bar) and its user-configurable theme.
// Modelled on NVIDIA's own DLSS 5 Developer Controls panel.

// Dialled down from the first pass: less saturated, less "neon" lime, closer to a muted olive.
static const ImVec4 kAccent(0.46f, 0.62f, 0.32f, 1.0f);
static const ImVec4 kText(0.90f, 0.92f, 0.93f, 1.0f);
static const ImVec4 kTextDim(0.55f, 0.58f, 0.60f, 1.0f);
// Solid dark grey, fully opaque -- not the translucent near-black from the first pass.
static const ImVec4 kPanelBg(0.16f, 0.16f, 0.17f, 1.0f);

static float PanelWidth(float scale)
{
    return 460.0f * scale;
}

static void HelpMarker(const char* tip)
{
    ImGui::SameLine();
    ImGui::TextColored(kTextDim, "(?)");

    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
        ImGui::TextUnformatted(tip);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// Upper-cased, letter-tracked captions, matching NVIDIA's own panel ("GLOBAL CONTROLS", ...).
static std::string Tracked(const char* text)
{
    std::string out;

    for (const char* p = text; *p; ++p)
    {
        if (*p == ' ')
        {
            out += "   ";
            continue;
        }

        out += (char) std::toupper((unsigned char) *p);
        out += ' ';
    }

    if (!out.empty() && out.back() == ' ')
        out.pop_back();

    return out;
}

static void SectionCaption(const char* text, float rowWidth)
{
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, kText);
    ImGui::TextUnformatted(Tracked(text).c_str());
    ImGui::PopStyleColor();

    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(p0, ImVec2(p0.x + rowWidth, p0.y), ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.14f)), 1.0f);
    ImGui::Dummy(ImVec2(rowWidth, 6.0f));
}

struct SliderResult
{
    bool changed;
    bool released;
};

// Custom-drawn slider: label on the left, a thin track with a filled portion and a round
// handle in the middle, and the value as its own right-aligned text -- not centred inside the
// track the way stock ImGui::SliderFloat draws it, which is what made the handle collide with
// the digits in the first pass.
static SliderResult NrSlider(const char* label, float* value, float vMin, float vMax, const char* fmt,
                              float rowWidth, bool showFill = true)
{
    ImGui::PushID(label);

    ImGuiStyle& style = ImGui::GetStyle();
    float labelWidth = rowWidth * 0.44f;
    float valueWidth = 52.0f;
    float trackWidth = rowWidth - labelWidth - valueWidth - style.ItemSpacing.x * 2.0f;
    if (trackWidth < 40.0f)
        trackWidth = 40.0f;

    ImGui::PushStyleColor(ImGuiCol_Text, kText);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::SameLine(labelWidth);

    ImVec2 pos = ImGui::GetCursorScreenPos();
    float rowH = ImGui::GetFrameHeight();
    float trackH = 4.0f;
    float radius = 6.5f;
    ImVec2 tMin(pos.x, pos.y + rowH * 0.5f - trackH * 0.5f);
    ImVec2 tMax(pos.x + trackWidth, tMin.y + trackH);

    ImGui::InvisibleButton("track", ImVec2(trackWidth, rowH));
    bool active = ImGui::IsItemActive();
    bool hovered = ImGui::IsItemHovered();

    ImGuiStorage* store = ImGui::GetStateStorage();
    ImGuiID wasActiveId = ImGui::GetID("wasActive");
    bool wasActive = store->GetBool(wasActiveId, false);

    bool changed = false;
    if (active)
    {
        float t = (ImGui::GetIO().MousePos.x - tMin.x) / trackWidth;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        float newVal = vMin + t * (vMax - vMin);
        if (newVal != *value)
        {
            *value = newVal;
            changed = true;
        }
    }

    bool released = wasActive && !active;
    store->SetBool(wasActiveId, active);

    float t = (*value - vMin) / (vMax - vMin);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    float handleX = tMin.x + t * trackWidth;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(tMin, tMax, ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.12f)), trackH * 0.5f);

    if (showFill)
        dl->AddRectFilled(tMin, ImVec2(handleX, tMax.y), ImGui::GetColorU32(kAccent), trackH * 0.5f);

    ImVec4 hCol = (hovered || active) ? kAccent : ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.88f);
    ImVec2 hCenter(handleX, tMin.y + trackH * 0.5f);
    dl->AddCircleFilled(hCenter, radius, ImGui::GetColorU32(hCol), 18);
    dl->AddCircle(hCenter, radius, ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.45f)), 18, 1.4f);

    ImGui::SameLine(labelWidth + trackWidth + style.ItemSpacing.x);
    ImGui::PushStyleColor(ImGuiCol_Text, kText);
    ImGui::Text(fmt, *value);
    ImGui::PopStyleColor();

    ImGui::PopID();

    return { changed, released };
}

static bool NrCombo(const char* label, int* v, const char* const* items, int count, float rowWidth)
{
    float labelWidth = rowWidth * 0.44f;

    ImGui::PushStyleColor(ImGuiCol_Text, kText);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::SameLine(labelWidth);

    ImGui::SetNextItemWidth(rowWidth - labelWidth);
    std::string id = std::string("##") + label;
    return ImGui::Combo(id.c_str(), v, items, count);
}

// One entry in the "Models" row -- the segmented Model A / B / C selector.
static bool ModelButton(const char* label, bool active, float width)
{
    if (active)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.22f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.30f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.35f));
        ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
        ImGui::PushStyleColor(ImGuiCol_Border, kAccent);
    }
    else
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.05f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.09f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 1.0f, 1.0f, 0.12f));
        ImGui::PushStyleColor(ImGuiCol_Text, kText);
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.12f));
    }

    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);

    bool clicked = ImGui::Button(label, ImVec2(width, 0.0f));

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(5);

    return clicked;
}

// A filled green square with a dark checkmark when set, matching NVIDIA's own panel -- not
// stock ImGui::Checkbox's outlined box with a coloured glyph.
static bool NrCheckbox(const char* label, bool* v)
{
    ImGui::PushID(label);

    float boxSize = ImGui::GetFontSize() + 5.0f;
    ImVec2 pos = ImGui::GetCursorScreenPos();

    bool clicked = ImGui::InvisibleButton("box", ImVec2(boxSize, boxSize));
    bool hovered = ImGui::IsItemHovered();
    if (clicked)
        *v = !*v;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 c0 = pos;
    ImVec2 c1 = ImVec2(pos.x + boxSize, pos.y + boxSize);

    if (*v)
    {
        ImVec4 fill = hovered ? ImVec4(std::min(kAccent.x + 0.08f, 1.0f), std::min(kAccent.y + 0.08f, 1.0f),
                                       std::min(kAccent.z + 0.08f, 1.0f), 1.0f)
                              : kAccent;
        dl->AddRectFilled(c0, c1, ImGui::GetColorU32(fill), 3.0f);

        ImU32 dark = ImGui::GetColorU32(ImVec4(0.06f, 0.09f, 0.05f, 1.0f));
        ImVec2 a(pos.x + boxSize * 0.22f, pos.y + boxSize * 0.55f);
        ImVec2 b(pos.x + boxSize * 0.42f, pos.y + boxSize * 0.76f);
        ImVec2 cpt(pos.x + boxSize * 0.80f, pos.y + boxSize * 0.26f);
        dl->AddLine(a, b, dark, 2.2f);
        dl->AddLine(b, cpt, dark, 2.2f);
    }
    else
    {
        dl->AddRectFilled(c0, c1, ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, hovered ? 0.10f : 0.06f)), 3.0f);
        dl->AddRect(c0, c1, ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.24f)), 3.0f, 0, 1.2f);
    }

    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, kText);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();

    ImGui::PopID();
    return clicked;
}

void RenderMenu(Config* config, float menuResScale)
{
    ImGuiIO& io = ImGui::GetIO();
    float rowWidth = PanelWidth(menuResScale);

    // Pinned to the left edge, vertically centred -- the position NVIDIA's own overlay uses.
    // Not user-movable: this is the only thing this build shows, so there is nothing to arrange
    // it around.
    float margin = 24.0f * menuResScale;
    ImGui::SetNextWindowPos(ImVec2(margin, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.0f, 0.5f));

    ImGui::PushStyleColor(ImGuiCol_WindowBg, kPanelBg);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.10f));
    ImGui::PushStyleColor(ImGuiCol_Text, kText);
    ImGui::PushStyleColor(ImGuiCol_CheckMark, kAccent);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1.0f, 1.0f, 1.0f, 0.06f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.10f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(1.0f, 1.0f, 1.0f, 0.14f));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.25f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.35f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.45f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.07f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.12f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 1.0f, 1.0f, 0.16f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.07f, 0.075f, 0.08f, 0.98f));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 18.0f) * menuResScale);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 13.0f * menuResScale));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar;

    bool anyChanged = false;

    if (ImGui::Begin("##DlssNrOverlay", nullptr, flags))
    {
        // Matches the old shared window's behaviour: claim focus so keyboard/mouse routes here
        // rather than being left with whatever last had it.
        if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow))
            ImGui::SetWindowFocus();

        ImGui::Dummy(ImVec2(rowWidth, 0.0f));

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.97f, 0.98f, 1.0f, 1.0f));
        ImGui::TextUnformatted(Tracked("DLSS 5 Developer Controls").c_str());
        ImGui::PopStyleColor();

        ImGui::Spacing();
        ImGui::Spacing();

        bool enabled = config->DlssNrEnabled.value_or_default();
        if (NrCheckbox("DLSS ON", &enabled))
        {
            config->DlssNrEnabled = enabled;
            anyChanged = true;
        }

        HelpMarker("Synthesises detail in the upscaler's output, before frame generation sees it."
                       "\n\nNeeds two similarly named files beside OptiScaler, one character apart:"
                       "\n  nvngx_dlssnr.dll       NVIDIA's model (~165 MB) -- you supply it"
                       "\n  nvngx.dll_dlssnr.dll   the forwarder (~13 KB) -- ships in this package"
                       "\nUndocumented and driven directly, so none of this is officially supported.");

        ImGui::TextColored(kTextDim, "Can be toggled with a key -- bind it under Keybinds, \"Neural Rendering\".");

        if (!DlssNr::IsRunning())
        {
            const char* reason = DlssNr::FailureReason();

            if (reason[0] != 0)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "Off for this session: %s.", reason);
                ImGui::SameLine();

                if (ImGui::SmallButton("Retry"))
                    DlssNr::RetryAfterFailure();
            }
            else if (enabled)
            {
                ImGui::TextColored(kTextDim, "Waiting for the upscaler to run.");
                // The one thing the old shared window told you here that this panel otherwise
                // wouldn't: this needs the game's own upscaler active, not just this checkbox.
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + rowWidth);
                ImGui::TextColored(kTextDim,
                                   "Needs DLSS or XeSS selected as the upscaler in the game's own "
                                   "video settings, and a save loaded -- this (and the rest of "
                                   "OptiScaler) does not run in menus.");
                ImGui::PopTextWrapPos();
            }
        }
        else
        {
            const auto ms = DlssNr::LastGpuTime();

            if (ms.has_value())
                ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.45f, 1.0f), "Running - %.2f ms per frame", ms.value());
            else
                ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.45f, 1.0f), "Running.");

            ImGui::SameLine();
            ImGui::TextColored(kTextDim, "(?)");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("The whole pass: the staging copies and the resolve as well as the"
                                  "\nmodel. Timing only the model would flatter the number."
                                  "\n\nCompare it against the frame time at the bottom of this window to"
                                  "\nsee what it is costing you.");
        }

        // Global Controls -- DlssNrLocalStructure / DlssNrLocalTone: NVIDIA's own name for
        // these two in its DLSS 5 developer overlay.
        SectionCaption("Global Controls", rowWidth);

        float localStructure = config->DlssNrLocalStructure.value_or_default();
        auto rStruct = NrSlider("Structure Intensity", &localStructure, 0.0f, 2.0f, "%.2f", rowWidth);
        if (rStruct.changed)
            config->DlssNrLocalStructure = localStructure;
        if (rStruct.released)
            anyChanged = true;
        HelpMarker("The model's structure-synthesis strength across the whole frame.");

        float localTone = config->DlssNrLocalTone.value_or_default();
        auto rTone = NrSlider("Tone Intensity", &localTone, 0.0f, 2.0f, "%.2f", rowWidth);
        if (rTone.changed)
            config->DlssNrLocalTone = localTone;
        if (rTone.released)
            anyChanged = true;
        HelpMarker("The model's tone-remapping strength across the whole frame.");

        // Model Automask -- DlssNrAutoMask, and the masked-region strength that rides with it.
        SectionCaption("Model Automask", rowWidth);

        bool autoMask = config->DlssNrAutoMask.value_or_default();
        if (NrCheckbox("Model Automask", &autoMask))
        {
            config->DlssNrAutoMask = autoMask;
            anyChanged = true;
        }
        HelpMarker("Lets the model find skin itself rather than treating the frame uniformly.");

        // Matches NVIDIA's own panel: greyed out while Automask is off. The value underneath is
        // unchanged either way -- this only stops it being dragged while it has nothing to act on.
        ImGui::BeginDisabled(!autoMask);
        ImGui::PushID("Automask");
        float skin = config->DlssNrSkinStructure.value_or_default();
        auto rSkin = NrSlider("Structure Intensity", &skin, -1.0f, 2.0f, "%.2f", rowWidth);
        if (rSkin.changed)
            config->DlssNrSkinStructure = skin;
        if (rSkin.released)
            anyChanged = true;
        ImGui::PopID();
        ImGui::EndDisabled();
        HelpMarker("-1 means follow the Global Controls Structure Intensity above, and is the"
                       "\nmodel's own default. 0 and above set the masked region's structure"
                       "\nindependently of the rest of the frame."
                       "\n\nGreyed out while Model Automask is off -- there is no mask for it to"
                       "\nshape without it.");

        // Models -- DlssNrPreset. NVIDIA ships no letters in the binary; "Model A/B/C" is this
        // fork's best match to the segmented selector in the developer overlay, alongside
        // Default (preset index 0), a real, distinct fourth state.
        SectionCaption("Models", rowWidth);

        static const char* nrPresetNames[] = { "Default", "Model A", "Model B", "Model C" };
        int preset = (int) config->DlssNrPreset.value_or_default();

        {
            float spacing = ImGui::GetStyle().ItemSpacing.x;
            float btnWidth = (rowWidth - spacing * (IM_ARRAYSIZE(nrPresetNames) - 1)) / IM_ARRAYSIZE(nrPresetNames);

            for (int i = 0; i < IM_ARRAYSIZE(nrPresetNames); i++)
            {
                if (i > 0)
                    ImGui::SameLine();

                ImGui::PushID(i);
                if (ModelButton(nrPresetNames[i], preset == i, btnWidth))
                {
                    preset = i;
                    config->DlssNrPreset = (uint32_t) preset;
                    anyChanged = true;
                }
                ImGui::PopID();
            }
        }
        HelpMarker("Not the same scale as the super resolution or ray reconstruction presets --"
                       "\nthe same letter means something different here."
                       "\n\nRead when the model is built, so a change rebuilds it after a moment.");

        static const char* nrStyleNames[] = { "Default (standard)", "Natural", "Cinematic" };
        int style = (int) config->DlssNrStyle.value_or_default();
        if (style > 2)
            style = 2;
        if (NrCombo("Style", &style, nrStyleNames, IM_ARRAYSIZE(nrStyleNames), rowWidth))
        {
            config->DlssNrStyle = (uint32_t) style;
            anyChanged = true;
        }
        HelpMarker("The model's own processing profiles."
                   "\n\nDefault (standard): the strongest, and most likely to look 'stylised'."
                   "\nNatural: the same detail work with a gentler hand."
                   "\nCinematic: tones down the shine and over-processing for a film-like look."
                   "\n\nThe names come from community testing, unlike the panel labels above --"
                   "\nNVIDIA ships no names for this control in the binaries.");

        float intensity = config->DlssNrIntensity.value_or_default();
        auto rIntensity = NrSlider("Intensity", &intensity, 0.0f, 2.0f, "%.2f", rowWidth);
        if (rIntensity.changed)
            config->DlssNrIntensity = intensity;
        if (rIntensity.released)
            anyChanged = true;
        HelpMarker("The model's own strength control, applied inside it. Distinct from the Global"
                       "\nControls above, and from Detail strength below, which scales the result"
                       "\nafterwards.");

        // Everything below is this fork's own instrumentation, with no equivalent in NVIDIA's
        // developer overlay -- kept under its original names.
        SectionCaption("Cost", rowWidth);

        static int pendingScale = -1;
        float scalePercent = pendingScale >= 0 ? (float) pendingScale
                                                : config->DlssNrWorkingScale.value_or_default() * 100.0f;

        auto rScale = NrSlider("Model resolution", &scalePercent, 25.0f, 100.0f, "%.0f%%", rowWidth);
        if (rScale.changed)
            pendingScale = (int) lroundf(scalePercent);

        if (rScale.released && pendingScale >= 0)
        {
            config->DlssNrWorkingScale = std::clamp(pendingScale, 25, 100) / 100.0f;
            pendingScale = -1;
            anyChanged = true;
        }
        HelpMarker("What fraction of the frame the model works at. Cost falls with the square of"
                       "\nthis, so half resolution is roughly a quarter of the time. The frame is"
                       "\nnever reduced -- only the model's own contribution is computed small and"
                       "\nenlarged. Applied when the handle is let go, not while it is moving.");

        SectionCaption("How much of it lands", rowWidth);

        float transfer = config->DlssNrTransferStrength.value_or_default();
        auto rTransfer = NrSlider("Detail strength", &transfer, 0.0f, 2.0f, "%.2f", rowWidth);
        if (rTransfer.changed)
            config->DlssNrTransferStrength = transfer;
        if (rTransfer.released)
            anyChanged = true;
        HelpMarker("How far the frame moves toward the model's picture. 0 gives back exactly what"
                       "\nthe upscaler produced. 1 is the model's picture. Above 1 carries on past"
                       "\nit in the same direction.");

        float colour = config->DlssNrColourStrength.value_or_default();
        auto rColour = NrSlider("Colour strength", &colour, 0.0f, 1.0f, "%.2f", rowWidth);
        if (rColour.changed)
            config->DlssNrColourStrength = colour;
        if (rColour.released)
            anyChanged = true;
        HelpMarker("Whether the model's colour arrives with its light. 0 keeps the game's own hue"
                       "\nexactly. 1 brings the model's colour as well, clamped into AP1.");

        SectionCaption("Colour", rowWidth);

        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + rowWidth);
        ImGui::TextColored(kTextDim,
                           "The model was trained on finished, sRGB-encoded frames. These decide how "
                           "the upscaler's linear output is mapped into something it recognises.");
        ImGui::PopTextWrapPos();

        float wpScale = config->DlssNrWhitePointScale.value_or_default();
        auto rWp = NrSlider("Paper white", &wpScale, 0.25f, 4.0f, "%.2fx", rowWidth);
        if (rWp.changed)
            config->DlssNrWhitePointScale = wpScale;
        if (rWp.released)
            anyChanged = true;
        HelpMarker("Multiplies the white point before the model sees the frame. Above 1 the picture"
                       "\nhanded over is darker, so highlights sit lower on the curve.");

        float maxRatio = config->DlssNrMaxRatio.value_or_default();
        auto rMax = NrSlider("Highlight guard", &maxRatio, 1.0f, 8.0f, "%.1fx", rowWidth);
        if (rMax.changed)
            config->DlssNrMaxRatio = maxRatio;
        if (rMax.released)
            anyChanged = true;
        HelpMarker("The most the pass may brighten any pixel, as a multiple of what it already was."
                       "\nDarkening is not capped by this -- only growth is.");

        SectionCaption("Inspect", rowWidth);

        if (DlssNr::CaptureInProgress())
        {
            ImGui::TextColored(kTextDim, "Capturing...");
        }
        else if (ImGui::Button("Capture 8 frames"))
        {
            DlssNr::RequestCapture(8);
        }
        HelpMarker("Writes eight consecutive frames twice: as the upscaler produced them, and again"
                       "\nonce the model's edit was applied. Into a dlssnr-capture folder beside"
                       "\nOptiScaler; each run overwrites the last.");

        static const char* compareNames[] = { "Off", "Side by side", "Wipe" };
        int compare = (int) config->DlssNrCompare.value_or_default();
        if (NrCombo("Compare", &compare, compareNames, IM_ARRAYSIZE(compareNames), rowWidth))
        {
            config->DlssNrCompare = (uint32_t) compare;
            anyChanged = true;
        }
        HelpMarker("Shows the pass against itself. Side by side puts the whole frame in each half;"
                       "\nwipe cuts a single frame at the split and plays normally. Neither needs the"
                       "\nmenu open to keep working.");

        if (compare != 0)
        {
            bool swap = config->DlssNrCompareSwap.value_or_default();
            if (NrCheckbox("Swap sides", &swap))
            {
                config->DlssNrCompareSwap = swap;
                anyChanged = true;
            }
        }

        if (compare == 1)
        {
            float zoom = config->DlssNrCompareZoom.value_or_default();
            auto rZoom = NrSlider("Zoom", &zoom, 1.0f, 2.0f, "%.2f", rowWidth);
            if (rZoom.changed)
                config->DlssNrCompareZoom = std::clamp(zoom, 1.0f, 2.0f);
            if (rZoom.released)
                anyChanged = true;
        }

        if (compare == 2)
        {
            float split = config->DlssNrCompareSplit.value_or_default();
            auto rSplit = NrSlider("Split", &split, 0.0f, 1.0f, "%.2f", rowWidth);
            if (rSplit.changed)
                config->DlssNrCompareSplit = std::clamp(split, 0.0f, 1.0f);
            if (rSplit.released)
                anyChanged = true;
        }

        static const char* debugNames[] = { "Off", "Proxy (what the model sees)", "Model output (raw)",
                                            "Difference (amplified)" };
        int debugView = (int) config->DlssNrDebugView.value_or_default();
        if (NrCombo("Debug view", &debugView, debugNames, IM_ARRAYSIZE(debugNames), rowWidth))
        {
            config->DlssNrDebugView = (uint32_t) debugView;
            anyChanged = true;
        }
        HelpMarker("Proxy is the picture handed to the model. Difference shows what the model"
                       "\nactually changed, amplified twenty times and centred on grey.");

        ImGui::End();
    }

    ImGui::PopStyleVar(5);
    ImGui::PopStyleColor(14);

    // This overlay saves as you go rather than needing a Save button -- there is nothing else
    // in this build's menu to put one on.
    if (anyChanged)
        config->SaveIni();
}

} // namespace DlssNr
