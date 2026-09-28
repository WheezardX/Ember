"""Generator: /Game/Ember/Generated/M_Terrain — the terrain master (EPIC_5_PLAN B2).

Ground colour is composed per tile on the CPU from store layers (worldcore/look.cpp, rules in
viz/looks/*.toml) and bound as a texture; this material only samples it and adds lighting-scale
detail so close-ups are not flat 10 m texels. Runs headless via `ember-dev regen-assets`.

Parameters (the runtime surface; docs/viz/assets.md):
    Albedo          texture2D  per-tile albedo (apron included)     default WhiteSquareTexture
    AlbedoScale     scalar     UV0 -> texture scale (skip apron)    default 1
    AlbedoOffset    scalar     UV0 -> texture offset                default 0
    DetailStrength  scalar     +/- brightness from world noise      default 0.10
    DetailScale     scalar     noise frequency (1/cm)               default 0.004  (~2.5 m features)
    Roughness       scalar                                          default 0.92
"""

import unreal

PATH = "/Game/Ember/Generated"
NAME = "M_Terrain"
FULL = f"{PATH}/{NAME}"

eal = unreal.EditorAssetLibrary
mel = unreal.MaterialEditingLibrary

if eal.does_asset_exist(FULL):
    eal.delete_asset(FULL)
mat = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
    NAME, PATH, unreal.Material, unreal.MaterialFactoryNew())


def expr(cls, x, y):
    return mel.create_material_expression(mat, cls, x, y)


def link(src, out, dst, inp):
    # MaterialEditingLibrary returns False on a bad pin name instead of raising; make it loud.
    if not mel.connect_material_expressions(src, out, dst, inp):
        raise RuntimeError(f"connect failed: {src.get_name()}.{out!r} -> {dst.get_name()}.{inp!r}")


def to_property(src, out, prop):
    if not mel.connect_material_property(src, out, prop):
        raise RuntimeError(f"connect failed: {src.get_name()}.{out!r} -> {prop}")


def scalar(name, value, x, y):
    e = expr(unreal.MaterialExpressionScalarParameter, x, y)
    e.set_editor_property("parameter_name", name)
    e.set_editor_property("default_value", value)
    return e


# UV = TexCoord0 * AlbedoScale + AlbedoOffset
uv = expr(unreal.MaterialExpressionTextureCoordinate, -1400, -200)
s = scalar("AlbedoScale", 1.0, -1400, -80)
o = scalar("AlbedoOffset", 0.0, -1400, 20)
mul = expr(unreal.MaterialExpressionMultiply, -1200, -150)
link(uv, "", mul, "A")
link(s, "", mul, "B")
add = expr(unreal.MaterialExpressionAdd, -1050, -120)
link(mul, "", add, "A")
link(o, "", add, "B")

tex = expr(unreal.MaterialExpressionTextureSampleParameter2D, -850, -200)
tex.set_editor_property("parameter_name", "Albedo")
white = unreal.load_asset("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture")
tex.set_editor_property("texture", white)
tex.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
link(add, "", tex, "UVs")

# detail = 1 + noise(world position * DetailScale) * DetailStrength, noise in [-1, 1]
wp = expr(unreal.MaterialExpressionWorldPosition, -1400, 250)
dscale = scalar("DetailScale", 0.004, -1400, 360)
wpm = expr(unreal.MaterialExpressionMultiply, -1200, 280)
link(wp, "", wpm, "A")
link(dscale, "", wpm, "B")
noise = expr(unreal.MaterialExpressionNoise, -1050, 280)
noise.set_editor_property("scale", 1.0)
noise.set_editor_property("levels", 4)
noise.set_editor_property("output_min", -1.0)
noise.set_editor_property("output_max", 1.0)
link(wpm, "", noise, "World Position")  # pin names: mel.get_material_expression_input_names()
strength = scalar("DetailStrength", 0.10, -1050, 420)
nm = expr(unreal.MaterialExpressionMultiply, -850, 300)
link(noise, "", nm, "A")
link(strength, "", nm, "B")
one = expr(unreal.MaterialExpressionConstant, -850, 420)
one.set_editor_property("r", 1.0)
detail = expr(unreal.MaterialExpressionAdd, -650, 320)
link(one, "", detail, "A")
link(nm, "", detail, "B")

base = expr(unreal.MaterialExpressionMultiply, -450, -100)
link(tex, "RGB", base, "A")
link(detail, "", base, "B")
to_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)

rough = scalar("Roughness", 0.92, -450, 150)
to_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)

mel.recompile_material(mat)
eal.save_asset(FULL, only_if_is_dirty=False)
unreal.log(f"EMBER_GENERATED {FULL}")
