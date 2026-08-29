# DLSS 5 Neural Rendering (`OptiScaler/dlssnr`)

A self-contained module that drives NVIDIA's DLSS Neural Rendering model (`nvngx_dlssnr.dll`, NGX
feature 18) over the frames OptiScaler already handles. Nothing in it is officially supported by
NVIDIA; the model ships in driver packages and is not redistributed here.

## For maintainers: the shape of the integration

Everything lives in this directory. The rest of OptiScaler touches it through **five guarded call
sites**, each one line, each wrapped in `#if OPTI_DLSSNR`:

| File | What the call does |
|---|---|
| `inputs/NVNGX_DLSS_Dx12.cpp` | the pass after an upscale (render path), and the split pipeline's three entry points |
| `menu/menu_overlay_dx.cpp` | the finished-frame pass at present |
| `hooks/Streamline_Hooks.cpp` | the hudless pass at Streamline tag time |
| `upscalers/IFeature_Dx11wDx12.cpp` | the pass inside the D3D11-on-D3D12 bridge |
| `menu/menu_common.cpp` | the settings panel, and the cost row in the timing table |

`OPTI_DLSSNR` is defined in `DlssNr.h`. Set it to `0` and the module compiles out entirely: the call
sites vanish, the module's translation units become empty, and the build produces a binary with no
trace of it (verified: zero `DLSS-NR` strings). The `[DlssNr]` entries in `Config.h`/`Config.cpp`
are delimited with `--- DLSS 5 Neural Rendering ---` comments and can be deleted as one block.

One change outside the module is a genuine upstream fix, separable on its own:
`shaders/output_scaling/OS_Dx12.cpp` sized its dispatch from the global current feature rather than
the resources passed in, which only coincides for the conventional Output Scaling chain.

## Files

| File | Role |
|---|---|
| `DlssNr.h` | umbrella header; the `OPTI_DLSSNR` switch |
| `DlssNr_Dx12.h/.cpp` | the model: forwarder loading, feature lifetime, the three evaluate paths (render, finished frame, hudless), encode/resolve orchestration, white point metering, capture |
| `DlssNr_Split.h/.cpp` | the split pipeline (RR 1:1 → NR → internal enlargement), OS absorption, native-1:1 serving |
| `DlssNr_Menu.cpp` | the settings panel |
| `DlssNr_Codec.h` | the compute shader: encode (Reinhard-on-luma proxy), resolve (delta composite, lighting-band accumulator, restores, hue-preserving clamp), downsample |
| `DlssNr_Probe.h` | frame reduction for the white point meter |
| `DlssNr_Capture.h` | matched before/after frame dumps |
| `forwarder/` | the caller-gate shim, built by `dlssnr_forwarder.vcxproj` into the release layout |

## Why a forwarder DLL exists

The model's snippet resolves the module that owns its caller's return address and refuses any whose
path does not contain `nvngx.dll`. The forwarder (`nvngx.dll_dlssnr.dll`, ~13 KB) exists only to
satisfy that check; every NGX call to the model originates from it. It is part of the solution and
builds with everything else.

## Design notes worth knowing before changing anything

- **Delta composite.** The model is shown an encoded proxy; its answer minus the proxy is the edit,
  applied onto the untouched original. At strength zero the frame is bit-identical, always.
- **Create-time parameters.** The model's tuning (preset, style, intensity, local *) is latched at
  feature creation; changes rebuild the feature after a settle. The driver's parameter block is not
  the SDK header's vtable (floats sit at slot 6); the forwarder probes it.
- **Never free under the GPU.** Every retired feature or surface is parked and freed 32 evaluates
  later; every internal feature is created on a private queue and fenced before use. Both rules
  were paid for with device hangs.
- **Two paths, one lock.** The render path runs on the game's render thread and the finished-frame
  path on the present thread; a mutex covers both.
- **Temporal filtering of the edit's detail band was measured to be a dead end** (twice, including
  with a trained DLAA pass): the model re-decides detail with the framing. Only the lighting band is
  accumulated. Detail stability comes from routing the pass through a real upscaler (the split).
