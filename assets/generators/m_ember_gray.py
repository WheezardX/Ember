"""Generator: /Game/Ember/Generated/M_EmberGray — the Phase 0 gray-shaded terrain master.

Runs INSIDE the editor in headless commandlet mode (`ember-dev regen-assets`), never the UI.
Parameters (the only runtime surface; see docs/viz/assets.md):
    Color      vector   base colour           default (0.35, 0.35, 0.35)
    Roughness  scalar   GGX roughness         default 0.9
Replaced by the M_Terrain master in Phase 1 (B2); kept as the debug/clay material.
"""

import unreal

PATH = "/Game/Ember/Generated"
NAME = "M_EmberGray"
FULL = f"{PATH}/{NAME}"

eal = unreal.EditorAssetLibrary
mel = unreal.MaterialEditingLibrary

if eal.does_asset_exist(FULL):
    eal.delete_asset(FULL)
tools = unreal.AssetToolsHelpers.get_asset_tools()
mat = tools.create_asset(NAME, PATH, unreal.Material, unreal.MaterialFactoryNew())

color = mel.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -500, -100)
color.set_editor_property("parameter_name", "Color")
color.set_editor_property("default_value", unreal.LinearColor(0.35, 0.35, 0.35, 1.0))
mel.connect_material_property(color, "", unreal.MaterialProperty.MP_BASE_COLOR)

rough = mel.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -500, 150)
rough.set_editor_property("parameter_name", "Roughness")
rough.set_editor_property("default_value", 0.9)
mel.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)

mel.recompile_material(mat)
eal.save_asset(FULL, only_if_is_dirty=False)
unreal.log(f"EMBER_GENERATED {FULL}")
