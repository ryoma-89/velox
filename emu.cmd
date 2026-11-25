@echo off
rem Velox CLI launcher: runs "python -m velox" from the repository root.
setlocal
set "PYTHONPATH=%~dp0;%PYTHONPATH%"
set "PYTHONUTF8=1"
python -m velox %*
exit /b %ERRORLEVEL%
