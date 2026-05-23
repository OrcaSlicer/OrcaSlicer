@REM OrcaSlicer external build script for Windows / Visual Studio 2026
@echo off
setlocal DISABLEDELAYEDEXPANSION

set "WP=%~dp0"
set "WP=%WP:~0,-1%"
for %%I in ("%WP%\..") do set "SOURCE_PARENT=%%~fI"

if "%ORCA_BUILD_ROOT%"=="" (
    set "BUILD_ROOT=%SOURCE_PARENT%\OrcaSlicer-build-vs2026"
) else (
    set "BUILD_ROOT=%ORCA_BUILD_ROOT%"
)

set debug=OFF
set debuginfo=OFF
if "%1"=="debug" set debug=ON
if "%2"=="debug" set debug=ON
if "%1"=="debuginfo" set debuginfo=ON
if "%2"=="debuginfo" set debuginfo=ON

if "%debug%"=="ON" (
    set "build_type=Debug"
    set "build_name=build-dbg"
) else (
    if "%debuginfo%"=="ON" (
        set "build_type=RelWithDebInfo"
        set "build_name=build-dbginfo"
    ) else (
        set "build_type=Release"
        set "build_name=build"
    )
)

set "DEPS_BUILD_DIR=%BUILD_ROOT%\deps-%build_name%"
set "SLICER_BUILD_DIR=%BUILD_ROOT%\%build_name%"
set "SIG_FLAG="
if defined ORCA_UPDATER_SIG_KEY set "SIG_FLAG=-DORCA_UPDATER_SIG_KEY=%ORCA_UPDATER_SIG_KEY%"

echo source dir set to %WP%
echo build root set to %BUILD_ROOT%
echo build type set to %build_type%

if not exist "%WP%\deps\CMakeLists.txt" (
    echo Missing required source directory: %WP%\deps
    echo Restore the deps directory from the original OrcaSlicer source package.
    exit /b 1
)

if not exist "%WP%\deps_src\CMakeLists.txt" (
    echo Missing required source directory: %WP%\deps_src
    echo Restore the deps_src directory from the original OrcaSlicer source package.
    exit /b 1
)

if "%1"=="slicer" goto :slicer

echo "building deps..."
if not exist "%DEPS_BUILD_DIR%" mkdir "%DEPS_BUILD_DIR%"
set CMAKE_POLICY_VERSION_MINIMUM=3.5
cmake -S "%WP%\deps" -B "%DEPS_BUILD_DIR%" -G "Visual Studio 18 2026" -A x64 -DCMAKE_BUILD_TYPE=%build_type%
if errorlevel 1 exit /b %errorlevel%
cmake --build "%DEPS_BUILD_DIR%" --config %build_type% --target deps --parallel
if errorlevel 1 exit /b %errorlevel%

if "%1"=="deps" exit /b 0

:slicer
echo "building Orca Slicer..."
if not exist "%SLICER_BUILD_DIR%" mkdir "%SLICER_BUILD_DIR%"
set CMAKE_POLICY_VERSION_MINIMUM=3.5
cmake -S "%WP%" -B "%SLICER_BUILD_DIR%" -G "Visual Studio 18 2026" -A x64 -DORCA_TOOLS=ON "-DDEP_BUILD_DIR:PATH=%DEPS_BUILD_DIR%" %SIG_FLAG% -DCMAKE_BUILD_TYPE=%build_type%
if errorlevel 1 exit /b %errorlevel%
cmake --build "%SLICER_BUILD_DIR%" --config %build_type% --target ALL_BUILD --parallel
if errorlevel 1 exit /b %errorlevel%

cd /d "%WP%"
call scripts\run_gettext.bat
if errorlevel 1 exit /b %errorlevel%
cmake --build "%SLICER_BUILD_DIR%" --target install --config %build_type%
if errorlevel 1 exit /b %errorlevel%
