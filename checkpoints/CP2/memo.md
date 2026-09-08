# CP2 — *History replays* (2026-09-07)

**Gate:** progression matches Epic 3's own QA imagery; interface conformance suite passes on the
playback driver. **Result: PASS.** Zero model risk: this proves bundle → world pack → interface
→ state stream → replay → renderer, end to end, on the flagship fire.

## What was produced
```
embersim run sim/scenarios/cp2-jolly-playback.scenario.toml      # 842 hourly ticks, 2.3 s
embersim replay runs/cp2/cp2-jolly-playback.replay.json          # OK: 37 checkpoints match
ember sim render runs/cp2/cp2-jolly-playback.replay.json --out checkpoints/CP2 --every 6 --mp4
```
| artifact | what to look for |
|---|---|
| `cp2-jolly-playback.mp4` | 141 frames (one per 6 h) over hillshade + fuel tint; burning cells orange (6 h residence), burned black; HUD clock in UTC + day/hour, burned ha, burning cells, containment |
| `frames/00040.png` | day 11: the first-perimeter interior as the hard t0 block (known Epic 3 caveat) + the detached NE streak |
| `frames/00090.png` | day 23 (Sep 3): the Labor Day run — south flank and the west lobe toward Cle Elum Lake active |
| `frames/00140.png` | day 36: final footprint, 14,996 ha (Epic 3: 150.0 km²; published 148.96 km²) |

## Checks
- **Matches Epic 3 QA imagery** (`store/incidents/hist-jolly-mountain-2017/qa.html` arrival
  heatmap): same isochrone shapes on the world grid (nearest-resampled, CP1 U1), same order of
  growth (north block → west/south runs Aug 30–Sep 4 → east fill mid-September). The observed
  size series (bundle `attributes.acres`) and the stream's `burned` metric agree at every
  perimeter time within the resample tolerance.
- **Conformance suite on `arrival-playback`:** capability honesty (all five delta kinds
  rejected with counts), monotone phase, arrival set once, exact dirty regions, determinism,
  rewind round-trip — pass (`sim/tests/conformance_test.cpp`, run against every registered id).
- **Replay pins hold:** re-simulation is bit-identical at all 37 checkpoints; tampering the
  pack hash in the replay is refused with the field named (`runner_test`).
- **Stream cost:** 8.0 MB for 842 ticks over 501,600 cells (dirty-region + 24-tick keyframes).
- Playback speed: 373 ticks/s at hourly dt on 0.5 M cells (the per-tick cost is the changed
  cells plus the metrics observer's O(n) sweep — fine for playback, and the observer is the
  thing to optimise if it ever matters).

## Observations (not gates)
- The containment observer reads ~92–98 % on playback because a replayed fire's perimeter is
  mostly *cold* burned edge (nothing spreads from it). That is the observer doing its job on an
  input that has no active front; it is not a claim about the real fire's containment.
- 3,917 burned cells lie on NB fuel (roads/rock/water crossed by mapped perimeters): playback
  shows them because the raster is truth; the CA will never ignite them (CP1 note).
- The playback driver's `intensity` is 1 everywhere (`provides_intensity = false`), so the
  renderer draws one orange; confidence classes are exposed via `diag`, not colour, in v1.

## Verdict
The whole non-model half of the stack works on real data: Epic 3's progression replays
through the same interface the default model will implement, the state stream renders without
model code, and the replay re-simulates bit-for-bit. CP3 can now be judged against this.
