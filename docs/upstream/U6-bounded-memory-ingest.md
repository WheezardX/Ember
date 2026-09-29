# U6 — Bounded-memory ingest for Terrain (upstream design note)

> **Status (2026-09-28): implemented** in Terrain as ADR 0008 (`adr/0008-chunked-ingest.md`,
> commits f2559ac..bb643e4); acceptance record `tasks/u6/acceptance.md`. Differences from this
> note: vegetation runs in full-width row bands (identical `instances.npy`, same order, no
> `layer_version` bump); the CHM outlier filter runs in two passes over one region-wide
> threshold with exact k-NN (re-query past the halo), so results do not depend on chunk size;
> bandwidth controls (`clip_to_aoi`, `max_scenes`, `max_fetch_gb`) and a download ledger were
> added for the household data cap. Parallel chunks and print exports remain "later".

**Owner:** Terrain (Epics 1–2 pipeline) · **Filed from:** Ember Epic 5, 2026-09-27
**Trigger:** the Three Queens 2026 bake (238 km² @ 10 m) peaked at 13.4 GB in the LiDAR stage
and was killed once for memory pressure on a 32 GB runner.
**Requirement (Brad):** ingest is rare and may be slow, but peak memory must not scale with
incident size. A 256 GB bake machine must still be able to bake a 10,000 km² fire.

## 1. Why more RAM alone does not fix this

A stage-by-stage audit of `terrain/` found that nothing streams or windows except GDAL internals
(`gdal.Warp`, `DEMProcessing`, `Translate`) and the final tiler. Every other stage holds the
whole AOI in memory.

| Stage | Where | Memory pattern | ~Peak scaling |
|---|---|---|---|
| LiDAR ingest | `sources/threedep_ept.py:185-216, 412` | PDAL standard mode, readers.ept → merge → writers.copc (not streamable) | O(points) ≈ 30–40 B/pt |
| Points → DEM | `dem/rasterize.py:47-84` | standard mode; writers.gdal full-AOI grids (count/min/max/mean/idw/stdev) | O(ground pts) + ~45 B/cell |
| DEM finalize | `dem/harmonize.py:72-116`, `dem/finalize.py:45-181` | full float64 reads; meshgrids; 2× EDT; fillnodata on the whole array | ~50–100 B/cell |
| Canopy CHM | `canopy/chm.py:82-326` | filters.outlier = KNN over all points (+ KD-tree); 9-array median stack | O(points) + ~72 B/cell |
| Season (S2) | `season/ndvi.py:164-271` | every matching scene held full-AOI float64, then `np.stack` + `nanmedian` | ~2 × N_scenes × 8 B/cell |
| Fuels | `fuels/lfps.py:64`, `fuels/normalize.py:65-98` | zip member read into RAM; full float64 destination per band | ~24 B/cell per band |
| Veg scatter | `veg/build.py:52-89`, `veg/scatter.py:81-129` | 4 full rasters + a Python object per instance (~150–200 B) before truncation | O(instances) × ~200 B |
| Tiling | `tiling/tiler.py:150` | whole-DEM read only for z-range; the rest is already per-tile | 4 B/cell |
| Exports | `mesh/core.py:76-111`, `veg/print_trees.py` | full surface mesh in float64/int64 + trimesh copies | ~100+ B/cell |

Scale check for a BIG GRASS-sized incident (97 × 106 km ≈ 10,000 km², ~100 M cells at 10 m, ~40×
Three Queens):

- **Points:** ~40 × 13 GB ≈ **500 GB** for LiDAR alone.
- **Season:** 2 × 20 scenes × 8 B × 100 M ≈ **32 GB**.
- **DEM finalize:** ~**10 GB**.

So 256 GB is not enough. OS paging is not a strategy either: whole-array numpy and PDAL access
patterns thrash, so a paged bake runs 10–100× slower and can still hit the commit limit. The fix
is **bounded working sets by design**, with the pagefile kept only as a safety net.

## 2. Strategy: chunked, out-of-core, resumable stages

**One rule for every stage:** process the AOI in **work chunks**, each a block of base-LOD tiles
from the existing `TileScheme`. A chunk carries a **halo** (extra margin) wide enough for the
stage's neighbourhood operations. Each chunk writes its result into a **windowed on-disk raster
or per-chunk file**, and the chunk is then released. Peak memory becomes
O(chunk + halo) × per-cell cost, independent of AOI size.

### 2.1 Shared machinery (new, small)

- **`WorkPlan`.** Chunks are `TileScheme` blocks sized from a memory budget
  (`[ingest] max_chunk_mb`, default e.g. 2048). A pre-flight estimator predicts per-stage peak
  memory from cells, points (EPT hierarchy counts) and scene count, then picks the chunk size or
  refuses clearly. No surprise OOMs.
- **Windowed-write helper.** Create the output COG/GeoTIFF up front at full extent, then write
  chunk windows into it (`ds.write(arr, window=...)`). Alternatively write per-chunk rasters and
  mosaic them with a VRT → `gdal.Translate` (both are bounded).
- **Resumability.** A chunk ledger (`<region>/.work/<stage>/<chunk>.done` + content hash) lets a
  killed bake resume at the next undone chunk instead of restarting. This also answers the
  memory-pressure kill we hit.
- **Optional parallelism.** Chunks are independent, so a process pool bounded by
  `budget / chunk_peak` workers turns the 256 GB machine into throughput, not risk.

### 2.2 Per-stage changes

| Stage | Chunked form | Halo | Notes |
|---|---|---|---|
| LiDAR ingest | readers.ept with chunk `bounds` (+ halo) → per-chunk COPC or LAZ (`execute_streaming` when there is no merge) | ~2 cells | Removes the O(AOI points) peak. Two resources → one reader per resource per chunk. Downstream reads per-chunk files (or a COPC per chunk + index). |
| Points → DEM | per-chunk readers.copc(bounds) → range/smrf → writers.gdal over chunk + halo; crop the halo; windowed write | ≥ idw radius; smrf window | Ground filtering near the chunk edge sees halo points, so seams match. |
| DEM finalize | harmonize per window (the geoid shift is per-pixel); composite and void fill per window | `feather_px`, `max_distance` | Windows larger than the largest void; flag voids that cross the halo for a second pass. |
| Canopy CHM | DSM per chunk (outlier KNN sees chunk + halo points only); CHM math per window | outlier radius; 1 px for pit-free | Output identical away from edges; halo makes edges identical too. |
| Season (S2) | per window: `WarpedVRT(..., window=)` per scene, float32, median per window | 0 | Memory ~2 × N × 4 B × window. Optionally cap N by best cloud cover. |
| Fuels | stream the unzip (`copyfileobj`); reproject per window into a window-sized destination | 0 (nearest) / 1 px (bilinear) | |
| Veg scatter | per chunk: windowed reads of cc/evt/height/dem; scatter the chunk's cells; append a structured array per chunk | 0 | **Keep the global seed and global (r, c) indices** (see §3). |
| Tiling | z-range from `ds.statistics()` / overviews / block accumulation | — | Everything else is already bounded. |
| Exports | STL/glTF from per-tile meshes, or chunk by `bed_size_mm`; always downsample glTF | — | Print product; lower priority. |

## 3. Conformance guard (vegetation)

The renderer's C++ scatter port reproduces Terrain's `instances.npy` exactly (Ember
`worldcore/tests/scatter_test.cpp`). Cells are independent: `cell_seed = hash64(tile_seed, r, c)`
over **global** canonical indices, with one `tile_seed = hash64(world_seed, layer_version,
base_lod, 0, 0)`. So chunked scatter that keeps the same `tile_seed` and global indices produces
**identical instances**, and only the within-list order changes (per chunk rather than
row-major). Do **not** switch to per-tile seeds as part of U6. That changes every placement, and
would need a `layer_version` bump and a coordinated port change. Also drop the whole-list
`max_instances` truncation semantics, or make it per chunk, and document the change.

## 4. Priority (largest peaks first)

1. **LiDAR ingest, points→DEM, CHM.** The O(points) stages; this is what killed Three Queens.
2. **Season.** N-scene stack; worst raster scaling.
3. **DEM finalize.**
4. **Veg scatter.** Python objects; also ~100× faster if vectorised per chunk.
5. **Fuels, tiling z-range.**
6. **Exports** (print product).

## 5. Acceptance

- **Flat memory:** on a fixed `max_chunk_mb`, peak RSS stays flat (±20%) across teanaway_dev
  (2 km²), Three Queens (238 km²) and a ≥ 2,000 km² AOI.
- **Equivalence:** chunked vs unchunked outputs on teanaway_dev and Three Queens:
  - DEM/derived/fuels/season: byte-identical, or within float tolerance where halos feather.
  - Scatter: **identical instance set**.
  - Ember conformance test: still exact.
- **Resumability:** kill a bake mid-stage; the re-run completes and skips done chunks.
- **Pre-flight:** the estimator's predicted peak is within 2× of measured, and the bake refuses
  (with the estimate) when the budget cannot be met at the minimum chunk size.

## 6. Meanwhile (until U6 lands)

- Bake one incident at a time on the runner, with nothing heavy alongside.
- For large fires, tighten the AOI buffer and/or use `points_per_cell = 2`.
- Keep a large pagefile as a crash cushion, not as the plan.
