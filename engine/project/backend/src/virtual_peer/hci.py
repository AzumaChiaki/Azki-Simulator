"""Truthful, bounded virtual HCI command plane. No radio or pairing claim.

Unsupported commands return Unknown HCI Command; feature bits never claim
link encryption, SSP, Secure Connections, advertising, or an ACL link.
"""
from dataclasses import dataclass
import secrets
import struct

from .crypto import CommonCryptoAES, CryptoError

SUCCESS = 0x00
UNKNOWN_COMMAND = 0x01
HARDWARE_FAILURE = 0x03
INVALID_PARAMETERS = 0x12


@dataclass(frozen=True)
class Command:
    name: str
    length: int
    supported_byte: int
    supported_bit: int
    return_bytes: int  # excludes status


# Supported-command positions cross-checked against pinned BlueZ btdev.c.
# Only handlers below are declared; no all-FF capability bitmap.
COMMANDS = {
    0x0C01: Command("Set_Event_Mask", 8, 5, 6, 0),
    0x0C03: Command("Reset", 0, 5, 7, 0),
    0x0C13: Command("Write_Local_Name", 248, 7, 0, 0),
    0x0C14: Command("Read_Local_Name", 0, 7, 1, 248),
    0x0C63: Command("Set_Event_Mask_Page_2", 8, 22, 2, 0),
    0x1001: Command("Read_Local_Version", 0, 14, 3, 8),
    0x1002: Command("Read_Local_Supported_Commands", 0, 14, 4, 64),
    0x1003: Command("Read_Local_Supported_Features", 0, 14, 5, 8),
    0x1004: Command("Read_Local_Extended_Features", 1, 14, 6, 10),
    0x1005: Command("Read_Buffer_Size", 0, 14, 7, 7),
    0x1009: Command("Read_BD_ADDR", 0, 15, 1, 6),
    0x2001: Command("LE_Set_Event_Mask", 8, 25, 0, 0),
    0x2002: Command("LE_Read_Buffer_Size", 0, 25, 1, 3),
    0x2003: Command("LE_Read_Local_Supported_Features", 0, 25, 2, 8),
    0x2017: Command("LE_Encrypt", 32, 27, 6, 16),
    0x2018: Command("LE_Rand", 0, 27, 7, 8),
    0x201C: Command("LE_Read_Supported_States", 0, 28, 3, 8),
}


def complete(opcode, status=SUCCESS, parameters=b""):
    body = struct.pack("<BHB", 1, opcode, status) + parameters
    if len(body) > 255:
        raise ValueError("Command Complete exceeds HCI event bound")
    return bytes((4, 0x0E, len(body))) + body


def supported_commands():
    bits = bytearray(64)
    for entry in COMMANDS.values():
        bits[entry.supported_byte] |= 1 << entry.supported_bit
    return bytes(bits)


class VirtualController:
    """An implemented HCI command subset; ACL/SMP are deliberately separate."""
    def __init__(self, address="02:66:00:00:00:01", crypto=None, random_bytes=None):
        parts = address.split(":") if isinstance(address, str) else []
        if len(parts) != 6 or any(len(part) != 2 or any(char not in '0123456789abcdefABCDEF' for char in part) for part in parts):
            raise ValueError("Invalid synthetic controller address")
        try:
            self.address = bytes(int(part, 16) for part in parts)
        except ValueError as error:
            raise ValueError("Invalid synthetic controller address") from error
        self.crypto = crypto if crypto is not None else CommonCryptoAES()
        self.random_bytes = random_bytes if random_bytes is not None else secrets.token_bytes
        self.reset()

    def reset(self):
        self.local_name = b"Azki Virtual Controller".ljust(248, b"\0")
        self.event_mask = bytes.fromhex("ffffffffffff1f00")
        self.event_mask_page_2 = bytes(8)
        self.le_event_mask = bytes.fromhex("1f00000000000000")

    def respond(self, opcode, params):
        if type(opcode) is not int or not 0 <= opcode <= 0xFFFF or not isinstance(params, bytes):
            raise ValueError("Invalid command object")
        entry = COMMANDS.get(opcode)
        if entry is None:
            return complete(opcode, UNKNOWN_COMMAND)
        if len(params) != entry.length:
            return complete(opcode, INVALID_PARAMETERS, bytes(entry.return_bytes))
        result = b""
        # LE support and BR/EDR-not-supported; extended features page present.
        # No classic encryption / SSP / LE link encryption claimed.
        features = bytes((0, 0, 0, 0, 0x60, 0, 0, 0x80))
        if opcode == 0x0C03:
            self.reset()
        elif opcode == 0x0C01:
            self.event_mask = params
        elif opcode == 0x0C63:
            self.event_mask_page_2 = params
        elif opcode == 0x2001:
            self.le_event_mask = params
        elif opcode == 0x0C13:
            self.local_name = params
        elif opcode == 0x0C14:
            result = self.local_name
        elif opcode == 0x1001:
            result = struct.pack("<BHBHH", 0x06, 1, 0x06, 0xFFFF, 1)
        elif opcode == 0x1002:
            result = supported_commands()
        elif opcode == 0x1003:
            result = features
        elif opcode == 0x1004:
            if params[0] > 1:
                return complete(opcode, INVALID_PARAMETERS, params + b"\1" + bytes(8))
            # Page 1 is host features. No SSP/LE-host/SC support has been
            # enabled by any host command, so report those bits cleared.
            result = params + b"\1" + (features if params[0] == 0 else bytes(8))
        elif opcode == 0x1005:
            result = struct.pack("<HBHH", 0, 0, 0, 0)  # no classic ACL/SCO path
        elif opcode == 0x1009:
            result = self.address[::-1]
        elif opcode == 0x2002:
            result = struct.pack("<HB", 0, 0)  # command-only endpoint has no ACL transport
        elif opcode in (0x2003, 0x201C):
            result = bytes(8)  # no link features/states implemented yet
        elif opcode == 0x2017:
            try:
                result = self.crypto.le_encrypt(params[:16], params[16:])
            except (CryptoError, OSError):
                return complete(opcode, HARDWARE_FAILURE, bytes(16))
            if not isinstance(result, bytes) or len(result) != 16:
                return complete(opcode, HARDWARE_FAILURE, bytes(16))
        elif opcode == 0x2018:
            try:
                result = self.random_bytes(8)
            except OSError:
                return complete(opcode, HARDWARE_FAILURE, bytes(8))
            if not isinstance(result, bytes) or len(result) != 8:
                return complete(opcode, HARDWARE_FAILURE, bytes(8))
        else:
            raise AssertionError("Declared command lacks an implementation")
        if len(result) != entry.return_bytes:
            raise AssertionError("Incorrect command return structure")
        return complete(opcode, SUCCESS, result)


class H4Commands:
    """Incremental H4 framing with per-packet and per-session quotas."""
    def __init__(self, controller, max_commands=4096):
        self.controller = controller
        self.pending = bytearray()
        self.max_commands = max_commands
        self.count = 0
        self.summary = {}
        self.events = []

    def feed(self, chunk):
        if not isinstance(chunk, bytes) or len(chunk) > 4096:
            raise ValueError("H4 read size limit")
        responses = []
        for byte in chunk:
            self.pending.append(byte)
            if self.pending[0] != 1:
                raise ValueError("Command endpoint cannot consume ACL/SCO/events; attach a link transport")
            if len(self.pending) < 4:
                continue
            if len(self.pending) != 4 + self.pending[3]:
                continue
            if self.count >= self.max_commands:
                raise ValueError("H4 command quota exceeded")
            opcode = struct.unpack_from("<H", self.pending, 1)[0]
            params = bytes(self.pending[4:])
            response = self.controller.respond(opcode, params)
            if len(self.events) < 256:
                event = {"index": self.count + 1, "opcode": f"0x{opcode:04x}",
                         "parameterBytes": len(params), "status": response[6]}
                if opcode == 0x1004 and len(params) == 1:
                    event["featurePage"] = params[0]
                if opcode == 0xFC32 and len(params) == 6:
                    event["matchesSyntheticAddressLE"] = params == self.controller.address[::-1]
                if opcode == 0xFC72 and len(params) == 7:
                    event["matchesSyntheticAddressLE"] = params[1:] == self.controller.address[::-1]
                self.events.append(event)
            self.pending.clear()
            self.count += 1
            key = (opcode, response[6])
            self.summary[key] = self.summary.get(key, 0) + 1
            responses.append(response)
        return responses

    def finish(self):
        if self.pending:
            raise ValueError("Truncated H4 command")

    def report(self):
        return [{"opcode": f"0x{opcode:04x}", "status": status, "count": count}
                for (opcode, status), count in sorted(self.summary.items())]
