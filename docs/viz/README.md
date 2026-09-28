# Epic 5 — renderer / visualizer docs

Plan: `EPIC_5_PLAN.md` (decisions D1–D10 in §4 / §8). Checkpoint bundles: `checkpoints/HCP*/`.

| Doc | What |
|---|---|
| [harness.md](harness.md) | `ember-dev`: build / run-scenario / evaluate / loop / bless; scenario format; what PASS means |
| [scene-facts.md](scene-facts.md) | `ember-scene-facts` v1 schema (the machine-checkable half of every capture) |
| [runner.md](runner.md) | Runner provisioning (engine pin, toolchain, GPU) — `ember-dev doctor` |
| [assets.md](assets.md) | Assets from code: generators, lock, the escape hatch |

## Layout

```
unreal/engine.toml          pinned engine (5.8.3) + targets
unreal/Ember/               UE project (content-free except Content/Ember/Generated)
  Source/EmberWorld/        world streaming module; compiles worldcore/ (one source of truth)
  Source/Ember/             game mode, capture harness, scene facts, lighting rig
worldcore/                  engine-free C++20: GeoTIFF + Terrain tile-store readers,
                            seam-exact heightfield meshes (CMake + doctest; CI: viz.yml)
ember/dev/                  ember-dev (Python)
viz/scenarios/              render scenarios        viz/goldens/  blessed captures
viz/budgets.toml            D8 perf budgets
assets/generators/          asset generator scripts + manifest; assets/generated.lock.json
```

## World data

The renderer reads Terrain's tile store directly at runtime (D3, D10): `manifest.json`
(schema v2) + per-tile uncompressed GeoTIFFs under `tiles/z{lod}/x{col}/y{row}/`. Coordinates:
UE X = east, Y = south, Z = up, centimetres, anchored at the tile grid's centre and `z_min`.
