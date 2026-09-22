@echo off
rem One-time setup: private venv next to this script, then dependencies.
setlocal
cd /d "%~dp0"
if not exist ".venv\Scripts\python.exe" (
  echo creating .venv
  python -m venv .venv || goto :fail
)
".venv\Scripts\python.exe" -m pip install --upgrade pip
".venv\Scripts\python.exe" -m pip install -r requirements.txt || goto :fail
echo.
echo done. drop PDFs next to run.cmd and run it
exit /b 0
:fail
echo.
echo setup failed
exit /b 1
