# HCP3 — "Fire replays in 3D" (Jolly Mountain 2017)

**Ask:** sign-off that the core product moment works: a real fire (the Epic 4 CP2 replay of the
Jolly Mountain 2017 arrival record, 35 days) burns in the renderer, and what you see matches the
state stream.

## What to look at

| File | What it shows |
|---|---|
| `orbits/jolly_timelapse_720p.mp4` | Whole fire, 35 days in 20 s, camera slowly circling: front, smoke plumes, scar accumulating |
| `orbits/jolly_split_2d_3d.mp4` | Epic 4's 2D CP2 animation beside the renderer's top-down timelapse, frame for frame, same scale |
| `stills/jolly_d06 … d35.jpg` | Scar progression from the same camera: days 6, 13, 19, 25, 35 |
| `stills/jolly_map_d35.jpg` | Final footprint straight down (compare with the 2D map) |
| `stills/jolly_run_mid_d19.jpg`, `jolly_run_close_d19.jpg` | The day-19 run at its peak hour (9,718 cells burning), mid distance and in the trees |
| `orbits/jolly_run_d19_720p.mp4` | 6 hours of the day-19 run in 10 s (weak shot, see below) |
| `stills/jolly_ground_*.jpg`, `jolly_ridge_lookout_d19.jpg`, `jolly_scar_morning_d20.jpg` | Added after your first look: ground level behind and ahead of the day-19 head, a ridge lookout 2 km off, and standing in the scar the next day. A baseline, not a result: they show what HCP4 and the ground plane have to fix |

## Pass criteria (EPIC_5_PLAN §4)

* **Front position matches the state stream (probe-verified): PASS.** `S_jolly_fire` asserts 71
  checks (`jolly_verdict.json`), all passing. At every capture the renderer's burned and burning
  cell counts equal counts computed independently from the `.ess` stream with Epic 4's own
  reader (e.g. day 19: 44,602 burned, 9,718 burning; day 35: 166,647), and five probe points
  report the right phase at the right time (ignition burned from t0; a cell the day-19 run
  reaches is unburned on day 13, burning on day 19, burned by day 25; a late western cell;
  a burnable cell never reached; an unburnable cell).
* **Scar accumulates correctly: PASS.** Stills d06 → d35 and the split.
* **Nothing burns that the stream says didn't: PASS** by the same counts and probes (the
  unburned and unburnable probes stay unburned all 35 days). Equal counts plus probes are strong
  evidence, not a cell-by-cell proof; a full per-cell diff of the fire texture against the stream
  is a cheap addition if you want it.
* **Goosebumps:** your call. Best: the timelapse's first half and the split.

## What's new since HCP2

* **Fire state player** (D1): worldcore reader for Epic 4's stream + replay, exact vs the Python
  reader. The renderer shows any sim time; a cell burns from its exact arrival time, so the
  timelapse front moves continuously.
* **Fire rendering** (D2): char on the ground and trees; a bright leading edge that fades over the
  first hour; faint smoulder; trees torch as the front arrives. From altitude the active edge
  widens with camera distance so it stays visible (like the IR map / the 2D orange).
* **Smoke v0**: plumes from recently burned ground, rising to a height set by how much is burning
  around them (up to ~2 km on day 19), bending and drifting with the wind, thinning downwind.
  No fluid sim (D6). Deterministic, so every frame is repeatable.
* **Pre-fire world**: the Jolly render world is baked from LANDFIRE 2016 (U10: the Epic 4 sim
  world uses LF2025, which already contains this scar), Lake Cle Elum water plane.
* Tooling: `[[fire_probes]]`, `ember-dev split`, `docs/viz/fire.md`.

## Known weak spots (proposed for HCP4, "the fire reads")

1. **Flames up close** (`jolly_run_close_d19`): torching crowns are a uniform orange carpet.
   Faithful to the data (a ~700-cell crown run in two hours) but no variation in flame
   intensity, height or flicker between trees; reads a little like autumn foliage.
2. **The 6-hour run clip** puts the camera inside the plume's haze; only the burning ring reads.
   Needs a camera above/upwind of the smoke and near-camera smoke thinning.
3. **Wind is a scenario constant** (from the west). The arrival-playback replay carries no wind
   field; plume-vs-HUD wind agreement is an HCP4 criterion.
4. **Scene exposure is still dark** (the -2 EV decision from HCP2 is open).
5. **At ground level it falls apart** (the added stills): burning crowns read as orange paint or
   autumn larch, not flame; the char ground shows a marbled noise pattern; crown-fire stands the
   next morning keep full dark crowns instead of standing as black snags; ash ground is flat
   grey. HCP4 (intensity classes, real flames, embers) plus the ground plane (duff, rocks, ground
   cover) own this.
6. ~~Capture is slow~~ **Fixed after bundling (ecda34d):** orbit frames stream to the GPU video
   encoder (NVENC) instead of a PNG each; the full Jolly scenario went from 13 min to 3 min.

## Perf / memory

Full Jolly forest (7.1 M trees in the region), fire and smoke: ~3.2 GB VRAM (D8 budget 4 GB).
Smoke peaks at ~5,300 puffs on day 19. No perf window was measured for this checkpoint.

## Next (agreed 2026-09-28)

HCP3 sign-off → **U6 bounded-memory chunked ingest** (bake one shared tile grid in chunks, so
large fires stream and grow seam-free) → **Big Grass 2026** (Owyhee rangeland, the contrast
fire) → ground plane, HCP4.

## Questions

1. Sign off HCP3?
2. Flames and smoke close-up to HCP4 as listed, or fix any before sign-off?
3. G1 fast capture right after sign-off, before U6?
