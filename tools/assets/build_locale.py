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
LANGS = ["de", "es", "es-MX", "fr", "it", "pl", "pt-BR", "ru", "ja", "ko", "zh-CN", "zh-TW"]
CJK = {"ja", "ko", "zh-CN", "zh-TW"}   # phase 2: UTF-8 records + a Unifont 16x16 glyph table, drawn by v2_draw_ui
MAXW = 30
MAXW_CJK_PX = 224          # 28 cells of 16/8-px glyphs per line
CJK_NO_LEAD = set("。、，．！？」』）〕】〉》・ー…：；!?,.)")   # no line starts with these
PASSWORD_IDX = 269         # the level letters are typed at box row 2 (row 12 of the 10x4 box at
                           # (15,10)); the CJK title takes box rows 0-1 (one 16-px line)
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


def cjk_wrap(text, G):
    """Lines of at most MAXW_CJK_PX by glyph advance: by words where spaces
    exist (Korean), by characters otherwise, closing punctuation never leads."""
    out = []
    for ln in text.replace("\r", "").split("\n"):
        ln = ln.replace("\xa0", " ").replace("\u3000", "　").strip()
        width = lambda t: sum(G[c]["w"] for c in t)
        if width(ln) <= MAXW_CJK_PX:
            out.append(ln)
            continue
        if " " in ln:
            cur = ""
            for w in ln.split(" "):
                if cur and width(cur + " " + w) > MAXW_CJK_PX:
                    out.append(cur); cur = w
                else:
                    cur = (cur + " " + w) if cur else w
            out.append(cur)
            continue
        cur = ""
        for c in ln:
            if width(cur + c) > MAXW_CJK_PX and cur and c not in CJK_NO_LEAD:
                out.append(cur); cur = c
            else:
                cur += c
        out.append(cur)
    return out


def build_bank_cjk(code, strings, G, orig=None):
    """UTF-8 records ([w][h][0x01][utf-8 with 0x0D breaks][0], the 0x01 marker
    routes loc_124c5 to the wide-glyph path) + the Unifont 16x16 table of
    every character used, located by the 8-byte trailer [u32 offset]"WGLY"."""
    orig = orig or {}
    n = 390
    recs = {}
    used = set()
    for i, text in strings.items():
        text = text.replace("\xa0", " ")
        if any(c not in G for c in text if c not in "\n\r"):
            print(f"  {code}: string {i} has glyphs outside the Unifont cache, kept English")
            continue
        if i in RAW_IDX:
            recs[i] = b"\x01" + text.encode("utf-8") + b"\0"; used |= set(text)
            continue
        lines = cjk_wrap(text, G)
        if i == PASSWORD_IDX:
            # the box is 10x4 at (15,10) in the world scripts (op 44 000d01..0f000a);
            # the level letters are typed at cells 18..21 of row 12 = box row 2,
            # so a 16-px title on box rows 0-1 sits exactly where the English
            # title + its blank row were; the width only grows (max with the
            # original 10 below) and the letters stay inside
            lines = [" ".join(l for l in lines if l).strip()]
        if i == TRY_AGAIN_IDX:
            lines = [" ".join(l for l in lines if l).strip()]      # one 16-px line: YES/NO sit on the fixed row below
        cells = lambda l: -(-sum(G[c]["w"] for c in l) // 8)
        w = max(cells(l) for l in lines) + 2
        h = 2 * len(lines) + 2
        ow, oh = orig.get(i, (0, 0))
        if i == TRY_AGAIN_IDX:
            w, h = max(w, 15), max(h, 6)      # YES at cell 15, NO at 21 (16-px glyphs: cells 21..26), rows 11-12
        if i == PASSWORD_IDX:
            h = max(h, 5)                     # title rows 11-12; the level letters land on row 13 (engine: a glyph
            #                                   typed under a live wide item moves one row down); frame row 14
        w, h = max(w, ow), max(h, oh)
        body = bytearray(b"\x01")
        for k, l in enumerate(lines):
            body += l.encode("utf-8")
            if k + 1 < len(lines):
                body.append(0x0D)
            used |= set(l)
        recs[i] = bytes((w, h)) + bytes(body) + b"\0"
    if 4 in recs and 5 in recs:
        blank = max(sum(G[c]["w"] for c in strings[4]), sum(G[c]["w"] for c in strings[5]))
        recs[6] = b"\x01" + (" " * (blank // 8)).encode() + b"\0"; used.add(" ")
    head_len = 0x14 + 2 * n
    offsets = [0] * n
    body = bytearray()
    for i in sorted(recs):
        offsets[i] = head_len + len(body)
        body += recs[i]
    assert head_len + len(body) < 0x8000, (code, head_len + len(body))
    table = bytearray(struct.pack("<H", len(used)))
    for c in sorted(used, key=ord):
        g = G[c]
        assert ord(c) < 0x10000, c
        table += struct.pack("<HBB", ord(c), g["w"], 0) + struct.pack("<16H", *g["rows"])
    hdr = b"LVLB" + code.encode("ascii").ljust(8, b"\0") + struct.pack("<HHHH", n, 0, GLYPH0, 0)
    payload = hdr + struct.pack("<%dH" % n, *offsets) + bytes(body)
    wide_off = len(payload)
    payload += bytes(table) + struct.pack("<I", wide_off) + b"WGLY"
    assert len(payload) <= 0x10000, (code, len(payload))
    return payload, len(recs), len(used)


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


def banks(texts_path=None, locale_path=None):
    """texts_path: the texts_exe.json whose box sizes are the floor (default the
    canonical extraction assets/texts_exe.json; build_content.py's tree carries
    its own copy of the same extraction). locale_path: the translation table
    bac_strings.py builds from the user's Blizzard Arcade Collection (not in
    the repository — Blizzard's text); build_content.py writes it into the tree."""
    L = json.load(open(locale_path or os.path.join(ROOT, "tools/assets/bac_lv_locale.json")))
    G = json.load(open(os.path.join(ROOT, "tools/assets/ps2p_glyphs.json")))
    G16 = json.load(open(os.path.join(ROOT, "tools/assets/unifont16_glyphs.json")))
    orig = {e["i"]: (e["w"], e["h"])
            for e in json.load(open(texts_path or os.path.join(ROOT, "assets/texts_exe.json")))["entries"]
            if e["i"] not in RAW_IDX}
    out = {}
    for k, code in enumerate(LANGS):
        strings = {int(i): s for i, s in L["pc"][code].items() if s is not None}
        if code in CJK:
            payload, nrec, ng = build_bank_cjk(code, strings, G16, orig)
        else:
            payload, nrec, ng = build_bank(code, strings, G, orig)
        out[LANG_CID0 + k] = (code, payload, nrec, ng)
    return out


def integrate(scratch):
    """Write the banks into the scratch tree (role unreferenced -> raw chunk)."""
    ex_path = os.path.join(scratch, "extras.json")
    extras = json.load(open(ex_path)) if os.path.exists(ex_path) else {}
    os.makedirs(os.path.join(scratch, "unreferenced"), exist_ok=True)
    tp = os.path.join(scratch, "texts_exe.json")
    # the translations: the tree's table (build_content.py, from the user's BAC),
    # else a developer's copy under tools/assets; neither -> no banks, the
    # English original only (the engine lists the banks it finds from 0x300 on)
    lp = next((p for p in (os.path.join(scratch, "bac_lv_locale.json"),
                           os.path.join(ROOT, "tools/assets/bac_lv_locale.json")) if os.path.exists(p)), None)
    if lp is None:
        print("language banks: no BAC translation table (bac_lv_locale.json) — English only")
        return
    print(f"language banks ({lp}):")
    for cid, (code, payload, nrec, ng) in sorted(banks(tp if os.path.exists(tp) else None, lp).items()):
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
