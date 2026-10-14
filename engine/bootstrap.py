# Original Azki Simulator bootstrap. Imported guest/runtime code is not run by the packager.
import os
import sys
from pathlib import Path

if not (sys.flags.isolated and sys.flags.no_site and sys.dont_write_bytecode):
    raise SystemExit("Engine requires -I -S -B")
engine = Path(__file__).resolve().parent
if len(sys.argv) < 6 or sys.argv[1] != "--project" or sys.argv[3] != "--runner" or sys.argv[5] != "--":
    raise SystemExit("usage: bootstrap.py --project ABSOLUTE_PROJECT --runner multi_device.py|backend/run.py -- ARGS")
project = Path(sys.argv[2])
runner = sys.argv[4]
if not project.is_absolute() or project.resolve() != project or not project.is_dir():
    raise SystemExit("Project must be an existing canonical private directory")
if runner not in ("multi_device.py", "backend/run.py"):
    raise SystemExit("Unsupported runner")
script = project / runner
if script.resolve() != script or not script.is_file():
    raise SystemExit("Runner must be a regular, non-symlink file")
stdlib = engine / "python/lib/python3.12"
sys.path[:] = [str(stdlib), str(stdlib / "lib-dynload"), str(stdlib / "site-packages"), str(project), str(project / "backend")]
for key in list(os.environ):
    if key.startswith(("PYTHON", "DYLD_")):
        del os.environ[key]
os.environ["PYTHONNOUSERSITE"] = "1"
os.environ["PYTHONDONTWRITEBYTECODE"] = "1"
sys.argv = [str(script), *sys.argv[6:]]
import runpy
runpy.run_path(str(script), run_name="__main__")
