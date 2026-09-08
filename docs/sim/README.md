# Sim tooling (Python side) — `ember sim ...`

The Epic 4 split (plan D1): the C++ core `embersim` (`sim/`) is engine-free and reads only
**world packs**; Python owns everything geospatial and everything visual-for-QA.

| command | what |
|---|---|
| `ember sim export --historic jolly-mountain-2017 [--out store/sim]` | scenario bundle + pinned Epic 1-2 world → `store/sim/<region>.ewp/` (formats.md §1; weather pack §2 if the bundle carries a timeline) |
| `ember sim synth <flat\|ramp\|ridge\|checker\|barrier> --out ...` | procedural test worlds (plan B2), same layout |
| `ember sim static <pack> --out checkpoints/CP1` | one PNG per layer with legend + `stats.md` (CP1 gate) |
| `ember sim render <run.replay.json\|run.ess> [--world pack] --out dir [--every N --mp4 --gif --crop x0,y0,x1,y1 --scale N]` | state stream → PNG frames → MP4/GIF (optionally a zoomed window) |
| `ember sim compare --run LABEL=replay … --out dir [--every N --cols C --mp4 --crop … --scale N]` | side-by-side composition of N runs on a shared clock (CP4/CP5/CP6) |
| `ember sim curves --run LABEL=replay … --out png [--table md]` | burned/containment/extent curves + a response table |
| `ember sim probe <replay\|ess> [--every N]` | per-tick text table: bbox, burned/burning, spots, overlays (the fastest way to see what a run did) |
| `ember weather --historic <id> --start <ISO> --hours N` | (Epic 3 side) attach a HRRR/RAWS window to a bundle for shadow runs |

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


## The C++ core — `embersim` (`sim/`)
```
simuild.bat simuild\Release configure     # MSVC + Ninja (CI: plain cmake on Linux + Windows)
simuild.bat simuild\Release build
simuild.bat simuild\Release test            # doctest: conformance, CA properties, suppression, fuzz
simuild\Release\embersim run <scenario.toml>    # -> <name>.replay.json + <name>.ess
simuild\Release\embersim replay <x.replay.json> # re-simulate and compare every checkpoint hash
simuild\Release\embersim info <pack.ewp> | models | version
```
Documents of record: ADR 0008 (interface), ADR 0009 (weather), `default-model-spec.md`
(`ember-ca`), `suppression.md` (commands + NWCG pack), `formats.md` (all file formats),
`tuning-memo.md` (every coefficient change), `implementor-guide.md` (write your own model).

## Checkpoints (Epic 4 spine) — `checkpoints/CPn/memo.md`
| CP | name | result |
|---|---|---|
| CP1 | The world loads | PASS — five upstream findings (grid alignment, DEM edge, canopy nodata, no weather, no structures) |
| CP2 | History replays | PASS — Jolly Mountain arrival raster through the playback driver, bit-identical replay |
| CP3 | First fire | PASS — point ignition on Teanaway terrain |
| CP4 | It reads honestly | PASS — legibility matrix; params v1.1.0 tuning; two fuel-break rules |
| CP5 | Shadow of a real fire | PASS as sanity (1.5× observed growth, right lobes, no suppression) — not validation |
| CP6 | The fight | PASS — campaign holds the head; undersized line breached by spotting |
| CP7 | Ship-shaped | PASS (Windows; Linux leg in CI) — 1 M cells × 14 days in 21 s, 72 MB |
