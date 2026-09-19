@echo off
REM SPDX-License-Identifier: GPL-3.0-or-later
REM
REM Configure and build on Windows.
REM
REM This is a .bat and not a shell script for a reason that is easy to lose: the
REM MSVC compiler is not on PATH, and vcvars64.bat sets up the environment in
REM the CURRENT shell. Calling it from bash sets variables in a subshell that
REM exits immediately, so cl.exe is still missing afterwards. CMake and Ninja
REM also live inside the Visual Studio install rather than on PATH.
REM
REM   tools\build.bat              configure + build into build\
REM   tools\build.bat werror       same, with -Werror on our own code (ADR-0033)
REM   tools\build.bat clean        delete the build tree first
REM
REM On macOS and Linux none of this applies -- see collab/README.md.

setlocal

set VS=C:\Program Files\Microsoft Visual Studio\2022\Community
set VCVARS=%VS%\VC\Auxiliary\Build\vcvars64.bat
set CM=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
set NJ=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe

if not exist "%VCVARS%" (
    echo ERROR: no Visual Studio 2022 at "%VS%".
    echo Install the "Desktop development with C++" workload, or edit VS above.
    exit /b 1
)

call "%VCVARS%" >nul
if errorlevel 1 exit /b 1

cd /d "%~dp0.."

if "%1"=="clean" (
    if exist build rmdir /s /q build
    shift
)

set EXTRA=
if "%1"=="werror" set EXTRA=-DADI_WERROR=ON

"%CM%" -S . -B build -G Ninja -DCMAKE_MAKE_PROGRAM="%NJ%" -DCMAKE_BUILD_TYPE=Debug %EXTRA%
if errorlevel 1 exit /b 1

"%CM%" --build build
