# Runner provisioning (EPIC_5_PLAN A4, D1/D2)

The runner is the one machine that builds and renders: Windows + discrete GPU + the pinned
engine. Today it is Brad's dev box (D2, on-demand: agents use it while a session is open).
`ember-dev doctor` checks every item below; a second machine should be provisionable from this
page alone.

| Item | Required | Runner today | Check |
|---|---|---|---|
| OS | Windows 10/11 x64 | Windows 11 Home 26200 | — |
| GPU | discrete, DX12, ≥ 8 GB | RTX 4080 SUPER 16 GB (driver 595.95) — **D8 reference GPU** | `gpu` |
| Engine | **UE 5.8.3, CL 58210709** (binary, Epic launcher) | `C:\Program Files\Epic Games\UE_5.8` | `engine`, `engine-files` |
| C++ toolchain | VS 2022 Build Tools, MSVC ≥ 14.38, C++ workload | MSVC 14.44.35207 | `msvc` |
| Windows SDK | 10.0.19041 – 10.9 | 10.0.26100.0 | `windows-sdk` |
| .NET Framework SDK | ≥ 4.6 (UBT's SwarmInterface refuses without it) | 4.8 | `netfx-sdk` |
| Python | terrain conda env (3.11) with ember installed editable | `C:\Users\xthat\miniforge3\envs\terrain` | `python:*` |
| ffmpeg | on PATH (MP4 bundles) | 8.1.1 (winget Gyan.FFmpeg.Essentials) | `ffmpeg` |
| Terrain store | sibling checkout `../Terrain/store` or `$EMBER_TERRAIN_STORE` | `C:\Projects\Terrain\store` | `terrain-store` |
| Disk | ≥ 50 GB free on the repo drive (DDC, shaders, runs) | ~310 GB | `disk` |

## From zero

1. Epic Games launcher → Unreal Engine → Library → install **5.8.3** exactly. (Upgrading is an
   ADR: change `unreal/engine.toml`, re-bless goldens deliberately.) A different install path
   is fine: set `EMBER_UE_ROOT`.
2. Visual Studio 2022 Build Tools with "Desktop development with C++", then add the .NET SDK:
   ```
   "C:\Program Files (x86)\Microsoft Visual Studio\Installer\setup.exe" modify ^
     --installPath "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools" ^
     --add Microsoft.Net.Component.4.8.SDK --add Microsoft.Net.Component.4.6.2.TargetingPack ^
     --quiet --norestart
   ```
   (elevated; pass the arguments as one string from PowerShell `Start-Process -Verb RunAs`).
3. `winget install Gyan.FFmpeg.Essentials`, CMake, Ninja (the sim/worldcore builds use them).
4. Clone Terrain and Ember side by side; `conda activate terrain`; `pip install -e ../Terrain`;
   `pip install -e .` (installs `ember` and `ember-dev`).
5. `ember-dev doctor` → all ok except `project-build`; `ember-dev regen-assets`;
   `ember-dev build`; `ember-dev loop S_terrain_gray` → PASS against the committed goldens.
   If a *new* GPU/driver produces a mismatch on an unchanged tree, that is driver drift: look
   at the sheet, and re-bless only with a note naming the driver.

## Availability

On-demand (D2). Nothing is scheduled; an agent session on this machine is the only thing that
opens an RHI. Cloud/non-GPU agents may edit code and run the engine-free checks
(`worldcore` tests, `regen-assets --check`, pytest) but never `run-scenario`.
