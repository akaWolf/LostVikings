#!/usr/bin/env python3
"""build_content.py — the console content pack (content/) for the V2_ONLY engine.

The SNES / Genesis material of the UX plan — the SNES DE parallax layers of
the 42 levels, the five SNES-exclusive levels in the progression (TR33, SNDS,
TMPL, RVTS, PDDY) with their passwords, the Genesis interludes, the SNES 1993
level variants (the F1 SNES BALANCE switch), the console finale, the language
banks and the SNES sound banks — lives in mods/console_content.mod.json: a mod
package over the OPEN asset tree (326 files — 210 new chunks 0x1B2..0x317,
edited level headers and world scripts, exe_static.bin with the LVX5 trailer).
The engine reads it through the asset store, in the V2_ONLY build only (the
default build keeps DATA.DAT as the oracle of the orig-vs-mirror verification):
V2_ASSETS_DIR=<content>/.compiled and V2_EXE_STATIC=<content>/exe_static.bin —
or, with neither variable set, a content/ directory next to the executable or
in the working directory (v2_main.cpp; V2_CONTENT=0 turns that lookup off).

This script builds that directory from the user's DATA.DAT, nothing else is
written:
  1. tools/data/extract_datadat.py: DATA.DAT -> content/raw (the manifest and
     the comp/dec chunks; the extractor verifies the round trip back to the
     archive byte for byte)
  2. tools/assets/assetc.py: the open tree content/<role>/... — every chunk
     extracted and compiled back byte-exact (the judge). Its inputs from the
     repo: assets_raw/chunk_map.json (the chunk roles) and assets_raw/lvs/
     *.lvsf (the DSL text of the six world scripts; the mod carries edited
     copies of that form, which a byte dump could not take)
  3. the mod package applied over the tree (edit_server.mod_import)
  4. edit_server.do_pack: content/.compiled/NNNN.bin — the 535 archive records
     (an untouched asset keeps the archive's own LZSS stream) plus the extras
     — and content/exe_static.bin (the mod's image)

The result has the scratch-tree layout of tools/assets/edit_server.py, so the
editor can work on it (--scratch content). Usage:
  python3 tools/assets/build_content.py [--data DATA.DAT] [--out content] [--fresh]
The same tree ships in the V2_ONLY release bundles as content-tools/ (the
scripts, the mod package and the two repo inputs): run it from the bundle
directory with DATA.DAT beside the executable.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

ARCHIVE_CHUNKS = 535          # the 1992 DATA.DAT
STATIC_IMAGE = 0x29F00        # exe_static.bin: the m2c static image; the LVX trailer sits past it


def fail(msg):
    sys.stderr.write("build_content: " + msg + "\n")
    sys.exit(1)


def find_data(arg):
    if arg:
        return os.path.abspath(arg)
    for cand in (os.path.join(os.getcwd(), "DATA.DAT"), os.path.join(ROOT, "DATA.DAT")):
        if os.path.exists(cand):
            return cand
    fail("DATA.DAT not found — pass --data <path> (the archive of a legal copy of the 1992 game)")


def main():
    ap = argparse.ArgumentParser(description="build the console content pack for the V2_ONLY engine")
    ap.add_argument("--data", help="DATA.DAT (default: ./DATA.DAT, then the repo root)")
    ap.add_argument("--out", default="content", help="output directory (default: ./content)")
    ap.add_argument("--mod", default=os.path.join(ROOT, "mods", "console_content.mod.json"),
                    help="the mod package (default: mods/console_content.mod.json)")
    ap.add_argument("--fresh", action="store_true", help="remove an existing output directory first")
    args = ap.parse_args()
    sys.argv = sys.argv[:1]   # assetc.main() reads argv[1] ("--pack")

    data = find_data(args.data)
    out = os.path.abspath(args.out)
    raw = os.path.join(out, "raw")
    cmap_src = os.path.join(ROOT, "assets_raw", "chunk_map.json")
    lvs_src = os.path.join(ROOT, "assets_raw", "lvs")
    for p, what in ((data, "DATA.DAT"), (args.mod, "the mod package"), (cmap_src, "assets_raw/chunk_map.json"),
                    (lvs_src, "assets_raw/lvs")):
        if not os.path.exists(p):
            fail(f"{what} not found: {p}")
    if os.path.abspath(os.path.dirname(data)) == out:
        fail("--out must not be the directory of DATA.DAT")
    if os.path.exists(out):
        if not args.fresh:
            fail(f"{out} exists — rerun with --fresh to rebuild it (everything inside is replaced)")
        shutil.rmtree(out)
    os.makedirs(raw)

    # 1. the archive -> raw chunks
    print(f"[1/4] extracting {data} -> {raw}")
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "data", "extract_datadat.py"), data, raw],
                       cwd=ROOT, capture_output=True, text=True)
    sys.stdout.write("".join("      " + l + "\n" for l in r.stdout.strip().splitlines()[-3:]))
    if r.returncode != 0:
        sys.stderr.write(r.stderr)
        fail("extract_datadat.py failed")
    man = json.load(open(os.path.join(raw, "manifest.json")))
    if len(man["entries"]) != ARCHIVE_CHUNKS:
        fail(f"{data}: {len(man['entries'])} chunks, expected {ARCHIVE_CHUNKS} (not the 1992 DATA.DAT?)")
    shutil.copy(cmap_src, os.path.join(raw, "chunk_map.json"))
    os.makedirs(os.path.join(raw, "lvs"))
    for fn in sorted(os.listdir(lvs_src)):
        if fn.endswith(".lvsf"):
            shutil.copy(os.path.join(lvs_src, fn), os.path.join(raw, "lvs", fn))
    smap = os.path.join(ROOT, "assets_raw", "sprite_map.json")   # derived contact sheets only; optional
    if os.path.exists(smap):
        shutil.copy(smap, os.path.join(raw, "sprite_map.json"))

    # 2. the open tree, judged byte-exact against the archive
    print(f"[2/4] converting the {ARCHIVE_CHUNKS} chunks into the open tree {out}")
    import assetc
    assetc.RAW = raw
    assetc.ASSETS = out
    assetc.main()

    # 3. the mod package
    print(f"[3/4] applying {args.mod}")
    import edit_server as E
    E.SCRATCH = out
    n_mod = E.mod_import(args.mod)

    # 4. the engine-facing store
    print("[4/4] packing")
    n_rec = E.do_pack()

    extras = json.load(open(os.path.join(out, "extras.json")))
    # an extra may take an archive id (0x1B2 does: its record replaces the archive's)
    ids = set(range(ARCHIVE_CHUNKS)) | {int(k, 16) for k in extras}
    if n_rec != len(ids):
        fail(f".compiled holds {n_rec} records, expected {len(ids)} ({ARCHIVE_CHUNKS} archive ids + {len(extras)} extras)")
    exe = os.path.join(out, "exe_static.bin")
    if not os.path.exists(exe):
        fail("the mod package carries no exe_static.bin")
    img = open(exe, "rb").read()
    trailer = re.findall(rb"LVX[0-9A-Z]", img[STATIC_IMAGE:])
    if len(img) <= STATIC_IMAGE or not trailer:
        fail(f"{exe}: no LVX trailer past the static image ({len(img)} B)")
    print(f"done: {out}")
    print(f"      {n_rec} records in .compiled ({ARCHIVE_CHUNKS} archive ids, {len(extras)} extras, "
          f"{ARCHIVE_CHUNKS + len(extras) - n_rec} of them replacing an archive id), "
          f"{n_mod} mod files, exe_static.bin {len(img)} B with the {trailer[0].decode()} trailer")
    print("      V2_ONLY build: a content/ directory beside the executable (or in the working")
    print("      directory) is picked up by itself; elsewhere:")
    print(f"      V2_ASSETS_DIR={out}/.compiled V2_EXE_STATIC={exe} ./vikings")
    return 0


if __name__ == "__main__":
    sys.exit(main())
