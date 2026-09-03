# Frame hold — freeze the NR input so settings can be A/B'd on one frame

Status: design, then implement (design-doc-first per DEVELOPMENT.md). D3D12 first; native Vulkan a
follow-up.

## The problem

Every comparison so far has been confounded because the scene moves between captures (the reversible
proxy shots were literally different times of day). You cannot tell a setting's effect from the
scene's when both change at once. Side-by-side/wipe compare only shows NR-on vs NR-off at the *current*
settings — it cannot compare two *settings*.

## The idea

A **Hold** toggle. On, it freezes the input to the Neural Rendering pass; while held, changing any of
our downstream settings (paper white, detail/colour strength, reversible mode, NR preset/style,
highlight guard, transfer) re-runs the model + composition on the SAME frozen frame, so the only thing
that moves is the setting. Hide the menu and it stays held (it is a config flag the pass reads). Untoggle
to resume. This is the base for later A/B tooling — style sweeps, slider sweeps, a two-version wipe.

## What freezes, and where

The pass reads the upscaler's colour output, encodes it to a proxy the model sees, keeps an untouched
copy, runs the model, and composes. The freeze point is the **raw colour the encode reads**, captured
once at hold-on into a persistent texture; while held the encode reads that copy instead of the live
output. Because both the proxy AND the untouched `keep` derive from the encode's source, freezing there
keeps them consistent (freezing the proxy alone would be wrong — settings must still re-encode).

Also frozen at hold-on:
- **The white point.** Snapshot the resolved white point and use it while held. This is the key
  constraint the user identified: the scan/meter/game-exposure MEASURES the white point live every
  frame, so if it keeps running while held it drifts and changes the held picture for a reason other
  than the setting under test. So in hold mode the snapshot is used for rendering. (In v1 the meter still dispatches; its value is
simply ignored while held, so the picture is stable even though the menu's live exposure readout may
keep moving. Skipping the dispatch is a later refinement -- the rendered result is already correct.)
- **Model history** (v2, not in v1). Ideally reset each held frame so the frozen input is judged
  identically every frame. v1 does NOT reset: the input is identical each frame so the model converges
  to a steady picture, and a setting change morphs to the new look over ~3-5 frames rather than
  snapping. Fine for A/B; snapping is a later refinement.

## What is impossible / does not work in hold (be honest about the limits)

- **Live white-point measurement.** The rendered white point holds the snapshot; the meter keeps
  running but its value is ignored, so the menu's live exposure/scan readout may still move while the
  picture does not.
- **DLSS SR / upscaler presets, FSR/XeSS choice, and anything UPSTREAM of this pass.** We hold the
  upscaler's *output*; the upscaler is not re-run, so changing its preset changes nothing on a held
  frame. Re-running a temporal upscaler on one frozen frame degenerates (no history/motion), so it is
  out of scope. Hold-frame is for DOWNSTREAM (our) settings.
- **The game's own post-process, tonemapper, and UI/HUD.** They run after this pass on the live
  present, so the HUD and any game post-effect keep updating over the frozen scene. Only the
  NR-composited scene content holds.
- **Temporal behaviour (ghosting, accumulation).** Guides are effectively static, so anything that
  only shows in motion cannot be evaluated held.

## Menu (the Compare category)

There is no "Compare" section today — the compare controls live under "Inspect". Create a **Compare**
`SeparatorText` and MOVE the existing compare controls into it (Compare mode, split, zoom, swap, tags),
then add the **Hold frame** toggle at its top. Debug view moves under Compare with the on-screen
compare tools; "Inspect" keeps the frame-capture tool. This
category is where future A/B tooling accretes.

## Config / persistence

`DlssNrHoldFrame` (bool, default false). Not really a persisted preference — it is a live testing
toggle — but it goes through the normal round-trip for consistency. It also wants a keybind (like NR
enable) so it can be held/released without opening the menu; wire it to the existing keybind system in
a follow-up if the flag alone is not enough.

## Guards (per DEVELOPMENT.md)

- Default off ⇒ byte-identical: the held texture is not allocated and the encode reads the live output
  exactly as now.
- The held texture is allocated on the transition to held and released/parked on hold-off; it is
  rebuilt if the output's size/format changes while held (same rule as the guide clones).
- Inert when NR is off.
- Passthrough unaffected (the freeze is on the encode's source, ahead of the passthrough branch).

## Scope

- v1 (shipped): D3D12 path — freeze colour + white point; the Compare category + moved settings + the
  Hold toggle. Adversarially reviewed (pass barriers/lifetime + menu) before deploy.
- v2 (follow-ups): reset model history each held frame (snap instead of morph); freeze depth + motion
  guides too (fully clean, camera-independent); native Vulkan hold; skip the meter dispatch while held;
  a keybind; a two-version wipe (hold A, change setting, wipe against the held A).
