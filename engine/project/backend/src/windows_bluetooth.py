"""Windows radio sidecar and guest ATT proxy.

WinRT terminates physical Bluetooth security/ATT. It is deliberately reported
separately from a dedicated-controller raw HCI transport. Construction is inert;
only an explicit dispatch starts the owned sidecar or a radio operation.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import threading
import time
import uuid


MAX_LINE = 1_048_576
RESERVED_SERVICES = frozenset(("1800", "1801", "180a", "1813"))


class BluetoothHostError(RuntimeError):
    def __init__(self, message, code="bluetooth_error"):
        super().__init__(message)
        self.code = code


def compact_uuid(value):
    text = str(value).lower()
    if len(text) == 36 and text.endswith("-0000-1000-8000-00805f9b34fb"):
        text = text[:8]
    if len(text) == 8 and text.startswith("0000"):
        text = text[4:]
    if len(text) in (4, 8):
        int(text, 16)
        return text
    return str(uuid.UUID(text))


class WindowsBluetoothHost:
    """One owned JSONL process, response demultiplexing, bounded event queue.

    No shell, arbitrary executable lookup or radio is created during init.
    A command timeout closes the session: mutating WinRT work cannot safely be
    retried merely because its response was not observed.
    """
    def __init__(self, executable, *, process_factory=subprocess.Popen):
        self.executable = Path(executable)
        self._process_factory = process_factory
        self._process = None
        self._responses = {}
        self._events = queue.Queue(maxsize=1024)
        self._lock = threading.Lock()
        self._write_lock = threading.Lock()
        self._failure = None
        self._thread = None
        self._event_overflows = 0
        self._closed = False

    def start(self):
        if self._closed:
            raise BluetoothHostError("This Bluetooth host session is closed", "closed")
        if self._process is not None:
            return
        if not self.executable.is_file():
            raise BluetoothHostError("Windows Bluetooth host is not built: " + str(self.executable), "host_unavailable")
        flags = getattr(subprocess, "CREATE_NO_WINDOW", 0) if os.name == "nt" else 0
        child_environment = os.environ.copy()
        bundled_dotnet = self.executable.parent.parent / "dotnet"
        if (bundled_dotnet / "dotnet.exe").is_file():
            # The apphost must use the runtime shipped beside this distribution.
            # Change only this owned child; leave the user's process/global
            # environment and any system .NET installation alone.
            runtime_root = str(bundled_dotnet.resolve())
            child_environment["DOTNET_ROOT"] = runtime_root
            child_environment["DOTNET_ROOT_X64"] = runtime_root
        self._process = self._process_factory(
            [str(self.executable)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, creationflags=flags, bufsize=0,
            env=child_environment,
        )
        self._thread = threading.Thread(target=self._read, name="azki-windows-bluetooth", daemon=True)
        self._thread.start()

    def _fail(self, error):
        with self._lock:
            self._failure = error
            waiters = list(self._responses.values())
        for waiter in waiters:
            try:
                waiter.put_nowait(error)
            except queue.Full:
                pass

    def _read(self):
        try:
            while True:
                line = self._process.stdout.readline(MAX_LINE + 1)
                if not line:
                    raise BluetoothHostError("Windows Bluetooth host exited", "host_exited")
                if len(line) > MAX_LINE or not line.endswith(b"\n"):
                    raise BluetoothHostError("Invalid Bluetooth host response length", "protocol_error")
                value = json.loads(line)
                if not isinstance(value, dict):
                    raise BluetoothHostError("Invalid Bluetooth host response", "protocol_error")
                if "event" in value:
                    try:
                        self._events.put_nowait(value)
                    except queue.Full:
                        self._event_overflows += 1
                        # Dropping a guest ATT request would lose a mutation's
                        # outcome. End this session instead of silently retrying.
                        if str(value["event"]).startswith("peripheral."):
                            raise BluetoothHostError("Peripheral event queue overflow", "event_overflow")
                elif isinstance(value.get("id"), str):
                    with self._lock:
                        waiter = self._responses.get(value["id"])
                    if waiter is not None:
                        waiter.put_nowait(value)
                else:
                    raise BluetoothHostError("Unidentified Bluetooth response", "protocol_error")
        except Exception as error:
            self._fail(error if isinstance(error, BluetoothHostError) else BluetoothHostError(str(error), "protocol_error"))

    def request(self, command, *, timeout=20, **fields):
        if not isinstance(command, str) or not (0.1 <= timeout <= 65):
            raise ValueError("Invalid Bluetooth command or timeout")
        self.start()
        identity = str(uuid.uuid4())
        waiter = queue.Queue(maxsize=1)
        with self._lock:
            if self._failure:
                raise self._failure
            self._responses[identity] = waiter
        try:
            request = {**fields, "id": identity, "command": command,
                       "timeoutMs": min(int(timeout * 1000), 60000)}
            raw = (json.dumps(request, separators=(",", ":")) + "\n").encode("utf-8")
            if len(raw) > MAX_LINE:
                raise ValueError("Bluetooth request exceeds 1 MiB")
            with self._write_lock:
                self._process.stdin.write(raw)
                self._process.stdin.flush()
            try:
                reply = waiter.get(timeout=timeout + 1)
            except queue.Empty:
                self.close()
                raise BluetoothHostError("Bluetooth operation timed out; session closed; result may be unknown", "timeout") from None
            if isinstance(reply, Exception):
                raise reply
            if reply.get("ok") is not True:
                error = reply.get("error", {})
                raise BluetoothHostError(str(error.get("message", "Bluetooth operation failed")), str(error.get("code", "bluetooth_error")))
            result = reply.get("result")
            if not isinstance(result, dict):
                raise BluetoothHostError("Bluetooth result is not an object", "protocol_error")
            return result
        finally:
            with self._lock:
                self._responses.pop(identity, None)

    def events(self, limit=128):
        if not isinstance(limit, int) or isinstance(limit, bool) or not 1 <= limit <= 1024:
            raise ValueError("Event limit must be 1..1024")
        if self._failure:
            raise self._failure
        result = []
        while len(result) < limit:
            try:
                result.append(self._events.get_nowait())
            except queue.Empty:
                break
        return result

    @property
    def dropped_scan_events(self):
        return self._event_overflows

    def close(self):
        if self._closed:
            return
        self._closed = True
        process = self._process
        if process is not None:
            try:
                process.stdin.close()  # host Dispose stops only its own radio resources
            except (OSError, ValueError):
                pass
            try:
                process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                # This exact Popen child belongs to this bridge; never enumerate
                # or terminate a system Bluetooth process/another simulator.
                process.terminate()
                process.wait(timeout=5)
            if process.stdout:
                process.stdout.close()
        if self._thread is not None and self._thread is not threading.current_thread():
            self._thread.join(timeout=1)
        self._fail(BluetoothHostError("Bluetooth host closed", "closed"))


class WindowsGuestPeripheralBridge:
    """Forward the existing external-* guest ATT protocol through WinRT.

    Physical SMP and virtual guest SMP are separate links. No link keys or
    fabricated authentication success are supplied by this adapter. A bridge
    admits one external logical peer; Windows still owns physical connections.
    """
    def __init__(self, host, guest_rpc, *, clock=time.monotonic):
        self.host = host
        self.rpc = guest_rpc
        self.clock = clock
        self.bridge_id = None
        self.peer = None
        self._pending = {}
        self._subscriptions = set()
        self._notifications = []
        self._mtu = 23
        self._maximums = {}
        self._keys = set()
        self._started = False
        self._generation = None

    def _call(self, command, **fields):
        value = {"schema": 1, "requestID": str(uuid.uuid4()), "command": command, **fields}
        if command != "external-open":
            if self.bridge_id is None:
                raise BluetoothHostError("Guest Bluetooth bridge is closed", "guest_closed")
            value["bridgeID"] = self.bridge_id
        result = self.rpc(value)
        if not isinstance(result, dict):
            raise BluetoothHostError("Invalid guest Bluetooth RPC result", "guest_protocol")
        return result

    def start(self, *, local_name="Azki", timeout=30, allow_service_omissions=False):
        if self.bridge_id is not None or isinstance(timeout, bool) or not 1 <= timeout <= 60 or not isinstance(allow_service_omissions, bool):
            raise ValueError("Bridge already opened or timeout outside 1..60")
        opened = self._call("external-open", connectionTimeoutSeconds=int(timeout))
        identity = opened.get("bridgeID")
        if not isinstance(identity, str) or not identity:
            raise BluetoothHostError("Guest did not return bridgeID", "guest_protocol")
        self.bridge_id = identity
        try:
            deadline = self.clock() + timeout
            while True:
                state = self._call("external-status")
                if state.get("state") == "failed":
                    raise BluetoothHostError(str(state.get("error") or "Guest Bluetooth failed"), "guest_unavailable")
                if state.get("state") == "ready":
                    break
                if self.clock() >= deadline:
                    raise BluetoothHostError("Guest GATT discovery timed out", "guest_timeout")
                time.sleep(0.05)
            mtu = state.get("mtu")
            if not isinstance(mtu, int) or isinstance(mtu, bool) or not 23 <= mtu <= 517:
                raise BluetoothHostError("Invalid guest ATT MTU", "guest_protocol")
            self._mtu = mtu
            services, omitted = [], []
            for service in state.get("services", []):
                if compact_uuid(service["uuid"]) in RESERVED_SERVICES:
                    omitted.append({"uuid": service["uuid"], "key": service["key"], "reason": "reserved-by-Windows"})
                else:
                    services.append(service)
            if omitted and not allow_service_omissions:
                raise BluetoothHostError("Guest contains Windows-reserved services; explicit allowServiceOmissions is required", "reserved_services")
            if not services:
                raise BluetoothHostError("Guest has no publishable Windows GATT services", "guest_services_unavailable")
            self._keys = {c["key"] for s in services for c in s["characteristics"]}
            published = self.host.request("peripheral.publish", services=services)
            self._generation = published["generation"]
            advertising = self.host.request("peripheral.start", connectable=True, discoverable=True)
            self._started = True
            return {"active": True, "bridgeID": identity, "guestMTU": mtu, "services": services,
                    "omittedServices": omitted, "requestedLocalName": local_name,
                    "physicalLocalName": "managed-by-Windows; guest name was not applied",
                    "physicalLinkSecurity": "managed-by-Windows", "guestAuthentication": "forwarded-to-guest",
                    "rawHci": False, "advertising": advertising, "radioDeliveryVerified": False}
        except BaseException:
            self.close()
            raise

    def _respond(self, operation, code=0, value=""):
        self.host.request("peripheral.respond", operationID=operation, attError=code, value=value)

    def _request(self, event):
        identity = event["operationID"]
        peer, key, kind = event["peerID"], event["key"], event["operation"]
        if self.peer is None:
            self.peer = peer
        if self.peer != peer or key not in self._keys:
            self._respond(identity, 0x11)
            return
        if len(self._pending) >= 64:
            self._respond(identity, 0x11)
            return
        if kind == "write" and event.get("offset", 0) != 0:
            self._respond(identity, 0x07)
            return
        if kind == "write" and len(bytes.fromhex(event.get("value", ""))) > self._mtu - 3:
            self._respond(identity, 0x0d)
            return
        result = self._call("external-request", operation=kind, key=key,
                            value=event.get("value", ""), offset=event.get("offset", 0), enabled=False)
        self._pending[result["operationID"]] = {"hostOperation": identity, "kind": kind,
            "maximumLength": event.get("maximumLength", 512), "deadline": self.clock() + 18}

    def _subscription(self, event):
        peer, key, enabled = event["peerID"], event["key"], event["enabled"]
        if enabled:
            if self.peer is None:
                self.peer = peer
            if self.peer != peer or key not in self._keys:
                return {"event": "guest.subscription_rejected", "data": {"peerID": peer, "key": key}}
            self._subscriptions.add(key)
            self._maximums[key] = event["maximumLength"]
        elif self.peer == peer:
            self._subscriptions.discard(key)
            self._maximums.pop(key, None)
        else:
            return None
        result = self._call("external-request", operation="subscribe", key=key, value="", offset=0, enabled=enabled)
        self._pending[result["operationID"]] = {"hostOperation": None, "kind": "subscribe", "deadline": self.clock() + 18}
        return None

    def poll(self, events):
        if not self._started:
            return events
        forwarded = []
        try:
            for event in events:
                name, data = event.get("event"), event.get("data", {})
                if name in ("peripheral.request", "peripheral.subscription") and data.get("generation") != self._generation:
                    continue
                if name == "peripheral.request":
                    self._request(data)
                elif name == "peripheral.subscription":
                    rejection = self._subscription(data)
                    if rejection:
                        forwarded.append(rejection)
                else:
                    forwarded.append(event)
            for operation, item in list(self._pending.items()):
                if self.clock() >= item["deadline"]:
                    # Submitted write/subscription may have committed. Invalidate
                    # the complete session instead of letting later work race it.
                    raise BluetoothHostError("Guest ATT operation timed out; outcome unknown; bridge closed", "guest_timeout")
                result = self._call("external-result", operationID=operation)
                if result.get("done") is not True:
                    continue
                self._pending.pop(operation)
                if "error" in result:
                    raise BluetoothHostError(str(result["error"]), "guest_error")
                if item["hostOperation"] is not None:
                    value = result.get("value", "")
                    if item["kind"] == "read":
                        value = bytes.fromhex(value)[:item["maximumLength"]].hex()
                    self._respond(item["hostOperation"], result.get("attError", 0), value)
                elif result.get("attError"):
                    raise BluetoothHostError("Guest CCCD rejected subscription", "guest_error")
            status = self._call("external-status")
            if status.get("state") != "ready":
                raise BluetoothHostError(str(status.get("error") or "Guest GATT link is no longer ready"), "guest_error")
            for item in status.get("notifications", []):
                key = item["key"]
                if key in self._subscriptions and self.peer:
                    data = bytes.fromhex(item["value"])
                    if len(data) > self._maximums.get(key, 0):
                        raise BluetoothHostError("Guest notification exceeds peer limit; value was not truncated", "notification_length")
                    self.host.request("peripheral.notify", key=key, peerID=self.peer, value=data.hex())
            return forwarded
        except BaseException:
            self.close()
            raise

    def close(self):
        identity = self.bridge_id
        self._started = False
        if identity is None:
            return
        try:
            self.host.request("peripheral.stop", timeout=3)
        finally:
            try:
                self._call("external-close")
            finally:
                self.bridge_id = None
                self._pending.clear()
                self._subscriptions.clear()
                self._maximums.clear()
                self._keys.clear()
                self.peer = None
                self._generation = None


class WindowsBluetoothService:
    """Runtime request contract: {action: 'scan.start', ...flat command fields}."""
    def __init__(self, host_executable=None, guest_rpc=None, *, host=None):
        executable = host_executable or os.environ.get("AZKI_WINDOWS_BLUETOOTH_HOST")
        self.host = host if host is not None else WindowsBluetoothHost(executable or Path(__file__).resolve().parents[3] / "windows-bluetooth/publish/AzkiBluetoothHost.exe")
        self.guest_rpc = guest_rpc
        self.bridge = None
        self._bridge_lock = threading.RLock()
        self._telemetry = queue.Queue(maxsize=1024)
        self._pump_stop = threading.Event()
        self._pump_thread = None
        self._raw_bridge = None
        self._raw_capabilities = None
        self._raw_controller = None
        self._raw_cleanup_failure = None
        self.close_report = None

    def prepare_raw_hci(self, device_path, *, reset_on_stop=True):
        """Trusted runtime API, before Popen. Never accepts a JSON-supplied PID.

        An operator-selected, already WinUSB-bound dedicated interface is
        required. No driver change or HCI command is performed by preparation.
        The descriptor's client PID environment must be merged into QEMU.
        """
        with self._bridge_lock:
            if self.bridge is not None or self._raw_bridge is not None or self._raw_cleanup_failure is not None:
                raise BluetoothHostError("A guest Bluetooth transport is already selected", "guest_transport_busy")
            if __package__:
                from .windows_bluetooth_usb import WinUsbHciTransport
                from .windows_bluetooth_hci import NamedPipeControllerBridge
            else:
                from windows_bluetooth_usb import WinUsbHciTransport
                from windows_bluetooth_hci import NamedPipeControllerBridge
            try:
                controller = WinUsbHciTransport.open(device_path)
            except BaseException as error:
                self._raw_cleanup_failure = getattr(error, "raw_cleanup", None)
                raise
            try:
                bridge = NamedPipeControllerBridge(controller, reset_on_stop=reset_on_stop)
            except BaseException as original:
                try:
                    controller.close()
                except BaseException as cleanup:
                    capabilities = controller.capabilities
                    self._raw_cleanup_failure = {
                        "controllerIdentity": capabilities.get("controllerIdentity"),
                        "exclusiveLease": capabilities.get("exclusiveLease"),
                        "resourcesQuarantined": True, "controllerClosed": False,
                        "error": "Private HCI pipe setup failed: " + str(original) + "; controller cleanup unverified: " + str(cleanup)}
                    raise BluetoothHostError(self._raw_cleanup_failure["error"], "controller_cleanup_failed") from original
                raise
            self._raw_capabilities = controller.capabilities
            self._raw_controller = controller
            self._raw_bridge = bridge
            return self.raw_hci_status()["descriptor"]

    def attach_core(self, owned_process):
        """Trusted runtime passes its actual Popen after job/ACL setup."""
        with self._bridge_lock:
            if self._raw_bridge is None:
                raise BluetoothHostError("No dedicated controller was prepared", "guest_transport_unavailable")
            self._raw_bridge.attach_core(owned_process)

    def raw_hci_status(self):
        with self._bridge_lock:
            if self._raw_bridge is None:
                if self._raw_cleanup_failure is None:
                    return None
                failure = self._raw_cleanup_failure
                identity = failure.get("controllerIdentity") or {}
                return {"descriptor": {"controllerInstanceID": identity.get("instanceID"),
                                       "controllerAddress": None, "addressSource": "not-queried"},
                        "controller": failure, "firmwareCompleteBluetooth": False,
                        "state": {"closed": True, "controllerClosed": False, "pipeClosed": None,
                                  "peerAuthenticated": False, "connected": False,
                                  "radioTeardownVerified": False, "error": failure["error"]}}
            capabilities = self._raw_controller.capabilities if self._raw_controller is not None else self._raw_capabilities or {}
            return {
                "descriptor": {**self._raw_bridge.descriptor,
                               "controllerInstanceID": capabilities.get("controllerInstanceID"),
                               "controllerAddress": capabilities.get("controllerAddress"),
                               "addressSource": capabilities.get("addressSource", "not-queried")},
                "controller": capabilities,
                "state": self._raw_bridge.stats,
                "firmwareCompleteBluetooth": False,
            }

    def _telemetry_event(self, event):
        try:
            self._telemetry.put_nowait(event)
        except queue.Full:
            # Telemetry is a copy. ATT requests never go through this queue.
            try:
                self._telemetry.get_nowait()
            except queue.Empty:
                pass
            self._telemetry.put_nowait(event)

    def _pump(self):
        while not self._pump_stop.wait(0.03):
            try:
                with self._bridge_lock:
                    if self.bridge is None:
                        return
                    for event in self.bridge.poll(self.host.events(1024)):
                        self._telemetry_event(event)
            except BaseException as error:
                with self._bridge_lock:
                    if self.bridge:
                        try:
                            self.bridge.close()
                        except Exception:
                            pass
                    self.bridge = None
                self._telemetry_event({"event": "guest.bridge_failed", "data": {
                    "code": getattr(error, "code", "guest_error"), "message": str(error)}})
                self._pump_stop.set()
                # Reader/protocol failure may prevent a normal stop command.
                # Closing only this owned child releases its WinRT resources.
                if getattr(self.host, "_failure", None):
                    self.host.close()
                return

    def _stop_bridge(self):
        self._pump_stop.set()
        thread = self._pump_thread
        if thread is not None and thread is not threading.current_thread():
            thread.join(timeout=2)
        with self._bridge_lock:
            if self.bridge:
                self.bridge.close()
                self.bridge = None
        if thread is not None and thread is not threading.current_thread():
            thread.join(timeout=2)
            if thread.is_alive():
                self.host.close()
                raise BluetoothHostError("Guest Bluetooth pump did not stop; host was closed", "guest_timeout")
        self._pump_thread = None

    def dispatch(self, args):
        if not isinstance(args, dict) or not isinstance(args.get("action"), str):
            raise ValueError("Bluetooth action is required")
        fields = dict(args)
        action = fields.pop("action")
        if action in ("hci.usb.list", "hci.usb.inspect"):
            try:
                from .windows_bluetooth_usb import dispatch_inspection
            except ImportError:
                from windows_bluetooth_usb import dispatch_inspection
            return dispatch_inspection(args)
        if action == "events":
            limit = fields.get("limit", 128)
            if not isinstance(limit, int) or isinstance(limit, bool) or not 1 <= limit <= 1024:
                raise ValueError("Event limit must be1..1024")
            events = []
            while len(events) < limit:
                try:
                    events.append(self._telemetry.get_nowait())
                except queue.Empty:
                    break
            with self._bridge_lock:
                if not self.bridge and len(events) < limit:
                    events += self.host.events(limit - len(events))
            return {"events": events, "droppedScanEvents": self.host.dropped_scan_events}
        if action in ("capabilities", "status"):
            raw = self.raw_hci_status()
            if raw is not None:
                state = raw["state"]
                return {"backend": "windows-winusb-hci", "available": not state["closed"] and not state.get("error"),
                        "rawHci": True, "guestRawHCI": state["peerAuthenticated"] and state["connected"],
                        "guestATTProxyAvailable": False, "guestPeripheralActive": False,
                        "firmwareCompleteBluetooth": False, "dedicatedController": raw,
                        "sco": False, "iso": False}
            result = self.host.request("status")
            return {**result, "backend": "windows-winrt", "guestATTProxyAvailable": self.guest_rpc is not None,
                    "guestPeripheralActive": self.bridge is not None,
                    "guestRawHCI": False, "firmwareCompleteBluetooth": False}
        if action == "peripheral.start":
            with self._bridge_lock:
                if self.bridge:
                    raise ValueError("Guest peripheral is already active")
                if self._raw_bridge is not None:
                    raise BluetoothHostError("Dedicated raw HCI already owns this guest; do not add a virtual SMP bridge", "guest_transport_busy")
                if self.guest_rpc is None:
                    raise BluetoothHostError("This device runtime has no guest ATT/HCI transport", "guest_transport_unavailable")
                bridge = WindowsGuestPeripheralBridge(self.host, self.guest_rpc)
                result = bridge.start(local_name=fields.get("localName", "Azki"), timeout=fields.get("timeout", 30),
                                      allow_service_omissions=fields.get("allowServiceOmissions", False))
                self.bridge = bridge
                self._pump_stop.clear()
                self._pump_thread = threading.Thread(target=self._pump, name="azki-windows-guest-radio", daemon=True)
                self._pump_thread.start()
            return result
        if action == "peripheral.stop":
            self._stop_bridge()
            return {"stopped": True}
        allowed = {"adapters", "scan.start", "scan.stop", "advertise.start", "advertise.stop",
                   "central.connect", "central.discover", "central.read", "central.write", "central.subscribe",
                   "central.descriptors", "central.read-descriptor", "central.write-descriptor", "central.disconnect"}
        if action not in allowed:
            raise ValueError("Unknown Windows Bluetooth action")
        return self.host.request(action, **fields)

    def close(self):
        failures = []
        for label, operation in (("guest_bridge", self._stop_bridge),
                                 ("dedicated_controller", self._raw_bridge.stop if self._raw_bridge is not None else lambda: None),
                                 ("host", self.host.close)):
            try:
                operation()
            except BaseException as error:
                failures.append((label, error))
        self.bridge = None
        raw = self.raw_hci_status()
        state = raw["state"] if raw is not None else None
        raw_closed = state is None or (state.get("closed") is True and
                                       state.get("controllerClosed") is True and
                                       state.get("pipeClosed") is not False and
                                       not state.get("error"))
        self.close_report = {
            "cleanupComplete": not failures and raw_closed,
            "dedicatedController": raw,
            "radioTeardownVerified": state.get("radioTeardownVerified", False) if state is not None else None,
            "cleanupErrors": [{"step": label, "message": str(error)[:512]} for label, error in failures],
        }
        if failures:
            raise failures[0][1]
        return self.close_report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, type=Path)
    parser.add_argument("--seconds", type=float, default=5)
    parser.add_argument("--request", default='{"action":"capabilities"}', help="One flat JSON action")
    args = parser.parse_args(argv)
    if not 0 <= args.seconds <= 60:
        parser.error("--seconds must be0..60")
    service = WindowsBluetoothService(args.host)
    try:
        print(json.dumps({"result": service.dispatch(json.loads(args.request))}), flush=True)
        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            for event in service.dispatch({"action": "events"})["events"]:
                print(json.dumps(event), flush=True)
            time.sleep(0.05)
        return 0
    except (ValueError, BluetoothHostError, OSError) as error:
        print(json.dumps({"error": str(error), "code": getattr(error, "code", "error")}), flush=True)
        return 1
    finally:
        service.close()


if __name__ == "__main__":
    raise SystemExit(main())
