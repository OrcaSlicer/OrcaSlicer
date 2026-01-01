@REM OrcaSlicer debug build script for Windows (VS2022)
@echo off
set WP=%CD%

@REM This script is a thin wrapper around build_release_vs2022.bat forcing the Debug configuration.
@REM Usage examples:
@REM   build_debug_vs2022.bat          -> deps + slicer Debug build
@REM   build_debug_vs2022.bat deps     -> only deps Debug build
@REM   build_debug_vs2022.bat slicer   -> only slicer Debug build

call "%WP%\build_release_vs2022.bat" debug %*

exit /b %ERRORLEVEL%
