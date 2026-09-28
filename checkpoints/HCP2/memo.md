# HCP2 — The forest is real

**Status:** awaiting Brad's sign-off (plan §5).
**Date:** 2026-09-28 · **Engine:** UE 5.8.3, now **SM6 + Nanite** · **Runner:** dev box, RTX 4080 SUPER
**World:** Three Queens 2026, 10 m, with Terrain's full scatter: 5,861,933 trees (3 conifers +
sagebrush + bunchgrass, `pnw_conifer` palette). teanaway_dev stays the fast fixture.
**What you're judging:** do density and heights read right to someone who's stood there? Is the
wind sway distracting? Does the budget hold with full instancing? (Conformance must be exact.)

## What to look at

| Item | File |
|---|---|
| Density flyover, low over the stand SW of the fire (15 s) | `orbits/density_flyover_720p.mp4` |
| Wind sway, close orbit (10 s; judge "distracting?") | `orbits/sway_close_720p.mp4` |
| Bookmarks: burn area from 2.2 km, eye level at a stand edge, forested ridge | `stills/*.jpg`, `runs/01-three-queens-forest/contact_sheet.png` |
| Species silhouette sheet (Douglas-fir, ponderosa, grand fir, sagebrush, bunchgrass, 1.8 m post) | `stills/lineup.jpg`, `stills/lineup_backlit.jpg` |
| Species / density / height / crown-cover stats | `forest_report.png`, `forest_report.json` |
| Scatter conformance (C++ vs Terrain) | `scatter_conformance.txt` |
| Perf, per frame, forest pose | `perf.csv` |

## Results

- **Conformance: exact.** The renderer's C++ scatter reproduces every Terrain instance - golden
  vectors, all 74,595 teanaway_dev trees and all **5,861,933** Three Queens trees (positions
  exact, attributes bit-equal) - scattered per streamed tile in ~2.3 s total.
- **Density follows the data exactly.** Trees per 10 m cell = 4 × LANDFIRE canopy cover in every
  CC bin (40/ha at CC 10 % up to 329/ha at CC 82 %); species shares match palette weights to 0.1 %.
- **Rendered canopy now matches LANDFIRE cover.** The crown-width render policy was making the
  forest read ~12 points denser than the data (+18 at CC 60-80 %). Calibrated: conifer crowns are
  0.23 × height (a 35 m fir gets an 8 m crown), rendered cover is within 0.4 points of LANDFIRE
  on average (`forest_report.png`, right panel).
- **Wind:** sway on every tree, bend ∝ height², per-tree phase, a slow gust band travelling
  downwind (WSW here). The harness owns the wind clock, so stills stay pixel-deterministic.
- **Budget with full instancing: met.** Forest pose, 1440p, **1,448,096 trees** instanced within 4 km:

  | Path | frame p50 / p95 | GPU | VRAM |
  |---|---|---|---|
  | as found (SM5, raster trees) | 18.3 / 19.5 ms | 17.7 ms | 2.06 GB |
  | **now (SM6, Nanite, tuned)** | **4.1 / 12.1 ms** | **7.9 ms** | **3.33 GB** |
  | terrain only, SM6 (HCP1 pose) | 3.6 / 6.4 ms | 3.9 ms | 2.65 GB |

## Findings (defects first)

1. **The renderer was not using Nanite (fixed).** Two silent defects since Phase 0:
   the content-free project never set `TargetedRHIs`, so UE ran at **SM5** (no Nanite, cascaded
   shadows); and the generated tree meshes had Nanite *disabled* (GeometryScript's option didn't
   stick). D5's premise was untrue until today. Both fixed; the generator now fails if Nanite is
   off. HCP0/HCP1 perf numbers were SM5 numbers - terrain captures were unchanged by the switch
   except low-sun self-shadowing (virtual shadow maps are sharper; two goldens re-blessed).
   SM6's shadow/Nanite infrastructure costs ~1.3 GB VRAM before any trees.
2. **Hitches with the forest loaded (open).** p95 is under budget, but ~13 frames in 300 spike to
   35-160 ms: the render thread waits on the GPU/RHI fence inside primitive updates. Absent
   without vegetation. Needs an Unreal Insights capture; scheduled with the async tile-compose
   work before HCP6 (where interactive feel is judged). Captures and MP4s are unaffected.
3. **Trees are toys up close (B3 v1, as expected).** The lineup shows it plainly: firs are stacked
   cone "lampshades", ponderosa is a blob crown on a pole, Douglas-fir and grand fir are nearly
   indistinguishable, crowns are solid (so stand interiors are dark and eye-level views inside a
   dense stand go black). From 1 km+ the forest reads right; below ~200 m it reads as a model.
   This is your "eventually render actual trees" item - see question 3.
4. **Heights are canopy-top heights (upstream question U9).** Every tree takes its 10 m cell's
   CHM height ±15 %: all three conifers average 33-34 m, there is no understory, and the
   species-max re-clamp piles 270 k ponderosa at exactly 45 m (spike in the middle panel).
   Real stands mix dominant, co-dominant and suppressed trees. That's a Terrain scatter-spec
   change, not a renderer fix - filed as a question.
5. **Trees outside the AOI (upstream U8).** Terrain scatters over fuels outside the DEM's AOI
   mask and gives those 24,074 trees (0.4 %) z = 0. The renderer drops them
   (`instances.no_surface`) instead of drawing them underground.
6. **Near/far split.** Trees exist within 4 km of the camera; past that the terrain's canopy
   colouring carries the forest. At the review poses the seam isn't visible; in very wide views
   the trees simply aren't there. Belongs to D11 (the LOD / world-context plan: a mid-field
   impostor tier).

## What changed (for the record)

SM6 target; Nanite on tree meshes; ISM instead of HISM (4× faster tile loads under Nanite);
trees out of the distance-field scene and no mesh DF/cards (−0.9 GB, and runtime DF builds had
been stalling the render thread); Nanite streaming pool 128 MB (−0.35 GB); wind WPO with a
harness-owned clock; VSM invalidation for swaying trees; crown ratio 0.30 → 0.23; no-surface
drop; flyover camera paths (`to_bookmark`); species lineup mode; `perf_exec_cmds`; per-frame perf
series; `ember-dev forest-report`. Scenarios `S_tq_forest`, `S_veg_lineup`; all nine scenarios
pass; pytest 84 passed, worldcore 27 cases.

## Questions for sign-off

1. **Density and heights** - from the flyover and oblique, does it read like this country?
   Should I pursue U9 (stand structure under the canopy) with Terrain, or is canopy-top fine
   for the product's viewing distances?
2. **Wind** - is the sway (strength 6: the top of a 33 m tree moves ~0.4-0.85 m, gusting) distracting,
   too little, or right?
3. **Trees up close** - the primitives hold from altitude. Do you want a B3 v2 before HCP3
   (procedural branch/foliage-card trees, still generated from code), or park it until the
   camera work (HCP6) makes ground level matter?
4. **Budget** - 3.33 GB of the 4 GB with 1.45 M trees. OK to keep D8 at 4 GB and treat the 4 km
   tree radius as the knob?
