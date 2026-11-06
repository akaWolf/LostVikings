#!/usr/bin/env python3
"""Create a reference-settings DATA.DAT for the scenario golden oracle.

Only SETUP audio variants of the documented archive can be normalized; the
result is checked against the complete reference MD5 before being written.
"""
import hashlib
import importlib.util
from pathlib import Path
import struct
import sys

script = Path(__file__).resolve().parents[1] / "tools/data/extract_datadat.py"
spec = importlib.util.spec_from_file_location("extract_datadat", script)
extract = importlib.util.module_from_spec(spec)
spec.loader.exec_module(extract)

REFERENCE_AUDIO = (0, 0, 5, 2, 0x220, 7, 1, 0xffff)


def reference_copy(data):
    info = extract.archive_info(data)
    if info["reference"] == "exact":
        return "exact", None
    if info["reference"] != "audio_settings_only":
        return "unknown", None
    normalized = bytearray(data)
    struct.pack_into("<8H", normalized, info["record0_offset"] + 2, *REFERENCE_AUDIO)
    if hashlib.md5(normalized).hexdigest() != extract.REFERENCE_MD5:
        raise ValueError("normalized archive does not match reference checksum")
    return "normalized", bytes(normalized)


def main():
    try:
        status, data = reference_copy(Path(sys.argv[1]).read_bytes())
        if data is not None:
            Path(sys.argv[2]).write_bytes(data)
        print(status)
    except (ValueError, OSError) as exc:
        sys.exit(f"Cannot prepare golden reference archive: {exc}")


if __name__ == "__main__":
    main()
