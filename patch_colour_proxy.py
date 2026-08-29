import io, re

# ==================== PROBE: RGB samples too ====================
p = 'OptiScaler/dlssnr/DlssNr_Probe.h'
t = io.open(p, encoding='utf-8').read()
old = "    Stats collect(float* lumas = nullptr)"
assert old in t
t = t.replace(old, "    Stats collect(float* lumas = nullptr, float* rgb = nullptr)", 1)
old = "                    lumas[y * kSide + x] = luma;"
assert old in t
t = t.replace(old, old + """

                if (rgb != nullptr)
                {
                    rgb[(y * kSide + x) * 3 + 0] = r;
                    rgb[(y * kSide + x) * 3 + 1] = g;
                    rgb[(y * kSide + x) * 3 + 2] = b;
                }""", 1)
io.open(p, 'w', encoding='utf-8').write(t)
print('probe in')

# ==================== CODEC: 24-entry curve + colour matrix ====================
p = 'OptiScaler/dlssnr/DlssNr_Codec.h'
t = io.open(p, encoding='utf-8').read()

old = """    uint  gCurveMode;    // 0: Reinhard against the white point. 1: the fitted curve below.
    float gCurveMinLog;  // log2 luminance of the curve's first entry
    float gCurveRangeLog; // log2 span across its 32 entries
    uint  gPad1;
    uint  gPad2;
    uint  gPad3;
    float4 gCurve[8];    // 32 toned (linear-display) luminance values, log-spaced in scene luminance
    float gRestoreSkipSkin; // 1: highlight restore leaves skin alone; 0: applies everywhere
};"""
assert old in t
t = t.replace(old, """    uint  gCurveMode;    // 0: Reinhard against the white point. 1: the fitted curve and matrix below.
    float gCurveMinLog;  // log2 luminance of the curve's first entry
    float gCurveRangeLog; // log2 span across its 24 entries
    float gRestoreSkipSkin; // 1: highlight restore leaves skin alone; 0: applies everywhere
    uint  gPad2;
    uint  gPad3;
    float4 gCurve[6];    // 24 toned (linear-display) luminance values, log-spaced in scene luminance
    float4 gMat[3];      // the game's colour transform after the curve, rows (w unused)
};

float3x3 CurveMatrix()
{
    return float3x3(gMat[0].xyz, gMat[1].xyz, gMat[2].xyz);
}

float3x3 Inverse3(float3x3 m)
{
    float3 c0 = cross(m[1], m[2]);
    float3 c1 = cross(m[2], m[0]);
    float3 c2 = cross(m[0], m[1]);
    float det = dot(m[0], c0);
    float3x3 adj = float3x3(c0.x, c1.x, c2.x, c0.y, c1.y, c2.y, c0.z, c1.z, c2.z);
    return abs(det) > 1e-6 ? adj / det : float3x3(1, 0, 0, 0, 1, 0, 0, 0, 1);
}""", 1)

old = """float CurveAt(int i)
{
    i = clamp(i, 0, 31);
    return gCurve[i >> 2][i & 3];
}

float CurvePos(float luma)
{
    float lg = log2(max(luma, 1e-6));
    return saturate((lg - gCurveMinLog) / max(gCurveRangeLog, 1e-4)) * 31.0;
}"""
assert old in t
t = t.replace(old, """float CurveAt(int i)
{
    i = clamp(i, 0, 23);
    return gCurve[i >> 2][i & 3];
}

float CurvePos(float luma)
{
    float lg = log2(max(luma, 1e-6));
    return saturate((lg - gCurveMinLog) / max(gCurveRangeLog, 1e-4)) * 23.0;
}""", 1)

old = """    float x = CurvePos(luma);
    int i = clamp((int) floor(x), 0, 30);
    float l0 = exp2(gCurveMinLog + gCurveRangeLog * (i / 31.0));
    float l1 = exp2(gCurveMinLog + gCurveRangeLog * ((i + 1) / 31.0));"""
assert old in t
t = t.replace(old, """    float x = CurvePos(luma);
    int i = clamp((int) floor(x), 0, 22);
    float l0 = exp2(gCurveMinLog + gCurveRangeLog * (i / 23.0));
    float l1 = exp2(gCurveMinLog + gCurveRangeLog * ((i + 1) / 23.0));""", 1)

# encode: the matrix after the luminance map
old = """        float luma = dot(frame, kLuma);
        float toned = gCurveMode != 0 ? CurveToned(luma) : (luma / gWhitePoint) / (1.0 + luma / gWhitePoint);
        float scale = luma > 1e-6 ? toned / luma : 0.0;

        gTarget[id.xy] = float4(LinearToSrgb(frame * scale), source.a);"""
assert old in t
t = t.replace(old, """        float luma = dot(frame, kLuma);
        float toned = gCurveMode != 0 ? CurveToned(luma) : (luma / gWhitePoint) / (1.0 + luma / gWhitePoint);
        float scale = luma > 1e-6 ? toned / luma : 0.0;
        float3 display = frame * scale;

        // The game's colour grade, learned: after the luminance map the fitted matrix turns the
        // picture into what the game would have made of it -- the model then sees the game's palette
        // as well as its contrast.
        if (gCurveMode != 0)
            display = max(mul(CurveMatrix(), display), float3(0.0, 0.0, 0.0));

        gTarget[id.xy] = float4(LinearToSrgb(display), source.a);""", 1)

# resolve: undo the matrix on the edit before it lands in linear light
old = """        float appliedLuma = dot(applied, kLuma);
        float3 appliedChroma = applied - appliedLuma;
        float gain = max(1.0 + appliedLuma * slope / originalLuma, 0.0);
        result = original * gain + appliedChroma * slope;"""
assert old in t
t = t.replace(old, """        // An edit made in the game's colour space is brought back through the inverse of the fitted
        // matrix before the luminance slope lands it in the scene.
        float3 appliedScene = gCurveMode != 0 ? mul(Inverse3(CurveMatrix()), applied) : applied;
        float appliedLuma = dot(appliedScene, kLuma);
        float3 appliedChroma = appliedScene - appliedLuma;
        float gain = max(1.0 + appliedLuma * slope / originalLuma, 0.0);
        result = original * gain + appliedChroma * slope;""", 1)

old = """    unsigned int curveMode;
    float curveMinLog;
    float curveRangeLog;
    unsigned int pad1;
    unsigned int pad2;
    unsigned int pad3;
    float curve[32];
    float restoreSkipSkin;
};"""
assert old in t
t = t.replace(old, """    unsigned int curveMode;
    float curveMinLog;
    float curveRangeLog;
    float restoreSkipSkin;
    unsigned int pad2;
    unsigned int pad3;
    float curve[24];
    float mat[12]; // three rows of four; the fourth is unused
};""", 1)
io.open(p, 'w', encoding='utf-8').write(t)
print('codec in')

# ==================== MODULE: paired samples, curve from bins, matrix by least squares ====================
p = 'OptiScaler/dlssnr/DlssNr_Dx12.cpp'
t = io.open(p, encoding='utf-8').read()

old = """struct ProxyCurve
{
    static constexpr unsigned int kSamples = probe::BlockReader::kSide * probe::BlockReader::kSide;
    std::vector<float> linear = std::vector<float>(kSamples, 0.0f);
    std::vector<float> finished = std::vector<float>(kSamples, 0.0f);
    bool linearFresh = false;
    bool finishedFresh = false;
    bool ready = false;
    unsigned int fits = 0;
    float minLog = -8.0f;
    float rangeLog = 12.0f;
    float toned[32] = {};
};"""
assert old in t
t = t.replace(old, """struct ProxyCurve
{
    static constexpr unsigned int kSamples = probe::BlockReader::kSide * probe::BlockReader::kSide;
    static constexpr int kEntries = 24;
    std::vector<float> linear = std::vector<float>(kSamples, 0.0f);
    std::vector<float> finished = std::vector<float>(kSamples, 0.0f);
    std::vector<float> linearRgb = std::vector<float>(kSamples * 3, 0.0f);
    std::vector<float> finishedRgb = std::vector<float>(kSamples * 3, 0.0f);
    bool linearFresh = false;
    bool finishedFresh = false;
    bool presentCapturePending = false; // the render path sampled this frame; the present samples the same one
    bool ready = false;
    unsigned int fits = 0;
    float minLog = -8.0f;
    float rangeLog = 12.0f;
    float toned[kEntries] = {};
    float mat[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
};""", 1)

# the fit: replace the whole function
start = t.find("// Histogram matching: the toned value for a scene luminance is the finished-frame luminance sitting")
end = t.find("static void FillCurve(codec::Params& params, bool wanted)")
assert 0 < start < end
t = t[:start] + r'''// The samples are the same 64x64 tiles of the same frame, once from the linear frame and once from
// the finished one, so every tile is a paired observation of what the game's post chain did to it.
// Two things are fitted: a luminance curve (the finished luminance averaged per log-luminance bin --
// UI and bloom average out as noise), and a 3x3 colour matrix on top of it by ridge-regularised least
// squares, which captures the grade's hue and saturation moves. Both are eased across fits.
static void FitProxyCurve()
{
    if (!g_curve.linearFresh || !g_curve.finishedFresh)
        return;

    g_curve.linearFresh = false;
    g_curve.finishedFresh = false;

    const unsigned int n = ProxyCurve::kSamples;
    const int K = ProxyCurve::kEntries;

    // Range from the linear frame's spread, trimmed.
    std::vector<float> lin = g_curve.linear;
    std::sort(lin.begin(), lin.end());
    const float lo = std::max(lin[n / 200], 1e-4f);
    const float hi = std::max(lin[n - 1 - n / 200], lo * 2.0f);
    const float minLog = log2f(lo);
    const float rangeLog = std::max(log2f(hi) - minLog, 0.5f);

    // Curve: finished luminance (linear-display) averaged per bin of scene luminance.
    std::vector<double> binSum(K, 0.0);
    std::vector<unsigned int> binCount(K, 0);

    for (unsigned int i = 0; i < n; ++i)
    {
        const float l = g_curve.linear[i];
        const float f = g_curve.finished[i];

        if (l <= 0.0f || f >= 0.985f) // clipped finished tiles say nothing about the curve
            continue;

        const float pos = (log2f(std::max(l, 1e-6f)) - minLog) / rangeLog * (K - 1);
        const int k = std::clamp((int) (pos + 0.5f), 0, K - 1);
        binSum[k] += SrgbToLinearCpu(std::clamp(f, 0.0f, 1.0f));
        ++binCount[k];
    }

    float toned[ProxyCurve::kEntries];
    int lastFilled = -1;

    for (int k = 0; k < K; ++k)
    {
        if (binCount[k] > 0)
        {
            toned[k] = (float) (binSum[k] / binCount[k]);

            // Fill any gap since the last filled bin by interpolation.
            if (lastFilled >= 0 && lastFilled < k - 1)
            {
                for (int j = lastFilled + 1; j < k; ++j)
                    toned[j] = toned[lastFilled] + (toned[k] - toned[lastFilled]) * (float) (j - lastFilled) / (float) (k - lastFilled);
            }
            else if (lastFilled < 0)
            {
                for (int j = 0; j < k; ++j)
                    toned[j] = toned[k] * (float) (j + 1) / (float) (k + 1);
            }

            lastFilled = k;
        }
    }

    if (lastFilled < 3)
        return; // too little to fit from

    for (int k = lastFilled + 1; k < K; ++k)
        toned[k] = toned[lastFilled];

    float last = 0.0f;

    for (int k = 0; k < K; ++k)
    {
        toned[k] = std::max(toned[k], last + 1e-4f);
        last = toned[k];
    }

    // Matrix: X = the tile's linear colour after the luminance map, Y = the finished tile in linear
    // display. Ridge toward identity, so a frame full of UI or bloom cannot produce nonsense.
    auto curveAt = [&](float l) {
        const float pos = std::clamp((log2f(std::max(l, 1e-6f)) - minLog) / rangeLog, 0.0f, 1.0f) * (K - 1);
        const int i = std::clamp((int) pos, 0, K - 2);
        const float f = pos - i;
        return toned[i] + (toned[i + 1] - toned[i]) * f;
    };

    double A[3][3] = {};
    double B[3][3] = {}; // B[c][j] = sum X_j * Y_c
    double xx = 0.0;
    unsigned int used = 0;

    for (unsigned int i = 0; i < n; ++i)
    {
        const float l = g_curve.linear[i];
        const float f = g_curve.finished[i];

        if (l <= 1e-4f || f >= 0.985f || f <= 0.01f)
            continue;

        const float scale = curveAt(l) / l;
        const double X[3] = { g_curve.linearRgb[i * 3 + 0] * scale, g_curve.linearRgb[i * 3 + 1] * scale,
                              g_curve.linearRgb[i * 3 + 2] * scale };
        const double Y[3] = { SrgbToLinearCpu(std::clamp(g_curve.finishedRgb[i * 3 + 0], 0.0f, 1.0f)),
                              SrgbToLinearCpu(std::clamp(g_curve.finishedRgb[i * 3 + 1], 0.0f, 1.0f)),
                              SrgbToLinearCpu(std::clamp(g_curve.finishedRgb[i * 3 + 2], 0.0f, 1.0f)) };

        for (int a = 0; a < 3; ++a)
        {
            for (int b = 0; b < 3; ++b)
                A[a][b] += X[a] * X[b];

            for (int c = 0; c < 3; ++c)
                B[c][a] += X[a] * Y[c];
        }

        xx += X[0] * X[0] + X[1] * X[1] + X[2] * X[2];
        ++used;
    }

    float mat[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };

    if (used >= 64)
    {
        const double lambda = 0.15 * xx / (double) used; // ridge weight, relative to the signal
        double R[3][3];

        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b)
                R[a][b] = A[a][b] + (a == b ? lambda * used : 0.0);

        // Invert R (3x3, symmetric positive definite after the ridge).
        const double det = R[0][0] * (R[1][1] * R[2][2] - R[1][2] * R[2][1]) -
                           R[0][1] * (R[1][0] * R[2][2] - R[1][2] * R[2][0]) +
                           R[0][2] * (R[1][0] * R[2][1] - R[1][1] * R[2][0]);

        if (std::abs(det) > 1e-12)
        {
            double inv[3][3];
            inv[0][0] = (R[1][1] * R[2][2] - R[1][2] * R[2][1]) / det;
            inv[0][1] = (R[0][2] * R[2][1] - R[0][1] * R[2][2]) / det;
            inv[0][2] = (R[0][1] * R[1][2] - R[0][2] * R[1][1]) / det;
            inv[1][0] = (R[1][2] * R[2][0] - R[1][0] * R[2][2]) / det;
            inv[1][1] = (R[0][0] * R[2][2] - R[0][2] * R[2][0]) / det;
            inv[1][2] = (R[0][2] * R[1][0] - R[0][0] * R[1][2]) / det;
            inv[2][0] = (R[1][0] * R[2][1] - R[1][1] * R[2][0]) / det;
            inv[2][1] = (R[0][1] * R[2][0] - R[0][0] * R[2][1]) / det;
            inv[2][2] = (R[0][0] * R[1][1] - R[0][1] * R[1][0]) / det;

            // Row c of M solves (A + lambda I) m = B_c + lambda e_c: the ridge pulls toward identity.
            for (int c = 0; c < 3; ++c)
            {
                double rhs[3] = { B[c][0], B[c][1], B[c][2] };
                rhs[c] += lambda * used;

                for (int j = 0; j < 3; ++j)
                    mat[c * 3 + j] = (float) (inv[j][0] * rhs[0] + inv[j][1] * rhs[1] + inv[j][2] * rhs[2]);
            }

            // Sanity: a grade is a modest rotation of the palette, never a wild one.
            const float mdet = mat[0] * (mat[4] * mat[8] - mat[5] * mat[7]) - mat[1] * (mat[3] * mat[8] - mat[5] * mat[6]) +
                               mat[2] * (mat[3] * mat[7] - mat[4] * mat[6]);

            if (mdet < 0.2f || mdet > 5.0f)
            {
                const float ident[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
                std::memcpy(mat, ident, sizeof(mat));
            }
        }
    }

    const float blend = g_curve.ready ? 0.3f : 1.0f;
    g_curve.minLog = g_curve.ready ? g_curve.minLog + (minLog - g_curve.minLog) * blend : minLog;
    g_curve.rangeLog = g_curve.ready ? g_curve.rangeLog + (rangeLog - g_curve.rangeLog) * blend : rangeLog;

    for (int k = 0; k < K; ++k)
        g_curve.toned[k] = g_curve.ready ? g_curve.toned[k] + (toned[k] - g_curve.toned[k]) * blend : toned[k];

    for (int j = 0; j < 9; ++j)
        g_curve.mat[j] = g_curve.ready ? g_curve.mat[j] + (mat[j] - g_curve.mat[j]) * blend : mat[j];

    if (!g_curve.ready)
        LOG_INFO("DLSS-NR proxy curve matched to the game: scene luminance {:.4f}..{:.2f}; colour matrix diag "
                 "{:.2f} {:.2f} {:.2f} from {} tiles",
                 lo, hi, mat[0], mat[4], mat[8], used);

    g_curve.ready = true;
    ++g_curve.fits;
}

''' + t[end:]

old = """    params.curveMode = 1;
    params.curveMinLog = g_curve.minLog;
    params.curveRangeLog = g_curve.rangeLog;
    std::memcpy(params.curve, g_curve.toned, sizeof(params.curve));"""
assert old in t
t = t.replace(old, """    params.curveMode = 1;
    params.curveMinLog = g_curve.minLog;
    params.curveRangeLog = g_curve.rangeLog;
    std::memcpy(params.curve, g_curve.toned, sizeof(params.curve));

    for (int r = 0; r < 3; ++r)
    {
        params.mat[r * 4 + 0] = g_curve.mat[r * 3 + 0];
        params.mat[r * 4 + 1] = g_curve.mat[r * 3 + 1];
        params.mat[r * 4 + 2] = g_curve.mat[r * 3 + 2];
        params.mat[r * 4 + 3] = 0.0f;
    }""", 1)

# render path: collect rgb too, and ask the present to sample the same frame
old = "        const probe::Stats stats = g_reader.collect(curveMatch ? g_curve.linear.data() : nullptr);"
assert old in t
t = t.replace(old, """        const probe::Stats stats = g_reader.collect(curveMatch ? g_curve.linear.data() : nullptr,
                                                     curveMatch ? g_curve.linearRgb.data() : nullptr);""", 1)
old = """    if ((autoWhite || curveMatch) && (g_frames % 30 == 0) && g_reducer.ensure(device))
    {
        ID3D12Resource* reducedFrame = g_reducer.dispatch(cmdList, target, width, height);
        g_reader.capture(cmdList, reducedFrame, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }"""
assert old in t
t = t.replace(old, """    if ((autoWhite || curveMatch) && (g_frames % 30 == 0) && g_reducer.ensure(device))
    {
        ID3D12Resource* reducedFrame = g_reducer.dispatch(cmdList, target, width, height);
        g_reader.capture(cmdList, reducedFrame, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        // The present that follows samples the finished version of this very frame.
        if (curveMatch)
            g_curve.presentCapturePending = true;
    }""", 1)

# present measurement: sample on request, rgb too
old = "    const probe::Stats stats = g_finishedReader.collect(g_curve.finished.data());"
assert old in t
t = t.replace(old, "    const probe::Stats stats = g_finishedReader.collect(g_curve.finished.data(), g_curve.finishedRgb.data());", 1)
old = """    if ((g_frames % 30) != 0)
        return;

    ID3D12Device* device = nullptr;

    if (FAILED(backBuffer->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
        return;

    const auto desc = backBuffer->GetDesc();"""
assert old in t
t = t.replace(old, """    if (!g_curve.presentCapturePending)
        return;

    g_curve.presentCapturePending = false;

    ID3D12Device* device = nullptr;

    if (FAILED(backBuffer->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
        return;

    const auto desc = backBuffer->GetDesc();""", 1)

old = """    return g_curve.ready ? "matched to the game's tonemapper" : "measuring the game's tonemapper...";"""
assert old in t
t = t.replace(old, """    return g_curve.ready ? "matched: the game's tone curve and colour matrix" : "measuring the game's tonemapper and grade...";""", 1)
io.open(p, 'w', encoding='utf-8').write(t)
print('module in')

p = 'OptiScaler/dlssnr/DlssNr_Menu.cpp'
m = io.open(p, encoding='utf-8').read()
old = '''            static const char* curveNames[] = { "Reinhard (white point above)", "Match the game's tonemapper" };'''
assert old in m
m = m.replace(old, '''            static const char* curveNames[] = { "Reinhard (white point above)", "Match the game's tonemapper and grade" };''', 1)
old = '''            HelpMarker("How the linear frame is compressed for the model, in the split and the DX11"
                       "\\nbridge -- the arrangements where the model sees the frame before the game's"
                       "\\ntonemapper. Reinhard is a generic guess. Match learns the game's own curve"
                       "\\nby comparing the linear frame with the finished one, twice a second, so the"
                       "\\nmodel is shown the game's actual contrast and shadow depth -- the statistics"
                       "\\nit was trained on -- and the edit is sized for the curve it will pass through."
                       "\\nEncode and resolve stay exact inverses either way."'''
assert old in m
m = m.replace(old, '''            HelpMarker("How the linear frame is compressed for the model, in the split and the DX11"
                       "\\nbridge -- the arrangements where the model sees the frame before the game's"
                       "\\ntonemapper. Reinhard is a generic guess. Match learns the game's own tone"
                       "\\ncurve AND its colour grade by comparing the same frame before and after the"
                       "\\ngame's post chain, twice a second, so the model is shown the game's actual"
                       "\\ncontrast, shadow depth and palette -- the statistics it was trained on --"
                       "\\nand its edit is brought back through the exact inverse. Bloom and grain are"
                       "\\nspatial and cannot be learned this way; the model does not see the glow."'''
, 1)
io.open(p, 'w', encoding='utf-8').write(m)
print('menu in')
