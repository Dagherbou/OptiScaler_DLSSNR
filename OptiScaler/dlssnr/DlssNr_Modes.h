#pragma once

#include <cstdint>

// Where the Neural Rendering pass sits relative to the upscaler.
//
// Kept in its own small header so Config and IFeature can name these without
// pulling in D3D12.

namespace DlssNr
{

enum class Mode : uint32_t
{
    // The model runs on the finished frame, at display resolution. The only
    // placement that needs nothing from the upscaler, so it is also the only
    // one that works when a game's own DLSS is passing straight through.
    PostProcess = 0,

    // Two stages on OptiScaler's own Dx12 DLSS / Ray Reconstruction. The first
    // feature runs 1:1 at render resolution, the model runs on that, and
    // OS_Dx12 FSR1 enlarges to display. Changing to or from this rebuilds the
    // first feature: its target size is latched at creation.
    MultiPass = 1,
};

enum class Transfer : uint32_t
{
    Classic = 0,
    MatchedResidual = 1,
};

inline Transfer ClampTransfer(uint32_t raw)
{
    return raw > (uint32_t) Transfer::MatchedResidual ? Transfer::Classic : (Transfer) raw;
}

inline uint32_t ClampHdrLift(uint32_t raw) { return raw > 1u ? 0u : raw; }

Transfer ConfiguredTransfer();
uint32_t ConfiguredHdrLift();

// Which upscaler the first pass is. OptiScaler does not substitute one for the
// other -- Ray Reconstruction needs G-buffer inputs a Super Resolution
// integration never supplies -- so this states which the game is set up for,
// and a mismatch falls back rather than half-applying.
enum class Feature1Pipeline : uint32_t
{
    RayReconstruction = 0,
    SuperResolution = 1,
};

// The mode as configured, before any fallback. Reads config; safe from any thread.
Mode ConfiguredMode();

inline bool UsesTwoFeatures(Mode mode) { return mode == Mode::MultiPass; }

// The model's working raster.
//
// WorkingScale is a fraction of display resolution, clamped to [0.25, 1], then
// clamped to the colour buffer so work never invents pixels. WorkAtNative uses
// the guide size -- the same integer raster as depth and motion vectors -- when
// both axes fit inside colour; otherwise colour. Both guide axes have to fit or
// neither is used: taking one axis from the guide and the other from the colour
// frame invents a third aspect ratio that matches neither.
struct WorkingExtent
{
    unsigned int width;
    unsigned int height;
};

inline WorkingExtent ResolveWorkingSize(float workScale, bool atNative, unsigned int colorWidth,
                                        unsigned int colorHeight, unsigned int displayWidth,
                                        unsigned int displayHeight, unsigned int guideWidth,
                                        unsigned int guideHeight)
{
    if (atNative)
    {
        const bool guideFits = guideWidth != 0 && guideHeight != 0 && guideWidth < colorWidth &&
                               guideHeight < colorHeight;
        return guideFits ? WorkingExtent { guideWidth, guideHeight }
                         : WorkingExtent { colorWidth, colorHeight };
    }

    if (displayWidth == 0 || displayHeight == 0)
    {
        displayWidth = colorWidth;
        displayHeight = colorHeight;
    }

    workScale = workScale < 0.25f ? 0.25f : (workScale > 1.0f ? 1.0f : workScale);

    unsigned int workWidth = (unsigned int) (displayWidth * workScale + 0.5f);
    unsigned int workHeight = (unsigned int) (displayHeight * workScale + 0.5f);

    if (workWidth > colorWidth)
        workWidth = colorWidth;

    if (workHeight > colorHeight)
        workHeight = colorHeight;

    if (workWidth == 0)
        workWidth = 1;

    if (workHeight == 0)
        workHeight = 1;

    return { workWidth, workHeight };
}

// Holds the model's working raster while the requested size is still moving --
// DRS under WorkAtNative, or a quality-preset change -- so the feature is not
// parked and recreated every frame. Recreating skips that evaluate, and doing
// it on every guide-size tick leaves NR off for the whole DRS window.
struct WorkingSizeHold
{
    unsigned int pendingWidth = 0;
    unsigned int pendingHeight = 0;
    unsigned long long pendingSince = 0;
};

inline WorkingExtent TickWorkingSizeHold(WorkingSizeHold& hold, WorkingExtent requested, WorkingExtent latched,
                                         bool haveFeature, bool colorChanged, unsigned long long frame,
                                         unsigned long long settleFrames, bool& commit)
{
    commit = false;

    if (!haveFeature)
    {
        hold = {};
        return requested;
    }

    if (colorChanged)
    {
        hold = {};
        commit = true;
        return requested;
    }

    if (requested.width == latched.width && requested.height == latched.height)
    {
        hold = {};
        return latched;
    }

    if (hold.pendingWidth != requested.width || hold.pendingHeight != requested.height)
    {
        hold.pendingWidth = requested.width;
        hold.pendingHeight = requested.height;
        hold.pendingSince = frame;
        return latched;
    }

    if (frame - hold.pendingSince >= settleFrames)
    {
        hold = {};
        commit = true;
        return requested;
    }

    return latched;
}

} // namespace DlssNr
