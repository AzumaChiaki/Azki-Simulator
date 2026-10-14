"""Per-instance production facade over the single verified installer protocol.

No socket, QMP, subprocess, storage transaction, credentials, or screen ownership.
The authenticated runner owns SessionAPI and calls tick from its existing loop.
IPC request parsing and same-user/PID checks belong to the runner's peer bridge.
"""
import copy
import os
import re
import stat
import time
import uuid

if __package__:
    from .installer import InstallationClient, Package
    from .virtual_packages import DEFAULT_PROFILE, require_install_profile
    from . import wire
else:
    from installer import InstallationClient, Package
    from virtual_packages import DEFAULT_PROFILE, require_install_profile
    import wire


class GuestOperations:
    def __init__(self, api, *, instance_id, profile_id, clock=time.monotonic):
        if str(uuid.UUID(instance_id)) != instance_id:
            raise ValueError('production operations require a canonical instance')
        require_install_profile(profile_id)
        self.api, self.clock = api, clock
        self.instance_id, self.profile_id = instance_id, profile_id
        self.client = InstallationClient(api, clock)
        self.operation = None; self.receipt = None; self.identity = None; self.tainted = None
        self.idle_events = 0; self.idle_error = None

    def status(self):
        status = self.api.status()
        operation = copy.deepcopy(self.operation)
        if operation:
            operation['elapsedSeconds'] = round(operation.pop('finished', self.clock()) - operation.pop('started'), 3)
            if operation['kind'] == 'install' and operation['state'] == 'running':
                snapshot = self.client.snapshot()
                operation.update(phase=self.client.state,
                                 packageID=snapshot.get('packageID'),packageSHA256=snapshot.get('packageSHA256'),
                                 acknowledgedSegments=snapshot.get('transportAcknowledgedSegments', 0))
        return {'schema': 1, 'instanceID': self.instance_id, 'profileID': self.profile_id,
                'sessionID': status.session_id, 'generation': status.generation,
                'authenticated': status.authenticated, 'transportReady': status.transport_ready,
                'backendKind': status.backend_kind, 'operation': operation,
                'idleMessagesObserved': self.idle_events,
                'idleError': self.idle_error,
                'supportedOperations': ['status', 'inventory', 'install', 'watchface-select', 'operation-status'],
                'supportedInstallKinds': ['watchface', 'quickapp'],
                'supportedInventoryKinds': ['watchface', 'quickapp'],
                'verifiedInstallKinds': ['watchface'] if self.profile_id == DEFAULT_PROFILE else [],
                'quickappGuestAcceptance': False}

    def operation_status(self, operation_id):
        if not self.operation or operation_id != self.operation['operationID']:
            raise ValueError('unknown operation for this instance')
        return {**self.status()['operation'], 'receipt': copy.deepcopy(self.receipt)}

    def last_receipt(self, operation_id):
        self.operation_status(operation_id)
        return copy.deepcopy(self.receipt)

    def _current(self):
        status = self.api.status()
        if not status.authenticated or not status.transport_ready or status.backend_kind not in ('guest', 'fixture'):
            raise ValueError('authenticated ready peer session required')
        return status.session_id, status.generation, status.backend_kind

    def _check_events(self, events, identity):
        if len(events) > 64:
            raise ValueError('business event batch exceeds bound')
        for event in events:
            if (event.session_id, event.generation) != identity[:2] or event.kind not in ('transport_ack', 'business_message'):
                raise ValueError('peer disconnected, changed generation, or reported an error')
            if event.kind == 'business_message' and ((event.channel, event.opcode) not in ((1, 2), (2, 1)) or len(event.payload) > 65535):
                raise ValueError('unexpected business channel or oversized message')

    def _begin(self, expected_session_id, expected_generation):
        if self.operation and self.operation['state'] == 'running':
            raise ValueError('another operation owns the instance business queue')
        identity = self._current()
        if type(expected_generation) is not int or identity[:2] != (expected_session_id, expected_generation):
            raise ValueError('stale peer session or generation')
        if identity == self.tainted:
            raise ValueError('failed operation requires a fresh authenticated generation')
        events = self.api.poll()
        self._check_events(events, identity)
        self.idle_events += len(events)
        self.identity = identity
        self.idle_error = None
        self.receipt = None
        return identity

    def start_install(self, package, *, expected_session_id, expected_generation, timeout=180):
        # The trusted staged inspection chooses the package kind. Neither ZIP
        # parsing nor transport ACK asserts guest signature/runtime acceptance.
        if not isinstance(package, Package) or package.kind not in ('watchface', 'quickapp') or package.profile != self.profile_id:
            raise ValueError('production installation requires an inspected package for this profile')
        self._begin(expected_session_id, expected_generation)
        self.client.start(package, timeout=timeout)
        self.operation = {'operationID': self.client.snapshot()['operationID'], 'kind': 'install',
                          'state': 'running', 'phase': self.client.state, 'packageID': package.identifier,
                          'packageKind': package.kind,
                          'packageSHA256': package.sha256, 'started': self.clock()}
        return self.operation['operationID']

    def start_install_staged(self, packages_directory, relative_name, inspection, **request):
        # The bridge supplies its fixed run/packages directory, not a UI path.
        if self.operation and self.operation['state'] == 'running':
            raise ValueError('another operation owns the instance business queue')
        if not isinstance(relative_name, str) or re.fullmatch(r'[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\.bin', relative_name) is None:
            raise ValueError('install requires an existing UUID.bin in this run packages directory')
        fd = os.open(packages_directory, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            info = os.fstat(fd)
            if info.st_uid != os.getuid() or stat.S_IMODE(info.st_mode) != 0o700:
                raise ValueError('staging directory must be private 0700')
            package = Package.from_inspection(relative_name, inspection, self.profile_id, dir_fd=fd)
        finally:
            os.close(fd)
        return self.start_install(package, **request)

    def start_select(self, identifier, *, expected_session_id, expected_generation, timeout=45):
        if not isinstance(identifier, str) or re.fullmatch(r'(?:[0-9]{12}|L[0-9]{11})', identifier) is None:
            raise ValueError('invalid watchface identity')
        if type(timeout) not in (int, float) or not 5 <= timeout <= 120:
            raise ValueError('invalid selection timeout')
        self._begin(expected_session_id, expected_generation)
        self.operation = {'operationID': str(uuid.uuid4()), 'kind': 'watchface-select', 'state': 'running',
                          'phase': 'before-select-inventory', 'packageID': identifier,
                          'packageKind': 'watchface', 'inventoryKind': 'watchface', 'started': self.clock()}
        self.deadline = self.clock() + timeout
        self.pages = set(); self.faces = {}; self.after_selection = False
        try:
            self._inventory()
        except (OSError, ValueError) as error:
            self._fail(error)
        return self.operation['operationID']

    def start_inventory(self, kind='watchface', *, expected_session_id, expected_generation, timeout=30):
        if kind not in ('watchface', 'quickapp'):
            raise ValueError('unsupported inventory kind')
        if type(timeout) not in (int, float) or not 5 <= timeout <= 120:
            raise ValueError('invalid inventory timeout')
        self._begin(expected_session_id, expected_generation)
        self.operation = {'operationID': str(uuid.uuid4()), 'kind': 'inventory', 'state': 'running',
                          'phase': 'wait-inventory', 'inventoryKind': kind, 'started': self.clock()}
        self.deadline = self.clock() + timeout
        self.pages = set(); self.faces = {}; self.after_selection = False
        try: self._inventory()
        except (OSError, ValueError) as error: self._fail(error)
        return self.operation['operationID']

    def _require_identity(self):
        if self._current() != self.identity:
            raise ValueError('peer session changed during operation')

    def _send(self, payload):
        self._require_identity()
        self.api.submit(1, 2, payload, expected_generation=self.identity[1])

    def _inventory(self, cursor=0):
        if cursor in self.pages or len(self.pages) >= 16:
            raise ValueError('inventory page cycle or bound')
        self.pages.add(cursor)
        self._send(wire.inventory_request(self.operation['inventoryKind'], cursor))

    def _fail(self, error):
        self.tainted = self.identity
        if self.operation and self.operation['kind'] == 'install':
            self.client.abort(str(error))
        if self.operation:
            self.operation.update(state='failed', error=str(error)[:512], finished=self.clock())
        self.receipt = {'state': 'failed', 'operationID': self.operation['operationID'],
                        'backendKind': self.identity[2], 'error': str(error)[:512]}
        for field in ('packageKind', 'inventoryKind'):
            if field in self.operation: self.receipt[field] = self.operation[field]

    def _select_events(self, events):
        for event in events:
            if event.kind == 'transport_ack': continue
            if event.channel != 1: continue
            route, _ = wire.route(event.payload)
            phase = self.operation['phase']
            kind = self.operation['inventoryKind']
            expected_route = (20 if kind == 'quickapp' else 4, 0)
            if phase in ('before-select-inventory', 'after-select-inventory', 'wait-inventory') and route == expected_route:
                faces, cursor = wire.inventory_response(kind, event.payload)
                for face in faces:
                    if face['id'] in self.faces or len(self.faces) >= 256:
                        raise ValueError('duplicate/oversized selection inventory')
                    self.faces[face['id']] = face
                if cursor:
                    self._inventory(cursor); continue
                if self.operation['kind'] == 'inventory':
                    current = [face['id'] for face in self.faces.values() if face.get('current', False)]
                    if len(current) > 1:
                        raise ValueError('guest returned multiple current watchfaces')
                    self.operation.update(state='completed', phase='inventory-confirmed', finished=self.clock())
                    self.receipt = {'state': 'completed', 'operationID': self.operation['operationID'],
                                    'backendKind': self.identity[2], 'inventory': list(self.faces.values()),
                                    'inventoryKind': kind,
                                    'currentID': current[0] if current else None, 'readOnly': True,
                                    'guestInventoryVerified': self.identity[2] == 'guest', 'guestInstalled': False}
                    return
                target = self.faces.get(self.operation['packageID'])
                if target is None:
                    raise ValueError('requested watchface is absent from guest inventory')
                if not self.after_selection:
                    self.operation['phase'] = 'wait-select-response'
                    self._send(wire.activate_watchface_request(target['id']))
                else:
                    if not target['current']:
                        raise ValueError('guest inventory did not mark requested watchface current')
                    self.operation.update(state='completed', phase='selection-inventory-confirmed', finished=self.clock())
                    self.receipt = {'state': 'completed', 'operationID': self.operation['operationID'],
                                    'backendKind': self.identity[2], 'targetInventory': target,
                                    'packageKind': 'watchface', 'inventoryKind': 'watchface',
                                    'guestSelectionVerified': self.identity[2] == 'guest',
                                    'fixtureSelectionVerified': self.identity[2] == 'fixture',
                                    'guestFrameVerified': False, 'guestRestartRetentionVerified': False}
                    return
            elif phase == 'wait-select-response' and route == (4, 1):
                if not wire.activate_watchface_response(event.payload):
                    raise ValueError('guest rejected requested watchface selection')
                self.after_selection = True; self.pages = set(); self.faces = {}
                self.operation['phase'] = 'after-select-inventory'; self._inventory()

    def tick(self):
        if not self.operation or self.operation['state'] != 'running':
            # Idle unsolicited events cannot accumulate in the bounded peer queue.
            status = self.api.status()
            if not status.authenticated or not status.transport_ready:
                return self.status()
            identity = (status.session_id, status.generation, status.backend_kind)
            try:
                events = self.api.poll(); self._check_events(events, identity)
                self.idle_events += len(events)
            except (ValueError, OSError) as error:
                self.idle_error = str(error)[:512]; self.tainted = identity
            return self.status()
        try:
            if self.operation['kind'] == 'install':
                self.client.tick()
                if self.client.state in ('completed', 'failed'):
                    self.receipt = self.client.snapshot()
                    self.receipt['packageKind'] = self.operation['packageKind']
                    self.operation.update(state=self.client.state, phase=self.client.state, finished=self.clock())
                    self.operation.update(packageID=self.receipt['packageID'],packageSHA256=self.receipt['packageSHA256'])
                    if self.client.state == 'failed':
                        self.tainted = self.identity
                        self.operation['error'] = self.receipt.get('error', 'guest installation failed')
            else:
                self._require_identity()
                if self.clock() >= self.deadline:
                    raise TimeoutError('inventory or selection operation deadline exceeded')
                events = self.api.poll(); self._check_events(events, self.identity)
                self._select_events(events)
        except (ValueError, OSError, TimeoutError) as error:
            self._fail(error)
        return self.status()

    def close(self):
        if self.operation and self.operation['state'] == 'running':
            self._fail('runner stopped before operation completed')
