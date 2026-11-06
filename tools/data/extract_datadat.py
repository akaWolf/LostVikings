#!/usr/bin/env python3
"""Stage 1.1 (ENGINE_REWRITE_ROADMAP): DATA.DAT extractor + manifest.

Format (canonized by v2_read_chunk / orig sub_10982):
  header: dword offsets at chunk_id*4; chunk count = first_offset/4
  LZSS chunk: [u16 decompressed_size_minus_one][LZSS stream]
  compressed_size(id) = offset(id+1) - offset(id)   (includes the u16)
  LZSS: flag byte, bit=1 literal, bit=0 back-ref (12-bit ring offset,
        4-bit len+3), 4KB zero-initialized ring, stop on dest underflow.

Outputs (default assets_raw/):
  manifest.json           id, offset, comp/decomp sizes, sha256 of both
  chunks/comp/NNNN.bin    raw compressed stream (incl. the u16 header)
  chunks/dec/NNNN.bin     legacy LZSS probe output (even for raw records)
Round-trip proof: re-concatenation of header table + comp blocks must be
byte-identical to DATA.DAT (verified here, aborts otherwise).
"""
import sys, os, json, hashlib, struct

# README's reference archive, with record 0's SETUP audio words (+2..+11)
# zeroed. A match proves all other bytes match that particular archive.
REFERENCE_MD5 = "d0f8a04202070a54d1909758ecd962b5"
REFERENCE_AUDIO_MASKED_MD5 = "c85b5b52b3a2af5c1fd0917639e564c8"
REFERENCE_SIZE = 1570444
# Known raw screens and an unreferenced record of unknown type, as recorded in
# assets_raw/chunk_map.json. These IDs are only classified for the reference
# archive family, not for an unrecognized archive with a different layout.
REFERENCE_RAW_SCREENS = {0x001, 0x17c, 0x180, 0x211, 0x212, 0x213}
REFERENCE_UNKNOWN_RECORD = 0x216

def archive_info(data: bytes) -> dict:
    """Describe the archive without modifying it or guessing at other variants."""
    info = {"size": len(data), "md5": hashlib.md5(data).hexdigest(),
            "sha256": hashlib.sha256(data).hexdigest()}
    if len(data) < 4:
        raise ValueError("file is too short for the offset table")
    offset = struct.unpack_from("<I", data)[0]
    if offset < 4 or offset % 4 or offset > len(data):
        raise ValueError(f"invalid first chunk offset {offset:#x}")
    if offset + 32 > len(data):
        raise ValueError("record 0 is too short for the game header")
    words = struct.unpack_from("<16H", data, offset)
    info["record0_offset"] = offset
    info["record0_magic"] = words[10]
    if words[10] == 0x6969:
        info["audio"] = dict(zip(("music_mute", "effects_mute", "sound_card",
                                  "music_set", "io", "irq", "dma", "drq"),
                                 words[1:9]))
        masked = bytearray(data)
        masked[offset + 2:offset + 18] = bytes(16)
        info["audio_masked_md5"] = hashlib.md5(masked).hexdigest()
    if info["md5"] == REFERENCE_MD5 and len(data) == REFERENCE_SIZE:
        info["reference"] = "exact"
    elif (len(data) == REFERENCE_SIZE and
          info.get("audio_masked_md5") == REFERENCE_AUDIO_MASKED_MD5):
        info["reference"] = "audio_settings_only"
    else:
        info["reference"] = "unknown"
    return info

def print_archive_info(info: dict) -> None:
    print(f"DATA.DAT: {info['size']} bytes; MD5 {info['md5']}; SHA-256 {info['sha256']}")
    if info["reference"] == "exact":
        print("Reference: exact match to the documented archive")
    elif info["reference"] == "audio_settings_only":
        print("Reference: differs only in record 0 SETUP audio fields")
    else:
        print("Reference: differs outside the recognized SETUP audio fields, or is a different archive; "
              "this does not by itself prove corruption")
    if "audio" in info:
        a = info["audio"]
        print(f"SETUP audio: music mute={a['music_mute']}, effects mute={a['effects_mute']}, "
              f"sound card={a['sound_card']}, music set={a['music_set']}, "
              f"IO=0x{a['io']:04X}, IRQ={a['irq']}, DMA={a['dma']}, DRQ=0x{a['drq']:04X}")
    else:
        print(f"Warning: record 0 magic is 0x{info['record0_magic']:04X}, expected 0x6969; "
              "audio settings not interpreted")

def lzss_decompress(src: bytes, out_size: int) -> bytes:
    ring = bytearray(4096)
    bx = 0
    si = 0
    dest = bytearray()
    dx = out_size
    while True:
        if si >= len(src):  # source exhausted (spill-read tolerance)
            return bytes(dest)
        flags = src[si]; si += 1
        for _ in range(8):
            if flags & 1:
                if si >= len(src): return bytes(dest)
                val = src[si]; si += 1
                ring[bx] = val; bx = (bx + 1) & 0xFFF
                dest.append(val)
                dx = (dx - 1) & 0xFFFF
                if dx >= out_size: return bytes(dest)
            else:
                if si + 1 >= len(src): return bytes(dest)
                ref = src[si] | (src[si+1] << 8); si += 2
                length = ((ref >> 12) & 0xF) + 3
                off = ref & 0xFFF
                for _ in range(length):
                    val = ring[off]
                    ring[bx] = val; bx = (bx + 1) & 0xFFF
                    dest.append(val)
                    dx = (dx - 1) & 0xFFFF
                    if dx >= out_size: return bytes(dest)
                    off = (off + 1) & 0xFFF
            flags >>= 1

def record_diagnostic(entry: dict, reference: str) -> dict:
    """Interpret a legacy probe without claiming unknown records are LZSS."""
    if reference not in ("exact", "audio_settings_only"):
        return {"kind": "unclassified", "status": "not_checked"}
    cid = entry["id"]
    if cid == 0:
        return {"kind": "setup_header", "status": "not_checked"}
    if cid in REFERENCE_RAW_SCREENS:
        return {"kind": "raw_screen", "status": "not_checked"}
    if cid == REFERENCE_UNKNOWN_RECORD:
        return {"kind": "unknown", "status": "not_checked"}
    if entry["comp_size"] < 2:
        return {"kind": "lzss_probe", "status": "missing_size"}
    expected = entry["decomp_size"] + 1
    return {"kind": "lzss_probe", "expected_size": expected,
            "status": "complete" if entry["decomp_got"] == expected else "short"}

def main():
    src_path = sys.argv[1] if len(sys.argv) > 1 else "DATA.DAT"
    out_dir  = sys.argv[2] if len(sys.argv) > 2 else "assets_raw"
    data = open(src_path, "rb").read()
    try:
        info = archive_info(data)
    except ValueError as exc:
        sys.exit(f"Invalid DATA.DAT: {exc}")
    print_archive_info(info)
    first_off = struct.unpack_from("<I", data, 0)[0]
    n = first_off // 4
    offs = [struct.unpack_from("<I", data, i*4)[0] for i in range(n)]
    # sanity: offsets monotonically non-decreasing, last block ends at EOF
    prev = 0
    for i, o in enumerate(offs):
        if o < prev or o > len(data):
            sys.exit(f"Invalid DATA.DAT: offset {i} is out of order or beyond EOF ({o:#x})")
        prev = o
    os.makedirs(f"{out_dir}/chunks/comp", exist_ok=True)
    os.makedirs(f"{out_dir}/chunks/dec", exist_ok=True)
    manifest = []
    rebuilt = bytearray(data[:first_off])   # header table verbatim
    for i in range(n):
        start = offs[i]
        end = offs[i+1] if i+1 < n else len(data)
        comp = data[start:end]
        entry = {"id": i, "offset": start, "comp_size": len(comp)}
        if len(comp) >= 2:
            dsize = struct.unpack_from("<H", comp, 0)[0]
            dec = lzss_decompress(comp[2:], dsize)
            entry["decomp_size"] = dsize
            entry["decomp_got"] = len(dec)
            entry["sha_dec"] = hashlib.sha256(dec).hexdigest()[:16]
            open(f"{out_dir}/chunks/dec/{i:04d}.bin", "wb").write(dec)
        else:
            entry["decomp_size"] = 0
            entry["decomp_got"] = 0
        entry["sha_comp"] = hashlib.sha256(comp).hexdigest()[:16]
        entry["diagnostic"] = record_diagnostic(entry, info["reference"])
        open(f"{out_dir}/chunks/comp/{i:04d}.bin", "wb").write(comp)
        rebuilt += comp
        manifest.append(entry)
    # round-trip proof
    identical = bytes(rebuilt) == data
    json.dump({"source": src_path, "size": len(data), "chunks": n,
               "roundtrip_identical": identical, "archive_info": info, "entries": manifest},
              open(f"{out_dir}/manifest.json", "w"), indent=1)
    probes = [e for e in manifest if e["diagnostic"]["kind"] == "lzss_probe"]
    incomplete = [e for e in probes if e["diagnostic"]["status"] != "complete"]
    print(f"chunks={n} roundtrip_identical={identical} "
          f"lzss_probes={len(probes)} incomplete={len(incomplete)} "
          f"not_checked={n - len(probes)}")
    for entry in incomplete:
        diagnostic = entry["diagnostic"]
        print(f"  record 0x{entry['id']:04X}: {diagnostic['status']}, "
              f"got {entry['decomp_got']} bytes, expected {diagnostic.get('expected_size', 'unknown')}")
    if info["reference"] == "unknown":
        print("  Record types not classified for this archive; LZSS probes were not validated.")
    else:
        print("  LZSS probes include unreferenced records of uncertain type; "
              "a complete probe is not proof of integrity.")
    if not identical:
        sys.exit(1)

if __name__ == "__main__":
    main()
