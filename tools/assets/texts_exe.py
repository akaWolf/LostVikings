#!/usr/bin/env python3
"""texts_exe.py — the game's dialog/menu texts (task #108 p12).

Every dialog line in the game lives in seg001 of the static EXE image
(exe_static.bin, seg001 at file offset 0x9480):

  seg001+0x000: 390 u16 pointers (table ends at min(ptr) = 0x30C)
  each record:  [width u8][height u8][chars... 0x0D = newline, 0x00 = end]
  chars are font glyph codes = ASCII (glyph = ch - 0x10, sub_1E0C7)
  readers: sub_12515 (pointer lookup) -> sub_12529 (dims) ->
           loc_124c5 (char walker: 0x0D newline, 0x00 terminator)

The string zone is 0x30C..0x4062 followed by zero padding; repacking
keeps the total budget (table + strings must fit the original zone).
Duplicate pointers (aliases) are preserved by text equality.

Usage:
  texts_exe.py extract [-o assets/texts_exe.json]
  texts_exe.py compile <json> [-o exe_static_patched.bin]
  texts_exe.py judge            # extract -> compile == original, byte-exact
"""
import argparse
import json
import os
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
SEG001 = 0x9480
TABLE_END = 0x30C          # = min pointer = number of entries * 2
NENT = TABLE_END // 2
# Hard budget: the original string zone ends at 0x4062 followed by zero
# padding up to 0x4220, where the next seg001 structure ("Test...")
# begins — edited texts may grow into that padding, never past it.
BUDGET_END = 0x4220


def load_image(path=None):
    with open(path or os.path.join(ROOT, "exe_static.bin"), "rb") as f:
        return bytearray(f.read())


def extract(img):
    seg = img[SEG001:]
    ptrs = [seg[i * 2] | (seg[i * 2 + 1] << 8) for i in range(NENT)]
    zone_end = 0
    entries = []
    for i, p in enumerate(ptrs):
        w, h = seg[p], seg[p + 1]
        j = p + 2
        while seg[j] != 0:
            j += 1
        zone_end = max(zone_end, j + 1)
        body = bytes(seg[p + 2:j])
        entries.append({
            "i": i, "ptr": p, "w": w, "h": h,
            "text": body.decode("ascii").replace("\r", "\n"),
        })
    # unreferenced records inside the zone (cut content — e.g. the lost
    # "THANKS TO SCOTT B..." credits blob at 0x3E53): keep them byte-exact
    orphans = []
    covered = set()
    for p in set(e["ptr"] for e in entries):
        j = p + 2
        while seg[j] != 0:
            j += 1
        covered.update(range(p, j + 1))
    pos = TABLE_END
    while pos < zone_end:
        if pos in covered or seg[pos] == 0:
            pos += 1
            continue
        j = pos + 2
        while seg[j] != 0:
            j += 1
        orphans.append({"ptr": pos, "hex": bytes(seg[pos:j + 1]).hex()})
        pos = j + 1
    return {"format": "exe_texts", "seg001": SEG001, "table_end": TABLE_END,
            "zone_end": zone_end, "entries": entries, "orphans": orphans}


def compile_texts(js, img):
    """Patch the seg001 text zone STRICTLY IN PLACE on `img`.
    Records must not move: the zone is also referenced by hard
    (non-table) pointers — a repack broke a live dialog (proven by a
    load-state control run). Every record keeps its original offset;
    an edited body must fit the original slot (shorter bodies are
    NUL-padded). The pointer table is written back unchanged."""
    entries = js["entries"]
    assert len(entries) == NENT, f"need {NENT} entries"
    seg_base = SEG001
    # original slot sizes come from the CURRENT image record walk
    for e in entries:
        p = e["ptr"]
        j = p + 2
        while img[seg_base + j] != 0:
            j += 1
        slot = j - (p + 2)                      # original body length
        body = e["text"].replace("\n", "\r").encode("ascii")
        if len(body) > slot:
            raise ValueError(
                f"text {e['i']} does not fit in place: {len(body)} > "
                f"{slot} bytes (records cannot move — hard references)")
        img[seg_base + p] = e["w"] & 0xFF
        img[seg_base + p + 1] = e["h"] & 0xFF
        img[seg_base + p + 2:seg_base + p + 2 + slot] = \
            body + b"\x00" * (slot - len(body))
        # pointer table entry stays as-is (rewritten for exactness)
        img[seg_base + e["i"] * 2] = p & 0xFF
        img[seg_base + e["i"] * 2 + 1] = p >> 8
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["extract", "compile", "judge"])
    ap.add_argument("json_path", nargs="?")
    ap.add_argument("-o", "--out")
    ap.add_argument("--image", help="alternate exe_static.bin")
    args = ap.parse_args()
    img = load_image(args.image)
    if args.cmd == "extract":
        js = extract(img)
        out = args.out or os.path.join(ROOT, "assets", "texts_exe.json")
        with open(out, "w") as f:
            json.dump(js, f, indent=1)
        print(f"{len(js['entries'])} texts -> {out}")
    elif args.cmd == "compile":
        with open(args.json_path) as f:
            js = json.load(f)
        orig = bytes(img)
        img = compile_texts(js, img)
        out = args.out or os.path.join(ROOT, "exe_static_patched.bin")
        with open(out, "wb") as f:
            f.write(img)
        changed = sum(1 for a, b in zip(orig, img) if a != b)
        print(f"patched image -> {out} ({changed} bytes changed)")
    else:
        js = extract(img)
        rebuilt = compile_texts(js, bytearray(img))
        ok = bytes(rebuilt) == bytes(load_image(args.image))
        print(f"judge: {'IDENTICAL' if ok else 'DIFF!'}")
        sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
