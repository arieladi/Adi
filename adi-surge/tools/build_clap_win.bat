@echo off
rem Build the Surge XT CLAP on Windows, then prove a CLAP came out (ADR-0007).
rem
rem   adi-surge\tools\build_clap_win.bat          configure + build surge-xt_CLAP, Release
rem   adi-surge\tools\build_clap_win.bat tests    ...then surge-testrunner, then ctest -j 4
rem
rem   set SURGE_CMAKE_ARGS=-DENABLE_LTO=OFF       extra configure flags, e.g. while iterating
rem
rem Exit codes: 0 ok, 1 vcvars, 2 configure, 3 build, 4 no .clap on disk, 5 ctest.
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
rem Canonical absolute path: the build tree caches its source path, and a
rem different spelling of the same directory re-points the cache.
for %%I in ("%~dp0..\surge") do set "SURGE=%%~fI"
set "BUILD=%SURGE%\build"
set "CLAP=%BUILD%\surge_xt_products\Surge XT.clap"

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
exit /b 0
