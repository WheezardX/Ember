# Flagship QA — Jolly Mountain 2017

**Status:** first flagship (Epic 3 E3). Regenerated 2026-08-30 with the current pipeline.
Validation is against **published NIFC/GeoMAC/InciWeb records**, not personal recall (the
fire postdates the operator's field experience), per the epic's honesty rule.

## What's in the package
`ember incident --historic jolly-mountain-2017` produces, under
`store/incidents/hist-jolly-mountain-2017/`:

- **35 immutable perimeter observations** (GeoMAC `Historic_Geomac_Perimeters_2017`,
  cleaned + deduped), Aug 12 → Sep 16 2017.
- **Arrival-time raster** (`derived/arrival_time.perimeter-interp-v1.cog.tif`) + confidence.
- **Baked world**: Copernicus GLO-30 DEM (EGM2008→NAVD88 harmonized) + LANDFIRE fuels
  (fbfm40, cc, ch, cbh, cbd, evt) at 30 m, pinned into the bundle (`world_manifest_hash`).
- **Fire-state tiles** (D3) + **QA report** (`qa.html`, F2).

## Validation vs published record

| Metric | Ember | Published | Verdict |
|---|---|---|---|
| Final size | **150.0 km²** (166,647 cells × 30 m) | 36,808 ac = **148.96 km²** | ✅ +0.7% |
| Perimeter count | 35 snapshots | day-by-day GeoMAC series | ✅ |
| Ignition / t0 | 2017-08-12 | discovered ~Aug 11 2017 | ✅ within a day |
| Duration | 841.6 h (~35 d) | mid-Aug → containment mid/late Sep | ✅ |
| Location | N of the Teanaway, Okanogan-Wenatchee NF, WA (UTM 10N) | Cle Elum R.D., WA | ✅ |

Burned area matches the documented footprint to <1%, the progression spans the documented
dates, and the AOI lands where it should. The arrival raster is deterministic given the
same perimeters (golden-style test in CI).

## Honest caveats (arrival v1 — `perimeter-interp-v1`)
- **`observed_frac` = 0.01.** Only the first perimeter's cells are "observed" (confidence
  class 1); ~99% of the footprint's *timing* is **interpolated** between perimeter
  snapshots (distance-transform from the previous front). The *extent* is observed; the
  *within-interval timing* is a modeled interpretation.
- **First perimeter = t0 everywhere inside it.** The interior of the first mapped
  perimeter is assumed all-burned at t0 (no earlier data). For Jolly the first perimeter
  is small (~1% of final), so the impact is limited, but it is an assumption.
- **Sharp isochrone boundaries** can appear between widely-spaced snapshots.
- **No hotspot-assist**: FIRMS NRT does not cover 2017, so satellite detections don't
  refine the timing here (they will for live fires — B3 exists).
- **NIROPS**: no 2017 products in the public archive (recent years only) — absent, expected.
- **Weather**: not attached — a 35-day hourly HRRR+RAWS timeline (~840 steps) is
  impractical to bake wholesale. A bounded window over the documented wind-driven run
  days is the right cross-check (follow-up).

## Regenerate
```
ember incident --historic jolly-mountain-2017   # perimeters → arrival → world bake → bundle
ember qa       --historic jolly-mountain-2017   # qa.html (arrival heatmap + growth + inventory)
ember tiles    --historic jolly-mountain-2017   # time-indexed fire-state tiles
```
Needs `TERRAIN_RUN_NETWORK=1` and `PROJ_NETWORK=ON` (datum harmonization). Outputs live
under `store/` (git-ignored). Bundle: `.../package/scenario.bundle.json`.

## Verdict
The Jolly Mountain arrival raster is a **faithful reconstruction of the mapped extent and
its published growth timeline**, with timing between snapshots explicitly modeled (and
flagged as such). Good enough to be 4.3's first playback input and 5.5's first demo; the
timing fidelity improves with hotspot-assist and denser observations.
