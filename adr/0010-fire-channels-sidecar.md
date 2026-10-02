# ADR 0010 — Fire channels sidecar: the renderer draws what the source said

**Status:** Accepted (Brad, 2026-10-02: "I agree with your choices" - the three Open points below are decided as proposed). External Sources X2.1. Plan decision D2 (Brad, 2026-10-01:
"agreed, I think" - this ADR is its second look).

## Context

The replay stream (`.ess` v1, ADR 0008) carries per cell a phase, an arrival time and a 1-3
intensity class. Everything else the renderer shows about fire behaviour it infers: flame height
from the class, the head from a spread rate derived from the arrival gradient (FireTex A),
torching from the class and the vegetation. That was right when our only sources were our own
model and perimeter playbacks.

External sources speak in physical quantities. A PyreCast / ELMFIRE run publishes, per hour,
flame length, head-fire spread rate and crown-fire type for every burning cell (X1 already
samples them at each cell's arrival into the fire timeline, as raw codes). Under the rendering
rule (Brad, 2026-10-01: "when we render off models, we render true to what they predict"), when
a source says the flames were 9 m and the fire was crowning, the render must show that - not our
class guess.

Three side channels already exist for this kind of data, each ad hoc: the derived spread rate,
the NIROPS heat grids (`<pack>.heat.json` + `.heat.bin`), the intensity classes. Plan rule 4:
the stream format and its golden hashes do not change.

## Decision

1. **A channels sidecar beside the world pack**: `<pack>.channels.json` (manifest) +
   `<pack>.channels.bin` (the grids, in manifest order), on the pack grid - discovered the way
   `<pack>.heat.json` is today. No change to `.ess`, `state_hash`, the sim core or the
   conformance suite. A pack without the sidecar renders exactly as now.
   *(The plan said "referenced from `replay.json`"; beside-the-pack needs no sim change and
   matches heat - flagged for Brad, see Open 2.)*

2. **Two kinds of channel.**
   - `at_arrival`: one value per cell - what the fire was doing when it reached the cell
     (flame length, spread rate, crown type at arrival). This ADR implements only this kind.
   - `series`: one grid per timestamp (the shape of today's heat grids). Reserved; moving
     `heat.bin` into it is a follow-up ticket after X2 settles.

3. **Each channel**: `name`, `unit` (SI, the only units the renderer sees), `kind`, `dtype`
   (`u8` / `u16`), `scale` + `offset` (value = raw x scale + offset), `nodata` (the raw value
   for "the source is silent here"), and `source` {`name`, `unit` as published, `conversion`,
   `note`}. Conversion happens once, in the adapter (plan D5).

4. **Initial names.**

   | Name | Unit | Meaning | PyreCast source |
   |---|---|---|---|
   | `flame_length_m` | m | flame length at arrival | `flame-length`, ft (x 0.3048) |
   | `spread_rate_mh` | m/h | head-fire spread rate at arrival | `spread-rate`, ft/min (x 18.288) |
   | `crown_class` | class | 0 surface, 1 passive (torching), 2 active crown fire | `crown-fire`, 0 / 1 / 2 |
   | `fireline_intensity_kwm` | kW/m | optional | (not published by PyreCast) |
   | `heat_class` | class | series, reserved (NIROPS heat) | - |
   | `burn_probability` | 0-1 | reserved (ensembles) | - |

   Unit evidence: PyreCast's site states flame length in ft and surface spread rate in ft/min;
   ELMFIRE's source at `cbf924a` computes flame length as Byram's relation divided by 0.3048
   (ft), spread rate natively in ft/min (x 0.3048 only with `SPREAD_RATE_IN_M`), and crown fire
   0 / 1 / 2 by crown fraction burned (< 0.1 / 0.1-0.9 / > 0.9). Aug 20 p90 values: flame length
   1-44 ft (median 5), spread 1-174 ft/min, crown 21 % surface / 51 % passive / 28 % active.

5. **Speaks / silent.** A cell whose raw value is `nodata` is silent: the renderer does exactly
   what it does today for that cell (class rules). Never interpolated, never mixed - per cell,
   either the source's number or today's rule.

6. **Renderer use (X2.3).** Where the channel speaks: flame-lick height from `flame_length_m`
   (replacing the per-class flame lengths of flames v2), torching from `crown_class` (1: single
   trees torch; 2: continuous crown flames over the canopy), FireTex A from `spread_rate_mh`
   (replacing the arrival-gradient estimate). Smoke source strength may follow spread x flame
   length later, not in X2.

7. **Fidelity.** The sidecar gains a line per channel: coverage of the source's burned cells,
   and that the value the renderer uses equals the source value after conversion (probe facts:
   at each fire probe, the source value and the rendered value).

8. **Our own model may fill the same channels later** (the default model can compute flame
   length and crown type); not in this plan.

## Alternatives

- **`.ess` v2 with per-cell physical fields.** Cleaner long term, but a breaking format change,
  new golden hashes and a sim-core change for a benefit only external sources need now. Rejected
  for now (D2); revisit if the game model starts producing the channels.
- **Keep the raw codes in the renderer.** No: the renderer would need to know every source's
  units. One conversion point (D5).

## Decided (were open; Brad 2026-10-02, as proposed)

1. **Active crown flame length unit.** In ELMFIRE's source, active crown cells get flame length
   = 2.5 x canopy height with canopy height in metres, while every other cell is in feet - one
   raster, two units. PyreCast's published values do not match that formula against our LANDFIRE
   canopy heights (active-crown flame / canopy height: median 0.9, not 2.5), so their build or
   their canopy data differs, and the data cannot settle it. Proposal: treat every value as ft,
   as PyreCast's own viewer labels it (render true to what the source says), and note the
   anomaly in the channel's `source.note`.
2. **Where the sidecar lives**: beside the pack (proposed) vs referenced from `replay.json`
   (the plan's wording, which needs embersim to write the reference).
3. **Quantisation**: `u16`. As built (X2.2) the scale is 0.01 of the SOURCE unit - flame
   0.003048 m (0.01 ft), spread 0.18288 m/h (0.01 ft/min) - so the source's 1-ft / 1-ft/min
   steps are kept exactly (a 0.01 m step would round them by up to 5 mm); `u8` cannot.
