# HCP1 — Terrain reads true

**Status:** awaiting Brad's sign-off (plan §5).
**Date:** 2026-09-27 · **Engine:** UE 5.8.3 · **Runner:** dev box, RTX 4080 SUPER
**World:** Three Queens 2026 (IRWIN {DFA477F2-…}, Kittitas County), a 10 m Terrain bake of the
incident AOI: 16.8 × 18.3 km, relief 664–2,126 m, 286 tiles over 4 LODs.
**What you're judging:** materials, lighting and scale honesty. Are the landforms recognizable?
Are there seams or cracks? Is the slope/landcover colouring plausible? Is the budget met on
terrain alone?

## What to look at

| Item | File |
|---|---|
| Orbits: dawn / noon / dusk around the fire (720p review copies; 1440p in the run dir) | `orbits/orbit_{dawn,noon,dusk}_720p.mp4` |
| Bookmarks: overview from the south, burn area from the west, Kachess Lake, Cooper Lake, NW alpine at dusk | `runs/01-three-queens-terrain/contact_sheet.png` |
| Side-by-side vs Terrain's own hillshade (top-down clay render, same 315°/45° light) | `hillshade_compare.png` |
| Teanaway with the same look (regression views) | `runs/03-teanaway-look/contact_sheet.png` |
| Perf | `perf.csv` |

## Results

- **Landforms register with the source.** The top-down render and `derived/hillshade.cog.tif`
  line up ridge for ridge and drainage for drainage. The render has lower contrast because UE
  adds sky fill light, where gdaldem is pure Lambert shading.
- **No tile seams or LOD cracks visible** in bookmarks or orbits. Same-LOD edges are
  bit-identical by construction and skirts cover LOD transitions.
- **Budget (60 fps @ 1440p, ≤ 4 GB VRAM), terrain only: met.**

  | Scenario | p50 | p95 | GPU avg / p95 | VRAM | Triangles | Tiles (LODs) |
  |---|---|---|---|---|---|---|
  | Three Queens, fire oblique | 2.65 ms | 10.1 ms | 3.4 / 3.5 ms | 1.35 GB | 13.2 M | 69 (12–14) |
  | Teanaway look | — | 3.2 ms | 2.4 ms | 0.9 GB | 0.19 M | 9 |
  | Teanaway + all 74,595 trees (HCP2 preview) | — | 6.7 ms | 5.0 ms | 0.95 GB | — | 9 |

  The Three Queens p95 tail is CPU streaming hitches, not rendering (see finding 3).
- **Vegetation conformance at full scale:** the renderer's C++ scatter reproduces all
  **5,861,933** Three Queens trees exactly (Terrain `veg/instances.npy`), in ~2 s of C++. Trees
  are HCP2, but the pipeline is ready.

## Findings (defects first)

1. **Lakes are holes (upstream U7).** LiDAR drops returns over water and Terrain's void fill
   stops at 5 px, so lakes are DEM nodata and the mesh has nothing there.
   - Three Queens has 23,856 interior hole cells, 99.3 % of them FBFM40 water; the biggest is
     190 ha of Kachess.
   - In the oblique views they read as dark blue water, but that's sky seen through the hole.
     The top-down view makes it obvious.
   - The right fix is Terrain's DEM finalize: hydro-flatten each water body to its shoreline
     elevation. A renderer patch can't do it cleanly, because lakes span tiles and per-tile
     lake levels would step at seams. Filed as U7 (`EPIC_4_PLAN.md` §11).
2. **Region edge = diorama cliff.** The world ends at the AOI with a vertical skirt. That's
   honest, but it's visible in the wide shots.
3. **Tile loads are synchronous.**
   - Composing a tile's albedo (4× supersample + mips) costs ~100+ ms of CPU.
   - One re-selection loaded enough tiles to block for 2.9 s.
   - Captures are unaffected (the harness waits), but the interactive shell (HCP6) would hitch.
   - Fix before HCP6: compose and mesh on a worker thread (worldcore is engine-free and
     stateless), and upload on the game thread.
4. **10 m landcover still shows near-field blockiness** (roads and meadow edges) despite the
   4× warp + blur. It's inherent in 10 m categorical data, and will be mostly covered by trees
   (HCP2).
5. Found and fixed on the way:
   - The manifest reader choked on Terrain's `"mesh": null`. Now null-safe, with a regression
     test.
   - A perf number measured during the bake (CPU contention) was re-measured on a quiet machine
     (13.7 → 3.2 ms).

## Built since HCP0 (Phase 1)

- **C3:** LOD streaming.
- **B2:** data-driven terrain look (`viz/looks/terrain_default.toml`: FBFM40 class colours →
  NDVI greening → canopy darkening → slope rock) and the generated `M_Terrain`.
- **C4:** exact C++ scatter port plus instanced vegetation.
- **B3 v1:** generated Nanite species meshes.
- **Harness:** orbit capture → MP4, per-scenario console commands and perf pose, and the
  hillshade comparison (`ember-dev hillshade-compare`).
- **Data:** the Three Queens incident bundle and 10 m bake.
- **Upstream design notes:** U6 (bounded-memory ingest) and U7 (lake hydro-flattening).
- Docs: `docs/viz/world.md`.

## Deviations

- HCP1's orbits come from the harness frame capture, not Movie Render Queue. MRQ is G1, the
  product render path, and arrives with HCP3.
- Tree meshes (B3) and instancing (C4) were built early, alongside HCP1.

## Questions for you

1. **Gate:** do the landforms read true (Kachess valley, Cooper Lake, the alpine NW)? Is the
   landcover colouring plausible for this country?
2. **Lakes (U7), before sign-off or after?**
   - Fix it upstream in Terrain now (I can implement U7 in the Terrain repo and re-bake).
   - Or accept holes for HCP1 and fix before HCP3, where the fire's valley and lake views
     matter more.
3. **Region edge:** leave the diorama, fade it out, or bake a coarse 30 m surround ring so the
   horizon continues?
4. **Look direction:** keep fuel-class colouring (reads like a map: legible, honest), or push
   toward photographic (NAIP / Sentinel-2 true colour, which would be a new upstream layer)?
