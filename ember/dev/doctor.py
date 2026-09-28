"""`ember-dev doctor` — is this machine a working runner? (EPIC_5_PLAN A4)

Every check is independent and reports ok / warn / fail with a fix hint; the runner
provisioning doc (docs/viz/runner.md) is the long form of these hints.
"""

from __future__ import annotations

import importlib.util
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

from ember.dev.scenario import terrain_store_root
from ember.dev.ue import Engine, repo_root


@dataclass
class Check:
    name: str
    status: str   # ok | warn | fail
    detail: str
    fix: str = ""


def _vswhere() -> Path:
    return Path(r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe")


def run_checks(eng: Engine) -> list[Check]:
    out: list[Check] = []

    inst = eng.installed_version()
    if inst is None:
        out.append(Check("engine", "fail", f"no engine at {eng.root}",
                         "install UE via the Epic launcher or set EMBER_UE_ROOT"))
    elif inst != (eng.version, eng.changelist):
        out.append(Check("engine", "fail", f"installed {inst[0]} CL {inst[1]}, pinned "
                         f"{eng.version} CL {eng.changelist}",
                         "install the pinned version; upgrades need an ADR + unreal/engine.toml"))
    else:
        out.append(Check("engine", "ok", f"UE {inst[0]} CL {inst[1]} at {eng.root}"))
    for exe in (eng.editor_exe, eng.editor_cmd_exe, eng.build_bat):
        if not exe.exists():
            out.append(Check("engine-files", "fail", f"missing {exe}", "repair the engine install"))
            break
    else:
        out.append(Check("engine-files", "ok", "editor, editor-cmd, Build.bat present"))

    vs = _vswhere()
    msvc = ""
    if vs.exists():
        p = subprocess.run([str(vs), "-products", "*", "-latest", "-property", "installationPath"],
                           capture_output=True, text=True)
        ip = Path(p.stdout.strip()) if p.stdout.strip() else None
        tools = ip / "VC" / "Tools" / "MSVC" if ip else None
        if tools and tools.exists():
            msvc = ", ".join(sorted(d.name for d in tools.iterdir()))
    if msvc:
        out.append(Check("msvc", "ok", f"MSVC {msvc} (UE 5.8 minimum 14.38)"))
    else:
        out.append(Check("msvc", "fail", "no MSVC toolset found",
                         "install VS 2022 Build Tools with the C++ workload"))

    netfx = Path(r"C:\Program Files (x86)\Windows Kits\NETFXSDK")
    if netfx.exists() and any(netfx.iterdir()):
        out.append(Check("netfx-sdk", "ok", ", ".join(d.name for d in netfx.iterdir())))
    else:
        out.append(Check("netfx-sdk", "fail", "no .NET Framework SDK (UBT SwarmInterface needs it)",
                         "VS installer: add Microsoft.Net.Component.4.8.SDK"))

    winsdk = Path(r"C:\Program Files (x86)\Windows Kits\10\Include")
    vers = sorted(d.name for d in winsdk.iterdir()) if winsdk.exists() else []
    out.append(Check("windows-sdk", "ok" if vers else "fail", ", ".join(vers) or "none",
                     "" if vers else "install a Windows 10/11 SDK"))

    smi = shutil.which("nvidia-smi")
    if smi:
        p = subprocess.run([smi, "--query-gpu=name,memory.total,driver_version",
                            "--format=csv,noheader"], capture_output=True, text=True)
        first = p.stdout.strip().splitlines()[0] if p.stdout.strip() else "?"
        out.append(Check("gpu", "ok", first))
    else:
        out.append(Check("gpu", "warn", "nvidia-smi not found; cannot confirm the reference GPU"))

    ff = shutil.which("ffmpeg")
    out.append(Check("ffmpeg", "ok" if ff else "warn", ff or "not on PATH",
                     "" if ff else "winget install Gyan.FFmpeg (needed for MP4 bundles)"))

    for mod in ("numpy", "scipy", "PIL", "pydantic", "typer"):
        if importlib.util.find_spec(mod) is None:
            out.append(Check(f"python:{mod}", "fail", "missing", "use the terrain conda env"))

    store = terrain_store_root()
    region = store / "teanaway_dev" / "manifest.json"
    have = region.exists()
    out.append(Check("terrain-store", "ok" if have else "warn",
                     f"{store} (teanaway_dev {'present' if have else 'missing'})",
                     "" if have else "clone Terrain beside Ember or set EMBER_TERRAIN_STORE"))

    free = shutil.disk_usage(repo_root()).free / 2**30
    out.append(Check("disk", "ok" if free > 50 else "warn",
                     f"{free:.0f} GB free on the repo drive"))

    built = eng.project.parent / "Binaries" / "Win64" / f"UnrealEditor-{eng.project.stem}.dll"
    out.append(Check("project-build", "ok" if built.exists() else "warn",
                     str(built) if built.exists() else "editor modules not built yet",
                     "" if built.exists() else "ember-dev build"))
    return out
