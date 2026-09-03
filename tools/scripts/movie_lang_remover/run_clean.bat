@echo off
title Movie Lang Remover
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0clean.ps1" %*
echo.
echo Finished. Press any key to close.
pause >nul
