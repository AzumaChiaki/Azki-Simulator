"""The kernel peer credential check has to be tested with a real second process.

peer_identity() is what gates the virtual peer's unix socket, and the Linux
branch reads SO_PEERCRED by hand. A wrong struct layout (pid/uid/gid order, or
the native int size) still returns two plausible integers, so only a real
socket pair with a real other process can show that each end reports the
*peer's* pid rather than its own. Both branches run on macOS and Linux alike.
"""
import json
import os
import socket
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "engine/project/backend/src"))

from virtual_peer.service import peer_identity  # noqa: E402


class PeerIdentityTests(unittest.TestCase):
    def setUp(self):
        if not hasattr(os, "fork"):
            raise unittest.SkipTest("a second process is required")
        self.directory = tempfile.TemporaryDirectory(prefix="azki-peer-")
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "control.sock"
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.listener.bind(str(self.path))
        self.listener.listen(1)
        self.listener.settimeout(20)
        self.addCleanup(self.listener.close)

    def _child_body(self):
        try:
            child_pid = os.getpid()
            connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            connection.settimeout(20)
            connection.connect(str(self.path))
            reported = peer_identity(connection)
            connection.sendall(json.dumps({"reported": list(reported), "self": child_pid}).encode())
            connection.close()
        except BaseException as error:  # noqa: BLE001 - reported back through the exit status
            os._exit(2)
        os._exit(0 if reported[0] == os.getuid() else 3)

    def test_each_end_reports_the_other_processes_pid(self):
        pid = os.fork()
        if pid == 0:
            self._child_body()
        try:
            accepted, _ = self.listener.accept()
            accepted.settimeout(20)
            parent_report = peer_identity(accepted)
            data = b""
            while True:
                chunk = accepted.recv(4096)
                if not chunk:
                    break
                data += chunk
            payload = json.loads(data)
            _, status = os.waitpid(pid, 0)
            self.assertEqual(os.WEXITSTATUS(status), 0, "child peer_identity did not report this uid")

            # Each side has to name the *other* process, which is what a
            # mis-ordered or self-read credential looks wrong about.
            self.assertEqual(parent_report, (os.getuid(), pid))
            self.assertEqual(payload["reported"], [os.getuid(), os.getpid()])
            self.assertNotEqual(payload["self"], os.getpid())
            accepted.close()
        finally:
            try:
                os.waitpid(pid, 0)
            except ChildProcessError:
                pass

    def test_rejects_a_socket_without_a_peer(self):
        unconnected = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.addCleanup(unconnected.close)
        with self.assertRaises((ValueError, OSError)):
            peer_identity(unconnected)


if __name__ == "__main__":
    unittest.main()
