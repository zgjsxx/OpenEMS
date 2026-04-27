@echo off
REM Install OpenEMS on Windows (from existing build)
REM Must run after build_windows.bat
REM Usage: scripts\install_windows.bat

setlocal

set "PROJECT_ROOT=%~dp0.."
set "BUILD_DIR=%PROJECT_ROOT%\build"
set "INSTALL_DIR=%PROJECT_ROOT%\install"

echo === OpenEMS Windows Install ===

cmake --install "%BUILD_DIR%" --prefix "%INSTALL_DIR%"

echo === Install complete: %INSTALL_DIR% ===
endlocal