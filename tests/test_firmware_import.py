"""The firmware import is the first step of the boot ladder, and the only step
that does not need the real firmware.

The probe is structural by design: a complete OTA is inventoried, never
authenticated, so a synthetic package carries the same shape as the real one.
That makes the import contract testable without the firmware nobody in this
repository has: the ZIP or the BES envelope is recognised, the unique
vela_ap.bin member is extracted into the private library, the models the AP
declares decide which presets the import is visible under, and everything that
is not a complete OTA is refused with a reason.

What this does not cover: the guest. Whether the imported firmware boots is
reported by the run itself, and that still waits for an authorized image.
"""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "Sources/AzkiSimulator/Runtime"))

from azki_simulator.firmware_catalog import CatalogError, FirmwareCatalog  # noqa: E402

BUNDLE = ROOT / "engine"


def synthetic_ota(directory, *, ap=None, resource=True, version="4.100.177"):
    """A complete OTA package: one AP member, optionally the resource image."""
    path = Path(directory) / "ota.zip"
    if ap is None:
        ap = b"\x00" * 64 + b"CHIP=best1503\nVERSION=" + version.encode() + b"\n"
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("vela_ap.bin", ap)
        if resource:
            archive.writestr("vela_resource.bin", b"RESOURCE" * 32)
        archive.writestr("version.ini", "sw_version=" + version + "\n")
    return path


class FirmwareImportTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="azki-import-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.library = self.root / "library"
        self.catalog = FirmwareCatalog(BUNDLE, self.library)

    def test_a_synthetic_ota_is_imported_and_becomes_a_profile(self):
        source = synthetic_ota(self.root)
        receipt = self.catalog.import_file(str(source))
        self.assertTrue(receipt["recognized"])
        self.assertTrue(receipt["imported"])
        self.assertEqual(receipt["inputKind"], "ota")
        self.assertEqual(receipt["sha256"], hashlib.sha256(source.read_bytes()).hexdigest())
        self.assertTrue(receipt["candidateIDs"])

        items = self.catalog.list_items()
        self.assertEqual(len(items), len(receipt["candidateIDs"]))
        self.assertEqual(items[0]["id"], receipt["candidateIDs"][0])
        self.assertEqual(items[0]["firmwareVersion"], "4.100.177")
        self.assertTrue(items[0]["availability"]["ready"], "导入后的固件必须是可启动的 profile")

        # The package and the extracted AP are objects in the private library.
        objects = sorted(path.name for path in (self.library / "objects").iterdir())
        self.assertIn(receipt["sha256"], objects)
        self.assertEqual(len(objects), 2)  # the package and its AP member

    def test_the_receipt_covers_every_candidate(self):
        # The decoder refuses a receipt whose items do not cover its candidates,
        # so an import that is usable has to carry an item per candidate.
        receipt = self.catalog.import_file(str(synthetic_ota(self.root)))
        item_ids = {item["id"] for item in receipt["items"]}
        self.assertTrue(set(receipt["candidateIDs"]).issubset(item_ids))
        self.assertTrue(receipt["needsSelection"] or len(receipt["candidateIDs"]) == 1)

    def test_an_ap_that_declares_a_known_model_offers_only_that_model(self):
        ap = b"CHIP=best1503\nmiwear.watch.q66ys\n"
        receipt = self.catalog.import_file(str(synthetic_ota(self.root, ap=ap)))
        self.assertEqual(len(receipt["candidateIDs"]), 1)
        self.assertTrue(receipt["candidateIDs"][0].startswith("miwear.watch.q66ys@"))
        self.assertFalse(receipt["needsSelection"])

    def test_an_ap_that_declares_an_unknown_model_still_yields_a_usable_receipt(self):
        # The AP names a device this build has no preset for. The candidates
        # fall back to every preset, so the items have to follow them; when they
        # did not, the receipt carried candidates without items and the decoder
        # refused the whole import.
        ap = b"CHIP=best1503\nmiwear.watch.q66nfc\n"
        receipt = self.catalog.import_file(str(synthetic_ota(self.root, ap=ap)))
        self.assertGreater(len(receipt["candidateIDs"]), 1)
        item_ids = {item["id"] for item in receipt["items"]}
        self.assertEqual(set(receipt["candidateIDs"]), item_ids)

    def test_a_bound_import_is_visible_only_under_that_model(self):
        ap = b"CHIP=best1503\nmiwear.watch.q66ys\n"
        receipt = self.catalog.import_file(str(synthetic_ota(self.root, ap=ap)),
                                           model="miwear.watch.q66ys")
        self.assertEqual(len(receipt["candidateIDs"]), 1)
        self.assertFalse(receipt["needsSelection"])
        for item in self.catalog.list_items():
            self.assertEqual(item["model"], "miwear.watch.q66ys")

    def test_a_zip_without_an_ap_member_is_refused(self):
        path = self.root / "no-ap.zip"
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr("vela_resource.bin", b"RESOURCE")
        with self.assertRaises(CatalogError) as error:
            self.catalog.import_file(str(path))
        self.assertEqual(error.exception.code, "unknown_firmware")
        self.assertEqual(self.catalog.list_items(), [])

    def test_a_file_that_is_not_an_ota_is_refused(self):
        path = self.root / "ap-only.bin"
        path.write_bytes(b"CHIP=best1503\n" + b"\x00" * 64)
        with self.assertRaises(CatalogError) as error:
            self.catalog.import_file(str(path))
        self.assertEqual(error.exception.code, "unknown_firmware")
        self.assertIn("请导入完整 OTA 固件包", str(error.exception))

    def test_importing_the_same_bytes_twice_replaces_the_record(self):
        source = synthetic_ota(self.root)
        first = self.catalog.import_file(str(source))
        second = self.catalog.import_file(str(source))
        self.assertEqual(first["sha256"], second["sha256"])
        self.assertEqual(len(self.catalog.list_items()), len(first["candidateIDs"]))
        objects = list((self.library / "objects").iterdir())
        self.assertEqual(len(objects), 2)

    def test_deleting_an_import_reclaims_its_objects(self):
        source = synthetic_ota(self.root)
        receipt = self.catalog.import_file(str(source))
        self.catalog.delete_import(receipt["sha256"])
        self.assertEqual(self.catalog.list_items(), [])
        self.assertEqual(list((self.library / "objects").iterdir()), [])

    def test_the_index_survives_a_new_catalog_instance(self):
        receipt = self.catalog.import_file(str(synthetic_ota(self.root)))
        reopened = FirmwareCatalog(BUNDLE, self.library)
        self.assertEqual([item["id"] for item in reopened.list_items()],
                         receipt["candidateIDs"])
        index = json.loads((self.library / "index.json").read_text())
        self.assertEqual(len(index["imports"]), 1)


if __name__ == "__main__":
    unittest.main()
