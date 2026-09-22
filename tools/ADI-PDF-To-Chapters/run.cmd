@echo off
rem Split every PDF next to this script into chapter-sized PDFs.
rem Options pass through, e.g.  run.cmd --dry-run   or   run.cmd --max-pages 20
setlocal
cd /d "%~dp0"
if not exist ".venv\Scripts\python.exe" (
  call "%~dp0install.cmd" || goto :fail
)
".venv\Scripts\python.exe" "%~dp0pdf_to_chapters.py" %*
set rc=%errorlevel%
rem double-clicked (no arguments): keep the window open to read the result
if "%~1"=="" pause
exit /b %rc%
:fail
if "%~1"=="" pause
exit /b 1
