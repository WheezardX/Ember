"""Generator: vegetation species meshes v2 + their materials (EPIC_5_PLAN B3 v2, HCP2 round 2).

    /Game/Ember/Generated/M_Veg                       master: lerp(TrunkColor, Color x per-tree
                                                      tint, vertex alpha) x vertex RGB; two-sided;
                                                      wind WPO (sway + per-tree lean + twig flutter)
    /Game/Ember/Generated/Veg/MI_Veg_<key>            per species: Color / TrunkColor (treegen.COLORS)
    /Game/Ember/Generated/Veg/SM_<key>_v<N>           per species x treegen.VARIANTS: Nanite mesh
    /Game/Ember/Generated/Veg/SM_<key>_v<N>_lite      trees only: the same tree with ~45 % of the
                                                      foliage, for the mid vegetation tier

Geometry comes from treegen.py (pure Python growth-form models: trunk, branch whorls, droop,
needle sprays / leaf clusters as opaque geometry) and is loaded with append_buffers_to_mesh.
Nanite `Preserve Area` keeps sparse foliage from thinning at distance. Bark is a flat colour
until licensed bark scans arrive. Runs headless via `ember-dev regen-assets`.
"""

import os
import sys

import unreal

_here = os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else os.path.join(
    unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir()), "..", "..", "assets",
    "generators")
sys.path.insert(0, _here)
import treegen  # noqa: E402

PATH = "/Game/Ember/Generated"
VEG = f"{PATH}/Veg"
eal = unreal.EditorAssetLibrary
mel = unreal.MaterialEditingLibrary
edits = unreal.GeometryScript_MeshEdits
colors = unreal.GeometryScript_VertexColors
normals = unreal.GeometryScript_Normals
newasset = unreal.GeometryScript_NewAssetUtils
tools = unreal.AssetToolsHelpers.get_asset_tools()


def fresh(path):
    if eal.does_asset_exist(path):
        eal.delete_asset(path)


def srgb(hexcol):
    c = [int(hexcol[i:i + 2], 16) / 255.0 for i in (0, 2, 4)]
    lin = [x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4 for x in c]
    return unreal.LinearColor(lin[0], lin[1], lin[2], 1.0)


# ------------------------------------------------------------------ master material
def build_material():
    full = f"{PATH}/M_Veg"
    fresh(full)
    mat = tools.create_asset("M_Veg", PATH, unreal.Material, unreal.MaterialFactoryNew())

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

    col = mel.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -900, -200)
    col.set_editor_property("parameter_name", "Color")
    col.set_editor_property("default_value", unreal.LinearColor(0.03, 0.06, 0.025, 1.0))
    trunk_col = mel.create_material_expression(
        mat, unreal.MaterialExpressionVectorParameter, -900, -400)
    trunk_col.set_editor_property("parameter_name", "TrunkColor")
    trunk_col.set_editor_property("default_value", unreal.LinearColor(0.06, 0.035, 0.02, 1.0))
    vc = mel.create_material_expression(mat, unreal.MaterialExpressionVertexColor, -900, 100)
    rnd = mel.create_material_expression(mat, unreal.MaterialExpressionPerInstanceRandom, -900, 0)

    # Per-tree foliage tint: brightness and a little yellow-green shift, so no two neighbours
    # match (PerInstanceRandom: constant over an instance, deterministic per component seed).
    tint = custom("EmberTint", -650, -100, ["Rnd"], (
        "float v = 0.84 + 0.32 * Rnd;\n"
        "float y = frac(Rnd * 7.31) - 0.5;\n"
        "return float3(v * (1.0 + 0.10 * y), v, v * (1.0 - 0.12 * y));\n"))
    link(rnd, "", tint, "Rnd")
    fol = mel.create_material_expression(mat, unreal.MaterialExpressionMultiply, -450, -150)
    link(col, "", fol, "A")
    link(tint, "", fol, "B")
    lerp = mel.create_material_expression(mat, unreal.MaterialExpressionLinearInterpolate,
                                          -300, -250)
    link(trunk_col, "", lerp, "A")
    link(fol, "", lerp, "B")
    link(vc, "A", lerp, "Alpha")          # vertex alpha: 0 = bark, 1 = foliage
    mul = mel.create_material_expression(mat, unreal.MaterialExpressionMultiply, -150, -100)
    link(lerp, "", mul, "A")
    link(vc, "", mul, "B")                # vertex RGB: shading / inner-crown darkening
    if not mel.connect_material_property(mul, "", unreal.MaterialProperty.MP_BASE_COLOR):
        raise RuntimeError("base colour")
    # Needles and leaves transmit light: Two-Sided Foliage shading with a subsurface colour of
    # the foliage tint (x alpha, so bark transmits nothing). Without it crowns render near-black.
    sss_gain = mel.create_material_expression(mat, unreal.MaterialExpressionConstant, -300, 50)
    sss_gain.set_editor_property("r", 0.6)
    sss = mel.create_material_expression(mat, unreal.MaterialExpressionMultiply, -150, 50)
    link(fol, "", sss, "A")
    link(sss_gain, "", sss, "B")
    sss_a = mel.create_material_expression(mat, unreal.MaterialExpressionMultiply, -50, 50)
    link(sss, "", sss_a, "A")
    link(vc, "A", sss_a, "B")
    if not mel.connect_material_property(sss_a, "", unreal.MaterialProperty.MP_SUBSURFACE_COLOR):
        raise RuntimeError("subsurface colour")
    rough = mel.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -300, 200)
    rough.set_editor_property("parameter_name", "Roughness")
    rough.set_editor_property("default_value", 0.85)
    if not mel.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS):
        raise RuntimeError("roughness")

    # Wind (world position offset). The runtime owns the clock (WindTime: frozen for stills,
    # frame/fps for orbits), so renders stay deterministic. Offsets are relative to the INSTANCE
    # pivot (LocalPosition(instance) -> world): bend grows with height^2; per-tree phase and a
    # static per-tree lean from PerInstanceRandom; a slow gust band travels downwind; foliage
    # (vertex alpha) adds a fast small flutter, the motion that makes wind readable.
    lpos = mel.create_material_expression(mat, unreal.MaterialExpressionLocalPosition, -1300, 450)
    lpos.set_editor_property("local_origin", unreal.LocalPositionOrigin.INSTANCE)
    lpos.set_editor_property("included_offsets", unreal.PositionIncludedOffsets.EXCLUDE_OFFSETS)
    local = mel.create_material_expression(mat, unreal.MaterialExpressionTransform, -1100, 450)
    local.set_editor_property("transform_source_type",
                              unreal.MaterialVectorCoordTransformSource.TRANSFORMSOURCE_INSTANCE)
    local.set_editor_property("transform_type", unreal.MaterialVectorCoordTransform.TRANSFORM_WORLD)
    link(lpos, "", local, "")
    wpos = mel.create_material_expression(mat, unreal.MaterialExpressionWorldPosition, -1100, 300)
    wpos.set_editor_property("world_position_shader_offset",
                             unreal.WorldPositionIncludedOffsets.WPT_EXCLUDE_ALL_SHADER_OFFSETS)
    opos = mel.create_material_expression(mat, unreal.MaterialExpressionSubtract, -900, 350)
    link(wpos, "", opos, "A")             # pivot = world position - (pivot -> vertex)
    link(local, "", opos, "B")
    params = {}
    for i, (name, default) in enumerate((("WindTime", 0.0), ("WindStrength", 6.0))):
        p = mel.create_material_expression(mat, unreal.MaterialExpressionScalarParameter,
                                           -1100, 640 + 90 * i)
        p.set_editor_property("parameter_name", name)
        p.set_editor_property("default_value", default)
        params[name] = p
    wdir = mel.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -1100, 820)
    wdir.set_editor_property("parameter_name", "WindDir")
    wdir.set_editor_property("default_value", unreal.LinearColor(1.0, 0.0, 0.0, 0.0))
    wind = custom("EmberWind", -600, 500, ["Local", "Pivot", "Rnd", "A", "T", "S", "D"], (
        "float h = max(Local.z, 0.0) / 1000.0;\n"                   # height above pivot, 10 m
        "float bend = S * h * h;\n"
        "float2 p = Pivot.xy * 0.01;\n"                             # metres
        "float ph = Rnd * 6.2831853;\n"
        "float along = dot(p, D.xy);\n"
        "float gust = 0.5 + 0.5 * sin(T * 0.35 - along * 0.012);\n"
        "float sway = 0.55 + 0.45 * sin(T * 1.6 + ph) * (0.6 + 0.4 * gust);\n"
        "float3 o = float3(D.xy * bend * sway * (0.7 + 0.6 * gust), -0.15 * bend * sway);\n"
        "float la = frac(Rnd * 13.7) * 6.2831853;\n"               # static lean, 0..12 cm/(10 m)^2
        "o.xy += float2(cos(la), sin(la)) * 12.0 * frac(Rnd * 3.3) * h * h;\n"
        "float fp = dot(Local, float3(0.021, 0.017, 0.013)) + ph;\n"
        "float fl = A * (S / 6.0) * (0.4 + 0.6 * gust) * (1.5 + 2.5 * h);\n"  # twig flutter, cm
        "o += fl * float3(sin(T * 7.3 + fp) * D.x, sin(T * 6.1 + fp * 1.3) * D.y + "
        "0.4 * sin(T * 8.7 + fp), 0.6 * sin(T * 9.1 + fp * 0.7));\n"
        "return o;\n"))
    link(local, "", wind, "Local")
    link(opos, "", wind, "Pivot")
    link(rnd, "", wind, "Rnd")
    link(vc, "A", wind, "A")
    link(params["WindTime"], "", wind, "T")
    link(params["WindStrength"], "", wind, "S")
    link(wdir, "", wind, "D")
    if not mel.connect_material_property(wind, "", unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET):
        raise RuntimeError("world position offset")
    mat.set_editor_property("two_sided", True)                  # foliage is single-layer sprays
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_TWO_SIDED_FOLIAGE)
    mat.set_editor_property("max_world_position_offset_displacement", 700.0)  # Nanite WPO bounds
    mat.set_editor_property("used_with_instanced_static_meshes", True)
    mat.set_editor_property("used_with_nanite", True)
    mel.recompile_material(mat)
    eal.save_asset(full, only_if_is_dirty=False)
    unreal.log(f"EMBER_GENERATED {full}")
    return mat


def build_instance(key, master):
    name = f"MI_Veg_{key}"
    full = f"{VEG}/{name}"
    fresh(full)
    mi = tools.create_asset(name, VEG, unreal.MaterialInstanceConstant,
                            unreal.MaterialInstanceConstantFactoryNew())
    mel.set_material_instance_parent(mi, master)  # (set_editor_property left no parameters)
    foliage, bark = treegen.COLORS[key]
    for param, hexcol in (("Color", foliage), ("TrunkColor", bark)):
        want = srgb(hexcol)
        mel.set_material_instance_vector_parameter_value(mi, param, want)
        got = mel.get_material_instance_vector_parameter_value(mi, param)
        if abs(got.r - want.r) > 1e-4 or abs(got.g - want.g) > 1e-4:  # verify by reading back
            raise RuntimeError(f"{name}: {param} not set (got {got})")
    mel.update_material_instance(mi)
    eal.save_asset(full, only_if_is_dirty=False)
    unreal.log(f"EMBER_GENERATED {full}")
    return mi


# ------------------------------------------------------------------ meshes
def build_mesh(key, variant, mi, lite=False):
    name = f"SM_{key}_v{variant}" + ("_lite" if lite else "")
    full = f"{VEG}/{name}"
    fresh(full)
    tm = treegen.build(key, variant, treegen.LITE_DETAIL if lite else 1.0)
    buf = unreal.GeometryScriptSimpleMeshBuffers()
    buf.set_editor_property("vertices", [unreal.Vector(*p) for p in tm.verts])
    buf.set_editor_property("triangles", [unreal.IntVector(*t) for t in tm.tris])
    buf.set_editor_property("vertex_colors", [unreal.LinearColor(*c) for c in tm.cols])
    # The static mesh builder requires a UV channel (asserts NumUVs > 0). Cylindrical-ish
    # planar UVs in metres - enough for a tiling bark texture later.
    buf.set_editor_property("uv0", [unreal.Vector2D((v[0] + v[1]) / 100.0, v[2] / 100.0)
                                    for v in tm.verts])
    dm = unreal.DynamicMesh()
    edits.append_buffers_to_mesh(dm, buf)
    if dm.get_triangle_count() != len(tm.tris):
        raise RuntimeError(f"{name}: {dm.get_triangle_count()} triangles, want {len(tm.tris)}")
    normals.recompute_normals(dm, unreal.GeometryScriptCalculateNormalsOptions())
    o = unreal.GeometryScriptCreateNewStaticMeshAssetOptions()
    o.set_editor_property("enable_nanite", True)
    o.set_editor_property("enable_collision", False)
    o.set_editor_property("enable_recompute_normals", False)
    sm, outcome = newasset.create_new_static_mesh_asset_from_mesh(dm, full, o)
    if sm is None or outcome != unreal.GeometryScriptOutcomePins.SUCCESS:
        raise RuntimeError(f"create static mesh {full}: {outcome}")
    sm.set_material(0, mi)
    # The creation option alone left nanite_settings.enabled False through HCP1; set it
    # explicitly. Preserve Area: sparse foliage keeps its coverage as Nanite simplifies.
    ns = sm.get_editor_property("nanite_settings")
    ns.set_editor_property("enabled", True)
    ns.set_editor_property("shape_preservation", unreal.NaniteShapePreservation.PRESERVE_AREA)
    sm.set_editor_property("nanite_settings", ns)
    # No mesh distance field / Lumen cards: instances never join the DF scene
    # (bAffectDistanceFieldLighting = false, VRAM), and building them at runtime in the uncooked
    # -game process stalled the render thread every ~40 frames (UpdatePrimitive fence waits).
    lib = unreal.EditorStaticMeshLibrary
    bs = lib.get_lod_build_settings(sm, 0)
    bs.set_editor_property("distance_field_resolution_scale", 0.0)
    bs.set_editor_property("max_lumen_mesh_cards", 0)
    lib.set_lod_build_settings(sm, 0, bs)
    eal.save_asset(full, only_if_is_dirty=False)
    unreal.log(f"EMBER_GENERATED {full}")
    unreal.log(f"EMBER_VEG {name}: {len(tm.tris)} triangles")


# v1 assets (one mesh per species, five keys) are replaced by per-variant meshes.
for old in ("pseudotsuga_menziesii", "pinus_ponderosa", "abies_grandis", "artemisia_shrub",
            "bunchgrass"):
    fresh(f"{VEG}/SM_{old}")

master = build_material()
for k in treegen.SPECIES:
    mi = build_instance(k, master)
    for v in range(treegen.VARIANTS):
        build_mesh(k, v, mi)
        if treegen.has_lite(k):  # mid tier (D11 section 2): same tree, less foliage
            build_mesh(k, v, mi, lite=True)

# Verify what the renderer depends on survived into the built assets.
for k in treegen.SPECIES:
    sm = unreal.load_asset(f"{VEG}/SM_{k}_v0")
    ns = sm.get_editor_property("nanite_settings")
    if not ns.get_editor_property("enabled"):
        raise RuntimeError(f"SM_{k}_v0: Nanite is not enabled")
    dm = unreal.DynamicMesh()
    unreal.GeometryScript_AssetUtils.copy_mesh_from_static_mesh(
        sm, dm, unreal.GeometryScriptCopyMeshFromAssetOptions(), unreal.GeometryScriptMeshReadLOD())
    res = colors.get_mesh_per_vertex_colors(dm)
    color_list = next(r for r in res if type(r).__name__ == "GeometryScriptColorList")
    arr = unreal.GeometryScript_List.convert_color_list_to_array(color_list)
    alphas = sorted({round(c.a, 2) for c in arr})
    if not alphas or alphas[-1] < 0.99:
        raise RuntimeError(f"SM_{k}_v0: foliage vertex alpha missing ({alphas[:6]})")
    unreal.log(f"EMBER_VEG {k}: {len(arr)} vertex colours, alpha values {alphas[:6]}")
