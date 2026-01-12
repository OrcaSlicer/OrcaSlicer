@REM OrcaSlicer build script for Windows
@echo off
set WP=%CD%

@REM Load optional local build configuration (.config) for generator and parallelism.
set "SCRIPT_DIR=%~dp0"
set "ORCA_CONFIG_FILE=%SCRIPT_DIR%.config"
if exist "%ORCA_CONFIG_FILE%" (
    for /f "usebackq tokens=1,2 delims== " %%A in ("%ORCA_CONFIG_FILE%") do (
        if /I "%%A"=="GENERATOR" (
            if not defined ORCA_FORCE_NINJA if not defined ORCA_FORCE_NMAKE (
                if /I "%%B"=="NINJA" (
                    set "ORCA_FORCE_NINJA=1"
                    set "ORCA_FORCE_NMAKE="
                ) else if /I "%%B"=="NMAKE" (
                    set "ORCA_FORCE_NINJA="
                    set "ORCA_FORCE_NMAKE=1"
                )
            )
        ) else if /I "%%A"=="PARALLEL" (
            set "CMAKE_BUILD_PARALLEL_LEVEL=%%B"
        )
    )
)

@REM If CMAKE_BUILD_PARALLEL_LEVEL is still not set, default it to the
@REM number of logical processors so that all cmake --build calls run
@REM in parallel.
if not defined CMAKE_BUILD_PARALLEL_LEVEL (
    if defined NUMBER_OF_PROCESSORS (
        set "CMAKE_BUILD_PARALLEL_LEVEL=%NUMBER_OF_PROCESSORS%"
    )
)

@REM Prepare a reusable parallel build flag for all cmake --build calls.
if defined CMAKE_BUILD_PARALLEL_LEVEL (
    set "CMAKE_BUILD_PARALLEL_OPTS=-j %CMAKE_BUILD_PARALLEL_LEVEL%"
) else (
    set "CMAKE_BUILD_PARALLEL_OPTS=-j"
)

@REM Pack deps
if "%1"=="pack" (
    setlocal ENABLEDELAYEDEXPANSION 
    cd %WP%/deps/build
    for /f "tokens=2-4 delims=/ " %%a in ('date /t') do set build_date=%%c%%b%%a
    echo packing deps: OrcaSlicer_dep_win64_!build_date!_vs2022.zip

    %WP%/tools/7z.exe a OrcaSlicer_dep_win64_!build_date!_vs2022.zip OrcaSlicer_dep
    exit /b 0
)

set debug=OFF
set debuginfo=OFF
if "%1"=="debug" set debug=ON
if "%2"=="debug" set debug=ON
if "%1"=="debuginfo" set debuginfo=ON
if "%2"=="debuginfo" set debuginfo=ON

@REM For single-config generators like NMake, a full Debug build is incompatible
@REM with the prebuilt Release-only third-party libraries (Boost, OpenCV, TBB,
@REM wxWidgets). When using the "debug" mode together with ORCA_FORCE_NMAKE,
@REM map the build type to RelWithDebInfo so that the OrcaSlicer binaries match
@REM the Release CRT and iterator level while still producing debug symbols.
if "%debug%"=="ON" (
    set build_dir=build-dbg
    if defined ORCA_FORCE_NMAKE (
        set build_type=RelWithDebInfo
    ) else (
        set build_type=Debug
    )
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

setlocal DISABLEDELAYEDEXPANSION 
cd deps
mkdir %build_dir%
cd %build_dir%
set "SIG_FLAG="
if defined ORCA_UPDATER_SIG_KEY set "SIG_FLAG=-DORCA_UPDATER_SIG_KEY=%ORCA_UPDATER_SIG_KEY%"

if "%1"=="slicer" (
    GOTO :slicer
)
echo "building deps.."

echo on
REM Set minimum CMake policy to avoid <3.5 errors
set CMAKE_POLICY_VERSION_MINIMUM=3.5
REM Choose generator: prefer Ninja (ORCA_FORCE_NINJA), then NMake (ORCA_FORCE_NMAKE),
REM otherwise default to the Visual Studio generator.
if defined ORCA_FORCE_NINJA (
    REM Clean existing CMake cache when switching generators
    if exist CMakeCache.txt del /f /q CMakeCache.txt
    if exist CMakeFiles rmdir /s /q CMakeFiles
    set CMAKE_GEN_ARGS=-G "Ninja"
    set CMAKE_BUILD_EXTRA_ARGS=
) else if defined ORCA_FORCE_NMAKE (
    REM Clean existing CMake cache when switching generators
    if exist CMakeCache.txt del /f /q CMakeCache.txt
    if exist CMakeFiles rmdir /s /q CMakeFiles
    set CMAKE_GEN_ARGS=-G "NMake Makefiles"
    set CMAKE_BUILD_EXTRA_ARGS=
) else (
    set CMAKE_GEN_ARGS=-G "Visual Studio 17 2022" -A x64
    set CMAKE_BUILD_EXTRA_ARGS=-- -m
)
cmake ../ %CMAKE_GEN_ARGS% -DCMAKE_BUILD_TYPE=%build_type%
cmake --build . --config %build_type% --target deps %CMAKE_BUILD_PARALLEL_OPTS% %CMAKE_BUILD_EXTRA_ARGS%
@echo off

if "%1"=="deps" exit /b 0

:slicer
echo "building Orca Slicer..."
cd %WP%
mkdir %build_dir%
cd %build_dir%

echo on
set CMAKE_POLICY_VERSION_MINIMUM=3.5
REM Force precompiled headers off to avoid cache-related issues.
set CMAKE_PCH_ARGS=-DBUILD_USE_PCH=OFF -DSLIC3R_PCH=OFF

if defined ORCA_FORCE_NINJA (
    if exist CMakeCache.txt del /f /q CMakeCache.txt
    if exist CMakeFiles rmdir /s /q CMakeFiles
    set CMAKE_GEN_ARGS=-G "Ninja"
    set CMAKE_BUILD_EXTRA_ARGS=
) else if defined ORCA_FORCE_NMAKE (
    if exist CMakeCache.txt del /f /q CMakeCache.txt
    if exist CMakeFiles rmdir /s /q CMakeFiles
    set CMAKE_GEN_ARGS=-G "NMake Makefiles"
    set CMAKE_BUILD_EXTRA_ARGS=
) else (
    set CMAKE_GEN_ARGS=-G "Visual Studio 17 2022" -A x64
    set CMAKE_BUILD_EXTRA_ARGS=-- -m
)
cmake .. %CMAKE_GEN_ARGS% -DORCA_TOOLS=ON %SIG_FLAG% -DCMAKE_BUILD_TYPE=%build_type% %CMAKE_PCH_ARGS%
if defined ORCA_FORCE_NINJA (
    cmake --build . --config %build_type% %CMAKE_BUILD_PARALLEL_OPTS% %CMAKE_BUILD_EXTRA_ARGS%
) else if defined ORCA_FORCE_NMAKE (
    cmake --build . --config %build_type% %CMAKE_BUILD_PARALLEL_OPTS% %CMAKE_BUILD_EXTRA_ARGS%
) else (
    cmake --build . --config %build_type% --target ALL_BUILD %CMAKE_BUILD_PARALLEL_OPTS% %CMAKE_BUILD_EXTRA_ARGS%
)
@echo off
cd ..
call scripts/run_gettext.bat
cd %build_dir%
cmake --build . --target install --config %build_type% %CMAKE_BUILD_PARALLEL_OPTS%
