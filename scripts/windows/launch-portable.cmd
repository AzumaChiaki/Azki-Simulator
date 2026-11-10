@echo off
setlocal
set "PYTHONUTF8=1"
set "PYTHONIOENCODING=utf-8"
if not exist "%~dp0python\pythonw.exe" (
    echo The packaged Python runtime is missing. Extract the complete Azki Simulator folder.
    pause
    exit /b 1
)
start "Azki Simulator" "%~dp0python\pythonw.exe" "%~dp0AzkiSimulator.pyw" %*
