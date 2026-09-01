
cbuffer Params : register(b0)
{
    uint  gMode;
    float gWhitePoint;
    uint  gWidth;
    uint  gHeight;
    float gTransferStrength;
    float gColourStrength;
    uint  gDebugView;
    float gMaxRatio;
    uint  gPassthrough;
    float gMvScaleX;     // motion vector units -> pixels of this dispatch
    float gMvScaleY;
    uint  gGuideWidth;   // the motion texture's valid region
    uint  gGuideHeight;
    uint  gCompareMode;  // 0 off, 1 side by side, 2 wipe
    float gCompareSplit; // where the wipe cuts, 0..1
    float gCompareZoom;  // side by side: 1 fits the frame, 2 fills the half
    uint  gCompareSwap;  // put the edited frame on the other side
    uint  gTransfer;     // 0 Classic, 1 Matched residual
    uint  gHdrLift;      // 0 H0, 1 H1; ignored unless gTransfer == 1
};

// Colours outside the AP1 gamut are impossible on any display and read as sparkle where a bright
// saturated pixel is pushed further. Clamping inside AP1 and coming back keeps everything reachable.
float3 ClampAp1(float3 color)
{
    const float3x3 bt709_to_ap1 = { 0.613097, 0.339523, 0.047379,
                                    0.070194, 0.916354, 0.013452,
                                    0.020616, 0.109570, 0.869815 };
    const float3x3 ap1_to_bt709 = { 1.705051, -0.621792, -0.083259,
                                    -0.130256, 1.140805, -0.010548,
                                    -0.024003, -0.128969, 1.152972 };
    return mul(ap1_to_bt709, max(mul(bt709_to_ap1, color), float3(0.0, 0.0, 0.0)));
}

// ---------------------------------------------------------------------------------------------
// The composition below (UpgradeToneMap's two-branch ratio, the OkLab hue correction, and the blend
// between a luminance-only result and the model's own colour) is taken from RenoDX's DLSS 5 addon by
// clshortfuse -- https://github.com/clshortfuse/renodx. It is their design, not ours; see
// Licenses/RenoDX_LICENSE.txt. The OkLab matrices are Bjorn Ottosson's published constants and the
// AP1, sRGB and PQ transforms are standard colour science.
// ---------------------------------------------------------------------------------------------

// OkLab, so the model's colour can be reached without its hue being invented on the way. A ratio
// applied to an RGB triple does not move hue, but a difference added to one does -- which is what the
// old composition did, and why a warm subject could come back green. Here the result's chroma is
// rebuilt in the model's own hue direction and only its magnitude is taken from the scaled colour.
float3 CbrtSigned(float3 v) { return sign(v) * pow(abs(v), 1.0 / 3.0); }

float3 ToOkLab(float3 color)
{
    const float3x3 rgb_to_lms = { 0.4122214708, 0.5363325363, 0.0514459929,
                                  0.2119034982, 0.6806995451, 0.1073969566,
                                  0.0883024619, 0.2817188376, 0.6299787005 };
    const float3x3 lms_to_lab = { 0.2104542553, 0.7936177850, -0.0040720468,
                                  1.9779984951, -2.4285922050, 0.4505937099,
                                  0.0259040371, 0.7827717662, -0.8086757660 };
    return mul(lms_to_lab, CbrtSigned(mul(rgb_to_lms, color)));
}

float3 FromOkLab(float3 lab)
{
    const float3x3 lab_to_lms = { 1.0, 0.3963377774, 0.2158037573,
                                  1.0, -0.1055613458, -0.0638541728,
                                  1.0, -0.0894841775, -1.2914855480 };
    const float3x3 lms_to_rgb = { 4.0767416621, -3.3077115913, 0.2309699292,
                                  -1.2684380046, 2.6097574011, -0.3413193965,
                                  -0.0041960863, -0.7034186147, 1.7076147010 };
    float3 lms = mul(lab_to_lms, lab);
    return mul(lms_to_rgb, lms * lms * lms);
}

// Takes the hue and the chroma direction from `correct`, and only the chroma magnitude from
// `incorrect`. Scaling a colour by a luminance ratio changes how saturated it reads; this puts the
// saturation back where the model meant it without letting the hue drift.
float3 HueOkLab(float3 incorrect, float3 correct)
{
    float3 incorrectLab = ToOkLab(incorrect);
    const float3 correctLab = ToOkLab(correct);
    const float incorrectChroma = length(incorrectLab.yz);
    const float correctChroma = length(correctLab.yz);
    incorrectLab.yz = correctLab.yz * (correctChroma == 0.0 ? 1.0 : incorrectChroma / correctChroma);
    return ClampAp1(FromOkLab(incorrectLab));
}

Texture2D<float4>   gSource   : register(t0);  // encode: the frame. resolve: the proxy.
Texture2D<float4>   gModel    : register(t1);  // resolve: what the model returned.
Texture2D<float4>   gOriginal : register(t2);  // resolve: the untouched frame.
Texture2D<float4>   gProxy    : register(t3);  // resolve: colorCopy (full-res encoded proxy)
RWTexture2D<float4> gTarget   : register(u0);  // encode: the proxy. resolve: the frame.
RWTexture2D<float4> gKeep     : register(u1);  // encode: the untouched copy. resolve: the edit history.
SamplerState        gLinear   : register(s0);  // so the edit can be read at a different size

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);

// sRGB rather than a plain 2.2 power: it is what an SDR game buffer actually carries, and the model was
// trained on those.
float3 LinearToSrgb(float3 v)
{
    v = saturate(v);
    return lerp(v * 12.92, 1.055 * pow(max(v, 1e-8), 1.0 / 2.4) - 0.055, step(0.0031308, v));
}

float3 SrgbToLinear(float3 v)
{
    v = saturate(v);
    return lerp(v / 12.92, pow((v + 0.055) / 1.055, 2.4), step(0.04045, v));
}

float3 DecodeRgb(float3 rgb)
{
    return gPassthrough != 0 ? rgb : SrgbToLinear(rgb);
}

float3 CubeScaleResidual(float3 P, float3 T)
{
    if (gPassthrough != 0)
        return T;
    float3 d = T - P;
    float alpha = 1.0;
    [unroll] for (int c = 0; c < 3; ++c)
    {
        if (d[c] > 1e-6)
            alpha = min(alpha, (1.0 - P[c]) / d[c]);
        else if (d[c] < -1e-6)
            alpha = min(alpha, (0.0 - P[c]) / d[c]);
    }
    return P + saturate(alpha) * d;
}

// Live UpgradeToneMap body, parameterized. Empty-model gate is the caller's job.
float3 ComposeUpgrade(float3 H, float3 proxyRgb, float3 modelRgb)
{
    float originalLuma = dot(H, kLuma);
    float proxyLuma = dot(proxyRgb, kLuma);
    float modelLuma = dot(modelRgb, kLuma);
    float ratio;
    if (originalLuma < proxyLuma)
        ratio = originalLuma / max(proxyLuma, 1e-6);
    else
        ratio = (modelLuma + max(0.0, originalLuma - proxyLuma)) / modelLuma;
    float3 upgraded = lerp(H, HueOkLab(modelRgb * ratio, modelRgb), gTransferStrength);
    float upgradedLuma = dot(upgraded, kLuma);
    const float kRatioFloor = 1.0 / 512.0;
    float lumaRatio = clamp((upgradedLuma + kRatioFloor) / (originalLuma + kRatioFloor), 0.0, gMaxRatio);
    return lerp(H * lumaRatio, upgraded, gColourStrength);
}

// The edit at an arbitrary position, exactly as the resolve computes its own.
float3 EditAt(float2 uvq)
{
    float3 p = gSource.SampleLevel(gLinear, uvq, 0).rgb;
    float3 m = gModel.SampleLevel(gLinear, uvq, 0).rgb;

    if (gPassthrough == 0)
    {
        p = SrgbToLinear(p);
        m = SrgbToLinear(m);
    }

    return m - p;
}


[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    // Normalised, so the source may be any size relative to this dispatch.
    float2 uv = (float2(id.xy) + 0.5) / float2(gWidth, gHeight);

    if (gMode == 2)
    {
        if (gTransfer == 0)
        {
            gTarget[id.xy] = gSource.SampleLevel(gLinear, uv, 0);
            return;
        }

        uint srcW, srcH;
        gSource.GetDimensions(srcW, srcH);
        if (srcW == gWidth && srcH == gHeight)
        {
            gTarget[id.xy] = gSource.Load(int3(id.xy, 0));
            return;
        }

        const float x0 = ((float) id.x * (float) srcW) / (float) gWidth;
        const float x1 = ((float) (id.x + 1) * (float) srcW) / (float) gWidth;
        const float y0 = ((float) id.y * (float) srcH) / (float) gHeight;
        const float y1 = ((float) (id.y + 1) * (float) srcH) / (float) gHeight;
        const float area = (x1 - x0) * (y1 - y0);

        const int i0 = (int) floor(x0);
        const int i1 = (int) ceil(x1) - 1;
        const int j0 = (int) floor(y0);
        const int j1 = (int) ceil(y1) - 1;

        float3 acc = 0.0;
        for (int j = j0; j <= j1; ++j)
        {
            const int jj = clamp(j, 0, (int) srcH - 1);
            const float t0y = (float) j;
            const float aY = max(y0, t0y);
            const float bY = min(y1, t0y + 1.0);
            const float wy = max(bY - aY, 0.0);
            for (int i = i0; i <= i1; ++i)
            {
                const int ii = clamp(i, 0, (int) srcW - 1);
                const float t0x = (float) i;
                const float aX = max(x0, t0x);
                const float bX = min(x1, t0x + 1.0);
                const float w = max(bX - aX, 0.0) * wy;
                acc += gSource.Load(int3(ii, jj, 0)).rgb * w;
            }
        }

        const int acx = clamp((int) floor(((float) id.x + 0.5) * (float) srcW / (float) gWidth), 0, (int) srcW - 1);
        const int acy = clamp((int) floor(((float) id.y + 0.5) * (float) srcH / (float) gHeight), 0, (int) srcH - 1);
        const float a = gSource.Load(int3(acx, acy, 0)).a;
        gTarget[id.xy] = float4(acc / area, a);
        return;
    }

    if (gMode == 0)
    {
        float4 source = gSource.Load(int3(id.xy, 0));
        float3 frame = max(source.rgb, float3(0.0, 0.0, 0.0));

        // Kept so the resolve has the frame as it was, rather than having to reconstruct it.
        gKeep[id.xy] = float4(frame, source.a);

        // Some games hand DLSS a frame that has already been through their tonemapper. The game says
        // which in its own DLSS creation flags, and converting one that needs no conversion is pure
        // damage, so it goes through untouched.
        if (gPassthrough != 0)
        {
            gTarget[id.xy] = float4(frame, source.a);
            return;
        }

        // What the model is shown. Mode 2 -- the default -- scales the frame and encodes it, and that
        // is all: the game is going to tone map this picture later, so tone mapping it here as well
        // shows the model a doubly compressed image. Measured against Cyberpunk's own numbers, the
        // Reinhard proxy handed the model a scene value of 1.0 as 0.55 and 1.5 as 0.64 -- flat, dark,
        // and nothing like the finished frame it was trained on. The model then synthesised weakly,
        // judged tone on a picture that does not exist, and its answer had to be un-crushed on the way
        // back. Mode 0 keeps that old curve, mode 1 the fitted one.
        float luma = dot(frame, kLuma);
        float3 display = frame / max(gWhitePoint, 1e-4);

        // A soft knee instead of a hard ceiling. Anything the curve leaves above 0.75 is rolled off
        // rather than clipped, so the model is never shown a field of flat white whose blown pixels
        // flip between frames -- unstable input is unstable output, and this is where a bright scene
        // would produce it.
        float displayLuma = dot(display, kLuma);

        if (displayLuma > 0.75)
        {
            float rolled = 0.75 + 0.25 * (1.0 - exp(-(displayLuma - 0.75) / 0.25));
            display *= rolled / displayLuma;
        }

        gTarget[id.xy] = float4(LinearToSrgb(display), source.a);
        return;
    }

    // Comparison, decided before anything is read, because side by side changes which part of the
    // frame this pixel is showing rather than just which version of it.
    //
    //   1  side by side  each half carries the whole frame, so both are squeezed horizontally
    //   2  wipe          one frame cut at the split, nothing resampled
    //
    // Neither needs the menu open to stay up. The wipe's split is a setting like any other; the menu
    // is only how you drag it.
    float2 cmpUv = uv;
    bool showOriginal = false;
    bool onDivider = false;
    bool outsideFrame = false;

    if (gCompareMode == 1)
    {
        showOriginal = (uv.x < 0.5) != (gCompareSwap != 0);

        // Each half is half as wide as the frame and just as tall, so the frame cannot fill it and
        // keep its shape. Stretching it to fit is what made both sides look squashed. Fitting it
        // properly leaves the halves letterboxed, which is the honest way round: a comparison that
        // changes the shape of what it is comparing is not showing you the picture.
        //
        // Zoom decides which is given up. At 1 the whole frame is there at its right proportions
        // with bars above and below; at 2 the half is filled and the sides are cropped away.
        float2 half2 = float2(uv.x < 0.5 ? uv.x * 2.0 : (uv.x - 0.5) * 2.0, uv.y) - 0.5;
        cmpUv = float2(0.5 + half2.x / gCompareZoom, 0.5 + half2.y * 2.0 / gCompareZoom);

        outsideFrame = cmpUv.x < 0.0 || cmpUv.x > 1.0 || cmpUv.y < 0.0 || cmpUv.y > 1.0;
        onDivider = abs(uv.x - 0.5) < (1.0 / max(gWidth, 1u));
    }
    else if (gCompareMode == 2)
    {
        showOriginal = (uv.x < gCompareSplit) != (gCompareSwap != 0);
        onDivider = abs(uv.x - gCompareSplit) < (1.0 / max(gWidth, 1u));
    }

    // Sampled rather than loaded: when the model ran at a reduced resolution these are smaller than the
    // frame, and its edit is enlarged here while the frame underneath stays untouched.
    float3 p = DecodeRgb(gSource.SampleLevel(gLinear, cmpUv, 0).rgb);
    float3 m = DecodeRgb(gModel.SampleLevel(gLinear, cmpUv, 0).rgb);
    float4 originalSample = gCompareMode == 1 ? gOriginal.SampleLevel(gLinear, cmpUv, 0)
                                              : gOriginal.Load(int3(id.xy, 0));
    const float normScale = gPassthrough != 0 ? 1.0 : max(gWhitePoint, 1e-4);
    float3 H = originalSample.rgb / normScale;

    if (gDebugView == 1)
    {
        gTarget[id.xy] = float4(p * gWhitePoint, originalSample.a);
        return;
    }
    if (gDebugView == 2)
    {
        gTarget[id.xy] = float4(m * gWhitePoint, originalSample.a);
        return;
    }
    if (gDebugView == 3)
    {
        float3 shown = saturate(0.5 + (m - p) * 20.0);
        gTarget[id.xy] = float4(SrgbToLinear(shown) * gWhitePoint, originalSample.a);
        return;
    }

    if (gDebugView == 4 || gDebugView == 5)
    {
        float3 shown = 0.0;
        if (gTransfer == 1)
        {
            float4 eSample = gCompareMode == 1 ? gProxy.SampleLevel(gLinear, cmpUv, 0)
                                               : gProxy.Load(int3(id.xy, 0));
            float3 P = DecodeRgb(eSample.rgb);
            uint srcW, srcH;
            gSource.GetDimensions(srcW, srcH);
            bool sameRate = (srcW == gWidth && srcH == gHeight);
            float3 T = sameRate ? m : CubeScaleResidual(P, P + (m - p));
            shown = (gDebugView == 4 ? P : T) * gWhitePoint;
        }
        gTarget[id.xy] = float4(shown, originalSample.a);
        return;
    }

    if (gTransferStrength == 0)
    {
        float4 o = gCompareMode == 1 ? gOriginal.SampleLevel(gLinear, cmpUv, 0)
                                     : gOriginal.Load(int3(id.xy, 0));
        float3 rgb = o.rgb;
        if (outsideFrame)
            rgb = 0.0;
        if (onDivider)
            rgb = gWhitePoint;
        gTarget[id.xy] = float4(rgb, o.a);
        return;
    }

    float3 result;
    if (gTransfer == 0)
    {
        if (dot(m, kLuma) <= 1e-5)
            result = H * normScale;
        else
            result = ComposeUpgrade(H, p, m) * normScale;
    }
    else
    {
        float4 eSample = gCompareMode == 1 ? gProxy.SampleLevel(gLinear, cmpUv, 0)
                                           : gProxy.Load(int3(id.xy, 0));
        float3 P = DecodeRgb(eSample.rgb);
        uint srcW, srcH;
        gSource.GetDimensions(srcW, srcH);
        bool sameRate = (srcW == gWidth && srcH == gHeight);
        float3 T = sameRate ? m : CubeScaleResidual(P, P + (m - p));

        if (dot(m, kLuma) <= 1e-5)
            result = H * normScale;
        else if (gHdrLift == 1)
            result = lerp(H, H + (T - P), gTransferStrength) * normScale;
        else
            result = ComposeUpgrade(H, P, T) * normScale;
    }

    if (showOriginal)
        result = originalSample.rgb;
    if (outsideFrame)
        result = 0.0;
    if (onDivider)
        result = gWhitePoint;
    gTarget[id.xy] = float4(max(result, 0.0), originalSample.a);
}
