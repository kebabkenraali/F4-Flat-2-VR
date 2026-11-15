@echo off
rem Build the F4SE plugin (fo4flat_probe.dll) with VS2022 + Ninja + vcpkg.
rem Run from anywhere; output: plugin\b\fo4flat_probe.dll
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo vcvars64.bat failed
    exit /b 1
)
set VCPKG_ROOT=C:\vcpkg
cmake -S "%~dp0plugin" -B "%~dp0plugin\b" -G Ninja -DCMAKE_BUILD_TYPE=Release -DVCPKG_ROOT_DIR=C:/vcpkg -DCMAKE_MAKE_PROGRAM="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if errorlevel 1 (
    echo CMake configure failed
    exit /b 1
)
cmake --build "%~dp0plugin\b"
if errorlevel 1 (
    echo Build failed
    exit /b 1
)
echo OK: %~dp0plugin\b\fo4flat_probe.dll
