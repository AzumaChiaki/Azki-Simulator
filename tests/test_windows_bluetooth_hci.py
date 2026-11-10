"""Fake controller + owned loopback TCP; no Windows radio or other process I/O."""
import importlib.util
from pathlib import Path
import queue
import socket
import sys
import threading
import time
import unittest
from unittest.mock import patch

PATH = Path(__file__).resolve().parents[1] / "engine/project/backend/src/windows_bluetooth_hci.py"
SPEC = importlib.util.spec_from_file_location("azki_hci_bridge_tested", PATH)
hci = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = hci
SPEC.loader.exec_module(hci)

COMMAND = bytes.fromhex("01 09 10 00")
ACL = bytes.fromhex("02 01 20 05 00 68 65 6c 6c 6f")
EVENT = bytes.fromhex("04 0e 04 01 03 0c 00")
VENDOR = bytes.fromhex("01 77 fc 00")
LOCAL_UNKNOWN = bytes.fromhex("04 0e 04 01 77 fc 01")
COMMAND_COMPLETE = bytes.fromhex("04 0e 04 02 09 10 00")
ADDRESS_COMPLETE = bytes.fromhex('04 0e 0a 01 09 10 00 bc 9a 78 56 34 12')


def wait_for(predicate, timeout=1.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if predicate():
            return
        time.sleep(.001)
    raise AssertionError("Owned test session did not reach the expected state")


def receive_exact(stream, count):
    data = bytearray()
    while len(data) < count:
        value = stream.recv(count - len(data))
        if not value:
            raise AssertionError("Owned guest stream ended early")
        data.extend(value)
    return bytes(data)


class FakeController:
    def __init__(self):
        self.queue = queue.Queue()
        self.writes = []
        self.closed = 0
        self.reset_reply = EVENT
        self.auto_reset = False
        self.send_error = False
        self.receive_error = False
        self._lock = threading.Lock()

    def send_h4(self, packet):
        with self._lock:
            self.writes.append(packet)
        if self.send_error:
            raise OSError("Fake terminal USB TX failure")
        if self.auto_reset and packet == hci.RESET and self.reset_reply is not None:
            self.queue.put(self.reset_reply)

    def receive(self, timeout):
        if self.receive_error:
            raise OSError("Fake terminal USB RX failure")
        try:
            return self.queue.get(timeout=max(0, timeout))
        except queue.Empty:
            return None

    def close(self):
        with self._lock:
            self.closed += 1


class FakeCore:
    pid = 1234
    def poll(self):
        return None


class FakePipe:
    chardev_argument = 'pipe,id=radio,path=AzkiSimulator-HCI-' + 'a' * 32
    def __init__(self):
        self.process = None
        self.connected = False
        self.available = True
        self.connect_error = None
        self.read_error = None
        self.close_error = None
        self.read_queue = queue.Queue()
        self.closed = 0
        self.io_threads = []
        self.connect_count = 0
        self.held_prefix = None
        self.output = bytearray()

    def attach_core(self, process):
        if self.process is not None or type(getattr(process, 'pid', None)) is not int or not callable(getattr(process, 'poll', None)):
            raise ValueError('An owned core object is required exactly once')
        if process.pid <= 0 or process.poll() is not None:
            raise ValueError('Owned core is not running')
        self.process = process

    def connect(self):
        self.io_threads.append(threading.get_ident())
        self.connect_count += 1
        if self.connect_error:
            raise self.connect_error
        self.connected = self.available
        return self.connected

    def read(self):
        self.io_threads.append(threading.get_ident())
        if self.read_error:
            raise self.read_error
        try:
            return self.read_queue.get_nowait()
        except queue.Empty:
            return b''

    def flush(self, pending):
        self.io_threads.append(threading.get_ident())
        if self.held_prefix is None:
            if pending:
                self.held_prefix = bytes(pending[:7])
            return 0
        if bytes(pending[:len(self.held_prefix)]) != self.held_prefix:
            raise AssertionError('In-flight pipe write prefix was mutated')
        count = min(2, len(self.held_prefix))
        self.output.extend(self.held_prefix[:count])
        self.held_prefix = None
        return count

    def close(self):
        self.io_threads.append(threading.get_ident())
        self.closed += 1
        if self.close_error:
            raise self.close_error


class BridgeTests(unittest.TestCase):
    def make(self, reset=False, controller=None):
        controller = controller or FakeController()
        bridge = hci.ControllerBridge(controller, reset_on_stop=reset, teardown_timeout=.04)
        self.addCleanup(bridge.stop)
        return bridge, controller

    def connect(self, bridge):
        peer = socket.create_connection(("127.0.0.1", bridge.descriptor["port"]), timeout=1)
        self.addCleanup(peer.close)
        wait_for(lambda: bridge.stats["connectionAccepted"])
        return peer

    def test_constructor_requires_explicit_reset_choice(self):
        controller = FakeController()
        with self.assertRaises(TypeError):
            hci.ControllerBridge(controller)
        with self.assertRaises(ValueError):
            hci.ControllerBridge(controller, reset_on_stop=1)
        self.assertEqual(controller.writes, [])
        self.assertEqual(controller.closed, 0)

    def test_descriptor_is_loopback_single_session_and_close_owns_only_controller(self):
        bridge, controller = self.make()
        descriptor = bridge.descriptor
        self.assertEqual(descriptor["host"], "127.0.0.1")
        self.assertIn("host=127.0.0.1,port=" + str(descriptor["port"]), descriptor["qemuChardev"])
        self.assertEqual(descriptor["machineOption"], "bt-hci-chardev=radio")
        self.assertFalse(descriptor["peerAuthenticated"])
        result = bridge.stop()
        self.assertTrue(result["closed"])
        self.assertFalse(result["threadAlive"])
        self.assertIsNone(result["pipeClosed"])
        self.assertEqual(controller.closed, 1)

        self.assertEqual(controller.writes, [])
        bridge.stop()
        self.assertEqual(controller.closed, 1)

    def test_split_guest_command_and_acl_keep_exact_content_and_counts(self):
        bridge, controller = self.make()
        peer = self.connect(bridge)
        for value in COMMAND:
            peer.sendall(bytes((value,)))
        peer.sendall(ACL[:3])
        time.sleep(.005)
        peer.sendall(ACL[3:])
        wait_for(lambda: bridge.stats["txPackets"] == 2)
        self.assertEqual(controller.writes, [COMMAND, ACL])
        result = bridge.stats
        self.assertEqual(result["txAttempts"], 2)
        self.assertEqual(result["txBytes"], len(COMMAND) + len(ACL))
        self.assertEqual(result["guestWireBytesReceived"], len(COMMAND) + len(ACL))

    def test_actual_tcp_rx_partial_sends_preserve_h4_frames(self):
        bridge, controller = self.make()
        original = bridge._send_guest
        call_count = [0]
        def partial(stream, data):
            call_count[0] += 1
            if call_count[0] == 1:
                raise BlockingIOError()
            return original(stream, data[:2])
        bridge._send_guest = partial
        peer = self.connect(bridge)
        controller.queue.put(EVENT); controller.queue.put(ACL)
        self.assertEqual(receive_exact(peer, len(EVENT) + len(ACL)), EVENT + ACL)
        wait_for(lambda: bridge.stats["rxPackets"] == 2)
        result = bridge.stats
        self.assertEqual(result["rxBytes"], len(EVENT) + len(ACL))
        self.assertEqual(result["guestWireBytesSent"], result["rxBytes"])
        self.assertEqual(result["controllerRxPackets"], 2)
        self.assertGreater(call_count[0], 3)

    def test_guest_close_is_terminal_and_never_accepts_a_new_guest(self):
        bridge, controller = self.make()
        peer = self.connect(bridge)
        peer.sendall(COMMAND)
        wait_for(lambda: bridge.stats["txPackets"] == 1)
        peer.close()
        wait_for(lambda: bridge.stats["closed"])
        self.assertFalse(bridge.stats["connected"])
        with self.assertRaises(OSError):
            other = socket.create_connection(("127.0.0.1", bridge.descriptor["port"]), timeout=.1)
            other.close()
        self.assertEqual(controller.writes, [COMMAND])

    def test_sco_iso_invalid_lengths_and_truncated_disconnect_are_rejected(self):
        for packet in (b"\x03\0\0\0", b"\x05\0\0\0\0", b"\x02\0\0\x01\x04"):
            with self.subTest(packet=packet):
                bridge, controller = self.make()
                peer = self.connect(bridge)
                peer.sendall(packet)
                wait_for(lambda: bridge.stats["closed"])
                self.assertIsNotNone(bridge.stats["error"])
                self.assertEqual(controller.writes, [])
        bridge, controller = self.make()
        peer = self.connect(bridge)
        peer.sendall(COMMAND[:2]); peer.close()
        wait_for(lambda: bridge.stats["closed"])
        self.assertIn("incomplete", bridge.stats["error"])
        self.assertEqual(controller.writes, [])

    def test_reset_success_is_real_matching_complete_and_sent_only_once(self):
        controller = FakeController(); controller.auto_reset = True
        bridge, controller = self.make(True, controller)
        result = bridge.stop()
        self.assertEqual(controller.writes, [hci.RESET])
        self.assertTrue(result["resetAttempted"])
        self.assertTrue(result["resetSent"])
        self.assertTrue(result["resetResponseMatched"])
        self.assertEqual(result["resetStatus"], 0)
        self.assertTrue(result["radioTeardownVerified"])
        self.assertFalse(result["overAirObservation"])
        self.assertEqual(result["txPackets"], 0)
        bridge.stop()
        self.assertEqual(controller.writes, [hci.RESET])

    def test_reset_rejection_timeout_and_wrong_opcode_never_retry_or_fake_success(self):
        for reply in (bytes.fromhex("04 0e 04 01 03 0c 01"), None,
                      bytes.fromhex("04 0e 04 01 01 10 00")):
            with self.subTest(reply=reply):
                controller = FakeController(); controller.auto_reset = True; controller.reset_reply = reply
                bridge, controller = self.make(True, controller)
                result = bridge.stop()
                self.assertEqual(controller.writes, [hci.RESET])
                self.assertFalse(result["radioTeardownVerified"])
                self.assertIsNotNone(result["teardownError"])
                self.assertEqual(controller.closed, 1)

    def test_unresolved_prior_guest_reset_cannot_be_mistaken_for_teardown(self):
        bridge, controller = self.make(True)
        peer = self.connect(bridge)
        peer.sendall(hci.RESET)
        wait_for(lambda: bridge.stats["txPackets"] == 1)
        result = bridge.stop()
        self.assertEqual(controller.writes, [hci.RESET])  # only original guest command
        self.assertFalse(result["resetAttempted"])
        self.assertFalse(result["radioTeardownVerified"])
        self.assertIn("Unresolved guest Reset", result["teardownError"])

    def test_short_old_reset_reply_and_zero_command_credit_do_not_trigger_blind_reset(self):
        for original, reply, expected in (
                (hci.RESET, bytes.fromhex("04 0e 03 01 03 0c"), "Unresolved guest Reset"),
                (COMMAND, bytes.fromhex("04 0e 04 00 09 10 00"), "command credit")):
            with self.subTest(reply=reply):
                bridge, controller = self.make(True)
                peer = self.connect(bridge)
                peer.sendall(original)
                wait_for(lambda: bridge.stats["txPackets"] == 1)
                controller.queue.put(reply)
                wait_for(lambda: bridge.stats["controllerRxPackets"] == 1)
                result = bridge.stop()
                self.assertEqual(controller.writes, [original])
                self.assertFalse(result["resetAttempted"])
                self.assertFalse(result["radioTeardownVerified"])
                self.assertIn(expected, result["teardownError"])

    def test_controller_tx_error_is_terminal_queued_commands_never_replayed(self):
        controller = FakeController(); controller.send_error = True
        bridge, controller = self.make(True, controller)
        peer = self.connect(bridge)
        peer.sendall(COMMAND + COMMAND)
        wait_for(lambda: bridge.stats["closed"])
        result = bridge.stats
        self.assertEqual(controller.writes, [COMMAND])
        self.assertEqual(result["txAttempts"], 1)
        self.assertEqual(result["txPackets"], 0)
        self.assertTrue(result["txOutcomeUncertain"])
        self.assertGreaterEqual(result["discardedTxPackets"], 1)
        self.assertFalse(result["resetAttempted"])
        self.assertFalse(result["radioTeardownVerified"])
        self.assertEqual(controller.closed, 1)

    def test_malformed_command_status_is_terminal_and_cannot_grant_reset_credit(self):
        for malformed in (bytes.fromhex("04 0f 03 00 01 09"),
                          bytes.fromhex("04 0f 05 00 01 09 10 00")):
            with self.subTest(malformed=malformed.hex()):
                bridge, controller = self.make(True)
                peer = self.connect(bridge)
                peer.sendall(COMMAND)
                wait_for(lambda: bridge.stats["txPackets"] == 1)
                controller.queue.put(malformed)
                wait_for(lambda: bridge.stats["closed"])
                result = bridge.stats
                self.assertIn("Malformed HCI Command Status", result["error"])
                self.assertIsNone(result["commandCredits"])
                self.assertEqual(controller.writes, [COMMAND])
                self.assertFalse(result["resetAttempted"])
                self.assertIn("Controller I/O failed", result["teardownError"])
                self.assertTrue(result["controllerClosed"])

    def test_controller_rx_error_is_terminal_without_a_reset_attempt(self):
        controller = FakeController(); controller.receive_error = True
        bridge, controller = self.make(True, controller)
        self.connect(bridge)
        wait_for(lambda: bridge.stats["closed"])
        self.assertEqual(controller.writes, [])
        self.assertFalse(bridge.stats["resetAttempted"])
        self.assertFalse(bridge.stats["radioTeardownVerified"])
        self.assertEqual(controller.closed, 1)

    def test_queue_bound_is_enforced_before_forwarding(self):
        bridge, controller = self.make()
        peer = self.connect(bridge)
        with patch.object(hci, "MAX_PENDING_BYTES", 3):
            peer.sendall(COMMAND)
            wait_for(lambda: bridge.stats["closed"])
        self.assertEqual(controller.writes, [])
        self.assertIn("bound", bridge.stats["error"])

    def test_zero_socket_send_is_terminal_instead_of_replaying_forever(self):
        bridge, controller = self.make()
        bridge._send_guest = lambda stream, data: 0
        self.connect(bridge)
        controller.queue.put(EVENT)
        wait_for(lambda: bridge.stats["closed"])
        self.assertIn("invalid progress", bridge.stats["error"])
        self.assertEqual(bridge.stats["rxPackets"], 0)
        self.assertEqual(bridge.stats["guestWireBytesSent"], 0)
        self.assertEqual(controller.closed, 1)

    def test_vendor_is_local_unknown_only_after_real_completion_in_wire_order(self):
        bridge, controller = self.make()
        peer = self.connect(bridge)
        peer.sendall(COMMAND + VENDOR)
        wait_for(lambda: controller.writes == [COMMAND])
        time.sleep(.02)
        self.assertEqual(bridge.stats['localVendorRejections'], 0)
        controller.queue.put(COMMAND_COMPLETE)
        self.assertEqual(receive_exact(peer, len(COMMAND_COMPLETE + LOCAL_UNKNOWN)), COMMAND_COMPLETE + LOCAL_UNKNOWN)
        self.assertEqual(controller.writes, [COMMAND])
        self.assertEqual(bridge.stats['commandCredits'], 2)
        self.assertEqual(bridge.stats['txAttempts'], 1)
        self.assertEqual(bridge.stats['localVendorRejections'], 1)
        row = bridge.stats['localVendorEvents'][0]
        self.assertEqual(row['source'], 'local-unsupported-vendor')
        self.assertFalse(row['physicalSent'])
        self.assertEqual(row['status'], 1)
        self.assertEqual(bridge.stats['vendorCommandsForwarded'], 0)

    def test_zero_physical_credit_cannot_be_replenished_by_local_vendor_failure(self):
        bridge, controller = self.make()
        peer = self.connect(bridge)
        peer.sendall(COMMAND + VENDOR)
        wait_for(lambda: controller.writes == [COMMAND])
        no_credit = bytes.fromhex('04 0e 04 00 09 10 00')
        controller.queue.put(no_credit)
        self.assertEqual(receive_exact(peer, len(no_credit)), no_credit)
        time.sleep(.02)
        self.assertEqual(bridge.stats['commandCredits'], 0)
        self.assertEqual(bridge.stats['localVendorRejections'], 0)
        # Only an actual controller credit update unlocks the local rejection.
        credit = bytes.fromhex('04 0e 03 01 00 00')
        controller.queue.put(credit)
        self.assertEqual(receive_exact(peer, len(credit + LOCAL_UNKNOWN)), credit + LOCAL_UNKNOWN)
        self.assertEqual(controller.writes, [COMMAND])
        self.assertEqual(bridge.stats['commandCredits'], 1)

    def test_first_vendor_command_without_observed_credits_fails_closed(self):
        bridge, controller = self.make()
        peer = self.connect(bridge)
        peer.sendall(VENDOR)
        wait_for(lambda: bridge.stats['closed'])
        self.assertIn('before real command credits', bridge.stats['error'])
        self.assertEqual(controller.writes, [])
        self.assertEqual(bridge.stats['localVendorRejections'], 0)
        self.assertEqual(bridge.stats['rxPackets'], 0)

    def test_standard_commands_are_serialized_and_stopped_vendor_is_not_retried(self):
        bridge, controller = self.make()
        peer = self.connect(bridge)
        peer.sendall(COMMAND + COMMAND + VENDOR)
        wait_for(lambda: controller.writes == [COMMAND])
        time.sleep(.02)
        self.assertEqual(controller.writes, [COMMAND])
        controller.queue.put(COMMAND_COMPLETE)
        wait_for(lambda: controller.writes == [COMMAND, COMMAND])
        self.assertEqual(bridge.stats['localVendorRejections'], 0)
        result = bridge.stop()
        self.assertEqual(controller.writes, [COMMAND, COMMAND])
        self.assertEqual(result['localVendorRejections'], 0)
        self.assertGreaterEqual(result['discardedTxPackets'], 1)

    def test_vendor_rejection_diagnostics_are_bounded_and_snapshot_copied(self):
        bridge, controller = self.make()
        peer = self.connect(bridge)
        controller.queue.put(COMMAND_COMPLETE)
        self.assertEqual(receive_exact(peer, len(COMMAND_COMPLETE)), COMMAND_COMPLETE)
        peer.sendall(VENDOR * 129)
        self.assertEqual(receive_exact(peer, len(LOCAL_UNKNOWN) * 129), LOCAL_UNKNOWN * 129)
        result = bridge.stats
        self.assertEqual(result['localVendorRejections'], 129)
        self.assertEqual(result['localVendorEventsOmitted'], 1)
        self.assertEqual(len(result['localVendorEvents']), 128)
        result['localVendorEvents'][0]['status'] = 0
        self.assertEqual(bridge.stats['localVendorEvents'][0]['status'], 1)
        self.assertEqual(controller.writes, [])

    def test_acl_completion_or_unmatched_command_complete_cannot_complete_pending_command(self):
        bridge, controller = self.make()
        peer = self.connect(bridge)
        peer.sendall(COMMAND + VENDOR)
        wait_for(lambda: controller.writes == [COMMAND])
        acl_completed = bytes.fromhex('04 13 05 01 01 00 01 00')
        controller.queue.put(acl_completed)
        self.assertEqual(receive_exact(peer, len(acl_completed)), acl_completed)
        self.assertIsNone(bridge.stats['commandCredits'])
        self.assertEqual(bridge.stats['localVendorRejections'], 0)
        # A late CC for another opcode carries real credit but does not settle
        # the still-unanswered Read BD_ADDR command.
        controller.queue.put(EVENT)
        self.assertEqual(receive_exact(peer, len(EVENT)), EVENT)
        time.sleep(.02)
        self.assertEqual(bridge.stats['localVendorRejections'], 0)
        controller.queue.put(COMMAND_COMPLETE)
        self.assertEqual(receive_exact(peer, len(COMMAND_COMPLETE + LOCAL_UNKNOWN)), COMMAND_COMPLETE + LOCAL_UNKNOWN)
        self.assertEqual(controller.writes, [COMMAND])

    def test_acl_after_a_credit_blocked_command_keeps_its_independent_transport(self):
        bridge, controller = self.make()
        peer = self.connect(bridge)
        peer.sendall(COMMAND + VENDOR + ACL + ACL)
        wait_for(lambda: len(controller.writes) == 3)
        self.assertEqual(controller.writes, [COMMAND, ACL, ACL])
        self.assertEqual(bridge.stats['localVendorRejections'], 0)
        controller.queue.put(COMMAND_COMPLETE)
        self.assertEqual(receive_exact(peer, len(COMMAND_COMPLETE + LOCAL_UNKNOWN)), COMMAND_COMPLETE + LOCAL_UNKNOWN)

    def test_stop_preserves_unresolved_command_despite_unmatched_positive_credit(self):
        bridge, controller = self.make(True)
        peer = self.connect(bridge)
        peer.sendall(COMMAND + VENDOR)
        wait_for(lambda: controller.writes == [COMMAND])
        credit = bytes.fromhex('04 0e 03 03 00 00')
        controller.queue.put(credit)
        self.assertEqual(receive_exact(peer, len(credit)), credit)
        result = bridge.stop()
        self.assertEqual(controller.writes, [COMMAND])
        self.assertEqual(result['outstandingCommands'], [{'opcode': '0x1009', 'count': 1}])
        self.assertTrue(result['commandOutcomeUncertain'])
        self.assertFalse(result['resetSent'])
        self.assertFalse(result['radioTeardownVerified'])
        self.assertTrue(result['controllerClosed'])
        self.assertIn('ordered drain', result['teardownError'])

    def test_late_guest_reset_complete_is_drained_and_cannot_acknowledge_stop_reset(self):
        bridge, controller = self.make(True)
        peer = self.connect(bridge)
        peer.sendall(hci.RESET + VENDOR)
        wait_for(lambda: controller.writes == [hci.RESET])
        release = threading.Thread(target=lambda: (time.sleep(.01), controller.queue.put(EVENT)))
        release.start()
        result = bridge.stop()
        release.join(1)
        # The first CC answers the guest Reset. The sole new stop Reset has
        # received no CC, and the queued local vendor failure answers neither.
        self.assertEqual(controller.writes, [hci.RESET, hci.RESET])
        self.assertTrue(result['resetSent'])
        self.assertFalse(result['resetResponseMatched'])
        self.assertFalse(result['radioTeardownVerified'])
        self.assertTrue(result['teardownCommandPending'])
        self.assertTrue(result['commandOutcomeUncertain'])
        self.assertEqual(result['localVendorRejections'], 0)
        self.assertTrue(result['controllerClosed'])

    def test_observed_address_requires_matching_successful_physical_read(self):
        bridge, controller = self.make()
        peer = self.connect(bridge)
        self.assertIsNone(bridge.stats['observedAddress'])
        self.assertEqual(bridge.stats['observedAddressSource'], 'not-queried')
        peer.sendall(COMMAND)
        wait_for(lambda: controller.writes == [COMMAND])
        controller.queue.put(ADDRESS_COMPLETE)
        self.assertEqual(receive_exact(peer, len(ADDRESS_COMPLETE)), ADDRESS_COMPLETE)
        self.assertEqual(bridge.stats['observedAddress'], '12:34:56:78:9A:BC')
        self.assertEqual(bridge.stats['observedAddressSource'], 'physical-hci-read-bdaddr')

    def test_unsolicited_failed_or_malformed_address_responses_do_not_create_identity(self):
        invalid = [bytes.fromhex('04 0e 04 01 09 10 01'),
                   bytes.fromhex('04 0e 09 01 09 10 00 bc 9a 78 56 34'),
                   bytes.fromhex('04 0e 0b 01 09 10 00 bc 9a 78 56 34 12 00'),
                   bytes.fromhex('04 0f 04 00 01 09 10')]
        for reply in invalid:
            with self.subTest(reply=reply.hex()):
                bridge, controller = self.make()
                peer = self.connect(bridge)
                controller.queue.put(ADDRESS_COMPLETE)  # no outstanding read
                self.assertEqual(receive_exact(peer, len(ADDRESS_COMPLETE)), ADDRESS_COMPLETE)
                self.assertIsNone(bridge.stats['observedAddress'])
                peer.sendall(COMMAND)
                wait_for(lambda: controller.writes == [COMMAND])
                controller.queue.put(reply)
                self.assertEqual(receive_exact(peer, len(reply)), reply)
                self.assertIsNone(bridge.stats['observedAddress'])
                self.assertEqual(bridge.stats['observedAddressSource'], 'not-queried')

    def test_local_vendor_completion_never_produces_physical_address_evidence(self):
        bridge, controller = self.make()
        peer = self.connect(bridge)
        controller.queue.put(EVENT)
        self.assertEqual(receive_exact(peer, len(EVENT)), EVENT)
        peer.sendall(VENDOR)
        self.assertEqual(receive_exact(peer, len(LOCAL_UNKNOWN)), LOCAL_UNKNOWN)
        self.assertEqual(bridge.stats['localVendorRejections'], 1)
        self.assertIsNone(bridge.stats['observedAddress'])
        self.assertEqual(controller.writes, [])

    def test_failed_followup_address_query_preserves_only_prior_observation(self):
        bridge, controller = self.make()
        peer = self.connect(bridge)
        peer.sendall(COMMAND)
        wait_for(lambda: controller.writes == [COMMAND])
        controller.queue.put(ADDRESS_COMPLETE)
        self.assertEqual(receive_exact(peer, len(ADDRESS_COMPLETE)), ADDRESS_COMPLETE)
        peer.sendall(COMMAND)
        wait_for(lambda: controller.writes == [COMMAND, COMMAND])
        failure = bytes.fromhex('04 0e 04 01 09 10 01')
        controller.queue.put(failure)
        self.assertEqual(receive_exact(peer, len(failure)), failure)
        self.assertEqual(bridge.stats['observedAddress'], '12:34:56:78:9A:BC')
        self.assertEqual(bridge.stats['observedAddressSource'], 'physical-hci-read-bdaddr')


class NamedPipeBridgeTests(unittest.TestCase):
    def make(self, reset=False, pipe=None, controller=None, connect_timeout=.1):
        pipe = pipe or FakePipe()
        controller = controller or FakeController()
        bridge = hci.NamedPipeControllerBridge(controller, reset_on_stop=reset,
            teardown_timeout=.04, pipe_factory=lambda: pipe, connect_timeout=connect_timeout)
        self.addCleanup(bridge.stop)
        return bridge, pipe, controller

    def attach(self, bridge):
        core = FakeCore()
        bridge.attach_core(core)
        wait_for(lambda: bridge.stats['connectionAccepted'] or bridge.stats['closed'])
        return core

    def test_inert_constructor_descriptor_and_stop_before_attach(self):
        with patch.object(hci.socket, 'socket') as create_socket:
            bridge, pipe, controller = self.make(True)
            descriptor = bridge.descriptor
            self.assertEqual(descriptor['qemuChardev'], pipe.chardev_argument)
            self.assertEqual(descriptor['qemuEnvironment']['AZKI_QEMU_PIPE_CLIENT_PID'], str(hci.os.getpid()))
            self.assertFalse(descriptor['peerAuthenticated'])
            self.assertFalse(bridge.stats['threadAlive'])
            self.assertFalse(bridge.stats['pipeClosed'])
            self.assertEqual(pipe.io_threads, [])
            self.assertEqual(controller.writes, [])
            self.assertEqual(controller.closed, 0)
            create_socket.assert_not_called()
            bridge.stop()
        self.assertEqual(controller.closed, 1)
        self.assertEqual(controller.writes, [])
        self.assertFalse(bridge.stats['resetAttempted'])
        self.assertEqual(pipe.closed, 1)
        self.assertTrue(bridge.stats['pipeClosed'])
        self.assertNotIn(threading.get_ident(), pipe.io_threads)

    def test_attach_requires_owned_core_not_pid_and_is_single_use(self):
        bridge, pipe, controller = self.make()
        with self.assertRaises(ValueError):
            bridge.attach_core(1234)
        self.assertEqual(pipe.io_threads, [])
        self.assertEqual(controller.writes, [])
        core = self.attach(bridge)
        self.assertIs(pipe.process, core)
        with self.assertRaises(hci.HciBridgeError):
            bridge.attach_core(core)
        self.assertTrue(bridge.descriptor['peerAuthenticated'])

    def test_verified_pipe_duplex_fragmentation_and_immutable_pending_prefix(self):
        bridge, pipe, controller = self.make()
        self.attach(bridge)
        for value in (COMMAND[:1], COMMAND[1:3], COMMAND[3:] + ACL[:2], ACL[2:]):
            pipe.read_queue.put(value)
        controller.queue.put(EVENT); controller.queue.put(ACL)
        wait_for(lambda: bridge.stats['txPackets'] == 2 and bridge.stats['rxPackets'] == 2)
        self.assertEqual(controller.writes, [COMMAND, ACL])
        self.assertEqual(bytes(pipe.output), EVENT + ACL)
        self.assertEqual(bridge.stats['rxBytes'], len(EVENT) + len(ACL))
        self.assertEqual(bridge.stats['guestWireBytesSent'], len(EVENT) + len(ACL))
        bridge.stop()
        self.assertEqual(len(set(pipe.io_threads)), 1)
        self.assertNotIn(threading.get_ident(), pipe.io_threads)
        self.assertEqual(pipe.closed, 1)
        self.assertTrue(bridge.stats['pipeClosed'])

    def test_pipe_close_failure_remains_observable_and_does_not_skip_controller_close(self):
        for attached in (False, True):
            with self.subTest(attached=attached):
                pipe = FakePipe()
                pipe.close_error = OSError('Owned pipe cancellation/close failed')
                bridge, pipe, controller = self.make(pipe=pipe)
                if attached:
                    self.attach(bridge)
                result = bridge.stop()
                self.assertTrue(result['closed'])
                self.assertTrue(result['controllerClosed'])
                self.assertFalse(result['pipeClosed'])
                self.assertIn('pipe close failed', result['error'])
                self.assertEqual(pipe.closed, 1)
                self.assertEqual(controller.closed, 1)
                bridge.stop()
                self.assertEqual(pipe.closed, 1)
                self.assertEqual(controller.closed, 1)

    def test_pipe_authentication_failure_never_touches_controller_until_close(self):
        pipe = FakePipe(); pipe.connect_error = PermissionError('Wrong QEMU server PID or private ACL')
        controller = FakeController(); controller.auto_reset = True
        bridge, pipe, controller = self.make(True, pipe, controller)
        self.attach(bridge)
        wait_for(lambda: bridge.stats['closed'])
        self.assertFalse(bridge.stats['peerAuthenticated'])
        self.assertEqual(controller.writes, [])
        self.assertEqual(controller.closed, 1)
        self.assertFalse(bridge.stats['resetAttempted'])
        self.assertIn('QEMU', bridge.stats['error'])

    def test_pipe_connect_timeout_is_terminal_and_does_not_fall_back_to_tcp(self):
        pipe = FakePipe(); pipe.available = False
        bridge, pipe, controller = self.make(True, pipe, connect_timeout=.02)
        bridge.attach_core(FakeCore())
        wait_for(lambda: bridge.stats['closed'])
        self.assertIn('deadline', bridge.stats['error'])
        self.assertFalse(bridge.stats['peerAuthenticated'])
        self.assertEqual(controller.writes, [])
        self.assertEqual(controller.closed, 1)
        count = pipe.connect_count
        bridge.stop()
        self.assertEqual(pipe.connect_count, count)

    def test_verified_pipe_stop_resets_once_with_all_controller_io_on_worker(self):
        controller = FakeController(); controller.auto_reset = True
        io_threads = []
        for name in ('send_h4', 'receive', 'close'):
            original = getattr(controller, name)
            def record(*args, _original=original):
                io_threads.append(threading.get_ident())
                return _original(*args)
            setattr(controller, name, record)
        bridge, pipe, controller = self.make(True, controller=controller)
        self.attach(bridge)
        result = bridge.stop()
        self.assertTrue(result['radioTeardownVerified'])
        self.assertEqual(controller.writes, [hci.RESET])
        self.assertEqual(set(io_threads), set(pipe.io_threads))
        self.assertEqual(len(set(io_threads)), 1)
        self.assertNotIn(threading.get_ident(), io_threads)

    def test_broken_verified_pipe_is_terminal_without_reconnect(self):
        bridge, pipe, controller = self.make()
        self.attach(bridge)
        pipe.read_error = ConnectionError('Owned QEMU exited')
        wait_for(lambda: bridge.stats['closed'])
        self.assertIn('QEMU exited', bridge.stats['error'])
        calls = pipe.connect_count
        bridge.stop()
        self.assertEqual(pipe.connect_count, calls)
        self.assertEqual(controller.closed, 1)

    def test_pipe_vendor_firewall_preserves_pending_write_prefix_and_actual_credit(self):
        bridge, pipe, controller = self.make()
        self.attach(bridge)
        pipe.read_queue.put(COMMAND + VENDOR + COMMAND)
        wait_for(lambda: controller.writes == [COMMAND])
        time.sleep(.02)
        self.assertEqual(bytes(pipe.output), b'')
        self.assertEqual(bridge.stats['localVendorRejections'], 0)
        controller.queue.put(COMMAND_COMPLETE)
        wait_for(lambda: controller.writes == [COMMAND, COMMAND])
        # Physical #2 can complete while the prior CC/local failure are still
        # in a fragmented pipe write. Their already-submitted prefix is stable.
        controller.queue.put(COMMAND_COMPLETE)
        expected = COMMAND_COMPLETE + LOCAL_UNKNOWN + COMMAND_COMPLETE
        wait_for(lambda: bytes(pipe.output) == expected)
        self.assertEqual(bridge.stats['txPackets'], 2)
        self.assertEqual(bridge.stats['controllerRxPackets'], 2)
        self.assertEqual(bridge.stats['localVendorRejections'], 1)
        self.assertEqual(bridge.stats['commandCredits'], 2)
        self.assertEqual(bridge.stats['rxPackets'], 3)
        self.assertFalse(bridge.stats['localVendorEvents'][0]['physicalSent'])

    def test_pipe_command_status_zero_credit_cannot_release_pending_vendor(self):
        bridge, pipe, controller = self.make()
        self.attach(bridge)
        pipe.read_queue.put(COMMAND + VENDOR)
        wait_for(lambda: controller.writes == [COMMAND])
        status = bytes.fromhex('04 0f 04 00 00 09 10')
        controller.queue.put(status)
        wait_for(lambda: bytes(pipe.output) == status)
        time.sleep(.02)
        self.assertEqual(bridge.stats['commandCredits'], 0)
        self.assertEqual(bridge.stats['localVendorRejections'], 0)
        result = bridge.stop()
        self.assertEqual(controller.writes, [COMMAND])
        self.assertEqual(result['localVendorRejections'], 0)
        self.assertGreaterEqual(result['discardedTxPackets'], 1)

if __name__ == "__main__":
    unittest.main()
