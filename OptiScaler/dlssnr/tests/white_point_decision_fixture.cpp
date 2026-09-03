#include "dlssnr/DlssNr_Exposure.h"

#include <cassert>
#include <cmath>

static void expect(const WhitePointDecision& d, float wp, uint32_t flag)
{
    assert(d.useGameExposure == flag);
    assert(std::fabs(d.whitePoint - wp) <= 1e-4f);
}

int main()
{
    const float scale = 8.0f;
    const float trim = 1.25f;
    const float held = 0.012f;
    const float pre = 1.0f;
    const float es = 1.0f;
    const float anchored = 12.0f;

    expect(DecideWhitePoint(0, false, true, true, scale, trim, held, pre, es, anchored), scale, 0);
    expect(DecideWhitePoint(1, false, true, true, scale, trim, held, pre, es, anchored), scale, 0);
    expect(DecideWhitePoint(2, false, true, true, scale, trim, held, pre, es, 0.0f), scale, 0);
    expect(DecideWhitePoint(0, true, true, true, scale, trim, held, pre, es, anchored), scale, 0);
    expect(DecideWhitePoint(1, true, true, false, scale, trim, held, pre, es, 0.0f), trim, 1);
    expect(DecideWhitePoint(1, true, false, true, scale, trim, held, pre, es, 0.0f), trim * GameWhite(held, pre, es),
           0);
    expect(DecideWhitePoint(1, true, false, false, scale, trim, 0.012f, pre, es, 0.0f), scale, 0);
    expect(DecideWhitePoint(2, true, true, true, scale, trim, held, pre, es, anchored), anchored, 0);
    expect(DecideWhitePoint(2, true, true, true, scale, trim, held, pre, es, 0.0f), scale, 0);
    expect(DecideWhitePoint(3, true, true, true, scale, trim, held, pre, es, anchored), scale, 0);
    expect(DecideWhitePoint(1, true, false, true, scale, trim, held, 2.0f, 0.5f, 0.0f),
           trim * GameWhite(held, 2.0f, 0.5f), 0);
    return 0;
}
