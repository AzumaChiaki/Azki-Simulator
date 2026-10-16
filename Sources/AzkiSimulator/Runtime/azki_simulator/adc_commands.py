"""Fixed PMU raw ADC commands through the authenticated runner input socket.

This is not a generic QMP bridge. Raw words are simulated register inputs, not
physical units or measurements. Acceptance and guest consumption stay distinct.
"""
from __future__ import annotations

import os
from pathlib import Path
import re
import socket
import stat
import time

from .minative import MError, _canonical, _decode, _inside_private, _peer_identity

DEADLINE_SECONDS = 2.0
MAX_REQUEST = 1024
MAX_RESPONSE = 4096
_SHA = re.compile(r"[0-9a-f]{64}\Z")


def supports_adc(plan):
    """Only an exact normalized declaration for this pinned core enables ADC."""
    if type(plan) is not dict or type(plan.get("identity")) is not dict:
        return False
    identity = plan["identity"]
    caps, core = identity.get("runtimeCapabilities"), identity.get("qemu")
    if type(caps) is not dict or type(core) is not dict:
        return False
    sha = core.get("sha256")
    if type(sha) is not str or not _SHA.fullmatch(sha):
        return False
    cap = caps.get("pmuAdcRaw")
    expected = {"schemaVersion": 1, "controlProtocol": "pmu.adcRaw.v1",
                "channels": 8, "minimum": 0, "maximum": 65535, "qemuSHA256": sha}
    return (type(cap) is dict and set(cap) == set(expected) and
            all(type(cap[k]) is type(v) and cap[k] == v for k, v in expected.items()))


def validate_arguments(arguments, *, write):
    if type(arguments) is not dict or set(arguments) != ({"values"} if write else set()):
        raise MError("invalid_adc", "ADC read takes no arguments; ADC write requires only values")
    if write:
        values = arguments["values"]
        if type(values) is not list or len(values) != 8 or any(
                type(v) is not int or not 0 <= v <= 65535 for v in values):
            raise MError("invalid_adc", "ADC requires exactly eight integer raw words in 0…65535")
        return {"command": "pmu.adcRaw.set", "values": list(values)}
    return {"command": "pmu.adcRaw.get"}


def _state(value):
    if type(value) is not dict or set(value) != {
            "schema", "fixtureEnabled", "version", "values", "readCount", "lastReadVersion"}:
        raise MError("adc_protocol", "Runner ADC state has an invalid schema")
    version = value["version"]
    if (type(value["schema"]) is not int or value["schema"] != 1 or
            value["fixtureEnabled"] is not True or type(version) is not int or
            not 0 <= version <= 2**64 - 1):
        raise MError("adc_protocol", "Runner ADC state is unsupported or disabled")
    for name, maximum in (("values", 65535), ("readCount", 2**64 - 1),
                           ("lastReadVersion", version)):
        words = value[name]
        if type(words) is not list or len(words) != 8 or any(
                type(v) is not int or not 0 <= v <= maximum for v in words):
            raise MError("adc_protocol", "Runner ADC counters or raw words are invalid")


def _response(payload, request):
    result = _decode(payload)
    if type(result) is not dict or set(result) != {"return"}:
        raise MError("adc_rejected", "Runner did not return an ADC acknowledgement")
    value = result["return"]
    write = request["command"] == "pmu.adcRaw.set"
    if type(value) is not dict or value.get("accepted") is not write or value.get("guestObserved") is not False:
        raise MError("adc_protocol", "Runner ADC acknowledgement is invalid")
    if set(value) == {"accepted", "guestObserved", "stateUnavailable"}:
        if not write or value["stateUnavailable"] is not True:
            raise MError("adc_protocol", "Only an acknowledged write can lack a state snapshot")
        return value
    if set(value) != {"accepted", "guestObserved", "state"}:
        raise MError("adc_protocol", "Runner ADC acknowledgement has unexpected fields")
    _state(value["state"])
    if write and value["state"]["values"] != request["values"]:
        raise MError("adc_protocol", "Returned ADC values differ from the acknowledged write")
    return value


def _remaining(deadline):
    seconds = deadline - time.monotonic()
    if seconds <= 0:
        raise MError("runner_timeout", "Command exceeded its two-second total deadline")
    return seconds


def _request(adapter, arguments, *, write):
    deadline = time.monotonic() + DEADLINE_SECONDS
    request = validate_arguments(arguments, write=write)
    if adapter is not None and not supports_adc(adapter.plan):
        raise MError("unsupported_hardware", "This exact core does not declare pmu.adcRaw support")
    try:
        result = _response(runner_exchange(adapter, request, write=write, deadline=deadline), request)
        _remaining(deadline)
        return result
    except MError as error:
        renamed = {"runner_outcome_unknown": ("adc_outcome_unknown", "ADC write may have been applied; state is unconfirmed. Do not automatically retry."),
                   "runner_timeout": ("adc_timeout", "ADC command exceeded its two-second total deadline"),
                   "runner_protocol": ("adc_protocol", str(error))}
        if error.code in renamed:
            raise MError(*renamed[error.code]) from error
        raise


def runner_exchange(adapter, request, *, write, deadline):
    """Send one fixed request to this instance's own runner input socket.

    Returns the raw response line. Shared by the narrow ADC and live-input
    commands; never a generic QMP path."""
    payload = _canonical(request) + b"\n"
    if len(payload) > MAX_REQUEST:
        raise MError("invalid_request", "Runner request exceeds 1 KiB")
    if adapter is None:
        raise MError("not_running", "This input requires a running instance")
    if not adapter._lock.acquire(timeout=_remaining(deadline)):
        raise MError("runner_timeout", "Command timed out waiting for this instance")
    sent = False
    try:
        if (adapter._stop_requested or adapter.process is None or
                adapter.process.poll() is not None or adapter._control is None):
            raise MError("not_running", "Instance input is not ready or is stopping")
        # This revalidates actual runner/core/source identity and control paths.
        control = adapter._read_control()
        path = _inside_private(control["input_socket"], adapter.run / "tmp")
        if path.name != "input.sock" or not path.parent.name.startswith("q66-qmp-"):
            raise MError("unsafe_control", "Input endpoint is not the fixed runner input socket")
        before = path.lstat()
        if not stat.S_ISSOCK(before.st_mode) or before.st_uid != os.getuid() or before.st_mode & 0o077:
            raise MError("unsafe_control", "Input endpoint must be this user's private socket")
        runner_pid = adapter._runner_pid()
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
            connection.settimeout(_remaining(deadline))
            connection.connect(str(path))
            if _peer_identity(connection) != (runner_pid, os.getuid()):
                raise MError("peer_identity", "Input socket peer is not this instance's owned runner")
            after = path.lstat()
            if (before.st_dev, before.st_ino) != (after.st_dev, after.st_ino):
                raise MError("peer_identity", "Input socket changed while connecting")
            connection.settimeout(_remaining(deadline))
            # Once any write could reach the peer, failed transport is uncertain.
            # Never retry automatically: QOM may already have accepted the set.
            sent = True
            connection.sendall(payload)
            received = bytearray()
            while b"\n" not in received:
                connection.settimeout(_remaining(deadline))
                data = connection.recv(MAX_RESPONSE + 1 - len(received))
                if not data:
                    raise MError("runner_protocol", "Runner closed without a response")
                received.extend(data)
                if len(received) > MAX_RESPONSE:
                    raise MError("runner_protocol", "Runner response exceeds 4 KiB")
            if received.count(b"\n") != 1 or not received.endswith(b"\n"):
                raise MError("runner_protocol", "Runner sent multiple or trailing messages")
            return bytes(received[:-1])
    except (OSError, ValueError, KeyError, TypeError, RecursionError) as error:
        if write and sent:
            raise MError("runner_outcome_unknown", "Write may have been applied; state is unconfirmed. Do not automatically retry.") from error
        if isinstance(error, socket.timeout):
            raise MError("runner_timeout", "Command exceeded its two-second total deadline") from error
        raise
    finally:
        adapter._lock.release()


def get_raw(adapter, arguments):
    return _request(adapter, arguments, write=False)


def set_raw(adapter, arguments):
    return _request(adapter, arguments, write=True)
