"""Portable Windows entry point; the desktop UI has one implementation."""
from pathlib import Path
import ctypes
import runpy
import sys


def main():
    root = Path(__file__).resolve().parent
    project = root / "project"
    sys.path.insert(0, str(project / "backend"))
    from src.windows_runtime_files import WindowsFiles
    # File owners do not inherit. Normalize only this application process's
    # creator owner before Tk creates its workspace and starts the controller.
    WindowsFiles().initialize_private_creator()
    app = root / "windows-app/app.py"
    sys.path.insert(0, str(app.parent))
    sys.argv = [str(app), "--project-root", str(project),
                "--qemu", str(root / "qemu-system-arm.exe"), *sys.argv[1:]]
    runpy.run_path(str(app), run_name="__main__")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        # pythonw has no console: make a failed portable launch visible.
        ctypes.windll.user32.MessageBoxW(None, str(error), "Azki Simulator", 0x10)
        raise
