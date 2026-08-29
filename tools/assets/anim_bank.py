#!/usr/bin/env python3
"""anim_bank.py — compressed sprite frame banks (task #108 p10).

Role 'anim_bank' chunks hold the big animated sprites (vikings,
monsters). Loaded whole into the chunk buffer (sub_116ae, stripe
section 5); the anim VM selects one via op 0x356E (resource id ->
ds:0x124D table) and decompresses ONE FRAME per op 0x34DC:

  chunk = [u16 frame-offset directory][frames...]   (offsets from the
  chunk start; directory length = min(offset)/2)

  frame = 128 strips, each: [mask u8][packed 4-bit pixels]
    walk mask bits 7..0; a set bit consumes one nibble; nibbles come
    in (hi, lo) pairs from each source byte; a CLEAR bit does NOT
    reset the pair state (the pending lo nibble survives zeros).
    Output pixel = nibble | runtime flags ((0x70 & sprite flags)|0x80
    — the palette layer bits are added at draw time, so stored colors
    are 4-bit). Decompressed strip = mask + 8 bytes -> 1152B/frame.

decode/encode are exact mirrors of the op 0x34DC loop; `judge` proves
encode(decode(x)) == x for every frame of every anim_bank chunk.
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from assetc import read_payload  # noqa: E402

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")


def frame_dir(data):
    """[(offset)] frame directory; length = min(offset)/2."""
    first = data[0] | (data[1] << 8)
    n = first // 2
    ptrs = []
    for i in range(n):
        p = data[i * 2] | (data[i * 2 + 1] << 8)
        ptrs.append(p)
    return ptrs


def decode_frame(data, off):
    """-> (strips, consumed): strips = 128 x (mask, [8 nibble-or-None],
    pad), consumed = source bytes eaten. `pad` is the UNCONSUMED lo
    nibble of the strip's last data byte (the engine never reads it;
    Blizzard's packer left garbage there — kept for byte-exactness)."""
    i = off
    strips = []
    for _ in range(128):
        mask = data[i]
        i += 1
        px = [None] * 8
        pending = None
        for b in range(8):
            if mask & (0x80 >> b):
                if pending is None:
                    byte = data[i]
                    i += 1
                    px[b] = byte >> 4
                    pending = byte & 0x0F
                else:
                    px[b] = pending
                    pending = None
        strips.append((mask, px, pending if pending is not None else 0))
    return strips, i - off


def encode_frame(strips):
    out = bytearray()
    for strip in strips:
        mask, px = strip[0], strip[1]
        pad = strip[2] if len(strip) > 2 else 0
        out.append(mask)
        hi = None
        for b in range(8):
            if mask & (0x80 >> b):
                v = px[b] & 0x0F
                if hi is None:
                    hi = v
                else:
                    out.append((hi << 4) | v)
                    hi = None
        if hi is not None:
            out.append((hi << 4) | (pad & 0x0F))
    return bytes(out)


def chunk_to_frames(data):
    """{frames:[{ptr, strips}], orphans:[{ptr,hex}], len} — the orphans are
    chunk bytes covered by no frame (tail FF padding etc), kept verbatim."""
    frames = []
    cov = bytearray(len(data))
    ptrs = frame_dir(data)
    for i in range(len(ptrs) * 2):
        cov[i] = 1
    for p in ptrs:
        strips, consumed = decode_frame(data, p)
        for i in range(p, p + consumed):
            cov[i] = 1
        frames.append({"ptr": p,
                       "strips": [[m, [(-1 if v is None else v) for v in px],
                                   pad] for m, px, pad in strips]})
    orphans = []
    i = 0
    while i < len(data):
        if cov[i]:
            i += 1
            continue
        j = i
        while j < len(data) and not cov[j]:
            j += 1
        orphans.append({"ptr": i, "hex": bytes(data[i:j]).hex()})
        i = j
    return {"frames": frames, "orphans": orphans, "len": len(data)}


def frames_to_chunk(js):
    """Rebuild the chunk: directory + frame blobs + orphan blobs, laid in
    the original offset order (byte-stable for untouched data; edited
    frames repack the layout). Duplicate-offset frames stay shared unless
    their content diverged."""
    frames = js["frames"]
    n = len(frames)
    enc = []
    for f in frames:
        strips = [(st[0], [None if v < 0 else v for v in st[1]], st[2])
                  for st in f["strips"]]
        enc.append(encode_frame(strips))
    items = []                                     # (orig_ptr, key, blob)
    seen = {}
    for f, e in zip(frames, enc):
        key = f["ptr"]
        if key in seen and seen[key] != e:
            key = (f["ptr"], id(f))                # alias broken by an edit
        if key not in seen:
            seen[key] = e
            items.append((f["ptr"], key, e))
    for o in js.get("orphans", []):
        items.append((o["ptr"], ("o", o["ptr"]), bytes.fromhex(o["hex"])))
    items.sort(key=lambda t: t[0])
    laid = {}
    pos = n * 2
    for _, key, blob in items:
        laid[key] = pos
        pos += len(blob)
    ptrs = []
    for f, e in zip(frames, enc):
        key = f["ptr"]
        if key in seen and seen[key] != e:
            key = (f["ptr"], id(f))
        ptrs.append(laid[key])
    out = bytearray()
    for p in ptrs:
        out += bytes((p & 0xFF, p >> 8))
    for _, key, blob in items:
        assert len(out) == laid[key], "layout gap"
        out += blob
    return bytes(out)


def judge_chunks():
    cm = json.load(open(os.path.join(ROOT, "assets_raw", "chunk_map.json")))
    banks = sorted(int(c, 16) for c, i in cm.items()
                   if i["roles"][0] == "anim_bank")
    ok = 0
    for cid in banks:
        data, _ = read_payload(cid, "lzss")
        js = chunk_to_frames(data)
        back = frames_to_chunk(js)
        if back == bytes(data):
            ok += 1
        else:
            i = next((i for i, (a, b) in enumerate(zip(back, data))
                      if a != b), min(len(back), len(data)))
            print(f"  DIFF {cid:04X}: len {len(back)} vs {len(data)}, "
                  f"first @{i:04X}")
    print(f"judge chunks: {ok}/{len(banks)}")
    return ok == len(banks)


def judge():
    cm = json.load(open(os.path.join(ROOT, "assets_raw", "chunk_map.json")))
    banks = sorted(int(c, 16) for c, i in cm.items()
                   if i["roles"][0] == "anim_bank")
    total = bad = 0
    for cid in banks:
        data, _ = read_payload(cid, "lzss")
        ptrs = frame_dir(data)
        for k, p in enumerate(ptrs):
            strips, consumed = decode_frame(data, p)
            back = encode_frame(strips)
            total += 1
            if back != bytes(data[p:p + consumed]):
                bad += 1
                print(f"  DIFF {cid:04X} frame {k} @{p:04X} "
                      f"({consumed}B vs {len(back)}B)")
    print(f"judge: {total - bad}/{total} frames byte-exact "
          f"across {len(banks)} anim banks")
    return bad == 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["judge", "judge_chunks", "info"])
    ap.add_argument("chunk", nargs="?")
    args = ap.parse_args()
    if args.cmd == "judge":
        sys.exit(0 if judge() else 1)
    if args.cmd == "judge_chunks":
        sys.exit(0 if judge_chunks() else 1)
    cid = int(args.chunk, 16)
    data, _ = read_payload(cid, "lzss")
    ptrs = frame_dir(data)
    print(f"{cid:04X}: {len(data)}B, {len(ptrs)} frames")
    for k, p in enumerate(ptrs[:10]):
        _, consumed = decode_frame(data, p)
        print(f"  frame {k}: @{p:04X} {consumed}B")


if __name__ == "__main__":
    main()
