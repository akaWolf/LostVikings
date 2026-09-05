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
L = json.load(open(os.path.join(ROOT, "tools/assets/bac_lv_locale.json")))
chars = set()
for code in CJK:
    for s in L["pc"][code].values():
        chars |= set(s)
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
