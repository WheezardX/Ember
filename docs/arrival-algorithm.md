# Arrival-time algorithm (`perimeter-interp-v1`)

The arrival-time raster is Ember's keystone derived product: per-cell **first-burn time**
(hours since t0) for a fire. It is an **interpretation algorithm** — a versioned, modeled
reconstruction layered over immutable perimeter observations — and it says so. Isochrones
are simply its contours.

## Inputs
- An ordered series of **cleaned, immutable perimeter observations** (GeoMAC for historic,
  WFIGS for live; cleaning + dedup in `arcgis.py`).
- Optionally, **FIRMS hotspots** for the AOI/window (live fires) for hotspot-assist.

## Grid
The AOI = final footprint + buffer, in the local UTM zone, at `resolution_m` (default
30 m). Perimeters are rasterized onto this grid.

## Algorithm
1. **Monotonic cumulative burning.** `cum_i = cum_{i-1} ∪ perimeter_i`; cells never
   un-burn (mapping regressions can't reduce the burned set).
2. **First snapshot** (i=0): every cell inside perimeter 0 gets `arrival = t0`,
   **confidence 1** (observed edge).
3. **Between snapshots** (t\_{i-1}, t\_i]: cells newly burned in this interval get arrival
   by **distance-transform interpolation** from the previous front — near the old edge →
   early, far → late — linearly between the two snapshot times. **Confidence 2**
   (interpolated).
4. **Hotspot-assist** (optional; live fires): within the mapped burned extent, a FIRMS
   detection means the cell was burning by its acquisition time, so where the interpolated
   arrival is *later* than a detection it is **pulled earlier** to the detection time, and
   the cell is marked **confidence 3** (satellite-observed). Detections outside the
   perimeters do **not** change the extent.

Deterministic given identical observations (golden-style test in CI).

## Outputs
- `derived/arrival_time.perimeter-interp-v1.cog.tif` — float32, hours since t0,
  NODATA = -9999 (unburned).
- `derived/confidence.perimeter-interp-v1.cog.tif` — uint8: **0** unburned, **1**
  observed-edge, **2** interpolated, **3** hotspot-inferred.
- Stats (in `bundle.provenance.arrival`): `t0`, `snapshots`, `duration_h`, `burned_cells`,
  `burned_km2`, `observed_frac` (fraction class 1 or 3), `hotspot_assist`, `hotspot_cells`,
  `arrival_h_range`.

"Fire state at time t" = the cells where `arrival_time <= t` (queryable tile-locally after
D3 fire-state tiling).

## Honest caveats
- **The extent is observed; the within-interval timing is modeled.** A low `observed_frac`
  (e.g. 0.01 for Jolly Mountain) means almost all timing is interpolated, not measured.
- **First perimeter = t0 everywhere inside it.** No earlier data exists, so the interior of
  the first mapped perimeter is assumed all-burned at t0.
- **Sharp isochrone boundaries** can appear between widely-spaced snapshots.
- **Historic fires get no hotspot-assist** — FIRMS NRT doesn't cover older years.
- **Live fires accrue their progression over time.** WFIGS returns one current perimeter
  per fetch; the series builds up through `--refresh` (append-only observations), so a
  freshly-discovered live fire may have a single day-0 blob until more perimeters publish.
