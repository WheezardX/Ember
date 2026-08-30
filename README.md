# Ember

Project Ember — the wildfire product. It turns fire incidents (historic and, later,
live) into replayable scenario bundles: immutable perimeter/hotspot observations, a
derived **arrival-time raster** (per-cell first-burn time), and a coarse baked world
(DEM + LANDFIRE fuels) for the fire's footprint.

Ember is built **on top of** the [`terrain`](https://github.com/WheezardX/Terrain)
world-data engine. The dependency is strictly one-way — ember imports terrain, never
the reverse — so terrain stays a standalone engine (also the "model your land"
physical-print product) and ember owns everything wildfire-specific. Ember calls
`terrain.run_pipeline` to bake terrain/fuels for an incident AOI;
`ember/incidents/bake.py` is the one seam. See `adr/0005` and `adr/0006`.

## Status

Epic 3, **Phase 1** — one historic fire end-to-end:

```
ember incident --historic jolly-mountain-2017
```

fetches GeoMAC perimeters → immutable observations → arrival-time raster → a coarse
DEM + LANDFIRE fuels bake for the fire AOI → a scenario bundle. `--no-bake` skips
the world bake for the fast arrival-only path. `--irwin` (live fires) is Phase 2.

See `EPIC_3_PLAN.md` for the plan and `WILDFIRE_DESIGN.md` for the product design.

## Setup

Ember shares terrain's geospatial toolchain (PDAL/GDAL/rasterio via conda-forge).
The simplest setup reuses Terrain's existing `terrain` conda env, with both packages
installed editable side-by-side:

```
git clone https://github.com/WheezardX/Terrain.git   # sibling of this repo
git clone https://github.com/WheezardX/Ember.git
conda activate terrain            # already has the geospatial stack
pip install -e ../Terrain         # the engine
pip install -e .                  # ember
```

Or create a standalone env from `environment.yml` (`conda env create -f environment.yml`),
then run the same two `pip install -e` commands into it.

`pyproject.toml` declares `terrain` as a git dependency for reproducible external
builds; an editable install already present in the environment satisfies it.

## Tests

```
pytest -q
```

`geo`/`network`/`slow` tests auto-skip when the toolchain or `TERRAIN_RUN_NETWORK=1`
is absent, so a fresh checkout runs green before provisioning.
