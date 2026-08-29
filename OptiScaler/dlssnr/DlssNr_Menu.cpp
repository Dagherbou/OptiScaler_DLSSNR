#include "pch.h"

#include "DlssNr.h"

#if OPTI_DLSSNR

#include <Config.h>
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

        ImGui::SeparatorText("Where it runs");

        static const char* injectNames[] = { "Finished frame", "Finished look, before UI (FG games)" };
        // Stored values: 1 = finished frame, 2 = hudless; a stored 0 (the retired pre-tonemapper
        // point) reads as the finished frame.
        int injectIndex = config->DlssNrInjectPoint.value_or_default() == DlssNr::INJECT_HUDLESS ? 1 : 0;
        if (ImGui::Combo("Inject point", &injectIndex, injectNames, IM_ARRAYSIZE(injectNames)))
            config->DlssNrInjectPoint = injectIndex == 1 ? DlssNr::INJECT_HUDLESS : DlssNr::INJECT_PRESENT;

        HelpMarker("Finished frame: the model sees the picture after the game's own tonemapper --"
                   "\nexactly what it was trained on -- and its answer is used as it comes. The"
                   "\ninterface is part of that picture; HUD detection below keeps the model off it."
                   "\n\nFinished look, before UI: the same picture before the interface is drawn,"
                   "\nwhich frame generation titles hand Streamline every frame. The model never"
                   "\ntouches the HUD, and generated frames inherit the result. Does nothing in"
                   "\ngames that do not tag that buffer; the log says when it engages."
                   "\n\nBoth apply live. The old pre-tonemapper point is retired: the finished"
                   "\nframe simply looks better, and the split covers the pre-tonemapper case.");

        bool split = config->DlssNrSplitPipeline.value_or_default();
        if (ImGui::Checkbox("Split pipeline: RR 1:1 + NR + internal SR", &split))
            config->DlssNrSplitPipeline = split;

        if (split)
        {
            const char* splitStatus = DlssNr::SplitStatus();

            if (splitStatus[0] == 0)
                ImGui::TextDisabled("Waiting for a Ray Reconstruction evaluate.");
            else if (splitStatus[0] == 'r' && splitStatus[1] == 'u')
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "%s", splitStatus);
            else if (splitStatus[0] == 'f')
            {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "%s", splitStatus);
                ImGui::SameLine();

                if (ImGui::SmallButton("Retry##split"))
                    DlssNr::RetryAfterFailure();
            }
            else
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f), "%s", splitStatus);
        }

        HelpMarker("Ray Reconstruction runs 1:1 as a pure denoiser, the model runs at render"
                       "\nresolution, and one enlargement at the end is done by an internal DLSS Super"
                       "\nResolution feature -- a temporal upscaler with the full G-buffer, not a"
                       "\nstretch. Everything stays linear HDR, so both features keep the game's own"
                       "\nflags."
                       "\n\nDenoise gets cheaper, the model runs on fewer pixels, and its detail rides"
                       "\nthrough the upscaler's accumulation. The published measurements at a 66%"
                       "\nrender scale: about 4.5 + 6 + 3.5 ms against roughly 17 conventionally."
                       "\n\nRay Reconstruction titles only, and needs a render scale below native (Quality"
                       "\nor lower -- at DLAA there is nothing to enlarge). Applies live, both ways: the"
                       "\nfeature is re-created in place, which costs a brief hitch. Falls back to the"
                       "\nconventional path, with a line in the log, if any stage refuses.");

        if (split)
        {
            // Output Scaling's Enable is the supersampling intent; the split absorbs it while running.
            const float ratio = config->OutputScalingMultiplier.value_or_default();
            const bool osIntent = config->OutputScalingEnabled.value_for_config().value_or(false);

            if (osIntent && ratio > 1.05f)
                ImGui::TextDisabled("Output Scaling absorbed: the split supersamples x%.2f itself.", ratio);
            else if (osIntent)
                ImGui::TextDisabled("Output Scaling absorbed, but its Ratio is 1.0 -- nothing to supersample.");
            else
                ImGui::TextDisabled("No supersampling: enable Output Scaling (and its Ratio) to add it.");

            bool includeRR = config->DlssNrSplitIncludeRR.value_or_default();
            if (ImGui::Checkbox("Include Ray Reconstruction in the supersample", &includeRR))
                config->DlssNrSplitIncludeRR = includeRR;

            HelpMarker("Off: RR denoises 1:1 at render size and the internal SR renders the"
                           "\nsupersampled image -- cheapest, since the expensive models stay small."
                           "\n\nOn: RR itself upscales to the supersampled size and the model works on"
                           "\nthat image, its cost governed by the Model resolution dropdown. The"
                           "\nconventional Output Scaling look with the model in the chain -- and RR's"
                           "\ncost rises with the square of the ratio, which is the price."
                           "\n\nApplies live; the feature is re-created in place.");

            if (includeRR)
            {
                const float rrRatioSet = config->DlssNrSplitIncludeRRRatio.value_or_default();
                const bool followsOs = rrRatioSet <= 1.05f;
                float rrRatioShown = followsOs ? ratio : rrRatioSet;

                if (ImGui::SliderFloat("Include-RR ratio", &rrRatioShown, 1.0f, 3.0f,
                                       followsOs ? "%.2f (following OS Ratio)" : "%.2f"))
                    config->DlssNrSplitIncludeRRRatio = rrRatioShown;

                HelpMarker("The ratio RR itself upscales to when included in the supersample. By"
                               "\ndefault it follows Output Scaling's Ratio -- but RR's cost rises with"
                               "\nthe square of this, and most of the reconstruction sharpness arrives"
                               "\nwell below the full ratio. 1.25 buys most of the sharpness for about"
                               "\n1.5x RR's normal cost instead of 4x at ratio 2."
                               "\n\nApplies live; the feature is re-created in place.");

                if (!followsOs)
                {
                    if (ImGui::SmallButton("Follow OS Ratio"))
                        config->DlssNrSplitIncludeRRRatio = 0.0f;
                }
            }

            {
                static const int srPresetValues[] = { 0, 5, 6, 10, 11 };
                static const char* srPresetNames =
                    "Driver default\0Preset E (classic, stable)\0Preset F (classic)\0"
                    "Preset J (transformer)\0Preset K (transformer, latest)\0";
                const int srPresetNow = (int) config->DlssNrSplitSrPreset.value_or_default();
                int srPresetIndex = 0;

                for (int i = 0; i < 5; ++i)
                {
                    if (srPresetValues[i] == srPresetNow)
                        srPresetIndex = i;
                }

                if (ImGui::Combo("Enlargement preset", &srPresetIndex, srPresetNames))
                    config->DlssNrSplitSrPreset = (uint32_t) srPresetValues[srPresetIndex];

                HelpMarker("The split's chain: Ray Reconstruction denoises 1:1 at render size, the"
                               "\nmodel draws its detail there, and an internal Super Resolution feature"
                               "\nenlarges that finished image to the output. This preset picks the SR"
                               "\nmodel for that last step -- everything the model drew passes through"
                               "\nit, so it decides how sharp that detail lands."
                               "\n\nSR presets are their own scale: E here is unrelated to RR's presets"
                               "\nor the model's. E is the proven classic; J and K (transformer) are"
                               "\nstrongest at extracting detail from an already-resolved image."
                               "\n\nApplies live; the enlargement is re-created in place. If the global"
                               "\nRender Presets Override is on, it wins over this.");

                float srSharp = config->DlssNrSplitSrSharpness.value_or_default();

                if (ImGui::SliderFloat("Enlargement sharpening", &srSharp, 0.0f, 1.0f, "%.2f"))
                    config->DlssNrSplitSrSharpness = srSharp;

                HelpMarker("Sharpening applied by the enlargement (via RCAS -- needs RCAS enabled"
                               "\nabove). 0 is off, and the default: the supersample downscale already"
                               "\nrestores some crispness on its own.");
            }
        }

        ImGui::SeparatorText("Cost");

        static const char* scaleNames[] = { "Full resolution", "75%", "50%", "33%" };
        static const float scaleValues[] = { 1.0f, 0.75f, 0.5f, 0.3333f };
        const float currentScale = config->DlssNrWorkingScale.value_or_default();
        int scaleIndex = 0;
        for (int i = 0; i < IM_ARRAYSIZE(scaleValues); ++i)
        {
            if (currentScale <= scaleValues[i] + 0.01f)
                scaleIndex = i;
        }
        if (ImGui::Combo("Model resolution", &scaleIndex, scaleNames, IM_ARRAYSIZE(scaleNames)))
            config->DlssNrWorkingScale = scaleValues[scaleIndex];

        HelpMarker("What fraction of the frame the model works at. Cost falls with the square of"
                       "\nthis, so half resolution is roughly a quarter of the time."
                       "\n\nThe frame is never reduced. Only the model's contribution is computed small"
                       "\nand enlarged, so the picture underneath is untouched whatever this says."
                       "\n\nWhat it trades: the shading the model adds is broad and survives enlargement;"
                       "\nthe fine structure it synthesises does not, and softens. Worth having when the"
                       "\npass costs more than you want to pay for the detail it returns."
                       "\n\nWorks at both inject points. Before frame generation this is the cheap way to"
                       "\nrun the model alongside Ray Reconstruction: the delta composite keeps the frame"
                       "\nitself at native detail.");

        ImGui::SeparatorText("How much of it lands");

        float transfer = config->DlssNrTransferStrength.value_or_default();
        if (ImGui::SliderFloat("Detail strength", &transfer, 0.0f, 4.0f, "%.2f"))
            config->DlssNrTransferStrength = transfer;

        HelpMarker("How much of the model's luminance edit reaches the frame."
                       "\n\n0 gives back exactly what the upscaler produced -- the encode and decode are"
                       "\nexact inverses, so this is a true bypass, not an approximation of one."
                       "\n\nAbove 1 exaggerates the edit. If nothing here seems to do anything, push this"
                       "\nto 4 and watch: that answers whether the model is contributing at all.");

        float editStability = config->DlssNrEditStability.value_or_default();
        if (ImGui::SliderFloat("Temporal stability", &editStability, 0.0f, 0.95f, "%.2f"))
            config->DlssNrEditStability = editStability;

        HelpMarker("Steadies the model's re-lighting of the scene -- the low-frequency part of its"
                       "\nedit, which otherwise pumps visibly. Carried by the game's own motion vectors,"
                       "\nand it stands down at motion boundaries, so nothing trails."
                       "\n\nDetail is deliberately NOT accumulated. Measured twice -- a hand-made filter"
                       "\nand a trained DLAA pass -- history on the detail band trades shimmer for"
                       "\nghosts at best: the model re-decides detail with the framing, so old answers"
                       "\ndo not belong to new frames. Detail stability comes from the split pipeline,"
                       "\nwhich routes the pass through a real upscaler accumulator."
                       "\n\n0 is off and bit-identical; anything above enables the hold.");


        float colour = config->DlssNrColourStrength.value_or_default();
        if (ImGui::SliderFloat("Colour strength", &colour, 0.0f, 4.0f, "%.2f"))
            config->DlssNrColourStrength = colour;

        HelpMarker("The same, for the colour part of the edit, which is separated out because it is"
                       "\nusually the part you do not want. Detail synthesis is a luminance change; a"
                       "\ncolour shift is mostly the model drifting.");

        ImGui::SeparatorText("Model");

        ImGui::TextUnformatted("Read when the model is built, so a change rebuilds it after a moment.");

        static const char* nrPresetNames[] = { "Default", "Preset 1", "Preset 2", "Preset 3" };
        int preset = (int) config->DlssNrPreset.value_or_default();
        if (ImGui::Combo("Model preset", &preset, nrPresetNames, IM_ARRAYSIZE(nrPresetNames)))
            config->DlssNrPreset = (uint32_t) preset;

        HelpMarker("Default leaves the choice to the model."
                       "\n\nNot the same scale as the super resolution or ray reconstruction presets --"
                       "\nthe same number means something different here.");

        static const char* nrStyleNames[] = { "Natural (default)", "Cinematic" };
        int style = (int) config->DlssNrStyle.value_or_default();
        if (ImGui::Combo("Style", &style, nrStyleNames, IM_ARRAYSIZE(nrStyleNames)))
            config->DlssNrStyle = (uint32_t) style;

        HelpMarker("The model has two, and only two. A slider offering more was a guess, and it"
                       "\nmatched the observation that most of its positions did nothing.");

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

        float globalTone = config->DlssNrGlobalTone.value_or_default();
        if (ImGui::SliderFloat("Global tone", &globalTone, 0.0f, 2.0f, "%.2f"))
            config->DlssNrGlobalTone = globalTone;

        HelpMarker("The model's overall re-exposure of the scene, as opposed to the local re-toning"
                   "\nabove. A parameter NVIDIA's own integration sets that was found in their"
                   "\nStreamline plugin; 1 is the model's default behaviour, 0 keeps the game's"
                   "\nglobal tone and lets the model add detail only. Read when the model is built.");

        float skin = config->DlssNrSkinStructure.value_or_default();
        if (ImGui::SliderFloat("Skin structure", &skin, -1.0f, 2.0f, "%.2f"))
            config->DlssNrSkinStructure = skin;

        HelpMarker("-1 means follow local structure, and is the model's own default -- it is not a"
                       "\nstrength of zero. 0 and above set skin independently of the rest of the frame.");

        bool autoMask = config->DlssNrAutoMask.value_or_default();
        if (ImGui::Checkbox("Auto skin mask", &autoMask))
            config->DlssNrAutoMask = autoMask;

        HelpMarker("Lets the model find skin itself rather than treating the frame uniformly.");

        bool uiCorrection = config->DlssNrUiCorrection.value_or_default();
        if (ImGui::Checkbox("UI correction", &uiCorrection))
            config->DlssNrUiCorrection = uiCorrection;


        HelpMarker("Keeps the model off the interface."
                       "\n\nIt matters most on the finished frame, where the HUD is part of the picture"
                       "\nthe model is handed and it will otherwise synthesise detail into text and"
                       "\nicons. Before frame generation the UI has not been drawn yet, so there is"
                       "\nnothing there to protect.");

        float hudDetect = config->DlssNrHudDetect.value_or_default();
        if (ImGui::SliderFloat("HUD detection", &hudDetect, 0.0f, 1.0f, "%.2f"))
            config->DlssNrHudDetect = hudDetect;

        HelpMarker("Keeps the model off the interface at the finished frame, by finding it. Where"
                   "\nthe game tags its UI layer through Streamline (frame generation titles) the"
                   "\nmask is exact. Elsewhere the interface is what stays put while the world"
                   "\nmoves: a pixel unchanged from last frame under a motion vector that says it"
                   "\nshould have changed. The estimate rises fast and fades slowly, so it holds"
                   "\nthrough pauses; standing perfectly still long enough lets it lapse."
                   "\n\nThe strength is how completely the model is kept off what was found."
                   "\n0 is off and bit-identical. The hudless inject point needs none of this.");

        ImGui::SeparatorText("Colour");

        if (config->DlssNrInjectPoint.value_or_default() == DlssNr::INJECT_PRESENT)
            ImGui::TextDisabled("SDR finished frames go over unconverted; scRGB HDR frames are\n"
                                "encoded with their own measured white point. Everything below\n"
                                "still applies.");

        {
        bool autoWhite = config->DlssNrAutoWhitePoint.value_or_default();
        if (ImGui::Checkbox("Automatic white point", &autoWhite))
            config->DlssNrAutoWhitePoint = autoWhite;

        HelpMarker("The upscaler's output is linear HDR with an open-ended range; the model was"
                       "\ntrained on finished, sRGB-encoded frames. The white point is what maps one to"
                       "\nthe other, and it belongs to the game's exposure."
                       "\n\nMeasured frame means in Cyberpunk alone have ranged from 0.065 in gameplay to"
                       "\n185 in another scene, so no fixed number can serve. This measures the frame and"
                       "\nfollows it."
                       "\n\nChanging it cannot shift the finished image: the encode and resolve use the"
                       "\nsame value and are exact inverses. It only moves what the model is shown.");

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

            HelpMarker("Too low and highlights flatten out before the model sees them; too high and"
                           "\nit works on a dark, featureless frame.");
        }

        {
            static const char* curveNames[] = { "Reinhard (white point above)", "Match the game's tonemapper" };
            int curve = (int) config->DlssNrProxyCurve.value_or_default();
            if (curve > 1)
                curve = 1;
            if (ImGui::Combo("Proxy curve", &curve, curveNames, IM_ARRAYSIZE(curveNames)))
                config->DlssNrProxyCurve = (uint32_t) curve;

            HelpMarker("How the linear frame is compressed for the model, in the split and the DX11"
                       "\nbridge -- the arrangements where the model sees the frame before the game's"
                       "\ntonemapper. Reinhard is a generic guess. Match learns the game's own curve"
                       "\nby comparing the linear frame with the finished one, twice a second, so the"
                       "\nmodel is shown the game's actual contrast and shadow depth -- the statistics"
                       "\nit was trained on -- and the edit is sized for the curve it will pass through."
                       "\nEncode and resolve stay exact inverses either way."
                       "\n\nNeeds an SDR display output to learn from; falls back to Reinhard until"
                       "\nthe first measurement lands. Does nothing at the finished-frame and hudless"
                       "\ninject points, which are already finished pictures.");

            const char* curveStatus = DlssNr::ProxyCurveStatus();

            if (curveStatus[0] != 0)
                ImGui::TextDisabled("%s", curveStatus);
        }

        float wpScale = config->DlssNrWhitePointScale.value_or_default();
        if (ImGui::SliderFloat("White point scale", &wpScale, 0.5f, 2.0f, "%.2fx"))
            config->DlssNrWhitePointScale = wpScale;

        HelpMarker("Multiplies the white point above -- automatic or manual -- before the model"
                       "\nsees the frame. Above 1, highlights sit lower on the curve and the model"
                       "\ntreats them as less extreme, so its tone edits back off; below 1, the"
                       "\nopposite. The encode and resolve stay exact inverses, so this only moves"
                       "\nwhat the model is shown, never the untouched image."
                       "\n\nIn SDR this becomes a proxy exposure: away from 1.00x the frame is run"
                       "\nthrough the curve just for the model's eyes -- it judges tone on the"
                       "\nre-exposed picture and the resolve inverts exactly, so at strength zero"
                       "\nnothing changes. Lower makes it treat your lights as more extreme (and"
                       "\nleave them alone more); higher, the opposite. At exactly 1.00x SDR frames"
                       "\ngo over untouched, as before.");

        float protectHl = config->DlssNrProtectHighlights.value_or_default();
        if (ImGui::SliderFloat("Highlight restore", &protectHl, 0.0f, 2.0f, "%.2f"))
            config->DlssNrProtectHighlights = protectHl;

        HelpMarker("How the highlights look. The model's trained instinct is to calm bright things"
                       "\n-- not only the peak of a lamp but the whole glow around it -- and that reads"
                       "\nas muted, in SDR as much as HDR. This pulls back the darkening of bright"
                       "\nregions, scaled by how bright they are; colour, brightening and structure"
                       "\ndetail pass untouched, so nothing else changes."
                       "\n\n0 is off and bit-identical; 1 removes all darkening from the brightest"
                       "\nregions. Above 1 it flips into a boost: what the model tried to dim gets"
                       "\nbrightened past the original instead -- extra punch, bounded by the"
                       "\nHighlight guard clamp. The blunter lever is Local tone below: at 0 the"
                       "\nmodel stops re-toning entirely, everywhere, dark corners included.");

        float shadowRestore = config->DlssNrShadowRestore.value_or_default();
        if (ImGui::SliderFloat("Shadow restore", &shadowRestore, 0.0f, 1.0f, "%.2f"))
            config->DlssNrShadowRestore = shadowRestore;

        HelpMarker("The mirror of Highlight restore, and the other half of the washed-out look:"
                       "\nthe model lifts dark regions toward its trained idea of a well-exposed"
                       "\npicture, and the scene loses its darkness -- an alley in shadow turns grey."
                       "\nThis pulls back the brightening of dark regions, scaled by how dark they"
                       "\nare; detail and colour pass untouched."
                       "\n\n0 is off and bit-identical; 1 removes all lift from the darkest regions."
                       "\nRun both restores together to keep the scene's full contrast.");

        float maxRatio = config->DlssNrMaxRatio.value_or_default();
        if (ImGui::SliderFloat("Highlight guard", &maxRatio, 1.0f, 8.0f, "%.1fx"))
            config->DlssNrMaxRatio = maxRatio;

        HelpMarker("The most the pass may brighten or darken any pixel."
                       "\n\nLights are where the model has least to say and where scaling its answer back"
                       "\ninto the frame does the most damage -- an early version turned every strip light"
                       "\nin the scene into a string of coloured cells. 1x disables the pass entirely;"
                       "\n2x leaves detail intact while making that failure impossible.");

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

#endif // OPTI_DLSSNR
