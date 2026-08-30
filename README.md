# Ember

[![CI](https://github.com/WheezardX/Ember/actions/workflows/ci.yml/badge.svg)](https://github.com/WheezardX/Ember/actions/workflows/ci.yml)

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

Epic 3 (fire data) is essentially complete: historic **and** live incidents assemble into
scenario bundles with terrain/fuels, an arrival-time raster, FIRMS hotspots, NIROPS IR,
and a HRRR + RAWS weather timeline; idempotent refresh, fire-state tiles, and a QA report.

## Commands

```
ember incident --historic jolly-mountain-2017   # historic fire → bundle
ember incident --irwin "{GUID}"                  # live fire (WFIGS) → bundle
    [--no-bake] [--no-enrich] [--weather [--weather-hours N]] [--dry-run]
ember qa       --historic jolly-mountain-2017    # qa.html (arrival heatmap + inventory)
ember tiles    --historic jolly-mountain-2017    # time-indexed fire-state tiles
```

`incident` fetches perimeters → immutable observations → arrival-time raster → a coarse
DEM + LANDFIRE fuels bake → a scenario bundle; enrichment adds FIRMS hotspots (live) +
NIROPS IR (best-effort), and `--weather` a HRRR/RAWS timeline. Re-running is an idempotent
refresh (observations are append-only). Outputs live under `store/` (git-ignored).

## Docs
- `docs/arrival-algorithm.md` — how the arrival raster is derived (+ its honest caveats)
- `docs/bundle-format.md` — scenario bundle + store layout reference
- `docs/CREDENTIALS.md` — API keys (FIRMS, Synoptic): where to generate + rotate
- `docs/flagships/jolly-mountain-2017.md` — flagship QA memo
- `EPIC_3_PLAN.md` (plan) · `WILDFIRE_DESIGN.md` (product design) · `adr/` (decisions)

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

### API keys

Some feeds need free API keys (FIRMS hotspots, Synoptic RAWS). Copy
`.secrets.toml.example` to `.secrets.toml` (git-ignored) and fill them in — see
**`docs/CREDENTIALS.md`** for where each key is generated and how to rotate it. A
missing key just skips that feed; the rest of the bundle still builds. WFIGS, NIROPS,
and HRRR need no key.

## Tests

```
pytest -q
```

`geo`/`network`/`slow` tests auto-skip when the toolchain or `TERRAIN_RUN_NETWORK=1`
is absent, so a fresh checkout runs green before provisioning.
