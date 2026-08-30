# Epic 3 — Live & Historic Fire Data Pipeline — Execution Plan

**Parent:** WILDFIRE_DESIGN.md → Epic 3
**Status:** Plan v0.1 — ready to task out
**Scope owner:** Brad / BNE Games LLC
**Predecessors:** Epic 1 (done), Epic 2 (complete, not deeply verified — see §7 agent notes)

---

## 0. Framing

Epics 1–2 answer *"what is this place and what can burn?"* Epic 3 answers *"what is burning / what burned?"* — and packages the answer so downstream consumers never care whether it came from a live feed or an archive.

Two consumers are waiting on this epic:
- **Story 4.3 (external playback driver)** needs a fire progression it can replay through the model interface. That makes the **arrival-time raster** (per-cell first-burn time on the canonical tile grid) the single most important artifact here.
- **Story 5.5 (IRWIN-ID-to-flyover)** needs everything: incident metadata, terrain+fuels for the fire's footprint, progression, weather. This epic delivers that as one **incident package**.

```
 SOURCES                          NORMALIZED (per incident, IRWIN-keyed)         DERIVED / PACKAGED
 ┌──────────────────┐             ┌──────────────────────────────────┐
 │ WFIGS incidents  │──B1/B2────▶ │ incident record (metadata)       │
 │ + perimeters     │             │ observations/                    │      ┌─────────────────────────┐
 ├──────────────────┤             │   perimeters (vector series)     │──D──▶│ arrival_time.cog.tif    │
 │ NASA FIRMS       │──B3───────▶ │   hotspots (points, FRP, conf.)  │      │ fire-state tiles (grid) │
 │ (VIIRS/MODIS)    │             │   ir_products (NIROPS polys)     │      ├─────────────────────────┤
 ├──────────────────┤             ├──────────────────────────────────┤      │ incident package:       │
 │ NIROPS IR        │──B4───────▶ │ weather/                         │──E──▶│  meta + terrain/fuels   │
 ├──────────────────┤             │   timeline.v0 (gridded+station)  │      │  ref (Epics 1–2) +      │
 │ HRRR (AWS grib)  │──C1───────▶ ├──────────────────────────────────┤      │  observations + arrival │
 │ RAWS (Synoptic)  │──C2───────▶ │ OBSERVATIONS ARE IMMUTABLE TRUTH │      │  + weather + provenance │
 ├──────────────────┤             │ derived products are versioned   │      └─────────────────────────┘
 │ ICS-209 archive, │──E1───────▶ │ interpretations of them          │        = scenario bundle v0
 │ perimeter history│             └──────────────────────────────────┘        (pre-Epic-7 pack shape)
 └──────────────────┘
```

**Load-bearing ideas:**
- **Everything keys off the IRWIN ID.** Multiple sources describing one fire reconcile into one incident record. Historic fires that predate IRWIN get synthetic ids in a reserved namespace.
- **Observations vs. interpretations.** Perimeters and hotspots are stored raw and immutable (with source provenance). The arrival-time raster is a *derived, versioned* product of an explicit algorithm — because interpolating between irregular perimeter observations is an editorial act, and we must be able to improve it without corrupting truth.
- **Live and historic are the same schema.** A live fire is an incident package that keeps growing; a historic fire is one that stopped. Only adapters and cadence differ. (Polling/scheduling is Epic 9 — here, "refresh" is a command, not a daemon.)
- **An incident defines its own AOI.** Fire bbox + configurable buffer → Epics 1–2 bake terrain/fuels for it on demand. This is the first real exercise of the on-demand pattern that becomes Epic 9.5.

---

## 1. Scope (in / out)

**In scope**
- Source adapters (via the Epic 1 `SourceAdapter` seam where it fits, or a parallel `IncidentSource` seam where it doesn't): WFIGS incident locations + perimeters, FIRMS hotspots, NIROPS IR products (best-effort), HRRR forecast/analysis subsets, RAWS observations via Synoptic.
- Normalized incident model + IRWIN-keyed store; geometry cleaning and perimeter series construction.
- **Arrival-time raster derivation** (perimeters + hotspot assist) on the canonical tile grid; fire-state tiling through the Epic 1 tiler.
- **Weather timeline schema v0** (mini-ADR co-owned with future Story 4.1) + assembly from HRRR/RAWS.
- **Incident/scenario package format v0** and a `terrain incident` command: IRWIN ID (or historic fire id) → complete package.
- Historic ingestion: NIFC perimeter history, ICS-209 archive (ICS-209-PLUS research dataset preferred), MTBS severity reference; **2–3 flagship historic fire packages, done deep** (default: Jolly Mountain 2017 + one large recent WA fire — confirm D6).

**Explicitly out of scope**
- Scheduled polling, live delta feeds, uptime — Epic 9 (9.3/9.4). The refresh command must be *idempotent and cheap* so Epic 9 can wrap it, nothing more.
- Fire simulation, spread inference beyond observation interpolation — Epic 4. The arrival raster interpolates observations; it does not model.
- Suppression/resource *gameplay* semantics — Epic 4/6. We do capture resource-commitment data from ICS-209s into packages (it's cheap here, expensive to backfill).
- Rendering — Epic 5 consumes packages; nothing visual here beyond QA plots.
- Full national season archive — story 3.5's breadth. Flagship-deep beats broad-shallow (design doc guideline); the pipeline makes breadth a batch job later.

**Definition of done for Epic 3:** `terrain incident --irwin <id>` on a current-season fire, and `terrain incident --historic jolly-mountain-2017`, each produce a validating scenario bundle: incident metadata, baked terrain+fuels reference for the fire's AOI, immutable observation series, versioned arrival-time raster + fire-state tiles, weather timeline v0, full provenance — reproducibly, with the historic path proven on committed fixture data in CI.

---

## 2. Stack additions

| Concern | Choice | Rationale |
|---|---|---|
| ArcGIS feature services | **REST queries via httpx + geojson paging** (or `arcgis`-lite helper of our own) | WFIGS layers are standard FeatureServer endpoints; avoid the heavy Esri SDK dependency. |
| FIRMS | FIRMS area API (CSV) with **MAP_KEY** | Simple, documented, free key. Secrets pattern: env var / `.secrets.toml`, never in settings or provenance. |
| HRRR | **herbie** library, AWS `noaa-hrrr-bdp-pds` archive | The de-facto Python tool; supports byte-range subsetting so we pull variables, not multi-GB gribs. Store AOI-cropped subsets only. |
| RAWS | **Synoptic Data API** (free public-data tier, token) | The practical programmatic path to RAWS; WIMS/FTP is the fallback nobody enjoys. |
| NIROPS | Directory-listing adapter over the public IR products site; per-incident shapefile/KMZ pulls | No clean API; treat as **best-effort**: failures degrade to "layer absent," never block a package. |
| ICS-209 history | **ICS-209-PLUS** cleaned research dataset as primary; FAMWEB raw as fallback | Years of sit-report parsing already done by researchers; don't re-suffer it. |
| Geometry | shapely 2.x (`make_valid`, STRtree), pyproj | Real perimeter data contains self-intersections, bowties, and duplicate rings. Cleaning is a *stage*, not a patch. |

---

## 3. Storage layout additions

```
store/
  incidents/
    <irwin-or-hist-id>/
      incident.json                  # normalized record: names, dates, agency, size/containment series
      aoi.geojson                    # fire bbox + buffer (drives Epic 1–2 bake); ref to region store
      observations/                  # IMMUTABLE, append-only, content-addressed
        perimeters/<ts>_<source>_<hash>.geojson
        hotspots/<sat>_<date>_<hash>.parquet
        ir/<flight_ts>_<hash>.geojson
        sitreps/<date>_209_<hash>.json
      weather/
        timeline.v0.parquet          # assembled per weather-timeline schema
        raw/                         # AOI-cropped HRRR subsets, RAWS pulls (content-addressed)
      derived/
        arrival_time.v<alg>.cog.tif  # per-cell first-burn hours since t0; alg version in name+meta
        confidence.v<alg>.cog.tif    # interpolated vs observed vs hotspot-inferred per cell
        progression_meta.json        # t0, snap times, alg params
      package/
        scenario.bundle.json         # the manifest tying all of the above + Epic 1–2 refs together
      tiles/                         # fire-state rasters through the standard tiler (time-indexed layer)
```

Weather grids and fire tiles align to the incident AOI's canonical grid (Epic 1 rules). The bundle manifest **pins the region store's world manifest hash** — the design doc's replay-pinning requirement (4.4/9.2) starts being honored here, before Epic 9 exists.

---

## 4. Settings additions

```toml
[incidents]
buffer_km    = 5.0                 # AOI = fire extent + buffer
profile      = "game"              # Epic 1 profile used for the on-demand bake
sources      = ["wfigs","firms","nirops"]   # priority/enable list
firms_days   = 10                  # hotspot lookback for live refresh

[incidents.weather]
gridded  = "hrrr"                  # analysis preferred, forecast fallback (recorded which)
station  = "synoptic"
variables = ["wind10_u","wind10_v","t2","rh2","precip"]
step_minutes = 60

[incidents.arrival]
algorithm  = "perimeter-interp-v1" # versioned; see D-stream
hotspot_assist = true              # hotspots refine timing between perimeter snaps
snap_tolerance_h = 2.0

[historic]
dataset  = "ics209-plus"
flagships = ["jolly-mountain-2017"]   # + confirm second/third (D6 ⚑)
```

API keys: `FIRMS_MAP_KEY`, `SYNOPTIC_TOKEN` via environment / untracked secrets file; loader refuses to write them into provenance or bundles.

---

## 5. Key decisions (defaults set; confirm the ⚑ ones)

| # | Decision | Recommendation | Notes |
|---|---|---|---|
| D1 ⚑ | Canonical progression product | **Arrival-time raster (+confidence raster), derived & versioned; vector observations immutable underneath** | Arrival-time is what playback (4.3) replays and Epic 5 renders (isochrones = contours of it). Confidence raster keeps us honest about interpolation. |
| D2 ⚑ | Weather timeline schema v0 owned here | **Yes — mini-ADR now, co-signed by future 4.1** | Epic 4 doesn't exist; someone must move first. Versioned schema + adapter pattern caps the regret. |
| D3 | Incident identity | IRWIN ID canonical; `hist:` namespace for pre-IRWIN fires; cross-refs (GeoMAC/MTBS ids) recorded | |
| D4 | HRRR handling | Analysis (F00) series preferred for historic truth; forecast cycles for live; AOI-cropped variable subsets only; which-was-used in provenance | Full gribs are multi-GB each; never store them. |
| D5 | NIROPS posture | Best-effort adapter; absence never blocks | Source is operationally organized, not API-organized. |
| D6 ⚑ | Flagship historic fires | **Jolly Mountain 2017** + propose **Schneider Springs 2021**; third optional | Wants Brad's call: local knowledge is the QA instrument for these packages. |
| D7 | Sitrep capture | Parse ICS-209 resource/containment/cost series into packages now, even though gameplay (Epic 6) consumes them much later | Cheap at ingest, painful to backfill; also the design doc calls 209s "season-replay gold." |

---

## 6. Workstreams & tasks

### Workstream A — Incident model & store
- **A1. Incident schema + store layout.** Pydantic models for incident record, observation envelopes (source, fetch time, geometry hash), bundle manifest v0; store scaffolding per §3; `hist:` id namespace.
  - *Done when:* schema docs merged; synthetic incident round-trips; bundle manifest validates. → *dep: none*
- **A2. AOI derivation + Epic 1–2 bake hookup.** Fire extent (union of perimeters/hotspots) + buffer → AOI polygon → invoke the Epic 1–2 pipeline for that AOI (reusing region stores when covered); record world-manifest pin in the bundle.
  - *Done when:* an incident with observations gets terrain+fuels baked hands-free; re-run reuses the existing bake; pin recorded. → *dep: A1, Epics 1–2*

### Workstream B — Fire observation adapters
- **B1. WFIGS incident locations.** Query FeatureServer by IRWIN ID / bbox / date; normalize metadata (names, discovery, size, containment history where present).
  - *Done when:* a current-season IRWIN ID resolves to a populated incident record; attribute mapping documented. → *dep: A1*
- **B2. WFIGS perimeters (current + history datasets).** Fetch all perimeter versions for an incident; geometry cleaning stage (`make_valid`, ring dedupe, CRS normalize); append-only observation series ordered by source timestamp; near-duplicate collapse (same geometry re-published) with all source records retained in provenance.
  - *Done when:* Jolly Mountain returns a clean, time-ordered perimeter series; every stored geometry is valid; cleaning actions logged per feature. → *dep: B1*
- **B3. FIRMS hotspots.** Area query per AOI/date-range across VIIRS (SNPP/NOAA-20/21) + MODIS; normalize (acq time UTC, FRP, confidence, satellite); dedupe overlapping detections; parquet per satellite-day.
  - *Done when:* detections for a known fire window match the public FIRMS map spot-check; confidence/day-night preserved. → *dep: A1*
- **B4. NIROPS adapter (best-effort).** Discover + fetch per-incident IR products (heat perimeter / intense / scattered classes); normalize to observation envelopes.
  - *Done when:* works for at least one fire that has products; absence degrades gracefully with a provenance note. → *dep: A1*

### Workstream C — Weather timeline
- **C1. Weather timeline schema v0 (mini-ADR).** Define the format future 4.1 consumes: regular time steps; gridded fields (low-res grid over AOI, SI units, documented staggering) + station series (location, obs); provenance of source per step. Explicitly versioned.
  - *Done when:* ADR merged with example files; schema validated; a "this will be renegotiated by Epic 4.1" note is in the ADR. → *dep: none*
- **C2. HRRR adapter.** herbie-based fetch of configured variables, AOI-cropped, analysis-preferred/forecast-fallback per D4; resample to timeline grid/steps.
  - *Done when:* Teanaway week assembles a gapless gridded timeline; cache hit on re-run; total stored bytes sane (<~100MB/fire-week). → *dep: C1*
- **C3. RAWS/Synoptic adapter.** Stations within/near AOI; pull obs for window (wind, temp, RH, fuel moisture where present); QC flags passed through.
  - *Done when:* known RAWS near Teanaway appear with plausible series; token handled per secrets pattern. → *dep: C1*
- **C4. Timeline assembly + QA.** Merge gridded + station into `timeline.v0.parquet`; gap report; station-vs-grid sanity comparison (wind speed bias plot).
  - *Done when:* one artifact per incident; QA page renders; gaps explicit rather than interpolated silently. → *dep: C2, C3*

### Workstream D — Progression derivation (the keystone)
- **D1. Arrival-time algorithm v1 (spec then code).** Spec: per-cell first-burn time from the ordered perimeter series — cells gain arrival between consecutive perimeters by distance-weighted interpolation (front marches from old ring to new); `hotspot_assist` tightens timing where detections fall between snaps; cells never un-burn; t0 = discovery or first observation. Confidence raster classes: observed-edge / interpolated / hotspot-inferred.
  - *Done when:* spec doc merged (it's an *interpretation algorithm* and says so); edge cases specified (islands, merging fires, unburned interior holes, perimeter regressions from mapping error). → *dep: B2*
- **D2. Arrival-time implementation.** Spec → `arrival_time.v1.cog.tif` + confidence on the incident AOI canonical grid; deterministic given identical observations.
  - *Done when:* Jolly Mountain arrival raster's isochrone contours visually match its published progression map; golden quantized-hash test in CI on fixture data. → *dep: D1, A2*
- **D3. Fire-state tiling.** Arrival raster → time-indexed fire-state tiles through the standard tiler (burned-by-time queryable per tile); manifest layer entries.
  - *Done when:* tiles align with terrain/fuel tiles by construction; a "state at time t" query needs only tile-local data. → *dep: D2*

### Workstream E — Historic ingestion & packaging
- **E1. Perimeter history + ICS-209-PLUS adapters.** Pull historic perimeters (NIFC history dataset) and sitrep series (209-PLUS) for a named fire; map into the same observation envelopes as live sources (D7: capture resource/containment/cost series).
  - *Done when:* Jolly Mountain assembles from historic sources through the identical downstream path as a live fire. → *dep: A1*
- **E2. Scenario bundle v0.** Bundle manifest implementation: pulls incident record, observation refs, derived refs, weather timeline, Epic 1–2 world pin into one validating package; `terrain incident` command wires A–E end-to-end for both `--irwin` and `--historic`.
  - *Done when:* both command forms produce validating bundles; bundle loads with zero network access afterward (self-contained given the store). → *dep: A2, B*, C4, D3, E1*
- **E3. Flagship packages.** Produce the D6 flagship fires deep: verified perimeter series, arrival raster eyeballed against published progression, weather cross-checked against the fire's documented wind events, 209 series attached.
  - *Done when:* each flagship has a short QA memo (what matches reality, what's dubious); Brad sign-off — local knowledge is the test harness here. → *dep: E2*
- **E4. MTBS severity reference (light).** Attach MTBS burn-severity raster to historic bundles where published, as reference layer only.
  - *Done when:* present + aligned for flagships; absent cleanly elsewhere. → *dep: E2*

### Workstream F — Orchestration, QA, docs
- **F1. Refresh semantics.** `terrain incident --refresh`: idempotent re-fetch (observations append-only, unchanged content skipped), derived products rebuilt only when observations changed; dry-run; structured logs. No daemons, no schedules (Epic 9 wraps this).
  - *Done when:* refreshing an unchanged incident does ~zero work; a new perimeter triggers exactly the derived rebuilds and nothing else. → *dep: E2*
- **F2. QA report.** Per-incident page: observation timeline chart, perimeter series small-multiples, arrival isochrones over hillshade, hotspots-vs-perimeter agreement, weather gap/bias plots, bundle validation status.
  - *Done when:* the D1-review question "do we believe this progression?" is answerable from the report alone. → *dep: D2, C4*
- **F3. Fixtures + CI.** Committed fixture observation sets (small, anonymized-not-needed — it's public data) driving the full historic path in CI; determinism tests per Epic 1's quantized-hash convention.
  - *Done when:* CI runs incident assembly + arrival derivation end-to-end offline. → *dep: E2*
- **F4. Docs.** Adapter/key setup, incident command quickstart, arrival-algorithm explainer (with its honesty caveats), bundle format reference, ADR index updates.
  - *Done when:* a new agent produces a bundle for a fire of their choosing from docs alone. → *dep: E2*

---

## 7. Execution order

**Phase 1 — One historic fire, end to end (the spine):** A1 → E1 (historic adapters first — no keys, stable data, CI-friendly) → A2 → B2-cleaning stage → D1/D2 → E2 minimal bundle. Jolly Mountain assembling into an arrival raster is the epic's Ember-wide unlock: it's 4.3's first input and 5.5's first demo.

**Phase 2 — Live sources:** B1/B2 live, B3 FIRMS, C1–C4 weather, B4 NIROPS best-effort. Now `--irwin` works on the current season.

**Phase 3 — Hardening + flagships:** D3 tiling, F1 refresh semantics, F2/F3 QA + CI, E3 flagship sign-offs, E4, F4.

**Agent notes:**
- Epic 2 is marked complete but **not deeply verified**. Before relying on it: run Epic 2's own F2 QA report for the incident AOIs and sanity-check fuel/canopy layers — this epic's A2 is the first external consumer and will inherit any latent misalignment. Treat surprises as Epic 2 bug tickets, not local workarounds.
- Verify current WFIGS/FIRMS/Synoptic endpoint shapes before coding adapters; federal feeds get reorganized (attribute renames, service moves) more often than their data changes.
- Perimeter data is *messier than the schema implies* — expect invalid geometry, timestamp lies, and republished duplicates. The cleaning stage (B2) and the immutable-observations rule exist for this; budget real time there.

---

## 8. Confirm before starting (⚑)

1. **D1 — arrival-time raster (+confidence) as the canonical derived progression**, over immutable vector observations. OK?
2. **D2 — weather timeline schema v0 authored in this epic** via mini-ADR, renegotiable by future 4.1. OK?
3. **D6 — flagship fires:** Jolly Mountain 2017 + Schneider Springs 2021 (+optional third)? Name your picks — local knowledge is the QA harness.

---

## 9. Notes for later stories

- **4.3 (playback driver):** replays `arrival_time` + confidence through the model interface; bundle manifest is its input contract.
- **4.1 (fire model interface):** inherits weather timeline v0 (C1's ADR) as a starting point, with explicit license to renegotiate; arrival rasters become validation targets for the default model ("does v-game roughly reproduce observed progressions?").
- **5.5 (IRWIN-to-flyover):** `terrain incident --irwin` is its entire backend; minutes-from-ID-to-bundle is the KPI's denominator — log stage timings now (F1) so the KPI is measurable later.
- **Epic 6 (game):** 209-derived resource/containment/cost series (D7) seed the resource system's reality checks and the historic-season scenarios.
- **Epic 9:** 9.3 wraps F1's refresh command on a schedule; 9.4's live delta feed streams what F1 computes; observation immutability + content addressing here are what make those diffs cheap.
