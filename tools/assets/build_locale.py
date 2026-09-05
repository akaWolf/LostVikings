#!/usr/bin/env python3
"""build_locale.py — the language banks of UX stage 6 (one mod chunk per
language, ids LANG_CID0 + k) from the BAC translations extracted by
bac_strings.py and the Press Start 2P glyph cache (ps2p_glyphs.py).

Bank payload (the engine maps it at seg001 virtual pointers 0x8000+ and
reads it through v2_text_byte / v2_text_ptr_of / v2_glyph_bytes):

  +0x00  "LVLB"          +0x04  code[8] ("fr", "pt-BR" ...)
  +0x0C  u16 n_strings   +0x0E  u16 n_glyphs   +0x10  u16 first glyph code
  +0x12  u16 n_ascii     +0x14  u16 offset[n_strings]  (0 = keep the English record)
  glyph page: n_glyphs x 72 B, then n_ascii x 72 B for the DOS codes 0x21.. (the
    language's face for digits/Latin drawn by the engine, e.g. the password
    letters), all in the DOS glyph format (the same 4 planes x 2
    strips x [mask + 8 data] cells as chunk 2, palette 3 = box fill, 2 = body,
    1 = the (-1,+1) shadow the original letters carry)
  records: [w][h][chars..., 0] with 0x0D line breaks; PC strings 4/5/6 (NO /
    YES / blank, drawn by cmd 0x0A at fixed cells) are raw like the originals

Text codes: 0x20 = the original space glyph, 0x0D = new line, 0x60.. = the
language's own glyph page (every letter of a localised string renders in
Press Start 2P — one face per language, no mix with the DOS caps). Lines
longer than MAXW are re-wrapped at word boundaries; the box is
(longest line + 2) x (lines + 2) cells like every original record.
"""
import json
import os
import struct
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
LANG_CID0 = 0x300
LANGS = ["de", "es", "es-MX", "fr", "it", "pl", "pt-BR", "ru"]   # Latin/Cyrillic; CJK = the presenter layer (later)
MAXW = 30
RAW_IDX = {4, 5, 6}
GLYPH0 = 0x60
TRY_AGAIN_IDX = 271      # class D2 draws YES/NO at fixed cells (15,11)/(21,11) under a box at (13,8):
                         # keep the title within 2 lines, the box >= 12 wide, two rows below the title
ASCII_PAGE = [chr(c) for c in range(0x21, 0x60)]   # the language's face for the DOS ASCII codes too


def wrap(text):
    lines = []
    for ln in text.replace("\r", "").split("\n"):
        ln = ln.replace("\xa0", " ").strip()
        if len(ln) <= MAXW:
            lines.append(ln)
            continue
        cur = ""
        for w in ln.split(" "):
            if not cur:
                cur = w
            elif len(cur) + 1 + len(w) <= MAXW:
                cur += " " + w
            else:
                lines.append(cur)
                cur = w
        lines.append(cur)
    while len(lines) > 1 and lines[-1] == "":
        lines.pop()
    return lines


def dos_glyph(rows):
    """8 row bitmasks -> 72-byte DOS glyph: body 2, shadow (-1,+1) 1, fill 3."""
    px = [[3] * 8 for _ in range(8)]
    body = [[(rows[y] >> (7 - x)) & 1 for x in range(8)] for y in range(8)]
    for y in range(8):
        for x in range(8):
            if body[y][x]:
                px[y][x] = 2
            elif x + 1 < 8 and y - 1 >= 0 and body[y - 1][x + 1]:
                px[y][x] = 1
    out = bytearray()
    for plane in range(4):
        for strip in range(2):
            out.append(0xFF)
            for k in range(8):
                col, row = k & 1, k >> 1
                out.append(px[strip * 4 + row][col * 4 + plane])
    assert len(out) == 72
    return bytes(out)


def build_bank(code, strings, glyphs, orig=None):
    """strings: {pc_index: text}; glyphs: {char: rows}; orig: {pc_index: (w, h)}
    of the English records — a localised box is never smaller than the
    original (the engine draws the password letters, YES/NO and the like at
    fixed cells inside it). -> payload bytes."""
    orig = orig or {}
    charset = sorted({c for s in strings.values() for c in s if c not in "\n\r\xa0 "})
    assert len(charset) <= 0x100 - GLYPH0, (code, len(charset))
    enc = {c: GLYPH0 + k for k, c in enumerate(charset)}
    n = 390
    recs = {}
    for i, text in strings.items():
        if i in RAW_IDX:
            body = text.replace("\xa0", " ")
            recs[i] = bytes(0x20 if c == " " else enc[c] for c in body) + b"\0"
            continue
        lines = wrap(text)
        if i == TRY_AGAIN_IDX:
            t = " ".join(l for l in lines if l).strip()
            if len(t) > 12 and " " in t:
                cut = min((k for k in range(len(t)) if t[k] == " "), key=lambda k: abs(k - len(t) // 2))
                lines = [t[:cut].strip(), t[cut:].strip()]
            else:
                lines = [t]
        ow, oh = orig.get(i, (0, 0))
        while len(lines) + 2 < oh:
            lines.append("")
        w = max(max(len(l) for l in lines) + 2, ow)
        h = len(lines) + 2
        body = bytearray()
        for k, l in enumerate(lines):
            body += bytes(0x20 if c == " " else enc[c] for c in l)
            if k + 1 < len(lines):
                body.append(0x0D)
        recs[i] = bytes((w, h)) + bytes(body) + b"\0"
    # the blank word for the YES/NO blink must cover the longer word
    if 4 in recs and 5 in recs:
        blank = max(len(recs[4]), len(recs[5])) - 1
        recs[6] = b"\x20" * blank + b"\0"
    head_len = 0x14 + 2 * n
    page = b"".join(dos_glyph(glyphs[c]) for c in charset)
    page += b"".join(dos_glyph(glyphs[c]) if c in glyphs else dos_glyph([0] * 8) for c in ASCII_PAGE)
    off = head_len + len(page)
    offsets = [0] * n
    body = bytearray()
    for i in sorted(recs):
        offsets[i] = off + len(body)
        body += recs[i]
    assert off + len(body) < 0x8000, (code, off + len(body))
    hdr = b"LVLB" + code.encode("ascii").ljust(8, b"\0") + struct.pack("<HHHH", n, len(charset), GLYPH0, len(ASCII_PAGE))
    payload = hdr + struct.pack("<%dH" % n, *offsets) + page + bytes(body)
    return payload, len(recs), len(charset)


def banks():
    L = json.load(open(os.path.join(ROOT, "tools/assets/bac_lv_locale.json")))
    G = json.load(open(os.path.join(ROOT, "tools/assets/ps2p_glyphs.json")))
    orig = {e["i"]: (e["w"], e["h"]) for e in json.load(open(os.path.join(ROOT, "assets/texts_exe.json")))["entries"]
            if e["i"] not in RAW_IDX}
    out = {}
    for k, code in enumerate(LANGS):
        strings = {int(i): s for i, s in L["pc"][code].items() if s is not None}
        payload, nrec, ng = build_bank(code, strings, G, orig)
        out[LANG_CID0 + k] = (code, payload, nrec, ng)
    return out


def integrate(scratch):
    """Write the banks into the scratch tree (role unreferenced -> raw chunk)."""
    ex_path = os.path.join(scratch, "extras.json")
    extras = json.load(open(ex_path)) if os.path.exists(ex_path) else {}
    os.makedirs(os.path.join(scratch, "unreferenced"), exist_ok=True)
    print("language banks:")
    for cid, (code, payload, nrec, ng) in sorted(banks().items()):
        with open(os.path.join(scratch, "unreferenced", f"{cid:04X}.bin"), "wb") as f:
            f.write(payload)
        extras[f"{cid:04X}"] = {"role": "unreferenced"}
        print(f"  {cid:04X} {code:6s} {nrec} strings, {ng} glyphs, {len(payload)} B")
    with open(ex_path + ".tmp", "w") as f:
        json.dump(extras, f, indent=1)
    os.replace(ex_path + ".tmp", ex_path)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--scratch":
        integrate(sys.argv[2])
    else:
        for cid, (code, payload, nrec, ng) in sorted(banks().items()):
            print(f"{cid:04X} {code:6s} {nrec} strings, {ng} glyphs, {len(payload)} B")
