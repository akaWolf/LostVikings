#!/usr/bin/env python3
"""bac_strings.py — the Blizzard Arcade Collection localisation of the Lost
Vikings strings (UX stage 6). Sources (read-only, /mnt/win):

  assets/lv_snes_strings.json   419 keys of the SNES DE string set: name
                                (msg0, amsgN, HINT_N, rstN_msg_*, STRING_N ...),
                                the English text, the box width/height (px)
  assets/strings/locale.strings the collection's string container: 13 language
                                pools (deDE enUS esES esMX frFR itIT jaJP koKR
                                plPL ptBR ruRU zhCN zhTW — the file order), each
                                pool = a table of 8-byte (u32, u32) entries we
                                do not need, then the strings of EVERY
                                collection title as one NUL-separated run —
                                exactly the directory's count (1998) of them,
                                in ONE key order shared by all 13 pools

The directory at 0x8008 (56 u32: 6 words, then 4 per pool — start, count,
end, 5) places every pool; the run starts where the table ends, and the
table's end is exact: a table word is an offset below the pool's size while
text bytes are >= 0x20 (a word >= 0x20202020). Every pool holds the count's
strings in the same key order (verified: every string unique on both sides
and identical in two languages sits at the same index in all 13 pools), so a
language string is the entry at the English string's index — no alignment.
Anything past the count is the next pool's head (its locale tag and table),
which an earlier reading of the file took for strings: the table's NUL-split
bytes came out as thousands of empty and one-byte "strings" in front of and
behind the run, and the alignment they forced put the level-1 exit line
(AUTO_DIALOG_A, PC 79) onto an empty one in ru/ko and onto table bytes in
ja/zh-CN — an empty dialogue box in the game. The LV keys are located in the
English run by their English text, then mapped onto the 390 PC EXE strings
(assets/texts_exe.json) by normalised text — the PC hints name PC keys
('S', 'TAB', 'CTRL'/'INS') where the BAC uses `{action_*}` placeholders, so
those go through a placeholder-blind word match and the placeholders are
rendered with the PC key names.

  python3 tools/assets/bac_strings.py [--out tools/assets/bac_lv_locale.json]
"""
import json
import os
import re
import struct
import sys

BAC = "/mnt/win/Program Files (x86)/Blizzard Arcade Collection/assets"
LANGS = ["de", "en", "es", "es-MX", "fr", "it", "ja", "ko", "pl", "pt-BR", "ru", "zh-CN", "zh-TW"]
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")

# `{action_*}` placeholders of the BAC strings -> the PC key names the DOS
# hints use (texts_exe.json 64..80: 'S' use/talk, 'E' use item, 'TAB'
# inventory, 'CTRL'/'INS' previous/next viking, 'SPACE' jump/attack)
PLACEHOLDER_PC = {                      # derived from the PC hints 64..80 (texts_exe.json)
    "action_lv_activate": "'S'",        # use/talk (HINT_0/1/5/9/10)
    "action_lv_primary": "'F'",         # jump / glide / release (HINT_4/5/9/11)
    "action_lv_secondary": "'D'",       # attack: Erik's bash, Baleog's arrow (HINT_12/13)
    "action_select_item": "'TAB'",      # inventory (HINT_4/6)
    "action_use_item": "'E'",           # use the selected item (HINT_7/14)
    "action_lv_switch_left": "'CTRL'",  # previous viking (AUTO_DIALOG_B)
    "action_lv_switch_right": "'INS'",  # next viking
}
PLACEHOLDER_RE = re.compile(r"\{([a-z_0-9]+)\}")


def read_pools(path=os.path.join(BAC, "strings", "locale.strings")):
    """-> 13 lists of the directory's count of strings each, one key order."""
    f = open(path, "rb").read()
    dirw = struct.unpack("<56I", f[0x8008:0x8008 + 224])
    base = 0x8008 + 224
    starts = [base + dirw[6 + k * 4] for k in range(13)] + [len(f)]
    counts = [dirw[7 + k * 4] for k in range(13)]
    pools = []
    for k in range(13):
        o, t1, count = starts[k], starts[k + 1], counts[k]
        size = t1 - o
        m = 0                                         # the table: 8-byte entries whose first word is an offset below the pool's size
        while struct.unpack("<I", f[o + 8 * m:o + 8 * m + 4])[0] < size:
            m += 1
        parts = f[o + 8 * m:t1].split(b"\x00")[:count]
        if len(parts) != count:
            raise ValueError(f"{path}: pool {k} ({LANGS[k]}) holds {len(parts)} strings, the directory says {count}")
        strings = [p.decode("utf-8") for p in parts]   # strict: the run is UTF-8 text; a decode error means the table's end was missed
        bad = [s for s in strings if any(ord(c) < 0x20 and c not in "\n\r\t" for c in s)]
        if bad:
            raise ValueError(f"{path}: pool {k} ({LANGS[k]}) carries {len(bad)} strings with control bytes: {bad[:3]!r}")
        pools.append(strings)
    if len({len(p) for p in pools}) != 1:
        raise ValueError(f"{path}: the pools differ in size: {[len(p) for p in pools]}")
    return pools


def norm(s):
    return re.sub(r"\s+", " ", s.replace("\xa0", " ")).strip().upper()


def words_blind(s):
    """Word set with placeholders, quoted key names and punctuation dropped."""
    s = PLACEHOLDER_RE.sub(" ", s)
    s = re.sub(r"'[A-Z0-9 ]+'", " ", s.upper())
    return {w for w in re.findall(r"[A-Z]{2,}", s)}


def check_same_order(E, P, code):
    """The pools share one key order: a string unique in both must sit at the
    same index (the placeholders, the names, the untranslated lines)."""
    from collections import Counter
    ce, cp = Counter(E), Counter(P)
    pos_p = {s: j for j, s in enumerate(P) if cp[s] == 1}
    moved = [(i, pos_p[s]) for i, s in enumerate(E) if ce[s] == 1 and s in pos_p and len(s.strip()) >= 2 and pos_p[s] != i]
    if moved:
        raise ValueError(f"locale.strings: the {code} pool is not in the English key order: {moved[:5]}")


def build(locale_path, keys_path, texts_path, out_path):
    """The translation table (the JSON build_locale.py reads) from the two BAC
    files and the PC texts: locale_path = strings/locale.strings, keys_path =
    lv_snes_strings.json (both from the collection's assets/), texts_path =
    the texts_exe.json of the tree (texts_exe.py extract)."""
    pools = read_pools(locale_path)
    E = pools[1]
    js = json.load(open(keys_path, encoding="utf-8"))["strings"]
    # LV keys -> English run index
    by_norm = {}
    for i, s in enumerate(E):
        by_norm.setdefault(norm(s), []).append(i)
    name_idx, unresolved = {}, []
    for e in js:
        k = norm(e["string"])
        if k in by_norm:
            name_idx[e["name"]] = by_norm[k][0]
        else:
            unresolved.append(e)
    # placeholder-blind match for the rest, inside the LV span of the run
    lo = min(name_idx.values()) - 50
    hi = max(name_idx.values()) + 50
    for e in unresolved:
        w = words_blind(e["string"])
        best, bs = None, 0.0
        for i in range(max(0, lo), min(len(E), hi)):
            v = words_blind(E[i])
            if not v or not w:
                continue
            j = len(w & v) / len(w | v)
            if j > bs:
                best, bs = i, j
        if best is not None and bs >= 0.6:
            name_idx[e["name"]] = best
    still = [e["name"] for e in js if e["name"] not in name_idx]
    print(f"LV keys located in the English run: {len(name_idx)}/{len(js)}; not located: {still}")
    # every language: the entry at the English string's index (one key order, read_pools)
    maps = {}
    for k, code in enumerate(LANGS):
        check_same_order(E, pools[k], code)
        maps[code] = list(range(len(E)))
    by_name = {}
    for name, i in name_idx.items():
        by_name[name] = {}
        for k, code in enumerate(LANGS):
            j = maps[code][i]
            by_name[name][code] = pools[k][j] if j is not None and j < len(pools[k]) else None
    # no LV key may come out blank: an English line with an empty counterpart means the
    # pools were read wrong (an empty record draws an empty dialogue box in the game)
    blank = [(n, code) for n in by_name for code in LANGS
             if E[name_idx[n]].strip() and (by_name[n][code] is None or not by_name[n][code].strip())]
    if blank:
        raise ValueError(f"locale.strings: {len(blank)} LV keys without a translation: {blank[:6]}")
    # quality: line-count agreement with English over the LV keys
    for code in LANGS:
        vals = [(E[name_idx[n]], by_name[n][code]) for n in by_name if by_name[n][code] is not None]
        ok = sum(1 for a, b in vals if a.count("\n") == b.count("\n") and (a.strip() == "") == (b.strip() == ""))
        print(f"  {code:6s} mapped {len(vals):3d}/{len(by_name)}  line-count agreement {ok}/{len(vals)}"
              f"  msg0={by_name.get('msg0', {}).get(code, '')[:18]!r}  405={by_name.get('STRING_405', {}).get(code, '')[:18]!r}")
    # PC strings -> LV keys
    pc = json.load(open(texts_path))["entries"]
    key_by_norm = {}
    for e in js:
        if e["name"] in name_idx:
            key_by_norm.setdefault(norm(e["string"]), e["name"])
    # the PC EXE holds YES / NO / the blank as raw 3-byte strings without the
    # [w][h] head (texts_exe.json shows them as 'S' / '' / ' '); the BAC keys
    # msg4 / msg3 / msg5 are those words
    # The title / options words (255..272 except the 269 / 271 boxes) are RAW
    # strings without the [w][h] head — texts_exe.json shows their first two
    # letters as w / h ('NE' + 'W GAME'); QUIT TO DOS (272) has no key in the
    # collection (a DOS-only item) and takes EXIT.
    RAW_MENU = {i for i in range(255, 273) if i not in (269, 271)}
    pc_key, pc_unmatched = {5: "msg4", 4: "msg3", 6: "msg5", 272: "g_msg_13"}, []
    for e in pc:
        text = (chr(e["w"]) + chr(e["h"]) + e["text"]) if e["i"] in RAW_MENU else e["text"]
        k = norm(text)
        if e["i"] in pc_key:
            continue
        if not k:
            continue
        if k in key_by_norm:
            pc_key[e["i"]] = key_by_norm[k]; continue
        w = words_blind(text); best, bs = None, 0.0
        for e2 in js:
            if e2["name"] not in name_idx:
                continue
            v = words_blind(e2["string"])
            if not v or not w:
                continue
            jac = len(w & v) / len(w | v)
            if jac > bs:
                best, bs = e2["name"], jac
        if best and bs >= 0.6:
            pc_key[e["i"]] = best
        else:
            pc_unmatched.append((e["i"], text[:40]))
    print(f"PC strings mapped to BAC keys: {len(pc_key)}/{sum(1 for e in pc if norm(e['text']))}; unmatched: {pc_unmatched}")
    placeholders = sorted({p for n in by_name for s in by_name[n].values() if s for p in PLACEHOLDER_RE.findall(s)})
    print("placeholders used:", placeholders)

    def render(s):
        return PLACEHOLDER_RE.sub(lambda m: PLACEHOLDER_PC.get(m.group(1), m.group(0)), s) if s else s
    pc_table = {code: {} for code in LANGS}
    for pi, name in pc_key.items():
        for code in LANGS:
            s = by_name[name].get(code)
            if s is not None:
                pc_table[code][str(pi)] = render(s)
    # every string of the collection's pool with its counterpart in each
    # language, keyed by the normalised English text: the content build looks
    # up the strings outside the EXE table here (the Genesis interlude lines
    # the scenes carry, build_locale.banks extra)
    pool = {}
    for i, s in enumerate(E):
        k = norm(s)
        if not k or k in pool:
            continue
        row = {}
        for kk, code in enumerate(LANGS):
            j = maps[code][i]
            if j is not None and j < len(pools[kk]) and pools[kk][j] is not None:
                row[code] = render(pools[kk][j])
        pool[k] = row
    json.dump({"languages": LANGS, "by_name": by_name, "pc": pc_table, "pc_key": {str(k): v for k, v in pc_key.items()},
               "pool": pool,
               "pc_unmatched": pc_unmatched, "placeholders": placeholders},
              open(out_path, "w"), ensure_ascii=False, indent=0)
    print("wrote", out_path)
    return {"keys": len(name_idx), "pc": len(pc_key)}


def main():
    import argparse
    ap = argparse.ArgumentParser(description="the BAC translations of the Lost Vikings strings -> bac_lv_locale.json")
    ap.add_argument("--bac", default=BAC, help="the collection's assets/ directory (strings/locale.strings, lv_snes_strings.json)")
    ap.add_argument("--locale", help="strings/locale.strings (default: under --bac)")
    ap.add_argument("--keys", help="lv_snes_strings.json (default: under --bac)")
    ap.add_argument("--texts", default=os.path.join(ROOT, "assets", "texts_exe.json"), help="the PC texts (texts_exe.py extract)")
    ap.add_argument("--out", default=os.path.join(ROOT, "tools", "assets", "bac_lv_locale.json"))
    a = ap.parse_args()
    build(a.locale or os.path.join(a.bac, "strings", "locale.strings"),
          a.keys or os.path.join(a.bac, "lv_snes_strings.json"), a.texts, a.out)


if __name__ == "__main__":
    main()
