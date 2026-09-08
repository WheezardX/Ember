# CP5 — *Shadow of a real fire* (2026-09-07)

**Gate:** order-of-magnitude sanity (growth direction / major runs on the right days);
explicitly **not** validation. **Result: PASS as a sanity check, with clear divergences
recorded below.**

> **Disclaimer (reuse verbatim for 8.4).** `ember-ca` is a legibility-tuned cartoon of fire
> behaviour. This comparison shows whether it behaves like *a* fire under the real weather that
> Jolly Mountain experienced; it does not show that it reproduces *that* fire, and nothing here
> should be read as a forecast, a reconstruction, or a claim about how the incident was managed.
> Suppression, which shaped the real perimeter every day, is absent from the model run.

## Setup
Both runs share the world pack, the clock, and the 120 h HRRR window attached to the bundle
(`ember weather --start 2017-08-30T12:00Z --hours 120`; 8×7 cells at 3 km; 8 held gap steps;
no precipitation in the source).

| run | scenario | what it is |
|---|---|---|
| observed | `cp5-jolly-shadow-playback` | Epic 3 arrival raster over the window (mapped perimeters, interpolated timing) |
| ember-ca | `cp5-jolly-shadow-ca` | the observed burned set at 08-30 12Z force-ignited (`ignite_from_arrival`), then the CA free-running under the HRRR winds/RH/T, spotting + crowning on, **no suppression** |

Artifacts: `cp5-jolly-shadow-split.mp4` (split-screen, one frame per hour), keyframes
`00030/00060/00090/00120.png` (days 1.25 / 2.5 / 3.75 / 5), `cp5-curves.png`, `cp5-table.md`.

## What the weather did (HRRR grid means over the AOI)
| day | mean wind | min RH | max T |
|---|---|---|---|
| Aug 30 | 5.1 m/s from the NW | 20 % | 29 °C |
| Aug 31 | 4.8 m/s from the NW | 24 % | 28 °C |
| Sep 1 | 2.6 m/s | 19 % | 32 °C |
| Sep 2 | 2.8 m/s | 16 % | 34 °C |
| Sep 3 | 2.8 m/s | 15 % | 33 °C |

## What happened vs what the model did
| | observed (raster) | ember-ca |
|---|---|---|
| burned at start (08-30 12Z) | 3,091 ha | 3,091 ha (same set) |
| burned at end (09-04 12Z) | 9,382 ha (+6,290) | 12,280 ha (+9,190) |
| growth on Aug 30–31 (windy) | +1,650 ha | +1,900 ha (to 4,987 ha by 09-01 12Z) |
| growth Sep 1–3 (hot, dry, lighter wind) | +4,600 ha | +7,300 ha |
| shape | lobes W/SW toward Cle Elum Lake and S; N and E edges held | expands on all sides; the SW lobe and S run appear, but so do N and E runs that never happened |

Reading it honestly:
- **Order of magnitude: yes.** About 1.5× the observed growth over five days, with the same start.
  A free-burning cartoon with no line, no retardant and no night-time RH recovery beyond what
  3 km HRRR gives it, landing within 1.5× of a fire that had a Type 1 team on it, is the right
  ballpark for this checkpoint.
- **Direction: partly.** The model finds the SW/S runs (fuel + slope + the NW wind push them
  there), which is the important qualitative check. It also runs N and E, where the real
  perimeter was held — by suppression and by the burn-out days that this window straddles —
  and nothing in the model resists that. CP6 is where line and retardant enter.
- **Days: the wrong emphasis.** Observed growth was steadier through the windy Aug 30–31 and
  accelerated Sep 2–4; the model grows fastest Sep 1–3 on RH and temperature (its moisture
  response), suggesting the RH/T→moisture cartoon is too strong relative to the wind term for
  timber. A tuning candidate, not a fix to make now: it would be tuning to one fire.
- **Flanks too fast.** The CA's burned set is rounder than the real one; the ellipse envelope
  (spec §11, tuning memo) lets flanks fill in from diagonal wavelets. This is the second
  candidate for a future tuning pass, again deferred to avoid fitting one incident.
- **Playback artefacts:** the observed `burning` count saw-tooths with the perimeter cadence
  (whole interpolation intervals ignite together) and its containment reads ~90 % because a
  replayed perimeter is mostly cold — both are properties of the raster, not of the fire.

Numbers above are from the run after the fuel-break rules landed (final hash
`0x3571e54b70f9c1e2`); the first run, before roads and rock edges were impassable to diagonal
moves, over-burned by 2× instead of 1.5× — the rules matter on real terrain.

## Verdict
The whole stack runs end to end on real terrain with real weather and produces a fire of the
right size and broadly the right shape over five real days. The divergences are the expected
ones (no suppression, cartoon moisture response, round flanks) and are recorded rather than
tuned away. Nothing here is validation, and the memo says so in language Epic 8.4 can reuse.
