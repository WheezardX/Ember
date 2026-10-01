"""Generator: /Game/Ember/Generated/M_Smoke + T_SmokePuff - smoke v0 puff cards (EPIC_5_PLAN
HCP3 smoke v0, D6 legibility-first).

T_SmokePuff: a 2x2 atlas of soft cloud puffs, R = density, G/B = the puff's surface normal
(x, y packed 0..1; from its rounded shape plus density bumps), so a flat card lights like a
billow. Written here in pure Python (deterministic, no source art).

M_Smoke: translucent, lit per pixel (Surface ForwardShading: sun shading at any distance, where
the translucency lighting volume only covers the first metres). Per-instance custom data from
AEmberSmokeActor:
    0  opacity       (puff life fade x plume density)
    1  glow          (fire light on the plume base, 0..1)
    2  variant       (atlas cell 0..3)
    3  depth fade    (cm; scales with the puff so big puffs do not slice into terrain)
Parameters:
    Color      vector  smoke albedo                 default sRGB ~#BDB9B3 (v1: lighter, per the Three Queens columns)
    GlowColor  vector  fire light on the smoke      default (4.0, 0.9, 0.15) linear
Runs headless via `ember-dev regen-assets`.
"""

import math
import os
import struct
import tempfile
import zlib

import unreal

PATH = "/Game/Ember/Generated"
eal = unreal.EditorAssetLibrary
mel = unreal.MaterialEditingLibrary
tools = unreal.AssetToolsHelpers.get_asset_tools()

CELL = 256          # atlas cell size (px); the atlas is 2 x 2 cells


# ------------------------------------------------------------------ puff texture
def _hash(ix, iy, seed):
    h = (ix * 374761393 + iy * 668265263 + seed * 2147483647) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    return ((h ^ (h >> 16)) & 0xFFFF) / 65535.0


def _value_noise(x, y, seed):
    ix, iy = math.floor(x), math.floor(y)
    fx, fy = x - ix, y - iy
    fx, fy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)
    a, b = _hash(ix, iy, seed), _hash(ix + 1, iy, seed)
    c, d = _hash(ix, iy + 1, seed), _hash(ix + 1, iy + 1, seed)
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy


def _fbm(x, y, seed):
    s, amp, tot = 0.0, 0.5, 0.0
    for o in range(4):
        s += amp * _value_noise(x, y, seed + o * 17)
        tot += amp
        x, y, amp = x * 2.03, y * 2.03, amp * 0.5
    return s / tot


def _puff(seed):
    """One CELL x CELL puff: density field (row-major floats)."""
    n = CELL
    dens = [0.0] * (n * n)
    for j in range(n):
        v = (j + 0.5) / n * 2.0 - 1.0
        for i in range(n):
            u = (i + 0.5) / n * 2.0 - 1.0
            r = math.sqrt(u * u + v * v)
            f = _fbm(u * 2.2 + 11.0, v * 2.2 + 7.0, seed)
            edge = r + 0.7 * (f - 0.5)                       # ragged, lobed silhouette
            # v1: a defined billow edge (reference: crisp cauliflower silhouettes), not the v0 haze
            base = max(0.0, min(1.0, (0.82 - edge) / 0.32))
            base = base * base * (3 - 2 * base) * base
            detail = _fbm(u * 6.0 + 3.0, v * 6.0 + 5.0, seed + 101)
            dens[j * n + i] = base * (0.6 + 0.4 * detail)
    return dens


def puff_texture():
    full = f"{PATH}/T_SmokePuff"
    if eal.does_asset_exist(full):
        eal.delete_asset(full)
    n, w = CELL, CELL * 2
    rows = [bytearray(1 + w * 4) for _ in range(w)]          # PNG rows: filter byte + RGBA
    for cell in range(4):
        dens = _puff(seed=cell * 31 + 5)
        ox, oy = (cell % 2) * n, (cell // 2) * n
        for j in range(n):
            v = (j + 0.5) / n * 2.0 - 1.0
            for i in range(n):
                u = (i + 0.5) / n * 2.0 - 1.0
                d = dens[j * n + i]
                # Normal: the rounded puff (outward in u, v) plus density bumps.
                gx = dens[j * n + min(i + 1, n - 1)] - dens[j * n + max(i - 1, 0)]
                gy = dens[min(j + 1, n - 1) * n + i] - dens[max(j - 1, 0) * n + i]
                nx, ny = 1.0 * u - 10.0 * gx, 1.0 * v - 10.0 * gy   # v1: rounder, deeper billows
                nz = 1.0
                ln = math.sqrt(nx * nx + ny * ny + nz * nz)
                nx, ny = nx / ln, ny / ln
                p = 1 + (ox + i) * 4
                row = rows[oy + j]
                row[p] = int(round(d * 255))
                row[p + 1] = int(round((nx * 0.5 + 0.5) * 255))
                row[p + 2] = int(round((ny * 0.5 + 0.5) * 255))
                row[p + 3] = 255
    raw = b"".join(bytes(r) for r in rows)

    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data
                + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))

    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, w, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    tmp = os.path.join(tempfile.mkdtemp(), "T_SmokePuff.png")
    with open(tmp, "wb") as f:
        f.write(png)
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", tmp)
    task.set_editor_property("destination_path", PATH)
    task.set_editor_property("destination_name", "T_SmokePuff")
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", False)
    tools.import_asset_tasks([task])
    tex = unreal.load_asset(full)
    if tex is None:
        raise RuntimeError("T_SmokePuff import failed")
    tex.set_editor_property("srgb", False)
    # Uncompressed BGRA8: block compression bands the soft density ramps.
    tex.set_editor_property("compression_settings",
                            unreal.TextureCompressionSettings.TC_VECTOR_DISPLACEMENTMAP)
    tex.set_editor_property("address_x", unreal.TextureAddress.TA_CLAMP)
    tex.set_editor_property("address_y", unreal.TextureAddress.TA_CLAMP)
    eal.save_asset(full, only_if_is_dirty=False)
    unreal.log(f"EMBER_GENERATED {full}")
    return tex


# ------------------------------------------------------------------ material
def build_material(tex):
    full = f"{PATH}/M_Smoke"
    if eal.does_asset_exist(full):
        eal.delete_asset(full)
    mat = tools.create_asset("M_Smoke", PATH, unreal.Material, unreal.MaterialFactoryNew())

    def link(src, out, dst, inp):
        if not mel.connect_material_expressions(src, out, dst, inp):
            raise RuntimeError(f"connect failed: {out!r} -> {inp!r}")

    def to_property(src, out, prop):
        if not mel.connect_material_property(src, out, prop):
            raise RuntimeError(f"connect failed -> {prop}")

    def custom(name, x, y, inputs, code, out=unreal.CustomMaterialOutputType.CMOT_FLOAT3):
        c = mel.create_material_expression(mat, unreal.MaterialExpressionCustom, x, y)
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

    def cdata(i, y):
        e = mel.create_material_expression(mat, unreal.MaterialExpressionPerInstanceCustomData,
                                           -1200, y)
        e.set_editor_property("data_index", i)
        return e

    opac, glow, variant, fade_cm = cdata(0, 0), cdata(1, 100), cdata(2, 200), cdata(3, 450)
    uv = mel.create_material_expression(mat, unreal.MaterialExpressionTextureCoordinate,
                                        -1200, -150)
    auv = custom("EmberPuffUV", -950, -100, ["UV", "V"], (
        "float v = floor(V + 0.5);\n"
        "float2 cell = float2(fmod(v, 2.0), floor(v / 2.0));\n"
        "return (saturate(UV) * 0.996 + 0.002 + cell) * 0.5;\n"),   # stay inside the cell
        unreal.CustomMaterialOutputType.CMOT_FLOAT2)
    link(uv, "", auv, "UV")
    link(variant, "", auv, "V")
    ptex = mel.create_material_expression(mat, unreal.MaterialExpressionTextureSampleParameter2D,
                                          -700, -100)
    ptex.set_editor_property("parameter_name", "PuffTex")
    ptex.set_editor_property("texture", tex)
    ptex.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
    link(auv, "", ptex, "UVs")

    col = mel.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -700, -350)
    col.set_editor_property("parameter_name", "Color")
    col.set_editor_property("default_value", unreal.LinearColor(0.52, 0.5, 0.46, 1.0))  # v1: the reference columns are cream-white in sun
    gcol = mel.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -700, 300)
    gcol.set_editor_property("parameter_name", "GlowColor")
    gcol.set_editor_property("default_value", unreal.LinearColor(4.0, 0.9, 0.15, 1.0))

    base = custom("EmberSmokeColor", -400, -300, ["P", "C"], (
        "return C.rgb * (0.7 + 0.3 * P.r);\n"))                  # dense cores a touch lighter
    link(ptex, "RGBA", base, "P")
    link(col, "RGBA", base, "C")
    to_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)

    nrm = custom("EmberSmokeNormal", -400, -150, ["P"], (
        "float2 n = P.gb * 2.0 - 1.0;\n"
        "return normalize(float3(n.x * 0.6, n.y * 0.6, sqrt(saturate(1.0 - dot(n, n))) + 0.6));\n"))
    link(ptex, "RGBA", nrm, "P")
    to_property(nrm, "", unreal.MaterialProperty.MP_NORMAL)

    dens = custom("EmberSmokeOpacity", -400, 50, ["P", "O"], (
        "return saturate(P.r * O);\n"), unreal.CustomMaterialOutputType.CMOT_FLOAT1)
    link(ptex, "RGBA", dens, "P")
    link(opac, "", dens, "O")
    fade = mel.create_material_expression(mat, unreal.MaterialExpressionDepthFade, -150, 100)
    names = mel.get_material_expression_input_names(fade)
    link(dens, "", fade, next(n for n in names if "Opacity" in n))
    link(fade_cm, "", fade, next(n for n in names if "Fade" in n))
    to_property(fade, "", unreal.MaterialProperty.MP_OPACITY)

    # Smoke v1: multiple scattering. Per-pixel surface lighting left the shaded side of every
    # puff near black; real smoke is lit through its volume by the sky (the reference columns
    # are bright grey even in shade). Add the skylight's diffuse colour x albedo as emissive.
    sky = mel.create_material_expression(mat, unreal.MaterialExpressionSkyLightEnvMapSample, -700, 450)
    up = mel.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector, -900, 430)
    up.set_editor_property("constant", unreal.LinearColor(0.0, 0.0, 1.0, 0.0))
    rough = mel.create_material_expression(mat, unreal.MaterialExpressionConstant, -900, 500)
    rough.set_editor_property("r", 1.0)
    sky_in = mel.get_material_expression_input_names(sky)
    link(up, "", sky, next(n for n in sky_in if "Direction" in n))
    link(rough, "", sky, next(n for n in sky_in if "Roughness" in n))
    emi = custom("EmberSmokeGlow", -400, 250, ["P", "G", "GC", "C", "S"], (
        "return GC.rgb * G * P.r * P.r + C.rgb * S * 0.9;\n"))
    link(ptex, "RGBA", emi, "P")
    link(glow, "", emi, "G")
    link(gcol, "RGBA", emi, "GC")
    link(col, "RGBA", emi, "C")
    link(sky, "", emi, "S")
    to_property(emi, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    for name, value, y in (("Roughness", 1.0, 600), ("Specular", 0.0, 700)):
        k = mel.create_material_expression(mat, unreal.MaterialExpressionConstant, -150, y)
        k.set_editor_property("r", value)
        to_property(k, "", getattr(unreal.MaterialProperty, f"MP_{name.upper()}"))

    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    mat.set_editor_property("translucency_lighting_mode",
                            unreal.TranslucencyLightingMode.TLM_SURFACE_PER_PIXEL_LIGHTING)
    mat.set_editor_property("used_with_instanced_static_meshes", True)
    mel.recompile_material(mat)
    eal.save_asset(full, only_if_is_dirty=False)
    unreal.log(f"EMBER_GENERATED {full}")
    return mat


build_material(puff_texture())
