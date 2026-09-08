# CP1 — *The world loads* (2026-09-07)

**Gate:** layers align pixel-perfect; fuel values plausible in SI; load time < 5 s; discrepancies
filed upstream. **Result: PASS with three upstream findings** (below). This is the first hostile
read of Epic 1–3 data by a consumer that needs all of it at once.

## What was produced
`ember sim export --historic jolly-mountain-2017 --out store/sim` → `store/sim/hist-jolly-mountain-2017.ewp/`
(world pack v1, `docs/sim/formats.md` §1), then `ember sim static … --out checkpoints/CP1`:

| artifact | what to look for |
|---|---|
| `hillshade.png`, `elevation_cm.png` | Teanaway terrain; Cle Elum Lake bottom-left; the AOI is a rotated rectangle so corners are nodata |
| `fbfm40.png` | Scott & Burgan classes tinted (TU/TL greens dominate; GS/GR on south aspects; NB grey = lake, rock, roads) |
| `cc_pct.png`, `ch_dm.png`, `cbh_dm.png`, `cbd_gm3.png` | canopy metrics in SI (%, dm, dm, g/m³) |
| `evt.png` | EVT codes (informational) |
| `arrival_s.png`, `confidence.png` | Epic 3 arrival raster **resampled onto the world grid**, drawn over hillshade |
| `stats.md` | per-layer min/max/valid/nodata + grid + hashes |

Timing: bundle → pack export **1.3 s**; static renders 0.6 s. Gate (< 5 s) met with margin.

## Alignment
The world layers (DEM, derived, six fuels) are all 800×627 @ 30 m, EPSG:32610, origin
(640781.5, 5254513.3): pixel-identical grids. The Epic 3 **arrival/confidence rasters are not
on that grid** (847×599, origin 640065.0, 5253986.3 — a 716.5 m / 527 m offset, not a multiple
of 30 m). The exporter resamples them (nearest) and records the source grid in
`world.json → arrival.resampled_from_grid`. All 166,647 burned cells landed inside the world
grid, so nothing was clipped. Downstream (CP2/CP5) comparisons are therefore on the world
grid, with up to half a cell of positional error from the resample.

## Plausibility (SI)
- Elevation 1.2 m … 2132 m. The 1st percentile is 679 m; the minimum comes from **1,223 cells in
  the right-edge strip (columns 783–799)** — an edge artifact, not terrain (see U2).
- fbfm40 91–202, all cells classified: TU 293k · TL 70k · GS 56k · NB 47k · SH 27k · GR 8k · SB 46.
  Reads as Teanaway/Cle Elum forest with grass-shrub on dry aspects. Plausible.
- Canopy: cc 0–84 %, ch 0.1–39 m, cbh 0–10 m, cbd 0–0.33 kg/m³ — the right ranges for the
  LANDFIRE products after the Epic 2 scale factors (ch/cbh ÷10, cbd ÷100). Units asserted at
  export from `fuels.provenance.json`.
- Arrival 0 … 841.6 h (35 days), 166,647 cells = 150.0 km² (Epic 3 flagship memo: 148.96 km²
  published). Confidence classes 1–2 only (no hotspot assist for 2017 — expected).
- Weather: **none** — the bundle has `weather: null`.

## Upstream findings (Epic 1–3 tickets; logged here per plan §5)
- **U1 (Epic 3, arrival.py):** derive the arrival/confidence rasters **on the baked world grid**
  (same origin/dims as the DEM) instead of the AOI-bbox grid, so consumers do not resample.
  Until then the exporter's nearest resample stands and is recorded in the pack manifest.
- **U2 (Epic 1, DEM finalize/harmonize):** the Copernicus DEM has a 17-column strip on the
  east edge with elevations 1–300 m (1,223 cells) where real terrain is 700 m+. Likely a
  mosaic/reprojection edge. The sim treats these as terrain (they are burnable fuel over a
  cliff); they should become nodata or be filled.
- **U3 (Epic 2, fuels normalize):** 81,860 canopy-layer nodata cells (16 %), of which **17,206
  are TU/TL timber cells** with cc = 0 after export. LANDFIRE "non-forest" zeros and true nodata
  are being conflated. The CA treats missing canopy as no crown (cannot crown-run there).
- **U4 (Epic 3, weather):** Jolly Mountain has no weather timeline; CP5 needs a bounded window
  (`ember incident --historic jolly-mountain-2017 --weather --weather-hours N`), per the
  flagship memo's own follow-up.
- **U5 (Epic 1, story 1.2):** no structures layer exists. The observer reports
  `structures = -1` (n/a) rather than zeros until one lands.
- Noted, not a ticket: 3,917 burned cells sit on NB fuel (roads, rock, water crossed by mapped
  perimeters). Playback shows them; the CA can never ignite them — a known, honest difference.

## Verdict
The Epic 1–3 data is sim-readable and coherent enough to build on: the world pack loads in
well under the budget, layers are pixel-aligned among themselves, values are in SI ranges, and
the arrival raster overlays the terrain where it should. The five findings above are the plan
doing its second job; none blocks CP2.
