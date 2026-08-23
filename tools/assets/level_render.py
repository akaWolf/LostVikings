#!/usr/bin/env python3
"""level_render.py — standalone level renderer (task #103, v2): compose a
full-level PNG from assets/ chunks ONLY — no game runs, no save-states.
Every step mirrors verified engine code:

  header stripe (chunk): width/height in 16x16 QUADS @+0x29/+0x2B,
      tilemap/tileset/quad-template chunk ids @+0x2E/+0x30/+0x32,
      spawn table @+0x43 (14-byte records, 0xFFFF-terminated), then the
      PALETTE LIST: 3-byte {chunk_id:u16, start_color:u8} entries,
      0xFFFF-terminated (sub_112ae)
  map expansion (sub_173C7): tilemap word & 0x3FF -> 8-byte template in
      the quad-template chunk = 4 draw-words for a 2x2 cell block
  draw word (sub_1689E): bits 15..6 byte offset into the tileset,
      bit4 hflip, bit5 vflip; tile = 64B Mode-X byteplanes
  palette (sub_112ae tail + sub_10E99): apply entries into a 768B DAC at
      start*3 (variable-length chunks), black out colors 16k (k=1..15),
      identity correction at full brightness (fade offsets 0). Palette-
      animated ranges show their frame-0 (the game cycles them at runtime).

Usage:
  python3 tools/assets/level_render.py 0028 [-o out.png]
  python3 tools/assets/level_render.py --all [-d outdir]
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from assetc import read_payload, tile_decode, png_write  # noqa: E402

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
HDR_DIR = os.path.join(ROOT, "assets", "level_headers")


def parse_header(raw):
    def w16(o):
        return raw[o] | (raw[o + 1] << 8)
    qw, qh = w16(0x29), w16(0x2B)
    tm_id, ts_id, gt_id = w16(0x2E), w16(0x30), w16(0x32)
    # spawn table @+0x43: 0x0E-byte records until 0xFFFF, then +2, then the
    # palette list (3-byte entries until 0xFFFF)
    di = 0x43
    while w16(di) != 0xFFFF:
        di += 0x0E
    di += 2
    pal_entries = []
    while w16(di) != 0xFFFF:
        pal_entries.append((w16(di), raw[di + 2]))
        di += 3
    return qw, qh, tm_id, ts_id, gt_id, pal_entries


def compose_palette(entries):
    pal = bytearray(768)
    for cid, start in entries:
        data, _ = read_payload(cid, "lzss")
        pal[start * 3:start * 3 + len(data)] = data
    for k in range(1, 16):                        # sub_112ae blackouts
        pal[k * 16 * 3:k * 16 * 3 + 3] = b"\x00\x00\x00"
    return [((pal[i * 3] << 2) | (pal[i * 3] >> 4),
             (pal[i * 3 + 1] << 2) | (pal[i * 3 + 1] >> 4),
             (pal[i * 3 + 2] << 2) | (pal[i * 3 + 2] >> 4)) for i in range(256)]


def render(hdr_cid_hex):
    with open(os.path.join(HDR_DIR, f"{hdr_cid_hex}.json")) as f:
        raw = bytes.fromhex(json.load(f)["raw"])
    qw, qh, tm_id, ts_id, gt_id, pal_entries = parse_header(raw)
    tmap, _ = read_payload(tm_id, "lzss")
    tgfx, _ = read_payload(ts_id, "lzss")
    gtld, _ = read_payload(gt_id, "lzss")
    pal = compose_palette(pal_entries)
    width, height = qw * 2, qh * 2
    stride = width * 8
    img = bytearray(stride * height * 8)
    tile_cache = {}
    for qy in range(qh):
        for qx in range(qw):
            wv = tmap[(qy * qw + qx) * 2] | (tmap[(qy * qw + qx) * 2 + 1] << 8)
            e = gtld[(wv & 0x3FF) << 3:((wv & 0x3FF) << 3) + 8]
            for (dy, dx, o) in ((0, 0, 0), (0, 1, 2), (1, 0, 4), (1, 1, 6)):
                dw = e[o] | (e[o + 1] << 8)
                toff = dw & 0xFFC0
                px = tile_cache.get(toff)
                if px is None:
                    px = tile_decode(tgfx[toff:toff + 64].ljust(64, b"\x00"))
                    tile_cache[toff] = px
                hflip = (dw >> 4) & 1
                vflip = (dw >> 5) & 1
                cx = (qx * 2 + dx) * 8
                cy = (qy * 2 + dy) * 8
                for y in range(8):
                    sy = 7 - y if vflip else y
                    row = px[sy * 8:sy * 8 + 8]
                    if hflip:
                        row = row[::-1]
                    img[(cy + y) * stride + cx:(cy + y) * stride + cx + 8] = row
    return png_write(stride, height * 8, bytes(img), pal), width, height


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("header", nargs="?", help="level header chunk id (hex)")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("-o", "--out")
    ap.add_argument("-d", "--outdir", default="/tmp/levels")
    args = ap.parse_args()
    if args.all:
        os.makedirs(args.outdir, exist_ok=True)
        for fn in sorted(os.listdir(HDR_DIR)):
            if not fn.endswith(".json"):
                continue
            cid = fn[:-5]
            try:
                png, w, h = render(cid)
            except Exception as e:
                print(f"{cid}: SKIP ({e})")
                continue
            out = os.path.join(args.outdir, f"level_{cid}.png")
            with open(out, "wb") as f:
                f.write(png)
            print(f"{cid}: {w}x{h} cells -> {out}")
        return
    if not args.header:
        ap.error("header id or --all required")
    png, w, h = render(args.header)
    out = args.out or f"/tmp/level_{args.header}.png"
    with open(out, "wb") as f:
        f.write(png)
    print(f"{args.header}: {w}x{h} cells -> {out}")


if __name__ == "__main__":
    main()
