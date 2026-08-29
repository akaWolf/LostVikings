#!/usr/bin/env python3
"""playtest.py — one-command edit-and-run loop for the level editor.

Takes editor exports (tilemap .json / level-header .json downloaded from
edit_*.html, or hand-edited copies), overlays them on a SCRATCH copy of
assets/ (the canonical tree is never touched), packs it with assetc, and
launches the game against the scratch tree. Optionally replays a recorded
input and snapshots a frame (V2_LADDER_SNAP) for headless before/after
checks — the exact flow that proved the editor end-to-end.

Examples:
  # play interactively with an edited tilemap
  tools/assets/playtest.py 00CB.json

  # headless: replay level1, snap frame 1300 to out.png
  tools/assets/playtest.py 00CB.json 00CA.json \
      --replay tests/replays/level1.inp --snap 1300 -o out.png
"""
import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import assetc  # noqa: E402
import level_render as LR  # noqa: E402

SUBDIR_BY_FORMAT = {
    "tilemap_u16": "tilemaps",
    "level_header_stripe": "level_headers",
}


def ppm_to_png(src, dst):
    d = open(src, "rb").read()
    px = d.split(b"\n", 3)[3]
    w, h = 320, 200
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += px[y * w * 3:(y + 1) * w * 3]

    def chunk(t, data):
        return (struct.pack(">I", len(data)) + t + data +
                struct.pack(">I", zlib.crc32(t + data) & 0xFFFFFFFF))
    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))
    open(dst, "wb").write(png)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("edits", nargs="+",
                    help="editor-export .json files (tilemap / level header)")
    ap.add_argument("--scratch", default="/tmp/lv_playtest",
                    help="scratch assets tree (recreated each run)")
    ap.add_argument("--replay", help="input replay for a headless run")
    ap.add_argument("--max-frames", type=int, default=0)
    ap.add_argument("--snap", type=int, default=0,
                    help="V2_LADDER_SNAP frame (implies headless-style env)")
    ap.add_argument("-o", "--out", default="/tmp/playtest_snap.png")
    ap.add_argument("--no-run", action="store_true",
                    help="only build the scratch tree + .compiled")
    args = ap.parse_args()

    root = LR.ROOT
    scratch = os.path.abspath(args.scratch)
    assert scratch != os.path.join(root, "assets")
    if os.path.exists(scratch):
        shutil.rmtree(scratch)
    shutil.copytree(os.path.join(root, "assets"), scratch)

    for path in args.edits:
        with open(path) as f:
            js = json.load(f)
        sub = SUBDIR_BY_FORMAT.get(js.get("format"))
        if sub is None:
            sys.exit(f"{path}: unknown format {js.get('format')!r} "
                     f"(expected one of {sorted(SUBDIR_BY_FORMAT)})")
        cid = js["chunk"].upper()
        dst = os.path.join(scratch, sub, f"{cid}.json")
        shutil.copyfile(path, dst)
        print(f"edit: {path} -> {sub}/{cid}.json")

    assetc.ASSETS = scratch
    assetc.pack()

    if args.no_run:
        print(f"scratch ready: V2_ASSETS_DIR={scratch}/.compiled")
        return

    env = dict(os.environ)
    env["V2_ASSETS_DIR"] = os.path.join(scratch, ".compiled")
    cmd = [os.path.join(root, "vikings")]
    if args.replay:
        env.setdefault("SDL_VIDEODRIVER", "dummy")
        env.setdefault("SDL_AUDIODRIVER", "dummy")
        env.setdefault("V2_NOVSYNC", "1")
        env.setdefault("V2_AIL_FRAME_TICKS", "1")
        cmd += ["--replay", args.replay]
        mf = args.max_frames or (args.snap + 100 if args.snap else 0)
        if mf:
            cmd.append(f"--max-frames={mf}")
    if args.snap:
        env["V2_LADDER_SNAP"] = str(args.snap)
        for f in (args.snap, args.snap + 1):
            p = f"/tmp/ladder_f{f}.ppm"
            if os.path.exists(p):
                os.unlink(p)
    print("run:", " ".join(cmd))
    r = subprocess.run(cmd, cwd=root, env=env)
    print(f"exit={r.returncode}")
    if args.snap:
        src = f"/tmp/ladder_f{args.snap}.ppm"
        if os.path.exists(src):
            ppm_to_png(src, args.out)
            print(f"snap -> {args.out}")
        else:
            print(f"snap frame {args.snap} not reached (no {src})")


if __name__ == "__main__":
    main()
