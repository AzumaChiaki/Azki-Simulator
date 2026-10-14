"""Small synchronous QMP client shared by the runner and integration checks."""
import json
import socket
import threading


class QMP:
    def __init__(self, path, timeout=2):
        self.lock = threading.RLock()
        self.closed = False
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket.settimeout(timeout)
        try:
            self.socket.connect(str(path))
            self.stream = self.socket.makefile('rwb')
            self._receive()
            self.execute('qmp_capabilities')
        except BaseException:
            self.socket.close()
            raise

    def _receive(self):
        while True:
            line = self.stream.readline()
            if not line:
                raise ConnectionError('QMP connection closed')
            response = json.loads(line)
            if 'event' not in response:
                return response

    def execute(self, command, arguments=None):
        # Web/native input and shutdown diagnostics share this connection.
        # Serialize the complete request/reply exchange, not just each write.
        with self.lock:
            if self.closed:
                raise ConnectionError('QMP connection closed')
            request = {'execute': command}
            if arguments is not None:
                request['arguments'] = arguments
            self.stream.write(json.dumps(request).encode() + b'\n')
            self.stream.flush()
            response = self._receive()
            if 'error' in response:
                raise RuntimeError(response['error'])
            return response['return']

    def close(self):
        with self.lock:
            if not self.closed:
                self.closed = True
                try:
                    self.stream.close()
                finally:
                    self.socket.close()
