# Sim tooling (Python side) — `ember sim ...`

The Epic 4 split (plan D1): the C++ core `embersim` (`sim/`) is engine-free and reads only
**world packs**; Python owns everything geospatial and everything visual-for-QA.

| command | what |
|---|---|
| `ember sim export --historic jolly-mountain-2017 [--out store/sim]` | scenario bundle + pinned Epic 1-2 world → `store/sim/<region>.ewp/` (formats.md §1; weather pack §2 if the bundle carries a timeline) |
| `ember sim synth <flat\|ramp\|ridge\|checker\|barrier> --out ...` | procedural test worlds (plan B2), same layout |
| `ember sim static <pack> --out checkpoints/CP1` | one PNG per layer with legend + `stats.md` (CP1 gate) |
| `ember sim render <run.replay.json\|run.ess> [--world pack] --out dir [--every N --mp4 --gif]` | state stream → PNG frames → MP4/GIF |

## The debug renderer is disposable
`ember/sim/render.py` is **2D, unstyled, tile-store-native** and exists so every checkpoint
has an archivable, CI-diffable artifact. Hillshade + fuel-class tint underlay; burning cells by
intensity (yellow→red), burned dark; suppression overlays (cyan line, magenta retardant, blue
water, white forced ignition, pale-blue extinguish); spot arcs; a HUD strip (UTC clock, day
hh:mm, wind vane, containment, burned ha, burning cells, cost). numpy + Pillow only; ffmpeg
encodes. Frames are ≤ 1600 px wide (integer downsample) and byte-deterministic.

**It is not Epic 5 and must not grow into a renderer.** Epic 5 consumes the state-stream and
replay formats only; this module's compositions are a requirements crib sheet, not code to
inherit.

## Modules
- `worldpack.py` — `export_bundle`, `export_synthetic`, `load_worldpack` (memory-mapped).
- `weatherpack.py` — `export_weather` (v0 timeline → int16 pack; rows flipped to north-up;
  gaps held + flagged per ADR 0009).
- `stream.py` — `.ess` reader (`read_stream`, `iter_frames`) and byte-exact writer
  (`write_stream`, used by tests and Python tools); `read_replay`.
- `render.py` — `render_run`, `render_static`, `render_all_static`, `encode_mp4`.
