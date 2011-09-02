@echo off
REM OrcaSlicer gettext
REM Created by SoftFever on 27/5/23.

REM Check for --full argument
set FULL_MODE=0
for %%a in (%*) do (
    if "%%a"=="--full" set FULL_MODE=1
)

if %FULL_MODE%==1 (
    .\tools\xgettext.exe --keyword=L --keyword=_L --keyword=_u8L --keyword=L_CONTEXT:1,2c --keyword=_L_PLURAL:1,2 --add-comments=TRN --from-code=UTF-8 --no-location --debug --boost -f ./localization/i18n/list.txt -o ./localization/i18n/OrcaSlicer.pot
    python scripts/HintsToPot.py ./resources ./localization/i18n
)
REM Print the current directory
echo %cd%
set pot_file="./localization/i18n/OrcaSlicer.pot"

REM Run the script for each .po file
for /r "./localization/i18n/" %%f in (*.po) do (
    call :processFile "%%f"
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
