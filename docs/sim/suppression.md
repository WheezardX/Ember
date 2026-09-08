# Suppression sim v1 — command schema, execution model, production-rate pack (Epic 4 E1)

**Status:** appendix to ADR 0008; document of record for stories 4.5/4.6 (v1 scope fence in
EPIC_4_PLAN.md §4). Commands are the floor of Epic 6's gameplay verb list; nothing here
reaches into a fire model — the suppression sim only emits **world deltas** through the
interface.

## 1. Commands (versioned, `commands_version = 1`, recorded verbatim in the replay)
Coordinates are **cell coordinates** `[x, y]` on the world-pack grid (integers). Python
tooling (`ember sim commands`) converts metres/lon-lat; the core never sees a CRS. Every
command has `t_s` (issue time, seconds since t0) and a `resource_id` that must exist in the
scenario's `[[resources]]` list (except air drops, which name an air resource).

```toml
[[commands]]  t_s = 3600   kind = "cut_line"   resource_id = "hc-1"
              path = [[120, 40], [140, 52], [160, 60]]        # polyline, ≥ 2 points
              method = "hand" | "dozer"                        # must match the resource type

[[commands]]  t_s = 5400   kind = "air_drop"   resource_id = "at-1"
              target = [[150, 30], [170, 30]]                  # segment (2 pts) or point (1 pt)
              agent = "retardant" | "water"
              volume_class = 1 | 2 | 3                          # → footprint width / load (pack)

[[commands]]  t_s = 7200   kind = "burnout"    resource_id = "hc-1"
              anchor_path = [[120, 40], [160, 60]]              # fires along this path
              firing_pattern = "strip"                          # v1: only strip

[[commands]]  t_s = 9000   kind = "mop_up"     resource_id = "hc-2"
              region = [[100, 30], [180, 30], [180, 80], [100, 80]]   # polygon (cells)
              depth_m = 30

[[commands]]  kind = "hold"                                     # reserved; refused in v1
```
Validation at load (refusal with a useful error): on-grid, ≥ 2 path points, known resource,
method/type match, monotone-nondecreasing `t_s` not required (commands are sorted by
`(t_s, order)` at load; the sorted order is what the replay records).

## 2. Resources (`[[resources]]` in the scenario)
```toml
[[resources]] id = "hc-1"  type = "hand_t1"       # hand_t1 | hand_t2 | dozer_t1 | dozer_t2 | dozer_t3
[[resources]] id = "at-1"  type = "airtanker_large"  # airtanker_large | airtanker_seat | helicopter_bucket
```
No travel-to-incident, fatigue, crew safety, or LCES modelling in v1 (fenced; Epic 6.6).

## 3. Execution model (deterministic, integer)
A tasked ground resource has a **line task**: the command path rasterised to a cell sequence
(Bresenham per segment, deduplicated, in path order) and a `progress_mm` accumulator.
Each tick (`SYS_SUPPRESSION = 2`, no random draws in v1):
1. **Rate** = pack production rate for `(resource type, fuel class of the current cell)` in
   chains/hour (midpoint of the published range; the range is kept in the pack for Epic 6),
   converted once at load to mm/s (`1 chain = 20.1168 m`). Dozer rates additionally pick the
   slope class (0–25 / 26–40 / 41–55 / 56–74 %, from the cell's `gx, gy`) and up/down (sign of
   the elevation change along the path). A cell with rate 0 (e.g. dozer on > 74 % slope) is
   **impassable**: the task stalls and reports `stalled` in the observer stream.
2. `progress_mm += rate_mms × dt_s`; while `progress_mm ≥ cell_mm` (the along-path step length
   is the cell size for 4-neighbour steps and `cell_mm × 1.414` for diagonal steps): advance
   to the next cell and emit `FuelRemoved` for it (v1 removes the full 30 m cell — the real
   line is 1–4 m wide; the cell is the resolution we have, and CP6's memo says so).
3. A finished task frees the resource; commands issued to a busy resource **queue** behind it
   (FIFO) — no interruption in v1.

**Air drops** resolve after `sortie_delay_s(type)` (pack) into a footprint: the target segment
rasterised, then widened by `footprint_half_width_cells(type, volume_class)`. Retardant emits
`RetardantApplied{load_permille(type, class), decay_class}`; water emits
`MoistureBumped{magnitude_m10(type, class), ttl_s}`. The resource is busy for
`turnaround_s(type)` after the drop.

**Burnout** emits `IgnitionForced{cause = burnout}` along `anchor_path` progressively at
`firing_rate_mms` (pack; a strip-firing walk), one cell per step — the model treats them as any
ignition, with the same wind risk.

**Mop-up** walks the burning cells inside `region` that are within `depth_m` of the region's
boundary (the edge), in ascending cell index, emitting `ExtinguishForced` at
`mopup_cells_per_hour(type)`.

Every emitted delta is tagged with `resource_id` and recorded in the state stream's overlay
record so the viz shows lines, drops, and firing as they land.

## 4. Production-rate pack `sim/packs/production_rates.nwcg.toml`
Transcribed from **NWCG Fire Line Production Rate Tables** (Fuels Management Committee, 2021;
originally *Wildland Fire Incident Management Field Guide*, PMS 210, 2014):
- Sustained line production of 20-person crews, chains/hour, Type I and Type II (Direct):
  from *San Dimas T&DC Tech Tip 1151-1805P, Fireline Production Rates (2011)*; FM 7, 11–13
  from "various sources pre-1980".
- Dozer single-pass construction, chains/hour, by dozer type × fuel model group × slope class ×
  up/down.
- Tractor plows are **not** transcribed (not used in the PNW game profile; add when needed).
The tables are indexed by the Anderson 13 fuel models. `[fm13_from_fbfm40]` maps our FBFM40
classes onto them (GR→1, GS→2, SH→5, TU→10, TL→8, SB→11) — an editorial mapping, flagged as
such in the pack. Spot-checked values vs the source PDF: Type I direct FM1/2/3 = 17 (12–21)
ch/h; FM 8/9/10 = 10.5 (9–12); Type II direct FM 4 = 7.0 (6.2–7.9); Type II dozer FM 1,2 up
slope class 1 = 85–125.

Cost rates in the pack are **placeholders** (order-of-magnitude, clearly labelled) for the
cost observer; Epic 6 owns real numbers.

## 5. Observers (4.7) — read-only, separate module
- **Containment %**: perimeter cells = cells with `phase ∈ {2, 3}` having ≥ 1 4-neighbour with
  `phase == 1`. A perimeter cell is *contained* iff it is cold (`phase == 3`) **or** every such
  neighbour is protected (`retardant ≥ retardant_secure_permille`). Cells with no unburned
  neighbours are interior. `containment = contained / perimeter` (permyriad); 10000 when the
  perimeter is empty and area > 0.
- **Area / perimeter series**: burned+burning cell counts, hectares via `cell_size`.
- **Structure outcomes**: if the world pack has `structures`, structure cells with
  `phase ∈ {2,3}` are *lost* (with the intensity class), structure cells within 2 cells of a
  burning cell are *threatened*; without the layer the observer reports `structures = null`
  (never zeros).
- **Cost**: Σ over resources of busy seconds × `cost_per_hour` + per-drop cost.
Observers receive `const` views only and live in `sim/src/observers.cpp` with no access to
model or suppression handles (compile-time enforcement: they take `FireStateView`).
