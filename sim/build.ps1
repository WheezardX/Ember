# Configure + build embersim on Windows with MSVC + Ninja (developer convenience; CI uses the
# same cmake invocations). Usage: pwsh sim/build.ps1 [-Config Release|Debug] [-Test]
param([string]$Config = "Release", [switch]$Test)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found; install VS 2022 Build Tools (C++ workload)" }
$build = Join-Path $root "build\$Config"
$cmd = "call `"$vcvars`" >nul && cmake -S `"$root`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=$Config && cmake --build `"$build`" --parallel"
if ($Test) { $cmd += " && `"$build\embersim_tests.exe`"" }
cmd /c $cmd
exit $LASTEXITCODE
