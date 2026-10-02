# External Model Strategy Memo — Authority Inversion & the Faithful-Renderer Position

**Status:** Discussion / suggestions — NOT an execution plan
**Audience:** Local Claude, for integration across WILDFIRE_DESIGN.md (research-surface framing, business notes), EPIC_4_PLAN.md, and EPIC_5_PLAN.md. Suggest; don't silently restructure. Where this memo conflicts with those documents, this memo is the *newer thinking* — but surface the conflict rather than quietly resolving it.
**Origin:** Conversation 2026-10-01, following research into the fire-model landscape (FARSITE/FlamMap, ELMFIRE, WRF-SFIRE, Technosylva Wildfire Analyst et al.).

---

## 1. The strategic position (Brad's call, 2026-10-01 — treat as settled direction)

**We do not compete on operational decision support.** Technosylva-class operational prediction (CAL FIRE, utility PSPS risk ops) requires validated models, liability posture, procurement relationships, and an entire team we don't have. The design doc's "not an operational prediction tool" non-goal is hereby load-bearing business strategy, not just legal posture.

**We build what they don't have: communication-grade visualization.** The incumbent's output is analyst-grade GIS cartography (Esri-stack dashboards, tablet apps). Nothing in the ecosystem explains a fire to a county commissioner, a community meeting, a PSPS-darkened customer base, or a newsroom. That gap is the lane.

**Sequencing:** presentation layer first; if that succeeds, the success becomes the platform for broader reach — *then* we can consider competing upmarket. Not before. Integrator: nothing in the plans should front-load "compete with Technosylva" work; anything that smells like operational decision support gets flagged, not built.

## 2. The authority inversion (the architectural principle)

Previous framing treated external models as *rival referees* our sim gets measured against. The corrected framing:

> **External model output is the authority. Our sim is the performer. Their output is the score.**

When an external model's output is ingested, our sim's job is **fidelity to that output** — synthesizing what it doesn't carry (temporal continuity between isochrones, front texture, fire dynamics, smoke, spotting moments) while never contradicting what it does carry. "Accuracy" against an ingested source means *agreement with the source wherever it speaks*, not agreement with reality. Our model runs unconstrained in exactly one place: the game.

Consequences:
- The liability/credibility story simplifies to: "We don't predict. We render *your* prediction, provably faithfully." (Feeds design doc 8.4 and Epic 5 HCP5 disclaimer language.)
- The referee-ensemble idea (multi-model comparison harness discussed earlier) is **demoted to an internal tuning instrument** for the game model's feel. It is not the product story and should not shape customer-facing framing. If already captured anywhere as product, downgrade it.
- CP5 in Epic 4 ("shadow of a real fire") keeps its purpose — sanity-checking the *game* model — but should not be confused with the fidelity story above. Two different questions: "is our cartoon plausible?" (CP5) vs. "do we honor ingested data?" (new conformance class, §4).

## 3. What external outputs actually carry (the ingest menu)

Richer than perimeter polygons — this shrinks what our sim must synthesize. Approximate, for the integrator to verify per format during adapter work:

| Source | Typical output contents | What our sim must fill in |
|---|---|---|
| FlamMap/FARSITE | Arrival-time grid, fireline intensity, flame length, crown activity rasters, perimeter polygons | Temporal continuity below output resolution; smoke; visual dynamics |
| ELMFIRE | Arrival time, spread rate, flame length rasters; probabilistic ensembles (burn probability over time) | Same; ensemble data additionally enables uncertainty visualization (hurricane-cone analog) |
| WRF-SFIRE | Full 4D: fire state + coupled winds, heat flux, (smoke fields) | Least — largely presentation of their numbers; big files (netCDF) |
| Technosylva (customer exports) | Perimeter isochrones, impact metrics (proprietary formats) | Most — closer to the thin/coarse case |
| Coarse/manual (hourly isochrones, hand-drawn projections) | Perimeter sequence only | Everything between the constraint surfaces |

Design implication: the ingest layer should normalize all of these toward the arrival-time(+intensity) raster shape Epic 3 already derives from observations — **one internal representation, many adapters** — with per-layer "source speaks / source silent" masks so the constrained model knows where it's pinned and where it freewheels.

## 4. The constrained model (suggested Epic 4 addition)

A third interface implementor between the existing poles (playback driver = pure interpolation, zero model; default model = fully free):

- **ConstrainedModel**: the default model running with state continuously nudged toward ingested arrival/intensity data — converge wherever the source speaks, freewheel plausibly between constraints. (Fire-science term of art: data assimilation; for viz purposes a simple nudging scheme suffices. WRF-SFIRE's perimeter-assimilation literature is prior art if the implementer wants references.)
- New capability flag (e.g., `supports_constraints`), same conformance suite as every implementor, plus a **new conformance class: constraint fidelity** — wherever the source defined arrival/intensity, the output matches within declared tolerance; violations are failures, not style.
- **Per-render fidelity metric**: every visualizer render that ingested external data ships a QA sidecar stating constraint agreement ("arrival within X min over Y% of constrained cells; intensity class agreement Z%"). That number is the product promise, stated mechanically. Suggest it appear in HCP bundles and in the provenance slate/sidecar from Epic 5 G2.

## 5. Ingest adapter roadmap (priority by population served)

1. **FlamMap/FARSITE file formats** — largest analyst population on earth; documented formats; read-only support (we never write their formats, no reason to). Also the Tier-1 cheap win: a one-time FlamMap run on the Teanaway scenario as a *calibration anchor* for the game model's CP4 tuning memo ("in-family with what agency analysts trust").
2. **ELMFIRE** — open tool, conda-installable, zero-permission; inputs are LANDFIRE fuels + weather, i.e., our Epic 1–3 store already feeds it. Doubles as the first *live* external-model experiment: run ELMFIRE locally on the Teanaway AOI → ingest → render. This is the research-surface demo ("ELMFIRE run in, broadcast flyover out") and the proof the 4.1 interface isn't shaped around our own model. Strong candidate for a checkpoint between Epic 4's CP2 and CP5.
3. **WRF-SFIRE (netCDF)** — research prestige tier; heavier files, smaller population; build when a collaborator shows up or the demo needs it.
4. **Technosylva exports** — cannot be built speculatively (proprietary formats; need samples from a design partner). The door-opener is 1–2 shipped: "we already render FlamMap and ELMFIRE." Flag as partnership-gated; their customers (utilities' community-relations side, CAL FIRE public information) are the eventual buyers of communication-grade output of their own sims.

KPI generalization (Epic 5): **minutes from their-file to flyover**, same clock and instrumentation as minutes-from-IRWIN. `ember-render --model-output <file>` as a sibling entry point to `--irwin`/`--historic`/`--replay`.

## 6. Integration pointers (where this lands, per document)

- **WILDFIRE_DESIGN.md**: research-surface vision section gains the authority-inversion principle and the faithful-renderer positioning; §8-adjacent business notes gain the §1 lane/sequencing statement; 8.4 disclaimer posture gains the "we render your prediction" framing.
- **EPIC_4_PLAN.md**: ConstrainedModel as a workstream addition (likely C-stream sibling); constraint-fidelity conformance class in A3; ELMFIRE local-run experiment as a named task/checkpoint candidate; referee-ensemble explicitly scoped as internal tuning only.
- **EPIC_5_PLAN.md**: `--model-output` entry point in G2's orchestrator; fidelity metric in QA sidecars and HCP bundles; HCP5/HCP7 release-checklist language updated to the faithful-renderer claim.
- Ingest adapters themselves probably constitute a small new workstream (Epic 4-adjacent — they produce interface-consumable inputs, not pixels); integrator's call where it lives.

## 7. Open questions for Brad (surface, don't resolve)

1. Nudging scheme tolerance defaults — what counts as "faithful" (arrival-time tolerance in minutes? intensity class off-by-one allowed?) — needs a first-pass number before the conformance class is testable.
2. Does the ELMFIRE experiment rate a formal checkpoint (with review bundle) or run as a task inside existing checkpoints?
3. FlamMap calibration anchor: one scenario or a small matrix (the CP4 micro-scenarios re-run through FlamMap)?
4. When coarse input underdetermines behavior (hand-drawn isochrones), how much liberty may the constrained model take visually before a "low-constraint render" warning label is warranted? (Ties to the honesty posture — probably a labeled confidence tier on renders.)
