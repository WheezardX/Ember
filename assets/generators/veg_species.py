"""Generator: vegetation species meshes v1 + their master material (EPIC_5_PLAN B3, B2 item 2).

    /Game/Ember/Generated/M_Veg                    lerp(TrunkColor, Color, vertex alpha) x vertex
                                                   colour RGB (shading/AO); Roughness (scalar);
                                                   wind WPO (WindTime, WindStrength, WindDir)
    /Game/Ember/Generated/Veg/SM_<palette key>     one Nanite static mesh per species in Terrain's
                                                   pnw_conifer palette (keys must match exactly)

Meshes are built from GeometryScript primitives (stacked tier cones for firs, a clear bole and
clumped ellipsoid crown for ponderosa, blob clusters for shrub, blade cones for bunchgrass),
deterministically (fixed seeds), with vertex colours that darken the inner/lower crown. The
runtime fits each mesh's bounds to the instance's height and crown radius, so only proportions
matter here. Runs headless via `ember-dev regen-assets`.
"""

import math
import random

import unreal

PATH = "/Game/Ember/Generated"
VEG = f"{PATH}/Veg"
eal = unreal.EditorAssetLibrary
mel = unreal.MaterialEditingLibrary
prim = unreal.GeometryScript_Primitives
edits = unreal.GeometryScript_MeshEdits
colors = unreal.GeometryScript_VertexColors
normals = unreal.GeometryScript_Normals
newasset = unreal.GeometryScript_NewAssetUtils

H = 1000.0  # nominal tree height, cm (proportions only)


def fresh(path):
    if eal.does_asset_exist(path):
        eal.delete_asset(path)


# ------------------------------------------------------------------ material
def build_material():
    full = f"{PATH}/M_Veg"
    fresh(full)
    mat = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        "M_Veg", PATH, unreal.Material, unreal.MaterialFactoryNew())

    def link(src, out, dst, inp):
        if not mel.connect_material_expressions(src, out, dst, inp):
            raise RuntimeError(f"connect failed: {out!r} -> {inp!r}")

    col = mel.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -600, -100)
    col.set_editor_property("parameter_name", "Color")
    col.set_editor_property("default_value", unreal.LinearColor(0.05, 0.08, 0.04, 1.0))
    trunk_col = mel.create_material_expression(
        mat, unreal.MaterialExpressionVectorParameter, -600, -300)
    trunk_col.set_editor_property("parameter_name", "TrunkColor")
    trunk_col.set_editor_property("default_value",
                                  unreal.LinearColor(0.06, 0.035, 0.02, 1.0))
    vc = mel.create_material_expression(mat, unreal.MaterialExpressionVertexColor, -600, 100)
    lerp = mel.create_material_expression(
        mat, unreal.MaterialExpressionLinearInterpolate, -400, -150)
    link(trunk_col, "", lerp, "A")
    link(col, "", lerp, "B")
    link(vc, "A", lerp, "Alpha")          # vertex alpha: 0 = bark, 1 = foliage
    mul = mel.create_material_expression(mat, unreal.MaterialExpressionMultiply, -250, 0)
    link(lerp, "", mul, "A")
    link(vc, "", mul, "B")
    if not mel.connect_material_property(mul, "", unreal.MaterialProperty.MP_BASE_COLOR):
        raise RuntimeError("base colour")
    rough = mel.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -300, 200)
    rough.set_editor_property("parameter_name", "Roughness")
    rough.set_editor_property("default_value", 0.85)
    if not mel.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS):
        raise RuntimeError("roughness")
    # Wind sway (world position offset). The runtime owns the clock (WindTime is set per frame
    # by the harness: frozen for stills, frame/fps for orbits), so renders stay deterministic.
    # Bend grows with height above the instance pivot squared (stiff bole, loose top); each
    # tree gets its own phase from its position; a slow gust band travels downwind.
    # Offset from the INSTANCE pivot in world units (ObjectPositionWS is the primitive's, not
    # the instance's: with it, whole HISM tiles bent as one and self-shadowing tore).
    lpos = mel.create_material_expression(mat, unreal.MaterialExpressionLocalPosition, -1100, 450)
    lpos.set_editor_property("local_origin", unreal.LocalPositionOrigin.INSTANCE)
    lpos.set_editor_property("included_offsets", unreal.PositionIncludedOffsets.EXCLUDE_OFFSETS)
    local = mel.create_material_expression(mat, unreal.MaterialExpressionTransform, -900, 450)
    local.set_editor_property("transform_source_type",
                              unreal.MaterialVectorCoordTransformSource.TRANSFORMSOURCE_INSTANCE)
    local.set_editor_property("transform_type", unreal.MaterialVectorCoordTransform.TRANSFORM_WORLD)
    link(lpos, "", local, "")
    wpos = mel.create_material_expression(mat, unreal.MaterialExpressionWorldPosition, -900, 300)
    wpos.set_editor_property("world_position_shader_offset",
                             unreal.WorldPositionIncludedOffsets.WPT_EXCLUDE_ALL_SHADER_OFFSETS)
    opos = mel.create_material_expression(mat, unreal.MaterialExpressionSubtract, -700, 350)
    link(wpos, "", opos, "A")             # pivot = world position - (pivot -> vertex)
    link(local, "", opos, "B")
    params = {}
    for i, (name, default) in enumerate((("WindTime", 0.0), ("WindStrength", 6.0))):
        p = mel.create_material_expression(mat, unreal.MaterialExpressionScalarParameter,
                                           -900, 640 + 90 * i)
        p.set_editor_property("parameter_name", name)
        p.set_editor_property("default_value", default)
        params[name] = p
    wdir = mel.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -900, 820)
    wdir.set_editor_property("parameter_name", "WindDir")
    wdir.set_editor_property("default_value", unreal.LinearColor(1.0, 0.0, 0.0, 0.0))
    wind = mel.create_material_expression(mat, unreal.MaterialExpressionCustom, -450, 500)
    wind.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    wind.set_editor_property("description", "EmberWind")
    ins = []
    for n in ("Local", "Pivot", "Rnd", "T", "S", "D"):
        ci = unreal.CustomInput()
        ci.set_editor_property("input_name", n)
        ins.append(ci)
    wind.set_editor_property("inputs", ins)
    wind.set_editor_property("code", (
        "float h = max(Local.z, 0.0) / 1000.0;\n"                  # height above pivot, 10 m
        "float bend = S * h * h;\n"
        "float2 p = Pivot.xy * 0.01;\n"                            # metres
        "float ph = Rnd * 6.2831853;\n"                            # per-instance, exact
        "float along = dot(p, D.xy);\n"
        "float gust = 0.5 + 0.5 * sin(T * 0.35 - along * 0.012);\n"
        "float sway = 0.55 + 0.45 * sin(T * 1.6 + ph) * (0.6 + 0.4 * gust);\n"
        "return float3(D.xy * bend * sway * (0.7 + 0.6 * gust), -0.15 * bend * sway);\n"))
    link(local, "", wind, "Local")
    link(opos, "", wind, "Pivot")
    # Per-tree phase: PerInstanceRandom is constant over an instance (a hash of the computed
    # pivot is not: float jitter between vertices flipped floor() and tore crowns apart).
    rnd = mel.create_material_expression(mat, unreal.MaterialExpressionPerInstanceRandom, -900, 560)
    link(rnd, "", wind, "Rnd")
    link(params["WindTime"], "", wind, "T")
    link(params["WindStrength"], "", wind, "S")
    link(wdir, "", wind, "D")
    if not mel.connect_material_property(wind, "", unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET):
        raise RuntimeError("world position offset")
    mat.set_editor_property("max_world_position_offset_displacement", 600.0)  # Nanite WPO bounds
    mat.set_editor_property("used_with_instanced_static_meshes", True)
    mat.set_editor_property("used_with_nanite", True)
    mel.recompile_material(mat)
    eal.save_asset(full, only_if_is_dirty=False)
    unreal.log(f"EMBER_GENERATED {full}")
    return mat


# ------------------------------------------------------------------ mesh helpers
def opts():
    return unreal.GeometryScriptPrimitiveOptions()


def xf(x=0.0, y=0.0, z=0.0, yaw=0.0, sx=1.0, sy=1.0, sz=1.0):
    return unreal.Transform(location=unreal.Vector(x, y, z), rotation=unreal.Rotator(0.0, 0.0, yaw),
                            scale=unreal.Vector(sx, sy, sz))


def part(builder, shade, foliage=True):
    """Build a primitive into its own mesh and colour it: RGB grey = shading multiplier,
    alpha = 1 foliage / 0 bark (M_Veg lerps TrunkColor -> Color by it)."""
    m = unreal.DynamicMesh()
    builder(m)
    flags = unreal.GeometryScriptColorFlags()
    for ch in ("red", "green", "blue", "alpha"):  # be explicit: alpha carries bark/foliage
        flags.set_editor_property(ch, True)
    rgba = unreal.LinearColor(shade, shade, shade, 1.0 if foliage else 0.0)
    colors.set_mesh_constant_vertex_color(m, rgba, flags, False)
    return m


def add(target, piece):
    edits.append_mesh(target, piece, unreal.Transform())


def trunk(target, radius, height, shade=1.0):
    add(target, part(lambda m: prim.append_cylinder(m, opts(), xf(), radius=radius, height=height,
                                                    radial_steps=8, height_steps=1, capped=True),
                     shade, foliage=False))


def tier_crown(target, rng, base_z, top_z, r0, r1, tiers, overlap, droop=0.0):
    """Stacked cones from base_z to top_z; radius tapers r0 -> r1; lower tiers darker."""
    span = (top_z - base_z)
    step = span / tiers
    for i in range(tiers):
        f = i / max(1, tiers - 1)
        r = r0 + (r1 - r0) * f
        z = base_z + i * step
        h = step * overlap
        shade = 0.55 + 0.45 * f + rng.uniform(-0.05, 0.05)
        jitter = r * 0.06
        add(target, part(lambda m, r=r, z=z, h=h, jitter=jitter: prim.append_cone(
            m, opts(), xf(rng.uniform(-jitter, jitter), rng.uniform(-jitter, jitter), z - droop * r,
                          yaw=rng.uniform(0, 360)),
            base_radius=r, top_radius=r * 0.08, height=h, radial_steps=10, height_steps=1,
            capped=True), shade))
    # leader
    add(target, part(lambda m: prim.append_cone(m, opts(), xf(0, 0, top_z - step * 0.2),
                                                base_radius=r1 * 0.9, top_radius=0.5,
                                                height=step * 1.2, radial_steps=8, height_steps=1,
                                                capped=True), 1.0))


def blobs(target, rng, count, cx, cy, cz, spread_xy, spread_z, r_min, r_max, squash,
          shade_lo, shade_hi):
    for _ in range(count):
        a = rng.uniform(0, 2 * math.pi)
        d = spread_xy * math.sqrt(rng.random())
        x, y = cx + d * math.cos(a), cy + d * math.sin(a)
        z = cz + rng.uniform(-spread_z, spread_z)
        r = rng.uniform(r_min, r_max)
        shade = shade_lo + (shade_hi - shade_lo) * ((z - (cz - spread_z)) / max(1e-6, 2 * spread_z))
        add(target, part(lambda m, x=x, y=y, z=z, r=r: prim.append_sphere_lat_long(
            m, opts(), xf(x, y, z, sz=squash), radius=r, steps_phi=6, steps_theta=10), shade))


# ------------------------------------------------------------------ species
def douglas_fir(m):
    rng = random.Random(1)
    trunk(m, H * 0.025, H * 0.30)
    tier_crown(m, rng, base_z=H * 0.18, top_z=H * 0.95, r0=H * 0.20, r1=H * 0.04, tiers=8,
               overlap=1.8, droop=0.15)


def ponderosa(m):
    rng = random.Random(2)
    trunk(m, H * 0.03, H * 0.62)   # long clear bole
    blobs(m, rng, 9, 0, 0, H * 0.78, H * 0.10, H * 0.14, H * 0.07, H * 0.11, 0.8, 0.55, 1.0)


def grand_fir(m):
    rng = random.Random(3)
    trunk(m, H * 0.02, H * 0.25)
    tier_crown(m, rng, base_z=H * 0.12, top_z=H * 0.97, r0=H * 0.13, r1=H * 0.03, tiers=11,
               overlap=1.9, droop=0.05)


def shrub(m):
    rng = random.Random(4)
    blobs(m, rng, 6, 0, 0, H * 0.28, H * 0.22, H * 0.08, H * 0.16, H * 0.24, 0.7, 0.55, 1.0)


def bunchgrass(m):
    rng = random.Random(5)
    for _ in range(9):
        a = rng.uniform(0, 360)
        tilt = rng.uniform(8, 25)
        h = H * rng.uniform(0.7, 1.0)
        piece = part(lambda mm, h=h: prim.append_cone(mm, opts(), xf(), base_radius=H * 0.03,
                                                      top_radius=0.5, height=h, radial_steps=5,
                                                      height_steps=1, capped=True),
                     rng.uniform(0.7, 1.0))
        edits.append_mesh(m, piece, unreal.Transform(location=unreal.Vector(0, 0, 0),
                                                     rotation=unreal.Rotator(tilt, 0.0, a),
                                                     scale=unreal.Vector(1, 1, 1)))


SPECIES = {
    "pseudotsuga_menziesii": douglas_fir,
    "pinus_ponderosa": ponderosa,
    "abies_grandis": grand_fir,
    "artemisia_shrub": shrub,
    "bunchgrass": bunchgrass,
}


def build_mesh(key, builder, mat):
    full = f"{VEG}/SM_{key}"
    fresh(full)
    m = unreal.DynamicMesh()
    builder(m)
    normals.recompute_normals(m, unreal.GeometryScriptCalculateNormalsOptions())
    o = unreal.GeometryScriptCreateNewStaticMeshAssetOptions()
    o.set_editor_property("enable_nanite", True)
    o.set_editor_property("enable_collision", False)
    o.set_editor_property("enable_recompute_normals", False)
    sm, outcome = newasset.create_new_static_mesh_asset_from_mesh(m, full, o)
    if sm is None or outcome != unreal.GeometryScriptOutcomePins.SUCCESS:
        raise RuntimeError(f"create static mesh {full}: {outcome}")
    sm.set_material(0, mat)
    # The creation option alone left nanite_settings.enabled False (trees rendered as raster
    # HISM through HCP1); set it explicitly (PostEditChange rebuilds) and check it after reload.
    ns = sm.get_editor_property("nanite_settings")
    ns.set_editor_property("enabled", True)
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
    tris = m.get_triangle_count()
    unreal.log(f"EMBER_GENERATED {full}")
    unreal.log(f"EMBER_VEG {key}: {tris} triangles")


mat = build_material()
for k, fn in SPECIES.items():
    build_mesh(k, fn, mat)

# Verify the bark/foliage alpha survived into the built asset (it drives M_Veg's lerp).
for k in SPECIES:
    sm = unreal.load_asset(f"{VEG}/SM_{k}")
    if not sm.get_editor_property("nanite_settings").get_editor_property("enabled"):
        raise RuntimeError(f"SM_{k}: Nanite is not enabled")
    dm = unreal.DynamicMesh()
    unreal.GeometryScript_AssetUtils.copy_mesh_from_static_mesh(
        sm, dm, unreal.GeometryScriptCopyMeshFromAssetOptions(), unreal.GeometryScriptMeshReadLOD())
    res = colors.get_mesh_per_vertex_colors(dm)
    color_list = next(r for r in res
                      if type(r).__name__ == "GeometryScriptColorList")
    lst = unreal.GeometryScript_List
    arr = (lst.convert_color_list_to_array(color_list)
           if hasattr(lst, "convert_color_list_to_array") else [])
    alphas = sorted({round(c.a, 2) for c in arr})
    unreal.log(f"EMBER_VEG {k}: {len(arr)} vertex colours, alpha values {alphas[:6]}")
