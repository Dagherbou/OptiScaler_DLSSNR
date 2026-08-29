#pragma once

// The split pipeline: Ray Reconstruction as a pure 1:1 denoiser, Neural Rendering at render
// resolution, one internal enlargement. Lives entirely in this module; the NGX input seam calls three
// functions and hands over what it alone knows -- the feature behind a handle, and how to evaluate it.

#include "DlssNr.h"

#if OPTI_DLSSNR

#include <nvsdk_ngx.h>
#include <d3d12.h>

class IFeature_Dx12;

namespace DlssNr::Split
{

// What the seam knows about the feature behind a handle.
struct FeatureView
{
    bool found = false;
    IFeature_Dx12* feature = nullptr;
    int changeBackendCounter = 0;
    bool rayReconstruction = true; // else a plain Super Resolution feature: re-created as DLSS, 1:1 = DLAA
};

// The seam's own evaluate for an OptiScaler-managed feature.
using EvaluateFn = NVSDK_NGX_Result (*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                                        NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);

// Before the seam creates a Ray Reconstruction feature: clamps its geometry if the split is on.
void OnCreate(NVSDK_NGX_Feature featureId, NVSDK_NGX_Parameter* params);

// Every Ray Reconstruction evaluate, before anything else: steers re-creations toward the split's
// geometry, or back.
void ManageTransition(uint32_t handleId, NVSDK_NGX_Parameter* params, const FeatureView& view,
                      ID3D12Device* device);

// Every Ray Reconstruction evaluate: returns true when the split served the frame, with the result in
// outResult; false hands the frame to the conventional path.
bool EvaluateRR(ID3D12GraphicsCommandList* cmdList, const NVSDK_NGX_Handle* handle,
                NVSDK_NGX_Parameter* params, PFN_NVSDK_NGX_ProgressCallback callback,
                const FeatureView& view, ID3D12Device* device, EvaluateFn evaluate,
                NVSDK_NGX_Result* outResult);

} // namespace DlssNr::Split

#endif // OPTI_DLSSNR
