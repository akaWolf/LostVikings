#!/usr/bin/env python3
"""Genesis ground-truth stand, step 1: ROM copies whose ATTRACT MODE plays an
interlude scene (docs2/GENESIS_ROM_INTERNALS.md §11).

The title screen (level 0x2F) times out into the attract loop: 68k 0x120A
advances the attract index [0x3C0] and 0x7312 takes the level from the table
at ROM 0x1238 and the recording from ROM 0x1246 (the scene levels 0x34..0x38
select their own recordings 0x151.. at 0x7330, so the recording table is only
kept consistent). Every slot of both tables is pointed at one scene; the
header checksum (0x18E: the word sum of 0x200..end) is recomputed because the
game verifies it at boot. The original LV.gen is never modified.

  python3 tools/assets/genesis_stand/make_attract_roms.py OUT_DIR
"""
import os, struct, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import smd2pc

SCENES = ((0x34, 0x151, "preh"), (0x35, 0x152, "egypt"), (0x36, 0x153, "factory"),
          (0x37, 0x154, "wacky"), (0x38, 0x155, "ship"))
ATTRACT_LEVELS, ATTRACT_RECORDINGS = 0x1238, 0x1246


def checksum(b):
    return sum(struct.unpack(">H", b[i:i + 2])[0] for i in range(0x200, len(b), 2)) & 0xFFFF


def main(out_dir):
    R = smd2pc.SmdRom().rom
    assert struct.unpack(">H", R[0x18E:0x190])[0] == checksum(R), "unexpected ROM (checksum)"
    lv = [struct.unpack(">h", R[ATTRACT_LEVELS + 2 * i:ATTRACT_LEVELS + 2 * i + 2])[0] for i in range(8)]
    n = lv.index(-1)                       # the -1 sentinel ends the level list
    os.makedirs(out_dir, exist_ok=True)
    for lvl, rec, name in SCENES:
        b = bytearray(R)
        for i in range(n):
            b[ATTRACT_LEVELS + 2 * i:ATTRACT_LEVELS + 2 * i + 2] = struct.pack(">H", lvl)
            b[ATTRACT_RECORDINGS + 2 * i:ATTRACT_RECORDINGS + 2 * i + 2] = struct.pack(">H", rec)
        b[0x18E:0x190] = struct.pack(">H", checksum(b))
        p = os.path.join(out_dir, f"lv_attract_{name}.gen")
        open(p, "wb").write(bytes(b))
        print(p)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "/tmp/lv_genesis_stand")
