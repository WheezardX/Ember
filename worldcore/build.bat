@echo off
rem Windows dev build wrapper: worldcore\build.bat <builddir> configure | build [ninja args] | test
rem Uses VS 2022 Build Tools (MSVC) + Ninja. CI uses plain cmake; this only sets up the env.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set PATH=%PATH%;C:\Program Files\CMake\bin;C:\Users\xthat\AppData\Local\Microsoft\WinGet\Packages\Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe
set HERE=%~dp0
set BUILDDIR=%1
shift
if "%1"=="configure" (
  cmake -S "%HERE%." -B "%BUILDDIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release
  exit /b %ERRORLEVEL%
)
if "%1"=="test" (
  "%BUILDDIR%\emberworld_tests.exe" %2 %3 %4 %5 %6
  exit /b %ERRORLEVEL%
)
shift
cmake --build "%BUILDDIR%" %1 %2 %3 %4 %5 %6
exit /b %ERRORLEVEL%
