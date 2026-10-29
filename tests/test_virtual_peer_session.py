"""The virtual peer's own half has to work without a watch, a radio or firmware.

The Linux port replaced the peer's Apple crypto with libcrypto and its peer
credential check with SO_PEERCRED. Those two paths are the first ones a real
pairing needs, and until now nothing ran them without a booted watch: the
macOS side had no end-to-end test either, so both platforms agreed only with
themselves. This file drives the peer's own machinery with a synthetic
peripheral and a synthetic package, so what can be proven without firmware is
pinned on both hosts, and what cannot is skipped with the reason written down.

Proven here: the peer answers on its own unix socket, the kernel peer
credential gates the guest transport, the SMP Just Works dialogue completes
against a peripheral whose confirm comes from an independent implementation of
the specification, the CCM/MIC link layer refuses tampered frames, and a
synthetic package binds to its receipt.

Not covered, and why: the HCI/L2CAP/ATT transport and the delivery of a package
to a guest need a real watch, so the peer refuses those requests here instead
of pretending; a peer client owned by another user cannot be produced without
a second account, so that refusal is skipped rather than simulated.
"""
import hashlib
import json
import os
from pathlib import Path
import socket
import sys
import tempfile
import time
import unittest
import uuid
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "engine/project/backend/src"))

from virtual_peer.service import PeerService  # noqa: E402
from virtual_peer.smp import LegacyCentral, VirtualAir  # noqa: E402
from virtual_peer.state import create_state, encode_state  # noqa: E402
from virtual_peer.virtual_packages import PackageError, load_staged, stage_watchface  # noqa: E402


def _gf_multiply(left, right):
    result = 0
    for _ in range(8):
        if right & 1:
            result ^= left
        high = left & 0x80
        left = (left << 1) & 0xFF
        if high:
            left ^= 0x1B
        right >>= 1
    return result


def _inverse(value):
    if value == 0:
        return 0
    for candidate in range(1, 256):
        if _gf_multiply(value, candidate) == 1:
            return candidate
    raise AssertionError("no multiplicative inverse")


def _rotl8(value, shift):
    return ((value << shift) | (value >> (8 - shift))) & 0xFF


def _build_sbox():
    table = []
    for index in range(256):
        inverse = _inverse(index)
        table.append(inverse ^ _rotl8(inverse, 1) ^ _rotl8(inverse, 2)
                     ^ _rotl8(inverse, 3) ^ _rotl8(inverse, 4) ^ 0x63)
    return bytes(table)


SBOX = _build_sbox()


def _expand_key(key):
    words = [list(key[index * 4:index * 4 + 4]) for index in range(4)]
    rcon = 1
    for index in range(4, 44):
        temp = list(words[index - 1])
        if index % 4 == 0:
            temp = temp[1:] + temp[:1]
            temp = [SBOX[byte] for byte in temp]
            temp[0] ^= rcon
            rcon = _gf_multiply(rcon, 2)
        words.append([words[index - 4][byte] ^ temp[byte] for byte in range(4)])
    return [sum(words[index * 4:index * 4 + 4], []) for index in range(11)]


def _add_round_key(state, round_key):
    return [byte ^ key_byte for byte, key_byte in zip(state, round_key)]


def _shift_rows(state):
    result = list(state)
    for row in range(1, 4):
        for column in range(4):
            result[row + 4 * column] = state[row + 4 * ((column + row) % 4)]
    return result


def _mix_columns(state):
    result = list(state)
    for column in range(4):
        a = state[4 * column:4 * column + 4]
        doubled = [_gf_multiply(byte, 2) for byte in a]
        tripled = [_gf_multiply(byte, 3) for byte in a]
        result[4 * column + 0] = doubled[0] ^ tripled[1] ^ a[2] ^ a[3]
        result[4 * column + 1] = a[0] ^ doubled[1] ^ tripled[2] ^ a[3]
        result[4 * column + 2] = a[0] ^ a[1] ^ doubled[2] ^ tripled[3]
        result[4 * column + 3] = tripled[0] ^ a[1] ^ a[2] ^ doubled[3]
    return result


def reference_aes(key, block):
    """AES-128 encryption from the specification, with no system library."""
    schedule = _expand_key(key)
    state = _add_round_key(list(block), schedule[0])
    for round_index in range(1, 10):
        state = _shift_rows([SBOX[byte] for byte in state])
        state = _mix_columns(state)
        state = _add_round_key(state, schedule[round_index])
    state = _shift_rows([SBOX[byte] for byte in state])
    return bytes(_add_round_key(state, schedule[10]))


def reference_le(key, data):
    """The little-endian byte order the HCI LE Encrypt command uses."""
    return reference_aes(key[::-1], data[::-1])[::-1]


def reference_c1(key, random, request, response, initiator, responder):
    """Bluetooth c1 confirm value, written out from the specification."""
    p1 = bytes((0, 0)) + request + response
    inner = bytes(left ^ right for left, right in zip(random, p1))
    outer = bytes(left ^ right for left, right in
                  zip(reference_le(key, inner), responder + initiator + bytes(4)))
    return reference_le(key, outer)


FIPS_KEY = bytes.fromhex("000102030405060708090a0b0c0d0e0f")
FIPS_PLAINTEXT = bytes.fromhex("00112233445566778899aabbccddeeff")
FIPS_CIPHERTEXT = bytes.fromhex("69c4e0d86a7b0430d8cdb78070b4c55a")

PEER_ADDRESS = bytes.fromhex("a00000000002")
GUEST_ADDRESS = bytes.fromhex("aabbccddeeff")
# The legacy Just Works flow derives one temporary key on both sides, so the
# link layer sees the same link key at both endpoints.
LINK_KEY = bytes(range(16))


class ReferenceCryptoTests(unittest.TestCase):
    """The oracle has to be right before it is allowed to judge the peer."""

    def test_the_reference_aes_matches_fips_197(self):
        self.assertEqual(reference_aes(FIPS_KEY, FIPS_PLAINTEXT), FIPS_CIPHERTEXT)

    def test_the_reference_c1_agrees_with_the_shipped_one(self):
        from virtual_peer.smp import c1 as shipped_c1
        key = bytes(16)
        random = bytes(range(16))
        request = bytes((1, 3, 0, 1, 16, 0, 1))
        response = bytes((2, 1, 0, 3, 16, 0, 1))
        self.assertEqual(reference_c1(key, random, request, response, PEER_ADDRESS, GUEST_ADDRESS[::-1]),
                         shipped_c1(key, random, request, response, PEER_ADDRESS, GUEST_ADDRESS[::-1]))


class FakeGuestController:
    """The peer's model of the guest link, without an HCI transport.

    The peer asks for the guest's link key and the guest answers with the same
    key it derived, which is what makes the virtual air link verifiable in
    both directions.
    """

    def __init__(self, address=GUEST_ADDRESS):
        self.address = address
        self.connected = True
        self.pending_key = None
        self.air = None
        self.requests = []

    def request_link_key(self, expected, random, ediv):
        self.pending_key = expected
        self.requests.append((expected, random, ediv))
        self.air = VirtualAir(expected, expected)


class SmpDialogueTests(unittest.TestCase):
    """The peer's SMP central against a synthetic peripheral."""

    def peripheral(self, central):
        """Answer the peer's pairing request the way a peripheral would."""
        sent = central.receive(b"\x0b\x00")
        self.assertEqual(len(sent), 1)
        channel, request = sent[0]
        self.assertEqual(channel, 6)
        self.assertEqual(request, bytes((1, 3, 0, 1, 16, 0, 1)))
        return request

    def test_the_just_works_dialogue_completes_and_stores_the_bond(self):
        controller = FakeGuestController()
        central = LegacyCentral(controller)
        request = self.peripheral(central)

        response = bytes((2, 1, 0, 3, 16, 0, 1))
        sent = central.receive(response)
        self.assertEqual(len(sent), 1)
        peer_confirm = sent[0][1]
        self.assertEqual(peer_confirm[0], 3)

        guest_random = bytes(range(16, 32))
        guest_confirm = reference_c1(bytes(16), guest_random, request, response,
                                     PEER_ADDRESS, GUEST_ADDRESS[::-1])
        sent = central.receive(bytes((3,)) + guest_confirm)
        self.assertEqual(sent[0][1][0], 4)
        self.assertEqual(central.state, "random")

        central.receive(bytes((4,)) + guest_random)
        self.assertEqual(central.state, "keys")
        self.assertEqual(len(controller.requests), 1)
        link_key, random, ediv = controller.requests[0]
        self.assertEqual(len(link_key), 16)
        self.assertEqual(len(random), 8)
        self.assertEqual(ediv, 0)

        # The guest distributes its key material; the bond is what survives.
        central.receive(bytes((6,)) + link_key)
        central.receive(bytes((7,)) + (7).to_bytes(2, "little") + bytes(range(8)))
        self.assertEqual(central.state, "bonded")
        self.assertEqual(central.bond[0], link_key)
        self.assertEqual(central.bond[2], 7)
        verified = [event for event in central.events if event.get("guestConfirmVerified")]
        self.assertEqual(len(verified), 1)
        self.assertFalse(verified[0]["mitmProtected"])

    def test_the_peer_confirm_equals_the_independent_implementation(self):
        controller = FakeGuestController()
        central = LegacyCentral(controller)
        request = self.peripheral(central)
        response = bytes((2, 1, 0, 3, 16, 0, 1))
        sent = central.receive(response)
        expected = reference_c1(bytes(16), central.random, request, response,
                                PEER_ADDRESS, GUEST_ADDRESS[::-1])
        self.assertEqual(sent[0][1][1:], expected)

    def test_a_wrong_guest_confirm_is_refused(self):
        controller = FakeGuestController()
        central = LegacyCentral(controller)
        self.peripheral(central)
        central.receive(bytes((2, 1, 0, 3, 16, 0, 1)))
        central.receive(bytes((3,)) + bytes(16))
        with self.assertRaises(ValueError) as error:
            central.receive(bytes((4,)) + bytes(range(16, 32)))
        self.assertIn("confirm mismatch", str(error.exception))

    def test_the_link_key_is_requested_only_after_the_confirm_is_verified(self):
        controller = FakeGuestController()
        central = LegacyCentral(controller)
        self.peripheral(central)
        central.receive(bytes((2, 1, 0, 3, 16, 0, 1)))
        central.receive(bytes((3,)) + bytes(16))
        self.assertEqual(controller.requests, [])
        with self.assertRaises(ValueError):
            central.receive(bytes((4,)) + bytes(range(16, 32)))
        self.assertEqual(controller.requests, [])


class VirtualAirTests(unittest.TestCase):
    """The link layer seals frames and proves them in both directions."""

    def test_a_payload_survives_both_directions(self):
        air = VirtualAir(LINK_KEY, LINK_KEY)
        # The constructor already proved one frame per direction, so the
        # counters start at one.
        self.assertEqual(air.count, [1, 1])
        sealed = air.transfer(0, 2, b"azki payload")
        self.assertEqual(sealed, b"azki payload")
        self.assertEqual(air.transfer(1, 2, b"answer"), b"answer")
        self.assertEqual(air.count, [2, 2])

    def test_the_evidence_keeps_hashes_instead_of_keys(self):
        air = VirtualAir(LINK_KEY, LINK_KEY)
        air.transfer(0, 2, b"azki payload")
        record = air.records[-1]
        self.assertTrue(record["micVerified"])
        self.assertEqual(len(record["ciphertextSHA256"]), 64)
        self.assertNotIn("key", record)

    def test_a_tampered_frame_is_refused(self):
        air = VirtualAir(LINK_KEY, LINK_KEY)
        nonce = (0).to_bytes(5, "little") + air.iv
        sealed = air.aes.ccm_encrypt(air.peer_key, nonce, b"azki payload", bytes((2,)), 4)
        tampered = bytes([sealed[0] ^ 1]) + sealed[1:]
        with self.assertRaises(ValueError):
            air.aes.ccm_decrypt(air.guest_key, nonce, tampered, bytes((2,)), 4)

    def test_two_different_endpoint_keys_cannot_build_the_link(self):
        # The class comment promises two independent endpoint keys, but one
        # crossing seals with one key and opens with the other inside a single
        # call, so the MIC only verifies when both endpoints derive the same
        # key. That is the case in the legacy Just Works flow, where the
        # temporary key is the link key and both sides derive it from s1.
        # Recorded here so the divergence between the comment and the code is
        # visible instead of silent.
        with self.assertRaises(ValueError) as error:
            VirtualAir(bytes(range(16)), bytes(range(16, 32)))
        self.assertIn("CCM authentication failed", str(error.exception))


class PeerSocketTests(unittest.TestCase):
    """The peer service on its own unix socket, without a guest."""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="azki-peer-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.socket_directory = self.root / "socket"
        self.run_directory = self.root / "run"
        self.socket_directory.mkdir(mode=0o700)
        self.run_directory.mkdir(mode=0o700)
        self.state = create_state(str(uuid.uuid4()))
        raw = encode_state(self.state)
        self.service = PeerService(str(self.socket_directory), str(self.run_directory), self.state,
                                   str(self.run_directory / "virtual-peer-state.pending.json"),
                                   hashlib.sha256(raw).hexdigest())
        self.service.start()
        self.addCleanup(self.service.close)

    def ask(self, payload):
        client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        client.settimeout(20)
        client.connect(str(self.service.peer_path))
        try:
            client.sendall(json.dumps(payload).encode() + b"\n")
            data = b""
            while not data.endswith(b"\n"):
                chunk = client.recv(65536)
                if not chunk:
                    break
                data += chunk
        finally:
            client.close()
        self.assertTrue(data, "the peer closed the socket without answering")
        return json.loads(data)

    def status_request(self):
        return {"schema": 1, "requestID": str(uuid.uuid4()), "command": "status"}

    def test_the_peer_answers_a_status_request(self):
        response = self.ask(self.status_request())
        self.assertTrue(response["ok"])
        result = response["result"]
        self.assertEqual(result["profileID"], "q66@4.100.177")
        self.assertIn("status", result["supportedOperations"])
        self.assertFalse(result["authenticated"])

    def test_an_unknown_command_is_refused_without_leaking_the_reason(self):
        response = self.ask({"schema": 1, "requestID": str(uuid.uuid4()), "command": "rm"})
        self.assertFalse(response["ok"])
        self.assertEqual(response["error"]["code"], "request_rejected")
        self.assertNotIn("Unknown peer command", json.dumps(response))

    def test_a_replayed_request_returns_the_same_answer(self):
        payload = self.status_request()
        first = self.ask(payload)
        second = self.ask(payload)
        self.assertEqual(first, second)

    def test_a_reused_identity_with_different_bytes_is_refused(self):
        payload = self.status_request()
        self.ask(payload)
        payload["command"] = "status"
        payload["extra"] = 1
        response = self.ask(payload)
        self.assertFalse(response["ok"])

    def test_the_guest_transport_refuses_a_foreign_process(self):
        # The guest socket is only for the assigned core process; the kernel
        # peer credential decides, so a second process cannot take the
        # transport over. The runner assigns the core pid first, so the
        # refusal has to come from the credential and not from an unassigned
        # transport. A refusal closes the connection, so the read has to reach
        # the end instead of waiting for data that never comes.
        self.service.attach_core(0x7FFFFFFF)
        client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        client.settimeout(20)
        client.connect(str(self.service.hci_path))
        try:
            data = client.recv(16)
        except ConnectionResetError:
            data = b""
        except socket.timeout:
            self.fail("the guest transport kept a foreign process connected")
        finally:
            client.close()
        self.assertEqual(data, b"")

    def test_the_assigned_core_process_may_take_the_guest_transport(self):
        # The other half of the same check: the process the runner named has
        # to be able to take the transport over.
        self.service.attach_core(os.getpid())
        client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        client.settimeout(20)
        client.connect(str(self.service.hci_path))
        self.addCleanup(client.close)
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline and self.service.connection is None:
            time.sleep(0.05)
        self.assertIsNotNone(self.service.connection, "the assigned core process was refused")

    def test_a_stale_session_generation_is_refused(self):
        payload = {"schema": 1, "requestID": str(uuid.uuid4()), "command": "connect",
                   "sessionID": "0" * 32, "generation": 99}
        response = self.ask(payload)
        self.assertFalse(response["ok"])
        self.assertEqual(response["error"]["code"], "stale_session")

    def test_a_peer_client_of_another_user_is_refused(self):
        if os.getuid() != 0:
            self.skipTest("需要另一个账号才能构造跨用户连接；单用户环境无法制造")
        raise unittest.SkipTest("跨用户连接需要第二个账号，本机没有可用账号")


class StagedPackageTests(unittest.TestCase):
    """A synthetic package binds to its receipt, and nothing else."""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="azki-package-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.run_directory = self.root / "run"
        self.run_directory.mkdir(mode=0o700)
        self.source = self.root / "quickapp.rpk"
        with zipfile.ZipFile(self.source, "w") as archive:
            archive.writestr("manifest.json", json.dumps(
                {"package": "com.azki.demo", "versionCode": 7, "name": "Demo",
                 "versionName": "1.0", "minPlatformVersion": 4, "deviceTypeList": ["wearable"]}))

    def stage(self):
        return stage_watchface(str(self.source), str(self.run_directory),
                               profile_id="q66@4.100.177")

    def test_a_synthetic_package_stages_and_binds_to_its_receipt(self):
        receipt = self.stage()
        self.assertEqual(receipt["status"], "staged")
        self.assertTrue(receipt["filename"].endswith(".bin"))
        report = load_staged(str(self.run_directory), receipt["filename"], receipt["sha256"],
                             profile_id="q66@4.100.177")
        self.assertEqual(report["format"], "rpk")
        self.assertEqual(report["metadata"]["packageName"], "com.azki.demo")
        self.assertEqual(report["metadata"]["versionCode"], 7)
        self.assertFalse(report["guestInstalled"])
        self.assertFalse(report["signatureVerified"])

    def test_a_changed_digest_is_refused(self):
        receipt = self.stage()
        with self.assertRaises(PackageError) as error:
            load_staged(str(self.run_directory), receipt["filename"], "0" * 64,
                        profile_id="q66@4.100.177")
        self.assertEqual(error.exception.code, "source_changed")

    def test_a_foreign_receipt_is_refused(self):
        receipt = self.stage()
        packages = self.run_directory / "packages"
        target = packages / receipt["filename"]
        target.write_bytes(b"PK\x03\x04tampered")
        with self.assertRaises(PackageError):
            load_staged(str(self.run_directory), receipt["filename"], receipt["sha256"],
                        profile_id="q66@4.100.177")

    def test_an_unknown_package_name_is_refused(self):
        with self.assertRaises(PackageError) as error:
            load_staged(str(self.run_directory), "../../etc/passwd", "0" * 64,
                        profile_id="q66@4.100.177")
        self.assertEqual(error.exception.code, "unsafe_path")


if __name__ == "__main__":
    unittest.main()
