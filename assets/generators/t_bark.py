"""Generator: /Game/Ember/Generated/Bark/T_Bark_<key>_{C,N} - bark texture sets for the trees.

Imports the CC0 sets fetched by `ember-dev fetch-textures assets/sources/bark.toml` from
store/textures/bark/<key>/ (outside git; see that manifest for sources, licences and which species
use each set):
    _C  colour, sRGB (M_Veg uses it as variation around the species' TrunkColor)
    _N  normal map, OpenGL-style at the source -> green flipped on import (UE is DirectX-style)
Runs headless via `ember-dev regen-assets` (after fetch-textures).
"""

import os
import tomllib

import unreal

PATH = "/Game/Ember/Generated/Bark"
here = os.path.dirname(os.path.abspath(__file__))
repo = os.path.normpath(os.path.join(here, "..", ".."))
manifest = tomllib.load(open(os.path.join(repo, "assets", "sources", "bark.toml"), "rb"))
src_root = os.path.join(repo, "store", "textures", "bark")
eal = unreal.EditorAssetLibrary

tasks = []
for s in manifest["set"]:
    for kind, fname in (("C", "color.jpg"), ("N", "normal_gl.jpg")):
        f = os.path.join(src_root, s["key"], fname)
        if not os.path.exists(f):
            raise RuntimeError(f"{f} missing: run `ember-dev fetch-textures assets/sources/bark.toml`")
        name = f"T_Bark_{s['key']}_{kind}"
        if eal.does_asset_exist(f"{PATH}/{name}"):
            eal.delete_asset(f"{PATH}/{name}")
        t = unreal.AssetImportTask()
        t.set_editor_property("filename", f)
        t.set_editor_property("destination_path", PATH)
        t.set_editor_property("destination_name", name)
        t.set_editor_property("automated", True)
        t.set_editor_property("replace_existing", True)
        t.set_editor_property("save", False)
        tasks.append(t)
unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks(tasks)

for s in manifest["set"]:
    for kind in ("C", "N"):
        full = f"{PATH}/T_Bark_{s['key']}_{kind}"
        tex = unreal.load_asset(full)
        if tex is None:
            raise RuntimeError(f"import failed: {full}")
        if kind == "N":
            tex.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_NORMALMAP)
            tex.set_editor_property("srgb", False)
            tex.set_editor_property("flip_green_channel", True)
        else:
            tex.set_editor_property("srgb", True)
        tex.set_editor_property("lod_group", unreal.TextureGroup.TEXTUREGROUP_WORLD)
        # resident, as the ground sets: stills are taken seconds after load (12 x 2K ~ 64 MB)
        tex.set_editor_property("never_stream", True)
        tex.set_editor_property("address_x", unreal.TextureAddress.TA_WRAP)
        tex.set_editor_property("address_y", unreal.TextureAddress.TA_WRAP)
        eal.save_asset(full, only_if_is_dirty=False)
        unreal.log(f"EMBER_GENERATED {full}")
