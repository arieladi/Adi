@echo off
rem Build the Surge XT CLAP on Windows, then prove a CLAP came out (ADR-0007).
rem
rem   adi-surge\tools\build_clap_win.bat          configure + build surge-xt_CLAP, Release
rem   adi-surge\tools\build_clap_win.bat tests    ...then surge-testrunner + ctest -j 4,
rem                                               then clap_smoke: load the CLAP headless,
rem                                               play A4, check it made the right sound
rem
rem   set SURGE_CMAKE_ARGS=-DENABLE_LTO=OFF       extra configure flags, e.g. while iterating
rem   set ADI_SURGE_SURGE_DIR=C:\...\adi-surge\surge
rem                                               the Surge clone, when this script runs from
rem                                               a checkout without one (e.g. a git worktree)
rem
rem Exit codes: 0 ok, 1 vcvars, 2 configure, 3 build, 4 no .clap on disk, 5 ctest,
rem             6 clap_smoke checks failed.
rem
rem It is a .bat, not a .sh: vcvars64.bat sets MSVC's environment in the CURRENT
rem cmd shell, so calling it from bash configures a subshell that exits at once.
rem CMake and Ninja come from inside the VS install, not PATH (ARCHITECTURE.md 2.3).
rem
rem NoDefaultCurrentDirectoryInExePath: Claude Code sets it to 1. With it set,
rem libs/luajitlib/CMakeLists.txt runs execute_process(COMMAND build-msvc-luajit.bat)
rem by bare name, Windows will not look in the WORKING_DIRECTORY, and configure
rem dies with "Build script exit code: no such file or directory". A person in a
rem Developer Prompt never has it set. Cleared here, for this process tree only.

setlocal
set "NoDefaultCurrentDirectoryInExePath="
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

rem Canonical absolute paths: a build tree caches its source path, and a
rem different spelling of the same directory re-points the cache.
set "SURGE_IN=%~dp0..\surge"
if defined ADI_SURGE_SURGE_DIR set "SURGE_IN=%ADI_SURGE_SURGE_DIR%"
for %%I in ("%SURGE_IN%") do set "SURGE=%%~fI"
for %%I in ("%~dp0clap_smoke") do set "SMOKE_SRC=%%~fI"
for %%I in ("%~dp0..\build-clap-smoke") do set "SMOKE_BUILD=%%~fI"
set "BUILD=%SURGE%\build"
set "CLAP=%BUILD%\surge_xt_products\Surge XT.clap"
set "CLAP_INCLUDE=%SURGE%\libs\clap-juce-extensions\clap-libs\clap\include"

call "%VCVARS%" >nul
if errorlevel 1 (echo vcvars64 failed & exit /b 1)
cmake --version | findstr /B /C:"cmake version"

cmake -S "%SURGE%" -B "%BUILD%" -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release %SURGE_CMAKE_ARGS%
if errorlevel 1 (echo configure failed & exit /b 2)

cmake --build "%BUILD%" --config Release --target surge-xt_CLAP --parallel
if errorlevel 1 (echo build failed & exit /b 3)

rem Assert the artifact, not the exit code.
if not exist "%CLAP%" (echo NO CLAP: "%CLAP%" is missing & exit /b 4)
echo CLAP: %CLAP%

if /i not "%~1"=="tests" exit /b 0

cmake --build "%BUILD%" --config Release --target surge-testrunner --parallel
if errorlevel 1 (echo surge-testrunner build failed & exit /b 3)

pushd "%BUILD%"
ctest -j 4
set "CTEST_RC=%errorlevel%"
popd
if not "%CTEST_RC%"=="0" (echo ctest failed & exit /b 5)

cmake -S "%SMOKE_SRC%" -B "%SMOKE_BUILD%" -G "Visual Studio 17 2022" -A x64 "-DCLAP_SMOKE_CLAP_INCLUDE=%CLAP_INCLUDE%"
if errorlevel 1 (echo clap_smoke configure failed & exit /b 2)
cmake --build "%SMOKE_BUILD%" --config Release
if errorlevel 1 (echo clap_smoke build failed & exit /b 3)

"%SMOKE_BUILD%\Release\clap_smoke.exe" "%CLAP%" --wav "%SMOKE_BUILD%\surge-xt-a4.wav"
if errorlevel 1 (echo clap_smoke failed & exit /b 6)
exit /b 0
