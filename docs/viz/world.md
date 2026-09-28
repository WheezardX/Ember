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

* **Placement is Terrain's, exactly.** `worldcore/scatter.cpp` ports `terrain/veg` bit-for-bit:
  the golden vectors and all 74,595 teanaway_dev instances match `veg/instances.npy` (positions
  exact, float32 attributes bit-equal). Cells hash on global canonical indices, so each streamed
  tile is scattered alone from its own finest-LOD layer rasters (verified bit-identical to the
  canonical rasters) and the union equals Terrain's whole-AOI scatter.
* **Render policies (not conformance):** trees are grounded on the rendered surface
  (`SurfaceSampler`, bilinear over the mesh's corner heights) instead of Terrain's raw DEM-cell z,
  which floats/sinks by metres on slopes; conifer crowns are at least 0.3 × height because the
  palette's constant `radius_m` makes tall trees needles (HCP2 review item).
* **Meshes:** `assets/generators/veg_species.py` → one Nanite mesh per palette key
  (`/Game/Ember/Generated/Veg/SM_<key>`) + `M_Veg` (bark/foliage via vertex alpha). A key with no
  generated mesh falls back to an engine cone/sphere.
* **Streaming:** `AEmberVegetationActor` instances finest tiles within `veg_radius_m` of the
  camera, one HISM per species per tile. Facts: `instances.total`, `by_species`,
  `generated_species`, `ungrounded`, `scatter_ms`.

Measured (teanaway_dev, 1440p, RTX 4080 SUPER): terrain only p95 3.9 ms; with all 74,595 trees
(generated meshes) p95 6.3 ms, 954 MB VRAM.

## Scenario fields (render scenario v1 additions)

```toml
fixed_lod = 14            # omit to stream
lod_refine_factor = 1.5
look = "viz/looks/terrain_default.toml"   # or "clay"
vegetation = true
veg_radius_m = 1500

[[orbits]]                # review-bundle MP4 (not diffed); frames captured each tick
name = "orbit_oblique"
bookmark = "oblique_nw"   # start pose; yaw advances `degrees` over `frames`
degrees = 360
frames = 240
fps = 30
```
