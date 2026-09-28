"""Generator: /Game/Ember/Generated/M_Water — flat lake surface (HCP1 lakes-as-holes fix).

Parameters:
    Color      vector   deep-water albedo      default sRGB #1E3440
    Roughness  scalar                           default 0.06 (reflects sky / shore via Lumen)
    Specular   scalar                           default 0.5
Wave normals are a later refinement (uv0 is world metres / 100 for that).
"""

import unreal

PATH = "/Game/Ember/Generated"
NAME = "M_Water"
FULL = f"{PATH}/{NAME}"
eal = unreal.EditorAssetLibrary
mel = unreal.MaterialEditingLibrary

if eal.does_asset_exist(FULL):
    eal.delete_asset(FULL)
mat = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
    NAME, PATH, unreal.Material, unreal.MaterialFactoryNew())


def to_property(src, prop):
    if not mel.connect_material_property(src, "", prop):
        raise RuntimeError(f"connect failed -> {prop}")


def scalar(name, value, y):
    e = mel.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -400, y)
    e.set_editor_property("parameter_name", name)
    e.set_editor_property("default_value", value)
    return e


col = mel.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -400, -150)
col.set_editor_property("parameter_name", "Color")
col.set_editor_property("default_value", unreal.LinearColor(0.013, 0.036, 0.052, 1.0))  # #1E3440
to_property(col, unreal.MaterialProperty.MP_BASE_COLOR)
to_property(scalar("Roughness", 0.06, 50), unreal.MaterialProperty.MP_ROUGHNESS)
to_property(scalar("Specular", 0.5, 200), unreal.MaterialProperty.MP_SPECULAR)
mel.recompile_material(mat)
eal.save_asset(FULL, only_if_is_dirty=False)
unreal.log(f"EMBER_GENERATED {FULL}")
