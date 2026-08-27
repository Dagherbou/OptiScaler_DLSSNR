"""Wires the inject-point choice into the config, the present hook and the menu."""

import io

ROOT = "C:/Games_Temp/OptiScaler/OptiScaler/"

# --- config ---------------------------------------------------------------------------------------

path = ROOT + "Config.h"
text = io.open(path, encoding="utf-8").read()
old = "    CustomOptional<bool> DlssNrEnabled { false };"
new = """    CustomOptional<bool> DlssNrEnabled { false };
    // 0 before frame generation, 1 on the finished frame at present. Defaults to the finished frame:
    // that is the picture the model was trained on, so it is the better image, and the cost is the
    // honest trade rather than something to hide behind a default.
    CustomOptional<uint32_t> DlssNrInjectPoint { 1 };"""
assert old in text
text = text.replace(old, new, 1)
io.open(path, "w", encoding="utf-8").write(text)

path = ROOT + "Config.cpp"
text = io.open(path, encoding="utf-8").read()
old = '            DlssNrEnabled.set_from_config(readBool("DlssNr", "Enabled"));'
new = old + '\n            DlssNrInjectPoint.set_from_config(readUInt("DlssNr", "InjectPoint"));'
assert old in text
text = text.replace(old, new, 1)

old = '    ini.SetValue("DlssNr", "Enabled", GetBoolValue(Instance()->DlssNrEnabled.value_for_config()).c_str());'
new = old + '\n    ini.SetValue("DlssNr", "InjectPoint", GetIntValue(Instance()->DlssNrInjectPoint.value_for_config()).c_str());'
assert old in text
text = text.replace(old, new, 1)
io.open(path, "w", encoding="utf-8").write(text)
print("config patched")

# --- present hook ----------------------------------------------------------------------------------

path = ROOT + "menu/menu_overlay_dx.cpp"
text = io.open(path, encoding="utf-8").read()

old = """        // If everything is ready render the frame
        if (ImGui::GetCurrentContext() && g_mainRenderTargetResource[0])
        {"""
new = """        // Neural Rendering over the finished frame, before the overlay is drawn on top of it -- the
        // model should be enhancing the game, not the menu. It records and submits its own list, since
        // the overlay's only runs when the menu is open.
        {
            UINT nrBackBufferIdx = pSwapChain->GetCurrentBackBufferIndex();
            DlssNr::EvaluateAtPresent((ID3D12CommandQueue*) currentSCCommandQueue,
                                      g_mainRenderTargetResource[nrBackBufferIdx], nrBackBufferIdx);
        }

        // If everything is ready render the frame
        if (ImGui::GetCurrentContext() && g_mainRenderTargetResource[0])
        {"""
assert old in text
text = text.replace(old, new, 1)

inc = '#include "menu_overlay_dx.h"'
assert inc in text
text = text.replace(inc, inc + '\n\n#include <dlssnr/DlssNr_Dx12.h>', 1)
io.open(path, "w", encoding="utf-8").write(text)
print("present hook patched")

# --- menu ---------------------------------------------------------------------------------------------

NL = "\\n"
path = ROOT + "menu/menu_common.cpp"
text = io.open(path, encoding="utf-8").read()

old = '''        ImGui::Spacing();
        ImGui::PushItemWidth(220.0f * ctx.menuResScale);

        ImGui::SeparatorText("How much of it lands");'''
new = ('''        ImGui::Spacing();
        ImGui::PushItemWidth(220.0f * ctx.menuResScale);

        ImGui::SeparatorText("Where it runs");

        static const char* injectNames[] = { "Before frame generation", "Finished frame (better image)" };
        int injectPoint = (int) config->DlssNrInjectPoint.value_or_default();
        if (ImGui::Combo("Inject point", &injectPoint, injectNames, IM_ARRAYSIZE(injectNames)))
            config->DlssNrInjectPoint = (uint32_t) injectPoint;

        ShowHelpMarker("Finished frame: the model sees the picture after the game's own tonemapper,"
                       "@which is exactly what it was trained on, and its answer is used as it comes."
                       "@Costs a run per presented frame, so roughly double with frame generation, and"
                       "@each generated frame is enhanced on its own."
                       "@@Before frame generation: one run per rendered frame and generated frames"
                       "@inherit the result, but the tonemapper has not run yet, so the model works on"
                       "@an approximation of it and the answer has to be applied carefully. Cheaper,"
                       "@and less detail."
                       "@@Takes effect on restart: only one model may exist at a time, and swapping it"
                       "@mid-session is not worth the crash it caused every time it was tried.");

        ImGui::SeparatorText("How much of it lands");''').replace("@", NL)
assert old in text
text = text.replace(old, new, 1)

# The colour section only means anything for the pre-frame-generation path.
old = '''        ImGui::SeparatorText("Colour");'''
new = '''        ImGui::SeparatorText("Colour");

        if (config->DlssNrInjectPoint.value_or_default() == DlssNr::INJECT_PRESENT)
            ImGui::TextUnformatted("Not used: the finished frame needs no conversion.");
        else'''
assert old in text
text = text.replace(old, new, 1)

# That `else` has to govern the whole block, so brace it.
old = '''        else
        bool autoWhite = config->DlssNrAutoWhitePoint.value_or_default();'''
new = '''        else
        {
        bool autoWhite = config->DlssNrAutoWhitePoint.value_or_default();'''
assert old in text
text = text.replace(old, new, 1)

old = '''        ImGui::SeparatorText("Inspect");'''
new = '''        }

        ImGui::SeparatorText("Inspect");'''
assert old in text
text = text.replace(old, new, 1)

io.open(path, "w", encoding="utf-8").write(text)
print("menu patched")
