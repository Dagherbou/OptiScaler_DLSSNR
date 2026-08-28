"""Applies what a general-purpose RenoDX DLSS 5 build revealed about the model.

Four things, none of which were guessable from the outside:

  * DLSSNR.UICorrection exists and was never being set.

  * The colour codec belongs behind the game's own HDR flag. That build applies its transform "only when
    NGX marks HDR", reading DLSS.Feature.Create.Flags. A game handing DLSS an already tone-mapped buffer
    needs no transform at all, and running one over it is pure damage.

  * Style has two values, named Natural and Cinematic -- not the nine a guessed slider offered.

  * The preset has four, named Default and Preset #1 to #3, not nine either.

The style and preset ranges match what was reported from playing: that only two styles did anything.
"""

import io

# --- forwarder: UI correction is create-time like the rest of the tuning -----------------------------

for path in ("C:/Games_Temp/dlssnr-forwarder/dlssnr_forwarder.cpp",
             "C:/Games_Temp/OptiScaler/OptiScaler/dlssnr/forwarder/dlssnr_forwarder.cpp",
             "C:/Games_Temp/cp2077-nr/dlssnr_ngx.cpp"):
    text = io.open(path, encoding="utf-8").read()

    old = "float skinStructure, int useAutoMask) {"
    new = "float skinStructure, int useAutoMask,\n                                               int uiCorrection) {"
    assert old in text, "create signature not found in " + path
    text = text.replace(old, new, 1)

    old = '''    setUInt(capabilityParams, "DLSSNR.UseAutoMask", (unsigned int) useAutoMask);'''
    assert old in text, "auto mask line not found in " + path
    # Only the create-time copy takes the new parameter; the evaluate-time one is left alone.
    head, sep, tail = text.partition(old)
    text = head + old + '''
    setUInt(capabilityParams, "DLSSNR.UICorrection", (unsigned int) uiCorrection);''' + tail

    io.open(path, "w", encoding="utf-8").write(text)
    print("forwarder patched:", path)

# --- OptiScaler module -------------------------------------------------------------------------------

PATH = "C:/Games_Temp/OptiScaler/OptiScaler/dlssnr/DlssNr_Dx12.cpp"
text = io.open(PATH, encoding="utf-8").read()

text = text.replace(
    """                                      ID3D12GraphicsCommandList*, void*, unsigned int, unsigned int, int,
                                      float, int, float, float, float, int);""",
    """                                      ID3D12GraphicsCommandList*, void*, unsigned int, unsigned int, int,
                                      float, int, float, float, float, int, int);""",
    1)

old = """                        cfg.DlssNrAutoMask.value_or_default() ? 1 : 0);"""
new = """                        cfg.DlssNrAutoMask.value_or_default() ? 1 : 0,
                        cfg.DlssNrUiCorrection.value_or_default() ? 1 : 0);"""
assert text.count(old) == 2, "expected two create sites, found %d" % text.count(old)
text = text.replace(old, new)

# --- the codec goes behind the game's own HDR flag ----------------------------------------------------

old = """    const bool haveCodec = g_codec.ensure(device);"""
new = """    // Whether the buffer the upscaler just wrote is linear HDR or an already tone-mapped picture is not
    // something to assume: the game says so, in the flags it created its own DLSS feature with. Running
    // the colour transform over a frame that has already been through a tonemapper is pure damage, and
    // skipping it on one that has not leaves the model reading ordinary values as enormously bright.
    unsigned int dlssFlags = 0;
    params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &dlssFlags);
    const bool isHdrBuffer = (dlssFlags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) != 0;

    static bool reportedHdr = false;

    if (!reportedHdr)
    {
        reportedHdr = true;
        LOG_INFO("DLSS-NR: the game's DLSS buffer is {} (create flags 0x{:X}), so the colour transform is {}",
                 isHdrBuffer ? "linear HDR" : "already tone-mapped", dlssFlags,
                 isHdrBuffer ? "on" : "off");
    }

    const bool haveCodec = g_codec.ensure(device);"""
assert old in text
text = text.replace(old, new, 1)

# Pass the frame straight through when it needs no conversion.
old = """    codec::Params encodeParams {};
    encodeParams.mode = codec::MODE_ENCODE;
    encodeParams.whitePoint = whitePoint;"""
new = """    codec::Params encodeParams {};
    encodeParams.mode = codec::MODE_ENCODE;
    // A frame that is already display-referred is handed over untouched: the encode becomes a copy and
    // the resolve adds the model's edit back at full scale.
    encodeParams.passthrough = isHdrBuffer ? 0u : 1u;
    encodeParams.whitePoint = whitePoint;"""
assert old in text
text = text.replace(old, new, 1)

old = """        resolveParams.maxRatio = cfg.DlssNrMaxRatio.value_or_default();"""
new = """        resolveParams.maxRatio = cfg.DlssNrMaxRatio.value_or_default();
        resolveParams.passthrough = isHdrBuffer ? 0u : 1u;"""
assert old in text
text = text.replace(old, new, 1)

io.open(PATH, "w", encoding="utf-8").write(text)
print("module patched")

# --- codec: honour passthrough ------------------------------------------------------------------------

PATH = "C:/Games_Temp/OptiScaler/OptiScaler/dlssnr/DlssNr_Codec.h"
text = io.open(PATH, encoding="utf-8").read()

text = text.replace("""    uint  gDebugView;
    float gMaxRatio;
};""", """    uint  gDebugView;
    float gMaxRatio;
    uint  gPassthrough;
    uint  gPad;
};""", 1)

text = text.replace("""    unsigned int debugView;
    float maxRatio;
};""", """    unsigned int debugView;
    float maxRatio;
    // Set when the game's own buffer is already tone-mapped, in which case there is nothing to convert.
    unsigned int passthrough;
    unsigned int pad;
};""", 1)

old = """        float luma = dot(frame, kLuma);
        float toned = (luma / gWhitePoint) / (1.0 + luma / gWhitePoint);
        float scale = luma > 1e-6 ? toned / luma : 0.0;

        gTarget[id.xy] = float4(LinearToSrgb(frame * scale), source.a);
        return;"""
new = """        if (gPassthrough != 0)
        {
            gTarget[id.xy] = float4(frame, source.a);
            return;
        }

        float luma = dot(frame, kLuma);
        float toned = (luma / gWhitePoint) / (1.0 + luma / gWhitePoint);
        float scale = luma > 1e-6 ? toned / luma : 0.0;

        gTarget[id.xy] = float4(LinearToSrgb(frame * scale), source.a);
        return;"""
assert old in text
text = text.replace(old, new, 1)

old = """    float4 proxySample = gSource.Load(int3(id.xy, 0));
    float3 proxy = SrgbToLinear(proxySample.rgb);
    float3 model = SrgbToLinear(gModel.Load(int3(id.xy, 0)).rgb);"""
new = """    float4 proxySample = gSource.Load(int3(id.xy, 0));
    float4 modelSample = gModel.Load(int3(id.xy, 0));

    // Nothing was encoded on the way in, so nothing is decoded here either.
    float3 proxy = gPassthrough != 0 ? proxySample.rgb : SrgbToLinear(proxySample.rgb);
    float3 model = gPassthrough != 0 ? modelSample.rgb : SrgbToLinear(modelSample.rgb);"""
assert old in text
text = text.replace(old, new, 1)

old = """    float originalLuma = dot(original, kLuma);
    float slope = gWhitePoint + originalLuma;"""
new = """    float originalLuma = dot(original, kLuma);
    // With no curve applied there is no slope to undo: the edit lands as it is.
    float slope = gPassthrough != 0 ? 1.0 : gWhitePoint + originalLuma;"""
assert old in text
text = text.replace(old, new, 1)

io.open(PATH, "w", encoding="utf-8").write(text)
print("codec patched")

# --- config -------------------------------------------------------------------------------------------

PATH = "C:/Games_Temp/OptiScaler/OptiScaler/Config.h"
text = io.open(PATH, encoding="utf-8").read()
old = "    CustomOptional<bool> DlssNrAutoMask { true };"
new = """    CustomOptional<bool> DlssNrAutoMask { true };
    // Keeps the model off the interface. It matters most on the finished frame, where the HUD is part
    // of the picture the model is handed.
    CustomOptional<bool> DlssNrUiCorrection { true };"""
assert old in text
text = text.replace(old, new, 1)
io.open(PATH, "w", encoding="utf-8").write(text)

PATH = "C:/Games_Temp/OptiScaler/OptiScaler/Config.cpp"
text = io.open(PATH, encoding="utf-8").read()
old = '            DlssNrAutoMask.set_from_config(readBool("DlssNr", "AutoMask"));'
assert old in text
text = text.replace(old, old + '\n            DlssNrUiCorrection.set_from_config(readBool("DlssNr", "UiCorrection"));', 1)
old = '    ini.SetValue("DlssNr", "AutoMask", GetBoolValue(Instance()->DlssNrAutoMask.value_for_config()).c_str());'
assert old in text
text = text.replace(old, old + '\n    ini.SetValue("DlssNr", "UiCorrection", GetBoolValue(Instance()->DlssNrUiCorrection.value_for_config()).c_str());', 1)
io.open(PATH, "w", encoding="utf-8").write(text)
print("config patched")
