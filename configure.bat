@echo off
REM Configure helper for OrcaSlicer Windows build tools.
REM - Downloads Ninja, Python, and Strawberry Perl installers
REM - Copies ninja.exe to C:\Programs\bin
REM - Starts the installers so you can complete them graphically.

setlocal ENABLEDELAYEDEXPANSION

REM --- Configuration: adjust versions/URLs here if needed ---
set "DOWNLOAD_ROOT=%TEMP%\orcaslicer-tooling"
set "NINJA_URL=https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-win.zip"
set "NINJA_ZIP=%DOWNLOAD_ROOT%\ninja-win.zip"
set "PYTHON_URL=https://www.python.org/ftp/python/3.12.6/python-3.12.6-amd64.exe"
set "PYTHON_EXE=%DOWNLOAD_ROOT%\python-3.12.6-amd64.exe"
set "PERL_URL=https://strawberryperl.com/download/5.38.2.2/strawberry-perl-5.38.2.2-64bit.msi"
set "PERL_MSI=%DOWNLOAD_ROOT%\strawberry-perl-5.38.2.2-64bit.msi"

REM --- Detect existing tools so we can skip reinstalling them ---
set "HAVE_NINJA="
where ninja >nul 2>&1 && set "HAVE_NINJA=1"
if not defined HAVE_NINJA if exist "C:\Programs\bin\ninja.exe" set "HAVE_NINJA=1"

set "HAVE_PYTHON="
where py >nul 2>&1 && set "HAVE_PYTHON=1"
if not defined HAVE_PYTHON where python >nul 2>&1 && set "HAVE_PYTHON=1"

set "HAVE_PERL="
if exist "C:\Strawberry\perl\bin\perl.exe" set "HAVE_PERL=1"
if not defined HAVE_PERL if exist "C:\Programs\Strawberry\perl\bin\perl.exe" set "HAVE_PERL=1"
if not defined HAVE_PERL if exist "C:\Program Files\Strawberry\perl\bin\perl.exe" set "HAVE_PERL=1"
if not defined HAVE_PERL where perl >nul 2>&1 && set "HAVE_PERL=1"

REM --- Create download directory ---
if not exist "%DOWNLOAD_ROOT%" (
    mkdir "%DOWNLOAD_ROOT%" || (
        echo [ERROR] Failed to create download directory "%DOWNLOAD_ROOT%".
        exit /b 1
    )
)

echo Downloads will be stored in: "%DOWNLOAD_ROOT%"

echo.
echo === Step 1: Downloading Ninja ===
if defined HAVE_NINJA (
    echo [INFO] Ninja already detected on this system; skipping Ninja download/install.
    goto :PYTHON
)
if not exist "%NINJA_ZIP%" (
    echo [INFO] Downloading Ninja from:
    echo        %NINJA_URL%
    powershell -Command "try { Invoke-WebRequest -UseBasicParsing -Uri '%NINJA_URL%' -OutFile '%NINJA_ZIP%' } catch { Write-Error $_; exit 1 }" || (
        echo [ERROR] Failed to download Ninja.
        goto :AFTER_NINJA
    )
) else (
    echo [INFO] Ninja archive already exists, skipping download.
)

:AFTER_NINJA
echo.
echo === Step 2: Extracting Ninja and copying to C:\Programs\bin ===
if exist "%NINJA_ZIP%" (
    set "NINJA_EXTRACT=%DOWNLOAD_ROOT%\ninja-extract"
    if exist "%NINJA_EXTRACT%" rmdir /s /q "%NINJA_EXTRACT%"
    mkdir "%NINJA_EXTRACT%" || (
        echo [WARN] Could not create "%NINJA_EXTRACT%"; skipping Ninja extraction.
        goto :PYTHON
    )
    powershell -Command "Expand-Archive -Path '%NINJA_ZIP%' -DestinationPath '%NINJA_EXTRACT%' -Force" || (
        echo [WARN] Failed to extract Ninja; you may need to unpack it manually.
        goto :PYTHON
    )
    if not exist "%NINJA_EXTRACT%\ninja.exe" (
        echo [WARN] ninja.exe not found after extraction; archive layout may have changed.
        goto :PYTHON
    )
    set "NINJA_BIN=C:\Programs\bin"
    if not exist "%NINJA_BIN%" mkdir "%NINJA_BIN%" >nul 2>&1
    copy /Y "%NINJA_EXTRACT%\ninja.exe" "%NINJA_BIN%\ninja.exe" >nul 2>&1 || (
        echo [WARN] Failed to copy ninja.exe to "%NINJA_BIN%".
        echo        Try running this script in an elevated (Administrator) command prompt and/or copy it manually.
        goto :PYTHON
    )
    echo [INFO] Installed ninja.exe to "%NINJA_BIN%".
) else (
    echo [WARN] Ninja zip not present; skipping Ninja install.
)

:PYTHON
echo.
echo === Step 3: Downloading Python installer ===
if defined HAVE_PYTHON (
    echo [INFO] Python already detected on this system; skipping Python download/install.
    goto :PERL
)
if not exist "%PYTHON_EXE%" (
    echo [INFO] Downloading Python from:
    echo        %PYTHON_URL%
    powershell -Command "try { Invoke-WebRequest -UseBasicParsing -Uri '%PYTHON_URL%' -OutFile '%PYTHON_EXE%' } catch { Write-Error $_; exit 1 }" || (
        echo [ERROR] Failed to download Python installer.
        goto :PERL
    )
) else (
    echo [INFO] Python installer already exists, skipping download.
)

echo.
echo === Step 4: Starting Python installer (GUI) ===
if exist "%PYTHON_EXE%" (
    echo [INFO] Launching Python installer; follow the on-screen instructions.
    start "Python Installer" "%PYTHON_EXE%"
) else (
    echo [WARN] Python installer not found; cannot start it.
)

:PERL
echo.
echo === Step 5: Downloading Strawberry Perl MSI ===
if defined HAVE_PERL (
    echo [INFO] Perl already detected on this system; skipping Strawberry Perl download/install.
    goto :VCPKG
)
if not exist "%PERL_MSI%" (
    echo [INFO] Downloading Strawberry Perl from:
    echo        %PERL_URL%
    powershell -Command "try { Invoke-WebRequest -UseBasicParsing -Uri '%PERL_URL%' -OutFile '%PERL_MSI%' } catch { Write-Error $_; exit 1 }" || (
        echo [ERROR] Failed to download Strawberry Perl MSI.
        goto :DONE
    )
) else (
    echo [INFO] Strawberry Perl MSI already exists, skipping download.
)

echo.
echo === Step 6: Starting Strawberry Perl MSI (GUI) ===
if exist "%PERL_MSI%" (
    echo [INFO] Launching Strawberry Perl installer; follow the on-screen instructions.
    start "Strawberry Perl Installer" msiexec /i "%PERL_MSI%"
) else (
    echo [WARN] Strawberry Perl MSI not found; cannot start it.
)

echo.
:VCPKG
echo === Step 7: Ensure expat and OpenSSL are installed via vcpkg ===
where vcpkg >nul 2>&1
if errorlevel 1 (
    echo [WARN] vcpkg not found on PATH; skipping 'vcpkg install expat'.
) else (
    echo [INFO] Running 'vcpkg install expat openssl'...
    vcpkg install expat openssl
    if errorlevel 1 (
        echo [WARN] 'vcpkg install expat openssl' failed. You may need to run it manually.
    ) else (
        echo [INFO] 'vcpkg install expat openssl' completed successfully.
    )
    echo [INFO] Running 'vcpkg integrate install'...
    vcpkg integrate install
    if errorlevel 1 (
        echo [WARN] 'vcpkg integrate install' failed. You may need to run it manually.
    ) else (
        echo [INFO] 'vcpkg integrate install' completed successfully.
    )
)

echo.
:GEN_CONFIG
echo === Step 8: Configure default generator and parallelism ===
set "CPU_COUNT=%NUMBER_OF_PROCESSORS%"
if "%CPU_COUNT%"=="" set "CPU_COUNT=1"

echo.
echo Select default CMake generator for OrcaSlicer builds:
echo   [1] Ninja (requires ninja.exe on PATH)
echo   [2] NMake Makefiles
set "GEN_CHOICE="
set /p GEN_CHOICE="Enter choice [1/2, default=1]: "
if "%GEN_CHOICE%"=="" set "GEN_CHOICE=1"

set "GEN_KIND=NINJA"
if "%GEN_CHOICE%"=="2" set "GEN_KIND=NMAKE"

set "PARALLEL_LEVEL="
if /I "%GEN_KIND%"=="NMAKE" (
    echo.
    echo NMake parallelism:
    set "PARALLEL_DEFAULT=%CPU_COUNT%"
    set /p PARALLEL_LEVEL="Enter parallel jobs [default=%PARALLEL_DEFAULT%]: "
    if "%PARALLEL_LEVEL%"=="" set "PARALLEL_LEVEL=%PARALLEL_DEFAULT%"
)

set "CONFIG_CFG=%~dp0.config"
if /I "%GEN_KIND%"=="NINJA" (
    > "%CONFIG_CFG%" (
        echo # Auto-generated local build config for OrcaSlicer.
        echo # Delete this file and re-run configure.bat to change settings.
        echo GENERATOR=NINJA
    )
) else (
    > "%CONFIG_CFG%" (
        echo # Auto-generated local build config for OrcaSlicer.
        echo # Delete this file and re-run configure.bat to change settings.
        echo GENERATOR=NMAKE
        echo PARALLEL=%PARALLEL_LEVEL%
    )
)

echo.
echo [INFO] Saved build configuration to "%CONFIG_CFG%".
echo        build_release_vs2022_fix.bat will use this generator/parallelism.

:DONE
echo.
echo Configuration helper finished.
echo - Ninja:   copied to C:\Programs\bin if extraction/copy succeeded.
echo - Python:  installer started if download succeeded.
echo - Perl:    Strawberry Perl MSI started if download succeeded.
echo - vcpkg:   'vcpkg install expat openssl' and 'vcpkg integrate install' run if vcpkg was found on PATH.

echo.
echo You may now close this window once installations are complete.

endlocal
exit /b 0
