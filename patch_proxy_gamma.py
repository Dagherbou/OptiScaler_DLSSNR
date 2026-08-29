import io

# ==================== CODEC ====================
p = 'OptiScaler/dlssnr/DlssNr_Codec.h'
t = io.open(p, encoding='utf-8').read()

old = """        // Reinhard on luminance alone, with chroma carried along untouched. Compressing each channel
        // separately is what shifted the hue of every saturated highlight.
        float luma = dot(frame, kLuma);
        float toned = gCurveMode != 0 ? CurveToned(luma) : (luma / gWhitePoint) / (1.0 + luma / gWhitePoint);
        float scale = luma > 1e-6 ? toned / luma : 0.0;
        float3 display = frame * scale;

        // The game's colour grade, learned: after the luminance map the fitted matrix turns the
        // picture into what the game would have made of it -- the model then sees the game's palette
        // as well as its contrast.
        if (gCurveMode != 0)
            display = max(mul(CurveMatrix(), display), float3(0.0, 0.0, 0.0));"""
assert old in t
t = t.replace(old, """        // What the model is shown. Mode 2 -- the default -- scales the frame and encodes it, and
        // that is all: the game is going to tone map this picture later, so tone mapping it here as
        // well shows the model a doubly compressed image. Measured against Cyberpunk's own numbers,
        // the Reinhard proxy handed the model a scene value of 1.0 as 0.55 and 1.5 as 0.64 -- flat,
        // dark, and nothing like the finished frame it was trained on. The model then synthesised
        // weakly, judged tone on a picture that does not exist, and its answer had to be un-crushed
        // on the way back. Mode 0 keeps that old curve, mode 1 the fitted one.
        float luma = dot(frame, kLuma);
        float toned = gCurveMode == 2   ? luma / max(gWhitePoint, 1e-4)
                      : gCurveMode == 1 ? CurveToned(luma)
                                        : (luma / gWhitePoint) / (1.0 + luma / gWhitePoint);
        float scale = luma > 1e-6 ? toned / luma : 0.0;
        float3 display = frame * scale;

        // The game's colour grade, learned: after the luminance map the fitted matrix turns the
        // picture into what the game would have made of it -- the model then sees the game's palette
        // as well as its contrast.
        if (gCurveMode == 1)
            display = max(mul(CurveMatrix(), display), float3(0.0, 0.0, 0.0));""", 1)

old = """        float3 appliedScene = gCurveMode != 0 ? mul(Inverse3(CurveMatrix()), applied) : applied;"""
assert old in t
t = t.replace(old, """        float3 appliedScene = gCurveMode == 1 ? mul(Inverse3(CurveMatrix()), applied) : applied;""", 1)
io.open(p, 'w', encoding='utf-8').write(t)
print('codec in')

# ==================== MODULE ====================
p = 'OptiScaler/dlssnr/DlssNr_Dx12.cpp'
d = io.open(p, encoding='utf-8').read()

old = """static void FillCurve(codec::Params& params, bool wanted)
{
    if (!wanted || !g_curve.ready)
        return;

    params.curveMode = 1;"""
assert old in d
d = d.replace(old, """// The proxy the model is shown: 0 the old Reinhard curve, 1 the curve fitted to the game, 2 a plain
// scale and encode. Two returns the white point as a divisor rather than a curve's shoulder, so the
// scale control is the only thing acting on it.
unsigned int ProxyMode(const Config& cfg)
{
    const unsigned int mode = cfg.DlssNrProxyCurve.value_or_default();
    return mode > 2 ? 2 : mode;
}

static void FillCurve(codec::Params& params, bool wanted)
{
    if (!wanted || !g_curve.ready)
        return;

    params.curveMode = 1;""", 1)

# the white point handed to the shader: a divisor in mode 2, the curve's white point otherwise
old = """    const float whitePoint = (autoWhite && g_autoWhitePointSettled
                                  ? g_autoWhitePoint
                                  : cfg.DlssNrWhitePoint.value_or_default()) *
                             cfg.DlssNrWhitePointScale.value_or_default();"""
assert old in d
d = d.replace(old, """    const float proxyMode = (float) ProxyMode(cfg);
    const float whitePoint = proxyMode == 2.0f
                                 ? cfg.DlssNrWhitePointScale.value_or_default()
                                 : (autoWhite && g_autoWhitePointSettled
                                        ? g_autoWhitePoint
                                        : cfg.DlssNrWhitePoint.value_or_default()) *
                                       cfg.DlssNrWhitePointScale.value_or_default();""", 1)

# the encode and resolve of the render path carry the mode
old = """    encodeParams.passthrough = isHdrBuffer ? 0u : 1u;
    encodeParams.whitePoint = whitePoint;
    FillCurve(encodeParams, curveMatch);"""
assert old in d
d = d.replace(old, """    encodeParams.passthrough = isHdrBuffer ? 0u : 1u;
    encodeParams.whitePoint = whitePoint;
    encodeParams.curveMode = ProxyMode(cfg);
    FillCurve(encodeParams, curveMatch);""", 1)

old = """        resolveParams.mode = codec::MODE_RESOLVE;
        resolveParams.whitePoint = whitePoint;
"""
assert old in d
d = d.replace(old, """        resolveParams.mode = codec::MODE_RESOLVE;
        resolveParams.whitePoint = whitePoint;
        resolveParams.curveMode = ProxyMode(cfg);
""", 1)

# the matched curve only fits when it is the chosen mode
old = "    const bool curveMatch = cfg.DlssNrProxyCurve.value_or_default() == 1 && isHdrBuffer;"
assert old in d
d = d.replace(old, "    const bool curveMatch = ProxyMode(cfg) == 1 && isHdrBuffer;", 1)
io.open(p, 'w', encoding='utf-8').write(d)
print('module in')

# ==================== CONFIG ====================
p = 'OptiScaler/Config.h'
c = io.open(p, encoding='utf-8').read()
old = """    // The curve the linear frame is compressed through for the model. 0: Reinhard against the white
    // point. 1: a curve fitted to the game's own tonemapper by matching the linear and finished frames'
    // histograms, so the model sees the game's contrast and shadow depth.
    CustomOptional<uint32_t> DlssNrProxyCurve { 0 };"""
assert old in c
c = c.replace(old, """    // What the linear frame is turned into for the model. 0: Reinhard against the white point --
    // compresses hard, and the game tone maps again afterwards. 1: a curve fitted to the game's own
    // tonemapper. 2 (default): scale and encode, nothing else, which is closest to the picture the
    // model was trained on.
    CustomOptional<uint32_t> DlssNrProxyCurve { 2 };""", 1)
io.open(p, 'w', encoding='utf-8').write(c)
print('config in')

# ==================== MENU ====================
p = 'OptiScaler/dlssnr/DlssNr_Menu.cpp'
m = io.open(p, encoding='utf-8').read()
old = '''            static const char* curveNames[] = { "Reinhard (white point above)", "Match the game's tonemapper and grade" };
            int curve = (int) config->DlssNrProxyCurve.value_or_default();
            if (curve > 1)
                curve = 1;'''
assert old in m
m = m.replace(old, '''            static const char* curveNames[] = { "Reinhard (compresses hard)",
                                                "Match the game's tonemapper and grade",
                                                "Scale and encode only (default)" };
            int curve = (int) config->DlssNrProxyCurve.value_or_default();
            if (curve > 2)
                curve = 2;''', 1)

start = m.find('            HelpMarker("How the linear frame is compressed for the model')
assert start > 0
end = m.find('inject points, which are already finished pictures.");', start)
assert end > start
end += len('inject points, which are already finished pictures.");')
m = m[:start] + '''            HelpMarker("What the model is shown in the split, where it sees the frame before the"
                       "\\ngame's tonemapper has run."
                       "\\n\\nScale and encode only: the frame, scaled by the white point control and"
                       "\\nencoded, with a soft knee at the top. Nothing else -- because the game is"
                       "\\ngoing to tone map this picture anyway, and doing it here as well shows the"
                       "\\nmodel a doubly compressed image. Measured in Cyberpunk, the old Reinhard"
                       "\\nproxy handed it a scene value of 1.0 as 0.55: flat, dark, and nothing like"
                       "\\nthe finished frame it was trained on."
                       "\\n\\nReinhard: that old curve, kept for comparison."
                       "\\n\\nMatch: a curve and colour matrix fitted to the game by comparing the same"
                       "\\nframe before and after its post chain. More faithful in principle, and it"
                       "\\nneeds an SDR output to learn from."');' + m[end:]
m = m.replace('needs an SDR output to learn from."\');', 'needs an SDR output to learn from.");', 1)
io.open(p, 'w', encoding='utf-8').write(m)
print('menu in')
