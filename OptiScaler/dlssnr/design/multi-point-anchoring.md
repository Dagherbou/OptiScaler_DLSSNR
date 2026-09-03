# Multi-point anchoring for the scanned-exposure white point

Status: BUILT (2026-09-03), compiles clean, not yet in-game tested. Written before code, per the
project's design-doc-first rule. D3D12 only for now (the scan is D3D12 only).

## The problem this solves

When the white point is driven by a buffer the scan found (`WhitePointSource == 2`), it is a
**one-point calibration**:

    whitePoint = anchorWhitePoint * (anchorValue / scanNow) * trim

`anchorValue` is the scan's value at the instant the user pressed Anchor, `anchorWhitePoint` the paper
white they had set there. This is exact only if the scanned buffer is *linearly proportional* to true
exposure. Real buffers rarely are -- they are log-encoded, or a proxy, or (GTA V) an accumulator only
loosely correlated with exposure. So the white point is correct near the anchor and **drifts the
further the lighting moves from it**. Observed directly: a GTA V clip that tracks well in the open lot
and loses it in a dark garage, needing a manual re-anchor.

A self-re-anchor toggle cannot fix this and must not be built:
- Continuously re-anchoring sets `anchorValue = scanNow` every frame, so the ratio is always 1 and the
  white point freezes -- the scan becomes inert.
- Periodically re-anchoring makes the white point track, then snap to hold whatever value (and
  whatever drift) it had at the snap, then track again -- visible jumps, and it cements error.
- There is no ground truth to re-anchor *against*. That absence is the whole reason we scan.

## The fix

Let the user place **several** anchor points at different lighting conditions instead of one, and
**interpolate** the white point across them by the scan value. Two points fit a line through the
buffer's actual (scanValue -> whitePoint) relationship; more points fit a piecewise-linear curve. The
white point is then correct across the whole range between the outermost points, and reasonable
(clamped extrapolation) beyond them.

This is the correct form of both the earlier "two white points" idea and the "auto re-anchor" wish:
the user calibrates a few points once per game, and it holds everywhere.

### The mapping

Given anchor points sorted ascending by scan value, `(v_0, w_0) .. (v_n, w_n)`, and the current scan
`s`:

- `n == 0` (no anchors): the scan source is not driving; fall back to the current single-anchor
  behaviour or the manual slider, unchanged. Multi-point never *removes* the existing path.
- `n == 1`: exactly today's ratio, `w_0 * (v_0 / s)`. One point is the current feature, so a single
  anchor behaves identically to now.
- `s <= v_0`: clamp to `w_0` (do not extrapolate a ratio past the darkest calibrated point -- a
  runaway white point in an unseen darker scene is worse than holding the last good one). Same at the
  bright end with `w_n`.
- `v_k <= s <= v_{k+1}`: linear interpolation **in the space that is linear for a white point**, which
  is log. A white point is a divisor; halving light should halve it, so interpolate `log(w)` against
  `log(v)`:

      t = (log s - log v_k) / (log v_{k+1} - log v_k)
      w = exp( lerp(log w_k, log w_{k+1}, t) )

  Linear-in-log means two points that happen to be a true exposure and its correct white points
  reproduce the exact ratio law between them, so the two-point case degrades gracefully to the
  one-point case and there is no discontinuity when a second point is added.
- `trim` multiplies the result, as now.
- `inverted` (the "runs the other way" flag) still applies: it flips whether w rises or falls with s,
  by sorting/'interpreting the pairs accordingly. Simplest: store points as (scanValue, whitePoint)
  and let the interpolation handle any monotonic direction, since sorting by scanValue plus lerp works
  whether whitePoint rises or falls across the points. The inverted flag then only affects the live
  ratio preview, not the table math.

### Degeneracies to guard (these are the sloppiness traps)

- Two points at the (nearly) same scan value: `log v_{k+1} - log v_k` near zero -> divide by zero.
  Reject a new anchor whose scan value is within a small factor (say 2%) of an existing point's, or
  replace that point rather than adding a second. Report it in the row's tooltip.
- A point with a non-finite or out-of-range scan value (`<= kFloor` or `>= kCeiling`) must never enter
  the table -- the Anchor button is disabled unless the live scan value is in range, same gate as
  today.
- Empty table with source == 2: behave exactly as "scan found nothing" does now.

## The UI (the user's design)

Under Colour, when `WhitePointSource == 2`:

- The **paper-white / trim slider stays**, and it controls whichever point is *selected* -- or the
  live, unanchored white point when nothing is selected.
- **Anchor here** captures `(currentScanValue, currentWhitePoint)` and **adds a row** to a dynamic
  table. It does not replace; each press is a new calibration point (subject to the near-duplicate
  guard above).
- The **table** lists each anchored point: its scan value and its white point, sorted by scan value,
  with a small "×" to delete the row. The row for the point nearest the current scan value is
  highlighted so the user can see which one is active right now.
- **Clicking a row selects it**: the slider now edits that point's white point (live, so the user sees
  the effect if the current lighting is near it). **Clicking it again deselects**, returning the slider
  to the live unanchored point.
- A one-line readout shows the interpolated result: `scan 0.043 -> white 5.9 (between point 2 and 3)`.

State model:
- `selectedAnchor`: index into the table, or -1 for "editing the live point". Menu-only; not persisted.
- The slider writes to `table[selectedAnchor].whitePoint` when a row is selected, else to the live
  `DlssNrWhitePointScale` (the current manual value) which is what the *next* Anchor will capture.

## Config / persistence

The anchor table must survive a restart (the whole promise is "set once per game"). Store it as a
small serialised list under `[DlssNr]`, e.g. `ScanAnchors=v0:w0;v1:w1;...` -- one key, parsed on load,
written on save, bounded to a small max (8 points is plenty). This replaces the single
`ScanAnchorValue`/`ScanAnchorWhitePoint` pair; migrate an existing single anchor into a one-row table
on first load so nobody loses their calibration.

Round-trip (per the config guard): the new key is read in `Config.cpp`, written in the save path, and
documented in the shipped `OptiScaler.ini`. The config-round-trip check should pass with it.

## What this deliberately does NOT do

- No auto-re-anchor. See above; it cannot work.
- No change to the scan's detection or readback. Multi-point corrects a *systematic* curve in the
  (scanValue -> whitePoint) relationship; it does not remove per-frame *noise* in the scanned value.

  Correction of an earlier overstatement: the GTA V candidate was called a monotonic "accumulator"
  whose value is not exposure. The logged data does not support that. Its value OSCILLATES up and
  down (2.38..3.03 in a static scene), it has spanned 0.081..6.19 across the session moving with the
  lighting, and its ratio to the real exposure texture holds at ~16 with a +/-13% wobble. That is a
  real, correlated exposure-like signal at a different scale -- exactly what the anchor's ratio
  cancels -- not a broken accumulator. So multi-point is expected to work here; what is left after it
  is the +/-13% static-scene noise, a smaller and separate problem (a short temporal smoothing of the
  scan value, not a calibration change).
- No Vulkan support yet -- the scan is D3D12 only until the Vulkan-scan pass lands, after which this UI
  and math are shared unchanged.

## Test plan (before it ships)

1. One anchor: white point identical to today's single-anchor path, frame for frame.
2. Two anchors bright+dark: white point matches each at its own lighting and interpolates smoothly
   between, with no jump as lighting crosses a calibrated point.
3. Below the darkest / above the brightest anchor: white point clamps, does not run away.
4. Near-duplicate anchor rejected; delete a row; delete all rows -> falls back to "nothing anchored".
5. Restart: the table persists; a pre-existing single anchor migrated to one row.
6. Source != 2: the table UI is hidden and nothing in this feature runs.
