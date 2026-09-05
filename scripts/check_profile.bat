@echo off
REM Runs check_profile.ps1, the Windows twin of check_profile.sh, from cmd.
REM -ExecutionPolicy Bypass is needed because a Windows client defaults to Restricted,
REM which refuses to run a checked-out .ps1 at all.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0check_profile.ps1" %*
exit /b %ERRORLEVEL%
