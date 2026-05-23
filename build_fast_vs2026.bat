@echo off
setlocal

set WP=%~dp0
cd /d "%WP%"

set build_type=Release
set build_dir=build
set target=ALL_BUILD

if "%1"=="debug" (
    set build_type=Debug
    set build_dir=build-dbg
)

if "%1"=="debuginfo" (
    set build_type=RelWithDebInfo
    set build_dir=build-dbginfo
)

if "%1"=="install" set target=install
if "%2"=="install" set target=install

if not exist "%build_dir%\CMakeCache.txt" (
    echo Build directory is not configured. Run build_release_vs2026.bat once first.
    exit /b 1
)

echo build type set to %build_type%
echo target set to %target%

cmake --build "%build_dir%" --config %build_type% --target %target% --parallel
