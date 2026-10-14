"""Offline contract for a future CoreBluetooth peripheral/guest GATT proxy.

This module never opens a radio, socket, controller, or CoreBluetooth manager.
It is not an HCI controller, SMP implementation, MiWear authenticator or live
backend. A future native delegate supplies characteristic *instance* keys,
observed limits and callbacks. Host queue acceptance is not remote delivery.
"""
from collections import deque
from dataclasses import dataclass
from typing import Callable
import math
import re


class BLEContractError(ValueError):
    pass


def _integer(value, low, high, name):
    if type(value) is not int or not low <= value <= high:
        raise BLEContractError(name)
    return value


def _bytes(value, limit):
    if type(value) is not bytes or len(value) > limit:
        raise BLEContractError("invalid/oversized value")
    return value


def _identifier(value):
    if type(value) is not str or not re.fullmatch(r"[A-Za-z0-9_.:@-]{1,128}", value):
        raise BLEContractError("invalid instance identifier")
    return value


@dataclass(frozen=True)
class Attribute:
    # Distinguishes repeated UUIDs. Native ObjectIdentifier maps to this key.
    key: str
    readable: bool = False
    writable: bool = False
    notifiable: bool = False
    max_length: int = 512
    # Explicit application stream contract, never inferred from a UUID.
    stream: bool = False

    def __post_init__(self):
        _identifier(self.key)
        _integer(self.max_length, 1, 512, "invalid characteristic limit")
        if any(type(v) is not bool for v in
               (self.readable, self.writable, self.notifiable, self.stream)):
            raise BLEContractError("invalid characteristic flags")


# ATT characteristic property bits (Bluetooth Core, GATT characteristic declaration).
_PROPERTY_BITS = ((0x01, "broadcast"), (0x02, "read"), (0x04, "write_without_response"), (0x08, "write"),
                  (0x10, "notify"), (0x20, "indicate"), (0x40, "signed_write"), (0x80, "extended_properties"))
_MODELLED = frozenset({"read", "write", "notify"})


def classify_properties(properties):
    """Map a guest characteristic-declaration property byte onto this model.

    Returns the named properties and the subset this offline model (and the
    current native adapter) cannot represent. Unmodelled properties must make
    a schema fail closed instead of being published as "compatible".
    """
    _integer(properties, 0, 0xff, "invalid property byte")
    names = tuple(name for bit, name in _PROPERTY_BITS if properties & bit)
    return {"properties": names, "unsupported": tuple(n for n in names if n not in _MODELLED)}


@dataclass(frozen=True)
class Write:
    attribute: str
    value: bytes
    offset: int = 0


@dataclass(frozen=True)
class PendingWrite:
    ticket: int
    generation: int
    requests: tuple[Write, ...]
    started_at: float
    deadline: float


@dataclass(frozen=True)
class Notification:
    generation: int
    central: str
    attribute: str
    value: bytes


def notification_value_limit(host_maximum_update_value_length, guest_att_mtu):
    """CoreBluetooth's value limit already excludes ATT headers; subtract once."""
    host = _integer(host_maximum_update_value_length, 1, 512, "host value limit")
    guest = _integer(guest_att_mtu, 23, 517, "guest ATT MTU")
    return min(host, guest - 3, 512)


def stream_fragments(value, limit):
    """Diagnostic/application stream chunks, NOT generic GATT/SAR wire framing.

    Caller must explicitly know a stream accepts arbitrary segmentation. For
    MiWear a separately verified SAR encoder must create complete legal frames.
    Never split an atomic characteristic value or a pre-encoded SAR frame here.
    """
    _bytes(value, 65536)
    _integer(limit, 1, 512, "fragment limit")
    return tuple(value[i:i + limit] for i in range(0, len(value), limit)) or (b"",)


class OfflinePeripheralSession:
    """Single-peer, serial-callback model; all data is fixture-only and bounded.

    write replies are delayed until complete_write(), which a live adapter must
    call ONLY after the guest's result. Multi-request write batches are refused
    unless a backend supports atomic commit; this model intentionally does not.
    """
    radio_enabled = False
    transport = "offline-fixture"

    def __init__(self, attributes, *, max_packets=64, max_bytes=65536):
        if type(attributes) not in (list, tuple) or not 1 <= len(attributes) <= 128:
            raise BLEContractError("invalid attribute table")
        if any(type(a) is not Attribute for a in attributes):
            raise BLEContractError("invalid attribute")
        self.attributes = {a.key: a for a in attributes}
        if len(self.attributes) != len(attributes):
            raise BLEContractError("duplicate instance key")
        self.max_packets = _integer(max_packets, 1, 256, "packet bound")
        self.max_bytes = _integer(max_bytes, 1, 65536, "byte bound")
        self.generation = 0
        self.central = None
        self.subscriptions = set()
        self.pending = None
        self.notifications = deque()
        self.queued_bytes = 0
        self.blocked = False
        self._ticket = 0
        self._limit = 0

    def enable_radio(self):
        raise BLEContractError("real radio is not implemented or authorized by this offline model")

    def open_fixture_peer(self, central, *, host_value_limit=20, guest_att_mtu=23):
        _identifier(central)
        limit = notification_value_limit(host_value_limit, guest_att_mtu)
        if self.central is not None:
            raise BLEContractError("only one explicit fixture peer per session")
        self.generation += 1
        self.central = central
        self._limit = limit
        return self.generation

    def _peer(self, central, generation):
        if (type(central) is not str or type(generation) is not int or
                self.central is None or central != self.central or generation != self.generation):
            raise BLEContractError("unknown or stale peer")

    def _attribute(self, key):
        if type(key) is not str or key not in self.attributes:
            raise BLEContractError("unknown characteristic instance")
        return self.attributes[key]

    def subscribe(self, central, generation, key):
        self._peer(central, generation)
        if not self._attribute(key).notifiable:
            raise BLEContractError("not notifiable")
        self.subscriptions.add(key)

    def unsubscribe(self, central, generation, key):
        self._peer(central, generation)
        self.subscriptions.discard(key)
        self.notifications = deque(n for n in self.notifications if n.attribute != key)
        self.queued_bytes = sum(len(n.value) for n in self.notifications)
        # A failed native update remains blocked until native ready callback.

    def read_fixture_value(self, central, generation, key, value, offset=0, *, att_mtu=23):
        self._peer(central, generation)
        attribute = self._attribute(key)
        if not attribute.readable:
            raise BLEContractError("read not permitted")
        _bytes(value, attribute.max_length)
        _integer(offset, 0, len(value), "invalid read offset")
        _integer(att_mtu, 23, 517, "read ATT MTU")
        return value[offset:offset + att_mtu - 1]

    def begin_write(self, central, generation, requests, *, now, timeout=5.0):
        self._peer(central, generation)
        if self.pending is not None:
            raise BLEContractError("write already pending")
        # Safe until guest reliable-write Prepare/Execute semantics are mapped.
        if type(requests) not in (tuple, list) or len(requests) != 1:
            raise BLEContractError("atomic multi-write backend not available")
        request = requests[0]
        if type(request) is not Write:
            raise BLEContractError("invalid write request")
        attribute = self._attribute(request.attribute)
        if not attribute.writable:
            raise BLEContractError("write not permitted")
        _integer(request.offset, 0, 0, "long/prepared writes not supported")
        _bytes(request.value, min(attribute.max_length, self._limit, self.max_bytes))
        if type(now) not in (int, float) or not math.isfinite(now) or now < 0:
            raise BLEContractError("invalid monotonic time")
        if type(timeout) not in (int, float) or not math.isfinite(timeout) or not 0 < timeout <= 10:
            raise BLEContractError("invalid write deadline")
        self._ticket += 1
        self.pending = PendingWrite(self._ticket, generation, (request,), now, now + timeout)
        return self.pending

    def complete_write(self, central, generation, ticket, *, guest_success, now):
        self._peer(central, generation)
        if self.pending is None or ticket != self.pending.ticket or type(ticket) is not int:
            raise BLEContractError("unknown/completed write")
        if type(guest_success) is not bool or type(now) not in (int, float) or not math.isfinite(now) or now < 0:
            raise BLEContractError("invalid result")
        if now < self.pending.started_at:
            raise BLEContractError("monotonic clock moved backwards")
        pending, self.pending = self.pending, None
        return "timeout" if now >= pending.deadline else ("guest-success" if guest_success else "guest-error")

    def expire_write(self, *, now):
        if type(now) not in (int, float) or not math.isfinite(now) or now < 0:
            raise BLEContractError("invalid monotonic time")
        if self.pending is not None and now >= self.pending.deadline:
            ticket, self.pending = self.pending.ticket, None
            return ticket
        return None

    def enqueue_notification(self, central, generation, key, value, *, split_stream=False):
        self._peer(central, generation)
        attribute = self._attribute(key)
        if key not in self.subscriptions or not attribute.notifiable:
            raise BLEContractError("not subscribed")
        if type(split_stream) is not bool:
            raise BLEContractError("invalid segmentation option")
        limit = min(self._limit, attribute.max_length)
        if split_stream:
            if not attribute.stream:
                raise BLEContractError("atomic values cannot be split")
            values = stream_fragments(value, limit)
        else:
            values = (_bytes(value, limit),)
        total = sum(map(len, values))
        if len(self.notifications) + len(values) > self.max_packets or self.queued_bytes + total > self.max_bytes:
            raise BLEContractError("notification queue full")
        self.notifications.extend(Notification(generation, central, key, v) for v in values)
        self.queued_bytes += total

    def pump(self, update_value: Callable[[Notification], bool]):
        """One native serial queue; callback MUST NOT reenter this model.

        Returns host-queue acceptance count, never delivery or MiWear ACK count.
        False retains the exact head until ready(); no busy retry or data loss.
        """
        accepted = 0
        while self.notifications and not self.blocked:
            head = self.notifications[0]
            result = update_value(head)
            if type(result) is not bool:
                raise BLEContractError("update callback must return bool")
            if not result:
                self.blocked = True
                break
            self.notifications.popleft()
            self.queued_bytes -= len(head.value)
            accepted += 1
        return accepted

    def ready(self, central, generation):
        self._peer(central, generation)
        self.blocked = False

    def close_fixture_peer(self):
        self.generation += 1
        self.central = None
        self.subscriptions.clear()
        self.notifications.clear()
        self.queued_bytes = 0
        self.pending = None
        self.blocked = False
        self._limit = 0
