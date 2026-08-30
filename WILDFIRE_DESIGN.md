# Project Ember — Design Document
*Working title. A wildfire visualization pipeline with a strategy game and a broadcast/research product built on top.*

**Status:** Draft v0.1
**Owner:** Brad / BNE Games LLC
**Granularity:** Epics and stories with guidelines. No task breakdown yet.

---

## 1. Vision

One data spine, three frontends:

1. **The Game** — A national-scale wildfire resource strategy sim. You run NIFC: assign crews, contract air tankers, triage competing fires, invest in the off-season. Play procedural seasons or replay historic ones.
2. **The Visualizer** — Automated 3D rendering of real, live fires from public data feeds. Target customers: news organizations, emergency managers, public information officers.
3. **The Research Surface** — Not a fire model. A pluggable sim interface so researchers can drive the visualizer and game world with their own models (ELMFIRE, FARSITE outputs, custom). We build presentation and orchestration; they bring the science.

### Design principles (non-negotiable)

- **Sim core is headless, deterministic, and pure.** The core contains *two* coupled simulations: the fire model and the suppression model, exchanging state through a defined delta stream. Sim = f(terrain, fuels, weather timeline, ignitions, player/AI commands, model params, seed). Rendering and gameplay *UI* are consumers; gameplay *commands* (resource assignments, tactics) are recorded inputs to the suppression sim, so a replay of commands re-simulates identically.
- **The fire model is a plugin.** The game ships a default model tuned for legibility and fun. The interface it implements is public and versioned. Anything that implements it — including replayed external data — drives the world identically.
- **Everything is a data pack.** Regions, fuel behaviors, assets, scenarios. First-party DLC and community mods use the same mechanism. Australia is not a special case; it's a pack we didn't make.
- **Replays are a file format, not a feature.** Any run is serializable and re-renderable bit-identically. Historic seasons, mod scenarios, research case studies, and bug reports are all "just replays."
- **Scale by proving small.** Every pipeline works on a targeted AOI first (suggest: Kittitas County / Teanaway — home turf, known LiDAR coverage, real fire history) before state → region → CONUS → international.

### Explicit non-goals

- We are not building a validated fire behavior model. Ever.
- We are not an operational decision tool. The visualizer informs and communicates; it does not predict for tactical use. (Legal/liability posture matters here — see Risks.)
- Photoreal per-tree world reconstruction. We target *plausible density and structure*, not botanical truth.

---

## 2. Architecture Overview (context for the epics)

```
┌─────────────────────────────────────────────────────┐
│  Frontends:   Game (UE)  │  Visualizer  │  Headless │
├─────────────────────────────────────────────────────┤
│  Sim Orchestrator: time control, replay, ensembles  │
├─────────────────────────────────────────────────────┤
│  Fire Model Interface (versioned plugin API)        │
│    ├─ Default game model (cellular, GPU)            │
│    ├─ External driver (ingest ELMFIRE/FARSITE out)  │
│    └─ Community/research models                     │
├─────────────────────────────────────────────────────┤
│  World Data Layer: terrain, fuels, weather, assets  │
│    (all content addressed as Data Packs)            │
├────────────────────────┬────────────────────────────┤
│  Runtime tiles via CDN │  Live delta feed (fire/wx) │
├────────────────────────┴────────────────────────────┤
│  HOSTED BAKE SERVICE (Epic 9): scheduled rescans,   │
│  bake to runtime format, versioned manifests        │
├─────────────────────────────────────────────────────┤
│  Ingestion Pipelines: GIS / fuels / live fire / wx  │
└─────────────────────────────────────────────────────┘
   Clients never touch source APIs directly.
```

---

## Epic 1 — GIS Terrain Pipeline

*Reuses and generalizes the existing sculpture-pipeline work (3DEP EPT/PDAL, DNR LiDAR, BlenderGIS).*

**Goal:** Given an AOI polygon, produce game-ready terrain (heightmap tiles, materials, hydrology/roads/structures vectors) with no artist in the loop.

**Stories**

- **1.1 AOI-to-terrain for a single targeted area.** Input: bounding polygon. Output: tiled DEM at fixed resolution + derived slope/aspect rasters. Source: USGS 3DEP (EPT via PDAL), fallback SRTM/Copernicus DEM for coverage gaps.
  *Guideline: resolution is a pipeline parameter from day one (game LOD vs. viz close-ups will want different settings). Store provenance metadata (source, date, CRS) in every tile.*
- **1.2 Vector context layers.** Roads, structures, water, admin boundaries from OpenStreetMap + federal sources. These matter for gameplay (values at risk, access) and viz (orientation landmarks).
  *Guideline: structures-at-risk is a first-class dataset, not decoration — the game economy depends on it.*
- **1.3 Tiling & streaming scheme.** Define the tile addressing scheme (suggest slippy-map XYZ or quadkey) shared by all raster layers so terrain, fuels, and fire state align by construction.
  *Guideline: this decision is load-bearing for everything downstream. Write an ADR before implementing.*
- **1.4 Scale-up: state → region → CONUS.** Batch orchestration, storage/cost model, incremental refresh.
  *Guideline: do not build this until 1.1–1.3 have shipped a playable AOI. Scale is a milestone gate, not a starting requirement.*
- **1.5 International sources (Canada first).** Abstract the source adapters so a region pack declares its own DEM/fuels sources.
  *Guideline: this story exists mainly to keep 1.1–1.4 honest about not hardcoding US sources. Canada/Australia ship as data packs (Epic 7).*

---

## Epic 2 — Fuels & Vegetation Pipeline

**Goal:** For any AOI, produce (a) sim-facing fuel rasters and (b) render-facing vegetation placement data.

**Stories**

- **2.1 LANDFIRE ingestion.** FBFM40 fuel models, canopy cover, canopy height, canopy base height, canopy bulk density → aligned to the Epic 1 tile scheme.
  *Guideline: LANDFIRE's raster categories become our canonical internal fuel representation. Non-US packs must map their national fuel classifications into this schema (or extend it via the pack system).*
- **2.2 Canopy structure from LiDAR.** Where point clouds exist, derive canopy height models from vegetation-classified returns to refine LANDFIRE's 30m data for close-up rendering.
  *Guideline: optional enhancement layer, never a dependency — most of CONUS must look acceptable from LANDFIRE alone.*
- **2.3 Seasonal freshness layer.** Sentinel-2 NDVI (or similar) → greenness/curing modifier over the static fuels.
  *Guideline: this is a single scalar raster modifier, not a second fuels system. Resist scope creep toward live fuel moisture science.*
- **2.4 Procedural vegetation placement.** Fuels + canopy rasters → deterministic scatter of instanced vegetation (species palettes per fuel model, density from canopy cover, height from CHM).
  *Guideline: placement must be seed-deterministic per tile so replays and multiplayer-adjacent features stay consistent. Species palettes live in data packs, not code.*

---

## Epic 3 — Live & Historic Fire Data Pipeline

**Goal:** Ingest what's burning (now or in the past) into the same tile-aligned world representation.

**Stories**

- **3.1 Incident feeds.** WFIGS/IRWIN incident metadata + perimeter polygons via NIFC open ArcGIS services. An IRWIN ID resolves to a normalized incident record.
- **3.2 Hotspot detections.** NASA FIRMS (VIIRS/MODIS) points, deduplicated and binned to tiles.
- **3.3 IR perimeter products.** NIROPS flight perimeters where available.
- **3.4 Weather.** HRRR forecast grids + RAWS station observations → the weather timeline format the sim interface consumes.
  *Guideline: weather is a *timeline input* to the sim, identical in shape whether it came from HRRR, a historic archive, or a scenario author's hand-edit.*
- **3.5 Historic season archive.** Batch-pull past seasons (perimeter progressions, ICS-209 sit reports, resource commitment data) into replay-format packages.
  *Guideline: output of this story is literally "content" — each historic fire becomes a scenario file (Epic 6/7 consume these). Prioritize 2–3 well-documented fires over broad shallow coverage.*

---

## Epic 4 — Sim Core & Fire Model Interface

**Goal:** The versioned, pluggable heart. Headless, deterministic, fast.

**Stories**

- **4.1 Fire Model Interface v1 (ADR + spec).** Define the contract: inputs (tile-aligned fuels/terrain/weather timeline, ignitions, params, **and a per-tick world-delta stream** — fuel removal/modification, moisture/retardant application, forced ignitions, forced extinguishment), tick semantics, outputs (fire state raster: burning/burned/intensity per cell, spotting events), determinism requirements, capability flags.
  *Guideline: capability flags must declare whether a model accepts mid-run world deltas. The default game model does; the playback driver (4.3) ignores them; external research models vary (some accept fuel-break edits, some are fixed-input). Gameplay degrades gracefully against models that can't be fought.*
  *Guideline: version it like a public API from day one. Breaking changes = new version + adapter. This document is the research strategy.*
- **4.2 Default game model.** Cellular propagation with slope/wind/fuel effects, tuned for legibility (players can predict behavior) and pace (fun > fidelity). GPU-resident.
  *Guideline: honest cartoon of Rothermel-style behavior — steeper = faster, wind-driven runs, fuel type matters visibly. Tunable coefficients exposed as a params file (modding hook, Epic 7). Do not chase validation.*
- **4.3 External playback driver.** A "model" implementation that replays externally supplied fire progressions (real perimeter timelines from Epic 3, or researcher model outputs) through the same interface.
  *Guideline: this story is what makes the visualizer product real and proves the interface isn't secretly coupled to our model. Build it immediately after 4.2, using a real historic fire.*
- **4.4 Headless runner + replay format.** CLI/service: config in, replay file out, faster-than-realtime. Replay = inputs + seed + interface version (re-simulate), optionally + baked state stream (re-render without the model).
  *Guideline: the baked-stream option is required — researchers won't always share their model, only its output. A replay must also pin the **world manifest hash** (Epic 9): determinism is meaningless if a LANDFIRE refresh silently changes the fuels under an old replay. Old manifests stay fetchable.*
- **4.5 Suppression simulation.** A sibling sim inside the deterministic core: takes commands (assign crew X to cut line along path P; tanker drop at target T; burnout from anchor A) plus world state, simulates execution over time — travel, line production rates, drop coverage — and emits world deltas the fire model consumes via 4.1.
  *Guideline: suppression never reaches into the fire model's internals; it only edits the world. Real fireline production-rate tables (chains/hour by crew type × fuel model, from the fire behavior handbooks) are the authenticity backbone here and are pack data (7.2), not code.*
- **4.6 Suppression effect semantics.** Define what each tactic does to world state: handline/dozer line → fuel removed to mineral soil along a widening path over time; retardant → a coating raster that reduces combustibility and decays (rain, time); water → short-lived moisture bump; burnout → sanctioned ignitions (same ignition input as wildfire, same wind risk); mop-up → forced extinguishment of edge cells.
  *Guideline: nothing is an instant "delete fire" button. Every tactic is rate-limited, positional, and defeatable — spotting over the line must be possible or the game has no tension and the sim has no honesty.*
- **4.7 Containment & outcome metrics.** Derive containment % from perimeter cells adjacent to secured line/burned-cold edge; derive values-at-risk outcomes (structures lost/saved) from fire state × Epic 1 structure data.
  *Guideline: these are computed observers on sim state — the scoreboard reads the world, it never writes it.*
- **4.8 (Later) Scripting bindings.** Python bindings on the headless runner for research/RL use.
  *Guideline: parked until an external party actually wants it. The architecture (4.1, 4.4) makes it cheap when demanded.*

---

## Epic 5 — Rendering & Visualizer

**Goal:** The shared 3D presentation layer, and the broadcast-facing product built on it.

**Stories**

- **5.1 Terrain + vegetation rendering.** Stream Epic 1/2 tiles into UE; instanced foliage from 2.4; LODs from national map view down to fire-line close-up.
- **5.2 Fire state rendering.** Fire raster → flame/ember/smoke presentation. Burned-area scarring. Smoke plume driven by wind field.
  *Guideline: read like the IR-overlay maps people already trust (perimeter clarity first), then add spectacle. Legibility beats cinema for both products.*
- **5.3 Camera & flyover system.** Orbit, chase-the-front, and authored flyover paths; timeline scrubbing of any replay.
- **5.4 Overlay/annotation layer.** Perimeter history, containment lines, evacuation zones, labels, scale cues — the "broadcast graphics" vocabulary.
- **5.5 Visualizer product v0: IRWIN-ID-to-flyover.** Given an incident ID: fetch data (Epic 3), build world (Epics 1–2), replay progression (4.3), output an interactive scene + rendered video.
  *Guideline: this is the wedge product and the integration test for the entire spine. Measure "minutes from ID to video" as the KPI. Ship scrappy during a real fire season and see who cares.*
- **5.6 Export formats.** Video render queue; later, packaged interactive scenes.

---

## Epic 6 — The Game

**Goal:** The NIFC-scale strategy loop. (Full game design doc is a separate document; these stories are the systems skeleton.)

**Stories**

- **6.1 National layer.** Map of regions; fires as abstract entities (size, growth potential, values at risk, containment %) driven by cheap statistical simulation. Preparedness Level 1–5 as the season's tension dial.
  *Guideline: most fires are never fully simulated. Only "problem fires" the player focuses get promoted to the real sim (Epic 4) on real terrain (Epics 1–2). This promotion mechanic is the game's core technical trick — prototype it early.*
- **6.2 Resource system.** Crews (Type 1/2, hotshots, smokejumpers), engines, dozers, air assets (exclusive-use vs call-when-needed contracts, MAFFS surge). Fatigue, repositioning time, cost.
  *Guideline: authenticity of texture over completeness — the family sniff test is the acceptance criterion.*
- **6.3 Incident gameplay.** On a promoted fire: assign resources to tactics (line, burnout, structure protection, air support), watch consequences through the shared renderer, make triage calls.
  *Guideline: this layer issues *commands* to the suppression sim (4.5) and reads outcomes (4.7). It contains zero fire/suppression logic itself — the UI for the sim, not a second sim.*
- **6.4 Season & off-season loop.** Budget cycles, training investments, equipment purchases, prescribed-burn programs with delayed probabilistic payoff, political/funding events.
- **6.5 Scenario & campaign system.** Procedural seasons (fire spawning from historical ignition/weather statistics) and historic season replays (from 3.5).
  *Guideline: scenarios are data-pack content (Epic 7). "2020 season" is the flagship marketing scenario.*
- **6.6 Tone & responsibility pass.** Fatalities, communities, and real events handled with respect; naming policy for historic incidents; consult people who were there.
  *Guideline: this is a story, not a vibe — it gets explicit review before any public showing.*

---

## Epic 7 — Data Packs & Modding

**Goal:** One packaging mechanism for regions, fuel behaviors, scenarios, and assets — used identically by first party, DLC, and community.

**Stories**

- **7.1 Data pack format & loader (ADR + spec).** Manifest, versioning, dependency declaration, load order/override rules, signature/trust model.
  *Guideline: decide this before Epics 2/4/6 hardcode anything. The test: "Could Australia ship as a pack with zero engine changes?" Litmus content: region terrain/fuels sources, fuel behavior tables, species palettes, resource types, scenarios, localization strings.*
- **7.2 Fuel behavior as data.** Model coefficients, fuel model definitions, and the "how does sagebrush burn" tables live in editable pack files consumed by 4.2.
  *Guideline: the modder who wants to tweak sagebrush should never touch code. Hot-reload in dev builds.*
- **7.3 Scenario authoring.** Scenario = AOI + weather timeline + ignitions + objectives + (optionally) scripted events, as a documented file format. In-game or external editor is a later decision; format first.
- **7.4 Region pack tooling.** The Epic 1–2 pipelines packaged so a motivated community member (or us, for Canada DLC) can generate a region pack from their national data sources.
  *Guideline: our internal pipeline IS the mod tool. Don't build it twice.*
- **7.5 Distribution & workshop integration.** Steam Workshop / mod.io etc. Parked until platform decisions are made; 7.1's trust model must anticipate it.

---

## Epic 8 — Platform, Distribution & Business

**Stories**

- **8.1 Tech stack ADRs.** Engine (UE presumed given team skills — confirm licensing/employment considerations), sim-core language/runtime (must run engine-free for headless), pipeline stack (Python/PDAL/GDAL presumed), cloud/storage/cost model for tile hosting.
- **8.2 Product sequencing decision.** Recommended: Visualizer v0 (5.5) → game vertical slice on one AOI → game. Revisit after Visualizer v0 market signal.
- **8.3 Funding avenues.** SBIR/STTR, Joint Fire Science Program, NSF — scoped to the shared infrastructure/research layer. Also: WA-specific opportunities.
- **8.4 Legal posture.** "Not an operational prediction tool" disclaimers; data licensing audit (federal = public domain; OSM = ODbL; Sentinel = free with attribution; check WFCA-style aggregators — go to primary sources).
- **8.5 Team & scope reality check.** Identify the 2–3 hires/collaborators that change feasibility (pipeline engineer, tech artist, fire SME advisor). Define the solo-viable subset (Visualizer v0 is plausibly solo; the full game is not).

---

## Epic 9 — Bake & Content Delivery Service

*Architecturally sits between the ingestion pipelines (Epics 1–3) and everything else; numbered last because it was recognized last. Clients — game, visualizer, headless — never touch source APIs; they consume baked runtime tiles from a CDN plus a small live delta feed.*

**Goal:** A hosted service that rescans sources on per-layer cadences, bakes to a compact streamable runtime format, and publishes versioned world manifests.

**Stories**

- **9.1 Runtime tile format (ADR + spec).** The baked, streamable representation: quantized heightmaps, palettized fuel rasters, tiled vectors, per-tile vegetation *scatter inputs* (seed + params — instances are regenerated deterministically client-side, never shipped). Content-addressed (hash-named) tiles.
  *Guideline: content addressing does triple duty — CDN cacheability, cheap diffs (a refresh only uploads changed tiles), and integrity for replay pinning. Design the format for range-request streaming; assume mobile-class bandwidth as the floor.*
- **9.2 World manifests & versioning.** A world version = a manifest mapping tile addresses → content hashes per layer. Frontends resolve "Teanaway, world v42" → tile set. Replays pin a manifest hash (see 4.4); old manifests remain fetchable indefinitely.
- **9.3 Scheduled rescan orchestration.** Per-layer cadences, not one monolithic rescan: terrain ≈ never (bake once, refresh on new LiDAR), LANDFIRE ≈ annual (their release cycle), NDVI ≈ weekly in season, fire perimeters/hotspots ≈ minutes–hours, weather ≈ hourly. Failed source fetches degrade to last-good data with staleness flagged in the manifest.
  *Guideline: be a polite consumer of federal APIs — cache aggressively, honor rate limits, never fan client load out to the sources. That's the whole point of the middle tier.*
- **9.4 Live delta feed.** The one non-static path: small, frequent fire-state and weather deltas for active incidents, published as append-only streams the visualizer tails.
  *Guideline: keep this path as thin as possible. Everything that can be static-file-on-CDN is; only genuinely live data earns a service with uptime expectations. This is a one-person ops budget.*
- **9.5 On-demand AOI baking.** Bake queue: request an AOI not yet covered → pipeline runs → tiles + manifest publish → requester notified. Cache forever.
  *Guideline: this defers the "pre-bake CONUS?" cost decision indefinitely — coverage grows where demand is. Pre-baking a region is just warming this cache, gated at 1.4.*
- **9.6 Bake pipeline as a portable artifact.** The bake service's core is a containerized pipeline runnable locally — which *is* the region-pack mod tooling (7.4) and the researcher's private-AOI path. Hosted service = same container + scheduler + storage.
  *Guideline: one codebase. If the hosted path and the local path drift, both rot.*
- **9.7 Cost & telemetry.** Storage/egress cost model per region; bake-time budgets; staleness dashboards.
  *Guideline: write down the monthly-cost ceiling you'll tolerate for a hobby-phase service and design 9.3/9.5 cadences backward from it.*

---

## 3. Milestone Sketch

| Milestone | Proves | Scope |
|---|---|---|
| **M0 — Teanaway Slice** | Data spine works | Epics 1–2 on one AOI; static 3D render of real terrain + plausible vegetation |
| **M1 — Replay a Real Fire** | Interface + viz work | Epic 3 + 4.3 + 5.1–5.3: a historic local fire (e.g., Jolly Mountain 2017) replayed as 3D flyover |
| **M2 — Visualizer v0** | Someone cares | 5.5 live during fire season; measure minutes-to-video and external interest |
| **M3 — Sim Vertical Slice** | The game is fun | 4.2 + 6.3 on one AOI: fight one fire with a small resource set |
| **M4 — Season Loop** | The game is a game | 6.1/6.2/6.4 wrapped around M3 |
| **M5 — Pack System Proof** | Modding is real | A second region (Canada?) shipped purely as a data pack |

## 4. Top Risks

1. **Scope: three products, one part-time person.** Mitigation: milestone gates with kill/park criteria; M2 is a genuine go/no-go signal.
2. **Data pipeline burnout.** GIS ingestion is unglamorous and endless. Mitigation: hard AOI-first discipline; scale only behind milestone gates.
3. **Rendering cost of vegetation at scale.** Mitigation: LODs designed at 5.1, not retrofitted; national view is cartographic, not foliage-rendered.
4. **Fire model credibility confusion.** Press/researchers mistaking the game model for a prediction. Mitigation: 8.4 posture, clear labeling, external-driver story (4.3) as the sanctioned research path.
5. **Sensitivity.** Real fires involve real losses. Mitigation: 6.6 is a gated story; visualizer defaults to informational framing.
6. **Employment/IP boundaries.** Confirm side-project posture w.r.t. employer agreements before public work.

## 5. Open Questions

- Sim core runtime: in-engine plugin vs. separate process with IPC? (Affects 4.4, 4.6, modding safety.)
- Tile scheme resolution(s) — one canonical + derived, or per-layer native?
- Multiplayer ambitions for the game (co-op incident command?) — affects determinism requirements now even if built never.
- Name. "Project Ember" is a placeholder; check trademark landscape when serious.

---

## 6. Glossary

**Fire domain**

| Term | Meaning |
|---|---|
| **NIFC** | National Interagency Fire Center — the Boise hub coordinating national wildland fire response; the player's role in the game |
| **NWCG** | National Wildfire Coordinating Group — sets interagency standards (fuel models, production rates, training) |
| **PL 1–5** | National Preparedness Level — escalating scale of how stretched national fire resources are; PL5 = everything is committed |
| **IRWIN** | Integrated Reporting of Wildland-Fire Information — interagency incident data exchange; every incident gets an IRWIN ID |
| **WFIGS** | Wildland Fire Interagency Geospatial Services — the public ArcGIS feeds of incident locations and perimeters |
| **ICS-209** | Incident Status Summary — the standardized daily situation report filed per large incident; the historic archive is season-replay gold |
| **NIROPS** | National Infrared Operations — USFS aircraft flying nighttime IR mapping missions over large fires; source of high-quality perimeters |
| **FIRMS** | Fire Information for Resource Management System — NASA's near-real-time satellite hotspot feed |
| **VIIRS / MODIS** | The satellite sensors behind FIRMS hotspot detections (375m / 1km resolution respectively) |
| **LANDFIRE** | Federal program publishing nationwide 30m rasters of fuels, vegetation, and fire regime data — our canonical fuels source |
| **FBFM40** | The 40 Scott & Burgan Fire Behavior Fuel Models — the standard classification of surface fuels (grass, shrub, timber litter, slash…) |
| **MTBS** | Monitoring Trends in Burn Severity — historic burned-area perimeters and severity rasters, 1984–present |
| **RAWS** | Remote Automated Weather Stations — the fire-weather station network (wind, humidity, fuel moisture) |
| **HRRR** | High-Resolution Rapid Refresh — NOAA's 3km hourly-updating forecast model; our weather timeline source |
| **MAFFS** | Modular Airborne FireFighting Systems — military C-130s converted to air tankers, activated when contract tankers run out (a PL5 surge mechanic) |
| **IMT** | Incident Management Team — the overhead team (Type 1 = most complex incidents) assigned to run a large fire |
| **Chain** | 66 feet. The fire world's unit for fireline length and spread rate (chains/hour). 80 chains = 1 mile |
| **Handline / dozer line** | Fireline cut to mineral soil by hand crews / bulldozers |
| **Burnout** | Intentionally firing fuels between a control line and the main fire to starve it |
| **FARSITE / FlamMap** | USFS fire growth / fire behavior modeling tools — the research incumbents |
| **ELMFIRE** | Open-source level-set fire spread model; reference implementation and likely first external-model integration target |
| **Rothermel model** | The 1972 surface fire spread equations underlying most operational US fire modeling |

**Geospatial & technical**

| Term | Meaning |
|---|---|
| **GIS** | Geographic Information System(s) — spatial data generally |
| **AOI** | Area of Interest — the bounding polygon a pipeline run targets |
| **DEM** | Digital Elevation Model — terrain height raster |
| **CHM** | Canopy Height Model — vegetation height raster derived from LiDAR |
| **LiDAR** | Laser-scanned point clouds; source of high-resolution terrain and canopy structure |
| **3DEP** | USGS 3D Elevation Program — the national LiDAR/DEM collection |
| **EPT** | Entwine Point Tiles — the cloud-streamable point cloud format 3DEP data is served in |
| **PDAL / GDAL** | Standard open-source toolkits for point cloud / raster-vector geospatial processing |
| **SRTM** | Shuttle Radar Topography Mission — global fallback DEM where LiDAR is absent |
| **NDVI** | Normalized Difference Vegetation Index — satellite greenness measure; our seasonal curing signal |
| **CRS** | Coordinate Reference System — map projection; every dataset must declare one |
| **OSM / ODbL** | OpenStreetMap and its Open Database License (attribution/share-alike obligations) |
| **CONUS** | Contiguous United States |
| **ADR** | Architecture Decision Record — short written record of a load-bearing technical decision |
| **LOD** | Level of Detail — rendering fidelity tiers by distance |
| **UE** | Unreal Engine |
| **IPC** | Inter-Process Communication |
| **CLI / API / GPU / KPI / SME** | Command-line interface / application programming interface / graphics processing unit / key performance indicator / subject-matter expert |
| **RL** | Reinforcement Learning — the ML research niche that wants headless fire environments |
| **DLC** | Downloadable Content — first-party post-launch content (ships via the same data-pack system as mods) |
| **SBIR / STTR** | Small Business Innovation Research / Technology Transfer — federal non-dilutive R&D grant programs |
| **JFSP** | Joint Fire Science Program — federal fire research funding body |
| **NSF** | National Science Foundation |

---

## 7. Reference Links

**Core data sources**

- LANDFIRE (fuels, canopy rasters): https://landfire.gov
- USGS 3DEP: https://www.usgs.gov/3d-elevation-program — LiDAR via AWS EPT: https://registry.opendata.aws/usgs-lidar/
- NIFC Open Data (WFIGS incidents & perimeters): https://data-nifc.opendata.arcgis.com
- NASA FIRMS (satellite hotspots): https://firms.modaps.eosdis.nasa.gov
- NIROPS (IR perimeter products): https://fsapps.nwcg.gov/nirops
- MTBS (historic burn perimeters/severity): https://www.mtbs.gov
- HRRR forecast model: https://rapidrefresh.noaa.gov/hrrr/
- RAWS climate archive (DRI): https://raws.dri.edu
- Sentinel-2 imagery (Copernicus Data Space): https://dataspace.copernicus.eu
- OpenStreetMap: https://www.openstreetmap.org
- ICS-209 / historic incident data: FAMWEB (https://famit.nwcg.gov) and NIFC Predictive Services intelligence archives; see also the research-cleaned "ICS-209-PLUS" dataset

**Suppression & fire behavior references**

- NWCG Fireline Production Rate Tables (2021 compilation, originally PMS 210): https://www.frames.gov/documents/behaveplus/publications/NWCG_2021_FireLineProductionRates.pdf (catalog entry: https://www.frames.gov/catalog/64042)
- Broyles, *Fireline Production Rates* (2011, USFS SDTDC field-observation study): https://www.fs.usda.gov/t-d/pubs/pdf/11511805.pdf
- Rothermel (1972), *A Mathematical Model for Predicting Fire Spread in Wildland Fuels*, USFS Research Paper INT-115 — search USFS Treesearch
- Scott & Burgan (2005), *Standard Fire Behavior Fuel Models* (the FBFM40 definitions), USFS RMRS-GTR-153 — search USFS Treesearch
- Anderson (1982), *Aids to Determining Fuel Models* (the original 13 fuel models) — search USFS Treesearch

**Models & tools**

- ELMFIRE (open-source spread model): https://elmfire.io — source: https://github.com/lautenberger/elmfire
- FlamMap / FARSITE (USFS Missoula Fire Lab): https://www.firelab.org/project/flammap
- SimFire / SimHarness (MITRE's RL fire environment — prior art for the research surface): https://github.com/mitrefireline
- PDAL: https://pdal.io — GDAL: https://gdal.org

**Funding**

- Joint Fire Science Program: https://www.firescience.gov
- SBIR/STTR portal: https://www.sbir.gov

---

## 8. Comparable Games & Revenue Analysis

### Methodology caveat

Unit/revenue figures below are third-party estimates (Boxleiter-method review multiples via games-stats.com, raijin.gg, steam-revenue-calculator.com, SteamSpy) unless marked **[announced]**. Estimators disagree by 2–3x on the same title — treat these as order-of-magnitude, gross (pre-Steam-cut, pre-refund, pre-regional-pricing). Rough rule: developer net ≈ 50–60% of Steam gross before taxes.

### Comparables

| Game | Year | Price | Est. units | Est. gross | Relevance / lesson |
|---|---|---|---|---|---|
| **911 Operator** (Jutsu Games) | 2017 | $14.99 | 500k–1M (SteamSpy) | $4M–$10M (estimators disagree) | Closest structural comp: emergency dispatch + resource allocation on real maps. "Play on any city in the world" was a headline selling point — validates our real-terrain hook. Spawned sequel (112 Operator) + DLC line |
| **Firefighting Simulator: The Squad** (astragon) | 2020 | ~$20 | ~97k | ~$1.6M | Proves a firefighting-theme audience exists at premium sim pricing; first-person/urban, not strategy — different lane |
| **Fire Commander** (Atomic Wolf) | 2022 | $14.99 | low thousands | ~$93k | The cautionary tale, in almost exactly our theme (tactical wildfire RTS). 6/10 reviews, faded fast. Theme alone doesn't sell; execution and systems depth do |
| **Against the Storm** (Eremite, 2-person-founded studio) | 2022 EA | $29.99 | 1M **[announced 2024]**, 1.2M by late 2024 | ~$20M+ implied | Ceiling for a small-team systems-first strategy game with no IP: early access + relentless updates + 95% rating |
| **Frostpunk** (11 bit) | 2018 | $29.99 | 3M **[announced 2021]**, 5M+ **[announced 2024]** | $50M+ implied | Genre ceiling: "grim resource triage under pressure" is a proven fantasy at scale, but built with AAA-adjacent production and narrative |
| **Infection Free Zone** (Jutsu Games) | 2024 EA | ~$20 | — | — | Same studio as 911 Operator doubling down on the real-world-map hook ("defend any real location") as the core marketing pitch. Independent confirmation the hook has legs |

Adjacent qualitative comps (no numbers pulled): *This Is the Police* (institution-management under pressure), *112 Operator* (direct sequel economics), *Rescue HQ*. Note: a 2020 indie already used the title *Wildfire* — factor into naming search.

### What the comps say

1. **The market slot exists and is proven at $15–30 premium.** Dispatch/triage strategy games with real-map hooks have sold 500k+ units. Nobody has shipped the *wildland* version well — Fire Commander is the only direct attempt and it under-executed.
2. **The real-terrain hook is validated twice** by the studio with the closest comp (911 Operator's "any city," Infection Free Zone's "any real location"). "Fight the fire on the actual ridge behind your town" is the same pitch with a better emotional charge — and our Epic 9 pipeline is the moat behind it, since faking it with generic terrain is easy but doing it with real fuels data is not.
3. **Theme is not a strategy.** The gap between Fire Commander (~$93k) and 911 Operator ($4M+) is systems depth, replayability, and polish — not subject matter.
4. **Season structure fits the proven loop.** Frostpunk/Against the Storm demonstrate that escalating-pressure runs with meta-progression (our fire season + off-season) is the shape players reward.

### Revenue scenarios — the game (Steam PC launch, ~$19.99–24.99)

| Scenario | Units (yr 1–2) | Gross | Dev net (rough) | Comp anchor | Honest odds |
|---|---|---|---|---|---|
| **Floor** | <5k | <$100k | <$50k | Fire Commander; also the *median* Steam indie outcome | Most likely single outcome |
| **Base** | 15k–40k | $300k–$800k | $150k–$450k | Solid niche sim reception, modest streamer pickup | Plausible with quality + the terrain hook |
| **Good** | 75k–150k | $1.5M–$3M | $800k–$1.7M | Firefighting Simulator territory; sustains a real studio | Requires standout execution + marketing beat (fire-season news cycle is free marketing, uncomfortably) |
| **Hit** | 500k+ | $5M+ | $2.5M+ | 911 Operator | Lottery odds; needs virality (streamers + "my hometown" clips are the plausible vector) |

*Planning stance: budget time/money against Floor–Base; architect (modding, packs, live data) so Good–Hit can be capitalized on if it happens. Console/mobile ports are upside multipliers on success, not part of the base case.*

### Comps beyond disaster themes — general resource management sims

Same estimate caveats apply; **[announced]** = official figures.

| Game | Year | Price | Units | Gross (est.) | What it demonstrates for us |
|---|---|---|---|---|---|
| **RimWorld** (Ludeon, ~1 dev at launch) | 2018 (EA 2013) | $34.99 | 1M **[announced 2018]**; 3.5M+ Steam per VG Insights | $100M+ implied lifetime | Indirect control + an *AI storyteller* pacing adversity. Mod ecosystem = decade of longevity |
| **Factorio** (Wube) | 2020 (EA 2016) | $35, never discounted | ~5.9M (Gamalytic) | ~$166M (Gamalytic) | Systems depth alone, zero marketing gloss, no sales ever. Engineering-brain audience pays full price |
| **Prison Architect** (Introversion, ~4 people) | 2015 (alpha 2012) | $29.99 | 2M **[announced]**; ~4.9M lifetime (Gamalytic) | $10.7M in alpha pre-orders alone **[announced]**; ~$32M lifetime | Institution-management fantasy; paid-alpha funding model; 10+ DLCs of tail revenue |
| **Oxygen Not Included** (Klei) | 2019 | $24.99 | ~7.4M (Gamalytic) | ~$91M (Gamalytic) | Simulation-of-flows (gas, heat, fluid) as the core toy — closest analog to "fire as a simulated substance players learn to read" |
| **Football Manager** (Sports Interactive) | annual | $60 | millions/yr | franchise scale | The season/off-season structure is literally our loop: recruit, train, contract, then compete through a schedule you don't fully control |

Qualitative adds: *Motorsport Manager* (season loop at indie scale), *Two Point Hospital* (institution management with charm as differentiator), *Transport Fever / OpenTTD* (network flow allocation).

### Takeaways from the non-disaster comps

1. **The buyer is the same person.** These games sell "learn a deep system, allocate scarce resources under pressure" — the theme (prisons, factories, colonies) is the costume. Our costume happens to be dramatic, real, and streamable.
2. **RimWorld's AI Storyteller is a direct design steal for the season generator.** Fires shouldn't spawn from flat randomness (6.5) — a pacing director deals ignitions, weather events, and lightning busts as narrative beats with rising tension and breathers. This is likely the difference between "random fire spawner" and a game people tell stories about.
3. **Indirect control is validated at the top of the genre.** RimWorld/ONI players command intentions, not units — matching our commands-to-suppression-sim architecture (4.5, 6.3). The frustration-vs-drama balance of watching your crews execute imperfectly is a feature, not a compromise.
4. **Mod ecosystems are the longevity engine** for every long-tail winner in this table (RimWorld, Factorio, ONI, OpenTTD). Epic 7 is not a nice-to-have; in this genre it's the difference between a launch spike and a decade.
5. **Price with confidence.** The genre's winners sit at $25–35 and rarely discount (Factorio famously never). A deep, real-data wildfire strategy game does not need to launch at $14.99.
6. **DLC/expansion tail is where these games make their second fortune** (Prison Architect's 10+ DLCs, RimWorld's expansions). Our region packs and historic-season scenarios are a natural, already-architected DLC line (7.1).

### Revenue notes — the visualizer

Different math entirely: B2B, small-N. Sketch: 5–20 clients (local broadcast, county emergency management, PIO shops) × $2k–$10k/season or per-incident pricing = $10k–$200k/yr — modest but recurring, season-spiky, and it subsidizes the shared data spine. Grant funding (8.3) is realistically the larger "revenue" line for the infrastructure layer in early years. Validate willingness-to-pay at M2 before building sales motion.
