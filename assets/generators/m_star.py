"""Generator: /Game/Ember/Generated/M_Star - night-sky star sprites (AEmberStarsActor).

Additive, unlit, two-sided; one soft round dot per plane (a gaussian on the UVs), coloured and
scaled by per-instance custom data (0-2: linear RGB, the star's tint x displayed brightness).
Not fogged: the sprites sit on a camera-centred sphere far beyond the terrain, where the height
fog would hide them completely - the actor dims stars near the horizon itself (airmass).
Parameters:
    Gain   scalar  overall star brightness       default 40
Runs headless via `ember-dev regen-assets`.
"""

import unreal

PATH = "/Game/Ember/Generated"
eal = unreal.EditorAssetLibrary
mel = unreal.MaterialEditingLibrary
tools = unreal.AssetToolsHelpers.get_asset_tools()


def build_material():
    full = f"{PATH}/M_Star"
    if eal.does_asset_exist(full):
        eal.delete_asset(full)
    mat = tools.create_asset("M_Star", PATH, unreal.Material, unreal.MaterialFactoryNew())

    def link(src, out, dst, inp):
        if not mel.connect_material_expressions(src, out, dst, inp):
            raise RuntimeError(f"connect failed: {out!r} -> {inp!r}")

    def cdata(i, y):
        e = mel.create_material_expression(mat, unreal.MaterialExpressionPerInstanceCustomData, -900, y)
        e.set_editor_property("data_index", i)
        return e

    r, g, b = cdata(0, 0), cdata(1, 100), cdata(2, 200)
    gain = mel.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -900, 300)
    gain.set_editor_property("parameter_name", "Gain")
    gain.set_editor_property("default_value", 40.0)
    uv = mel.create_material_expression(mat, unreal.MaterialExpressionTextureCoordinate, -900, -200)

    c = mel.create_material_expression(mat, unreal.MaterialExpressionCustom, -500, 0)
    c.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    c.set_editor_property("description", "EmberStar")
    ins = []
    for n in ("UV", "R", "G", "B", "Gain"):
        ci = unreal.CustomInput()
        ci.set_editor_property("input_name", n)
        ins.append(ci)
    c.set_editor_property("inputs", ins)
    c.set_editor_property("code", (
        "float2 d = UV - 0.5;\n"
        "float r2 = dot(d, d) * 4.0;\n"              # 0 centre .. 1 at the plane's inscribed circle
        "float a = exp(-r2 * 9.0) - exp(-9.0);\n"    # a soft dot that reaches 0 at the edge
        "return max(a, 0.0) * Gain * float3(R, G, B);\n"))
    for src, inp in ((uv, "UV"), (r, "R"), (g, "G"), (b, "B"), (gain, "Gain")):
        link(src, "", c, inp)
    if not mel.connect_material_property(c, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR):
        raise RuntimeError("connect failed -> emissive")

    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_ADDITIVE)
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    mat.set_editor_property("two_sided", True)
    mat.set_editor_property("used_with_instanced_static_meshes", True)
    # no height fog / atmosphere on the sprites (the property name moved between UE versions)
    fog_off = []
    for prop in ("apply_fogging", "use_translucency_vertex_fog", "apply_cloud_fogging"):
        try:
            mat.set_editor_property(prop, False)
            fog_off.append(prop)
        except Exception:
            pass
    unreal.log(f"M_Star fog off: {fog_off}")
    mel.recompile_material(mat)
    eal.save_asset(full, only_if_is_dirty=False)
    unreal.log(f"EMBER_GENERATED {full}")
    return mat


build_material()
