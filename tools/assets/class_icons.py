#!/usr/bin/env python3
"""class_icons.py — harvest per-class sprite icons from V2S1 save-states.

The engine has already composed every live object's sub-sprites into its
arenas; this re-renders them with the SAME strip model as
state_render.draw_sprites (verified against the game screen), but
per-object, cropped, into RGBA icons keyed by (script chunk, class).

Class attribution: OBJ_ANIM_IDX (ds:0x16ED+si) holds the spawn's class
index — sub_13e52 stores scratch_34 (= code_seg_idx) there. Exception:
slots 0/2/4 are the vikings BY ENGINE CONSTRUCTION — the viking health
table (VIK_HEALTH, word_29BCD+vk*2) aliases exactly those three cells of
the class column, which only works because vikings always occupy the
first three slots (they spawn first at level init, spawn order
code_seg 1,0,2). They are stored as pseudo-keys vik0/vik1/vik2.

Among multiple states the icon with the most opaque pixels wins (fullest
frame of the walk cycle).

Usage:
  python3 tools/assets/class_icons.py STATE... [-d build/levels_atlas/icons]
"""
import argparse
import json
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import level_render as LR  # noqa: E402
import state_render as SR  # noqa: E402

OBJ_CODE_SEG, OBJ_ANIM_IDX = 0x1355, 0x16ED
OBJ_SUB_SLOT, OBJ_SUB_END, OBJ_SUB_COUNT = 0x1A85, 0x1AAD, 0x1AD5
DS_LEVEL, DS_OBJ_COUNT = 0x25AD, 0x0372


def png_rgba(w, h, rgba):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += rgba[y * w * 4:(y + 1) * w * 4]

    def chunk(t, data):
        return (struct.pack(">I", len(data)) + t + data +
                struct.pack(">I", zlib.crc32(t + data) & 0xFFFFFFFF))
    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(bytes(raw), 9)) +
            chunk(b"IEND", b""))


def render_object(blocks, si, pal):
    """Render object si's sub-sprites, exactly the draw_sprites strip walk,
    into a cropped RGBA buffer. Returns (w, h, rgba, opaque) or None."""
    ds = blocks["DS  "]

    def w16(a):
        return ds[a] | (ds[a + 1] << 8)
    if w16(OBJ_SUB_COUNT + si) == 0:
        return None
    lo, hi = w16(OBJ_SUB_SLOT + si), w16(OBJ_SUB_END + si)
    if hi <= lo or hi > 0x100:
        return None
    seg_map = [
        (w16(0x2E73), blocks["SPRT"]), (w16(0x2E5D), blocks["GTLD"]),
        (w16(0x2E67), blocks["ANIM"]), (w16(0x2E5F), blocks["TGFX"]),
        (w16(0x2E61), blocks["GS  "]),
    ]
    px = {}
    for di in range(lo, hi, 2):
        flags = w16(0x44D + di)
        if not (flags & 0x8000):
            continue
        typ = flags & 7
        if typ not in SR.SPRITE_GEOM:
            continue
        nstr, rows, cols, wid = SR.SPRITE_GEOM[typ]
        if typ == 2:
            nstr = w16(0xC4D + di)
            if nstr == 0 or nstr > 0x100:
                continue
        wx, wy = w16(0x64D + di), w16(0x74D + di)
        if wx >= 0x8000:
            wx -= 0x10000
        if wy >= 0x8000:
            wy -= 0x10000
        soff, sseg = w16(0x84D + di), w16(0x94D + di)
        need = 4 * nstr * 9
        sprt, lin = None, 0
        for base, blk in seg_map:
            if base == 0 or sseg < base:
                continue
            cand = ((sseg - base) << 4) + soff
            if 1 <= cand and cand + need <= len(blk):
                sprt, lin = blk, cand
                break
        if sprt is None:
            continue
        hflip = (flags >> 9) & 1
        sect = nstr * 9
        for p in range(4):
            base = lin + p * sect
            for st in range(nstr):
                mask = sprt[base + st * 9 - 1]
                if not mask:
                    continue
                data = sprt[base + st * 9:base + st * 9 + 8]
                for b in range(8):
                    if not (mask & (0x80 >> b)):
                        continue
                    row = st * rows + (b // cols)
                    col = b % cols
                    x = col * 4 + p
                    if hflip:
                        x = (wid - 1) - x
                    px[(wx + x, wy + row)] = data[b]
    if not px:
        return None
    xs = [x for x, _ in px]
    ys = [y for _, y in px]
    x0, y0 = min(xs), min(ys)
    w, h = max(xs) - x0 + 1, max(ys) - y0 + 1
    if w > 128 or h > 128:
        return None
    rgba = bytearray(w * h * 4)
    for (x, y), c in px.items():
        o = ((y - y0) * w + (x - x0)) * 4
        r, g, b = pal[c]
        rgba[o:o + 4] = bytes((r, g, b, 255))
    return w, h, bytes(rgba), len(px)


def harvest(state_path):
    blocks = SR.load_state(state_path)
    ds = blocks["DS  "]

    def w16(a):
        return ds[a] | (ds[a + 1] << 8)
    dac = blocks["DAC "]
    pal = [(((dac[i * 3] << 2) | (dac[i * 3] >> 4)),
            ((dac[i * 3 + 1] << 2) | (dac[i * 3 + 1] >> 4)),
            ((dac[i * 3 + 2] << 2) | (dac[i * 3 + 2] >> 4)))
           for i in range(256)]
    level = w16(DS_LEVEL)
    tables = LR.level_tables()
    if level >= len(tables):
        return None, []
    script = tables[level][1]
    out = []
    count = min(w16(DS_OBJ_COUNT), 0x28)
    for si in range(0, count, 2):
        if w16(OBJ_CODE_SEG + si) == 0:
            continue
        icon = render_object(blocks, si, pal)
        if icon is None:
            continue
        if si in (0, 2, 4):
            key = f"vik{si // 2}"        # engine invariant, see module docstring
        else:
            key = f"{w16(OBJ_ANIM_IDX + si):02X}"
        out.append((key, icon))
    return script, out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("states", nargs="+")
    ap.add_argument("-d", "--outdir",
                    default=os.path.join(LR.ROOT, "build", "levels_atlas", "icons"))
    args = ap.parse_args()
    os.makedirs(args.outdir, exist_ok=True)
    man_path = os.path.join(args.outdir, "icons.json")
    manifest = {}
    if os.path.exists(man_path):
        with open(man_path) as f:
            manifest = json.load(f)
    for sp in args.states:
        script, icons = harvest(sp)
        if script is None:
            print(f"{sp}: unknown level, skipped")
            continue
        skey = f"{script:04X}"
        best = manifest.setdefault(skey, {})
        for key, (w, h, rgba, opaque) in icons:
            cur = best.get(key)
            if cur and cur.get("opaque", 0) >= opaque:
                continue
            fn = f"{skey}_{key}.png"
            with open(os.path.join(args.outdir, fn), "wb") as f:
                f.write(png_rgba(w, h, rgba))
            best[key] = {"file": fn, "w": w, "h": h, "opaque": opaque}
        print(f"{sp}: script {skey}, {len(icons)} objects")
    with open(man_path, "w") as f:
        json.dump(manifest, f, indent=1, sort_keys=True)
    total = sum(len(v) for v in manifest.values())
    print(f"manifest: {total} icons -> {man_path}")


if __name__ == "__main__":
    main()
