#pragma once

#include <algorithm>
#include <cmath>

namespace DlssNr
{

inline float Overlap1D(float box0, float box1, int i)
{
    const float t0 = (float) i;
    const float t1 = (float) (i + 1);
    const float a = box0 > t0 ? box0 : t0;
    const float b = box1 < t1 ? box1 : t1;
    return b > a ? (b - a) : 0.0f;
}

struct AreaSample
{
    float r, g, b, a;
};

// src is row-major srcW*srcH. Matches spec §7.1 / HLSL Mode 2 Matched.
inline AreaSample AreaDownsamplePixel(const AreaSample* src, int srcW, int srcH, int dstW, int dstH, int x, int y)
{
    if (dstW == srcW && dstH == srcH)
        return src[y * srcW + x];

    const float x0 = ((float) x * (float) srcW) / (float) dstW;
    const float x1 = ((float) (x + 1) * (float) srcW) / (float) dstW;
    const float y0 = ((float) y * (float) srcH) / (float) dstH;
    const float y1 = ((float) (y + 1) * (float) srcH) / (float) dstH;
    const float area = (x1 - x0) * (y1 - y0);

    const int i0 = (int) floorf(x0);
    const int i1 = (int) ceilf(x1) - 1;
    const int j0 = (int) floorf(y0);
    const int j1 = (int) ceilf(y1) - 1;

    float ar = 0.f, ag = 0.f, ab = 0.f;
    for (int j = j0; j <= j1; ++j)
    {
        const int jj = std::clamp(j, 0, srcH - 1);
        const float wy = Overlap1D(y0, y1, j);
        for (int i = i0; i <= i1; ++i)
        {
            const int ii = std::clamp(i, 0, srcW - 1);
            const float w = Overlap1D(x0, x1, i) * wy;
            const AreaSample& s = src[jj * srcW + ii];
            ar += s.r * w;
            ag += s.g * w;
            ab += s.b * w;
        }
    }

    const float cx = ((float) x + 0.5f) * (float) srcW / (float) dstW;
    const float cy = ((float) y + 0.5f) * (float) srcH / (float) dstH;
    const int acx = std::clamp((int) floorf(cx), 0, srcW - 1);
    const int acy = std::clamp((int) floorf(cy), 0, srcH - 1);

    return { ar / area, ag / area, ab / area, src[acy * srcW + acx].a };
}

} // namespace DlssNr
