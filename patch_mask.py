"""Adds the scaling ratio, and a control mask so the model can be told where to work.

Two of the parameters the model exposes that were not being used.

ScalingRatio is a correctness item. Everything has been tested at 1:1 so far; the moment the upscaler is
not running at native, colour and output are display resolution while depth and motion are render
resolution, and the model is entitled to be told the ratio between them.

ControlMask is the interesting one: an explicit per-pixel mask. If the model can be told where to work,
that is a better lever than any global strength -- it can be kept off the sky, off surfaces moving too
fast for synthesised detail to survive, and off anything else worth excluding.

Its polarity is not documented, so this does not assume one. There is an invert switch and a debug view
that shows the mask, which together settle the question by looking rather than by guessing. It is off by
default: an untested mask is a quality risk, and the standing requirement here is no degradation.
"""

import io

# --- forwarder: the mask and the ratio ----------------------------------------------------------------

for path in ("C:/Games_Temp/dlssnr-forwarder/dlssnr_forwarder.cpp",
             "C:/Games_Temp/OptiScaler/OptiScaler/dlssnr/forwarder/dlssnr_forwarder.cpp",
             "C:/Games_Temp/cp2077-nr/dlssnr_ngx.cpp"):
    text = io.open(path, encoding="utf-8").read()

    old = """                                               float localTone, float skinStructure, int useAutoMask) {"""
    new = """                                               float localTone, float skinStructure, int useAutoMask,
                                               ID3D12Resource *controlMask) {"""
    assert old in text, "evaluate signature not found in " + path
    text = text.replace(old, new, 1)

    old = """    setResource(capabilityParams, "DLSSNR.Output", output);"""
    new = """    setResource(capabilityParams, "DLSSNR.Output", output);

    // An explicit per-pixel mask, when one is supplied. Always written, including null, because this
    // block belongs to the driver and outlives the feature -- leaving a stale pointer in it would hand
    // the model a mask it was never given.
    setResource(capabilityParams, "DLSSNR.ControlMask", controlMask);"""
    assert old in text, "output line not found in " + path
    text = text.replace(old, new, 1)

    old = """    setFloat(capabilityParams, "DLSSNR.MVecScaleX","""
    new = """    if (controlMask != nullptr) {
        setUInt(capabilityParams, "DLSSNR.ControlMaskSubrectBaseX", 0);
        setUInt(capabilityParams, "DLSSNR.ControlMaskSubrectBaseY", 0);
        setUInt(capabilityParams, "DLSSNR.ControlMaskSubrectWidth", guideWidth);
        setUInt(capabilityParams, "DLSSNR.ControlMaskSubrectHeight", guideHeight);
    }

    // The relationship between the guides and the picture, which the model is entitled to know rather
    // than infer from the subrects.
    setFloat(capabilityParams, "DLSSNR.ScalingRatio",
             guideWidth ? (float) width / (float) guideWidth : 1.0f);

    setFloat(capabilityParams, "DLSSNR.MVecScaleX","""
    assert old in text, "mvec scale not found in " + path
    text = text.replace(old, new, 1)

    io.open(path, "w", encoding="utf-8").write(text)
    print("forwarder patched:", path)

# --- OptiScaler: signature and call sites ---------------------------------------------------------------

PATH = "C:/Games_Temp/OptiScaler/OptiScaler/dlssnr/DlssNr_Dx12.cpp"
text = io.open(PATH, encoding="utf-8").read()

old = """                                      unsigned int, unsigned int, unsigned int, int, int, float, int,
                                      float, float, float, int);"""
new = """                                      unsigned int, unsigned int, unsigned int, int, int, float, int,
                                      float, float, float, int, ID3D12Resource*);"""
assert old in text
text = text.replace(old, new, 1)

old = """        cfg.DlssNrAutoMask.value_or_default() ? 1 : 0);

    g_nr.reset = false;"""
assert text.count(old) == 2, "expected two evaluate sites, found %d" % text.count(old)
new = """        cfg.DlssNrAutoMask.value_or_default() ? 1 : 0, mask);

    g_nr.reset = false;"""
text = text.replace(old, new)

io.open(PATH, "w", encoding="utf-8").write(text)
print("module signature patched")
