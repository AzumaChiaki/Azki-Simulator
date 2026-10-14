"""Least-write launch policy for developer-inner-sandbox sessions.

Generated-mode profiles may create storage caches only under the session's private
prepared project assets directory, via the pinned yaffs-image/legacy-ui-db builders.
"""
import json
import os
from pathlib import Path
import sys
import stat

from .minative import MError


def execution_prefix(adapter):
    plan, run = adapter.plan, adapter.run_root
    identity = plan["identity"]
    generated = identity.get("mode") == "generated"
    if sys.platform != "darwin" or not Path("/usr/bin/sandbox-exec").is_file():
        raise MError("isolation_unavailable", "Required macOS sandbox launcher is unavailable")
    if not generated and not identity["profile"].get("resource_path"):
        raise MError("isolation_unavailable", "Resource cache generation is outside this read-only launch policy")
    read_paths = set()
    def collect(value):
        if isinstance(value, dict):
            if isinstance(value.get("path"), str) and value["path"].startswith("/") and type(value.get("bytes")) is int:
                read_paths.add(value["path"])
            for item in value.values(): collect(item)
        elif isinstance(value, list):
            for item in value: collect(item)
    collect(identity)
    interpreter = identity["python"]
    read_paths.add(interpreter["invocation"])
    read_paths.update(link["path"] for link in interpreter["links"])
    # CPython must enumerate package directories while importing encodings and
    # other stdlib modules. Exact file grants alone let it execute but fail
    # during init_fs_encoding. This subtree is the manifest-pinned bundled
    # interpreter library, outside the writable session/project overlays.
    python_library = Path(interpreter["invocation"]).parent.parent / "lib"
    try:
        library_info = python_library.lstat()
    except OSError as error:
        raise MError("unsafe_python", "Bundled Python library is unavailable") from error
    if not stat.S_ISDIR(library_info.st_mode) or stat.S_ISLNK(library_info.st_mode):
        raise MError("unsafe_python", "Bundled Python library must be a real directory")
    # Python's file finder also enumerates the directories containing the
    # manifest-pinned project modules. Grant listing on those directories
    # only; module contents remain limited to the exact pinned files above.
    project_root = Path(identity["project"])
    import_directories = {project_root}
    for key in ("runtimeSourcePins", "importableSource"):
        for pin in identity.get(key, []):
            file_path = Path(pin["path"])
            if file_path.is_relative_to(project_root):
                parent = file_path.parent
                while parent.is_relative_to(project_root):
                    import_directories.add(parent)
                    if parent == project_root:
                        break
                    parent = parent.parent
    for link in identity.get("dependencyLinks", []):
        read_paths.add(link["logicalPath"])
        read_paths.update(part["path"] for part in link["chain"])
    quote = lambda value: json.dumps(str(value), ensure_ascii=False)
    builders = [item["path"] for item in identity.get("builders", [])]
    writable = [run] + ([Path(identity["project"]) / "assets"] if generated else [])
    storage_binding = identity.get("instanceStorage")
    if storage_binding:
        instance_data = adapter.root / "instance-data"
        expected_images = {"dataImage": "nand-data.bin", "spareImage": "nand-spare.bin",
                           "norImage": "nor-config.bin"}
        for key, name in expected_images.items():
            image = storage_binding.get(key)
            if image is None and key == "norImage":
                continue
            if image != str(instance_data / name):
                raise MError("unsafe_instance_storage", "Instance storage images must remain in this session")
        writable.append(instance_data)
    # Exact content files are pinned above. Directory listing/metadata does not
    # grant file contents; all writes remain in this instance's private run.
    reads = " ".join("(literal " + quote(path) + ")" for path in sorted(read_paths))
    profile = (
        "(version 1)(deny default)(allow process-fork)(allow process-info*)(allow sysctl-read)"
        "(allow file-read-metadata)(allow signal (target children))"
        "(allow process-exec (literal " + quote(interpreter["executable"]["path"]) + ")"
        " (literal " + quote(interpreter["invocation"]) + ")"
        " (literal " + quote(identity["qemu"]["path"]) + ")"
        + "".join(" (literal " + quote(path) + ")" for path in builders) + ")"
        "(allow file-read* (literal \"/\") (subpath \"/System/Library\") (subpath \"/usr/lib\") " + reads + ")"
        "(allow file-read* (subpath " + quote(python_library) + "))"
        + "".join("(allow file-read-data (literal " + quote(path) + "))"
                 for path in sorted(import_directories))
        + "(allow file-read-data (literal \"/dev/urandom\") (literal \"/dev/random\"))"
        + "".join("(allow file-read* file-write* (subpath " + quote(path) + "))" for path in writable) +
        "(allow network-bind network-inbound network-outbound (local unix-socket (subpath " + quote(run) + ")))"
    )
    # Caller constructs this locally after inspecting/approving the whole plan.
    # No remote request supplies commands, env vars, profiles or file allowlists.
    # Keep a potentially large exact file allowlist out of Darwin's argv limit.
    policy = adapter.root / ("policy-" + run.name + ".sb")
    descriptor = os.open(policy, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(descriptor, "w") as stream:
        stream.write(profile)
    return (sys.executable, "-I", "-S", str(Path(__file__).with_name("minative_launch.py")),
            str(adapter.seconds + 15 if adapter.seconds else 0), str(adapter.runner_metadata), str(os.getpid()),
            "/usr/bin/sandbox-exec", "-f", str(policy))
