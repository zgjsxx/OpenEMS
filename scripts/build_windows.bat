@echo off
REM Build OpenEMS on Windows (Release mode with Visual Studio)
REM Usage: scripts\build_windows.bat [--debug] [--clean]

setlocal enabledelayedexpansion

set "PROJECT_ROOT=%~dp0.."
set "BUILD_DIR=%PROJECT_ROOT%\build"

set "BUILD_TYPE=Release"
set "CLEAN=0"

for %%a in (%*) do (
    if "%%a"=="--debug" set "BUILD_TYPE=Debug"
    if "%%a"=="--clean" set "CLEAN=1"
)

if %CLEAN%==1 (
    echo Cleaning build directory: %BUILD_DIR%
    if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
)

echo === OpenEMS Windows Build (type=%BUILD_TYPE%) ===

cmake -S "%PROJECT_ROOT%" -B "%BUILD_DIR%" -DCMAKE_BUILD_TYPE=%BUILD_TYPE%

cmake --build "%BUILD_DIR%" --config %BUILD_TYPE%

echo === Build complete ===
endlocal