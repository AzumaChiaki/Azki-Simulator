@echo off
setlocal
set "AZKI_PROJECT=%~dp0.."
set "AZKI_PYTHON="
if exist "%AZKI_PROJECT%\python\pythonw.exe" set "AZKI_PYTHON=%AZKI_PROJECT%\python\pythonw.exe"
if not defined AZKI_PYTHON if exist "%AZKI_PROJECT%\runtime\python\pythonw.exe" set "AZKI_PYTHON=%AZKI_PROJECT%\runtime\python\pythonw.exe"
if not defined AZKI_PYTHON if exist "%AZKI_PROJECT%\tools\python\pythonw.exe" set "AZKI_PYTHON=%AZKI_PROJECT%\tools\python\pythonw.exe"
if not defined AZKI_PYTHON if exist "%AZKI_PROJECT%\dist\windows\python\pythonw.exe" set "AZKI_PYTHON=%AZKI_PROJECT%\dist\windows\python\pythonw.exe"
if not defined AZKI_PYTHON (
    echo Run the Windows build or install the packaged Python runtime with Tk and Pillow first.
    echo Alternatively launch engine\windows-app\app.py with a configured Python interpreter.
    pause
    exit /b 1
)
if exist "%AZKI_PROJECT%\windows-app\app.py" (
    start "Azki Simulator" "%AZKI_PYTHON%" "%AZKI_PROJECT%\windows-app\app.py" --project-root "%AZKI_PROJECT%\project" --qemu "%AZKI_PROJECT%\qemu-system-arm.exe" %*
) else (
    start "Azki Simulator" "%AZKI_PYTHON%" "%AZKI_PROJECT%\engine\windows-app\app.py" --project-root "%AZKI_PROJECT%\engine\project" --qemu "%AZKI_PROJECT%\dist\windows\qemu-system-arm.exe" %*
)
