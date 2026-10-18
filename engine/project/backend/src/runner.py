#!/usr/bin/env python3
"""Run the original Q66 application with the partial BES1503 QEMU model."""
import argparse
from contextlib import ExitStack
import hashlib
import json
import math
import os
from pathlib import Path
import re
import resource
import shlex
import subprocess
import tempfile
import sys
import time
import threading

from .control import WatchControl, NativeWindow

from .qmp import QMP
from .instance_data import (validate_instance_images, instance_machine_options, initialize_instance_data, validate_instance_nor,
                            load_instance_settings, apply_instance_settings, read_instance_properties,
                            instance_settings_options,
                            instance_settings_runtime, instance_settings_walk,
                            initialize_instance_nor, instance_nor_machine_options, validate_nor_geometry,
                            instance_nor_result_geometry)


QMP_START_SECONDS = 60
# Seconds after the control socket opens before host battery/charger state is
# applied; the guest must already be past its power-on charger check.
RUNTIME_SETTINGS_DELAY = 8
# Guest seconds until every preset's pedometer service counts IMU samples
# (Band 9..11 subscribe their accelerometer 45..60 s after power-on).
WALK_START_DELAY = 75
# How often the runner samples the vCPU run state while a debugger may hold it.
GDB_STATE_POLL_SECONDS = .25
GDB_CHARDEV = re.compile(r'tcp:127\.0\.0\.1:([0-9]{1,5})(?:,|$)')
POWER_EVENT = re.compile(rb'^AZKI-POWER-EVENT (poweroff|reboot|panic)$', re.M)


def guest_power_event(path):
    """The core prints one line when the guest powers off or resets itself."""
    try:
        with open(path, 'rb') as stream:
            stream.seek(0, 2)
            stream.seek(max(0, stream.tell() - 65536))
            events = POWER_EVENT.findall(stream.read())
    except OSError:
        return None
    event = events[-1].decode() if events else None
    return event if event in ('poweroff', 'reboot') else None
WATCH_FLAGS = ('bt_peer', 'bt_tx_capture', 'bt_hci_reset_fixture', 'panel_id_fixture',
               'lcd_frame_capture', 'panel_te_fixture', 'spi0_unconnected',
               'gpu_probe', 'gpu_id_fixture', 'gpu_software_renderer', 'touch',
               'power_adc_fixture', 'bt_hci_reject_fixture')
DEBUG_FLAGS = WATCH_FLAGS + ('gpu_no_render', 'gpu_completion_fixture', 'gh3x2x_fixture',
                             'gh3x2x_reg0724', 'revision_id')


def validate_virtual_guest_identity(state, board, firmware_sha256, qemu_sha256):
    """Match the synthetic identity to the explicitly selected board profile.

    Firmware/core digests are descriptive metadata. Geometry and the synthetic
    state's private handoff are checked separately; neither uses a code allowlist.
    """
    if type(board) is not dict or state['profileID'] != board.get('profile_id'):
        raise ValueError('Virtual mode requires matching exact state/board profiles')


def initialize_startup_nor(storage, virtual_state, *, skip_oobe, property_file):
    """Initialize new storage only; changing the preference never resets a device.

    Synthetic manufacturing identity is independent from setup completion and
    account binding. In interactive mode the firmware owns its first-run state.
    """
    if storage is None or not storage['norInitialize']:
        return False
    if virtual_state is not None:
        from .virtual_peer.state import factory_image, config_image
        factory_seed = factory_image(virtual_state, skip_oobe=skip_oobe)
        config_seed = config_image(virtual_state)
        if storage.get('configBytes', 0) and config_seed is None:
            raise ValueError('This exact NOR layout requires a matching configuration seed')
        initialize_instance_nor(storage, factory_seed, config_seed)
    elif skip_oobe:
        from .factory import local_desktop_image
        if storage['factoryBytes'] != 128 * 1024:
            raise ValueError('No verified local factory seed format for this exact partition size')
        initialize_instance_nor(storage, local_desktop_image(None, property_file))
    # Without an identity or setup seed, leave the new erased image intact.
    return True


def close_control_bounded(control, timeout=2):
    """A stalled input server must not prevent guest fault capture or reaping."""
    errors = []
    def close():
        try:
            control.close()
        except Exception as error:
            errors.append(error)
    worker = threading.Thread(target=close, daemon=True, name='control-shutdown')
    worker.start()
    worker.join(timeout)
    if worker.is_alive():
        raise RuntimeError('Control shutdown exceeded its deadline')
    if errors:
        raise RuntimeError(str(errors[0]))


def tap_point(value):
    try:
        x, y = map(int, value.split(','))
        if 0 <= x < 1024 and 0 <= y < 1024:
            return x, y
    except ValueError:
        pass
    raise argparse.ArgumentTypeError('Expected nonnegative X,Y below 1024; actual panel bounds are enforced by input')

def close_process(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def qemu_display_backend(qemu, window, platform=None):
    if not window:
        return 'none'
    platform = sys.platform if platform is None else platform
    if platform == 'darwin':
        return 'cocoa,show-cursor=on'
    available = subprocess.run([str(qemu), '-display', 'help'],
                               capture_output=True, text=True, timeout=5)
    backends = set((available.stdout + '\n' + available.stderr).split())
    for name in ('gtk', 'sdl'):
        if name in backends:
            return name + ',show-cursor=on'
    raise RuntimeError('This QEMU build has no GTK/SDL native window backend')


def scb_word(dump, wanted):
    """Read one register from HMP's SCB dump."""
    for line in (dump or '').splitlines():
        match = re.match(r'\s*(?:0x)?([0-9a-fA-F]+):\s*(.*)', line)
        if match:
            address = int(match[1], 16)
            words = re.findall(r'0x([0-9a-fA-F]{8})', match[2])
            index = (wanted - address) // 4
            if address <= wanted and 0 <= index < len(words):
                return int(words[index], 16)
    return 0


def cortex_fault_status(dump):
    return scb_word(dump, 0xe000ed28)


def cortex_fault_handler(dump):
    # The stock fault handler can clear CFSR before reaching its halt loop.
    active = scb_word(dump, 0xe000ed04) & 0x1ff
    return {3: 'HardFault', 4: 'MemManage', 5: 'BusFault', 6: 'UsageFault'}.get(active)


def lcd_frames(trace):
    """Accept both the older SHA256 log and the inexpensive snapshot log."""
    frames = []
    for match in re.finditer(
            r'BES experimental LCD frame captured: index=(\d+) '
            r'address=(0x[0-9a-f]+) width=(\d+) height=(\d+) pitch=(\d+)'
            r'(?: sha256=([0-9a-f]{64}))?', trace):
        frame = dict(index=int(match[1]), address=match[2], width=int(match[3]),
                     height=int(match[4]), pitch=int(match[5]),
                     source='guest-memory-snapshot', ui_verified=False)
        if match[6]:
            frame['rgb565_sha256'] = match[6]
        frames.append(frame)
    return frames


def gpu_command_records(trace):
    """Merge compact submissions with optional trace payloads by command ID."""
    commands = {}
    latest = None
    compact = 'BES GPU command submitted:' in trace
    pattern = (
        r'BES GPU command submitted: id=(?P<id>\d+) address=(?P<address>0x[0-9a-f]+) '
        r'bytes=(?P<bytes>\d+)'
        r'|BES GPU command captured: address=(?P<capture_address>0x[0-9a-f]+) '
        r'bytes=(?P<capture_bytes>\d+) data=(?P<data>[0-9a-f]+) \(not executed\)')
    for event in re.finditer(pattern, trace):
        if event['id'] is not None:
            index = int(event['id'])
            latest = dict(index=index, address=event['address'],
                          byte_count=int(event['bytes']), source='original-AP', executed=False)
            commands[index] = latest
            continue
        payload = bytes.fromhex(event['data'])
        size = int(event['capture_bytes'])
        if len(payload) != size:
            raise RuntimeError('GPU command capture length mismatch')
        if compact:
            if (latest is None or latest['address'] != event['capture_address'] or
                    latest['byte_count'] != size or 'data_hex' in latest):
                raise RuntimeError('GPU payload does not match its submission')
        else:
            index = len(commands)
            latest = dict(index=index, address=event['capture_address'], byte_count=size,
                          source='original-AP', executed=False)
            commands[index] = latest
        latest['data_hex'] = payload.hex()
    for event in re.finditer(r'BES GPU rendered: id=(\d+) draws=(\d+) pixels=(\d+) targets=(\d+)', trace):
        index = int(event[1])
        if index not in commands:
            raise RuntimeError('GPU completion references missing submission')
        commands[index].update(executed=True, renderer='software-subset',
                               draws=int(event[2]), pixels=int(event[3]), targets=int(event[4]))
    for event in re.finditer(r'BES GPU render (rejected|cancelled): id=(\d+) (?:reason=)?([^\n]+)', trace):
        index = int(event[2])
        if index in commands:
            commands[index].update(render_status=event[1], render_reason=event[3])
    return [commands[index] for index in sorted(commands)]

def main():
    here = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stop-on-fault', action='store_true',
                        help='Diagnostics only: stop on a sampled Cortex fault; normal sessions keep running')
    parser.add_argument('--fault-diagnostics', action='store_true',
                        help='Opt-in private exception log; excludes raw HCI and instruction tracing')
    parser.add_argument('--verbose-logs', action='store_true', help='Enable verbose QEMU guest error/HCI diagnostics')
    parser.add_argument('--profile', choices=('watch', 'baseline', 'custom'),
                        help='watch: boot with display/touch (default); baseline: bare model; custom: debug flags')
    parser.add_argument('--serve', nargs='?', type=int, const=8765, metavar='PORT',
                        help='Interactive watch at http://127.0.0.1:8765; runs until Ctrl+C')
    windows = parser.add_mutually_exclusive_group()
    windows.add_argument('--window', action='store_true',
                         help='Show QEMU native window with a visible host cursor (Cocoa on macOS)')
    windows.add_argument('--python-window', action='store_true',
                         help='Use the optional Python LCD viewer instead of QEMU native window')
    parser.add_argument('--tap', action='append', type=tap_point, default=[], metavar='X,Y',
                        help='Tap a pixel after the first visible frame; repeat for a sequence')
    parser.add_argument('--trace', action='store_true',
                        help='Also log translated instructions and CPU interrupts')
    parser.add_argument('--help-all', action='store_true', help='Show low-level debug options')
    parser.add_argument('--firmware', type=Path, default=here / 'firmware/vela_ap.bin',
                        help='Application image (default: firmware/vela_ap.bin in this project)')
    parser.add_argument('--resource', type=Path, nargs='?', metavar='IMAGE',
                        const=here / 'firmware/vela_resource.bin',
                        help='Load resource ROMFS (default image: firmware/vela_resource.bin in this project)')
    parser.add_argument('--audio-image', type=Path,
                        help='Load the product audio image (P65 only)')
    parser.add_argument('--sensor-image', type=Path,
                        help='Load the product sensor image (P65 only)')
    parser.add_argument('--bluetooth', choices=('reject', 'bluez', 'hci', 'virtual'), default='reject',
                        help='reject: startup fixture; bluez: virtual radio; hci: real phone via Linux adapter')
    parser.add_argument('--hci-socket', type=Path,
                        help='Unix socket provided by hci_bridge.py; required for --bluetooth hci')
    parser.add_argument('--factory-image', type=Path,
                        help='Q66 factory LittleFS; hci uses device/hci-factory.bin when present')
    parser.add_argument('--data-image', type=Path,
                        help='Hash-bound local NAND data seed copied into volatile emulated storage')
    parser.add_argument('--instance-data-image', type=Path)
    parser.add_argument('--instance-data-seed', type=Path,
                        help='Exact legacy UI image, used only for a fresh erased instance')
    parser.add_argument('--instance-spare-image', type=Path)
    parser.add_argument('--instance-data-sha256')
    parser.add_argument('--instance-spare-sha256')
    parser.add_argument('--instance-nor-image', type=Path)
    parser.add_argument('--instance-nor-sha256')
    parser.add_argument('--instance-nor-initialize', action='store_true')
    parser.add_argument('--instance-settings', type=Path,
                        help='Host settings applied to the instance persist.db before boot')
    parser.add_argument('--skip-oobe', action=argparse.BooleanOptionalAction, default=True,
                        help='Complete first-run setup locally; --no-skip-oobe keeps firmware setup. Never resets an existing instance.')
    parser.add_argument('--skip-binding', action='store_true',
                        help='Start the original Chinese watchface with local setup marked complete; no App/account binding')
    parser.add_argument('--qemu', type=Path, default=here / 'build/qemu-system-arm')
    parser.add_argument('--board-profile', type=Path,
                        help='Validated product-specific BES1503 memory geometry JSON')
    parser.add_argument('--seconds', type=float,
                        help='Wall-clock limit (watch/--serve/window modes: unlimited)')
    parser.add_argument('--log-dir', type=Path, default=here / 'logs/watch')
    parser.add_argument('--gdb', type=int, nargs='?', const=0, metavar='PORT',
                        help='Serve the GDB remote protocol on 127.0.0.1 (PORT, or a free port when omitted/0); '
                             'the chosen port is printed and published in control.json')
    parser.add_argument('--gdb-wait', action='store_true',
                        help='With --gdb: hold the CPU at reset until the debugger continues it')
    parser.add_argument('--bt-peer', action='store_true',
                        help='Enable experimental BT wake and ideal IQ fixture; no controller ROM/HCI')
    parser.add_argument('--bt-tx-capture', action='store_true',
                        help='Capture H4 commands with TX-done IRQ; requires --bt-peer; no HCI responses')
    parser.add_argument('--revision-id', type=int, choices=range(16), default=0,
                        help='Provisional AON revision ID (default 0; experimental boot uses 5)')
    parser.add_argument('--panel-id-fixture', action='store_true',
                        help='Experimental RM690C0_XINLI ID and SPI command capture; no pixels')
    parser.add_argument('--lcd-frame-capture', action='store_true',
                        help='Capture bounded RGB565 GRA frames with IRQ67; requires panel fixture')
    parser.add_argument('--gh3x2x-fixture', action='store_true',
                        help='Experimental GH3X2X SPI registers; no optical samples or IRQ')
    parser.add_argument('--spi0-unconnected', action='store_true',
                        help='SPI0 no-slave mode with pull-high MISO; original sensor failure path')
    parser.add_argument('--gpu-probe', action='store_true',
                        help='VGLite reset/idle probe with unsupported identity; no renderer')
    parser.add_argument('--gpu-id-fixture', action='store_true',
                        help='Q66 VGLite identity fixture; requires --gpu-probe; no rendering')
    parser.add_argument('--gpu-no-render', action='store_true',
                        help='Experimental GPU accept-and-ignore commands; no pixels or IRQ')
    parser.add_argument('--gpu-software-renderer', action='store_true',
                        help='Bounded VGLite A8/path software renderer with END IRQ; requires GPU ID')
    parser.add_argument('--gpu-completion-fixture', action='store_true',
                        help='Experimental GPU END completion and IRQ70; requires --gpu-no-render; no pixels')
    parser.add_argument('--bt-hci-reset-fixture', action='store_true',
                        help='Experimental HCI Reset response over original RX transport; requires TX capture')
    parser.add_argument('--panel-te-fixture', action='store_true',
                        help='Experimental 60 Hz TE on GPIO65/IRQ44; requires panel and frame capture')
    parser.add_argument('--gh3x2x-reg0724', type=lambda value: int(value, 0),
                        help='Explicit hypothetical 16-bit register0724 reset value; default unknown')
    parser.add_argument('--touch', action='store_true', help='CST9217 touch controller and absolute pointer input')
    parser.add_argument('--power-adc-fixture', action='store_true',
                        help='Constant PMU battery/NTC test inputs to avoid low-battery shutdown')
    parser.add_argument('--bt-hci-reject-fixture', action='store_true',
                        help='Respond Unknown HCI Command after Reset, without Bluetooth emulation')
    debug_options = {option for action in parser._actions if action.dest in DEBUG_FLAGS
                     for option in action.option_strings}
    if '--help-all' not in sys.argv:
        for action in parser._actions:
            if action.dest in DEBUG_FLAGS:
                action.help = argparse.SUPPRESS
    else:
        parser.print_help()
        return 0
    parser.add_argument('--virtual-peer-state',type=Path)
    parser.add_argument('--virtual-peer-state-sha256')
    parser.add_argument('--virtual-peer-output',type=Path)
    args = parser.parse_args()
    virtual_state = virtual_peer = None
    firmware_bytes = args.firmware.read_bytes() if args.board_profile or args.bluetooth == 'virtual' else None
    firmware_sha256 = None  # No startup AP content scan; profile geometry is checked below.
    virtual_requested = any((args.virtual_peer_state,args.virtual_peer_state_sha256,args.virtual_peer_output))
    if args.bluetooth == 'virtual':
        if not all((args.virtual_peer_state,args.virtual_peer_state_sha256,args.virtual_peer_output)) or args.factory_image or args.trace or args.tap or args.serve is not None:
            parser.error('Virtual mode requires private state and excludes raw diagnostic, automatic touch, radio and external factory inputs')
        from .virtual_peer.state import load_state
        virtual_state = load_state(args.virtual_peer_state,args.virtual_peer_state_sha256)
        if not args.board_profile:
            parser.error('Virtual mode requires an exact AP-bound board profile')
    elif virtual_requested:
        parser.error('Private peer state is only accepted in virtual mode')
    geometry = {}
    property_file = 'factory.prop'
    if args.board_profile:
        board = json.loads(args.board_profile.read_text())
        property_file = board.get('local_property_file', 'factory.prop')
        if virtual_state is not None and board.get('virtual_peer_state_sha256') != args.virtual_peer_state_sha256:
            parser.error('Board virtual state identity mismatch')
        allowed = {'ap-offset', 'reset-sp', 'flash-bytes', 'factory-offset',
                   'nv-offset', 'nand-data-offset'}
        optional = {'panel-power0','panel-power1','panel-power2','panel-id','resource-yaffs',
                    'pmu-calibration','panel-te-pin','touch-i2c','touch-power-pin',
                    'touch-reset-pin','touch-irq-pin','touch-module-id','touch-health-format','charger-i2c',
                    'charger-id','charger-map','pmu-adc-ntc','pmu-adc-ntc-room','config-offset','config-bytes',
                    'nand-data-bytes','ram-bytes'}
        geometry = board['geometry']
        if board.get('chip') not in ('best1503', 'best1502p') or not allowed <= set(geometry) or set(geometry)-allowed-optional or any(
                type(v) is not int or not 0 <= v <= 0xffffffff for v in geometry.values()):
            parser.error('Unsupported board profile or invalid memory geometry')
        if virtual_state is not None:
            try:
                validate_virtual_guest_identity(virtual_state, board, firmware_sha256,
                                                board.get('qemu_sha256'))
            except (ValueError, OSError) as error:
                parser.error(str(error))
    instance_storage = None
    if any((args.instance_data_image, args.instance_spare_image,
            args.instance_data_sha256, args.instance_spare_sha256)):
        if args.data_image or not args.board_profile or not args.resource:
            parser.error('Instance storage requires board/resource and excludes --data-image seed')
        try:
            instance_storage = validate_instance_images(
                args.instance_data_image, args.instance_spare_image,
                args.instance_data_sha256, args.instance_spare_sha256, geometry)
            if board.get('instance_storage') != instance_storage:
                parser.error('Instance storage differs from board transaction identity')
        except (ValueError, OSError) as error:
            parser.error(str(error))
    instance_nor = None
    if args.instance_nor_image or args.instance_nor_sha256 or args.instance_nor_initialize:
        if args.factory_image or not args.board_profile:
            parser.error('NOR transaction excludes factory input and requires a board identity')
        try:
            validate_nor_geometry(board, ap_sha256=firmware_sha256, ap_bytes=len(firmware_bytes))
            instance_nor = validate_instance_nor(args.instance_nor_image, args.instance_nor_sha256,
                                                 args.instance_nor_initialize, board, instance_storage)
            if board.get('instance_nor') != instance_nor:
                parser.error('NOR differs from board transaction identity')
        except (ValueError, OSError) as error:
            parser.error(str(error))
    if args.data_image:
        if not args.board_profile or not args.resource or not args.data_image.is_file():
            parser.error('--data-image requires an existing file, resource and board profile')
        if args.data_image.stat().st_size != geometry.get('nand-data-bytes'):
            parser.error('NAND data seed size differs from original partition')
        with args.data_image.open('rb') as seed:
            if hashlib.file_digest(seed,'sha256').hexdigest()!=board.get('data_image_sha256'):
                parser.error('NAND data seed hash mismatch')
    if args.bluetooth == 'bluez' and sys.platform == 'darwin':
        print('macOS detected: BlueZ backend unavailable; using reject fixture.', flush=True)
        args.bluetooth = 'reject'
    explicit_debug = any(token.split('=')[0] in debug_options for token in sys.argv[1:])
    args.profile = args.profile or ('custom' if explicit_debug else 'watch')
    if args.profile != 'custom' and explicit_debug:
        parser.error('Low-level peripheral flags require --profile custom')
    if args.profile == 'watch':
        for name in WATCH_FLAGS:
            setattr(args, name, True)
        args.revision_id = 5
    if not args.skip_oobe:
        args.skip_binding = False
    if args.skip_binding:
        if args.profile != 'watch':
            parser.error('--skip-binding requires the watch profile')
        if args.resource is None:
            args.resource = here / 'firmware/vela_resource.bin'
    if args.bluetooth != 'reject':
        if args.profile != 'watch':
            parser.error('--bluetooth bluez/hci requires --profile watch')
        if args.bluetooth == 'bluez' and not (here / 'build/bluez-radio').is_file():
            parser.error('Build the Bluetooth backend with bash emulation/tools/build-bluez.sh')
        args.bt_hci_reset_fixture = args.bt_hci_reject_fixture = False
    if args.bluetooth == 'hci' and (not args.hci_socket or not args.hci_socket.is_socket()):
        parser.error('--bluetooth hci requires an existing --hci-socket from hci_bridge.py')
    if args.hci_socket and args.bluetooth != 'hci':
        parser.error('--hci-socket requires --bluetooth hci')
    if args.factory_image is None and args.bluetooth == 'hci':
        configured_factory = here / 'device/hci-factory.bin'
        if configured_factory.is_file():
            args.factory_image = configured_factory
    if args.factory_image and (not args.factory_image.is_file() or
                               args.factory_image.stat().st_size != 128 * 1024):
        parser.error('--factory-image requires a 128 KiB Q66 LittleFS partition')
    if (args.tap or args.serve is not None or args.window or args.python_window) and not args.touch:
        parser.error('--tap/--serve/--window/--python-window require the watch profile or a custom touch model')
    if args.touch and not args.lcd_frame_capture:
        parser.error('--touch requires --lcd-frame-capture')
    if args.serve is not None and not 0 <= args.serve <= 65535:
        parser.error('--serve port must be between 0 and 65535')
    if args.serve is not None or args.tap or args.python_window:
        try:
            import PIL.Image
        except ImportError:
            parser.error('--serve/--tap/--python-window require Pillow: python3 -m pip install Pillow')
    if args.python_window:
        try:
            import tkinter
            import PIL.ImageTk
        except ImportError:
            parser.error('--python-window requires a Python installation with tkinter and Pillow ImageTk')
    if args.gpu_id_fixture and not args.gpu_probe:
        parser.error('--gpu-id-fixture requires --gpu-probe')
    if args.gpu_no_render and not args.gpu_id_fixture:
        parser.error('--gpu-no-render requires --gpu-id-fixture')
    if args.gpu_completion_fixture and not args.gpu_no_render:
        parser.error('--gpu-completion-fixture requires --gpu-no-render')
    if args.gpu_software_renderer and (not args.gpu_id_fixture or args.gpu_no_render or
                                       args.gpu_completion_fixture):
        parser.error('--gpu-software-renderer requires --gpu-id-fixture and excludes no-render/completion fixture')
    if args.spi0_unconnected and args.gh3x2x_fixture:
        parser.error('--spi0-unconnected and --gh3x2x-fixture are mutually exclusive')
    if args.bt_tx_capture and not args.bt_peer:
        parser.error('--bt-tx-capture requires --bt-peer')
    if args.bt_hci_reset_fixture and not args.bt_tx_capture:
        parser.error('--bt-hci-reset-fixture requires --bt-tx-capture')
    if args.bt_hci_reject_fixture and not args.bt_hci_reset_fixture:
        parser.error('--bt-hci-reject-fixture requires --bt-hci-reset-fixture')
    if args.lcd_frame_capture and not args.panel_id_fixture:
        parser.error('--lcd-frame-capture requires --panel-id-fixture')
    if args.panel_te_fixture and not args.lcd_frame_capture:
        parser.error('--panel-te-fixture requires --lcd-frame-capture')
    if args.gh3x2x_reg0724 is not None:
        if not args.gh3x2x_fixture or not 0 <= args.gh3x2x_reg0724 <= 0xffff:
            parser.error('--gh3x2x-reg0724 requires --gh3x2x-fixture and a 16-bit value')
    if args.gdb is not None and not 0 <= args.gdb <= 65535:
        parser.error('--gdb port must be between 0 and 65535')
    if args.gdb_wait and args.gdb is None:
        parser.error('--gdb-wait requires --gdb')
    # A debugger session has no implicit lifetime: halted time is the user's.
    duration = (args.seconds if args.seconds is not None else
                None if args.gdb is not None else
                None if (args.serve is not None or args.window or args.python_window or args.profile == 'watch') else 3)
    if duration is not None and (not duration > 0 or not math.isfinite(duration)):
        parser.error('--seconds must be positive')
    args.qemu = args.qemu.resolve()
    args.firmware = args.firmware.resolve()
    if not args.qemu.is_file():
        parser.error(f'QEMU executable missing: {args.qemu}')
    if not args.firmware.is_file():
        parser.error(f'Firmware missing: {args.firmware}')
    if args.resource and not args.resource.is_file():
        parser.error(f'Resource image missing: {args.resource}')
    if args.audio_image and not args.audio_image.is_file():
        parser.error(f'Audio image missing: {args.audio_image}')
    if args.sensor_image and not args.sensor_image.is_file():
        parser.error(f'Sensor image missing: {args.sensor_image}')
    try:
        display_backend = qemu_display_backend(args.qemu, args.window)
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        parser.error(str(error))
    logdir = args.log_dir.resolve()
    logdir.mkdir(parents=True, exist_ok=True)
    factory_source = args.factory_image
    if virtual_state is not None and instance_nor is None:
        parser.error('Virtual mode requires a private NOR transaction')
    if virtual_state is not None and args.virtual_peer_output != logdir/'virtual-peer-state.pending.json':
        parser.error('Virtual peer output must be in this private run')
    if args.skip_binding and instance_nor is None:
        from .factory import local_desktop_image
        desktop_factory = logdir / 'desktop-factory.bin'
        if factory_source and factory_source.resolve() == desktop_factory:
            parser.error('--factory-image must differ from the generated desktop-factory.bin; use another --log-dir')
        try:
            image = local_desktop_image(factory_source, property_file)
        except (OSError, ValueError, RuntimeError) as error:
            parser.error(str(error))
        args.factory_image = desktop_factory
        with args.factory_image.open('wb') as stream:
            args.factory_image.chmod(0o600)
            stream.write(image)
        print('Local setup properties installed; desktop boot still requires verification. No account binding.', flush=True)
    if args.skip_binding and board.get('machine') == 'azki-p65':
        # P65 never mounts the Q65 misc LittleFS as /misc: its rc.sysinit
        # sources properties from /tmp/factory.prop and /etc/build.prop in the
        # AP ROMFS. Mirror the verified reference fixture: patch a boot copy of
        # the AP's /build.prop with the local OOBE properties; the original
        # firmware file stays untouched.
        from p65_setup import local_boot_image
        boot_firmware = logdir / 'p65-local-setup-ap.bin'
        if boot_firmware == args.firmware:
            parser.error('--firmware must be the original image, not the generated boot copy')
        guest_properties = None
        if virtual_state is not None:
            from .virtual_peer.state import boot_properties
            guest_properties = boot_properties(virtual_state)
        try:
            image = local_boot_image(args.firmware.read_bytes(), guest_properties)
        except (OSError, ValueError, RuntimeError) as error:
            # Require a structurally valid P65 watch ROMFS; never fall back to
            # an unmodified OOBE image or another model's firmware.
            parser.error(str(error))
        with boot_firmware.open('wb') as stream:
            boot_firmware.chmod(0o600)
            stream.write(image)
        args.firmware = boot_firmware
        print('P65 local OOBE properties prepared in /etc/build.prop (boot copy; original firmware unchanged).', flush=True)
    if instance_nor is not None and instance_nor['norInitialize']:
        try:
            initialize_startup_nor(instance_nor, virtual_state,
                                   skip_oobe=args.skip_oobe, property_file=property_file)
        except (OSError, ValueError, RuntimeError) as error:
            parser.error('NOR instance initialization failed: ' + str(error))
        print('First-run setup: ' + ('skip OOBE.' if args.skip_oobe else 'firmware OOBE; no local binding injected.'), flush=True)
    elif instance_nor is not None:
        print('Existing instance setup and pairing preserved; the OOBE preference only applies to a new instance.', flush=True)
    if args.instance_data_seed:
        try:
            if not args.skip_oobe:
                raise ValueError('Interactive OOBE must not use a completed-setup NAND seed')
            proof = board.get('local_ui_seed', {})
            if (instance_storage is None or instance_nor is None or not instance_nor['norInitialize']
                    or proof.get('profile_id') != board['profile_id']
                    or proof.get('partition', {}).get('offset') != instance_storage['dataOffset']
                    or proof.get('partition', {}).get('size') != instance_storage['dataBytes']):
                raise ValueError('Data seed requires this exact profile and a fresh NOR/NAND transaction')
            seeded = initialize_instance_data(instance_storage, args.instance_data_seed, initialize=True)
        except (OSError, ValueError, RuntimeError) as error:
            parser.error('Instance UI initialization failed: ' + str(error))
        print('First local UI data initialized.' if seeded else 'Existing instance UI data preserved.', flush=True)
    instance_settings = None
    boot_clock = None
    if args.instance_settings:
        try:
            instance_settings = load_instance_settings(args.instance_settings, instance_storage)
            clock = boot_clock = apply_instance_settings(instance_storage, instance_settings, here / 'build/instance-props',
                                                         logdir / 'tmp' if (logdir / 'tmp').is_dir() else tempfile.gettempdir())
        except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
            parser.error('Instance settings failed: ' + str(error))
        print('Instance settings applied' + (f'; boot clock {clock}.' if clock is not None else '.'), flush=True)
    # Remove snapshots from previous runs so absent evidence cannot look current.
    for name in ('registers.txt', 'scb.txt', 'result.json', 'qemu.log',
                 'uart0.log', 'uart1.log', 'uart2.log', 'hci-tx.json', 'panel-spi.json',
                 'lcd-frames.json', 'lcd-frame.ppm', 'lcd-frame.png', 'hci-rx.json',
                 'gh3x2x-spi.json', 'gpu-commands.json', 'touch-input.json', 'before-tap.png',
                 'lcd-frame.raw', 'control.json', 'stack.txt', 'pc-memory.txt'):
        (logdir / name).unlink(missing_ok=True)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    deadline_reached = interrupted = False
    window_closed = False
    tap_index = 0
    registers = qmp_error = guest_status = scb = None
    diagnostic_errors = []
    frame_file = logdir / 'lcd-frame.raw'
    with tempfile.TemporaryDirectory(prefix='q66-qmp-') as temporary, ExitStack() as cleanup:
        qmp_path = Path(temporary) / 'qmp.sock'
        machine = board.get('machine', 'best1503')
        isolated_p65 = machine == 'azki-p65'
        # Generic BEST1503 consumes board geometry as QOM properties. The
        # isolated P65 core has a fixed memory map and rejects those options.
        if machine == 'best1503':
            for name, value in geometry.items():
                machine += f',{name}={value}'
        if args.bluetooth == 'virtual':
            from .virtual_peer.service import PeerService
            virtual_peer = PeerService(temporary,logdir,virtual_state,args.virtual_peer_output,args.virtual_peer_state_sha256).start()
            cleanup.callback(virtual_peer.close)
            radio_socket = virtual_peer.hci_path
            machine += ',bt-hci-chardev=radio'
        elif args.bluetooth == 'bluez':
            radio_log = cleanup.enter_context((logdir / 'bluez.log').open('w'))
            radio_socket = Path(temporary) / 'watch-h4.sock'
            phone_socket = Path(temporary) / 'phone-h4.sock'
            radio = subprocess.Popen([str(here / 'build/bluez-radio'), str(radio_socket),
                                      str(phone_socket)], stdout=radio_log, stderr=radio_log)
            cleanup.callback(close_process, radio)
            deadline = time.monotonic() + 5
            while not radio_socket.exists():
                if radio.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError('BlueZ virtual radio failed to start; see bluez.log')
                time.sleep(.02)
            machine += ',bt-hci-chardev=radio'
            print('Phone H4: ' + str(phone_socket), flush=True)
        elif args.bluetooth == 'hci':
            radio_socket = args.hci_socket.resolve()
            machine += ',bt-hci-chardev=radio'
            print('Real Bluetooth H4: ' + str(radio_socket), flush=True)
        if boot_clock is not None and not isolated_p65:
            # The PMU RTC (read directly by some firmware) starts at the same wall time.
            machine += f',rtc-seconds={boot_clock}'
        if args.resource:
            machine += ',resource-image=' + str(args.resource.resolve()).replace(',', ',,')
        if args.audio_image:
            machine += ',audio-image=' + str(args.audio_image.resolve()).replace(',', ',,')
        if args.sensor_image:
            machine += ',sensor-image=' + str(args.sensor_image.resolve()).replace(',', ',,')
        if instance_storage and not isolated_p65:
            machine += instance_machine_options(instance_storage)
            print('Private instance NAND transaction: ' + instance_storage['dataImage'], flush=True)
        if instance_nor:
            # The isolated P65 core implements the same compact catalog NOR
            # writeback as the generic BEST1503 machine; its NAND stays RAM-only.
            machine += instance_nor_machine_options(instance_nor)
            print('Private instance NOR transaction: ' + instance_nor['norImage'], flush=True)
        if args.data_image and not isolated_p65:
            machine += ',data-image=' + str(args.data_image.resolve()).replace(',', ',,')
            print('Local UI NAND seed: ' + str(args.data_image.resolve()), flush=True)
        if args.factory_image:
            machine += ',factory-image=' + str(args.factory_image.resolve()).replace(',', ',,')
            print('Factory storage: ' + str(args.factory_image.resolve()), flush=True)
        for name in DEBUG_FLAGS:
            value = getattr(args, name)
            if name == 'gh3x2x_reg0724':
                if value is not None:
                    machine += f',gh3x2x-reg0724={value}'
            elif name == 'revision_id':
                machine += f',revision-id={value}'
            elif value:
                machine += ',' + name.replace('_', '-') + '=on'
        if instance_settings is not None and not isolated_p65:
            # Host-authored board inputs resolved against this instance's
            # hardware preset; a rejected value stops the boot instead of
            # silently running with a different device.
            try:
                for option in instance_settings_options(instance_settings, adc_fixture=args.power_adc_fixture,
                                                        ntc_channels=geometry.get('pmu-adc-ntc', 0),
                                                        ntc_sample=geometry.get('pmu-adc-ntc-room', 8990)):
                    machine += ',' + option
            except ValueError as error:
                parser.error('Instance settings failed: ' + str(error))
        if args.lcd_frame_capture:
            machine += ',lcd-frame-file=' + str(frame_file).replace(',', ',,')
        command = [str(args.qemu), '-M', machine, '-display', display_backend,
                   '-monitor', 'none', '-no-reboot', '-kernel', str(args.firmware),
                   '-qmp', f'unix:{qmp_path},server=on,wait=off',
                   '-d', 'in_asm,int,unimp,guest_errors' if args.trace else 'guest_errors', '-D', str(logdir / 'qemu.log')]
        if args.bluetooth != 'reject':
            command += ['-chardev', f'socket,id=radio,path={radio_socket}']
        # Local QEMU diagnostics intentionally include raw H4/link material.
        # Keep them in the private run directory; redact before sharing.
        # Only the explicitly requested fault-only mode narrows the output.
        if args.fault_diagnostics:
            command[command.index('-d')+1] = 'int'
        for index in range(3):
            uart_path = logdir / f'uart{index}.log'
            uart_path.touch(mode=0o600)
            uart_path.chmod(0o600)
            command += ['-serial', f'file:{uart_path}']
        if args.gdb is not None:
            # Loopback only. The debugger is a local tool the user asked for; it
            # coexists with the virtual Bluetooth peer, whose protocol clocks
            # stand still while the CPU is halted (see GDB_STATE_POLL_SECONDS).
            command += ['-gdb', f'tcp:127.0.0.1:{args.gdb}']
            if args.gdb_wait:
                command.append('-S')
        if args.trace:
            print(shlex.join(command), flush=True)
        else:
            print(f'Firmware profile: {args.profile}; logs: {logdir}', flush=True)
        with (logdir / 'stderr.log').open('w') as stderr:
            started = time.monotonic()
            proc = subprocess.Popen(command, stdout=stderr, stderr=stderr)
            if virtual_peer is not None: virtual_peer.attach_core(proc.pid)
            control = None
            connection = None
            native_window = None
            try:
                if proc.poll() is None:
                    # A freshly copied, quarantined core is assessed by macOS on
                    # its first exec (several seconds on a cold machine). A live
                    # QEMU gets a generous window; an exited one fails at once.
                    while not qmp_path.exists():
                        if proc.poll() is not None:
                            raise RuntimeError('QEMU exited before its control socket started')
                        if time.monotonic() - started > QMP_START_SECONDS:
                            raise RuntimeError('QEMU control socket did not start')
                        time.sleep(.02)
                    connection = QMP(qmp_path)
                    control = WatchControl(connection, temporary,
                        bluetooth_log=logdir/'qemu.log' if args.bluetooth in ('hci','bluez') else None,
                        log_directory=logdir, frame_file=frame_file if args.lcd_frame_capture else None)
                    input_socket = Path(temporary) / 'input.sock'
                    adc_raw = control.probe_adc_raw() if args.power_adc_fixture else False
                    control.ntc_channels = geometry.get('pmu-adc-ntc', 0)
                    if instance_settings is not None and instance_settings.get('battery'):
                        control.battery_channel = instance_settings['battery']['channel']
                    has_input = args.touch or adc_raw
                    if has_input:
                        control.serve_input(input_socket)
                    gdb_endpoint = None
                    if args.gdb is not None:
                        gdb_endpoint = {'host': '127.0.0.1', 'port': args.gdb, 'waitForAttach': bool(args.gdb_wait)}
                        for chardev in control.execute('query-chardev'):
                            match = GDB_CHARDEV.search(chardev.get('filename', '')) if chardev.get('label') == 'gdb' else None
                            if match:
                                gdb_endpoint['port'] = int(match[1])
                        if not 0 < gdb_endpoint['port'] <= 65535:
                            raise RuntimeError('GDB server port could not be determined')
                        print(f"GDB server: target remote 127.0.0.1:{gdb_endpoint['port']}"
                              + ('; CPU held at reset until the debugger continues.' if args.gdb_wait else ''), flush=True)
                    # best1502p machines (azki-p65) have no band ADC ABI: the
                    # parent strips the pmuAdcRaw capability from their plan
                    # and its control validator rejects a hardware_inputs
                    # record, so only best1503 runners publish one.
                    control_record = dict(
                        qmp=str(qmp_path), framebuffer=str(frame_file), pid=proc.pid,
                        input_socket=str(input_socket) if has_input else None)
                    if not isolated_p65:
                        control_record['hardware_inputs'] = {'pmu.adcRaw': {
                            'supported': adc_raw, 'schema': 1, 'channels': 8,
                            'minimum': 0, 'maximum': 65535,
                            'unit': 'uncalibrated-raw',
                            'readEvidence': 'guest-ispi-fifo-dequeue'}}
                    if virtual_peer is not None:
                        control_record['virtual_peer'] = virtual_peer.descriptor()
                    if gdb_endpoint is not None:
                        control_record['gdb'] = gdb_endpoint
                    (logdir / 'control.json').write_text(json.dumps(control_record, indent=2) + '\n')
                    if args.lcd_frame_capture:
                        print('LCD framebuffer: ' + str(frame_file), flush=True)
                    if args.serve is not None:
                        print('Watch: ' + control.serve(args.serve), flush=True)
                    if args.window:
                        print('QEMU native window started (' + display_backend + ').', flush=True)
                    if args.python_window:
                        native_window = NativeWindow(control)
                        native_window.start()
                        print('Python window started (direct QEMU LCD; 30 FPS refresh).', flush=True)
                    # azki-p65 does not expose the generic battery-charger
                    # QOM property used by BEST1503 sessions. Keep settings
                    # for host bookkeeping, but never issue unsupported QMP
                    # writes to the isolated P65 core.
                    runtime_settings = (instance_settings_runtime(instance_settings)
                                        if instance_settings is not None and not isolated_p65 else [])
                    # Guest-progress seconds: wall time minus time the CPU was
                    # held by a debugger (or -S). Without --gdb this is wall time.
                    halted_total, halted_since, state_check_due = 0.0, None, 0
                    def guest_seconds(now):
                        return now - halted_total - (now - halted_since if halted_since is not None else 0)
                    runtime_settings_due = guest_seconds(time.monotonic()) + RUNTIME_SETTINGS_DELAY
                    walk_steps = instance_settings_walk(instance_settings) if instance_settings is not None else 0
                    walk_due = guest_seconds(time.monotonic()) + WALK_START_DELAY
                    tap_index, pressed, due = 0, False, 0
                    fault_check_due = 0
                    fault_records = []
                    last_fault = None
                    with (logdir / 'qemu.log').open('rb') as live_trace:
                        tail = b''
                        ready = False
                        while proc.poll() is None:
                            now = time.monotonic()
                            if virtual_peer is not None and virtual_peer.failure:
                                raise RuntimeError('Virtual peer service failed')
                            if args.gdb is not None and now >= state_check_due:
                                state_check_due = now + GDB_STATE_POLL_SECONDS
                                run_state = control.execute('query-status')
                                running = run_state.get('running', True) is True
                                if not running and run_state.get('status') in ('debug', 'paused') and not any(
                                        chardev.get('label') == 'gdb' and not chardev.get('filename', '').startswith('disconnected:')
                                        for chardev in control.execute('query-chardev')):
                                    # The debugger left without detaching (crash, closed
                                    # terminal). Do not leave the device frozen with nobody
                                    # able to continue it; an attached client keeps control.
                                    control.execute('cont')
                                    running = True
                                    print('GDB client disconnected; guest resumed.', flush=True)
                                if running and halted_since is not None:
                                    halted_total += now - halted_since; halted_since = None
                                elif not running and halted_since is None:
                                    halted_since = now
                                if virtual_peer is not None:
                                    virtual_peer.set_guest_running(running)
                            if duration is not None and now - started >= duration:
                                raise subprocess.TimeoutExpired(command, duration)
                            if native_window is not None and not native_window.pump():
                                window_closed = True
                                break
                            tail = (tail + live_trace.read())[-65536:]
                            if runtime_settings and guest_seconds(now) >= runtime_settings_due:
                                for name, value in runtime_settings:
                                    control.execute('qom-set', {'path': '/machine', 'property': name, 'value': value})
                                runtime_settings = []
                            if walk_steps and guest_seconds(now) >= walk_due:
                                # A live walk already requested by the host is kept.
                                state = control.health()
                                if state['walkSteps'] < walk_steps:
                                    control.health({'walkSteps': walk_steps})
                                walk_steps = 0
                            if now >= fault_check_due:
                                fault_check_due = now + 1
                                fault_status = control.execute('human-monitor-command',
                                    {'command-line': 'xp /11wx 0xe000ed00'})
                                if cortex_fault_status(fault_status) or cortex_fault_handler(fault_status):
                                    fault = (cortex_fault_status(fault_status), cortex_fault_handler(fault_status))
                                    if fault != last_fault and len(fault_records) < 8:
                                        # Save evidence before any input/peer cleanup. CFSR is
                                        # sticky and a sampled handler may recover, so this
                                        # diagnostic alone never ends an interactive session.
                                        (logdir/'fault-scb.txt').write_text(fault_status)
                                        fault_registers = control.execute('human-monitor-command', {'command-line':'info registers'})
                                        if args.fault_diagnostics:
                                            sp_match = re.search(r'R13=([0-9a-fA-F]+)', fault_registers)
                                            if sp_match:
                                                sp = int(sp_match[1],16)
                                                if 0x20000000 <= sp <= 0x20160000-128:
                                                    stack = control.execute('human-monitor-command', {'command-line':f'xp /32wx 0x{sp:x}'})
                                                    # Keep only code/stack addresses and exception-return
                                                    # markers, never arbitrary stack data or key material.
                                                    words = [int(w,16) for line in stack.splitlines() for w in re.findall(r'0x([0-9a-fA-F]{8})',line.partition(':')[2])]
                                                    pointers = [{'slot':index,'address':f'0x{word:08x}'} for index,word in enumerate(words)
                                                        if (0x00200000 <= word < 0x00360000 or 0x0c000000 <= word < 0x0d000000
                                                            or 0x20000000 <= word < 0x20160000 or word in (0xfffffff1,0xfffffff9,0xfffffffd))]
                                                    (logdir/'fault-stack-addresses.json').write_text(json.dumps({'sp':f'0x{sp:08x}','pointers':pointers},indent=2)+'\n')
                                        if virtual_peer is not None:
                                            pattern = r'R(?:13|14|15)=[0-9a-fA-F]+' if args.fault_diagnostics else r'R(?:14|15)=[0-9a-fA-F]+'
                                            fault_registers = ' '.join(re.findall(pattern, fault_registers))
                                        (logdir/'fault-registers.txt').write_text(fault_registers)
                                        fault_records.append({'elapsedSeconds':round(now-started,3),
                                            'cfsr':fault[0], 'handler':fault[1], 'registers':fault_registers})
                                        (logdir/'fault-observations.json').write_text(json.dumps(fault_records,indent=2)+'\n')
                                        print('Guest SCB fault observed; diagnostic snapshot saved.', flush=True)
                                    last_fault = fault
                                    if args.stop_on_fault:
                                        break
                            if re.search(rb'fault address 0x|Taking exception \d+ \[[^\]]*(?:Abort|Fault|Undef|Invalid)', tail):
                                if args.stop_on_fault:
                                    print('Guest fault detected; explicit diagnostic stop requested.', flush=True)
                                    break
                            if not ready and args.tap and b'BES experimental LCD frame captured:' in tail:
                                try:
                                    ready = control.frame_image().getbbox() is not None
                                except (OSError, RuntimeError, ConnectionError):
                                    pass
                            if ready and tap_index < len(args.tap) and now >= due:
                                x, y = args.tap[tap_index]
                                if not pressed:
                                    if tap_index == 0:
                                        (logdir / 'before-tap.png').write_bytes(control.frame())
                                    control.touch(x, y, True)
                                    pressed, due = True, now + .2
                                else:
                                    control.touch(x, y, False)
                                    tap_index += 1
                                    pressed, due = False, now + 1
                            time.sleep(.01 if native_window is not None else .05)
            except subprocess.TimeoutExpired:
                deadline_reached = True
            except KeyboardInterrupt:
                interrupted = True
            except (OSError, ValueError, RuntimeError, ConnectionError) as error:
                qmp_error = str(error)
            finally:
                runtime_seconds = time.monotonic() - started
                shutdown_start = time.monotonic()
                shutdown_phases = []
                def shutdown_phase(name):
                    shutdown_phases.append({'phase':name,'elapsedSeconds':round(time.monotonic()-shutdown_start,6)})
                    (logdir/'shutdown-phases.json').write_text(json.dumps(shutdown_phases)+'\n')
                shutdown_phase('begin')
                if virtual_peer is not None:
                    try: virtual_peer.close()
                    except (OSError,RuntimeError): diagnostic_errors.append('Virtual peer shutdown failed')
                    shutdown_phase('peer-closed')
                if native_window is not None:
                    try:
                        native_window.close()
                    except (OSError, RuntimeError) as error:
                        qmp_error = qmp_error or str(error)
                if control is not None:
                    try:
                        close_control_bounded(control)
                    except (OSError, RuntimeError) as error:
                        qmp_error = qmp_error or ('Control shutdown: ' + str(error))
                    (logdir / 'touch-input.json').write_text(json.dumps(control.events, indent=2) + '\n')
                shutdown_phase('input-closed')
                if proc.poll() is None:
                    try:
                        connection = connection or QMP(qmp_path)
                        guest_status = connection.execute('query-status')
                        connection.execute('stop')
                        registers = connection.execute('human-monitor-command',
                            {'command-line': 'info registers'})
                        if virtual_peer is not None:
                            registers = ' '.join(re.findall(r'R(?:14|15)=[0-9a-fA-F]+',registers))
                        (logdir / 'registers.txt').write_text(registers)
                        for reg, filename, count in ([] if virtual_peer is not None else [('13','stack.txt',128),('15','pc-memory.txt',32)]):
                            match=re.search(r'R'+reg+r'=([0-9a-fA-F]+)',registers)
                            if match:
                                address=int(match[1],16)
                                intervals=[(0x20000000,0x20160000),(0x00200000,0x00360000),
                                    (0x0c000000,0x0c000000+geometry.get('flash-bytes',0x1000000)),
                                    (0x38000000,0x3a000000),(0x3c000000,0x3e000000)]
                                remaining=next(((hi-address)//4 for lo,hi in intervals if lo<=address<hi),0)
                                if remaining:
                                    count=min(count,remaining)
                                    dump=connection.execute('human-monitor-command',
                                        {'command-line':f'xp /{count}wx 0x{address:x}'})
                                    (logdir/filename).write_text(dump)
                        scb = connection.execute('human-monitor-command',
                            {'command-line': 'xp /32wx 0xe000ed00'})
                        (logdir / 'scb.txt').write_text(scb)
                        shutdown_phase('health-captured')
                    except (OSError, ValueError, RuntimeError, ConnectionError) as error:
                        diagnostic_errors.append('Shutdown state capture: ' + str(error))
                    finally:
                        if connection is not None:
                            try:
                                connection.execute('quit')
                            except (OSError, ValueError, RuntimeError, ConnectionError) as error:
                                diagnostic_errors.append('QEMU quit: ' + str(error))
                        if proc.poll() is None:
                            try:
                                proc.wait(timeout=2)
                            except subprocess.TimeoutExpired:
                                proc.terminate()
                                try:
                                    proc.wait(timeout=2)
                                except subprocess.TimeoutExpired:
                                    proc.kill()
                                    proc.wait()
                shutdown_phase('core-reaped')
                if connection is not None:
                    try:
                        connection.close()
                    except (OSError, RuntimeError) as error:
                        diagnostic_errors.append('QMP close: ' + str(error))
                process_seconds = time.monotonic() - started
                if control is not None and args.lcd_frame_capture:
                    try:
                        control.save_frame(logdir / 'lcd-frame.ppm')
                    except (ImportError, OSError, ValueError, RuntimeError) as error:
                        diagnostic_errors.append('LCD snapshot: ' + str(error))
                (logdir / 'control.json').unlink(missing_ok=True)
                shutdown_phase('control-removed')
    trace = (logdir / 'qemu.log').read_text(errors='replace') if (
        logdir / 'qemu.log').exists() else ''
    faults = list(dict.fromkeys(re.findall(r'fault address (0x[0-9a-fA-F]+)', trace)))
    exceptions = list(dict.fromkeys(re.findall(r'Taking exception \d+ \[([^\]]+)\]', trace)))
    guest_faults = [name for name in exceptions if any(marker in name.lower()
                    for marker in ('fault', 'abort', 'undef', 'invalid'))]
    cfsr = cortex_fault_status(scb)
    if cfsr and not guest_faults:
        guest_faults.append(f'SCB CFSR=0x{cfsr:08x}')
    if cortex_fault_handler(scb) and not guest_faults:
        guest_faults.append(cortex_fault_handler(scb) + ' handler active')
    # The original power-off path ends here even without a CPU exception.
    # A bounded run that reaches this halt must not look like healthy waiting.
    halted = bool(registers and re.search(r'R15=00274a(?:26|46)\b', registers))
    guest_power = guest_power_event(logdir / 'stderr.log')
    if guest_power and proc.returncode == 0 and not deadline_reached and not interrupted:
        # The guest itself powered off or requested a reset (PMU SOFT_POWER_OFF,
        # AON global reset, watchdog); the host mirrors it instead of failing.
        outcome = 'guest_poweroff' if guest_power == 'poweroff' else 'guest_reboot'
    elif proc.returncode != 0 or qmp_error:
        outcome = 'host_error'
    elif faults or guest_faults:
        outcome = 'guest_fault'
    elif halted:
        outcome = 'guest_halted'
    elif interrupted:
        outcome = 'interrupted'
    elif window_closed:
        outcome = 'window_closed'
    elif deadline_reached and tap_index < len(args.tap):
        outcome = 'input_incomplete'
    elif deadline_reached:
        outcome = 'time_limit'
    else:
        outcome = 'qemu_stopped'
    milestones = {
        'ram_timing': 'BES RAM_CFG =',
        'pmu_id': 'reg=0x00 -> 0x1530',
        'flash_jedec': 'BES NOR command=0x9f',
        'flash_wrap': 'BES NOR command=0x77',
        'psram_mode_read': 'BES PSRAM command=0x06',
        'psram_calibration_read': 'BES PSRAM command=0x02',
        'uart_configuration': '0x0c9351b8:',
        'dma_initialization': '0x0c930d30:',
        'watchdog_initialization': '0x0c935558:',
        'watchdog_configuration_completed': '0x0c93556c:',
        'i2c_initialization': '0x0c9323f8:',
        'bluetooth_patch_installation': '0x0c92a268:',
        'bluetooth_dump_configuration': '0x0c928cb4:',
        'bluetooth_rf_control': '0x0c92a198:',
        'bluetooth_modem_configuration_completed': '0x0c92a1c6:',
        'bluetooth_wakeup_request': 'BES MCU-to-BT wake request pending',
        'bluetooth_wakeup_timeout': '0x0c92c9b0:',
        'bluetooth_wakeup_acknowledged': 'BES experimental BT peer acknowledged wake',
        'bluetooth_ideal_iq_sample': 'BES experimental ideal IQ sample:',
        'bluetooth_iq_search_returned': '0x0c92b99a:',
        'bluetooth_calibration_state_restored': '0x0c92b9c8:',
        'bluetooth_intersys_send': '0x0c933b76:',
        'bluetooth_intersys_completion_wait': '0x0c92b80a:',
        'bluetooth_tx_captured': 'BES experimental BT TX captured:',
        'bluetooth_tx_original_callback': '0x0c92b62c:',
        'bluetooth_configuration_upload_returned': '0x0c92a10a:',
        'bluetooth_poweron_register_upload_returned': '0x0c92a10e:',
        'bluetooth_revision_assertion': '0x0c92a062:',
        'gpio_configuration': 'BES GPIO ',
        'display_initialization': '0x0c934b20:',
        'display_configuration_returned': '0x0c934b9c:',
        'display_spi_backend_missing': 'BES LCDC SPI transfer unavailable:',
        'display_panel_id_fixture_read': 'BES experimental panel ID:',
        'display_panel_identified': '0x0c35db9c:',
        'display_panel_command_captured': 'BES experimental panel TX:',
        'display_frame_generator_requested': '0x0c9349da:',
        'display_frame_backend_missing': 'BES LCDC frame generator unavailable:',
        'display_lcd_initializer_returned': '0x0c928330:',
        'display_frame_captured': 'BES experimental LCD frame captured:',
        'display_original_frame_irq': '0x0c9286b4:',
        'nor_nv_programmed': 'BES NOR volatile NV program:',
        'spi0_configuration': 'BES SPI0 configuration:',
        'spi0_backend_missing': 'BES SPI0 transfer unavailable:',
        'sensor_original_ready_check': '0x0c6f93b8:',
        'sensor_original_scratch_check': '0x0c6f93da:',
        'sensor_original_config_table_uploaded': '0x0c6f9518:',
        'sensor_unknown_0724': 'BES experimental GH3X2X unsupported read: register=0x0724',
        'sensor_communication_failed': '0x0c6f94c6:',
        'sensor_initializer_returned_error': '0x0c6f94ca:',
        'gpu_identity_accepted': '0x0c92004c:',
        'gpu_identity_rejected': '0x0c9200ce:',
        'gpu_command_captured': 'BES GPU command ',
        'gpu_original_completion_irq': '0x0c926dd4:',
        'gpu_original_wait_returned': '0x0c92665c:',
        'display_original_te_callback': '0x0c934ad0:',
        'display_original_frame_irq_handler': '0x0c927940:',
        'display_original_frame_worker': '0x0c9286b4:',
        'sensor_0724_fixup_entered': '0x0c6f962c:',
        'sensor_unknown_01ce': 'BES experimental GH3X2X unsupported read: register=0x01ce',
        'sensor_0724_branch_returned': '0x0c6f93fa:',
        'sensor_trim_sequence_started': '0x0c6f941a:',
        'sensor_unknown_0718': 'BES experimental GH3X2X unsupported read: register=0x0718',
    }
    captured, received = [], []
    for event in re.finditer(
            r'BES experimental BT TX captured: ([0-9a-f ]+)\((?:no HCI response|transport only)\)'
            r'|BES experimental BT RX event: ([0-9a-f ]+)\((?:Reset|Reject) fixture\)'
            r'|BES backend H4 RX delivered: ([0-9a-f ]+)', trace):
        packet = bytes.fromhex(event[1] or event[2] or event[3])
        if event[1]:
            record = {'index': len(captured), 'packet_hex': packet.hex(' '),
                      'packet_type': 'command' if packet[0] == 1 else 'acl',
                      'hci_response_generated': False}
            if packet[0] == 1:
                record.update(opcode=f'0x{int.from_bytes(packet[1:3], "little"):04x}',
                              parameter_length=packet[3])
            else:
                record.update(handle=int.from_bytes(packet[1:3], 'little') & 0xfff,
                              data_length=int.from_bytes(packet[3:5], 'little'))
            captured.append(record)
        else:
            received.append({'index': len(received), 'packet_hex': packet.hex(' '),
                             'source': args.bluetooth + '-backend' if event[3] else
                             'experimental-reset-fixture' if packet[6] == 0 else 'unsupported-command-fixture',
                             'controller_rom_executed': False})
            response_opcode = None
            if packet[0] == 4 and packet[1] == 0x0e and len(packet) >= 7:
                response_opcode = int.from_bytes(packet[4:6], 'little')
            elif packet[0] == 4 and packet[1] == 0x0f and len(packet) >= 7:
                response_opcode = int.from_bytes(packet[5:7], 'little')
            for command_record in reversed(captured):
                if (command_record.get('opcode') and
                    int(command_record['opcode'], 16) == response_opcode and
                    not command_record['hci_response_generated']):
                    command_record['hci_response_generated'] = True
                    break
    if args.bt_tx_capture and virtual_peer is None:
        (logdir / 'hci-tx.json').write_text(json.dumps(captured, indent=2) + '\n')
    if virtual_peer is None and (args.bt_hci_reset_fixture or args.bluetooth != 'reject'):
        (logdir / 'hci-rx.json').write_text(json.dumps(received, indent=2) + '\n')
    panel_packets = []
    for event in re.finditer(
            r'BES experimental panel (?:ID: command=0x([0-9a-f]{2}) value=0x([0-9a-f]{2}) '
            r'\(RM690C0_XINLI fixture\)|TX: command=0x([0-9a-f]{2}) length=(\d+) '
            r'data=([0-9a-f ]*)\(capture only\))'
            r'|BES panel DCS power mode: command=0x(0a) value=0x([0-9a-f]{2})', trace):
        power_event = event[6] is not None
        read_event = event[1] is not None or power_event
        payload = bytes.fromhex(event[7] if power_event else event[2] if read_event else event[5])
        if not read_event and len(payload) != int(event[4]):
            raise RuntimeError('Panel capture length does not match log')
        panel_packets.append({'index': len(panel_packets),
                              'direction': 'read' if read_event else 'write',
                              'command': '0x' + (event[6] if power_event else event[1] if read_event else event[3]),
                              'data_hex': payload.hex(' '),
                              'source': 'panel-dcs-state' if power_event else
                                        'firmware-config-id-fixture' if read_event else 'original-AP',
                              'vendor_command_effects_emulated': False})
    if args.panel_id_fixture:
        (logdir / 'panel-spi.json').write_text(json.dumps(panel_packets, indent=2) + '\n')
    frames = lcd_frames(trace)
    if args.lcd_frame_capture:
        (logdir / 'lcd-frames.json').write_text(json.dumps(frames, indent=2) + '\n')
    sensor_events = []
    for m in re.finditer(r'BES experimental GH3X2X (read|write): '
                        r'register=(0x[0-9a-f]{4}) value=(0x[0-9a-f]{4})', trace):
        sensor_events.append({'index': len(sensor_events), 'direction': m[1],
                              'register': m[2], 'value': m[3],
                              'source': 'register-fixture' if m[1] == 'read' else 'original-AP',
                              'physical_sensor_emulated': False})
    if args.gh3x2x_fixture:
        (logdir / 'gh3x2x-spi.json').write_text(json.dumps(sensor_events, indent=2) + '\n')
    gpu_commands = gpu_command_records(trace) if args.gpu_probe else []
    if args.gpu_probe:
        (logdir / 'gpu-commands.json').write_text(json.dumps(gpu_commands, indent=2) + '\n')
    screenshot = logdir / 'lcd-frame.ppm'
    nonblack_pixels = None
    shared_frame_count = ((control.frame_sequence or 0) // 2) if control is not None else 0
    if screenshot.is_file():
        try:
            from PIL import Image
        except ImportError:
            pass  # PPM remains available without the optional Pillow package.
        else:
            with Image.open(screenshot) as picture:
                picture.save(logdir / 'lcd-frame.png')
                rgb = picture.convert('RGB')
                nonblack_pixels = sum(count for count, color in
                                      rgb.getcolors(rgb.width * rgb.height)
                                      if color != (0, 0, 0))
    storage_writeback_error = None
    if instance_storage:
        with (logdir / 'stderr.log').open('rb') as errors:
            errors.seek(0, 2)
            errors.seek(max(0, errors.tell() - 8192))
            for line in errors.read(8192).decode(errors='replace').splitlines():
                if any(marker in line for marker in ('NAND instance writeback failed',
                       'NAND instance erase writeback failed', 'Cannot open private NAND transaction',
                       'NOR instance writeback failed', 'Cannot open private NOR transaction')):
                    storage_writeback_error = line[:512]
    result = {'command': command, 'profile': args.profile, 'instruction_trace': args.trace,
              'touch_enabled': args.touch,
              'power_adc_fixture': args.power_adc_fixture,
              'bt_hci_reject_fixture': args.bt_hci_reject_fixture,
              'touch_input_count': trace.count('BES touch input:'),
              'touch_report_acknowledgements': trace.count('BES touch report acknowledged:'),
              'touch_i2c_rejection_count': trace.count('BES touch I2C rejected:'),
              'requested_tap_count': len(args.tap),
              'sent_tap_count': tap_index, 'returncode': proc.returncode,
              'outcome': outcome, 'boot_verified': False,
              'time_limit_reached': deadline_reached, 'interrupted': interrupted,
              'requested_seconds': duration, 'runtime_seconds': round(runtime_seconds, 3),
              'process_wall_seconds': round(process_seconds, 3),
              'window_closed': window_closed,
              'fault_addresses': faults, 'guest_status_before_stop': guest_status,
              'guest_exceptions': exceptions, 'guest_faults': guest_faults,
              'guest_halt_loop_reached': halted,
              'guest_power_event': guest_power,
              'registers': registers, 'scb': scb, 'qmp_error': qmp_error,
              'diagnostic_errors': diagnostic_errors,
              'cortex_fault_status': cfsr,
              'interrupt_trace_enabled': args.trace,
              'trace_milestones': {name: marker in trace for name, marker in milestones.items()},
              'bluetooth_controller_emulated': args.bluetooth in ('bluez','virtual'),
              'bluetooth_backend': args.bluetooth,
              'bluetooth_physical_transport_selected': args.bluetooth == 'hci',
              'bluetooth_real_phone_pairing_verified': False,
              'virtual_peer_state': virtual_peer.state_receipt() if virtual_peer is not None else None,
              'virtual_peer_error': virtual_peer.failure if virtual_peer is not None else None,
              'resource_image': str(args.resource.resolve()) if args.resource else None,
              'data_image': (instance_storage['dataImage'] if instance_storage else
                             str(args.data_image.resolve()) if args.data_image else None),
              'data_spare_image': instance_storage['spareImage'] if instance_storage else None,
              'data_writeback': instance_storage is not None,
              'instance_data_sha256': args.instance_data_sha256,
              'instance_spare_sha256': args.instance_spare_sha256,
              'storage_writeback_error': storage_writeback_error,
              'nor_image': instance_nor['norImage'] if instance_nor else None,
              'nor_writeback': instance_nor is not None,
              'nor_initialized': args.instance_nor_initialize if instance_nor else False,
              **instance_nor_result_geometry(instance_nor),
              'instance_nor_sha256': args.instance_nor_sha256,
              'factory_image': str(args.factory_image.resolve()) if args.factory_image else None,
              'factory_source_image': str(factory_source.resolve()) if factory_source else None,
              'local_setup_bypass': args.skip_binding,
              'skip_oobe_requested': args.skip_oobe,
              'oobe_initialization': ('existing-state-preserved' if instance_nor and not instance_nor['norInitialize']
                                      else 'skip' if args.skip_oobe else 'interactive'),
              'xiaomi_account_binding_completed': False,
              'bluetooth_peer_mode': 'wake-ideal-iq-experimental' if args.bt_peer else 'absent',
              'bluetooth_tx_capture': args.bt_tx_capture,
              'bluetooth_hci_responses': bool(received),
              'bluetooth_tx_packet_count': len(captured),
              'bluetooth_acl_tx_count': sum(p['packet_type'] == 'acl' for p in captured),
              'bluetooth_acl_rx_count': sum(p['packet_hex'].startswith('02 ') for p in received),
              'aon_revision_id': args.revision_id,
              'revision_id_source': 'model-choice',
              'display_panel_model': 'RM690C0_XINLI-id-and-tx-fixture' if args.panel_id_fixture else 'absent',
              'display_frame_capture_enabled': args.lcd_frame_capture,
              'display_frame_output': bool(frames or shared_frame_count) and screenshot.is_file(),
              'display_frame_count': max(shared_frame_count, max((frame['index'] for frame in frames), default=0)),
              'display_frame_log_count': len(frames),
              'display_framebuffer': str(frame_file) if args.lcd_frame_capture else None,
              'display_nonblack_pixel_count': nonblack_pixels,
              'display_ui_verified': False,
              'panel_te_fixture': args.panel_te_fixture,
              'panel_te_edge_count': trace.count('BES experimental panel TE edge:'),
              'sensor_gh3x2x_fixture': args.gh3x2x_fixture,
              'spi0_unconnected': args.spi0_unconnected,
              'gpu_probe': args.gpu_probe,
              'gpu_id_fixture': args.gpu_id_fixture,
              'gpu_no_render': args.gpu_no_render,
              'gpu_renderer_emulated': args.gpu_software_renderer,
              'gpu_renderer_mode': 'software-subset' if args.gpu_software_renderer else 'absent',
              'gpu_rendered_command_count': sum(c['executed'] for c in gpu_commands),
              'gpu_render_rejection_count': trace.count('BES GPU render rejected:'),
              'gpu_irq_emulated': args.gpu_completion_fixture or args.gpu_software_renderer,
              'gpu_completion_fixture': args.gpu_completion_fixture,
              'gpu_completion_count': trace.count('BES experimental GPU completion:'),
              'bt_hci_reset_fixture': args.bt_hci_reset_fixture,
              'bt_hci_reset_response_count': sum(p['source'] == 'experimental-reset-fixture' for p in received),
              'bt_hci_reject_response_count': sum(p['source'] == 'unsupported-command-fixture' for p in received),
              'bt_rx_acknowledgement_count': trace.count('BES experimental BT RX acknowledged by AP'),
              'gpu_command_capture_count': len(gpu_commands),
              'spi0_unconnected_rx_value': 255 if args.spi0_unconnected else None,
              'spi0_unconnected_transfer_count': trace.count('BES SPI0 unconnected transfer:'),
              'sensor_reg0724_reset_hypothesis': args.gh3x2x_reg0724,
              'sensor_reg0724_source': 'explicit-model-choice' if args.gh3x2x_reg0724 is not None else 'unknown',
              'sensor_samples_generated': False,
              'sensor_irq_emulated': False,
              'sensor_register_read_count': sum(p['direction'] == 'read' for p in sensor_events),
              'sensor_register_write_count': sum(p['direction'] == 'write' for p in sensor_events),
              'display_spi_transaction_count': len(panel_packets),
              'display_id_fixture_response_count': sum(p['source'] == 'firmware-config-id-fixture' for p in panel_packets),
              'display_power_mode_response_count': sum(p['source'] == 'panel-dcs-state' for p in panel_packets),
              'display_command_capture_count': sum(p['direction'] == 'write' for p in panel_packets),
              'machine': 'best1503 partial boot peripheral model',
              'firmware_modified': False,
              'firmware_sha256': firmware_sha256,
              'hash_verification': 'not-required-for-startup',
              'board_geometry': geometry,
              'boot_rom': 'synthetic reset vectors: SP=0x%08x, PC=0x%08x' % (
                  geometry.get('reset-sp', 0x2015fec0),
                  0x0c000011 + geometry.get('ap-offset', 0xc0000))}
    if (instance_settings is not None and instance_storage is not None and proc.returncode == 0
            and outcome in ('time_limit', 'interrupted', 'window_closed', 'qemu_stopped', 'guest_poweroff', 'guest_reboot')):
        # Informational snapshot of the firmware's own settings; never a health input.
        try:
            properties = read_instance_properties(instance_storage, here / 'build/instance-props',
                                                  logdir / 'tmp' if (logdir / 'tmp').is_dir() else tempfile.gettempdir())
            observed = logdir / 'instance-props-observed.json'
            fd = os.open(observed, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
            with os.fdopen(fd, 'w') as stream:
                json.dump({'properties': properties}, stream, ensure_ascii=False)
        except (OSError, ValueError, subprocess.SubprocessError):
            pass
    (logdir / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f'Outcome: {outcome}; QEMU exit: {proc.returncode}; boot not verified')
    print(f'Runtime: {runtime_seconds:.2f} s; LCD frames: {result["display_frame_count"]}')
    if args.tap:
        print(f'Taps sent: {tap_index}/{len(args.tap)}; reports acknowledged: ' +
              str(result['touch_report_acknowledgements']))
    if qmp_error:
        print('Control error: ' + qmp_error)
    for error in diagnostic_errors:
        print('Diagnostic warning: ' + error)
    if faults:
        print('Guest fault addresses: ' + ', '.join(faults))
    print(f'Logs: {logdir}')
    if virtual_peer is None: print((logdir / 'stderr.log').read_text(), end='')
    # A clean host shutdown does not establish a successful firmware boot.
    return 1 if outcome in ('host_error', 'guest_fault', 'guest_halted', 'input_incomplete') else 2


if __name__ == '__main__':
    raise SystemExit(main())
