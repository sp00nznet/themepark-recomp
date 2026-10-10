@echo off
rem Configure and build the 64-bit host with clang-cl + Ninja.
rem Needs Visual Studio 2022 (any edition, or the Build Tools) with the C++ x64
rem tools, LLVM (clang-cl), CMake and Ninja. PCRECOMP names the pcrecomp
rem checkout to build against (default ..\tools).
setlocal
rem vswhere goes on PATH rather than in quotes: a quoted path with "(x86)" in
rem it breaks inside for /f.
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;C:\Program Files (x86)\Microsoft Visual Studio\Installer"
for /f "usebackq delims=" %%i in (`vswhere.exe -latest -products * -property installationPath`) do set "VS=%%i"
if not defined VS (echo Visual Studio 2022 not found & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%ProgramFiles%\LLVM\bin;%VS%\VC\Tools\Llvm\x64\bin;%PATH%"
cd /d "%~dp0"
if not defined BUILD_DIR set "BUILD_DIR=build"
set "PCARG="
if defined PCRECOMP set "PCARG=-DPCRECOMP=%PCRECOMP%"
if not exist %BUILD_DIR%\build.ninja cmake -S . -B %BUILD_DIR% -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=RelWithDebInfo %PCARG% || exit /b 1
if defined PCRECOMP cmake -B %BUILD_DIR% %PCARG% >nul || exit /b 1
cmake --build %BUILD_DIR% %*
