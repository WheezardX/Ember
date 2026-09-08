# CP3 — *First fire* (2026-09-07)

**Gate:** fire spreads plausibly outward, respects unburnable cells, never un-burns;
bit-identical re-run. **Result: PASS** (with `ca_params.v1.toml` at v1.1.0 — the v1.0.0 pack
produced a 10 ha fire in 24 h, which is why CP4's tuning was done before this memo was written;
see `docs/sim/tuning-memo.md`).

## What was produced
```
embersim run sim/scenarios/cp3-jolly-point.scenario.toml     # 2160 ticks (36 h at 60 s), 8.0 s
embersim replay runs/cp3/cp3-jolly-point.replay.json         # OK: 37 checkpoints match
ember sim render … --every 90 --mp4                          # 25 frames, one per 1.5 h
```
Point ignition at cell (330, 250) — a GS2 timber-grass patch at 1,477 m NW of the eventual
Jolly Mountain footprint — under constant benign weather (1.5 m/s westerly, 28 °C, RH 20 %,
no precipitation), spotting and crowning on at pack defaults, no suppression.

| artifact | what to look for |
|---|---|
| `cp3-jolly-point.mp4` | orange burning rim advancing outward, black interior; the front follows terrain (uphill lobes) and fuel (skips rock/barren, slows in litter) |
| `frames/00012.png` | 18 h: 32 ha, elongated along the slope, not the wind |
| `frames/00024.png` | 36 h: 121 ha |

Growth series (from `ember sim curves`): 9 h → 14 ha (radius 309 m); 18 h → 32 ha (498 m);
27 h → 69 ha (1.0 km); 36 h → 121 ha (1.3 km). Frames are rendered cropped to cells
(250–450, 150–330) at 3× so the front reads; `frames/render.json` records the window.

## Checks
- **Outward, plausible:** the burned set grows monotonically with a continuous front; roughly
  linear radius growth after the first hours; ~120 ha in a day and a half from a point in mixed
  timber-grass under light wind is a believable slow day, and the shape is terrain-driven (the
  slope-equivalent wind dominates a 1.5 m/s breeze), which is the legible behaviour CP4 wants.
- **Unburnable respected:** no NB / DEM-nodata cell ever burns (property tests + the stream: the
  observer's burned count never includes phase-0 cells); the fire skirts the rock and barren
  patches visible in `fbfm40.png`.
- **Never un-burns:** phase monotone under the conformance suite for `ember-ca`; arrival set once.
- **Bit-identical re-run:** `embersim replay` matches all 37 checkpoint hashes; final
  `0x631afd562e1b17e1` (state hash = XOR of per-cell hashes since the CP7 perf pass; earlier FNV values differ).
- Runner throughput on the 0.5 M-cell world: 271 ticks/s (the O(n) metrics observer per tick
  dominates; the model alone runs ~20k ticks/s on a 512² world). Within the §6 budget with
  ~2× margin; H2 will hash/observe at checkpoints only if more is needed.

## Findings
- The default-pack pace was the real CP3 finding (fixed at CP4). A point fire is the wrong
  instrument for judging *rate*; it is the right one for judging *shape*, which reads well.
- 16-direction anisotropy (~10 %) is visible as slightly faceted rings in the calm phases.

## Verdict
The default model's core loop works on real terrain and behaves like a fire: it spreads
outward, is shaped by slope and fuel, honours barriers, and reproduces exactly.
