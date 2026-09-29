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
    FireTex         texture2D  fire state over the replay grid (EmberFireActor, linear):
                               R burning, G burned (incl. burning), B hours since arrival
                               (/255), A 1 inside the grid              default black
    FireRect        vector     fire grid in UE cm: (x0, y0, width, height)
    FireOn          scalar     0 = no fire bound                        default 0
    FireTime        scalar     the player's clock (s) for flame flicker default 0
    FireGain        scalar     flame emissive strength                  default 1
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


# ---- fire (HCP3): the replay's state texture, sampled by world position -------------------------
def linear_black():
    """/Game/Ember/Generated/T_LinearBlack: a 4x4 black texture with sRGB off - the default for
    the linear-colour FireTex samplers (the engine's Black is sRGB, and a Linear Color sampler
    with an sRGB default fails the material compile). Imported from a PNG written here."""
    import os
    import struct
    import tempfile
    import zlib

    full = f"{PATH}/T_LinearBlack"
    if eal.does_asset_exist(full):
        eal.delete_asset(full)
    w = h = 4
    raw = b"".join(b"\x00" + b"\x00\x00\x00\xff" * w for _ in range(h))

    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data
                + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))

    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
    tmp = os.path.join(tempfile.mkdtemp(), "T_LinearBlack.png")
    with open(tmp, "wb") as f:
        f.write(png)
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", tmp)
    task.set_editor_property("destination_path", PATH)
    task.set_editor_property("destination_name", "T_LinearBlack")
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", False)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    tex = unreal.load_asset(full)
    if tex is None:
        raise RuntimeError("T_LinearBlack import failed")
    tex.set_editor_property("srgb", False)
    tex.set_editor_property("compression_settings",
                            unreal.TextureCompressionSettings.TC_VECTOR_DISPLACEMENTMAP)
    eal.save_asset(full, only_if_is_dirty=False)
    unreal.log(f"EMBER_GENERATED {full}")
    return tex


def custom(name, x, y, inputs, code, out=unreal.CustomMaterialOutputType.CMOT_FLOAT3):
    c = expr(unreal.MaterialExpressionCustom, x, y)
    c.set_editor_property("output_type", out)
    c.set_editor_property("description", name)
    ins = []
    for n in inputs:
        ci = unreal.CustomInput()
        ci.set_editor_property("input_name", n)
        ins.append(ci)
    c.set_editor_property("inputs", ins)
    c.set_editor_property("code", code)
    return c


rect = expr(unreal.MaterialExpressionVectorParameter, -1400, 600)
rect.set_editor_property("parameter_name", "FireRect")
rect.set_editor_property("default_value", unreal.LinearColor(0.0, 0.0, 1.0, 1.0))
fire_on = scalar("FireOn", 0.0, -1400, 700)
fire_time = scalar("FireTime", 0.0, -1400, 790)
fire_gain = scalar("FireGain", 1.0, -1400, 880)
# UV over the fire grid, jittered by the detail noise (a few metres) so 30 m cells read organic.
fuv = custom("EmberFireUV", -1050, 600, ["WP", "R", "N"], (
    "float2 uv = (WP.xy - R.xy) / R.zw;\n"
    "uv += N * 1500.0 / R.zw;\n"          # ~15 m jitter: 30 m cells stop reading as squares
    "return uv;\n"), unreal.CustomMaterialOutputType.CMOT_FLOAT2)
link(wp, "", fuv, "WP")
link(rect, "RGBA", fuv, "R")
link(noise, "", fuv, "N")
ftex = expr(unreal.MaterialExpressionTextureSampleParameter2D, -850, 600)
ftex.set_editor_property("parameter_name", "FireTex")
ftex.set_editor_property("texture", linear_black())
ftex.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
link(fuv, "", ftex, "UVs")
# Ground: charcoal black -> dark ash (noise), settling over the days after the burn. Warm, not
# grey: a neutral grey reads slate-blue under the skylight at altitude (HCP3 Jolly).
fcol = custom("EmberFireGround", -450, 450, ["Base", "F", "N", "On"], (
    "float burned = smoothstep(0.35, 0.65, F.g + N * 0.18) * F.a * On;\n"
    "float age_h = F.b * F.b * 200.0;\n"
    "float ash = saturate(N * 0.9 + 0.35);\n"
    "float3 fresh = lerp(float3(0.010, 0.009, 0.008), float3(0.035, 0.032, 0.03), ash * ash);\n"
    "float3 old = lerp(float3(0.028, 0.022, 0.017), float3(0.075, 0.063, 0.052), ash * ash);\n"
    "float3 charc = lerp(fresh, old, saturate(age_h / 72.0));\n"
    "return lerp(Base, charc, burned);\n"))
link(base, "", fcol, "Base")
link(ftex, "RGBA", fcol, "F")
link(noise, "", fcol, "N")
link(fire_on, "", fcol, "On")
to_property(fcol, "", unreal.MaterialProperty.MP_BASE_COLOR)
# Emissive: flickering flames on burning cells; a fading ember glow for ~12 h after the front.
femi = custom("EmberFireGlow", -450, 700, ["F", "N", "On", "T", "G", "WP", "Cam"], (
    "float inside = F.a * On;\n"
    "float age_h = F.b * F.b * 200.0;\n"
    "float burning = smoothstep(0.3, 0.7, F.r + N * 0.2) * inside;\n"
    # hottest at the front (first ~hour after arrival), fading to smouldering over the burn. From
    # altitude the band widens (decay grows with camera distance) so the active edge stays a few
    # pixels wide, like the IR maps (design doc 5.2: legibility first); unchanged within 3 km.
    "float dkm = length(WP - Cam) / 100000.0;\n"
    "float front = exp(-age_h / (0.7 * max(1.0, dkm / 3.0)));\n"
    "float breakup = saturate(N * 1.0 + 0.75);\n"
    "float flick = 0.7 + 0.3 * sin(T * 7.0 + N * 23.0) * sin(T * 3.1 + N * 11.0);\n"
    "float3 flame = lerp(float3(3.0, 0.45, 0.04), float3(5.0, 1.8, 0.25), front) * flick;\n"
    "float patch = saturate(N * 2.0 - 0.2);\n"
    "float smoulder = burning * (1.0 - front) * 0.07 * patch;\n"
    "float embers = F.g * (1.0 - saturate(F.r * 4.0)) * inside\n"
    "             * saturate(1.0 - age_h / 12.0) * patch * 0.08;\n"
    "return G * (burning * front * breakup * flame\n"
    "            + (smoulder + embers) * float3(1.2, 0.18, 0.02));\n"))
link(ftex, "RGBA", femi, "F")
link(noise, "", femi, "N")
link(fire_on, "", femi, "On")
link(fire_time, "", femi, "T")
link(fire_gain, "", femi, "G")
link(wp, "", femi, "WP")
link(expr(unreal.MaterialExpressionCameraPositionWS, -650, 900), "", femi, "Cam")
to_property(femi, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

rough = scalar("Roughness", 0.92, -450, 150)
to_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)

mel.recompile_material(mat)
eal.save_asset(FULL, only_if_is_dirty=False)
unreal.log(f"EMBER_GENERATED {FULL}")
