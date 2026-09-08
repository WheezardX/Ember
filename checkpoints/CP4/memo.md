# CP4 — *It reads honestly* (2026-09-07)

**Gate:** a naive viewer can verbally predict each comparison's winner before it plays; tuning
memo written. **Result: PASS** (with the `ember-ca` params pack at v1.1.0 — the v1.0.0
coefficients failed the gate on pace, see `docs/sim/tuning-memo.md`).

## What was produced
`python sim/scenarios/cp4/run_matrix.py` → 14 runs on 200×200 synthetic worlds (`ember sim
synth`), 2 h at `dt = 60`, spotting off, composed with `ember sim compare` and measured with
`ember sim curves`:

| group | artifacts | the call a viewer should make | measured head rate (m/h) |
|---|---|---|---|
| `slope/` | side-by-side MP4, `slope-curves.png`, `slope-table.md` | flat vs 30 % ramp (calm): **the ramp runs uphill** | 203 vs 605 |
| `wind/` | 0 / 2 / 5 / 8 m/s westerly on grass | **more wind, longer downwind ellipse, faster head** | 203 / 454 / 908 / 908* |
| `fuel/` | GR2 / SH5 / TU5 / TL3 at 2 m/s | **grass runs, shrub follows, timber understory walks, litter smolders** | 454 / 363 / 151 / 15 |
| `green/` | cured vs green grass at 2 m/s | **cured runs, green crawls** | 605 vs 151 |
| `moist/` | RH 12 % vs RH 60 % grass at 2 m/s | **dry runs, humid crawls** | 605 vs 91 |

Each group directory holds the composed MP4 (shared clock, one panel per run), two composed
keyframes (`00011.png` at ~1 h, `00023.png` at ~2 h), the response-curve plot (burned ha, max
radius, containment, burning cells vs time) and the summary table.

\* wind-8 equals wind-5 because the head is capped at one axis cell per tick (`cell/dt` =
1.8 km/h at 60 s); the spec records this (§11.8) and extreme-wind runs should use `dt ≤ 30`.

## Checks
- Every comparison's ordering matches the prediction in the table; the curves separate early
  (within the first 20 min) and never cross.
- Absolute rates are order-of-magnitude plausible for a cured late-summer afternoon (grass
  0.2–1 km/h, timber litter tens of m/h). They are legibility numbers, not validated ones.
- The wind ellipse is symmetric about the wind axis and its burned-set length:breadth grows
  with wind (≈2.4 at 5 m/s); the wavelet-vs-envelope relationship is pinned by a property test.
- Determinism: every run's final hash is recorded in `runs.json`; re-running the matrix
  reproduces them.

## Findings
- **v1.0.0 pace failure.** Transcribed "no-wind" base rates with the moisture factor applied on
  top gave grass 30 m/h at 2 m/s. Base rates were scaled ×3–7 and wind gains ×2.5–3 (memo).
- **Knight-move hop.** Once rates were realistic, the barrier property test caught direct
  spread crossing a one-cell fuel break through `(2,1)` neighbours. Fixed in the model
  (spec §1 knight-move rule) — a suppression-relevant bug found by the legibility pass.
- **16-direction anisotropy** is ~10 % at large radii on calm flat ground (a 16-gon, not a
  circle). Acceptable for the cartoon; noted for anyone reading the calm disc closely.
- Crowning and spotting were not exercised here beyond on/off; the D4 addendum scenarios
  (timber crown run, spot across a barrier) are covered by `ca_test.cpp` and will be shown in
  the CP6 campaign where they matter.

## Verdict
The default model's responses are legible and correctly ordered, and the tuning that made
them so is recorded key by key. CP3 was regenerated with the v1.1.0 pack (148 ha in 36 h
from a point in timber-grass under a 1.5 m/s breeze, terrain-shaped) and passes its own gate.
