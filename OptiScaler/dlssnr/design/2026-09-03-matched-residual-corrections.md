# Matched residual corrections: empty-model gate, P from colorCopy, strength-0 copy

> **Rebase note (2026-09-03):** `2026-09-03-rebase-onto-dlss-neural-rendering.md`
> wins where this file conflicts. Empty-model + t3 \(P\) still apply. The row that says
> “Vulkan has no working scale” is false on `dlss-neural-rendering`.

**Status:** design for implementation. Reviewed and rewritten 2026-09-03; supersedes the earlier draft of this file.
**Branch:** `dlssnr-hhkbble`, tip `c453bbe6`. Every line number below is from that tip.
**Scope:** the resolve (Mode 1) of `precompile/dlssnr.hlsl`, both compiled blobs, the resolve bind on Dx12 and Vulkan, comments that describe the resolve, one menu string.
**Does not change:** encode, Mode 2 area kernel, Mode 3 meter, Classic compose, UpgradeToneMap, default `Transfer = 1`, WorkingScale, NGX evaluate, Config, cbuffer, descriptor layouts.

This is a correction slice, not a second transfer. Area downsample and Matched residual stay the live path. Three things sit on top of that path and are wrong; this file locks what changes and, at least as carefully, what does not.

The 2026-09-02 multipass brief (bilinear Classic, Classic default, rebuild \(P\) only if `colorCopy` is missing, its own compose) is a draft PR against an older state. It does not apply to this branch and is not a reference for this work.

---

## 0. One-screen summary for the implementer

| # | Defect on `c453bbe6` | Fix |
|---|---|---|
| 1 | Empty-model gate measures `luma(T)` after `model` has been replaced by the residual result (`dlssnr.hlsl:617`, `625–628`). When the model returns an empty frame under Matched at reduced scale, \(T\) is a high-pass of the frame, not empty, and the gate composes it. | Measure `luma(m)` **before** the residual block. Keep the existing `luma(model) <= 1e-5` test as well: line `648` divides by `modelLuma` and only that test keeps it safe. Gate = union. |
| 2 | Resolve rebuilds the full-res proxy as `saturate(SoftKnee(original))` (`611`) instead of reading the encode it already has. The rebuild rests on a false comment (`266–271`: the encode "only ever wrote a reduced one") and clips passthrough frames that the encode did not clip. | Bind `colorCopy` on t3 for the resolve; \(P\) = `Decode` of that texture. Remove SoftKnee from the resolve. Rebuild **both** shader blobs. |
| 3 | Detail-strength help says strength 0 "gives back exactly what the upscaler produced" (`DlssNr_Menu.cpp:221`); a Dx12 comment says the same (`DlssNr_Dx12.cpp:1628–1630`). Strength 0 returns the keep (`hdrCopy`), which is the upscaler frame with negatives clamped and one float round-trip. Capture "before" is the encode, not that frame, so the sentence sends testers to the wrong reference. | Reword both. No behaviour change. |

Defect 1 is the only one with a visible failure mode. Defect 2 is a definitional and maintenance fix whose picture change is at quantisation level on the HDR path and real only on passthrough frames with values above 1. The test plan says so, so nobody hunts for a difference that is not there.

---

## 1. The defects, from the code

### 1.1 Gate

`dlssnr.hlsl:595–618` replaces `proxy` and `model` when `gTransfer == 1 && modelRanSmall`. `625` then takes `modelLuma = dot(model, kLuma)` and `628` tests it.

For an empty model (\(m = 0\)): `edit = -p` (`544`). Inside the block, `model = CubeScaleResidual(P, P - p)`. Where the frame is smooth, \(P \approx p\) and \(T \approx 0\): the gate fires, output is \(H\). Where the frame has texture, \(P \neq p\), \(T = P - \alpha p\) is a high-pass remnant with luma far above \(10^{-5}\): the gate does not fire and \(T\) is composed as if it were the model's picture. `proxyLuma` is `luma(P)`, at most `originalLuma`, so the headroom branch runs: where the knee fired, `ratio = (T_l + (H_l - P_l)) / T_l` is large and the pixel brightens toward the guard; where it did not, the result is \(T\) itself rescaled to the frame's luminance, bounded below by `1 / guard`. The output is a high-pass of the frame wearing the frame's brightness, wherever the frame has detail and the model returned nothing. Strength 0 hides this (`lerp(original, ..., 0)`); default strengths do not.

Classic is unaffected: without the swap `model` is \(m\), and the test at `628` is already on \(m\).

### 1.2 \(P\)

The encode writes `colorCopy = LinearToSrgb(SoftKnee(frame / W))` at display size (`462–468`; `CreateScratch(..., width, height)` at `DlssNr_Dx12.cpp:1247`; encode dispatched with `Width = width` at `1492–1498`). Mode 2 writes the reduced copy to `colorSmall` (`1512–1523`). The comment on SoftKnee (`266–271`) says the encode "only ever wrote a reduced one" and that recomputing "costs less than the texture read it replaces". The first is false; the second is an opinion the code then depends on.

`p` is `Decode(Sample(colorSmall))`: the stored encode, area-averaged, stored again, sampled. `P` should be the same stored encode at full resolution. Today it is a recomputation from `hdrCopy` that skips the encode's storage format and, in passthrough, applies a `saturate` the encode never applied (`274–275` return `display` unchanged; `611` saturates it). `Decode(colorCopy)` is the definition. It also means the resolve cannot drift from the encode if the curve ever changes.

What this fix is **not**: the darker/redder 50 % picture described at `597–610` came from an unsaturated SoftKnee, and `611` already saturates. `colorCopy` is inside the unit cube for the same reason (`LinearToSrgb` saturates first, `240`). On the HDR path the two sources agree to storage precision. CubeScale still discards a residual that pushes a channel already at 1 further out; both sources share that wall.

The resolve does not bind `colorCopy` today. t3 is `motionIn` (`1708`) and the shader reads t3 only in Mode 3 tile \((0,0)\) (`342–345`). `EditAt` (`251–263`) is dead code.

### 1.3 Copy

At strength 0: `upgraded = original` (`660` with `saturate(0)`, or `632`), `lumaRatio = 1` (`680`), `amplified = 1` (`693`), `boundedRatio = 1` (`714`), `result = original` (`721`), `result *= normScale` (`724`). `original` is `hdrCopy / normScale` (`527`), and `hdrCopy` is `max(source.rgb, 0)` (`441`, written at `444`). So strength 0 returns the keep: the upscaler frame with negative components clamped and one divide-multiply round trip in float. The help text's "exactly what the upscaler produced" is nearly true and points at the wrong thing: the only "before" a tester can obtain is Capture, and Capture records `colorCopy` (`1719`), the sRGB-encoded knee'd proxy.

---

## 2. Facts that hold on this branch (verified; do not re-litigate)

| Fact | Where |
|---|---|
| `colorCopy` is full resolution and in `NON_PIXEL_SHADER_RESOURCE` from the post-encode barrier until after the resolve | `DlssNr_Dx12.cpp:1247`, `1503–1504`, `1790–1791` |
| Mode 2 is always the area box; downsample constants are Mode / Width / Height only | `dlssnr.hlsl:378–435`, `DlssNr_Dx12.cpp:1514–1519` |
| t3 is read only in Mode 3 tile \((0,0)\); the meter passes the game's exposure texture there | `dlssnr.hlsl:342–345`, `DlssNr_Dx12.cpp:1471–1472` |
| `DispatchPass` substitutes `InSource` for a null 4th SRV | `DlssNr_Dx12.cpp:1051–1057` |
| `kSrvCount = 5`; the 5th (`InPrevEdit`) is vestigial and has no HLSL register | `DlssNr_Dx12.h:58–62`, `87–94` |
| Default `Transfer = 1`, default `WorkingScale = 1.0`, default `UseProxy = false` | `Config.h:286`, `333`, `355` |
| The Enlargement combo is disabled at Model resolution 100 % | `DlssNr_Menu.cpp:183–195` |
| `Transfer` is not a feature-rebuild input | `TuningMatchesFeature`, `DlssNr_Dx12.cpp:900–909` |
| `modelRanSmall` is `gSource.GetDimensions() != (gWidth, gHeight)`; guides are render-res, so `gGuideWidth` is not a substitute | `dlssnr.hlsl:591–593` |
| Non-passthrough requires a float format (`isHdrBuffer`); `_SRGB` scratch can only occur in passthrough, where `Decode` is the identity and all four SRVs share the format | `DlssNr_Dx12.cpp:829–843`, `1365` |
| Vulkan has no working scale: `proxy`, `keep`, `output` are all `width × height` FP16, evaluate runs at that size, `modelRanSmall` is false | `DlssNrFeature_Vk.cpp:555–559`, `658–663` |
| Vulkan binds a 1×1 never-cleared dummy for a `VK_NULL_HANDLE` read slot. It does **not** substitute t0 | `DlssNr_Vk.cpp:117–184`, `194–199` |
| Vulkan runs its own blob `dlssnr_spv` from `DlssNr_Shader_Vk.h`, built by dxc `-spirv -D VK_MODE`; Dx12 runs `DlssNr_cso` built by fxc `cs_5_0` | `DlssNr_Vk.cpp:5`, `89`; `DlssNr_Dx12.cpp:1025`; `shader_tools/build_precompiled_shader_vk.bat`; `dlssnr/README.md:60–70` |
| `fxc.exe`, `dxc.exe`, `create_header.py` are tracked in `OptiScaler/shaders/shader_tools/` | `git ls-files` |
| `.cso` and `.spv` are tracked alongside the headers | `git ls-files OptiScaler/shaders/dlssnr/precompile` |

---

## 3. Non-goals

- Do not change Mode 2. Area is the only shrink. Do not branch it on `gTransfer`. Do not bring back bilinear Classic.
- Do not change the default. `Transfer` stays `1`.
- Do not change UpgradeToneMap (two-sided guard, strength above 1 as `pow`, OkLab near-zero-chroma guard, `ClampAp1`). The 2026-09-02 §8.7 compose is older and worse.
- Do not touch the encode curve or `LinearToSrgb`.
- Do not add cbuffer fields, Config keys, debug views, a UAV, a sixth SRV, or a descriptor-layout entry.
- Do not use the vestigial 5th SRV slot (`InPrevEdit`) for `colorCopy`. t3 is the register.
- Do not rename `gMotion`. A rename touches HLSL, two C++ layout comments and the meter; it buys nothing functional. Fix its declaration comment instead.
- Do not add a `Decode` helper in this slice. Write \(P\)'s decode inline, exactly as `514–515` do.
- Do not add Vulkan working scale. Bind t3 on Vulkan so the shader is one program; the residual block stays dead there.
- Do not change Capture semantics (see §13).
- Do not gate on `luma(T)` **alone** or on `luma(m)` **alone** (see §4, Empty model).

---

## 4. Contracts

| Name | Rule |
|---|---|
| Area | Mode 2 is today's box: float edges, overlap weights, `acc / area`, clamp-to-edge, centre-texel alpha. |
| Default | Missing INI key: `Transfer = 1`. |
| Classic unchanged | With `gTransfer == 0`, the resolve's HLSL is unchanged from `c453bbe6`; nothing in this slice runs on that path. Identity is at the source level: the rebuilt blob may be scheduled differently by fxc and the driver, so a capture diff is a fence against visible change, not a zero-byte test. |
| Same-rate skip | `modelRanSmall == false` \(\Rightarrow\) the residual block does not run; `proxy, model` stay `p, m`. **This is what makes Classic and Matched identical at 100 %.** Without the skip, `P + (m - p)` is two float roundings away from \(m\) even when \(p = P\), and \(p\) is a filtered sample while \(P\) is a load. (CubeScale itself is the identity on a decoded \(m\), which `SrgbToLinear` keeps inside the cube; the roundings are the whole difference.) Keep the skip. |
| Strength 0 | `TransferStrength = 0`, Compare off or Wipe, Debug off: output is `hdrCopy` to storage precision (`(x / W) * W` in float). Not a claim about the upscaler UAV; not Capture "before". |
| Alpha | Unchanged. `originalSample.a`. |
| Empty model | Let `emptyModel = dot(m, kLuma) <= 1e-5`, measured on the decoded model sample **before** the residual block. Output is \(H\) when `emptyModel` **or** `dot(model, kLuma) <= 1e-5` after the block. The second clause guards the division at `648`; without it a residual that legitimately lands on black (\(P\) dark, `edit` negative, CubeScale clamps \(T\) to exactly 0 while \(m > 10^{-5}\)) divides by zero. Threshold unchanged. |
| \(P\) (Matched, reduced only) | `Decode` of `colorCopy` at this pixel: `SampleLevel(gLinear, cmpUv, 0)` when `gCompareMode == 1`, else `Load(int3(id.xy, 0))`, mirroring `originalSample` at `516–517`. `Decode(rgb) = gPassthrough != 0 ? rgb : SrgbToLinear(rgb)`. No extra `saturate`: `SrgbToLinear` already saturates, and passthrough must not clip. |
| `proxyLuma` | When \(P\) replaces `proxy`, `proxyLuma = dot(P, kLuma)`. The compose ratio at `638–648` reads it; leaving `luma(p)` there is the blur-as-headroom bug. |
| \(T\) | `CubeScaleResidual(P, P + edit)` with `edit = m - p` from the small textures, computed before the block (`544`). Unchanged helper. Passthrough returns \(T\) as computed. |
| Debug views | 1–3 stay where they are (`532–555`), on `p`, `m`, `edit`, before the block. They do not show \(P\) or \(T\). |
| t3 | One register, mode-dependent: meter = exposure texture; resolve = `colorCopy` (Dx12) / `g_vk.proxy` (Vulkan); encode / downsample = unread stand-in. |
| Blobs | `DlssNr_Shader.h` (fxc) and `DlssNr_Shader_Vk.h` (dxc SPIR-V) are rebuilt from the same `dlssnr.hlsl` in the same commit. |

---

## 5. Resolve data flow (Mode 1)

```
encode (unchanged)          colorCopy = LinearToSrgb(SoftKnee(frame / W))   or  max(frame, 0) if passthrough
                            hdrCopy   = max(frame, 0)
Mode 2 (unchanged)          colorSmall = area(colorCopy)                     when work != colour
NGX (unchanged)             output = model(colorSmall | colorCopy)

resolve
  p  = Decode(gSource.Sample(cmpUv))          t0 = colorSmall | colorCopy
  m  = Decode(gModel.Sample(cmpUv))           t1 = output
  H  = original = hdrCopy / normScale         t2 = hdrCopy   (Sample if SBS, else Load)
  proxyLuma = luma(p)
  edit = m - p
  debug views 1..3                            (unchanged, before anything below)

  emptyModel    = luma(m) <= 1e-5             BEFORE the block
  modelRanSmall = dims(t0) != (gWidth, gHeight)

  if gTransfer == 1 && modelRanSmall
      P         = Decode(t3 at this pixel)    t3 = colorCopy (Dx12) | g_vk.proxy (Vk)
      proxy     = P
      proxyLuma = luma(P)
      model     = CubeScaleResidual(P, P + edit)

  modelLuma = luma(model)
  if emptyModel || modelLuma <= 1e-5:  upgraded = original
  else:                                 today's UpgradeToneMap(original, proxy, model)   (unchanged from 634 on)
  ... guard, colour blend, normScale, compare, write               (unchanged)
```

---

## 6. Bindings

### 6.1 t3 register

| Dispatch | t3 resource | Shader use |
|---|---|---|
| Mode 3 meter | game exposure texture (`frame.ExposureTexture`) | `gMotion.Load(int3(0,0,0)).r`, unchanged |
| Mode 1 resolve | Dx12 `g_nr.colorCopy`; Vulkan `g_vk.proxy.view` | \(P\), Matched and reduced only |
| Mode 0 encode, Mode 2 downsample | stand-in (Dx12: `InSource`; Vulkan: dummy) | unread |

HLSL identifier stays `gMotion`. Its declaration comment (`220`) must state both live uses and that nothing in the shader reads motion vectors.

### 6.2 Dx12 call site

`DlssNr_Dx12.cpp:1708–1709`, today:

```cpp
DispatchPass(cmdList, resolveParams, modelInput, g_nr.output, g_nr.hdrCopy, motionIn,
             nullptr, target, nullptr);
```

Required:

```cpp
DispatchPass(cmdList, resolveParams, modelInput, g_nr.output, g_nr.hdrCopy, g_nr.colorCopy,
             nullptr, target, nullptr);
```

`motionIn` is still consumed by `g_nr.evaluate` (`1571–1578`) and the proxy path; only the resolve stops receiving it. No new barrier: `colorCopy` is already `NON_PIXEL_SHADER_RESOURCE` here (§2). Nothing new about the view either: the downsample binds `colorCopy` as t0 through the same `DispatchPass`, and at 100 % the resolve already does (`modelInput` is `colorCopy` when not reduced, `1510`).

Passing `nullptr` instead would silently bind `modelInput` (§2, null substitution). When reduced that is `colorSmall`, read with `Load(int3(id.xy, 0))` at full-resolution coordinates: the wrong texel inside the work-size region and zero (out of bounds) outside it; only the side-by-side path degrades to \(p\). A must-not-ship bind, not a fallback.

### 6.3 Vulkan call site

`DlssNrFeature_Vk.cpp:686–687`, today:

```cpp
if (!g_vk.pass->Dispatch(cmdBuffer, resolve, width, height, g_vk.proxy.view, g_vk.output.view, g_vk.keep.view,
                         VK_NULL_HANDLE, colour->Resource.ImageViewInfo.ImageView, VK_NULL_HANDLE))
```

Required:

```cpp
if (!g_vk.pass->Dispatch(cmdBuffer, resolve, width, height, g_vk.proxy.view, g_vk.output.view, g_vk.keep.view,
                         g_vk.proxy.view, colour->Resource.ImageViewInfo.ImageView, VK_NULL_HANDLE))
```

`g_vk.proxy` is already in `SHADER_READ_ONLY_OPTIMAL` for this dispatch (`682`); the same view in two combined-image-sampler slots with the same layout is legal. No new image, no layout change, no descriptor-layout change.

Facts an implementer must not get backwards: a `VK_NULL_HANDLE` on Vulkan binds the 1×1 dummy, not t0; a missed bind after this slice would make \(P\) a `Load` outside a 1×1 image, not Classic's proxy. Today the residual block is dead on Vulkan (`modelRanSmall` false) so the bind changes nothing visible; it is required so the shader has one meaning for t3 on the resolve.

---

## 7. Shader edits (Mode 1 only)

All code above `544` (`edit`) and from `634` (`else` of the gate) on is unchanged; the only edits above `544` are the §8 comments. The changed region, written to compile (verified with both fxc `cs_5_0` and dxc `-spirv -D VK_MODE` against `c453bbe6`):

```hlsl
    float3 edit = model - proxy;                          // 544, unchanged

    // ... debug view 3, unchanged ...

    // Measured on the model's own answer, before the residual below may replace `model`. An empty
    // frame from the model is "hand the frame back", whatever the residual would have made of it.
    const bool emptyModel = dot(model, kLuma) <= 1e-5;

    uint proxyW, proxyH;
    gSource.GetDimensions(proxyW, proxyH);
    const bool modelRanSmall = proxyW != gWidth || proxyH != gHeight;

    if (gTransfer == 1 && modelRanSmall)
    {
        // The frame's own proxy at full resolution is the encode this pass already wrote; on the
        // resolve t3 is that texture. Read the way `original` is read: sampled for side by side,
        // loaded otherwise. Decoded like `proxy` and `model` above. Not saturated again: the encode
        // went through LinearToSrgb, which saturates first, so this is already inside the unit
        // cube, and a passthrough frame must come back exactly as it was stored.
        const float4 encodeSample = gCompareMode == 1 ? gMotion.SampleLevel(gLinear, cmpUv, 0)
                                                      : gMotion.Load(int3(id.xy, 0));
        const float3 fullProxy = gPassthrough != 0 ? encodeSample.rgb : SrgbToLinear(encodeSample.rgb);

        proxy = fullProxy;
        proxyLuma = dot(proxy, kLuma);
        model = CubeScaleResidual(fullProxy, fullProxy + edit);
    }

    float modelLuma = dot(model, kLuma);
    float3 upgraded;

    // Two reasons to hand the frame back untouched. The model returned an empty frame for this
    // pixel (emptyModel, measured before the residual). Or the composed model picture is black
    // here, which the residual can legitimately produce on a dark pixel; the highlight branch below
    // divides by modelLuma and needs it non-zero.
    if (emptyModel || modelLuma <= 1e-5)
    {
        upgraded = original;
    }
    else
    {
        // ... 636–660 unchanged ...
    }
```

`SoftKnee` is no longer called on the resolve. Keep the function; the encode calls it (`466`).

---

## 8. Comments that must change in the same patch

The code carries its reasoning in comments and a later reader plans on them (the resolve already did, from `266–271`). Leaving these would re-teach the defects.

| File:lines | Today | Required |
|---|---|---|
| `dlssnr.hlsl:220` | "resolve, accumulating: the game's motion vectors." | t3 has two live uses: the meter's exposure texture (Mode 3, tile 0) and the resolve's full-resolution encode (`colorCopy` / `g_vk.proxy`). The name is the slot's history; nothing here reads motion vectors. |
| `dlssnr.hlsl:266–271` | "the resolve has to be able to reproduce it ... the encode only ever wrote a reduced one ... recomputing costs less than the texture read" | The knee is applied by the encode only. The resolve reads the encode's output back (t3) when it needs the frame's own proxy at full resolution; it does not recompute the curve. |
| `dlssnr.hlsl:341` | "The motion slot is free here: the meter has no use for motion vectors." | t3 is free here; the meter puts the game's exposure texture in it, the resolve the full-resolution encode. |
| `dlssnr.hlsl:465` | "The resolve reproduces this exactly, so the two agree on what the frame's own proxy is." | The resolve reads this texture back rather than recomputing it, so the two cannot disagree on what the frame's own proxy is. |
| `dlssnr.hlsl:569–590` | "the encode is a pure function, so SoftKnee reproduces it exactly" and the same-rate paragraph ("agree to within the proxy surface's precision") | Keep the blur-as-headroom rationale. Replace the recompute sentences with: the full-resolution proxy is the encode itself, read from t3. Same-rate paragraph: skipped because there is no residual to carry, and the skip is what keeps Classic and Matched bit-identical at 100 %. |
| `dlssnr.hlsl:597–610` | the saturate rationale | Deleted with the code it explains. The fact that survives (encode is inside the cube; CubeScale needs that; a channel at 1 still discards a residual pushing it out) goes into the short comment in §7. |
| `dlssnr.hlsl:630–631` | "The model can return an empty frame ..." | The two-reason comment in §7. |
| `DlssNr_Dx12.cpp:1628–1630` | "Resolve takes the difference ... adds that back to the frame. At strength zero the result is what the upscaler produced, exactly" | The resolve composes the model's answer against the keep (`hdrCopy`) as the shader describes; the additive description is from an older composition. At strength zero the keep comes back as stored. |

`DlssNr_Common.h:134–144` (Transfer) is accurate and stays. The Enlargement help (`DlssNr_Menu.cpp:196–207`) is accurate and stays, attribution included.

---

## 9. Menu copy

`DlssNr_Menu.cpp:221`, today:

> 0 gives back exactly what the upscaler produced. 1 is the model's picture.

Replace with:

> 0 leaves the frame as it arrived, with none of the model's picture in it. 1 is the model's picture.

No internal names in the widget. The rest of that help block stays.

---

## 10. Build: two blobs, one source

Both from `OptiScaler/shaders/dlssnr/precompile`. Tools are tracked in `../../shader_tools/`.

Dx12 (the README command; **not** `build_precompiled_shader_fxc.bat`, which would name the array `dlssnr_cso` and break `DlssNr_Dx12.cpp:1025`; **not** `build_precompiled_shader.bat`, which emits DXIL):

```
..\..\shader_tools\fxc.exe -T cs_5_0 -E CSMain -O3 dlssnr.hlsl -Fo DlssNr_Shader.cso
python ..\..\shader_tools\create_header.py DlssNr_Shader.cso DlssNr_Shader.h DlssNr_cso
```

Vulkan (what `build_precompiled_shader_vk.bat dlssnr` runs, spelled out so the output file names match the tracked ones):

```
..\..\shader_tools\dxc.exe -spirv -T cs_6_0 -E CSMain -O3 -Qstrip_debug -D VK_MODE -Cc -Vi dlssnr.hlsl -Fo DlssNr_Shader_Vk.spv
python ..\..\shader_tools\create_header.py DlssNr_Shader_Vk.spv DlssNr_Shader_Vk.h dlssnr_spv
```

Commit `dlssnr.hlsl`, `DlssNr_Shader.h`, `DlssNr_Shader.cso`, `DlssNr_Shader_Vk.h`, `DlssNr_Shader_Vk.spv`. `git status --short OptiScaler/shaders/dlssnr/precompile` must show exactly five ` M` lines and no `??` (the tree otherwise carries untracked build output; a case-different or `_DX11` name in that directory means the wrong command ran). Nothing includes a `_DX11` header; do not add one.

Sanity anchor: on the unmodified `c453bbe6` source, these exact commands reproduce the tracked `.cso` and `.spv` byte for byte. If they do not on the implementer's machine before any edit, stop and find out why before trusting the rebuilt headers.

A stale header is a silent old shader: the constructor passes `source = nullptr` (`DlssNr_Dx12.cpp:1022–1025`).

---

## 11. File-level work and suggested order

| Step | File | Change |
|---|---|---|
| 1 | `shaders/dlssnr/precompile/dlssnr.hlsl` | §7 code; §8 comments |
| 2 | `precompile/DlssNr_Shader.h`, `.cso`, `DlssNr_Shader_Vk.h`, `.spv` | §10 rebuild |
| 3 | `shaders/dlssnr/DlssNr_Dx12.cpp` | §6.2 bind; §8 comment at 1628 |
| 4 | `dlssnr/DlssNrFeature_Vk.cpp` | §6.3 bind |
| 5 | `dlssnr/DlssNr_Menu.cpp` | §9 string |
| 6 | Tests §12 | C4 and V1 first (nothing should move), then C7, then the rest |

Steps 1–4 land in one commit. If they must be split, the binds (3, 4) go **first**: the old shader does not read t3 on the resolve, so an early bind is inert. The reverse order is not safe: the new shader with the old Dx12 bind reads \(P\) from the motion-vector texture under Matched at reduced scale and the picture is garbage until the bind lands. No Config, no cbuffer, no `DlssNr_Modes.h`, no descriptor layouts.

---

## 12. Test plan

Controls, by their live names: **Model resolution** (`DlssNrWorkingScale`, slider under "Cost"), **Enlargement** (`DlssNrTransfer`, Classic / Matched residual; disabled at 100 %), **Detail strength**, **Compare**, **Debug view**, `UseProxy` (INI only, default off). Default matrix unless a row says otherwise: NR on, `UseProxy` off, Output Scaling off, Compare off, Debug view off, Detail and Colour strength at 1, one frozen scene, one model version. "Today" means a `c453bbe6` build.

| ID | Setup | Pass if |
|---|---|---|
| C0 | Delete `[DlssNr] Transfer` and `WorkingScale` from the INI, start | Enlargement shows Matched residual and is greyed (100 %). Picture equals today. |
| C1 | Detail strength 0, Model resolution 67 %, either Enlargement | Toggling NR on/off shows no difference (the keep is the upscaler frame with negatives clamped). Independent of \(P\) and the gate by construction; this is the identity contract, not an algorithm test. |
| C2 | Model resolution 67 %, toggle Enlargement Classic ↔ Matched | Picture changes. Log shows a new "DLSS-NR composition: ... transfer ..." line and **no** new "DLSS-NR running at" line (no feature rebuild, `1340`). No hitch. |
| C3 | Set Matched at 67 %, then Model resolution 100 % | Combo greys out; INI still says `Transfer=1`; picture equals Classic at 100 % (residual block skipped). |
| C4 | Classic at 67 % | **No visible change against today.** Nothing in this slice runs on Classic. Capture 8 frames on a paused scene with both builds if a numeric check is wanted; tolerate last-bit noise from the rebuilt blob, flag anything larger. |
| C5 | Matched at 67 %, normal scene and a saturated-channel scene (low paper white, strongly coloured light) | No visible change against today. Differences are at storage precision on the HDR path. Not darker, not redder. Halo against Classic still smaller. If C5 shows a visible change on a non-passthrough title, something other than §7 was changed. |
| C6 | Passthrough title whose buffer is FP16 with the HDR flag clear and values above 1 | Only case where \(P\) moves visibly: highlights above 1 no longer clip under Matched at 67 %. Implementer-only; not required for acceptance if no such title is at hand. 8-bit titles cannot distinguish (`saturate(hdrCopy) == colorCopy` there). |
| C7 | **Gate.** Temporary one-line shader edit: after `float3 model = ...` (`515`) insert `model = float3(0.0, 0.0, 0.0);`, rebuild the fxc header only, run Matched at 67 % | Output is the frame to storage precision, as C1 (NR looks off; no texture artefacts). fxc folds `emptyModel` to true with that line and removes the compose; expected. With today's shader plus the same temporary line, the picture degrades to a high-pass of the frame wherever it has detail (§1.1). Classic at 67 % is the frame in both. Revert the line, rebuild, confirm C4 again. Do not commit the line or ship a control for it. |
| V1 | Vulkan title, NR on, validation layers on | Pass creates, dispatches, no descriptor/layout validation errors, picture equals today (residual block is dead on Vulkan). Confirms the SPIR-V rebuild and the t3 bind. |
| G | Pass GPU time in the overlay, Matched at 67 % | Within noise of today: one full-res Load replaces a handful of ALU. |

C7 is the acceptance test for defect 1. C4 and V1 are the regression fences. C5 is a fence, not a proof of \(P\).

---

## 13. Known and parked (not this slice)

- **Capture "before" is the encode, labelled as the upscaler frame.** `DlssNr_Dx12.cpp:1719` records `colorCopy`; `DlssNr_Capture.h:329` calls it "the frame as the upscaler produced it" and the menu help at `DlssNr_Menu.cpp:435–436` "as the upscaler produced them". True in passthrough only. Either record `g_nr.hdrCopy` (in `NON_PIXEL_SHADER_RESOURCE` at that point, `1505–1506` to `1735`) or relabel. A capture-semantics decision, one line either way; decide it separately.
- **`_SRGB` scratch formats.** `TranslateTypelessFormats` does not remap `*_UNORM_SRGB` (`Shader_Dx12.cpp:22–69`), so a passthrough game with an `_SRGB` buffer would get hardware-linearised SRVs. Pre-existing, uniform across t0–t3 (all `desc.Format`), `Decode` is the identity there, and `CreateScratch` asks for `ALLOW_UNORDERED_ACCESS`, which `_SRGB` formats do not support, so on such a buffer the scratch is never created and the pass never runs. Not introduced or worsened by this slice.
- **`EditAt`** (`dlssnr.hlsl:251–263`) is dead code. Leave it.
- **`InPrevEdit` / `kSrvCount = 5`**: vestigial. Leave it.
- **Vulkan working scale, H1 / `gHdrLift`, debug views 4–5, a `Decode` helper, renaming `gMotion`**: out of scope by decision, not by oversight.

---

## 14. Decisions locked

| Topic | Choice | Rejected, and why |
|---|---|---|
| Gate | `emptyModel (luma(m) before block) || luma(model) <= 1e-5 after block` | `luma(T)` alone: composes a high-pass when \(m\) is empty. `luma(m)` alone: divides by zero at `648` when \(T\) lands on exactly black. |
| \(P\) | `Decode(colorCopy)` from t3, no extra saturate | `saturate(SoftKnee(H))`: duplicate of the encode with a false premise, clips passthrough. A second encode UAV: nothing to gain. |
| `proxyLuma` | `luma(P)` when \(P\) replaces `proxy` | Leaving `luma(p)`: the blur-as-headroom bug returns. |
| Same-rate | Keep the skip | Forcing the residual path: not bit-identical to Classic, nothing to carry. |
| t3 | `colorCopy` / `g_vk.proxy` on resolve; exposure on meter; identifier `gMotion` kept, comment fixed | 6th SRV, 5th vestigial slot, rename: cost without function. `nullptr` on Dx12: silently `colorSmall` loaded at full-resolution coordinates (§6.2). `VK_NULL_HANDLE` on Vulkan: the 1×1 dummy. |
| Blobs | fxc `DlssNr_cso` and dxc `dlssnr_spv` rebuilt together | fxc only: Vulkan runs an older program than the source it is documented to share. |
| Shrink | Area, both Transfer values | Bilinear Classic (2026-09-02). |
| Default | Matched (`1`) | Classic default (2026-09-02). |
| Compose | This branch's UpgradeToneMap | 2026-09-02 §8.7. |
| Strength-0 copy | "leaves the frame as it arrived" | "exactly what the upscaler produced": overstates, and points at a Capture that records the encode. |
| Capture semantics | Parked, §13 | Fixing it here: a separate decision. |

---

## 15. Acceptance

Ship when all of:

1. C4 and V1: nothing moved where nothing should.
2. C7: an empty model under Matched at reduced scale returns the frame.
3. C1 and C3: identity contracts hold.
4. C5: no visible regression on the HDR path.
5. Both headers rebuilt from the committed `dlssnr.hlsl`; `git status --short OptiScaler/shaders/dlssnr/precompile` shows the five tracked files modified and nothing untracked.
6. §8 comments and §9 string landed; no comment left that says the resolve recomputes the encode or that t3 carries motion vectors.
7. Classic path, Mode 2, default Transfer, Config, cbuffer: untouched (diff review).

---

## 16. What this revision corrected in the previous draft

For readers who saw the earlier version of this file.

- The gate is a union, not "`luma(m)` instead of `luma(T)`". The earlier draft (and the review that followed it) missed the unguarded division at `648`.
- `saturate(SoftKnee(H))` and `Decode(colorCopy)` are the same function in exact arithmetic but not the same bits on the GPU; the "0–1e-17" figure was a float64 check of the pure functions, not of stored textures. The \(P\) change is a definitional and passthrough fix, not the 50 % colour-shift fix, which `611` already contains.
- Vulkan's null-slot stand-in is a 1×1 dummy, not the proxy and not t0. The SPIR-V blob is a second build output of the same source and must be rebuilt; "do not commit dxc outputs" referred to DXIL and was misleading.
- The same-rate skip is required for bit-identity; the earlier "no longer required" reasoning was wrong.
- The test matrix named controls that do not exist (`WorkAtNative`, "Post-process", "Multi-pass"); C0/C1/C6 were presented as algorithm tests but cannot exercise the change; C7 lacked a concrete procedure.
- `proxyLuma` was assigned in §7 but absent from §5; it is a contract now.
- `gMotion` stays; the earlier draft allowed a rename, which only adds surface.
- Comments at `DlssNr_Dx12.cpp:1628–1630` and `dlssnr.hlsl:341`, `465`, `569–590` were missing from the file list.
- Landing order was stated as free; the shader must not land before the Dx12 bind (§11).
