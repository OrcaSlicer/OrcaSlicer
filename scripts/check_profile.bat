@echo off
REM Runs check_profile.ps1, the Windows twin of check_profile.sh, from cmd.
REM Arguments are passed straight through, so anything the .ps1 takes works here:
REM     scripts\check_profile.bat -Vendor Elegoo validate_custom
REM -ExecutionPolicy Bypass is needed because a Windows client defaults to Restricted,
REM which refuses to run a checked-out .ps1 at all.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0check_profile.ps1" %*
exit /b %ERRORLEVEL%
