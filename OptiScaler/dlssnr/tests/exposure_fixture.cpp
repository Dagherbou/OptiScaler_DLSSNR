#include "dlssnr/DlssNr_Exposure.h"

#include <cassert>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>

int main()
{
    const float nanE = std::numeric_limits<float>::quiet_NaN();
    const float infE = std::numeric_limits<float>::infinity();

    assert(SanitizeExposure(nanE) == 1.0f);
    assert(SanitizeExposure(infE) == 1.0f);
    assert(SanitizeExposure(0.0f) == 1.0f);
    assert(SanitizeExposure(1e7f) == 1.0f);

    assert(FoldZeroToOne(0.0f) == 1.0f);
    const float gwNominal = GameWhite(0.012f, 1.0f, 1.0f);
    assert(std::fabs(GameWhite(0.012f, 0.0f, 1.0f) - gwNominal) <= 1e-4f);
    assert(std::fabs(GameWhite(0.012f, 1.0f, 0.0f) - gwNominal) <= 1e-4f);

    assert(std::fabs(GameWhite(0.012f, 1.0f, 1.0f) - (1.0f / 0.012f)) <= 1e-4f);

    std::string headerPath(__FILE__);
    const auto slash = headerPath.find_last_of("/\\");
    if (slash != std::string::npos)
        headerPath = headerPath.substr(0, slash) + "/../DlssNr_Exposure.h";
    else
        headerPath = "OptiScaler/dlssnr/DlssNr_Exposure.h";

    std::ifstream in(headerPath);
    assert(in && "DlssNr_Exposure.h must be readable for the invert-line check");
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    assert(text.find("// T0 invert: return e * s / p;") != std::string::npos);

    return 0;
}
