# DLSS 5 Neural Rendering (`OptiScaler/dlssnr`)

A self-contained module that drives NVIDIA's DLSS Neural Rendering model (`nvngx_dlssnr.dll`, NGX
feature 18) over the frames OptiScaler already handles. Nothing in it is officially supported by
NVIDIA; the model ships in driver packages and is not redistributed here.

## For maintainers: how to remove it

**Set `OPTI_DLSSNR` to `0` in `dlssnr/DlssNr_Switch.h`.** That is the whole procedure. The switch
lives in a header containing nothing but the macro, so `Config.h` can test it without pulling in
D3D12.

With it at `0`: the guarded call sites vanish, the three module sources become empty translation
units, the settings panel loses its section, and the `[DlssNr]` config entries are not even
declared. Verified by building it both ways — the resulting `OptiScaler.dll` contains **zero**
occurrences of `DlssNr`, `dlssnr` or `Neural Rendering`, and is 117 KB smaller.

To remove the *source* as well: delete this directory, drop `dlssnr_forwarder.vcxproj` from the
solution, and delete the `#if OPTI_DLSSNR` blocks listed below. Nothing else refers to it.

| File | Blocks | What the calls do |
|---|---|---|
| `inputs/NVNGX_DLSS_Dx12.cpp` | 5 | the pass after an upscale (render path), and the split pipeline's entry points |
| `menu/menu_common.cpp` | 2 | the settings panel, and the cost row in the timing table |
| `menu/menu_overlay_dx.cpp` | 1 | the finished-frame pass at present |
| `upscalers/IFeature_Dx11wDx12.cpp` | 1 | the pass inside the D3D11-on-D3D12 bridge |
| `Config.h` / `Config.cpp` | 3 | the `[DlssNr]` declarations and their read/write runs |

One change outside the module is **a genuine upstream fix, separable on its own and worth taking
regardless of this feature**: `shaders/output_scaling/OS_Dx12.cpp` sized its dispatch from the global
current feature rather than from the resources passed in. Those coincide for the conventional Output
Scaling chain, so the bug stayed invisible until something else called it.

## Files

| File | Role |
|---|---|
| `DlssNr_Switch.h` | the `OPTI_DLSSNR` macro, and nothing else |
| `DlssNr.h` | umbrella header; documents the call sites |
| `DlssNr_Dx12.h/.cpp` | the model: forwarder loading, feature lifetime, the evaluate paths, encode/resolve orchestration, white point metering, capture |
| `DlssNr_Split.h/.cpp` | the split pipeline (RR/DLSS 1:1 → NR → internal enlargement), Output Scaling absorption, native-1:1 serving |
| `DlssNr_Menu.cpp` | the settings panel |
| `DlssNr_Codec.h` | the compute shader: encode (scale and sRGB-encode with a soft knee), resolve (ratio composition against a measured slope, temporal accumulator, restores, AP1 clamp), downsample |
| `DlssNr_Probe.h` | frame reduction and readback for the white point meter |
| `DlssNr_Capture.h` | matched before/after frame dumps |
| `forwarder/` | the caller-gate shim, built by `dlssnr_forwarder.vcxproj` into the release layout |

## Why a forwarder DLL exists

The model's snippet resolves the module that owns its caller's return address and refuses any whose
path does not contain `nvngx.dll`. The forwarder (`nvngx.dll_dlssnr.dll`, ~13 KB) exists only to
satisfy that check; every NGX call to the model originates from it. It contains no NVIDIA code, is
part of the solution, and builds with everything else.

## Design notes worth knowing before changing anything

- **Ratio composition, not a delta.** The model is shown an encoded proxy; what it returns is
  composed back as a ratio against the original's luminance, scaled by a measured slope, with the
  chroma added. Composing it additively — which earlier revisions did — discards the model's
  behaviour in highlights and makes every arrangement look alike. At strength zero the frame is
  bit-identical, always.
- **Create-time parameters.** The model's tuning (preset, style, intensity, local *) is latched at
  feature creation; changes rebuild the feature after a settle. The driver's parameter block is not
  the SDK header's vtable (floats sit at slot 6); the forwarder probes it. Rebuilding every frame
  exhausts the driver's latches and the feature stops responding until the process restarts, which
  is why the rebuild is debounced.
- **Never free under the GPU.** Every retired feature or surface is parked and freed 32 evaluates
  later; every internal feature is created on a private queue and fenced before use. Both rules were
  paid for with device hangs.
- **Two paths, one lock.** The render path runs on the game's render thread and the finished-frame
  path on the present thread; a mutex covers both. Removing it produces crashes that look random and
  are not.
- **Temporal filtering of the edit's detail band was measured to be a dead end** (twice, including
  with a trained DLAA pass): the model re-decides detail with the framing. Only the lighting band is
  accumulated. Detail stability comes from routing the pass through a real upscaler — the split.
- **The model's own UI correction went with it.** It only ever acted on a UI layer the game tagged
  through Streamline, which almost no title does, and it could not be shown to change anything when
  one did. Removing it removed the Streamline tag hook as well, so the module no longer touches that
  file at all. The model is created with the parameter at its own default.
- **HUD detection was tried and removed.** Measured with grain, chromatic aberration and depth of
  field all off, a static HUD pixel still scored 0.31 on the "did not change" test, because game
  interfaces are translucent and animated. Separation from the world was 2.5:1 — not a detector at
  any threshold. The split is the answer to the interface, because it never sees it.
