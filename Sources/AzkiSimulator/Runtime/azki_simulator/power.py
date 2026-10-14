"""Client-side bounded cold power transactions using existing bridge commands.

No live firmware patching, guest reboot persistence or new execution approval
is implied. Every new start still passes the selected helper's identity gate.
"""
from pathlib import Path

from .bridge import BridgeError, request


def _checked(response):
    if not isinstance(response, dict) or response.get("ok") is not True or not isinstance(response.get("result"), dict):
        raise BridgeError("power_failed", "The session did not confirm the power operation")
    return response["result"]


def power_transaction(source, operation, target=None, *, call=request):
    if operation not in ("shutdown", "restart", "switch"):
        raise BridgeError("unsupported_power", "Choose shutdown, cold restart or cold switch")
    source = Path(source)
    if operation == "switch":
        if target is None or Path(target).resolve() == source.resolve():
            raise BridgeError("invalid_target", "A cold switch needs a separate, explicitly prepared session")
        target = Path(target)
    elif target is not None:
        raise BridgeError("invalid_target", "Only a cold switch accepts a target session")
    destination = target if operation == "switch" else source
    # Preflight the destination before stopping a working source. Never grant
    # execution via this API or interpret a prepared descriptor as permission.
    if operation != "shutdown":
        candidate = _checked(call(destination, "status"))
        if candidate.get("executionApproved") is not True:
            raise BridgeError("execution_not_approved", "Destination identity has not been approved")
        if operation == "switch" and candidate.get("state") != "stopped":
            raise BridgeError("target_busy", "Cold switch requires a stopped target session")
    stopped = _checked(call(source, "stop"))
    if stopped.get("hostProcessRunning") is not False or stopped.get("state") not in ("stopped", "crashed"):
        raise BridgeError("stop_unconfirmed", "Source core termination is not confirmed; no new core was started")
    if stopped.get("backend") == "minative" and stopped.get("cleanup") not in (None, "reaped"):
        raise BridgeError("stop_unconfirmed", "Source process-group cleanup is not confirmed")
    if operation == "shutdown":
        return {"operation": operation, "mode": "host-core-stop", "coreStopped": True,
                "helperStopped": False, "guestPersistenceVerified": False}
    started = _checked(call(destination, "start"))
    return {"operation": operation, "mode": "cold-restart" if operation == "restart" else "cold-switch",
            "coreStopped": True, "destination": str(destination), "startStatus": started,
            "liveReload": False, "guestPersistenceVerified": False}
