@echo off
REM SPDX-License-Identifier: GPL-3.0-or-later
REM
REM Windows: configure (once) and build the JUCE-on tree in build-juce\, or one
REM target of it. The companion to tools\build.bat, which builds the core with
REM no JUCE. Needs JUCE fetched: bash tools/fetch_external.sh --with-juce.
REM
REM   tools\build-juce.bat                  everything: adi_play, adi_vst3_probe, the fixture VST3, the probes
REM   tools\build-juce.bat adi_play         one target
REM
REM Debug, with warnings as errors on our own code (ADI_WERROR=ON), as CI's
REM JUCE job builds it. Executables land in build-juce\<target>_artefacts\Debug\.
REM Keep the checkout path short: JUCE's generated paths overflow MAX_PATH from
REM deep folders.
setlocal
REM Claude Code sets this, and vcvars64 then cannot find vswhere.exe beside it.
set NoDefaultCurrentDirectoryInExePath=
set VS=C:\Program Files\Microsoft Visual Studio\2022\Community
set CM=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
set NJ=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe
if not exist "%VS%\VC\Auxiliary\Build\vcvars64.bat" (
    echo ERROR: no Visual Studio 2022 at "%VS%".
    exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0.."
if not exist build-juce\CMakeCache.txt (
    "%CM%" -S . -B build-juce -G Ninja -DCMAKE_MAKE_PROGRAM="%NJ%" -DCMAKE_BUILD_TYPE=Debug -DADI_WITH_JUCE=ON -DADI_WERROR=ON
    if errorlevel 1 exit /b 1
)
if "%1"=="" (
    "%CM%" --build build-juce
) else (
    "%CM%" --build build-juce --target %1
)
