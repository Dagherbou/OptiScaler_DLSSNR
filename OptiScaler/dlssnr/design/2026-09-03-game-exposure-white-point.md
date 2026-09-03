# Game exposure as paper white (hhkbble rewrite)

> **Rebase note (2026-09-03):** [2026-09-03-rebase-onto-dlss-neural-rendering.md](2026-09-03-rebase-onto-dlss-neural-rendering.md)
> wins where this file conflicts. Approach B still applies on D3D12 source 1. The origin
> WhitePointSource dropdown and scan survive. `ResolveWhitePoint` stays for sources 0 and 2.

**Status:** design for implementation. Rewritten 2026-09-03 for `dlssnr-hhkbble`; review locks
applied. Supersedes the `1c1d39e2` draft of this filename. A plan must be written from **this**
file, not from the old offsets, file list, rebuild commands, or “no native Vulkan pass”
assumption.
**Branch:** `dlssnr-hhkbble`. Line numbers are from the working tree after the matched-residual
corrections (resolve t3 = `colorCopy` / `g_vk.proxy`).
**Path:** Approach B — Encode and Resolve **sample the exposure texture on the GPU**. Zero frames of
lag on the picture. The menu number may lag; it is a readout, not the control signal.

This is a **replacement** of the live Dx12 path (Mode 3 courier → readback → CPU
`ResolveWhitePoint` → `gWhitePoint`), not a second white-point mechanism beside it.

The model is shown `frame / W`. Today `W` is either the Paper white slider or a CPU value
`clamp(PreExposure / gameExposure * slider, 0.01, 4096)` computed from a texel that arrived
**three frames ago**. Some games already hand DLSS an `ExposureTexture` whose job is to say what
the colour buffer’s units are. This document makes that texture the optional source of `W`
**this frame**, with the existing checkbox as a preference the user can turn off.

`GameW` polarity is a **required acceptance gate** (§7.2, §14 T0), not a settled constant. The
starting algebra matches today’s CPU path when `Exposure.Scale` is 1 (FSR’s published
convention). The live `0.01…4096` clamp is **dropped** (§7.2). Invert only `PaperWhite()` /
`GameWhite()` if a real title shows the other way.

This file **supersedes** the Mode 3 t3 row in `2026-09-03-matched-residual-corrections.md`. It
does **not** supersede that file’s resolve t3 row (`colorCopy` / `g_vk.proxy`).

---

## 0. What changed versus the `1c1d39e2` draft

The old draft was written against `dlssnr-pr-multipass` before this branch existed. These
assumptions are **false here** and must not be planned from:

| Old draft | This branch |
|---|---|
| `HdrLift` at offset 72; new fields from 76 | **No `HdrLift`.** Last field is `DebugScale` at 72. New fields still start at 76, appended after `DebugScale`. |
| t3 free / vestigial; t4 = exposure | **t3 is live on resolve.** Binding exposure on resolve t3 makes Matched-at-reduced \(P\) the exposure texel. Exposure is t4. |
| Only rebuild fxc via `build_precompiled_shader.bat` | Rebuild **both** blobs with the §13 commands. Do **not** run that bat. |
| “Do not add a native Vulkan composition path” | Native Vulkan composition **already exists**. It still must **not** sample the game’s `VkImage` (§7.5). |
| Multi-pass `IFeature_Dx12` is a second NR call site | **No Multi-pass NR.** The only Dx12 entry is `EvaluateAfterUpscale`. |
| INI `UseGameExposure` | Keep **`WhitePointFromExposure`**. |
| Slider range `0.25 … 4.0` | Keep **`0.25 … 240`**, logarithmic. |
| Hairline writes `1.0` (normalised white) | Hairline is written **after** `result *= normScale` (`dlssnr.hlsl:717` then `730`). That site is in **frame units**. Today it writes `gWhitePoint` because that *is* `W`. After this change write **`PaperWhite()`** there, not `1.0` and not the slider. |
| Debug views 1–5 use `PaperWhite()` | Views 1–3 already use **`gDebugScale`**. Do not switch them to `PaperWhite()`. |
| `CopyTextureRegion` of the game resource | **Rejected.** Keep the 1×1 Mode 3 courier for the menu / held `E` only. |
| `codec::TypedFormat` / `DlssNr_Codec.h` / `area_fixture.cpp` | **None exist.** Usable helper is `TranslateTypelessFormats`. Fixture is new and standalone. |
| Checkbox “off by default” | Default is already **`true`**. Fix the comment; do not flip the default. |
| `TEXTURE1D` is usable | **2D only.** HLSL is `Texture2D<float4>`. |
| Vulkan `Dispatch` “six views or a seventh, either is fine” | **Six views.** Binding 8 is always the existing dummy inside `WriteDescriptors`. |

What is still true from the old draft: Approach B; three predicates; picture ≠ menu clock; dummy
SRV; no `DLSSD.Exposure` alias; bridges make a **present** texture visible to NR even when
AutoExposure is on; default no barrier; polarity T0; no `g_nrMutex` on the menu.

---

## 1. Why this exists

`W` is a property of how the game stores light, not of how bright this shot is.

A measured frame-mean (the deleted statistical probe) chases content. The live “take white point
from the game” path is the right *source* and the wrong *clock*: Mode 3 copies the 1×1 into a UAV,
a 4-slot ring maps it ~3 frames later, and `ResolveWhitePoint` writes that stale float into
`gWhitePoint`. Encode and resolve then divide by a number the game is no longer using.

Approach B keeps the source and drops the clock. The shader `Load`s texel `(0,0).r` of this
frame’s texture. Readback stays, for the overlay and for the hole’s held `E`.

If the game does not supply a **usable** texture, the buffer is usually already in a space where
`W = 1` is right. Cyberpunk 2077 is the working example. Those games must keep today’s look
(slider only, no dispatch that treats the frame’s top-left pixel as exposure).

---

## 2. Product option

One checkbox, one slider, the **existing** INI keys. The slider is **not** a second white point.
It is one stored float whose **meaning** changes with whether game exposure is actually in force.

| State | Checkbox | Slider label / format | Stored float means | `W` the shader uses |
|---|---|---|---|---|
| Game exposure **effective** | on, enabled | `Paper white` `%.2f×` | multiplier on the game’s `W` | `slider × GameW` |
| Game exposure **off** (user, or not allowed) | off, or disabled | `Paper white` `%.2f` | absolute `W` | `slider` |

The `×` in the format string is Unicode `×` (U+00D7), not ASCII `x`.

`1.0` is identity in both columns. Toggling does **not** rewrite the stored float and does **not**
rescale it (1.25× on `GameW ≈ 80` and absolute 1.25 are different pictures; that is intended).
Range stays `0.25 … 240` in both modes, `ImGuiSliderFlags_Logarithmic`, same as today.

Today’s slider always prints `%.2fx` while often meaning an absolute `W`. That is a lie. The `×`
appears only when the value is a multiplier. The current label split
(`Paper white (x exposure)` vs `Paper white`) goes away: one label, format carries the meaning.

INI (keys unchanged):

```
[DlssNr]
WhitePointFromExposure=true
WhitePointScale=1.0
```

`WhitePointFromExposure` is a **preference**. It is not a promise that this frame used the
texture. Fresh install / missing key: `true` (already `CustomOptional<bool> { true }`). A game
that cannot supply a usable exposure never becomes effective, so the default does not move
Cyberpunk.

Comment fixes in `Config.h` (no new keys):

- `DlssNrWhitePointFromExposure`: delete “Off by default until it has been seen to work”.
- `DlssNrWhitePointScale`: it is the slider (absolute `W` or a multiplier), not “multiplies the
  auto or manual white point”.
- Delete the orphan meter-chase paragraph at `Config.h:288–298` (no field under it).

Unknown / hand-edited bools follow the existing `CustomOptional<bool>` pattern
(`DlssNrAutoCapture` is the default-true precedent).

The Colour menu has **no** Reset (`R`) buttons today. Do not add them.

---

## 3. Non-goals

- Do not bring back frame-mean metering. Do not revive `WhitePointForMean`, `kWhitePointBlend`,
  or `kTargetEncodedMean`. Deleting those unused functions is allowed and **out of scope** unless
  a plan lists them.
- Do not change Classic / Matched residual / UpgradeToneMap / Mode 2 except the source of `W`.
  Do not replace residual-block `gMotion` reads (`dlssnr.hlsl:602–603`) with `gExposure`.
- Do not bind exposure on **resolve** t3. t3 on resolve is `colorCopy` / `g_vk.proxy`.
- Do not rename `gMotion`. Do not add a sixth Dx12 SRV (`kSrvCount` stays 5).
- Do not renumber Vulkan `[[vk::binding]]` 0–7. t4 is **binding 8**.
- Do not add a seventh view to `DlssNr_Vk::Dispatch`.
- Do not rebuild the NR feature, the Dx12 PSO, or scratch textures when the checkbox moves.
  Next `Dispatch` is enough. Both blobs are rebuilt **once** with this change (§13).
- Do not sample the game’s native-Vulkan `ExposureTexture` image. Do not pass its `ImageView`
  into the pass.
- Do not invent a `DLSSD.Exposure` alias. One key: `NVSDK_NGX_Parameter_ExposureTexture`
  (`"ExposureTexture"`). Do not call `GetResource(ExposureTexture, "DLSSD.Exposure")`.
- Do not let the user tick the checkbox when the environment cannot honour it.
- Do not drive the picture from the UI readout. Readback never writes
  `DlssNrConstants.WhitePoint` except the hole row in §7.3, which writes a **synthesized
  absolute `W`**, not the raw texel.
- Do not change `DlssNrUseProxy`’s early return. Encode still uses `PaperWhite()` and t4 per
  §7.3. The courier still runs (§7.6). Resolve does not.
- Do not change `AutoExposure()`, `ForceAutoExposure`, or how the DX12 **upscaler** consumes
  that flag. The NR `Set` is **after** `dx12Feature->Evaluate` (§8.6).
- Do not invent a D3D12 resource state. Default is no barrier (§7.4).
- Do not `CopyTextureRegion` / `CopyResource` the game’s exposure texture.
- Do not throw out of `Dispatch` because an exposure pointer is unusable.
- Do not take `g_nrMutex` on the menu thread.
- Do not shrink the slider to 4.0. Do not rename the INI keys.
- Do not switch debug views 1–3 from `gDebugScale` to `PaperWhite()`.
- Do not add Multi-pass / `IFeature_Dx12.cpp` NR plumbing.
- Do not change the shared Vulkan dummy’s format, layout, or never-cleared contract.

---

## 4. Enablement (the only state machine)

Three predicates plus native Vulkan. Do not collapse them.

```
CanEnable   = sessionHasTexture && !sessionPassthrough && haveEvaluated && !IsRunningVk()
Preference  = Config.DlssNrWhitePointFromExposure   // may be true while CanEnable is false
Effective   = CanEnable && Preference
```

| Predicate | Meaning |
|---|---|
| `haveEvaluated` | `DlssNr_Dx12::Dispatch` has recorded at least one **encode**. Set only there, after the bindless restore-skip (`~1425`) and the create-hold / feature-null / `haveCodec` returns, immediately before encode is recorded. Peeking the parameter block in `EvaluateAfterUpscale` does **not** set it. Native Vulkan does **not** write this latch. |
| `sessionHasTexture` | At least one of those encodes was handed a **usable** `ExposureTexture` (§7.3). Latched for the process. A later null or unusable pointer does **not** clear it. Native Vulkan never sets this. |
| `sessionPassthrough` | The last Dx12 encode’s `isHdrBuffer` was false. Updated every Dx12 encode. |
| `IsRunningVk()` | Native Vulkan is the live path. `CanEnable` is false. Existing function; not a new latch. |

Storage: `std::atomic<bool>` next to `g_nr` for the three Dx12 latches. `Dispatch` already holds
`g_nrMutex` when it writes them. The menu reads atomics **without** that mutex. Never hand the
game `ID3D12Resource*` to ImGui.

Lifetime:

- **Park / resolution rebuild:** invalidate in-flight readback slots for the overlay
  (`meterExposureValid[]` / slot fences). Do **not** zero the last finite held `E`. Do **not**
  clear `CanEnable` latches. Do **not** park the dummy.
- **`Shutdown`:** clear the three Dx12 latches, held `E`, dummy, and the readback ring. Today’s
  `Shutdown` (`~1981`) does not zero `exposureFrames` / `gameExposure`; this change must.

`AutoExposure` does **not** enter `CanEnable`. If a title’s texture is garbage, the user turns
the checkbox off.

`ForceAutoExposure` / AutoExposure games that **never** publish a usable texture stay at
`CanEnable == false`. The slider remains an absolute `W`.

### 4.1 What the user can do

- **Checkbox interactable** iff `CanEnable`. Otherwise `BeginDisabled`.
- **Checkbox shown checked** iff `Effective`. When `CanEnable` is false, pass a `false`
  temporary into `ImGui::Checkbox` so it draws unchecked even if `Preference` is still true in
  the INI. Only write `Preference` when `CanEnable` and the user toggles.
- Losing `CanEnable` does **not** write `Preference = false`.

### 4.2 What the slider means

Follow **`Effective`**, not `Preference` alone and not per-frame holes.

- `Effective == true` → multiplier. Format `%.2f×`. Help: multiplier.
- `Effective == false` → absolute `W`. Format `%.2f`. Help: absolute.

A missing texture on one evaluate must not flip the slider format. See §7.3 for that frame’s
`W`.

### 4.3 Status line (exactly one, always visible under the checkbox)

`ImGui::Text` / `TextDisabled` / green `ImVec4(0.45f, 0.8f, 0.45f, 1.0f)` (today’s green at
`DlssNr_Menu.cpp:357`). Not a second checkbox.

| Condition (first match) | Colour | Copy |
|---|---|---|
| `IsRunningVk()` | disabled | `This path cannot sample the game’s exposure. Paper white below is in use.` |
| `!haveEvaluated` | disabled | `Waiting for Neural Rendering.` |
| `sessionPassthrough` | disabled | `This buffer is already tone-mapped. Paper white is unused.` |
| `!sessionHasTexture` | disabled | `This game does not supply an exposure.` |
| `CanEnable && Effective` | green | `Using the game’s exposure.` |
| `CanEnable && !Effective` | green | `This game supplies an exposure.` |

Drop today’s orange “Tick above to use it” and the orange Cyberpunk line on an interactable
box.

When `IsRunningVk() && ExposureOfferedVk()`, **append** this sentence to the checkbox help
marker (not a second status line): `This game offered one; it is not sampled on this path.`

### 4.4 Numeric readout

Under the slider, only when `CanEnable`. `P` / `S` from the latest CPU `Get`. `E` / `game white`
from the last successful courier readback.

```
Exposure 0.012  ·  pre 1.00  ·  scale 1.00  ·  game white 83.3  ·  in use 83.3
```

If `Effective` is false, replace `in use …` with `not in use`.

`game white` is `GameWhite(e, p, s)`. `in use` is `slider × gameW`.

Until the first successful readback: `Exposure —` and `game white —`. Never print `0` as a fake
exposure.

Help marker: `Shown for the menu. The pass samples the texture on the GPU; this number can lag
a frame or two.`

When `!CanEnable`, print nothing numeric.

---

## 5. Menu copy

Replace `DlssNr_Menu.cpp` ~293–403. No `R` buttons.

**Blurb** (disabled text; fold today’s passthrough sentence in):

```
The model was trained on finished, sRGB-encoded frames. The upscaler’s
output is not one: it is linear and open-ended. Paper white is how that
buffer is mapped into something the model recognises. A frame the game
reports as already tone-mapped is passed over untouched and none of
this applies.
```

**Checkbox** label: `Take white point from the game`

Help marker (checkbox). When `IsRunningVk() && ExposureOfferedVk()`, append the sentence in
§4.3.

```
When a game hands DLSS a 1×1 exposure texture, that texture is the
engine’s own scale for the colour buffer. Tick this to derive paper
white from it instead of guessing.

The box is disabled when this session has not seen such a texture, when
the buffer is already tone-mapped, or on native Vulkan (the image
layout is not known, so it is not sampled).

The preference is kept if the box is disabled. A later D3D12 game that
does supply an exposure will use it again.
```

**Slider** label: `Paper white` in both modes.

Help when **`Effective`**:

```
A multiplier on the white point taken from the game. 1.00× means use
that value as-is.

Above 1 the picture handed to the model is darker; below 1, brighter.
If a game still looks washed out after taking its exposure, this is
the bias.

At detail strength 0 this does not move the edited picture.
```

Help when **not `Effective`**:

```
What the frame is divided by before the model sees it.

The model was trained on finished frames where white sits at 1. The
upscaler’s output is linear and open-ended, so something has to say
where white is, and 1.0 is right for most games that do not supply
an exposure.

Above 1 the picture handed over is darker; below 1, the opposite.
If a game looks washed out or flat, this is the first thing to move.

At detail strength 0 this does not move the edited picture.
```

Drop the current slider help’s “There is no other white point”, the Monster Hunter paragraph,
and the “this was once a multiplier on a measured white point” history.

---

## 6. Contracts (must hold)

| Name | Rule | How |
|---|---|---|
| Strength 0 | Unchanged picture | Encode and Resolve use the **same** `W` this dispatch. Strength 0 still writes `hdrCopy`. Compare off, debug off. |
| Same `W` both passes | Encode and Resolve agree | Same four values on **both** constant structs: `WhitePoint`, `UseGameExposure`, `PreExposure`, `ExposureScale`. Same t4. Same `PaperWhite()`. Dx12 today builds `resolveParams` as a fresh `{}` (`~1631`): `resolveParams = encodeParams` then overwrite `Mode` / strengths / compare / debug as today. Vulkan already does `resolve = encode`. Downsample may leave the new fields zero. |
| Identity slider | `1.0` does nothing extra | Absolute: `W = 1`. Multiplier: `W = GameW`. |
| No rescale on toggle | Stored float is untouched | Do not rewrite `WhitePointScale` when the box moves. |
| Unsupported cannot enable | Checkbox not interactable | `BeginDisabled(!CanEnable)`. Drawn unchecked via a `false` temporary. |
| Slider matches Effective | Format and help follow `Effective` | Unicode `×` only when `Effective`. |
| Passthrough | No encode curve, no `W` | Existing early-return at `dlssnr.hlsl:448–451`. `normScale` stays `1.0` when `gPassthrough != 0`. `gUseGameExposure` is 0. |
| No usable texture this process | Today’s look | `Effective` false. `W = slider`. No Mode 3. |
| Picture ≠ menu clock | GPU sample is authoritative | Readback never feeds `WhitePoint` except the §7.3 hole row. |
| Typed + untyped | Bridges work | `Get` as `ID3D12Resource**` then `void**`, one name. |
| One Dx12 call site | No Multi-pass fill | `EvaluateAfterUpscale` only. Invoked from `NVNGX_DLSS_Dx12.cpp` (2), `IFeature_Dx11wDx12.cpp:421`, `IFeature_VkwDx12.cpp:2112`. |
| Bridges + AutoExposure | Present texture is visible to **NR** | §8.6. Upscaler `Evaluate` is unchanged. Missing + AE is not an error. |
| Dummy SRV (Dx12) | No empty descriptor | t4 is always a valid 2D SRV. Missing / unusable → 1×1 `R32_FLOAT` of `1.0`. **Not** `InSource`. `DispatchPass` 5th-slot null → this dummy. |
| Bindable or dummy | No throw, no device removal | §7.3. |
| Bad texel | No NaN `W` | §7.2. `Load` only after the flag check. |
| Debug stays still | Instrument ≠ live `W` | Views 1–3 keep `gDebugScale`. |
| Hairline is frame-unit white | Line stays visible when `GameW` is large | After `result *= normScale`, write `PaperWhite()`, not `1.0`, not `gWhitePoint`. |
| Default no barrier | Native path stays safe | §7.4. |
| Menu lock-free | Overlay does not hitch | Atomics only. |
| Polarity gate | Wrong `GameW` does not ship | §14 T0. |
| t3 residual contract | Unchanged | Resolve t3 remains `colorCopy` / `g_vk.proxy`. |
| Native Vulkan | Slider only | Six-view `Dispatch`. Binding 8 = existing dummy. `gUseGameExposure = 0`. |
| INI / range | Unchanged keys and travel | `WhitePointFromExposure`, `WhitePointScale`, `0.25…240` log. |
| Clamp | No `0.01…4096` | `max(W, 1e-4)` only. Intentional vs today’s CPU clamp. |

---

## 7. Shader

Live shader is `OptiScaler/shaders/dlssnr/precompile/dlssnr.hlsl`, entry `CSMain`. A stale
`DlssNr_Shader.h` / `DlssNr_Shader_Vk.h` is a silent old shader (Dx12 constructor passes
`source = nullptr`; Vulkan loads `dlssnr_spv`).

### 7.1 Constants

Append after `DebugScale`. Do not reorder existing fields. `Transfer` stays at 68.

```
offset  0  uint   Mode
offset  4  float  WhitePoint          // slider, except the §7.3 hole row
offset 68  uint   Transfer
offset 72  float  DebugScale
offset 76  uint   UseGameExposure     // 1 only when Effective && this-frame usable
offset 80  float  PreExposure         // NVSDK_NGX_Parameter_DLSS_Pre_Exposure; 0 → 1
offset 84  float  ExposureScale       // NVSDK_NGX_Parameter_DLSS_Exposure_Scale ("DLSS.Exposure.Scale"); 0 → 1
```

```cpp
static_assert(offsetof(DlssNrConstants, Transfer) == 68, ...);
static_assert(offsetof(DlssNrConstants, DebugScale) == 72, ...);
static_assert(offsetof(DlssNrConstants, UseGameExposure) == 76, ...);
static_assert(offsetof(DlssNrConstants, PreExposure) == 80, ...);
static_assert(offsetof(DlssNrConstants, ExposureScale) == 84, ...);
static_assert(sizeof(DlssNrConstants) == 256, "CBV size is 256-aligned");
```

HLSL, immediately after `gDebugScale`, same order, no extra padding:

```
uint  gUseGameExposure;
float gPreExposure;
float gExposureScale;
```

There is no `HdrLift`. Do not add one.

### 7.2 `PaperWhite()` and polarity

Used at exactly two picture sites, plus the hairline:

| Site | Today | After |
|---|---|---|
| Encode `dlssnr.hlsl:466` | `SoftKnee(frame / max(gWhitePoint, 1e-4))` | `SoftKnee(frame / PaperWhite())` **below** the passthrough return at 448–451. Do not call `PaperWhite()` on that early-return path. |
| Resolve `normScale` at 526 | `gPassthrough != 0 ? 1.0 : max(gWhitePoint, 1e-4)` | `gPassthrough != 0 ? 1.0 : PaperWhite()` |
| Hairline at 730 (after `result *= normScale` at 717) | `float3(gWhitePoint, …)` | `float3(PaperWhite(), PaperWhite(), PaperWhite())` |

Not used by debug views 1–3 (`gDebugScale`). Not used by residual \(P\).

```
float PaperWhite()
{
    float slider = max(gWhitePoint, 1e-4);
    if (gUseGameExposure == 0)
        return slider;

    float e = gExposure.Load(int3(0, 0, 0)).r;
    if (!(e > 1e-6 && e < 1e6))
        e = 1.0;

    float s = max(gExposureScale, 1e-4);
    float p = max(gPreExposure, 1e-4);
    float gameW = p / max(e * s, 1e-4);   // starting polarity; see T0
    return max(slider * gameW, 1e-4);
}
```

Do not hoist `gExposure.Load` above the flag check. Do not reintroduce
`clamp(..., 0.01, 4096)`.

Host mirror `GameWhite(e, p, s)` in `dlssnr/DlssNr_Exposure.h` (pch-free, no d3d12). Same
gates, same starting `p / (e * s)`, same `max(..., 1e-4)`. A commented invert line
(`// return e * s / p;`) is what T0 flips in both copies.

\[
\mathrm{GameW} = \frac{P}{E \cdot S}, \qquad
W = \begin{cases}
\mathrm{gWhitePoint} \times \mathrm{GameW} & gUseGameExposure = 1 \\
\mathrm{gWhitePoint} & \text{otherwise}
\end{cases}
\]

On a normal Effective frame `gWhitePoint` is the slider. On a hole it is already
`slider × GameWhite(heldE, thisP, thisS)` and the flag is 0, so `PaperWhite()` returns that
absolute `W` unchanged.

**T0.** Title that publishes a usable texture (GTA V Enhanced is the known publisher on this
fork; any other publisher is fine). Same scene, strength > 0, wipe on, debug off:

1. Box off, hand-tune the absolute slider until the edited half looks right. Note `W_abs`.
2. Box on, slider `1.00×`. The picture must match `W_abs` (same order of magnitude; not
   crushed vs blown).
3. If inverse, change **only** the `gameW` line to `e * s / p` in HLSL and in `GameWhite()`.
   Record the title. Do not change §4, the SRV slot, or the menu. Do not rescale the slider.

### 7.3 t4 and a usable resource

**t4 becomes `gExposure`.** `kSrvCount` stays 5. Dx12 root signature does not change. Rename
`DispatchPass`’s `InPrevEdit` to `InExposure` **in place** (it stays the 5th argument). Do not
swap it with `InMotion`.

```
#ifdef VK_MODE
[[vk::binding(8, 0)]]
#endif
Texture2D<float4> gExposure : register(t4);
```

Declare it immediately after `gMotion` (t0–t4 together). Do **not** give it
`[[vk::binding(5, 0)]]` — that is `gTarget`.

`CreateShaderResourceView` **throws** on null, `DENY_SHADER_RESOURCE`, `BUFFER`, and
`UNKNOWN` (`Shader_Dx12.cpp:189–250`). It will happily build a 1D or MSAA view that does **not**
match `Texture2D<float4>`. Usable must exclude those so `Dispatch` never throws.

**Usable** (all of these):

1. `res != nullptr`
2. `GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D`
3. `DepthOrArraySize == 1`
4. `SampleDesc.Count == 1`
5. `DENY_SHADER_RESOURCE` is not set
6. Let `viewFmt = Shader_Dx12::TranslateTypelessFormats(desc.Format)`. If `viewFmt == desc.Format`
   **and** `desc.Format` is a `*_TYPELESS` **resource** enum (the translate switch did not map
   it), unusable. `R16_TYPELESS` is this case. Do **not** reject a *mapped* view format whose
   name still contains `TYPELESS` (e.g. `R24G8_TYPELESS` → `R24_UNORM_X8_TYPELESS`). Do **not**
   use `TypedGuideFormat` (`R16_TYPELESS` → `UNORM`).

1D, 2D arrays, 3D, MSAA, buffers, DENY, leftover-typeless → dummy, no throw. Keep
`Texture2D<float4>` and `Load(int3(0,0,0)).r`.

Only a usable resource may latch `sessionHasTexture` or be bound as encode/resolve t4 when
`Effective`.

**`DispatchPass` 5th slot:** change the fallback **inside** `DispatchPass`
(`DlssNr_Dx12.cpp:1056`) from `InSource` to the Dx12 dummy. Other slots may keep the `InSource`
stand-in. Callers still pass the dummy or the game resource; they must not rely on “we always
pass non-null.”

Dx12 dummy: one 1×1 `R32_FLOAT`, value `1.0`, created with the compose pass, released in
`Shutdown`. Survives park. Separate from the Vulkan dummy.

| This frame | `gUseGameExposure` | encode/resolve t4 | `gWhitePoint` |
|---|---|---|---|
| `Effective` and usable | `1` | game resource | slider (the stored float) |
| `Effective` and hole, held `E` exists | `0` | dummy | `slider × GameWhite(heldE, thisP, thisS)` |
| `Effective` and hole, no held `E` yet | `0` | dummy | slider |
| not `Effective` | `0` | dummy | slider |

Hole = `Effective` in the UI, this frame’s pointer null or unusable. Do **not** bind a
last-frame pointer. Do **not** run `gUseGameExposure=1` against dummy `E=1`. GTA V dropped the
texture three times in one session; the hole **keeps the user multiplier** on the held `E`
(`slider × GameWhite(...)`, not bare `GameW` and not bare `slider` when a held `E` exists).
Log the hole at most once per process.

Held `E` is the last finite Mode 3 readback. `thisP` / `thisS` are this frame’s CPU scalars.

SRV format for a usable game resource: `viewFmt` from the usable check. `Load` on `.r` is
enough for `R32_FLOAT`, `R16_FLOAT`, and RGBA stand-ins.

### 7.4 Barriers (Dx12)

Use the pointer only inside this `Dispatch`. Do not cache it.

- **Default:** no barrier. Mode 3 already `Load`s as an SRV without a transition.
- **If `Config::ExposureResourceBarrier` has a value** (INI `[Hotfix] ExposureResourceBarrier`,
  integer `D3D12_RESOURCE_STATES`; any set value is enough for the test row):
  transition `(D3D12_RESOURCE_STATES)value` → `NON_PIXEL_SHADER_RESOURCE` **before the first
  dispatch that binds the game resource as t4**, and restore to that same integer **after the
  last** such dispatch.

  First / last this frame (game resource on t4):
  - `Effective && usable` and resolve runs: first = encode, last = resolve (Mode 3 sits
    between).
  - `Effective && usable` and `UseProxy` returns (no resolve): first = encode, last = Mode 3.
  - `!Effective && usable` (courier only): first = last = Mode 3.
  - not usable: no transition.

DX11 / Vulkan w/Dx12 copies are already in a known readable state at the hand-off. Still apply
the hotfix if set.

### 7.5 Native Vulkan: do not sample the game image

`DlssNrFeature_Vk.cpp:394–404` stays policy.

Locked consequences:

- Keep the **six-view** `Dispatch` signature. Do not add a seventh view.
- `WriteDescriptors` always writes binding 8 with the **existing** dummy
  (`readInfo(VK_NULL_HANDLE)` → `_dummyView`, `VK_IMAGE_LAYOUT_GENERAL`). Same path as today’s
  null t1–t3.
- Do not change the dummy’s format (`R16G16B16A16_SFLOAT`), `UNDEFINED → GENERAL` once, or
  never-cleared contract. It is **not** the Dx12 `R32_FLOAT = 1.0` image. `PaperWhite()` must
  not `Load` it (`gUseGameExposure` stays 0).
- Pool `COMBINED_IMAGE_SAMPLER` count `4 * kSlots` → `5 * kSlots`. Add
  `CreateBinding(8, COMBINED_IMAGE_SAMPLER)`. Do not renumber 0–7.
- Update the “seven resources / four read views” comments (`DlssNr_Vk.h:12–15, 58–59`,
  `DlssNr_Vk.cpp:119`) to eight resources / five reads.
- `EvaluateAfterUpscaleVk` must not pass the peeked exposure `ImageView`. `WhitePoint` = slider.
- Keep the existing peek + log of `ExposureTexture` / `DLSS.Pre.Exposure`. It does not enable
  the box.

Vulkan **w/Dx12** is not this path.

### 7.6 Mode 3 (menu + held `E` only)

Mode 3 is no longer the picture courier. One dispatch site, not two.

**When:** this frame’s `frame.ExposureTexture` is usable, **independent of `Effective`**.
**Where:** after encode, before downsample, **before** the `UseProxy` return
(`DlssNr_Dx12.cpp:1547–1565`). That way proxy still fills the ring. Do not also run it after
resolve.

**Binds (after-lines).** `DispatchPass(cmdList, constants, InSource, InModel, InOriginal,
InMotion, InExposure, OutTarget, OutKeep)`:

| Pass | InMotion (t3) | InExposure (t4) | Notes |
|---|---|---|---|
| Meter (Mode 3) | `nullptr` (unread `InSource` stand-in) | usable game resource | 1×1. Tile 0 = `gExposure.Load`. |
| Encode | `nullptr` | §7.3 table | |
| Downsample | `nullptr` | same t4 as encode (unread) | |
| Resolve | `g_nr.colorCopy` | §7.3 table | t3 **stays** `colorCopy`. |

Vulkan encode/resolve argument order does not change. Binding 8 is dummy inside the pass.

HLSL tile 0: change `gMotion.Load` to `gExposure.Load`. Update comments at `dlssnr.hlsl:220`
and `339–343` so they no longer say the meter binds exposure on t3.

Do not dispatch Mode 3 when the pointer is missing or unusable. Once per process, log when the
courier *does* run (width, height, format, `P`, `S`) so Cyberpunk “no Mode 3” is checkable.

Today the meter sits before encode and wraps `target` UAV → SRV → UAV (`~1469–1474`). **Move
that existing pair with the courier.** Do not invent a second barrier recipe.

The 64×64 tile-mean branch stays dead. Shrinking the meter UAV is out of scope.

**Delete** `ResolveWhitePoint`. Do not leave a hollow function.

---

## 8. CPU plumbing

### 8.1 `DlssNrFrameInfo`

The struct already has `ExposureTexture` and `PreExposure` (`DlssNr_Common.h:80–85`). Add:

```cpp
float ExposureScale = 1.0f;   // 0 already folded to 1 at the reader
```

Always write `frame.ExposureTexture`, never `frame.Exposure`. Slider stays in Config.

### 8.2 Reader (one function, one NR call site)

```cpp
struct NrExposure
{
    ID3D12Resource* texture = nullptr;
    float pre = 1.0f;
    float scale = 1.0f;
};
NrExposure ReadNrExposure(NVSDK_NGX_Parameter* params);
```

- Resource: `Get` `NVSDK_NGX_Parameter_ExposureTexture` as `ID3D12Resource**`, then as
  `void**`. No second name.
- `NVSDK_NGX_Parameter_DLSS_Pre_Exposure`, `NVSDK_NGX_Parameter_DLSS_Exposure_Scale`: `Get`
  float; missing or `0` → `1`.
- Used only by `EvaluateAfterUpscale`. Replace `~1869–1878`.

Usable is decided inside `Dispatch`.

Order on a path that will record encode:

1. `isHdrBuffer` (already here).
2. Early-outs that do not encode: do **not** write `haveEvaluated`. Do not increment
   `exposureFrames` as a stand-in (that field is deleted, §8.3).
3. When encode will be recorded:
   1. `usable = ExposureUsable(frame.ExposureTexture)`
   2. `haveEvaluated = true`
   3. `sessionPassthrough = !isHdrBuffer`
   4. if `usable` → `sessionHasTexture = true`
   5. `Effective = CanEnable && Preference` using the updated latches
   6. Fill **encode and resolve** constants (`UseGameExposure` is 1 only when
      `Effective && usable`; `WhitePoint` per the §7.3 table)
   7. Encode (t4 per §7.3)
   8. Mode 3 if `usable` (t4 = game resource, even when encode used dummy)
   9. Downsample if reduced
   10. `UseProxy` return, **or** evaluate + resolve (resolve t3 = `colorCopy`, t4 per §7.3)
4. Hotfix restore after the last game-t4 dispatch (§7.4)

### 8.3 Menu queries

Declared in `DlssNrFeature_Dx12.h`, implemented in `DlssNr_Dx12.cpp`:

```cpp
bool GameExposureCanEnable();
bool GameExposureEffective();
enum class GameExposureWait { NativeVulkan, Waiting, Passthrough, Absent, Available };
GameExposureWait GameExposureUiState();   // first match in §4.3

struct GameExposureReadout
{
    bool haveNumbers;
    float e, p, s;
    float gameW;
    float slider;
};
GameExposureReadout GameExposureMenuReadout();
```

**Delete** `GameExposureStatus`, `ExposureStatus`, `exposureFrames`, `exposureOfferedNow`,
`exposureEverOffered` from the Dx12 path. The menu calls only the four functions above plus
`IsRunningVk` / `ExposureOfferedVk`. Do not leave two status machines.

### 8.4 Readback (menu + hole `E`)

Keep the existing 4-slot `meterReadback` ring and fence distance. Map only when the slot is
old enough. Never stall. Never write the decoded `E` into `WhitePoint` except by producing
held `E` for the §7.3 hole formula.

Decode `R32_FLOAT` from the pass’s own meter UAV (tile 0).

Park: §4 lifetime. Overlay shows `—` until a new slot returns; held `E` for the hole stays.

### 8.5 Logging

Once per process: first usable texture (width, height, format, `P`, `S`) — this is also the
Mode 3-ran log. Once: texel fails the range gate (readback, best effort). Once: UI-`Effective`
hole. Once: non-null pointer fails `ExposureUsable`.

Existing “game exposure X -> white point would be Y” lines must not claim the CPU float is
what the encode used.

### 8.6 Bridges: NR sees a DX12 pointer or null

`EvaluateAfterUpscale` already runs while the DX12 copies are in the block (DX11 restore and
Vulkan null-out happen after). Keep that order around NR.

**Do not** `mask |= Exposure` unconditionally.

**Do not** `Set` the exposure copy before `dx12Feature->Evaluate`. That would change what the
upscaler sees on AE-on titles. Sequence in both bridges:

1. Prepare / copy as below.
2. `dx12Feature->Evaluate` with **today’s** exposure `Set` rules (`!AutoExposure()` only).
3. **Then**, before `EvaluateAfterUpscale`: `Set(ExposureTexture, dx12Copy)` if a DX12 copy
   exists this frame, else `Set(ExposureTexture, nullptr)`.
4. `EvaluateAfterUpscale`.
5. Restore the original pointer as today.

The block at step 3 contains a DX12 resource or `nullptr`. Never a D3D11 or Vulkan handle.
`ReadNrExposure` then `GetDesc()` is safe. Do not `Set` a stale last-frame cache pointer.

**`IFeature_Dx11wDx12` / `ProcessDx11Textures`**

1. Peek `ExposureTexture` (existing D3D11 Get) **before** `PrepareUpscalerResources`.
2. `mask |= Exposure` if the pointer is present **or** `!AutoExposure()`.
3. If absent and `AutoExposure()`: do not add Exposure to the mask.
4. `MissingExposure` handler: `if (prepareResult.MissingExposure && !AutoExposure())` then
   today’s force-AE + `changeBackend`. When AE is on, Exposure is optional; a miss skips the
   NR Set (step 3 above), it does not fail Prepare or flip the backend.
5. Do not edit `with_dx12/dx11_with_dx12.cpp` cache-required semantics.

**`IFeature_VkwDx12`**

1. `Get` `ExposureTexture` even when `AutoExposure()` is true (today the Get is inside
   `if (!AutoExposure())` at 1067).
2. If present: create `ExpCopy` the same way `1080–1081` does when AE is off, then
   `CopyTextureFromVkToDx12` as today. Copy failure while AE is on: skip NR exposure (Set
   nullptr), do **not** fail the whole evaluate.
3. If absent and `AutoExposure()`: do nothing (no `changeBackend`).
4. If `!AutoExposure()` and (Get failed **or** `NvVkResourceNotValid`): keep today’s warn +
   force AE. Do not copy the live `Get != Success && !NvVkResourceNotValid` condition
   (`1069–1076`) — failed Get leaves `nullptr`, `NvVkResourceNotValid(nullptr)` is true, so
   that `&&` almost never fires.

Do not change `AutoExposure()` itself. Native DX12 needs no bridge change.

---

## 9. Data flow

```
NGX block ── ReadNrExposure ─► FrameInfo.ExposureTexture / PreExposure / ExposureScale
                │
                ▼
EvaluateAfterUpscale → Dispatch (encode path only)
  usable?  latch sessionHasTexture
  haveEvaluated = true
                │
  Effective && usable ── encode/resolve t4 = game, flag=1, WhitePoint=slider
  Effective && hole   ── encode/resolve t4 = dummy, flag=0,
                         WhitePoint=slider×GameWhite(heldE,thisP,thisS) or slider
  otherwise           ── encode/resolve t4 = dummy, flag=0, WhitePoint=slider
                │
                ▼
  Encode: SoftKnee(frame / PaperWhite())     ◄── picture
  Mode 3 1×1 if usable (t4 = game)           ◄── menu + held E
  Downsample / UseProxy return / Resolve
  Resolve hairline: PaperWhite() after * normScale
  debug 1–3: * gDebugScale
                │
                ▼
  hotfix restore if any
```

w/Dx12: peek + copy happen before the upscaler; NR `Set` is after the upscaler, before this
diagram.

Native Vulkan: skip the GPU sample; `W = slider`; binding 8 = dummy; no Mode 3.

---

## 10. Interaction walkthrough (must stay true)

1. **Cyberpunk, first launch (Dx12).** Preference default true. First encode: no usable
   texture, linear HDR → status `This game does not supply an exposure.`, box disabled and
   unchecked, slider `1.00` absolute. Picture = today’s (slider `W`). No Mode 3 (no first-usable
   log).
2. **A title with a usable texture, first launch (Dx12).** After the first encode the box
   enables and shows checked. Slider becomes `1.00×`. `W = GameW` **this frame**. Status
   `Using the game’s exposure.` Readout fills when the fenced slot completes.
3. **User unticks.** Box enabled, unchecked. Status `This game supplies an exposure.` Slider
   `1.00` absolute. `W = 1`. Mode 3 still runs (usable). Preference persisted false.
4. **User ticks again, drags to 1.25×.** `W = 1.25 × GameW`. Stored float `1.25`. Untick:
   slider shows `1.25` (no `×`), `W = 1.25`. Tick: `1.25×` on `GameW` again. Float not
   rewritten.
5. **Passthrough title.** After encode: box disabled, unchecked; tone-map status; slider
   absolute; no readout.
6. **Menu opened before the first NR encode** (create-hold included). Waiting line; slider
   absolute; no readout.
7. **Strength 0.** Edited pixels = `hdrCopy` (compare off, debug off).
8. **DX11 or Vulkan w/Dx12, AutoExposure on, texture present.** After the first encode the
   box can enable. Upscaler `Evaluate` still ran with today’s AE-on Sets. NR saw the DX12 copy.
9. **DX11 or Vulkan w/Dx12, AutoExposure on, no texture.** Box stays disabled. No backend
   change. `ExposureTexture` was `nullptr` at NR, not a D3D11/Vulkan handle.
10. **Native Vulkan.** Box disabled. Native-Vulkan status. Slider absolute. Picture = today.
    Validation clean. If the game offered a texture, checkbox help says so.
11. **Unusable pointer.** Absent this frame. No throw. If one was usable earlier: hole,
    `WhitePoint = slider × GameWhite(heldE, thisP, thisS)` (or slider if no held `E`), UI stays
    multiplier.
12. **GTA V drops the texture for a frame.** UI stays multiplier. Picture uses
    `slider × GameWhite(heldE, thisP, thisS)` once a readback has landed, not a flash to
    `W ≈ slider`.
13. **Park / resolution rebuild.** Box stays in whatever `CanEnable` it had. Readout may show
    `—`; held `E` for the next hole stays.
14. **`UseProxy` on.** Encode uses `PaperWhite()` and t4 per §7.3. Mode 3 still runs if
    usable. Resolve does not.

---

## 11. Files

| File | Change |
|---|---|
| `shaders/dlssnr/precompile/dlssnr.hlsl` | `gExposure` t4 binding 8; three CB fields; `PaperWhite()` at 466 / 526 / 730 as §7.2; Mode 3 tile 0 + comments 220 / 339–343; rebuild §13 |
| `shaders/dlssnr/precompile/DlssNr_Shader.h` + `.cso` | Rebuild (`DlssNr_cso`) |
| `shaders/dlssnr/precompile/DlssNr_Shader_Vk.h` + `.spv` | Rebuild (`dlssnr_spv`) |
| `shaders/dlssnr/DlssNr_Common.h` | `ExposureScale` on `FrameInfo`; three fields + asserts |
| `shaders/dlssnr/DlssNr_Dx12.cpp` | reader, `ExposureUsable`, latches, dummy, `DispatchPass` 5th→dummy, four after-lines in §7.6, hotfix span, courier after encode (move today’s `target` UAV↔SRV pair with it), queries; **delete** `ResolveWhitePoint`; delete peek-machine fields |
| `shaders/dlssnr/DlssNr_Dx12.h` | 5th SRV is `InExposure` |
| `shaders/dlssnr/DlssNr_Vk.cpp` / `DlssNr_Vk.h` | binding 8 + pool count; dummy via `readInfo(VK_NULL_HANDLE)`; six-view signature unchanged; comments 7→8 / 4→5 |
| `dlssnr/DlssNrFeature_Vk.cpp` | flag 0; slider `W`; do not pass the game view |
| `dlssnr/DlssNr_Exposure.h` | pch-free `GameWhite()` |
| `dlssnr/tests/exposure_fixture.cpp` | §12 |
| `dlssnr/DlssNrFeature_Dx12.h` | new queries; delete `ExposureStatus` / `GameExposureStatus` |
| `upscalers/IFeature_Dx11wDx12.cpp` | §8.6 |
| `upscalers/IFeature_VkwDx12.cpp` | §8.6 including `ExpCopy` when AE on |
| `Config.h` | comment-only |
| `dlssnr/DlssNr_Menu.cpp` | §5 |
| `dlssnr/README.md` | one line on game exposure this-frame on D3D12; add the Vulkan blob command to “Editing the shader” |

Do not touch NGX evaluate, guides, WorkingScale, Mode 2, residual binds, or
`with_dx12/dx11_with_dx12.cpp`. Do not edit `IFeature_Dx12.cpp`.

---

## 12. Test plan

`dlssnr/tests/exposure_fixture.cpp` is a standalone `main` with asserts. Header is pch-free.

```
cl /nologo /EHsc /std:c++17 /W4 /I OptiScaler OptiScaler\dlssnr\tests\exposure_fixture.cpp /Fe:exposure_fixture.exe
exposure_fixture.exe
```

`#include "dlssnr/DlssNr_Exposure.h"` from that compile (`/I OptiScaler`). Expected: exit 0.

Named asserts:

- `E` in `{NaN, +Inf, 0, 1e7}` → treated as `1` (same gate as the shader)
- `P = 0` or `S = 0` → folded to `1` before `GameWhite`
- `GameWhite(0.012, 1, 1) == 1 / 0.012` within `1e-4`
- commented invert `e * s / p` is present and not compiled in

**T0.** GTA V Enhanced, or any other title that publishes `ExposureTexture`. Required before
done. Strength > 0, wipe on, debug off. Box off → note `W_abs`. Box on, `1.00×` → same look,
not the inverse.

Default matrix: Dx12, `UseProxy` off, Output Scaling off, Compare off unless a row says
otherwise.

- [ ] **T0** as above.
- [ ] Cyberpunk (or any no-texture HDR title): box disabled after first encode, picture
      matches the build before this change at slider `1.00`. No first-usable / Mode 3 log.
- [ ] A publishing title: box enables, default on, slider shows `×`, readout gets a finite
      `E` after the fence, not a fake `0`. Camera / exposure move: edited picture tracks this
      frame, not a 3-frame lag.
- [ ] Untick / tick / drag: §10.4. Strength 0 still matches `hdrCopy`.
- [ ] DX11 w/Dx12 **and** Vulkan w/Dx12, AE **on**, texture present: box can enable; no
      device removal; upscaler behaviour unchanged. AE **on**, no texture: box stays off, no
      backend change.
- [ ] Native Vulkan: box disabled, native-Vulkan status, picture = slider only, validation
      layers clean (binding 8 dummy).
- [ ] Passthrough / 8-bit output: box disabled, tone-map status.
- [ ] Menu before the first encode (create-hold): waiting line, no crash, no numeric `0`.
- [ ] `WhitePointFromExposure=true` in INI on a no-texture game: box stays off, slider
      absolute, preference still `true` after save.
- [ ] First-unusable log fires if a non-null pointer fails `ExposureUsable` (implementer
      confirms from the log when such a pointer appears; no title is required).
- [ ] Native DX12, hotfix unset: no exposure barrier, no device removal. Hotfix set to any
      integer (`[Hotfix] ExposureResourceBarrier`): transition pair around the §7.4 span only.
- [ ] Wipe hairline stays visible when `Effective` and `GameW` is large (`PaperWhite()` after
      `* normScale`, not `1.0`).
- [ ] Debug views 1–3 do not brighten/darken when ticking the box at slider `1.00`.
- [ ] Park / resolution rebuild: box stays enabled if it was; readout may show `—` then
      recover; a subsequent hole still has held `E` if one had landed.
- [ ] Matched residual at 67%: \(P\) still from t3 `colorCopy`, not t4. Empty-model gate
      unchanged.
- [ ] `UseProxy` on, publishing title: readout still gets an `E` (courier ran).
- [ ] Host fixture exit 0.

---

## 13. Rebuild both blobs

From `OptiScaler/shaders/dlssnr/precompile`. Do **not** run `build_precompiled_shader.bat`,
`build_precompiled_shader_fxc.bat`, or `build_precompiled_shader_vk.bat`.

```
..\..\shader_tools\fxc.exe -T cs_5_0 -E CSMain -O3 dlssnr.hlsl -Fo DlssNr_Shader.cso
python ..\..\shader_tools\create_header.py DlssNr_Shader.cso DlssNr_Shader.h DlssNr_cso

..\..\shader_tools\dxc.exe -spirv -T cs_6_0 -E CSMain -O3 -Qstrip_debug -D VK_MODE -Cc -Vi dlssnr.hlsl -Fo DlssNr_Shader_Vk.spv
python ..\..\shader_tools\create_header.py DlssNr_Shader_Vk.spv DlssNr_Shader_Vk.h dlssnr_spv
```

Commit `dlssnr.hlsl` and both headers + `.cso` + `.spv` with the HLSL. Do not add a `_DX11`
header.

---

## 14. Implementation order (plan input)

1. **Host helper + fixture.** `GameWhite()` and `exposure_fixture.cpp`. No GPU.
2. **`NrExposure` + `EvaluateAfterUpscale` fill.** No bind yet.
3. **Usable check, Dx12 dummy, `InExposure` rename, `DispatchPass` 5th→dummy, latches,
   hotfix span.** Four after-lines in §7.6. No throw on a bad pointer.
4. **Bridges (§8.6).** Peek / optional copy / `ExpCopy` when AE on. NR Set after upscaler
   Evaluate. AE-on + missing must not flip backend. Block at NR is DX12 or null.
5. **HLSL `PaperWhite()`, t4 binding 8, sites in §7.2, Mode 3 `gExposure.Load`, both blobs
   (§13).** Vulkan layout + pool; six-view `Dispatch`; native dummy t4.
6. **Delete `ResolveWhitePoint`.** Mode 3 after encode if usable (move the `target` UAV↔SRV
   pair). Hole row uses `slider × GameWhite(heldE, thisP, thisS)`.
7. **Menu + atomics.** Colour block per §5. Delete `GameExposureStatus`. No `g_nrMutex` on
   the overlay.
8. **T0, then the rest of §12.** Invert only the helper if needed.

---

## 15. What a plan must not reopen

- Three predicates plus `IsRunningVk()`. Box interactable iff `CanEnable`; drawn unchecked
  via a `false` temporary.
- Slider format and help follow `Effective`. Range `0.25…240` log. INI keys unchanged.
  Format `×` is Unicode.
- Picture samples this frame on D3D12. Readback never writes `WhitePoint` except the hole
  formula `slider × GameWhite(heldE, thisP, thisS)`.
- `1.0` is identity. Toggle does not rewrite or rescale the float.
- Default preference `true` does not move a no-texture HDR title.
- Exposure is t4 / Vulkan binding 8. Resolve t3 stays `colorCopy` / `g_vk.proxy`.
- `DispatchPass` 5th null is the Dx12 dummy, never `InSource`. Rename `InPrevEdit` in place.
- Mode 3 after encode, before downsample / UseProxy, whenever **usable**, even if
  `!Effective`.
- Six-view Vulkan `Dispatch`. Binding 8 = existing dummy. No game `ImageView`. No
  `CopyTextureRegion` of the game resource.
- No `DLSSD.Exposure`. No `HdrLift`. No Multi-pass fill. No INI rename.
- No `build_precompiled_shader*.bat`. Both blobs, §13 commands.
- Debug views stay on `gDebugScale`. Hairline is `PaperWhite()` after `* normScale`.
- Encode 466 and resolve 526 as the exact replacements in §7.2. Passthrough ternary stays.
- Encode and resolve constants: `resolveParams = encodeParams`, then overwrite Mode /
  strengths / compare / debug. Delete `ResolveWhitePoint`.
- Usable is non-MSAA non-array 2D only.
- NR Set is after the upscaler Evaluate. Upscaler AE behaviour is unchanged.
- No `0.01…4096` clamp. `max(W, 1e-4)` only.
- Delete `GameExposureStatus`. Reject today’s orange-but-safe-to-leave-on box.
- Do not change Mode 2, residual math, default `Transfer`, or Capture.
- This file supersedes the matched-residual Mode 3 t3 row only.
