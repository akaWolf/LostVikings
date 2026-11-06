"""Tests for DATA.DAT identification; no game data is required."""
import importlib.util
from pathlib import Path
import struct
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / "tools/data/extract_datadat.py"
spec = importlib.util.spec_from_file_location("extract_datadat", SCRIPT)
extract = importlib.util.module_from_spec(spec)
spec.loader.exec_module(extract)


class ArchiveInfoTest(unittest.TestCase):
    def setUp(self):
        self.data = bytearray(64)
        struct.pack_into("<I", self.data, 0, 8)
        struct.pack_into("<16H", self.data, 8,
                         1, 0, 0, 5, 2, 0x220, 7, 1, 0xffff, 0xc285,
                         0x6969, 0, 0, 0, 0, 0)

    def test_audio_change_is_separate_from_other_changes(self):
        baseline = extract.archive_info(self.data)
        with patch.object(extract, "REFERENCE_SIZE", len(self.data)), \
             patch.object(extract, "REFERENCE_MD5", baseline["md5"]), \
             patch.object(extract, "REFERENCE_AUDIO_MASKED_MD5", baseline["audio_masked_md5"]):
            self.assertEqual(extract.archive_info(self.data)["reference"], "exact")
            self.data[16] = 1  # music set at record 0 + 8
            result = extract.archive_info(self.data)
            self.assertEqual(result["reference"], "audio_settings_only")
            self.assertEqual(result["audio"]["music_set"], 1)
            self.data[50] ^= 1
            self.assertEqual(extract.archive_info(self.data)["reference"], "unknown")

    def test_bad_magic_is_not_interpreted(self):
        self.data[8 + 20] = 0
        result = extract.archive_info(self.data)
        self.assertNotIn("audio", result)
        self.assertEqual(result["reference"], "unknown")

    def test_short_and_invalid_inputs(self):
        for data in (b"", b"\x03\x00\x00\x00", b"\x08\x00\x00\x00"):
            with self.subTest(data=data), self.assertRaises(ValueError):
                extract.archive_info(data)


class RecordDiagnosticTest(unittest.TestCase):
    def test_size_is_stored_minus_one(self):
        payload = extract.lzss_decompress(b"\x07ABC", 2)
        entry = {"id": 2, "comp_size": 6, "decomp_size": 2,
                 "decomp_got": len(payload)}
        self.assertEqual(payload, b"ABC")
        self.assertEqual(extract.record_diagnostic(entry, "exact"),
                         {"kind": "lzss_probe", "expected_size": 3, "status": "complete"})

    def test_truncated_stream(self):
        payload = extract.lzss_decompress(b"\x07AB", 2)
        entry = {"id": 2, "comp_size": 5, "decomp_size": 2,
                 "decomp_got": len(payload)}
        self.assertEqual(extract.record_diagnostic(entry, "exact")["status"], "short")
        entry["comp_size"] = 1
        self.assertEqual(extract.record_diagnostic(entry, "exact")["status"], "missing_size")

    def test_reference_special_records_and_unknown_archive(self):
        for cid, kind in ((0, "setup_header"), (1, "raw_screen"), (0x216, "unknown")):
            entry = {"id": cid}
            self.assertEqual(extract.record_diagnostic(entry, "audio_settings_only"),
                             {"kind": kind, "status": "not_checked"})
            self.assertEqual(extract.record_diagnostic(entry, "unknown"),
                             {"kind": "unclassified", "status": "not_checked"})


if __name__ == "__main__":
    unittest.main()
