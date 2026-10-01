# Fire in the renderer — state player, scar, flames, smoke v0 (EPIC_5_PLAN D1, D2, D7; HCP3)

## State player (D1)

`AEmberFireActor` loads an Epic 4 replay (`<name>.replay.json` + its `.ess` state stream) through
worldcore's `emberworld::fire` reader (`worldcore/src/firestate.cpp`; tested against the Epic 4
Python reader: `worldcore/tests/fire_test.cpp`, fixtures `fire_small`, `fire_jolly`). For any sim
time it reconstructs the state (nearest keyframe + tick dirty lists) and writes one texture over
the replay's world grid:

| channel | meaning |
|---|---|
| R | burning: 85 × intensity class (1 surface .. 3 crown fire), 0 = not burning; a stream without classes is drawn as 3 |
| G | burned incl. burning (the scar so far): 64 + 63 × the class it burned at, 0 = not |
| B | sqrt(hours since arrival / 200): minute-scale at the front, ~8 days at 1 |
| A | local rate of spread, log scale: 0 at <= 15 m/h .. 1 at >= 300 m/h (HCP4 heading: the head draws brighter and whiter). Static per cell, from the arrival field (worldcore `spread_rate_mh`) |

A cell burns from its exact arrival time (arrival is written once in the stream), not from the
next tick record, so timelapses move continuously instead of stepping hour by hour. The
`fire.cells_*` facts and probes use the same rule as the pixels.

## Clocks (D7)

The fire player owns sim time; flame flicker, wind sway and smoke motion run on the render clock.
Captures set `t_s` (sim seconds since the replay's t0) with a frozen render clock (pixel-
deterministic goldens). Orbits sweep `t_from_s → t_to_s` across their frames with the render
clock at `frame / fps`, so a 35-day timelapse still has flames flickering at real speed. Without
`t_s` a replay scenario shows the final footprint.

## Materials

* `M_Terrain` (`assets/generators/m_terrain.py`): char (fresh near-black, greying to ash over
  ~3 days, noise-broken so 30 m cells do not read as squares); a bright leading edge that decays
  over the first hour after arrival; faint patchy smoulder and embers behind it.
* `M_Veg` (`m_veg.py`): sampled at each tree's pivot. Burned trees char (some crowns scorched
  brown); trees torch only as the front arrives (flame decays with e^-age/1.2 h).

## Smoke v0 (`AEmberSmokeActor`; D6 legibility-first, no fluid sim)

Sources are 300 m bins of the fire grid weighted by time since arrival (e^-age/1.5 h: smoke comes
off the front, the burned-out interior stops smoking). Each source emits a chain of camera-facing
puff cards along a bent-over plume:

* injection height `H = 90 √(strength of the 5×5-bin / 1.5 km cluster)`, 150-4000 m (neighbouring
  bins rise as one column); rise `z = H (1 - e^(-d / 0.8H))` over downwind distance `d`;
* drift with the scenario wind (`wind_from_deg`, `smoke_wind_ms`), plume length `6H`
  (1.5-18 km), radius growing with distance and height, puffs spaced at a quarter of the
  mid-plume radius so they always overlap, stretched 1-1.6× so no card reads as a disc;
* opacity `0.5 (1 - e^(-strength/25))` (weak sources are wisps), faded in over the first three
  base radii, thinned downwind, and faded when the camera is inside a puff; flame light on the
  base of burning sources.

Every puff is a pure function of (source bin, puff index, render clock), so stills are
deterministic and scrubbing is free. Cards are sorted back to front each layout (`M_Smoke`:
translucent, lit per pixel, depth-faded into terrain by 0.4 × radius; `T_SmokePuff` is a
generated 2×2 atlas of billows with packed normals). Budget: 20,000 puffs, strongest plumes first.
Facts: `smoke.{sources, plumes, puffs, max_top_m}`.

Not yet (HCP4): plume/HUD wind-vane agreement from a real wind field, intensity-driven column
heights from the sim, shadows from the plume, night/glow pass, Niagara embers.

### Smoke v1 + observed heat (2026-10-01, tuned against the Three Queens 2026 photos)

Smoke v1 (df1944b) changes the geometry to buoyant columns. Each column rises near-vertically at
speed `W0` to `H = 250 √cluster` (300-5000 m), then leans downwind. Puffs are smaller than the
column is wide, so billows read. A skylight term lights the shaded sides. Puffs are packed toward
the base (life fraction u^1.5, opacity compensated) and faded in by height, which removes the row
of grey discs at column bases. Three more pieces:

* **Smoulder.** Each burned cell keeps a weak 5-day tail, which gives low wisps.
* **Pall.** Recent smoke drives a valley smoke layer (the fog's second layer), a paler warm haze
  and a slightly dimmer sun.
* **Observed heat.** A perimeter playback only knows when a cell burned. The NIROPS night flights
  say where it was still hot, often for weeks.
  * Built by `python -m ember.incidents.ir_heat --obs <incident>/observations/ir_perimeters
    --replay <run>.replay.json`.
  * Output: one class grid per flight on the replay grid (0 none, 1 isolated, 2 scattered,
    3 intense), written as `<pack>.heat.json` + `.heat.bin` beside the world pack. Three Queens:
    31 flights, 11 MB.
  * `AEmberFireActor` loads it when present and shows the last flight at or before now, if it is
    no older than 72 h, fading `e^-age/48 h`. It is used on cells the playback has already burned.

  Observed heat drives two things:

  * **Interior columns.** Each 300 m bin sums intense heat (0.5 per cell) and scattered heat
    (0.06 per cell). It then draws an activity per flight (u^4), so a few pockets carry most of the
    smoke. That gives several distinct columns, not a uniform field. A day cycle peaks at 16:00
    solar time and falls to 0.25 at night. Columns are `90 √cluster`, 200-1200 m, and narrower
    than a front's. They also light their own base at night.
  * **Glow points.** A sparse per-flight draw (intense 35 %, scattered 6 %, isolated points all)
    becomes live fire in FireTex: a fresh age, class 2 for intense. On a dark slope that reads as
    the discrete points and short lines of the night photos. By day the draw thins to 15 %, since
    pockets are lost against sunlit ground.

Facts: `fire.{observed_heat, heat_flight_utc, heat_age_h, heat_isolated, heat_scattered,
heat_intense}`.

Still open: a night sky (stars, deep blue; the atmosphere goes black), brighter night fire points,
a pall lit by the fire at night, HRRR wind (S_tq26_play uses a stand-in SW 5 m/s), and the
reservoir's drawdown beach (the shore photos show a wide dry lakebed).

## Eye-level flames (`AEmberFlameActor`; HCP4 H4-5)

Near the camera the terrain's flame glow is a flat wash, so the flame actor stands procedural
flame cards (`M_Flame`, additive, unlit) on every burning cell within 400 m (full to 220 m, fading
out by 400 m, where the terrain glow takes over; M_Terrain dims its own flame term to a glowing bed
over the same range). Per class: surface fire 14 cards per 30 m cell, 0.4-1.1 m; class 2 10 cards,
1.2-3.5 m; crown fire 6 cards, 6-22 m (a stream without classes is class 3). Freshness and the head
(FireTex A) scale the height as they scale the glow; cards closer than max(6 m, 1.2 x height) to
the camera are skipped. The flame is defined in world units from the card centre (instance data,
not the mesh UVs): a teardrop tongue whose edge and top the fire-clock noise tears, with a finer
internal flicker; the card is oversized so the tongue never meets its edge. Stills freeze the
fire clock, so captures stay deterministic. Tune the shape offline: the numpy mirror of the HLSL
renders a strip of cards in seconds. Facts: `flames.cells`, `flames.cards`. Scenario field
`flames = true` (default).

M_Veg torching is now tongues of flame licking up through the crown (3-D value noise scrolling up
on the fire clock) at a quarter of the old strength, not the whole crown painted orange. The old
flicker phase (N x 23 over a smooth 2.5 m noise) froze into concentric contour rings in every
still; it now comes from a 1.5 m value noise, one period per blob.

## Firebrands and spot fires (`AEmberFirebrandActor`; HCP4 H4-4)

The stream's spot records (formats.md §4 TICK spots: launch cell, landing cell, launch / land time,
ignited) are read once by worldcore (`Stream::spots()`). For the shown time the fire actor lists the
firebrands launched in the last 30 sim minutes; the firebrand actor draws:

- **showers:** each brand launched in the last 20 sim minutes is a loose shower of short spark
  streaks from its launch cell to its landing cell (up out of the column, gliding down downwind,
  cooling yellow -> red), fading with age;
- **new spot fires:** a brand that ignited marks its landing cell with a ground flare for 30 sim
  minutes after landing (the CA already burns the cell; the flare says "this one jumped").

Sparks keep a minimum on-screen size (0.25 % of the distance) so a 150 m shower reads from altitude.
Everything is a pure function of (brand, sim time, camera): deterministic stills, free scrubbing.
`M_Firebrand` is additive and unlit, depth-faded into the ground. Facts: `fire.spots_total`,
`fire.spots_ignited_total`, `fire.spots_recent`, `firebrands.showers / spot_glows / sprites`.
Scenario field `firebrands = true` (default).

## Probes

`[[fire_probes]]` (name, x, y in the replay grid's CRS) report `fire.probes.<name>` = the phase
shown at that point (-1 outside, 0 unburnable, 1 unburned, 2 burning, 3 burned). `S_jolly_fire`
asserts probe phases and exact burned/burning counts at each capture, computed independently
from the stream with `ember.sim.stream.iter_frames`: "front position matches the state stream
(probe-verified); nothing burns that the stream says didn't".

## 2D | 3D split (`ember-dev split <scenario> --left <2D mp4>`)

Puts Epic 4's 2D playback MP4 beside a straight-down timelapse orbit of the same replay
(`map_timelapse` in `S_jolly_fire`: same frame count and rate as the 2D video). The 3D side is
cropped to the replay grid from the camera geometry (UE FOV is horizontal: scale =
width / (2 d tan(fov/2)) px per metre at the target) and scaled to the 2D map rows, so both maps
line up; the 2D HUD clock serves both.

## Scenario fields

```toml
[scenario]
replay = "../../runs/cp2/cp2-jolly-playback.replay.json"   # relative to the scenario file
smoke = true              # default with a replay
smoke_wind_ms = 8.0

[[captures]]
name = "d19"
bookmark = "overview"
t_s = 1645200             # sim seconds since the replay's t0

[[orbits]]
name = "timelapse"
bookmark = "overview"
t_from_s = 0
t_to_s = 3031200

[[fire_probes]]
name = "run_d19"
x = 657386.5
y = 5242948.3
```

Dev fixture: `S_tq_fire_synth` renders a synthetic 48 h wind-driven fire over Three Queens
(`ember-dev synth-fire three_queens_2026 --ignition 0.507,0.488`), so fire rendering is built and
regression-tested independently of the real Jolly Mountain world.
