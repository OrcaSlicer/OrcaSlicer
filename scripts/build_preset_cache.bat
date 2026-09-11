@echo off
rem Build the per-vendor system preset caches (one <vendor>.opc per vendor) by
rem running the generate_system_cache.exe dev tool against a profiles directory,
rem and make every profiles directory named on the command line ship-ready:
rem install the caches into it and delete the preset JSONs they replace, so a
rem build ships one copy of its presets instead of two.
rem
rem   scripts\build_preset_cache.bat [build_dir] [target_dir ...]
rem
rem   build_dir   defaults to "build"
rem   target_dir  profiles directories to ship into. Caches are generated into
rem               the source tree's resources\profiles, which is what every
rem               packaging step copies from; a target may be that same
rem               directory, which then only gets pruned.
rem   --prune-source
rem               allow a target that is the directory the caches were generated
rem               into (resources\profiles). Pruning it deletes the checkout's
rem               own preset JSONs, which is a packaging step - not something a
rem               build should do to a working tree by surprise. CI passes it.
rem
rem Shipping deletes, so it is a CI packaging step. A vendor's own <vendor>.json
rem goes along with its preset JSONs: the cache carries the vendor profile and
rem the version it was built at, so discovery, version checks and installing all
rem read it there. Only a vendor that has a cache is pruned, so non-vendor JSONs
rem (blacklist.json) are left alone, as are the vendor directories themselves -
rem thumbnails, covers and bed models still live there.
rem
rem   set CONFIG=<cfg> to pin the build config for multi-config generators
rem   (default: the config of the tool already in the build tree, else Release)
setlocal enabledelayedexpansion

set "REPO_ROOT=%~dp0.."

set "PRUNE_SOURCE="
:parse_flags
if /i "%~1"=="--prune-source" (
    set "PRUNE_SOURCE=1"
    shift
    goto :parse_flags
)

set "BUILD_DIR=%~1"
if "%BUILD_DIR%"=="" set "BUILD_DIR=build"
if not exist "%BUILD_DIR%\" (
    echo ERROR: build tree not found: %BUILD_DIR% 1>&2
    exit /b 1
)
if not "%~1"=="" shift

rem Newest match wins: a stale binary silently produces a stale cache layout.
call :find_tool
if not defined CONFIG (
    for %%c in (Debug Release RelWithDebInfo MinSizeRel) do (
        echo !TOOL! | findstr /i "\\%%c\\" >nul && set "CONFIG=%%c"
    )
)
if not defined CONFIG set "CONFIG=Release"

echo Building generate_system_cache in %BUILD_DIR% (%CONFIG%)
cmake --build "%BUILD_DIR%" --config %CONFIG% --target generate_system_cache
if errorlevel 1 (
    echo ERROR: could not build generate_system_cache - configure the build tree with -DORCA_TOOLS=ON: 1>&2
    echo        cmake -S "%REPO_ROOT%" -B "%BUILD_DIR%" -DORCA_TOOLS=ON 1>&2
    exit /b 1
)
call :find_tool
if not defined TOOL (
    echo ERROR: generate_system_cache.exe not found under %BUILD_DIR% - build with -DORCA_TOOLS=ON 1>&2
    exit /b 1
)

set "PROFILES=%REPO_ROOT%\resources\profiles"
if not exist "%PROFILES%\" (
    echo ERROR: profiles directory not found: %PROFILES% 1>&2
    exit /b 1
)
for %%d in ("%PROFILES%") do set "PROFILES=%%~fd"

rem Add the slicer's runtime DLL directory to PATH so generate_system_cache.exe
rem can resolve its dependencies (TKernel.dll etc.) without a full install step.
set "DLL_DIR="
for /f "delims=" %%f in ('dir /s /b "%BUILD_DIR%\TKernel.dll" 2^>nul') do (
    if not defined DLL_DIR set "DLL_DIR=%%~dpf"
)
if defined DLL_DIR set "PATH=%DLL_DIR%;%PATH%"

echo Generating per-vendor preset caches in %PROFILES%
rem Start clean so vendors that went away - and caches written by older tool
rem versions - don't linger next to the freshly generated ones.
del /q "%PROFILES%\*.opc" 2>nul
del /q "%PROFILES%\*.cache" 2>nul
"%TOOL%" --path "%PROFILES%" --log_level 2
if errorlevel 1 exit /b %errorlevel%

:next_target
if "%~1"=="" exit /b 0
call :ship "%~1"
if errorlevel 1 exit /b 1
shift
goto :next_target

:ship
set "TARGET=%~1"
if not exist "%TARGET%\" (
    echo ERROR: profiles directory not found: %TARGET% 1>&2
    exit /b 1
)
for %%d in ("%TARGET%") do set "TARGET=%%~fd"
if /i "%TARGET%"=="%PROFILES%" if not defined PRUNE_SOURCE (
    echo %TARGET%: skipped - this is where the caches were generated.
    echo   Pass --prune-source to prune it; that deletes this checkout's preset JSONs.
    exit /b 0
)
if /i not "%TARGET%"=="%PROFILES%" copy /y "%PROFILES%\*.opc" "%TARGET%\" >nul

set /a SHIPPED=0
set /a PRUNED=0
for %%c in ("%PROFILES%\*.opc") do (
    set /a SHIPPED+=1
    set "VENDOR=%%~nc"
    if exist "%TARGET%\!VENDOR!.json" (
        del /q "%TARGET%\!VENDOR!.json"
        set /a PRUNED+=1
    )
    if exist "%TARGET%\!VENDOR!\" (
        for /f %%n in ('dir /s /b "%TARGET%\!VENDOR!\*.json" 2^>nul ^| find /c /v ""') do set /a PRUNED+=%%n
        del /s /q "%TARGET%\!VENDOR!\*.json" >nul 2>&1
        rem Deepest first, so a directory the delete above emptied goes too; rd
        rem refuses the ones still holding covers or meshes.
        for /f "delims=" %%d in ('dir /s /b /ad "%TARGET%\!VENDOR!" 2^>nul ^| sort /r') do rd "%%d" 2>nul
    )
)
echo %TARGET%: !SHIPPED! caches, dropped !PRUNED! preset JSONs
exit /b 0

:find_tool
set "TOOL="
for /f "delims=" %%f in ('dir /s /b /o-d "%BUILD_DIR%\generate_system_cache.exe" 2^>nul') do (
    if not defined TOOL set "TOOL=%%f"
)
exit /b 0
