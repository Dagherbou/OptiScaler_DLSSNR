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

// Palette sampled straight off a capture of NVIDIA's own panel rather than eyeballed, so the
// numbers below are what the screenshot measures, give or take JPEG noise:
//   accent green   #84B63A - #97B948   (slider fill, checkbox fill, selected model)
//   panel bg       #1A191A - #202021   (darker than the grey this overlay used before)
//   unfilled track #282828
//   title text     #E7E7E7   caption text #C6C7CB
//   row label      #A2A2A0   value text   #8A8A8C  (labels sit dimmer than captions)
//   disabled       caption #605E5F, label #4D4C4A, track #41413F, handle #424242
static const ImVec4 kAccent(0.549f, 0.729f, 0.239f, 1.0f);
static const ImVec4 kTitle(0.906f, 0.906f, 0.906f, 1.0f);
static const ImVec4 kCaption(0.776f, 0.780f, 0.796f, 1.0f);
static const ImVec4 kText(0.635f, 0.635f, 0.627f, 1.0f);
static const ImVec4 kValue(0.541f, 0.541f, 0.549f, 1.0f);
static const ImVec4 kTextDim(0.376f, 0.369f, 0.373f, 1.0f);
static const ImVec4 kTrack(0.157f, 0.157f, 0.157f, 1.0f);
static const ImVec4 kPanelBg(0.110f, 0.110f, 0.114f, 1.0f);

static float PanelWidth(float scale) { return 460.0f * scale; }

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
    ImGui::PushStyleColor(ImGuiCol_Text, kCaption);
    ImGui::TextUnformatted(Tracked(text).c_str());
    ImGui::PopStyleColor();

    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(p0, ImVec2(p0.x + rowWidth, p0.y),
                                        ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.14f)), 1.0f);
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
static SliderResult NrSlider(const char* label, float* value, float vMin, float vMax, const char* fmt, float rowWidth,
                             bool showFill = true)
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
    dl->AddRectFilled(tMin, tMax, ImGui::GetColorU32(kTrack), trackH * 0.5f);

    if (showFill)
        dl->AddRectFilled(tMin, ImVec2(handleX, tMax.y), ImGui::GetColorU32(kAccent), trackH * 0.5f);

    ImVec4 hCol = (hovered || active) ? kAccent : ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.88f);
    ImVec2 hCenter(handleX, tMin.y + trackH * 0.5f);
    dl->AddCircleFilled(hCenter, radius, ImGui::GetColorU32(hCol), 18);
    dl->AddCircle(hCenter, radius, ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.45f)), 18, 1.4f);

    ImGui::SameLine(labelWidth + trackWidth + style.ItemSpacing.x);
    ImGui::PushStyleColor(ImGuiCol_Text, kValue);
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
    // Sampled off NVIDIA's panel: the selected pill is a dark olive fill (#353D1D) with a green
    // border and green label; the unselected ones are flat neutral grey (#3A3A3A, text #8C8C8C).
    if (active)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.208f, 0.239f, 0.114f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.247f, 0.286f, 0.137f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.278f, 0.322f, 0.153f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
        ImGui::PushStyleColor(ImGuiCol_Border, kAccent);
    }
    else
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.227f, 0.227f, 0.227f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.267f, 0.267f, 0.267f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.298f, 0.298f, 0.298f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.549f, 0.549f, 0.549f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.10f));
    }

    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);

    bool clicked = ImGui::Button(label, ImVec2(width, 0.0f));

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(5);

    return clicked;
}

// The small right-aligned "Show Mask" / "Show Masks" toggle that rides on the right end of a
// section row in NVIDIA's panel: label first, small box after it, the pair pushed to the right
// edge of the row rather than following the section label.
static bool NrRightCheckbox(const char* label, bool* v, float rowWidth)
{
    ImGui::PushID(label);

    float boxSize = ImGui::GetFontSize() + 1.0f;
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float pairWidth = ImGui::CalcTextSize(label).x + spacing + boxSize;

    ImGui::SameLine(rowWidth - pairWidth);

    ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();

    ImGui::SameLine(0.0f, spacing);

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
        dl->AddRectFilled(c0, c1, ImGui::GetColorU32(kAccent), 2.0f);

        ImU32 dark = ImGui::GetColorU32(ImVec4(0.06f, 0.09f, 0.05f, 1.0f));
        ImVec2 a(pos.x + boxSize * 0.22f, pos.y + boxSize * 0.55f);
        ImVec2 b(pos.x + boxSize * 0.42f, pos.y + boxSize * 0.76f);
        ImVec2 cpt(pos.x + boxSize * 0.80f, pos.y + boxSize * 0.26f);
        dl->AddLine(a, b, dark, 2.0f);
        dl->AddLine(b, cpt, dark, 2.0f);
    }
    else
    {
        dl->AddRectFilled(c0, c1, ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, hovered ? 0.10f : 0.05f)), 2.0f);
        dl->AddRect(c0, c1, ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.20f)), 2.0f, 0, 1.0f);
    }

    ImGui::PopID();
    return clicked;
}

// A filled green square with a dark checkmark when set, matching NVIDIA's own panel -- not
// stock ImGui::Checkbox's outlined box with a coloured glyph.
static bool NrCheckbox(const char* label, bool* v, bool caps = false)
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
    // Section-level rows ("DLSS ON", "MODEL AUTOMASK", "DEVELOPER MASKING") are letter-tracked
    // caps in the caption colour; the per-object rows under them stay sentence case.
    ImGui::PushStyleColor(ImGuiCol_Text, caps ? kCaption : kText);
    ImGui::TextUnformatted(caps ? Tracked(label).c_str() : label);
    ImGui::PopStyleColor();

    ImGui::PopID();
    return clicked;
}

void RenderMenu(Config* config, float menuResScale)
{
    ImGuiIO& io = ImGui::GetIO();
    auto& state = State::Instance();
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
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.157f, 0.157f, 0.157f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.196f, 0.196f, 0.196f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.227f, 0.227f, 0.227f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.25f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.35f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.45f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.07f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.12f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 1.0f, 1.0f, 0.16f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.086f, 0.086f, 0.090f, 0.98f));

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

        ImGui::PushStyleColor(ImGuiCol_Text, kTitle);
        ImGui::TextUnformatted(Tracked("DLSS 5 Developer Controls").c_str());
        ImGui::PopStyleColor();

        ImGui::Spacing();
        ImGui::Spacing();

        bool enabled = config->DlssNrEnabled.value_or_default();
        if (NrCheckbox("DLSS ON", &enabled, true))
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
                ImGui::TextColored(kTextDim, "Needs DLSS or XeSS selected as the upscaler in the game's own "
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
        auto rStruct = NrSlider("Structure Intensity", &localStructure, 0.0f, 1.0f, "%.2f", rowWidth);
        if (rStruct.changed)
            config->DlssNrLocalStructure = localStructure;
        if (rStruct.released)
            anyChanged = true;
        HelpMarker("The model's structure-synthesis strength across the whole frame.");

        float localTone = config->DlssNrLocalTone.value_or_default();
        auto rTone = NrSlider("Tone Intensity", &localTone, 0.0f, 1.0f, "%.2f", rowWidth);
        if (rTone.changed)
            config->DlssNrLocalTone = localTone;
        if (rTone.released)
            anyChanged = true;
        HelpMarker("The model's tone-remapping strength across the whole frame.");

        // Model Automask -- DlssNrAutoMask. In NVIDIA's panel this is a letter-tracked caps row
        // of its own with a "Show Mask" toggle on the right, not a section caption with a divider,
        // so it is drawn that way here.
        ImGui::Spacing();

        bool autoMask = config->DlssNrAutoMask.value_or_default();
        if (NrCheckbox("Model Automask", &autoMask, true))
        {
            config->DlssNrAutoMask = autoMask;
            anyChanged = true;
        }
        HelpMarker("Lets the model find skin itself rather than treating the frame uniformly.");

        // Greyed out, and not because a setting is off: the model keeps its mask to itself. It is
        // never handed back as a resource across the interface this fork drives, so there is
        // nothing for an overlay to draw.
        ImGui::BeginDisabled(true);
        bool showMask = false;
        NrRightCheckbox("Show Mask", &showMask, rowWidth);
        ImGui::EndDisabled();

        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("NVIDIA's panel can draw the automask over the frame. The model does not hand"
                              "\nits mask back through the interface this fork drives, so there is nothing"
                              "\nhere to display.");

        // Matches NVIDIA's own panel: greyed out while Automask is off. The value underneath is
        // unchanged either way -- this only stops it being dragged while it has nothing to act on.
        ImGui::BeginDisabled(!autoMask);
        ImGui::PushID("Automask");
        float skin = config->DlssNrSkinStructure.value_or_default();
        auto rSkin = NrSlider("Structure Intensity", &skin, -1.0f, 1.0f, "%.2f", rowWidth);
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

        // Developer Masking -- NVIDIA's per-object, engine-level masking. The game's own renderer
        // tags individual objects (the "Pitcher", "Grapes" and "Bottles" of NVIDIA's demo scene)
        // and hands those masks to DLSS through Streamline, so an artist can dial each object
        // separately. There is nothing here for this fork to drive: OptiScaler sits below the
        // engine, in the graphics API, with no object list and no way to author such masks. The
        // row is drawn because the panel this copies has it, and is disabled because it cannot be
        // made to work -- not because a setting is off.
        ImGui::Spacing();

        ImGui::BeginDisabled(true);
        bool devMasking = false;
        bool showMasks = false;
        NrCheckbox("Developer Masking", &devMasking, true);
        NrRightCheckbox("Show Masks", &showMasks, rowWidth);
        ImGui::EndDisabled();

        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + rowWidth);
        ImGui::TextColored(kTextDim, "Per-object masks come from the game's own renderer, so this one stays "
                                     "NVIDIA-only -- an injector has no object list to mask.");
        ImGui::PopTextWrapPos();

        // Models -- DlssNrPreset. NVIDIA ships no letters in the binary; "Model A/B/C" is this
        // fork's best match to the segmented selector in the developer overlay, and matches the
        // three NVIDIA describes publicly. Default (preset index 0) is kept as a fourth button
        // that NVIDIA's panel does not show, because it is a real, distinct state here: dropping
        // it to match the screenshot exactly would make that state unreachable from the UI.
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

        // Frame Generation -- NVIDIA's own DLSS-G (Streamline), driven the same way the old
        // shared menu's "MFG" combo and "Force Dynamic MFG" checkbox did: straight through
        // config->FGDLSSGInterpolationCount / FGDLSSGForceDMFG, which DLSSG_Dx12::Dispatch()
        // reads every frame and hands to sl::DLSSGOptions.numFramesToGenerate. Nothing here
        // touches OptiFG (the Nukem's FSR3-based fallback used when the game has no native
        // frame generation) -- that path is FGOutput::FSRFG/XeFG, not FGOutput::DLSSG, and is
        // deliberately left out of this panel.
        SectionCaption("Frame Generation", rowWidth);

        if (state.activeFgOutput == FGOutput::DLSSG && state.currentFG != nullptr)
        {
            auto* fg = state.currentFG;

            bool fgActive = config->FGEnabled.value_or_default();
            if (NrCheckbox("Frame Generation", &fgActive))
            {
                config->FGEnabled = fgActive;
                state.fgChanged = true;
                anyChanged = true;
            }
            HelpMarker("NVIDIA's own DLSS Frame Generation, via Streamline. Not OptiFG.");

            int maxCount = fg->GetMaxInterpolationCount();
            if (maxCount > 1)
            {
                static const char* multNames[] = { "2X", "3X", "4X", "5X", "6X" };
                int shown = std::min(maxCount, (int) IM_ARRAYSIZE(multNames));
                int current = std::clamp((int) fg->GetInterpolatedFrameCount() - 1, 0, shown - 1);

                bool dmfgForced = config->FGDLSSGForceDMFG.value_or_default();

                ImGui::BeginDisabled(dmfgForced);
                {
                    float spacing = ImGui::GetStyle().ItemSpacing.x;
                    float btnWidth = (rowWidth - spacing * (shown - 1)) / shown;

                    for (int i = 0; i < shown; i++)
                    {
                        if (i > 0)
                            ImGui::SameLine();

                        ImGui::PushID(i);
                        if (ModelButton(multNames[i], current == i, btnWidth))
                        {
                            LOG_DEBUG("DLSSG Interpolation Count set to: {}", i + 1);
                            config->FGDLSSGInterpolationCount = i + 1;
                            anyChanged = true;
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::EndDisabled();
                HelpMarker("Sets Streamline's numFramesToGenerate directly -- how many extra frames"
                           "\nDLSS-G inserts between real ones. 2X inserts one, 3X inserts two, and"
                           "\nso on. Capped by what your GPU and driver report supporting."
                           "\n\nGreyed out while Multi is on below -- the driver picks the count then.");

                if (fg->GetDMFGSupport())
                {
                    if (NrCheckbox("Multi (Dynamic Frame Generation)", &dmfgForced))
                    {
                        config->FGDLSSGForceDMFG = dmfgForced;
                        anyChanged = true;
                    }
                    HelpMarker("Lets NVIDIA's driver vary the multiplier itself, frame to frame, to hold"
                               "\nthe FPS target below -- instead of a fixed 2X/3X/4X.");

                    ImGui::BeginDisabled(!dmfgForced);
                    float fpsTarget = config->FGDLSSGFramerateTargetDMFG.value_or_default();
                    auto rFps = NrSlider("DMFG FPS Target", &fpsTarget, 0.0f, 200.0f, "%.0f", rowWidth);
                    if (rFps.changed)
                        config->FGDLSSGFramerateTargetDMFG = fpsTarget;
                    if (rFps.released)
                        anyChanged = true;
                    ImGui::EndDisabled();
                    HelpMarker("0 auto-detects your display's refresh rate.");
                }
            }
        }
        else
        {
            ImGui::TextColored(kTextDim, "NVIDIA DLSS Frame Generation is not the active output right now.");
        }

        // Everything below is this fork's own instrumentation, with no equivalent in NVIDIA's
        // developer overlay -- kept under its original names.
        SectionCaption("Cost", rowWidth);

        static int pendingScale = -1;
        float scalePercent =
            pendingScale >= 0 ? (float) pendingScale : config->DlssNrWorkingScale.value_or_default() * 100.0f;

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
        ImGui::TextColored(kTextDim, "The model was trained on finished, sRGB-encoded frames. These decide how "
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
