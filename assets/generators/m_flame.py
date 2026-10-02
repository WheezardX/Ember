"""Generator: /Game/Ember/Generated/M_Flame - eye-level flame cards (EPIC_5_PLAN HCP4 H4-5: flames
with shape instead of paint).

Additive, unlit, vertical camera-facing cards (AEmberFlameActor places them on burning cells near
the camera). The flame is procedural: a tongue that narrows upward, its edge eaten by value noise
scrolling up with FireTime (frozen in stills, so captures stay deterministic), hot yellow-white at
the base core, orange, then red at the ragged tips. Shape comes from world position relative to
the card centre, so the card's UV layout does not matter. Per-instance custom data:
    0  intensity    (class x freshness)
    1  seed         (0..1: per-card noise offset)
    2  half height  (cm)
    3  half width   (cm)
    4-6 card centre (UE cm, world)
Parameters:
    FireTime   scalar  the fire clock (s)            default 0
    Gain       scalar  overall brightness            default 1
Runs headless via `ember-dev regen-assets`.
"""

import unreal

PATH = "/Game/Ember/Generated"
eal = unreal.EditorAssetLibrary
mel = unreal.MaterialEditingLibrary
tools = unreal.AssetToolsHelpers.get_asset_tools()


def build_material():
    full = f"{PATH}/M_Flame"
    if eal.does_asset_exist(full):
        eal.delete_asset(full)
    mat = tools.create_asset("M_Flame", PATH, unreal.Material, unreal.MaterialFactoryNew())

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
                                           -1100, y)
        e.set_editor_property("data_index", i)
        return e

    def scalar(name, default, y):
        e = mel.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -1100, y)
        e.set_editor_property("parameter_name", name)
        e.set_editor_property("default_value", default)
        return e

    inten, seed, hh, hw = cdata(0, 0), cdata(1, 100), cdata(2, 200), cdata(3, 300)
    t = scalar("FireTime", 0.0, 400)
    gain = scalar("Gain", 1.0, 500)
    wp = mel.create_material_expression(mat, unreal.MaterialExpressionWorldPosition, -1100, -200)
    # the card centre comes in the instance data: ObjectPositionWS is not per instance here
    px, py, pz = cdata(4, -500), cdata(5, -400), cdata(6, -300)

    one = mel.create_material_expression(mat, unreal.MaterialExpressionConstant, -1100, 650)
    one.set_editor_property("r", 1.0)
    dist = mel.create_material_expression(mat, unreal.MaterialExpressionConstant, -1100, 750)
    dist.set_editor_property("r", 40.0)   # cm: the flame base melts into the ground
    fade = mel.create_material_expression(mat, unreal.MaterialExpressionDepthFade, -850, 700)
    names = mel.get_material_expression_input_names(fade)
    link(one, "", fade, next(n for n in names if "Opacity" in n))
    link(dist, "", fade, next(n for n in names if "Fade" in n))

    cam = mel.create_material_expression(mat, unreal.MaterialExpressionCameraPositionWS, -1100, -600)
    flame = custom("EmberFlame", -500, 0, ["WP", "PX", "PY", "PZ", "Cam", "I", "S", "HH", "HW", "T", "G", "DF"], (
        "float3 P = float3(PX, PY, PZ);\n"
        # card-local coordinates: y 0 base .. 1 tip, x 0 centre .. 1 edge (cards are vertical)
        "float y = saturate((WP.z - P.z) / (2.0 * HH) + 0.5);\n"
        "float x = length(WP.xy - P.xy) / HW;\n"
        # Tuned offline (scratch flame_proto.py mirrors this maths in numpy: seconds per look
        # instead of a UE round trip).
        # value noise, scrolling up (the fire clock), three octaves
        "float n = 0.0, amp = 0.6;\n"
        "float2 q = float2(x * 4.0 + S * 17.0, y * 5.0 - T * 2.6 + S * 5.0);\n"
        "for (int o = 0; o < 3; ++o)\n"
        "{\n"
        "    float2 i = floor(q), f = frac(q);\n"
        "    f = f * f * (3.0 - 2.0 * f);\n"
        "    float4 h = frac(sin(float4(dot(i, float2(12.9898, 78.233)), dot(i + float2(1, 0), float2(12.9898, 78.233)),\n"
        "                                dot(i + float2(0, 1), float2(12.9898, 78.233)), dot(i + 1.0, float2(12.9898, 78.233)))) * 43758.5453);\n"
        "    n += amp * lerp(lerp(h.x, h.y, f.x), lerp(h.z, h.w, f.x), f.y);\n"
        "    q = q * 2.3 + float2(3.1, -T * 1.3);\n"
        "    amp *= 0.5;\n"
        "}\n"
        "n /= 1.05;\n"
        # signed across-coordinate: the noise sways the tongue more the higher it gets
        "float2 fwd = normalize(P.xy - Cam.xy + 1e-3);\n"
        "float xs0 = dot(WP.xy - P.xy, float2(-fwd.y, fwd.x)) / HW;\n"
        "float xs = xs0 + (n - 0.5) * 1.8 * y;\n"
        # teardrop: full width low down, tapering hard to a point; the noise eats the edge
        "float w = 0.8 * pow(saturate(1.0 - y), 1.3) * pow(saturate(y * 6.0), 0.35) * (0.4 + 1.2 * n);\n"
        # (flames v2: crisper edges - many small licks with soft edges read as pale ghosts; the
        # reference tongues are sharp-edged with dark gaps between them)
        "float body = saturate((w - abs(xs)) * 10.0);\n"
        "body *= saturate((1.5 * n + 0.05 - y) * 8.0);\n"            # ragged top: tongues tear off
        "body *= saturate(y * 25.0);\n"                               # base melts into the ground
        # internal texture: a finer noise scrolling faster, so the fill is not flat
        "float2 q2 = float2(xs0 * 6.0 + S * 3.0, y * 9.0 - T * 5.0 + S * 11.0);\n"
        "float2 i2 = floor(q2), f2 = frac(q2);\n"
        "f2 = f2 * f2 * (3.0 - 2.0 * f2);\n"
        "float4 h2 = frac(sin(float4(dot(i2, float2(12.9898, 78.233)), dot(i2 + float2(1, 0), float2(12.9898, 78.233)),\n"
        "                             dot(i2 + float2(0, 1), float2(12.9898, 78.233)), dot(i2 + 1.0, float2(12.9898, 78.233)))) * 43758.5453);\n"
        "body *= 0.55 + 0.45 * lerp(lerp(h2.x, h2.y, f2.x), lerp(h2.z, h2.w, f2.x), f2.y);\n"
        "float heat = body * (1.0 - 0.9 * y);\n"
        "float3 red = float3(1.6, 0.15, 0.01);\n"
        "float3 orange = float3(5.0, 1.1, 0.08);\n"
        "float3 core = float3(6.0, 2.6, 0.35);\n"
        "float3 c = lerp(red, orange, saturate(heat * 1.6));\n"
        "c = lerp(c, core, saturate(heat * 2.0 - 1.0));\n"
        "return G * I * DF * body * body * c;\n"))
    for src, inp in ((wp, "WP"), (px, "PX"), (py, "PY"), (pz, "PZ"), (cam, "Cam"), (inten, "I"), (seed, "S"), (hh, "HH"), (hw, "HW"),
                     (t, "T"), (gain, "G"), (fade, "DF")):
        link(src, "", flame, inp)
    if not mel.connect_material_property(flame, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR):
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
