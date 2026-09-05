#!/usr/bin/env python3
"""ps2p_glyphs.py — rasterise Press Start 2P (fonts/PressStart2P-Regular.ttf,
OFL) at its native 8 px into 8x8 bitmaps for every character the localised
Lost Vikings strings use (tools/assets/bac_lv_locale.json, the Latin and
Cyrillic languages) and cache them in tools/assets/ps2p_glyphs.json as
{char: [8 row bitmasks, bit 7 = left]}. Needs Pillow:
  nix-shell -p python3Packages.pillow --run "python3 tools/assets/ps2p_glyphs.py"
build_locale.py reads the cache, so the mod build itself needs no Pillow."""
import json, os
from PIL import Image, ImageFont, ImageDraw
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
LATIN = ["de", "en", "es", "es-MX", "fr", "it", "pl", "pt-BR", "ru"]
L = json.load(open(os.path.join(ROOT, "tools/assets/bac_lv_locale.json")))
chars = set()
for code in LATIN:
    for s in L["pc"][code].values():
        chars |= set(s)
chars |= set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .,!?'-:;()/")
chars = {c for c in chars if c not in "\n\r"}
font = ImageFont.truetype(os.path.join(ROOT, "fonts/PressStart2P-Regular.ttf"), 8)
out, missing = {}, []
for c in sorted(chars):
    im = Image.new("L", (16, 16), 0)
    ImageDraw.Draw(im).text((0, 0), c, font=font, fill=255)
    rows = []
    for y in range(8):
        v = 0
        for x in range(8):
            if im.getpixel((x, y)) > 127:
                v |= 0x80 >> x
        rows.append(v)
    if any(im.getpixel((x, y)) > 127 for y in range(16) for x in range(16) if x >= 8 or y >= 8):
        missing.append(c)            # spills the 8x8 cell
    if not any(rows) and c != " " and c != "\xa0":
        missing.append(c)
    out[c] = rows
json.dump(out, open(os.path.join(ROOT, "tools/assets/ps2p_glyphs.json"), "w"), ensure_ascii=False, indent=0)
print(f"{len(out)} glyphs cached; empty or spilling: {missing}")
