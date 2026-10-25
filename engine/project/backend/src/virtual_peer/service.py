"""Runner-owned local HCI + bounded business IPC; never owns QMP or processes."""
from collections import OrderedDict, deque
import ctypes
import json
import os
from pathlib import Path
import selectors
import socket
import stat
import struct
import sys
import threading
import time

from .auth_protocol import AuthenticationCentral
from .external import ExternalCentral
from .link import H4Link
from .secure_link import SecureLinkController
from .production_client import GuestOperations
from .state import canonical_uuid, strict_json, validate_state, write_pending
from .virtual_packages import load_staged

COMMANDS = ('status','connect','inventory','install','watchface-select','operation-status')
MAX_REQUEST = 2048
MAX_RESPONSE = 65536


class GuestClock:
    """Monotonic seconds that stand still while the guest CPU is halted.

    A debugger may stop the guest at a breakpoint for any length of time. The
    peer's protocol deadlines describe guest progress, so they are measured on
    this clock; a halted guest never looks like an unresponsive device.
    """
    def __init__(self, source=time.monotonic):
        self.source = source
        self.lock = threading.Lock()
        self.paused_total = 0.0
        self.paused_since = None

    def __call__(self):
        with self.lock:
            now = self.source()
            paused = self.paused_total + (now - self.paused_since if self.paused_since is not None else 0.0)
            return now - paused

    @property
    def running(self):
        with self.lock: return self.paused_since is None

    def set_running(self, running):
        with self.lock:
            now = self.source()
            if running and self.paused_since is not None:
                self.paused_total += now - self.paused_since; self.paused_since = None
            elif not running and self.paused_since is None:
                self.paused_since = now


def peer_identity(connection):
    if os.getuid() != os.geteuid():
        raise ValueError('Kernel peer identity unavailable')
    if sys.platform == 'darwin':
        library = ctypes.CDLL(None, use_errno=True)
        uid, gid = ctypes.c_uint(), ctypes.c_uint()
        library.getpeereid.argtypes = [ctypes.c_int,ctypes.POINTER(ctypes.c_uint),ctypes.POINTER(ctypes.c_uint)]
        library.getpeereid.restype = ctypes.c_int
        if library.getpeereid(connection.fileno(),ctypes.byref(uid),ctypes.byref(gid)):
            raise ValueError('Kernel peer identity unavailable')
        raw = connection.getsockopt(0,2,4)
        pid = struct.unpack('=i',raw)[0]
        if uid.value != os.getuid() or pid <= 0: raise ValueError('Kernel peer identity mismatch')
        return uid.value, pid
    if sys.platform.startswith('linux'):
        # struct ucred {pid_t pid; uid_t uid; gid_t gid} (SOL_SOCKET/SO_PEERCRED).
        # The pid namespace stays shared with the supervisor -- bwrap is invoked
        # with the namespaces enumerated and no --unshare-pid (it has no
        # --share-pid), so this pid is the host-visible identity of the
        # connected process.
        pid, uid, gid = struct.unpack('=iii', connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
        if uid != os.getuid() or pid <= 0: raise ValueError('Kernel peer identity mismatch')
        return uid, pid
    raise ValueError('Kernel peer identity unavailable')


def request(raw):
    if not 1 <= len(raw) <= MAX_REQUEST: raise ValueError('Request size bound')
    value = strict_json(raw)
    if type(value) is not dict: raise ValueError('Request must be an object')
    base = {'schema','requestID','command'}
    cmd = value.get('command')
    if isinstance(cmd, str) and cmd.startswith('external-'):
        fields = {'external-open': set(), 'external-status': {'bridgeID'}, 'external-reconnect': {'bridgeID'},
                  'external-close': {'bridgeID'}, 'external-request': {'bridgeID','operation','key','value','offset','enabled'},
                  'external-result': {'bridgeID','operationID'}}
        if cmd == 'external-open' and 'connectionTimeoutSeconds' in value:
            fields[cmd].add('connectionTimeoutSeconds')
            if type(value['connectionTimeoutSeconds']) is not int or not 1 <= value['connectionTimeoutSeconds'] <= 300:
                raise ValueError('Connection timeout must be 1–300 seconds')
        if cmd not in fields or set(value) != base | fields[cmd] or type(value.get('schema')) is not int or value.get('schema') != 1 or not canonical_uuid(value.get('requestID')):
            raise ValueError('Invalid external bridge request')
        if cmd != 'external-open' and not canonical_uuid(value['bridgeID']): raise ValueError('Invalid bridge identity')
        if cmd == 'external-request':
            if value['operation'] not in ('read','write','subscribe') or not isinstance(value['key'],str) or len(value['key']) > 16 or not isinstance(value['value'],str) or len(value['value']) > 1024 or type(value['offset']) is not int or not 0 <= value['offset'] <= 512 or type(value['enabled']) is not bool:
                raise ValueError('Invalid external ATT request')
        if cmd == 'external-result' and not canonical_uuid(value['operationID']): raise ValueError('Invalid operation identity')
        return value
    extra = {'status':set(),'connect':set(),'inventory':{'kind'},
        'install':{'filename','sha256'},'watchface-select':{'identifier'},
        'operation-status':{'operationID'}}
    if type(cmd) is not str or cmd not in extra: raise ValueError('Unknown peer command')
    if 'connectionTimeoutSeconds' in value:
        base.add('connectionTimeoutSeconds')
        if type(value['connectionTimeoutSeconds']) is not int or not 1 <= value['connectionTimeoutSeconds'] <= 300:
            raise ValueError('Connection timeout must be 1–300 seconds')
    if cmd != 'status': base |= {'sessionID','generation'}
    if set(value) != base | extra[cmd] or type(value['schema']) is not int or value['schema'] != 1 or not canonical_uuid(value['requestID']):
        raise ValueError('Invalid peer request schema')
    if cmd != 'status' and (type(value['sessionID']) is not str or len(value['sessionID']) != 32 or
        any(c not in '0123456789abcdef' for c in value['sessionID']) or
        type(value['generation']) is not int or not 0 <= value['generation'] < (1<<63)):
        raise ValueError('Invalid peer session identity')
    if cmd == 'inventory' and value['kind'] not in ('watchface','quickapp'): raise ValueError('Unsupported inventory kind')
    if cmd == 'operation-status' and not canonical_uuid(value['operationID']): raise ValueError('Invalid operation identity')
    if cmd == 'watchface-select' and (type(value['identifier']) is not str or not 1 <= len(value['identifier']) <= 64 or not value['identifier'].isascii() or not value['identifier'].isalnum()):
        raise ValueError('Invalid watchface identity')
    if cmd == 'install':
        name, digest = value['filename'], value['sha256']
        if type(name) is not str or not name.endswith('.bin') or not canonical_uuid(name[:-4]) or type(digest) is not str or len(digest) != 64 or any(c not in '0123456789abcdef' for c in digest):
            raise ValueError('Invalid staged package identity')
    return value


class PeerService:
    def __init__(self, socket_directory, run_directory, private_state, output_path, input_sha256):
        self.root = Path(socket_directory)
        self.run = Path(run_directory)
        for directory in (self.root,self.run):
            info = directory.lstat()
            if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
                raise ValueError('Private peer directories required')
        self.private_state = validate_state(private_state)
        self.output = Path(output_path)
        if self.output.parent != self.run or self.output.name != 'virtual-peer-state.pending.json':
            raise ValueError('State output must be in this private run')
        self.input_sha256 = input_sha256
        self.output_sha256 = None
        self.controller = SecureLinkController(private_state['identity']['address'])
        self.controller.encryption_events = deque(maxlen=128)
        self.connection_timeout = 30
        self.clock = GuestClock()
        self.central = self._central()
        self.protocol = H4Link(self.controller,self.central.consume,max_packets=None)
        self.central.link = self.protocol
        self.operations = GuestOperations(self.central.api, instance_id=private_state['instanceID'],profile_id=private_state['profileID'],clock=self.clock)
        self.hci_path = self.root/'hci.sock'
        self.peer_path = self.root/'peer.sock'
        self.selector = selectors.DefaultSelector()
        self.listeners = []
        self.owned = {}
        self.clients = {}
        self.core_pid = None
        self.connection = None
        self.tx = bytearray()
        self.stop = threading.Event()
        self.core_ready = threading.Event()
        self.thread = None
        self.failure = None
        self.closed = False
        self.last_log_state = None
        self.last_diagnostic = None
        self.log_path = self.run / 'virtual-peer.log'
        self.log_path.touch(mode=0o600, exist_ok=True)
        self.log_path.chmod(0o600)
        self.replay = OrderedDict()
        self.external = None

    def _confirmed(self, binding):
        self._log('authenticated link_encryption_and_smp_verified')
        candidate = {**self.private_state, 'binding':dict(binding),
                     'guestLinkBond':{key:binding[key] for key in ('ltk','random','ediv')}}
        self.output_sha256 = write_pending(self.output,candidate,binding)
        self.private_state = candidate

    def _central(self):
        return AuthenticationCentral(self.controller,self.private_state,self._confirmed,
                                     connection_timeout=self.connection_timeout)

    def _log(self, message):
        import time
        line = '[' + str(round(time.monotonic(), 3)) + '] ' + str(message)[:2048] + '\n'
        try:
            with self.log_path.open('a', encoding='utf-8') as stream:
                stream.write(line)
        except OSError:
            pass

    def state_receipt(self):
        return {'inputStateSHA256':self.input_sha256,'outputStateSHA256':self.output_sha256,
                'stateWritten':self.output_sha256 is not None}

    def descriptor(self):
        return {'schema':1,'socket':str(self.peer_path),'transport':'local-virtual-hci','automaticConnect':False}

    def start(self):
        try:
            for path,kind in ((self.hci_path,'hci-listener'),(self.peer_path,'peer-listener')):
                server = socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
                self.listeners.append(server)
                server.bind(str(path)); os.chmod(path,0o600)
                info = path.lstat(); self.owned[path] = (info.st_dev,info.st_ino)
                server.listen(2); server.setblocking(False)
                self.selector.register(server,selectors.EVENT_READ,kind)
            self.thread = threading.Thread(target=self._run,name='azki-virtual-peer',daemon=True)
            self.thread.start()
        except BaseException:
            self.close(); raise
        return self

    def set_guest_running(self, running):
        """Runner-observed vCPU run state (debugger halt, -S prelaunch)."""
        self.clock.set_running(bool(running))

    def attach_core(self,pid):
        if type(pid) is not int or pid <= 0 or self.core_pid is not None:
            raise ValueError('Core process identity must be assigned exactly once')
        self.core_pid = pid; self.core_ready.set()

    def _phase(self):
        central = self.central
        if central.state == 'inactive': return 'idle'
        if central.state == 'failed': return 'failed'
        if central.authenticated: return 'authenticated'
        if central.state == 'wait_link_encryption': return 'authenticating'
        if central.state == 'wait_link_completion':
            return 'awaiting_guest_confirmation' if self.controller.air is None else 'authenticating'
        if central.state in ('auth_send','wait_auth'):
            if central.auth_stage == 'app_verify' and self.controller.air is None:
                return 'awaiting_guest_confirmation'
            return 'authenticating' if central.auth_stage == 'app_confirm' else 'binding'
        if central.state in ('wait_bind_info','bind_info','binding_settle'): return 'binding'
        if central.session and not self.controller.connected and central.state != 'reconnect': return 'disconnected'
        return 'connecting'

    def status(self):
        result = self.operations.status()
        result['supportedOperations'] = list(COMMANDS)
        result['externalBridge'] = None if self.external is None else {'bridgeID':self.external.bridge_id,'state':self.external.state,'error':self.external.error}
        result.update(authPhase=self._phase(),physicalRadio=False,
                      connectAvailable=self.external is None and self.connection is not None and self.central.state in ('inactive','failed'),
                      authTimeoutSeconds=self.connection_timeout,
                      connectionTimeoutSeconds=self.connection_timeout)
        result.update(self.central.failure_diagnostic or dict(failureCode=None,failurePhase=None,
                      failureGeneration=None,disconnectReason=self.controller.disconnect_reason,
                      guestDisconnectReason=self.controller.guest_disconnect_reason))
        result['transportMetrics']=dict(self.central.business.metrics)
        info=(self.central.session or {}).get('bindInfo',{})
        for key in ('reportedModel','expectedModel','modelMatchesFirmwareProfile'):
            if key in info:result[key]=info[key]
        return result

    def _record_diagnostic(self):
        value=self.central.failure_diagnostic
        if value is None or value == self.last_diagnostic:return
        # Fixed code/enumeration/integer metadata only, never exception text.
        self.last_diagnostic=dict(value)
        self._log('failure '+json.dumps(value,separators=(',',':')))
        temporary=self.run/('.virtual-peer-diagnostic-'+os.urandom(8).hex()+'.tmp')
        try:
            fd=os.open(temporary,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
            with os.fdopen(fd,'wb') as stream:
                stream.write(json.dumps({'schema':1,**value},separators=(',',':')).encode()+b'\n')
            os.replace(temporary,self.run/'virtual-peer-diagnostic.json')
        except OSError:
            try:temporary.unlink()
            except FileNotFoundError:pass

    def _dispatch_external(self, value):
        command = value['command']
        if command == 'external-open':
            if self.external is not None: raise ValueError('External bridge already open')
            if self.connection is None: raise ValueError('Guest HCI transport is not ready')
            self.operations.close()
            if self.controller.connected: self._send(self.controller.disconnect_local())
            self.central.business.invalidate(self.controller.generation)
            self.external = ExternalCentral(self.controller, value.get('connectionTimeoutSeconds', self.connection_timeout), clock=self.clock)
            bond = self.private_state.get('guestLinkBond') or self.private_state['binding']
            if bond:
                self.external.smp.bond = (bytes.fromhex(bond['ltk']), bytes.fromhex(bond['random']), bond['ediv'])
            self.external.link = self.protocol
            self.protocol.on_l2cap = self.external.consume
            return {'bridgeID':self.external.bridge_id}
        bridge = self.external
        if bridge is None or value['bridgeID'] != bridge.bridge_id: raise ValueError('Stale external bridge')
        if command == 'external-status': return bridge.status()
        if command == 'external-reconnect':
            self._save_external_bond()
            if self.controller.connected: self._send(self.controller.disconnect_local())
            bridge.reconnect(self.clock())
            return {'reconnecting': True}
        if command == 'external-request':
            return bridge.submit(value['operation'], value['key'], value['value'], value['offset'], value['enabled'])
        if command == 'external-result': return bridge.results.pop(value['operationID'], {'done':False})
        if command == 'external-close':
            self._save_external_bond()
            if self.controller.connected: self._send(self.controller.disconnect_local())
            self.central.p256.close()
            self.central = self._central()
            self.central.link = self.protocol
            self.protocol.on_l2cap = self.central.consume
            self.operations = GuestOperations(self.central.api, instance_id=self.private_state['instanceID'], profile_id=self.private_state['profileID'], clock=self.clock)
            self.external = None
            return {'closed':True}
        raise ValueError('Unknown external command')

    def _save_external_bond(self):
        if self.external is None or self.external.smp.bond is None: return
        ltk, random, ediv = self.external.smp.bond
        bond = {'ltk':ltk.hex(), 'random':random.hex(), 'ediv':ediv}
        if self.private_state.get('guestLinkBond') == bond: return
        candidate = {**self.private_state, 'guestLinkBond':bond}
        if candidate['binding'] is not None:
            candidate['binding'] = {**candidate['binding'], **bond}
        self.output_sha256 = write_pending(self.output, candidate, candidate['binding'])
        self.private_state = candidate

    def _dispatch(self,value):
        command = value['command']
        if command.startswith('external-'): return self._dispatch_external(value)
        if self.external is not None and command != 'status': raise ValueError('External Bluetooth owns guest connection')
        current = self.central.api.status()
        if command != 'status' and (value['sessionID'] != current.session_id or value['generation'] != current.generation):
            raise ValueError('Stale peer session generation')
        if 'connectionTimeoutSeconds' in value:
            self.connection_timeout = value['connectionTimeoutSeconds']
            self.central.set_connection_timeout(self.connection_timeout)
        if command == 'status': return self.status()
        if command == 'connect':
            if self.connection is None: raise ValueError('Guest HCI transport is not ready')
            if self.central.state not in ('inactive','failed'): raise ValueError('Guest connection is already active')
            if self.central.state == 'failed':
                old = self.central
                self.operations.close()
                if self.controller.connected: self._send(self.controller.disconnect_local())
                self.central = self._central()
                # An in-progress binding can be retried only inside the same
                # owned process; it is never accepted as persistent success.
                if self.private_state['binding'] is None and old.auth_key is not None:
                    self.central.auth_key = old.auth_key; self.central.smp.bond = old.smp.bond
                old.p256.close()
                self.protocol.on_l2cap = self.central.consume
                self.central.link = self.protocol
                self.operations = GuestOperations(self.central.api,instance_id=self.private_state['instanceID'],profile_id=self.private_state['profileID'],clock=self.clock)
            self.central.state = 'idle'; self.central.next_at = self.clock()
            return self.status()
        expected = dict(expected_session_id=value['sessionID'],expected_generation=value['generation'])
        if command == 'inventory':
            operation = self.operations.start_inventory(kind=value['kind'],**expected)
        elif command == 'install':
            inspection = load_staged(self.run,value['filename'],value['sha256'],profile_id=self.private_state['profileID'])
            operation = self.operations.start_install_staged(self.run/'packages',value['filename'],inspection,**expected)
        elif command == 'watchface-select':
            operation = self.operations.start_select(value['identifier'],**expected)
        elif command == 'operation-status':
            return self.operations.operation_status(value['operationID'])
        else: raise ValueError('Unknown peer command')
        return {'operationID':operation}

    def process_request(self,raw):
        """Executed only by the single service thread (unit-testable without IO)."""
        request_id = None
        try:
            value = request(raw); request_id = value['requestID']
            if value['command'] != 'status' and not value['command'].startswith('external-'):
                current = self.central.api.status()
                if value['sessionID'] != current.session_id or value['generation'] != current.generation:
                    raise ValueError('Stale peer session generation')
            identity = bytes(raw)
            if request_id in self.replay:
                old_identity, old_response = self.replay[request_id]
                if identity != old_identity: raise ValueError('Request identity replay differs')
                return old_response
            result = self._dispatch(value)
            response = {'schema':1,'requestID':request_id,'ok':True,'result':result}
        except Exception as error:
            # Never serialize an exception which may contain guest bytes,
            # filesystem names, package metadata, or private state values.
            code = 'stale_session' if type(error) is ValueError and str(error) == 'Stale peer session generation' else 'request_rejected'
            response = {'schema':1,'requestID':request_id,'ok':False,
                        'error':{'code':code,'message':'Peer request rejected; refresh status before retrying.'}}
        encoded = json.dumps(response,separators=(',',':'),ensure_ascii=True,allow_nan=False).encode()+b'\n'
        if len(encoded) > MAX_RESPONSE:
            encoded = json.dumps({'schema':1,'requestID':request_id,'ok':False,
                'error':{'code':'response_bound','message':'Peer response exceeds its bound.'}},separators=(',',':')).encode()+b'\n'
        if request_id is not None and request_id not in self.replay:
            self.replay[request_id] = (bytes(raw),encoded)
            while len(self.replay)>64: self.replay.popitem(last=False)
        return encoded

    def _send(self,packets):
        for packet in packets:
            if len(self.tx)+len(packet) > 1048576: raise ValueError('HCI pending output bound')
            self.tx.extend(packet)

    def _drop(self,connection):
        self.clients.pop(connection,None)
        try: self.selector.unregister(connection)
        except KeyError: pass
        connection.close()

    def _run(self):
        try:
            while not self.stop.is_set():
                now = time.monotonic()
                guest_now = self.clock()
                if self.connection:
                    if self.external is not None:
                        self._send(self.external.tick(guest_now))
                        self._save_external_bond()
                    else:
                        self._send(self.central.tick(guest_now))
                        while self.central.auth_events:
                            self._log('auth ' + json.dumps(self.central.auth_events.popleft(), separators=(',', ':')))
                        self.operations.tick()
                    phase = self._phase()
                    operation = self.operations.status()["operation"]
                    log_state = (phase, operation.get("kind") if operation else None, operation.get("phase") if operation else None, self.central.state, self.central.auth_stage, self.controller.generation)
                    if log_state != self.last_log_state:
                        self._log('state ' + json.dumps(log_state) + '; authenticated=' + str(self.central.authenticated))
                        self.last_log_state = log_state
                    if self.tx:
                        try:
                            sent = self.connection.send(self.tx)
                            del self.tx[:sent]
                        except BlockingIOError: pass
                self._record_diagnostic()
                for connection,client in list(self.clients.items()):
                    if now > client['deadline']: self._drop(connection)
                for key,mask in self.selector.select(.02):
                    connection,kind = key.fileobj,key.data
                    if kind in ('hci-listener','peer-listener'):
                        client,_ = connection.accept()
                        try:
                            uid,pid = peer_identity(client)
                            if kind == 'hci-listener':
                                if not self.core_ready.wait(2) or pid != self.core_pid or self.connection is not None:
                                    raise ValueError('Unexpected guest HCI peer')
                                client.setblocking(False); self.connection = client
                                self._log('transport connected; awaiting bind info')
                                self.selector.register(client,selectors.EVENT_READ,'hci')
                            else:
                                if len(self.clients)>=2: raise ValueError('Peer client bound')
                                client.setblocking(False)
                                self.clients[client] = {'input':bytearray(),'output':None,'deadline':now+self.connection_timeout,'pid':pid}
                                self.selector.register(client,selectors.EVENT_READ,'client')
                        except (OSError,ValueError): client.close()
                    elif kind == 'hci':
                        data = connection.recv(4096)
                        if not data:
                            if self.external is not None: self.external._fail('Guest transport closed')
                            self.central._fail('Guest transport closed')
                            self.operations.close()
                            self._drop(connection); self.connection=None
                        else: self._send(self.protocol.feed(data))
                    else:
                        client = self.clients[connection]
                        if mask & selectors.EVENT_READ:
                            try: data = connection.recv(MAX_REQUEST+1)
                            except (ConnectionError,BlockingIOError):
                                self._drop(connection); continue
                            if not data: self._drop(connection); continue
                            client['input'].extend(data)
                            if len(client['input'])>MAX_REQUEST+1: self._drop(connection); continue
                            if b'\n' in client['input']:
                                raw,tail = bytes(client['input']).split(b'\n',1)
                                if tail: self._drop(connection); continue
                                client['output'] = bytearray(self.process_request(raw))
                                self.selector.modify(connection,selectors.EVENT_WRITE,'client')
                        if mask & selectors.EVENT_WRITE:
                            try: sent = connection.send(client['output'])
                            except (ConnectionError,BlockingIOError):
                                self._drop(connection); continue
                            del client['output'][:sent]
                            if not client['output']: self._drop(connection)
        except Exception as error:
            self.failure = 'virtual-peer-service-failed'
            self.central._fail(str(error))
            self._record_diagnostic()
        finally:
            self.operations.close()
            self.central.business.invalidate(self.controller.generation)
            for client in list(self.clients): self._drop(client)
            if self.connection: self._drop(self.connection); self.connection=None

    def close(self):
        if self.closed: return
        self.stop.set(); self.core_ready.set()
        if self.thread:
            self.thread.join(2.5)
            if self.thread.is_alive(): raise RuntimeError('Virtual peer shutdown exceeded bound')
        self.central.p256.close()
        for server in self.listeners:
            try: self.selector.unregister(server)
            except KeyError: pass
            server.close()
        self.listeners.clear(); self.selector.close()
        self.closed = True
        for path,identity in self.owned.items():
            try:
                info = path.lstat()
                if (info.st_dev,info.st_ino) == identity: path.unlink()
            except FileNotFoundError: pass
