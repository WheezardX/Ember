"""Generator: /Game/Ember/Generated/Ground/T_Ground_<Set>_{C,N} - the ground detail sets (ground
plane v1, EPIC_5_PLAN 8f GP3).

The pixels are synthesised by ground_tex_host.py in the host (terrain env) Python - numpy is not
available inside UE - which `ember-dev regen-assets` names in EMBER_HOST_PYTHON. This script runs
it into a temp folder and imports the PNGs:
    _C  sRGB, default compression: RGB colour multiplier (mean 0.5), A height
    _N  linear, TC_Masks: RG slope offset, B roughness, A ambient occlusion
"""

import os
import subprocess
import tempfile

import unreal

PATH = "/Game/Ember/Generated/Ground"
SETS = ["Litter", "Grass", "Rock", "Shrub"]
SIZE = 1024

eal = unreal.EditorAssetLibrary
here = os.path.dirname(os.path.abspath(__file__))
host = os.environ.get("EMBER_HOST_PYTHON")
if not host or not os.path.exists(host):
    raise RuntimeError("EMBER_HOST_PYTHON is not set: run through `ember-dev regen-assets`")
out = tempfile.mkdtemp(prefix="ember_ground_")
r = subprocess.run([host, os.path.join(here, "ground_tex_host.py"), out, "--size", str(SIZE)],
                   capture_output=True, text=True)
if r.returncode != 0:
    raise RuntimeError(f"ground_tex_host.py failed: {r.stdout}\n{r.stderr}")

tasks = []
for s in SETS:
    for kind in ("C", "N"):
        name = f"T_Ground_{s}_{kind}"
        full = f"{PATH}/{name}"
        if eal.does_asset_exist(full):
            eal.delete_asset(full)
        t = unreal.AssetImportTask()
        t.set_editor_property("filename", os.path.join(out, f"{name}.png"))
        t.set_editor_property("destination_path", PATH)
        t.set_editor_property("destination_name", name)
        t.set_editor_property("automated", True)
        t.set_editor_property("replace_existing", True)
        t.set_editor_property("save", False)
        tasks.append(t)
unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks(tasks)

for s in SETS:
    for kind in ("C", "N"):
        full = f"{PATH}/T_Ground_{s}_{kind}"
        tex = unreal.load_asset(full)
        if tex is None:
            raise RuntimeError(f"import failed: {full}")
        # both kinds are data: C holds mean-0.5 colour MULTIPLIERS (an sRGB decode would make
        # 2 x C average ~0.43 and darken the near ground), N slope / roughness / AO
        tex.set_editor_property("srgb", False)
        if kind == "N":
            tex.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_MASKS)
        tex.set_editor_property("lod_group", unreal.TextureGroup.TEXTUREGROUP_WORLD)
        # resident: captures happen seconds after load, before streamed mips would arrive
        # (the first ground renders showed only the blurred low mips); 8 x 1024^2 ~ 30 MB
        tex.set_editor_property("never_stream", True)
        tex.set_editor_property("address_x", unreal.TextureAddress.TA_WRAP)
        tex.set_editor_property("address_y", unreal.TextureAddress.TA_WRAP)
        eal.save_asset(full, only_if_is_dirty=False)
        unreal.log(f"EMBER_GENERATED {full}")
