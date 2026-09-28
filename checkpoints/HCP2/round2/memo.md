# HCP2 round 2 — The forest is real (comprehensive review)

**Status:** awaiting Brad's review and sign-off (plan §5).
**Date:** 2026-09-28 · **Engine:** UE 5.8.3, SM6 + Nanite · **Runner:** dev box, RTX 4080 SUPER
**World:** Three Queens 2026, 10 m. Terrain commit `e6044a9` (scatter v2); Ember `75d596b`,
`e619c15`, `f210d8c` (all local, not pushed).

## Your round-1 feedback and what changed

| You said | What changed | Evidence |
|---|---|---|
| Too dense | Stand structure: every tree draws a crown class (dominant / co-dominant / intermediate / suppressed) and a height under the canopy top; crowns scale with tree size. Mean height 34 → 23 m, real understory. Rendered canopy cover tracks LANDFIRE (50 % where it says 50 %, 82 % where it says 82 %). | `forest_report.png`, `stills/low_flyover.jpg`, `stills/ridge.jpg` |
| Species mix off; pine should be rare | One species mix per LANDFIRE forest type. Three Queens: western hemlock 26 %, Pacific silver fir 24 %, Douglas-fir 17 %, mountain hemlock 12 %, western redcedar 8 %, **ponderosa 1.2 %**; alder / cottonwood in riparian strips, vine maple in shrubland. | `forest_report.json`; Terrain ADR 0007 |
| Couldn't see the wind | Locked-off 10 s clip (fixed camera, sun behind it, crowns against the sky); twig flutter added on top of the sway. | `orbits/wind_locked_720p.mp4` |
| Close-up trees cartoonish — fix before moving on | 15 species modelled from their growth forms (trunk taper and flare, branch whorls, droop, nodding hemlock leaders, flat needle sprays, pine tufts, broadleaf crowns), 4 variants each, foliage as real geometry with light passing through needles. Bark is a flat colour until you have licensed scans. | `stills/lineup.jpg`, `stills/wind_edge.jpg`, `species_sheet.png` |
| Radius can't be the only budget lever; explain the LOD plan | Vegetation tiers (near / mid / far) by 320 m cells + Nanite foliage settings; written plan D11 (tiers, budget table, world-context rings). | `docs/viz/D11-lod-world-context.md`, perf below |

Still exact: the renderer reproduces every Terrain tree (24,814 teanaway, 5,856,630 Three Queens)
— `scatter_conformance.txt`.

## What to look at

| Item | File |
|---|---|
| Wind, locked-off camera | `orbits/wind_locked_720p.mp4` |
| Low flyover over the stand SW of the fire | `orbits/density_flyover_720p.mp4` |
| Close orbit | `orbits/sway_close_720p.mp4` |
| Burn area from 2.2 km, forested ridge, eye level at a stand edge, low over the canopy | `stills/forest_oblique.jpg`, `ridge.jpg`, `ground_forest.jpg`, `low_flyover.jpg` |
| Species lineup on Cooper Lake (lit and backlit), species sheet | `stills/lineup*.jpg`, `species_sheet.png` |
| Teanaway fixture (drier east-side forest, pine present) | `stills/teanaway_*.jpg` |
| Stats | `forest_report.png` |

## Budget (60 fps @ 1440p = 16.7 ms, 4 GB VRAM)

| Pose | Round 1 (primitive trees) | Full new trees, no tiers | **Now** |
|---|---|---|---|
| Aerial oblique, 2.2 km out | GPU 7.9 ms, 3.3 GB | GPU 12.0 ms, 3.5 GB | **GPU 5.7 ms, p95 8.7 ms, 3.4 GB** |
| Low flyover, 120 m over the canopy | — | GPU 22.8 ms | **GPU 7.8 ms, p95 10.3 ms, 3.3 GB** |

How: trees within 500 m draw in full with wind and live shadows; 500 m–4 km draw a lite copy of
the same tree (same branches, ~45 % of the foliage) with no wind, cached shadows, and no
understory; past 4 km the terrain colouring carries the forest. Plus two Nanite settings
(2-pixel triangles, half-resolution sun shadows) that were invisible at 1440p. Tried and rejected:
Nanite's experimental voxel foliage (cheaper, but distant trees went bald).

## Findings / known issues

1. **The scenes read dark.** Exposure has been fixed at −2 EV since HCP0 (tuned on bare terrain);
   dense conifer canopy is dark by nature, and together forests look like late evening. A
   lighting/exposure pass is cheap but changes every golden — your call (question 2).
2. **Stutter.** ~12 frames in 300 spike to 35–125 ms (render thread waiting on the GPU), with or
   without the new trees. p95 is inside budget, but you would feel it when flying the camera.
   Needs a profiler trace; scheduled before HCP6 with async tile loading.
3. **Tier edges pop.** When a 320 m cell switches near ↔ lite while the camera moves, foliage
   density steps. A dithered cross-fade is in the plan, not built.
4. **Forest ends at 4 km.** Past that, trees are terrain colour only; distant ridgelines lack a
   treed silhouette. D11: canopy height into far terrain + the surround rings.
5. **Bark is a placeholder colour** until licensed scans arrive.
6. **Upstream U8 still open:** 23,771 trees outside the AOI's DEM mask (dropped by the renderer).

## Decisions for you

1. **Tree source going forward** (D11 §3): (a) more generated variants, (b) modular crown
   sections, or (c) SpeedTree / library trees under an ADR. Today's pipeline works with any.
2. **Exposure / lighting pass** now (brighter forests; all goldens re-blessed), or after the fire
   visuals exist?
3. **Density** — does the low flyover now read like this country?
4. **Wind** — right amount now that you can see it?
5. **D11 plan** — agree with the tiers, the per-system budget table and the world-context rings?
