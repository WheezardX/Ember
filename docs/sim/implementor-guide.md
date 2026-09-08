# Implementing a fire model against the Ember interface (v1)

This is the research-facing on-ramp (plan H3): everything you need to plug a model into the
Ember runner **without reading the sim source**. The contract is ADR 0008; the file formats
are `formats.md`; the default model's spec (`default-model-spec.md`) is a worked example, not a
requirement. The acceptance test is the conformance suite — the same one the built-in models
pass.

## 1. What you implement
One C++20 class deriving from `embersim::IFireModel` (`sim/src/interface.h`) and one factory
function. The runner never sees anything else.

```cpp
class MyModel final : public embersim::IFireModel {
public:
    Caps caps() const override;                       // §2
    void init(const World&, const WeatherSampler*, const ParamsPack&, const Seeds&, int32_t t_start_s) override;
    TickOutput advance(int32_t dt_s, std::span<const Delta> deltas) override;   // §3
    FireStateView state() const override;             // §4
    int32_t now_s() const override;
    bool rewind(int32_t t_s) override;                // only if caps().supports_rewind
};
std::unique_ptr<IFireModel> make_my_model();
```
Register it in `sim/src/interface.cpp` (`make_model` / `model_ids`) under a string id. The
runner selects models by that id from a scenario's `[model] id = "..."`. (A dynamic plug-in
loader is deliberately not part of v1 — a model is a source contribution.)

## 2. Capabilities are a promise
`Caps` is checked, not decorative:
- `accepts` — bitmask of the delta kinds you apply. Every delta of a kind you did **not**
  declare must come back in `TickOutput.rejected` with its cell count. Declaring a kind and
  ignoring it fails conformance.
- `deterministic` — same world, params, seeds and delta sequence ⇒ identical `state_hash()`
  after every tick. Use `hash64(run_seed, SYSTEM, entity, tick, k)` (`hash.h`) for any
  randomness; there is no RNG object to share. Name each stream in `rng_streams`.
- `supports_rewind` — `rewind(t)` must reproduce exactly the state `advance` would have
  reached at `t`. Playback models say yes; simulation models normally say no.
- `max_dt_s` — the runner refuses scenarios with a larger `dt_s`. `0` means unbounded.
- `uses_weather` — if false the runner may pass a null sampler.
- `interface_version` — leave the default (`INTERFACE_VERSION`); the runner refuses a
  different major.

## 3. The tick
`advance(dt_s, deltas)` advances from `now_s()` to `now_s() + dt_s`. Rules the conformance
suite enforces:
1. **Apply deltas first**, in the order given, before spreading. Semantics per kind are in
   ADR 0008 §3; your interpretation of magnitudes may differ (that is what params are for),
   but the phase effects are fixed: `FuelRemoved` → unburnable (burning/burned cells keep
   their phase), `IgnitionForced` → burning if unburned, `ExtinguishForced` → burned if
   burning.
2. **Phase is monotone**: unburnable stays unburnable; unburned → burning → burned; never
   backwards (except inside `rewind`).
3. **`arrival_s` is written once**, when the cell first becomes burning, and is `-1` before.
4. **`dirty` is exact**: every cell whose `phase`, `intensity`, or `arrival_s` changed this
   tick is listed (sorted, unique), and nothing else. The state stream and the renderer trust
   it.
5. **Report, never drop**: unsupported delta kinds → `rejected`.
6. Spot events (if `provides_spotting`) go in `spots` on the tick they launch.
7. `diag` is yours — key/value integers for QA (active cells, held weather steps, whatever).

Everything the model needs is borrowed from `World` (read-only arrays; see `worldpack.h` for
units: elevation cm, canopy dm / g/m³, FBFM40 codes) and, per tick, from the
`WeatherSampler` the runner has already positioned at the tick's start time
(`at_cell(x, y)` → wind cm/s, T 0.1 K, RH 0.1 %, precip 0.01 mm/step; ADR 0009 for the
interpolation and gap rules).

## 4. State
`state()` returns pointers into your own arrays: `phase` (u8 0–3), `intensity` (u8 0–3),
`arrival_s` (i32), optionally `retardant` (u16 permille, or null). They must stay valid until
the next `advance`/`rewind`. `state_hash()` has a default implementation over those three
arrays; keep it unless you have a reason.

## 5. Determinism checklist
- Integer state; floats only in init-time precomputation quantised into tables.
- Iterate cells in ascending index; keep any active list sorted before you iterate.
- No `std::rand`, no `std::hash`, no unordered containers in the tick path.
- No dependence on `sizeof(long)`, `char` signedness, or evaluation order of function
  arguments.
- Build with the repository's flags (strict FP, no fast-math) — CI checks Linux GCC and
  Windows MSVC produce identical hashes on the golden scenarios.

## 6. Running the conformance suite
Add a `TEST_CASE` is not necessary: `sim/tests/conformance_test.cpp` iterates `model_ids()`.
Build and run:
```
sim\build.bat sim\build\Release configure
sim\build.bat sim\build\Release build
sim\build.bat sim\build\Release test -tc="conformance*"
```
Then run a scenario (`formats.md` §3) with `[model] id = "my-model"` and look at the result:
```
sim\build\Release\embersim run my.scenario.toml
ember sim render runs\my\my.replay.json --world <pack> --out runs\my\frames --mp4
```

## 7. Worked example: a constant-rate circle
The smallest useful model — every burning cell ignites its 8 neighbours after
`cell_mm / rate_mms` seconds, burns for `residence_s`, ignores weather, accepts only
forced ignition/extinguish. Fifty lines on top of `model_null.cpp`; it passes conformance.
Study `model_null.cpp` (the reference for delta handling and dirty lists) and
`model_playback.cpp` (the reference for `rewind`) before `model_ca.cpp` (the reference for
everything else).
