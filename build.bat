@echo off
rem Configure (if needed) and build CensorCut + run all tests.
rem Double-click or run from a cmd shell at the repo root.

setlocal

set REPO=%~dp0
set REPO=%REPO:~0,-1%

rem MSVC environment for ninja + cl.exe.
if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
    call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
)

set LIBVLC_ROOT=%REPO%\third_party\libvlc-sdk
set PATH=D:\Qt\6.8.3\msvc2022_64\bin;%LIBVLC_ROOT%\bin;%PATH%

rem Configure only if the build dir doesn't exist or CMakeCache is stale.
if not exist "%REPO%\build\CMakeCache.txt" (
    echo === Configuring ===
    cmake -B "%REPO%\build" -S "%REPO%" -G Ninja ^
        -DCMAKE_BUILD_TYPE=Release ^
        -DCMAKE_PREFIX_PATH="D:/Qt/6.8.3/msvc2022_64;D:/vcpkg/installed/x64-windows"
    if errorlevel 1 goto :err
)

echo === Building ===
cmake --build "%REPO%\build" -j
if errorlevel 1 goto :err

if /I "%~1"=="--no-tests" goto :done

echo === C++ tests ===
ctest --test-dir "%REPO%\build" --output-on-failure
if errorlevel 1 goto :err

if exist "%REPO%\python\censorcut" (
    echo === Python tests ===
    pushd "%REPO%\python"
    python -m unittest discover -s tests
    if errorlevel 1 (
        popd
        goto :err
    )
    popd
)

if exist "%REPO%\sync\Cargo.toml" (
    echo === Rust sync tests ===
    set "PATH=%USERPROFILE%\.cargo\bin;%PATH%"
    pushd "%REPO%\sync"
    cargo test
    if errorlevel 1 (
        popd
        goto :err
    )
    popd
)

:done
echo === All builds and tests passed ===
endlocal & exit /b 0

:err
echo === BUILD FAILED ===
endlocal & exit /b 1
