import io


def cut(text, start_marker, end_marker, label):
    s = text.find(start_marker)
    assert s > 0, 'start missing: ' + label
    e = text.find(end_marker, s)
    assert e > s, 'end missing: ' + label
    removed = text[s:e].count(chr(10))
    print('  cut %-28s %4d lines' % (label, removed))
    return text[:s] + text[e:]


# ==================== CODEC ====================
p = 'OptiScaler/dlssnr/DlssNr_Codec.h'
t = io.open(p, encoding='utf-8').read()

old = """    uint  gCurveMode;    // 0: Reinhard against the white point. 1: the fitted curve and matrix below.
    float gCurveMinLog;  // log2 luminance of the curve's first entry
    float gCurveRangeLog; // log2 span across its 24 entries
    float gRestoreSkipSkin; // 1: highlight restore leaves skin alone; 0: applies everywhere
    uint  gPad2;
    uint  gPad3;
    float4 gCurve[6];    // 24 toned (linear-display) luminance values, log-spaced in scene luminance
    float4 gMat[3];      // the game's colour transform after the curve, rows (w unused)
};"""
if old not in t:
    # the skin field was already renamed to a pad
    old = old.replace("    float gRestoreSkipSkin; // 1: highlight restore leaves skin alone; 0: applies everywhere",
                      "    uint  gPad1;")
assert old in t, 'cbuffer tail'
t = t.replace(old, "};", 1)

t = cut(t, "float3x3 CurveMatrix()", "// The edit at an arbitrary position", 'curve helpers + matrix')

old = """        float toned = gCurveMode == 2   ? luma / max(gWhitePoint, 1e-4)
                      : gCurveMode == 1 ? CurveToned(luma)
                                        : (luma / gWhitePoint) / (1.0 + luma / gWhitePoint);
        float scale = luma > 1e-6 ? toned / luma : 0.0;
        float3 display = frame * scale;

        // The game's colour grade, learned: after the luminance map the fitted matrix turns the
        // picture into what the game would have made of it -- the model then sees the game's palette
        // as well as its contrast.
        if (gCurveMode == 1)
            display = max(mul(CurveMatrix(), display), float3(0.0, 0.0, 0.0));"""
assert old in t, 'encode body'
t = t.replace(old, """        float3 display = frame / max(gWhitePoint, 1e-4);""", 1)

old = """        float3 appliedScene = gCurveMode == 1 ? mul(Inverse3(CurveMatrix()), applied) : applied;
        float appliedLuma = dot(appliedScene, kLuma);
        float3 appliedChroma = appliedScene - appliedLuma;
        float gain = max(1.0 + appliedLuma * slope / originalLuma, 0.0);
"""
assert old in t, 'resolve gain'
t = t.replace(old, """        float appliedLuma = dot(applied, kLuma);
        float3 appliedChroma = applied - appliedLuma;
        float gain = max(1.0 + appliedLuma * slope / originalLuma, 0.0);
""", 1)

t = cut(t, "        // With the game's curve fitted, the edit does not have to be scaled back",
        "        result = original * gain + appliedChroma * slope;", 'curve inverse use')

old = """    unsigned int curveMode;
    float curveMinLog;
    float curveRangeLog;"""
assert old in t, 'Params curve fields'
i = t.find(old)
j = t.find("};", i)
tail = t[i:j]
keep = [l for l in tail.split(chr(10)) if not any(k in l for k in
        ('curveMode', 'curveMinLog', 'curveRangeLog', 'float curve[', 'float mat[', 'pad1', 'pad2', 'pad3',
         'restoreSkipSkin'))]
t = t[:i] + chr(10).join(x for x in keep if x.strip()) + chr(10) + t[j:]
assert 'gCurveMode' not in t and 'CurveToned' not in t and 'gMat[' not in t
io.open(p, 'w', encoding='utf-8').write(t)
print('codec done')

# ==================== MODULE ====================
p = 'OptiScaler/dlssnr/DlssNr_Dx12.cpp'
d = io.open(p, encoding='utf-8').read()

d = cut(d, "// The game-matched proxy curve. Two histograms", "bool g_splitActive = false;", 'curve state + fit')
d = cut(d, "// The proxy the model is shown: 0 the old Reinhard curve", "// The extras the official integration sets", 'ProxyMode/FillCurve')
d = cut(d, "// Reads the finished frame's luminance samples for the proxy curve", "void EvaluateAtPresent(", 'present measurement')

old = """    const bool curveMatch = ProxyMode(cfg) == 1 && isHdrBuffer;

    if (autoWhite || curveMatch)
    {
        const probe::Stats stats = g_reader.collect(curveMatch ? g_curve.linear.data() : nullptr,
                                                     curveMatch ? g_curve.linearRgb.data() : nullptr);

        if (curveMatch && stats.valid)
        {
            g_curve.linearFresh = true;
            FitProxyCurve();
        }
"""
assert old in d, 'render sampling'
d = d.replace(old, """    if (autoWhite)
    {
        const probe::Stats stats = g_reader.collect();
""", 1)

old = """    const unsigned int proxyMode = ProxyMode(cfg);
    const float whitePoint = proxyMode == 2
                                 ? cfg.DlssNrWhitePointScale.value_or_default()
                                 : (autoWhite && g_autoWhitePointSettled
                                        ? g_autoWhitePoint
                                        : cfg.DlssNrWhitePoint.value_or_default()) *
                                       cfg.DlssNrWhitePointScale.value_or_default();"""
assert old in d, 'white point'
d = d.replace(old, """    // The proxy is the frame divided by this and encoded: the scale control is the only thing acting
    // on it, which is what a paper-white control is.
    const float whitePoint = cfg.DlssNrWhitePointScale.value_or_default();""", 1)

old = """    encodeParams.curveMode = (proxyMode == 1 && !g_curve.ready) ? 2u : proxyMode;
    FillCurve(encodeParams, curveMatch);
"""
assert old in d, 'encode fill'
d = d.replace(old, "", 1)

old = """        resolveParams.curveMode = (proxyMode == 1 && !g_curve.ready) ? 2u : proxyMode;
        FillCurve(resolveParams, curveMatch);
"""
assert old in d, 'resolve fill'
d = d.replace(old, "", 1)

old = """    if ((autoWhite || curveMatch) && (g_frames % 30 == 0) && g_reducer.ensure(device))"""
assert old in d, 'reducer gate'
d = d.replace(old, """    if (autoWhite && (g_frames % 30 == 0) && g_reducer.ensure(device))""", 1)

# whatever remains of the pending-capture flag
i = d.find("        if (curveMatch)")
if i > 0:
    j = d.find("    }", i)
    d = d[:i] + d[j:]

# the present pass no longer needs to measure anything for a curve
old = """    // The proxy curve needs the finished frame's histogram whether or not this path runs the model.
    // An SDR backbuffer only: an scRGB frame is not in the model's world and would teach the wrong
    // curve. Cheap -- a copy, a reduction and a readback, twice a second.
    if (cfg.DlssNrProxyCurve.value_or_default() == 1 && g_splitActive)
    {
        MeasureFinishedFrame(queue, backBuffer, backBufferIndex);
    }

"""
if old in d:
    d = d.replace(old, "", 1)

assert 'g_curve' not in d and 'ProxyMode' not in d and 'FillCurve' not in d and 'MeasureFinishedFrame' not in d
assert 'ProxyCurveStatus' not in d and 'curveMatch' not in d
io.open(p, 'w', encoding='utf-8').write(d)
print('module done')

# ==================== HEADER ====================
p = 'OptiScaler/dlssnr/DlssNr_Dx12.h'
h = io.open(p, encoding='utf-8').read()
old = """
// Whether the game-matched proxy curve is measuring or matched; empty when Reinhard is in use.
const char* ProxyCurveStatus();"""
assert old in h
h = h.replace(old, "", 1)
io.open(p, 'w', encoding='utf-8').write(h)

# ==================== CONFIG ====================
p = 'OptiScaler/Config.h'
c = io.open(p, encoding='utf-8').read()
i = c.find("    CustomOptional<uint32_t> DlssNrProxyCurve { 2 };")
assert i > 0
j = c.find(chr(10), i) + 1
ls = i
while True:
    prev = c.rfind(chr(10), 0, ls - 1) + 1
    if c[prev:ls].lstrip().startswith('//'):
        ls = prev
    else:
        break
c = c[:ls] + c[j:]
assert 'ProxyCurve' not in c
io.open(p, 'w', encoding='utf-8').write(c)

p = 'OptiScaler/Config.cpp'
c = io.open(p, encoding='utf-8').read()
for old in ('            DlssNrProxyCurve.set_from_config(readUInt("DlssNr", "ProxyCurve"));' + chr(10),
            '    ini.SetValue("DlssNr", "ProxyCurve", GetIntValue(Instance()->DlssNrProxyCurve.value_for_config()).c_str());' + chr(10)):
    assert old in c
    c = c.replace(old, "", 1)
assert 'ProxyCurve' not in c
io.open(p, 'w', encoding='utf-8').write(c)
print('config done')

# ==================== MENU ====================
p = 'OptiScaler/dlssnr/DlssNr_Menu.cpp'
m = io.open(p, encoding='utf-8').read()
start = m.find("        {\n            static const char* curveNames[]")
if start < 0:
    start = m.find("            static const char* curveNames[]")
    start = m.rfind("        {", 0, start)
assert start > 0
endmark = 'ImGui::TextDisabled("%s", curveStatus);'
e = m.find(endmark, start)
assert e > start
e = m.find("        }", e) + len("        }") + 1
while m[e:e + 1] == chr(10):
    e += 1
m = m[:start] + m[e:]
assert 'ProxyCurve' not in m and 'curveNames' not in m
io.open(p, 'w', encoding='utf-8').write(m)
print('menu done')
