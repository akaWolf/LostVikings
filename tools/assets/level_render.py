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

# --- level order + script (class-table) chunk tables from the static EXE ---
# DS linear base inside exe_static.bin = 0x19F00 (proven: the level-order
# word table ds:0x940C lands at file 0x2330C and matches the 42 known level
# header ids). sub_111b1 reads: header chunk = ds:[level*2-0x6BF4]
# (=0x940C wrapped), template chunk = ds:[level*2-0x6B94] (=0x946C) — the
# "template" chunk IS the level_script (.lvs) chunk; its first bytes are
# the CLASS TABLE (0x15-byte records, sub_13e52 layout).
_EXE_DS = 0x19F00
_LVL_TBL, _SCRIPT_TBL = 0x940C, 0x946C


def level_tables():
    """[(header_chunk, script_chunk)] in game level order (42 entries)."""
    exe = os.path.join(ROOT, "exe_static.bin")
    with open(exe, "rb") as f:
        d = f.read()
    def w(base, i):
        o = _EXE_DS + base + i * 2
        return d[o] | (d[o + 1] << 8)
    return [(w(_LVL_TBL, i), w(_SCRIPT_TBL, i)) for i in range(42)]


def script_for_header(hdr_cid):
    for hc, sc in level_tables():
        if hc == hdr_cid:
            return sc if sc != 0xFFFF else None
    return None


def class_record(script_raw, cls):
    """sub_13e52 template record (0x15 bytes at cls*0x15) — verified layout:
    +0 sprite chunk id (0xFFFF none/invisible, 0xFFFE pool sprite),
    +2 sub-sprite count (bit7: pool+2), +3 object PC (word, +3 applied at
    spawn), +7 res handle, +9 width, +0xA height, +0xB res cost,
    +0xD state idx, +0xF class bits, +0x11/+0x13 vel maxes."""
    o = cls * 0x15
    if o + 0x15 > len(script_raw):
        return None
    def w(off):
        return script_raw[o + off] | (script_raw[o + off + 1] << 8)
    return {
        "spr": w(0), "sub": script_raw[o + 2] & 0x7F,
        "pool_plus2": (script_raw[o + 2] >> 7) & 1,
        "pc": w(3), "w": script_raw[o + 9], "h": script_raw[o + 0xA],
        "state": w(0xD), "bits": w(0xF),
    }


def parse_header(raw):
    def w16(o):
        return raw[o] | (raw[o + 1] << 8)
    qw, qh = w16(0x29), w16(0x2B)
    tm_id, ts_id, gt_id = w16(0x2E), w16(0x30), w16(0x32)
    # spawn table @+0x43: 0x0E-byte records until 0xFFFF (sub_13bbd layout:
    # +0 x, +2 y, +4/+6 params, +8 class index, +0xA anim word — bit 0x800 =
    # permanent spawn, +0xC pool select), then +2, then the palette list
    # (3-byte {chunk, start_color} entries until 0xFFFF)
    di = 0x43
    spawns = []
    while w16(di) != 0xFFFF:
        spawns.append({
            "x": w16(di), "y": w16(di + 2),
            "p1": w16(di + 4), "p2": w16(di + 6),
            "cls": w16(di + 8), "anim": w16(di + 10),
            "pool": w16(di + 12),
        })
        di += 0x0E
    di += 2
    pal_entries = []
    while w16(di) != 0xFFFF:
        pal_entries.append((w16(di), raw[di + 2]))
        di += 3
    return qw, qh, tm_id, ts_id, gt_id, pal_entries, spawns


# 4x6 hex glyphs for overlay labels (1 = pixel set), plain ASCII-art rows.
_HEX_FONT = {c: g.split() for c, g in {
    "0": "0110 1001 1001 1001 1001 0110", "1": "0010 0110 0010 0010 0010 0111",
    "2": "0110 1001 0001 0110 1000 1111", "3": "1110 0001 0110 0001 1001 0110",
    "4": "1001 1001 1111 0001 0001 0001", "5": "1111 1000 1110 0001 1001 0110",
    "6": "0110 1000 1110 1001 1001 0110", "7": "1111 0001 0010 0010 0100 0100",
    "8": "0110 1001 0110 1001 1001 0110", "9": "0110 1001 1001 0111 0001 0110",
    "A": "0110 1001 1001 1111 1001 1001", "B": "1110 1001 1110 1001 1001 1110",
    "C": "0110 1001 1000 1000 1001 0110", "D": "1110 1001 1001 1001 1001 1110",
    "E": "1111 1000 1110 1000 1000 1111", "F": "1111 1000 1110 1000 1000 1000",
}.items()}


def draw_text(img, stride, hpx, x, y, text, color):
    for ch in text:
        g = _HEX_FONT.get(ch)
        if g is None:
            x += 5
            continue
        for gy, row in enumerate(g):
            for gx, bit in enumerate(row):
                if bit == "1" and 0 <= x + gx < stride and 0 <= y + gy < hpx:
                    img[(y + gy) * stride + x + gx] = color
        x += 5


def draw_overlay(img, stride, hpx, spawns, grid, marker, marker2):
    if grid:
        # quad-grid: small crosses at 16px intersections only (keeps the
        # art readable; a full line grid drowned it)
        for y in range(0, hpx, 16):
            for x in range(0, stride, 16):
                img[y * stride + x] = marker2
                if x + 1 < stride:
                    img[y * stride + x + 1] = marker2
                if y + 1 < hpx:
                    img[(y + 1) * stride + x] = marker2
    for sp in spawns:
        x, y = sp["x"], sp["y"]
        if x >= stride or y >= hpx:
            continue
        perm = sp["anim"] & 0x800
        c = marker if not perm else marker2
        for d in range(-3, 4):                    # crosshair at the spawn point
            if 0 <= x + d < stride:
                img[y * stride + x + d] = c
            if 0 <= y + d < hpx:
                img[(y + d) * stride + x] = c
        draw_text(img, stride, hpx, x + 2, y - 7, f"{sp['cls']:X}", marker)


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
    qw, qh, tm_id, ts_id, gt_id, pal_entries, spawns = parse_header(raw)
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


def render_overlay(hdr_cid_hex, grid=True):
    with open(os.path.join(HDR_DIR, f"{hdr_cid_hex}.json")) as f:
        raw = bytes.fromhex(json.load(f)["raw"])
    qw, qh, tm_id, ts_id, gt_id, pal_entries, spawns = parse_header(raw)
    tmap, _ = read_payload(tm_id, "lzss")
    tgfx, _ = read_payload(ts_id, "lzss")
    gtld, _ = read_payload(gt_id, "lzss")
    pal = compose_palette(pal_entries)
    # overlay colors: repurpose the two visually loudest palette slots
    pal = pal[:254] + [(255, 64, 255), (64, 255, 64)]
    marker, marker2 = 254, 255
    width, height = qw * 2, qh * 2
    stride, hpx = width * 8, height * 8
    img = bytearray(stride * hpx)
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
    draw_overlay(img, stride, hpx, spawns, grid, marker, marker2)
    return png_write(stride, hpx, bytes(img), pal), width, height, len(spawns)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("header", nargs="?", help="level header chunk id (hex)")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--overlay", action="store_true",
                    help="spawn markers (class ids) + 16px quad grid")
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
    if args.overlay:
        png, w, h, nsp = render_overlay(args.header)
        print(f"spawns: {nsp}")
    else:
        png, w, h = render(args.header)
    out = args.out or f"/tmp/level_{args.header}.png"
    with open(out, "wb") as f:
        f.write(png)
    print(f"{args.header}: {w}x{h} cells -> {out}")


if __name__ == "__main__":
    main()
