#!/usr/bin/env python3
"""state_render.py — viewer v1 (task #103): compose the level image from a
V2 save-state (V2S1) EXACTLY as the engine draws it — zero re-implementation:

  FS block   : the engine's TRANSLATED render map (the dd9c cascade output);
               u16 per cell, bits 15..6 = byte offset into the tile-GFX
               segment, bit4 = hflip, bit5 = vflip (verified 1689E blit)
  TGFX block : the whole tile-GFX segment (64B tiles, Mode-X byteplanes)
  DAC block  : the final 768-byte palette the engine programmed (6-bit VGA)
  DS block   : level header stripe @25B3 (width @+0x29, height @+0x2B)

Produce a state file with any V2_ONLY run:
  V2_SAVE_STATE=/tmp/x.state ./vikings --replay ... --max-frames N

Usage:
  python3 tools/assets/state_render.py /tmp/x.state [-o out.png] [--raw-map]
    --raw-map renders the UNtranslated TMAP block instead (debug aid).
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from assetc import tile_decode, png_write  # noqa: E402


def load_state(path):
    d = open(path, "rb").read()
    if d[:4] != b"V2S1":
        raise SystemExit(f"{path}: not a V2S1 state file")
    n = struct.unpack("<I", d[4:8])[0]
    off = 8
    blocks = {}
    for _ in range(n):
        tag = d[off:off + 4].decode()
        ln = struct.unpack("<I", d[off + 4:off + 8])[0]
        blocks[tag] = d[off + 8:off + 8 + ln]
        off += 8 + ln
    return blocks


# Verified object/sprite model (stage-4 records + seg003 renderers):
#   object di=0x00..0xFE step 2; flags ds:[di+0x44D]: bit15 active,
#   bits13-14 priority, bit9 hflip, bits0-2 type; world x/y ds:[di+0x64D]/
#   [di+0x74D]; sprite far ptr ds:[di+0x84D]/[di+0x94D]; type-2 strip count
#   ds:[di+0xC4D]. Data: 4 plane sections x (N strips x [mask][8 data]);
#   the stored pointer aims at the FIRST DATA byte (mask sits at -1).
#   type 1: N=2, 4 rows/strip, 2 cols/row (8x8);  mask bits 7,6->r0 ...
#   type 2: N=cnt, 1 row/strip, 8 cols (32xN);    mask bits 7..0 -> cols
#   type 4: N=8, 2 rows/strip, 4 cols/row (16x16) mask 7..4->r0, 3..0->r1
#   pixel x = col*4 + plane (hflip mirrors across the sprite width).
SPRITE_GEOM = {1: (2, 4, 2, 8), 2: (None, 1, 8, 32), 4: (8, 2, 4, 16)}


def draw_sprites(img, stride, hgt_px, blocks):
    ds = blocks["DS  "]

    def w16(a):
        return ds[a] | (ds[a + 1] << 8)

    # segment map: object sprite pointers resolve into whichever shadow
    # arena their segment word names (vikings' frames live in the tiledata
    # arena — the anim system composes them there)
    seg_map = [
        (w16(0x2E73), blocks["SPRT"]),            # DS_SEG_SPRITE
        (w16(0x2E5D), blocks["GTLD"]),            # DS_SEG_TILEDATA
        (w16(0x2E67), blocks["ANIM"]),            # DS_SEG_ANIM
        (w16(0x2E5F), blocks["TGFX"]),            # DS_SEG_TILEGFX
        (w16(0x2E61), blocks["GS  "]),            # DS_SEG_GS
    ]
    drawn = 0
    for di in range(0xFE, -2, -2):                # engine order: reverse scan
        flags = w16(0x44D + di)
        if not (flags & 0x8000):
            continue
        typ = flags & 7
        if typ not in SPRITE_GEOM:
            continue
        nstr, rows, cols, wid = SPRITE_GEOM[typ]
        if typ == 2:
            nstr = w16(0xC4D + di)
            if nstr == 0 or nstr > 0x100:
                continue
        wx = w16(0x64D + di)
        wy = w16(0x74D + di)
        if wx >= 0x8000: wx -= 0x10000
        if wy >= 0x8000: wy -= 0x10000
        soff = w16(0x84D + di)
        sseg = w16(0x94D + di)
        need = 4 * nstr * 9
        sprt = None
        lin = 0
        for base, blk in seg_map:
            if base == 0 or sseg < base:
                continue
            cand = ((sseg - base) << 4) + soff
            if 1 <= cand and cand + need <= len(blk):
                sprt = blk
                lin = cand
                break
        if sprt is None:
            continue
        hflip = (flags >> 9) & 1
        sect = nstr * 9
        for p in range(4):
            base = lin + p * sect
            for st in range(nstr):
                mask = sprt[base + st * 9 - 1]
                data = sprt[base + st * 9:base + st * 9 + 8]
                if not mask:
                    continue
                for b in range(8):
                    if not (mask & (0x80 >> b)):
                        continue
                    row = st * rows + (b // cols)
                    col = b % cols
                    x = col * 4 + p
                    if hflip:
                        x = (wid - 1) - x
                    px_x = wx + x
                    px_y = wy + row
                    if 0 <= px_x < stride and 0 <= px_y < hgt_px:
                        img[px_y * stride + px_x] = data[b]
        drawn += 1
    return drawn


def render(blocks, use_raw_map=False):
    ds = blocks["DS  "]

    def w16(a):
        return ds[a] | (ds[a + 1] << 8)

    # header stripe +0x29/+0x2B are QUAD dimensions (16x16 metatiles) —
    # sub_173C7 iterates exactly these as cols/rows; the cell map is 2x
    # in both axes (verified: fresh-engine FS word-compare, 100% base match)
    qw, qh = w16(0x25B3 + 0x29), w16(0x25B3 + 0x2B)
    width, height = qw * 2, qh * 2
    if use_raw_map == "pristine":
        # sub_173C7 expansion: the RAW tilemap is QUAD-based — each u16
        # (& 0x3FF) indexes an 8-byte gs_tiledata template = 4 draw-words
        # for a 2x2 cell block (top: e[0],e[2]; bottom: e[4],e[6]).
        # The runtime FS then gets mutated (animation stamps, pickups) —
        # this rebuilds the pristine render map the level starts with.
        # the state's GTLD/TMAP arenas get reused at runtime (sprite frames
        # land in the tiledata arena) — take the PRISTINE chunks instead,
        # by the ids the level header stripe names (+0x2E map, +0x32 bg)
        from assetc import read_payload
        tmap, _ = read_payload(w16(0x25B3 + 0x2E), "lzss")
        gtld, _ = read_payload(w16(0x25B3 + 0x32), "lzss")
        qc, qr = qw, qh
        fs = bytearray(width * height * 2)
        for qy in range(qr):
            for qx in range(qc):
                wv = tmap[(qy * qc + qx) * 2] | (tmap[(qy * qc + qx) * 2 + 1] << 8)
                si = (wv & 0x3FF) << 3
                e = gtld[si:si + 8]
                for (dy, dx, o) in ((0, 0, 0), (0, 1, 2), (1, 0, 4), (1, 1, 6)):
                    cell = (qy * 2 + dy) * width + qx * 2 + dx
                    fs[cell * 2] = e[o]
                    fs[cell * 2 + 1] = e[o + 1]
        fs = bytes(fs)
    else:
        fs = blocks["TMAP"] if use_raw_map else blocks["FS  "]
    tgfx = blocks["TGFX"]
    dac = blocks["DAC "]
    pal = []
    for i in range(256):
        r, g, b = dac[i * 3], dac[i * 3 + 1], dac[i * 3 + 2]
        pal.append(((r << 2) | (r >> 4), (g << 2) | (g >> 4), (b << 2) | (b >> 4)))

    tile_cache = {}
    stride = width * 8
    img = bytearray(stride * height * 8)
    for cell in range(width * height):
        wv = fs[cell * 2] | (fs[cell * 2 + 1] << 8)
        toff = wv & 0xFFC0
        hflip = (wv >> 4) & 1
        vflip = (wv >> 5) & 1
        px = tile_cache.get(toff)
        if px is None:
            px = tile_decode(tgfx[toff:toff + 64])
            tile_cache[toff] = px
        cx = (cell % width) * 8
        cy = (cell // width) * 8
        for y in range(8):
            sy = 7 - y if vflip else y
            row = px[sy * 8:sy * 8 + 8]
            if hflip:
                row = row[::-1]
            img[(cy + y) * stride + cx:(cy + y) * stride + cx + 8] = row
    nspr = draw_sprites(img, stride, height * 8, blocks)
    return png_write(stride, height * 8, bytes(img), pal), width, height, nspr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("state", help="V2S1 save-state file")
    ap.add_argument("-o", "--out", help="output PNG")
    ap.add_argument("--raw-map", action="store_true",
                    help="render the raw TMAP words instead of the FS render map")
    ap.add_argument("--live", action="store_true",
                    help="render the LIVE FS map (runtime stamps/pickups) "
                         "instead of the pristine quad expansion")
    args = ap.parse_args()
    blocks = load_state(args.state)
    mode = True if args.raw_map else (False if args.live else "pristine")
    png, w, h, nspr = render(blocks, mode)
    out = args.out or (os.path.splitext(args.state)[0] +
                       ("_raw.png" if args.raw_map else ".png"))
    with open(out, "wb") as f:
        f.write(png)
    print(f"{args.state}: {w}x{h} cells, {nspr} sprites -> {out}")


if __name__ == "__main__":
    main()
