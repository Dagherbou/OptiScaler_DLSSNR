# Working-scale transfer (Classic | Matched residual)

**Status:** design for implementation  
**Date:** 2026-09-02 (revised: first-class UI option, Classic default; plan-input locks)  
**Scope:** `DlssNr_Dx12::Dispatch`, `precompile/dlssnr.hlsl`, Config, NR menu  
**Placement:** Post-process and Multi-pass (both already call the same `Dispatch`)

The new compose is a **user-facing option**, not a silent replace. Default stays the
live path so existing INIs and eyes do not move. Switching does not rebuild the NR
feature and does not add a second network, invert the encode knee, or change Feature 1 /
FSR1 Multi-pass enlarge.

Cost when the new option is on: well under one DLSS Super Resolution pass. Classic
stays at today’s cost.

`gTransfer` (this option) and `gTransferStrength` (the detail slider) are different
fields. Do not overload one for the other.

---

## 1. Why a second transfer exists

The model already has a full-resolution encoded proxy (`g_nr.colorCopy`) and a
full-resolution untouched frame (`g_nr.hdrCopy`). Classic resolve ignores `colorCopy`
and compares the linear original \(H\) to a bilinear upsample of the small proxy
\(U(p)\).

That difference is two things glued together:

\[
H - U(p) = \underbrace{(H - P)}_{\text{encode knee / headroom}} + \underbrace{(P - U(p))}_{\text{spatial loss}}
\]

Classic UpgradeToneMap treats both as HDR headroom. Halo at high-contrast edges is
mostly the second term.

**Matched residual** splits them. It transfers the model’s *change* onto the sharp
proxy \(P\), then runs the same UpgradeToneMap at one resolution:

\[
T = P + \big(U(m) - U(p)\big), \qquad F = \mathcal{H}(H, P, T)
\]

\(U(\cdot)\) is hardware-bilinear `SampleLevel` of the **stored** small texture,
then `Decode`. That is \(P + (S^{-1}(U(m_e)) - S^{-1}(U(p_e)))\), **not**
\(U(m-p)\) and not a work-res residual UAV.

\(P\) is `colorCopy` decoded (or copied, in passthrough). \(H\) is `hdrCopy`. \(p,m\)
stay at working resolution.

Affine / BGU / JBU / GLU are out of this document. They are a later upgrade if Matched
residual still cannot restyle hue across edges.

---

## 2. Product option

One combo, one INI key, two packed behaviours. Do not split “downsample kernel” and
“compose” into two user controls in this slice — switching compose while the model
still sees a different shrink makes A/B meaningless.

| `DlssNrTransfer` | Menu label | Downsample (work ≠ colour) | Resolve |
|---|---|---|---|
| `0` (default) | Classic | today’s `SampleLevel` bilinear | today’s UpgradeToneMap on \(H\), \(U(p)\), \(U(m)\) |
| `1` | Matched residual | encoded exact area (§7.1) | \(T = P+(U(m)-U(p))\) (or \(T=m\) if same-rate), then H0 or H1 |

`width` / `height` in this document are the Dispatch colour size (`target` desc):
post-process is usually display; Multi-pass is Feature 1. “Same-rate” means
`workW == width && workH == height`, not “Cost slider reads 100%”.

### 2.1 Where it lives

Menu: **Cost** section, immediately under the model-resolution slider (same group as
WorkingScale). It is about how a *small* model is brought back, not about Placement.

INI: `[DlssNr] Transfer=0|1`. Read/write like the other `DlssNr*` keys.

C++ enum (add next to `DlssNr::Mode` in `DlssNr_Modes.h`):

```cpp
enum class Transfer : uint32_t
{
    Classic = 0,
    MatchedResidual = 1,
};
```

Unknown / hand-edited values clamp to Classic, same pattern as `ConfiguredMode()`.
Add `ConfiguredTransfer()` next to it. `ConfiguredHdrLift()` clamps unknown values
to `0` (H0).

### 2.2 When the two options differ

They differ only when `workW != width || workH != height`. At same-rate (WorkingScale
`== 1`, WorkAtNative off, and the colour buffer is that size — not a smaller native
raster, not an Output-Scaling buffer larger than display) both options must produce
the same compose: \(T = m\), UpgradeToneMap\((H, P, m)\) with \(P\) the full-res
proxy, which matches Classic’s 1:1 sample of `colorCopy`.

Keep the combo **enabled** at full resolution so the INI value is not lost when the
user drags Cost to 100% and back. Help text states that the control only changes the
picture when the model is smaller than the frame this pass composes onto.

### 2.3 Runtime change

Changing Transfer takes effect on the **next** `Dispatch`. No NR feature rebuild, no
PSO swap, no new scratch (both kernels already share `colorSmall`). The model will
see a different \(p_e\) if work ≠ colour, because the shrink is part of the pack —
that is intended. Work size unchanged ⇒ `TickWorkingSizeHold` does not rebuild.

`gTransfer` is read in **Mode 2 and Mode 1**. The downsample `DlssNrConstants` must
set it (clamped). Today that struct only fills `Mode` / `Width` / `Height`; leaving
`gTransfer` at zero-init keeps Classic bilinear even when the menu says Matched.

NGX’s own history may take a few frames to settle after the shrink changes. That is
not a feature rebuild. Do not wait for the 30-frame work-size hold — Transfer does
not change work size.

`DlssNrUseProxy` still returns before resolve (pre-existing). Transfer is a no-op
on that path. Do not change the early return.

### 2.4 H1

`HdrLift` (H0 / H1) is an Inspect control and applies **only** when Transfer is
Matched residual. Classic ignores it. Do not expose four combinations.

H1 still uses `TransferStrength` as a lerp toward the additive result. It ignores
`ColourStrength` (no OkLab mix). See §8.8.

### 2.5 What this is not

- Not a third Placement.
- Not `LegacyResolve` as a hidden checkbox to delete later.
- Not two independent sliders for “filter” and “mix”.

---

## 3. Non-goals

- Do not train or run a second neural net.
- Do not invert \(K\) or SRTM after the model.
- Do not temporally accumulate model RGB (already measured dead).
- Do not EASU / Lanczos the model RGB. Multi-pass FSR1 after `Run` is a different
  upsample (R→D of the finished frame) and stays as it is.
- Do not downsample open linear HDR and re-encode. Encode stays first; shrink stays
  in the encoded domain for **both** options.
- Do not implement C1 affine, C3 JBU, C4 LUT, or C5 GLU in this slice.
- Do not rewrite `OptiScaler/dlssnr/DlssNr_Codec.h`. That string is not the live CSO.
  Leave a one-line note at the top if you touch the file at all: live shader is
  `precompile/dlssnr.hlsl`.
- Do not change NGX evaluate, guides, WorkingScale hold, or feature-create hold.
- Do not change the `DlssNrUseProxy` early return.
- Do not change Classic’s nonzero-strength math relative to the live shader, except
  the shared Strength-0 bypass (§4).

---

## 4. Contracts (must hold)

Shared by both Transfer values:

| Name | Rule | How |
|---|---|---|
| Strength 0 | Edited-side bytes equal `hdrCopy` | If `TransferStrength == 0` **and** `DebugView == 0`, write `gOriginal` (`hdrCopy`) with no `/W`, no lerp, no extra `max`. Encode already stored `max(source, 0)` into `hdrCopy`. This is **not** a claim on the upscaler UAV. Applies to Classic too — today’s `/W` lerp path is not bit-identical to `hdrCopy`. |
| Strength 0 test | T1 is measurable | `Compare == 0` (no letterbox / divider / SBS resample). The Capture button’s “before” is `colorCopy` (encoded) — do not use it for T1. |
| Alpha | Unchanged | Copy `hdrCopy.a` (or the Load of `gOriginal`). |
| Guides | Untouched | Depth / motion stay render-resolution NGX inputs. |
| Passthrough | No sRGB, no headroom module | `ColourIsLinearHdr && FormatCanHoldLinearHdr` is false → `gPassthrough = 1`, \(E = P = O_{rgb}\). |
| Default look | Fresh install / missing INI | Transfer = Classic. Picture matches the live build at nonzero strength. |
| Empty model | \(H\) untouched | Gate on \(\mathrm{dot}(m, kLuma) \le 10^{-5}\), **not** \(\mathrm{dot}(T, kLuma)\). If \(m \approx 0\) then \(T = P - p\) is a high-pass and must not enter UpgradeToneMap or H1. Both options, H0 and H1. Live compatibility; do not “fix” the threshold. |

Matched residual only:

| Name | Rule | How |
|---|---|---|
| Algorithm identity | \(m = p \Rightarrow T = P\) | Residual path. Not bit-identity of the final resource unless strength is 0. |
| Scale-one identity | `workW == width && workH == height` | Skip residual upsample. \(T = m\). Then \(\mathcal{H}(H,P,T)\). |

---

## 5. Data flow

```
colour-sized UAV (after upscaler or Feature 1)
        │
        ▼
Encode  →  colorCopy = E     (encoded proxy, colour size)     UNCHANGED
           hdrCopy   = H     (untouched linear or native; already max(rgb,0))
        │
        ▼
[if work != colour]
  Mode 2: Classic  → bilinear SampleLevel
          Matched  → encoded area (§7.1)
  gTransfer must be set on this dispatch
  modelInput = colorSmall
[else]
  modelInput = colorCopy
        │
        ▼
NGX NR(modelInput) → output = m_e                         UNCHANGED
        │
        ▼
Resolve (colour-sized dispatch)
  Classic:  UpgradeToneMap(H, U(p), U(m))                 live shader
  Matched:  T = P + (U(m) − U(p))   or   T = m if same-rate
            F = H0(H, P, T)  or  H1 if Inspect says so
```

Multi-pass: `IFeature_Dx12::Evaluate` already calls `DlssNr::Run` then FSR1. This spec
only changes `Run` / `Dispatch`. Feature 1 1:1 and the enlarger are unchanged. Transfer
is available in both Placements.

---

## 6. Resources and bindings

No new scratch textures. `colorCopy` already lives for the whole evaluate.

Always bind `colorCopy` as `t3` on resolve, including Classic (unread there). One
call site, no branch in C++ around descriptors.

### 6.1 Shader registers (unchanged count)

`kSrvCount` stays 5. Slot 3 (`t3`) is currently bound to motion and unread in every
mode. This slice **repurposes `t3` as the full-resolution proxy**.

| Register | Name | Encode | Downsample | Resolve |
|---|---|---|---|---|
| t0 | `gSource` | display colour | `colorCopy` | work proxy \(p_e\) (`colorSmall` or `colorCopy`) |
| t1 | `gModel` | stand-in | stand-in | `g_nr.output` \(m_e\) |
| t2 | `gOriginal` | stand-in | stand-in | `hdrCopy` \(H\) |
| t3 | `gProxy` | stand-in | stand-in | `colorCopy` \(E\), always colour-sized |
| t4 | unused | stand-in | stand-in | stand-in |
| u0 | `gTarget` | `colorCopy` | `colorSmall` | game output |
| u1 | `gKeep` | `hdrCopy` | stand-in | stand-in |
| s0 | `gLinear` | unused | Classic downsample | Classic and Matched residual |

Rename in HLSL: `gMotion` → `gProxy`. Do not keep a motion semantic on that slot.

### 6.2 `DispatchPass` C++

Rename the 4th input from `InMotion` to `InProxy` (same pointer argument, new name).

Resolve call site today:

```cpp
DispatchPass(cmdList, resolveParams, modelInput, g_nr.output, g_nr.hdrCopy, motionIn,
             nullptr, target, nullptr);
```

becomes (both Transfer values):

```cpp
DispatchPass(cmdList, resolveParams, modelInput, g_nr.output, g_nr.hdrCopy, g_nr.colorCopy,
             nullptr, target, nullptr);
```

Encode and downsample still pass `nullptr` for `InProxy` (stand-in = `InSource`).

`motionIn` remains an NGX evaluate input only. Do not bind it to the compose shader.

Downsample constants (required, both Transfer values):

```cpp
DlssNrConstants down {};
down.Mode = DlssNrMode_Downsample;
down.Width = workWidth;
down.Height = workHeight;
down.Transfer = (uint32_t) DlssNr::ConfiguredTransfer();
```

Zero-init `Transfer` is Classic bilinear. That is a bug if the menu is Matched.

### 6.3 Views must be numeric, not `_SRGB` (required)

Area downsample and residual both need the **stored** encode numbers. Hardware sRGB
views filter in linear (`D3D11.3` functional spec). Today `CreateScratch` copies the
output format, and `Shader_Dx12::CreateShaderResourceView` /
`CreateUnorderedAccessView` only remap TYPELESS — `_UNORM_SRGB` stays `_SRGB`.
`CreateUnorderedAccessView` has no format override. Passthrough SDR (the P test)
is this path, not a rare HDR edge.

**Must, in this slice:**

1. `CreateScratch` stores the typed non-sRGB mapping already described in
   `DlssNr_Codec.h::TypedFormat` (`R8G8B8A8_UNORM_SRGB` → `R8G8B8A8_UNORM`, same
   for BGRA). Apply to `colorCopy`, `colorSmall`, `g_nr.output`, and `hdrCopy`
   (float HDR is unchanged). UAV create then inherits a numeric format.
2. If any SRV in this pass would still bind an `_SRGB` format, pass the UNORM
   override into `CreateShaderResourceView`.

Typical HDR path (`R16G16B16A16_FLOAT`) is already numeric. Do not leave this to
PR discovery.

---

## 7. Downsample (Mode 2)

Runs only if `workWidth != width || workHeight != height`. Writes `colorSmall`.

Branch on `gTransfer` (same clamped uint uploaded on this pass — §6.2):

**Classic** — keep the live line, bit for bit:

```
gTarget[id.xy] = gSource.SampleLevel(gLinear, uv, 0);
```

**Matched residual** — encoded exact area (§7.1). Kaiser / Lanczos are a later A/B,
not this slice, and would be a third Transfer value if they ever ship.

### 7.1 Area algorithm (Matched only)

Destination dispatch is `workW × workH` (`gWidth`, `gHeight`). Source size from
`gSource.GetDimensions(srcW, srcH)` — do not pack sizes into unused constants.
Box bounds are **float** (`(x * srcW) / dstW` in float, not uint). Integer
truncation of the box is a bug.

Source texel \(i\) occupies \([i, i+1)\). Destination pixel \((x,y)\) covers

\[
\left[\frac{x \cdot srcW}{dstW},\; \frac{(x+1)\cdot srcW}{dstW}\right)
\times
\left[\frac{y \cdot srcH}{dstH},\; \frac{(y+1)\cdot srcH}{dstH}\right).
\]

Let \(x_0,x_1\) be those float edges. Overlapping integer texels are
\(i = \lfloor x_0 \rfloor \ldots \lceil x_1 \rceil - 1\) (empty if \(x_1 \le x_0\)).
For each overlapping source texel, weight = product of 1D overlap lengths.
`Load` `clamp(i, 0, srcW-1)` (clamp-to-edge: out-of-range indices still contribute
the edge texel). Output is `acc / area` where `area = (x1-x0)*(y1-y0)`.
Do **not** renormalise by the summed weights — that would change edge behaviour.
Weights are non-negative. No negative lobes.

**Do not hard-cap at 5 taps.** `WorkingScale ∈ [0.25, 1]` is typically ≤5 per
axis; `WorkAtNative` uses the guide raster and can be 4K/720p (≈5.3) or
4K/540p (≈7). A hardcoded `i0 .. i0+4` that still divides by the full dest box
darkens the model input. Loop the overlapping range. Do not `[unroll]` a
fixed 5. A dest pixel’s overlap count is at most \(\lceil src/dst \rceil + 1\)
per axis.

If `dst == src`, `Load` and copy (C++ `reduced` should not reach this).

Use `Load`, not `SampleLevel`. Average stored RGB. **Centre-texel alpha:** the
source texel that contains the dest pixel centre
\(((x+0.5)\cdot srcW/dstW,\; (y+0.5)\cdot srcH/dstH)\), clamped. NR does not use
alpha. Do not decode: this is \(D_e(E)\), not \(S(D(P))\).

---

## 8. Resolve (Mode 1)

### 8.1 Decode helper

```
Decode(rgb) = passthrough ? rgb : SrgbToLinear(rgb)
```

`SrgbToLinear` already saturates. That is correct for the encoded cube.

### 8.2 Strength 0 (both options)

Runs only after debug views (§11). After compare UV is known:

```
if (gTransferStrength == 0)
{
    float4 o = (gCompareMode == 1)
        ? gOriginal.SampleLevel(gLinear, cmpUv, 0)
        : gOriginal.Load(int3(id.xy, 0));
    // letterbox / divider still apply
    gTarget[id.xy] = float4(o.rgb, o.a);   // no extra max, no /W
    return;
}
```

`o` is `hdrCopy`. Encode already clamped negatives. T1 requires Compare off so
this is a Load of that keep.

### 8.3 Samples used by both options

```
float3 p = Decode(gSource.SampleLevel(gLinear, cmpUv, 0).rgb);
float3 m = Decode(gModel.SampleLevel(gLinear, cmpUv, 0).rgb);

float4 originalSample = (gCompareMode == 1)
    ? gOriginal.SampleLevel(gLinear, cmpUv, 0)
    : gOriginal.Load(int3(id.xy, 0));

float normScale = passthrough ? 1 : max(gWhitePoint, 1e-4);
float3 H = originalSample.rgb / normScale;
```

Classic does not read `gProxy`. Matched does:

```
float4 eSample = (gCompareMode == 1)
    ? gProxy.SampleLevel(gLinear, cmpUv, 0)
    : gProxy.Load(int3(id.xy, 0));
float3 P = Decode(eSample.rgb);
```

Wipe (`CompareMode == 2`) Loads \(P\) and \(H\). Side-by-side samples at `cmpUv`
(including debug 4/5).

`sameRate` comes from the small source, **not** from `gGuideWidth` (that field is
zero today and is not overloaded in this slice):

```
uint srcW, srcH;
gSource.GetDimensions(srcW, srcH);
bool sameRate = (srcW == gWidth && srcH == gHeight);
```

Do not set `GuideWidth` / `GuideHeight` to work size for this. Do not overload
`MvScale*`.

### 8.4 Classic compose (Transfer == 0)

Live shader, with `proxy = p`, `model = m`, `original = H`. Do not substitute \(P\).
Do not build \(T\). Do not run cube-scale or H1.

This is the default. A user who never opens the combo must see today’s picture at
nonzero strength.

### 8.5 Matched residual — \(T\)

```
float3 T = sameRate ? m : (P + (m - p));
```

**Residual domain (locked).** One resolve pass, no extra UAV:

1. Hardware-bilinear `SampleLevel` on the **encoded** (or native) small textures.
2. `Decode` each sample.
3. \(d = m - p\) in that decoded space, \(T = P + d\).

That is \(S^{-1}(U(m_e)) - S^{-1}(U(p_e))\), not \(U(S^{-1}(m_e) - S^{-1}(p_e))\). A
work-res `RGBA16F` residual is a later A/B if colour still smears. Do not add that
pass now.

Both small textures are sampled at the same `cmpUv`. Do not upsample \(m\) as a
finished picture and call that \(T\).

### 8.6 Keep \(T\) inside the proxy cube (Matched + HDR only)

```
if (!passthrough)
{
    float3 d = T - P;
    float alpha = 1.0;
    [unroll] for (int c = 0; c < 3; ++c)
    {
        if (d[c] > 1e-6)       alpha = min(alpha, (1.0 - P[c]) / d[c]);
        else if (d[c] < -1e-6) alpha = min(alpha, (0.0 - P[c]) / d[c]);
    }
    alpha = saturate(alpha);
    T = P + alpha * d;
}
```

Passthrough: leave \(T\) as computed.

### 8.7 H0 — Matched default lift

Same UpgradeToneMap as Classic, but `proxy = P`, `model = T`, `original = H`.
Empty-model gate is on \(m\), not \(T\):

```
if dot(m, kLuma) <= 1e-5:
    upgraded = H
else
    proxyLuma    = dot(P, kLuma)
    modelLuma    = dot(T, kLuma)
    originalLuma = dot(H, kLuma)
    if originalLuma < proxyLuma: ratio = originalLuma / max(proxyLuma, 1e-6)
    else: ratio = (modelLuma + max(0, originalLuma - proxyLuma)) / modelLuma
    upgraded = lerp(H, HueOkLab(T * ratio, T), TransferStrength)

lumaRatio = clamp((upgradedLuma + 1/512) / (originalLuma + 1/512), 0, MaxRatio)
result = lerp(H * lumaRatio, upgraded, ColourStrength) * normScale
```

OkLab’s hue source is \(T\), not \(H\). `MaxRatio` still only clamps the luma-only mix,
as today.

### 8.8 H1 — Inspect only, Matched only

Empty-model gate first (same \(m\) test as H0): result is \(H \cdot normScale\).

Otherwise:

```
float3 add = H + (T - P);
float3 F   = lerp(H, add, gTransferStrength);
result     = F * normScale;
```

No UpgradeToneMap, no OkLab, no MaxRatio, no `ColourStrength`. Detail strength still
moves the picture; Colour strength is ignored (do not hide the slider — it is a
shared Inspect control). Store `max(result, 0)` with the common compose store.

Allowed to look worse on highlight hue. Default `HdrLift = 0` (H0). Classic ignores
this field.

### 8.9 Debug views

Always listed in the menu (six entries). Do not shrink the combo when Transfer is
Classic — a stored `DebugView == 4` would then sit past the last item.

| `DlssNrDebugView` | Picture | Notes |
|---|---|---|
| 0 | composed \(F\) | Current Transfer + lift |
| 1 | \(U(p) \cdot W\) | Live view 1. What the model was shown |
| 2 | \(U(m) \cdot W\) | Live view 2 |
| 3 | live wrap | `SrgbToLinear(saturate(0.5 + (m-p)*20)) * W` — keep the wrap; do not “simplify” to `0.5+(m-p)*20` |
| 4 | \(P \cdot W\) | Full-res proxy. Classic: write 0 |
| 5 | \(T \cdot W\) | After cube-scale (§8.6), before lift. Classic: write 0 |

Views 1–5 run **before** the strength-0 return, so they work at strength 0 (live
behaviour for 1–3). Views 1–3 always use \(p,m\). Compare wipe / side-by-side
unchanged (original side is `originalSample.rgb`).

---

## 9. Config and menu

| Key | Type | Default | Meaning |
|---|---|---|---|
| `Transfer` | uint | 0 | 0 Classic, 1 Matched residual |
| `HdrLift` | uint | 0 | 0 H0, 1 H1. Used only if Transfer == 1 |

No `LegacyResolve`.

`TransferStrength == 0` remains the identity control (`hdrCopy`). No extra bypass
checkbox. Update the Detail-strength help if it still claims bit-identity with the
upscaler UAV — identity is `hdrCopy`.

**Cost** section (after the resolution slider):

```
Transfer          [ Classic            v ]
                  [ Matched residual     ]
```

Help for Transfer (same tone as Placement / Cost):

> How a below-frame model is brought back onto the picture this pass writes.
>
> Classic is the current path: shrink with bilinear, then fold the small answer
> directly onto the full-resolution picture. It is the default.
>
> Matched residual keeps a sharp copy of the picture the model was shown, adds only
> what the model changed, and then runs the same highlight-aware compose. The shrink
> is an area filter so the model is not fed an aliased thumbnail.
>
> When the model is the same size as this frame the two match. Changing this does
> not rebuild the model; the next frame uses the new shrink and the new compose.

**Inspect:**

- Combo **HDR lift** (enabled only if Transfer == Matched residual):
  `UpgradeToneMap` / `Additive headroom`. Classic ignores the stored value.
- Debug view combo always has six names. Views 4–5 are black on Classic.
- HDR lift help: additive path uses Detail strength; Colour strength does not apply.

Do not mention “legacy”, “A/B only”, or “temporary” in the UI.

---

## 10. File-level work

| File | Change |
|---|---|
| `dlssnr/DlssNr_Modes.h` | `enum class Transfer`; `ConfiguredTransfer()`; `ConfiguredHdrLift()` |
| `shaders/dlssnr/precompile/dlssnr.hlsl` | Mode 2 branch; `gProxy`; Classic vs Matched resolve; H1; strength-0; debug 4/5; empty-model gate on \(m\) |
| `shaders/dlssnr/precompile/DlssNr_Shader.h` | Rebuild CSO (§13) |
| `shaders/dlssnr/DlssNr_Dx12.cpp` | Bind `colorCopy` as slot 3; `CreateScratch` via `TypedFormat`; set `Transfer` on **downsample and resolve**; set `HdrLift` on resolve |
| `shaders/dlssnr/DlssNr_Dx12.h` | Rename `InMotion` → `InProxy` |
| `shaders/dlssnr/DlssNr_Common.h` | Add `Transfer`, `HdrLift` to constants. Do not document `GuideWidth` as work size |
| `Config.h` / `Config.cpp` | `DlssNrTransfer { 0 }`, `DlssNrHdrLift { 0 }`; DebugView comment becomes 0–5 |
| `dlssnr/DlssNr_Menu.cpp` | §9 |
| `dlssnr/README.md` | Two transfers, Classic default. Replace the lines that say composition is “not a delta” and that strength zero is bit-identical to the upscaler UAV |

### 10.1 Constant buffer

Add two uints at the end of `DlssNrConstants` (256-aligned; there is room). Do not
overload `MvScale*` or `GuideWidth` / `GuideHeight`.

```
uint gTransfer;   // 0 Classic, 1 Matched residual
uint gHdrLift;    // 0 H0, 1 H1; ignored unless gTransfer == 1
```

after `gCompareSwap`. HLSL cbuffer order must match C++. Pad explicitly if the
compiler inserts padding; `static_assert` offsets in the PR if needed.

`CreateConstantsBuffer` uploads `sizeof(DlssNrConstants)`.

---

## 11. Shader structure (Mode 1)

```
if id out of range: return
compute uv, cmpUv, showOriginal, onDivider, outsideFrame

sample p, m, H as §8.3

if DebugView == 1,2,3: existing live wrap, using p/m; return
if DebugView == 4 or 5:
    if Transfer != Matched: write 0; return
    build P (and T if 5, after §8.6); write P*W or T*W; return

if TransferStrength == 0: write hdrCopy rgb/a (plus letterbox/divider); return

if Transfer == Classic:
    result = live UpgradeToneMap(H, p, m)   // empty-model gate on m, as today
else:
    build P, T as §8.5–8.6
    if dot(m, kLuma) <= 1e-5: result = H * normScale
    else if HdrLift == 1:     result = lerp(H, H + (T - P), TransferStrength) * normScale
    else:                     result = H0(H, P, T)   // gate already applied

apply showOriginal / letterbox / divider
store max(result,0), original alpha     // compose path only; strength 0 already returned
```

Encode (Mode 0) is unchanged.

Mode 2: `if (gTransfer == 0) bilinear; else area;` — `gTransfer` from **this**
dispatch’s constants.

---

## 12. Performance

| Pass | Classic | Matched residual |
|---|---|---|
| Encode | unchanged | unchanged |
| Downsample | 1 bilinear (today) | area `Load`s over the dest box, work pixels only. Typically ≤5×5 at WorkingScale ≥ 0.25; more under WorkAtNative |
| NR | unchanged | unchanged (same size; different pixels) |
| Resolve | today | +1 full-res `Load` of `colorCopy` |

No extra resource, no extra barrier. Matched add: a fraction of a millisecond at 4K.
If it approaches a Super Resolution pass, the bug is a mistaken extra full-res pass.

\(T\) is in-register. Classic must not pay the area kernel.

---

## 13. Rebuild the CSO

From `OptiScaler/shaders/dlssnr/precompile`:

```bat
..\shader_tools\build_precompiled_shader.bat dlssnr
```

The bat also writes `dlssnr_Shader_Dx11.cso` and `DlssNr_Shader_Dx11.h`. NR has no
Dx11 consume path and those files are not in the tree. **Do not add them.** Commit
only `dlssnr_Shader.cso` and `DlssNr_Shader.h` (`DlssNr_cso`) with the HLSL.

The Dx12 constructor passes `source = nullptr`; a stale header is a silent old
shader.

---

## 14. Test plan

Freeze model version and scene. Use the wipe and debug views. Do not move
Transfer / Colour / WhitePoint sliders in the same take as T2.

**Default matrix for T0–T7 and G:** Placement = Post-process, `WorkAtNative` off,
Output Scaling off, `DlssNrUseProxy` off, Compare off unless a row says otherwise.
“Cost 67% / 100%” means `WorkingScale` 0.67 / 1.0 of **display**, which is also
same-rate vs colour in this matrix. Multi-pass Quality caps the slider at
native/display (often ~67%) — that would make T2/T5 same-rate. Multi-pass is **M
only**.

| ID | Change | Pass if |
|---|---|---|
| TA | Host-side area fixture (float box, no GPU) | 1D overlap weights match the dest interval; 2×2→1 is the mean of four stored RGB; `dst==src` is a copy; a 3→2 box is not uint-truncated |
| T0 | Fresh config, Transfer unset | Looks like today’s live build at default strengths. |
| T1 | `TransferStrength = 0`, either option; Compare 0, Debug 0 | Output bytes match `hdrCopy` (encode keep), not the Capture “before” (`colorCopy`). |
| T2 | Toggle Classic ↔ Matched at WorkingScale 0.67 | Picture changes; no hitch. Log must **not** show `ParkNrFeature` / resolutionChanged / a feature rebuild. A few frames of NGX history drift is allowed; that is not the 30-frame work-size hold. |
| T3 | Toggle at WorkingScale 1.0 | No visible difference (scale-one). INI still records the choice. Combo stays enabled. |
| T4 | Classic, WorkingScale 0.67 | Matches live bilinear + cross-rate compose (nonzero strength). |
| T5 | Matched vs Classic, WorkingScale 0.67 | Matched: less edge halo, original detail kept. Debug-1 may differ (area vs bilinear). |
| T6 | Matched H0 vs H1 | H1 does not amplify highlight boil; hue follow may weaken. Detail strength still moves H1; Colour strength does not. Default H0. |
| T7 | Implementer-only: resolve bind `gSource` = `gModel` for one take (no menu control) | Debug 5 equals debug 4 (\(m=p \Rightarrow T=P\)). Revert the bind. |
| P | Passthrough game (8-bit or non-float) | No double encode on either option. Scratch / views are numeric (§6.3). |
| M | Multi-pass + either Transfer | Feature 1 1:1; FSR1 after `Run`; no double NR. Do not use this row as the T2/T5 scale. |
| G | Timestamps, Matched | Extra vs Classic ≪ one DLSS SR. |

---

## 15. Acceptance

Ship when:

1. TA, T0, and T1 hold.
2. T2 / T3: combo is sticky, instant, and does not rebuild NR.
3. T4: Classic is the live path at nonzero strength.
4. T5: Matched is a real option (visible difference, not a rename).
5. Multi-pass and passthrough run on both values.
6. Matched extra GPU time is under one Super Resolution pass.

Do not start C1 affine until someone is actually using Matched and still loses
hue-across-stripes. Next design would still apply to \(P\) then H0 — not a new
downsample, and not EASU on \(m\).

---

## 16. Decisions locked

| Topic | Choice | Rejected |
|---|---|---|
| Product | One Transfer combo; Classic default | Silent replace; hidden Legacy checkbox; split kernel vs compose |
| Classic | Live bilinear + live UpgradeToneMap | Area shrink while Classic is selected |
| Matched pack | Area + residual onto \(P\) + H0 | Upsample \(m\) as a picture; RGB ratio; \(U(m-p)\) residual UAV |
| Residual domain | Bilinear stored texels, then decode | Work-res decoded residual UAV (later) |
| H1 | Inspect, Matched only; lerp with Detail strength; ignore Colour strength | Fourth user combo; dead sliders; full additive with no lerp |
| Empty model | Gate on \(\mathrm{luma}(m)\) | Gate on \(\mathrm{luma}(T)\) |
| \(s=1\) | Both options: \(T = m\); `sameRate` from `gSource.GetDimensions` | Fit at full res; `GuideWidth` as work size |
| Strength 0 | Copy `hdrCopy` on both options when Debug is off | Algebraic lerp through `/W`; claim on the upscaler UAV; extra `max` |
| Debug | Views 1–5 before strength 0; six menu entries always | Strength 0 first; hide 4/5 on Classic |
| Area taps | Loop the overlapping box; float bounds; clamp-to-edge; geometric `area` | Hard 5-tap cap; uint box; renormalise after dropping taps |
| Numeric views | `CreateScratch` + TypedFormat in this slice | Leave `_SRGB` to PR discovery |
| Extra textures | None | Full-res \(T\) UAV |
| Slot 3 | Always `gProxy` = `colorCopy` | 6th SRV; bind motion; bind only if Matched |
| Runtime | Constants only, next frame; downsample **and** resolve carry `gTransfer` | Feature rebuild on toggle; Transfer only on resolve |
| CSO | Commit Dx12 header + hlsl | Add the bat’s Dx11 outputs |

---

## 17. Open only if implementation hits them

Not algorithm TBDs. Resolve in the PR if they appear:

- `DlssNrConstants` vs HLSL padding after the two new uints.
- Debug 4/5 in side-by-side: sample \(P\) at `cmpUv` (§8.3).
