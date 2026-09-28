# Epic 4 — Sim Core & Fire Model Interface — Execution Plan

**Parent:** WILDFIRE_DESIGN.md → Epic 4 (stories 4.1–4.8)
**Status:** Plan v1.0 — ⚑ decisions confirmed 2026-09-07 (see §9); **executed 2026-09-07: CP1–CP7 passed** (`checkpoints/CP*/memo.md`; CP7's Linux determinism leg runs in CI on push)
**Scope owner:** Brad / BNE Games LLC
**Predecessors:** Epic 1 (done), Epic 2 (complete, lightly verified), Epic 3 (planned/in flight — CP1/CP2 below are its real verification)

---

## 0. Framing

This is the keystone epic. Everything before it produced *data about the world*; this epic makes the world *run*. It is also, deliberately, the integration test of Epics 1–3: the sim core is the first consumer that reads terrain, fuels, weather, and fire history together and does something with them that a human can watch and judge.

Because of that, this plan is organized around **visual checkpoints (CP1–CP7)** — each one a watchable artifact (animation or interactive view) that proves a layer of the stack, catches upstream data problems while they're cheap, and gives you demo material at every stage. Workstreams exist to serve checkpoints, not the other way around.

```
                    ┌────────────────────────────────────────────────────┐
                    │                SIM CORE (engine-free)              │
                    │                                                    │
  Epic 1–3 stores──▶│  World Loader ──▶ World State (tiles, fuels, wx)   │
  scenario bundles  │                     │            ▲                 │
                    │                     ▼            │ world deltas    │
                    │   ┌─ Fire Model Interface ─┐   ┌─┴──────────────┐  │
                    │   │  default cellular model │   │ Suppression    │  │
                    │   │  playback driver        │   │ Simulation     │◀─┼── commands
                    │   │  (future: external)     │   └────────────────┘  │   (tests now,
                    │   └───────────┬─────────────┘                       │    game later)
                    │               ▼                                     │
                    │   Observers: containment, outcomes, stats           │
                    │               ▼                                     │
                    │   State Stream ──▶ Replay File (pins everything)    │
                    └───────────────┬────────────────────────────────────┘
                                    ▼
                    Debug Viz Harness (2D, disposable, NOT Epic 5):
                    frame renders → MP4/GIF per checkpoint; optional local web viewer
```

**Load-bearing ideas:**

- **Two coupled sims, one deterministic core** (design doc principle): the fire model and the suppression sim exchange state only through the versioned interface's world-delta stream. Gameplay (Epic 6) will issue *commands*; tests issue them now.
- **The interface is the product.** Story 4.1's spec outlives any model implementation. Default model, playback driver, and future research models are all just implementors. If the playback driver feels privileged or the default model reaches around the interface, the epic has failed regardless of how pretty the fire looks.
- **Determinism is a design constraint, not an aspiration** — same discipline as Epic 2's scatter spec: integer/fixed-point state, hierarchical seeded RNG, enumerated draw order, golden vectors, cross-platform CI.
- **Legibility over fidelity** for the default model: players (and demo viewers) must *predict* fire behavior from what they see — steeper is faster, wind stretches the ellipse, grass runs and timber smolders. Ballpark-plausible against real fires is a sanity check, never a validation claim.
- **The debug viewer is disposable by design.** 2D, unstyled, tile-store-native. Its job is truth, not beauty. Epic 5 owns beauty; nothing here may grow into a renderer.

---

## 1. Scope (in / out)

**In scope**
- **4.1** Fire Model Interface v1: schema-first spec (inputs, tick semantics, world-delta stream, outputs, capability flags, determinism contract) as an ADR + machine-readable schemas.
- Sim-side **world loader**: reads Epic 1–3 stores/bundles (tiles, fuels, greenness, weather timeline v0, arrival rasters) into sim-ready arrays; the weather timeline ADR gets co-signed or renegotiated here (Epic 3 D2's promised counterparty).
- **4.3** Playback driver: replays Epic 3 arrival/confidence rasters through the interface.
- **4.2** Default game model v1: cellular, CPU-reference-normative (see D2 ⚑), slope/wind/fuel/moisture response, crowning approximation, stochastic spotting, mid-run world-delta support. Coefficients in versioned pack files.
- **4.5/4.6** Suppression sim v1: command schema; execution of handline, dozer line, retardant drop, water drop, burnout, mop-up along caller-supplied paths; NWCG production-rate pack tables; emitted world deltas.
- **4.7** Observers: containment %, area/perimeter stats, structure outcomes (from Epic 1 structures layer), cost accumulation.
- **4.4** Headless runner + **replay format v1** (commands + seeds + versions + world-manifest pin; optional baked state stream).
- **Debug viz harness**: deterministic frame renderer → MP4/GIF/PNG per run (CI-friendly); optional local web viewer (stretch, see D6).
- Performance budget + benchmark suite; cross-platform determinism CI.

**Explicitly out of scope**
- Epic 5 rendering, UE integration, cameras, VFX. The state-stream format is the only thing Epic 5 may depend on.
- Game UI, AI dispatch, pathfinding for crews — Epic 6. Suppression paths arrive as polylines from the caller.
- Fire-model *scientific* validity; calibration against research models; publication claims. (CP5 is a sanity check with explicit disclaimers.)
- **4.8** Python bindings — parked per design doc; the C API (D1) makes them cheap when demanded.
- Live incident streaming into a running sim — Epic 9 territory; sims run from bundles.

**Definition of done for Epic 4:** All seven checkpoints passed with their artifacts archived; `embersim run scenario.toml` produces a replay that re-simulates bit-identically on Linux + Windows CI; the same scenario with a suppression command script produces a visibly, measurably different outcome; interface + replay + state-stream schemas are versioned documents Epic 5 and Epic 6 can build against without reading sim source.

---

## 2. Load-bearing technical decisions (defaults set; ⚑ = confirm)

| # | Decision | Recommendation | Rationale / consequences |
|---|---|---|---|
| D1 ⚑ | **Core language & shape** | **C++20 static library, engine-free, with a C API boundary; CMake; builds as CLI (`embersim`) now and links into a UE module later (Epic 5/6)** | Matches Brad's expertise for the code he'll live in longest; UE interop is trivial later; C API keeps Python bindings (4.8) and non-UE frontends cheap. Alternative (Rust core + FFI) noted for memory-safety appeal — viable, but adds a toolchain to the UE story for marginal gain here. Python-prototype-then-port rejected: the port *is* the risk. |
| D2 ⚑ | **CPU-normative, GPU-later** (amends design doc 4.2's "GPU-resident") | **The CPU implementation is the normative, deterministic reference. GPU acceleration is a future derived implementation validated against golden vectors.** | Cross-hardware GPU determinism is a tar pit; CI needs bit-exactness on cheap runners. Scale math says CPU is fine: a 30m-resolution fire AOI of 30×30 km ≈ 1M cells; budget (§6) targets ≥500× realtime single-threaded. The design doc line gets a footnote, not a rewrite — GPU still likely for Epic 5-era eye candy and Epic 6 ensembles. |
| D3 | **Determinism mechanics** | Fixed-point (integer) cell state & accumulators; float permitted only in *precomputation* baked to quantized lookup tables at sim-init; single canonical cell iteration order; hierarchical PCG64 streams `H(run_seed, system, entity, tick)` with every draw enumerated in the spec (Epic 2 E1 pattern) | Cross-platform C++ float determinism via compiler-flag discipline is possible but fragile under agents; integer state makes golden hashes trustworthy and diffs meaningful. |
| D4 | **Default model family** | **Tick-based cellular automaton** with directional spread-rate accumulation (16-neighbor), elliptical wind/slope shaping, per-fuel base rates, moisture/curing modifiers, threshold crowning, seeded stochastic spotting | Chosen *because* it accepts mid-run world deltas naturally — suppression is the point. Minimum-travel-time / Huygens approaches (FARSITE-style) are faster for static forecasts but hostile to interactive mutation. Coefficients start from published Rothermel-shaped curves then get tuned for legibility (CP4), and live in pack files (7.2 shape). |
| D5 | **Sim grid vs data grid** | Sim runs on the canonical 30m game-profile grid 1:1 for v1; interface carries `cell_size` so nothing hardcodes it | Resampling/decoupling deferred until a real perf or design need; keeps CP2 comparisons pixel-aligned with Epic 3 arrival rasters. |
| D6 | **Debug viewer tier** | **Tier 1 (required): deterministic frame renderer** (hillshade + fuels underlay, fire state, suppression overlays) → PNG frames → MP4/GIF, runs in CI. **Tier 2 (stretch): local static web viewer** (maplibre/deck.gl over the tile store + state stream) for scrubbing | Tier 1 makes every checkpoint archivable and reviewable async (you'll be reviewing agent output — MP4s beat "trust me"). Tier 2 only if it stays disposable. |
| D7 | **Weather timeline v0 disposition** | Loader consumes Epic 3's schema as-is; any friction becomes a co-signed ADR amendment, not a sim-side workaround | This is the promised 4.1-renegotiation moment; capture it formally. |
| D8 | **Spotting & stochasticity posture** | All randomness flows through declared, seeded streams with per-system toggles (`spotting.enabled`, `spotting.deterministic_test_mode`) | Scenario authors and tests need reproducible chaos; capability flags expose which streams a model uses. |

---

## 3. The Interface (story 4.1 — sketch to be formalized in the ADR)

Schema-first; this section is the plan's contract sketch, the ADR is the document of record.

**Provided to a model at init:** immutable world views — elevation/slope/aspect, fuel layers (Epic 2 internal schema), greenness, structures mask; `cell_size`, grid dims, t0; weather timeline handle; model params blob (pack file, opaque to the core); seeds.

**Per tick `advance(dt, deltas[]) -> TickOutput`:**
- `deltas[]` — ordered world mutations since last tick: `FuelRemoved{cells|path,width}`, `RetardantApplied{cells, load, decay_class}`, `MoistureBumped{cells, magnitude, ttl}`, `IgnitionForced{cells, cause}`, `ExtinguishForced{cells}`. Models declare acceptance via capability flags; unaccepted delta kinds are reported, not silently dropped.
- `TickOutput` — dirty-region fire state (per-cell: phase ∈ {unburnable, unburned, burning, burned}, intensity class, arrival time when first burning), spotting events (source→target cells, seed provenance), model diagnostics (opaque key-values for QA).

**Capability flags (minimum set):** `accepts_deltas` (per delta kind), `provides_intensity`, `provides_spotting`, `deterministic`, `supports_rewind` (playback: yes; CA: no), `max_dt`.

**Versioning:** semantic-versioned interface; models declare the version they implement; the runner refuses mismatches. Golden conformance suite (H-stream) runs against *any* implementor — the playback driver and default model both pass the same suite, which is the proof the interface isn't secretly shaped around either.

---

## 4. Suppression sim v1 (stories 4.5/4.6 — scope fence)

Command schema (versioned, replay-recorded): `CutLine{resource_id, path, method: hand|dozer}`, `AirDrop{resource_id, target: point|segment, agent: retardant|water, volume_class}`, `Burnout{anchor_path, firing_pattern}`, `MopUp{region, depth_m}`, `Hold{...}` reserved.

Execution model v1 — deliberately simple agents: a tasked resource has a position on its path, a production rate drawn from the **NWCG production-rate pack table** (crew type × fuel model, using range midpoints; ranges retained for Epic 6 to exploit as crew-quality variance), and advances along the line each tick emitting `FuelRemoved` deltas over a method-dependent width. Air drops resolve after a configured sortie delay into `RetardantApplied` / `MoistureBumped` footprints. Burnout emits `IgnitionForced` along the firing path (same wind risk as any fire — the model doesn't know it's "friendly"). Mop-up emits `ExtinguishForced` over secured edge cells at a rate.

**Fences:** no travel-to-incident logistics, no fatigue, no crew safety modeling, no LCES/entrapment mechanics in v1 (Epic 6 decides how to treat these with the respect they require — flagged forward explicitly). Paths are caller-supplied; validity checks (on-grid, reachable-ish) only.

Containment (4.7): perimeter cells adjacent to secured line or cold burned edge / total active perimeter, computed by an observer that reads state and never writes it. Structure outcomes: structure cells intersecting burned cells at fire end, with intensity class recorded.

---

## 5. Checkpoints (the spine of the epic)

Each checkpoint = a named artifact set (MP4s/PNGs + a one-page QA memo) archived under `checkpoints/CPn/`. A checkpoint *fails loudly*: problems found upstream become Epic 1–3 tickets, logged in the memo — that's this epic doing its second job.

| CP | Name | What it proves | Artifact | Gate to pass |
|---|---|---|---|---|
| **CP1** | *The world loads* | Epics 1–2 data is sim-readable and coherent (first hostile read of Epic 2!) | Static renders: hillshade + each fuel layer + structures over the Teanaway AOI; loader stats page | Layers align pixel-perfect; fuel values plausible in SI; load time < 5s; discrepancies filed upstream |
| **CP2** | *History replays* | Epic 3 bundles + the interface + the state stream + the viz harness, with zero model risk | MP4: Jolly Mountain arrival raster animated through the playback driver over hillshade, isochrone overlay | Progression matches Epic 3's own QA imagery; interface conformance suite passes on playback driver |
| **CP3** | *First fire* | Default model core loop on real terrain | MP4: point ignition, uniform benign weather, Teanaway; growth rings visible | Fire spreads plausibly outward, respects unburnable cells, never un-burns; bit-identical re-run |
| **CP4** | *It reads honestly* | Slope/wind/fuel/moisture responses are **legible** | Scripted micro-scenario matrix (flat vs slope; calm vs wind sweep; grass vs timber vs shrub; green vs cured), side-by-side MP4s + response-curve plots | A naive viewer can verbally predict each comparison's winner before it plays; tuning memo written |
| **CP5** | *Shadow of a real fire* | Whole stack integration: real ignition + real weather timeline vs. what actually happened | Split-screen MP4: default model run vs. Epic 3 arrival playback, Jolly Mountain, shared clock | Order-of-magnitude sanity (growth direction/major runs on the right days); explicitly **not** validation — memo states divergences and disclaimer |
| **CP6** | *The fight* | Suppression sim + deltas + containment observer | MP4 pair: same fire, no-action vs. scripted campaign (line + drops + burnout); containment/outcome dashboard frames | Actions visibly alter outcome; a deliberately undersized line gets breached by spotting; metrics move sensibly; replay of the command script is deterministic |
| **CP7** | *Ship-shaped* | Replay format, perf, cross-platform determinism at scale | Perf report vs budget; replay files re-simulated on Linux+Windows CI; fuzz summary | Budgets met (§6); byte-identical cross-platform state hashes; schema docs frozen at v1 |

CP1→CP2 need no fire model at all — they're pure Epics 1–3 verification and should land *fast*. If CP1 or CP2 surfaces real upstream rot, that's the plan working, and Epic 4 proper hasn't been blocked by it (D-stream work proceeds on synthetic worlds in parallel).

---

## 6. Performance & scale budgets (benchmarked at CP7, tracked from CP3)

- Reference task: 1M-cell AOI (≈30×30 km @ 30m), 14 sim-days, hourly weather, spotting on.
- **Budget: ≥500× realtime single-threaded** on a mainstream desktop core (i.e., that task in ≲40 min of CPU… no — in ≈2.4 sim-hours/wall-second ⇒ full task ≲ 3 wall-minutes). Stretch: 2000× with the threaded tile scheduler.
- Memory: ≤ 512MB resident for the reference task. State stream on disk: ≤ 50MB via dirty-region keyframing.
- Tick cost regression tracked in CI from CP3 onward (fail on >15% regression without a waiver note).

Rationale: Epic 6's promotion mechanic and future ensembles (research/RL) both want "many fast fires," and off-season time-skip wants faster-than-realtime by orders of magnitude. Budgets are set where CPU comfortably lands per D2's scale math, so nobody reaches for the GPU prematurely.

---

## 7. Workstreams & tasks

### Workstream A — Interface & schemas (story 4.1)
- **A1. Interface ADR v1.** Formalize §3: types, tick/delta semantics, capability flags, determinism contract, versioning policy. Machine-readable schema (FlatBuffers or C structs + JSON schema for configs — pick in ADR) for state stream + replay.
  - *Done:* ADR merged after review; schemas compile; conformance-suite test list enumerated. → *dep: none*
- **A2. Weather timeline co-sign (D7).** Loader-side review of Epic 3 schema v0 against sim needs; amendment ADR if needed.
  - *Done:* co-signed or amended; Epic 3 notified of any changes. → *dep: A1 draft*
- **A3. Conformance suite.** Interface-level tests any model must pass: delta reporting, determinism under fixed seeds, dirty-region correctness, capability honesty.
  - *Done:* suite runs against a trivial "null model"; wired for B/C/D implementors. → *dep: A1*

### Workstream B — World loader & sim state
- **B1. Store/bundle reader.** Tiles + layers + bundle manifests (Epic 3) → contiguous sim arrays; world-manifest pin verified; SI/units asserted at load (Epic 2 D6 paranoia pays off here).
  - *Done:* CP1 artifacts generated; loader stats (per-layer min/max/histograms) in the memo. → *dep: A1 types*
- **B2. Synthetic world kit.** Procedural test worlds (flat, ramp, ridge, checkerboard fuels) for model development decoupled from real-data availability.
  - *Done:* D-stream develops against these; kit documented. → *dep: B1 types only*
- **B3. Weather sampler.** Timeline → per-tick, per-region wind/moisture fields with defined interpolation; test vectors.
  - *Done:* sampler deterministic; hourly→tick interpolation spec'd in ADR. → *dep: A2*

### Workstream C — Playback driver (story 4.3)
- **C1. Arrival-raster playback model.** Implements the interface: emits phase transitions per the arrival raster clock; honest capability flags (`accepts_deltas:none`, `supports_rewind:true`); confidence raster exposed as diagnostics.
  - *Done:* conformance suite passes; CP2 artifact generated. → *dep: A1, A3, B1*

### Workstream D — Default model (story 4.2)
- **D1. Model spec.** The CA defined draw-by-draw (D3/D4/D8): neighbor graph, directional rate accumulation, elliptical shaping under wind+slope, fuel base rates, moisture/greenness modifiers, crowning threshold via canopy metrics, spotting emission/transport/ignition, delta application semantics, fixed-point formats.
  - *Done:* spec merged; every stochastic draw enumerated with its stream key; params file schema defined. → *dep: A1*
- **D2. Core CA implementation.** Spec → C++; synthetic-world tests (ring growth on flat, ellipse under wind, upslope bias on ramp — property-tested against analytic expectations).
  - *Done:* CP3 on real terrain; properties hold; golden hashes committed. → *dep: D1, B2, B1*
- **D3. Weather + fuel response integration.** B3 sampler wired; per-fuel behavior from params pack; greenness modifier.
  - *Done:* CP4 matrix generated; tuning memo (what was changed and why, vs published-curve starting points). → *dep: D2, B3*
- **D4. Crowning + spotting.** Canopy-driven crowning approximation; spotting per D8 with seeded transport.
  - *Done:* demonstrable in CP4 addendum scenarios (timber crown run; spot across a barrier); toggles work. → *dep: D3*
- **D5. Delta application.** Full delta-kind support with dirty-region efficiency; adversarial tests (delta on burning cell, overlapping deltas, delta+spot same tick — ordering per spec).
  - *Done:* conformance delta tests pass; CP6 unblocked from the model side. → *dep: D2*
- **D6. CP5 shadow run.** Historic ignition + weather timeline vs playback, split-screen tooling in the viz harness.
  - *Done:* CP5 artifacts + divergence memo with disclaimer language (reused later by 8.4). → *dep: D3, C1*

### Workstream E — Suppression sim (stories 4.5/4.6/4.7)
- **E1. Command schema + production-rate pack.** §4 commands formalized; NWCG table transcribed into a versioned pack file (midpoints + ranges, source-cited).
  - *Done:* schema in ADR appendix; pack validates; spot-check vs source PDF. → *dep: A1*
- **E2. Execution engine.** Resource agents advancing along paths, emitting deltas per §4; sortie delays; burnout as forced ignition.
  - *Done:* synthetic-world tests (line stops flat-ground fire; undersized line breached when spotting enabled). → *dep: E1, D5*
- **E3. Observers.** Containment %, perimeter/area series, structure outcomes, cost accumulation; read-only enforcement (separate module, no state handles).
  - *Done:* dashboard frames render in viz harness; unit tests on contrived states. → *dep: E2*
- **E4. CP6 campaign.** Scripted multi-command scenario on real terrain; paired artifacts.
  - *Done:* CP6 gate criteria met; memo. → *dep: E2, E3, D-stream through D5*

### Workstream F — Runner & replay (story 4.4)
- **F1. `embersim` CLI.** Scenario config (TOML: bundle/world ref, model choice+params, command script, seeds, duration) → run → state stream + replay file; stage timings logged.
  - *Done:* one command reproduces any checkpoint; ergonomics doc. → *dep: A1, B1*
- **F2. Replay format v1.** Commands + seeds + interface/model/params versions + world-manifest pin; optional baked state stream; `embersim replay` re-simulates or re-emits; refuses version/pin mismatches with useful errors.
  - *Done:* re-simulation bit-identical; baked-stream path renders without model code present. → *dep: F1*
- **F3. Determinism CI matrix.** Linux + Windows runners; state-hash comparison at checkpointed ticks; compiler-flag lockdown documented.
  - *Done:* green matrix on golden scenarios; a seeded intentional divergence is caught. → *dep: F2*

### Workstream G — Debug viz harness (D6)
- **G1. Frame renderer.** State stream + world → composed PNG frames (hillshade/fuel underlay, fire phases/intensity, suppression overlays, HUD: clock, wind vane, containment) → MP4/GIF via ffmpeg. Deterministic output for CI diffing of *images* on goldens.
  - *Done:* CP2 producible; render config documented; explicitly labeled disposable/2D in README. → *dep: A1 stream schema, B1*
- **G2. Comparison tooling.** Split-screen/side-by-side composition, response-curve plots, checkpoint memo template generator.
  - *Done:* CP4/CP5 artifact generation is one command each. → *dep: G1*
- **G3. (Stretch) Local web viewer.** Static maplibre/deck.gl page reading tile store + state stream; time scrubber.
  - *Done (if attempted):* runs from `file://` or trivial static serve; zero server code; feature-frozen at scrubbing. → *dep: G1 formats*

### Workstream H — QA, perf, docs
- **H1. Property/fuzz suite.** Invariants (no un-burning, unburnable stays unburnable, delta idempotence where spec'd, observer read-only), command fuzzer, long-run soak.
  - *Done:* suite in CI; found-bug regression tests accumulate. → *dep: D2, E2*
- **H2. Benchmark suite (§6).** Reference task + micro-benches; regression tracking from CP3.
  - *Done:* CP7 perf report generated from CI data. → *dep: F1*
- **H3. Docs.** Interface guide for implementors (the research-facing doc seed), scenario authoring guide, params/pack references, checkpoint memo archive index, ADR index.
  - *Done:* an outside agent implements a toy model (e.g., constant-rate circle) against docs alone and passes conformance. → *dep: A*, F*, stable schemas*

---

## 8. Execution order (checkpoint-driven phases)

- **Phase 0 — Contracts:** A1→A3, D1 spec, E1 schema, F1 skeleton. Nothing burns yet; everything is reviewable.
- **Phase 1 — Prove the past (CP1, CP2):** B1, C1, G1. *Pure Epics 1–3 verification; upstream bugs surface here.* B2 in parallel unblocks model work regardless.
- **Phase 2 — Prove the fire (CP3, CP4):** D2, D3, B3, G2; D4 for the CP4 addendum.
- **Phase 3 — Prove the whole cloth (CP5):** D6.
- **Phase 4 — Prove the fight (CP6):** D5, E2, E3, E4.
- **Phase 5 — Prove it ships (CP7):** F2, F3, H1, H2, H3.

Parallelism note for agent teams: (B2+D1/D2 on synthetic worlds) ∥ (B1+C1+G1 on real data) is the intended two-track split in Phases 1–2; they converge at CP3.

---

## 9. Confirmed decisions (⚑) — 2026-09-07

1. **D1 — C++20 engine-free core with C API** (`embersim` CLI via CMake now, UE module later). **Confirmed.** Shape clarified at confirmation: Python keeps what Epics 1–3 already own — the bundle→**world pack** exporter (rasterio), the synthetic-world kit, the viz harness, and scenario tooling under `ember sim ...`; the C++ core consumes only flat world packs and has zero geospatial dependencies. No C++ toolchain existed on the dev box at confirmation; VS Build Tools 2022 + CMake + Ninja are installed as part of F1, and CI gains a Linux+Windows matrix (F3).
2. **D2 — CPU-normative determinism, GPU deferred.** **Confirmed.** Footnote added to WILDFIRE_DESIGN.md story 4.2.
3. **CP5 demo fire — Jolly Mountain 2017.** **Confirmed** as the recurring hero. Note: the bundle in the store has **no weather timeline attached** (`weather: null`; the flagship memo deferred it). CP5 requires a bounded-window `ember incident --historic jolly-mountain-2017 --weather` run — logged as an upstream (Epic 3) task in the CP1 memo.
4. **D6 Tier 2 web viewer — attempt as stretch after CP6**, only if all gates through CP6 are met; feature-frozen at time scrubbing; zero server code.

Findings at kickoff that reshape tasks (details in `checkpoints/CP1/memo.md` once generated):
- The Epic 3 arrival raster is **not pixel-aligned** with the baked world grid (different origin and dims), contrary to D5's assumption. The loader resamples arrival/confidence onto the world grid (nearest) and records the resample in the world pack manifest; an Epic 3 ticket asks for the arrival grid to be snapped to the world grid at derivation time.
- **No structures layer exists** upstream (Epic 1 story 1.2 was never built). The interface carries an optional structures mask; the structure-outcome observer reports "no structures layer" rather than zeros.

## 10. Notes for later epics

- **Epic 5:** consumes the state-stream schema + replay files only; G-harness compositions (split-screen, HUD elements) are a requirements crib sheet, not code to inherit. GPU model port, if ever, validates against F3's golden hashes.
- **Epic 6:** commands schema is the gameplay verb list's floor; production-rate *ranges* (E1) are the crew-quality mechanic's raw material; promotion mechanic budget assumptions come from §6; crew-safety/entrapment treatment deliberately deferred to 6.6's respect pass.
- **Epic 7:** model params + production-rate packs are already 7.2-shaped; the interface ADR + conformance suite are the "community model" on-ramp.
- **Research surface (design doc §Vision):** H3's implementor guide + conformance suite + replay format are the entire external-model story; 4.8 bindings become a demand-driven follow-up.
- **Epic 9:** replay world-pinning exercised here end-to-end; state streams are 9.4's payload candidate.

---

## 11. Follow-ups (tracked here; tick when done)

Status after execution (2026-09-08). The definition of done in §1 is met except for the
Linux leg of the determinism matrix, which only runs in CI.

**Closes the definition of done**
- [ ] F3a — Push, run `.github/workflows/sim.yml` for the first time, fix first-run breakage
      (Ninja install on both runners is not guaranteed), confirm Linux == Windows == committed
      goldens (`sim/scenarios/golden/*.hashes`).

**Plan items left open**
- [ ] H3b — The implementor-guide acceptance test: an agent with only `docs/sim/` writes a toy
      constant-rate model and passes `conformance_test`. Guide exists; test not run.
- [ ] H2b — Perf regression as a CI gate (>15 % on the `ca_test` perf probe / bench scenario
      fails without a waiver). Needs a recorded baseline first (CP7 memo has day-one numbers).
- [ ] F1b — `SuppressionSim::issue(const Command&)` for late commands so the C API's
      `es_sim_issue_command` works (Epic 6 needs it; returns "not supported" today).
- [ ] D6 Tier 2 — local static web viewer (stretch; not attempted).
- [ ] 4.8 — Python bindings (parked by design; C API exists).

**Upstream tickets from CP1 (file in Terrain / Ember trackers)**
- [ ] U1 (Epic 3, arrival.py) — derive arrival/confidence rasters on the baked world grid.
- [ ] U2 (Epic 1, DEM) — Copernicus east-edge strip (1–300 m over 700 m+ terrain) → nodata/fill.
- [ ] U3 (Epic 2, fuels) — canopy nodata vs non-forest zero conflated (17k timber cells, cc = 0).
- [ ] U4 (Epic 3, weather) — done as `ember weather --start/--hours`; consider making the
      refresh path accept a window too.
- [ ] U5 (Epic 1, story 1.2) — structures layer (observer reports `-1` until it exists).
- [ ] U6 (Epics 1–2, all ingest stages) — bounded-memory, chunked, resumable ingest; peak RAM
      must not scale with incident size (Three Queens peaked 13.4 GB and was killed once).
      Design: `docs/upstream/U6-bounded-memory-ingest.md` (filed from Epic 5, 2026-09-27).
- [ ] U7 (Epic 1, DEM finalize) — hydro-flatten water bodies: LiDAR drops water returns and void
      fill stops at 5 px, so lakes are DEM nodata -> holes in every mesh. Three Queens: 23,856
      interior nodata cells, 99.3 % FBFM40 water (Kachess hole 190 ha). Fill each water body
      (FBFM40 98 / NHD) with its shoreline elevation (flat surface) before tiling. Found at HCP1.

**Model-quality candidates noted, deliberately not tuned (would be fitting one fire)**
- [ ] CP5: RH/T→moisture response too strong relative to wind for timber (model grows fastest
      on the hot dry days, the real fire on the windy ones).
- [ ] CP5: flanks too fast — burned-set LB ≈ 60 % of the wavelet LB; consider a flank-rate term.
- [ ] Head-speed cap `cell/dt` (spec §11.8): document `dt ≤ 30` guidance in the scenario
      authoring section of `formats.md`, or make the CA advance sub-tick ignitions.
- [ ] Air-drop footprints/loads are pack guesses (`suppression.md` §4); source real numbers.

**Housekeeping**
- [ ] `git push` (23 Epic 4 commits are local only).
- [ ] CRLF normalisation: several generated `.md/.json/.hashes` files were written with CRLF;
      add a `.gitattributes` (`* text=auto eol=lf`) so CI diffs of goldens stay clean.
