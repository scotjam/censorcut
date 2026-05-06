@echo off
rem Launch CensorCut with Qt + libVLC DLLs on PATH and VLC plugin path set.
rem Sets up MSVC's runtime environment in case anything in the build needs it.
rem Double-click this file from D:\censorcut-repo\ to run the editor.

setlocal

set REPO=%~dp0
set REPO=%REPO:~0,-1%

if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
    call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
)

set LIBVLC_ROOT=%REPO%\third_party\libvlc-sdk
set PATH=D:\Qt\6.8.3\msvc2022_64\bin;%LIBVLC_ROOT%\bin;%PATH%
set VLC_PLUGIN_PATH=%LIBVLC_ROOT%\plugins

if not exist "%REPO%\build\censorcut.exe" (
    echo Build first: cmake -B build -S . then cmake --build build -j
    pause
    exit /b 1
)

start "" "%REPO%\build\censorcut.exe"
endlocal
