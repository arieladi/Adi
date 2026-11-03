@echo off
rem One-time setup; see install.ps1 for what it does and its options, e.g.
rem   install.cmd -LogDir E:\InteractionLogs
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
