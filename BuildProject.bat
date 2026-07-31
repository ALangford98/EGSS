@echo off
REM Generates a Visual Studio solution. Pass an action to override the
REM default, e.g.  BuildProject.bat vs2019
setlocal

set ACTION=%1
if "%ACTION%"=="" set ACTION=vs2022

if not exist "vendor\bin\premake\premake5.exe" (
    echo.
    echo   premake5.exe not found at vendor\bin\premake\
    echo.
    echo   It is gitignored, so a fresh clone will not have it. Download it
    echo   from https://premake.github.io/download and put premake5.exe there.
    echo.
    pause
    exit /b 1
)

call vendor\bin\premake\premake5.exe %ACTION%
pause
