@echo off
setlocal

set "WP=%~dp0"
set "WP=%WP:~0,-1%"
for %%I in ("%WP%\..") do set "SOURCE_PARENT=%%~fI"

if "%ORCA_BUILD_ROOT%"=="" (
    set "BUILD_ROOT=%SOURCE_PARENT%\OrcaSlicer-build-vs2026"
) else (
    set "BUILD_ROOT=%ORCA_BUILD_ROOT%"
)

set "build_type=Release"
set "build_name=build"
set "target=ALL_BUILD"

if "%1"=="debug" (
    set "build_type=Debug"
    set "build_name=build-dbg"
)

if "%1"=="debuginfo" (
    set "build_type=RelWithDebInfo"
    set "build_name=build-dbginfo"
)

if "%1"=="install" set "target=install"
if "%2"=="install" set "target=install"

set "SLICER_BUILD_DIR=%BUILD_ROOT%\%build_name%"

if not exist "%SLICER_BUILD_DIR%\CMakeCache.txt" (
    echo External build directory is not configured.
    echo Run build_release_vs2026_external.bat once first.
    exit /b 1
)

echo build root set to %BUILD_ROOT%
echo build type set to %build_type%
echo target set to %target%

cmake --build "%SLICER_BUILD_DIR%" --config %build_type% --target %target% --parallel
