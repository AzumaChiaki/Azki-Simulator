"""Guest installation driver for the virtual peer's authenticated SessionAPI.

Call start(), then tick() from the owner of the peer event loop. No sleeps,
processes, real Bluetooth, disk inventory edits, or fallback authentication.
Each API must have exactly one business owner for this operation: the firmware
does not echo an operation UUID. A failed attempt taints this driver until the
peer establishes a new session/generation so late messages cannot complete a
different operation.
"""
from dataclasses import dataclass, replace
import hashlib
import os
import re
import stat
import struct
import time
import uuid

if __package__:
    from . import wire
    from .virtual_packages import DEFAULT_PROFILE, require_install_profile
else:
    import wire
    from virtual_packages import DEFAULT_PROFILE, require_install_profile

MAX_PACKAGE = 32 * 1024 * 1024


@dataclass(frozen=True)
class Package:
    kind: str
    identifier: str
    version: int
    payload: bytes
    profile: str = DEFAULT_PROFILE

    def __post_init__(self):
        if self.kind not in ('quickapp', 'watchface'):
            raise ValueError('unsupported install kind')
        pattern = r'(?:[0-9]{12}|L[0-9]{11})'
        if not isinstance(self.identifier, str) or not self.identifier or (self.kind == 'watchface' and not re.fullmatch(pattern, self.identifier)):
            raise ValueError('invalid package identity')
        if type(self.version) is not int or not 0 <= self.version <= 0x7fffffffffffffff:
            raise ValueError('invalid package version')
        if not isinstance(self.payload, bytes) or not 1 <= len(self.payload) <= MAX_PACKAGE:
            raise ValueError('package byte bound')
        require_install_profile(self.profile)
        if self.kind == 'watchface':
            if len(self.payload) < 52 or self.payload[:4] != bytes.fromhex('5aa53412') or self.payload[40:52] != self.identifier.encode():
                raise ValueError('watchface header identity mismatch')

    @property
    def data_type(self):
        return 0x40 if self.kind == 'quickapp' else 0x10

    @property
    def md5(self):
        return hashlib.md5(self.payload).digest()

    @property
    def sha256(self):
        return hashlib.sha256(self.payload).hexdigest()

    @classmethod
    def from_inspection(cls, path, report, profile=DEFAULT_PROFILE, *, dir_fd=None):
        """Bind an existing offline package inspection to the exact bytes read.

        Its signatureVerified/runtimeCompatibility remain unproven; guest
        preflight and the unmodified installer still decide acceptance.
        """
        require_install_profile(profile)
        if not isinstance(report, dict) or report.get('profileID') != profile:
            raise ValueError('package inspection belongs to another exact profile')
        if dir_fd is not None and (not isinstance(path, str) or path in ('.', '..') or '/' in path or not path):
            raise ValueError('staged package must be a single relative filename')
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC, dir_fd=dir_fd)
        try:
            before = os.fstat(fd)
            if not stat.S_ISREG(before.st_mode) or before.st_nlink != 1 or not 1 <= before.st_size <= MAX_PACKAGE:
                raise ValueError('unsafe or oversized package file')
            if dir_fd is not None and (before.st_uid != os.getuid() or before.st_mode & 0o077):
                raise ValueError('staged package must be private to current user')
            data = bytearray()
            while len(data) <= MAX_PACKAGE:
                chunk = os.read(fd, min(65536, MAX_PACKAGE + 1 - len(data)))
                if not chunk:
                    break
                data.extend(chunk)
            after = os.fstat(fd)
            stable = lambda item: (item.st_dev, item.st_ino, item.st_size, item.st_mtime_ns, item.st_ctime_ns)
            if stable(before) != stable(after) or len(data) != before.st_size:
                raise ValueError('package changed while reading')
        finally:
            os.close(fd)
        payload = bytes(data)
        if report.get('bytes') != len(payload) or report.get('sha256') != hashlib.sha256(payload).hexdigest():
            raise ValueError('package inspection does not match exact source bytes')
        metadata = report.get('metadata', {})
        if report.get('format') == 'rpk':
            return cls('quickapp', metadata['packageName'], metadata['versionCode'], payload, profile)
        if report.get('format') == 'watchface_bin_candidate':
            return cls('watchface', metadata['faceID'], 0, payload, profile)
        raise ValueError('requires an unpacked RPK or BIN; engineering project is not installable')


class InstallationClient:
    def __init__(self, api, clock=time.monotonic, *, acceptance_readback=False):
        self.api = api
        self.clock = clock
        self.state = 'idle'
        self.identity = None
        self.receipt = None
        self.tainted = None
        # Optional research readback remains independent from product success.
        # Production uses the standard original-guest installer terminal.
        self.acceptance_readback = acceptance_readback

    def _status(self):
        status = self.api.status()
        if not status.authenticated or not status.transport_ready:
            raise ValueError('authenticated guest DeviceConfirm and ready transport are required')
        if status.backend_kind not in ('guest', 'fixture'):
            raise ValueError('unknown transport provenance')
        if type(status.generation) is not int or status.generation < 0 or not status.session_id:
            raise ValueError('invalid session identity')
        identity = (status.session_id, status.generation, status.backend_kind)
        if self.identity is not None and identity != self.identity:
            raise ValueError('session generation changed during operation')
        if type(status.remote_mps) is not int or not 64 <= status.remote_mps <= 65535:
            raise ValueError('invalid negotiated MPS')
        return status, identity

    def start(self, package, *, timeout=120):
        if self.state not in ('idle', 'completed', 'failed'):
            raise ValueError('another install is active')
        if not isinstance(package, Package) or type(timeout) not in (int, float) or not 5 <= timeout <= 600:
            raise ValueError('invalid package or operation deadline')
        self.identity = None
        status, identity = self._status()
        if identity == self.tainted:
            raise ValueError('failed operation requires a fresh authenticated session generation')
        # A caller must finish/drain earlier traffic before starting an operation.
        if self.api.poll():
            raise ValueError('peer has pending events; drain them before beginning exclusive install')
        self.identity = identity
        self.package = package
        self.started = self.clock()
        self.deadline = self.started + timeout if self.acceptance_readback else float('inf')
        self.phase_deadline = self.deadline
        self.before = {}
        self.after = {}
        self.pages = set()
        self.pending_terminal = None
        self.segments = None
        self.segment_request = None
        self.segment_index = 0
        self.segment_total = 0
        self.segment_requests = {}
        self.segments_exhausted = False
        self.mass_attempts = 0
        self.receipt = {
            'schema': 'azki.guest-install.operation.v1', 'operationID': str(uuid.uuid4()),
            'backendKind': status.backend_kind, 'sessionID': status.session_id, 'generation': status.generation,
            'profile': package.profile, 'packageID': package.identifier, 'kind': package.kind,
            'sourcePackageID': package.identifier, 'sourcePackageSHA256': package.sha256,
            'packageSHA256': package.sha256, 'packageMD5': package.md5.hex(), 'packageBytes': len(package.payload),
            'packageVersion': package.version, 'state': 'starting', 'events': [],
            'transportAcknowledgedSegments': 0, 'guestInstallerAccepted': False,
            'guestInstalled': False, 'guestLaunchVerified': False, 'guestRestartRetentionVerified': False,
            'inventoryDeltaVerified': False,
            'signatureVerified': False, 'fixtureFlowCompleted': False,
        }
        try:
            if package.kind == 'watchface' or self.acceptance_readback:
                self._request_inventory('before', 0)
            else:
                self._send(1, 2, wire.prepare_request(self.package), 'wait_prepare')
        except Exception as error:
            self._fail(str(error))
        return self.snapshot()

    def _event(self, phase, **details):
        if len(self.receipt['events']) >= 64:
            raise ValueError('operation evidence bound')
        self.receipt['events'].append({'phase': phase, 'elapsed': round(self.clock() - self.started, 6), **details})

    def _send(self, channel, opcode, payload, state):
        self._status()
        if not isinstance(payload, bytes) or len(payload) > 65533:
            raise ValueError('backend payload bound')
        request_id = self.api.submit(channel, opcode, payload, expected_generation=self.identity[1])
        if not isinstance(request_id, str) or not 1 <= len(request_id) <= 128:
            raise ValueError('invalid transport request identity')
        self.state = state
        self.phase_deadline = min(self.deadline, self.clock() +
            ((20 if channel == 2 else 30) if self.acceptance_readback else (120 if channel == 2 else 60)))
        return request_id

    def _request_inventory(self, when, cursor):
        if cursor in self.pages or len(self.pages) >= 16:
            raise ValueError('inventory pagination cycle or page bound')
        self.pages.add(cursor)
        self._send(1, 2, wire.inventory_request(self.package.kind, cursor), 'wait_' + when)

    def _advance_segment(self):
        window = 1 if self.acceptance_readback else min(8, max(1, self.api.status().send_window))
        while not self.segments_exhausted and len(self.segment_requests) < window:
            try:
                segment = next(self.segments)
            except StopIteration:
                self.segments_exhausted = True
                break
            self.segment_total, self.segment_index = struct.unpack_from('<HH', segment)
            self.segment_request = self._send(2, 1, segment, 'wait_segment')
            self.segment_requests[self.segment_request] = self.segment_index
        if self.segments_exhausted and not self.segment_requests:
            self.state = 'wait_terminal'
            self.phase_deadline = self.deadline if self.acceptance_readback else self.clock() + min(600, 180 + len(self.package.payload)/(1024*1024)*20)
            self._event('all-mass-transport-acks', segments=self.segment_index)
            if self.pending_terminal is not None:
                self._accept_terminal(self.pending_terminal)

    def _accept_terminal(self, result):
        if (result['id'] or self.acceptance_readback) and result['id'] != self.package.identifier:
            raise ValueError('terminal identity does not match this operation')
        if not result['accepted']:
            raise ValueError('guest installer rejected package, code %d' % result['code'])
        self.receipt['terminal'] = result
        self.receipt['guestInstallerAccepted'] = self.identity[2] == 'guest'
        self._event('matching-installer-terminal', code=result['code'])
        if not self.acceptance_readback:
            self.receipt['guestInstalled'] = self.identity[2] == 'guest'
            self.receipt['fixtureFlowCompleted'] = self.identity[2] == 'fixture'
            self.receipt['successAuthority'] = 'original-guest-installer-terminal'
            self.state = 'completed'
            return
        self.pages = set()
        self._request_inventory('after', 0)

    def _business(self, payload):
        route, _ = wire.route(payload)
        kind = self.package.kind
        terminal_route = (20, 2) if kind == 'quickapp' else (4, 5)
        if route == terminal_route:
            if self.state not in ('wait_segment', 'wait_terminal'):
                raise ValueError('uncorrelated early/duplicate install terminal')
            result = wire.terminal_response(kind, payload)
            if (result['id'] or self.acceptance_readback) and result['id'] != self.package.identifier:
                raise ValueError('terminal identity mismatch')
            if self.pending_terminal is not None:
                raise ValueError('duplicate install terminal')
            self.pending_terminal = result
            if self.state == 'wait_terminal':
                self._accept_terminal(result)
            return
        if self.state in ('wait_before', 'wait_after') and route == (20 if kind == 'quickapp' else 4, 0):
            when = self.state.removeprefix('wait_')
            entries, cursor = wire.inventory_response(kind, payload)
            inventory = self.before if when == 'before' else self.after
            for entry in entries:
                if entry['id'] in inventory or len(inventory) >= wire.MAX_ITEMS:
                    raise ValueError('duplicate or excessive paginated inventory')
                inventory[entry['id']] = entry
            if cursor:
                self._request_inventory(when, cursor)
                return
            self.receipt[when + 'Inventory'] = list(inventory.values())
            self._event(when + '-inventory', count=len(inventory))
            if when == 'before':
                if not self.acceptance_readback and kind == 'watchface':
                    used = set(inventory)
                    identifier = next(('L%011d' % number for number in range(1, len(used)+2) if 'L%011d' % number not in used), None)
                    payload = self.package.payload[:40] + identifier.encode() + self.package.payload[52:]
                    self.package = replace(self.package, identifier=identifier, payload=payload)
                    self.receipt.update(packageID=identifier, packageSHA256=self.package.sha256,
                                        packageMD5=self.package.md5.hex(), standardLocalIDAllocated=True)
                elif self.acceptance_readback:
                    prior = inventory.get(self.package.identifier)
                    if prior and (kind == 'watchface' or prior['version'] >= self.package.version):
                        raise ValueError('target already present without a new version; cannot prove installation delta')
                self._send(1, 2, wire.prepare_request(self.package), 'wait_prepare')
            else:
                target = inventory.get(self.package.identifier)
                if target is None or (kind == 'quickapp' and target['version'] != self.package.version):
                    raise ValueError('successful terminal is not confirmed by inventory identity/version')
                for identifier, original in self.before.items():
                    if identifier != self.package.identifier:
                        remaining = inventory.get(identifier)
                        # The original installer can activate the new face, so
                        # another face's current flag is expected to change.
                        stable = ('id', 'version', 'name', 'fingerprint') if kind == 'quickapp' else ('id', 'version', 'name')
                        if remaining is None or any(remaining.get(key) != original.get(key) for key in stable):
                            raise ValueError('unrelated inventory entry changed during installation')
                self.receipt['guestInstalled'] = self.identity[2] == 'guest'
                self.receipt['fixtureFlowCompleted'] = self.identity[2] == 'fixture'
                self.receipt['inventoryDeltaVerified'] = True
                self.state = 'completed'
            return
        if self.state == 'wait_prepare' and route == ((20, 1) if kind == 'quickapp' else (4, 4)):
            result = wire.prepare_response(kind, payload)
            self.receipt['preflight'] = result
            if result['code'] != 0:
                raise ValueError('guest installation preflight rejected, code %d' % result['code'])
            if result['id'] and result['id'] != self.package.identifier:
                raise ValueError('preflight identity mismatch')
            self.fallback_slice = result['slice']
            self._event('preflight-accepted', code=result['code'])
            self._send(1, 2, wire.mass_prepare_request(self.package), 'wait_mass_prepare')
            return
        if self.state == 'wait_mass_prepare' and route == (22, 0):
            result = wire.mass_prepare_response(payload)
            if result['code'] != 0:
                if not self.acceptance_readback and self.mass_attempts < 6:
                    self.mass_attempts += 1; self.state = 'wait_mass_retry'
                    self.retry_at = self.clock()+10; self.phase_deadline=self.retry_at+60
                    self._event('mass-prepare-busy', code=result['code'], attempt=self.mass_attempts)
                    return
                raise ValueError('guest MASS preparation rejected, code %d' % result['code'])
            if result['id'] and result['id'] != self.package.md5:
                raise ValueError('MASS preparation echoed another MD5')
            status, _ = self._status()
            self.segments = iter(wire.mass_segments(self.package, result['offset'], result['slice'] or self.fallback_slice, status.remote_mps))
            self.receipt['mass'] = {'resumeOffset': result['offset'], 'requestedSlice': result['slice'],
                'effectiveSlice': min(result['slice'] or self.fallback_slice or 4096, status.remote_mps-2),
                'sendWindow': min(8,max(1,status.send_window)), 'echoedMD5': result['id'].hex()}
            self._event('mass-prepared', resumeOffset=result['offset'])
            self._advance_segment()
            return
        # Unrelated business responses are bounded and retained only by route,
        # never as raw protobuf/authentication payload or fake completion proof.
        self._event('unrelated-business', type=route[0], subtype=route[1])

    def tick(self):
        if self.state in ('idle', 'failed', 'completed'):
            return self.snapshot()
        try:
            self._status()
            if self.state == 'wait_mass_retry' and self.clock() >= self.retry_at:
                self._send(1, 2, wire.mass_prepare_request(self.package), 'wait_mass_prepare')
            if self.clock() >= self.phase_deadline:
                raise TimeoutError('install deadline in ' + self.state)
            events = self.api.poll()
            if not isinstance(events, list) or len(events) > 64:
                raise ValueError('backend event queue bound')
            # A queued disconnect/error applies to the whole batch. Do not send
            # the next segment after an earlier ACK before noticing it below.
            for event in events:
                if (event.session_id, event.generation) != self.identity[:2]:
                    raise ValueError('stale/foreign session event')
                if event.kind in ('disconnected', 'error'):
                    raise ValueError('peer disconnected or reported an error')
                if event.kind not in ('transport_ack', 'business_message'):
                    raise ValueError('unknown peer event type')
            for event in events:
                if event.kind == 'transport_ack':
                    if self.state == 'wait_segment' and event.request_id in self.segment_requests:
                        index = self.segment_requests.pop(event.request_id)
                        self.receipt['transportAcknowledgedSegments'] += 1
                        self.phase_deadline=min(self.deadline,self.clock()+(20 if self.acceptance_readback else 120))
                        self._advance_segment()
                elif event.kind == 'business_message':
                    if event.channel != 1 or event.opcode != 2:
                        raise ValueError('installation requires authenticated protobuf responses')
                    if not isinstance(event.payload, bytes) or len(event.payload) > 65535:
                        raise ValueError('business response bound')
                    self._business(event.payload)
                if self.state == 'completed':
                    break
        except Exception as error:
            self._fail(str(error))
        return self.snapshot()

    def _fail(self, reason):
        self.state = 'failed'
        self.tainted = self.identity
        self.segments = None
        self.receipt.update(error=reason, requiresFreshSession=True, guestInstalled=False, fixtureFlowCompleted=False)

    def abort(self, reason='owner stopped the operation'):
        """Stop producing protocol traffic; no guest cancel command is claimed.

        The owner must disconnect before reusing the transport. A failed
        generation remains tainted, including after a native runner stop.
        """
        if self.state not in ('idle', 'failed', 'completed'):
            self._fail(str(reason)[:512])
        return self.snapshot()

    def snapshot(self):
        import copy
        if self.receipt is None:
            return {'state': self.state, 'guestInstalled': False}
        result = copy.deepcopy(self.receipt)
        result['state'] = self.state
        result['elapsedSeconds'] = round(self.clock() - self.started, 6)
        return result
