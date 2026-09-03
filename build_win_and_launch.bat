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
REM The freshly built binary is launched from the default release tree
REM (build\src\Release\orca-slicer.exe); a build with a non-default config or
REM arch lands elsewhere, so launch it manually in that case.

setlocal

REM Separate --no-kill from the options build_win.bat should receive.
set no_kill=OFF
set build_args=
:parse_args
if "%~1" == "" goto :args_done
if /I "%~1" == "--no-kill" (
    set no_kill=ON
) else (
    set build_args=%build_args% %~1
)
shift
goto :parse_args
:args_done

REM Default to a slicer build when no options were given.
if "%build_args%" == "" set build_args=-s

echo == Building OrcaSlicer with: build_win.bat%build_args% ==
call build_win.bat %build_args%
if not %errorlevel% == 0 (
    echo.
    echo Build failed; not relaunching OrcaSlicer.
    exit /b %errorlevel%
)

set "app=build\src\Release\orca-slicer.exe"
if not exist "%app%" (
    echo Build finished, but no executable at %app%. Launch it manually.
    exit /b 1
)

if "%no_kill%" == "OFF" (
    echo Stopping any running OrcaSlicer...
    taskkill /F /IM orca-slicer.exe >nul 2>nul
)

echo Launching %app%
start "" "%app%"
exit /b 0
