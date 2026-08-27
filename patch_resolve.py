"""Adds the kept copy of the frame that the new resolve needs, and updates both dispatches.

The resolve no longer reconstructs the frame by inverting the tone curve -- it adds the model's edit to
the frame as it was. That means it needs the frame as it was, so the encode keeps a copy while it is
already reading it, rather than paying for a separate copy.
"""

import io

PATH = "C:/Games_Temp/OptiScaler/OptiScaler/dlssnr/DlssNr_Dx12.cpp"
text = io.open(PATH, encoding="utf-8").read()

# --- the kept copy on the state struct ------------------------------------------------------------

old = """    // Only created when a game hands over typeless guides, which most do and Cyberpunk does not."""
new = """    // The frame as the upscaler wrote it. The resolve adds the model's edit to this rather than
    // reconstructing it by inverting the tone curve, which is what turned every light in the frame into
    // a string of coloured cells.
    ID3D12Resource* hdrCopy = nullptr;

    // Only created when a game hands over typeless guides, which most do and Cyberpunk does not."""
assert old in text
text = text.replace(old, new, 1)

# --- allocate and free it alongside the others -----------------------------------------------------

old = """        if (g_nr.colorCopy != nullptr)
        {
            g_nr.colorCopy->Release();
            g_nr.colorCopy = nullptr;
        }
    }

    if (g_nr.output == nullptr)
    {
        g_nr.output = CreateScratch(device, desc.Format, width, height);
        g_nr.colorCopy = CreateScratch(device, desc.Format, width, height);
    }"""
new = """        if (g_nr.colorCopy != nullptr)
        {
            g_nr.colorCopy->Release();
            g_nr.colorCopy = nullptr;
        }

        if (g_nr.hdrCopy != nullptr)
        {
            g_nr.hdrCopy->Release();
            g_nr.hdrCopy = nullptr;
        }
    }

    if (g_nr.output == nullptr)
    {
        g_nr.output = CreateScratch(device, desc.Format, width, height);
        g_nr.colorCopy = CreateScratch(device, desc.Format, width, height);
        g_nr.hdrCopy = CreateScratch(device, desc.Format, width, height);
    }"""
assert old in text
text = text.replace(old, new, 1)

old = """    if (g_nr.feature == nullptr && g_nr.output != nullptr && g_nr.colorCopy != nullptr)"""
new = """    if (g_nr.feature == nullptr && g_nr.output != nullptr && g_nr.colorCopy != nullptr &&
        g_nr.hdrCopy != nullptr)"""
assert old in text
text = text.replace(old, new, 1)

old = """    if (g_nr.depthClone != nullptr)
    {
        g_nr.depthClone->Release();
        g_nr.depthClone = nullptr;
    }"""
new = """    if (g_nr.hdrCopy != nullptr)
    {
        g_nr.hdrCopy->Release();
        g_nr.hdrCopy = nullptr;
    }

    if (g_nr.depthClone != nullptr)
    {
        g_nr.depthClone->Release();
        g_nr.depthClone = nullptr;
    }"""
assert old in text
text = text.replace(old, new, 1)

# --- encode: also keep the untouched frame ---------------------------------------------------------

old = """    g_codec.dispatch(cmdList, encodeParams, target, nullptr, g_nr.colorCopy);"""
new = """    g_codec.dispatch(cmdList, encodeParams, target, nullptr, nullptr, g_nr.colorCopy, g_nr.hdrCopy);"""
assert old in text
text = text.replace(old, new, 1)

# The kept copy has to be readable by the resolve.
old = """    // The transition doubles as the wait for the encode's writes.
    Barrier(cmdList, g_nr.colorCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);"""
new = """    // The transitions double as the wait for the encode's writes.
    Barrier(cmdList, g_nr.colorCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cmdList, g_nr.hdrCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);"""
assert old in text
text = text.replace(old, new, 1)

# --- resolve ---------------------------------------------------------------------------------------

old = """        resolveParams.debugView = cfg.DlssNrDebugView.value_or_default();"""
new = """        resolveParams.debugView = cfg.DlssNrDebugView.value_or_default();
        resolveParams.maxRatio = cfg.DlssNrMaxRatio.value_or_default();"""
assert old in text
text = text.replace(old, new, 1)

old = """        g_codec.dispatch(cmdList, resolveParams, g_nr.colorCopy, g_nr.output, target);"""
new = """        g_codec.dispatch(cmdList, resolveParams, g_nr.colorCopy, g_nr.output, g_nr.hdrCopy, target,
                         nullptr);"""
assert old in text
text = text.replace(old, new, 1)

# --- put the kept copy back ------------------------------------------------------------------------

old = """    // Put any guide clones back where the next frame's copy expects to find them."""
new = """    Barrier(cmdList, g_nr.hdrCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Put any guide clones back where the next frame's copy expects to find them."""
assert old in text
text = text.replace(old, new, 1)

io.open(PATH, "w", encoding="utf-8").write(text)
print("module patched")

# --- the highlight clamp as a setting ---------------------------------------------------------------

PATH = "C:/Games_Temp/OptiScaler/OptiScaler/Config.h"
text = io.open(PATH, encoding="utf-8").read()
old = """    // 0 off, 1 the picture the model was shown, 2 its raw answer, 3 what it changed, amplified."""
new = """    // The most the pass may multiply or divide a pixel by. A detail pass has no business restyling a
    // light source, whatever the model returns.
    CustomOptional<float> DlssNrMaxRatio { 2.0f };

    // 0 off, 1 the picture the model was shown, 2 its raw answer, 3 what it changed, amplified."""
assert old in text
text = text.replace(old, new, 1)
io.open(PATH, "w", encoding="utf-8").write(text)

PATH = "C:/Games_Temp/OptiScaler/OptiScaler/Config.cpp"
text = io.open(PATH, encoding="utf-8").read()
old = """            DlssNrDebugView.set_from_config(readUInt("DlssNr", "DebugView"));"""
new = """            DlssNrMaxRatio.set_from_config(readFloat("DlssNr", "MaxRatio"));
            DlssNrDebugView.set_from_config(readUInt("DlssNr", "DebugView"));"""
assert old in text
text = text.replace(old, new, 1)

old = """    ini.SetValue("DlssNr", "DebugView", GetIntValue(Instance()->DlssNrDebugView.value_for_config()).c_str());"""
new = """    ini.SetValue("DlssNr", "MaxRatio", GetFloatValue(Instance()->DlssNrMaxRatio.value_for_config()).c_str());
    ini.SetValue("DlssNr", "DebugView", GetIntValue(Instance()->DlssNrDebugView.value_for_config()).c_str());"""
assert old in text
text = text.replace(old, new, 1)
io.open(PATH, "w", encoding="utf-8").write(text)
print("config patched")

# --- menu -------------------------------------------------------------------------------------------

PATH = "C:/Games_Temp/OptiScaler/OptiScaler/menu/menu_common.cpp"
text = io.open(PATH, encoding="utf-8").read()
NL = "\\n"
old = '''        ImGui::SeparatorText("Inspect");'''
new = ('''        float maxRatio = config->DlssNrMaxRatio.value_or_default();
        if (ImGui::SliderFloat("Highlight guard", &maxRatio, 1.0f, 8.0f, "%.1fx"))
            config->DlssNrMaxRatio = maxRatio;

        ShowHelpMarker("The most the pass may brighten or darken any pixel."
                       "@@Lights are where the model has least to say and where scaling its answer back"
                       "@into the frame does the most damage -- an early version turned every strip light"
                       "@in the scene into a string of coloured cells. 1x disables the pass entirely;"
                       "@2x leaves detail intact while making that failure impossible.");

        ImGui::SeparatorText("Inspect");''').replace("@", NL)
assert old in text
text = text.replace(old, new, 1)
io.open(PATH, "w", encoding="utf-8").write(text)
print("menu patched")
