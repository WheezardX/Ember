"""Generator: vegetation species meshes v2 + their materials (EPIC_5_PLAN B3 v2, HCP2 round 2).

    (the master /Game/Ember/Generated/M_Veg comes from m_veg.py)
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

master = unreal.load_asset(f"{PATH}/M_Veg")  # m_veg.py (runs first)
if master is None:
    raise RuntimeError("M_Veg missing: m_veg.py must run before veg_species.py")
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
