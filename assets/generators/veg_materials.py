"""Generator: per-species vegetation material instances (split out of veg_species.py, 2026-09-30).

    /Game/Ember/Generated/Veg/MI_Veg_<key>    per species: Color / TrunkColor (treegen.COLORS),
                                              FireOutcomes 1 for trees (8g burned-area mosaic)

Separate from the meshes so a material setting change re-runs in seconds instead of rebuilding
every Nanite tree mesh (~20 min). Instances are updated IN PLACE (never deleted), so the meshes
built by veg_species.py keep their material references. Runs headless via `ember-dev regen-assets`.
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
import tomllib  # noqa: E402

_bark = tomllib.load(open(os.path.join(_here, "..", "sources", "bark.toml"), "rb"))
BARK = {sp: s["key"] for s in _bark["set"] for sp in s["species"]}   # species key -> bark set
eal = unreal.EditorAssetLibrary
mel = unreal.MaterialEditingLibrary
tools = unreal.AssetToolsHelpers.get_asset_tools()


def srgb(hexcol):
    c = [int(hexcol[i:i + 2], 16) / 255.0 for i in (0, 2, 4)]
    lin = [x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4 for x in c]
    return unreal.LinearColor(lin[0], lin[1], lin[2], 1.0)


def build_instance(key, master):
    name = f"MI_Veg_{key}"
    full = f"{VEG}/{name}"
    mi = unreal.load_asset(full) if eal.does_asset_exist(full) else None
    if mi is None:
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
    # trees get per-tree burned outcomes (bare / consumed / scorched / survivor; 8g mosaic);
    # low plants keep the cover-style collapse
    tree = key in treegen.CONIFERS or key in treegen.BROADLEAF
    mel.set_material_instance_scalar_parameter_value(mi, "FireOutcomes", 1.0 if tree else 0.0)
    # bark texture (t_bark.py; species -> set in assets/sources/bark.toml); none: flat TrunkColor
    bset = BARK.get(key)
    mel.set_material_instance_scalar_parameter_value(mi, "BarkOn", 1.0 if bset else 0.0)
    if bset:
        for param, kind in (("BarkColor", "C"), ("BarkNormal", "N")):
            tex = unreal.load_asset(f"{PATH}/Bark/T_Bark_{bset}_{kind}")
            if tex is None:
                raise RuntimeError(f"T_Bark_{bset}_{kind} missing: t_bark.py must run before veg_materials.py")
            mel.set_material_instance_texture_parameter_value(mi, param, tex)
    mel.update_material_instance(mi)
    eal.save_asset(full, only_if_is_dirty=False)
    unreal.log(f"EMBER_GENERATED {full}")
    return mi


master = unreal.load_asset(f"{PATH}/M_Veg")  # m_veg.py (runs first)
if master is None:
    raise RuntimeError("M_Veg missing: m_veg.py must run before veg_materials.py")
for k in treegen.SPECIES:
    build_instance(k, master)
