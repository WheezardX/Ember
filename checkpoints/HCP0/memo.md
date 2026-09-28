# HCP0 — The loop closes

**Status:** SIGNED OFF by Brad, 2026-09-27 (Q1: yes). See the resolution at the end.
**Date:** 2026-09-27 · **Engine:** UE 5.8.3 (CL 58210709) · **Runner:** dev box, RTX 4080 SUPER
**What you're judging:** the agent workflow itself. Do you trust the harness enough to review
bundles like this instead of screens?

## The loop, end to end, hands-free

```
ember-dev loop S_terrain_gray      # build -> headless run -> evaluate; exit 0 only on PASS
```

| Stage | Time (measured) | What it does |
|---|---|---|
| `build` | 5–36 s incremental | UBT EmberEditor build; errors parsed to file/line/code JSON |
| `run-scenario` | ~13.5 s (≈81 s first run: shader compile) | `UnrealEditor -game -RenderOffscreen` with a run plan: load Teanaway tiles, 3 bookmarked captures at 2560×1440, facts JSON each, 300-frame perf window, exit |
| `evaluate` | 0.3–2 s | SSIM vs goldens (global + 4×4 region grid), 8 fact asserts, 3 budget checks → verdict JSON + contact sheet |
| **inner loop total** | **19.8–33.6 s** | gate: ≤ 10 min ✔ |

No editor UI was opened at any point. The editor binary runs only as `-game` (captures) and as a
headless commandlet (asset generation). **Two unchanged runs produce pixel-identical captures**
(SSIM 1.000, mean luma diff 0). With fixed exposure, fixed warmup, and no DOF or motion blur,
the renderer is deterministic on this runner, so any diff means something changed.

## The proof sequence (`runs/`, one contact sheet each: capture | golden | heatmap)

1. **`01-winding-bug`: facts pass, picture wrong.** The very first run reported correct facts:
   9 tiles, 41,472 triangles (= 144 × 144 × 2, exactly the AOI's valid pixels). But the image
   showed only the tile *skirts*, because the surface was back-face culled (winding inverted
   for UE). This is why the plan says "probes before vibes" and still *requires* looking. Fixed
   in `worldcore` with a unit test that pins UE's winding convention. (Against today's goldens
   this run scores SSIM 0.75 / region 0.59.)
2. **Exposure sweep** (`exposure_sweep.png`). Gray-shaded terrain at EV bias 0 … −4, run
   through the harness (~13 s per run). −2 was chosen: a readable mid-gray and a sky that isn't
   blown out. Auto-exposure is off by design, since it would make captures drift.
3. **`02-material-change-caught`: calibration.** Switching the terrain to the generated clay
   material (roughness 0.9) changed the low grazing view. Mean luma diff was 10.9, localised on
   the sun-facing slope in the heatmap. It **passed** the first thresholds (0.97 / 0.90). Since
   noise is zero, I tightened the defaults to 0.995 / 0.98, reviewed the sheet, and blessed the
   change as intended.
4. **`03-deliberate-break-caught`: the planted regression.** One line in `AEmberTerrainActor`
   flips the Y component of the normals, a plausible sign bug. Unit tests can't see it
   (worldcore is correct) and neither can facts (all 11 checks pass). Evaluate flags **all 3
   captures**: SSIM 0.981 / 0.985 / 0.987, worst regions 0.946 / 0.966 / 0.970. Under the old
   thresholds this would also have slipped through.
5. **`04-fixed-green`.** Reverted. PASS: 3/3 match, 11/11 checks. Loop time 19.8 s.

## What exists now (Phase 0 scope: A1, A2, A4, C1/C2 minimal, B1 skeleton)

- **A1 `ember-dev`:** `doctor`, `build`, `run-scenario`, `evaluate`, `loop`, `bless`,
  `bundle`, `regen-assets`, `scenarios`. Render scenarios are TOML data (`viz/scenarios/`), and
  budgets live in `viz/budgets.toml`.
- **A2 scene facts:** `ember-scene-facts` v1 (camera, world/data extent, tiles/LOD histogram,
  triangles, draw calls, primitives, per-process VRAM via DXGI, frame/game/render/GPU times).
  Console: `Ember.DumpFacts`.
- **A4:** `docs/viz/runner.md` plus `ember-dev doctor`. Provisioning required one addition:
  the .NET Framework 4.8 SDK, since UBT refuses to build the editor target without it (UAC
  approved during the session).
- **C1/C2 minimal:** `worldcore/`, engine-free C++20 (tile-store manifest v2, uncompressed
  GeoTIFF reader, heightfield→mesh with vertices on pixel corners, so same-LOD seams are
  bit-exact by construction). It's compiled into the UE module, with 8 doctest cases / 326
  assertions against synthetic regions (CI) and real `teanaway_dev`.
- **B1 skeleton:** `assets/generators/` → `M_EmberGray` generated headless in ~15 s; lock file
  with an engine-free `--check` in the new `viz.yml` CI workflow.
- **Perf, terrain only, 1440p:** p95 frame 3.9 ms (~250 fps), GPU 2.6 ms, VRAM 0.9 GB, 86 draw
  calls. Budget (16.7 ms, 4 GB) met by a wide margin, as expected for 41k triangles.

## Deviations from the plan (flagged, not silent)

- **D10 (added at kickoff, confirmed):** the upstream data is thinner than the plan assumed.
  The only tiled terrain is `teanaway_dev` (1.44 km @ 10 m), Jolly Mountain is 30 m and
  untiled, there's no imagery, and the Epic 2 scatter "spec" is the code itself. Phase 0 ran on
  `teanaway_dev`, and Phase 1 must bake more (see Q2).
- The CSV profiler isn't used. The facts subsystem measures frame, game, render and GPU times
  directly, which is simpler and already in the verdict. `-csvCaptureFrames` can be added if
  deeper breakdowns are needed.
- The GameMode loads the engine's `Entry` map and spawns everything at runtime, so the project
  has no `.umap` at all. That's stricter than the plan required.
- The model-eyes reviewer (A3) is not built yet. It's plan Phase 0 adjacent, and I'd rather
  calibrate it on HCP1's material work, where aesthetics actually vary.

## Open questions for you

1. **Gate:** do you trust this loop enough to review bundles instead of screens? Anything you'd
   want added to a bundle before HCP1 (e.g. MP4s: orbit capture is Phase 1 work)?
2. **Phase 1 bakes (D10):** I propose (a) a **10 × 10 km Teanaway region** centred on the dev
   AOI at 10 m game profile via Terrain's pipeline, and (b) **Jolly Mountain** tiled at 10 m if
   3DEP EPT covers the AOI, else 30 m. OK, or a different Teanaway footprint (e.g. one you've
   stood in, for the HCP2 "does it look like Teanaway" judgement)?
3. **Oblique view, dark slot drainage** (`oblique_nw`): a very narrow, very dark channel. It's
   either real creek incision in the 3DEP DEM or an artifact (hydro-flattening, void fill). I'll
   check the source DEM in Phase 1. Do you know that drainage?

## Next (Phase 1 → HCP1 "Terrain reads true")

B2 terrain master material (slope/aspect/landcover/greenness blending from store layers), C2
LOD rings with skirts across LODs, C3 camera-driven streaming, orbit-MP4 capture mode (G1-lite),
H1 budgets on every verdict (already on), the bigger bakes, then the HCP1 bundle: dawn/noon/dusk
orbits, a bookmark sheet, side-by-side against Epic 1 hillshades, and a perf CSV.

## Resolution (2026-09-27)

1. **Gate:** Brad signed off; bundles replace screens.
2. **Datasets:** Teanaway is not required. Brad wants to iterate on different fires to exercise
   different data. Proposal sent: Jolly Mountain + BIG GRASS, with teanaway_dev kept as the fast
   golden fixture.
3. **Dark slot drainage:** checked against Google Maps satellite and a hillshade of the source
   DEM. The AOI is Suncadia above Cle Elum/Roslyn (Skyline Ridge, Horizon Ridge Rd, Carry
   Canyon, Deer Creek Rd). The slot is a real forested N-S draw about 6-9 m deep along
   ~-120.925, not a DEM artifact. It reads as a dark "slot" because the afternoon sun (az 245°,
   el 35°) shadows its east-facing wall, which then gets only blue skylight on flat gray clay.
   This should resolve with real materials and vegetation in Phase 1; re-check at HCP1.
