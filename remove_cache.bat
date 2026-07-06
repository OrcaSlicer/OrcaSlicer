@echo off
setlocal

:: Removes all OrcaSlicer cache files so the next launch rebuilds from scratch.
::
:: Clears:
::   resources/profiles/*.cache       bundled per-vendor binary caches
::   %APPDATA%\OrcaSlicer\system\*.cache  user per-vendor binary caches
::   %APPDATA%\OrcaSlicer\guide_profile_cache.json  guide JSON cache

set REPO=%~dp0
set PROFILES=%REPO%resources\profiles
set USER_SYSTEM=%APPDATA%\OrcaSlicer\system
set GUIDE_CACHE=%APPDATA%\OrcaSlicer\guide_profile_cache.json

echo Removing OrcaSlicer cache files...
echo.

:: Bundled caches
set FOUND=0
for %%F in ("%PROFILES%\*.cache") do (
    del "%%F"
    echo   Deleted: %%~nxF  ^(bundled^)
    set FOUND=1
)
if "%FOUND%"=="0" echo   No bundled .cache files found in resources\profiles\

echo.

:: User per-vendor caches
set FOUND=0
for %%F in ("%USER_SYSTEM%\*.cache") do (
    del "%%F"
    echo   Deleted: %%~nxF  ^(user^)
    set FOUND=1
)
if "%FOUND%"=="0" echo   No user .cache files found in %USER_SYSTEM%\

echo.

:: Guide JSON cache
if exist "%GUIDE_CACHE%" (
    del "%GUIDE_CACHE%"
    echo   Deleted: guide_profile_cache.json
) else (
    echo   No guide_profile_cache.json found
)

echo.
echo Done. Next launch will rebuild all caches from JSON.
endlocal
