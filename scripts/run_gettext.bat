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
set "filtered_list=%TEMP%\orca_gettext_filtered_%RANDOM%_%RANDOM%.txt"
set "missing_list=%TEMP%\orca_gettext_missing_%RANDOM%_%RANDOM%.txt"

if %FULL_MODE%==1 (
    call :prepareGettextList "%list_file%" "%filtered_list%" "%missing_list%"
    .\tools\xgettext.exe --keyword=L --keyword=_L --keyword=_u8L --keyword=L_CONTEXT:1,2c --keyword=_L_PLURAL:1,2 --add-comments=TRN --from-code=UTF-8 --no-location --debug --boost -f "%filtered_list%" -o ./localization/i18n/OrcaSlicer.pot
    python scripts/HintsToPot.py ./resources ./localization/i18n
)
REM Print the current directory
echo %cd%
set "pot_file=./localization/i18n/OrcaSlicer.pot"

REM Run the script for each .po file
for /r "./localization/i18n/" %%f in (*.po) do (
    call :processFile "%%f"
)
call :reportMissing "%missing_list%"

if exist "%filtered_list%" del "%filtered_list%"
if exist "%missing_list%" del "%missing_list%"

endlocal
goto :eof

:prepareGettextList
    set "input_list=%~1"
    set "filtered=%~2"
    set "missing=%~3"
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
        ) else (
            >> "%missing%" echo(!entry!
        )
    )
goto :eof

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
goto :eof

:processFile
    set "file=%~1"
    set "dir=%~dp1"
    set "name=%~n1"
    set "lang=%name:OrcaSlicer_=%"
    if %FULL_MODE%==1 (
        .\tools\msgmerge.exe -N -o "%file%" "%file%" "%pot_file%"
    )
    if not exist "./resources/i18n/%lang%" mkdir "./resources/i18n/%lang%"
    .\tools\msgfmt.exe --check-format -o "./resources/i18n/%lang%/OrcaSlicer.mo" "%file%"
goto :eof
