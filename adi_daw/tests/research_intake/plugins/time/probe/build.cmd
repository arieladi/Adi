@echo off
rem Run in an x64 MSVC developer terminal. VST3_SDK_ROOT contains pluginterfaces/.
if "%VST3_SDK_ROOT%"=="" exit /b 2
cd /d "%~dp0"
cl /nologo /std:c++20 /EHsc /O2 /MT /LD /W4 /I"%VST3_SDK_ROOT%" adi_time_probe.cpp /Fe:ADITimeProbe.vst3 /link /INCREMENTAL:NO
if errorlevel 1 exit /b %errorlevel%
cl /nologo /std:c++20 /EHsc /O2 /MT /W4 /I"%VST3_SDK_ROOT%" selftest.cpp /Fe:selftest.exe /link /INCREMENTAL:NO
if errorlevel 1 exit /b %errorlevel%
selftest.exe
exit /b %errorlevel%

