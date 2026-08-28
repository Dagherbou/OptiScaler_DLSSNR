"""Tells the model the truth about the guides instead of guessing.

Two values were being invented rather than read, and the model uses both for its own temporal
reprojection -- it takes depth, motion vectors and a reset flag, which is only meaningful if it
reprojects internally. Get either wrong and its history lands on the wrong pixels, which is what
detail crawling along shadow edges looks like.

  DepthInverted was hardcoded to 1. The game states it, in the flags it created its own DLSS feature
  with, and that word is already being read for the HDR bit.

  MVecScale was computed as a resolution ratio. That is not what it means: the game supplies its own
  MV.Scale.X/Y describing how its motion vectors are encoded. At DLAA the invented ratio is exactly
  1.0, so a game whose vectors are in normalised units was reporting almost no motion at all, and the
  model would have been reprojecting its history onto the same pixel every frame.

The values are captured where the upscaler hands them over, because the finished-frame path runs long
after that call has returned.
"""

import io

PATH = "C:/Games_Temp/OptiScaler/OptiScaler/dlssnr/DlssNr_Dx12.cpp"
text = io.open(PATH, encoding="utf-8").read()

# --- remember what the game said ------------------------------------------------------------------------

old = """    unsigned int guideWidth = 0;
    unsigned int guideHeight = 0;
    bool guidesReady = false;"""
new = """    unsigned int guideWidth = 0;
    unsigned int guideHeight = 0;
    bool guidesReady = false;

    // How the game encodes its guides, as the game itself reports it. Captured with the guides, since
    // the finished-frame path runs long after the upscaler's call has returned.
    bool guideDepthInverted = false;
    float guideMvScaleX = 1.0f;
    float guideMvScaleY = 1.0f;"""
assert old in text
text = text.replace(old, new, 1)

# --- read them alongside the guides ------------------------------------------------------------------------

old = """    if (guideWidth == 0 || guideHeight == 0)
    {
        guideWidth = width;
        guideHeight = height;
    }
"""
new = """    if (guideWidth == 0 || guideHeight == 0)
    {
        guideWidth = width;
        guideHeight = height;
    }

    // The game states its depth convention in the flags it created its own feature with, so there is no
    // reason to assume one.
    unsigned int createFlags = 0;
    params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &createFlags);
    g_nr.guideDepthInverted = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;

    // And it states how its motion vectors are encoded. Inventing a resolution ratio here meant handing
    // the model vectors it could not interpret.
    float mvScaleX = 1.0f;
    float mvScaleY = 1.0f;

    if (params->Get(NVSDK_NGX_Parameter_MV_Scale_X, &mvScaleX) != NVSDK_NGX_Result_Success)
        mvScaleX = 1.0f;

    if (params->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &mvScaleY) != NVSDK_NGX_Result_Success)
        mvScaleY = 1.0f;

    g_nr.guideMvScaleX = mvScaleX;
    g_nr.guideMvScaleY = mvScaleY;

    static bool reportedGuides = false;

    if (!reportedGuides)
    {
        reportedGuides = true;
        LOG_INFO("DLSS-NR guides: depth {}, motion vector scale {} x {} (the game's own values, not "
                 "assumed)",
                 g_nr.guideDepthInverted ? "inverted" : "not inverted", mvScaleX, mvScaleY);
    }
"""
assert old in text
text = text.replace(old, new, 1)

# --- pass them instead of the guesses ------------------------------------------------------------------------

old = """        height, guideWidth, guideHeight, 1, g_nr.reset ? 1 : 0, cfg.DlssNrIntensity.value_or_default(),"""
new = """        height, guideWidth, guideHeight, g_nr.guideDepthInverted ? 1 : 0, g_nr.reset ? 1 : 0,
        cfg.DlssNrIntensity.value_or_default(),"""
assert old in text
text = text.replace(old, new, 1)

old = """        g_nr.output, width, height, g_nr.guideWidth, g_nr.guideHeight, 1, g_nr.reset ? 1 : 0,"""
new = """        g_nr.output, width, height, g_nr.guideWidth, g_nr.guideHeight, g_nr.guideDepthInverted ? 1 : 0,
        g_nr.reset ? 1 : 0,"""
assert old in text
text = text.replace(old, new, 1)

io.open(PATH, "w", encoding="utf-8").write(text)
print("module reads the guides' conventions")

# --- forwarder takes the scale rather than deriving one -----------------------------------------------------

for path in ("C:/Games_Temp/dlssnr-forwarder/dlssnr_forwarder.cpp",
             "C:/Games_Temp/OptiScaler/OptiScaler/dlssnr/forwarder/dlssnr_forwarder.cpp",
             "C:/Games_Temp/cp2077-nr/dlssnr_ngx.cpp"):
    t = io.open(path, encoding="utf-8").read()

    old = """                                               float localTone, float skinStructure, int useAutoMask) {"""
    new = """                                               float localTone, float skinStructure, int useAutoMask,
                                               float mvScaleX, float mvScaleY) {"""
    assert old in t, "evaluate signature not found in " + path
    t = t.replace(old, new, 1)

    old = """    setFloat(capabilityParams, "DLSSNR.MVecScaleX",
             guideWidth ? (float) width / (float) guideWidth : 1.0f);
    setFloat(capabilityParams, "DLSSNR.MVecScaleY",
             guideHeight ? (float) height / (float) guideHeight : 1.0f);"""
    new = """    // The game's own encoding, passed through. Deriving this from the resolutions was a guess, and at
    // native resolution it came out as exactly 1.0 -- so a game using normalised vectors was telling
    // the model almost nothing had moved.
    setFloat(capabilityParams, "DLSSNR.MVecScaleX", mvScaleX);
    setFloat(capabilityParams, "DLSSNR.MVecScaleY", mvScaleY);"""
    assert old in t, "mvec scale not found in " + path
    t = t.replace(old, new, 1)

    io.open(path, "w", encoding="utf-8").write(t)
    print("forwarder patched:", path)

# --- the module's call sites gain the two arguments -------------------------------------------------------

PATH = "C:/Games_Temp/OptiScaler/OptiScaler/dlssnr/DlssNr_Dx12.cpp"
text = io.open(PATH, encoding="utf-8").read()

text = text.replace(
    """                                      unsigned int, unsigned int, unsigned int, int, int, float, int,
                                      float, float, float, int);""",
    """                                      unsigned int, unsigned int, unsigned int, int, int, float, int,
                                      float, float, float, int, float, float);""", 1)

old = """        cfg.DlssNrAutoMask.value_or_default() ? 1 : 0);"""
assert text.count(old) == 2, "expected two evaluate sites, found %d" % text.count(old)
text = text.replace(old, """        cfg.DlssNrAutoMask.value_or_default() ? 1 : 0, g_nr.guideMvScaleX, g_nr.guideMvScaleY);""")

io.open(PATH, "w", encoding="utf-8").write(text)
print("call sites updated")
