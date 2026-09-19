@echo off
rem Leave this window open (minimised is fine) - it prints recording status.
setlocal
cd /d "%~dp0"
title interaction logger
if exist ".venv\Scripts\python.exe" (
  ".venv\Scripts\python.exe" logger_daemon.py %*
) else (
  echo .venv not found - run install.cmd first
  pause
  exit /b 1
)
