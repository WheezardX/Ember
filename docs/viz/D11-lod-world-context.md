# D11 — LOD, vegetation tiers and world context (plan)

**Status:** draft for review (2026-09-28). **Owner:** Epic 5. **Inputs:** HCP1 review (Google-
Maps-like LOD, world continues past the AOI), HCP2 review round 1 (budget must leave room for
fire, smoke and weather; tree repetition; modular trees).

## 1. Frame and memory budget (D8: 60 fps at 1440p, 4 GB VRAM, RTX 4080 SUPER)

Every system gets a line; `ember-dev evaluate` checks each against facts once the system exists.

| System | GPU ms | VRAM |
|---|---|---|
| Engine base (SM6: Nanite, VSM, Lumen pools) | — | 1.2 GB |
| Terrain (meshes, albedo, streaming) | 3.0 | 0.8 GB |
| Vegetation (all tiers) | 3.0 | 0.6 GB |
| Fire + smoke + embers (Niagara, D2–D4) | 4.0 | 0.6 GB |
| Sky, clouds, weather | 2.0 | 0.3 GB |
| Post / TSR / UI overlays | 1.5 | — |
| Headroom | 3.2 | 0.5 GB |
| **Total** | **16.7** | **4.0 GB** |

Measured today (HCP2 forest pose): terrain ~3.9 ms, vegetation ~4 ms and 0.7 GB with every tree a
full mesh to 4 km — over its line, hence the tiers below.

## 2. Vegetation tiers (implemented 2026-09-28, HCP2 round 2)

Each streamed 1.28 km tile is split into 4 x 4 cells (320 m); each cell picks its tier from its
distance to the camera (horizontal distance plus the camera's height above ground). A cell whose
tier changes is rebuilt from the tile's cached trees (no re-scatter).

| Tier | Distance | Representation | Wind | Shadows |
|---|---|---|---|---|
| **Near** | 0 – 500 m (`veg_near_radius_m`) | full growth-form meshes | sway + flutter, per instance within 250 m | live (VSM invalidated each frame) |
| **Mid** | 500 m – 4 km (`veg_radius_m`) | lite meshes: the same trees (same seeds, same branches) with ~45 % of the foliage; understory < 12 m skipped | off | cached |
| **Far** | 4 km + | no instances: the look's canopy colouring in the terrain albedo | — | terrain's |

Engine settings that go with it (DefaultEngine.ini): `r.Nanite.MaxPixelsPerEdge=2`,
`r.Shadow.Virtual.ResolutionLodBiasDirectional=1` - dense foliage is Nanite's software-raster
worst case; no visible change at 1440p.

Measured (Three Queens, 1440p, RTX 4080 SUPER):

| Pose | Before tiers | After |
|---|---|---|
| aerial oblique (2.2 km out) | GPU 12.0 ms, 3.5 GB | GPU 5.9 ms, p95 11.4 ms, 3.3 GB |
| low flyover (120 m over the canopy) | GPU 22.8 ms | GPU 7.8 ms, p95 10.3 ms, 3.3 GB |

Tried and rejected: Nanite `VOXELIZE` shape preservation (UE 5.8, experimental) - 35 % cheaper
but distant trees lost their foliage (bare trunks). Still open: dithered cross-fade at cell tier
edges; octahedral impostors if the mid tier needs to go further out; canopy height in far terrain
LODs; the render-thread hitches (~12 per 300 frames, 35–125 ms, RHI fence waits).

## 3. Tree variation and tree source (open - Brad to choose after the HCP2 review)

Today: 4 whole-tree variants per species generated from code (`treegen.VARIANTS`), a lite
(~45 % foliage) copy of each for the mid tier, picked per instance by a position hash, plus
per-instance height, crown width, yaw, tint and lean.

Options, not decided (Brad, 2026-09-28: the banded idea "was just a suggestion"):

* **More generated variants** (8–16 per species): cheapest; Nanite memory grows ~linearly.
* **Modular crown sections** (trunk + 2–3 bands x 4–5 variants, each band randomly rotated):
  125+ combinations per species from ~15 meshes; 3–4 instances per near tree. UE 5.7+'s
  experimental Nanite Assemblies target this pattern.
* **SpeedTree / purchased library trees** (B4 escape hatch + ADR): photoreal fastest; SpeedTree 9
  exports Nanite-ready geometry foliage with WPO wind; licence terms to confirm. The pipeline
  (placement, tiers, variants, budgets) is independent of where meshes come from.

## 4. World context (HCP1: "Google-Maps-like LOD")

* The terrain quadtree extends past the incident AOI: AOI tiles at 10 m (LiDAR), then a surround
  ring at 30 m (3DEP 1 arc-second) and 90 m beyond, each ring its own Terrain region with the same
  manifest/tile format, stitched by skirts at ring edges (fixes the region-edge "diorama cliff").
* Land cover in the surround: LANDFIRE FBFM40/EVT at 30 m through the same look; no trees
  instanced outside the AOI (far tier canopy only).
* Continuous zoom: the streaming manager already selects LOD by distance; the surround adds
  coarser root tiles. Past ~100 km a cartographic basemap (Epic 9 CDN tiles) takes over.
* Streaming: tile compose/mesh on worker threads (HCP1 finding 3) before any of this ships.

## 5. Order of work

1. B3 v2 near-tier trees (in progress) → 2. near/mid/far vegetation tiers + per-system budget
checks → 3. async tile compose → 4. surround rings (30 m / 90 m) → 5. modular near trees →
6. cartographic far field with Epic 9.
