#!/usr/bin/env python3
"""unifont_glyphs.py — rasterise GNU Unifont (fonts/unifont.otf, 16 px pixel
font, GPLv2+ with font exception / OFL dual) at 16 px into 16-row bitmaps
for every character of the CJK-language Lost Vikings strings (ja, ko,
zh-CN, zh-TW in tools/assets/bac_lv_locale.json) and cache them in
tools/assets/unifont16_glyphs.json as {char: {"w": 8|16, "rows": [16 ints,
bit 15 = left]}}. Needs Pillow:
  nix-shell -p python3Packages.pillow --run "python3 tools/assets/unifont_glyphs.py"
"""
import json, os
from PIL import Image, ImageFont, ImageDraw
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
CJK = ["ja", "ko", "zh-CN", "zh-TW"]
# The translation table is a build artifact now (build_content.py writes it
# into the content tree from the user's collection; it is not in the repo):
# content/bac_lv_locale.json, or the path given as the first argument. The
# characters: the CJK strings of the EXE table (menu words included) and the
# scene lines of the Genesis interludes, read from the content image at
# SCENE_TEXT_BANK (integrate_snes.write_scene_texts) and looked up in the
# table's pool by their normalised English text (build_locale.banks extra).
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bac_strings import norm
TABLE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "content/bac_lv_locale.json")
IMAGE = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "content/exe_static.bin")
L = json.load(open(TABLE))
chars = set()
for code in CJK:
    for s in L["pc"][code].values():
        if s: chars |= set(s)
if os.path.exists(IMAGE) and "pool" in L:
    img = open(IMAGE, "rb").read(); base = 0x10BC0            # SCENE_TEXT_BANK
    n = 18; ptrs = [int.from_bytes(img[base + 2 * k: base + 2 * k + 2], "little") for k in range(n)]
    for p in ptrs:
        q = 0x9480 + p + 2; e = q
        while img[e]: e += 1
        row = L["pool"].get(norm(img[q:e].decode("latin-1").replace("\r", "\n")), {})
        for code in CJK:
            if row.get(code): chars |= set(row[code])
chars |= set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .,!?'-:;()/")
chars = {c for c in chars if c not in "\n\r"}
font = ImageFont.truetype(os.path.join(ROOT, "fonts/unifont.otf"), 16)
out, bad = {}, []
for c in sorted(chars):
    im = Image.new("L", (32, 24), 0)
    ImageDraw.Draw(im).text((0, 0), c, font=font, fill=255)
    cols = [x for x in range(32) if any(im.getpixel((x, y)) > 127 for y in range(24))]
    rows_used = [y for y in range(24) if any(im.getpixel((x, y)) > 127 for x in range(32))]
    w = 16 if (cols and cols[-1] >= 8) else 8
    if c == " " or c == "　":
        w = 16 if c == "　" else 8
    if rows_used and (rows_used[-1] > 15 or (cols and cols[-1] > 15)):
        bad.append(c)
    rows = []
    for y in range(16):
        v = 0
        for x in range(16):
            if im.getpixel((x, y)) > 127:
                v |= 0x8000 >> x
        rows.append(v)
    out[c] = {"w": w, "rows": rows}
json.dump(out, open(os.path.join(ROOT, "tools/assets/unifont16_glyphs.json"), "w"), ensure_ascii=False)
print(f"{len(out)} glyphs cached ({sum(1 for v in out.values() if v['w'] == 16)} wide); spilling 16x16: {bad[:20]}")
