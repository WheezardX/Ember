# World rendering — terrain, look, vegetation (EPIC_5_PLAN C1–C4, B2, B3)

Everything the renderer draws of the world comes from a Terrain tile-store region
(`manifest.json` v2 + `tiles/z{lod}/x{col}/y{row}/*.tif`), read at runtime by the engine-free
`worldcore/` library that the UE module `EmberWorld` compiles. No per-AOI cooking (D3, D10).

## Terrain mesh (C2)

`worldcore/heightfield.cpp`. Vertices sit on content-pixel corners; a corner's height is the mean
of the four pixel centres around it (the apron supplies neighbours), so same-LOD tile edges are
bit-identical by construction and normals come from an apron ring (seamless shading). Quads
touching a corner with no valid pixel are dropped; skirts hide cracks between LODs. Frame:
UE X = east, Y = south, Z = up, cm, anchored at the tile-grid centre and `z_min`. Winding is UE's
(front face: `cross(p1-p0, p2-p0).z < 0` for up-facing triangles) — pinned by a unit test after
the first render showed only skirts.

## Streaming (C3)

`worldcore/lod.cpp: select_tiles` — from every coarsest tile, refine to children while the camera
is within `lod_refine_factor × tile span`; children Terrain skipped (all-nodata) are simply
absent. Tested: selections never overlap and cover every finest tile exactly once.
`AEmberTerrainActor` re-selects when the camera moves > 1 m and diffs sections in/out.
Scenario `fixed_lod = N` pins one level (fixtures keep exact tile/triangle asserts).

## Look (B2)

`viz/looks/*.toml` → `worldcore/look.cpp` → one albedo texture per tile → `M_Terrain`.
Per source pixel: FBFM40 class colour → NDVI greening (vegetated classes) → canopy darkening →
slope-rock blend. Then 4× supersampling with a world-aligned domain warp of the class lookup and
a validity-aware blur, so 10 m categories read as organic patches (texels across a tile edge
compose identically — tested) and a full CPU mip chain. Tuning the look is a TOML edit.
`look = "clay"` in a scenario uses the flat `M_EmberGray` instead (fixtures).

## Water (HCP1 follow-up; stopgap for upstream U7)

LiDAR drops returns over water and Terrain fills voids only up to 5 px, so lakes are DEM nodata:
holes in the mesh. `ember-dev water <region>` derives a water layer once per region into
Ember's store (`store/render/<region>/water/`; Ember never writes into Terrain's store):

* **bodies:** 8-connected FBFM40 water (98) plus DEM holes inside the AOI;
* **level:** 10th percentile of the body's **own** LiDAR water returns per 5-cell block over its
  3×3-block neighbourhood (~150 m), then filled and smoothed. The low percentile finds a
  reservoir's water line rather than its exposed drawdown ring (Kachess: 683.1 m against a
  689 m full pool). "Local" makes rivers follow their gradient (Cle Elum: 664 m at the lake,
  731 m upriver). Bodies with no returns take their shoreline's 10th percentile;
* **per tile, per LOD:** `water_level.tif` on the tile's apron grid.

The terrain mesh drops corners touching water to 1 m below the level (the lakebed closes holes
and never z-fights LiDAR water returns). A flat `M_Water` surface is drawn per water pixel.
Scenario field `water = true` (default) uses the layer when present. Three Queens: 51 bodies,
115 tiles; near-black hole pixels in the top-down view went from 1.68 % to 0. Known: 10 m
stair-step shorelines up close. U7 (hydro-flattening in Terrain) remains the data fix.

## Vegetation (C4, B3)

* **Placement is Terrain's, exactly.** `worldcore/scatter.cpp` ports `terrain/veg` scatter v2
  (Terrain ADR 0007: one species mix per LANDFIRE EVT, stand-structure crown classes, crowns
  scaled to height) bit-for-bit: the golden vectors and every teanaway_dev (24,814) and Three
  Queens (5,856,630) instance match `veg/instances.npy` (positions exact, float32 attributes
  bit-equal). Cells hash on global canonical indices, so each streamed
  tile is scattered alone from its own finest-LOD layer rasters (verified bit-identical to the
  canonical rasters) and the union equals Terrain's whole-AOI scatter.
* **Render policies (not conformance):** trees are grounded on the rendered surface
  (`SurfaceSampler`, bilinear over the mesh's corner heights) instead of Terrain's raw DEM-cell z,
  which floats/sinks by metres on slopes. Instances with no rendered surface under them (Terrain
  scatters over fuels outside the DEM's AOI mask, z = 0 - upstream U8) are dropped and counted
  (`instances.no_surface`); `instances.ungrounded` (a tile whose surface failed) must stay 0.
  Crowns are Terrain's (scatter v2 sizes them: 0.5 x (crown_base_m + crown_ratio x height));
  the mesh bounds are fitted to each instance's height and crown diameter.
* **Meshes (B3 v2):** `assets/generators/treegen.py` is a pure-Python growth-form model (trunk
  taper and flare, branch whorls, branch angle / droop / upturn, crown profile, drooping leaders,
  flat needle sprays, pine tufts, broadleaf branching and leaf clusters), one parameter set per
  palette species (15). Foliage is opaque geometry, not alpha cards. `veg_species.py` loads each
  species x `treegen.VARIANTS` (4) into Nanite meshes `SM_<key>_v<N>` (Preserve Area, no DF /
  Lumen cards) with a per-species material instance `MI_Veg_<key>` (foliage and bark colour).
  The renderer picks a variant per instance from a hash of its position. `M_Veg` is two-sided,
  Two-Sided Foliage shading (needles transmit light), per-instance tint. Bark is a flat colour
  until licensed bark scans arrive. Preview without UE: `treegen.build(key, variant)`; tests
  check determinism, triangle budget and foliage/wood flags. Long-term variation plan (modular
  crown sections): `D11-lod-world-context.md` §3.
* **Wind (HCP2):** `M_Veg` world-position offset - bend ∝ (height above the instance pivot)²,
  per-tree phase and a static per-tree lean from `PerInstanceRandom` (fixed
  `InstancingRandomSeed` per component), a slow gust band travelling downwind, and a fast small
  flutter on foliage (vertex alpha). The runtime owns the clock: stills freeze it (t = 0, so goldens
  stay deterministic), orbits/flyovers run t = frame / fps, the perf window runs real time.
  Tree components use `ShadowCacheInvalidationBehavior = Always` (moving WPO + cached shadow
  pages gave saw-toothed self-shadows).
* **Streaming and tiers:** `AEmberVegetationActor` scatters finest tiles within `veg_radius_m`
  of the camera and caches their trees; each tile's 4 x 4 cells are near (full meshes, wind, live
  shadows), mid (lite meshes, no wind, cached shadows, understory skipped) or unloaded, by
  distance - see `D11-lod-world-context.md` §2. One ISM per mesh per cell; Nanite culls. Trees
  are kept out of the distance-field scene (`bAffectDistanceFieldLighting = false`, ~0.9 GB VRAM
  at 1.45 M instances). Facts: `instances.total`, `near`, `cells_near`, `tiles_near`,
  `mid_culled`, `by_species`, `generated_species`, `ungrounded`, `no_surface`, `scatter_ms`.
* **Silhouettes:** `veg_lineup = true` replaces the scatter with one tree per species at its
  palette mid height in a row across the first capture's view, plus a 1.8 m post
  (`S_veg_lineup`).

**Rendering path.** The project targets **SM6** (`DefaultEngine.ini` `TargetedRHIs`). Until HCP2
it silently ran at SM5 - no Nanite, cascaded shadows - so HCP0/HCP1 perf numbers are SM5
numbers. Terrain captures were unchanged by the switch except low-sun self-shadowing, which
virtual shadow maps resolve better.

Measured (1440p, RTX 4080 SUPER):

| scene | instances | frame p50 / p95 | GPU | VRAM |
|---|---|---|---|---|
| teanaway_dev, SM5 raster trees (HCP1) | 74,595 | - / 6.3 ms | - | 954 MB |
| Three Queens `forest_oblique`, SM5 raster | 1,448,096 | 18.3 / 19.5 ms | 17.7 ms | 2.06 GB |
| same, SM6 + Nanite, DF on, pool 512 MB | 1,448,096 | 5.0 / 14.6 ms | 8.8 ms | 4.65 GB |
| same, final (ISM, no DF, pool 128 MB) | 1,448,096 | 4.2 / 10.0 ms | 8.3 ms | 3.2-3.4 GB |

Open: ~7 render-thread hitches of 35-180 ms per 300 frames with the forest loaded (waits on the
RHI/GPU fence inside `UpdatePrimitive`; absent without vegetation). Needs an Insights capture;
scheduled with the async tile-compose work before HCP6.

## Forest statistics (`ember-dev forest-report <region>`)

From Terrain's own scatter output (`veg/instances.npy`, the conformance oracle) and the rasters it
was scattered from: species shares vs palette weights, trees per cell by CC bin vs the accept
rule (`candidates_per_cell × CC`), per-species heights vs CHM, and the rendered crown cover
(Poisson overlap of crown discs per cell under the render crown policy) vs LANDFIRE CC. Writes
`runs/dev/forest/<region>/forest_report.{json,png}`.

## Scenario fields (render scenario v1 additions)

```toml
fixed_lod = 14            # omit to stream
lod_refine_factor = 1.5
look = "viz/looks/terrain_default.toml"   # or "clay"
vegetation = true
veg_radius_m = 1500
wind_strength = 6.0       # crown-top sway (cm) of a 10 m tree; grows with height^2
wind_from_deg = 270       # compass direction the wind blows from
veg_lineup = false        # silhouette sheet instead of the scatter
perf_exec_cmds = []       # console commands as the perf window opens (e.g. "ProfileGPU")

[[orbits]]                # review-bundle MP4 (not diffed); frames captured each tick
name = "orbit_oblique"
bookmark = "oblique_nw"   # start pose; yaw advances `degrees` over `frames`
degrees = 360
frames = 240
fps = 30                  # also the wind clock: t = frame / fps

[[orbits]]                # flyover: every pose field eased from bookmark to to_bookmark
name = "density_flyover"
bookmark = "fly_a"
to_bookmark = "fly_b"
frames = 450
```

The harness keeps every camera 3 m above the rendered surface. Flyovers fly a smoothed altitude:
the ground under the aim point and under the camera is sampled for every frame, averaged over
+-3 s (raised where needed to keep 30 m of clearance) and smoothed twice, so the camera holds
height like a drone instead of tracing the 10 m ground (HCP2: "jittery in the vertical").
Each orbit writes its per-frame camera path next to the MP4 (`orbits/<name>.camera.csv`).

Orbit video capture: the harness streams each frame's raw BGRA pixels over a pipe into one ffmpeg
per orbit, which encodes on the GPU (`h264_nvenc`, constant quality ~cq 18, yuv420p) straight to
`orbits/<name>.mp4` (`ember-dev` puts `ffmpeg` + `orbit_codec` in the plan; `libx264` if this
ffmpeg has no NVENC; PNG frames + encode afterwards if there is no ffmpeg). Stills stay lossless
PNG (they are diffed; orbits are not). Each orbit logs its timing, e.g. Jolly 1440p:
`70-95 ms/frame | screenshot wait ~45 ms | frame out 3 ms | place (fire+smoke+camera) 20-40 ms`
(was ~490 ms/frame with a PNG per frame, ~365 ms of it PNG compression). Next: an asynchronous
readback so the screenshot wait overlaps the next frame's render.
`perf.json` carries `frame_ms_series`, `game_ms_series` and `gpu_ms_series` for the window.
