#include "pch.h"

#include "DlssNr.h"
#include "DlssNr_Modes.h"

#include <Config.h>
#include <State.h>
#include <menu/menu_common.h>

#include <imgui/imgui.h>

namespace DlssNr
{

// The "(?)" marker every control carries, matching the rest of the menu.
static void HelpMarker(const char* tip)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");

    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
        ImGui::TextUnformatted(tip);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void RenderMenu(Config* config, float menuResScale)
{

    // DLSS Neural Rendering -----------------------------
    ImGui::Spacing();
    if (auto ch = ScopedCollapsingHeader("DLSS Neural Rendering"); ch.IsHeaderOpen())
    {
        ScopedIndent indent {};
        ImGui::Spacing();

        bool enabled = config->DlssNrEnabled.value_or_default();
        if (ImGui::Checkbox("Enable Neural Rendering", &enabled))
            config->DlssNrEnabled = enabled;

        HelpMarker("Synthesises detail in the upscaler's output, before frame generation sees it."
                       "\n\nNeeds two similarly named files beside OptiScaler, one character apart:"
                       "\n  nvngx_dlssnr.dll       NVIDIA's model (~165 MB) -- you supply it"
                       "\n  nvngx.dll_dlssnr.dll   the forwarder (~13 KB) -- ships in this package"
                       "\nUndocumented and driven directly, so none of this is officially supported.");

        // The toggle can be bound to a key, and nobody would think to look for it under Keybinds
        // unless told. Dimmed, because it is a note rather than a setting.
        ImGui::TextDisabled("Can be toggled with a key -- bind it under Keybinds, \"Neural Rendering\".");

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
                ImGui::TextUnformatted("Waiting for the upscaler to run.");
        }
        else
        {
            // The cost belongs here rather than only in the upscaler's breakdown: that tooltip needs
            // OptiScaler's own upscaler to have run, and with native DLSS passing through there is
            // nothing in it to hang this off.
            const auto ms = DlssNr::LastGpuTime();

            if (ms.has_value())
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "Running - %.2f ms per frame",
                                   ms.value());
            else
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "Running.");

            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("The whole pass: the staging copies and the resolve as well as the"
                                  "\nmodel. Timing only the model would flatter the number."
                                  "\n\nCompare it against the frame time at the bottom of this window to"
                                  "\nsee what it is costing you.");
        }

        ImGui::Spacing();
        ImGui::PushItemWidth(220.0f * menuResScale);

        ImGui::SeparatorText("Placement");

        {
            static const char* modeNames[] = { "Post-process", "Multi-pass" };
            int mode = (int) config->DlssNrMode.value_or_default();
            if (mode < 0 || mode > (int) DlssNr::Mode::MultiPass)
                mode = (int) DlssNr::Mode::PostProcess;

            if (ImGui::Combo("Placement", &mode, modeNames, IM_ARRAYSIZE(modeNames)))
                config->DlssNrMode = (uint32_t) mode;

            HelpMarker("Where the model sits relative to the upscaler."
                           "\n\nPost-process runs it on the finished frame. It is the only one that needs"
                           "\nnothing from the upscaler, so it is also the only one that works when a game"
                           "\nis using its own DLSS and OptiScaler is passing it straight through."
                           "\n\nMulti-pass runs a first pass 1:1 -- denoising if that is Ray Reconstruction,"
                           "\nantialiasing as DLAA if it is Super Resolution -- then the model at render"
                           "\nresolution, then a spatial FSR1 enlarge to display. Dx12 DLSS / Ray"
                           "\nReconstruction only; anything else, or a first-pass pipeline that does not"
                           "\nmatch the live upscaler, falls back to post-process.");

            const auto selected = (DlssNr::Mode) config->DlssNrMode.value_or_default();

            if (DlssNr::UsesTwoFeatures(selected))
            {
                static const char* pipelineNames[] = { "Ray Reconstruction", "Super Resolution" };
                int pipeline = (int) config->DlssNrFeature1Pipeline.value_or_default();

                if (ImGui::Combo("First pass", &pipeline, pipelineNames, IM_ARRAYSIZE(pipelineNames)))
                    config->DlssNrFeature1Pipeline = (uint32_t) pipeline;

                HelpMarker("Which upscaler the first pass is."
                               "\n\nThis states what the game is set up for; it does not switch anything."
                               "\nOptiScaler cannot substitute one for the other -- Ray Reconstruction needs"
                               "\nG-buffer inputs a Super Resolution integration never supplies -- so a"
                               "\nmismatch falls back to post-process rather than half-applying the"
                               "\narrangement, and says so in the log.");
            }
        }

        ImGui::SeparatorText("Cost");

        // The slider is always work / display, so the same number costs the same
        // in both placements. Match-render locks work to the game's native raster
        // and does not overwrite WorkingScale, so unchecking restores it.
        bool atNative = config->DlssNrWorkAtNative.value_or_default();
        if (ImGui::Checkbox("Match render resolution", &atNative))
            config->DlssNrWorkAtNative = atNative;

        HelpMarker("Lock the model to the game's render resolution."
                       "\n\nThe slider then shows that size as a percentage of display and cannot be"
                       "\nmoved. Unchecking puts the last display-relative value back, clamped to"
                       "\nwhatever the current placement allows.");

        unsigned int displayH = 0;
        unsigned int nativeH = 0;

        if (auto* feature = State::Instance().currentFeature; feature != nullptr && !feature->IsFrozen())
        {
            displayH = feature->DisplayHeight();
            nativeH = feature->NRSourceHeight();

            if (nativeH == 0)
                nativeH = feature->RenderHeight();
        }

        const bool haveRatio = displayH != 0 && nativeH != 0;
        int nativePercent = 100;

        if (haveRatio)
            nativePercent = (int) lroundf((float) nativeH / (float) displayH * 100.0f);

        if (nativePercent < 1)
            nativePercent = 1;

        const bool multiPass = DlssNr::ConfiguredMode() == DlssNr::Mode::MultiPass;
        int maxPercent = 100;

        if (multiPass && haveRatio && nativePercent < maxPercent)
            maxPercent = nativePercent;

        int minPercent = 25;

        if (maxPercent < minPercent)
            minPercent = maxPercent;

        // Applied when the handle is let go, not while it is moving. Every distinct
        // value here is a different working size, and a different working size tears
        // down the scratch textures and rebuilds the model.
        static int pendingScale = -1;

        if (atNative)
            pendingScale = -1;

        int scalePercent = 100;

        if (atNative)
            scalePercent = haveRatio ? nativePercent : 100;
        else if (pendingScale >= 0)
            scalePercent = pendingScale;
        else
            scalePercent = (int) lroundf(config->DlssNrWorkingScale.value_or_default() * 100.0f);

        if (scalePercent < minPercent)
            scalePercent = minPercent;

        if (scalePercent > maxPercent)
            scalePercent = maxPercent;

        ImGui::BeginDisabled(atNative);

        if (ImGui::SliderInt("Model resolution", &scalePercent, minPercent, maxPercent, "%d%%"))
            pendingScale = scalePercent;

        if (ImGui::IsItemDeactivatedAfterEdit() && pendingScale >= 0)
        {
            config->DlssNrWorkingScale =
                std::clamp(pendingScale, minPercent, maxPercent) / 100.0f;
            pendingScale = -1;
        }

        ImGui::EndDisabled();

        HelpMarker("What fraction of display resolution the model works at. The same percentage"
                       "\ncosts the same in post-process and multi-pass. Cost falls with the square of"
                       "\nthis, so half resolution is roughly a quarter of the time."
                       "\n\nThe frame is never reduced. Only the model's contribution is computed small"
                       "\nand enlarged, so the picture underneath is untouched whatever this says."
                       "\n\nMulti-pass cannot go above the game's render resolution as a fraction of"
                       "\ndisplay -- there is nothing larger for the model to read."
                       "\n\nWhat it trades: the shading the model adds is broad and survives enlargement;"
                       "\nthe fine structure it synthesises does not, and softens.");

        ImGui::SeparatorText("How much of it lands");

        float transfer = config->DlssNrTransferStrength.value_or_default();
        if (ImGui::SliderFloat("Detail strength", &transfer, 0.0f, 2.0f, "%.2f"))
            config->DlssNrTransferStrength = transfer;

        HelpMarker("How far the frame moves toward the model's picture."
                       "\n\nThe model's answer is not added to the frame -- it is a complete picture of its"
                       "\nown, rescaled so its luminance sits where the original says it should. This"
                       "\nblends between the two, so both ends are real pictures and everything between"
                       "\nthem is one too."
                       "\n\n0 gives back exactly what the upscaler produced. 1 is the model's picture."
                       "\n\nAbove 1 carries on past it in the same direction, which is not something the"
                       "\nmodel asked for -- use it to see what it is doing, then come back down. This"
                       "\nis the control to push if you want more effect: Intensity belongs to the model"
                       "\nand it decides what to do with it.");

        float colour = config->DlssNrColourStrength.value_or_default();
        if (ImGui::SliderFloat("Colour strength", &colour, 0.0f, 1.0f, "%.2f"))
            config->DlssNrColourStrength = colour;

        HelpMarker("Whether the model's colour arrives with its light."
                       "\n\n0 keeps the game's own hue exactly -- every pixel is the original colour with"
                       "\nonly its brightness carrying the model's verdict. Game-accurate colour, with"
                       "\nthe detail. 1 brings the model's colour as well, in its own hue, clamped into"
                       "\nAP1 so nothing unreachable is asked for."
                       "\n\nThis cannot shift hue on its own: it interpolates between two finished"
                       "\npictures rather than adding a colour difference to one, which is what used to"
                       "\nlet a warm subject come back green.");

        ImGui::SeparatorText("Model");

        ImGui::TextUnformatted("Read when the model is built, so a change rebuilds it after a moment.");

        static const char* nrPresetNames[] = { "Default", "Preset 1", "Preset 2", "Preset 3" };
        int preset = (int) config->DlssNrPreset.value_or_default();
        if (ImGui::Combo("Model preset", &preset, nrPresetNames, IM_ARRAYSIZE(nrPresetNames)))
            config->DlssNrPreset = (uint32_t) preset;

        HelpMarker("Default leaves the choice to the model."
                       "\n\nNot the same scale as the super resolution or ray reconstruction presets --"
                       "\nthe same number means something different here.");

        static const char* nrStyleNames[] = { "Default (standard)", "Natural", "Cinematic" };
        int style = (int) config->DlssNrStyle.value_or_default();

        if (style > 2)
            style = 2;

        if (ImGui::Combo("Style", &style, nrStyleNames, IM_ARRAYSIZE(nrStyleNames)))
            config->DlssNrStyle = (uint32_t) style;

        HelpMarker("The model's own processing profiles."
                   "\n\nDefault (standard): the strongest. Boosts local contrast and deepens"
                   "\nlighting, and can oversaturate or look stylised -- most of what reads as"
                   "\n'the model changed my game's look' is this profile."
                   "\n\nNatural: the same detail work with a gentler hand. Keeps skin tones and"
                   "\ntonal balance closer to what the game rendered."
                   "\n\nCinematic: tones down the shine and over-processing for a film-like look."
                   "\n\nRead when the model is built, so a change rebuilds it after a moment. The"
                   "\nnames come from community testing; NVIDIA ships no names in the binaries.");

        float intensity = config->DlssNrIntensity.value_or_default();
        if (ImGui::SliderFloat("Intensity", &intensity, 0.0f, 2.0f, "%.2f"))
            config->DlssNrIntensity = intensity;

        HelpMarker("The model's own strength control, applied inside it. Distinct from detail"
                       "\nstrength above, which scales the result afterwards.");

        float localStructure = config->DlssNrLocalStructure.value_or_default();
        if (ImGui::SliderFloat("Local structure", &localStructure, 0.0f, 2.0f, "%.2f"))
            config->DlssNrLocalStructure = localStructure;

        float localTone = config->DlssNrLocalTone.value_or_default();
        if (ImGui::SliderFloat("Local tone", &localTone, 0.0f, 2.0f, "%.2f"))
            config->DlssNrLocalTone = localTone;


        float skin = config->DlssNrSkinStructure.value_or_default();
        if (ImGui::SliderFloat("Skin structure", &skin, -1.0f, 2.0f, "%.2f"))
            config->DlssNrSkinStructure = skin;

        HelpMarker("-1 means follow local structure, and is the model's own default -- it is not a"
                       "\nstrength of zero. 0 and above set skin independently of the rest of the frame.");

        bool autoMask = config->DlssNrAutoMask.value_or_default();
        if (ImGui::Checkbox("Auto skin mask", &autoMask))
            config->DlssNrAutoMask = autoMask;

        HelpMarker("Lets the model find skin itself rather than treating the frame uniformly.");

        ImGui::SeparatorText("Colour");

        ImGui::TextDisabled("The model was trained on finished, sRGB-encoded frames. The upscaler's\n"
                            "output is not one: it is linear and open-ended. These decide how it is\n"
                            "mapped into something the model recognises. A frame the game reports as\n"
                            "already tone-mapped is passed over untouched and none of this applies.");

        {
        float wpScale = config->DlssNrWhitePointScale.value_or_default();
        if (ImGui::SliderFloat("Paper white", &wpScale, 0.25f, 4.0f, "%.2fx"))
            config->DlssNrWhitePointScale = wpScale;

        HelpMarker("What the frame is divided by before the model sees it. There is no other white"
                       "\npoint; this is the whole of it."
                       "\n\nThe model was trained on finished frames where white sits at 1. The"
                       "\nupscaler's output is linear and open-ended, so something has to say where"
                       "\nwhite is, and 1.0 is right for most games."
                       "\n\nAbove 1 the picture handed over is darker, so highlights sit lower on the"
                       "\ncurve and the model treats them as less extreme; below 1, the opposite. If a"
                       "\ngame looks washed out or flat, this is the first thing to move."
                       "\n\nThis was once a multiplier on a measured white point. The measurement is"
                       "\ngone: it read scene brightness rather than where white belongs, handed the"
                       "\nmodel a picture three times too dark, and left the highlight path nothing to"
                       "\ngive back."
                       "\n\nAt strength zero the frame is still bit-identical whatever this says.");

        float maxRatio = config->DlssNrMaxRatio.value_or_default();
        if (ImGui::SliderFloat("Highlight guard", &maxRatio, 1.0f, 8.0f, "%.1fx"))
            config->DlssNrMaxRatio = maxRatio;

        HelpMarker("The most the pass may brighten any pixel, as a multiple of what it already"
                       "\nwas. Darkening is not capped by this -- only growth is."
                       "\n\nLights are where the model has least to say and where rescaling its answer"
                       "\ninto the frame does the most damage: an early version turned every strip light"
                       "\nin the scene into a string of coloured cells. 2x leaves detail intact while"
                       "\nmaking that failure impossible. Raise it only if bright areas look clipped.");

        }

        ImGui::SeparatorText("Inspect");

        if (DlssNr::CaptureInProgress())
        {
            ImGui::TextDisabled("Capturing...");
        }
        else if (ImGui::Button("Capture 8 frames"))
        {
            DlssNr::RequestCapture(8);
        }

        HelpMarker("Writes eight consecutive frames twice: as the upscaler produced them, and again"
                       "\nonce the model's edit was applied."
                       "\n\nSame frames, same run, one variable -- which is what comparing two video"
                       "\ncaptures can never be, since they have different camera paths and a codec in"
                       "\nbetween that discards exactly the fine temporal detail in question."
                       "\n\nRaw, into a dlssnr-capture folder beside OptiScaler. Bounded to eight frames,"
                       "\nand each run overwrites the last.");

        static const char* compareNames[] = { "Off", "Side by side", "Wipe" };
        int compare = (int) config->DlssNrCompare.value_or_default();
        if (ImGui::Combo("Compare", &compare, compareNames, IM_ARRAYSIZE(compareNames)))
            config->DlssNrCompare = (uint32_t) compare;

        HelpMarker("Shows the pass against itself, so the two can be seen at once rather than"
                       "\ntoggled and remembered."
                       "\n\nSide by side puts the whole frame in each half, untouched on the left and"
                       "\nedited on the right. Both halves are squeezed horizontally to fit, so it is"
                       "\nfor looking at rather than playing in."
                       "\n\nWipe cuts a single frame at the split and resamples nothing, so the picture"
                       "\nis the right shape and can be played normally. Drag the split below; it is a"
                       "\nstored setting and stays put once the menu is closed."
                       "\n\nNeither needs the menu open to keep working. A hairline marks the join.");

        if (compare != 0)
        {
            bool swap = config->DlssNrCompareSwap.value_or_default();
            if (ImGui::Checkbox("Swap sides", &swap))
                config->DlssNrCompareSwap = swap;

            HelpMarker("Puts the edited frame on the other side."
                           "\n\nWorth doing once you have decided which you prefer: the eye is not"
                           "\neven-handed about left and right, and a difference can read as an"
                           "\nimprovement purely from where it sits. If the same side still wins after"
                           "\nswapping, it is the pass you are seeing and not the placement.");
        }

        if (compare == 1)
        {
            float zoom = config->DlssNrCompareZoom.value_or_default();
            if (ImGui::SliderFloat("Zoom", &zoom, 1.0f, 2.0f, "%.2f"))
                config->DlssNrCompareZoom = std::clamp(zoom, 1.0f, 2.0f);

            HelpMarker("How much of the frame each half shows."
                           "\n\nA half is half as wide as the frame and just as tall, so the frame"
                           "\ncannot fill it and keep its shape."
                           "\n\nAt 1 the whole frame is there at its right proportions, with bars above"
                           "\nand below. At 2 the half is filled and the sides are cropped away"
                           "\ninstead. Anything between trades one for the other.");
        }

        if (compare == 2)
        {
            float split = config->DlssNrCompareSplit.value_or_default();
            if (ImGui::SliderFloat("Split", &split, 0.0f, 1.0f, "%.2f"))
                config->DlssNrCompareSplit = std::clamp(split, 0.0f, 1.0f);

            HelpMarker("Where the wipe cuts. Left of it is the frame as the upscaler produced it,"
                           "\nright of it is the frame the model edited.");
        }

        static const char* debugNames[] = { "Off", "Proxy (what the model sees)", "Model output (raw)",
                                            "Difference (amplified)" };
        int debugView = (int) config->DlssNrDebugView.value_or_default();
        if (ImGui::Combo("Debug view", &debugView, debugNames, IM_ARRAYSIZE(debugNames)))
            config->DlssNrDebugView = (uint32_t) debugView;

        HelpMarker("Proxy is the picture handed to the model -- if that looks wrong, the white point"
                       "\nis wrong and nothing downstream can be judged."
                       "\n\nDifference shows what the model actually changed, amplified twenty times and"
                       "\ncentred on grey. A flat grey frame there means it is doing nothing.");

        ImGui::PopItemWidth();
    }
}

} // namespace DlssNr

