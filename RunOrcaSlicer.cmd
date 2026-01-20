@echo off
setlocal ENABLEEXTENSIONS

REM Runs OrcaSlicer from the build output with a correct runtime PATH.
REM This fixes startup failures like:
REM   "OrcaSlicer.dll was not loaded, error=126"
REM which usually means a dependent DLL cannot be found.
REM
REM Usage:
REM   RunOrcaSlicer.cmd
REM   RunOrcaSlicer.cmd build-dbg
REM   RunOrcaSlicer.cmd build-dbginfo
REM
REM Optional:
REM   set ORCA_COPY_DEPS=1   -> copy missing DLLs from deps bin into app dir before running

set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"

set "BUILD_DIR=%~1"
if "%BUILD_DIR%"=="" set "BUILD_DIR=build-dbg"

set "APP_DIR=%ROOT%\%BUILD_DIR%\OrcaSlicer"
set "DEPS_DIR=%ROOT%\deps\%BUILD_DIR%\OrcaSlicer_dep\usr\local"

set "APP_EXE=%APP_DIR%\orca-slicer.exe"
if not exist "%APP_EXE%" (
  REM Fallback name sometimes used by older scripts/builds
  set "APP_EXE=%APP_DIR%\OrcaSlicer.exe"
)

if not exist "%APP_EXE%" (
  echo [ERROR] Could not find OrcaSlicer executable.
  echo         Looked for:
  echo           %APP_DIR%\orca-slicer.exe
  echo           %APP_DIR%\OrcaSlicer.exe
  exit /b 2
)

if not exist "%DEPS_DIR%\bin" (
  echo [WARN] Deps bin folder not found:
  echo        %DEPS_DIR%\bin
  echo        Continuing anyway; app may fail if it relies on deps DLLs.
)

REM Prepend paths so the correct DLLs win.
set "PATH=%APP_DIR%;%DEPS_DIR%\bin;%DEPS_DIR%\bin\occt;%DEPS_DIR%\lib;%DEPS_DIR%\lib\occt;%PATH%"

if /I "%ORCA_COPY_DEPS%"=="1" (
  echo [INFO] Copying missing runtime DLLs into app dir...
  if exist "%DEPS_DIR%\bin" (
    for %%F in ("%DEPS_DIR%\bin\*.dll") do (
      if not exist "%APP_DIR%\%%~nxF" copy /y "%%~fF" "%APP_DIR%\" >nul
    )
  )
  if exist "%DEPS_DIR%\bin\occt" (
    for %%F in ("%DEPS_DIR%\bin\occt\*.dll") do (
      if not exist "%APP_DIR%\%%~nxF" copy /y "%%~fF" "%APP_DIR%\" >nul
    )
  )
)

echo [INFO] Launching: "%APP_EXE%"
pushd "%APP_DIR%" >nul
"%APP_EXE%"
set "EXITCODE=%ERRORLEVEL%"
popd >nul

if not "%EXITCODE%"=="0" (
  echo [WARN] OrcaSlicer exited with code %EXITCODE%.
)

exit /b %EXITCODE%
