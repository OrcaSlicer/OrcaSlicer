set WP=%CD%

set debug=OFF
set debuginfo=OFF
if "%1"=="debug" set debug=ON
if "%2"=="debug" set debug=ON
if "%1"=="debuginfo" set debuginfo=ON
if "%2"=="debuginfo" set debuginfo=ON
if "%debug%"=="ON" (
    set build_type=Debug
    set build_dir=build-dbg
) else (
    if "%debuginfo%"=="ON" (
        set build_type=RelWithDebInfo
        set build_dir=build-dbginfo
    ) else (
        set build_type=Release
        set build_dir=build
    )
)
echo build type set to %build_type%

cd deps
mkdir %build_dir%
cd %build_dir%
set DEPS=%CD%/OrcaSlicer_dep
set "SIG_FLAG="
if defined ORCA_UPDATER_SIG_KEY set "SIG_FLAG=-DORCA_UPDATER_SIG_KEY=%ORCA_UPDATER_SIG_KEY%"
if "%1"=="slicer" (
    GOTO :slicer
)
echo "building deps.."

echo cmake ../ -G "Visual Studio 16 2019" -A x64 -DCMAKE_BUILD_TYPE=%build_type%
cmake ../ -G "Visual Studio 16 2019" -A x64 -DCMAKE_BUILD_TYPE=%build_type%
cmake --build . --config %build_type% --target deps -- -m

if "%1"=="deps" exit /b 0

:slicer
echo "building Orca Slicer..."
cd %WP%
mkdir %build_dir%
cd %build_dir%

echo cmake .. -G "Visual Studio 16 2019" -A x64 -DCMAKE_BUILD_TYPE=%build_type%
cmake .. -G "Visual Studio 16 2019" -A x64 -DCMAKE_BUILD_TYPE=%build_type% %SIG_FLAG%
cmake --build . --config %build_type% --target ALL_BUILD -- -m
cd ..
call scripts/run_gettext.bat
cd %build_dir%
cmake --build . --target install --config %build_type%
