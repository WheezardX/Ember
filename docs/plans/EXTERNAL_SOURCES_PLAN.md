# External Sources — Execution Plan (their file in, faithful flyover out)

**Status:** v1.0, 2026-10-01 — written by the strategy session; Brad confirmed D1-D4 the same
day (§3). D7 is his call, later. Ready to hand to the executor on Brad's word.
**Parents:** `docs/plans/EXTERNAL_MODEL_STRATEGY_MEMO.md` (the position; this plan supersedes its
§4 and §5), WILDFIRE_DESIGN.md story 4.3 and the research surface, ADR 0008.
**North star:** Three Queens 2026 (Brad, 2026-10-01). Every phase proves itself on that fire.

---

## 0. Framing

The position (Brad, settled): we do not predict. We render someone else's prediction, or the
observed record, faithfully and at communication grade. This plan makes that true in code, in
three small phases, each ending in a review bundle.

What changed since the memo (agreed with Brad 2026-10-01):

- **No new model.** ELMFIRE and FlamMap output arrival time per cell. The existing
  `arrival-playback` driver replays an arrival raster exactly, so an external model's file goes
  through an adapter into the driver we already have. The memo's ConstrainedModel is deferred
  (§7). `ember-ca` is the game model and nothing else.
- **The load-bearing change is the data the renderer is given**, not the sim. External sources
  speak in physical quantities (flame length, spread rate, crown fire type). Our stream carries a
  1-3 class and the renderer invents the rest. Three workarounds already exist: spread rate
  derived into FireTex A, NIROPS heat in `<pack>.heat.bin`, and the classes. Phase X2 gives them
  one home.
- **Real forecast files for the north-star fire are on disk.** 34 PyreCast / ELMFIRE runs for
  Three Queens (2026-08-12 to 09-11, percentiles 10/30/50/70/90) are mirrored at
  `store/incidents/dfa477f2-305c-49ca-b82e-2af072ab55f8/external/pyrecast-elmfire/` (read its
  `PROVENANCE.md` first). The 2026-08-20 05:11 UTC run is the last one before the Aug 21 blow-up
  and shares its timestamp with that night's NIROPS flight.
- **ELMFIRE's build prerequisites are installed in WSL** (Ubuntu 24.04.3: gfortran 13.3,
  Open MPI 4.1.6, GDAL 3.8.4; 28 threads, 15 GB visible to WSL).

Lane discipline: "the forecast as issued, beside what happened" is ours. Scoring the forecast is
not. No skill metrics, no precision / recall, no "the model was wrong" framing in any artifact.

## 1. Hard rules for this plan

1. **PyreCast data is internal-only.** Its terms prohibit commercial use and redistribution of
   data or derived products without written permission. Therefore: nothing derived from the
   mirrored files is committed to git (no rasters, no test fixtures cut from them, no renders, no
   review bundle). PyreCast-derived bundles go under `store/review/` (git-ignored), not
   `checkpoints/`. Every sheet or video made from them carries the line
   "Data source: PyreCast Wildfire Forecasting Platform (pyrecast.org)" and "internal".
2. **Tests use synthetic fixtures** shaped like the real files (same dtype, nodata, grid offset).
3. **ELMFIRE runs (X3) are under ELMFIRE's free licence** (corrected 2026-10-02; the plan
   assumed EPL-2.0). At the pinned commit `cbf924a` ELMFIRE is AGPL-3.0 + Commons Clause, with a
   separate CloudFire, Inc. commercial licence for "any activity primarily intended for
   commercial advantage". D8 (Brad, 2026-10-02): "We move forward with the Free version. Before
   we pitch for grant money we'll re-evaluate." So X3 runs as research / internal development;
   its outputs may be committed and reviewed internally (usual size limits), carry the ELMFIRE
   (AGPL + Commons Clause) notice in their provenance, and go into no grant pitch, sale or
   commercial material until Brad has re-evaluated the licence.
4. **Stream v1 and its golden hashes do not change in this plan.** No edit to `.ess` v1, to
   `state_hash`, or to the conformance suite's existing checks.
5. Standing rules still apply: download ledger with an estimate before any fetch; no heredocs;
   per-task commits (`External X1.2: ...`); stop for sign-off at each checkpoint.

## 2. Facts the executor needs (verified 2026-10-01)

| Item | Value |
|---|---|
| Three Queens world pack | `store/sim/hist-three-queens-2026-ir.ewp`: 620 x 576 at 30 m, EPSG:32610, origin (625806.38, 5260038.42), t0 2026-07-16T04:28Z, no weather pack |
| Observed playback | `sim/scenarios/tq26-ir-playback.scenario.toml` -> `runs/tq26/`; client scenario `S_tq26_play` |
| PyreCast arrival raster | `{pct}.tif`: 2301 x 2210 at 30 m, EPSG:32610, left 599828.56, top 5284344.5, float32, nodata 0, hours since `run_ts`, max 336, new growth only |
| Grid relationship | same CRS and cell size; the origins differ by 865.93 cells in x, so it is a nearest resample with a sub-cell shift (<= 15 m), not a copy. Record the shift in the output manifest |
| PyreCast hourly tars | `{pct}_{var}.tar`, up to 169 hourly GeoTIFFs on the same grid: `crown-fire` u8 0/1/2 (nodata 255), `flame-length` u8 1-44, `spread-rate` u8 1-174, `hours-since-burned` i16 1-401 (includes area burned before the run) |
| Units of the hourly variables | NOT stated in the files. ELMFIRE's native outputs are flame length in ft; confirm flame length, spread rate and the crown classes against the ELMFIRE / PyreCast docs before converting. Do not guess |
| Aug 20 05:11 UTC run, 48 h growth | p10 526 ac, p50 660 ac, p90 2,333 ac; observed between the IR flights 2,529 ac (acreage only; direction not compared) |
| Python stream writer | `ember/dev/firesynth.py` already writes `.ess` with intensity classes from Python |

## 3. Decisions (✔ = confirmed by Brad; ⚑ = his call, still open)

| # | Decision | Recommendation | Why |
|---|---|---|---|
| D1 ✔ | Order of work | This plan runs next, ahead of HCP5-7. The look push pauses except for defects. **Confirmed (Brad, 2026-10-01)**, with his reminder that HCP6 is also still owed: HCP5, HCP6 and HCP7 all remain open and follow this plan (§5) | Brad 2026-10-01: the external-source position "is what we need to focus on" |
| D2 ✔ | Where physical channels live | A **channels sidecar** referenced from `replay.json` (new ADR 0010), not a stream v2. **Confirmed (Brad, 2026-10-01: "agreed, I think")** - the ADR is where it gets its second look | Keeps `.ess` v1, the hashes and CI goldens untouched; external sources need no sim-core change; it generalises `heat.bin`. Alternative: `.ess` v2 with extra per-cell fields - cleaner long term, but a breaking format change for a benefit only the game model needs, later |
| D3 ✔ | Fidelity tolerance (memo Q1) | Dense sources: every source-burned cell arrives within one stream tick of the source time, and no cell burns that the source did not burn. Reported in a sidecar; a miss fails evaluation. **Confirmed (Brad, 2026-10-01): start strict and adapt if needed, with a bias against any additional modelling** - so if the strict test cannot be met, the executor reports why and asks; it does not loosen the test or fill the gap with simulation on its own | The adapter path is a replay, so anything looser would hide a bug |
| D4 ✔ | Checkpoints (memo Q2) | Three light checkpoints X1-X3: a sheet, one MP4, a one-page memo, explicit sign-off. **Confirmed (Brad, 2026-10-01): "Light it is. Make progress and if it isn't up to snuff, we pivot and take those lessons learned."** So: get each phase to a reviewable result quickly, do not polish ahead of the gate, and end every memo with a short "what we learned / what we would change" section so a pivot has something to stand on | Same rhythm as the HCPs, smaller bundles |
| D5 | Canonical units | Channels are stored in SI (m, m/h, kW/m); adapters convert at the boundary and record the source unit | One conversion point |
| D6 | Percentiles | X1 renders p10 / p50 / p90 as three separate replays. Drawing the envelope as lines in the 3D scene waits for the HCP5 overlay layer and becomes a stated requirement on it | No overlay framework exists yet |
| D8 ✔ | ELMFIRE licence | **Free (AGPL + Commons Clause) version, re-evaluate before any grant pitch** (Brad, 2026-10-02) | The licence is not EPL-2.0 as first assumed (rule 3) |
| D7 ✔ | Approaching PyreCast / SIG | **Brad approaches them himself; PyreCast data is used internally now; re-evaluate before any public demo** (Brad, 2026-10-02: "I'll approach and similar to Elmfire. We move forward now internally, we reevaluate before any public demo.") | A working render of their own forecast is the opening; the strategy must not depend on their answer (X3 is the independent path) |

## 4. Phases

### X1 — The forecast as issued (no schema change)

Goal: the Aug 20 forecast for Three Queens plays in the client beside the observed fire.

- **X1.1 Source normaliser** (`ember/external/`): one small internal representation, the *fire
  timeline*: per-cell arrival time plus optional per-cell layers, each with a "source speaks"
  mask, units, and provenance (source, run time, percentile, licence class). Written to disk as
  arrays + a JSON manifest. This is the "one representation, many adapters" seam.
- **X1.2 PyreCast adapter**: a run directory -> fire timeline per percentile. Arrival from
  `{pct}.tif`. Flame length, spread rate and crown class sampled from the hourly granule at each
  cell's arrival hour, kept in the timeline for X2 (X1 does not render them). Missing granules
  (`got` < `n` in `archive_manifest.json`) leave the mask unset for those cells; never interpolate
  silently.
- **X1.3 Composite arrival on the pack grid**: observed arrival (the NIROPS pack) for everything
  burned at or before `run_ts`; forecast arrival after it, converted to seconds since the pack's
  t0. Write a world pack variant per percentile and run `arrival-playback` (hourly ticks, as the
  observed scenario). The result is "observed so far, then the forecast".
- **X1.4 Fidelity sidecar v0** (`<run>.fidelity.json`): source and grid shift; cells the source
  burned vs cells the stream burned; share arriving within one tick; cells burned without source
  support (must be 0). `ember-dev evaluate` fails on a miss (D3).
- **X1.5 Scenario `S_tq26_forecast_0820`**: the `S_tq26_play` bookmarks (`kachess_hero`,
  `aug21_run`, `overview`); captures at run + 24 h and + 48 h for p10 / p50 / p90 and for the
  observed playback at the same clock times; one timelapse run -> + 72 h, p50 beside observed.
- **X1.6 Bundle** (under `store/review/X1/`, rule 1): the 4-up sheet (p10 | p50 | p90 |
  observed), the timelapse, the fidelity sidecars, a memo with the minutes from run directory to
  MP4 (the memo's "their file to flyover" KPI, first measurement).

**Gate X1 (Brad):** the forecast plays; the sheet makes the spread between percentiles and the
observed outcome visible without commentary; fidelity sidecars are clean.

**X1 SIGNED OFF (Brad, 2026-10-02).** Bundle in `store/review/X1/` (internal): the 4-up sheet,
the p50 | observed timelapse, fidelity sidecars clean for p10 / p50 / p90, memo; first KPI ~6.5
min their file to flyover. Decisions made on the way: the forecast's starting perimeter is shown
at the run time (Brad: "when we render off models, we render true to what they predict.
Contrasting that to what was observed is one of the features we intend to provide"); a smoke-off
map view for comparison sheets. Added at Brad's request: the forecast drawn as a conventional
2D map beside our render, frame-synced, with an elapsed-since-run counter
(`ember external x1-map-video`; Brad on the map-view version: "exactly what I had in mind") -
the demo format for future sources.

### X2 — Channels: the renderer draws what the source said

Goal: flame height, crown behaviour and head brightness come from the source's numbers where it
speaks, and from today's class rules where it is silent, and the render says which.

- **X2.1 ADR 0010** (after D2): the channels sidecar. Two kinds: `at_arrival` (one value per
  cell: what the fire was doing when it got there) and `series` (one grid per timestamp, the
  shape of today's `heat.bin`). Each channel: name, unit, dtype, scale, mask, source. Initial
  names: `flame_length_m`, `spread_rate_mh`, `crown_class` (0 surface / 1 passive / 2 active),
  `fireline_intensity_kwm` (optional), `heat_class` (series), `burn_probability` (reserved).
  The ADR also states that the game model may fill the same channels later (not in this plan).
- **X2.2 Reader in worldcore** next to `emberworld::fire`, with a test against the Python
  writer, like the stream reader's.
- **X2.3 Renderer** (`AEmberFireActor`, `AEmberFlameActor`, M_Veg torching, smoke source
  strength): where a channel speaks, use it - flame card height from `flame_length_m` (replacing
  the per-class height ranges), torching and crown flames from `crown_class`, FireTex A from the
  source's `spread_rate_mh` instead of the value derived from the arrival gradient. Where it is
  silent, behave exactly as today.
- **X2.4 Facts and probes**: `fire.channels` (which speak, coverage %), and at each fire probe
  the source value and the value the renderer used. The fidelity sidecar gains a line per channel.
- **X2.5 No regressions**: every existing scenario has no channels and must render pixel-identical
  to its golden. `heat.bin` stays as it is in this plan; moving it to a `series` channel is a
  follow-up ticket once X2 has settled.

**Gate X2 (Brad):** an A/B sheet of the same forecast run, class-based (X1) vs channel-driven,
with the source's own flame-length and crown maps alongside. The question for Brad is the one
HCP4 left open: can you now point at where it is hot and what kind of fire it is?

**X2 review 1 (Brad, 2026-10-02, client fly-through, p90 + 38 h): NOT closed.** Burn scar: "a huge
improvement. Looks more like a real burn scar with a mix of untouched or partially burned areas.
The ground fire only scorching trees is great. Crown fire areas and torching taking away the
foliage." The A/B sheet is hard to read; the difference is clear in a fly-through. Flames:
"still needs a lot of work, the varied flame height is an improvement from before but the fire
still looks thin and transparent. All flames seem to emit from the ground too where as in
torching and crowns you'd see it in the tree tops." Brad: "we need to improve flame
representation before we can close X2". So X2.6 (added): **flames v3** - a flame body that
occludes (not only additive glow), fuller tongues, and torching / crown flames placed in the
canopy (crown base to above the tree tops, from the pack's canopy base / height) with the ground
fire separate below. Also asked: a compass in the play HUD (upper right). Follow-up (smaller):
logs in the scar keep a uniform ember glow for ~a day and never burn down - vary smoulder time,
let some burn down / away.

**X2 SIGNED OFF (Brad, 2026-10-02):** "Still lots of work on making fire look real but this is a
good place to call it for this phase." Delivered: ADR 0010 channels (PyreCast flame length,
spread rate, crown class per cell; fidelity per channel), the class / scorch / torching from the
source (the burn scar "a huge improvement"), flames v3 (an occluding body, opaque base thinning to
the tips, torching / crown flames in the canopy with tops at ground + flame length, a varied lean
toward the spread with the tips bending over), the play-HUD compass. Assumption carried (§7b):
PyreCast active-crown flame length read as metres.
**Flame work left for a later look phase** (not X2): flames only draw within 400 m (the terrain
glow beyond); fronts can still read as rows of cards; no smoke / soot at the tips, no embers in
the flames, no flame light on the surroundings beyond the terrain glow; motion is per-card noise
(no puffing / detaching lick shapes); the log smoulder follow-up above.

### X3 — Our own ELMFIRE run (the clean-licence source)

Goal: a forecast we produced ourselves, from our own store, rendered through the same path. This
is what can be shown outside.

- **X3.1 Build** ELMFIRE in WSL at a pinned commit (outside the repo), run its own verification
  case, add a check to `ember-dev doctor`. Record the ELMFIRE licence notice (AGPL-3.0 + Commons
  Clause, rule 3 / D8) in the provenance pattern.
- **X3.2 Inputs from the store** (`ember sim elmfire-inputs`): fuels, canopy and topography
  rasters in the units ELMFIRE expects; an ignition from the NIROPS perimeter of
  2026-08-20 05:11 UTC; hourly weather for 2026-08-20 to 08-23. The pack has no weather, so this
  needs an HRRR window for Three Queens through the existing `ember weather --historic` path -
  **estimate the download and get Brad's go before fetching.** Fuel moisture: state the method
  used in the memo (a documented simple scheme or fixed scenario values); do not present it as
  calibrated.
- **X3.3 ELMFIRE adapter**: native outputs (time of arrival in s, flame length, spread rate,
  fireline intensity, crown fire) -> fire timeline. Shares the normaliser with X1.2.
- **X3.4 Render and bundle** (may live in `checkpoints/X3/`, it is ours): observed | our ELMFIRE
  run, the same bookmarks and times as X1; fidelity sidecar; minutes from file to flyover for
  both adapters; `ember-render --model-output <dir>` as the entry-point name reserved for
  Epic 5 G2 (a thin wrapper is enough here).

**Gate X3 (Brad):** the same pipeline produced a shippable-class forecast render with no
PyreCast data in it; the memo states plainly what our run is (an uncalibrated ELMFIRE run on our
inputs) and what it is not.

### X4 — Fold the position into the documents (proposed diffs, Brad approves)

The memo asked that conflicts be surfaced, not silently resolved. The executor proposes edits,
does not merge them unreviewed: WILDFIRE_DESIGN.md (vision: authority inversion and the
faithful-renderer position; 8.4 posture: "we render your prediction"); EPIC_4_PLAN §10-11 (the
referee-ensemble idea is internal tuning only; ConstrainedModel deferred); EPIC_5_PLAN (G2 gains
`--model-output`; fidelity sidecar in bundles; HCP5 E1 gains two requirements from this plan:
percentile envelope lines, and suppression lines from the record, see §6).

## 5. Order and dependencies

X1 -> X2 -> X3 -> X4. X3.1 (the build) and the X3.2 download estimate can run alongside X1, since
they touch nothing else. X2 starts only after the X1 gate, because the A/B needs the X1 render.

**After this plan: HCP5, HCP6, HCP7, all still open.** Nothing here needs them: X1-X3 use
bookmarks, captures and timelapse orbits, which exist. What HCP6 ("the camera speaks") already
has from work pulled forward: the free-fly play client with a scrubbing timeline
(`ember-dev play`), and data-driven orbits / flyovers in scenario files. What it still lacks: a
chase-the-front camera (none exists), the shell's overlay toggles (waits on HCP5's overlay
layer), and Brad's hands-on session and sign-off. The overhead-vs-ground camera decision is
still pending and belongs to HCP6. This plan feeds the later checkpoints: HCP5 inherits the
envelope and suppression-line requirements (§6), HCP7's `ember-render` inherits
`--model-output` and the fidelity sidecar.

## 6. Tickets this plan creates but does not do

- **Suppression from the record (HCP5 input).** For real fires the suppression is recorded, not
  simulated: NIFC publishes Event Line data (dozer line, hand line, burnout and more) in yearly
  Operational Data Archives, and there is a quality-assured 2017-2024 fireline dataset with
  outcomes. An Epic 3 adapter plus a renderer vocabulary for line types; closes the 8g "fire
  lines and removal" note for the visualizer.
- **Rain in the game model.** Precipitation -> dead fuel moisture -> the moisture response the CA
  already has. An Epic 4 follow-up; it is how the external models treat rain too.
- **`heat.bin` as a `series` channel** (after X2).
- **Envelope lines in the 3D scene** (HCP5 E1 requirement, D6).

## 7. Parked, with reasons

- **ConstrainedModel** (our sim nudged toward someone else's data): only needed for sparse
  sources such as hand-drawn isochrones, and it is the one place we would be inventing fire
  behaviour. Fill gaps with observations first (as the Three Queens satellite timing does) and
  label the render's confidence. Revisit when a real sparse source shows up.
- **FlamMap / FARSITE adapter**: the largest analyst population, and the second adapter after X3.
  It needs a human session in the FlamMap GUI on Windows, so it waits for Brad's time.
- **WRF-SFIRE, Technosylva exports**: as the memo says - collaborator-gated and partner-gated.
- **Suppression in the game**: the unbounded part. Epic 4's v1 scope fence stands; an Epic 6
  problem.
- **Paintover / image-model tooling for look iteration**: a separate thread, decision pending.

## 7b. Verify before any public demo (assumptions in use)

Everything here is acceptable for internal work and must be re-checked before anything is shown
outside (Brad, 2026-10-02):

1. **PyreCast active-crown flame length taken as metres** (all other flame lengths ft) - ADR 0010
   decided point 1, amended; `ember.external.channels.source_si`. Ask PyreCast / SIG.
2. **PyreCast data permission** (D7): internal use only until PyreCast / SIG agree.
3. **ELMFIRE licence** (D8): the free (AGPL + Commons Clause) version, re-evaluated before any
   grant pitch.
4. **The forecast's starting perimeter shown at the run time** (X1.3): a flag in the source with
   no time; our display choice, stated in the fidelity sidecar.

## 8. Open points for Brad

1. D7: whether and when to approach PyreCast / SIG, after the X1 bundle exists.
2. EPIC_5_PLAN records HCP4 as signed off on 2026-09-30 with the legibility-at-distance caveat.
   If you consider it open, say so and the plan should record that; X2's gate is where that
   caveat gets its real test either way.

Answered: the GitHub repo is private (Brad, 2026-10-01). Rule 1 stays as written anyway - a
private repo can gain collaborators or go public, and git history is hard to clean.
