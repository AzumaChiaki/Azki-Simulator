"""Static, per-session adapter for the separately supplied Mi Band runner.

Production starts bind the small session descriptor to the local request and
require a trusted local isolation launcher. File hashes are descriptive metadata. This module never
imports supplied Python, connects QMP, builds images, or changes the handoff.
"""
from __future__ import annotations

import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import re
import signal
import socket
import stat
import struct
import subprocess
import sys
import threading
import time
import traceback
import uuid

SCHEMA = "azki-minative-plan-v1"
MAX_JSON = 4 * 1024 * 1024
MAX_ASSET = 768 * 1024 * 1024
MAX_FRAME_FILE = 8 * 1024 * 1024
FRAME_HEADER = struct.Struct("<8s6I")
GEOMETRIES = {(192, 490), (212, 520), (336, 480), (432, 514)}
RUNTIME_FILES = (
    "multi_device.py", "device_catalog.py", "firmware_resources.py",
    "legacy_ui_seed.py", "backend/src/legacy_ui_db.c",
    "backend/src/yaffs_image.c", "backend/src/runner.py",
    "backend/src/factory.py", "backend/src/control.py", "backend/src/qmp.py",
    "runtime_fingerprint.py",
)
# Frozen scope, not a family-prefix wildcard. New versions require an explicit
# source change/review; no alias matching or automatic catalog discovery.
INCLUDED_PROFILE_IDS = frozenset((
    "miwear.watch.n66cn@3.1.17@38d1681ec71b", "miwear.watch.n66cn@3.1.32@489f20502ae2",
    "miwear.watch.n66nfc@3.1.17@38d1681ec71b", "miwear.watch.n66nfc@3.1.32@489f20502ae2",
    "miwear.watch.n66tc@3.1.17@38d1681ec71b", "miwear.watch.n66tc@3.1.32@489f20502ae2",
    "miwear.watch.n67cn@3.1.171@01591845f76e", "miwear.watch.n67cn@3.1.175@a8274e4cb29c",
    "miwear.watch.n67cn@3.1.187@6d33afe20089", "miwear.watch.o66cn@3.1.16@435fd32b5a23",
    "miwear.watch.o66cn@3.1.23@1a1bc068db45", "miwear.watch.o66cn@3.1.26@d4e61778eee0",
    "miwear.watch.o66cn@3.1.28@03ac3e2b8325", "miwear.watch.o66lj@3.1.16@435fd32b5a23",
    "miwear.watch.o66lj@3.1.23@1a1bc068db45", "miwear.watch.o66lj@3.1.26@d4e61778eee0",
    "miwear.watch.o66nfc@3.1.16@435fd32b5a23", "miwear.watch.o66nfc@3.1.23@1a1bc068db45",
    "miwear.watch.o66nfc@3.1.26@d4e61778eee0", "miwear.watch.o66nfc@3.1.28@03ac3e2b8325",
    "miwear.watch.o66tc@3.1.11@90daf23812bf", "miwear.watch.o66tc@3.1.16@435fd32b5a23",
    "miwear.watch.o66tc@3.1.23@1a1bc068db45", "miwear.watch.o66tc@3.1.26@d4e61778eee0",
    "miwear.watch.o66tc@3.1.28@03ac3e2b8325", "miwear.watch.p67cn@3.101.021@5b17ada0842d",
    "miwear.watch.p67cn@3.101.028@6534235d895f", "miwear.watch.p67cn@3.101.036@17a1ea48d581",
    "miwear.watch.p67cn@3.101.042@616d0faba20c", "miwear.watch.p67cn@3.101.043@a4ce85647786",
    "miwear.watch.p67tc@3.101.021@5b17ada0842d", "miwear.watch.p67tc@3.101.028@a1d6e3e08a1c",
    "miwear.watch.p67tc@3.101.036@17a1ea48d581", "miwear.watch.p67tc@3.101.043@a4ce85647786",
    "miwear.watch.q66glt@4.200.078@754da125b286", "miwear.watch.q66tc@4.100.155@f1c824fef81d",
    "miwear.watch.q66u@4.100.108@6ef691453a86", "q66@4.100.071", "q66@4.100.177",
))
# A user-imported full OTA runs on the instance's hardware preset. Its profile
# ID binds that preset model, the package's own version and its OTA SHA-256
# prefix; boot compatibility is checked per launch, not by a version allowlist.
GENERATED_PROFILE_ID = re.compile(
    r"(?P<model>miwear\.watch\.[a-z0-9]{3,16}|q66)@(?P<version>[0-9][0-9.]{0,31})@(?P<sha>[0-9a-f]{12})\Z")


def generated_profile(profile_id):
    """Return (model, version, sha prefix) for a generated imported-firmware ID."""
    match = GENERATED_PROFILE_ID.fullmatch(profile_id) if type(profile_id) is str else None
    if match is None or profile_id in INCLUDED_PROFILE_IDS:
        return None
    try:
        _geometry(match["model"])
    except MError:
        return None
    return match["model"], match["version"], match["sha"]


def profile_in_scope(profile_id):
    return profile_id in INCLUDED_PROFILE_IDS or generated_profile(profile_id) is not None


MAX_RUN_SECONDS = 28800
# Long interactive sessions append to qemu.log; generated assets live outside the run directory.
MAX_OUTPUT_FILE = 512 * 1024 * 1024
MAX_OUTPUT_TOTAL = 1024 * 1024 * 1024


def _validate_control_schema(value, plan):
    # Import at use time: adc_commands also uses the fixed transport helpers in
    # this module. A runner cannot grant itself capabilities through this file.
    from .adc_commands import supports_adc
    from .virtual_peer import validate_control
    keys = {"qmp", "framebuffer", "pid", "input_socket"}
    adc = supports_adc(plan)
    if adc:
        keys.add("hardware_inputs")
    # The virtual peer endpoint is optional: it exists only when the runner
    # actually started one, and is validated whenever it is present.
    peer = type(value) is dict and "virtual_peer" in value
    if peer:
        keys.add("virtual_peer")
    # A user-enabled debugger is reported, never granted, by the runner: the
    # native parent alone decides whether QEMU serves GDB on loopback.
    if type(value) is dict and "gdb" in value:
        keys.add("gdb")
        gdb = value["gdb"]
        if (type(gdb) is not dict or set(gdb) != {"host", "port", "waitForAttach"} or gdb["host"] != "127.0.0.1"
                or type(gdb["port"]) is not int or not 0 < gdb["port"] <= 65535 or type(gdb["waitForAttach"]) is not bool):
            raise MError("unsafe_control", "Runner debugger endpoint is not a loopback port")
    if type(value) is not dict or set(value) != keys or type(value.get("pid")) is not int:
        raise MError("unsafe_control", "Unsupported runner control record")
    if peer:
        validate_control(value["virtual_peer"])
    if adc:
        hardware = value["hardware_inputs"]
        expected = {"supported": True, "schema": 1, "channels": 8, "minimum": 0,
                    "maximum": 65535, "unit": "uncalibrated-raw",
                    "readEvidence": "guest-ispi-fifo-dequeue"}
        entry = hardware.get("pmu.adcRaw") if type(hardware) is dict else None
        if (type(hardware) is not dict or set(hardware) != {"pmu.adcRaw"}
                or type(entry) is not dict or set(entry) != set(expected)
                or any(type(entry[k]) is not type(v) or entry[k] != v for k, v in expected.items())):
            raise MError("unsafe_control", "Runner ADC declaration differs from the pinned capability")


class MError(ValueError):
    def __init__(self, code, message):
        self.code = code
        super().__init__(message)


def _canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=True, allow_nan=False).encode("ascii")


def _digest(value):
    return hashlib.sha256(_canonical(value)).hexdigest()


def _pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise MError("invalid_json", "Duplicate JSON key")
        result[key] = value
    return result


def _decode(data):
    try:
        return json.loads(data.decode("utf-8", errors="strict"), object_pairs_hook=_pairs,
                          parse_constant=lambda _: (_ for _ in ()).throw(ValueError("Nonfinite JSON")))
    except (UnicodeError, ValueError, RecursionError) as error:
        if isinstance(error, MError):
            raise
        raise MError("invalid_json", "Expected bounded strict UTF-8 JSON") from error


def _path(value):
    path = Path(value)
    if not path.is_absolute() or any(part in (".", "..") for part in path.parts):
        raise MError("unsafe_path", "An absolute normalized path is required")
    return path


def _directory_fd(path, *, private=False):
    """Atomically reject every linked component on Darwin without reading parents."""
    path = _path(path)
    if sys.platform == "darwin":
        # Darwin SDK sys/fcntl.h: O_NOFOLLOW_ANY rejects symlinks anywhere in
        # one path lookup. Do not fall back after a denial/unsupported kernel:
        # doing so would require reading parent directories outside the grant.
        fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | 0x20000000)
    else:
        fd = os.open("/", os.O_RDONLY | os.O_DIRECTORY)
        try:
            for part in path.parts[1:]:
                new = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd)
                os.close(fd)
                fd = new
        except BaseException:
            os.close(fd)
            raise
    try:
        info = os.fstat(fd)
        if not stat.S_ISDIR(info.st_mode) or (private and
                (info.st_uid != os.getuid() or stat.S_IMODE(info.st_mode) != 0o700)):
            raise MError("unsafe_session", "A same-user mode-0700 directory is required")
        return fd
    except BaseException:
        os.close(fd)
        raise


def _open_file(path, limit=MAX_ASSET, *, private=False):
    path = _path(path)
    if sys.platform == "darwin" and not private:
        # A user-selected input has a literal-file read grant, not a directory
        # listing grant. Atomic full-path nofollow needs only parent traversal.
        fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | 0x20000000)
    else:
        directory = _directory_fd(path.parent, private=private)
        try:
            fd = os.open(path.name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=directory)
        finally:
            os.close(directory)
    info = os.fstat(fd)
    if (not stat.S_ISREG(info.st_mode) or info.st_size > limit or info.st_nlink != 1
            or info.st_uid not in (0, os.getuid()) or info.st_mode & 0o022
            or (private and (info.st_uid != os.getuid() or stat.S_IMODE(info.st_mode) != 0o600))):
        os.close(fd)
        raise MError("unsafe_file", "Expected bounded, owned, non-linked regular file")
    return fd, info


def _read(path, limit=MAX_JSON, *, private=False):
    fd, before = _open_file(path, limit, private=private)
    try:
        data = bytearray()
        while True:
            chunk = os.read(fd, min(1024 * 1024, limit + 1 - len(data)))
            if not chunk:
                break
            data.extend(chunk)
            if len(data) > limit:
                raise MError("file_changed", "File exceeds its bounded read limit")
        after = os.fstat(fd)
        if (before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (
                after.st_size, after.st_mtime_ns, after.st_ctime_ns):
            raise MError("file_changed", "File changed while being read")
        return bytes(data)
    finally:
        os.close(fd)


def _json(path, *, private=False):
    value = _decode(_read(path, private=private))
    if not isinstance(value, dict):
        raise MError("invalid_json", "Expected a JSON object")
    return value


def _pin(path, limit=MAX_ASSET):
    path = _path(path)
    fd, before = _open_file(path, limit)
    try:
        digest = hashlib.sha256()
        total = 0
        while chunk := os.read(fd, 4 * 1024 * 1024):
            total += len(chunk)
            if total > limit:
                raise MError("file_changed", "Identity input grew beyond limit")
            digest.update(chunk)
        after = os.fstat(fd)
        if (before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (
                after.st_size, after.st_mtime_ns, after.st_ctime_ns) or total != before.st_size:
            raise MError("file_changed", "Identity input changed during hashing")
        return {"path": str(path), "bytes": total, "sha256": digest.hexdigest()}
    finally:
        os.close(fd)


def _require_sha(actual, expected, description):
    if not isinstance(expected, str) or not re.fullmatch("[0-9a-f]{64}", expected) or actual != expected:
        raise MError("identity_mismatch", description + " SHA-256 mismatch")


def _geometry(model):
    if model.startswith("miwear.watch.n66"):
        return 192, 490
    if model.startswith(("miwear.watch.n67", "miwear.watch.p67")):
        return 336, 480
    if model == "q66" or model.startswith(("miwear.watch.o66", "miwear.watch.q66")):
        return 212, 520
    if model in ("miwear.watch.p65", "miwear.watch.q65"):
        return 432, 514
    raise MError("unsupported_profile", "Model is outside the Mi Band scope")


# Board devices an instance's external settings may drive. The instance
# hardware preset (its model) selects the part; firmware version never does,
# so an imported OTA keeps the same devices as the bundled profile it runs on.
# Band 9 / 9 Pro / 10 carry an HP4560 charger; Band 10 Pro / 11 an HP4570 with
# a CW2218 coulomb counter. The firmware's battery voltage path reads PMU ADC
# channel 2 (ISPI register 0x58) on Band 9 / 9 Pro / 11; the Band 10 / 10 Pro
# BESPOWER manager reads it on channel 4 (x4.9; N67 .187 0x0c0e2f00, P67 .043
# 0x0c3dbf48), where channels 2 and 5 are its shell/skin thermistors.
BATTERY_ADC_CHANNEL = 2
BATTERY_ADC_CHANNELS = {"miwear.watch.n67": 4, "miwear.watch.p67": 4,
                        "miwear.watch.p65": 6}
BATTERY_GAUGES = {"hp4560": ("miwear.watch.n66", "miwear.watch.n67", "miwear.watch.o66"),
                  "hp4570": ("miwear.watch.p67", "miwear.watch.q66"),
                  "soft": ("miwear.watch.p65",)}


def board_hardware(model):
    """Devices of one hardware preset, or None when the model is out of scope."""
    if type(model) is not str or not model:
        return None
    for gauge, prefixes in BATTERY_GAUGES.items():
        if model == "q66" or model.startswith(prefixes):
            channel = next((c for p, c in BATTERY_ADC_CHANNELS.items() if model.startswith(p)),
                           BATTERY_ADC_CHANNEL)
            sensors = health_sensors(model) or {}
            return {"batteryADCChannel": channel, "batteryGauge": gauge,
                    "coulombCounter": "cw2218" if gauge == "hp4570" else None,
                    "motionSensor": sensors.get("motion"), "heartRateSensor": sensors.get("optical"),
                    "sensorBoard": sensors.get("board"),
                    "healthInputs": {"steps": bool(sensors.get("steps")),
                                     "heartRate": bool(sensors.get("heartRate"))}}
    return None


# Motion and optical sensors each preset's firmware probes, with the wiring
# read from its drivers (chip-select GPIO, interrupt GPIO, SPI controller and,
# for the IMU, how the part is mounted relative to the band). The core models
# the chip; the firmware's own drivers and algorithms turn the raw samples
# into steps and heart rate. Only inputs verified on a preset are offered:
# steps = the firmware counted walked steps from the IMU, heartRate = the
# firmware's heart-rate algorithm reported the simulated pulse.
#   n66 Band 9 3.1.32 / o66 Band 9 Pro 3.1.28: LSM6DSO SPI0 CS53 INT71,
#     HX3695 SPI0 CS50 INT14 (board_imu_initialize / board_afe_initialize).
#   n67 Band 10 3.1.187: ICM-42607-P SPI0 CS13 INT10 (driver config byte 10,
#     axis map 7 = x<->y, -z), HX3695 on SPI1 CS61 INT0 (board_hx3695_initialize
#     0x0c39256e: CS 61, IRQ 0, power 1); its driver programs the sample period
#     in 1/32000 s ticks (1280 for 25 Hz).
#   p67 Band 10 Pro 3.101.043: board_sensor_initialize probes BMI220 before
#     ICM-42607-P (SPI0, CS GPIO13 from the SPI0 pinmux, IRQ 29 at 0x0c40cf9c)
#     and HX3695 before GH3020 (same wiring as n67); its driver reads the
#     ambient and LED phase words of each channel (0x1b = 0x90). The IMU
#     mount is taken from n67: at rest the firmware sees gravity on +Z.
#   q66 Band 11 4.200.078: LSM6DSO probed before BMI2x0, SPI0 CS53 INT71 (IRQ
#     0x47 in the driver config at 0x2c9941c0), as on Band 9. Its optical
#     front end is a TYHX HX3697 on SPI0 CS50 INT14 (driver config
#     0x2c99416c; paged registers, ID 0x69 0x19).
HEALTH_SENSORS = {
    "miwear.watch.n66": {"board": "imu:lsm6dso:spi0:53:71;ppg:hx3695:spi0:50:14",
                         "motion": "lsm6dso", "optical": "hx3695", "steps": True, "heartRate": True},
    # O66: steps verified on the watch face, heart rate in the Heart Rate app
    # (72 -> 74, 100 -> 103).
    "miwear.watch.o66": {"board": "imu:lsm6dso:spi0:53:71;ppg:hx3695:spi0:50:14",
                         "motion": "lsm6dso", "optical": "hx3695", "steps": True, "heartRate": True},
    # N67: steps verified on the Steps card (walk 150 -> 139) and heart rate
    # in the Heart Rate app (72 -> 71, 100 -> 103).
    "miwear.watch.n67": {"board": "imu:icm42607:spi0:13:10:+y+x-z;ppg:hx3695:spi1:61:0:32000",
                         "motion": "icm42607", "optical": "hx3695", "steps": True, "heartRate": True},
    # P67: steps verified on the activity card (walk 150 -> 142), heart rate
    # in the Heart Rate app (72 -> 72, 100 -> 102).
    "miwear.watch.p67": {"board": "imu:icm42607:spi0:13:29:+y+x-z;ppg:hx3695:spi1:61:0:32000",
                         "motion": "icm42607", "optical": "hx3695", "steps": True, "heartRate": True},
    # Q66: steps verified on the activity page (walk 100 -> 92), heart rate in
    # the Heart Rate app (72 -> 74, 100 -> 102).
    "miwear.watch.q66": {"board": "imu:lsm6dso:spi0:53:71;ppg:hx3697:spi0:50:14",
                         "motion": "lsm6dso", "optical": "hx3697", "steps": True, "heartRate": True},
}


def health_sensors(model):
    """Modeled health sensors of one preset, or None."""
    if type(model) is not str:
        return None
    for prefix, sensors in HEALTH_SENSORS.items():
        if model.startswith(prefix):
            return sensors
    return None


# Battery inputs verified to drive a preset's own firmware. The fuel gauge is
# firmware code: at boot it restores its state of charge from a persist key and
# corrects it against the battery voltage it samples on PMU ADC channel 2, so
# a boot level is the persist value plus the raw sample of the matching open
# circuit voltage. Band 9 (3.1.32): persist.gauge.cap is the capacity x1000
# (get_cap_data_init "get cap from kvdb"), kept while it is within 15 % of the
# capacity the OCV table gives for the voltage; the level shown is capacity
# x 1.055 (cap_ui). The OCV table (gauge state +0xa4, index 0 = 100 %; the
# gauge derives it for the battery temperature, here the core's
# room-temperature thermistor input), adc_mV = 0.051538 * raw - 23.3 and
# vbat = adc_mV * 7 were read from the running firmware. The charger state is
# applied only where the firmware finds its charger chip.
BAND9_OCV_MV = (
    4463.5, 4453.5, 4443.5, 4434.5, 4424.5, 4416.5, 4407.5, 4397.5, 4388.4, 4378.4,
    4369.4, 4359.4, 4348.4, 4338.3, 4327.3, 4316.2, 4305.2, 4293.3, 4282.2, 4258.9,
    4247.8, 4234.9, 4223.9, 4210.3, 4199.2, 4188.2, 4175.2, 4164.2, 4152.2, 4141.2,
    4129.2, 4118.2, 4107.2, 4095.3, 4084.4, 4074.3, 4063.4, 4053.4, 4042.4, 4032.4,
    4022.4, 4011.4, 4000.4, 3988.4, 3975.4, 3963.4, 3951.4, 3940.6, 3931.6, 3923.6,
    3915.6, 3907.7, 3900.7, 3894.7, 3888.7, 3881.8, 3871.0, 3866.0, 3860.1, 3855.1,
    3850.1, 3845.0, 3841.0, 3836.0, 3832.0, 3828.0, 3823.0, 3819.9, 3815.7, 3811.6,
    3807.6, 3803.4, 3798.3, 3792.4, 3787.3, 3782.3, 3776.3, 3771.4, 3767.2, 3761.4,
    3758.3, 3754.2, 3749.2, 3744.2, 3738.1, 3732.1, 3726.2, 3721.1, 3713.1, 3706.2,
    3700.4, 3699.4, 3697.5, 3696.4, 3691.4, 3685.2, 3663.9, 3629.9, 3588.8, 3538.8,
    3476.4)
# Band 9 Pro (O66 .28) runs the same gauge: persist.gauge.cap, ADC 2 x7
# (gauge state 0x20097058: divider 7.0 at +0x58, same OCV table at +0xa4);
# a requested 37 % boot level shows 37 % on both. Runtime charger state shows on both
# (PMU AC_ON edge -> hp4560(x) detect worker -> charge screen / face bolt).
# Band 10 (N67 3.1.187) runs a BESPOWER soft gauge: at boot it restores the
# level from persist.charger.per (whole percent); while discharging it steps the
# level down only when it exceeds the level its OCV table gives for the battery
# voltage (0x0c0e2fec; table for -10..49 C at 0x2c5e3208, uV, index 0 = 100 %).
# The voltage is whole ADC millivolts x 4.9 from PMU ADC 4 (0x0c0e2f00), with
# the Band 9 conversion (raw 21000 reads 5189.1 mV in the gauge log).
BAND10_OCV_MV = (
    4415.0, 4405.8, 4397.1, 4388.5, 4379.3, 4370.7, 4360.9, 4351.6, 4342.4, 4331.9,
    4321.5, 4311.0, 4300.6, 4289.5, 4279.0, 4267.3, 4255.7, 4244.0, 4232.3, 4220.0,
    4208.3, 4196.6, 4184.9, 4173.2, 4160.9, 4149.2, 4137.5, 4126.5, 4114.8, 4103.7,
    4092.6, 4081.0, 4069.9, 4058.8, 4048.4, 4037.3, 4026.8, 4016.4, 4005.9, 3994.8,
    3983.8, 3972.1, 3961.0, 3949.3, 3938.9, 3928.4, 3919.2, 3910.6, 3902.6, 3894.6,
    3887.8, 3880.4, 3874.3, 3867.5, 3861.3, 3855.2, 3849.7, 3844.1, 3838.6, 3833.0,
    3827.5, 3822.6, 3817.7, 3813.4, 3808.4, 3804.1, 3799.2, 3795.5, 3791.2, 3786.9,
    3782.6, 3778.9, 3774.0, 3770.3, 3766.0, 3761.1, 3756.8, 3751.2, 3746.9, 3742.0,
    3736.5, 3732.8, 3728.5, 3722.9, 3718.0, 3711.2, 3705.7, 3699.6, 3693.4, 3685.4,
    3679.3, 3676.8, 3675.0, 3672.5, 3670.0, 3665.1, 3658.3, 3639.3, 3603.0, 3557.5,
    3500.2)
# Band 10 Pro (P67 3.101.043) runs the same soft gauge with persist.gauge_soc;
# its table for 12..49 C is at 0x2cd05168 (volttosoc 0x0c3dc460; a factory
# voltage calibration applies only when one is stored).
BAND10PRO_OCV_MV = (
    4438.5, 4428.0, 4419.0, 4411.2, 4403.5, 4395.7, 4388.0, 4379.6, 4371.2, 4362.2,
    4353.2, 4343.9, 4333.7, 4323.5, 4313.6, 4302.7, 4288.5, 4277.3, 4265.2, 4253.7,
    4241.3, 4229.6, 4217.1, 4205.1, 4193.0, 4180.9, 4168.5, 4156.4, 4144.0, 4131.9,
    4120.1, 4108.0, 4096.2, 4086.0, 4074.2, 4062.8, 4051.0, 4039.5, 4028.0, 4017.8,
    4006.0, 3994.2, 3982.2, 3969.8, 3957.4, 3945.6, 3934.4, 3924.5, 3915.5, 3906.8,
    3898.8, 3891.3, 3883.6, 3876.4, 3869.6, 3862.8, 3856.3, 3850.1, 3844.2, 3838.0,
    3832.1, 3826.5, 3821.3, 3815.7, 3810.4, 3805.8, 3800.8, 3796.1, 3791.2, 3786.5,
    3782.2, 3777.5, 3773.2, 3768.6, 3763.9, 3759.6, 3754.6, 3750.0, 3745.0, 3739.7,
    3735.1, 3730.4, 3726.1, 3721.1, 3715.2, 3709.0, 3702.2, 3695.7, 3688.6, 3680.2,
    3671.8, 3665.4, 3660.0, 3655.6, 3650.2, 3645.0, 3640.8, 3630.6, 3605.4, 3554.2,
    3500.1)
# Band 11 (Q66 4.200.078) soft gauge: ADC 2 x 7.0 (vol_gain) in whole
# millivolts. After a cold boot it takes the capacity its OCV table gives for
# the battery voltage (get_cap_data_init "[sram]get cap from table") and shows
# capacity x 1.055 (get_cap_init 0x0c6ab650, cap_ui). The table is built at run
# time for the battery temperature (0x200b408c, read at the core's 26 C
# thermistor input).
BAND11_OCV_MV = (
    4442.9, 4432.8, 4422.6, 4412.6, 4402.6, 4392.5, 4382.5, 4371.5, 4361.6, 4350.6,
    4340.5, 4329.5, 4318.5, 4307.5, 4294.6, 4283.5, 4270.6, 4259.5, 4246.6, 4234.6,
    4222.6, 4210.6, 4198.6, 4186.5, 4174.6, 4162.6, 4150.6, 4138.6, 4127.5, 4115.6,
    4104.5, 4093.6, 4082.6, 4071.6, 4060.6, 4049.6, 4039.6, 4028.6, 4018.6, 4008.6,
    3998.6, 3988.6, 3976.6, 3964.7, 3952.8, 3940.6, 3928.6, 3919.5, 3911.4, 3903.4,
    3896.4, 3889.4, 3883.4, 3877.4, 3871.4, 3866.3, 3860.4, 3855.3, 3850.2, 3845.3,
    3840.3, 3836.2, 3831.3, 3828.2, 3823.2, 3820.2, 3816.2, 3812.2, 3808.2, 3805.1,
    3801.2, 3797.1, 3793.1, 3788.0, 3782.9, 3778.8, 3773.9, 3768.9, 3763.9, 3759.8,
    3755.8, 3751.8, 3747.8, 3742.8, 3735.9, 3729.9, 3723.8, 3718.8, 3710.9, 3702.9,
    3699.7, 3698.6, 3697.6, 3696.6, 3692.8, 3688.8, 3674.1, 3637.6, 3592.0, 3532.5,
    3453.4)
BATTERY_INPUTS = {
    "miwear.watch.n66": {"levelKey": "persist.gauge.cap", "levelScale": 1000,
                         "ocvMillivolts": BAND9_OCV_MV, "adcSlope": 0.051538,
                         "adcOffset": -23.3, "divider": 7.0, "uiScale": 1.055,
                         "charger": True},
    "miwear.watch.o66": {"levelKey": "persist.gauge.cap", "levelScale": 1000,
                         "ocvMillivolts": BAND9_OCV_MV, "adcSlope": 0.051538,
                         "adcOffset": -23.3, "divider": 7.0, "uiScale": 1.055,
                         "charger": True},
    "miwear.watch.n67": {"levelKey": "persist.charger.per", "levelScale": 1,
                         "ocvMillivolts": BAND10_OCV_MV, "adcSlope": 0.051538,
                         "adcOffset": -23.3, "divider": 4.9, "wholeMillivolts": True,
                         "charger": True},
    "miwear.watch.p67": {"levelKey": "persist.gauge_soc", "levelScale": 1,
                         "ocvMillivolts": BAND10PRO_OCV_MV, "adcSlope": 0.051538,
                         "adcOffset": -23.3, "divider": 4.9, "wholeMillivolts": True,
                         "charger": True},
    "miwear.watch.q66": {"ocvMillivolts": BAND11_OCV_MV, "adcSlope": 0.051538,
                         "adcOffset": -23.3, "divider": 7.0, "uiScale": 1.055,
                         "charger": True},
    # P65 exposes raw channel 6 and PMU AC_ON. Its percentage conversion stays
    # hidden until the soft gauge's own OCV table and UI result are calibrated.
    "miwear.watch.p65": {"charger": True},
}


def battery_inputs(model):
    """Verified battery inputs of one preset, or None."""
    if type(model) is not str:
        return None
    for prefix, inputs in BATTERY_INPUTS.items():
        if model.startswith(prefix):
            return inputs
    return None


def _table_capacity(ocv, millivolts):
    """Capacity (0…100) the gauge interpolates from its OCV table."""
    if millivolts >= ocv[0]:
        return 100.0
    if millivolts <= ocv[100]:
        return 0.0
    for index in range(1, 101):
        if millivolts >= ocv[index]:
            return 100 - index + (millivolts - ocv[index]) / (ocv[index - 1] - ocv[index])
    return 0.0


def _table_voltage(ocv, capacity):
    """Open-circuit voltage (mV) the table gives for a capacity (0…100)."""
    index = 100 - capacity
    low = min(99, int(index))
    return ocv[low] + (index - low) * (ocv[low + 1] - ocv[low])


def battery_level_capacity(inputs, level):
    """Capacity a gauge that shows capacity x uiScale restores for a level."""
    return min(100.0, (level + 0.5) / inputs["uiScale"])


def battery_level_value(inputs, level):
    """Saved state of charge for a boot level (the gauge's own persist value)."""
    if "uiScale" in inputs:
        return round(battery_level_capacity(inputs, level) * inputs["levelScale"])
    return level * inputs["levelScale"]


def battery_level_sample(inputs, level):
    """PMU ADC raw count of the open-circuit voltage at a level (0…100 %)."""
    ocv = inputs["ocvMillivolts"]
    if "uiScale" in inputs and "levelKey" in inputs:
        # The gauge restores the saved capacity and shows it x uiScale; the
        # voltage is that capacity's open-circuit voltage.
        millivolts = _table_voltage(ocv, battery_level_capacity(inputs, level))
        return round((millivolts / inputs["divider"] - inputs["adcOffset"]) / inputs["adcSlope"])
    if "uiScale" in inputs:
        # The gauge shows its table capacity x uiScale (at most 100) from the
        # whole-millivolt reading: take the reading whose shown level is the
        # boot level, else the closest one.
        divider, scale = inputs["divider"], inputs["uiScale"]
        def shown(adc_mv):
            return min(100, int(_table_capacity(ocv, adc_mv * divider) * scale))
        low = math.floor(ocv[100] / divider) - 1
        high = math.ceil(ocv[0] / divider) + 1
        adc_mv = min(range(low, high + 1),
                     key=lambda mv: (abs(shown(mv) - level), abs(shown(mv) + 0.5 - level)))
        return round((adc_mv + 0.3 - inputs["adcOffset"]) / inputs["adcSlope"])
    if inputs.get("wholeMillivolts"):
        # The gauge sees whole ADC millivolts: take the lowest reading above
        # the next lower level's voltage, so the level the gauge derives from
        # it is never below the boot level, and aim inside that millivolt.
        divider = inputs["divider"]
        floor_mv = ocv[101 - level] if level else ocv[100] - divider
        adc_mv = math.floor(floor_mv / divider) + 1.3
    else:
        adc_mv = ocv[100 - level] / inputs["divider"]
    return round((adc_mv - inputs["adcOffset"]) / inputs["adcSlope"])


def _catalog(project):
    project = _path(project)
    scope = _json(project / "profiles/mi-band-scope.json")
    ids = scope.get("included_profile_ids")
    if (scope.get("schema_version") != 1 or scope.get("scope") != "xiaomi_smart_band_only"
            or not isinstance(ids, list) or not 39 <= len(ids) <= 64 or len(set(ids)) != len(ids)
            or any(not isinstance(x, str) or not profile_in_scope(x) for x in ids)
            or not INCLUDED_PROFILE_IDS <= set(ids)):
        raise MError("unsupported_scope", "Scope must contain the reviewed profiles plus generated imported-firmware profiles")
    catalog = _json(project / "profiles/catalog.json")
    rows = catalog.get("profiles")
    if catalog.get("schema_version") != 1 or not isinstance(rows, list) or len(rows) > 100:
        raise MError("invalid_catalog", "Unsupported catalog")
    known = {}
    for row in rows:
        if not isinstance(row, dict) or not isinstance(row.get("id"), str) or row["id"] in known:
            raise MError("invalid_catalog", "Missing or duplicate profile ID")
        known[row["id"]] = row
    if not set(ids) <= known.keys():
        raise MError("invalid_catalog", "Catalog is missing a scoped profile")
    for key in ids:
        row = known[key]
        chip = "best1502p" if row.get("model") in ("miwear.watch.p65", "miwear.watch.q65") else "best1503"
        if (row.get("ap", {}).get("chip") != chip
                or "@".join(key.split("@")[:2]) != str(row.get("model")) + "@" + str(row.get("version"))):
            raise MError("invalid_catalog", "Profile and supported board identity disagree")
        _geometry(row["model"])
    return known


def list_profiles(project):
    """Pure static catalog projection. Historical status never becomes capability."""
    rows = _catalog(project)
    return [{"id": key, "model": rows[key]["model"], "version": rows[key]["version"],
             "width": _geometry(rows[key]["model"])[0], "height": _geometry(rows[key]["model"])[1],
             "desktopVerified": False} for key in sorted(INCLUDED_PROFILE_IDS)]


def _expand_pins(paths, *, pin=_pin):
    result = []
    for value in sorted(set(str(_path(x)) for x in paths)):
        path = Path(value)
        if path.is_dir():
            fd = _directory_fd(path)
            os.close(fd)
            entries = []
            for directory, dirs, files in os.walk(path, followlinks=False):
                for name in dirs:
                    if (Path(directory) / name).is_symlink():
                        raise MError("unsafe_path", "Dependency directories cannot contain links")
                entries.extend(Path(directory) / name for name in files)
                if len(entries) > 20000:
                    raise MError("identity_limit", "Dependency identity exceeds 20000 files")
            result.extend(pin(p) for p in sorted(entries))
        else:
            result.append(pin(path))
    if len(result) > 20000 or sum(p["bytes"] for p in result) > 2 * 1024**3:
        raise MError("identity_limit", "Dependency identity exceeds bounded manifest limits")
    return result


def _python_identity(python, *, pin=_pin):
    """Explicitly record venv links; all other inputs reject symlink traversal."""
    logical = _path(python)
    fd = _directory_fd(logical.parent)
    os.close(fd)
    cursor = logical
    links = []
    for _ in range(8):
        if not cursor.is_symlink():
            break
        target = os.readlink(cursor)
        links.append({"path": str(cursor), "target": target})
        cursor = Path(os.path.abspath(cursor.parent / target)) if not os.path.isabs(target) else Path(target)
        fd = _directory_fd(cursor.parent)
        os.close(fd)
    else:
        raise MError("unsafe_python", "Interpreter link chain is too long")
    info = pin(cursor, 128 * 1024 * 1024)
    if not os.access(cursor, os.X_OK):
        raise MError("unsafe_python", "Interpreter is not executable")
    cfg = logical.parent.parent / "pyvenv.cfg"
    return {"invocation": str(logical), "executable": info, "links": links,
            "venvConfig": pin(cfg, 65536) if cfg.exists() else None}


def _dependency_link_target(logical):
    """Resolve every path component without silently following a link.

    Targets are interpreted component by component, so a '..' after a linked
    directory has filesystem semantics rather than premature lexical cleanup.
    """
    logical = _path(logical)
    pending = list(logical.parts[1:])
    resolved, chain, seen = [], [], set()
    fd = os.open("/", os.O_RDONLY | os.O_DIRECTORY)
    visits = 0
    try:
        while pending:
            visits += 1
            if visits > 1024:
                raise MError("unsafe_dependency_link", "Dependency path expansion exceeds its bound")
            part = pending.pop(0)
            if part in ("", "."):
                continue
            if part == "..":
                if resolved:
                    resolved.pop()
                    parent = os.open("..", os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd)
                    os.close(fd)
                    fd = parent
                continue
            before = os.stat(part, dir_fd=fd, follow_symlinks=False)
            if stat.S_ISLNK(before.st_mode):
                node = (before.st_dev, before.st_ino)
                if (node in seen or len(chain) >= 32 or before.st_nlink != 1
                        or before.st_uid not in (0, os.getuid())):
                    raise MError("unsafe_dependency_link", "Unowned, duplicate or cyclic dependency link")
                seen.add(node)
                target = os.readlink(part, dir_fd=fd)
                after = os.stat(part, dir_fd=fd, follow_symlinks=False)
                if ((before.st_dev, before.st_ino, before.st_mtime_ns, before.st_ctime_ns) !=
                        (after.st_dev, after.st_ino, after.st_mtime_ns, after.st_ctime_ns)):
                    raise MError("file_changed", "Dependency link changed during resolution")
                if not target or len(os.fsencode(target)) > 4096:
                    raise MError("unsafe_dependency_link", "Dependency link target exceeds its bound")
                chain.append({"path": str(Path("/").joinpath(*resolved, part)), "target": target})
                if os.path.isabs(target):
                    os.close(fd)
                    fd = os.open("/", os.O_RDONLY | os.O_DIRECTORY)
                    resolved = []
                pending = target.split("/") + pending
                continue
            resolved.append(part)
            if pending:
                child = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd)
                actual = os.fstat(child)
                if (actual.st_dev, actual.st_ino) != (before.st_dev, before.st_ino):
                    os.close(child)
                    raise MError("file_changed", "Dependency ancestor changed during resolution")
                os.close(fd)
                fd = child
        return {"logicalPath": str(logical), "chain": chain,
                "realPath": str(Path("/").joinpath(*resolved))}
    finally:
        os.close(fd)


def _dependency_link_identity(logical, *, pin=_pin):
    before = _dependency_link_target(logical)
    # The resolved endpoint must obey exactly the same regular-file, owner,
    # permission, hard-link and size checks as direct dependency files.
    file_identity = pin(before["realPath"])
    if _dependency_link_target(logical) != before:
        raise MError("file_changed", "Dependency link mapping changed during inspection")
    return {**before, "file": file_identity}


def _dependency_links(paths, *, pin=_pin):
    values = [str(_path(path)) for path in paths]
    if len(values) > 512 or len(values) != len(set(values)):
        raise MError("identity_limit", "Dependency mappings must be unique and contain at most 512 paths")
    return [_dependency_link_identity(path, pin=pin) for path in sorted(values)]


def inspect_backend(project, python, profile_id, *, prepared, dependency_paths=(), bridge_paths=(),
                    dependency_links=(), bootstrap=None, engine_root=None, instance_storage=None, engine_info=None):
    """Read only. Caller supplies already prepared storage/board evidence.

    dependency_paths must cover the selected interpreter's non-system runtime
    (stdlib/site-packages and non-system dynamic libraries). dependency_links
    records the original load names with any linked ancestors/leaf. The adapter records
    the precise declared closure, not an invented dependency-completeness claim.
    """
    engine_identity = None
    runtime_capabilities = {}
    declarations = {}
    project = _path(project)
    if engine_root is not None:
        from .bundled_runtime import _manifest
        from .instance_storage import checked_capabilities
        engine = engine_info if engine_info is not None else _manifest(engine_root)
        engine_identity = engine["sha256"]
        for relative, record in engine["files"].items():
            declarations[str(engine["root"] / relative)] = (record["sha256"], "manifest")
            if relative.startswith("project/"):
                declarations[str(project / relative[len("project/"):])] = (record["sha256"], "manifest")
            elif relative.startswith("lib/"):
                declarations[str(project.parent / relative)] = (record["sha256"], "manifest")
        runtime_capabilities = checked_capabilities(engine["manifest"],
            engine["files"][engine["manifest"]["entrypoints"]["qemu"]]["sha256"])
    project = _path(project)
    rows = _catalog(project)
    scoped = _json(project / "profiles/mi-band-scope.json")["included_profile_ids"]
    if not profile_in_scope(profile_id) or profile_id not in scoped:
        raise MError("unsupported_profile", "Choose a complete included profile ID; aliases are rejected")
    if not isinstance(prepared, dict) or set(prepared) - {"board", "resource", "seed", "seedProof", "resourceProof", "factory"}:
        raise MError("invalid_prepared", "Unknown prepared input field")
    # Generated mode: nothing is pre-pinned beyond AP/OTA (and an optional catalog
    # resource); the runner creates board/storage/seed inside the sandbox.
    generated = not prepared
    if not generated and not all(prepared.get(key) for key in ("board", "resource")):
        raise MError("preparation_required", "Static inspection requires existing board and resource inputs")
    if not dependency_paths or not bridge_paths:
        raise MError("identity_incomplete", "Explicit interpreter/dependency and bridge identity paths are required")
    profile = rows[profile_id]
    expected_chip = "best1502p" if profile["model"] in ("miwear.watch.p65", "miwear.watch.q65") else "best1503"
    if expected_chip == "best1502p":
        # P65 implements the pinned ADC ABI; Q65 has independent storage but
        # must continue to reject band ADC/live hardware inputs.
        allowed = ("nandPersistence", "norPersistence", "virtualPeer")
        if profile["model"] == "miwear.watch.p65":
            allowed += ("pmuAdcRaw",)
        runtime_capabilities = {key: value for key, value in runtime_capabilities.items()
                                if key in allowed}
    declarations[str(_path(profile["ap_path"]))] = (profile["ap"]["sha256"], "catalog")
    if profile.get("ota_path"):
        declarations[str(_path(profile["ota_path"]))] = (profile.get("ota_sha256"), "catalog")
    members = [p for p in profile.get("partitions", []) if p.get("name") == "vela_resource.bin"]
    if profile.get("resource_path") and len(members) == 1 and members[0].get("sha256"):
        declarations[str(_path(profile["resource_path"]))] = (members[0]["sha256"], "catalog")
    descriptions = {}
    def pin(path, limit=MAX_ASSET):
        path = _path(path)
        key = str(path)
        if key not in descriptions:
            declared = declarations.get(key)
            if declared is None:
                descriptions[key] = _pin(path, limit)  # Small unlisted host metadata, descriptive only.
            else:
                fd, info = _open_file(path, limit)
                os.close(fd)
                descriptions[key] = {"path":key, "bytes":info.st_size,
                                     "sha256":declared[0], "sha256Source":declared[1]}
        return dict(descriptions[key])
    ap = pin(profile["ap_path"])
    if ap["bytes"] != profile["ap"]["size"]:
        raise MError("identity_mismatch", "AP size mismatch")
    ota = pin(profile["ota_path"]) if profile.get("ota_path") else None
    if ota is None and profile.get("ota_sha256") is not None:
        raise MError("identity_mismatch", "OTA identity has no source file")
    members = [p for p in profile.get("partitions", []) if p.get("name") == "vela_resource.bin"]
    if len(members) > 1:
        raise MError("invalid_catalog", "Ambiguous ROMFS resource")
    storage = "romfs" if members else "yaffs"
    if generated:
        if profile["ap"].get("chip") != expected_chip:
            raise MError("identity_mismatch", "Generated mode requires a BES1503 band profile")
        if ota is None and not profile.get("resource_path"):
            raise MError("preparation_required", "Generated mode needs the OTA package or a catalog resource")
        board = None
        resource = pin(profile["resource_path"]) if profile.get("resource_path") else None
        prepared_pins = {"resource": resource} if resource is not None else {}
    else:
        board = _json(prepared["board"])
        if (board.get("profile_id") != profile_id or board.get("chip") != expected_chip
                or not isinstance(board.get("geometry"), dict)
                or any(type(v) is not int or not 0 <= v <= 0xffffffff for v in board["geometry"].values())):
            raise MError("identity_mismatch", "Prepared board differs from exact profile")
        resource = pin(prepared["resource"])
        cache = project / "assets/devices" / (profile.get("ota_sha256") or "unused")
        expected_resource = (Path(profile["resource_path"]) if profile.get("resource_path") else
                             cache / ("vela_resource.bin" if members else "nand-system.bin"))
        if str(expected_resource) != resource["path"]:
            raise MError("identity_mismatch", "Prepared resource is not the runner's selected path")
        prepared_pins = {key: pin(value) for key, value in sorted(prepared.items()) if value is not None}
        if board.get("local_setup_bypass") is True:
            if not prepared.get("factory") or prepared_pins["factory"]["bytes"] != 128 * 1024:
                raise MError("preparation_required", "Local desktop startup requires a pinned 128 KiB generated factory image")
        elif prepared.get("factory"):
            raise MError("invalid_prepared", "This board does not select a local desktop factory overlay")
        if storage == "romfs":
            if resource["bytes"] != members[0].get("size") or any(prepared.get(k) for k in ("seed", "seedProof")):
                raise MError("identity_mismatch", "ROMFS size/seed does not match selected profile")
        else:
            if not all(prepared.get(key) for key in ("resourceProof", "seed", "seedProof")):
                raise MError("preparation_required", "YAFFS needs existing resource and seed proofs/images")
            proof = _json(prepared["resourceProof"])
            seed = _json(prepared["seedProof"])
            if (proof.get("schema_version") != 2 or proof.get("ota_sha256") != profile["ota_sha256"]
                    or proof.get("ap_sha256") != ap["sha256"] or not proof.get("remount_content_verified")
                    or str(prepared["resourceProof"]) != str(cache / "nand-system.json")):
                raise MError("identity_mismatch", "YAFFS resource proof is incomplete or belongs to another profile")
            _require_sha(resource["sha256"], proof.get("sha256"), "YAFFS resource")
            if seed.get("schema_version") != 2 or seed.get("profile_id") != profile_id or seed.get("ap_sha256") != ap["sha256"]:
                raise MError("identity_mismatch", "YAFFS seed proof belongs to another profile")
            expected_seed = cache / "local-ui" / str(seed.get("cache_config_sha256")) / "nand-data.bin"
            if str(prepared["seed"]) != str(expected_seed) or Path(prepared["seedProof"]) != expected_seed.with_suffix(".json"):
                raise MError("identity_mismatch", "YAFFS seed path is not the selected immutable cache")
            _require_sha(prepared_pins["seed"]["sha256"], seed.get("sha256"), "YAFFS seed")
            if (board.get("data_image_sha256") != seed["sha256"] or board.get("local_ui_seed") != seed
                    or prepared_pins["seed"]["bytes"] != board["geometry"].get("nand-data-bytes")):
                raise MError("identity_mismatch", "Board/seed geometry or content differs")
    nor_capability = runtime_capabilities.get("norPersistence")
    if nor_capability is not None and profile_id not in nor_capability["profiles"]:
        # Firmware outside the packaged table: persistent NOR and the virtual
        # peer follow the layout the runner derives from this AP.
        from .instance_storage import _nor_capability, derived_nor_layout, virtual_profile
        layout = derived_nor_layout(project, profile_id, storage)
        if layout is not None and len(nor_capability["profiles"]) < 64:
            extended = {**nor_capability, "profiles": {**nor_capability["profiles"], profile_id: layout}}
            qemu_sha = engine["files"][engine["manifest"]["entrypoints"]["qemu"]]["sha256"]
            try:
                _nor_capability(extended, profile_id, ap["sha256"], qemu_sha)
            except ValueError:
                extended = None
            peer = runtime_capabilities.get("virtualPeer")
            if extended is not None and layout.get("apSHA256") == ap["sha256"]:
                runtime_capabilities["norPersistence"] = extended
                if peer is not None and virtual_profile(profile_id) is not None:
                    runtime_capabilities["virtualPeer"] = {
                        **peer, "profiles": {**peer["profiles"], profile_id: {"apSHA256": ap["sha256"]}}}
    runtime_files = (RUNTIME_FILES[:-1] + ("backend/src/instance_data.py",) + RUNTIME_FILES[-1:]
                     if runtime_capabilities else RUNTIME_FILES)
    if runtime_capabilities.get("virtualPeer"):
        from .virtual_peer import RUNTIME_FILES as peer_runtime_files
        runtime_files += peer_runtime_files
    runtime = [{**pin(project / name), "path": name} for name in runtime_files]
    source_manifest = {"schema": "minative-python-runtime-v1", "files": runtime}
    # Include imported siblings omitted from the upstream evidence fingerprint.
    import_paths = [*project.glob("*.py"), *(project / "backend/src").glob("*.py")]
    if runtime_capabilities.get("virtualPeer"):
        import_paths += [project / name for name in peer_runtime_files]
        cache = project / "backend/src/virtual_peer/__pycache__"
        if cache.exists():
            import_paths.append(cache)
    # -B forbids cache writes but Python may still read existing bytecode.
    import_paths += [p for p in (project / "__pycache__", project / "backend/src/__pycache__") if p.exists()]
    extra_runtime = _expand_pins(import_paths, pin=pin)
    identity = {"schema": SCHEMA, "hashVerification": "not-required-for-startup", "project": str(project), "profile": profile,
                "catalog": pin(project / "profiles/catalog.json"),
                "scope": pin(project / "profiles/mi-band-scope.json"),
                "python": _python_identity(python, pin=pin), "qemu": pin(project / "backend/build/qemu-system-arm"),
                "runtime": source_manifest, "runtimeSHA256": _digest(source_manifest),
                "importableSource": extra_runtime, "adapter": pin(Path(__file__).absolute()),
                # Absolute pins let the sandbox grant exactly these non-Python runtime
                # sources, which revalidation stats (relative records grant no path).
                "runtimeSourcePins": [pin(project / name) for name in runtime_files],
                "dependencies": _expand_pins(dependency_paths, pin=pin), "dependencyLinks": _dependency_links(dependency_links, pin=pin),
                "bridge": _expand_pins(bridge_paths, pin=pin),
                "ap": ap, "ota": ota, "storageKind": storage,
                "prepared": prepared_pins, "board": board,
                "mode": "generated" if generated else "prepared",
                "bindingCompleted": False, "guestInstallationVerified": False}
    if engine_identity is not None:
        identity["engineManifestSHA256"] = engine_identity
    if runtime_capabilities:
        identity["runtimeCapabilities"] = runtime_capabilities
    if instance_storage is not None:
        from .instance_storage import validate_binding
        binding = _decode(_canonical(instance_storage))
        validate_binding(binding, project=project, profile_id=profile_id, model=profile["model"],
                         qemu_sha256=identity["qemu"]["sha256"],
                         capability=runtime_capabilities.get("nandPersistence"),
                         nor_capability=runtime_capabilities.get("norPersistence"), ap_sha256=ap["sha256"],
                         virtual_capability=runtime_capabilities.get("virtualPeer"))
        identity["instanceStorage"] = binding
    if bootstrap is not None:
        identity["bootstrap"] = pin(_path(bootstrap), MAX_JSON)
    if storage == "yaffs":
        identity["builders"] = [pin(project / "backend/build" / name) for name in ("yaffs-image", "legacy-ui-db")]
    # Every supported band keeps /data (persist.db) on a YAFFS NAND partition,
    # including ROMFS-resource models; the settings tool is pinned for all.
    if (project / "backend/build/instance-props").is_file():
        identity.setdefault("builders", []).append(pin(project / "backend/build/instance-props"))
    if identity.get("builders"):
        # The builders compile/read these vendored sources inside the sandbox.
        references = project / "references"
        identity["builderSources"] = ([pin(path) for path in sorted(references.rglob("*")) if path.is_file()]
                                      if references.is_dir() else [])
    width, height = _geometry(profile["model"])
    return {"schema": SCHEMA, "backend": "minative", "profileID": profile_id,
            "width": width, "height": height, "identity": identity,
            "approvalDigest": _digest(identity),
            "spec": {"project": str(project), "python": str(_path(python)), "profile_id": profile_id,
                     "prepared": {key: str(value) for key, value in prepared.items() if value is not None},
                     "dependency_paths": [str(_path(p)) for p in dependency_paths],
                     "dependency_links": [str(_path(p)) for p in dependency_links],
                     "bootstrap": str(_path(bootstrap)) if bootstrap is not None else None,
                     "engine_root": str(_path(engine_root)) if engine_root is not None else None,
                     **({"instance_storage": binding} if instance_storage is not None else {}),
                     "bridge_paths": [str(_path(p)) for p in bridge_paths]}}


def revalidate_plan(plan):
    """Recheck the local launch envelope without hashing its dependency closure.

    SHA fields describe catalog/build inputs; they are not an execution permit.
    Accepted image integrity belongs to storage preparation and commit.
    """
    if not isinstance(plan, dict) or plan.get("schema") != SCHEMA:
        raise MError("invalid_plan", "Unsupported minative identity plan")
    identity, spec = plan["identity"], plan["spec"]
    if identity.get("project") != spec.get("project") or identity["profile"]["id"] != plan["profileID"]:
        raise MError("invalid_plan", "Launch paths/profile differ")
    if spec.get("engine_root"):
        from .bundled_runtime import _manifest
        _manifest(spec["engine_root"])  # Structure, bounds and paths only.
    seen = set()
    def visit(value):
        if isinstance(value, dict):
            path = value.get("path")
            if isinstance(path, str) and path.startswith("/") and "bytes" in value and path not in seen:
                seen.add(path)
                fd, info = _open_file(_path(path))
                os.close(fd)
                if info.st_size != value["bytes"]:
                    raise MError("identity_changed", "Prepared file size changed")
            for child in value.values(): visit(child)
        elif isinstance(value, list):
            for child in value: visit(child)
    visit(identity)
    for mapping in identity.get("dependencyLinks", []):
        observed = _dependency_link_target(mapping["logicalPath"])
        if any(observed[key] != mapping[key] for key in ("logicalPath", "chain", "realPath")):
            raise MError("identity_changed", "Dependency path mapping changed")
    if identity.get("instanceStorage"):
        from .instance_storage import validate_binding
        capabilities = identity.get("runtimeCapabilities", {})
        validate_binding(identity["instanceStorage"], project=spec["project"], profile_id=plan["profileID"],
            model=identity["profile"]["model"], qemu_sha256=identity["qemu"]["sha256"],
            capability=capabilities.get("nandPersistence"), nor_capability=capabilities.get("norPersistence"),
            ap_sha256=identity["ap"]["sha256"], virtual_capability=capabilities.get("virtualPeer"))
    return plan


def with_instance_storage(plan, binding):
    """Attach the already prepared storage transaction without rereading Engine files."""
    result = _decode(_canonical(plan))
    result["identity"]["instanceStorage"] = binding
    result["spec"]["instance_storage"] = binding
    result["approvalDigest"] = _digest(result["identity"])
    return result


def _inside_private(path, root, *, directory=False):
    path, root = _path(path), _path(root)
    try:
        relative = path.relative_to(root)
    except ValueError as error:
        raise MError("unsafe_control", "Control path escapes the private run") from error
    if not relative.parts:
        raise MError("unsafe_control", "Control path must name an entry inside the run")
    parent = path if directory else path.parent
    for ancestor in [root, *[root.joinpath(*relative.parts[:n]) for n in range(1, len(relative.parts) + int(directory))]]:
        fd = _directory_fd(ancestor, private=True)
        os.close(fd)
    fd = _directory_fd(parent, private=True)
    os.close(fd)
    return path


def _peer_identity(sock):
    if sys.platform == "darwin":
        # Python builds differ in whether socket.getpeereid is exposed.
        libc = ctypes.CDLL(None, use_errno=True)
        uid, gid = ctypes.c_uint(), ctypes.c_uint()
        getpeereid = libc.getpeereid
        getpeereid.argtypes = (ctypes.c_int, ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint))
        getpeereid.restype = ctypes.c_int
        if getpeereid(sock.fileno(), ctypes.byref(uid), ctypes.byref(gid)):
            raise MError("peer_identity", "Cannot authenticate runner uid")
        pid = struct.unpack("i", sock.getsockopt(0, 2, 4))[0]  # SOL_LOCAL, LOCAL_PEERPID
        return pid, uid.value
    if sys.platform.startswith("linux"):
        pid, uid, _ = struct.unpack("3i", sock.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
        return pid, uid
    raise MError("peer_identity", "Kernel peer PID authentication is unavailable")


def _group_members(group):
    """Darwin's killpg(pgid, 0) may report EPERM for an already empty group."""
    if sys.platform != "darwin":
        try:
            os.killpg(group, 0)
            return [group]
        except ProcessLookupError:
            return []
    library = ctypes.CDLL("/usr/lib/libproc.dylib", use_errno=True)
    listing = library.proc_listpids
    listing.argtypes = (ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_int)
    listing.restype = ctypes.c_int
    entries = (ctypes.c_int * 4096)()
    ctypes.set_errno(0)
    size = listing(2, group, entries, ctypes.sizeof(entries))  # PROC_PGRP_ONLY
    if size < 0 or size >= ctypes.sizeof(entries) or (size == 0 and ctypes.get_errno()):
        raise MError("cleanup_unconfirmed", "Cannot enumerate the owned process group")
    return [entries[i] for i in range(size // ctypes.sizeof(ctypes.c_int)) if entries[i] > 0]


def classify_result(result, returncode, *, stopped=False):
    """A bounded time_limit exit 2 is normal completion, never desktop proof."""
    base = {"returncode": returncode, "desktopVerified": False, "guestInstalled": False}
    if not isinstance(result, dict):
        return dict(base, state="stopped" if stopped else "failed", reason="stopped" if stopped else "missing_result")
    outcome = result.get("outcome")
    if (result.get("guest_faults") or result.get("fault_addresses") or result.get("cortex_fault_status")
            or result.get("guest_halt_loop_reached") or outcome in ("guest_fault", "guest_halted")):
        reason = "guest_fault"
    elif type(result.get("gpu_render_rejection_count", 0)) is not int or result.get("gpu_render_rejection_count", 0) > 0:
        reason = "gpu_rejection"
    elif result.get("qmp_error") or outcome == "host_error":
        reason = "host_error"
    elif result.get("touch_i2c_rejection_count", 0):
        reason = "input_rejection"
    elif outcome == "time_limit" and result.get("time_limit_reached") is True and returncode in (0, 2):
        return dict(base, state="completed", reason="time_limit")
    elif outcome in ("guest_poweroff", "guest_reboot") and returncode in (0, 2):
        # The firmware itself powered off or restarted; the host mirrors it.
        return dict(base, state="stopped", reason=outcome)
    elif stopped and outcome in ("interrupted", "window_closed", "qemu_stopped"):
        return dict(base, state="stopped", reason="stopped")
    else:
        reason = outcome if isinstance(outcome, str) and len(outcome) <= 64 else "invalid_result"
    return dict(base, state="failed", reason=reason)


def _failure_detail(result, log):
    """One readable line explaining a failed run: the runner's own error
    (for example an engine-level incompatibility) or its control failure."""
    if isinstance(result, dict) and isinstance(result.get("qmp_error"), str):
        return result["qmp_error"][:512]
    try:
        fd = os.open(log, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    except OSError:
        return None
    try:
        size = os.fstat(fd).st_size
        tail = os.pread(fd, 8192, max(0, size - 8192)).decode("utf-8", "replace")
    finally:
        os.close(fd)
    lines = [line.strip() for line in tail.splitlines() if line.strip()]
    for line in reversed(lines):
        if "error:" in line or line.startswith(("ValueError", "RuntimeError", "OSError")):
            return line.split("error:", 1)[-1].strip()[:512] or None
    return None


class MinativeAdapter:
    """One launch, one immutable plan, one private run directory.

    execution_prefix/isolation_verified are trusted embedding-process arguments,
    never RPC inputs. Production prefixes must write runner_metadata containing
    {pid: runner PID, supervisorPID: Popen PID}, mode 0600, before control connects.
    A fixture-only flag exists solely for authored test runners; it is reported.
    """
    def __init__(self, plan, root, approved_digest=None, seconds=0, *, execution_prefix=(),
                 isolation_verified=False, fixture_only=False, native_broker=False):
        if type(seconds) is not int or not 0 <= seconds <= MAX_RUN_SECONDS:
            raise MError("invalid_duration", "Duration must be integer 0 (unlimited) or 1…%d seconds" % MAX_RUN_SECONDS)
        self.plan = _decode(_canonical(plan))
        self.root = _path(root)
        fd = _directory_fd(self.root, private=True)
        os.close(fd)
        self.native_broker = None
        if native_broker:
            if fixture_only or execution_prefix or isolation_verified:
                raise MError("invalid_isolation", "Native broker cannot be mixed with direct launch or fixture modes")
            from .native_broker import NativeBrokerSession
            self.native_broker = NativeBrokerSession(self.root, self.plan, seconds)
        self.run = self.native_broker.run if self.native_broker else self.root / ("m-" + uuid.uuid4().hex[:12])
        self.runner_metadata = self.run / "runner-process.json"
        self.approved_digest, self.seconds = approved_digest, seconds
        self.execution_prefix = tuple(execution_prefix)
        self.isolation_verified, self.fixture_only = isolation_verified, fixture_only
        self.process = None
        self._state = "ready"
        self._reason = None
        self._control = None
        self._held = False
        self._point = (0, 0)
        self._stop_requested = False
        self._lock = threading.RLock()
        self._cancel = threading.Event()
        self._watcher = None
        self._log = None
        self._cleanup = None
        self._finished = False
        self._observed_board = None
        self._detail = None
        self._developer_process_identity = None

    @property
    def run_root(self):
        return self.run

    def launch_spec(self):
        spec = self.plan["spec"]
        # Inherit no PYTHONPATH/PYTHONHOME/DYLD or service-discovery variables.
        env = {"PATH": "/usr/bin:/bin", "LANG": "en_US.UTF-8", "LC_ALL": "en_US.UTF-8",
               "HOME": str(self.run), "TMPDIR": str(self.run / "tmp"),
               "PYTHONNOUSERSITE": "1", "PYTHONDONTWRITEBYTECODE": "1", "PYTHONUNBUFFERED": "1"}
        command = ([spec["python"], "-I", "-S", "-B", spec["bootstrap"], "--project", spec["project"],
                    "--runner", "multi_device.py", "--"] if spec.get("bootstrap") else
                   [spec["python"], "-B", "-s", "-E", str(Path(spec["project"]) / "multi_device.py")])
        command += ["--device", self.plan["profileID"], "--headless", "--log-dir", str(self.run)]
        if self.seconds:
            command += ["--seconds", str(self.seconds)]
        if self.plan["identity"].get("instanceStorage"):
            from .instance_storage import instance_arguments
            command += instance_arguments(self.plan["identity"]["instanceStorage"], run_directory=self.run)
        command += ["--skip-oobe" if self.plan.get("skipOOBE", True) else "--no-skip-oobe"]
        return {"command": command,
                "env": env, "cwd": spec["project"], "runnerMetadata": str(self.runner_metadata),
                "runDirectory": str(self.run), "maxWallSeconds": self.seconds + 15 if self.seconds else 0}

    def start(self):
        with self._lock:
            if self._state != "ready":
                raise MError("invalid_state", "An adapter instance can be started once")
            if not self.approved_digest or self.approved_digest != self.plan.get("approvalDigest"):
                raise MError("approval_required", "Exact current identity digest needs explicit execution approval")
            if self.native_broker is None and (not self.isolation_verified or (not self.execution_prefix and not self.fixture_only)):
                raise MError("isolation_unavailable", "A trusted local sandbox supervisor is required")
            revalidate_plan(self.plan)
            if self.native_broker is None:
                self.run.mkdir(mode=0o700)
                (self.run / "tmp").mkdir(mode=0o700)
            else:
                self.native_broker.check()
            if len(os.fsencode(self.run / "tmp/q66-qmp-12345678/input.sock")) > 103:
                raise MError("unsafe_session", "Run path is too long for bounded Unix sockets")
            try:
                if self.native_broker is not None:
                    self.process = self.native_broker.launch()
                else:
                    spec = self.launch_spec()
                    fd = os.open(self.run / "adapter-runner.log", os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
                    self._log = os.fdopen(fd, "wb")
                    self.process = subprocess.Popen([*self.execution_prefix, *spec["command"]], cwd=spec["cwd"],
                        env=spec["env"], stdin=subprocess.DEVNULL, stdout=self._log, stderr=self._log,
                        close_fds=True, start_new_session=True, umask=0o077)
            except BaseException:
                if self._log:
                    self._log.close()
                    self._log = None
                self._state = "failed"
                raise
            self._state = "starting"
            self._watcher = threading.Thread(target=self._watch, name="minative-watch", daemon=True)
            self._watcher.start()
            return self.status()

    def _runner_pid(self):
        if self.fixture_only and not self.execution_prefix:
            return self.process.pid
        metadata = _json(self.runner_metadata, private=True)
        pid = metadata.get("pid")
        if (type(pid) is not int or pid < 2 or metadata.get("supervisorPID") != self.process.pid
                or os.getpgid(pid) != self.process.pid):
            raise MError("peer_identity", "Runner supervisor metadata does not match owned group")
        return pid

    def _read_control(self):
        value = _json(self.run / "control.json", private=True)
        _validate_control_schema(value, self.plan)
        if value["pid"] < 2 or os.getpgid(value["pid"]) != self.process.pid:
            raise MError("peer_identity", "Core process does not belong to this session group")
        frame = _inside_private(value["framebuffer"], self.run)
        input_path = _inside_private(value["input_socket"], self.run / "tmp")
        qmp_path = _inside_private(value["qmp"], self.run / "tmp")
        if frame != self.run / "lcd-frame.raw" or input_path.name != "input.sock" or qmp_path.name != "qmp.sock":
            raise MError("unsafe_control", "Unexpected runner endpoint name")
        if input_path.parent != qmp_path.parent or not input_path.parent.name.startswith("q66-qmp-"):
            raise MError("unsafe_control", "Runner socket directory does not match")
        paths = [input_path, qmp_path]
        if "virtual_peer" in value:
            peer_path = _inside_private(value["virtual_peer"]["socket"], self.run / "tmp")
            if peer_path.name != "peer.sock" or peer_path.parent != input_path.parent:
                raise MError("unsafe_control", "Virtual peer endpoint differs from runner directory")
            paths.append(peer_path)
        for path in paths:
            info = path.lstat()
            if not stat.S_ISSOCK(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
                raise MError("unsafe_control", "Runner endpoint must be a same-user private socket")
        identity = _json(self.run / "identity.json", private=True)
        expected = self.plan["identity"]
        generated = expected.get("mode") == "generated"
        resource = expected["prepared"].get("resource")
        wanted = {"profile_id": self.plan["profileID"]}
        if not generated:
            wanted["data_sha256"] = expected["prepared"].get("seed", {}).get("sha256")
        storage_binding = expected.get("instanceStorage")
        if storage_binding:
            wanted["instance_data_sha256"] = storage_binding["dataSHA256"]
            wanted["instance_spare_sha256"] = storage_binding["spareSHA256"]
            if storage_binding.get("norPersistent") is True:
                wanted["instance_nor_sha256"] = storage_binding["norSHA256"]
                wanted["instance_nor_initialize"] = storage_binding["norInitialize"]
            if storage_binding.get("virtualPeerEnabled") is True:
                wanted["virtual_peer_state_sha256"] = storage_binding["virtualPeerStateSHA256"]
        virtual_enabled = storage_binding and storage_binding.get("virtualPeerEnabled") is True
        if not virtual_enabled and "virtual_peer_state_sha256" in identity:
            raise MError("identity_mismatch", "Runner added an unapproved virtual identity")
        for key, expected_value in wanted.items():
            if identity.get(key) != expected_value or (type(expected_value) is bool and type(identity.get(key)) is not bool):
                raise MError("identity_mismatch", "Runner identity differs: " + key)
        board = _json(self.run / "board.json", private=True)
        # Current runners echo the core identity into the board report so the
        # selected core, storage descriptor, and actual QEMU process stay on
        # one profile-bound contract. Consume it before comparing the remaining
        # board description with the host-prepared plan.
        board.pop("qemu_sha256", None)  # Descriptive runner evidence, not a startup gate.
        if virtual_enabled:
            if board.pop("virtual_peer_state_sha256", None) != storage_binding["virtualPeerStateSHA256"]:
                raise MError("identity_mismatch", "Runner virtual identity differs from this instance")
        elif "virtual_peer_state_sha256" in board:
            raise MError("identity_mismatch", "Runner added an unapproved virtual board binding")
        if storage_binding:
            wanted_storage = {"schema": 1, **{key: storage_binding[key] for key in
                ("dataImage", "spareImage", "dataSHA256", "spareSHA256", "dataOffset", "dataBytes")}}
            if (board.pop("instance_storage", None) != wanted_storage
                    or board.get("geometry", {}).get("nand-data-offset") != storage_binding["dataOffset"]
                    or board.get("geometry", {}).get("nand-data-bytes") != storage_binding["dataBytes"]):
                raise MError("identity_mismatch", "Runner instance data binding differs")
            if storage_binding.get("norPersistent") is True:
                if "norAPBytes" in storage_binding:
                    wanted_nor = {"schema": 1, "profileID": self.plan["profileID"],
                        "apSHA256": expected["ap"]["sha256"],
                        "apBytes": storage_binding["norAPBytes"], "apOffset": storage_binding["norAPOffset"],
                        "flashBytes": storage_binding["norFlashBytes"],
                        "factoryOffset": storage_binding["norFactoryOffset"],
                        "factoryBytes": (expected.get("board") or board)["factory_bytes"],
                        **{key: storage_binding[key] for key in
                        ("norImage", "norSHA256", "norOffset", "norBytes", "norInitialize")}}
                    if "norConfigOffset" in storage_binding:
                        wanted_nor.update(configOffset=storage_binding["norConfigOffset"],
                                          configBytes=storage_binding["norConfigBytes"])
                    nor_number_keys = ("schema", "apBytes", "apOffset", "flashBytes", "factoryOffset",
                                       "factoryBytes", "norOffset", "norBytes")
                else:
                    nor_capability = expected.get("runtimeCapabilities", {}).get("norPersistence", {})
                    layout = nor_capability.get("profiles", {}).get(self.plan["profileID"])
                    if type(layout) is not dict:
                        raise MError("identity_mismatch", "Runner NOR profile is absent from the pinned capability")
                    model = expected["profile"]["model"]
                    factory_bytes = 8192 if model.startswith(("miwear.watch.n66", "miwear.watch.o66", "miwear.watch.n67")) else 131072
                    wanted_nor = {"schema": 1, "profileID": self.plan["profileID"],
                        "apSHA256": layout["apSHA256"], "apBytes": layout["apBytes"],
                        "apOffset": layout["apOffset"], "flashBytes": layout["flashBytes"],
                        "factoryOffset": layout["factoryOffset"], "factoryBytes": factory_bytes,
                        **{key: storage_binding[key] for key in
                        ("norImage", "norSHA256", "norOffset", "norBytes", "norInitialize")}}
                    if "configBytes" in layout:
                        wanted_nor.update(configOffset=layout["configOffset"], configBytes=layout["configBytes"])
                    nor_number_keys = ("schema", "apBytes", "apOffset", "flashBytes", "factoryOffset",
                                       "factoryBytes", "norOffset", "norBytes")
                observed_nor = board.pop("instance_nor", None)
                if (not isinstance(observed_nor, dict)
                        or {k:v for k,v in observed_nor.items() if k != "apSHA256"} != {k:v for k,v in wanted_nor.items() if k != "apSHA256"}
                        or any(type(observed_nor.get(key)) is not int for key in nor_number_keys)
                        or type(observed_nor.get("norInitialize")) is not bool):
                    raise MError("identity_mismatch", "Runner NOR binding differs")
            elif "instance_nor" in board:
                raise MError("identity_mismatch", "Runner added an unapproved NOR binding")
        if generated:
            # Observed, not pre-approved: the sandboxed runner derived this board from the pinned AP/OTA.
            self._observed_board = {"sha256": hashlib.sha256(_canonical(board)).hexdigest(),
                                    "chip": board.get("chip"), "profile_id": board.get("profile_id")}
            if board.get("profile_id") != self.plan["profileID"]:
                raise MError("identity_mismatch", "Generated board belongs to another profile")
        else:
            if {k:v for k,v in board.items() if k != "ap_sha256"} != {k:v for k,v in expected["board"].items() if k != "ap_sha256"}:
                raise MError("identity_mismatch", "Runner board or source evidence differs")
            if "factory" in expected["prepared"]:
                generated_factory = _pin(self.run / "desktop-factory.bin", 128 * 1024)
                if (generated_factory["sha256"], generated_factory["bytes"]) != (expected["prepared"]["factory"]["sha256"], 128 * 1024):
                    raise MError("identity_mismatch", "Generated factory initialization differs from approved image")
        return value

    def _watch(self):
        end = time.monotonic() + self.seconds + 15 if self.seconds else None
        publication_deadline = None
        next_quota = 0
        try:
            while self.process.poll() is None:
                if getattr(self.process, "protocol_error", None):
                    self._reason = "native_broker_protocol"
                    self.stop()
                    return
                if self._cancel.wait(.05):
                    return
                if end is not None and time.monotonic() >= end:
                    with self._lock:
                        self._reason = "supervisor_timeout"
                    self.stop()
                    return
                if time.monotonic() >= next_quota:
                    self._check_outputs()
                    next_quota = time.monotonic() + 1
                if self._control is None and (self.run / "control.json").exists():
                    try:
                        value = self._read_control()
                    except (FileNotFoundError, MError) as error:
                        if isinstance(error, MError) and error.code != "invalid_json":
                            raise
                        # Upstream write_text does not publish atomically. Allow
                        # a short bounded partial-write interval, never forever.
                        publication_deadline = publication_deadline or time.monotonic() + .5
                        if time.monotonic() > publication_deadline:
                            raise MError("invalid_control", "Runner evidence did not finish publication") from error
                        continue
                    with self._lock:
                        if self.native_broker is None and not self.fixture_only:
                            self._developer_process_identity = {
                                "runnerPID": self._runner_pid(), "corePID": value["pid"]}
                        self._control = value
                        if not self._stop_requested:
                            self._state = "running"
            with self._lock:
                self._finish()
        except (OSError, ValueError, KeyError, TypeError) as error:
            traceback.print_exc()
            with self._lock:
                self._reason = (error.code if isinstance(error, MError) else
                                "runner_internal_error" if isinstance(error, (KeyError, TypeError)) else "runner_io_error")
            self.stop()

    def _check_outputs(self):
        """One bounded pass per second; supervisor also enforces RLIMIT_FSIZE."""
        count = total = 0
        pending = [self.run]
        while pending:
            folder = pending.pop()
            fd = _directory_fd(folder, private=True)
            try:
                # scandir is incremental: an overpopulated directory must not
                # first allocate an unbounded names list as os.walk would.
                with os.scandir(fd) as entries:
                    for entry in entries:
                        count += 1
                        if count > 2048:
                            raise MError("output_limit", "Run output exceeds 2048 entries")
                        path = folder / entry.name
                        try:
                            info = entry.stat(follow_symlinks=False)
                        except FileNotFoundError:
                            continue
                        if stat.S_ISLNK(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
                            raise MError("unsafe_output", "Run output must remain private and free of symbolic links")
                        if stat.S_ISREG(info.st_mode):
                            if info.st_nlink != 1 or info.st_size > MAX_OUTPUT_FILE:
                                raise MError("output_limit", "A run output exceeds the 512 MiB file limit")
                            total += info.st_size
                            if total > MAX_OUTPUT_TOTAL:
                                raise MError("output_limit", "Run output exceeds 1 GiB")
                        elif stat.S_ISDIR(info.st_mode):
                            pending.append(path)
                        elif not (stat.S_ISSOCK(info.st_mode) and (self.run / "tmp") in path.parents):
                            raise MError("unsafe_output", "Unexpected output node type")
            finally:
                os.close(fd)

    def _send_input(self, x, y, down, timeout):
        if type(x) is not int or type(y) is not int or type(down) is not bool or not (0 <= x < self.plan["width"] and 0 <= y < self.plan["height"]):
            raise MError("invalid_input", "Input must use integer coordinates inside this profile")
        if self.process is None or self.process.poll() is not None or self._control is None:
            raise MError("not_running", "Runner input is not ready")
        control = self._read_control()
        path = Path(control["input_socket"])
        before = path.lstat()
        runner_pid = self._runner_pid()
        deadline = time.monotonic() + timeout
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
            connection.settimeout(max(.001, deadline - time.monotonic()))
            connection.connect(str(path))
            if _peer_identity(connection) != (runner_pid, os.getuid()):
                raise MError("peer_identity", "Input peer is not this owned runner")
            after = path.lstat()
            if (before.st_dev, before.st_ino) != (after.st_dev, after.st_ino):
                raise MError("peer_identity", "Input socket changed during connection")
            connection.settimeout(max(.001, deadline - time.monotonic()))
            if down:
                # An ACK can be lost after the guest received a press. Retain
                # the release obligation before sending, not after the reply.
                self._held, self._point = True, (x, y)
            connection.sendall(_canonical({"x": x, "y": y, "down": down}) + b"\n")
            data = bytearray()
            while b"\n" not in data:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise MError("input_timeout", "Runner input exceeded its total deadline")
                connection.settimeout(remaining)
                part = connection.recv(257 - len(data))
                if not part:
                    raise MError("input_protocol", "Runner closed without an input acknowledgement")
                data.extend(part)
                if len(data) > 256:
                    raise MError("input_protocol", "Runner input reply exceeds 256 bytes")
            if bytes(data).count(b"\n") != 1 or not data.endswith(b"\n"):
                raise MError("input_protocol", "Runner sent an invalid input reply")
            response = _decode(bytes(data[:-1]))
            if response != {"return": {}}:
                raise MError("input_rejected", "Runner rejected the input event")
        self._held, self._point = down, (x, y)
        return {"accepted": True, "guestAcknowledged": False}

    def input(self, x, y, down):
        with self._lock:
            if self._stop_requested:
                raise MError("not_running", "Session is stopping")
            try:
                return self._send_input(x, y, down, .5)
            except socket.timeout as error:
                raise MError("input_timeout", "Runner input exceeded its total deadline") from error

    def read_frame(self, previous_header=None):
        """Return stable header/pixels, or None for a validated unchanged header.

        No-argument callers retain the complete snapshot contract. The fast
        path still checks the current control identity, private file, exact
        geometry, format and capacity before skipping the pixel read.
        """
        with self._lock:
            if self.process is None or self.process.poll() is not None or self._control is None:
                raise MError("not_running", "Runner frame is not ready")
            control = self._read_control()
            width, height = self.plan["width"], self.plan["height"]
            exact = FRAME_HEADER.size + width * height * 2
            # The multi-device core allocates one fixed-capacity buffer (2 MiB +
            # header) for every geometry; only the leading exact frame is read.
            try:
                fd, info = _open_file(control["framebuffer"], MAX_FRAME_FILE, private=True)
            except FileNotFoundError as error:
                raise MError("frame_unavailable", "Waiting for the first shared frame file") from error
            try:
                header = os.pread(fd, 32, 0)
                if len(header) != 32:
                    raise MError("frame_unavailable", "Shared frame header is not ready")
                magic, version, sequence, w, h, stride, fmt = FRAME_HEADER.unpack(header)
                # Before its first frame the core publishes a zeroed header.
                if magic == b"Q66FRAME" and sequence == 0:
                    raise MError("frame_unavailable", "No frame has been published yet")
                if magic != b"Q66FRAME" or version != 1 or (w, h) != (width, height) or stride != width * 2 or fmt != 1:
                    raise MError("invalid_frame", "Frame geometry/format differs from selected profile")
                if sequence == 0 or sequence % 2:
                    raise MError("frame_unavailable", "Shared frame is being updated")
                if info.st_size < exact:
                    raise MError("invalid_frame", "Shared frame is shorter than its exact geometry")
                if header == previous_header:
                    if os.pread(fd, 32, 0) != header:
                        raise MError("frame_unavailable", "Shared frame changed during header check")
                    return None
                pixels = os.pread(fd, stride * height, 32)
                if len(pixels) != stride * height or os.pread(fd, 32, 0) != header:
                    raise MError("frame_unavailable", "Shared frame changed during copy")
                return header, pixels
            finally:
                os.close(fd)

    def _finish(self):
        if self._finished or self.process is None or self.process.poll() is None:
            return
        self._reap_group()
        result = None
        try:
            result = _json(self.run / "result.json", private=True)
        except FileNotFoundError:
            pass
        except (OSError, ValueError):
            self._reason = self._reason or "invalid_result"
        outcome = classify_result(result, self.process.returncode, stopped=self._stop_requested)
        self._state = "failed" if self._reason else outcome["state"]
        self._reason = self._reason or outcome["reason"]
        if self.native_broker is None and self.plan["identity"].get("instanceStorage"):
            try:
                from .developer_storage import publish_completion
                publish_completion(self)
            except (OSError, ValueError):
                self._state = "failed"
                self._reason = "developer_storage_completion_unconfirmed"
        if self._state == "failed":
            self._detail = _failure_detail(result, self.run / "adapter-runner.log")
        if self._log:
            self._log.close()
            self._log = None
        self._control = None
        self._held = False
        self._finished = True

    def _reap_group(self):
        """Bounded group cleanup also applies after a leader exits by itself."""
        if self.native_broker is not None:
            if self.process.confirm_cleanup():
                if not self._cleanup:
                    self._cleanup = "reaped"
            else:
                self._cleanup = "group_exit_unconfirmed"
                self._reason = self._reason or "native_cleanup_unconfirmed"
                self._state = "failed"
            return
        if not _group_members(self.process.pid):
            if not self._cleanup:
                self._cleanup = "reaped"
            return
        for sig, grace in ((signal.SIGTERM, .5), (signal.SIGKILL, .5)):
            try:
                os.killpg(self.process.pid, sig)
            except ProcessLookupError:
                break
            limit = time.monotonic() + grace
            while time.monotonic() < limit:
                self.process.poll()  # reap the direct child as soon as possible
                if not _group_members(self.process.pid):
                    break
                time.sleep(.025)
            else:
                continue
            break
        try:
            self.process.wait(timeout=.5)
        except subprocess.TimeoutExpired:
            self._reason = "cleanup_timeout"
        if not _group_members(self.process.pid):
            if not self._cleanup:
                self._cleanup = "reaped"
        else:
            self._cleanup = "group_exit_unconfirmed"
            self._reason = self._reason or "cleanup_incomplete"

    def stop(self):
        with self._lock:
            if self.process is None:
                self._state = "stopped"
                return self.status()
            self._stop_requested = True
            if self._held:
                try:
                    self._send_input(*self._point, False, .25)
                except (OSError, ValueError):
                    self._cleanup = "touch_release_unconfirmed"
            self._cancel.set()
            if self.native_broker is not None:
                # Native app owns the sibling group. Never signal/reap its PID.
                self._reap_group()
                self._finish()
                return self.status()
            # SIGINT lets the runner perform QMP quit and emit result.json. A
            # trusted prefix supervisor must forward/catch this bounded stop.
            if self.process.poll() is None:
                try:
                    self.process.send_signal(signal.SIGINT)
                    self.process.wait(timeout=9)
                except subprocess.TimeoutExpired:
                    pass
                except ProcessLookupError:
                    pass
            self._reap_group()
            self._finish()
            return self.status()

    def status(self):
        from .virtual_peer import supports_peer
        with self._lock:
            if self.process is not None and self.process.poll() is not None:
                self._finish()
            return {"backend": "minative", "profileID": self.plan["profileID"],
                    "width": self.plan["width"], "height": self.plan["height"],
                    "state": self._state, "reason": self._reason,
                    "pid": self.process.pid if self.process is not None else None,
                    "lastExit": self.process.returncode if self.process is not None else None,
                    "error": self._reason if self._state == "failed" else None,
                    "detail": self._detail,
                    "runDirectory": str(self.run), "cleanup": self._cleanup,
                    "approvalDigest": self.plan["approvalDigest"], "fixtureOnly": self.fixture_only,
                    "guestInstallSupported": supports_peer(self.plan), "desktopVerified": False,
                    "identityMode": self.plan["identity"].get("mode", "prepared"),
                    "observedBoard": self._observed_board,
                    "debugger": (self._control or {}).get("gdb"),
                    "maxRunSeconds": self.seconds}
