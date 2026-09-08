# ADR 0009 — Weather timeline v0: Epic 4 co-sign and amendments

**Status:** Accepted (2026-09-07). Epic 4 workstream A2 (plan decision D7). Co-signs
ADR 0007 as the sim's weather input; amends it in three small, backwards-compatible ways.
ADR 0007 stays **accepted** (no longer provisional).

## Context
ADR 0007 (Epic 3, C1) defined `weather-timeline-v0` speculatively for a consumer that did not
exist. The consumer now exists: the sim core's weather sampler (B3) turns the timeline into
per-tick, per-cell wind and dead-fuel-moisture inputs for the default model, and the world-pack
exporter must serialise it into an engine-free form. This ADR is the promised renegotiation.

Loader-side review of v0 against sim needs:

| Need | v0 status | Verdict |
|---|---|---|
| Regular time axis, UTC t0 | `t0 + step_minutes × num_steps` | ✅ as-is |
| Wind as components (no direction averaging) | `wind10_u/v` m/s | ✅ as-is |
| Inputs for dead fuel moisture | `t2` K, `rh2` % | ✅ (moisture is derived model-side, see below) |
| Precipitation for retardant/moisture decay | `precip` mm/step, optional | ✅ optional; absent → 0 |
| Gridded field in the world's CRS | `GridSpec` in UTM, cell-centred | ✅ resampled by the exporter |
| Explicit gaps | `gaps[]` + per-step provenance | ✅ but v0 does not say what a *consumer* does at a gap — amended |
| Sub-hourly ticks | 60-min steps only | amended: interpolation rule |
| Stations | `StationSeries` + parquet | not consumed by the sim v1 (QA only) — recorded, not amended |
| Wind at 10 m vs mid-flame | 10 m only | model-side reduction factor (params), not a schema change |

## Decision
1. **Co-signed.** The sim consumes `weather-timeline-v0` manifests + npz sidecars unchanged.
   The Python exporter (`ember sim export`) resamples the gridded field onto a coarse sim
   weather grid in the world pack (`weather.json` + `weather.bin`, int16-quantised:
   wind cm/s, t2 0.1 K, rh2 0.1 %, precip 0.01 mm) — the *only* form the C++ core reads.
2. **Amendment A — interpolation rule (normative for consumers).** Between steps, the sim
   interpolates **linearly in time** on the quantised integers (Q16 fraction, rounded half away from zero — `lerp_q16` in `sim/src/fixed.h`)
   for wind/t2/rh2, and treats `precip` as a **per-step accumulation applied at the step's
   start**. Spatially, a cell takes the weather cell containing its centre (**nearest, no
   bilinear**) in v1. Rationale: keeps per-tick math integer; bilinear can come with a minor
   version if CP4 shows visible blockiness.
3. **Amendment B — gap semantics.** A step whose `gridded_source` is `missing` (or is absent
   from the sidecar) is **held from the last valid step** (sample-and-hold), and the runner
   records `weather.held_steps` in the replay's diagnostics. A timeline that begins with
   missing steps is refused at export with a useful error. Never silently interpolate across
   a gap — matches C4's rule.
4. **Amendment C — coverage window.** A run whose `[t0, t0+duration]` extends past the
   timeline is allowed: **before** the timeline → refused; **after** its end → the last step
   is held and `weather.held_tail_s` is recorded. Scenario authors can set `[weather]
   constant = {...}` to run with no timeline at all (CP3/CP4 synthetic weather).

`schema_version` stays 0; the amendments constrain consumers, not producers.

## Alternatives considered
- Deriving dead-fuel moisture in Epic 3 and storing it as a timeline variable — rejected;
  moisture response is *model* territory (different models want different derivations) and
  the FBFM40 moisture-of-extinction pairing belongs with the fuel params pack.
- Bilinear spatial sampling in v1 — deferred (see A).
- Requiring `precip` — rejected; live HRRR windows may lack it and the sim degrades to
  "no rain" honestly (recorded in diagnostics).

## Consequences
- Epic 3 needs no code change. Notified items: (1) Jolly Mountain's bundle has `weather:
  null` — a bounded-window `--weather` bake is required for CP5; (2) the sim's `held_steps`
  diagnostic will expose any `missing` steps in a timeline as a QA signal.
- The world-pack `weather.json` records the source timeline's `t0`, `step_minutes`, per-step
  provenance, and gap list verbatim, so a replay's weather is auditable without the store.
