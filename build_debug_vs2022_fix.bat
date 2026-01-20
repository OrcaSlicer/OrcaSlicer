@echo off
REM Wrapper to build OrcaSlicer in Debug with VS2022 Community and CMake even if they are not on PATH.
REM It sets up the VS developer environment and then delegates to build_debug_vs2022.bat.

setlocal ENABLEEXTENSIONS ENABLEDELAYEDEXPANSION

REM --- Load optional local build configuration (generator, parallelism overrides) ---
set "SCRIPT_DIR=%~dp0"
set "ORCA_CONFIG_FILE=%SCRIPT_DIR%.config"
if exist "%ORCA_CONFIG_FILE%" (
    for /f "usebackq tokens=1,2 delims== " %%A in ("%ORCA_CONFIG_FILE%") do (
        if /I "%%A"=="GENERATOR" (
            if /I "%%B"=="NINJA" (
                set "ORCA_FORCE_NINJA=1"
                set "ORCA_FORCE_NMAKE="
            ) else if /I "%%B"=="NMAKE" (
                set "ORCA_FORCE_NINJA="
                set "ORCA_FORCE_NMAKE=1"
            )
        ) else if /I "%%A"=="PARALLEL" (
            set "CMAKE_BUILD_PARALLEL_LEVEL=%%B"
        ) else if /I "%%A"=="BUILD_TYPE" (
            set "ORCA_DEFAULT_BUILD_TYPE=%%B"
        )
    )
)

REM --- Configure Visual Studio 2022 Community environment via VsDevCmd ---
set "VS_DEV_CMD=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"

if not exist "%VS_DEV_CMD%" (
    echo [ERROR] VsDevCmd.bat not found at "%VS_DEV_CMD%".
    echo        Please verify your Visual Studio 2022 Community installation path.
    exit /b 1
)

call "%VS_DEV_CMD%" -arch=x64
if errorlevel 1 (
    echo [ERROR] Failed to initialize Visual Studio 2022 x64 build environment via VsDevCmd.
    exit /b 1
)

REM --- Ensure a consistent Windows SDK target for all translation units ---
REM Some dependencies (e.g. Boost.Asio) emit a warning if _WIN32_WINNT is not defined.
REM We target Windows 7+ here (0x0601) by default.
set "CL=/D_WIN32_WINNT=0x0601 /DWINVER=0x0601 %CL%"

REM --- Configure CMake build parallelism based on CPU logical cores ---
if not defined CMAKE_BUILD_PARALLEL_LEVEL (
    set "CMAKE_BUILD_PARALLEL_LEVEL=%NUMBER_OF_PROCESSORS%"
)

REM --- Detect total pagefile size (swap) and decide whether PCH is allowed ---
set "ORCA_PCH=ON"
set "ORCA_PAGEFILE_MB="
for /f "usebackq tokens=1" %%S in (`powershell -NoProfile -Command "try { $s = (Get-CimInstance Win32_PageFileUsage | Measure-Object -Property AllocatedBaseSize -Sum).Sum; if (-not $s) { $s = 0 }; Write-Output $s } catch { Write-Output 0 }"`) do (
    set "ORCA_PAGEFILE_MB=%%S"
)
if not defined ORCA_PAGEFILE_MB set "ORCA_PAGEFILE_MB=0"

REM Threshold: ~90 GB of pagefile (~90000 MB)
set "ORCA_PCH=OFF"
for /f "tokens=*" %%V in ("%ORCA_PAGEFILE_MB%") do set "_TMP_PAGE_MB=%%V"
for /f "tokens=1 delims=." %%V in ("%_TMP_PAGE_MB%") do set "_TMP_PAGE_MB_INT=%%V"
if not "%_TMP_PAGE_MB_INT%"=="" if %_TMP_PAGE_MB_INT% GEQ 90000 set "ORCA_PCH=ON"

if "%ORCA_PCH%"=="ON" (
    echo [INFO] Detected pagefile size %_TMP_PAGE_MB_INT% MB ^(>= 90000 MB^); enabling precompiled headers.
) else (
    echo [INFO] Detected pagefile size %_TMP_PAGE_MB_INT% MB ^(< 90000 MB^); disabling precompiled headers.
)

REM --- Ensure CMake is available ---
set "CMAKE_BIN=C:\Program Files\CMake\bin"
if exist "%CMAKE_BIN%\cmake.exe" (
    set "PATH=%CMAKE_BIN%;!PATH!"
) else (
    echo [WARN] cmake.exe not found at "%CMAKE_BIN%". Assuming CMake is already on PATH.
)

REM --- Ensure C:\Programs\bin is on PATH if it exists (for ninja.exe, etc.) ---
if exist "C:\Programs\bin" (
    echo !PATH! | find /I "C:\Programs\bin" >nul 2>&1
    if errorlevel 1 (
        set "PATH=C:\Programs\bin;!PATH!"
        echo [INFO] Added C:\Programs\bin to PATH.
    )
)

REM --- Prefer a Windows Perl (for OpenSSL) if available ---
set "ORCA_PERL_EXE="
if exist "C:\Strawberry\perl\bin\perl.exe" set "ORCA_PERL_EXE=C:\Strawberry\perl\bin\perl.exe"
if exist "C:\Programs\Strawberry\perl\bin\perl.exe" set "ORCA_PERL_EXE=C:\Programs\Strawberry\perl\bin\perl.exe"
if exist "C:\Program Files\Strawberry\perl\bin\perl.exe" set "ORCA_PERL_EXE=C:\Program Files\Strawberry\perl\bin\perl.exe"
if defined ORCA_PERL_EXE (
    for %%D in ("%ORCA_PERL_EXE%") do set "ORCA_PERL_DIR=%%~dpD"
    set "PATH=%ORCA_PERL_DIR%;!PATH!"
    echo [INFO] Using Perl from "%ORCA_PERL_EXE%" for OpenSSL build.
) else (
    echo [WARN] No Strawberry Perl found at standard locations.
    echo [WARN] If OpenSSL build fails with a Perl path error, install Strawberry Perl from https://strawberryperl.com/.
)

REM --- Prefer Ninja generator if ninja.exe is available; otherwise fall back to NMake ---
REM If ORCA_FORCE_NINJA / ORCA_FORCE_NMAKE are already set (e.g. by .config),
REM respect them and skip auto-detection.
if not defined ORCA_FORCE_NINJA if not defined ORCA_FORCE_NMAKE (
    set "NINJA_PATH="
    for /f "usebackq delims=" %%P in (`where ninja.exe 2^>nul`) do (
        if not defined NINJA_PATH set "NINJA_PATH=%%P"
    )
    if defined NINJA_PATH (
        echo [INFO] Ninja detected at !NINJA_PATH!; using Ninja generator.
        set "ORCA_FORCE_NINJA=1"
    ) else (
        echo [INFO] Ninja not detected; using NMake Makefiles generator.
        set "ORCA_FORCE_NMAKE=1"
    )
)

REM --- Run the new VS2022 debug build script from the repo root ---
pushd "%SCRIPT_DIR%" >nul 2>&1

REM Decide build type:
REM - If user passes debug/debuginfo at CLI, that wins.
REM - Otherwise use BUILD_TYPE from .config (default Release if missing).
set "ORCA_REQUESTED_BUILD_TYPE="
set "ORCA_HAS_CLI_OVERRIDE=0"

for %%X in (%*) do (
    if /I "%%X"=="debug" (
        set "ORCA_REQUESTED_BUILD_TYPE=Debug"
        set "ORCA_HAS_CLI_OVERRIDE=1"
    ) else if /I "%%X"=="debuginfo" (
        set "ORCA_REQUESTED_BUILD_TYPE=RelWithDebInfo"
        set "ORCA_HAS_CLI_OVERRIDE=1"
    )
)

if "%ORCA_HAS_CLI_OVERRIDE%"=="0" (
    if defined ORCA_DEFAULT_BUILD_TYPE (
        set "ORCA_REQUESTED_BUILD_TYPE=%ORCA_DEFAULT_BUILD_TYPE%"
    ) else (
        set "ORCA_REQUESTED_BUILD_TYPE=Release"
    )
)

REM If we are using the NMake generator, keep a configured single-config build dir
REM consistent with the requested type. (Debug is mapped later by build_release_vs2022.bat
REM when using NMake, but configuring RelWithDebInfo here keeps the cache consistent.)
if defined ORCA_FORCE_NMAKE (
    if /I "%ORCA_REQUESTED_BUILD_TYPE%"=="Debug" (
        cmake -S . -B build-dbg -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=RelWithDebInfo
    ) else if /I "%ORCA_REQUESTED_BUILD_TYPE%"=="RelWithDebInfo" (
        cmake -S . -B build-dbginfo -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=RelWithDebInfo
    ) else (
        cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
    )
)

REM Route to the appropriate underlying script based on requested build type.
if /I "%ORCA_REQUESTED_BUILD_TYPE%"=="Release" (
    call "%SCRIPT_DIR%build_release_vs2022.bat" %*
) else if /I "%ORCA_REQUESTED_BUILD_TYPE%"=="RelWithDebInfo" (
    call "%SCRIPT_DIR%build_release_vs2022.bat" debuginfo %*
) else (
    call "%SCRIPT_DIR%build_debug_vs2022.bat" %*
)
set "EXITCODE=%ERRORLEVEL%"

popd >nul 2>&1

exit /b %EXITCODE%
