#!/usr/bin/env python3
"""build_content.py — the console content pack (content/) for the V2_ONLY engine.

The SNES / Genesis material of the UX plan — the SNES DE parallax layers of
the 42 levels, the five SNES-exclusive levels in the progression (TR33, SNDS,
TMPL, RVTS, PDDY) with their passwords, the Genesis interludes, the SNES 1993
level variants (the F1 SNES BALANCE switch), the console finale, the language
banks and the SNES music and effects — is CONVERTED FROM THE CONSOLE IMAGES by
tools/assets/integrate_snes.py (snes2pc, smd2pc / genesis_scene, parallax_snes,
the SNES sound driver and banks, the language banks from the BAC translations
cached in bac_lv_locale.json) on top of the open tree of the user's DATA.DAT.
None of it is in the repository or in the release bundles: the build needs the
two ROM images, and the pack it writes is never redistributed.

The engine reads the pack through the asset store, in the V2_ONLY build only
(the default build keeps DATA.DAT as the oracle of the orig-vs-mirror
verification): V2_ASSETS_DIR=<content>/.compiled and
V2_EXE_STATIC=<content>/exe_static.bin — or, with neither variable set, a
content/ directory next to the executable or in the working directory
(v2_main.cpp; V2_CONTENT=0 turns that lookup off).

Steps (nothing outside the output directory is written):
  1. tools/data/extract_datadat.py: DATA.DAT -> content/raw (the manifest and
     the comp/dec chunks; the extractor verifies the round trip back to the
     archive byte for byte)
  2. tools/assets/assetc.py: the open tree content/<role>/... — every chunk
     extracted and compiled back byte-exact (the judge). Its inputs from the
     repo: assets_raw/chunk_map.json (the chunk roles) and assets_raw/lvs/
     *.lvsf (the DSL text of the six world scripts; the converters edit 1C6
     in that form). texts_exe.py extract -> content/texts_exe.json: the
     dialog boxes of the static image (the language banks keep their sizes)
  3. integrate_snes.do_integrate(content): the console content from the two
     images — or, with --mod, a mod package of edit_server.py --export-mod
     applied instead (edit_server.mod_import)
  4. edit_server.do_pack: content/.compiled/NNNN.bin — the 535 archive records
     (an untouched asset keeps the archive's own LZSS stream) plus the extras
     — and content/exe_static.bin (the texts baked on the image, the LVX
     trailer of the extra level slots kept)

The ROM images: --snes-rom / --genesis-rom, else found by SHA-256 among the
.sfc/.smc/.gen/.bin/.md/.rom files of the working directory, its roms/, the
repo root's roms/ and the directory of DATA.DAT. The known images are the ones
the converters were verified against (the store they produce is byte for byte
the one of the reference build); --any-rom accepts another dump — the
converters check the chunk tables themselves, the result is unverified.

The result has the scratch-tree layout of tools/assets/edit_server.py, so the
editor can work on it (--scratch content). Usage:
  python3 tools/assets/build_content.py [--data DATA.DAT] [--out content] [--fresh]
                                       [--snes-rom X.sfc --genesis-rom Y.gen | --mod PKG.json]
The same tree ships in the V2_ONLY release bundles as content-tools/ (the
scripts and their repo inputs, no game data): run it from the bundle directory
with DATA.DAT and the two images beside the executable.
"""
import argparse
import hashlib
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
# the images the converters were verified against (tools/assets/snes2pc.py, smd2pc.py)
SNES_ROM_SHA256 = "4bf0ef43b5e47ea253157b1a1cba87a523e639cac3733617e049e8e6dd617af2"
GENESIS_ROM_SHA256 = "0d71e903be21b77c9a77ee84d5990fd84a07aa7ec876ff733efbf05497388b83"
ROM_EXTS = (".sfc", ".smc", ".gen", ".bin", ".md", ".rom")
ROM_SIZE_MIN, ROM_SIZE_MAX = 0x80000, 0x800000


def fail(msg):
    sys.stderr.write("build_content: " + msg + "\n")
    sys.exit(1)


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for blk in iter(lambda: f.read(1 << 20), b""):
            h.update(blk)
    return h.hexdigest()


def find_data(arg):
    if arg:
        return os.path.abspath(arg)
    for cand in (os.path.join(os.getcwd(), "DATA.DAT"), os.path.join(ROOT, "DATA.DAT")):
        if os.path.exists(cand):
            return cand
    fail("DATA.DAT not found — pass --data <path> (the archive of a legal copy of the 1992 game)")


def find_rom(arg, want, name, flag, data_dir, any_rom):
    """--<flag> <file> (its hash checked unless --any-rom), else the file with
    the known hash among the ROM-shaped files of the usual directories."""
    if arg:
        p = os.path.abspath(arg)
        if not os.path.exists(p):
            fail(f"--{flag}: {p} not found")
        h = sha256_of(p)
        if h != want and not any_rom:
            fail(f"{p}: SHA-256 {h[:16]}… is not the {name} image the converters were verified "
                 f"against ({want[:16]}…); --any-rom uses it anyway (unverified result)")
        return p
    dirs, seen = [], set()
    for d in (os.getcwd(), os.path.join(os.getcwd(), "roms"), os.path.join(ROOT, "roms"), data_dir):
        d = os.path.abspath(d)
        if d not in seen and os.path.isdir(d):
            seen.add(d)
            dirs.append(d)
    for d in dirs:
        for fn in sorted(os.listdir(d)):
            p = os.path.join(d, fn)
            if not os.path.isfile(p) or os.path.splitext(fn)[1].lower() not in ROM_EXTS:
                continue
            if not ROM_SIZE_MIN <= os.path.getsize(p) <= ROM_SIZE_MAX:
                continue
            if sha256_of(p) == want:
                return p
    fail(f"the {name} image was not found — pass --{flag} <file> (searched " + ", ".join(dirs) + ")")


BAC_INSTALLS = (   # the collection's assets/ in the usual installations
    "/mnt/win/Program Files (x86)/Blizzard Arcade Collection/assets",
    "C:/Program Files (x86)/Blizzard Arcade Collection/assets",
    "C:/Program Files (x86)/Steam/steamapps/common/Blizzard Arcade Collection/assets",
    "~/.steam/steam/steamapps/common/Blizzard Arcade Collection/assets",
    "~/.local/share/Steam/steamapps/common/Blizzard Arcade Collection/assets",
)


def bac_pair(d):
    """(locale.strings, lv_snes_strings.json) in d: the two files flat, or the
    collection's assets/ layout (strings/locale.strings beside lv_snes_strings.json)."""
    for loc in (os.path.join(d, "locale.strings"), os.path.join(d, "strings", "locale.strings")):
        keys = os.path.join(d, "lv_snes_strings.json")
        if os.path.isfile(loc) and os.path.isfile(keys):
            return loc, keys
    return None


def find_bac(arg, data_dir):
    """--bac: the collection's assets/ (or its parent), a directory holding the
    two files, or the locale.strings file itself; else the usual places. None =
    no translations (English only)."""
    if arg:
        p = os.path.abspath(os.path.expanduser(arg))
        cands = [os.path.dirname(p)] if os.path.isfile(p) else [p, os.path.join(p, "assets")]
        for d in cands:
            pair = bac_pair(d)
            if pair:
                return pair
        fail(f"--bac {arg}: locale.strings + lv_snes_strings.json not found there (the collection's assets/ holds "
             "strings/locale.strings and lv_snes_strings.json)")
    dirs = [os.getcwd(), os.path.join(os.getcwd(), "bac"), ROOT, os.path.join(ROOT, "bac"), data_dir]
    dirs += [os.path.expanduser(d) for d in BAC_INSTALLS]
    for d in dirs:
        pair = bac_pair(d)
        if pair:
            return pair
    return None


def main():
    ap = argparse.ArgumentParser(description="build the console content pack for the V2_ONLY engine")
    ap.add_argument("--data", help="DATA.DAT (default: ./DATA.DAT, then the repo root)")
    ap.add_argument("--out", default="content", help="output directory (default: ./content)")
    ap.add_argument("--snes-rom", help="the SNES DE image (default: found by its SHA-256, see the module doc)")
    ap.add_argument("--genesis-rom", help="the Genesis image (default: found by its SHA-256)")
    ap.add_argument("--any-rom", action="store_true", help="accept images with other hashes (unverified result)")
    ap.add_argument("--mod", help="apply this mod package (edit_server.py --export-mod) instead of converting the images")
    ap.add_argument("--bac", help="the Blizzard Arcade Collection's assets/ (or a directory with its locale.strings + "
                                  "lv_snes_strings.json) for the language banks; default: the usual places, none = English only")
    ap.add_argument("--fresh", action="store_true", help="remove an existing output directory first")
    args = ap.parse_args()
    sys.argv = sys.argv[:1]   # assetc.main() reads argv[1] ("--pack")

    data = find_data(args.data)
    out = os.path.abspath(args.out)
    raw = os.path.join(out, "raw")
    cmap_src = os.path.join(ROOT, "assets_raw", "chunk_map.json")
    lvs_src = os.path.join(ROOT, "assets_raw", "lvs")
    for p, what in ((data, "DATA.DAT"), (cmap_src, "assets_raw/chunk_map.json"), (lvs_src, "assets_raw/lvs")):
        if not os.path.exists(p):
            fail(f"{what} not found: {p}")
    image = next((p for p in (os.path.join(ROOT, "exe_static.bin"), os.path.join(os.getcwd(), "exe_static.bin"))
                  if os.path.exists(p)), None)
    if not image:
        fail("exe_static.bin (the static EXE image: the repo root, or the bundle directory) not found")
    mod = snes = genesis = None
    if args.mod:
        mod = os.path.abspath(args.mod)
        if not os.path.exists(mod):
            fail(f"--mod: {mod} not found")
    else:
        snes = find_rom(args.snes_rom, SNES_ROM_SHA256, "SNES DE", "snes-rom", os.path.dirname(data), args.any_rom)
        genesis = find_rom(args.genesis_rom, GENESIS_ROM_SHA256, "Genesis", "genesis-rom", os.path.dirname(data),
                           args.any_rom)
    bac = None if mod else find_bac(args.bac, os.path.dirname(data))
    if os.path.abspath(os.path.dirname(data)) == out:
        fail("--out must not be the directory of DATA.DAT")
    if os.path.exists(out):
        if not args.fresh:
            fail(f"{out} exists — rerun with --fresh to rebuild it (everything inside is replaced)")
        shutil.rmtree(out)
    os.makedirs(raw)
    # every user path is absolute from here on; the tools address their own
    # tables relative to the repo root (tools/data/disasm.py, lvsc.py ...)
    os.chdir(ROOT)

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

    # 2. the open tree, judged byte-exact against the archive; the image's texts
    print(f"[2/4] converting the {ARCHIVE_CHUNKS} chunks into the open tree {out}")
    import assetc
    assetc.RAW = raw
    assetc.ASSETS = out
    assetc.main()
    import texts_exe as TX
    with open(os.path.join(out, "texts_exe.json"), "w") as f:
        json.dump(TX.extract(TX.load_image(image)), f, indent=1)
    # the translations for the language banks: the table bac_strings.py aligns
    # from the user's Blizzard Arcade Collection (Blizzard's text — never in
    # the repository); without it build_locale leaves the English original
    if bac:
        print(f"      translations: {bac[0]}")
        import bac_strings
        bac_strings.build(bac[0], bac[1], os.path.join(out, "texts_exe.json"), os.path.join(out, "bac_lv_locale.json"))
    elif not mod:
        print("      no Blizzard Arcade Collection found (locale.strings + lv_snes_strings.json): the language banks are "
              "skipped, English only — --bac <dir> adds them")

    # 3. the console content
    import edit_server as E
    E.SCRATCH = out
    if mod:
        print(f"[3/4] applying {mod}")
        n_src = E.mod_import(mod)
        source = f"{n_src} files of {mod}"
    else:
        print(f"[3/4] converting the console images: {snes}, {genesis}")
        os.environ["LV_SNES_ROM"] = snes
        os.environ["LV_GENESIS_ROM"] = genesis
        import integrate_snes as I
        slots = I.do_integrate(out)
        source = f"{len(slots)} console level slots from the two images"

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
        fail("no exe_static.bin came out of step 3")
    img = open(exe, "rb").read()
    trailer = re.findall(rb"LVX[0-9A-Z]", img[STATIC_IMAGE:])
    if len(img) <= STATIC_IMAGE or not trailer:
        fail(f"{exe}: no LVX trailer past the static image ({len(img)} B)")
    print(f"done: {out}")
    print(f"      {n_rec} records in .compiled ({ARCHIVE_CHUNKS} archive ids, {len(extras)} extras, "
          f"{ARCHIVE_CHUNKS + len(extras) - n_rec} of them replacing an archive id); {source}; "
          f"exe_static.bin {len(img)} B with the {trailer[0].decode()} trailer")
    print("      V2_ONLY build: a content/ directory beside the executable (or in the working")
    print("      directory) is picked up by itself; elsewhere:")
    print(f"      V2_ASSETS_DIR={out}/.compiled V2_EXE_STATIC={exe} ./vikings")
    return 0


if __name__ == "__main__":
    sys.exit(main())
