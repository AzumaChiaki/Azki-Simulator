"""Expose the imported headless runner through the existing private v1 bridge.

The native host reads a validated atomic copy of each shared frame. Input goes
only through the runner's Unix input socket; its QMP connection is never shared.
"""
import os
from pathlib import Path
import threading
import uuid

from . import APPLICATION_VERSION, PROTOCOL_VERSION
from .bridge import (BridgeError, CAPABILITIES, SessionBridge, atomic_json,
                     create_session, validate_frame)
from .minative import MAX_RUN_SECONDS, MError, MinativeAdapter, _json, revalidate_plan
from .adc_commands import supports_adc, get_raw, set_raw
from .live_commands import live_get, live_set
from .virtual_peer import supports_peer, COMMANDS as GUEST_COMMANDS


def create_minative_session(root, plan, *, instance_id=None, library_root=None,
                            virtual_key_file=None, virtual_key_sha256=None, virtual_serial=None, virtual_address=None, skip_oobe=True):
    descriptor = create_session(root)
    root = Path(descriptor["directory"])
    if instance_id is not None:
        from .instance_storage import prepare_session_storage
        from .minative import with_instance_storage
        storage = prepare_session_storage(library_root, root, instance_id, plan,
            virtual_key_file=virtual_key_file, virtual_key_sha256=virtual_key_sha256,
            virtual_serial=virtual_serial, virtual_address=virtual_address)
        descriptor["instanceStorage"] = storage
        if storage["available"]:
            plan = with_instance_storage(plan, storage)
    plan = {**plan, "skipOOBE": bool(skip_oobe)}
    descriptor["skipOOBE"] = bool(skip_oobe)
    descriptor.update(backend="minative", profileID=plan["profileID"],
                      width=plan["width"], height=plan["height"],
                      approvalDigest=plan["approvalDigest"], backendPlan="backend-plan.json")
    atomic_json(root / "backend-plan.json", plan)
    atomic_json(root / "session.json", descriptor)
    return descriptor


class MinativeSessionBridge(SessionBridge):
    def __init__(self, root, approved_digest=None, max_runtime=0, *, native_broker=False):
        if (type(native_broker) is not bool or type(max_runtime) is not int
                or not 0 <= max_runtime <= MAX_RUN_SECONDS):
            raise BridgeError("invalid_native_broker", "Runtime must be integer 0 (unlimited) or 1…28800 seconds, with an explicit local broker mode")
        super().__init__(root, max_runtime=max_runtime)
        self.native_broker = native_broker
        if self.session.get("backend") != "minative" or self.session.get("backendPlan") != "backend-plan.json":
            raise BridgeError("unsafe_session", "Expected an explicit multi-device session")
        self.plan = _json(self.root / "backend-plan.json", private=True)
        if (self.plan.get("approvalDigest") != self.session.get("approvalDigest") or
                self.plan.get("profileID") != self.session.get("profileID") or
                (self.plan.get("width"), self.plan.get("height")) != (self.width, self.height)):
            raise BridgeError("unsafe_session", "Backend plan differs from session identity")
        self.approved_digest = approved_digest
        self.adapter = None
        self._frames_stop = threading.Event()
        self._frames_worker = None

    def status(self):
        details = self.adapter.status() if self.adapter else {}
        adapter_state = details.get("state", "ready")
        running = adapter_state in ("starting", "running", "stopping")
        failed = adapter_state == "failed" or self.last_error is not None
        return {"id": self.session["id"], "state": "running" if running else "crashed" if failed else "stopped",
                "adapterState": adapter_state, "backend": "minative", "profileID": self.plan["profileID"],
                "launchMode": "native-sibling-sandboxes" if self.native_broker else "developer-inner-sandbox",
                "applicationVersion": APPLICATION_VERSION, "protocolVersion": PROTOCOL_VERSION,
                "pid": details.get("pid") if running else None,
                "lastExit": details.get("lastExit"), "error": self.last_error or details.get("error"),
                "stopReason": details.get("reason"), "failureDetail": details.get("detail"),
                "cleanup": details.get("cleanup"),
                "debugger": details.get("debugger") if running else None,
                "executionApproved": self.approved_digest == self.plan["approvalDigest"],
                "approvalDigest": self.plan["approvalDigest"], "maxRuntimeSeconds": self.max_runtime,
                "hostProcessRunning": running, "bootVerified": False, "guestHealth": "unverified",
                "guestInstallSupported": supports_peer(self.plan), "capabilities": list(CAPABILITIES) +
                    (list(GUEST_COMMANDS) if supports_peer(self.plan) else []) +
                    (["pmu-adc-get", "pmu-adc-set"] if supports_adc(self.plan) else []) +
                    ["live-get", "live-set"],
                "generation": self.generation, "hardwareInputs": ["touch"] +
                    (["pmu.adcRaw"] if supports_adc(self.plan) else []),
                "width": self.width, "height": self.height}

    def start(self):
        if self.status()["state"] == "running":
            return self.status()
        if self.approved_digest != self.plan["approvalDigest"]:
            raise BridgeError("execution_not_approved", "The complete multi-device identity awaits explicit approval")
        if self.adapter:
            if self.plan["identity"].get("instanceStorage"):
                raise BridgeError("instance_preparation_required", "实例数据盘每次运行需要新会话快照；请停止后重新准备")
            stopped = self.stop()
            if stopped.get("cleanup") not in (None, "reaped"):
                raise BridgeError("stop_unconfirmed", "Previous process group cleanup is not confirmed")
        if len(list(self.root.glob("m-*"))) >= 8:
            raise BridgeError("quota", "Session run limit reached; preserve evidence and use a new session")
        options = {"native_broker": True} if self.native_broker else {}
        adapter = MinativeAdapter(self.plan, self.root, self.approved_digest, self.max_runtime, **options)
        # Adapter checks the local envelope once immediately before launch.
        if not self.native_broker:
            from .minative_isolation import execution_prefix
            adapter.execution_prefix = execution_prefix(adapter)
            adapter.isolation_verified = True
        # Native mode asks the independently running, fixed native broker. It
        # cannot Popen a guest, reapply a sandbox, or turn this flag into proof
        # of confinement. The native host owns guest launch/stop authority.
        adapter.start()
        self.adapter, self.run_root = adapter, adapter.run_root
        self.last_error = None
        self.generation += 1
        self.touch_batches.new_generation(self.generation)
        self._frames_stop = threading.Event()
        self._frames_worker = threading.Thread(target=self._mirror_frames,
            args=(adapter, self._frames_stop), name="minative-native-frames", daemon=True)
        self._frames_worker.start()
        return self.status()

    def _mirror_frames(self, adapter, cancelled):
        import time

        previous = None
        interval = 1.0 / 60.0
        deadline = time.monotonic()
        # Diagnostic work counters, not guest or display FPS. No per-frame I/O.
        self._frame_metrics = dict(polls=0, published=0, unchanged=0, unavailable=0,
                                   copiedBytes=0, missedDeadlines=0)
        try:
            while not cancelled.wait(max(0.0, deadline - time.monotonic())):
                if adapter.status()["state"] not in ("starting", "running"):
                    return
                try:
                    self._frame_metrics["polls"] += 1
                    try:
                        frame = adapter.read_frame(previous_header=previous)
                        if frame is None:
                            self._frame_metrics["unchanged"] += 1
                            continue
                        header, pixels = frame
                        # Confirms format, exact dimensions, stable even sequence.
                        validate_frame(header, pixels, (self.width, self.height))
                    except MError as error:
                        if error.code in ("not_running", "frame_unavailable"):
                            self._frame_metrics["unavailable"] += 1
                            continue
                        raise
                    temporary = self.root / (".frame-" + uuid.uuid4().hex)
                    try:
                        fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
                        with os.fdopen(fd, "wb") as stream:
                            stream.write(header)
                            stream.write(pixels)
                        if cancelled.is_set():
                            return
                        os.replace(temporary, self.root / "frame.raw")
                        previous = header
                        self._frame_metrics["published"] += 1
                        self._frame_metrics["copiedBytes"] += len(header) + len(pixels)
                    finally:
                        temporary.unlink(missing_ok=True)
                finally:
                    deadline += interval
                    now = time.monotonic()
                    if deadline <= now:
                        self._frame_metrics["missedDeadlines"] += 1
                        deadline = now + interval
        except (OSError, ValueError, KeyError) as error:
            self.last_error = getattr(error, "code", "frame_copy_failed")
            adapter.stop()

    def input(self, arguments):
        if not self.adapter:
            raise BridgeError("not_running", "Multi-device runner is not running")
        return self.adapter.input(arguments.get("x"), arguments.get("y"), arguments.get("down"))

    def pmu_adc_get(self, arguments):
        return get_raw(self.adapter, arguments)

    def pmu_adc_set(self, arguments):
        return set_raw(self.adapter, arguments)

    def live_get(self, arguments):
        return live_get(self.adapter, arguments)

    def live_set(self, arguments):
        return live_set(self.adapter, arguments)

    def stop(self):
        self._frames_stop.set()
        if self.adapter:
            self.adapter.stop()
        worker = self._frames_worker
        if worker and worker is not threading.current_thread():
            worker.join(timeout=2)
            if worker.is_alive():
                self.last_error = "frame_worker_stop_unconfirmed"
                return self.status()
        self._frames_worker = None
        (self.root / "frame.raw").unlink(missing_ok=True)
        return self.status()
