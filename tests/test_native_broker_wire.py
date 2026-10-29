"""The supervisor's state records must satisfy the helper's real validator.

native_broker.NativeBrokerProcess._state is the party that decides whether a
launch was acknowledged: it demands exact key sets, bounds on pid and exitCode,
a cleanup verdict, and an error field no longer than 1024 bytes. The C#
supervisor builds those records by hand, so a key renamed, an extra field, or
raw exception text on the wire only shows up while a guest is booting -- the
one path this repository cannot exercise without firmware. These cases drive
the shipped validator with the payloads parsed out of SessionSupervisor.cs.
"""
import re
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "Sources/AzkiSimulator/Runtime"))

from azki_simulator.minative import MError  # noqa: E402
from azki_simulator.native_broker import NativeBrokerProcess  # noqa: E402

SUPERVISOR = ROOT / "linux/src/AzkiSimulator.Linux/Core/SessionSupervisor.cs"
SESSION_ID = "3f1b2c4d-5e6f-4a7b-8c9d-0e1f2a3b4c5d"
RUN_NAME = "m-0123456789ab"
# Values the C# record fills in at run time; the validator only checks their
# type and range, so any in-range witness will do.
WITNESS = {"_sessionId": SESSION_ID, "_runName": RUN_NAME,
           "_guestGroup": 4242, "exit": 0, "pid": 4242, "exitCode": 0}


def published_records():
    """Every JsonObject the supervisor publishes as a native status record."""
    source = SUPERVISOR.read_text()
    records = []
    for match in re.finditer(r'PublishState\(new JsonObject \{', source):
        depth, index = 1, match.end()
        while depth:
            if source[index] == "{":
                depth += 1
            elif source[index] == "}":
                depth -= 1
            index += 1
        body = source[match.end():index - 1]
        fields = re.findall(r'\["([a-zA-Z]+)"\]\s*=\s*([^,\n]+(?:\n\s*[^,\n]+)?)', body)
        record = {}
        for key, raw in fields:
            value = raw.strip().rstrip(",")
            literal = re.fullmatch(r'"(.*)"', value)
            if literal:
                record[key] = literal.group(1)
            elif re.fullmatch(r"-?\d+", value):
                record[key] = int(value)
            elif value in WITNESS:
                record[key] = WITNESS[value]
            else:
                # A ternary (state chosen from the exit code, cleanup verdict) or
                # an interpolation: take the first string literal it can produce.
                choice = re.findall(r'"([^"]+)"', value)
                if choice:
                    record[key] = choice[0]
                elif key == "exitCode":
                    record[key] = 0
                else:
                    raise AssertionError(f"unparsed value for {key}: {value!r}")
        records.append(record)
    return records


class _Session:
    """Stand-in holding only what the validator reads from the session."""

    def __init__(self, directory):
        self.directory = directory
        self.config = {"version": 1, "id": SESSION_ID, "run": RUN_NAME}

    def check(self):
        return None


class NativeBrokerWireTests(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.TemporaryDirectory(prefix="azki-broker-wire-")
        self.addCleanup(self.root.cleanup)
        directory = Path(self.root.name)
        directory.chmod(0o700)
        self.session = _Session(directory)
        self.process = NativeBrokerProcess(self.session)
        self.path = directory / "state.json"

    def publish(self, record):
        self.path.write_text(__import__("json").dumps(record, sort_keys=True) + "\n")
        self.path.chmod(0o600)

    def test_records_are_published_and_parseable(self):
        records = published_records()
        self.assertGreaterEqual(len(records), 3)

    def test_every_supervisor_record_passes_the_helper_validator(self):
        for record in published_records():
            with self.subTest(state=record.get("state"), keys="+".join(sorted(record))):
                self.publish(record)
                # One process per record: the helper treats a terminal record as
                # immutable, which is the behavior, not a failure of this record.
                NativeBrokerProcess(self.session)._state()

    def test_error_field_never_carries_exception_text(self):
        for record in published_records():
            if "error" not in record:
                continue
            # published_records only accepts a string literal or the first
            # literal of a ternary, so a raw `error.Message` would surface here
            # as that expression rather than as a fixed sentence.
            with self.subTest(state=record.get("state")):
                self.assertIsInstance(record["error"], str)
                self.assertLessEqual(len(record["error"].encode()), 1024)
                self.assertNotIn("Message", record["error"])

    def test_validator_rejects_the_mistakes_this_contract_can_make(self):
        running = {"version": 1, "id": SESSION_ID, "run": RUN_NAME, "state": "running", "pid": 4242}
        terminal = {"version": 1, "id": SESSION_ID, "run": RUN_NAME, "state": "exited",
                    "exitCode": 0, "cleanup": "reaped", "pid": 4242}
        broken = [
            ("running without a pid", {k: v for k, v in running.items() if k != "pid"}),
            ("running with an extra field", running | {"exitCode": 0}),
            ("a state the helper does not know", terminal | {"state": "stopped"}),
            ("exit code out of range", terminal | {"exitCode": 256}),
            ("wrong session id", terminal | {"id": "not-this-launch"}),
            ("cleanup verdict missing", {k: v for k, v in terminal.items() if k != "cleanup"}),
            ("cleanup verdict not in the contract", terminal | {"cleanup": "maybe"}),
            ("error longer than the record bound", terminal | {"error": "x" * 1025}),
            ("error that is not text", terminal | {"error": None}),
        ]
        for label, record in broken:
            with self.subTest(label), self.assertRaises(MError):
                self.publish(record)
                self.process._state()

    def test_terminal_record_is_immutable(self):
        self.publish(terminal := {"version": 1, "id": SESSION_ID, "run": RUN_NAME, "state": "exited",
                                 "exitCode": 0, "cleanup": "reaped", "pid": 4242})
        self.process._state()
        with self.assertRaises(MError):
            self.publish(terminal | {"cleanup": "unconfirmed"})
            self.process._state()


if __name__ == "__main__":
    unittest.main()
