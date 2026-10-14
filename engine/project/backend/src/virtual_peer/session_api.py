"""Stable in-process contract for the authenticated guest protocol backend.

This module contains types and a structural interface only. Instantiating an
implementation must never infer authentication from transport ACKs. Business
payloads are plaintext; the backend owns encryption and secret material.
"""
from dataclasses import dataclass
from typing import Protocol

CHANNEL_PROTOBUF = 1
CHANNEL_MASS = 2
OPCODE_WRITE = 1
OPCODE_WRITE_ENCRYPTED = 2
MAX_MESSAGE_BYTES = 65535


@dataclass(frozen=True)
class SessionStatus:
    session_id: str
    generation: int
    authenticated: bool
    transport_ready: bool
    remote_mps: int
    backend_kind: str  # actual implementation source: 'guest' or 'fixture'
    send_window: int = 1


@dataclass(frozen=True)
class SessionEvent:
    session_id: str
    generation: int
    kind: str  # transport_ack, business_message, disconnected, error
    channel: int = 0
    opcode: int = 0
    payload: bytes = b''
    request_id: str = ''
    error: str = ''


class SessionAPI(Protocol):
    def status(self) -> SessionStatus: ...

    def submit(self, channel: int, opcode: int, payload: bytes, *, expected_generation: int) -> str:
        """Fail closed unless this generation has real DeviceConfirm success.

        Protobuf requires opcode2; MASS requires opcode1 but remains behind the
        same authentication guard. Queue at most eight bounded submissions.
        Transport ACK events do not imply business acceptance.
        """
        ...

    def poll(self) -> list[SessionEvent]:
        """Return only this backend's bounded events; no secret material."""
        ...
