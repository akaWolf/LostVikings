#!/usr/bin/env python3
"""bac_strings.py — the Blizzard Arcade Collection localisation of the Lost
Vikings strings (UX stage 6). Sources (read-only, /mnt/win):

  assets/lv_snes_strings.json   419 keys of the SNES DE string set: name
                                (msg0, amsgN, HINT_N, rstN_msg_*, STRING_N ...),
                                the English text, the box width/height (px)
  assets/strings/locale.strings the collection's string container: 13 language
                                pools (deDE enUS esES esMX frFR itIT jaJP koKR
                                plPL ptBR ruRU zhCN zhTW — the file order), each
                                pool = a hash index we do not decode + the
                                strings of EVERY collection title as one
                                NUL-separated run in a shared key order

The per-language runs are in the same key order, minus keys a language lacks,
so a language string is found by aligning its run with the English run:
anchors = identical strings and identical `{placeholder}` sets (monotone via
the longest increasing subsequence), the intervals between anchors 1:1 when
their lengths agree and by a small shape alignment (line count, ending
punctuation, digits, emptiness, length class) otherwise. The LV keys are
located in the English run by their English text, then mapped onto the 390
PC EXE strings (assets/texts_exe.json) by normalised text — the PC hints
name PC keys ('S', 'TAB', 'CTRL'/'INS') where the BAC uses `{action_*}`
placeholders, so those go through a placeholder-blind word match and the
placeholders are rendered with the PC key names.

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
    f = open(path, "rb").read()
    dirw = struct.unpack("<56I", f[0x8008:0x8008 + 224])
    base = 0x8008 + 224
    starts = [base + dirw[6 + k * 4] for k in range(13)] + [len(f)]
    pools = []
    for k in range(13):
        o, t1 = starts[k], starts[k + 1]
        m, prev = 0, -1
        while True:                                   # the index: ascending first words
            a, b = struct.unpack("<II", f[o + 8 * m:o + 8 * m + 8])
            if a < prev or b > 0x2000:
                break
            prev, m = a, m + 1
        parts = f[o + 8 * m:t1].split(b"\x00")
        while parts and parts[-1] == b"":
            parts.pop()
        pools.append([p.decode("utf-8", "replace") for p in parts])
    return pools


def norm(s):
    return re.sub(r"\s+", " ", s.replace("\xa0", " ")).strip().upper()


def words_blind(s):
    """Word set with placeholders, quoted key names and punctuation dropped."""
    s = PLACEHOLDER_RE.sub(" ", s)
    s = re.sub(r"'[A-Z0-9 ]+'", " ", s.upper())
    return {w for w in re.findall(r"[A-Z]{2,}", s)}


def shape(s):
    return (s.count("\n"), s.strip() == "", tuple(re.findall(r"\d+", s)),
            tuple(sorted(PLACEHOLDER_RE.findall(s))), s.strip()[-1:] in ("?", "!", "."),
            min(len(s) // 12, 6))


def lis_pairs(pairs):
    """Longest monotone chain (both coordinates increasing) of (i, j) pairs."""
    import bisect
    pairs = sorted(set(pairs))
    tails, prev, idx = [], [-1] * len(pairs), []
    for n, (i, j) in enumerate(pairs):
        p = bisect.bisect_left([pairs[t][1] for t in tails], j)
        if p and pairs[tails[p - 1]][0] >= i:       # keep strictly increasing i
            continue
        if p == len(tails):
            tails.append(n)
        else:
            tails[p] = n
        prev[n] = tails[p - 1] if p else -1
    out, n = [], tails[-1] if tails else -1
    while n >= 0:
        out.append(pairs[n]); n = prev[n]
    return out[::-1]


def align(E, P, band=1500):
    """i (English index) -> j (P index) or None."""
    n, m = len(E), len(P)
    # 1. anchors: identical non-trivial strings, unique on both sides
    from collections import Counter
    ce, cp = Counter(E), Counter(P)
    pos_p = {}
    for j, s in enumerate(P):
        if cp[s] == 1:
            pos_p[s] = j
    pairs = [(i, pos_p[s]) for i, s in enumerate(E) if ce[s] == 1 and s in pos_p and len(s.strip()) >= 2
             and abs(pos_p[s] - i) <= band]
    # identical placeholder sets (unique on both sides) count as anchors too
    def phkey(s):
        t = tuple(sorted(PLACEHOLDER_RE.findall(s)))
        return (t, s.count("\n")) if t else None
    ke = Counter(phkey(s) for s in E); kp = Counter(phkey(s) for s in P)
    pos_pk = {phkey(s): j for j, s in enumerate(P) if phkey(s) and kp[phkey(s)] == 1}
    pairs += [(i, pos_pk[phkey(s)]) for i, s in enumerate(E) if phkey(s) and ke[phkey(s)] == 1
              and phkey(s) in pos_pk and abs(pos_pk[phkey(s)] - i) <= band]
    anchors = lis_pairs(pairs)
    anchors = [(-1, -1)] + anchors + [(n, m)]
    out = [None] * n
    for (i0, j0), (i1, j1) in zip(anchors, anchors[1:]):
        if i1 >= 0 and i1 < n and j1 >= 0 and j1 < m:
            out[i1] = j1
        li, lj = i1 - i0 - 1, j1 - j0 - 1
        if li <= 0:
            continue
        if li == lj:
            for k in range(li):
                out[i0 + 1 + k] = j0 + 1 + k
            continue
        # small shape alignment (Needleman-Wunsch), intervals are short
        A = [shape(E[i0 + 1 + k]) for k in range(li)]
        B = [shape(P[j0 + 1 + k]) for k in range(lj)]
        if li * lj > 250000:                          # a runaway interval: fall back to 1:1 prefix
            for k in range(min(li, lj)):
                out[i0 + 1 + k] = j0 + 1 + k
            continue
        def sim(a, b):
            return (3 * (a[0] == b[0]) + 2 * (a[1] == b[1] and a[1]) + 2 * (a[2] == b[2] and bool(a[2]))
                    + 3 * (a[3] == b[3] and bool(a[3])) + (a[4] == b[4]) + (a[5] == b[5]) - 2)
        GAP = -2
        H = [[0] * (lj + 1) for _ in range(li + 1)]
        for x in range(1, li + 1):
            H[x][0] = x * GAP
        for y in range(1, lj + 1):
            H[0][y] = y * GAP
        for x in range(1, li + 1):
            for y in range(1, lj + 1):
                H[x][y] = max(H[x - 1][y - 1] + sim(A[x - 1], B[y - 1]), H[x - 1][y] + GAP, H[x][y - 1] + GAP)
        x, y = li, lj
        while x > 0 and y > 0:
            if H[x][y] == H[x - 1][y - 1] + sim(A[x - 1], B[y - 1]):
                out[i0 + x] = j0 + y; x -= 1; y -= 1
            elif H[x][y] == H[x - 1][y] + GAP:
                x -= 1
            else:
                y -= 1
    return out


def main():
    out_path = os.path.join(ROOT, "tools", "assets", "bac_lv_locale.json")
    if "--out" in sys.argv:
        out_path = sys.argv[sys.argv.index("--out") + 1]
    pools = read_pools()
    E = pools[1]
    js = json.load(open(os.path.join(BAC, "lv_snes_strings.json"), encoding="utf-8"))["strings"]
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
    # every language
    maps = {}
    for k, code in enumerate(LANGS):
        maps[code] = list(range(len(E))) if code == "en" else align(E, pools[k])
    by_name = {}
    for name, i in name_idx.items():
        by_name[name] = {}
        for k, code in enumerate(LANGS):
            j = maps[code][i]
            by_name[name][code] = pools[k][j] if j is not None and j < len(pools[k]) else None
    # quality: line-count agreement with English over the LV keys
    for code in LANGS:
        vals = [(E[name_idx[n]], by_name[n][code]) for n in by_name if by_name[n][code] is not None]
        ok = sum(1 for a, b in vals if a.count("\n") == b.count("\n") and (a.strip() == "") == (b.strip() == ""))
        print(f"  {code:6s} mapped {len(vals):3d}/{len(by_name)}  line-count agreement {ok}/{len(vals)}"
              f"  msg0={by_name.get('msg0', {}).get(code, '')[:18]!r}  405={by_name.get('STRING_405', {}).get(code, '')[:18]!r}")
    # PC strings -> LV keys
    pc = json.load(open(os.path.join(ROOT, "assets", "texts_exe.json")))["entries"]
    key_by_norm = {}
    for e in js:
        if e["name"] in name_idx:
            key_by_norm.setdefault(norm(e["string"]), e["name"])
    # the PC EXE holds YES / NO / the blank as raw 3-byte strings without the
    # [w][h] head (texts_exe.json shows them as 'S' / '' / ' '); the BAC keys
    # msg4 / msg3 / msg5 are those words
    pc_key, pc_unmatched = {5: "msg4", 4: "msg3", 6: "msg5"}, []
    for e in pc:
        k = norm(e["text"])
        if e["i"] in pc_key:
            continue
        if not k:
            continue
        if k in key_by_norm:
            pc_key[e["i"]] = key_by_norm[k]; continue
        w = words_blind(e["text"]); best, bs = None, 0.0
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
            pc_unmatched.append((e["i"], e["text"][:40]))
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
    json.dump({"languages": LANGS, "by_name": by_name, "pc": pc_table, "pc_key": {str(k): v for k, v in pc_key.items()},
               "pc_unmatched": pc_unmatched, "placeholders": placeholders},
              open(out_path, "w"), ensure_ascii=False, indent=0)
    print("wrote", out_path)


if __name__ == "__main__":
    main()
