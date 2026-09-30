"""Generator: /Game/Ember/Generated/M_Firebrand - glowing firebrand / spot-fire sprites (EPIC_5_PLAN
HCP4 H4-4: spot fires light up where the stream says; ember streaks from the head toward them).

Additive, unlit, camera-facing cards (AEmberFirebrandActor lays them out): a soft round glow with a
hot core, so a streak of cards reads as a spark trail. Per-instance custom data:
    0  intensity   (brightness; fades with the brand's age)
    1  heat        (0 = deep red ember .. 1 = yellow-white spark / new spot fire)
Parameters:
    Gain       scalar  overall brightness           default 1
Runs headless via `ember-dev regen-assets`.
"""

import unreal

PATH = "/Game/Ember/Generated"
eal = unreal.EditorAssetLibrary
mel = unreal.MaterialEditingLibrary
tools = unreal.AssetToolsHelpers.get_asset_tools()


def build_material():
    full = f"{PATH}/M_Firebrand"
    if eal.does_asset_exist(full):
        eal.delete_asset(full)
    mat = tools.create_asset("M_Firebrand", PATH, unreal.Material, unreal.MaterialFactoryNew())

    def link(src, out, dst, inp):
        if not mel.connect_material_expressions(src, out, dst, inp):
            raise RuntimeError(f"connect failed: {out!r} -> {inp!r}")

    def custom(name, x, y, inputs, code):
        c = mel.create_material_expression(mat, unreal.MaterialExpressionCustom, x, y)
        c.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
        c.set_editor_property("description", name)
        ins = []
        for n in inputs:
            ci = unreal.CustomInput()
            ci.set_editor_property("input_name", n)
            ins.append(ci)
        c.set_editor_property("inputs", ins)
        c.set_editor_property("code", code)
        return c

    def cdata(i, y):
        e = mel.create_material_expression(mat, unreal.MaterialExpressionPerInstanceCustomData,
                                           -900, y)
        e.set_editor_property("data_index", i)
        return e

    inten, heat = cdata(0, 0), cdata(1, 100)
    uv = mel.create_material_expression(mat, unreal.MaterialExpressionTextureCoordinate, -900, -150)
    gain = mel.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -900, 250)
    gain.set_editor_property("parameter_name", "Gain")
    gain.set_editor_property("default_value", 1.0)

    # Soft intersection with the ground: a spot-fire flare sits on the slope, no hard clip line.
    one = mel.create_material_expression(mat, unreal.MaterialExpressionConstant, -900, 350)
    one.set_editor_property("r", 1.0)
    dist = mel.create_material_expression(mat, unreal.MaterialExpressionConstant, -900, 450)
    dist.set_editor_property("r", 1500.0)   # cm
    fade = mel.create_material_expression(mat, unreal.MaterialExpressionDepthFade, -700, 400)
    names = mel.get_material_expression_input_names(fade)
    link(one, "", fade, next(n for n in names if "Opacity" in n))
    link(dist, "", fade, next(n for n in names if "Fade" in n))

    glow = custom("EmberFirebrandGlow", -500, 0, ["UV", "I", "H", "G", "DF"], (
        "float r = length(saturate(UV) * 2.0 - 1.0);\n"
        "float halo = saturate(1.0 - r);\n"
        "halo *= halo;\n"                                            # soft falloff to the card edge
        "float core = saturate(1.0 - r * 3.0);\n"                    # hot centre
        "float3 ember = float3(6.0, 0.9, 0.08);\n"
        "float3 spark = float3(9.0, 5.0, 1.6);\n"
        "float3 c = lerp(ember, spark, saturate(H));\n"
        "return G * I * DF * (c * halo + spark * core * core * 1.5);\n"))
    link(fade, "", glow, "DF")
    link(uv, "", glow, "UV")
    link(inten, "", glow, "I")
    link(heat, "", glow, "H")
    link(gain, "", glow, "G")
    if not mel.connect_material_property(glow, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR):
        raise RuntimeError("connect failed -> emissive")

    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_ADDITIVE)
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    mat.set_editor_property("two_sided", True)
    mat.set_editor_property("used_with_instanced_static_meshes", True)
    mel.recompile_material(mat)
    eal.save_asset(full, only_if_is_dirty=False)
    unreal.log(f"EMBER_GENERATED {full}")
    return mat


build_material()
