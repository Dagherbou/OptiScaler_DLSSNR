#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

inline float FoldZeroToOne(float v) { return (v > 1e-6f) ? v : 1.0f; }

inline float SanitizeExposure(float e)
{
    if (!(e > 1e-6f && e < 1e6f))
        return 1.0f;
    return e;
}

inline float GameWhite(float e, float p, float s)
{
    e = SanitizeExposure(e);
    p = (std::max) (FoldZeroToOne(p), 1e-4f);
    s = (std::max) (FoldZeroToOne(s), 1e-4f);
    const float gameW = p / (std::max) (e * s, 1e-4f);
    // T0 invert: return e * s / p;
    return (std::max) (gameW, 1e-4f);
}

struct WhitePointDecision
{
    float whitePoint;         // written to encode/resolve gWhitePoint
    uint32_t useGameExposure; // 1 only for D3D12 Approach B this frame
};

inline WhitePointDecision DecideWhitePoint(uint32_t source,      // WhitePointSource 0/1/2
                                           bool isHdrBuffer,     //
                                           bool usable,          // ExposureUsable(gameRes)
                                           bool heldE,           // finite gameExposure > 1e-6
                                           float scale,          // WhitePointScale
                                           float trim,           // clamp(WhitePointTrim, 0.25, 4)
                                           float heldExposure,   // last courier E
                                           float pre,            // frame.PreExposure
                                           float exposureScale,  // frame.ExposureScale
                                           float anchoredOrZero) // ExposureScan::AnchoredWhitePoint(...) or 0
{
    WhitePointDecision d { scale, 0 };
    if (!isHdrBuffer)
        return d;
    if (source != 1 && source != 2)
        return d;
    if (source == 2)
    {
        if (anchoredOrZero > 0.0f)
            d.whitePoint = anchoredOrZero;
        return d;
    }
    // source == 1
    if (usable)
    {
        d.useGameExposure = 1;
        d.whitePoint = trim; // PaperWhite() multiplies by GameW
        return d;
    }
    if (heldE)
    {
        d.whitePoint = trim * GameWhite(heldExposure, pre, exposureScale);
        return d;
    }
    return d; // hole without held E: Scale
}
