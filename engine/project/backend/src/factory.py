#!/usr/bin/env python3
"""Prepare Q66 factory storage, including explicitly requested test identities."""
import argparse
import json
from pathlib import Path
import re
import secrets
import sys


def littlefs_class():
    try:
        from littlefs import LittleFS
    except ImportError:
        # Optional dependencies can be installed locally, without changing Python.
        dependencies = Path(__file__).resolve().parent.parent / '.deps'
        if dependencies.is_dir():
            sys.path.insert(0, str(dependencies))
        try:
            from littlefs import LittleFS
        except ImportError as error:
            raise RuntimeError('Factory storage requires littlefs-python; run '
                               'python -m pip install --target emulation/.deps littlefs-python==0.19.0') from error
    return LittleFS


def local_desktop_image(source=None, property_file='factory.prop'):
    """Overlay local setup completion, preserving the input factory partition."""
    LittleFS = littlefs_class()
    if property_file not in ('factory.prop', 'app_nv.prop'):
        raise ValueError('Unsupported local property file')
    fs = LittleFS(block_size=8192, block_count=16, read_size=2048,
                  prog_size=2048, cache_size=2048, lookahead_size=8, name_max=96,
                  mount=source is None)
    if source is not None:
        data = Path(source).read_bytes()
        if len(data) != 128 * 1024:
            raise ValueError('Expected a 128 KiB Q66 factory partition')
        fs.context.buffer[:] = data
        fs.mount()
    try:
        try:
            with fs.open(property_file, 'r') as stream:
                lines = stream.read().splitlines()
        except FileNotFoundError:
            lines = []
        # These are local UI properties, not Xiaomi authentication credentials.
        # The desktop normally enters touch gesture sleep (D104), which this
        # model does not yet implement. Keep this local debug UI awake through
        # the stock display settings: auto timeout is a boolean, dimmer is ms.
        overrides = {
            'persist.setup_wizard': '1',
            'persist.locale': 'zh_CN',
            'persist.screen.auto_screen_timeout': '0',
            'persist.screen.dimmer_timeout': '2147483647',
        }
        keys = tuple(overrides)
        lines = [line for line in lines if line.partition('=')[0].strip() not in keys]
        with fs.open(property_file, 'w') as stream:
            stream.write('\n'.join(lines + [f'{key}={value}' for key, value in overrides.items()]) + '\n')
    finally:
        fs.unmount()
    return bytes(fs.context.buffer)


def random_properties():
    return {
        'ro.factory.did': str(10**12 + secrets.randbelow(9 * 10**12)),
        'ro.factory.psk': secrets.token_hex(16),
        'ro.factory.psn': 'Q66' + secrets.token_hex(6).upper(),
    }


def product_definition(model):
    """Read model, PID and board identity from this target's original ROMFS."""
    firmware = Path(__file__).resolve().parents[1] / 'firmware/vela_ap.bin'
    data = firmware.read_bytes()
    marker = data.find(b'"product_info":')
    if marker < 0:
        raise ValueError('Original firmware product_info table not found')
    start = data.rfind(b'{', 0, marker)
    info, _ = json.JSONDecoder().raw_decode(data[start:start + 16384].decode(errors='replace'))
    return next((item for item in info['product_info'] if item['miwear_model'] == model), None)


def make_image(properties):
    LittleFS = littlefs_class()
    lines = []
    for key, value in properties.items():
        if key not in ('product.model', 'product.pid') and not re.fullmatch(r'ro\.factory\.[a-zA-Z0-9_]+', key):
            raise ValueError('Expected a ro.factory.* property or product.model/product.pid')
        if not isinstance(value, str) or not value or len(value.encode()) > 254 or any(
                c in value for c in '\x00\n\r'):
            raise ValueError(f'Invalid property value for {key}')
        lines.append(f'{key}={value}\n')
    # Captured from the original littlefs_bind configuration, not SDK guesses.
    fs = LittleFS(block_size=8192, block_count=16, read_size=2048,
                  prog_size=2048, cache_size=2048, lookahead_size=8)
    with fs.open('factory.prop', 'w') as stream:
        stream.write(''.join(lines))
    fs.unmount()
    return bytes(fs.context.buffer)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mac', required=True, help='Actual controller address, XX:XX:XX:XX:XX:XX')
    parser.add_argument('--properties', type=Path, help='Optional ro.factory.* JSON properties')
    parser.add_argument('--model', help='Binding model; defaults to imported product.model/ro.factory.model or miwear.watch.q66nfc')
    parser.add_argument('--random-identity', type=Path, metavar='JSON',
                        help='Generate and save a test DID/PSK/PSN once, reusing the JSON on later runs')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.properties and args.random_identity:
        parser.error('--properties and --random-identity are mutually exclusive')
    if not re.fullmatch(r'(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}', args.mac):
        parser.error('--mac must be a colon-separated 6-byte controller address')
    properties_path = args.properties or args.random_identity
    if args.random_identity and not args.random_identity.exists():
        args.random_identity.parent.mkdir(parents=True, exist_ok=True)
        # Exclusive creation keeps an existing device identity stable.
        with args.random_identity.open('x') as stream:
            args.random_identity.chmod(0o600)
            json.dump(random_properties(), stream, indent=2)
            stream.write('\n')
    properties = json.loads(properties_path.read_text()) if properties_path else {}
    if not isinstance(properties, dict):
        parser.error('--properties must contain a JSON object')
    properties['ro.factory.mac_bt'] = args.mac.upper()
    # The original SDK queries this exact key when building BindPsk_BindInfo.
    # The OTA build.prop lacks it. Use the model of this Q66 NFC firmware,
    # preserving a model supplied by an imported factory identity.
    model = args.model or properties.get('product.model') or properties.get(
        'ro.factory.model') or 'miwear.watch.q66nfc'
    if not isinstance(model, str) or not 0 < len(model.encode()) < 32:
        parser.error('Binding model must fit the original 32-byte buffer')
    properties['product.model'] = model
    product = product_definition(model)
    if product:
        properties.setdefault('ro.factory.model', model)
        properties.setdefault('ro.factory.pid', product['miwear_pid'])
        properties.setdefault('ro.factory.board_id', str(product['board_id']))
    if 'ro.factory.pid' in properties:
        properties.setdefault('product.pid', properties['ro.factory.pid'])
    data = make_image(properties)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    # Identity images can later contain genuine PSK credentials.
    descriptor = args.output.open('wb')
    try:
        args.output.chmod(0o600)
        descriptor.write(data)
    finally:
        descriptor.close()
    print(f'Factory image: {args.output}; {len(data)} bytes')
    print('Properties: ' + ', '.join(sorted(properties)))
    if args.random_identity:
        print(f'Test identity saved/reused: {args.random_identity}; official binding is unverified.')
    if not all(k in properties for k in ('ro.factory.did', 'ro.factory.psk')):
        print('DID/PSK absent: native QR identity can be fixed; official App binding is unverified.')


if __name__ == '__main__':
    main()
