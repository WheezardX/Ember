# ADR 0008 — Fire Model Interface v1

**Status:** Accepted (2026-09-07). Epic 4 workstream A1 (story 4.1). Versioned like a public API:
breaking changes = new major version + adapter, never an edit.

## Context
Epic 4 makes the world run. Three things must plug into one contract and be indistinguishable
to the runner: the **default cellular model** (4.2), the **arrival-raster playback driver**
(4.3), and future **external/research models**. A **suppression sim** (4.5/4.6) mutates the
world while the fire burns, and **observers** (4.7) read state without writing it. Design-doc
constraints: headless, deterministic, faster-than-realtime, and capability flags that say
honestly what a model can and cannot do so gameplay degrades gracefully.

Confirmed plan decisions this ADR implements: D1 (C++20 engine-free core with a C API),
D2 (CPU-normative determinism), D3 (fixed-point state + hierarchical seeded hashing),
D5 (sim grid = data grid 1:1, `cell_size` carried, never hardcoded), D7 (weather v0 consumed
via a co-sign, see ADR 0009), D8 (all randomness through declared seeded streams).

## Decision

### 1. Shape
- A **C++20 abstract class `embersim::IFireModel`** is the implementor contract; a flat
  **C API (`embersim.h`)** wraps the runner for UE/Python/other frontends. Models are
  registered by string id (`"ember-ca"`, `"arrival-playback"`, `"null"`).
- The core links **no geospatial or engine libraries**. Its only input is a **world pack**
  (flat little-endian arrays + JSON manifest, `docs/sim/formats.md`) produced by Python
  tooling (`ember sim export`) from Epic 1–3 stores/bundles. This is the D1 split: Python
  owns geodata I/O, C++ owns the run.
- **Time is integer seconds** from the world pack's `t0`. A run advances in ticks of `dt_s`
  (default 60); a model declares `max_dt_s` and the runner refuses larger.
- **The grid is the world pack's grid**: `nx × ny` cells, row-major, `cell_size_m` (30 for the
  game profile). Cell index `i = y*nx + x`. Everything downstream (state stream, commands,
  observers) uses the same index space.

### 2. Provided at init (immutable world views)
`WorldView` — borrowed, read-only pointers into the loaded pack:
`elevation_cm` (int32), `fbfm40` (uint8, 0 = nodata/unburnable), `cc_pct` (uint8),
`ch_dm`/`cbh_dm` (uint16, decimetres), `cbd_gm3` (uint16, g/m³), `evt` (uint16), optional
`greenness` (uint8, 0–255, absent → params default), optional `structures` (uint8 mask),
optional `arrival_s`/`confidence` (playback inputs), a `WeatherField` handle (ADR 0009),
`cell_size_m`, `nx`, `ny`, `t0_unix`. Plus an opaque **params blob** (a pack file the core does
not interpret) and a `Seeds` struct (`run_seed` u64 + per-system overrides).

### 3. Per-tick contract
```
TickOutput advance(int32 dt_s, span<const Delta> deltas)
```
- `deltas[]` are **ordered world mutations since the last tick**, applied by the model
  *before* it spreads, in the order given:
  `FuelRemoved{cells}`, `RetardantApplied{cells, load_permille, decay_class}`,
  `MoistureBumped{cells, magnitude_permille, ttl_s}`, `IgnitionForced{cells, cause}`,
  `ExtinguishForced{cells}`.
- A model **declares acceptance per delta kind** (`Caps.accepts`). Unaccepted kinds are
  returned in `TickOutput.rejected[]` with the kind and count — **reported, never silently
  dropped**. The runner logs them into the state stream so a replay shows the fight the model
  ignored.
- `TickOutput` carries: `dirty[]` (cell indices whose state changed this tick), `spots[]`
  (`{src, dst, launch_s, land_s, stream_key}`), `rejected[]`, and `diag[]` (opaque
  key/value pairs for QA: e.g. playback's confidence histogram, CA's active-cell count).
- Fire state is exposed as **structure-of-arrays** views valid until the next `advance`:
  `phase[]` (u8: 0 unburnable, 1 unburned, 2 burning, 3 burned), `intensity[]` (u8 class
  0–3), `arrival_s[]` (i32, `-1` = not yet). Invariants any implementor must hold (enforced by
  the conformance suite): unburnable never changes phase except via `FuelRemoved` (which only
  makes cells unburnable) and `IgnitionForced` is refused on unburnable; **cells never un-burn**
  (phase is monotone 1→2→3, except playback under `rewind`); `arrival_s` is set exactly once,
  when phase first becomes 2; `ExtinguishForced` acts only on burning cells (2→3).
- `state_hash()` — the XOR over all cells of `hash64(index, phase, intensity, arrival_s)`
  (`cell_hash` in `interface.h`). XOR makes it *incremental*: the runner keeps a shadow of the
  visible state and updates the hash in O(dirty) per tick, verifying against a full recompute
  at every keyframe (a mismatch means the model broke the dirty-region contract). Two runs are
  "identical" iff every ticked hash matches. This is the golden-vector currency. (v1.0 used
  FNV-1a over the whole state; replaced 2026-09-07 because a full pass over 1 M cells per tick
  dominated the reference benchmark.)

### 4. Capability flags (`Caps`)
`interface_version` (semver string), `model_id`, `model_version`, `accepts` (bitmask over the
five delta kinds), `provides_intensity`, `provides_spotting`, `deterministic`,
`supports_rewind`, `max_dt_s`, `uses_weather`, `rng_streams[]` (names of the seeded streams the
model draws from — an *honesty* declaration the fuzz suite checks by re-running with each
stream's seed perturbed). Default CA: accepts all, intensity yes, spotting yes, deterministic,
no rewind, `max_dt_s = 600`. Playback: accepts none, intensity from confidence (class 1),
spotting no, deterministic, **rewind yes**, `max_dt_s` unbounded.

### 5. Determinism contract (binding on the default model and the runner)
- **State is integer/fixed-point.** Floats may appear only in *init-time precomputation*
  (e.g. the world→weather cell map) whose results are quantized into integer tables; trig
  tables are generated once and committed as constants (`sim/src/tables.h`), so no libm call
  runs in the sim. No float enters the per-tick path.
- **Iteration order is canonical**: cells in ascending index; neighbours in the fixed
  16-direction order of the model spec; active lists are kept sorted before iteration.
- **Randomness** is `hash64(run_seed, SYSTEM, entity, tick, draw)` using Terrain's
  `splitmix64`-fold (`terrain/veg/hashing.py`, ported bit-exactly to `sim/src/hash.h`).
  SYSTEM ids are enumerated in the model spec; every draw in a model has a documented tuple.
  No global RNG state exists.
- **Cross-platform**: the same scenario + seeds must produce byte-identical state hashes on
  Linux GCC/Clang and Windows MSVC (CI matrix, F3). Implementations must not depend on
  `sizeof(long)`, `char` signedness, or unspecified evaluation order.

### 6. Versioning & conformance
- Interface version **1.0.0** is declared in `embersim/version.h`. A model reports the
  version it implements; the runner refuses a major mismatch and warns on minor.
- The **conformance suite** (`sim/tests/conformance/`) runs against **every** registered
  model with a synthetic world: capability honesty (rejected deltas are exactly the undeclared
  kinds), monotone phase, single-set arrival, determinism under fixed seeds, dirty-region
  correctness (every changed cell is listed, no unchanged cell is), rewind round-trip when
  declared, `max_dt_s` enforcement. Passing the same suite with both the playback driver and
  the CA is the proof the interface is not secretly shaped around either.

### 7. Machine-readable schemas
Chosen: **C structs in `embersim.h` (ABI) + JSON manifests for the world pack, replay,
scenario, and params files** (JSON Schema documents under `docs/sim/schemas/`). FlatBuffers
rejected — it adds a codegen toolchain for no benefit while the only consumers are our own
C++, Python, and (later) UE code; C structs with explicit widths and little-endian files are
sufficient and diff-friendly. The state stream is a documented little-endian binary
(`docs/sim/formats.md`) with a Python reader.

## Alternatives considered
- **Huygens / minimum-travel-time model as the default** — faster for static forecasts,
  hostile to mid-run world deltas. Rejected; the CA accepts deltas naturally (D4).
- **Float state with `-ffp-contract=off` discipline** — feasible but fragile under
  agent-written code and across MSVC/GCC; integer state makes hashes trustworthy (D3).
- **A Python-side interface with C++ only for the kernel** — would privilege Python
  frontends over UE; the C API keeps both cheap (D1).
- **Per-model private world formats** — rejected; one world pack keeps the playback driver
  and CA on the same pixels (CP2/CP5 comparisons).

## Consequences
- Epic 5 depends only on the state-stream and replay formats; Epic 6 on the command schema
  (ADR appendix in `docs/sim/suppression.md`) and observer outputs.
- The weather timeline v0 is consumed as-is with three amendments (ADR 0009).
- Python bindings (4.8) are a thin wrapper over `embersim.h` when demanded.
- The playback driver's `supports_rewind` and the CA's lack of it are both honest; a
  scrubbing viewer over a CA run must use the baked state stream, not the model.
