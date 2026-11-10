#!/usr/bin/env python3
"""Opt-in Windows UI/real-runtime smoke test; never substitutes a fake frame."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import time
from types import SimpleNamespace
import uuid

from app import SimulatorApp


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project-root", type=Path, required=True)
    parser.add_argument("--qemu", type=Path, required=True)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--profile", required=True)
    parser.add_argument("--firmware", type=Path, required=True)
    parser.add_argument("--resource", type=Path)
    parser.add_argument("--audio", type=Path)
    parser.add_argument("--sensor", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--persistent", action="store_true", help="Exercise a saved instance through two real UI launches")
    options = parser.parse_args()
    if os.name != "nt" or not 1 <= options.timeout <= 600 or options.output.exists():
        parser.error("Requires Windows, a fresh output path and timeout1..600s")
    import tkinter as tk
    root = tk.Tk()
    options.output.parent.mkdir(parents=True, exist_ok=True)
    app = SimulatorApp(root, options)
    receipt = {"scope": "Windows UI and real-runtime transport; not firmware boot/hardware acceptance",
               "profileID": options.profile, "ownPID": os.getpid(), "started": False,
               "realFrameReceived": False, "touchRequested": False, "firmwareBootAccepted": False,
               "persistent": options.persistent, "launches": []}
    calls = []
    launch_requests = []
    stop_results = []
    original_request = app.client.request
    def tracked_request(command, args=None, **kwargs):
        if command in ("prepare", "start"):
            launch_requests.append({"command": command, "args": dict(args or {})})
        return original_request(command, args, **kwargs)
    app.client.request = tracked_request
    original = app._call
    def tracked(command, args=None, **kwargs):
        if command in ("touch", "button", "crown"):
            calls.append({"command": command, "args": args})
        if command == "stop":
            callback = kwargs.get("callback")
            def stopped(value):
                stop_results.append(value)
                if callback:
                    callback(value)
            kwargs["callback"] = stopped
        return original(command, args, **kwargs)
    app._call = tracked
    started = time.monotonic()
    display_at = None
    closing = False
    creating = False
    instance_id = None
    phase = "select"
    prior_revision = None
    def finish(error=None):
        nonlocal closing
        if closing:
            return
        closing = True
        if error:
            receipt["error"] = error
        receipt["inputRequests"] = calls
        receipt["launchRequests"] = launch_requests
        receipt["stopResults"] = stop_results
        receipt["statusText"] = app.status.get()
        receipt["elapsedSeconds"] = time.monotonic() - started
        app.close()
    def tick():
        nonlocal display_at, creating, instance_id, phase, prior_revision
        if closing:
            return
        try:
            if time.monotonic() - started > options.timeout:
                finish("Timed out before real UI/frame transport completed")
                return
            if phase == "select" and app.profiles:
                index = next((i for i, p in enumerate(app.profiles) if p["id"] == options.profile), None)
                if index is None:
                    finish("Requested profile not present in runtime catalog")
                    return
                app.profile.current(index)
                if options.persistent:
                    if not creating:
                        creating = True
                        def created(value):
                            nonlocal instance_id
                            instance_id = value["instanceID"]
                            receipt["instanceID"] = instance_id
                            app.refresh_instances(instance_id)
                        app._call("instances", {"action": "create", "profileID": options.profile,
                                                "name": "UI verification " + uuid.uuid4().hex[:12]},
                                  callback=created, error_callback=lambda error: finish(str(error)))
                    row = app.current_instance()
                    if instance_id is None or not row or row.get("instanceID") != instance_id:
                        root.after(100, tick)
                        return
                for key, value in (("firmwarePath", options.firmware), ("resourcePath", options.resource),
                                   ("audioPath", options.audio), ("sensorPath", options.sensor)):
                    if value:
                        app.fields[key].set(str(value.resolve()))
                receipt["started"] = True
                phase = "frame"
                app.start()
            if phase == "frame" and app.running and app.last_frame is not None and display_at is None:
                frame = app.last_frame
                image = frame.image()
                if image.getbbox() is not None:
                    launch = len(receipt["launches"]) + 1
                    output = options.output.with_suffix(f".launch-{launch}.frame.png")
                    image.save(output)
                    receipt.update(realFrameReceived=True, width=frame.width, height=frame.height,
                                   frameSHA256=hashlib.sha256(output.read_bytes()).hexdigest())
                    receipt["launches"].append({"launch": launch, "frameSHA256": receipt["frameSHA256"],
                                                "width": frame.width, "height": frame.height})
                    left, top, width, height = app.panel_rectangle
                    x, y = left + width // 2, top + height // 2
                    # Exercise our own controller function, without sending OS
                    # input or operating any other application/window.
                    app.pointer(SimpleNamespace(x=x, y=y), True)
                    app.pointer(SimpleNamespace(x=x, y=y), False)
                    receipt["touchRequested"] = len(calls) >= 2
                    receipt["frontendPanel"] = {"width": app.canvas.winfo_width(),
                                                 "height": app.canvas.winfo_height(),
                                                 "imageAllocated": app.photo is not None}
                    display_at = time.monotonic()
            if display_at is not None and time.monotonic() - display_at > 2:
                if options.persistent and len(receipt["launches"]) == 1:
                    phase = "saved"
                    display_at = None
                    app.stop()
                    root.after(100, tick)
                    return
                finish()
                return
            if phase == "saved" and not app.running:
                persistence = stop_results[-1].get("persistence", {}) if stop_results else {}
                if persistence.get("committed") is not True:
                    finish("First UI stop did not commit the saved instance")
                    return
                row = app.current_instance()
                if (row and row.get("instanceID") == instance_id and row.get("initialized") is True
                        and row.get("headRevision") == persistence.get("revision")):
                    prior_revision = row["headRevision"]
                    receipt["firstCommittedRevision"] = prior_revision
                    phase = "frame"
                    app.start()
            root.after(100, tick)
        except Exception as error:
            finish(str(error))
    root.after(100, tick)
    root.mainloop()
    receipt["runtimePID"] = app.client.process.pid
    receipt["ownedRuntimeClosed"] = app.client.process.poll() is not None
    receipt["runtimeExitCode"] = app.client.process.returncode
    receipt["runtimeClose"] = app.client.close_report
    receipt["uiTransportPassed"] = all(receipt.get(key) for key in ("realFrameReceived", "touchRequested", "ownedRuntimeClosed")) and "error" not in receipt and not app.client.close_report.get("forced")
    if options.persistent:
        persistence = app.client.close_report.get("stopResult", {}).get("persistence", {})
        receipt["secondCommit"] = persistence
        saved_launch = launch_requests[2:]
        saved_args = {"instanceID": instance_id, "bluetooth": "reject"}
        receipt["savedArgumentsVerified"] = (len(saved_launch) == 2 and
                                              all(call["args"] == saved_args for call in saved_launch))
        receipt["savedInstancePassed"] = (len(receipt["launches"]) == 2 and prior_revision is not None
                                           and receipt["savedArgumentsVerified"]
                                           and persistence.get("committed") is True
                                           and persistence.get("previousRevision") == prior_revision
                                           and persistence.get("revision") != prior_revision)
        receipt["uiTransportPassed"] = receipt["uiTransportPassed"] and receipt["savedInstancePassed"]
    options.output.parent.mkdir(parents=True, exist_ok=True)
    with options.output.open("x", encoding="utf-8") as stream:
        json.dump(receipt, stream, ensure_ascii=False, indent=2)
        stream.write("\n")
    print(json.dumps(receipt, ensure_ascii=False))
    return 0 if receipt["uiTransportPassed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
