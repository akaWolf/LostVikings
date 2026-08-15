#!/usr/bin/env python3
"""Stage 1.1 (ENGINE_REWRITE_ROADMAP): DATA.DAT extractor + manifest.

Format (canonized by v2_read_chunk / orig sub_10982):
  header: dword offsets at chunk_id*4; chunk count = first_offset/4
  chunk:  [u16 decompressed_size][LZSS stream]
  compressed_size(id) = offset(id+1) - offset(id)   (includes the u16)
  LZSS: flag byte, bit=1 literal, bit=0 back-ref (12-bit ring offset,
        4-bit len+3), 4KB zero-initialized ring, stop on dest underflow.

Outputs (default assets_raw/):
  manifest.json           id, offset, comp/decomp sizes, sha256 of both
  chunks/comp/NNNN.bin    raw compressed stream (incl. the u16 header)
  chunks/dec/NNNN.bin     decompressed payload
Round-trip proof: re-concatenation of header table + comp blocks must be
byte-identical to DATA.DAT (verified here, aborts otherwise).
"""
import sys, os, json, hashlib, struct

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

def main():
    src_path = sys.argv[1] if len(sys.argv) > 1 else "DATA.DAT"
    out_dir  = sys.argv[2] if len(sys.argv) > 2 else "assets_raw"
    data = open(src_path, "rb").read()
    first_off = struct.unpack_from("<I", data, 0)[0]
    n = first_off // 4
    offs = [struct.unpack_from("<I", data, i*4)[0] for i in range(n)]
    # sanity: offsets monotonically non-decreasing, last block ends at EOF
    prev = 0
    for i, o in enumerate(offs):
        assert o >= prev, f"non-monotonic offset at {i}"
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
        open(f"{out_dir}/chunks/comp/{i:04d}.bin", "wb").write(comp)
        rebuilt += comp
        manifest.append(entry)
    # round-trip proof
    identical = bytes(rebuilt) == data
    json.dump({"source": src_path, "size": len(data), "chunks": n,
               "roundtrip_identical": identical, "entries": manifest},
              open(f"{out_dir}/manifest.json", "w"), indent=1)
    short = sum(1 for e in manifest if e["decomp_got"] != e["decomp_size"])
    print(f"chunks={n} roundtrip_identical={identical} "
          f"decomp_mismatch={short}")
    if not identical:
        sys.exit(1)

if __name__ == "__main__":
    main()
