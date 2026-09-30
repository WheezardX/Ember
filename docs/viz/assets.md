# Assets from code (EPIC_5_PLAN §3, workstream B)

Every `.uasset` in `unreal/Ember/Content/Ember/Generated/` is produced by a script in
`assets/generators/`, run headless:

```
ember-dev regen-assets            # UnrealEditor-Cmd <uproject> -run=pythonscript -script=<gen>.py
ember-dev regen-assets --check    # engine-free: lock current? no unclaimed assets? (CI: viz.yml)
```

* `assets/generators/manifest.toml` lists each generator and the asset paths it owns.
* A generator must print `EMBER_GENERATED <asset path>` for each output (the runner checks).
* `assets/generated.lock.json` records each script's sha256 (line-ending-normalised) and its
  outputs; `regen-assets` rewrites it only when every generator succeeds.
* `.uasset`s are committed for build speed but are build artifacts. Byte-identical regeneration
  is not asserted (packages embed GUIDs); cleanliness is defined on inputs: no generator edited
  without regenerating, no asset in the generated folder that no generator claims.
* Runtime code touches assets only through Material Instance Dynamics / Niagara user
  parameters. A new look = a new parameter or new generator code, never a hand edit.

## Generated assets

| Asset | Generator | Parameters | Used by |
|---|---|---|---|
| `/Game/Ember/Generated/M_EmberGray` | `m_ember_gray.py` | `Color` (vector, 0.35 gray), `Roughness` (scalar, 0.9) | `AEmberTerrainActor` clay mode (`look = "clay"`: fixtures, debugging) |
| `/Game/Ember/Generated/M_Veg` + `Veg/SM_<palette key>` (5) | `veg_species.py` | `Color` (foliage), `TrunkColor`, `Roughness`; meshes: vertex RGB = shading, alpha = bark 0 / foliage 1 | `AEmberVegetationActor` (one HISM per species) |
| `/Game/Ember/Generated/Ground/T_Ground_<Set>_{C,N}` (Litter, Grass, Rock, Shrub) | `t_ground.py` (pixels from `ground_tex_host.py` in the host Python via `EMBER_HOST_PYTHON`: numpy is not in UE) | C: sRGB colour multiplier (mean 0.5) + height in A; N: linear slope offset RG, roughness B, AO A; 1024^2, wrap, NeverStream | `M_Terrain` ground detail (ground plane v1) |
| `/Game/Ember/Generated/Cover/SM_Cover_<item>_v<N>` + `MI_Cover_<item>` (fern, huckleberry, shrub, rock, log, stump, snag, pole) | `ground_cover.py` (geometry: `groundgen.py`; new items go at the END of `ITEMS`, whose index seeds each mesh) | M_Veg instances (Color / TrunkColor) | `AEmberGroundCoverActor` (near-camera cover, rules in the look's `[cover]`; grass reuses `SM_bunchgrass_v*`). Per item `pose`: `upright` (default: lowest point under the footprint), `conform` (logs: along the rendered surface under both ends, rolled to the cross slope, resting on humps, 20-35 % buried, ~15 % propped), `leaner` (poles: foot dug in, pitched 20-40 deg into the crowns). Lying meshes run along +X from their foot. |
| `/Game/Ember/Generated/M_Terrain` | `m_terrain.py` | `Albedo` (texture, per tile), `AlbedoScale`/`AlbedoOffset` (UV0 → texture, skips the apron), `DetailStrength` (0.10), `DetailScale` (0.004 /cm), `Roughness` (0.92) | `AEmberTerrainActor` look mode: albedo composed per tile by `worldcore/look.cpp` from `viz/looks/*.toml` |

## Writing a generator

```python
import unreal
eal, mel = unreal.EditorAssetLibrary, unreal.MaterialEditingLibrary
if eal.does_asset_exist(FULL): eal.delete_asset(FULL)       # regenerate from scratch
mat = unreal.AssetToolsHelpers.get_asset_tools().create_asset(NAME, PATH, unreal.Material,
                                                              unreal.MaterialFactoryNew())
p = mel.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -500, 0)
p.set_editor_property("parameter_name", "Roughness")
mel.connect_material_property(p, "", unreal.MaterialProperty.MP_ROUGHNESS)
mel.recompile_material(mat)
eal.save_asset(FULL, only_if_is_dirty=False)
unreal.log(f"EMBER_GENERATED {FULL}")
```

Add it to `manifest.toml`, run `ember-dev regen-assets`, commit script + asset + lock together.

**Pin names are not what the editor UI suggests.** `connect_material_expressions` returns
`False` on a wrong pin name instead of raising, so a graph can "generate fine" with a node
silently disconnected (M_Terrain's detail noise did, until checked). Wrap every connection in a
raising helper (see `m_terrain.py: link()`), and look pins up with
`MaterialEditingLibrary.get_material_expression_input_names(expr)` — e.g. Noise's position pin is
`"World Position"`, TextureSample's UV pin is `"UVs"`.

## Escape hatch (plan §3)

If an asset cannot be script-authored at acceptable quality (most likely tree meshes), the
fallback is one logged human acquisition/authoring session per checkpoint phase, recorded in an
ADR with provenance and licence (Fab/Megascans terms only; agents never scrape assets). None so
far.
