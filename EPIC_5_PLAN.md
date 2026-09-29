# Epic 5 — Rendering & Visualizer (Unreal Engine) — Execution Plan

**Parent:** WILDFIRE_DESIGN.md → Epic 5 (stories 5.1–5.6)
**Status:** Plan v0.1 — ready to task out after ⚑ confirmations
**Scope owner:** Brad / BNE Games LLC
**Predecessors:** Epics 1–3 (data spine), Epic 4 (sim core, state stream + replay schemas) — Epic 5 consumes only Epic 4's published schemas, never its internals.

---

## 0. Framing

This epic puts eyes on everything. It builds the UE frontend that streams the world (Epics 1–2), plays fire state (Epic 4 state streams — live model or Epic 3 playback), and produces the two visible products' shared foundation: the interactive viewer/game shell and the **Visualizer v0** (story 5.5: IRWIN ID → rendered flyover, the wedge).

It also has an unusual second mandate, per Brad's constraints:

1. **The agent iteration loop is a deliverable, not a workflow accident.** Agents must be able to *build → cook/run → capture frames + telemetry → evaluate → iterate* end-to-end from the command line. Workstream A builds that harness first; nothing else starts until the loop closes (HCP0).
2. **No interactive editor.** All engine work happens through command-line entry points: UnrealBuildTool for compiles, `-game` mode and cooked builds for running, and **headless editor commandlets** (`UnrealEditor-Cmd -run=pythonscript`) for the rare asset that must exist as a `.uasset`. The distinction that makes this workable: *the editor binary in headless/commandlet mode is allowed; the editor UI is not.* Assets are generated from checked-in scripts (§3) so text remains the source of truth.
3. **Checkpoints are human-reviewed.** Epic 4's checkpoints could gate on hashes and property tests; this epic's core questions — *does it read? does it look like Teanaway? would a news producer put this on air?* — are aesthetic and only Brad can answer them. Every HCP produces a review bundle (MP4s, contact sheets, perf CSVs, a one-page memo) and **waits for explicit sign-off**. Agent self-evaluation (image diffs, scene-fact probes, model-eyes review) exists to *converge before* asking for human time, not to replace it.

```
            ┌────────────────────────────────────────────────────────────┐
            │                      UE PROJECT (C++)                      │
            │                                                            │
 tile store │  Runtime World Streamer          Fire State Player         │
 (E1/E2) ──▶│   terrain meshes ◀── LODs         (E4 state stream /       │
 bundles    │   veg instancing ◀── scatter       replay files)           │
 (E3) ─────▶│   (E2 spec, C++ port,                │                     │
 state ────▶│    golden-vector conformant)         ▼                     │
 streams    │  Materials/Niagara = seed pack    Fire/smoke/scar VFX      │
 (E4)       │  assets, parameter-driven (MIDs)     │                     │
            │                                      ▼                     │
            │  Overlay/Annotation layer      Camera system + Sequencer   │
            │        └──────────────┬──────────────┘                     │
            │                       ▼                                    │
            │   Run modes: interactive (-game) │ batch render (MRQ CLI)  │
            └───────────────────────┬────────────────────────────────────┘
                                    ▼
            Agent Iteration Harness (Workstream A): build / run-scenario /
            capture (PNG+MP4+CSV+scene-facts JSON) / diff vs goldens /
            contact sheets / HCP bundle assembly → Brad reviews
```

**Load-bearing ideas:**

- **Runtime streaming, no per-AOI cooking** (D3 ⚑): the app reads the Epic 1–3 tile store and bundle formats directly at runtime. One ingestion path serves the interactive game, the visualizer, and — later — Epic 9's CDN tiles. Cook-per-fire would poison the 5.5 KPI (minutes from IRWIN to video) and fork the code.
- **Assets-from-code** (§3): the seed pack of unavoidable `.uassets` (master materials, Niagara systems, tree meshes) is *generated* by version-controlled Python commandlet scripts. Everything else is C++ + runtime parameters + store data.
- **Probes before vibes**: every scenario run emits machine-checkable "scene facts" (tile/LOD states, instance counts vs Epic 2 expectations, draw calls, VRAM, frame times) alongside images. Agents iterate against facts and golden-image diffs; model-eyes screenshot review is the tie-breaker, not the foundation — general VLMs are strong at "does this look wrong" and weak at precise counting/measurement, so the harness never asks them to count.
- **The debug harness from Epic 4 is the requirements crib, not the codebase.** Nothing 2D-matplotlib survives into UE; the split-screen/HUD *ideas* do.

---

## 1. Scope (in / out)

**In scope**
- **A. Agent iteration harness** (build/run/capture/evaluate CLI + HCP bundle tooling) — the enabling deliverable.
- **B. Asset generation system**: Python-commandlet framework + the seed pack (master materials, Niagara fire/smoke/ember systems, procedural conifer meshes v1, UI fonts/materials).
- **5.1** Runtime world streaming: store reader (C++ port of tile/manifest formats), terrain mesh generation with LODs, slope/aspect/landcover material blending, vegetation instancing via a **golden-vector-conformant C++ port of the Epic 2 scatter spec**.
- **5.2** Fire state rendering: state-stream/replay ingest, fire front + intensity presentation, burned scarring, ember/heat glow, wind-driven smoke plume (bounded art scope — legibility first, per design doc).
- **5.3** Camera system: orbit, chase-the-front, authored flyover paths (Sequencer), timeline scrubbing; interactive shell app.
- **5.4** Overlay/annotation layer: perimeter history, containment, labels, evac-zone display, scale cues, clock/wind HUD — the broadcast vocabulary.
- **5.5** Visualizer v0: one command, IRWIN ID → (Epic 3 bundle) → (Epic 4 playback state stream) → staged UE render → MP4. KPI instrumented end to end.
- **5.6** Export v1: Movie Render Queue batch renders (MP4/PNG sequence); packaged-interactive-scene format deferred.
- Performance budgets + tracking for both run modes.

**Explicitly out of scope**
- Gameplay: input beyond camera/scrub, resource UI, command issuing — Epic 6. (The state player renders suppression *effects* present in streams; it does not author commands.)
- Photoreal/per-tree fidelity chasing; licensed asset-store content beyond what §3's human-in-the-loop task explicitly acquires.
- National-scale map view (cartographic mode) — Epic 6's national layer; this epic renders incident-scale AOIs.
- Live-feed tailing during render — visualizer renders from bundles (Epic 3's refresh command updates them; Epic 9 automates that later).
- Web/streamed viewer, packaged interactive distribution — later 5.6 iteration.
- Engine source modifications (D1 keeps us on a binary engine unless forced; any exception is an ADR).

**Definition of done for Epic 5:** All HCPs signed off by Brad; `ember-render --irwin <id>` (or `--historic jolly-mountain-2017`) produces a broadcast-plausible MP4 hands-free with stage timings logged against the KPI; the interactive shell runs the same scene at budget on the reference GPU; every `.uasset` in the repo regenerates from scripts; an agent can take a small visual task (e.g., "make burned scar darker at high intensity") from prompt to reviewed contact sheet without a human touching the editor.

---

## 2. The iteration loop (Workstream A — build this first)

**Toolchain entry points** (Windows, per D2):
- **Build:** `Build.bat <Target> Win64 Development` (UBT incremental — the inner loop, seconds-to-a-minute); `RunUAT.bat BuildCookRun -cook -stage -pak` for cooked checkpoint builds (outer loop, minutes).
- **Run (iteration):** uncooked `-game` mode with `-ExecCmds` scenario scripts — no editor UI, fast startup, live content from the store.
- **Run (headless capture):** `-game -RenderOffscreen -unattended -nosplash` for CI-style capture on the GPU runner; every scenario script ends in deterministic camera bookmarks → `HighResShot`/screenshot requests → `quit`.
- **Batch video:** Movie Render Queue via its command-line executor in `-game` mode (sequence + preset + output dir as args) — the same path is story 5.6's product output.
- **Assets:** `UnrealEditor-Cmd <proj> -run=pythonscript -script=<gen.py>` (§3).
- **Telemetry:** CSV profiler capture (`-csvCaptureFrames`), custom scene-facts dump (JSON: loaded tiles, LOD histogram, instance counts per species, draw calls, VRAM, tri counts) written by a `USceneFactsSubsystem` at capture points.

**The loop contract (what an agent actually runs):**
```
ember-dev build                          # UBT wrapper, parsed errors
ember-dev run-scenario S_terrain_orbit   # scenario = data-driven: world ref, replay ref,
                                         #   camera bookmarks/paths, capture list, budget tags
ember-dev evaluate S_terrain_orbit       # image-diff vs goldens (perceptual + per-region),
                                         #   scene-facts assertions, perf-budget check,
                                         #   contact sheet + verdict JSON
ember-dev bundle HCP1                    # assemble review bundle for Brad
```
Scenario definitions, camera bookmarks, golden images, and budgets are all repo files — adding a test view is a data change. Verdict JSON is the agent's feedback signal; images are attached for model-eyes review where facts can't decide (aesthetics, artifacts, "does the smoke look like smoke").

**Infrastructure (D2 ⚑):** one **Windows + discrete-GPU runner** with the pinned engine installed is the render/capture host — agents drive it via the local coding-agent session; a cloud/non-GPU agent may edit code and even compile, but anything that opens an RHI runs on the runner. HCP bundles land in a synced review folder so Brad can review MP4s/contact sheets from anywhere.

**Human-review protocol:** each HCP bundle = MP4(s) + contact sheet PNGs + perf CSV summary + one-page memo (what changed, what the agent already fixed, open questions with A/B images). Brad replies with sign-off or notes; notes become tickets; the HCP re-bundles. No HCP auto-passes.

---

## 3. Assets-from-code (Workstream B — the editor-avoidance strategy)

The honest constraint: a few things in UE want to be `.uassets` — material graphs, Niagara systems, static meshes with build settings (Nanite), fonts. Strategy:

- **Generator scripts are the source of truth.** `assets/generators/*.py`, run headless via commandlet, create/update the `.uassets` deterministically (`unreal.AssetTools`, `MaterialEditingLibrary`, Niagara scripting API, static-mesh-from-mesh-description builders). `.uassets` are committed for build speed but treated as build artifacts: `ember-dev regen-assets` must reproduce them; CI verifies regeneration cleanliness.
- **The seed pack (small, enumerated):**
  1. `M_Terrain` master material — heightmap-displaced or vertex-driven, blends by slope/aspect/landcover/greenness textures streamed from the store; all knobs are parameters.
  2. `M_Veg_*` masters — leaf/bark with wind sway params, intensity-driven scorch.
  3. Niagara: `NS_FireFront` (intensity-classed flame cards/volumes), `NS_Smoke` (wind-field-driven plume), `NS_Embers`; all parameter-driven from sim state.
  4. Procedural conifer/shrub/grass meshes v1 — generated static meshes (LOD chain, Nanite-enabled at generation time), species palette keys matching Epic 2 packs.
  5. HUD/overlay materials + fonts.
- **Runtime rule:** gameplay/render code touches assets only through Material Instance Dynamics and Niagara user parameters. New looks = new parameters or new generator code — never a hand-edited asset.
- **Escape hatch (logged):** if a specific asset genuinely can't be script-authored at acceptable quality (possible for tree meshes), the fallback is a *single human acquisition/authoring task* (Fab/Megascans pull or one editor session), recorded in an ADR with the asset checked in and its provenance documented. Budget: ≤ 1 such session per checkpoint phase, ideally zero.
- **Licensing note:** any acquired content must be UE-project-licensed (Fab/Megascans terms); agents never scrape meshes/textures from the web.

---

## 4. Load-bearing decisions (defaults set; ⚑ = confirm)

| # | Decision | Recommendation | Rationale / consequences |
|---|---|---|---|
| D1 ⚑ | Engine version & flavor | **Pin the current stable UE 5.x binary (launcher/installed) release at kickoff; agent verifies latest at start; no source build unless an ADR forces it** | Binary engine keeps the runner setup reproducible and compile times sane; source build is a big hammer we haven't needed. Pin exact version in repo; upgrades are deliberate ADR events. |
| D2 ⚑ | Runner infrastructure | **Dedicated Windows + discrete-GPU machine as the sole render/capture host** (Brad's dev box or a standing GPU VM — name it) | Everything visual needs an RHI; cloud coding agents without GPUs get code-only tasks. Which machine, and whether it's always-on for agent access, is Brad's call. |
| D3 ⚑ | World ingestion | **Runtime streaming from the store/bundles; zero per-AOI cook** | See §0. Consequence: terrain meshes are built at runtime (D4) and the store readers get a C++ port (shared later with Epic 6). |
| D4 | Terrain mesh approach | **v1: runtime-generated tile meshes (CPU heightmap → mesh, LOD ring scheme, skirt/apron stitching per Epic 1 rules); UE Landscape system not used** | Landscape wants editor-authored actors and fights custom tiling. Runtime meshes are editor-less and store-native. Nanite terrain / HLOD optimization = deferred investigation ticket, not v1. |
| D5 | Vegetation rendering | **Seed-pack Nanite static meshes instanced at runtime (HISM/ISM per tile × species) from the C++ scatter port** | Nanite handles density; instancing is runtime-friendly; scatter conformance to Epic 2 golden vectors is a hard gate (HCP2) — the same trees in the same places as the reference implementation, provably. |
| D6 | Fire/smoke fidelity posture | **Legibility-first stylization: intensity-classed front, honest scar, wind-coherent plume. No volumetric fluid sim in v1** | Design doc 5.2: read like the IR maps people trust, then spectacle. Volumetrics are an HCP4 stretch experiment only if budget survives. |
| D7 | Timeline authority | The fire state player owns sim time; cameras, Niagara, overlays, and MRQ all slave to it (scrub = seek) | One clock or nothing composites; MRQ renders are deterministic per replay because state is. |
| D8 | Perf budgets | **Interactive:** 60 fps @ 1440p on the reference GPU for a 30×30 km AOI incident view; ≤ 4 GB VRAM. **Batch:** quality caps unlimited, but the 5.5 KPI budget is **≤ 15 min IRWIN→MP4** on the runner (cold bundle) / ≤ 5 min warm | Budgets tracked from HCP1 in every verdict JSON; regressions fail evaluation like a broken test. |
| D9 | Model-eyes reviewer | Screenshot review by a frontier VLM is part of `evaluate` for aesthetic/anomaly triage; verdicts are advisory, never gating; prompts + rubric are repo files | Keeps agent iteration converging between human reviews without pretending VLM judgment is ground truth. |

---

## 5. Human checkpoints (HCP0–HCP7)

Every HCP: review bundle per §2, explicit Brad sign-off, upstream bugs filed as tickets in the owning epic. Sequence is strict; parallel work may run ahead at risk but nothing merges past an unsigned HCP.

| HCP | Name | What Brad is judging | Bundle contents | Gate |
|---|---|---|---|---|
| **HCP0** | *The loop closes* | The agent workflow itself | Screen-free proof: build log → gray-shaded Teanaway terrain run → captures → an *intentionally broken* change caught by evaluate → fixed → green. Memo documents the full loop timing | Loop runs end-to-end hands-free; inner-loop (edit→verdict) ≤ 10 min; Brad trusts the harness enough to review bundles instead of screens |
| **HCP1** | *Terrain reads true* | Materials, lighting, scale honesty | Orbit MP4s (dawn/noon/dusk sun angles), contact sheet of bookmarks, side-by-side vs Epic 1 QA hillshades, perf CSV | Landforms recognizable; no seams/cracks between tiles/LODs; slope/aspect/landcover blending plausible; 60 fps budget met on terrain alone |
| **HCP2** | *The forest is real* | "Does Teanaway look like Teanaway" | Scatter-conformance report (C++ vs Epic 2 golden vectors — must be exact), density flyover MP4, ground-level + ridge bookmarks, species distribution stats vs Epic 2 E4 expectations | Conformance exact; densities/heights read right to someone who's stood there; wind sway not distracting; budget holds with full instancing |
| **HCP3** | *Fire replays in 3D* | The core product moment — first time a real fire burns in the renderer | Jolly Mountain playback MP4 (whole-fire timelapse + front close-up), split vs Epic 4's 2D CP2 animation, scar progression stills | Front position matches the state stream (probe-verified); scar accumulates correctly; nothing burns that the stream says didn't; goosebumps optional but likely |
| **HCP4** | *The fire reads* | Legibility of the art (design doc 5.2's ordering: IR-map trust, then spectacle) | Intensity-class A/Bs, ember/spot moment captures, smoke plume vs wind-vane overlay MP4, optional night pass, perf CSV under full VFX | A viewer can point at the screen and say where it's hot, where it's heading, what's already lost; plume direction never contradicts the HUD wind; budget holds |
| **HCP5** | *Broadcast layer* | Would a producer air this / would a PIO trust it | Overlay permutation contact sheets (perimeters+history, containment, labels, evac zones, scale cues, clock), one fully-dressed timelapse MP4, comparison against a WFCA-style 2D map of the same fire | Overlays legible at 1080p broadcast scale; nothing misleads (disclaimer chyron present per design-doc 8.4 posture); toggle system works per-render |
| **HCP6** | *The camera speaks* | Feel — the first hands-on-controller checkpoint | Interactive session on the runner (Brad drives): orbit/chase/scrub; plus three authored-flyover MP4s from Sequencer paths defined in data | Scrubbing is instant-feeling; chase-the-front frames the story; authored paths are producible by editing a data file, no editor |
| **HCP7** | *Visualizer v0 ships* | The wedge product, end to end | `ember-render --irwin <current-season-id>` cold-run MP4 + stage-timing report vs KPI; `--historic jolly-mountain-2017` companion; a "release checklist" memo (disclaimers, provenance slate, output specs) | ≤ 15 min cold KPI met; output judged shareable-quality; Brad would show it to a stranger in fire country |

HCP3 is the demo that proves the whole company thesis — data spine → sim interface → renderer on a real fire. Expect to reuse its MP4 in every conversation about this project for a year.

---

## 6. Workstreams & tasks

### Workstream A — Iteration harness (§2)
- **A1. `ember-dev` CLI**: build wrapper (parsed UBT errors → structured), run-scenario, capture collection, evaluate (image diff: perceptual metric + per-region masks; scene-facts assertions; budgets), bundle assembler. Scenario/bookmark/golden formats documented.
  - *Done:* HCP0 bundle produced by the tool itself. → *dep: C1 minimal (gray terrain) for the proof run*
- **A2. Scene-facts subsystem** (UE side): JSON dump of tiles/LODs/instances/perf at capture points; console-command triggerable.
  - *Done:* facts assert in evaluate; documented schema. → *dep: project skeleton*
- **A3. Model-eyes review step** (D9): rubric-driven VLM screenshot triage with verdict-JSON integration; advisory tags only.
  - *Done:* runs in evaluate; rubric in repo; false-positive rate noted after HCP1 use. → *dep: A1*
- **A4. Runner provisioning doc + smoke test**: engine pin install, SDKs, ffmpeg, scheduled availability; `ember-dev doctor`.
  - *Done:* second machine could be provisioned from the doc alone. → *dep: D1/D2 confirmed*

### Workstream B — Asset generation (§3)
- **B1. Commandlet framework**: `regen-assets` runner, per-generator manifests, regeneration-cleanliness CI check.
- **B2. Seed pack v1**: terrain + veg masters, Niagara fire/smoke/embers, HUD mats. Parameter surfaces documented per asset.
- **B3. Procedural conifer/shrub/grass meshes**: generated LOD'd, Nanite-enabled species set keyed to the Epic 2 palette; silhouette review sheet for HCP2.
- **B4. Escape-hatch protocol**: the ADR template + provenance rules for any human-acquired asset.
  - *Done (stream):* every repo `.uasset` regenerates; HCP1/2/4 consume only pack assets. → *dep: A1 skeleton; B3 informs HCP2*

### Workstream C — World streaming (5.1)
- **C1. Store/bundle readers (C++)**: tile/manifest/layer formats + Epic 3 bundle + Epic 4 state-stream/replay parsing; shared module engineered for Epic 6 reuse; async IO.
- **C2. Terrain tiles → runtime meshes**: heightmap mesh builder, LOD rings + stitching per Epic 1 apron rules, material binding (M_Terrain + streamed texture layers: landcover, greenness, hillshade-assist).
- **C3. Streaming manager**: camera-driven load/evict, budget-aware, facts-instrumented.
- **C4. Scatter port**: Epic 2 spec in C++, **golden-vector conformance test in CI**, per-tile HISM/ISM population with species → mesh/material mapping.
  - *Done (stream):* HCP1 (C1–C3) and HCP2 (C4) gates. → *dep: A2, B2/B3*

### Workstream D — Fire state rendering (5.2)
- **D1. State player**: stream/replay → per-tile fire state textures over time; the D7 master clock; seek/scrub.
- **D2. Front + scar presentation**: intensity-classed front treatment on terrain material, progressive scorch/scar layers, arrival-history tinting option.
- **D3. Niagara wiring**: fire/smoke/embers driven by state textures + weather (wind field from the timeline); spotting-event moments surfaced.
- **D4. Wind/plume coherence pass**: plume advection matches HUD wind; sanity probes (plume-vs-wind angle fact).
  - *Done (stream):* HCP3 (D1–D2 minimal + smoke v0), HCP4 (D2–D4 full). → *dep: C-stream, B2*

### Workstream E — Overlays & annotation (5.4)
- **E1. Overlay framework**: data-driven layer registry (perimeters/history, containment, labels, evac zones, scale bar, north, clock/wind HUD), per-render toggle config.
- **E2. Broadcast dressing**: title/locator slates, provenance + disclaimer chyron (text from design-doc 8.4 posture), safe-area compliance.
  - *Done (stream):* HCP5 gate. → *dep: C1 (vector layers), D1 (state for containment)*

### Workstream F — Cameras, sequencer, interactive shell (5.3)
- **F1. Camera rigs**: orbit, free, chase-the-front (front centroid/heading from state facts); input mapping for the shell.
- **F2. Data-driven flyovers**: path/keyframe definitions in scenario files → Sequencer at runtime; bookmark system shared with A1 captures.
- **F3. Interactive shell**: minimal front-end (load scenario, scrub, camera switch, overlay toggles) — the future Epic 6 host, kept deliberately thin.
  - *Done (stream):* HCP6 gate. → *dep: D1 clock, E1 toggles*

### Workstream G — Visualizer v0 & export (5.5/5.6)
- **G1. MRQ batch path**: sequence + preset via CLI; ffmpeg mux; output spec (1080p/4K presets).
- **G2. `ember-render` orchestrator**: IRWIN/historic id → Epic 3 `terrain incident` (bundle) → Epic 4 playback (state stream) → scenario synth (auto flyover framing the fire extent) → MRQ → MP4 + provenance sidecar; stage timings vs KPI.
- **G3. Auto-framing heuristics**: fire-extent-aware default shots (establish, timelapse, front close, final scar) so untouched output is watchable.
  - *Done (stream):* HCP7 gate; KPI instrumented per design doc 5.5. → *dep: everything prior; G1 early for HCP bundles*

### Workstream H — Perf & docs
- **H1. Budget tracking** (D8) in evaluate from HCP1; VRAM/frame-time regressions fail verdicts.
- **H2. Docs**: runner setup, harness guide, asset-generator guide, scenario/flyover authoring, visualizer runbook. *Done:* an agent adds a new overlay layer from docs alone.

---

## 7. Execution order

- **Phase 0 — HCP0:** A1/A2/A4 + C1/C2 minimal (gray terrain) + B1 skeleton. The loop, proven.
- **Phase 1 — HCP1:** B2 (terrain master), C2/C3 full, H1 on. Then **HCP2:** B3, C4.
- **Phase 2 — HCP3:** D1/D2 + smoke v0 (B2 Niagara), G1 early (bundles want MP4s anyway). Then **HCP4:** D3/D4 art pass.
- **Phase 3 — HCP5:** E1/E2. **HCP6:** F1–F3.
- **Phase 4 — HCP7:** G2/G3, H2, release-checklist memo.

Two-track note for agent teams: B (assets) and C (streaming) parallelize after HCP0; D cannot start honest work before C2 exists but can develop against a synthetic flat-world state stream (reuse Epic 4's B2 synthetic kit through the C1 reader) — same trick as Epic 4's two-track phase.

---

## 8. Confirm before starting (⚑)

1. **D1 — binary engine, exact version pinned at kickoff** (agent verifies current stable then; upgrades are ADRs). OK?
2. **D2 — which Windows+GPU machine is the runner**, and is it agent-accessible on a schedule or always-on?
3. **D3 — runtime streaming, zero per-AOI cook** — this shapes C-stream and the KPI. OK?
4. **HCP cadence** — sign-off is async via review bundles; want a standing weekly review slot instead/in addition?

**Confirmed 2026-09-27 (Brad):**
- D1: UE **5.8.3** binary (CL 58210709, `C:\Program Files\Epic Games\UE_5.8`), pinned; upgrades are ADR events; no source build.
- D2: the dev box is the runner and the D8 reference GPU — RTX 4080 SUPER 16 GB, i7-14700K, 32 GB RAM. On-demand: agents use it when a session is open here; not always-on.
- D3: runtime streaming from the store/bundles; zero per-AOI cook.
- HCP cadence: async review bundles, explicit sign-off per HCP; no standing slot.
- **D10 (added at kickoff) — world source.** Upstream reality check: the only tiled Epic 1/2
  region is `teanaway_dev` (1.44 × 1.44 km @ 10 m, 14 tiles, 74,595 scattered trees); Jolly
  Mountain exists only as a 30 m Copernicus bake + `.ewp`; there is no imagery; the Epic 2
  scatter "spec" is the code (`terrain/veg/{hashing,scatter}.py`) + `tests/golden/scatter_pnw.json`.
  Decision: C1 reads **Terrain's tile store** (manifest v2, per-tile uncompressed TIFFs) as the
  canonical render world; `.ewp` is read only for fire-state grid alignment. Phase 0 runs on
  `teanaway_dev`. Phase 1 bakes (a) a larger Teanaway region (~10 × 10 km @ 10 m) for the
  HCP1/HCP2 orbits and (b) the Jolly Mountain AOI through Terrain's tiler (10 m where 3DEP
  covers it, else 30 m) for HCP3. The C++ scatter port's conformance oracle is the golden JSON
  plus the full `teanaway_dev/veg/instances.npy`.

---

## 8b. Direction from HCP1 review (Brad, 2026-09-28)

- **D11 — LOD / world-context plan (to write before HCP5–HCP7).** Today's renderer streams one
  AOI's quadtree and stops at its edge (a "diorama cliff"). The target is a **Google-Maps-like
  LOD system**: continuous zoom from regional context down to the fire line, with the world
  continuing past the incident AOI at progressively coarser resolution (e.g. a 30 m / 90 m
  surround from national DEM + land cover, then cartographic far-field). Output is a written
  plan: tile pyramid spanning AOI + surround, per-LOD data sources, streaming budget, handoff
  to Epic 9's CDN tiles. It also covers the region-edge treatment (HCP1 Q3).
- **Look:** map-like fuel-class colouring is accepted for now. Photographic imagery is not
  requested.
- **Trees:** eventually render actual trees (beyond B3's generated primitives); an HCP2+
  concern, tied to the LOD plan's near-field tier.
- **Lakes:** water plane now (HCP1 Q2); U7 (hydro-flattened DEM upstream) stays open as the
  long-term data fix.

## 8c. HCP2 review, round 1 (Brad, 2026-09-28) - not signed off

- **Density reads too dense; species mix wrong** (pine should be rare here; it is 36 % everywhere
  because the palette maps every forest EVT to one DF 5 : PP 4 : GF 2 mix). Decision: **Ember edits
  Terrain directly** - per-EVT species mixes in the palette, and a stand-structure scatter
  (fewer canopy trees, heights under the CHM ceiling, crowns scaled to tree size; closes U9).
  The C++ port stays exactly conformant (golden vectors + instances.npy oracle regenerated).
- **Wind not visible** under a moving camera: locked-off sway clip, stronger motion, secondary
  branch-tip flutter.
- **Close-up trees cartoonish - fix before moving on.** Decision: **hybrid B3 v2** - tree
  geometry generated from code (branch skeletons, needle-clump cards, species-specific), with a
  small licensed set of photo-scanned bark/needle textures under a B4 ADR (licence + provenance).
- **Budget/LOD:** 4 GB stays; the radius is not the only lever. Vegetation tiers: near (~0.8 km,
  full trees, wind, live shadows), mid (to ~6 km, per-species octahedral impostors, cached
  shadows), far (no instances; canopy colour + canopy height in the terrain mesh), context (D11).
  Frame/VRAM budget table per system (terrain, vegetation, fire/smoke, weather, post, headroom)
  enforced in evaluate as systems land.
- Order: Terrain species + stand structure -> B3 v2 trees -> vegetation tiers + budgets -> wind
  clip -> HCP2 re-bundle. D11 written alongside the tiers: `docs/viz/D11-lod-world-context.md`
  (draft 2026-09-28; tree source / variation strategy is OPEN: more generated variants, modular
  crown sections (Brad's suggestion, not a decision), or SpeedTree / library trees via B4).

## 8d. HCP2 signed off (Brad, 2026-09-28, round 2)

"The wind sway looks excellent. The forest looks really good now ... I think we're good for trees
for now. We'll come back to the variation issue another time." Flyover vertical jitter fixed
(smoothed altitude). Next direction: the ground plane - better terrain texturing plus ground
cover (low vegetation, rocks, duff); it will need its own line in the budget.

**Ground plane decisions (Brad, 2026-09-28):** HCP3 first; the ground comes right after, so fire
char/ash and ground detail land in one terrain-material pass. Ground textures procedural from code
until licensed scans (with bark). Direction: FBFM40 fuel model drives ground detail (TL litter/duff
and logs, TU understory, GR/GS/SH grass and shrub, NB rock/snow) plus near-only ground-cover
instances; own budget line (~1.5 ms), terrain toward Nanite to make room.

**Sequencing after HCP3 (Brad, 2026-09-28):** finish Jolly (HCP3), then **U6 bounded-memory
chunked ingest** (docs/upstream/U6-bounded-memory-ingest.md) BEFORE the next fire. Runtime
streaming already works tile by tile; the bake does not (whole AOI in memory, ~12 GB peak at
~500 km2), and separately baked regions cannot be stitched without seams (edge context, per-region
height quantization, LOD pyramids). U6 bakes one shared tile grid in haloed chunks with a ledger,
so regions grow/resume seam-free. Next fire after U6: **BIG GRASS 2026** (IRWIN
{6B0C72B3-0E12-4695-9022-E1113C0AA8D1}), Owyhee sage/grass rangeland, 83 x 103 km, the
rangeland contrast. Prepared: `viz/worlds/big_grass_2026.terrain.toml` (a tile-aligned
25.6 x 19.2 km window, pre-fire LF2025, vegetation off). Still needed: Great Basin veg palette
(sagebrush-steppe EVTs), and fire timing - we hold one final perimeter only (arrival raster all
zero), so a replay needs IR/FIRMS progression (Epic 3).

## 8e. HCP3 signed off (Brad, 2026-09-28)

"Yes HCP3 is signed off, lets knock out U6." Bundle `checkpoints/HCP3` (Jolly Mountain 2017:
S_jolly_fire 12 captures, 71/71 checks incl. exact burned/burning counts and probes vs the Epic 4
stream; timelapse, 2D|3D split, scar stills, ground-level baseline). Carried to HCP4 / the ground
plane: flame intensity variety (the uniform orange carpet), ground-level fire look (flames read as
paint, marbled char, burned stands keep crowns), smoke near the camera, wind from a real field,
exposure. Capture moved to NVENC (13 -> 3 min per Jolly run). Next: U6.

## 8f. Ground plane v1 (started 2026-09-29) - then a simple fly camera

Brad (2026-09-29): "lets work on terrain, after that I'll want the simple camera and the ability to
launch the client with a scenario and fly around in it. As you work, consider more opportunities to
optimize our pipeline for speed." Upstream done first: U11/U12 (canopy heights, per-tree ground),
season freeze (no Sentinel re-fetch while iterating), goldens re-blessed (455c337).

Today the ground is the per-tile macro albedo (10 m data, 2.5 m texels) plus brightness noise: at
eye level it reads as flat tan. v1 keeps the macro colour as the far/map look and adds, near the
camera, what the fuel model says is on the ground (8d decisions: FBFM40 drives it, procedural
textures from code until licensed scans, own ~1.5 ms budget line, char/ash in the same pass).

- **GP1 harness: wait for shaders** before the first capture (the cold run after a build captured
  grey fallback materials, 2026-09-29) - reliability and no wasted re-runs.
- **GP2 ground mix** (worldcore/look): a second per-tile texture, RGBA = weights of four ground
  sets - litter/duff (TL, TU, SB), grass (GR, GS, agriculture), rock/scree (NB9, steep slopes),
  shrub soil (SH) - with the same boundary warp and blur as the albedo so they line up. Rules in
  `viz/looks/*.toml`.
- **GP3 detail sets** (assets from code): per set a tiling albedo-variation + normal + roughness
  texture generated in Python (numpy, no scans), 2-4 m repeat, anti-tiling (two scales, rotated).
  M_Terrain: macro colour x detail variation, detail normals, blended by the mix, fading out by
  ~300 m to the macro look (the map look holds from the air).
- **GP4 ground cover**: near-only instances (<= ~80 m): grass tufts, fern and shrub clumps (fall
  colour: vine maple / huckleberry, per the Three Queens field photo), rocks, logs, stumps.
  Deterministic hash placement in worldcore by FBFM40 + canopy cover + slope, like the scatter;
  generated meshes; own tier and budget line.
- **GP5 fire on the ground**: char and ash over the detail sets; burned cover blackened or gone;
  logs and stumps smoulder after the front (field photo: hollow stump burning inside, green crowns
  around it).
- **GP6 review**: `S_ground_tq` / `S_ground_teanaway` close bookmarks (forest floor, meadow, talus,
  shrub, burned ground), budgets, bundle for Brad.
- **Then F1-lite**: a free-fly / orbit camera and `ember-dev play <scenario>` to launch the client
  and fly around (pulled forward from HCP6).

Speed candidates to take along the way (each output-identical, measured): shader wait (GP1); one
UE launch for several scenarios (today each run pays ~40 s of engine start); cache composed per-tile
ground textures on disk keyed by inputs + look (today composed on every load); stage-scoped
Terrain re-runs (`terrain veg` exists; canopy/tile only).

## 9. Notes for later epics

- **Epic 6:** C1 readers, F3 shell, camera rigs, and the state player are its foundation; the command-issuing UI plugs into Epic 4's suppression schema on top of this scene. National cartographic view is new work there, not a HCP5 overlay retrofit.
- **Epic 7:** species→mesh mapping and overlay layer registry are pack-shaped already; generator scripts make community *visual* packs conceivable (documented, not promised).
- **Epic 8/8.4:** HCP5's disclaimer/provenance dressing is the legal posture made visible; reuse its language everywhere public.
- **Epic 9:** C3's streaming manager is the CDN client-in-waiting; keep the reader's IO behind an interface so file:// → https:// is a transport swap.
- **Research surface:** `ember-render --replay <file>` (falls out of G2) renders *any* conformant replay — a researcher's model output becomes a broadcast-grade flyover, which is the visualization-gap pitch from the design doc, delivered.
