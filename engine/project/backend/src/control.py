"""Local display and pointer controls for a running Q66 QEMU instance."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
import os
from pathlib import Path
from socketserver import StreamRequestHandler, ThreadingUnixStreamServer
import struct
import stat
import sys
import threading
import time


# Pillow scales RGB565 channels to 0..255. Clear only those added low bits
# to match smash.lua: R5 << 3, G6 << 2, B5 << 3. The conversion stays in C
# inside Pillow instead of adding a Python loop for every displayed pixel.
RGB565_LEFT_ALIGNED = ([value & 0xf8 for value in range(256)] +
                      [value & 0xfc for value in range(256)] +
                      [value & 0xf8 for value in range(256)])


class NativeWindow:
    def __init__(self, control, scale=1):
        self.control = control
        self.scale = scale
        self.thread = None
        self.closed = threading.Event()
        self.root = None
        self.next_refresh = 0.0
        self.last_frame = None

    def start(self):
        # Tk/AppKit must be created and serviced on the main thread.  The
        # runner calls pump() so the GUI cannot block timeout/fault checks.
        try:
            self._run()
        except ImportError as error:
            raise RuntimeError('Python window requires tkinter and Pillow') from error
        except Exception as error:
            raise RuntimeError(f'Python window failed to start: {error}') from error

    def _run(self):
        import tkinter as tk
        from PIL import Image, ImageTk
        root = tk.Tk()
        self.root = root
        self.tk_error = tk.TclError
        root.title('Q66 Watch')
        width, height = 212 * self.scale, 520 * self.scale
        label = tk.Label(root, bg='black', cursor='arrow')
        label.pack()
        pressed = [False]
        last_point = [106, 260]
        last_move = [0.0]
        def touch(event, down):
            x = max(0, min(211, int(event.x * 212 / width)))
            y = max(0, min(519, int(event.y * 520 / height)))
            last_point[:] = [x, y]
            pressed[0] = down
            try:
                self.control.touch(x, y, down)
            except Exception:
                pass
        label.bind('<ButtonPress-1>', lambda e: touch(e, True))
        label.bind('<ButtonRelease-1>', lambda e: touch(e, False))
        def move(event):
            now = time.monotonic()
            if pressed[0] and now - last_move[0] >= 1 / 30:
                last_move[0] = now
                touch(event, True)
        label.bind('<B1-Motion>', move)
        def release(event=None):
            if pressed[0]:
                pressed[0] = False
                try:
                    self.control.touch(*last_point, False)
                except (OSError, RuntimeError, ConnectionError):
                    pass
        root.bind('<FocusOut>', release)
        self.release = release
        def refresh():
            try:
                image = self.control.frame_image()
                if self.control.frame_sequence is not None and self.last_frame == self.control.frame_sequence:
                    return
                self.last_frame = self.control.frame_sequence
                image = image.resize((width, height), Image.Resampling.NEAREST)
                photo = ImageTk.PhotoImage(image)
                label.configure(image=photo); label.image = photo
            except (OSError, RuntimeError, ConnectionError):
                pass
        root.protocol('WM_DELETE_WINDOW', self.close)
        self.refresh = refresh

    def pump(self):
        if self.closed.is_set():
            return False
        try:
            self.root.update_idletasks()
            self.root.update()
            now = time.monotonic()
            if now >= self.next_refresh and not self.closed.is_set():
                self.next_refresh = now + 1 / 30
                self.refresh()
        except self.tk_error as error:
            if not self.closed.is_set():
                raise RuntimeError(f'Python window failed while updating: {error}') from error
        return not self.closed.is_set()

    def close(self):
        if self.closed.is_set():
            return
        if self.root is not None:
            if hasattr(self, 'release'):
                self.release()
            try:
                self.root.destroy()
            except self.tk_error as error:
                raise RuntimeError(f'Python window failed while closing: {error}') from error
        self.closed.set()


PAGE = '''<!doctype html><html lang="zh-CN"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Q66 Watch</title><style>
body{margin:0;background:#17191c;color:#eee;font:16px system-ui;display:grid;
place-content:center;min-height:100vh;text-align:center;gap:14px}
img{width:212px;height:520px;background:#000;border-radius:28px;touch-action:none;
box-shadow:0 0 0 8px #34373d;user-select:none}
small{color:#aaa}p{margin:0}button{padding:8px;cursor:pointer}
dialog{max-width:90vw;background:#fff;color:#17191c;border:0;border-radius:12px}
dialog img{width:min(320px,75vw);height:auto;border-radius:0;box-shadow:none}
dialog p{max-width:320px;margin:12px auto}
</style><p>Q66 Watch</p><img id="screen" draggable="false" alt="Watch display">
<small id="status">Connecting...</small>
<!--APP_BINDING-->
<dialog id="pairing"><p>用小米运动健康扫描此二维码</p>
<img id="pairing-qr" alt="App pairing QR"><p id="pairing-address"></p>
<p>此码使用当前蓝牙地址。手环出现配对请求后，点击勾选按钮。</p>
<button id="close-pairing">关闭</button></dialog>
<script>
const screen=document.getElementById('screen'),status=document.getElementById('status');
const binding=document.getElementById('app-binding'),pairing=document.getElementById('pairing');
document.getElementById('close-pairing').onclick=()=>pairing.close();
if(binding)binding.onclick=async()=>{binding.disabled=true;try{
 const r=await fetch('/pairing.json',{cache:'no-store'});if(!r.ok)throw Error(await r.text());
 const data=await r.json();document.getElementById('pairing-address').textContent=data.controller_address;
 document.getElementById('pairing-qr').src='/pairing.png?t='+Date.now();pairing.showModal();
}catch(e){status.textContent=e.message}finally{binding.disabled=false}};
let pressed=false, pointer=null, pending=Promise.resolve(), moving=false, lastPoint=null;
function send(e,down){
 lastPoint={clientX:e.clientX,clientY:e.clientY};
 const r=screen.getBoundingClientRect();
 const x=Math.max(0,Math.min(211,Math.floor((e.clientX-r.left)*212/r.width)));
 const y=Math.max(0,Math.min(519,Math.floor((e.clientY-r.top)*520/r.height)));
 pending=pending.then(async()=>{const r=await fetch('/touch',{method:'POST',
 headers:{'Content-Type':'application/json'},body:JSON.stringify({x,y,down})});
 if(!r.ok)throw Error(await r.text());status.textContent=`${down?'Touch':'Release'}: ${x}, ${y}`;
 }).catch(e=>{status.textContent=e.message;});
}
screen.onpointerdown=e=>{if(pressed)return;e.preventDefault();pressed=true;pointer=e.pointerId;screen.setPointerCapture(e.pointerId);send(e,true)};
screen.onpointermove=e=>{if(pressed&&e.pointerId===pointer&&!moving){moving=true;send(e,true);setTimeout(()=>moving=false,40)}};
function release(e){
 if(!pressed)return;
 const point=e&&e.type!=='lostpointercapture'?e:lastPoint;
 const id=pointer;pressed=false;pointer=null;send(point,false);
 if(screen.hasPointerCapture(id))screen.releasePointerCapture(id);
}
screen.onpointerup=screen.onpointercancel=screen.onlostpointercapture=e=>{if(e.pointerId===pointer)release(e)};
window.addEventListener('blur',()=>release());
document.addEventListener('visibilitychange',()=>{if(document.hidden)release()});
async function refresh(){try{const r=await fetch('/frame.png',{cache:'no-store'});
 if(!r.ok)throw Error(await r.text());const blob=await r.blob();
 const old=screen.src;screen.src=URL.createObjectURL(blob);if(old.startsWith('blob:'))URL.revokeObjectURL(old);
 if(!status.textContent.startsWith('Touch:')&&!status.textContent.startsWith('Release:'))status.textContent='Ready';
 }catch(e){status.textContent=e.message}setTimeout(refresh,33)}refresh();
</script></html>'''.encode('utf-8')


class WatchControl:
    def __init__(self, qmp, directory, bluetooth_log=None, log_directory=None, frame_file=None):
        self.qmp = qmp
        self.directory = Path(directory)
        self.frame_file = Path(frame_file) if frame_file else None
        self.frame_lock = threading.RLock()
        self.frame_sequence = None
        self.frame_picture = self.frame_png = None
        self.frame_geometry_identity = None
        self.frame_geometry_lock = threading.Lock()
        self.lock = threading.RLock()
        self.events = []
        self.bluetooth_log = bluetooth_log
        self.log_directory = Path(log_directory) if log_directory else self.directory
        self.pairing_info = self.pairing_png = None
        self.server = self.thread = None
        self.input_server = self.input_thread = None
        self._servers_stop = threading.Event()
        # Board inputs the runner resolved for this session (live.get reports
        # them so the host maps levels/temperatures without guessing wiring).
        self.battery_channel = None
        self.ntc_channels = 0

    def _serve_requests(self, server):
        # Both selector wait and accept are bounded. A stale readiness event
        # must not trap the server thread in accept during guest shutdown.
        server.timeout = .1
        server.socket.settimeout(.1)
        while not self._servers_stop.is_set():
            try:
                server.handle_request()
            except (OSError, ValueError):
                if not self._servers_stop.is_set():
                    raise

    def execute(self, command, arguments=None):
        with self.lock:
            return self.qmp.execute(command, arguments)

    def touch(self, x, y, down):
        if type(x) is not int or type(y) is not int or type(down) is not bool:
            raise ValueError('x/y must be integers and down must be boolean')
        width, height = self.frame_geometry()
        if not 0 <= x < width or not 0 <= y < height:
            raise ValueError(f'Touch coordinates must be inside {width} x {height}')
        # Round up to preserve the requested physical pixel after QEMU scaling.
        events = [{'type': 'abs', 'data': {'axis': axis, 'value': value}}
                  for axis, value in (('x', (x*32767+width-2)//max(1,width-1)),
                                      ('y', (y*32767+height-2)//max(1,height-1)))]
        events.append({'type': 'btn', 'data': {'button': 'left', 'down': down}})
        with self.lock:
            self.qmp.execute('input-send-event', {'events': events})
            self.events.append({'x': x, 'y': y, 'down': down})

    def frame_geometry(self):
        """Read only a stable shared-frame header; touch never needs its pixels."""
        if self.frame_file is None:
            # Preserve the compatibility path for cores without shared LCD output.
            return self.frame_image().size
        with self.frame_geometry_lock:
            return self._shared_frame_geometry()

    def _shared_frame_geometry(self):
        path = self.frame_file
        if (not path.is_absolute() or '..' in path.parts or
                path.parent != self.log_directory or path.name != 'lcd-frame.raw'):
            raise RuntimeError('Unsafe QEMU LCD frame path')
        flags = os.O_RDONLY | os.O_NONBLOCK
        if sys.platform == 'darwin':
            # O_NOFOLLOW_ANY: reject symlinks anywhere without reading parents
            # outside this session's sandbox grant. No permissive fallback.
            fd = os.open(path, flags | 0x20000000)
        else:
            parent = os.open('/', os.O_RDONLY | os.O_DIRECTORY)
            try:
                for part in path.parent.parts[1:]:
                    child = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW,
                                    dir_fd=parent)
                    os.close(parent)
                    parent = child
                fd = os.open(path.name, flags | os.O_NOFOLLOW, dir_fd=parent)
            finally:
                os.close(parent)
        try:
            info = os.fstat(fd)
            identity = (info.st_dev, info.st_ino, info.st_size)
            if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or
                    stat.S_IMODE(info.st_mode) != 0o600 or info.st_nlink != 1 or
                    not 32 <= info.st_size <= 32 + 1024 * 1024 * 2 or
                    (self.frame_geometry_identity is not None and
                     identity != self.frame_geometry_identity)):
                raise RuntimeError('Unsafe or replaced QEMU LCD frame file')
            for _ in range(3):
                header = os.pread(fd, 32, 0)
                if len(header) != 32:
                    raise RuntimeError('Waiting for the first QEMU LCD frame')
                magic, version, sequence, width, height, stride, pixel_format = struct.unpack(
                    '<8s6I', header)
                if magic != b'Q66FRAME' or version != 1 or pixel_format != 1:
                    raise RuntimeError('Unsupported QEMU LCD frame format')
                if sequence == 0:
                    raise RuntimeError('Waiting for the first QEMU LCD frame')
                if sequence & 1:
                    continue
                if not (1 <= width <= 1024 and 1 <= height <= 1024 and
                        width * 2 <= stride <= 2048 and stride % 2 == 0 and
                        32 + stride * height <= info.st_size):
                    raise RuntimeError('Invalid QEMU LCD frame dimensions or capacity')
                if os.pread(fd, 32, 0) != header:
                    continue
                after = os.fstat(fd)
                named = os.stat(path, follow_symlinks=False)
                if any((value.st_dev, value.st_ino, value.st_size) != identity or
                       not stat.S_ISREG(value.st_mode) or value.st_nlink != 1 or
                       value.st_uid != os.getuid() or stat.S_IMODE(value.st_mode) != 0o600
                       for value in (after, named)):
                    raise RuntimeError('QEMU LCD frame file changed while reading')
                self.frame_geometry_identity = identity
                return width, height
            raise RuntimeError('Waiting for a complete QEMU LCD frame header')
        finally:
            os.close(fd)

    def probe_adc_raw(self):
        """Enable the narrow capability only when this core proves its schema."""
        self.adc_raw_available = False
        try:
            state = self._adc_raw_state()
        except (OSError, RuntimeError, ValueError, TypeError):
            return False
        self.adc_raw_available = state['fixtureEnabled']
        return self.adc_raw_available

    def _adc_raw_state(self):
        encoded = self.qmp.execute('qom-get', {
            'path': '/machine', 'property': 'pmu-adc-state'})
        if type(encoded) is not str or len(encoded) > 2048:
            raise RuntimeError('Invalid pmu.adcRaw state response')
        state = json.loads(encoded)
        if type(state) is not dict or set(state) != {
                'schema', 'fixtureEnabled', 'version', 'values',
                'readCount', 'lastReadVersion'}:
            raise RuntimeError('Invalid pmu.adcRaw state schema')
        if type(state['schema']) is not int or state['schema'] != 1 or \
                type(state['fixtureEnabled']) is not bool:
            raise RuntimeError('Unsupported pmu.adcRaw state schema')
        if type(state['version']) is not int or not 0 <= state['version'] <= 2**64-1:
            raise RuntimeError('Invalid pmu.adcRaw version')
        for name, maximum in (('values', 65535), ('readCount', 2**64-1),
                              ('lastReadVersion', state['version'])):
            values = state[name]
            if type(values) is not list or len(values) != 8 or any(
                    type(value) is not int or not 0 <= value <= maximum for value in values):
                raise RuntimeError('Invalid pmu.adcRaw ' + name)
        return state

    def adc_raw(self, values=None):
        """Apply/read raw words only; never accept arbitrary QMP arguments."""
        if values is not None and (type(values) is not list or len(values) != 8 or any(
                type(value) is not int or not 0 <= value <= 65535 for value in values)):
            raise ValueError('pmu.adcRaw requires eight integers in 0..65535')
        with self.lock:
            if not getattr(self, 'adc_raw_available', False):
                raise RuntimeError('unsupported_hardware_input: pmu.adcRaw')
            if values is not None:
                self.qmp.execute('qom-set', {
                    'path': '/machine', 'property': 'pmu-adc-raw',
                    'value': ','.join(map(str, values))})
            try:
                state = self._adc_raw_state()
            except (OSError, RuntimeError, ValueError, TypeError):
                if values is not None:
                    # The write was acknowledged, but the snapshot failed.
                    # Do not tell the caller that nothing happened/retry blindly.
                    return {'accepted': True, 'guestObserved': False,
                            'stateUnavailable': True}
                raise
            return {'accepted': values is not None, 'guestObserved': False,
                    'state': state}

    def dispatch_input(self, data):
        if type(data) is not dict:
            raise ValueError('Expected an input object')
        if set(data) == {'x', 'y', 'down'}:
            self.touch(data['x'], data['y'], data['down'])
            return {}
        if set(data) == {'command'} and data['command'] == 'pmu.adcRaw.get':
            return self.adc_raw()
        if set(data) == {'command', 'values'} and data['command'] == 'pmu.adcRaw.set':
            # None must not turn a malformed write into a read.
            if type(data['values']) is not list:
                raise ValueError('pmu.adcRaw values must be an array')
            return self.adc_raw(data['values'])
        if set(data) == {'command'} and data['command'] == 'health.get':
            return self.health()
        if set(data) == {'command', 'value'} and data['command'] == 'health.set':
            return self.health(data['value'])
        if set(data) == {'command', 'steps'} and data['command'] == 'health.walk':
            return self.walk(data['steps'])
        if set(data) == {'command'} and data['command'] == 'live.get':
            return self.live()
        if set(data) == {'command', 'value'} and data['command'] == 'charger.set':
            return self.charger(data['value'])
        if set(data) == {'command', 'channels'} and data['command'] == 'pmu.adcChannels.set':
            return self.adc_channels(data['channels'])
        raise ValueError('Unsupported input command or fields')

    def walk(self, steps):
        """Walk steps more from now at the current cadence (live input)."""
        if type(steps) is not int or not 1 <= steps <= 100000:
            raise ValueError('health.walk steps must be an integer in 1..100000')
        with self.lock:
            state = self.health()
            target = max(state['walkSteps'], state['walked']) + steps
            if target > 10000000:
                raise ValueError('health.walk would exceed 10000000 steps since boot')
            return self.health({'walkSteps': target})

    def charger(self, value):
        """Plug/unplug the modeled charger now; the firmware's own charger
        driver sees the PMU plug edge and charger chip status."""
        if value not in ('unplugged', 'charging', 'full'):
            raise ValueError('charger.set value must be unplugged, charging or full')
        with self.lock:
            self.qmp.execute('qom-set', {'path': '/machine', 'property': 'battery-charger', 'value': value})
            return self.live()

    def adc_channels(self, channels):
        """Replace selected raw ADC words, keeping the others (live input)."""
        if (type(channels) is not dict or not 1 <= len(channels) <= 8 or any(
                type(key) is not str or key not in '01234567' or len(key) != 1 or
                type(value) is not int or not 0 <= value <= 65535
                for key, value in channels.items())):
            raise ValueError('pmu.adcChannels.set requires {"0".."7": raw 0..65535}')
        with self.lock:
            if not getattr(self, 'adc_raw_available', False):
                raise RuntimeError('unsupported_hardware_input: pmu.adcRaw')
            values = list(self._adc_raw_state()['values'])
            for key, value in channels.items():
                values[int(key)] = value
            self.adc_raw(values)
            return self.live()

    def live(self):
        """One snapshot of every live input this session drives."""
        with self.lock:
            try:
                health = self.health()
            except RuntimeError:
                health = None
            charger = self.qmp.execute('qom-get', {'path': '/machine', 'property': 'battery-charger'})
            adc = self._adc_raw_state()['values'] if getattr(self, 'adc_raw_available', False) else None
            return {'health': health, 'charger': charger if charger in ('unplugged', 'charging', 'full') else None,
                    'adc': adc, 'batteryChannel': self.battery_channel, 'ntcChannels': self.ntc_channels}

    def health(self, value=None):
        """Live health input of the session's modeled sensors.

        value: {"walkSteps": int, "cadence": int, "heartRate": int}, any subset.
        walkSteps is the number of steps walked since boot (raising it walks the
        difference; the firmware counts them itself). Only fixed fields reach
        the core, never arbitrary QMP arguments."""
        limits = {'walkSteps': ('walk', 0, 10000000), 'cadence': ('cadence', 40, 200),
                  'heartRate': ('heart-rate', 0, 255)}
        with self.lock:
            state = self.qmp.execute('qom-get', {'path': '/machine', 'property': 'health-fixture'})
            if not isinstance(state, str) or state == 'off':
                raise RuntimeError('unsupported_hardware_input: health')
            if value is not None:
                if type(value) is not dict or not value or set(value) - set(limits):
                    raise ValueError('health.set requires walkSteps, cadence or heartRate')
                fields = []
                for key, item in value.items():
                    name, low, high = limits[key]
                    if type(item) is not int or not low <= item <= high:
                        raise ValueError('health.%s must be an integer in %d..%d' % (key, low, high))
                    fields.append('%s=%d' % (name, item))
                self.qmp.execute('qom-set', {'path': '/machine', 'property': 'health-fixture',
                                             'value': ';'.join(fields)})
                state = self.qmp.execute('qom-get', {'path': '/machine', 'property': 'health-fixture'})
            parsed = dict(item.split('=', 1) for item in state.split(';') if '=' in item)
            return {'walkSteps': int(parsed.get('walk', 0)), 'cadence': int(parsed.get('cadence', 0)),
                    'heartRate': int(parsed.get('heart-rate', 0)), 'walked': int(parsed.get('walked', 0))}

    def _shared_frame(self):
        """Read the LCD snapshot published by QEMU, independent of QMP/Web."""
        from PIL import Image
        for _ in range(3):
            with self.frame_file.open('rb', buffering=0) as stream:
                header = stream.read(32)
                if len(header) != 32:
                    raise RuntimeError('Waiting for the first QEMU LCD frame')
                magic, version, sequence, width, height, stride, pixel_format = struct.unpack(
                    '<8s6I', header)
                if magic != b'Q66FRAME' or version != 1 or pixel_format != 1:
                    raise RuntimeError('Unsupported QEMU LCD frame format')
                if sequence == 0:
                    raise RuntimeError('Waiting for the first QEMU LCD frame')
                if sequence & 1:
                    continue
                if not (1 <= width <= 1024 and 1 <= height <= 1024 and
                        width * 2 <= stride <= 1024 * 2):
                    raise RuntimeError('Invalid QEMU LCD frame dimensions')
                if sequence == self.frame_sequence:
                    return self.frame_picture
                pixels = stream.read(stride * height)
                stream.seek(0)
                after = stream.read(32)
            # Odd sequences mark an in-progress write.  Do not expose torn
            # frames when the next transfer starts during this file read.
            if after != header or len(pixels) != stride * height:
                continue
            picture = Image.frombytes('RGB', (width, height), pixels,
                                      'raw', 'BGR;16', stride, 1)
            # BGR;16 is Pillow's name for the little-endian RGB565 layout.
            # Its RGB output already has R/G/B in the correct order.
            picture = picture.point(RGB565_LEFT_ALIGNED)
            self.frame_sequence = sequence
            self.frame_picture = picture
            self.frame_png = None
            return picture
        if self.frame_picture is not None:
            return self.frame_picture
        raise RuntimeError('Waiting for a complete QEMU LCD frame')

    def frame_image(self):
        with self.frame_lock:
            if self.frame_file is not None:
                return self._shared_frame().copy()
            return self._qmp_frame()

    def _qmp_frame(self):
        """Compatibility path for QEMU builds without the shared LCD output."""
        from PIL import Image
        path = self.directory / 'live-frame.ppm'
        with self.lock:
            path.unlink(missing_ok=True)
            self.qmp.execute('screendump', {'filename': str(path)})
            deadline = time.monotonic() + 2.0
            last_error = None
            while time.monotonic() < deadline:
                try:
                    with Image.open(path) as picture:
                        picture.load()
                        return picture.copy()
                except (FileNotFoundError, OSError) as error:
                    last_error = error
                    time.sleep(0.02)
            raise RuntimeError(f'QEMU display frame unavailable: {last_error}')

    def frame(self):
        with self.frame_lock:
            picture = self.frame_image()
            if self.frame_png is None or self.frame_file is None:
                output = io.BytesIO()
                picture.save(output, format='PNG', compress_level=1)
                self.frame_png = output.getvalue()
            return self.frame_png

    def save_frame(self, path):
        self.frame_image().save(path)

    def binding(self):
        from .binding import binding_url, controller_address, decode_qr, qr_png
        with self.lock:
            if not self.bluetooth_log:
                raise RuntimeError('请使用 hci 或 bluez 蓝牙后端。')
            if self.pairing_info is None:
                address = controller_address(self.bluetooth_log)
                original = decode_qr(self.frame())
                url = binding_url(original,address)
                png = qr_png(url)
                info = dict(controller_address=address, original_url=original,
                            binding_url=url, firmware_modified=False)
                (self.log_directory/'app-binding-qr.png').write_bytes(png)
                (self.log_directory/'app-binding.json').write_text(
                    json.dumps(info,ensure_ascii=False,indent=2)+'\n')
                self.pairing_info,self.pairing_png = info,png
            return self.pairing_info,self.pairing_png

    def serve(self, port):
        from PIL import Image  # Validate optional display dependency up front.
        control = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def reply(self, code, payload, content_type):
                self.send_response(code)
                self.send_header('Content-Type', content_type)
                self.send_header('Content-Length', str(len(payload)))
                self.send_header('Cache-Control', 'no-store')
                self.end_headers()
                self.wfile.write(payload)

            def do_GET(self):
                if self.path == '/':
                    button = '<button id="app-binding">App 扫码绑定</button>'.encode() if control.bluetooth_log else b''
                    self.reply(200, PAGE.replace(b'<!--APP_BINDING-->',button), 'text/html; charset=utf-8')
                elif self.path == '/frame.png':
                    try:
                        self.reply(200, control.frame(), 'image/png')
                    except (OSError, RuntimeError, ConnectionError) as error:
                        self.reply(503, str(error).encode(), 'text/plain')
                elif self.path in ('/pairing.json','/pairing.png'):
                    try:
                        info,png = control.binding()
                        payload,kind = (png,'image/png') if self.path.endswith('.png') else (
                            json.dumps(info).encode(),'application/json')
                        self.reply(200,payload,kind)
                    except (OSError, RuntimeError, ValueError, ConnectionError) as error:
                        self.reply(503,str(error).encode(),'text/plain; charset=utf-8')
                else:
                    self.reply(404, b'Not found', 'text/plain')

            def do_POST(self):
                if self.path != '/touch':
                    self.reply(404, b'Not found', 'text/plain')
                    return
                try:
                    if self.headers.get('Origin', control.url) != control.url:
                        raise ValueError('Origin mismatch')
                    length = int(self.headers.get('Content-Length', '0'))
                    if not 0 < length <= 256:
                        raise ValueError('Invalid input length')
                    data = json.loads(self.rfile.read(length))
                    if not isinstance(data, dict):
                        raise ValueError('Expected an input object')
                    control.touch(data['x'], data['y'], data['down'])
                    self.reply(200, b'{}', 'application/json')
                except (ValueError, KeyError, TypeError) as error:
                    self.reply(400, str(error).encode(), 'text/plain')
                except (OSError, RuntimeError, ConnectionError) as error:
                    self.reply(503, str(error).encode(), 'text/plain')

        self.server = ThreadingHTTPServer(('127.0.0.1', port), Handler)
        self.server.daemon_threads = True
        self.url = f'http://127.0.0.1:{self.server.server_port}'
        self.thread = threading.Thread(target=self._serve_requests, args=(self.server,), daemon=True)
        self.thread.start()
        return self.url

    def serve_input(self, path):
        """Local input relay for display.py; frames stay in shared memory."""
        control = self
        pending = threading.BoundedSemaphore(8)
        def unique_fields(pairs):
            result = {}
            for key, value in pairs:
                if key in result:
                    raise ValueError('Duplicate input field')
                result[key] = value
            return result
        class Handler(StreamRequestHandler):
            def handle(self):
                self.connection.settimeout(2)
                if not pending.acquire(blocking=False):
                    self.wfile.write(b'{"error":"Input relay busy"}\n')
                    return
                try:
                    try:
                        line = self.rfile.readline(257)
                        if len(line) > 256 or not line.endswith(b'\n'):
                            raise ValueError('Input request too large or unterminated')
                        data = json.loads(line, object_pairs_hook=unique_fields)
                        result = {'return': control.dispatch_input(data)}
                    except (ValueError, KeyError, TypeError, OSError, RuntimeError) as error:
                        result = {'error': str(error)[:512]}
                    try:
                        self.wfile.write(json.dumps(result).encode() + b'\n')
                    except (BrokenPipeError, ConnectionResetError):
                        pass  # An abandoned input client is normal during stop.
                finally:
                    pending.release()
        self.input_server = ThreadingUnixStreamServer(str(path), Handler)
        self.input_server.daemon_threads = True
        Path(path).chmod(0o600)
        self.input_thread = threading.Thread(target=self._serve_requests, args=(self.input_server,), daemon=True)
        self.input_thread.start()

    def close(self):
        self._servers_stop.set()
        for server, thread in ((self.server, self.thread), (self.input_server, self.input_thread)):
            if server is None:
                continue
            if thread is not None and thread is not threading.current_thread():
                thread.join(timeout=.5)
            server.server_close()
            if thread is not None and thread.is_alive():
                raise RuntimeError('Input server did not stop within its deadline')
