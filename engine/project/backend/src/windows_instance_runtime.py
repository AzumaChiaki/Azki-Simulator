"""Bind exact firmware sessions to private, jointly committed Windows instances.

The file store owns the transaction protocol. This adapter selects real board
images and supplies completion evidence from the owned runtime; JSON callers
cannot provide a health/commit assertion or a writable image path.
"""
from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import re
import subprocess
import uuid

from .windows_instances import WindowsInstanceStore, InstanceStorageError


def failure(code, message):
    from .windows_runtime import RuntimeFailure
    return RuntimeFailure(code, message)


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def domain(mode):
    return "raw-usb" if mode == "usb" else "virtual"


class PersistentInstances:
    def __init__(self, runtime):
        self.runtime = runtime
        self.lease = self.work = None
        self._properties = {}

    def store(self):
        self.runtime._workspace()
        return WindowsInstanceStore(self.runtime.workspace, self.runtime.files)

    def actions(self, args):
        action = args.get("action")
        if action == "list" and set(args) == {"action"}:
            result = self.store().list()
            for row in result["instances"]:
                row["persistence"] = row.get("persistenceCapabilities")
            return result
        if action == "create" and set(args) == {"action", "profileID", "name"}:
            selected = next((row for row in self.runtime.profiles()["profiles"]
                             if row["id"] == args["profileID"]), None)
            if selected is None:
                raise failure("unknown_profile", "Select an exact catalog version or hardware preset")
            return self.store().create(selected["id"], args["name"], selected["model"])
        if action == "delete" and set(args) <= {"action", "instanceID", "expectedRevision"} and "instanceID" in args:
            return self.store().delete(args["instanceID"], args.get("expectedRevision"))
        if action == "recover" and set(args) == {"action", "instanceID", "operation"}:
            return self.store().recover(args["instanceID"], args["operation"])
        raise ValueError("Unsupported instance action or fields")

    def properties(self, machine):
        if machine not in ("best1503", "azki-p65", "azki-q65"):
            raise failure("unsupported_machine", "Instance persistence requires a known Azki board")
        identity = (digest(self.runtime.qemu), machine)
        if identity not in self._properties:
            process = subprocess.run([str(self.runtime.qemu), "-machine", machine + ",help"],
                                     capture_output=True, timeout=15,
                                     creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            if process.returncode or len(process.stdout) + len(process.stderr) > 1024 * 1024:
                raise failure("core_properties_unavailable", "Could not read the selected core's board properties")
            text = (process.stdout + process.stderr).decode("utf-8", "replace")
            names = set(re.findall(r"^\s*([a-z][a-z0-9-]+)=", text, re.MULTILINE))
            if not names:
                raise failure("core_properties_unavailable", "Core returned no readable board property inventory")
            self._properties[identity] = names
        return self._properties[identity]

    @staticmethod
    def controller(args):
        path = args.get("hciDevicePath")
        if type(path) is not str or not path:
            raise failure("hci_device_required", "Select the explicitly owned WinUSB controller interface")
        from .windows_bluetooth_usb import resolve_device_interface
        identity = resolve_device_interface(path)
        if str(identity.get("driver", "")).casefold() != "winusb":
            raise failure("hci_driver_required", "The selected dedicated interface is not bound to WinUSB")
        return identity["instanceID"]

    def prepare(self, args):
        runtime = self.runtime
        self.lease = self.store().lease(args["instanceID"])
        try:
            head = self.lease.head()
            if self.lease.pending_path.exists():
                raise InstanceStorageError("recovery_required", "An unpublished run is preserved; recover it before starting")
            mode = args.get("bluetooth", "reject")
            if mode not in ("off", "reject", "winrt", "usb"):
                raise ValueError("Choose an available Bluetooth transport")
            selected_domain = domain(mode)
            controller = self.controller(args) if selected_domain == "raw-usb" else None
            context = {"persistent": True, "instanceID": args["instanceID"],
                       "transportDomain": selected_domain, "deferReceipt": True}
            request = dict(args)
            request.pop("instanceID", None)
            if head:
                binding = head["binding"]
                if binding.get("transportDomain", "virtual") != selected_domain:
                    raise failure("instance_transport_mismatch", "This instance belongs to another Bluetooth transport domain; create a separate instance")
                if controller is not None and controller.casefold() != str(binding.get("controllerInstanceID", "")).casefold():
                    raise failure("instance_controller_mismatch", "Raw Bluetooth state can reopen only with the same dedicated controller")
                if mode == "winrt" and not binding["persistence"]["identity"]:
                    raise failure("guest_identity_unsupported", "This instance has no verified virtual factory identity; its saved storage was preserved")
                if args.get("profileID", binding["profileID"]) != binding["profileID"]:
                    raise failure("instance_firmware_mismatch", "An initialized instance cannot change firmware profiles")
                if set(args) - {"instanceID", "bluetooth", "hciDevicePath", "profileID", "skipOOBE"}:
                    raise failure("instance_inputs_immutable", "Reopen this initialized instance by instanceID; imported firmware inputs are immutable")
                request = {"profileID": binding["profileID"], "bluetooth": mode,
                           "skipOOBE": binding["firstRunSkipOOBE"],
                           "firmwarePath": head["inputs"]["firmware.bin"]["path"]}
                for label, field in (("resource.bin", "resourcePath"), ("audio.bin", "audioPath"), ("sensor.bin", "sensorPath")):
                    if label in head["inputs"]:
                        request[field] = head["inputs"][label]["path"]
                if args.get("hciDevicePath"):
                    request["hciDevicePath"] = args["hciDevicePath"]
                context["session"] = runtime.files.private_directory(runtime.workspace / ("session-" + uuid.uuid4().hex))
                self.work = self.lease.begin_work(context["session"])
                context.update(restoring=True, work=self.work,
                               wantIdentity=binding["persistence"]["identity"])
            else:
                if request.get("profileID") != self.lease.info["profileID"]:
                    raise failure("instance_firmware_mismatch", "Select the instance's original hardware/profile before its first import")
                context["wantIdentity"] = selected_domain == "virtual" and self.lease.info["model"] != "miwear.watch.p65"
            runtime._prepare_session(request, context=context)
            prepared = runtime.prepared
            if head:
                binding = head["binding"]
                if (prepared["recipe"]["ap"]["sha256"] != binding["apSHA256"] or
                        prepared["recipe"]["id"] != binding["executionProfileID"] or
                        prepared["board"]["geometry"] != binding["boardGeometry"] or
                        prepared["board"].get("nor_geometry") != binding.get("norGeometry")):
                    raise failure("instance_geometry_mismatch", "Saved storage differs from this exact firmware/board layout; no migration or reseeding was attempted")
            else:
                binding, inputs, seeds = self._initial_seed(prepared, selected_domain, controller)
                head = self.lease.initialize(binding, inputs, seeds)
                if runtime.guest_state is not None:
                    runtime.guest_state.close()
                    runtime.guest_state = None
                self.work = self.lease.begin_work(prepared["session"])
                if binding["persistence"]["identity"]:
                    from .windows_guest_state import create_windows_guest_state
                    runtime.guest_state = create_windows_guest_state(prepared["recipe"], prepared["board"],
                        Path(self.work["files"]["guest-state/guest-state.json"]["path"]).parent,
                        instance_id=args["instanceID"], skip_oobe=binding["firstRunSkipOOBE"], api=runtime.files)
            self._wire(prepared, self.work)
            prepared["instanceID"] = args["instanceID"]
            prepared["instanceBinding"] = binding
            prepared["storageMode"] = "persistent-owned-working-copy"
            runtime.last_prepare_args = {"instanceID": args["instanceID"], "bluetooth": mode,
                                         **({"hciDevicePath": args["hciDevicePath"]} if args.get("hciDevicePath") else {})}
            runtime._write_prepared_receipt()
            result = runtime._prepared_result()
            result.update(instanceID=args["instanceID"], persistence={
                **binding["persistence"], "baseRevision": self.work["baseRevision"],
                "workingRevision": self.work["transactionID"],
                "firstRunSkipOOBE": binding["firstRunSkipOOBE"],
                "physicalBluetoothVerified": False})
            return result
        except BaseException:
            runtime.stop({})
            raise

    def _erased(self, path, size):
        fs = self.lease.fs
        fs.api.private_directory(Path(path).parent, exist_ok=True)
        handle = fs.new_handle(path)
        try:
            remaining = size
            while remaining:
                data = b"\xff" * min(remaining, 1024 * 1024)
                fs.write(handle, data)
                remaining -= len(data)
            if not fs.k.FlushFileBuffers(handle):
                raise fs.api.error("Flush initial erased instance storage")
        finally:
            fs.close(handle)

    def _initial_seed(self, prepared, selected_domain, controller):
        runtime, fs = self.runtime, self.lease.fs
        board, recipe = prepared["board"], prepared["recipe"]
        properties = self.properties(board.get("machine", "best1503"))
        geometry = board["geometry"]
        nand = {"data-image", "data-spare-image", "data-writeback"} <= properties
        nand = nand and all(type(geometry.get(key)) is int for key in ("nand-data-offset", "nand-data-bytes"))
        nor = {"nor-image", "nor-writeback"} <= properties and type(board.get("nor_geometry")) is dict
        if nor:
            from .instance_data import validate_nor_geometry
            validate_nor_geometry(board, ap_sha256=recipe["ap"]["sha256"], ap_bytes=recipe["ap"]["size"])
        identity = runtime.guest_state is not None
        capability = {"nand": bool(nand), "nor": bool(nor), "identity": identity,
                      "bindings": bool(nor and (identity or selected_domain == "raw-usb")),
                      "volatileAreas": ([] if nand else ["userNAND"]) + ([] if nor else ["NOR"]),
                      "identityKind": "virtual" if identity else "none", "manufacturingIdentity": identity}
        if not nand and not nor:
            raise failure("instance_persistence_unsupported", "This exact core/profile exposes no verified writable instance storage")
        seed_root = runtime.files.private_directory(prepared["session"] / "instance-seed")
        files, expected = {}, {}
        if nand:
            size = geometry["nand-data-bytes"]
            for name, extent in (("nand-data.bin", size), ("nand-spare.bin", size // 2048 * 64)):
                path = seed_root / name
                if name == "nand-data.bin" and prepared["dataImage"]:
                    fs.copy(prepared["dataImage"], path, maximum=extent, exact=extent)
                else:
                    self._erased(path, extent)
                files[name], expected[name] = path, {"bytes": extent}
        if identity:
            for name, path in (("guest-state/guest-nor.bin", runtime.guest_state.nor_path),
                               ("guest-state/virtual-peer-state.json", runtime.guest_state.state_path),
                               ("guest-state/guest-state.json", runtime.guest_state.manifest_path)):
                files[name] = path
                expected[name] = {"bytes": board["nor_geometry"]["bytes"]} if name.endswith(".bin") else {"maxBytes": 4096 if name.endswith("virtual-peer-state.json") else 8192}
        elif nor:
            path = seed_root / "nor-config.bin"
            if prepared["profile"]["model"] == "miwear.watch.p65" and "audio-image" in prepared["images"]:
                # Seed immutable original audio exactly once. Reopening must
                # retain subsequent real NOR writes rather than install the
                # input over saved audio bytes on every launch.
                audio = fs.read(prepared["images"]["audio-image"], maximum=3 * 1024**2)
                offset = 0xd00000 - board["nor_geometry"]["offset"]
                size = board["nor_geometry"]["bytes"]
                if len(audio) < 0x128 or audio[:4] != bytes.fromhex("1cec57be") or not 0 <= offset < offset + len(audio) <= size:
                    raise failure("p65_audio_layout", "Original BES audio does not fit the exact saved NOR layout")
                seed = bytearray(b"\xff" * size)
                seed[offset:offset + len(audio)] = audio
                fs.write_new(path, bytes(seed))
            else:
                self._erased(path, board["nor_geometry"]["bytes"])
            files["nor-config.bin"], expected["nor-config.bin"] = path, {"bytes": board["nor_geometry"]["bytes"]}
            if selected_domain == "raw-usb":
                # Until the raw manufacturing format is verified, persist real
                # erased NOR and actual guest writes. Never save virtual SMP.
                metadata = {"schema": 1, "instanceID": self.lease.instance_id,
                            "model": prepared["profile"]["model"], "identityKind": "none",
                            "controllerInstanceID": controller, "controllerAddress": None,
                            "addressSource": "not-observed"}
                metadata_path = seed_root / "raw-manufacturing.json"
                fs.write_new(metadata_path, (json.dumps(metadata, sort_keys=True) + "\n").encode())
                files["raw-manufacturing.json"], expected["raw-manufacturing.json"] = metadata_path, {"maxBytes": 8192}
        inputs = {"firmware.bin": prepared["session"] / "inputs/firmware.bin"}
        if recipe.get("resource_path"):
            inputs["resource.bin"] = Path(recipe["resource_path"])
        for option, label in (("audio-image", "audio.bin"), ("sensor-image", "sensor.bin")):
            if option in prepared["images"]:
                inputs[label] = Path(prepared["images"][option])
        binding = {"schema": 1, "model": prepared["profile"]["model"],
                   "profileID": prepared["profile"]["id"], "executionProfileID": recipe["id"],
                   "apSHA256": recipe["ap"]["sha256"], "boardGeometry": copy.deepcopy(geometry),
                   "firstRunSkipOOBE": prepared["skipOOBE"], "persistence": capability,
                   "expectedFiles": expected, "transportDomain": selected_domain,
                   "controllerInstanceID": controller, "controllerAddress": None,
                   "addressSource": "not-observed", "factoryFormatVerified": identity}
        if nor:
            binding["norGeometry"] = copy.deepcopy(board["nor_geometry"])
        return binding, inputs, files

    @staticmethod
    def _wire(prepared, work):
        files = work["files"]
        prepared["instanceFiles"] = files
        prepared["dataImage"] = None
        prepared["factory"] = None if work["binding"]["persistence"]["nor"] else prepared["factory"]
        if prepared["profile"]["model"] == "miwear.watch.p65" and "nor-config.bin" in files:
            prepared["images"].pop("audio-image", None)

    def finish(self, health):
        if self.lease is None:
            return None
        lease, self.lease = self.lease, None
        work, self.work = self.work, None
        receipt = None
        try:
            if work is not None:
                try:
                    receipt = lease.commit_work(work, health)
                except InstanceStorageError as error:
                    unknown = error.code == "commit_outcome_unknown"
                    receipt = {"instanceID": lease.instance_id, "committed": None if unknown else False,
                               "previousRevision": work["baseRevision"], "revision": None if unknown else work["baseRevision"],
                               "reason": error.code, "message": str(error), "guestFilesystemClean": False}
                except (OSError, ValueError) as error:
                    receipt = {"instanceID": lease.instance_id, "committed": False,
                               "previousRevision": work["baseRevision"], "revision": work["baseRevision"],
                               "reason": "instance_storage_failed", "message": str(error),
                               "guestFilesystemClean": False}
            else:
                head = lease.head()
                receipt = {"instanceID": lease.instance_id, "committed": False,
                           "revision": head["revision"] if head else None,
                           "reason": "no-owned-guest-completion", "guestFilesystemClean": False}
        finally:
            try:
                lease.close()
            except OSError as error:
                if receipt is None:
                    receipt = {"instanceID": lease.instance_id, "committed": None, "reason": "lease_close_failed"}
                receipt["leaseClosed"] = False
                receipt["cleanupError"] = str(error)
            else:
                if receipt is not None:
                    receipt["leaseClosed"] = True
        return receipt
