"""Native Windows firmware controller, exposed as bounded JSONL over stdio.

The Windows UI and automation use the same controller. Firmware runs in the
existing QEMU board implementations; display/input are genuine shared frames
and QMP hardware inputs. There is no synthetic firmware screen or boot claim.
"""
from __future__ import annotations

import argparse
import ast
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time
import uuid
import zipfile


class RuntimeFailure(RuntimeError):
    def __init__(self, code, message):
        self.code = code
        super().__init__(message)


def _sha(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def _qemu_read_path(path):
    # Only already-imported private read-only images reach this helper.
    # QEMU/GLib's Windows executable need not be longPathAware: use an
    # extended-length Win32 spelling after the runtime has validated the
    # original local-drive path. Writable stores retain their native validator.
    value = str(Path(path).absolute())
    return "\\\\?\\" + value if os.name == "nt" else value


def _unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Duplicate JSON field")
        result[key] = value
    return result


def _literal_tuple(path, name):
    for node in ast.parse(Path(path).read_text(encoding="utf-8")).body:
        if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == name for t in node.targets):
            value = ast.literal_eval(node.value)
            if isinstance(value, tuple) and all(isinstance(x, str) for x in value):
                return value
    raise RuntimeFailure("runtime_changed", f"Missing launch flag tuple {name}")


class WindowsRuntime:
    def __init__(self, project_root, qemu, workspace):
        self.project = Path(project_root).absolute()
        self.qemu = Path(qemu).absolute()
        self.workspace = Path(workspace).absolute()
        self.lock = threading.RLock()
        self.files = self.prepared = self.process = self.job = self.qmp = self.reader = None
        self.bluetooth_service = self.guest_peer = self.guest_state = None
        self.hardware = None
        self.persistent = None
        self.stop_result = None
        self.stderr_file = None
        self.last_prepare_args = None

    def _catalog(self):
        path = self.project.parent / "catalog.json"
        if not path.is_file():
            raise RuntimeFailure("catalog_missing", "The Windows package has no firmware metadata catalog")
        catalog = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(catalog, dict) or not isinstance(catalog.get("profiles"), list):
            raise RuntimeFailure("catalog_invalid", "Invalid firmware metadata catalog")
        return catalog

    def profiles(self, args=None):
        rows, hardware = [], {}
        for profile in self._catalog()["profiles"]:
            row = {"id": profile["id"], "name": profile.get("displayName", profile["id"]),
                   "model": profile["model"], "version": profile["version"],
                   "width": profile["width"], "height": profile["height"], "importOnly": False}
            rows.append(row)
            hardware.setdefault(row["model"], {**row, "id": "hardware:" + row["model"],
                                               "version": "import", "importOnly": True,
                                               "name": row["name"].split(" · ")[0] + " — imported firmware"})
        for model, name, width, height in (
                ("miwear.watch.p65", "红米手表6", 432, 514),
                ("miwear.watch.q65", "红米手表7", 432, 514),
                ("miwear.watch.q66ys", "小米手环11 原神定制版", 212, 520)):
            hardware.setdefault(model, {"id": "hardware:" + model, "name": name + " — imported firmware",
                                        "model": model, "version": "import", "width": width,
                                        "height": height, "importOnly": True})
        return {"profiles": rows + list(hardware.values()), "catalogProfiles": len(rows),
                "hardwarePresets": len(hardware), "firmwareIncluded": False}

    def _workspace(self):
        from .windows_runtime_files import WindowsFiles
        self.files = self.files or WindowsFiles()
        self.files.initialize_private_creator()
        self.files.private_directory(self.workspace, exist_ok=True)
        marker = self.workspace / ".azki-windows-runtime-owner.json"
        expected = {"schema": "azki.windows.runtime.v1", "ownerSID": self.files.sid}
        if marker.exists():
            handle = self.files.open_read(marker)
            self.files.close(handle)
            if json.loads(marker.read_text(encoding="utf-8")) != expected:
                raise RuntimeFailure("workspace_owner", "Workspace has another runtime owner marker")
        else:
            with marker.open("x", encoding="utf-8") as stream:
                json.dump(expected, stream)

    def _copy_input(self, source, target, limit):
        import msvcrt
        # Source inputs may have normal public read ACLs, but are opened with
        # no write/delete sharing, no reparse resolution, regular-file checks,
        # stable handle identity and a bounded exact copy. Never reopen by name.
        handle = self.files.open_read(source, require_private=False, share_write=False)
        fd = None
        try:
            identity = self.files.information(handle)
            if not 0 < identity[2] <= limit:
                raise RuntimeFailure("input_size", "Firmware input exceeds the bounded image limit")
            fd = msvcrt.open_osfhandle(handle, os.O_RDONLY | os.O_BINARY)
            native_handle = handle
            handle = None
            digest = hashlib.sha256()
            copied = 0
            with os.fdopen(fd, "rb") as incoming, target.open("xb") as outgoing:
                fd = None
                before = os.fstat(incoming.fileno())
                while block := incoming.read(4 * 1024 * 1024):
                    copied += len(block)
                    if copied > limit:
                        raise RuntimeFailure("input_changed", "Firmware input grew while copying")
                    outgoing.write(block)
                    digest.update(block)
                after = os.fstat(incoming.fileno())
                if (copied != identity[2] or self.files.information(native_handle) != identity or
                        before.st_mtime_ns != after.st_mtime_ns):
                    raise RuntimeFailure("input_changed", "Firmware input changed during its stable-handle copy")
            return digest.hexdigest()
        finally:
            if fd is not None:
                os.close(fd)
            self.files.close(handle)

    def _persistent(self):
        if self.persistent is None:
            from .windows_instance_runtime import PersistentInstances
            self.persistent = PersistentInstances(self)
        return self.persistent

    def instances(self, args):
        return self._persistent().actions(args)

    def prepare(self, args):
        if args.get("instanceID") is not None:
            if self.process is not None and self.process.poll() is None:
                raise RuntimeFailure("instance_running", "Stop the owned instance before preparing another firmware")
            self.stop({})
            self.process = self.prepared = None
            self.stop_result = None
            return self._persistent().prepare(args)
        return self._prepare_session(args)

    def _prepare_session(self, args, *, context=None):
        if self.process is not None and self.process.poll() is None:
            raise RuntimeFailure("instance_running", "Stop the owned instance before preparing another firmware")
        if context is None:
            self.stop({})
        self.process = None
        self.prepared = None
        self.stop_result = None
        if not self.qemu.is_file() or self.qemu.suffix.lower() != ".exe":
            raise RuntimeFailure("core_missing", "Select the built Windows qemu-system-arm.exe")
        profile_id = args.get("profileID")
        if type(profile_id) is not str or not 0 < len(profile_id) <= 512:
            raise ValueError("profileID is required")
        if type(args.get("firmwarePath")) is not str or not args["firmwarePath"]:
            raise ValueError("firmwarePath is required")
        if type(args.get("skipOOBE", True)) is not bool:
            raise ValueError("skipOOBE must be a boolean")
        chosen = next((p for p in self.profiles()["profiles"] if p["id"] == profile_id), None)
        if chosen is None:
            raise RuntimeFailure("unknown_profile", "Select an exact catalog version or hardware preset")
        mode = args.get("bluetooth", "reject")
        if mode not in ("off", "reject", "winrt", "usb", "serial"):
            raise ValueError("bluetooth must be reject, winrt, usb or serial")
        if mode == "serial":
            raise RuntimeFailure("serial_hci_unavailable", "A serial HCI backend has not been validated for this controller")
        self._workspace()
        if context and context.get("session"):
            session = self.files.local_path(context["session"])
            if session.parent != self.workspace or not session.name.startswith("session-"):
                raise ValueError("Persistent work requires a private owned session")
            handle = self.files.open_read(session, directory=True)
            self.files.close(handle)
        else:
            session = self.files.private_directory(self.workspace / ("session-" + uuid.uuid4().hex))
        imports = self.files.private_directory(session / "inputs")
        run = self.files.private_directory(session / "run")
        supplied = imports / "firmware.bin"
        supplied_sha = self._copy_input(args["firmwarePath"], supplied, 768 * 1024**2)
        sys.path.insert(0, str(self.project)) if str(self.project) not in sys.path else None
        import device_catalog
        from multi_device import board_profile
        metadata = self._catalog()
        exact = next((p for p in metadata["profiles"] if p["id"] == profile_id), None)
        recipe = copy.deepcopy(exact["recipe"]) if exact else {"model": chosen["model"], "version": "import"}
        recipe.update(id=profile_id, model=chosen["model"], version=chosen["version"], resource_path=None)
        with supplied.open("rb") as incoming:
            envelope = incoming.read(4)
        if envelope == b"\x60ZZ~":
            # N67 is an envelope of separately indexed ZIP members, so
            # ZipFile on the whole object only exposes its final member.
            # Use the exact catalog's original offset/size/CRC/SHA inventory.
            if (exact is None or not chosen["model"].startswith("miwear.watch.n67") or
                    supplied_sha != exact["recipe"].get("ota_sha256")):
                raise RuntimeFailure("ota_inventory_required", "N67 envelope requires its exact catalog OTA and partition inventory")
            from firmware_resources import checked_member
            records = [row for row in recipe["partitions"] if row["name"] == "vela_ap.bin"]
            if len(records) != 1 or not 0 < records[0]["size"] <= 64 * 1024**2:
                raise RuntimeFailure("ota_members", "N67 inventory requires one bounded AP member")
            ap = imports / "vela_ap.bin"
            ap.write_bytes(checked_member(supplied.read_bytes(), records[0]))
            recipe.update(ota_path=str(supplied), ota_sha256=supplied_sha)
        elif zipfile.is_zipfile(supplied):
            with zipfile.ZipFile(supplied) as archive:
                names = archive.namelist()
                if len(names) != len(set(names)) or names.count("vela_ap.bin") != 1:
                    raise RuntimeFailure("ota_members", "OTA requires one unambiguous vela_ap.bin")
                ap_info = archive.getinfo("vela_ap.bin")
                if not 0 < ap_info.file_size <= 64 * 1024**2:
                    raise RuntimeFailure("ota_members", "AP exceeds supported image size")
                ap = imports / "vela_ap.bin"
                with archive.open(ap_info) as incoming, ap.open("xb") as outgoing:
                    while block := incoming.read(4 * 1024 * 1024):
                        outgoing.write(block)
            recipe.update(ota_path=str(supplied), ota_sha256=supplied_sha,
                          partitions=device_catalog.zip_member_records(supplied))
        else:
            ap = supplied
            recipe.update(ota_path=None, ota_sha256=None, partitions=copy.deepcopy(recipe.get("partitions", [])))
        recipe["ap_path"] = str(ap)
        recipe["ap"] = device_catalog.inspect_ap(ap)
        if exact and recipe["ap"]["sha256"] != exact["recipe"]["ap"]["sha256"]:
            raise RuntimeFailure("version_mismatch", "Firmware AP differs from the selected catalog version; use its hardware import preset")
        if exact is None:
            # A UI hardware preset is not a firmware identity. Bind imported
            # state to the model and exact supplied bytes, using the existing
            # import-version convention; never pass 'hardware:...' to pairing.
            recipe["id"] = f"{chosen['model']}@0@{supplied_sha[:12]}"
            recipe["version"] = "0"
            if chosen["model"] == "q66":
                known = [row for row in metadata["profiles"] if row["model"] == "q66" and
                         row["recipe"]["ap"]["sha256"] == recipe["ap"]["sha256"]]
                if known:
                    recipe["id"] = known[0]["id"]
                    recipe["version"] = known[0]["version"]
        if args.get("resourcePath"):
            resource = imports / "vela_resource.bin"
            self._copy_input(args["resourcePath"], resource, 512 * 1024**2)
            recipe["resource_path"] = str(resource)
        cache = self.files.private_directory(session / "storage-cache")
        old_cache = device_catalog.CACHE
        data_image = None
        try:
            device_catalog.CACHE = cache
            if chosen["model"] == "miwear.watch.q65":
                from .q65_runner import board_profile as q65_board
                board = q65_board(recipe)
                resource, _, storage_kind = device_catalog.prepare_storage(recipe)
            else:
                resource, _, storage_kind = device_catalog.prepare_storage(recipe)
                board = board_profile(recipe, resource.stat().st_size, storage_kind)
            if storage_kind == "yaffs" and args.get("skipOOBE", True) and not (context and context.get("restoring")):
                import legacy_ui_seed
                legacy_ui_seed.CACHE = cache
                data_image, _, _ = legacy_ui_seed.prepare_ui_seed(recipe, board["geometry"]["nand-data-bytes"])
        finally:
            device_catalog.CACHE = old_cache
        images = {"resource-image": str(resource)}
        for key, option, names in (
                ("audioPath", "audio-image", ("vela_audio_img.bin",)),
                ("sensorPath", "sensor-image", ("vela_sensor_img.bin", "vela_sensor.bin"))):
            if args.get(key):
                image = imports / names[0]
                self._copy_input(args[key], image, 16 * 1024**2)
                images[option] = str(image)
            elif option == "audio-image" and recipe.get("ota_path") and chosen["model"] == "miwear.watch.p65":
                # Latest P65 uses the original BES CP image in NOR, not the
                # Q65 NAND ROMFS convention. Reuse its authoritative extractor.
                from multi_device import extract_p65_audio
                images[option] = str(extract_p65_audio(supplied.read_bytes(), recipe["partitions"], imports))
            elif recipe.get("ota_path") and chosen["model"] in ("miwear.watch.q65", "miwear.watch.p65"):
                with zipfile.ZipFile(supplied) as archive:
                    available = [name for name in names if name in archive.namelist()]
                    if len(available) == 1:
                        entry = archive.getinfo(available[0])
                        if not 0 < entry.file_size <= 16 * 1024**2:
                            raise RuntimeFailure("ota_members", "Subprocessor image exceeds its bound")
                        image = imports / available[0]
                        with image.open("xb") as stream:
                            stream.write(archive.read(entry))
                        images[option] = str(image)
        if chosen["model"] in ("miwear.watch.q65", "miwear.watch.p65") and "sensor-image" not in images:
            raise RuntimeFailure("sensor_image_missing", "Selected watch needs its own sensor image")
        skip = args.get("skipOOBE", True)
        factory = None
        if skip and board.get("local_setup_bypass") and chosen["model"] != "miwear.watch.p65" and mode != "winrt" and not (context and board.get("nor_geometry")):
            if chosen["model"] == "miwear.watch.q65":
                from .q65_factory import local_desktop_image
                factory_data = local_desktop_image()
            else:
                from .factory import local_desktop_image
                factory_data = local_desktop_image(property_file=board.get("local_property_file", "factory.prop"))
            factory = imports / "desktop-factory.bin"
            factory.write_bytes(factory_data)
        want_identity = mode == "winrt" or bool(context and context.get("wantIdentity") and board.get("nor_geometry"))
        if chosen["model"] == "miwear.watch.p65":
            if mode == "winrt":
                raise RuntimeFailure("factory_identity_unsupported", "P65 raw factory identity format is not verified in the latest engine; its original AP and saved storage were preserved")
            want_identity = False
        if want_identity:
            from .windows_guest_state import create_windows_guest_state
            state_root = session / "guest-state"
            if context and context.get("restoring"):
                state_root = Path(context["work"]["files"]["guest-state/guest-state.json"]["path"]).parent
            try:
                self.guest_state = create_windows_guest_state(recipe, board, state_root,
                    instance_id=context.get("instanceID") if context else None,
                    skip_oobe=skip, api=self.files)
            except ValueError as error:
                if mode == "winrt":
                    raise RuntimeFailure("factory_identity_unsupported", "No verified factory/NOR identity seed for this exact firmware: " + str(error)) from error
                # Ordinary offline persistence can still retain NAND/NOR;
                # never invent an unsupported virtual factory identity.
                self.guest_state = None
            if self.guest_state is not None and self.guest_state.requires_boot_property_overlay and not (chosen["model"] == "miwear.watch.p65" and skip):
                self.guest_state.close()
                self.guest_state = None
                raise RuntimeFailure("identity_overlay_unavailable", "P65 WinRT identity currently requires the local setup boot copy; enable skipOOBE or use offline mode with the original AP")
        boot_copy = boot_receipt = None
        if chosen["model"] == "miwear.watch.p65" and skip:
            from .windows_boot_copy import write_windows_boot_copy
            boot_copy = imports / "p65-local-setup-ap.bin"
            try:
                boot_receipt = write_windows_boot_copy(ap, boot_copy,
                    expected_source_sha256=recipe["ap"]["sha256"], model=chosen["model"],
                    skip_oobe=True,
                    identity_properties=self.guest_state.boot_properties if self.guest_state else None,
                    api=self.files)
            except BaseException:
                if self.guest_state is not None:
                    self.guest_state.close()
                    self.guest_state = None
                raise
        prepared = {"profile": chosen, "recipe": recipe, "board": board, "session": session,
                    "run": run, "ap": ap, "images": images, "dataImage": data_image,
                    "factory": factory, "skipOOBE": skip, "sourceSHA256": supplied_sha,
                    "coreSHA256": _sha(self.qemu), "bluetooth": mode, "hciDevicePath": args.get("hciDevicePath"),
                    "bootCopy": boot_copy, "bootCopyReceipt": boot_receipt,
                    "firmwareModified": bool(boot_receipt and boot_receipt["bootCopyModified"])}
        self.prepared = prepared
        self.last_prepare_args = copy.deepcopy(args)
        if not (context and context.get("deferReceipt")):
            self._write_prepared_receipt()
        return self._prepared_result()

    def _write_prepared_receipt(self):
        p = self.prepared
        self._write_session("prepared.json", {"profileID": p["profile"]["id"],
            "firmwareSHA256": p["sourceSHA256"], "executionProfileID": p["recipe"]["id"],
            "AP_SHA256": p["recipe"]["ap"]["sha256"], "coreSHA256": p["coreSHA256"],
            "board": p["board"], "firmwareModified": p["firmwareModified"],
            "bootCopy": p["bootCopyReceipt"], "storageMode": p.get("storageMode", "volatile-guest-working-state"),
            "instanceID": p.get("instanceID"), "instanceBinding": p.get("instanceBinding")})

    def _prepared_result(self):
        p = self.prepared
        return {"prepared": True, "profile": p["profile"], "session": str(p["session"]),
                "executionProfileID": p["recipe"]["id"], "firmwareSHA256": p["sourceSHA256"],
                "AP_SHA256": p["recipe"]["ap"]["sha256"], "firmwareModified": p["firmwareModified"],
                "bootCopy": p["bootCopyReceipt"], "storageMode": p.get("storageMode", "volatile-guest-working-state")}

    def _write_session(self, name, value):
        with (self.prepared["session"] / name).open("x", encoding="utf-8") as stream:
            json.dump(value, stream, indent=2, ensure_ascii=False)
            stream.write("\n")

    def _command(self):
        p = self.prepared
        board, run = p["board"], p["run"]
        machine = board.get("machine", "best1503")
        if machine == "best1503":
            machine += "".join(f",{key}={value}" for key, value in board["geometry"].items())
        machine += ",revision-id=5"
        flag_file = self.project / "backend/src" / ("q65_runner.py" if board.get("machine") == "azki-q65" else "runner.py")
        for flag in _literal_tuple(flag_file, "FLAGS" if board.get("machine") == "azki-q65" else "WATCH_FLAGS"):
            option = flag.replace("_", "-")
            if p["bluetooth"] in ("winrt", "usb") and option in ("bt-hci-reset-fixture", "bt-hci-reject-fixture"):
                continue
            machine += "," + option + "=on"
        if p["bluetooth"] in ("winrt", "usb"):
            machine += ",bt-hci-chardev=radio"
        if self.guest_state is not None:
            machine += ",nor-image=" + str(self.guest_state.nor_path).replace(",", ",,") + ",nor-writeback=on"
        saved_files = p.get("instanceFiles", {})
        if "nand-data.bin" in saved_files:
            machine += ",data-image=" + saved_files["nand-data.bin"]["path"].replace(",", ",,")
            machine += ",data-spare-image=" + saved_files["nand-spare.bin"]["path"].replace(",", ",,") + ",data-writeback=on"
        if "nor-config.bin" in saved_files:
            machine += ",nor-image=" + saved_files["nor-config.bin"]["path"].replace(",", ",,") + ",nor-writeback=on"
        for key, value in p["images"].items():
            machine += "," + key + "=" + _qemu_read_path(value).replace(",", ",,")
        if p["factory"]:
            machine += ",factory-image=" + _qemu_read_path(p["factory"]).replace(",", ",,")
        if p["dataImage"]:
            machine += ",data-image=" + _qemu_read_path(p["dataImage"]).replace(",", ",,")
        if board.get("machine", "best1503") == "best1503":
            from .windows_hardware import model_profile
            from .instance_data import instance_settings_options
            hardware = model_profile(self.project, p["profile"]["model"])
            if hardware["sensors"]:
                for option in instance_settings_options({"health": {
                        "sensorBoard": hardware["sensors"]["board"],
                        "cadence": 110, "heartRate": 72, "worn": True}},
                        adc_fixture=True):
                    machine += "," + option
        machine += ",lcd-frame-file=" + str(run / "lcd-frame.raw").replace(",", ",,")
        command = [str(self.qemu), "-accel", "tcg,thread=multi", "-M", machine,
                   "-display", "none", "-monitor", "none", "-nic", "none", "-no-reboot",
                   "-kernel", str(p.get("bootCopy") or p["ap"]), "-qmp", self.qmp.qemu_argument,
                   "-d", "guest_errors", "-D", str(run / "qemu.log")]
        if self.guest_peer is not None:
            command += ["-chardev", self.guest_peer.chardev_argument]
        elif p.get("rawHCIDescriptor") is not None:
            command += ["-chardev", p["rawHCIDescriptor"]["qemuChardev"]]
        if board.get("machine") == "azki-q65":
            command += ["-smp", "2"]
        for index in range(3):
            command += ["-serial", "file:" + str(run / f"uart{index}.log")]
        return command

    def start(self, args):
        if self.process is not None and self.process.poll() is None:
            raise RuntimeFailure("instance_running", "This runtime already owns a running instance")
        if self.process is not None or (self.prepared is not None and self.prepared.get("resourcesClosed")):
            self.prepare(args if args.get("profileID") or args.get("instanceID") else self.last_prepare_args or args)
        elif self.prepared is None:
            self.prepare(args if args.get("profileID") or args.get("instanceID") else self.last_prepare_args or args)
        if args.get("instanceID") is not None and args["instanceID"] != self.prepared.get("instanceID"):
            raise RuntimeFailure("prepared_instance_mismatch", "Prepare the selected instance before starting it")
        if "bluetooth" in args and args["bluetooth"] != self.prepared["bluetooth"]:
            raise RuntimeFailure("prepared_transport_mismatch", "Prepare the selected Bluetooth transport before starting it")
        if _sha(self.qemu) != self.prepared["coreSHA256"]:
            raise RuntimeFailure("core_changed", "QEMU changed after preparation; prepare again before execution")
        if _sha(self.prepared["ap"]) != self.prepared["recipe"]["ap"]["sha256"]:
            raise RuntimeFailure("firmware_changed", "Prepared AP changed before execution")
        if self.prepared.get("bootCopy") and _sha(self.prepared["bootCopy"]) != self.prepared["bootCopyReceipt"]["bootCopySHA"]:
            raise RuntimeFailure("boot_copy_changed", "Prepared P65 boot copy changed before execution")
        try:
            return self._start_prepared(args)
        except BaseException:
            self.stop({})
            raise

    def _start_prepared(self, args):
        from .windows_qmp import WindowsQMP
        from .windows_frame import WindowsFrameReader
        from .windows_runtime_files import OwnedWindowsJob
        if self.prepared["bluetooth"] == "winrt":
            if self.bluetooth_service is not None:
                # A pre-start capabilities query has no guest callback yet.
                self.bluetooth_service.close()
                self.bluetooth_service = None
            self.guest_peer = self.guest_state.new_peer(run_directory=self.prepared["run"]).start()
        elif self.prepared["bluetooth"] == "usb":
            self._bluetooth_service()
            device_path = args.get("hciDevicePath") or self.prepared.get("hciDevicePath")
            if not device_path:
                raise RuntimeFailure("hci_device_required", "Select the explicitly owned WinUSB controller interface before starting USB HCI")
            self.prepared["rawHCIDescriptor"] = self.bluetooth_service.prepare_raw_hci(device_path, reset_on_stop=True)
            expected_controller = self.prepared.get("instanceBinding", {}).get("controllerInstanceID")
            actual_controller = self.prepared["rawHCIDescriptor"].get("controllerInstanceID")
            if expected_controller and str(actual_controller).casefold() != expected_controller.casefold():
                raise RuntimeFailure("instance_controller_mismatch", "The opened controller differs from this instance's saved controller identity")
        self.job = OwnedWindowsJob(self.files)
        self.qmp = WindowsQMP(api=self.files)
        command = self._command()
        self._write_session("launch.json", {"command": command, "coreSHA256": self.prepared["coreSHA256"],
                                            "firmwareModified": self.prepared.get("firmwareModified", False),
                                            "bootCopy": self.prepared.get("bootCopyReceipt")})
        self.stderr_file = (self.prepared["run"] / "stderr.log").open("xb")
        try:
            environment = os.environ.copy()
            environment["AZKI_QEMU_PIPE_CLIENT_PID"] = str(os.getpid())
            if self.guest_peer is not None:
                environment.update(self.guest_peer.required_environment)
            if self.prepared.get("rawHCIDescriptor"):
                environment.update(self.prepared["rawHCIDescriptor"].get("qemuEnvironment", {}))
            self.process = subprocess.Popen(command, stdin=subprocess.DEVNULL,
                                            stdout=self.stderr_file, stderr=self.stderr_file, env=environment,
                                            creationflags=subprocess.CREATE_NO_WINDOW | subprocess.CREATE_NEW_PROCESS_GROUP)
            self._write_session("owned-child.json", {"pid": self.process.pid, "command": command,
                                                     "qmp": "private-current-user-named-pipe",
                                                     "jobAttachmentPending": True})
            try:
                self.job.attach(self.process)
                self._write_session("job-attached.json", {"pid": self.process.pid, "jobKillOnClose": True})
            except BaseException:
                self.process.terminate()  # Exact just-created owned child, never a PID search.
                self.process.wait(timeout=5)
                raise
            if self.guest_peer is not None:
                self.guest_peer.attach_core(self.process)
            elif self.prepared.get("rawHCIDescriptor"):
                self.bluetooth_service.attach_core(self.process)
            self.qmp.accept(self.process)
            if self.guest_peer is not None:
                self.guest_peer.wait_ready(15)
            profile = self.prepared["profile"]
            self.reader = WindowsFrameReader(self.prepared["run"] / "lcd-frame.raw",
                                             expected=(profile["width"], profile["height"]), api=self.files)
            return {"running": True, "pid": self.process.pid, "session": str(self.prepared["session"]),
                    "firmwareModified": self.prepared.get("firmwareModified", False),
                    "bootCopy": self.prepared.get("bootCopyReceipt"), "bootVerified": False}
        except BaseException:
            self.stop({})
            raise

    def status(self, args=None):
        running = self.process is not None and self.process.poll() is None
        if self.process is not None and not running and not (self.prepared and self.prepared.get("resourcesClosed")):
            self.stop({})
        state = {"running": running, "prepared": self.prepared is not None,
                 "pid": self.process.pid if self.process is not None else None,
                 "exitCode": self.process.poll() if self.process is not None else None,
                 "session": str(self.prepared["session"]) if self.prepared else None,
                 "firmwareModified": bool(self.prepared and self.prepared.get("firmwareModified")), "bootVerified": False}
        if self.prepared and self.prepared.get("instanceID"):
            state["instanceID"] = self.prepared["instanceID"]
        if self.stop_result is not None:
            state["stopResult"] = self.stop_result
        if running and self.qmp is not None:
            state["core"] = self.qmp.execute("query-status")
        return state

    def frame(self, args=None):
        self._running()
        try:
            return self.reader.snapshot()
        except FileNotFoundError:
            return {"ready": False}

    def _running(self):
        if self.process is None or self.process.poll() is not None or self.qmp is None:
            raise RuntimeFailure("not_running", "Start a prepared firmware instance first")

    def touch(self, args):
        self._running()
        if set(args) != {"x", "y", "down"} or type(args["x"]) is not int or type(args["y"]) is not int or type(args["down"]) is not bool:
            raise ValueError("touch requires integer x/y and boolean down")
        profile = self.prepared["profile"]
        width, height = profile["width"], profile["height"]
        if not 0 <= args["x"] < width or not 0 <= args["y"] < height:
            raise ValueError("Touch is outside the selected hardware geometry")
        events = [{"type": "abs", "data": {"axis": axis, "value": (value * 32767 + size - 2) // max(1, size - 1)}}
                  for axis, value, size in (("x", args["x"], width), ("y", args["y"], height))]
        events.append({"type": "btn", "data": {"button": "left", "down": args["down"]}})
        self.qmp.execute("input-send-event", {"events": events})
        return {"accepted": True, "guestObserved": False}

    def _watch_input(self):
        self._running()
        kind = self.qmp.execute("qom-get", {"path": "/machine", "property": "type"})
        if kind not in ("azki-p65-machine", "azki-q65-machine"):
            raise RuntimeFailure("unsupported_hardware_input", "Selected band has no modeled watch buttons/crown")

    def button(self, args):
        self._watch_input()
        if (set(args) != {"button", "clicks"} or args["button"] not in ("lower", "crown") or
                type(args["clicks"]) is not int or args["clicks"] not in (1, 2) or
                args["button"] == "crown" and args["clicks"] != 1):
            raise ValueError("Button requires lower single/double or crown single click")
        name = "right" if args["button"] == "lower" else "middle"
        for index in range(args["clicks"]):
            try:
                self.qmp.execute("input-send-event", {"events": [{"type": "btn", "data": {"button": name, "down": True}}]})
                time.sleep(.2)
            finally:
                self.qmp.execute("input-send-event", {"events": [{"type": "btn", "data": {"button": name, "down": False}}]})
            if index + 1 < args["clicks"]:
                time.sleep(.1)
        return {"accepted": True, "guestObserved": False}

    def crown(self, args):
        self._watch_input()
        if set(args) != {"delta"} or type(args["delta"]) is not int or not 1 <= abs(args["delta"]) <= 32:
            raise ValueError("Crown delta must be an integer in -32..-1 or 1..32")
        name = "wheel-up" if args["delta"] < 0 else "wheel-down"
        for _ in range(abs(args["delta"])):
            for down in (True, False):
                self.qmp.execute("input-send-event", {"events": [{"type": "btn", "data": {"button": name, "down": down}}]})
        return {"accepted": True, "guestObserved": False}

    def _bluetooth_service(self):
        if self.bluetooth_service is None:
            from .windows_bluetooth import WindowsBluetoothService
            helper = self.qemu.parent / "windows-bluetooth/AzkiBluetoothHost.exe"
            if not helper.is_file():
                helper = None
            # Direct independent transport: background ATT callbacks must never
            # acquire dispatch's UI/control lock to deliver a firmware reply.
            callback = self.guest_peer.call if self.guest_peer is not None else None
            self.bluetooth_service = WindowsBluetoothService(host_executable=helper, guest_rpc=callback)
        return self.bluetooth_service

    def bluetooth(self, args):
        if isinstance(args, dict) and args.get("action") in ("hci.usb.list", "hci.usb.inspect"):
            # Device selection must work before a firmware/WinRT session exists,
            # including on a host with no Windows-managed Bluetooth adapter.
            from .windows_bluetooth_usb import dispatch_inspection
            return dispatch_inspection(args)
        return self._bluetooth_service().dispatch(args)

    def input(self, args):
        self._running()
        if self.hardware is None:
            from .windows_hardware import WindowsHardware, model_profile
            hardware = model_profile(self.project, self.prepared["profile"]["model"])
            self.hardware = WindowsHardware(self.qmp, self.prepared["run"], self.prepared["board"], hardware)
        return self.hardware.dispatch(args)

    def _instance_health_snapshot(self):
        """Stop the owned CPUs before sampling; no arbitrary memory route."""
        if (self.persistent is None or self.persistent.work is None or self.qmp is None or
                self.process is None or self.process.poll() is not None):
            return {"qmpHealthy": False, "guestFault": True, "reason": "owned-running-core-health-unavailable"}
        try:
            self.qmp.execute("stop")
            paused = self.qmp.execute("query-status")
            fault = self.qmp.execute("human-monitor-command", {"command-line": "xp /6wx 0xe000ed24"})
            registers = self.qmp.execute("human-monitor-command", {"command-line": "info registers"})
            import re
            values = [int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{8})", fault)]
            xpsr = re.search(r"XPSR=([0-9a-fA-F]{8})", registers)
            known = type(paused) is dict and paused.get("running") is False and len(values) == 6 and xpsr is not None
            active = int(xpsr.group(1), 16) & 511 if xpsr else None
            return {"qmpHealthy": known, "guestFault": not known or values[1] != 0 or values[2] != 0 or active in (3, 4, 5, 6),
                    "CFSR": values[1] if len(values) > 1 else None,
                    "HFSR": values[2] if len(values) > 2 else None, "activeException": active}
        except (OSError, RuntimeError, ValueError, TypeError) as error:
            return {"qmpHealthy": False, "guestFault": True, "reason": str(error)[:512]}

    def _storage_log_healthy(self):
        if self.prepared is None:
            return False
        markers = (b"NAND instance writeback failed", b"NAND instance erase writeback failed",
                   b"NOR instance writeback failed", b"Q65 storage writeback failed")
        try:
            for name in ("stderr.log", "qemu.log"):
                path = self.prepared["run"] / name
                if not path.is_file() or path.stat().st_size > 512 * 1024**2:
                    return False
                with path.open("rb") as stream:
                    tail = b""
                    while chunk := stream.read(1024 * 1024):
                        value = tail + chunk
                        if any(marker in value for marker in markers):
                            return False
                        tail = value[-128:]
            return True
        except OSError:
            return False

    def stop(self, args=None):
        if (self.stop_result is not None and self.prepared is not None and self.prepared.get("resourcesClosed") and
                (self.persistent is None or self.persistent.lease is None)):
            return self.stop_result
        errors = []
        bluetooth_cleanup = None
        peer_healthy = True

        def attempt(label, operation):
            try:
                return operation()
            except Exception as error:
                errors.append({"step": label, "message": str(error)[:512]})
                return None

        # Each cleanup stage runs even if an earlier host/guest transport fails.
        # The guest callback owns its transport lock; it never needs dispatch's
        # outer runtime lock, so stopping a background ATT pump cannot deadlock.
        if self.bluetooth_service is not None:
            service = self.bluetooth_service
            returned = attempt("bluetooth", service.close)
            reported = getattr(service, "close_report", None)
            bluetooth_cleanup = reported if isinstance(reported, dict) else returned
            if isinstance(bluetooth_cleanup, dict) and not bluetooth_cleanup.get("cleanupComplete", False):
                errors.append({"step": "bluetooth_resources", "message": "Bluetooth resource closure was not verified"})
            self.bluetooth_service = None
        if self.guest_peer is not None:
            peer = self.guest_peer
            attempt("guest_peer", peer.close)
            peer_healthy = not getattr(peer, "failure", None) and not getattr(getattr(peer, "core", None), "failure", None)
            self.guest_peer = None
        if self.hardware is not None:
            attempt("hardware_close", self.hardware.close)
            self.hardware = None
        health_snapshot = self._instance_health_snapshot()
        if self.qmp is not None:
            if self.process is not None and self.process.poll() is None:
                try:
                    self.qmp.execute("quit")
                except (OSError, RuntimeError, ValueError):
                    pass  # Quit can close its pipe before a correlated reply.
            attempt("qmp_close", self.qmp.close)
            self.qmp = None
        if self.reader is not None:
            attempt("frame_close", self.reader.close)
            self.reader = None
        forced = False
        if self.process is not None and self.process.poll() is None:
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                forced = True
        if self.job is not None:
            attempt("job_close", self.job.close)  # Exact owned child tree only.
            self.job = None
        if self.process is not None and self.process.poll() is None:
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                # A failed job assignment/close still retains this exact Popen
                # handle. Never infer identity from a PID or scan other tasks.
                attempt("owned_process_terminate", self.process.terminate)
                attempt("owned_process_wait", lambda: self.process.wait(timeout=5))
        if self.guest_state is not None:
            attempt("guest_state", self.guest_state.close)
            self.guest_state = None
        if self.stderr_file is not None:
            attempt("log_close", self.stderr_file.close)
            self.stderr_file = None
        closed = self.process is None or self.process.poll() is not None
        if closed and self.process is not None:
            process_handle = getattr(self.process, "_handle", None)
            if process_handle is not None and hasattr(process_handle, "Close"):
                attempt("owned_process_handle_close", process_handle.Close)
        result = {"running": not closed, "ownedChildClosed": closed,
                  "exitCode": self.process.poll() if self.process else None,
                  "jobCloseRequired": forced, "cleanupErrors": errors,
                  "cleanupComplete": closed and not errors}
        if isinstance(bluetooth_cleanup, dict):
            # Resource closure and a controller Reset acknowledgement are
            # independent facts. Preserve both; neither proves RF switched off.
            result["bluetoothCleanup"] = bluetooth_cleanup
        if self.persistent is not None and self.persistent.lease is not None:
            storage_healthy = self._storage_log_healthy()
            completion = {"ownedChildClosed": closed, "exitCode": result["exitCode"],
                          "cleanupComplete": result["cleanupComplete"], "forced": forced,
                          "guestFault": bool(health_snapshot["guestFault"]),
                          "qmpHealthy": bool(health_snapshot["qmpHealthy"]),
                          "storageHealthy": storage_healthy, "peerHealthy": bool(peer_healthy)}
            persistence = attempt("instance_finish", lambda: self.persistent.finish(completion))
            if persistence is not None:
                result["persistence"] = persistence
                result["persistenceHealth"] = health_snapshot
                if persistence.get("leaseClosed") is False:
                    errors.append({"step": "instance_lease_close", "message": persistence.get("cleanupError", "Instance lease closure failed")})
        if self.prepared is not None and self.process is not None:
            receipt = self.prepared["session"] / "owned-exit.json"
            if not receipt.exists():
                attempt("exit_receipt", lambda: self._write_session("owned-exit.json", {
                    **result, "pid": self.process.pid,
                    "APUnchanged": _sha(self.prepared["ap"]) == self.prepared["recipe"]["ap"]["sha256"],
                    "bootCopyUnchanged": (_sha(self.prepared["bootCopy"]) == self.prepared["bootCopyReceipt"]["bootCopySHA"]
                                          if self.prepared.get("bootCopy") else None),
                    "coreUnchanged": _sha(self.qemu) == self.prepared["coreSHA256"]}))
        result["cleanupComplete"] = closed and not errors
        if self.prepared is not None:
            # A prepared WinRT session already owns guest state. Explicit stop
            # or a failure before Popen closes it too; retry in a fresh session
            # rather than reusing a closed transport or CREATE_NEW log paths.
            self.prepared["resourcesClosed"] = True
        self.stop_result = result
        return result

    def dispatch(self, command, args):
        if command not in {"profiles", "prepare", "start", "status", "frame", "touch", "button", "crown", "stop", "bluetooth", "input", "instances"}:
            raise RuntimeFailure("unsupported_command", "Unknown Windows runtime command")
        if type(args) is not dict:
            raise ValueError("args must be an object")
        with self.lock:
            return getattr(self, command)(args)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project-root", type=Path, required=True)
    parser.add_argument("--qemu", type=Path, required=True)
    parser.add_argument("--workspace", type=Path, required=True)
    args = parser.parse_args(argv)
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", newline="\n")
    if hasattr(sys.stdin, "reconfigure"):
        sys.stdin.reconfigure(encoding="utf-8", errors="strict", newline="\n")
    runtime = WindowsRuntime(args.project_root, args.qemu, args.workspace)
    try:
        while True:
            line = sys.stdin.readline(1024 * 1024 + 2)
            if not line:
                break
            request_id = None
            oversized = len(line.encode("utf-8")) > 1024 * 1024 or not line.endswith("\n")
            try:
                if oversized:
                    raise ValueError("Request exceeds bounded JSONL limit")
                request = json.loads(line, object_pairs_hook=_unique,
                                     parse_constant=lambda _: (_ for _ in ()).throw(ValueError("Non-finite JSON number")))
                if type(request) is not dict or set(request) != {"id", "command", "args"}:
                    raise ValueError("Expected id/command/args JSON object")
                request_id = request["id"]
                if type(request_id) not in (int, str):
                    raise ValueError("Request id must be integer or string")
                result = runtime.dispatch(request["command"], request["args"])
                response = {"id": request_id, "ok": True, "result": result}
            except Exception as error:
                response = {"id": request_id, "ok": False,
                            "error": {"code": getattr(error, "code", "runtime_error"), "message": str(error)[:1024]}}
            print(json.dumps(response, ensure_ascii=False, allow_nan=False), flush=True)
            if oversized:
                break  # Do not consume or resynchronize an unbounded request.
    finally:
        runtime.stop({})
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
