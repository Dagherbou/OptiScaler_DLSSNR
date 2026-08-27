"""Adds the DLSS-NR options that were missing, and corrects the ones that were guessed.

Skin structure defaults to -1, meaning follow local structure -- that is the model's own default, and
1.0 was simply wrong. The transfer and colour strengths are new: the model's answer is now applied as a
difference, so how much of it lands is a real control rather than all-or-nothing.
"""

import io

ROOT = "C:/Games_Temp/OptiScaler/OptiScaler/"

# --- Config.h ------------------------------------------------------------------------------------

path = ROOT + "Config.h"
text = io.open(path, encoding="utf-8").read()

old = """    CustomOptional<bool> DlssNrEnabled { false };
    CustomOptional<bool> DlssNrToneTransform { true };
    CustomOptional<float> DlssNrWhitePoint { 2.0f };
    CustomOptional<uint32_t> DlssNrPreset { 0 };
    CustomOptional<float> DlssNrIntensity { 1.0f };
    CustomOptional<uint32_t> DlssNrStyle { 0 };
    CustomOptional<float> DlssNrLocalStructure { 1.0f };
    CustomOptional<float> DlssNrLocalTone { 1.0f };
    CustomOptional<float> DlssNrSkinStructure { 1.0f };
    CustomOptional<bool> DlssNrAutoMask { true };"""

new = """    CustomOptional<bool> DlssNrEnabled { false };
    CustomOptional<uint32_t> DlssNrPreset { 0 };
    CustomOptional<float> DlssNrIntensity { 1.0f };
    CustomOptional<uint32_t> DlssNrStyle { 0 };
    CustomOptional<float> DlssNrLocalStructure { 1.0f };
    CustomOptional<float> DlssNrLocalTone { 1.0f };
    // -1 means follow local structure, which is the model's own default. It is not a strength of zero.
    CustomOptional<float> DlssNrSkinStructure { -1.0f };
    CustomOptional<bool> DlssNrAutoMask { true };

    // How much of the model's edit reaches the frame. Separated because detail synthesis is a luminance
    // edit and any colour shift is usually the part you do not want, and allowed past 1.0 because
    // exaggerating an edit is the only honest way to see whether there is one.
    CustomOptional<float> DlssNrTransferStrength { 1.0f };
    CustomOptional<float> DlssNrColourStrength { 1.0f };

    // The linear value the encode maps to display white. Derived from the frame by default: measured
    // means in one game have ranged from 0.065 to 185, so no fixed number can serve.
    CustomOptional<bool> DlssNrAutoWhitePoint { true };
    CustomOptional<float> DlssNrWhitePoint { 2.0f };

    // 0 off, 1 the picture the model was shown, 2 its raw answer, 3 what it changed, amplified.
    CustomOptional<uint32_t> DlssNrDebugView { 0 };"""

assert old in text, "config block not found"
text = text.replace(old, new, 1)
io.open(path, "w", encoding="utf-8").write(text)
print("Config.h patched")

# --- Config.cpp ----------------------------------------------------------------------------------

path = ROOT + "Config.cpp"
text = io.open(path, encoding="utf-8").read()

old = """            DlssNrToneTransform.set_from_config(readBool("DlssNr", "ToneTransform"));
            DlssNrWhitePoint.set_from_config(readFloat("DlssNr", "WhitePoint"));"""
new = """            DlssNrTransferStrength.set_from_config(readFloat("DlssNr", "TransferStrength"));
            DlssNrColourStrength.set_from_config(readFloat("DlssNr", "ColourStrength"));
            DlssNrAutoWhitePoint.set_from_config(readBool("DlssNr", "AutoWhitePoint"));
            DlssNrWhitePoint.set_from_config(readFloat("DlssNr", "WhitePoint"));
            DlssNrDebugView.set_from_config(readUInt("DlssNr", "DebugView"));"""
assert old in text, "config read block not found"
text = text.replace(old, new, 1)

old = """    ini.SetValue("DlssNr", "ToneTransform",
                 GetBoolValue(Instance()->DlssNrToneTransform.value_for_config()).c_str());
    ini.SetValue("DlssNr", "WhitePoint", GetFloatValue(Instance()->DlssNrWhitePoint.value_for_config()).c_str());"""
new = """    ini.SetValue("DlssNr", "TransferStrength",
                 GetFloatValue(Instance()->DlssNrTransferStrength.value_for_config()).c_str());
    ini.SetValue("DlssNr", "ColourStrength",
                 GetFloatValue(Instance()->DlssNrColourStrength.value_for_config()).c_str());
    ini.SetValue("DlssNr", "AutoWhitePoint",
                 GetBoolValue(Instance()->DlssNrAutoWhitePoint.value_for_config()).c_str());
    ini.SetValue("DlssNr", "WhitePoint", GetFloatValue(Instance()->DlssNrWhitePoint.value_for_config()).c_str());
    ini.SetValue("DlssNr", "DebugView", GetIntValue(Instance()->DlssNrDebugView.value_for_config()).c_str());"""
assert old in text, "config write block not found"
text = text.replace(old, new, 1)

io.open(path, "w", encoding="utf-8").write(text)
print("Config.cpp patched")
