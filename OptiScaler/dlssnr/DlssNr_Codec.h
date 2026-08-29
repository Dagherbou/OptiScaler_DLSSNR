// Turns the upscaler's linear HDR output into the kind of picture Neural Rendering was trained on, and
// folds the model's answer back into the frame.
//
// Two passes:
//
//   ENCODE   linear HDR -> an sRGB-encoded picture in [0,1], plus an untouched copy of the frame. The
//            picture is what the model is shown; the copy is what the answer is folded back into.
//
//   RESOLVE  proxy + model output + that copy -> the finished frame.
//
// The first version of this decoded the model's output back through the inverse of the tone curve, and
// that is what turned every strip light in Cyberpunk into a string of coloured cells. Two reasons, both
// fatal on highlights:
//
//   * The curve was applied per channel, so a saturated bright light had its channels compressed by
//     different amounts and came back a different hue.
//
//   * x/(1-x) diverges as x approaches one. A light sitting at 0.99 in the encoded picture reconstructs
//     to a hundred times the white point, and the model nudging one channel by a thousandth moves that
//     by tens of percent. Highlights are exactly where the model has least to say and where the inverse
//     amplifies most, which is the worst possible combination.
//
// So nothing is reconstructed by inversion any more. The encode maps luminance and carries chroma along
// unchanged, so hue survives. The resolve keeps the original frame and adds the model's edit to it,
// scaled by the local slope of the curve -- which for Reinhard against a white point works out to the
// tidy (whitePoint + luminance), so a one percent edit on a bright light stays a one percent edit. At
// zero edit the frame is bit-for-bit what the upscaler produced.
//
// On top of that the edit is rolled off as the proxy approaches white, and the total change is clamped
// to a ratio of the original. Neither should be load-bearing given the above; they are there because a
// detail pass has no business restyling a light source, whatever the model returns.

#pragma once

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>

namespace codec
{
constexpr int MODE_ENCODE = 0;
constexpr int MODE_RESOLVE = 1;
// Shrinks the frame so the model can work on fewer pixels. Filtered, not point sampled: the guidance is
// explicit that a nearest-neighbour enlargement of this pass turns into harsh aliasing.
constexpr int MODE_DOWNSAMPLE = 2;
// Writes the HUD mask into the frame copy's alpha (see the shader).
constexpr int MODE_HUDMASK = 3;

// Debug views, so the model's contribution can be looked at rather than guessed at.
constexpr int DEBUG_OFF = 0;
constexpr int DEBUG_PROXY = 1;      // the picture the model was shown
constexpr int DEBUG_MODEL = 2;      // the model's raw answer
constexpr int DEBUG_DIFFERENCE = 3; // what it changed, amplified

inline const char* kShaderSource = R"(
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
    uint  gAccumulate;   // 0 off, 1 blend with the reprojected history, 2 restart the history
    float gMvScaleX;     // motion vector units -> pixels of this dispatch
    float gMvScaleY;
    uint  gGuideWidth;   // the motion texture's valid region
    uint  gGuideHeight;
    float gStability;    // how much of the history survives each frame; 0 is off
    float gProtectHighlights; // the top fraction of the range where the edit fades out; 0 is off
    float gHudDetect;    // strength of the HUD mask carried in the original's alpha; 0 is off
    float gShadowRestore; // pulls back the brightening of dark regions; 0 is off
    uint  gCurveMode;    // 0: Reinhard against the white point. 1: the fitted curve and matrix below.
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
}

// A soft skin-tone classifier on gamma-space colour: the classic YCbCr region skin occupies. Not a
// person detector -- wood and sand can qualify -- but for deciding where a restore should hold back it
// errs in the harmless direction.
float SkinWeight(float3 rgb)
{
    float y = 0.299 * rgb.r + 0.587 * rgb.g + 0.114 * rgb.b;
    float cb = 0.5 - 0.168736 * rgb.r - 0.331264 * rgb.g + 0.5 * rgb.b;
    float cr = 0.5 + 0.5 * rgb.r - 0.418688 * rgb.g - 0.081312 * rgb.b;
    float inCr = smoothstep(0.50, 0.53, cr) * (1.0 - smoothstep(0.66, 0.70, cr));
    float inCb = smoothstep(0.28, 0.31, cb) * (1.0 - smoothstep(0.48, 0.52, cb));
    float lit = smoothstep(0.10, 0.20, y);
    return inCr * inCb * lit;
}

// The fitted proxy curve: what the game's own tonemapper does to luminance, learned by matching the
// linear frame's histogram to the finished frame's. Log-spaced in scene luminance, linear in between.
float CurveAt(int i)
{
    i = clamp(i, 0, 23);
    return gCurve[i >> 2][i & 3];
}

float CurvePos(float luma)
{
    float lg = log2(max(luma, 1e-6));
    return saturate((lg - gCurveMinLog) / max(gCurveRangeLog, 1e-4)) * 23.0;
}

float CurveToned(float luma)
{
    float x = CurvePos(luma);
    int i = (int) floor(x);
    return lerp(CurveAt(i), CurveAt(i + 1), x - i);
}

// d(scene luminance) / d(toned luminance) at this luminance: the slope the resolve needs to land an
// edit made in the toned picture as the equivalent edit in the scene.
float CurveSlope(float luma)
{
    float x = CurvePos(luma);
    int i = clamp((int) floor(x), 0, 22);
    float l0 = exp2(gCurveMinLog + gCurveRangeLog * (i / 23.0));
    float l1 = exp2(gCurveMinLog + gCurveRangeLog * ((i + 1) / 23.0));
    float dT = max(CurveAt(i + 1) - CurveAt(i), 1e-5);
    return (l1 - l0) / dT;
}

Texture2D<float4>   gSource   : register(t0);  // encode: the frame. resolve: the proxy.
Texture2D<float4>   gModel    : register(t1);  // resolve: what the model returned.
Texture2D<float4>   gOriginal : register(t2);  // resolve: the untouched frame.
Texture2D<float4>   gMotion   : register(t3);  // resolve, accumulating: the game's motion vectors.
Texture2D<float4>   gPrevEdit : register(t4);  // resolve, accumulating: last frame's accumulated edit.
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

)" R"(
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    // Normalised, so the source may be any size relative to this dispatch.
    float2 uv = (float2(id.xy) + 0.5) / float2(gWidth, gHeight);

    if (gMode == 2)
    {
        gTarget[id.xy] = gSource.SampleLevel(gLinear, uv, 0);
        return;
    }

    if (gMode == 3)
    {
        // The HUD mask, written into the frame copy's alpha. The interface is what stays put while
        // the world around it moves: a pixel unchanged from last frame under a motion vector that
        // says it should have changed. Exact where the game tags its UI layer (gModel's alpha, when
        // gAccumulate says one arrived). The estimate rises quickly and decays slowly, so it holds
        // through pauses in movement instead of flickering the interface in and out.
        float4 cur = gTarget[id.xy];
        float4 prev = gSource.Load(int3(id.xy, 0));
        float2 mv = gMotion.Load(int3(uv * float2(gGuideWidth, gGuideHeight), 0)).xy *
                    float2(gMvScaleX, gMvScaleY);
        float moving = saturate((length(mv) - 0.5) / 2.0);
        float same = 1.0 - saturate(length(cur.rgb - prev.rgb) / 0.02);
        float detect = moving * same;

        if (gAccumulate == 1)
            detect = max(detect, gModel.Load(int3(id.xy, 0)).a);

        float mask = detect > prev.a ? lerp(prev.a, detect, 0.25) : prev.a * 0.98;
        gKeep[id.xy] = float4(cur.rgb, mask);
        gTarget[id.xy] = float4(cur.rgb, mask);
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

        // Reinhard on luminance alone, with chroma carried along untouched. Compressing each channel
        // separately is what shifted the hue of every saturated highlight.
        float luma = dot(frame, kLuma);
        float toned = gCurveMode != 0 ? CurveToned(luma) : (luma / gWhitePoint) / (1.0 + luma / gWhitePoint);
        float scale = luma > 1e-6 ? toned / luma : 0.0;
        float3 display = frame * scale;

        // The game's colour grade, learned: after the luminance map the fitted matrix turns the
        // picture into what the game would have made of it -- the model then sees the game's palette
        // as well as its contrast.
        if (gCurveMode != 0)
            display = max(mul(CurveMatrix(), display), float3(0.0, 0.0, 0.0));

        gTarget[id.xy] = float4(LinearToSrgb(display), source.a);
        return;
    }

    // Sampled rather than loaded: when the model ran at a reduced resolution these are smaller than the
    // frame, and its edit is enlarged here while the frame underneath stays untouched.
    float4 proxySample = gSource.SampleLevel(gLinear, uv, 0);
    float4 modelSample = gModel.SampleLevel(gLinear, uv, 0);

    // Nothing was encoded on the way in, so nothing is decoded here either.
    float3 proxy = gPassthrough != 0 ? proxySample.rgb : SrgbToLinear(proxySample.rgb);
    float3 model = gPassthrough != 0 ? modelSample.rgb : SrgbToLinear(modelSample.rgb);
    float4 originalSample = gOriginal.Load(int3(id.xy, 0));
    float3 original = originalSample.rgb;

    // The slope of the encode at this pixel, so an edit made in the compressed picture lands as the
    // equivalent edit in the original. For Reinhard against a white point this is exactly
    // whitePoint + luminance -- bounded everywhere, unlike the inverse of the curve.
    float originalLuma = dot(original, kLuma);
    // With no curve applied there is no slope to undo: the edit lands as it is.
    float slope = gPassthrough != 0 ? 1.0
                  : (gCurveMode != 0 ? CurveSlope(originalLuma) : gWhitePoint + originalLuma);

    if (gDebugView == 1)
    {
        gTarget[id.xy] = float4(proxy * gWhitePoint, originalSample.a);
        return;
    }

    if (gDebugView == 2)
    {
        gTarget[id.xy] = float4(model * gWhitePoint, originalSample.a);
        return;
    }

    float3 edit = model - proxy;

    // Coring was tried here and removed: the per-frame churn's amplitude overlaps the real detail's,
    // so an amplitude threshold cannot separate them -- it only relocated the noise to the threshold.

    if (gDebugView == 4)
    {
        // The HUD mask as the resolve will use it: white where the edit is held off, the frame
        // dimmed underneath so the mask reads against it. Shows exactly what HUD detection found.
        float m = saturate(originalSample.a);
        float3 dimmed = original * 0.25;
        gTarget[id.xy] = float4(lerp(dimmed, float3(1.0, 1.0, 1.0), m * (gHudDetect > 0.0 ? 1.0 : 0.0)), 1.0);
        return;
    }

    if (gDebugView == 3)
    {
        // Amplified and centred on grey, so both directions of the edit are visible at once.
        float3 shown = saturate(0.5 + edit * 20.0);
        gTarget[id.xy] = float4(SrgbToLinear(shown) * gWhitePoint, originalSample.a);
        return;
    }

    // The edit, averaged over time. The model re-decides a measurable fraction of its answer every
    // frame even on a static scene; blending each frame's edit with its own reprojected history keeps
    // the consistent part -- the detail -- and cancels the part that re-randomises. NVIDIA's own
    // motion vectors carry the history to where the surface is now.
)" R"(
    if (gAccumulate == 1 || gAccumulate == 2)
    {
        // Only the lighting is accumulated. Two unrelated temporal filters -- the hand-made variance
        // clip and a trained DLAA pass -- both under-stabilised the detail band and both ghosted on
        // it, which settles the question: the edit's detail is re-decided with the framing rather
        // than attached to surfaces, so reprojecting it mixes genuinely different answers. History
        // cannot fix that band; routing the pass through a real upscaler accumulator (the split)
        // can. The lighting band is smooth and forgiving, and its accumulation measurably kills the
        // pumping without ghosts -- so that is all this does. History: alpha = lighting, rgb unused.
        float2 px = 1.0 / float2(gWidth, gHeight);
        float lowNow = dot(edit, kLuma);

        const float2 kWide[8] = { float2(-0.7, -0.2), float2(0.6, -0.6), float2(0.2, 0.7),
                                  float2(-0.5, 0.5),  float2(0.9, 0.1),  float2(-0.9, -0.6),
                                  float2(0.1, -0.9),  float2(0.5, 0.3) };

        {
            [unroll]
            for (int i = 0; i < 8; ++i)
                lowNow += dot(EditAt(uv + kWide[i] * 12.0 * px), kLuma);

            lowNow /= 9.0;
        }

        float accumulatedLow = lowNow;

        if (gAccumulate == 1)
        {
            float2 mv = gMotion.Load(int3(uv * float2(gGuideWidth, gGuideHeight), 0)).xy *
                        float2(gMvScaleX, gMvScaleY);
            float2 uvPrev = uv + mv / float2(gWidth, gHeight);

            if (all(uvPrev >= 0.0) && all(uvPrev <= 1.0))
            {
                // Where the motion field disagrees with itself across this band's footprint -- a car
                // against a streaming road -- history dies outright: hold and clamp both collapse, so
                // nothing trails. Where the field is coherent, the hold is strong and the pumping
                // cannot survive it.
                float divergence = 0.0;

                [unroll]
                for (int k = 0; k < 4; ++k)
                {
                    float2 tapUv = uv + kWide[k * 2] * 12.0 * px;
                    float2 mvTap = gMotion.Load(int3(tapUv * float2(gGuideWidth, gGuideHeight), 0)).xy *
                                   float2(gMvScaleX, gMvScaleY);
                    divergence = max(divergence, length(mvTap - mv));
                }

                float coherent = 1.0 - saturate(divergence / 2.0);
                float lowHold = coherent * max(gStability, 0.9);
                float clampWidth = lerp(0.015, 0.10, coherent);

                float prevLow = gPrevEdit.SampleLevel(gLinear, uvPrev, 0).a;
                prevLow = clamp(prevLow, lowNow - clampWidth, lowNow + clampWidth);
                accumulatedLow = lerp(lowNow, prevLow, lowHold);
            }
        }

        gKeep[id.xy] = float4(0.0, 0.0, 0.0, accumulatedLow);
        edit += accumulatedLow - lowNow;
    }

    // Split so the detail the model synthesised and any colour it shifted can be dialled apart.
    float lumaEdit = dot(edit, kLuma);
    float3 colourEdit = edit - lumaEdit;
    float3 applied = lumaEdit * gTransferStrength + colourEdit * gColourStrength;

    // Highlight restore. The model's trained instinct is to calm bright things -- not only the
    // near-clipped peak but the whole glow around a lamp -- and that reads as muted, in SDR too.
    // The achromatic darkening of bright regions is pulled back, scaled by how bright the original
    // is; colour shifts, brightening and structure pass untouched, so the model's detail stays.
    // 0 is off; 1 removes all darkening from the brightest regions.
    if (gProtectHighlights > 0.0 || gShadowRestore > 0.0)
    {
        float relLuma = saturate(dot(original, kLuma) / max(gWhitePoint, 1e-4));
        float appliedLuma = dot(applied, kLuma);

        // Highlight restore: the model's darkening of bright regions is pulled back -- except on skin,
        // when asked: the model's softening of a lit face is its skin work, not a muted highlight.
        float skinHold = 1.0;

        if (gRestoreSkipSkin > 0.0)
        {
            float3 gammaRgb = gPassthrough != 0 ? proxy : LinearToSrgb(saturate(proxy));
            skinHold = 1.0 - SkinWeight(gammaRgb) * gRestoreSkipSkin;
        }

        if (appliedLuma < 0.0)
            applied -= appliedLuma * gProtectHighlights * smoothstep(0.25, 0.9, relLuma) * skinHold;

        // Shadow restore, the mirror: the model lifts dark regions toward its trained idea of a
        // well-exposed picture, and that lift is the other half of the washed-out look -- the alley
        // that lost its darkness. Brightening is pulled back where the original is dark; detail and
        // colour pass, and the scene keeps its drama.
        if (appliedLuma > 0.0)
            applied -= appliedLuma * gShadowRestore * (1.0 - smoothstep(0.05, 0.45, relLuma));
    }

    // No highlight rolloff. It was a second belt after the clamp below, and it discarded the model's
    // contribution exactly where a lit scene carries its punch -- the two inject points now apply the
    // edit identically, with the clamp as the one safety in both.

    // HUD detection. At the finished frame the interface is part of the picture, and the model's own
    // UI correction is NVIDIA's to tune, not ours. The original's alpha carries a mask -- exact where
    // the game tags its UI layer through Streamline, estimated elsewhere as "unchanged while the world
    // moved" -- and the edit fades where the mask says interface. The hudless and split arrangements
    // never see the UI at all. 0 is off.
    if (gHudDetect > 0.0)
        applied *= 1.0 - saturate(originalSample.a) * gHudDetect;

    float3 result;

    if (gPassthrough != 0 || originalLuma < 0.002)
    {
        // Display space (or near black): an offset lands as an offset.
        result = original + applied * slope;
    }
    else
    {
        // Linear light: an achromatic offset is a colour change -- adding equal amounts to R, G and B
        // pulls every colour toward grey, and the game's per-channel tone curve then bends whatever
        // remains. That was the split's colour drift. The luminance part of the edit is applied as a
        // ratio instead, which preserves chromaticity exactly; the colour part stays additive.
        // An edit made in the game's colour space is brought back through the inverse of the fitted
        // matrix before the luminance slope lands it in the scene.
        float3 appliedScene = gCurveMode != 0 ? mul(Inverse3(CurveMatrix()), applied) : applied;
        float appliedLuma = dot(appliedScene, kLuma);
        float3 appliedChroma = appliedScene - appliedLuma;
        float gain = max(1.0 + appliedLuma * slope / originalLuma, 0.0);
        result = original * gain + appliedChroma * slope;
    }

    // A detail pass should not be able to restyle anything, whatever comes back -- and that includes
    // restyling by accident. The old clamp bounded each channel separately, and on a saturated pixel
    // the smallest channel hits its bound first: an achromatic edit lands as a hue shift, and a
    // magenta-graded scene bends green. The bound is enforced on luminance instead, and the whole
    // edit is scaled by one factor -- what lands is a smaller version of the same change, never a
    // bent colour. The small constant keeps the bound meaningful near black.
    float resultLuma = dot(result, kLuma);
    float ceilingLuma = originalLuma * gMaxRatio + 0.01;
    float floorLuma = max(originalLuma / gMaxRatio - 0.01, 0.0);
    float editLuma2 = resultLuma - originalLuma;
    float limit = 1.0;

    if (resultLuma > ceilingLuma && editLuma2 > 1e-6)
        limit = (ceilingLuma - originalLuma) / editLuma2;
    else if (resultLuma < floorLuma && editLuma2 < -1e-6)
        limit = (floorLuma - originalLuma) / editLuma2;

    result = original + applied * slope * saturate(limit);
    gTarget[id.xy] = float4(max(result, float3(0.0, 0.0, 0.0)), gHudDetect > 0.0 ? 1.0 : originalSample.a);
}
)";

struct Params
{
    unsigned int mode;
    float whitePoint;
    unsigned int width;
    unsigned int height;
    float transferStrength;
    float colourStrength;
    unsigned int debugView;
    float maxRatio;
    // Set when the game's own buffer is already tone-mapped, in which case there is nothing to convert.
    unsigned int passthrough;
    // 0 off, 1 blend with the reprojected history, 2 restart the history.
    unsigned int accumulate;
    float mvScaleX;
    float mvScaleY;
    unsigned int guideWidth;
    unsigned int guideHeight;
    float stability;
    float protectHighlights;
    float hudDetect;
    float shadowRestore;
    unsigned int curveMode;
    float curveMinLog;
    float curveRangeLog;
    float restoreSkipSkin;
    unsigned int pad2;
    unsigned int pad3;
    float curve[24];
    float mat[12]; // three rows of four; the fourth is unused
};

static_assert(sizeof(Params) % 4 == 0, "root constants are dwords");
static_assert(sizeof(Params) / 4 <= 60, "root constants must leave room for the descriptor table");

// A typeless resource cannot be viewed, and the buffer the upscaler writes is occasionally declared that
// way, so the typed member of the same family is substituted.
inline DXGI_FORMAT TypedFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        // The sRGB view cannot be bound as a typed UAV, and the shader does its own transfer function.
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    default:
        return f;
    }
}

// Owns the compute pipeline and the descriptors both passes need. The dispatches are recorded onto the
// caller's command list, so there is no queue or fence to manage here.
class Codec
{
  public:
    bool ensure(ID3D12Device* device)
    {
        if (pipeline_ != nullptr)
            return true;

        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;

        if (FAILED(D3DCompile(kShaderSource, strlen(kShaderSource), nullptr, nullptr, nullptr, "main",
                              "cs_5_1", 0, 0, &code, &errors)))
        {
            if (errors != nullptr)
                errors->Release();

            return false;
        }

        if (errors != nullptr)
            errors->Release();

        D3D12_DESCRIPTOR_RANGE ranges[2] = {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 5; // proxy, model, original, motion, previous edit
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 2; // result, kept copy
        ranges[1].OffsetInDescriptorsFromTableStart = 5;

        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 2;
        params[0].DescriptorTable.pDescriptorRanges = ranges;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.Num32BitValues = sizeof(Params) / 4;

        D3D12_STATIC_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
        rootDesc.NumParameters = 2;
        rootDesc.pParameters = params;
        rootDesc.NumStaticSamplers = 1;
        rootDesc.pStaticSamplers = &sampler;

        ID3DBlob* serialized = nullptr;

        if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr)))
        {
            code->Release();
            return false;
        }

        HRESULT hr = device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                 IID_PPV_ARGS(&root_));
        serialized->Release();

        if (FAILED(hr))
        {
            code->Release();
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
        pso.pRootSignature = root_;
        pso.CS.pShaderBytecode = code->GetBufferPointer();
        pso.CS.BytecodeLength = code->GetBufferSize();
        hr = device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipeline_));
        code->Release();

        if (FAILED(hr))
            return false;

        // Five descriptors per dispatch, two dispatches a frame; a ring of eight keeps a frame's
        // descriptors from being overwritten while it is still in flight.
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = kRingSlots * kPerDispatch;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

        if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap_))))
            return false;

        stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        device_ = device;
        return true;
    }

    // Every texture must already be in the state its slot needs: sources shader-readable, targets
    // writable. Slots a pass does not read still have to be populated, or the descriptor is undefined.
    void dispatch(ID3D12GraphicsCommandList* cmd, const Params& constants, ID3D12Resource* source,
                  ID3D12Resource* model, ID3D12Resource* original, ID3D12Resource* target,
                  ID3D12Resource* keep, ID3D12Resource* motion = nullptr,
                  ID3D12Resource* prevEdit = nullptr)
    {
        if (pipeline_ == nullptr)
            return;

        const unsigned int slot = ring_;
        ring_ = (ring_ + 1) % kRingSlots;

        D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap_->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += (SIZE_T) slot * kPerDispatch * stride_;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap_->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += (UINT64) slot * kPerDispatch * stride_;

        ID3D12Resource* srvs[5] = { source, model != nullptr ? model : source,
                                    original != nullptr ? original : source,
                                    motion != nullptr ? motion : source,
                                    prevEdit != nullptr ? prevEdit : source };

        for (int i = 0; i < 5; ++i)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            srv.Format = TypedFormat(srvs[i]->GetDesc().Format);

            D3D12_CPU_DESCRIPTOR_HANDLE handle = cpu;
            handle.ptr += (SIZE_T) i * stride_;
            device_->CreateShaderResourceView(srvs[i], &srv, handle);
        }

        ID3D12Resource* uavs[2] = { target, keep != nullptr ? keep : target };

        for (int i = 0; i < 2; ++i)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            uav.Format = TypedFormat(uavs[i]->GetDesc().Format);

            D3D12_CPU_DESCRIPTOR_HANDLE handle = cpu;
            handle.ptr += (SIZE_T) (5 + i) * stride_;
            device_->CreateUnorderedAccessView(uavs[i], nullptr, &uav, handle);
        }

        ID3D12DescriptorHeap* heaps[] = { heap_ };
        cmd->SetDescriptorHeaps(1, heaps);
        cmd->SetComputeRootSignature(root_);
        cmd->SetPipelineState(pipeline_);
        cmd->SetComputeRootDescriptorTable(0, gpu);
        cmd->SetComputeRoot32BitConstants(1, sizeof(Params) / 4, &constants, 0);
        cmd->Dispatch((constants.width + 7) / 8, (constants.height + 7) / 8, 1);
    }

    void destroy()
    {
        if (pipeline_ != nullptr)
        {
            pipeline_->Release();
            pipeline_ = nullptr;
        }

        if (root_ != nullptr)
        {
            root_->Release();
            root_ = nullptr;
        }

        if (heap_ != nullptr)
        {
            heap_->Release();
            heap_ = nullptr;
        }

        device_ = nullptr;
    }

  private:
    static const unsigned int kRingSlots = 8;
    static const unsigned int kPerDispatch = 7;

    ID3D12Device* device_ = nullptr;
    ID3D12RootSignature* root_ = nullptr;
    ID3D12PipelineState* pipeline_ = nullptr;
    ID3D12DescriptorHeap* heap_ = nullptr;
    unsigned int stride_ = 0;
    unsigned int ring_ = 0;
};
} // namespace codec
