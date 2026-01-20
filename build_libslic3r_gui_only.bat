@echo off
setlocal

set VSDEVCMD=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat
if not exist "%VSDEVCMD%" (
  echo VS developer command script not found: "%VSDEVCMD%"
  exit /b 1
)

call "%VSDEVCMD%" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b %errorlevel%

cd /d "C:\Users\tamus\projects\3d\slicers\OrcaSlicer_GaussianSplat\build-dbg"
if errorlevel 1 exit /b %errorlevel%

cmake --build . --target libslic3r_gui --config RelWithDebInfo
exit /b %errorlevel%
