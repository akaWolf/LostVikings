"""Reference archive normalization tests without game data."""
import importlib.util
from pathlib import Path
import struct
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("reference_dat", Path(__file__).with_name("reference_dat.py"))
reference_dat = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reference_dat)


class ReferenceCopyTest(unittest.TestCase):
    def test_normalizes_only_audio_fields(self):
        data = bytearray(64)
        struct.pack_into("<I", data, 0, 8)
        struct.pack_into("<16H", data, 8, 1, 0, 0, 5, 2, 0x220, 7, 1,
                         0xffff, 0xc285, 0x6969, 0, 0, 0, 0, 0)
        baseline = reference_dat.extract.archive_info(data)
        with patch.object(reference_dat.extract, "REFERENCE_SIZE", len(data)), \
             patch.object(reference_dat.extract, "REFERENCE_MD5", baseline["md5"]), \
             patch.object(reference_dat.extract, "REFERENCE_AUDIO_MASKED_MD5", baseline["audio_masked_md5"]):
            self.assertEqual(reference_dat.reference_copy(data), ("exact", None))
            original = bytes(data)
            for music_set in (1, 7):
                struct.pack_into("<H", data, 16, music_set)
                self.assertEqual(reference_dat.reference_copy(data), ("normalized", original))
            data[50] ^= 1
            self.assertEqual(reference_dat.reference_copy(data), ("unknown", None))


if __name__ == "__main__":
    unittest.main()
