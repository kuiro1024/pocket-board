@echo off
setlocal

cd /d "%~dp0"

rem Usage:
rem   build-pocketboard-setup.bat            ... ask for version (Enter = keep current)
rem   build-pocketboard-setup.bat 1.3.0      ... use 1.3.0

set "PB_VERSION=%~1"
set "PB_CURRENT="
for /f "tokens=4 delims=( " %%v in ('findstr /r /c:"^project(PocketBoard VERSION" CMakeLists.txt') do set "PB_CURRENT=%%v"

if "%PB_VERSION%"=="" (
    echo Current version: %PB_CURRENT%
    set /p "PB_VERSION=New version (x.y.z, Enter = keep %PB_CURRENT%): "
)
if "%PB_VERSION%"=="" set "PB_VERSION=%PB_CURRENT%"

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\build-setup-from-scratch.ps1" -Version "%PB_VERSION%"
if errorlevel 1 (
    echo.
    echo PocketBoard setup build failed.
    pause
    exit /b 1
)

echo.
echo PocketBoard %PB_VERSION% setup build completed.
echo Output: %~dp0dist\installer\PocketBoardSetup.exe
pause
