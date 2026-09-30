# The iteration harness (`ember-dev`) — EPIC_5_PLAN §2, workstream A

The loop an agent runs to change the UE renderer without ever opening the editor:

```
ember-dev doctor                 # is this machine a working runner? (engine pin, toolchain, GPU, data)
ember-dev build                  # UBT incremental build of EmberEditor; parsed errors
ember-dev run-scenario S         # headless capture run -> runs/viz/S/<stamp>/
ember-dev evaluate S             # goldens + facts + budgets -> verdict.json + contact_sheet.png
ember-dev loop S                 # build + run-scenario + evaluate; exit 0 only on PASS
ember-dev bless S                # accept the latest run's captures as goldens (after looking!)
ember-dev regen-assets [--check] # assets-from-code (docs/viz/assets.md)
ember-dev scenarios              # list render scenarios
```

Every command takes `--json` where it produces a result. `ember-dev` is installed with ember
(`pip install -e .` in the terrain env); `python -m ember.dev ...` is equivalent.

Measured on the runner (HCP0, 2026-09-27): **inner loop 20–35 s** (incremental build 5–20 s,
harness run ~13 s, evaluate ~2 s). First run after a clean checkout adds shader compilation
(~1 min).

## Streaming perf (`perf_orbit`)

`perf_orbit = "<orbit>"` makes the perf window fly that orbit or flyover in real time (one pose
per frame, nothing captured), so frame times include streaming. Orbits with `capture = false`
are paths only (no video). `facts/perf.json` reports `hitches_33ms` / `_50ms` / `_100ms` (frames
over 2x / 3x / 6x a 60 fps frame). `S_stream_tq` is the reference run (a low pass over a Three
Queens stand with trees + ground cover): 0 hitches, max 12 ms after the 2026-09-30 streaming pass
(was 28 hitches, max 503 ms). Capture runs stream synchronously (goldens stay reproducible:
tiles arriving over many frames leave Lumen in a slightly different state); play mode and perf
windows stream asynchronously. For a breakdown of a hitch add
`perf_exec_cmds = ["t.HitchFrameTimeThreshold 30", "stat dumphitches"]`.

## Flying around a scenario (`ember-dev play`)

```
ember-dev play S_ground_tq                  # windowed 1920x1080, starts at the first capture's bookmark
ember-dev play S_ground_fire_tq -b smoulder # any bookmark; --res 2560x1440, --fullscreen, --wait
```

The harness loads the scenario exactly as for a capture run (look, trees, ground cover, water,
fire replay, smoke), then hands the view to a free-fly pawn (`AEmberFlyPawn`) instead of
capturing. Speed follows the height above ground (~4 m/s at eye level, ~0.8 x height above
that) and the camera never goes below 1 m. Controls: mouse look, WASD, E/Space up, Q/C down,
Shift x4, Ctrl x0.25, mouse wheel speed, G walk (eye 1.7 m, follows the ground) / fly, 1-5 sun
dawn..dusk; with a fire replay P plays the fire, `,` `.` step -/+1 h, `[` `]` halve/double the
rate; L toggles a camera lamp (inspect dense foliage); H hides the help block; Esc quits. The
fire line shows the date and US Pacific local time of the shown moment and the day / hours
since the stream's t0 (the incident start). Play mode geomorphs terrain tile swaps (0.6 s,
M_Terrain WPO from UV2; captures pop, so goldens stay deterministic; perf windows morph too:
fact `tiles.morphed`). Logs go to `runs/play/<scenario>/<stamp>/log/`.

### Atmosphere and far field (8g items 5-6)

Haze = exponential height fog densest at the region's lowest ground, coloured by the sky
atmosphere (tracks time of day; x1.8 so the horizon matches) plus a sun glow. Scenario fields
`haze_density` (0.0015; 0 = off), `haze_falloff` (0.006 = ~1.7 km scale height - UE's falloff is
per ~10 m, so 0.05 meant no haze 200 m above the valleys), `haze_start_m` (200). Past the tree
radius (0.85 x `veg_radius_m`; everywhere when `vegetation = false`) M_Terrain draws a canopy
impression on forested ground: 30 / 9 / 3.5 m value-noise stands, crowns and gaps with crown
bumps, each octave fading to its mean below ~2 px (it aliased into a herringbone).

## What happens in a run

`run-scenario` writes a **run plan** (`plan.json`, `ember-run-plan` v1 — the scenario resolved
to absolute paths) and launches

```
UnrealEditor.exe unreal/Ember/Ember.uproject -game -RenderOffscreen -unattended -nosplash
    -nosound -windowed -ResX=W -ResY=H -EmberRun=<plan.json> -abslog=<run>/log/Ember.log ...
```

No window, no editor UI. `AEmberGameMode` sees `-EmberRun=` and spawns `AEmberHarness`, which:
loads the world (`AEmberTerrainActor`, Terrain tile store via worldcore), then for each capture
places the camera from its bookmark, renders `warmup_frames`, grabs the frame, writes
`captures/<name>.png` and `facts/<name>.json`; then measures `perf_frames` frames into
`facts/perf.json`; then writes `run_status.json` and exits. A watchdog (`--timeout`, default
600 s) kills a hung run. The harness's own `run_status.json` is authoritative for success; the
process exit code is kept for diagnosis.

Run directory:
```
runs/viz/<scenario>/<stamp>/
  plan.json  run.json  run_status.json  log/Ember.log
  captures/<capture>.png      facts/<capture>.json  facts/perf.json
  verdict.json  contact_sheet.png  diff/<capture>.png      (written by evaluate)
```

## Render scenarios (`viz/scenarios/*.toml`, `render_scenario_version = 1`)

Adding a test view is a data change. Schema (validated by `ember/dev/scenario.py`):

```toml
render_scenario_version = 1
[scenario]
name = "S_terrain_gray"
description = "..."
world = "terrain:teanaway_dev"   # Terrain store region ($EMBER_TERRAIN_STORE or ../Terrain/store),
                                 # or a path relative to this file
replay = "..."                   # optional .replay.json (fire state; Phase 2)
resolution = [2560, 1440]
budget = "interactive"           # key into viz/budgets.toml
perf_frames = 300                # 0 = no perf window
exposure_mode = "auto"           # default; histogram metering, EV100 [0.5, 10], bias -1.3
                                 # (exposure_ev_min / exposure_ev_max / auto_exposure_bias).
                                 # Captures adapt instantly, so stills stay deterministic;
                                 # play mode adapts at 1.5 stops/s. A bookmark may set its own
                                 # exposure_ev_min (closed canopy meters below the floor).
exposure_bias = -2.0             # manual EV100 bias (exposure_mode = "manual": calibration
                                 # fixtures S_terrain_gray / S_tq_hillshade)

[[bookmarks]]                    # orbit-style: survives terrain edits
name = "overview_n"
target_frac = [0.5, 0.5]         # 0..1 of the DATA extent, x east / y south  (or target_cell = [x, y])
distance_m = 2300
yaw_deg = 0                      # compass bearing the camera looks toward
pitch_deg = -40
fov_deg = 55
sun = "noon"                     # dawn | morning | noon | afternoon | dusk | "az,el"

[[captures]]
name = "overview_n"
bookmark = "overview_n"
warmup_frames = 30
ssim_min = 0.995                 # defaults; loosen per capture only with a reason
region_ssim_min = 0.98
golden = true                    # false: captured + shown, never diffed

[[assert]]                       # scene facts that must hold (docs/viz/scene-facts.md)
fact = "tiles.loaded"
op = "=="
value = 9
capture = "overview_n"           # optional; default: every capture
```

Bookmark targets address the **data extent** — the valid AOI — not the tile grid, which can
extend past it (teanaway_dev: 1.44 km of data in a 1.92 km tile grid).

### Reference cameras (EPIC_5_PLAN 8i R1)

A bookmark can instead be an **absolute camera**, for matching a real photo:

```toml
[[bookmarks]]
name = "kachess_ridge"
camera_lonlat = [-121.2032, 47.4069]   # or camera_xy = [x, y] in the world's CRS (metres)
camera_agl_m = 1.7                     # default; or camera_alt_m (the DEM's datum, NOT GPS altitude)
yaw_deg = 200                          # compass bearing the camera looks toward
pitch_deg = 0
fov_deg = 69.4                         # horizontal; a phone's 26 mm-equivalent main camera

[[captures]]
name = "kachess_ridge"
bookmark = "kachess_ridge"
reference = "store/reference/tq/kachess_ridge.jpg"   # optional: the photo
```

`ember-dev ref-camera photo.jpg` prints that stanza from the photo's EXIF (GPS, compass
direction if the phone wrote it, 35 mm-equivalent focal length, time taken). Pitch is not in
EXIF: start level and refine against the pair. `ember-dev ref-pair <scenario>` writes
`<run>/refpairs/<capture>_pair.jpg`: photo | render | the render's skyline and ridge edges over
the photo (for lining up the pose). Give the scenario the photo's aspect ratio (phones: 4:3) so
nothing is cropped. Absolute cameras are kept 0.3 m over the ground (not the 3 m of orbit
bookmarks) and cannot start an orbit. Scene facts report `camera.world_x_m / world_y_m /
world_z_m / agl_m / bearing_deg`; `S_refcam_tq` asserts a camera lands where asked.

`fov_deg` is the **horizontal** field of view at every aspect ratio (the capture camera keeps
X-FOV). Before 2026-09-30 the engine default held a 16:9 frame's vertical FOV, so non-16:9
captures came out narrower than asked; every scenario then was 16:9 except the new two.

### NAIP colour probe (EPIC_5_PLAN 8i R2, advisory)

`ember-dev naip <region>` fetches the latest NAIP year over the region (Planetary Computer COGs,
overview level nearest 10 m only: ~30 MB for Three Queens, logged in the store's download
ledger) -> `store/render/<region>/naip/naip_rgb.tif`. `S_naip_tq` renders a 3 x 3 grid of
straight-down sites 2.5 km up (vegetation streamed in, fog and atmosphere off, noon); then
`ember-dev naip-probe S_naip_tq` maps every pixel to the ground from the camera facts, averages
into NAIP's cells and compares median CIELAB colour per FBFM40 group (timber, grass, shrub,
barren, water, ...): delta-E raw and after one global brightness match. Output:
`<run>/naip_probe.json` and a render | NAIP sheet (`naip_probe.jpg`: check alignment there
first). Advisory - no pass / fail until tolerances are tuned (8i open question 2).

## Evaluate: what PASS means

* **Golden diff** per capture: SSIM on luminance, globally and on a 4×4 region grid; the region
  minimum catches local breaks a global mean averages away. Goldens live in
  `viz/goldens/<scenario>/` downscaled to 1280 px wide; captures are BOX-downscaled to match.
  A capture with no golden is `new` (not a failure).
* **Scene-facts asserts** from the scenario.
* **Budgets** (`viz/budgets.toml`, EPIC_5_PLAN D8) against `facts/perf.json`: key
  `<fact path with . as __>_max|_min`, e.g. `perf__frame_ms_p95_max = 16.7`.
* The run itself succeeded (`run_status.json` exit 0).

Why the thresholds are tight (0.995 / 0.98): two unchanged runs are **pixel-identical** on the
runner (instant-adapting or fixed exposure, fixed warmup, no DOF/motion blur). The HCP0 calibration found that the
first defaults (0.97 / 0.90) let both a real material change and a flipped-normal regression
pass. Tight thresholds are cheap because noise is zero; loosen per capture (with a comment) for
views that are legitimately non-deterministic (Niagara, later).

The **contact sheet** (one row per capture: capture | golden | dissimilarity heatmap) is what a
reviewer — human or model — looks at. The verdict JSON is the agent's feedback signal.

## Blessing goldens

`bless` copies captures over goldens. Only bless after looking at the contact sheet and
deciding the change is intended; say why in the commit message. Goldens are committed.

## Gotchas (learned the hard way)

* **Shaders compile lazily in `-game`:** the first run after a build used to capture grey
  fallback materials (2026-09-29: S_forest_teanaway oblique ssim 0.75 cold vs 0.81 warm). Warmup
  frames now only count once `GShaderCompilingManager` and the asset compiler are idle (log:
  `waiting for N shader job(s)`; capped at 900 s).
* **Facts can pass while the picture is wrong.** The first HCP0 run had correct tile and
  triangle counts and rendered only skirts (inverted winding). Always look at the sheet.
* Run from an **activated** terrain env (`conda activate terrain`). Calling the env's
  `python.exe`/`ember-dev.exe` bare leaves `<env>\Library\bin` off PATH and anything using BLAS
  (numpy `@`, terrain's reprojection) dies silently with 0xc06d007f / exit 127. Agents: prefix
  `PATH=<env>/Library/bin:<env>:<env>/Scripts:$PATH`.
* Run-scenario needs the editor target built (`ember-dev build`); uncooked `-game` loads the
  editor-built project modules.
