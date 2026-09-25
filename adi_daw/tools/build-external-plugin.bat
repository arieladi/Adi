@echo off
REM SPDX-License-Identifier: GPL-3.0-or-later
REM
REM Windows: build one third-party plug-in as CLAP (ADR-0166), Release, with
REM MSVC and Ninja -- the same toolchain as the DAW, no MinGW or clang-cl.
REM
REM   tools\build-external-plugin.bat <name> [target]
REM
REM   smartelectronix  all eleven, through plugins\external\smartelectronix
REM   chowtape         ChowTapeModel's own tree, which makes its own CLAP
REM   chowcentaur      through plugins\external\chowcentaur
REM   zlequalizer      through plugins\external\zlequalizer
REM   dragonfly        the four reverbs, through plugins\external\dragonfly
REM
REM Sources: tools/fetch_plugins.sh. ADI_PLUGIN_SOURCES names its DEST (default
REM third_party\plugins); builds go to ADI_PLUGIN_BUILD\<name> (default
REM build-external\<name>). Keep both short: JUCE's generated paths overrun
REM MAX_PATH from deep ones. The .clap files found are listed at the end.
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
cd /d "%~dp0.."
set ROOT=%CD%
if not defined ADI_PLUGIN_SOURCES set ADI_PLUGIN_SOURCES=%ROOT%\third_party\plugins
if not defined ADI_PLUGIN_BUILD set ADI_PLUGIN_BUILD=%ROOT%\build-external
REM CMake reads backslashes as escapes in some places; hand it forward slashes.
set PS=%ADI_PLUGIN_SOURCES:\=/%

set NAME=%~1
set SRC=
set ARG=
if /i "%NAME%"=="smartelectronix" set "SRC=%ROOT%\plugins\external\smartelectronix" & set "ARG=-DSMX_SOURCE=%PS%/smartelectronix"
if /i "%NAME%"=="chowtape" set "SRC=%ADI_PLUGIN_SOURCES%\AnalogTapeModel\Plugin" & set "ARG=-DCHOWTAPE_BUILD_CLAP=ON"
if /i "%NAME%"=="chowcentaur" set "SRC=%ROOT%\plugins\external\chowcentaur" & set "ARG=-DKLON_SOURCE=%PS%/KlonCentaur"
if /i "%NAME%"=="zlequalizer" set "SRC=%ROOT%\plugins\external\zlequalizer" & set "ARG=-DZL_SOURCE=%PS%/ZLEqualizer"
if /i "%NAME%"=="dragonfly" set "SRC=%ROOT%\plugins\external\dragonfly" & set "ARG=-DDRAGONFLY_SOURCE=%PS%/dragonfly-reverb"
if "%SRC%"=="" (
    echo usage: %~nx0 smartelectronix^|chowtape^|chowcentaur^|zlequalizer^|dragonfly [target]
    exit /b 2
)

call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set BUILD=%ADI_PLUGIN_BUILD%\%NAME%
if not exist "%BUILD%\CMakeCache.txt" (
    "%CM%" -S "%SRC%" -B "%BUILD%" -G Ninja -DCMAKE_MAKE_PROGRAM="%NJ%" -DCMAKE_BUILD_TYPE=Release "%ARG%"
    if errorlevel 1 exit /b 1
)
if "%~2"=="" (
    "%CM%" --build "%BUILD%"
) else (
    "%CM%" --build "%BUILD%" --target %~2
)
if errorlevel 1 exit /b 1
echo.
for /r "%BUILD%" %%f in (*.clap) do if exist "%%f" echo %%f
