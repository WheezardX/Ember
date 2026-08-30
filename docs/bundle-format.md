# Scenario bundle format

A **scenario bundle** ties an incident's immutable observations, derived products, weather,
and the pinned terrain world into one manifest. It is **self-contained**: given the store
directory, a bundle loads and replays with **zero network access**.

## Store layout
```
store/incidents/<id>/                 # <id> = id_to_dirname(incident_id)
  incident.json                       # IncidentRecord (metadata + size series)
  aoi.geojson                         # fire footprint + buffer (drives the world bake)
  observations/                       # IMMUTABLE, append-only, content-addressed
    perimeters/<ts>_<hash>.geojson
    hotspots/firms_<ts>_<hash>.geojson
    ir/nirops_<year>_<hash>.json
  derived/
    arrival_time.perimeter-interp-v1.cog.tif
    confidence.perimeter-interp-v1.cog.tif
  weather/
    timeline.v0.json                  # WeatherTimeline manifest (see below)
    grid.timeline.v0.npz              # HRRR gridded sidecar
    stations.timeline.v0.parquet      # RAWS station sidecar
  tiles/
    firestate.manifest.json           # D3 fire-state tiles index
    z{lod}/x/y/arrival_time.tif       # time-indexed fire-state tiles
  qa.html                             # F2 QA report
  package/scenario.bundle.json        # THE manifest
```
The baked terrain world lives beside it at `store/<world_region>/` (DEM + fuels).

## `scenario.bundle.json` (BundleManifest)
| field | meaning |
|---|---|
| `schema_version` | manifest version (1) |
| `incident_id` | IRWIN `{GUID}` (live) or `hist:<slug>-<year>` (historic) |
| `created_utc` | assemble time |
| `aoi_geojson` | store-relative AOI path |
| `world_region` / `world_manifest_hash` | the pinned terrain world (replay integrity; = terrain `RunResult.config_hash`) |
| `weather` | store-relative weather timeline manifest, or null |
| `observations[]` | ObservationEnvelope list (see below) |
| `derived` | `{arrival_time, confidence}` → store-relative COG paths |
| `provenance` | `arrival` stats, `perimeter_source`, `buffer_km`, `resolution_m`, `world`, `refresh` (`perimeters_total`/`perimeters_added`) |

### ObservationEnvelope
`kind` (`perimeter`|`hotspots`|`ir`|`sitrep`), `source` (`geomac-<year>`|`wfigs`|`firms`|
`nirops`), `observed_at`, `fetched_at`, `geometry_hash` (content hash — the dedup + append-only
key), `path` (store-relative), `attributes` (e.g. `acres`, hotspot `count`, IR `product_count`).
Observations are **immutable and append-only**: re-fetching an already-seen hash is a no-op;
new ones are appended (F1 refresh).

## WeatherTimeline (`weather/timeline.v0.json`)
Manifest + sidecars (see `ember/weather/schema.py`, adr/0007): `t0`, `step_minutes`,
`num_steps`, `variables` (`wind10_u`/`wind10_v`/`t2`/`rh2`/`precip`) with `units`, a
`grid` (GridSpec, UTM) → `grid_data` npz, `stations` (RAWS) → `station_data` parquet,
per-step `steps` provenance (`hrrr:anl` / `hrrr:anl+f01precip` / `missing`), and explicit
`gaps` (never silently interpolated).

## Regenerating views
`ember qa` (report) and `ember tiles` (fire-state tiles) read an existing bundle — no
re-fetch. `ember incident` re-run is an idempotent refresh.
