#!/usr/bin/env python3
"""Run each supplied catalog row in its own native Windows runtime process.

The manifest names user-provided SHA-addressed inputs, not download URLs.
Records distinguish real display/input checks from full firmware acceptance.
"""
from __future__ import annotations
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import traceback


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def worker(options, row):
    package = options.package.resolve()
    project = package / "project"
    sys.path.insert(0, str(project / "backend"))
    sys.path.insert(0, str(package / "windows-app"))
    from src.windows_runtime import WindowsRuntime
    from frames import decode_frame
    case = options.workspace / hashlib.sha256(row["profileID"].encode()).hexdigest()[:16]
    runtime = WindowsRuntime(project, package / "qemu-system-arm.exe", case)
    record = {"profileID": row["profileID"], "schema": "azki.windows.display.acceptance.v1",
              "bootVerified": False, "physicalBluetoothVerified": False, "frames": []}
    args = {"profileID": row["profileID"], "firmwarePath": str(options.objects / row["firmwareAsset"]),
            "skipOOBE": True, "bluetooth": "reject"}
    if row.get("resourceAsset"):
        args["resourcePath"] = str(options.objects / row["resourceAsset"])
    try:
        record["prepared"] = runtime.prepare(args)
        record["started"] = runtime.start(args)
        start = time.monotonic()
        seen = set()
        while time.monotonic() - start < options.seconds:
            time.sleep(.5)
            if runtime.process.poll() is not None:
                raise RuntimeError("Owned core exited during display observation")
            frame = runtime.frame()
            if not frame.get("ready") or frame["sequence"] in seen:
                continue
            seen.add(frame["sequence"])
            image = decode_frame(frame).image()
            entry = {key: value for key, value in frame.items() if key != "pixelsBase64"}
            entry.update(elapsed=round(time.monotonic() - start, 3),
                         pixelsSHA256=hashlib.sha256(image.tobytes()).hexdigest(),
                         colors=len(image.getcolors(image.width * image.height) or []))
            record["frames"].append(entry)
            if entry["colors"] > 16:
                image.save(case / "last-frame.png")
        record["status"] = runtime.status()
        record["faultRegisters"] = runtime.qmp.execute("human-monitor-command", {"command-line": "xp /6wx 0xe000ed24"})
        record["registers"] = runtime.qmp.execute("human-monitor-command", {"command-line": "info registers"})
        words = [int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{8})", record["faultRegisters"])]
        xpsr = re.search(r"XPSR=([0-9a-fA-F]{8})", record["registers"])
        active = int(xpsr.group(1), 16) & 511 if xpsr else None
        record["faultFreeAtEnd"] = len(words) == 6 and words[1] == words[2] == 0 and active is not None and active not in (3, 4, 5, 6)
        record["hardwareCapabilities"] = runtime.input({"command": "capabilities"})
        width, height = row["expectedWidth"], row["expectedHeight"]
        record["input"] = {"down": runtime.touch({"x": width // 2, "y": height // 2, "down": True}),
                           "up": runtime.touch({"x": width // 2, "y": height // 2, "down": False}),
                           "guestObserved": False}
        time.sleep(.5)
        record["displayObserved"] = any(item["colors"] > 16 for item in record["frames"])
    except BaseException as error:
        record["error"] = {"type": type(error).__name__, "message": str(error), "traceback": traceback.format_exc(),
                           "subprocessStdout": str(getattr(error, "stdout", ""))[-4096:],
                           "subprocessStderr": str(getattr(error, "stderr", ""))[-4096:]}
    finally:
        record["stop"] = runtime.stop()
        if case.is_dir():
            save(case / "acceptance.json", record)
    print(json.dumps({"profileID": row["profileID"], "displayObserved": record.get("displayObserved", False),
                      "error": record.get("error"), "stop": record["stop"]}, ensure_ascii=False), flush=True)
    return 0 if record.get("displayObserved") and record.get("faultFreeAtEnd") and record["stop"]["cleanupComplete"] and "error" not in record else 1


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="strict", newline="\n")
    sys.stderr.reconfigure(encoding="utf-8", errors="backslashreplace", newline="\n")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--objects", type=Path, required=True)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--seconds", type=int, default=45, choices=range(10, 181))
    parser.add_argument("--workers", type=int, default=3, choices=range(1, 5))
    parser.add_argument("--worker-row", type=int)
    options = parser.parse_args()
    if os.name != "nt":
        parser.error("This acceptance runner requires actual Windows")
    plan = json.loads(options.manifest.read_text(encoding="utf-8"))
    if options.worker_row is not None:
        return worker(options, plan["profiles"][options.worker_row])
    sys.path.insert(0, str(options.package / "project/backend"))
    from src.windows_runtime_files import WindowsFiles
    files = WindowsFiles()
    files.initialize_private_creator()
    files.private_directory(options.workspace)
    save(options.workspace / "owner.json", {"schema": "azki.windows.acceptance.matrix.v1", "ownerSID": files.sid})
    # Verify the transferred original bytes once before any firmware execution.
    verified = []
    for identity, asset in plan["assets"].items():
        if not re.fullmatch("[a-f0-9]{64}", identity):
            raise ValueError("Invalid input asset identity")
        path = options.objects / identity
        if path.stat().st_size != asset["bytes"] or digest(path) != identity:
            raise ValueError("Transferred firmware asset differs from its hash: " + identity)
        verified.append(identity)
    save(options.workspace / "verified-assets.json", verified)

    def run_row(pair):
        index, row = pair
        if row.get("blockedByHistoricalRestriction"):
            return {"profileID": row["profileID"], "status": "not-executed", "reason": "historical-execution-restriction"}
        command = [sys.executable, "-I", "-B", str(Path(__file__).resolve()),
                   "--package", str(options.package), "--manifest", str(options.manifest),
                   "--objects", str(options.objects), "--workspace", str(options.workspace),
                   "--seconds", str(row.get("observationSeconds", options.seconds)), "--worker-row", str(index)]
        try:
            result = subprocess.run(command, capture_output=True, timeout=row.get("observationSeconds", options.seconds) + 180,
                                    creationflags=subprocess.CREATE_NO_WINDOW,
                                    env=dict(os.environ, PYTHONUTF8="1", PYTHONIOENCODING="utf-8"))
            stem = hashlib.sha256(row["profileID"].encode()).hexdigest()[:16]
            (options.workspace / (stem + ".stdout")).write_bytes(result.stdout)
            (options.workspace / (stem + ".stderr")).write_bytes(result.stderr)
            return {"profileID": row["profileID"], "status": "display-pass" if result.returncode == 0 else "failed",
                    "exitCode": result.returncode, "case": stem}
        except subprocess.TimeoutExpired:
            # subprocess.run closes only its exact owned worker. The worker's
            # non-inherited kill-on-close Job handle closes its own QEMU child.
            return {"profileID": row["profileID"], "status": "worker-timeout"}

    rows = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=options.workers) as pool:
        for result in pool.map(run_row, enumerate(plan["profiles"])):
            rows.append(result)
            save(options.workspace / "matrix.json", {"schema": 1, "rows": rows,
                                                       "fullFirmwareBootVerified": False})
            print(json.dumps(result, ensure_ascii=False), flush=True)
    return int(any(row["status"] not in ("display-pass", "not-executed") for row in rows))


if __name__ == "__main__":
    raise SystemExit(main())
