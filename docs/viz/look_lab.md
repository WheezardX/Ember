# Look lab — small scenarios for iterating on the look

Brad (2026-10-01): "create some small scenarios for testing purposes, especially for iterating on
look". The regression scenarios check that nothing broke (19 scenarios, ~23 min); the look lab is
for tuning one thing against its reference photo in under half a minute.

```
ember-dev look L_ground             # build (incremental) + regen changed generators + render + sheet
ember-dev look L_flames --no-build --no-regen   # just render + sheet
ember-dev regress --tier look       # all of them, checked like the regression (~2.5 min)
```

`look` prints the time per step and writes `runs/viz/<L_x>/<run>/look_sheet.jpg`: per capture,
**now | previous render | reference photo** (the capture's `reference`, a still from the
reference library). Every lab scenario runs on `teanaway_dev` (1.44 km fixture, renders in ~18 s).

| Scenario | Isolates | Reference |
|---|---|---|
| `L_ground` | timber floor at eye height + a close-up: ground shader, cover, relief | Kachess stand / litter photos |
| `L_trees` | one tree per species in a row: bark, trunks, crowns | WETC open ponderosa stand |
| `L_trees_stand` | a stand at eye height: bark close, spacing, crown base | Kachess stand photo |
| `L_flames` | the lab fire's front LINE at 6 h from 50 m (flames v2), + a 4 s clip (`burn`, fire 6 h -> 6 h 10 min) | WETC surface fire in litter |
| `L_night` | the same front after dark | spruce-fir night torching |
| `L_smoke` | columns + pall from 800 m at 8 h (peak burning) | Glass Fire telephoto |
| `L_smoke_motion` | 4 s of the lab columns, fixed camera, fire 6 -> 7 h (smoke must only rise) | - |
| `L_scar` | the burned mosaic at 20 h from above | spruce-fir aerial scar |

Three Queens 2026 views (`three_queens_2026`, ~30-45 s):

| Scenario | Isolates | Reference |
|---|---|---|
| `L_sky` | ground-level exposure / sky: the Kachess shore with and without the Aug 9 front | InciWeb Aug 9 shore photo |
| `L_smoke_far` | merged far-field smoke, the Aug 21 run from 14 km | the Kachess pall photo |
| `L_smoke_mid_motion` | 4 s from 4 km (the merge cross-fade), fire sweeping 2 h | - |
| `L_stars` | the night sky over Kachess, ~1 am mid-August, north + south-west | (check: Polaris ~47 deg up due north) |

Motion clips: scratch tools measure them (frame-to-frame change, block-matched up / down motion).
Video captures show a known exposure ramp-and-snap every ~25 frames (whole-frame brightness, not
the subject) - read spikes with that in mind.

The fire scenarios use a 24 h synthetic fire with intensity classes (head 3 / flanks 2 / backing 1,
kept after burn-out so the burned mosaic has variety):

```
ember-dev synth-fire teanaway_dev --hours 24 --ignition 0.25,0.5 --classes --name lab
```

On the 48 x 48-cell fixture grid it peaks at ~8 h (1,030 cells burning) and has burned most of the
region by 14 h; camera targets come from its arrival field. The timber views sit on the densest
canopy in the fixture (0.128, 0.344: cover 65 %, TU5) - teanaway_dev is mostly open forest.
