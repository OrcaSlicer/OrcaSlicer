@echo off
REM Build OrcaSlicer, then stop any running instance and launch the new one.
REM
REM Usage:
REM   build_win_and_launch.bat [build_win.bat options] [--no-kill]
REM
REM Without options this runs `build_win.bat -s` (deps are built first when the
REM default tree is missing). Options are passed through to build_win.bat, e.g.
REM   build_win_and_launch.bat -l -x
REM   build_win_and_launch.bat --config debug
REM `--no-kill` skips stopping a running OrcaSlicer before relaunching.
REM
REM The freshly built binary is launched from the default release tree. VS
REM generator puts it in a per-config subdir (src\Release), Ninja single-config
REM puts it straight in src\; scan the candidates and use the newest.
REM
REM The launch detaches the app (start "" with its output sent to NUL) so the
REM script returns immediately even when stdout is piped; otherwise the running
REM app keeps the pipe open and the caller waits until OrcaSlicer exits.

setlocal enableDelayedExpansion

REM Console colors, empty under ORCA_NO_COLOR / NO_COLOR so piped output and
REM test harnesses stay plain.
set "RED="
set "GREEN="
set "YELLOW="
set "CYAN="
set "BOLD="
set "NC="
REM The ESC byte is captured via a nested cmd so the file stays free of raw control bytes.
for /f %%E in ('echo prompt $E ^| cmd') do set "ESC=%%E"
if not defined ORCA_NO_COLOR if not defined NO_COLOR (
    set "RED=!ESC![91m"
    set "GREEN=!ESC![92m"
    set "YELLOW=!ESC![93m"
    set "CYAN=!ESC![96m"
    set "BOLD=!ESC![1m"
    set "NC=!ESC![0m"
)

REM Separate --no-kill from the options build_win.bat should receive.
set no_kill=OFF
set build_args=
set want_help=OFF
:parse_args
if "%~1" == "" goto :args_done
if /I "%~1" == "--no-kill" (
    set no_kill=ON
) else (
    if /I "%~1" == "-h" set want_help=ON
    if /I "%~1" == "--help" set want_help=ON
    set build_args=%build_args% %~1
)
shift
goto :parse_args
:args_done

REM Default to a slicer build when no options were given.
if "%build_args%" == "" set build_args= -s

echo %BOLD%== Building OrcaSlicer with: build_win.bat%build_args% ==%NC%
call build_win.bat %build_args%
if not %errorlevel% == 0 (
    echo.
    echo %RED%Build failed; not relaunching OrcaSlicer.%NC%
    exit /b %errorlevel%
)

REM Help was just passed through; nothing to relaunch.
if "%want_help%" == "ON" exit /b 0

REM Locate the freshly built binary. The VS and Ninja Multi-Config generators
REM place it under src\<config>\orca-slicer.exe, single-config Ninja under
REM src\orca-slicer.exe. Probe the usual spots and pick the first that exists.
set "app="
for %%c in (Release RelWithDebInfo Debug) do (
    if not defined app if exist "build\src\%%c\orca-slicer.exe" set "app=build\src\%%c\orca-slicer.exe"
)
if not defined app if exist "build\src\orca-slicer.exe" set "app=build\src\orca-slicer.exe"
if not defined app if exist "build\OrcaSlicer\orca-slicer.exe" set "app=build\OrcaSlicer\orca-slicer.exe"
if not defined app (
    echo %RED%Build finished, but no executable found under build\. Launch it manually.%NC%
    exit /b 1
)

if "%no_kill%" == "OFF" (
    echo %YELLOW%Stopping any running OrcaSlicer...%NC%
    taskkill /F /IM orca-slicer.exe >nul 2>nul
)

echo %GREEN%Launching %app%%NC%
start "" "%app%" >nul 2>nul
exit /b 0
