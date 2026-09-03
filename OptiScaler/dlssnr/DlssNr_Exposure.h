#pragma once
#include <algorithm>
#include <cmath>

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
    p = (std::max)(FoldZeroToOne(p), 1e-4f);
    s = (std::max)(FoldZeroToOne(s), 1e-4f);
    const float gameW = p / (std::max)(e * s, 1e-4f);
    // T0 invert: return e * s / p;
    return (std::max)(gameW, 1e-4f);
}
