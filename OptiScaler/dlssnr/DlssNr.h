#pragma once

// DLSS 5 Neural Rendering for OptiScaler.
//
// Everything lives under OptiScaler/dlssnr/. The rest of OptiScaler touches it through a handful of
// guarded call sites, each one line, each wrapped in `#if OPTI_DLSSNR`; set the macro to 0 (here, or
// on the compiler command line) and the module compiles out entirely -- the call sites vanish, the
// module sources become empty translation units, and only the [DlssNr] config entries remain, inert.
//
// Call sites, for the record:
//   inputs/NVNGX_DLSS_Dx12.cpp        the render-path pass after an upscale, and the split pipeline
//   menu/menu_overlay_dx.cpp          the finished-frame pass at present
//   hooks/Streamline_Hooks.cpp        the hudless pass at tag time
//   upscalers/IFeature_Dx11wDx12.cpp  the pass inside the D3D11-on-D3D12 bridge
//   menu/menu_common.cpp              the settings panel

#ifndef OPTI_DLSSNR
#define OPTI_DLSSNR 1
#endif

#if OPTI_DLSSNR
#include "DlssNr_Dx12.h"
#include "DlssNr_Split.h"
#endif
