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
| `/Game/Ember/Generated/M_EmberGray` | `m_ember_gray.py` | `Color` (vector, 0.35 gray), `Roughness` (scalar, 0.9) | `AEmberTerrainActor` (Phase 0 clay terrain; debug material after B2) |

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

## Escape hatch (plan §3)

If an asset cannot be script-authored at acceptable quality (most likely tree meshes), the
fallback is one logged human acquisition/authoring session per checkpoint phase, recorded in an
ADR with provenance and licence (Fab/Megascans terms only; agents never scrape assets). None so
far.
