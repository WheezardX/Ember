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
