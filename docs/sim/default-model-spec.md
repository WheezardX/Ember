# Default fire model `ember-ca` v1 — specification (Epic 4 D1)

**Status:** document of record for story 4.2. Every integer format, iteration order, and
random draw the implementation makes is defined here; `sim/src/model_ca.cpp` is the
reference implementation and must not contain behaviour this document does not describe.
Coefficients live in the params pack `sim/packs/ca_params.v1.toml` (Epic 7 shape); the spec
names each parameter, the pack gives it a value.

Design intent (plan D4, design doc 4.2): an **honest cartoon** of Rothermel-shaped behaviour
— steeper is faster, wind stretches the ellipse, grass runs and timber smolders — tuned for
**legibility** (CP4), sanity-checked against a real fire (CP5), never validated. It is a
tick-based cellular automaton because a CA accepts mid-run world deltas naturally, which is
the point (suppression).

## 0. Notation and fixed-point conventions
- Grid: `nx × ny`, row-major, index `i = y*nx + x`, `x` east, **`y` south** (raster rows).
- Time: `t` integer seconds since `t0`; tick length `dt_s` (default 60, `max_dt_s = 600`).
- `Q8` = integer × 256 represents a real (256 = 1.0). `Q16` = × 65536. `>>` is an arithmetic
  shift on non-negative values only; every quantity below is non-negative unless stated.
- Lengths in **millimetres** (u32), speeds in **mm/s** (i32), wind in **cm/s** (i32 components),
  moisture in **tenths of a percent** (`m10`), temperature in **0.1 K**, RH in **0.1 %**.
- `isqrt(n)` = floor(sqrt(n)) on u64, by integer Newton iteration.
- `hash64(...)` and `splitmix64` are Terrain's (`terrain/veg/hashing.py`), ported bit-exactly
  to `sim/src/hash.h`. A draw `D(system, entity, tick, k)` = `hash64(run_seed, system, entity,
  tick, k)` → u64. Systems: `SYS_SPOT_LAUNCH = 3`, `SYS_SPOT_TRANSPORT = 4`,
  `SYS_SPOT_IGNITE = 5` (1–2 reserved for the runner/suppression sim).

## 1. Neighbourhood
Sixteen directions in this fixed order (dx, dy; dy positive = south):

| k | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| dx | 1 | 1 | 0 | -1 | -1 | -1 | 0 | 1 | 2 | 1 | -1 | -2 | -2 | -1 | 1 | 2 |
| dy | 0 | -1 | -1 | -1 | 0 | 1 | 1 | 1 | -1 | -2 | -2 | -1 | 1 | 2 | 2 | 1 |

Distances `d_k` (mm) = `round(cell_mm × {1, √2, √5})` per ring, computed once at init from
`cell_size_m` (e.g. 30 000, 42 426, 67 082). Unit vectors `u_k` (Q16 pair) are compile-time
constants (`(65536,0)`, `(46341,-46341)`, …, knight moves `(58617, -29309)` etc. =
`round(65536·dx/√5)`).

## 2. Cell state
Dense arrays (all cells):
- `phase` u8: 0 unburnable, 1 unburned, 2 burning, 3 burned.
- `intensity` u8: 0 none, 1 low, 2 moderate, 3 high/crown. Set once at ignition.
- `arrival_s` i32: −1 until ignition; set once.
- `fuel` u8: FBFM40 code (0 = none). `FuelRemoved` sets `fuel = 0` and `phase = 0`; the
  original code is kept in `fuel_orig` for rendering.
- `retardant` u16 permille coverage; `bump_m10` u16 moisture bump; `bump_until_s` i32.
- `crown` u8 flag (0/1), decided at ignition.

Sparse **active table** for burning cells: `progress[16]` u32 mm per direction. Rows are
allocated on ignition and released on burnout; iteration over burning cells is always in
ascending cell index (the active list is sorted at the start of each tick).

Init: `phase = 1` where `fuel` is burnable (class ≠ NB), else `0`; DEM nodata cells are
unburnable. Elevation gradient per cell (`gx, gy` in Q8 rise/run, central differences, edges
one-sided, nodata neighbours treated as the cell's own elevation) is precomputed once.

## 3. Per-tick algorithm (`advance(dt_s, deltas)`)
Order is normative:
1. **Apply deltas** in the given order (§7).
2. **Sample weather** at `t` (ADR 0009 rule): per weather-grid cell `u, v` cm/s, `t2`, `rh2`,
   `precip`. Derive per weather cell: dead fuel moisture `m10` (§4.3) and, at a step boundary,
   apply `precip` (§4.3, §7).
3. **Land pending spots** with `land_s ≤ t`, in ascending `(land_s, src, draw)` order (§6).
4. **Decay** retardant and expire moisture bumps (§7).
5. **Spread** (§4–§5): for each burning cell `i` ascending; for `k = 0..15`: target
   `j = i + dx_k + nx·dy_k` (skip if off-grid); skip unless `phase[j] == 1`; compute `R_k`
   (mm/s); `p = progress[i][k]`; `progress[i][k] = p + R_k·dt_s`; if the new value `≥ d_k`,
   propose ignition of `j` at `a = t + (d_k − p) / R_k` (integer division; `R_k > 0` is
   guaranteed when `d_k` is reached). Proposals are collected; after the loop each proposed
   cell ignites with its **minimum** `a` (ties keep the lowest source index — guaranteed by
   ascending iteration and strict `<` updates). Newly ignited cells do not spread this tick.
6. **Spot launches** from burning cells, ascending index (§6).
7. **Burnout**: burning cells with `t + dt_s − arrival_s ≥ residence_s(fuel)` become
   `phase = 3`; their active rows are released.
8. Emit `dirty` = sorted unique indices touched in steps 1–7; `spots`; `diag`
   (`active_cells`, `ignitions`, `spot_launches`, `weather_step`, `held_steps`).

Ignition of cell `j` at time `a` (used by steps 3, 5, and `IgnitionForced`): `phase = 2`,
`arrival_s = a`, allocate active row with all `progress = 0`, decide `crown` and `intensity`
(§5.4) from the conditions **at the ignition tick**.

## 4. Spread rate `R_k`
```
R_k = R0(fuel)                       mm/s, params
      ×Q8 f_moist(fuel, m10_eff)     §4.3
      ×Q8 f_green(fuel, greenness)   §4.4
      ×Q8 f_ret(retardant)           §7
      ×Q8 f_crown                    §5.4 (crown_mult_q8 if crown else 256)
      = R_base
R_head = R_base × (25600 + k_wind_q8(fuel) × |V|_cms) / 25600, capped at R_base × head_cap_q8/256
R_k    = R_head × r_k(θ_k) >> 16                                  §4.2
```
Each `×Q8` is `(a × f) >> 8` applied left to right in the order listed (fixed so hashes are
reproducible).

### 4.1 Effective wind–slope vector `V` (cm/s, grid axes)
- Mid-flame wind from the 10 m field: `W = (u, −v) × wind_reduction_q8(fuel) >> 8` — note
  the sign flip: `v` is northward, grid `y` is southward.
- Slope-equivalent wind, pointing **uphill**: `S = (−gx, −gy) × slope_equiv_cms_per_q8unit`
  (`gx, gy` Q8 rise/run; params value is cm/s per Q8 unit of tan-slope, so at 30 % slope with
  the default 700 the magnitude is `0.3·256·700/256 ≈ 210` cm/s). Magnitude is capped at
  `slope_equiv_cap_cms`.
- `V = W + S`; `|V| = isqrt(Vx² + Vy²)`.

### 4.2 Elliptical shaping
- Length-to-breadth `LB_q8 = 256 + (lb_per_ms_q8 × |V|) / 100`, capped at `lb_max_q8`.
- Eccentricity `e_q16 = isqrt((65536 − (2^32 / LB_q8²)) × 65536)` (0 when `LB = 1`).
- If `|V| == 0`: `r_k = 65536` for all k (a circle). Otherwise
  `cosθ_k = (u_k · V) / |V|` in Q16 (signed, −65536..65536) and
  `r_k = (65536 − e) × 65536 / (65536 − (e × cosθ_k >> 16))` — the focus-form ellipse: `1` at the
  head, `(1−e)/(1+e)` at the back. Denominator is ≥ 1 by construction.

### 4.3 Dead fuel moisture and `f_moist`
From the weather cell's `rh2` (0.1 %) and `t2` (0.1 K), 1-h moisture in tenths of a percent:
```
rh = rh2 / 10 (integer percent)
m10 = 10 + 2·rh                       rh ≤ 60      (1 % + RH/5)
m10 = 130 + (rh − 60) × 10 / 3        rh > 60      (13 % … 26 %)
m10 += tadj:  t2 < 2831 (10 °C): +20;  t2 > 3031 (30 °C): −10;  else 0
m10_eff = clamp(m10 + bump_m10 + wet_m10, 0, 400)
```
`wet_m10` is a per-weather-cell rain term: at each weather step start, `wet_m10 += precip_0.01mm
× rain_m10_per_mm / 100`, then each tick `wet_m10 = max(0, wet_m10 − dry_m10_per_hour × dt_s /
3600)`. (Shape from the Fireline Handbook fine-dead-fuel-moisture table, coarsened; a legible
cartoon, not NFDRS.)

`f_moist_q8 = 0` if `m10_eff ≥ mx10(fuel)` (moisture of extinction), else
`f_moist_q8 = ((mx10 − m10_eff) × 256 / mx10)²  >> 8`, then `max(f_moist_q8, f_moist_floor_q8)`
only when `m10_eff < mx10` (so extinction still stops spread).

### 4.4 Greenness `f_green`
For herbaceous-dominated classes (`herbaceous = true` in the pack: GR, GS, and SH with a
live component): `f_green_q8 = 256 − ((256 − green_min_q8) × greenness) / 255`; others 256.
When the world pack has no greenness layer, `greenness = default_greenness` (params).

## 5. Fuel classes, intensity, crowning
### 5.1 Classes
FBFM40 code → class: 91–99 `NB` (non-burnable; 91 urban, 92 snow, 93 agriculture, 98 water,
99 barren), 101–109 `GR`, 121–124 `GS`, 141–149 `SH`, 161–165 `TU`, 181–189 `TL`,
201–204 `SB`. Unknown codes are `NB` and counted in `diag.unknown_fuel_cells`. Every
parameter is defined per class in `[class.X]` and may be overridden per code in `[fuel.NNN]`.

### 5.2 Residence
`residence_s(fuel)` — burning → burned after this long (grass short, timber/slash long).

### 5.3 Intensity class
At ignition, using `R_head` of the **igniting** cell (for forced ignitions and spots: the
cell's own `R_base`): class 1 if `R_head < intensity_t1_mms`, 2 if `< intensity_t2_mms`,
else 3. Crowning forces 3.

### 5.4 Crowning (approximation)
At ignition, `crown = 1` iff all hold: class ∈ {TU, TL} (timber), `cbh_dm ≤ crown_cbh_max_dm`,
`cbd_gm3 ≥ crown_cbd_min_gm3`, `cc_pct ≥ crown_cc_min_pct`, surface intensity class ≥ 2,
`|V| ≥ crown_wind_min_cms`. Effects: `f_crown = crown_mult_q8`, `intensity = 3`, the cell is
a spot **source**. Toggle `crowning.enabled`.

## 6. Spotting (stochastic; toggles `spotting.enabled`, `spotting.deterministic_test_mode`)
For each burning cell `i` (ascending) with `crown == 1` **or** (`intensity == 3` and class ∈
{SH, SB, TU}), and `|W| ≥ spot_wind_min_cms`:
1. **Launch draw** `h = D(SYS_SPOT_LAUNCH, i, tick, 0)`; launch iff
   `h % 1_000_000 < spot_rate_ppm_per_s × dt_s` (capped at 999 999).
2. **Transport** (only if launched): distance `d_m = spot_dist_min_m + h1 % (spot_dist_max_m −
   spot_dist_min_m + 1)` with `h1 = D(SYS_SPOT_TRANSPORT, i, tick, 1)`, then scaled by wind:
   `d_m = d_m × |W| / spot_wind_ref_cms` (integer), clamped to `[cell_m, spot_dist_max_m ×
   spot_dist_wind_cap]`. Bearing = downwind direction of `W` (integer degrees via `iatan2`,
   a deterministic integer atan2 with 1° resolution) plus jitter `(h2 % (2·spot_jitter_deg +
   1)) − spot_jitter_deg`, `h2 = D(SYS_SPOT_TRANSPORT, i, tick, 2)`. Target cell
   `(x + round(d_m × cos_q14[deg] / cell_m / 16384), y + round(d_m × sin_q14[deg] /
   cell_m / 16384))` using the constant 360-entry Q14 sine table. Off-grid → recorded as
   `landed = 0`.
3. **Landing time** `land_s = t + d_m × 1000 / spot_flight_mms` (≥ `t + dt_s` so a spot never
   ignites in its launch tick).
4. **Ignition draw** on landing (step 3 of the tick order at the landing tick): target must be
   `phase == 1`; `h3 = D(SYS_SPOT_IGNITE, i, launch_tick, 3)`; ignites iff `h3 % 1000 <
   spot_ignite_permille(fuel_j) × f_moist_q8(j) / 256`. Ignition at `a = land_s` with
   `cause = spot`.
5. Every launch is recorded as a `SpotEvent{src, dst, launch_s, land_s, landed, ignited,
   stream_key = h}`.

`deterministic_test_mode`: every launch draw succeeds and every ignition draw succeeds
(transport draws unchanged) — for tests that need a spot across a barrier.

## 7. Delta application semantics
Applied in the order received; each cell in a delta is processed in the order listed:
- `FuelRemoved{cells}`: `fuel = 0`, `phase = 0` **unless** `phase ∈ {2, 3}` (a burning/burned
  cell keeps its phase: line cut through the black stays black; recorded in
  `diag.fuel_removed_on_black`). Active row, if any, is released (a burning cell that loses its
  fuel stops spreading — mineral soil).
- `RetardantApplied{cells, load_permille, decay_class}`: `retardant = min(1000, retardant +
  load)`; `decay_class` chooses `retardant_decay_permille_per_hour[class]`. Decay per tick:
  `retardant −= decay × dt_s / 3600` and, at a weather step with precip,
  `retardant −= precip_0.01mm × retardant_wash_permille_per_mm / 100`. Effect
  `f_ret_q8 = 256 − (retardant × retardant_eff_q8) / 1000`.
- `MoistureBumped{cells, magnitude_m10, ttl_s}`: `bump_m10 = max(bump_m10, magnitude)`,
  `bump_until_s = max(bump_until_s, t + ttl_s)`; at `t ≥ bump_until_s` the bump is cleared.
- `IgnitionForced{cells, cause}`: for `phase == 1` cells, ignite at `a = t` (§3); other phases
  are ignored and counted in `diag.ignition_refused`.
- `ExtinguishForced{cells}`: `phase == 2` → `phase = 3` (cold), row released; others ignored.
Adversarial cases are pinned by tests: delta on a burning cell, overlapping deltas in one
tick (later wins for `FuelRemoved`/`Extinguish`, additive for retardant, max for bumps),
delta + spot on the same cell in one tick (deltas apply first, so a `FuelRemoved` cell cannot
be spot-ignited that tick).

## 8. Weather sampling (ADR 0009 amendments A–C)
Weather grid cell of sim cell `(x, y)` = `((x × cell_mm + cell_mm/2 + ox_mm) / wdx_mm, …)`
where `ox_mm` is the offset between the two grids' origins recorded in the world pack
(nearest, no bilinear). In time: step `s = (t − wt0_s) / wstep_s`, `frac_q16 = ((t − wt0_s) −
s·wstep_s) × 65536 / wstep_s`, value = `lerp_q16(a, b, frac_q16)` = `a + round_half_away((b − a) × frac_q16 / 65536)` between steps
`s` and `s+1` (last step held past the end; missing steps held from the previous valid step,
counted in `diag.held_steps`). Constant weather (`[weather] constant`) is a 1×1 grid with one
step.

## 9. Capability declaration
`accepts = all five`, `provides_intensity = true`, `provides_spotting = true`,
`deterministic = true`, `supports_rewind = false`, `max_dt_s = 600`, `uses_weather = true`,
`rng_streams = ["spot_launch", "spot_transport", "spot_ignite"]`.

## 10. Properties the tests pin (synthetic worlds, `sim/tests/`)
- Flat, uniform fuel, calm: burned set after `T` is a discrete disc; radius grows linearly
  with `T` at `R0 × f_moist × f_green` within one cell.
- Uniform wind: length-to-breadth of the burned set matches `LB` within 10 %; the head is
  downwind.
- Ramp: upslope arrival times precede downslope at equal distance; ratio increases with slope.
- Grass vs timber at equal conditions: grass front further; timber cells stay burning longer.
- Cured vs green grass: cured further.
- Barrier of `FuelRemoved` cells stops spread with spotting off; with
  `deterministic_test_mode` a spot crosses it.
- Same scenario twice: identical per-tick state hashes; perturbing one seed changes them.
