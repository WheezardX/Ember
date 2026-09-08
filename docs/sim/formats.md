# Sim file formats v1 — world pack, weather pack, scenario, state stream, replay

**Status:** reference for ADR 0008 §7. All binary files are **little-endian**; all arrays are
**row-major, `y` (south) outer, `x` (east) inner**. Versions are independent integers; a
consumer refuses a major it does not know. Python readers/writers: `ember/sim/`; C++:
`sim/src/`.

## 1. World pack (`<name>.ewp/`, version 1) — produced by `ember sim export`
The **only** world input the core reads. A directory:
```
world.json          manifest (below)
elevation_cm.bin    i32   DEM in centimetres; INT32_MIN = nodata (unburnable)
fbfm40.bin          u8    Scott & Burgan 40 code; 0 = nodata
cc_pct.bin          u8    canopy cover %, 0..100 (nodata → 0)
ch_dm.bin           u16   canopy height, decimetres (nodata → 0)
cbh_dm.bin          u16   canopy base height, decimetres (nodata → 0)
cbd_gm3.bin         u16   canopy bulk density, g/m³ (kg/m³ × 1000; nodata → 0)
evt.bin             u16   EVT code (informational)
greenness.bin       u8    optional, 0 cured .. 255 green
structures.bin      u8    optional mask (absent upstream today)
arrival_s.bin       i32   optional, seconds since t0, -1 = never (playback input)
confidence.bin      u8    optional, Epic 3 confidence class (0 = unburned)
hillshade.bin       u8    optional, viz underlay only
weather.json/.bin   optional weather pack (§2)
```
`world.json`:
```json
{ "format": "ember-world-pack", "version": 1, "name": "hist-jolly-mountain-2017",
  "grid": { "nx": 800, "ny": 627, "cell_size_m": 30.0, "crs": "EPSG:32610",
            "origin_x": 640781.5, "origin_y": 5254513.3 },          // top-left corner, metres
  "t0_utc": "2017-08-12T00:00:00Z", "t0_unix": 1502496000,
  "source": { "bundle": "store/incidents/hist-jolly-mountain-2017/package/scenario.bundle.json",
              "incident_id": "hist:jolly-mountain-2017", "world_region": "...",
              "world_manifest_hash": "5fd2…", "ember_version": "0.0.1", "exported_utc": "…" },
  "layers": { "elevation_cm": { "file": "elevation_cm.bin", "dtype": "i32", "unit": "cm",
                                "stats": { "min": 121, "max": 213236, "valid": 488261, "nodata": 13339 } }, … },
  "arrival": { "algorithm": "perimeter-interp-v1", "resampled_from_grid": { "nx": 847, "ny": 599,
               "origin_x": 640065.0, "origin_y": 5253986.3 }, "method": "nearest" } | null,
  "weather": "weather.json" | null,
  "pack_hash": "sha256 over the listed layer files, in manifest order" }
```
Synthetic worlds (`ember sim synth`) write the same layout with `"source": {"synthetic": …}`
and an arbitrary `crs` (`"LOCAL"`).

## 2. Weather pack (`weather.json` + `weather.bin`, version 1) — ADR 0009 form
```json
{ "format": "ember-weather-pack", "version": 1,
  "grid": { "nx": 15, "ny": 19, "dx_m": 3000.0, "dy_m": 3000.0, "origin_x": …, "origin_y": … },
  "t0_unix": …, "step_s": 3600, "num_steps": 6,
  "variables": ["wind10_u", "wind10_v", "t2", "rh2", "precip"],
  "quant": { "wind10_u": "cm/s", "wind10_v": "cm/s", "t2": "0.1K", "rh2": "0.1%", "precip": "0.01mm" },
  "file": "weather.bin",
  "steps": [ { "index": 0, "valid_time": "…", "source": "hrrr:anl", "held_from": null }, … ],
  "gaps": [], "precip_present": false,
  "source_timeline": "weather/timeline.v0.json" }
```
`weather.bin`: `i16[num_steps][5][ny][nx]`. The five variables are always present in this order;
a variable the source lacked is all zeros and named in `"missing_variables"`. A step held from a
previous one (gap) is materialised in the file **and** flagged in `steps[i].held_from`.

## 3. Scenario (`*.scenario.toml`, `scenario_version = 1`) — `embersim run`
```toml
scenario_version = 1
[scenario]
name = "cp3-point-ignition"
world = "../store/sim/hist-jolly-mountain-2017.ewp"   # relative to this file
t_start_s = 0            # seconds after the pack's t0 (playback: where to begin)
ignite_from_arrival = false   # true: every cell with 0 <= arrival_s <= t_start_s is force-ignited at t_start_s (shadow runs, CP5)
duration_s = 86400
dt_s = 60
seed = 20260907          # run_seed (u64)

[model]
id = "ember-ca"          # ember-ca | arrival-playback | null
params = "../sim/packs/ca_params.v1.toml"
[model.overrides]        # optional dotted overrides into the params pack
"spotting.enabled" = false

[weather]
mode = "pack"            # pack | constant   (pack requires the world pack to carry one)
[weather.constant]       # used when mode = "constant"
wind10_u_cms = 0
wind10_v_cms = 0
t2_dk = 2981             # 25 °C
rh2_dpct = 250           # 25 %
precip_cmm = 0

[[ignitions]]
t_s = 0
cells = [[400, 300]]
cause = "scenario"

[[resources]]            # see docs/sim/suppression.md
id = "hc-1"
type = "hand_t1"
[[commands]]
t_s = 3600
kind = "cut_line"
resource_id = "hc-1"
path = [[380, 280], [420, 280]]
method = "hand"
# commands_file = "campaign.commands.toml"   # alternative: [[commands]] in a separate file

[output]
dir = "runs/cp3-point-ignition"   # relative to this file; <name>.ess + <name>.replay.json
keyframe_every = 60               # ticks
metrics_every = 1                 # observer cadence in ticks (1 = every tick; the HUD carries values between)
stream = true
```

## 4. State stream (`<name>.ess`, version 1)
Header:
```
u8[8]  magic "EMBRSTRM"     u32 version = 1
u32 nx   u32 ny   u32 cell_mm   i64 t0_unix   u32 dt_s   u32 keyframe_every   u32 flags
u8[32] world_pack_sha256    str model_id    str model_version    str interface_version
u32 n_resources { str id  str type }
```
(`str` = `u32 len` + UTF-8 bytes.) Then records until `kind = 3`:
```
u8 kind = 2  KEYFRAME:  u32 tick  i32 t_s
             u32 n_runs { u32 len  u8 phase }         RLE over all cells, index order
             u32 n_runs { u32 len  u8 intensity }
u8 kind = 1  TICK:      u32 tick  i32 t_s  u64 state_hash
             u32 n_dirty   { u32 idx  u8 phase  u8 intensity  i32 arrival_s }
             u32 n_spots   { u32 src  u32 dst  i32 launch_s  i32 land_s  u8 landed  u8 ignited }
             u32 n_overlay { u8 delta_kind  u32 idx  u16 magnitude  u16 resource_idx }
             u32 n_rejected{ u8 delta_kind  u32 count }
             METRICS: i32 containment_permyriad  u32 burning  u32 burned  u32 perimeter
                      i32 structures_lost (-1 = no layer)  i32 structures_threatened
                      i64 cost_cents  u32 busy_resources
                      i32 wind_u_cms  i32 wind_v_cms  i32 m10   (grid-mean weather, HUD)
             u32 n_diag { str key  i64 value }
u8 kind = 3  END:       u32 ticks  u64 final_hash
```
A keyframe is written after init (tick 0, before any tick record) and every `keyframe_every`
ticks. Ticks with nothing dirty still write a tick record (metrics + hash). Delta kinds:
1 FuelRemoved, 2 RetardantApplied, 3 MoistureBumped, 4 IgnitionForced, 5 ExtinguishForced.

## 5. Replay (`<name>.replay.json`, version 1)
```json
{ "format": "ember-replay", "version": 1,
  "interface_version": "1.0.0", "embersim_version": "0.1.0", "commands_version": 1, "stream_version": 1,
  "model": { "id": "ember-ca", "version": "1.0.0", "params_file": "…", "params_sha256": "…",
             "overrides": { … } },
  "world": { "pack": "…", "pack_sha256": "…", "world_manifest_hash": "…", "grid": { … } },
  "scenario": { …the resolved scenario, cells and seconds… },
  "seeds": { "run_seed": 20260907 },
  "commands": [ …sorted, validated… ],
  "result": { "ticks": 1440, "final_state_hash": "0x…", "checkpoints": [ { "tick": 0, "hash": "0x…" }, … ],
              "diagnostics": { "weather_held_steps": 0, "weather_held_tail_s": 0, "rejected_deltas": { … } },
              "timing": { "load_ms": …, "run_ms": …, "ticks_per_s": … } },
  "stream": "cp3-point-ignition.ess" | null }
```
`embersim replay X.replay.json` reloads the pinned pack (refusing on `pack_sha256` /
`world_manifest_hash` / version mismatch with a message naming the field), re-simulates, and
compares every checkpoint hash. `ember sim render X.replay.json` renders from the stream
without any model code.
