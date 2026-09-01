#include "../DlssNr_Area.h"
#include "../DlssNr_Modes.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using DlssNr::AreaDownsamplePixel;
using DlssNr::AreaSample;

static int gFails = 0;

static void ExpectNear(const char* name, float got, float want, float eps = 1e-5f)
{
    if (std::fabs(got - want) > eps)
    {
        std::printf("FAIL %s: got %g want %g\n", name, got, want);
        ++gFails;
    }
}

static void TestOverlap1D()
{
    // dest pixel 0 of 3->2: box [0, 1.5). Texel 0 weight 1, texel 1 weight 0.5
    ExpectNear("ovl0", DlssNr::Overlap1D(0.f, 1.5f, 0), 1.f);
    ExpectNear("ovl1", DlssNr::Overlap1D(0.f, 1.5f, 1), 0.5f);
    ExpectNear("ovl2", DlssNr::Overlap1D(0.f, 1.5f, 2), 0.f);
}

static void Test2x2Mean()
{
    AreaSample src[4] = {
        { 1, 0, 0, 1 }, { 0, 1, 0, 1 },
        { 0, 0, 1, 1 }, { 1, 1, 1, 1 },
    };
    const auto p = AreaDownsamplePixel(src, 2, 2, 1, 1, 0, 0);
    ExpectNear("mean.r", p.r, 0.5f);
    ExpectNear("mean.g", p.g, 0.5f);
    ExpectNear("mean.b", p.b, 0.5f);
}

static void TestCopy()
{
    AreaSample src[1] = { { 0.25f, 0.5f, 0.75f, 0.125f } };
    const auto p = AreaDownsamplePixel(src, 1, 1, 1, 1, 0, 0);
    ExpectNear("copy.r", p.r, 0.25f);
    ExpectNear("copy.a", p.a, 0.125f);
}

static void Test3to2NotUintTruncated()
{
    // src 3, dst 2, x=1: float box [1.5, 3). uint box would be [1, 3) and overweight texel 1.
    AreaSample src[3] = { { 1, 0, 0, 1 }, { 0, 1, 0, 1 }, { 0, 0, 1, 1 } };
    const auto p = AreaDownsamplePixel(src, 3, 1, 2, 1, 1, 0);
    const float area = 1.5f;
    ExpectNear("3to2.r", p.r, 0.f);
    ExpectNear("3to2.g", p.g, 0.5f / area);
    ExpectNear("3to2.b", p.b, 1.f / area);
}

static void TestClamps()
{
    if (DlssNr::ClampTransfer(0) != DlssNr::Transfer::Classic) { std::printf("FAIL t0\n"); ++gFails; }
    if (DlssNr::ClampTransfer(1) != DlssNr::Transfer::MatchedResidual) { std::printf("FAIL t1\n"); ++gFails; }
    if (DlssNr::ClampTransfer(2) != DlssNr::Transfer::Classic) { std::printf("FAIL t2\n"); ++gFails; }
    if (DlssNr::ClampHdrLift(0) != 0) { std::printf("FAIL h0\n"); ++gFails; }
    if (DlssNr::ClampHdrLift(1) != 1) { std::printf("FAIL h1\n"); ++gFails; }
    if (DlssNr::ClampHdrLift(9) != 0) { std::printf("FAIL h9\n"); ++gFails; }
}

int main()
{
    TestOverlap1D();
    Test2x2Mean();
    TestCopy();
    Test3to2NotUintTruncated();
    TestClamps();
    if (gFails)
    {
        std::printf("%d FAIL\n", gFails);
        return 1;
    }
    std::printf("TA PASS\n");
    return 0;
}
