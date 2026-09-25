@echo off
REM SPDX-License-Identifier: GPL-3.0-or-later
REM
REM Windows: configure (once) and build ADI's own plug-ins (plugins\, ADR-0166)
REM in build-plugins\, or one target of it. Release: these ship.
REM
REM   tools\build-plugins.bat                everything
REM   tools\build-plugins.bat adi_rmsc_CLAP  one target
REM
REM Needs JUCE fetched: bash tools/fetch_external.sh --with-juce. The CLAP bridge
REM (clap-juce-extensions, pinned) is fetched by CMake at configure time.
REM Plug-ins land in build-plugins\<name>\<target>_artefacts\Release\<format>\.
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
REM ADI Airwindows is built when ADI_AIRWIN_SOURCE names an airwin2rack checkout
REM (tools/fetch_plugins.sh); setting it reconfigures an existing build.
if defined ADI_AIRWIN_SOURCE (
    "%CM%" -S plugins -B build-plugins -G Ninja -DCMAKE_MAKE_PROGRAM="%NJ%" -DCMAKE_BUILD_TYPE=Release "-DADI_AIRWIN_SOURCE=%ADI_AIRWIN_SOURCE%"
    if errorlevel 1 exit /b 1
) else if not exist build-plugins\CMakeCache.txt (
    "%CM%" -S plugins -B build-plugins -G Ninja -DCMAKE_MAKE_PROGRAM="%NJ%" -DCMAKE_BUILD_TYPE=Release
    if errorlevel 1 exit /b 1
)
if "%1"=="" (
    "%CM%" --build build-plugins
) else (
    "%CM%" --build build-plugins --target %1
)
