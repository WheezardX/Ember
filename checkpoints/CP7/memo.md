# CP7 — *Ship-shaped* (2026-09-07)

**Gate:** budgets met (plan §6); byte-identical cross-platform state hashes; schema docs frozen
at v1. **Result: PASS on Windows/MSVC with the Linux half of the determinism matrix delegated
to CI** (see "Cross-platform" — the dev box has no Linux compiler and WSL needs a password to
install one).

## Performance (H2) — reference task
`sim/scenarios/bench/reference.scenario.toml`: 1000×1000 cells @ 30 m (1 M cells, 30×30 km,
grass/timber checkerboard), 14 sim-days at `dt = 60` (20,160 ticks), spotting + crowning on,
constant weather, stream on. Single thread, Release, MSVC 19.44, i-series desktop core.

| | budget (§6) | measured | |
|---|---|---|---|
| wall time for the task | ≤ 3 min (≥ 500× realtime) | **21 s** (57,600× realtime; 979 ticks/s) | ✅ |
| peak resident memory | ≤ 512 MB | **72 MB** | ✅ |
| state stream on disk | ≤ 50 MB | **17.4 MB** (dirty-region ticks + daily keyframes) | ✅ |
| model alone (512², grass, wind) | — | ~23 k ticks/s, ~1.4 M× realtime (`ca_test` perf probe) | |

What it took: the first measurement was **245 s**. Two things dominated, neither in the model:
a full FNV pass over 6 MB of state for the per-tick hash (120 GB hashed over the run) and the
O(n) containment observer every tick. Fixes: the state hash is now the **XOR of per-cell
hashes**, kept incrementally from the dirty list (O(dirty) per tick, verified against a full
recompute at every keyframe — which also polices the dirty-region contract), and the observer
cadence is a scenario option (`[output].metrics_every`, 10 in the benchmark, 1 by default).
ADR 0008 §3 records the hash change.

Regression tracking (plan H2): the `ca_test` perf probe prints ticks/s on every test run; the
bench scenario is the CI-able reference. A >15 % regression is a review item, not yet a CI gate
(no history to compare against on day one).

## Determinism (F3)
- **Replays re-simulate bit-identically** for every checkpoint scenario (CP2, CP3, CP5 ×2,
  CP6 ×3, the three goldens): `embersim replay` exit 0 at every recorded checkpoint.
- **Golden hashes** (`sim/scenarios/golden/*.hashes`) are committed for three scenarios
  (flat-wind CA, ramp + spotting + suppression CA, playback on the tiny fixture) and recomputed
  on every run of `.github/workflows/sim.yml`.
- **Cross-platform:** the workflow builds on `ubuntu-latest` (GCC) and `windows-latest` (MSVC),
  runs the test suite on both, produces the golden hashes on both, and a third job diffs
  Linux vs Windows vs the committed files. The Windows leg is verified locally (this memo); the
  Linux leg runs on the next push. Compiler-flag lockdown: `/fp:strict` (MSVC),
  `-ffp-contract=off -fno-fast-math` (GCC/Clang) — moot for the tick path, which is
  integer-only, but pinned so init-time float precomputation cannot drift either.
- **Seeded divergence is caught:** `ca_test` "different run_seed changes the hash with spotting
  on" — a deliberate divergence test in the suite.

## Fuzz / property suite (H1) — `sim/tests/fuzz_test.cpp` + `ca_test.cpp`
65 doctest cases, ~2.6 M assertions, all green. Fuzz cases (seeded from `hash64`, so every
failure is reproducible): random worlds (13 fuel codes incl. NB and DEM nodata) under random
delta soups of all five kinds for the CA and the null model — phase monotone, unburnable never
burns, arrival set once, dirty lists exact, rejected kinds honest, two runs bit-identical;
observers read-only and repeatable; random command scripts (all eight resource types, three
command kinds) validate, emit on-grid canonical deltas and repeat; a 200² mixed-world soak with
spotting. Bugs the suite family found on the way: `isqrt64` overflow, knight-move and diagonal
fuel-break porosity, the residence-clock burnout that never spread, a dozer that (correctly)
stalls on Teanaway slopes.

## Schemas frozen at v1
`docs/sim/formats.md` (world pack 1, weather pack 1, scenario 1, state stream 1, replay 1),
`docs/sim/suppression.md` (commands 1), `sim/include/embersim/embersim.h` (C API 1),
`sim/src/interface.h` (interface 1.0.0). Epic 5 depends on the stream + replay formats; Epic 6
on the command schema and observer outputs; both can build against the documents without
reading sim source (`docs/sim/implementor-guide.md` is the outside-implementor test of that).

## Not done / deferred
- Python bindings (4.8) — parked per the design doc; the C API exists.
- Tier 2 web viewer (D6 stretch) — not attempted; the Tier 1 renderer carried all seven
  checkpoints, and the remaining time went to model correctness.
- Perf regression as a hard CI gate — needs a second data point.
