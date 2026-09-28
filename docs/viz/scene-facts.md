# Scene facts — `ember-scene-facts` v1 (EPIC_5_PLAN A2)

Written by `UEmberSceneFactsSubsystem` (`unreal/Ember/Source/Ember/Private/EmberSceneFacts.cpp`)
at every capture point (`facts/<capture>.json`), after the perf window (`facts/perf.json`),
and on demand via the console command `Ember.DumpFacts <path.json>`. Read by
`ember/dev/facts.py`; scenario `[[assert]]`s and `viz/budgets.toml` address fields by dotted
path (`tiles.loaded`, `perf.frame_ms_p95`).

Principle (plan §0 "probes before vibes"): anything countable is a fact, so neither an agent
nor a VLM ever has to count from pixels.

```jsonc
{
  "format": "ember-scene-facts", "version": 1,
  "scenario": "S_terrain_gray", "capture": "overview_n",   // "perf" for the perf window
  "frame": 35,                                             // frames since world start
  "time_utc": "2026-09-28T00:10:15.715Z",
  "camera": { "location_cm": [x, y, z], "rotation_pyr": [pitch, yaw, roll], "fov_deg": 55 },
  "world": {
    "region": "teanaway_dev", "crs": "EPSG:32610", "tile_px": 64,
    "anchor_m": [x, y, z],              // region-CRS metres placed at UE origin
    "extent_m": [minx, miny, maxx, maxy],       // tile grid (finest LOD)
    "data_extent_m": [minx, miny, maxx, maxy]   // valid data (bookmarks address this)
  },
  "tiles": {
    "total": 14, "loaded": 9, "triangles": 41472, "skirt_triangles": 5760,
    "nodata_corners": 16416, "load_ms": 8.4, "lod_histogram": { "14": 9 }
  },
  "instances": { "total": 0, "enabled": true, "tiles": 0, "radius_m": 4000,
                 "scatter_ms": 0.0, "ungrounded": 0, "no_surface": 0,
                 "near": 0, "near_radius_m": 500, "cells_near": 0, "tiles_near": 0,
                 "mid_culled": 0,
                 "generated_species": 5, "by_species": {} },  // vegetation (world.md)
  "environment": { "sun": "noon", "sun_azimuth_deg": 180, "sun_elevation_deg": 58,
                   "exposure_bias": -2 },
  "render": {
    "resolution": [2560, 1440], "rhi": "D3D12", "gpu": "NVIDIA GeForce RTX 4080 SUPER",
    "draw_calls": 85, "primitives": 237232,     // RHI counters, last frame
    "vram_mb": 902.6,                           // DXGI per-process local usage (Windows)
    "texture_mb": 483.2                         // RHI texture memory stats
  },
  "perf": {                                     // over the perf window, else trailing 120 frames
    "frames": 300, "frame_ms_avg": 3.74, "frame_ms_p50": 2.76, "frame_ms_p95": 3.49,
    "frame_ms_max": 274.7, "game_ms_avg": 1.94, "render_ms_avg": 2.93,
    "gpu_ms_avg": 2.47, "gpu_ms_p95": 3.00, "fps_avg": 267.3,
    "draw_calls_avg": 86.1, "primitives_avg": 237233,
    "frame_ms_series": [], "game_ms_series": [], "gpu_ms_series": []  // perf window only
  }
}
```

Notes
* `triangles` counts terrain surface triangles only; skirts are separate. For teanaway_dev at
  the finest LOD it equals `144 × 144 × 2` — the mesh covers exactly the valid AOI pixels.
* `vram_mb` is what the D8 budget (≤ 4 GB) is checked against. `-1` = unavailable.
* Frame timing comes from `FApp::GetDeltaTime`, `GGameThreadTime`, `GRenderThreadTime`,
  `RHIGetGPUFrameCycles`. With `-RenderOffscreen` there is no present/vsync, so frame time is
  the real cost.
* Adding a fact: extend `BuildFacts`, document it here, bump nothing (additive). Renaming or
  changing meaning bumps `version`.
