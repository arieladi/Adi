@echo off
REM SPDX-License-Identifier: GPL-3.0-or-later
REM
REM BLAKE3 throughput four ways (ADR-0153): portable, SSE4.1, AVX2, AVX-512.
REM Needs Visual Studio 2022 and third_party\BLAKE3 (tools/fetch_external.sh).
REM Builds into build-bench-blake3\ and prints one line per variant. Each binary
REM dispatches at run time, so a CPU without AVX-512 prints its AVX2 speed on the
REM last line; on an AVX-512 CPU (an 11th-gen Intel i9, say) the last line is
REM the AVX-512 number docs/AWAITING.md is waiting for.
setlocal
REM Claude Code sets this, and vcvars64 then cannot find vswhere.exe beside it.
set NoDefaultCurrentDirectoryInExePath=
set VS=C:\Program Files\Microsoft Visual Studio\2022\Community
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set ROOT=%~dp0..
set B=%ROOT%\third_party\BLAKE3\c
if not exist "%B%\blake3.c" (
    echo ERROR: no BLAKE3 at %B% -- run tools/fetch_external.sh --build-only
    exit /b 1
)
set OUT=%ROOT%\build-bench-blake3
if not exist "%OUT%" mkdir "%OUT%"
cd /d "%OUT%"
set COMMON=/nologo /O2 /I"%B%" "%ROOT%\tools\bench_blake3.c" "%B%\blake3.c" "%B%\blake3_dispatch.c" "%B%\blake3_portable.c"

cl /nologo /O2 /I"%B%" /c "%B%\blake3_sse2.c" "%B%\blake3_sse41.c" >nul || exit /b 1
cl /nologo /O2 /arch:AVX2 /I"%B%" /c "%B%\blake3_avx2.c" >nul || exit /b 1
cl /nologo /O2 /arch:AVX512 /I"%B%" /c "%B%\blake3_avx512.c" >nul || exit /b 1
cl %COMMON% /DBLAKE3_NO_SSE2 /DBLAKE3_NO_SSE41 /DBLAKE3_NO_AVX2 /DBLAKE3_NO_AVX512 /Fe:portable.exe >nul || exit /b 1
cl %COMMON% blake3_sse2.obj blake3_sse41.obj /DBLAKE3_NO_AVX2 /DBLAKE3_NO_AVX512 /Fe:sse41.exe >nul || exit /b 1
cl %COMMON% blake3_sse2.obj blake3_sse41.obj blake3_avx2.obj /DBLAKE3_NO_AVX512 /Fe:avx2.exe >nul || exit /b 1
cl %COMMON% blake3_sse2.obj blake3_sse41.obj blake3_avx2.obj blake3_avx512.obj /Fe:avx512.exe >nul || exit /b 1

echo CPU: %PROCESSOR_IDENTIFIER%
<nul set /p=portable        & "%OUT%\portable.exe"
<nul set /p=SSE2 + SSE4.1   & "%OUT%\sse41.exe"
<nul set /p=up to AVX2      & "%OUT%\avx2.exe"
<nul set /p=up to AVX-512   & "%OUT%\avx512.exe"
