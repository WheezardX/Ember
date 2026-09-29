"""Generator: ground-cover meshes + materials (ground plane v1, EPIC_5_PLAN 8f GP4).

    /Game/Ember/Generated/Cover/MI_Cover_<item>          M_Veg instance: Color / TrunkColor
    /Game/Ember/Generated/Cover/SM_Cover_<item>_v<N>     Nanite mesh, groundgen.VARIANTS each

Geometry from groundgen.py (fern, huckleberry, shrub, rock, log, stump); grass reuses the
scatter's SM_bunchgrass_v*. Same build as veg_species.py: Nanite with Preserve Area, no distance
field or Lumen cards (instances never join the DF scene). Runs via `ember-dev regen-assets`.
"""

import os
import sys

import unreal

_here = os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else os.path.join(
    unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir()), "..", "..", "assets",
    "generators")
sys.path.insert(0, _here)
import groundgen  # noqa: E402

PATH = "/Game/Ember/Generated"
COVER = f"{PATH}/Cover"
eal = unreal.EditorAssetLibrary
mel = unreal.MaterialEditingLibrary
edits = unreal.GeometryScript_MeshEdits
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
    name = f"MI_Cover_{key}"
    full = f"{COVER}/{name}"
    fresh(full)
    mi = tools.create_asset(name, COVER, unreal.MaterialInstanceConstant,
                            unreal.MaterialInstanceConstantFactoryNew())
    mel.set_material_instance_parent(mi, master)
    foliage, wood = groundgen.COLORS[key]
    for param, hexcol in (("Color", foliage), ("TrunkColor", wood)):
        want = srgb(hexcol)
        mel.set_material_instance_vector_parameter_value(mi, param, want)
        got = mel.get_material_instance_vector_parameter_value(mi, param)
        if abs(got.r - want.r) > 1e-4 or abs(got.g - want.g) > 1e-4:
            raise RuntimeError(f"{name}: {param} not set (got {got})")
    mel.update_material_instance(mi)
    eal.save_asset(full, only_if_is_dirty=False)
    unreal.log(f"EMBER_GENERATED {full}")
    return mi


def build_mesh(key, variant, mi):
    name = f"SM_Cover_{key}_v{variant}"
    full = f"{COVER}/{name}"
    fresh(full)
    gm = groundgen.build(key, variant)
    buf = unreal.GeometryScriptSimpleMeshBuffers()
    buf.set_editor_property("vertices", [unreal.Vector(*p) for p in gm.verts])
    buf.set_editor_property("triangles", [unreal.IntVector(*t) for t in gm.tris])
    buf.set_editor_property("vertex_colors", [unreal.LinearColor(*c) for c in gm.cols])
    buf.set_editor_property("uv0", [unreal.Vector2D((v[0] + v[1]) / 100.0, v[2] / 100.0)
                                    for v in gm.verts])
    dm = unreal.DynamicMesh()
    edits.append_buffers_to_mesh(dm, buf)
    if dm.get_triangle_count() != len(gm.tris):
        raise RuntimeError(f"{name}: {dm.get_triangle_count()} triangles, want {len(gm.tris)}")
    normals.recompute_normals(dm, unreal.GeometryScriptCalculateNormalsOptions())
    o = unreal.GeometryScriptCreateNewStaticMeshAssetOptions()
    o.set_editor_property("enable_nanite", True)
    o.set_editor_property("enable_collision", False)
    o.set_editor_property("enable_recompute_normals", False)
    sm, outcome = newasset.create_new_static_mesh_asset_from_mesh(dm, full, o)
    if sm is None or outcome != unreal.GeometryScriptOutcomePins.SUCCESS:
        raise RuntimeError(f"create static mesh {full}: {outcome}")
    sm.set_material(0, mi)
    ns = sm.get_editor_property("nanite_settings")
    ns.set_editor_property("enabled", True)
    ns.set_editor_property("shape_preservation", unreal.NaniteShapePreservation.PRESERVE_AREA)
    sm.set_editor_property("nanite_settings", ns)
    lib = unreal.EditorStaticMeshLibrary
    bs = lib.get_lod_build_settings(sm, 0)
    bs.set_editor_property("distance_field_resolution_scale", 0.0)
    bs.set_editor_property("max_lumen_mesh_cards", 0)
    lib.set_lod_build_settings(sm, 0, bs)
    eal.save_asset(full, only_if_is_dirty=False)
    unreal.log(f"EMBER_GENERATED {full}")


master = unreal.load_asset(f"{PATH}/M_Veg")
if master is None:
    raise RuntimeError("M_Veg missing: m_veg.py must run before ground_cover.py")
for k in groundgen.ITEMS:
    mi = build_instance(k, master)
    for v in range(groundgen.VARIANTS):
        build_mesh(k, v, mi)
    sm = unreal.load_asset(f"{COVER}/SM_Cover_{k}_v0")
    if not sm.get_editor_property("nanite_settings").get_editor_property("enabled"):
        raise RuntimeError(f"SM_Cover_{k}_v0: Nanite is not enabled")
