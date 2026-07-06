@echo off
setlocal

:: Usage: gen_cache.bat [Release|Debug|RelWithDebInfo]
:: Generates per-vendor .cache files into resources/profiles/
:: using generate_system_cache.exe from the local build tree.

set BUILD_CONFIG=%~1
if "%BUILD_CONFIG%"=="" set BUILD_CONFIG=Release

set REPO=%~dp0
set BUILD_DIR=%REPO%build
set EXE=%BUILD_DIR%\src\dev-utils\%BUILD_CONFIG%\generate_system_cache.exe
set PROFILES=%REPO%resources\profiles

if not exist "%EXE%" (
    echo ERROR: %EXE% not found.
    echo Build the project first with ORCA_TOOLS=ON, config %BUILD_CONFIG%.
    exit /b 1
)

if not exist "%PROFILES%" (
    echo ERROR: profiles dir not found: %PROFILES%
    exit /b 1
)

:: DLLs live in the main Release output, not next to the tool exe
set PATH=%BUILD_DIR%\src\%BUILD_CONFIG%;%PATH%

echo Generating system presets cache...
echo   Tool   : %EXE%
echo   Profiles: %PROFILES%
echo.

"%EXE%" --path "%PROFILES%" --log_level 2
if %ERRORLEVEL% neq 0 (
    echo.
    echo ERROR: generate_system_cache failed ^(exit %ERRORLEVEL%^).
    exit /b %ERRORLEVEL%
)

echo.
echo Done. Cache files written to: %PROFILES%
dir /b "%PROFILES%\*.cache" 2>nul
endlocal
