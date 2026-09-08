# CP6 — *The fight* (2026-09-07)

**Gate:** actions visibly alter outcome; a deliberately undersized line gets breached by
spotting; metrics move sensibly; replay of the command script is deterministic.
**Result: PASS.**

## What was produced
| artifact | scenario | what to look for |
|---|---|---|
| `cp6-pair.mp4`, `pair/000{00,12,24,36,48}.png` | `cp6-noaction` vs `cp6-campaign` | same point ignition (330, 250), same 6 m/s SW wind, 31 °C / RH 15 %, same seed, 24 h; right panel adds the campaign. Window = cells (290–400, 170–290) at 4× |
| `cp6-undersized.mp4`, `undersized/000{04,08,12}.png` | `cp6-undersized` | a single-pass dozer line (cyan) across a 9 m/s shrub run; embers (yellow arcs) cross it, the line itself never burns |
| `cp6-curves.png`, `cp6-table.md` | both arms | burned ha, containment, burning cells vs time |

The campaign (all commands in `sim/scenarios/cp6-campaign.scenario.toml`, cell coordinates):
three Type 1 hand crews each cut a third of an indirect E–W line across the head's path
(y = 195), two more cut the flanks from that line back toward the heel while a Type 2 crew ties
the heel, a large airtanker lays retardant along the indirect line twice (5 h, 9 h), a crew
fires a burnout strip on the fire side of the line at 10 h, and the heel is mopped up from 14 h.

## Checks
- **Actions alter the outcome.** No-action: the head runs north to row 181 and east to
  column 376 by 24 h (150 ha, 6 % contained). Campaign: **nothing crosses the box** — zero
  burned cells north of the indirect line, west of the west flank, east of the east flank, or
  south of the heel at any hour; the head stalls at the retardant-covered line and the burnout
  fills the strip in front of it. Total black is 165 ha (the burnout's 71 forced ignitions are
  in it), containment peaks at 24 % and ends at 10 %, cost $110,901 (placeholder rates).
- **Undersized line breached by spotting.** The 121-cell dozer line is intact at the end
  (all cells unburnable), 1,297 cells burned west of it and **1,204 east of it**; 560 spot
  launches, 76 ignitions, first breach at 3.9 h (0.9 h after the head reached the line).
  With spotting off (CP4 barrier test) the same line holds.
- **Metrics move sensibly.** Containment rises as the burnout links the fire to the line and
  falls again as the flanks grow along the unburned strip between the fire and the flank
  lines (the observer's definition — perimeter adjacent to unburned fuel — is strict and
  honest about that). Cost accrues while crews are busy and stops when they finish. Burning
  cells drop after the mop-up starts.
- **Deterministic replay.** `embersim replay` returns 0 for all three runs; the command
  script, seeds, params and world hashes are pinned in each `.replay.json`.
- **Rejected deltas:** none — the CA accepts all five kinds.

## Findings
- **Dozers stall on the Teanaway.** The first campaign draft used a Type 1 dozer for the
  indirect line; it produced **zero line**: the NWCG table gives 0 ch/h above 74 % slope and
  0–9 ch/h at 41–55 %, and the row it was sent along has a median slope of 53 % (max 94 %).
  That is the production-rate pack doing exactly what it should, on real terrain, and it is
  why the campaign uses hand crews.
- **Line porosity was a model bug.** Realistic rates exposed direct spread hopping a one-cell
  line through knight moves (CP4) and squeezing through the corners of a staircase hand line
  through diagonal moves (this checkpoint). Both are fixed in the model (spec §1 fuel-break
  rules) with property tests; CP3–CP5 were regenerated afterwards.
- **Sequencing matters.** Cutting the flank lines from the heel northward finished the north
  ends last, after the fire had reached them; cutting from the indirect line back toward the
  heel holds. A burnout anchored outside the flank lines put fire outside the box — an
  authoring error the probe caught (`ember sim probe`), not a model one.
- The whole-AOI render made a 50-cell fire unreadable; `--crop/--scale` were added to the
  debug renderer for this checkpoint.
- Air drops are a cartoon (`docs/sim/suppression.md` §4): footprint width and load are pack
  guesses, not NWCG data.

## Verdict
Suppression changes what burns, through world deltas only, with rates from the NWCG tables
and outcomes that can be read off the observers — and the sim is honest enough to let an
undersized line fail. Epic 6 has its verb list and its scoreboard.
