@echo off
REM OrcaSlicer gettext
REM Created by SoftFever on 27/5/23.
setlocal EnableExtensions EnableDelayedExpansion

REM Check for --full argument
set FULL_MODE=0
for %%a in (%*) do (
    if "%%a"=="--full" set FULL_MODE=1
)

set "list_file=./localization/i18n/list.txt"
set "pot_file=./localization/i18n/OrcaSlicer.pot"
set "filtered_list=%TEMP%\orca_gettext_filtered_%RANDOM%_%RANDOM%.txt"
set "missing_list=%TEMP%\orca_gettext_missing_%RANDOM%_%RANDOM%.txt"
set "generated_root=%TEMP%\orca_gettext_generated_%RANDOM%_%RANDOM%"
set "generated_i18n=%generated_root%\i18n"
set "generated_pot=%generated_i18n%\OrcaSlicer.pot"
set "has_sources=0"
set "script_exit_code=0"

if %FULL_MODE%==1 (
    call :prepareGettextList "%list_file%" "%filtered_list%" "%missing_list%"
    if "!has_sources!"=="1" (
        if not exist "%generated_i18n%" mkdir "%generated_i18n%"
        .\tools\xgettext.exe --keyword=L --keyword=_L --keyword=_u8L --keyword=L_CONTEXT:1,2c --keyword=_L_PLURAL:1,2 --add-comments=TRN --from-code=UTF-8 --no-location --debug --boost -f "%filtered_list%" -o "%generated_pot%"
        if errorlevel 1 (
            set "script_exit_code=1"
        ) else (
            python scripts/HintsToPot.py ./resources "%generated_i18n%"
            if errorlevel 1 (
                set "script_exit_code=1"
            ) else (
                call :replaceIfMeaningful "%pot_file%" "%generated_pot%"
                if errorlevel 1 set "script_exit_code=1"
            )
        )
    ) else (
        echo No existing source files found in %list_file%; skipping template regeneration.
    )
)

if not "!script_exit_code!"=="0" goto :cleanup

REM Print the current directory
echo %cd%

REM Run the script for each .po file
for /r "./localization/i18n/" %%f in (*.po) do (
    call :processFile "%%f"
    if errorlevel 1 set "script_exit_code=1"
)

:cleanup
call :reportMissing "%missing_list%"

if exist "%filtered_list%" del "%filtered_list%"
if exist "%missing_list%" del "%missing_list%"
if exist "%generated_root%" rd /s /q "%generated_root%"

endlocal & exit /b %script_exit_code%

:prepareGettextList
    set "input_list=%~1"
    set "filtered=%~2"
    set "missing=%~3"
    set "has_sources=0"
    type nul > "%filtered%"
    type nul > "%missing%"
    for /f "usebackq delims=" %%l in ("%input_list%") do (
        set "entry=%%l"
        if "!entry!"=="" (
            >> "%filtered%" echo.
        ) else if "!entry:~0,1!"=="#" (
            >> "%filtered%" echo(!entry!
        ) else if exist "!entry!" (
            >> "%filtered%" echo(!entry!
            set "has_sources=1"
        ) else (
            >> "%missing%" echo(!entry!
        )
    )
exit /b 0

:reportMissing
    set "missing=%~1"
    if exist "%missing%" (
        for %%s in ("%missing%") do set "missing_size=%%~zs"
        if not "!missing_size!"=="0" (
            echo.
            echo Skipped missing source files listed in %list_file%:
            for /f "usebackq delims=" %%m in ("%missing%") do echo   - %%m
        )
    )
exit /b 0

:replaceIfMeaningful
    set "target=%~1"
    set "candidate=%~2"
    if exist "%target%" (
        call :filesEqualIgnoringPotDate "%target%" "%candidate%"
        if not errorlevel 1 (
            del "%candidate%"
            exit /b 0
        )
    )
    move /Y "%candidate%" "%target%" > nul
    if errorlevel 1 exit /b 1
exit /b 0

:filesEqualIgnoringPotDate
    set "left=%~1"
    set "right=%~2"
    if not exist "%left%" exit /b 1
    if not exist "%right%" exit /b 1
    python -c "import re,sys,pathlib; pattern=re.compile(r'^\"POT-Creation-Date: .*(?:\\r?\\n)?', re.M); normalize=lambda p: pattern.sub('', pathlib.Path(p).read_text(encoding='utf-8-sig')); sys.exit(0 if normalize(sys.argv[1])==normalize(sys.argv[2]) else 1)" "%left%" "%right%"
exit /b %errorlevel%

:processFile
    set "file=%~1"
    set "name=%~n1"
    set "lang=%name:OrcaSlicer_=%"
    if %FULL_MODE%==1 if exist "%pot_file%" (
        set "merged_file=%TEMP%\orca_gettext_merged_%RANDOM%_%RANDOM%.po"
        .\tools\msgmerge.exe -N -o "!merged_file!" "%file%" "%pot_file%"
        if errorlevel 1 (
            if exist "!merged_file!" del "!merged_file!"
            echo Error encountered with msgmerge command for language !lang!.
            exit /b 1
        )
        call :replaceIfMeaningful "%file%" "!merged_file!"
        if errorlevel 1 exit /b 1
    )
    if not exist "./resources/i18n/!lang!" mkdir "./resources/i18n/!lang!"
    .\tools\msgfmt.exe --check-format -o "./resources/i18n/!lang!/OrcaSlicer.mo" "%file%"
    if errorlevel 1 (
        echo Error encountered with msgfmt command for language !lang!.
        exit /b 1
    )
exit /b 0
