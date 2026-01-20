@echo off
setlocal

set "APP_DIR=C:\Users\tamus\projects\3d\slicers\OrcaSlicer_GaussianSplat\build-dbg\OrcaSlicer"
set "VSDEVCMD=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"

if not exist "%APP_DIR%\orca-slicer.exe" (
  echo ERROR: orca-slicer.exe not found in "%APP_DIR%"
  exit /b 2
)

if exist "%VSDEVCMD%" (
  call "%VSDEVCMD%" -arch=x64 -host_arch=x64 >nul
)

cd /d "%APP_DIR%" || exit /b 3

echo == Running ==
orca-slicer.exe 1>stdout.txt 2>stderr.txt
echo EXITCODE:%ERRORLEVEL%

echo.
echo == stderr ==
type stderr.txt

echo.
echo == stdout ==
type stdout.txt

echo.
echo == Present DLLs (top-level) ==
dir /b *.dll

echo.
if exist "%APP_DIR%\OrcaSlicer.dll" (
  where dumpbin >nul 2>&1
  if errorlevel 1 (
    echo dumpbin not found in PATH (VS tools not available).
    echo Install VS C++ tools or run from a Developer Command Prompt.
  ) else (
    echo == dumpbin /dependents OrcaSlicer.dll ==
    dumpbin /dependents OrcaSlicer.dll
  )
)

exit /b 0
