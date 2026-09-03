# DLSS-NR development rules, guards, and review methodology

Why this file exists: the fixes in this fork keep introducing new regressions, and the last one
(a "Trim (x the scan)" slider shown for the scan source with no anchor set) was caught by the user's
eye, not by any process. Some regression classes can be caught mechanically; others only by a written
spec plus a mandatory review. This file is both — the invariants that must hold, the state specs that
changes are reviewed against, and the per-change-type review each change must pass **before deploy**.

The rule above the rules: **a change is not done when it compiles. It is done when it has passed the
review for its change type below, against the specs here.**

---

## 1. Invariants (must always hold)

1. **Default-identical.** A new toggle/feature must leave the prior behaviour byte-identical when it is
   off / at its default. New shader constants are zero-initialised and gated; the off path takes the
   exact code it took before. (Verified for the reversible proxy; this is the bar for every toggle.)
2. **No dead control.** A control never appears when the thing it acts on does not exist. A trim with
   nothing to trim, an "edit point" with no points, a "clear" with nothing set — all forbidden. This
   is the rule the Trim-on-no-anchor regression broke.
3. **One quantity, one control.** Two sliders must not edit the same underlying value, and one slider
   must not silently change meaning without its label saying so.
4. **Config round-trip.** Every new `[DlssNr]` key is (a) declared with a default, (b) read in
   `Config.cpp`, (c) saved in `Config.cpp`, (d) present in the shipped `OptiScaler.ini`. Missing any of
   the four is a silent "setting does nothing" or "setting is lost on restart".
5. **Shared struct == cbuffer.** `DlssNrConstants` (C++) and the `Params` cbuffer (HLSL) are ONE
   ordered list of 4-byte scalars. Append only, to the end, in both, in the same order. A mismatch
   silently corrupts every constant past the divergence — the worst outcome in the tree.
6. **Shader rebuild is real.** After editing `dlssnr.hlsl`, recompile BOTH targets (dxc `cs_6_0` DXIL →
   `DlssNr_Shader.h` array `DlssNr_cso`; dxc `-spirv -D VK_MODE` → `DlssNr_Shader_Vk.h` array
   `dlssnr_spv`) and confirm the `.cso`/`.spv` mtime is newer than the source and the built DLL is
   newer than the regenerated header. A stale header ships old shader logic silently.
7. **Passthrough is sacred.** Any encode/decode/proxy change must leave already-tone-mapped
   (`gPassthrough != 0`) frames untouched. Both the encode AND every place that reproduces the encode
   (e.g. the matched-residual `fullProxy`) must carry the same passthrough gate. (This is exactly the
   defect the reversible-proxy review caught.)
8. **Vulkan lifetime.** Never free a Vulkan resource the GPU may still use: drain (`vkDeviceWaitIdle`)
   before a resize/teardown on a live device, and ABANDON (never `vkDestroy`/`.reset()`) handles that
   belong to a device that has gone away.
9. **Local until asked.** No push without the user asking for that ref. See `push-discipline` memory.

---

## 2. State specs (the reviewable artifacts)

### 2.1 Colour → White-point source menu (`DlssNr_Menu.cpp`)

`WhitePointSource`: 0 paper-white-only, 1 game exposure, 2 scan buffer. What each state SHOWS. A menu
change is reviewed by walking every row of this table and confirming the code matches it — no extra
control, no missing one.

| Source | Sub-state | Sliders shown | Buttons / other |
|--------|-----------|---------------|-----------------|
| 0 paper white | — | **Paper white** (absolute) | — |
| 1 game exposure | game supplies one | **Trim (x the game's exposure)** + Reset | green "exposure -> white point" readout |
| 1 game exposure | game supplies none | Trim (x the game's exposure) + Reset | amber "supplies no exposure -- paper white in use" |
| 2 scan | 0 anchors | **Paper white** only | "Anchor here" (greyed unless a scan value exists); **NO Trim** |
| 2 scan | ≥1 anchor, none selected | **Trim (x the scan)** + Reset only (Paper white hidden; Anchor captures the trimmed effective white) | Anchor here; table rows (× delete, `>` = active); readout |
| 2 scan | ≥1 anchor, row selected | **Paper white (editing point N)** + Trim + Reset | as above; selected row shows `[editing]` |
| 2 scan | exactly 1 anchor | + **"the number runs the other way"** checkbox | (checkbox hidden at 0 or ≥2 anchors) |

Invariant checks against this table: Trim only with ≥1 anchor (rule 2). Inverted checkbox only at
exactly 1 anchor (direction is in the data at ≥2). Paper white shows only to set the FIRST point or
to edit a selected row; in the steady state (≥1 anchor, none selected) it is hidden and Anchor captures
the trimmed effective white instead.

---

## 3. Per-change-type review (run BEFORE deploy)

Match the change to its type and run that review — as an adversarial pass (a subagent given the diff
and the relevant spec above, or the same walk done by hand). The reviewer's job is to try to break it,
then confirm each numbered item or report a defect. This is not optional for the types marked ★.

- **★ Menu / UI change.** Walk §2's state table row by row against the code. For every control ask:
  can this appear when the thing it acts on is absent? does any control change meaning without its
  label? are two controls editing one value? (The regression that prompted this file is a §2 row that
  the code and table disagreed on.)
- **★ Shader (`dlssnr.hlsl`) change.** Default-off byte-identical (rule 1); cbuffer↔struct layout
  (rule 5); passthrough gated everywhere the encode is reproduced (rule 7); numerical safety of any new
  curve (NaN/Inf/÷0, output range); both shaders rebuilt (rule 6).
- **★ Config key change.** Round-trip all four points (rule 4).
- **★ Vulkan feature/resource change.** Lifetime (rule 8): every free preceded by a drain or an abandon;
  resources rebuilt when render size / working scale / device changes.
- **Non-NR-path touch.** Confirm the change is inert when NR is off (no alloc, no hook effect) so
  FSR/XeSS/DLSS users who merely left NR on cannot regress.

---

## 4. Mechanical guards (cheap, run them)

These catch the classes that a script can catch, so review time goes to the classes it cannot:

- **Config round-trip grep.** For each `CustomOptional<...> DlssNrX`, assert a `readX("DlssNr","X")`
  and an `ini.SetValue("DlssNr","X", ...)` exist. (Catches rule 4.)
- **Retired-identifier grep.** A removed setting/flag name must not survive as a live reader.
- **cbuffer/struct field-count check.** Count scalars in `DlssNrConstants` vs the `Params` cbuffer;
  they must be equal and in the same order. (Catches rule 5.)
- **Shader freshness check.** Assert `.cso`/`.spv` and their headers are newer than `dlssnr.hlsl`, and
  the built DLL newer than the headers, in the deploy step. (Catches rule 6.)
- **Version stamp.** The menu title already shows the build id + timestamp; confirm it matches the
  commit being tested so a screenshot is unambiguous about which build it is.

Build these iteratively as small scripts; none is a substitute for §3's reviews, and §3 is not a
substitute for these.
