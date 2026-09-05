#!/usr/bin/env python3
"""integrate_snes.py — put the five SNES DE exclusives INTO the PC
progression, in the SNES order (task #104 final stage).

Verified mechanics this rides on:
  - the next level lives IN THE LEVEL HEADER at +0x16: the stripe
    decompresses over DS 0x25B3 and that word lands exactly on
    DS_LEVEL_LOAD (0x25C9) — proven with a hw-watchpoint backtrace
    (v2_lzss_decompress <- v2_load_template);
  - end-of-world levels point to scene 0x29 instead — all five
    insertions go MID-world, so no scene logic is touched;
  - level slots 48-52 behave like regular levels in every engine gate
    (the only `level < 0x25` gate is the F5/F6 debug cheat);
  - the SNES/SMD progression order (also byte-verified in the SMD level
    table @0x7EF8): BBLS->TR33->VLCN, JMNN->SNDS->TMPL->TTRS,
    JNKR->RVTS->CBLT, WRLR->PDDY->TRPD;
  - passwords TR33/SNDS/TMPL/RVTS/PDDY (the консоль originals) ship in
    an LVX1 trailer appended to exe_static.bin: the V2 engine reads it
    and extends the header/template tables and the password opcodes
    (op_D2 show / op_D3 verify) — no trailer, no behavior change.

New archive ids (extras.json -> assetc.pack store-mode records):
  each level takes 6: hdr, map, tiles, masks(=tiles+1), gtld, pal.

Usage:
  integrate_snes.py [--scratch /tmp/lv_edit_scratch] [--pack]
"""
import argparse
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import assetc as AC  # noqa: E402
import level_render as LR  # noqa: E402
import snes2pc as SP  # noqa: E402
import smd2pc as SM  # noqa: E402

# Music tracks (header +0x05 -> table @ds:0xA384 -> XMID chunk base, the
# engine adds ds:0x86B8 = the sound-card offset; card 2 = AdLib/OPL).
# Sizes/notes measured from the XMI EVNT streams of the card-2 chunks.
MUSIC_TRACKS = {
    0:  ("1EE", "menu/intro", 724, 27.0, 3),
    1:  ("1E9", "UNUSED — byte-identical to track 5", 1356, 43.7, 0),
    2:  ("1D0", "Spaceship", 1394, 57.3, 8),
    3:  ("1D5", "Caves", 1254, 47.8, 7),
    4:  ("1DA", "Egypt", 1024, 67.5, 6),
    5:  ("1DF", "Factory", 1356, 43.7, 8),
    6:  ("1F3", "short cue", 134, 17.7, 1),
    7:  ("1E4", "Candy", 2267, 94.9, 8),
    8:  ("1F8", "UNUSED — full unique theme", 1236, 57.0, 0),
    9:  ("1FD", "UNUSED — short piece", 222, 23.6, 0),
    10: ("202", "empty (0 notes)", 0, 0.0, 1),
}

# level slot, SNES hdr, donor (same world: head base/banks/anims/.lvs),
# password, next slot, canonical predecessor (its header gets next=slot)
PLAN = [
    dict(slot=48, snes=0x16C, donor="002A", pw=b"TR33", next=10,
         prev_hdr="0032", base=0x217),   # BBLS -> TR33 -> VLCN
    dict(slot=49, snes=0x171, donor="0053", pw=b"SNDS", next=50,
         prev_hdr="0053", base=0x21D),   # JMNN -> SNDS -> TMPL
    dict(slot=50, snes=0x176, donor="0055", pw=b"TMPL", next=16,
         prev_hdr=None, base=0x223),     # SNDS -> TMPL -> TTRS
    dict(slot=51, snes=0x17B, donor="007A", pw=b"RVTS", next=21,
         prev_hdr="0078", base=0x229),   # JNKR -> RVTS -> CBLT
    dict(slot=52, snes=0x180, donor="00A6", pw=b"PDDY", next=32,
         prev_hdr="00A8", base=0x22F),   # WRLR -> PDDY -> TRPD
]
# UX stage 1 (route B, user decision 2026-09-02): every scene slot runs on
# a COPY of its world's template (new chunk ids) with the scene classes
# appended — the D8 timed exit and the D9 letter blocks — so the room's
# props (bubble geyser 4A, Egypt trap/pot/gem, Factory hook/button, Ship
# chairs) run their native world code through the SMD->PC class bijection
# exactly like the five console-exclusive levels do. The canonical world
# templates 1C1-1C5 and the 1C6 scene script stay byte-identical.
SCENE_TMPL = {53: (0x1C2, 0x25D), 54: (0x1C3, 0x25E), 55: (0x1C4, 0x25F),
              56: (0x1C5, 0x260), 57: (0x1C1, 0x261)}
# world template (.lvs) chunks per world of each insert
TMPL_CHUNK = {48: 0x1C2, 49: 0x1C3, 50: 0x1C3, 51: 0x1C4, 52: 0x1C5}

# UX stage 7 — the "SNES balance" option (user 2026-09-02: off by default;
# 2026-09-04: whole-level SNES variants for the levels that differ, no
# cherry-picking, CMB0 stays PC). The 13 levels of VERSIONS_DIFF §5.1 get a
# second head + map/tiles/masks/quads/palette converted from the SNES DE ROM
# (the same snes2pc pipeline as the five exclusives) under NEW chunk ids; the
# LVX trailer lists them with LVX_ALT against the canonical slot number and
# v2_load_template swaps the head in only while the option is on. The
# canonical head, script, password, next level and the parallax pair stay.
LVX_ALT = 0x0010
# GR8T is NOT in the list (user 2026-09-04: "drop the exactly identical ones"):
# its DE data equal the PC data in every byte that matters (map by quad
# content, type bits, spawn records, head) — the 3 type-bit cells of §5.1
# exist only in the 1993 ROM. Every level below differs in at least one of
# those (BBLS also in the viking start block of the head).
BALANCE_LEVELS = [4, 5, 6, 8, 9, 12, 20, 23, 24, 25, 26, 32]   # LLM0 FL0T TRSS CVRN BBLS PHR0 JNKR SMRT V8TR NFL8 WKYY TRPD
BALANCE_BASE = 0x2A0                                             # 6 ids per level: hdr map tiles masks gtld pal


def balance_integrate(scratch, lvx):
    """Convert the 13 SNES-variant levels and append their LVX_ALT records."""
    import json as _json
    rom = SP.SnesRom()
    tables = LR.level_tables()
    pws = LR.level_passwords()
    print("SNES balance variants:")
    for k, lvl in enumerate(BALANCE_LEVELS):
        b = BALANCE_BASE + 6 * k
        pc_hdr, script = tables[lvl]
        de_hdr = rom.rom[0x7686 + lvl * 2] | (rom.rom[0x7686 + lvl * 2 + 1] << 8)
        hp = os.path.join(scratch, "level_headers", f"{pc_hdr:04X}.json")
        pc_raw = bytes.fromhex(_json.load(open(hp))["raw"])
        nxt = pc_raw[0x16] | (pc_raw[0x17] << 8)
        cids = {"hdr": b, "map": b + 1, "tiles": b + 2, "gtld": b + 4, "pal": b + 5}
        info = SP.convert_level(de_hdr, f"{pc_hdr:04X}", scratch, new_cids=cids,
                                next_level=nxt, music=pc_raw[0x05])
        # the console fields of the canonical head (parallax pair refs + fx/fy)
        ap = os.path.join(scratch, "level_headers", f"{b:04X}.json")
        aj = _json.load(open(ap))
        araw = bytearray(bytes.fromhex(aj["raw"]))
        araw[0x34:0x43] = pc_raw[0x34:0x43]
        aj["raw"] = bytes(araw).hex()
        with open(ap + ".tmp", "w") as f:
            _json.dump(aj, f, indent=1)
        os.replace(ap + ".tmp", ap)
        TMPL_CHUNK[lvl] = script
        lvx.append({"slot": lvl, "hdr": b, "pw": pws[lvl].encode(), "flags": LVX_ALT})
        print(f"  level {lvl} {pws[lvl]}: SNES head {de_hdr:04X} -> alt head {b:04X} (next {nxt:02X}, track {pc_raw[0x05]:02X})")
TMPL_CHUNK.update({slot: dst for slot, (src, dst) in SCENE_TMPL.items()})

# The six SMD/Genesis-only scenes (task: embed into gameplay, as on SMD):
# five between-world cutscenes replace the PC timewarp room (scene 0x29)
# on the world edges, the sixth is the game-completion cutscene wedged
# before the PC ending scroller (45). All run as D8 timed scenes that
# transition by their head's +0x16 next field; prev_hdr/prev_next say
# which canonical header edge gets retargeted at the scene slot.
# The chain follows the SNES shape the user pointed at (video refs in
# memory): world end -> the TIMEWARP VORTEX (scene 41, canonical on
# DOS/SNES) -> the SMD world cutscene -> the first level of the next
# world. The vortex edge stays canonical (prev_hdr=None: world enders
# keep next=0x29); the hop vortex->scene comes from the 1C6 ladder
# patch (S_8C00 branch constants), scene->level from the scene heads.
PLAN_SMD = [
    # The world-name banner comes from the ROM itself: the scene stripe's
    # first sprite bank IS the letters chunk (0x144 PREHISTORIA, 0x145
    # EGYPT, 0x146 FACTORY, 0x147 WACKY, 0x148 STARSHIP).
    # de_bg (task #114): the scene field is the START AREA of the world's
    # first DE level (lvl = the DE slot of our `next` level) composited
    # over the DE parallax pair from that level's own header (+0x39 quad
    # map / +0x3D quad table on the level's tileset). Pairs per world:
    # Prehistoria 012/017 (purple mountains + dino silhouettes, row0=1
    # skips the empty sky row of the 32x16 map), Egypt 028/02E
    # (pyramids), Factory 040/03F (brick wall strip, tiled), Wacky
    # 058/057 (checkered candy — the pair of DE 059 = our NFL8),
    # Ship 06B/06D (starfield, tiled; DE 077 = our TFFF).
    # gen_bg (UX plan stage 1): the scene composed from the Genesis data
    # exactly as the Genesis VDP (and thus the BAC Definitive Edition)
    # shows it — camera/spots per world in genesis_scene.WORLD_CAMERA.
    # Worlds without a fitted camera yet fall back to the de_bg bake.
    # donor = the PC level of the world whose stripe (sprite banks, anim
    # chunks, viking palette rows) covers every prop class the SMD scene
    # spawns (measured over the world's headers): Preh BBLS 0032 (4A geyser
    # + 61 bubbles), Egypt 004F (6D trap, 0A trigger, 16 pot, 0B gem),
    # Factory 007E (92 button, 9B hook), Ship TFFF 00CE (86 chairs); Wacky's
    # scene carries no props (00A6 as before)
    dict(slot=53, smd=0x13D, donor="0032", pw=b"CUT1", next=4,
         prev_hdr=None, base=0x235,    # vortex(prev=GRND) -> scene -> LLM0
         gen_bg=dict(world="preh"),
         de_bg=dict(lvl=0x01A, map=0x12, gt=0x17, world="preh")),
    dict(slot=54, smd=0x13E, donor="004F", pw=b"CUT2", next=11,
         prev_hdr=None, base=0x23B,    # vortex(prev=VLCN) -> scene -> QCKS
         gen_bg=dict(world="egypt"),
         de_bg=dict(lvl=0x02F, map=0x28, gt=0x2E, world="egypt")),
    dict(slot=55, smd=0x13F, donor="007E", pw=b"CUT3", next=17,
         prev_hdr=None, base=0x241,    # vortex(prev=TTRS) -> scene -> JLLY
         gen_bg=dict(world="factory"),
         de_bg=dict(lvl=0x041, map=0x40, gt=0x3F, world="factory")),
    dict(slot=56, smd=0x140, donor="00A6", pw=b"CUT4", next=25,
         prev_hdr=None, base=0x247,    # vortex(prev=V8TR) -> scene -> NFL8
         gen_bg=dict(world="wacky"),
         de_bg=dict(lvl=0x05B, map=0x55, gt=0x54, world="wacky")),
    dict(slot=57, smd=0x141, donor="00CE", pw=b"CUT5", next=33,
         prev_hdr=None, base=0x24D,    # vortex(prev=TRPD) -> scene -> TFFF
         gen_bg=dict(world="ship"),
         de_bg=dict(lvl=0x077, map=0x6B, gt=0x6D, world="ship")),
    # NO slot for SMD 0x08C: that is the game-completion scene, and the
    # PC has its OWN version at slot 46 (00DA forest — vikings + the
    # 4B/4C props + music track 8; the SNES version is 0x082 with track
    # 9, same class set). The canonical MSTR -> 45 -> 46 ending stays.
]

# S_8C00 world ladder of the 1C6 scene script: branch constant -> our
# scene slot (the ladder writes DS_LEVEL_LOAD after the vortex scene).
LADDER_PATCH = {0x04: 53, 0x0B: 54, 0x11: 55, 0x19: 56, 0x21: 57}


def patch_1c6_ladder(scratch):
    """Retarget the five world-entry branches of the S_8C00 ladder at the
    SMD scenes. Text edit of the .lvsf (same literal widths — the lvsc
    build keeps every offset); idempotent."""
    p = os.path.join(scratch, "level_scripts", "1C6.lvsf")
    lines = open(p).read().split("\n")
    # locate the ladder head: "o 51 0300" followed by "o 74 ab25 ..."
    at = -1
    for i in range(len(lines) - 1):
        if lines[i] == "o 51 0300" and lines[i + 1].startswith("o 74 ab25"):
            at = i
            break
    if at < 0:
        raise SystemExit("1C6 ladder head not found")
    patched = 0
    for i in range(at, min(at + 40, len(lines))):
        m = lines[i]
        if m.startswith("o 51 ") and lines[i + 1] == "o 57 c925":
            val = int(m[5:7], 16) | (int(m[7:9], 16) << 8)
            if val in LADDER_PATCH:
                nv = LADDER_PATCH[val]
                lines[i] = "o 51 %02x%02x" % (nv & 0xFF, nv >> 8)
                patched += 1
            elif val in LADDER_PATCH.values():
                patched += 1        # already retargeted (idempotent rerun)
    if patched != len(LADDER_PATCH):
        raise SystemExit(f"1C6 ladder: patched {patched} branches, "
                         f"expected {len(LADDER_PATCH)}")
    tmp = p + ".tmp"
    with open(tmp, "w") as f:
        f.write("\n".join(lines))
    os.replace(tmp, p)
    print(f"  1C6 ladder: {patched} world branches -> scene slots")


def patch_next(scratch, hdr_cid_hex, next_level):
    """Set the +0x16 next-level word of a canonical header in scratch."""
    p = os.path.join(scratch, "level_headers", f"{hdr_cid_hex}.json")
    with open(p) as f:
        j = json.load(f)
    raw = bytearray(bytes.fromhex(j["raw"]))
    old = raw[0x16] | (raw[0x17] << 8)
    raw[0x16], raw[0x17] = next_level & 0xFF, next_level >> 8
    j["raw"] = bytes(raw).hex()
    tmp = p + ".tmp"
    with open(tmp, "w") as f:
        json.dump(j, f, indent=1)
    os.replace(tmp, p)
    print(f"  next({hdr_cid_hex}): {old} -> {next_level}")


LVX_FULLSCREEN = 0x0001   # slot is presented on all 200 rows (no HUD band)
LVX_CAMLOCK = 0x0002      # camera parked at (pin_x, pin_y) + both scroll axes
                          # locked (v2_lvx_pin_camera); the pin = flags bits
                          # 4-11 (x, 0..255) / 12-15 (y, 0..15) = the scene
                          # map's off-screen left columns + the Genesis
                          # camera mod 16 (genesis_scene.layout, UX stage 1)
LVX_TRIO = 0x0004         # the record's 18B = sub_11446 mode-2 viking table
LVX_NOGATE = 0x0008       # no sub_10813 off-screen input gate: the Genesis
                          # recordings walk the trio in from x -68 (the SMD
                          # engine has no such gate — docs2/GENESIS_ROM_INTERNALS.md)
                          # (3 x {x,y,anim} for Erik/Baleog/Olaf, ds:0x8508):
                          # the scene head's +0x07 mode byte is 2 (UX stage 1)


def build_lvx(scratch, entries):
    """exe_static.bin + LVX4 trailer: [magic][u16 n][32B records:
    level u16, hdr_cid u16, tmpl_cid u16, pw 4B, demo_cid u16, flags u16,
    trio 18B (3 x {x i16, y u16, anim u16}; zeros unless LVX_TRIO)].
    demo_cid != 0 arms the canonical attract-demo machinery on the slot
    (sub_12d72 RLE input replay, ac=0x8000) — the scene choreography.
    flags (UX plan stage 0): LVX_FULLSCREEN | LVX_CAMLOCK on the interlude
    scene slots; 0 on the gameplay inserts (the engine reads them via
    v2_lvx_flags; LVX1/LVX2 images decode with flags = 0)."""
    src = os.path.join(scratch, "exe_static.bin")
    if not os.path.exists(src):
        src = os.path.join(LR.ROOT, "exe_static.bin")
    img = open(src, "rb").read()
    # strip a previous trailer of any version (idempotent rebuilds)
    for magic in (b"LVX4", b"LVX3", b"LVX2", b"LVX1"):
        m = img.rfind(magic)
        if m >= 0 and m >= len(img) - 4 - 2 - 32 * 64:
            img = img[:m]
    tr = b"LVX4" + struct.pack("<H", len(entries))
    for e in entries:
        tr += struct.pack("<HHH", e["slot"], e["hdr"], TMPL_CHUNK[e["slot"]])
        tr += e["pw"]
        tr += struct.pack("<HH", e.get("demo", 0), e.get("flags", 0))
        tr += e.get("trio") or bytes(18)
    out = os.path.join(scratch, "exe_static.bin")
    tmp = out + ".tmp"
    with open(tmp, "wb") as f:
        f.write(img + tr)
    os.replace(tmp, out)
    print(f"  exe_static: {len(img)}B + LVX4 trailer {len(tr)}B -> {out}")


# ---------------------------------------------------------------------------
# #113 scene choreography: the vikings on the SNES/SMD interludes MOVE on a
# scripted demo (the video: Olaf rides his shield past, Erik answers and
# shows his sprint). PC plays it through the canonical attract machinery:
# ac=0x8000 -> sub_12d72 pops RLE (keys u16, count u16) pairs from the chunk
# loaded at ds:2193 and ORs them into the input word — so the choreography
# is DATA: one small chunk per scene. Action bits of the input word
# ([3B6]/[3B8]): RIGHT 0x100, LEFT 0x200, UP(ladder/jump) 0x800, DOWN
# 0x400, NEXT viking 0x10 / PREV 0x20 (sub_12e84 — the gameplay switch;
# 0x2000 = TAB = the pause/inventory menu, live only on HUD levels).
# No ACTION 0x8000 bit — that is the D8 skip button. The tail (0, 0x7FFF)
# parks the input silent until the D8 timer ends the scene.
DEMO_CID = {53: 0x253, 54: 0x254, 55: 0x255, 56: 0x256, 57: 0x257}

# UX stage 1 (2026-09-03, reversed from the SMD 68k — docs2/GENESIS_ROM_INTERNALS
# .md §7): the Genesis scenes drive their vikings with INPUT RECORDINGS. ROM
# chunks 0x151..0x155 (68k @0x72EA: level 0x34.. -> 0x151 + (level - 0x34))
# hold pairs (key16 BE, count16 BE) = the key word held count ticks (player
# @0x172A); the key bits are the engine's own (R 0x100, L 0x200, D 0x400,
# U 0x800, ACTION 0x8000, NEXT 0x10, PREV 0x20, S 0x80 = use/talk); the tail
# (ESC 0x1000, 0xFFFF) = ESC held forever = the scene ends (our D8's ESC
# exit fires on the first ESC tick). The PC demo stream (sub_12d72) has the
# same pair semantics in LE — the recording ships byte-swapped, verbatim.
# No hand-made walk-ins/events/timings: the lines are the E1 talk spots of
# the scene head (smd2pc.convert_scene) answering the vikings' S presses.
SMD_RECORDING = {53: 0x151, 54: 0x152, 55: 0x153, 56: 0x154, 57: 0x155}
TICK_HZ = 70.0        # PC engine ticks per second (VGA 70 Hz); the Genesis ran its recordings at 60


def smd_recording(slot):
    """The scene's Genesis input recording as [(key, count)] (host ints)."""
    b = SM.SmdRom().chunk(SMD_RECORDING[slot])
    assert len(b) % 4 == 0 and len(b) >= 8, (slot, len(b))
    pairs = [struct.unpack(">HH", b[i:i + 4]) for i in range(0, len(b), 4)]
    assert pairs[-1] == (0x1000, 0xFFFF), (slot, pairs[-1])   # the ESC tail
    return pairs


def smd_recording_ticks(slot):
    """Ticks from the scene start to the recording's ESC tail."""
    return sum(n for _, n in smd_recording(slot)[:-1])


def write_demo_chunks(scratch):
    import json as _json
    ex_path = os.path.join(scratch, "extras.json")
    extras = _json.load(open(ex_path)) if os.path.exists(ex_path) else {}
    for slot, cid in DEMO_CID.items():
        script = smd_recording(slot)
        blob = b"".join(struct.pack("<HH", k, n) for k, n in script)
        d = os.path.join(scratch, "unreferenced")
        os.makedirs(d, exist_ok=True)
        p = os.path.join(d, f"{cid:04X}.bin")
        tmp = p + ".tmp"
        with open(tmp, "wb") as f:
            f.write(blob)
        os.replace(tmp, p)
        extras[f"{cid:04X}"] = {"role": "unreferenced"}
        busy = sum(n for _, n in script[:-1])
        print(f"  demo chunk {cid:04X} (slot {slot}): {len(script)} pairs, "
              f"{busy} ticks (~{busy / TICK_HZ:.1f} s) of choreography")
    tmp = ex_path + ".tmp"
    with open(tmp, "w") as f:
        _json.dump(extras, f, indent=1)
    os.replace(tmp, ex_path)



# The 1C6 D8 timed-scene controller, byte for byte (S_8A90..S_8AD0 + its
# one-byte anim "0E" at S_8AD0 — read off the canonical chunk 01C6 with
# level_render.read_payload): prolog jmp (record pc; spawn enters at pc+3),
# `19 anim / 2F / acc=0x1000 / 62 08 (OR field 08) / field[16]==0 ? default
# duration : field[16] -> field[1C] / loop: yield, field[1C]==0 -> exit,
# 97 / AA [03B8]&0x18 -> exit / 99 [03B8]&0x1E / A8 [0100] -> exit (the
# button edges = early exit) / jmp loop / exit: 0F (VM exit + [334]|=1 =
# level end -> the head's next)`. Six absolute operands relocate; the
# default duration constant is ours (the scene length).
D8_CANON = bytes.fromhex(
    "03d79419d08a2f5100106208510000781 6ab8a513c00561c03af8a5216561c0001"
    "510000731ccf8a97000100aa18b803cf8a991eb803a8000100cf8a03af8a0f0e"
    .replace(" ", ""))
D8_RELOC = [(1, 0x94D7, 3), (4, 0x8AD0, 0x40), (17, 0x8AAB, 0x1B),
            (25, 0x8AAF, 0x1F), (38, 0x8ACF, 0x3F), (48, 0x8ACF, 0x3F),
            (58, 0x8ACF, 0x3F), (61, 0x8AAF, 0x1F)]   # (offset, canon, rel)

def d8_timer_blob(base, ticks):
    """The D8 controller relocated to `base` with the default duration
    `ticks` (1C6 ships 0x3C; our scenes ran 300 on the patched 1C6). The
    prolog jmp (S_94D7 = 1C6's shared object setup, which the world
    templates keep elsewhere) retargets to base+3: D8 needs none of it and
    the spawn path enters at pc+3 anyway."""
    # UX stage 1 (E0 port): the letters take their block index from the
    # shared counter [0x158] (e0_letters_blob) — the D8 (spawn row 0, the
    # first object to run) zeroes it, `51 0000 57 5801`, right after the
    # prolog; every canon operand below shifts by the 6 inserted bytes.
    INS = bytes((0x51, 0x00, 0x00, 0x57, 0x58, 0x01))
    b = bytearray(D8_CANON[:3] + INS + D8_CANON[3:])
    assert len(b) == 0x41 + len(INS), len(b)
    for off, canon, rel in D8_RELOC:
        if off >= 3: off += len(INS)            # operand positions after the insert
        if off > 3: rel += len(INS)             # code targets shift; the prolog's
                                                # base+3 = the insert itself stays
        assert b[off] | (b[off + 1] << 8) == canon, (off, b[off:off + 2].hex())
        tgt = base + rel
        b[off], b[off + 1] = tgt & 0xFF, tgt >> 8
    o = 19 + len(INS)
    assert b[o:o + 3] == bytes((0x51, 0x3C, 0x00)), b[o:o + 3].hex()
    b[o + 1], b[o + 2] = ticks & 0xFF, ticks >> 8
    # The 1C6 loop ends the scene on two input edges: `aa 18 b803` (ESC,
    # LUT_BIT_MASK idx 0x18 = 0x1000) and `99 1e b803 / a8 000100` (ACTION
    # 0x8000 = the jump/attack button). The interludes are DEMO-driven and
    # the choreography presses ACTION (Erik's jumps), so the ACTION exit
    # goes (six 0x01 NOPs keep every offset); the ESC skip stays — and the
    # engine's own demo path (v2_transition_kick_102ad) skips on any key
    # anyway, the intro mask turns a keypress into 0xFFFF.
    a8 = D8_CANON.find(bytes.fromhex("a8000100cf8a"))   # canon offset (relocated above)
    assert a8 > 0, "D8 canon: ACTION exit not found"
    a8 += len(INS)
    b[a8:a8 + 6] = bytes([0x01] * 6)
    return bytes(b)


# The Prehistoria bubbles = the SMD room's own geyser and bubble classes as
# BYTECODE (docs2/GENESIS_ROM_INTERNALS.md §11; tools/assets/smd_lvs.py 4A/25/
# 24/23): the geyser 4A (bank 4 @0x2AB43..0x2B065; the scene row's pool 0x46
# selects its lane cycle 0x46..0x4E: one bubble per 20 ticks at x+0xC0/+0x20/
# +0/+0x86 (small 0x23)/+0x50/+0xE0/+0x30/+0x54 (medium 0x24)/+0x50, then the
# counter reloads from field 34 = the pool), the bubbles 0x25/0x24/0x23 (bank
# 3 @0x25C38/0x25D46/0x25D92: rise 1 px/tick, wobble, pop after 110 ticks;
# a touching viking — `2C 79` search, dy >= -10 — gets the bubble's DY added
# to its OBJ_VEL_Y (`5B 3a`) = the ride Olaf takes to the right shelf) and
# their six anims (0x25DDB..0x25E76: static / wobble+pop per size). Port
# edits, all documented: the anim frame operands map onto our pool units
# (smd2pc decor: 32x32 stride 16 -> 16, 16x16 4 -> 8, 8x8 1 -> 4), a `15
# <type>` sprite-type prefix is added to the big (01 = 32 rows) and small
# (00 = 8 rows) anims the SMD lets the record imply (the medium one carries
# its own `15 02`), the three pop sounds (SMD ids 0x28/0x2A/0x29) are
# dropped (no PC ids known), and the classes' despawn exits (`1C -> 0x225A0`
# = op 10) go to a one-byte `10` stub in the blob.
BUB_ANIM = [  # (smd_start, smd_end, size, add_type_prefix)
    (0x25DDB, 0x25DE8, 32, True), (0x25DE8, 0x25E0E, 32, True),
    (0x25E0E, 0x25E1D, 16, False), (0x25E1D, 0x25E43, 16, True),
    (0x25E43, 0x25E50, 8, True), (0x25E50, 0x25E72, 8, True),
    (0x25E72, 0x25E76, 0, False)]                 # the shared `0E / 03 -> self` end block
BUB_ANIM_END = 0x25E72
BUB_TYPE = {32: 0x01, 16: 0x02, 8: 0x00}          # cs:32D7 rows: [0]=8 [1]=32 [2]=16
BUB_STRIDE = {32: (16, 16), 16: (4, 4), 8: (1, 1)}   # (smd tile stride, pc unit stride: type 2 = 16, type 4 = 4, type 1 = 1)
BUB_CLASSES = {0x25: (0x25C38, 0x25D43), 0x24: (0x25D46, 0x25D8F), 0x23: (0x25D92, 0x25DDB)}
GEYSER_4A = (0x2AB43, 0x2B068)
SMD_DESPAWN = 0x225A0                              # `03 a0a5` prolog target = op 10
BUBBLE_SLOTS = {53}                                # Prehistoria only


def bubble_anims(base, frames):
    """The six bubble anims + the end block relocated to `base`; returns
    (bytes, {smd_anim_addr: pc_addr})."""
    import smd_lvs as SL
    out = bytearray(); amap = {}
    # pass 1: sizes (the jumps need the end block's final address)
    def build(end_addr):
        out = bytearray(); amap = {}
        for a0, a1, size, prefix in BUB_ANIM:
            amap[a0] = base + len(out)
            if prefix:
                out += bytes((0x15, BUB_TYPE[size]))
            pc = a0
            while pc < a1:
                op = SL.R[pc]
                if op == 0x01:                           # frame (1-byte payload here)
                    fr = SL.R[pc + 1]
                    if size:
                        ss, ps = BUB_STRIDE[size]
                        assert fr % ss == 0, (hex(pc), fr)
                        fr = frames[size] + (fr // ss) * ps
                        assert fr < 256, fr
                    out += bytes((0x01, fr)); pc += 2
                elif op == 0x02:                         # sound: dropped (no PC id)
                    pc += 3
                elif op == 0x03:                         # jump -> the end block
                    tgt = 0x28000 + struct.unpack("<h", SL.R[pc + 1:pc + 3])[0]
                    assert tgt == BUB_ANIM_END, hex(tgt)
                    out += bytes((0x03,)) + struct.pack("<H", end_addr); pc += 3
                elif op in (0x08, 0x0A):                 # X/Y absolute, one sub-sprite
                    out += SL.R[pc:pc + 3]; pc += 3
                elif op in (0x0C, 0x0F, 0x15):           # layer bits / delay / type: 1-byte payload
                    out += SL.R[pc:pc + 2]; pc += 2
                elif op == 0x0E:                         # end frame
                    out += bytes((0x0E,)); pc += 1
                else:
                    raise AssertionError(f"bubble anim: unexpected cmd {op:02X} @{pc:X}")
            assert pc == a1, (hex(pc), hex(a1))
        return bytes(out), amap
    b0, m0 = build(0)
    end_addr = m0[BUB_ANIM_END]
    b1, m1 = build(end_addr)
    assert len(b1) == len(b0) and m1[BUB_ANIM_END] == end_addr
    return b1, m1


def bubble_blobs(base, frames):
    """[anims][cls 25][cls 24][cls 23][geyser 4A][10] at `base`; returns
    (bytes, {cls: record code address})."""
    import smd_lvs as SL
    anims, amap = bubble_anims(base, frames)
    parts = [anims]; addr = {}
    def total(): return sum(len(x) for x in parts)
    # the despawn stub `10` goes last: its address needs the final layout,
    # so lay the classes out twice (their sizes do not depend on it)
    def layout(stub):
        parts[:] = [anims]; addr.clear()
        for cls, (r0, r1) in BUB_CLASSES.items():
            a = base + total(); addr[cls] = a
            blob, _ = SL.port_blob([(r0, r1)], 3, a, op19_map=amap, extra_targets={SMD_DESPAWN: stub})
            parts.append(blob)
        a = base + total(); addr[0x4A] = a
        blob, _ = SL.port_blob([GEYSER_4A], 4, a, extra_targets={SMD_DESPAWN: stub})
        parts.append(blob)
        return base + total()
    stub = layout(0)
    stub2 = layout(stub)
    assert stub2 == stub
    parts.append(bytes((0x10,)))
    return b"".join(parts), addr


D8_REST = "04000000101000000500000000000000"   # 1C6's D8 record body
# the SMD E1 record body in PC shape (docs2/GENESIS_ROM_INTERNALS.md §2/§6):
# +5 = the SMD bank byte 3, +9/+A 16x16, +D = 5, +F = CLASS_BITS 0x0040 —
# the group the vikings' use probe (op 38 filter 0x0040) looks for
E1_REST = "03000000101000000500400000000000"
# the SMD E0 record body in PC shape: bank 4, 32x32, +D = 5, VEL_X/Y_MAX 0x1000
# (the Starship letters fly in at exactly 16 px/tick — a smaller clamp would
# slow them down)
E0_REST = "04000000202000000500000000100010"
# D8 timer durations (16-bit tick counts, d8_timer_blob). The scene really
# ends when the Genesis recording reaches its ESC tail (smd_recording, the
# D8's ESC exit); the timer is the fallback two seconds behind it.
SCENE_TICKS = {slot: smd_recording_ticks(slot) + 140 for slot in SMD_RECORDING}
TMPL_BUF = 0xC00 * 16    # the template lives in the animdata segment: 0xC00 paragraphs
# Where the blocks go when the template has no room past its payload: the
# Ship template 1C1 is 48972 B (180 B short of the buffer), so its scene
# copy overlays the blocks onto a class region the scene never runs —
# class D2 (S_3311..S_3694, 899 B): no Ship level spawns D2, no label in
# the region is referenced from outside it (the .lvsf reference graph: 0
# incoming; only its own `X = X+0` aliases), and the D2 record is pointed
# at the 13DB stub like 1C6 does. The region keeps its exact length (the
# blob is zero-padded), so nothing else in the template shifts.
# 2026-09-03: the E1 talk-spot class (187 B) joined the blocks and the Ship
# tail (180 B) cannot take it next to the D8 — the adjacent D7 region
# (S_3694..S_374A, 182 B, also unreached from the Ship levels' spawns and
# op-14 templates: lvs_full.full_walk) merges into the overlay region;
# both victim records go to the 13DB stub.
FREE_REGION = {0x1C1: (0x3311, 0x374A, ["D2", "D7"])}


def build_scene_templates(scratch, nblk=8, ticks=SCENE_TICKS, frames=None, text_idx=None):
    """One template per scene slot: the canonical world template text
    (assets_raw/lvs/<src>.lvsf) under a new chunk id, its orphan D8/E0/E1
    records retargeted at the blocks — appended past the payload when
    the 48K template buffer has room, else overlaid on FREE_REGION.
    Registered in extras.json as role level_script."""
    ex_path = os.path.join(scratch, "extras.json")
    extras = json.load(open(ex_path)) if os.path.exists(ex_path) else {}
    import genesis_scene as GS
    pin_y_of = {e["slot"]: GS.layout(e["gen_bg"]["world"], e["gen_bg"])["pin"][1]
                for e in PLAN_SMD if e.get("gen_bg")}
    for slot, (src, dst) in sorted(SCENE_TMPL.items()):
        pin_y = pin_y_of.get(slot, 0)
        txt = open(os.path.join(AC.RAW, "lvs", f"{src:X}.lvsf")).read()
        m = re.search(r"^chunk %04X size (\d+)$" % src, txt, re.M)
        if not m:
            raise SystemExit(f"template {src:X}: size header not found")
        n = int(m.group(1))
        # the banner blocks ride behind the world's pool bank 0x12F in the
        # scene's bank 0 (smd2pc.convert_scene, gen_bg): block K = pool
        # frame len(0x12F)/72 + 1 + 16K
        pool_len = len(LR.read_payload(0x12F, "lzss")[0])
        assert pool_len % 72 == 0, pool_len
        frame0 = pool_len // 72 + 1
        bub = (frames or {}).get(slot) if slot in BUBBLE_SLOTS else None   # {32/16/8: pool unit base}
        talk = text_idx is not None            # the E1 talk-spot class (e1_talk_blob)
        t_slot = ticks[slot] if isinstance(ticks, dict) else ticks
        # the blocks, sized at base 0 (rebuilt at their final addresses)
        sz_timer = len(d8_timer_blob(0, t_slot))
        sz_letters = len(e0_letters_blob(0, nblk, frame0, pin_y))
        sz_bub = len(bubble_blobs(0, bub)[0]) if bub else 0
        sz_talk = len(e1_talk_blob(0)) if talk else 0
        need = sz_timer + sz_letters + sz_bub + sz_talk
        # placement: everything appended past the payload when the 48K
        # buffer has room; else the letters (the big block) overlay
        # FREE_REGION and the small controllers still append — the Ship
        # template 1C1 leaves exactly 180 B past its payload
        talk_in_region = False
        if n + need <= TMPL_BUF:
            tail_base, region = n, None
            order = ["timer", "letters"] + (["bub"] if bub else []) + (["talk"] if talk else [])
            where = "appended"
        else:
            rbase, rend, victims = FREE_REGION[src]
            talk_in_region = bool(talk) and sz_letters + sz_bub + sz_talk <= rend - rbase
            assert sz_letters + sz_bub <= rend - rbase, (src, sz_letters + sz_bub, rend - rbase)
            sz_tail = sz_timer + (sz_talk if talk and not talk_in_region else 0)
            assert n + sz_tail <= TMPL_BUF, (src, n + sz_tail, TMPL_BUF)
            tail_base, region = n, (rbase, rend, victims)
            order = ["timer"] + (["talk"] if talk and not talk_in_region else [])
            where = (f"letters{' + talk spots' if talk_in_region else ''} overlaid on "
                     f"{'+'.join(victims)} {rbase:04X}-{rend:04X}, controllers appended")
        # build the tail (appended) and the region blob at their addresses
        addr = {}
        tail = b""
        for name in order:
            a = tail_base + len(tail); addr[name] = a
            if name == "timer":   tail += d8_timer_blob(a, t_slot)
            elif name == "letters": tail += e0_letters_blob(a, nblk, frame0, pin_y)
            elif name == "bub":   tail += bubble_blobs(a, bub)[0]
            elif name == "talk":  tail += e1_talk_blob(a)
        region_blob = b""
        if region:
            rbase, rend, victims = region
            a = rbase; addr["letters"] = a
            region_blob = e0_letters_blob(a, nblk, frame0, pin_y)
            if talk_in_region:
                addr["talk"] = rbase + len(region_blob)
                region_blob += e1_talk_blob(addr["talk"])
        new_n = n + len(tail)
        recs = [("D8", "record D8 sprite=FFFF flags=00 code==%04X rest=%s" % (addr["timer"], D8_REST)),
                ("E0", "record E0 sprite=FFFE flags=01 code==%04X rest=%s" % (addr["letters"], E0_REST))]
        if bub:
            _, baddr = bubble_blobs(addr["bub"], bub)
            bsprite = 0x262 + (slot - 53)          # the scene's bubble sprite chunk (smd2pc)
            for cls in (0x25, 0x24, 0x23):
                m2 = re.search(r"^record %02X sprite=\S+ flags=(\S+) code=\S+ rest=(\S+)$" % cls, txt, re.M)
                assert m2, f"template {src:X}: record {cls:02X} not found"
                recs.append(("%02X" % cls, "record %02X sprite=%04X flags=%s code==%04X rest=%s" % (cls, bsprite, m2.group(1), baddr[cls], m2.group(2))))
            m2 = re.search(r"^record 4A sprite=(\S+) flags=(\S+) code=\S+ rest=(\S+)$", txt, re.M)
            assert m2, f"template {src:X}: record 4A not found"
            recs.append(("4A", "record 4A sprite=%s flags=%s code==%04X rest=%s" % (m2.group(1), m2.group(2), baddr[0x4A], m2.group(3))))
        if talk:
            recs.append(("E1", "record E1 sprite=FFFF flags=01 code==%04X rest=%s" % (addr["talk"], E1_REST)))
        for cls, new in recs:
            txt, k = re.subn(r"^record %s .*$" % cls, new, txt, count=1, flags=re.M)
            if k != 1:
                raise SystemExit(f"template {src:X}: record {cls} not found")
        txt = txt.replace(m.group(0), "chunk %04X size %d" % (dst, new_n), 1)
        if region:
            rbase, rend, victims = region
            lines = txt.split("\n")
            i0 = lines.index("S_%04X:" % rbase)
            i1 = lines.index("S_%04X:" % rend)
            inside = {ln[:-1] for ln in lines[i0:i1] if re.match(r"^[SA]_[0-9A-F]{4}:$", ln)}
            # outside the region only the region's own aliases mention its
            # labels (verified when the region was chosen) — drop those
            keep = [ln for k, ln in enumerate(lines)
                    if not (i0 <= k < i1) and not (re.match(r"^[SA]_[0-9A-F]{4} = ", ln) and ln.split(" = ")[0] in inside)]
            j = keep.index("S_%04X:" % rend)
            pad = bytes(rend - rbase - len(region_blob))
            keep.insert(j, "blob @%04X %s" % (rbase, (region_blob + pad).hex()))
            txt = "\n".join(keep)
            # the overlaid classes must never resolve into our code
            for victim in victims:
                txt, k = re.subn(r"^record %s sprite=(\S+) flags=(\S+) code=\S+ rest=(\S+)$" % victim,
                                 r"record %s sprite=\1 flags=\2 code=S_13DB rest=\3" % victim, txt, count=1, flags=re.M)
                if k != 1:
                    raise SystemExit(f"template {src:X}: record {victim} not found")
            stray = [ln for ln in keep if not ln.startswith("record") and any(
                     re.search(r"\b%s\b" % lab, ln) for lab in inside)]
            if stray:
                raise SystemExit(f"template {src:X}: {len(stray)} lines still reference the overlaid region: {stray[:3]}")
        if tail:
            txt = txt.rstrip("\n") + "\nblob @%04X %s\n" % (tail_base, tail.hex())
        d = os.path.join(scratch, "level_scripts")
        os.makedirs(d, exist_ok=True)
        for name, body in ((f"{dst:X}.lvsf", txt),
                           (f"{dst:04X}.size.json", json.dumps({"payload_len": new_n}))):
            p = os.path.join(d, name)
            with open(p + ".tmp", "w") as f:
                f.write(body)
            os.replace(p + ".tmp", p)
        extras[f"{dst:04X}"] = {"role": "level_script"}
        print(f"  scene template {dst:04X} = {src:04X} ({n}B): D8 timer @{addr['timer']:04X} "
              f"({t_slot} ticks) + {nblk} letters @{addr['letters']:04X}"
              + (f" + bubbles 4A/25/24/23 (pool units {bub}) @{addr['bub']:04X}" if bub else "")
              + (f" + E1 talk spots @{addr['talk']:04X}" if talk else "")
              + f", {len(tail)}B appended" + (f" + {len(region_blob)}B {where}" if region else ""))
    with open(ex_path + ".tmp", "w") as f:
        json.dump(extras, f, indent=1)
    os.replace(ex_path + ".tmp", ex_path)



# ---------------------------------------------------------------------------
# UX stage 1, dialogue. The scene lines are the Genesis-only strings 389-406
# of the SMD text table (BE pointers @ROM 0x900C, base = the table, records
# in the PC shape [w][h][chars 0x0D/0x00]). op 0x41 resolves a text index
# through seg001[idx*2] and reads the record at that seg001 offset (16-bit,
# es = seg001 = image 0x9480): the pointer table is the fixed 390-entry
# block at seg001+0 and the string zone ends at 0x4062 with 446 B of
# padding — too small for the 776 B of scene records. The 64 KB seg001
# window, however, runs on over seg002/seg003 up to image 0x19480, and the
# image is all zeros from 0x10BC0 (the end of seg003's code) to 0x19F00
# (seg004): nothing in V2_ONLY reads it (the m2c segments never execute
# there, v2 reads the image only at the DS/seg001 tables). The scene text
# BANK lives there: 18 pointer words at image 0x10BC0 = seg001 0x7740, i.e.
# text indices 0x3BA0 + k, records right behind them. Dual builds do not
# load exe_static.bin at all (no LVX scenes) — nothing else changes.
SCENE_TEXT_BANK = 0x10BC0                 # image offset of the pointer words
SCENE_TEXT_IDX0 = (SCENE_TEXT_BANK - 0x9480) // 2   # = 0x3BA0
SMD_TEXT_TABLE = 0x900C
SMD_TEXT_COUNT = 443
SMD_SCENE_LINES = list(range(389, 407))   # the 18 interlude lines


def smd_text_record(rom, i):
    """Record bytes [w][h][chars]\\0 of SMD text-table entry i, verbatim."""
    base = SMD_TEXT_TABLE
    ptr = (rom[base + 2 * i] << 8) | rom[base + 2 * i + 1]
    o = base + ptr
    j = o + 2
    while rom[j] != 0:
        j += 1
    return bytes(rom[o:j + 1])


def write_scene_texts(scratch):
    """Bake the 18 scene lines into the scratch exe_static image at
    SCENE_TEXT_BANK (pointer words + records) — before build_lvx appends
    the trailer. Returns {line: text index}."""
    src = os.path.join(scratch, "exe_static.bin")
    if not os.path.exists(src):
        src = os.path.join(LR.ROOT, "exe_static.bin")
    img = bytearray(open(src, "rb").read())
    rom = SM.SmdRom().rom
    recs = [smd_text_record(rom, i) for i in SMD_SCENE_LINES]
    ptrs = bytearray()
    body = bytearray()
    off = SCENE_TEXT_BANK - 0x9480 + 2 * len(recs)      # seg001 offset of record 0
    for r in recs:
        ptrs += struct.pack("<H", off + len(body))
        body += r
    bank = bytes(ptrs + body)
    end = SCENE_TEXT_BANK + len(bank)
    assert end <= 0x19F00, end
    tail = img[SCENE_TEXT_BANK:SCENE_TEXT_BANK + 0x1000]
    assert all(b == 0 for b in tail) or img[SCENE_TEXT_BANK:SCENE_TEXT_BANK + 4] == bank[:4], \
        "scene text bank region is not the pristine zero block"
    img[SCENE_TEXT_BANK:SCENE_TEXT_BANK + 0x1000] = bytes(0x1000)
    img[SCENE_TEXT_BANK:end] = bank
    out = os.path.join(scratch, "exe_static.bin")
    with open(out + ".tmp", "wb") as f:
        f.write(img)
    os.replace(out + ".tmp", out)
    idx = {ln: SCENE_TEXT_IDX0 + k for k, ln in enumerate(SMD_SCENE_LINES)}
    print(f"  scene texts: {len(recs)} SMD lines ({len(body)}B) -> image {SCENE_TEXT_BANK:05X}, "
          f"indices {SCENE_TEXT_IDX0:04X}..{SCENE_TEXT_IDX0 + len(recs) - 1:04X}")
    return idx


# The scene lines = the SMD class E1 "talk spot" ported as BYTECODE (docs2/
# GENESIS_ROM_INTERNALS.md §6, tools/assets/smd_lvs.py E1): the same VM
# dialect, the same field LUT (16 SPAWN_POOL = the row's pool, 1C TIMER,
# 1E/20 X/Y, 30/32 message/sender, 3C PARTNER), the same DS scratch
# (0x206/0x208/0x20A, 0x42). The object waits for message 0x14 (the
# viking's S-key use branch — PC 1C6 sends the identical bytes), sets the
# text colour by the speaker's slot (op 46 = cmd type 6), places the box
# at the viking (dx/dy from its own spot; the tail code and the 52-px lift
# come from the marker flags in pool bits 13-15: `97/A9 1e2e`, `AE 1c2e`,
# `AE 1a2e` = bit tests of field 2E = the saved pool word), shows line
# `pool & 0x0FFF`, holds 80 ticks, closes (op 42) and bumps the line
# index: the next S on the same spot says the next line. The rows carry
# the SMD pool words VERBATIM (flags + SMD line number); the port edits,
# all documented: (1) after the mask, `51 <delta> 59 16` adds the PC
# text-bank offset (SCENE_TEXT_IDX0 - 389: our copies of lines 389..406 sit
# at 0x3BA0+k); (2) the sfx operand `02 080b` (an SMD sound id) becomes the
# PC dialogue blip `02 2f50` used by every PC text box (1C1..1C6); (3) the
# three colour words of SUB_COLOR become the PC RGB555 of the Genesis fills.
E1_SMD_MAIN = (0x25ADE, 0x25B7C)     # class code (after the 03 despawn prolog)
E1_SMD_SUB = (0x27B0D, 0x27B27)      # SUB_COLOR: op 46 by partner slot, 06
E1_EDITS = {0x25B4F: bytes.fromhex("2f50"),      # 02 080b -> 02 2f50 (PC dialogue sfx)
            # SUB_COLOR: `partner==2 -> jump 46 0280; ==4 -> jump 46 0080;
            # else 46 0180` (op 73 jumps when EQUAL) = the Genesis command-6
            # colour by speaker: 0x8000|1 Erik (slot 0), |2 Baleog (slot 2),
            # |0 Olaf (slot 4) — a CRAM pick on the SMD. The PC command 6
            # takes an RGB555 word (r bits 0-4, g 5-9, b 10-14, each doubled
            # into the VGA DAC colour 3 = the box fill). The fills measured on
            # the real Genesis (Mednafen, genesis_stand): Erik (206,0,0),
            # Baleog (0,170,0), Olaf (170,170,0) -> 5-bit 25 / 21 / 21+21.
            0x27B1C: bytes.fromhex("1900"),           # 46 0180 Erik   0x0019 = red
            0x27B20: bytes.fromhex("a002"),           # 46 0280 Baleog 0x02A0 = green
            0x27B24: bytes.fromhex("b502")}           # 46 0080 Olaf   0x02B5 = yellow
E1_MASK_PC = 0x25AEA                 # `5F 16` (pool &= 0x0FFF): the delta add goes after it


def e1_talk_blob(base):
    """Class E1 for the scene templates (smd_lvs.port_blob: prolog, code,
    colour sub, relocated jumps/calls)."""
    import smd_lvs as SL
    delta = SCENE_TEXT_IDX0 - SMD_SCENE_LINES[0]
    ins = {E1_MASK_PC: bytes((0x51, delta & 0xFF, delta >> 8, 0x59, 0x16))}
    blob, _ = SL.port_blob([E1_SMD_MAIN, E1_SMD_SUB], 3, base, edits=E1_EDITS, inserts=ins)
    return blob


# The banner letters = the SMD class E0 ported as BYTECODE (docs2/GENESIS_ROM
# _INTERNALS.md; tools/assets/smd_lvs.py E0, bank 4 @0x2D7A2): each letter
# takes its block index from the shared counter [0x158] (spawn order),
# arms the block's anim, waits `pool` ticks (the row's stagger), then moves
# by WORLD: Prehistoria/Wacky fall under gravity (flag 0x8000) until y >= 10
# (Wacky bounces while |dy| > 8), Egypt slides left at 2 px/tick bobbing
# +-3 px every 8 ticks (15 phases), Factory slides left 3 px/tick for 90
# ticks (the first letter bumps back), Starship flies in from the right at
# 16 px/tick for 18 ticks — every path lands exactly on the BAC banner
# positions (x0 80/50/32, y0 10..12). Port edits, all documented: the level
# ids (SMD 0x34..0x38 -> our slots 0x35..0x39) and the level word address
# ([0x1A3E] -> PC DS_LEVEL 0x25AD) — six `51 xx00 / 74 3e1a` pairs; the
# anim addresses -> our static block anims (below); the three sfx ops (SMD
# ids 0x51 land/bump, 0x28 bounce) are dropped — no PC ids are known for
# them (the letters were silent before too). [0x158]/[0x328] are untouched
# scratch on the PC engine (no v2 reader/writer); the D8 timer zeroes the
# counter at scene start (d8_timer_blob) — the SMD relies on its per-level
# RAM clear.
E0_SMD_MAIN = (0x2D7A2, 0x2D92E)     # class code + its WAIT sub (anims follow at 0x2D92E)
E0_SMD_ANIM_BASE = 0x2D92E           # the init anim (frame 0), then 8 x 5-byte block anims
E0_SMD_ANIMS = [0x2D937 + 5 * k for k in range(8)]
E0_LEVEL_SITES = {0x2D829: 0x34, 0x2D831: 0x35, 0x2D839: 0x36, 0x2D841: 0x37, 0x2D849: 0x38, 0x2D8EC: 0x34}
E0_LEVEL_ADDR_SITES = (0x2D82C, 0x2D834, 0x2D83C, 0x2D844, 0x2D84C, 0x2D8EF)   # 74 3e1a ...
E0_SFX_DROPS = (0x2D8CF, 0x2D908, 0x2D911)
SLOT_OF_SMD_LEVEL = {0x34: 53, 0x35: 54, 0x36: 55, 0x37: 56, 0x38: 57}


E0_LAND_Y_SITE = 0x2D8E7             # `81 0a00`: the falling letters stop at y >= 10 (SCREEN y)


def e0_letters_blob(base, nblk=8, frame0=1, pin_y=0):
    """(blob) = prolog + the E0 code ported to `base` + the block anims.
    `frame0` = the pool frame of block 0 (65 on route B: the banner bank
    rides behind the world's pool bank 0x12F — smd2pc). `pin_y` = the
    scene's camera pin (genesis_scene.layout): the E0 rows and the landing
    threshold are SCREEN coordinates on the SMD, our map = screen + pin."""
    import smd_lvs as SL
    edits = {}
    assert SL.R[E0_LAND_Y_SITE:E0_LAND_Y_SITE + 3] == bytes((0x81, 0x0A, 0x00))
    edits[E0_LAND_Y_SITE + 1] = struct.pack("<H", 10 + pin_y)
    for pc, smd_lvl in E0_LEVEL_SITES.items():
        assert SL.R[pc] == 0x51 and SL.R[pc + 1] == smd_lvl, hex(pc)
        edits[pc + 1] = bytes((SLOT_OF_SMD_LEVEL[smd_lvl], 0))
    for pc in E0_LEVEL_ADDR_SITES:
        assert SL.R[pc] == 0x74 and SL.R[pc + 1:pc + 3] == bytes.fromhex("3e1a"), hex(pc)
        edits[pc + 1] = struct.pack("<H", 0x25AD)          # DS_LEVEL
    for pc in E0_SFX_DROPS:
        assert SL.R[pc] == 0x02, hex(pc)
    def anims(a0):
        """static block anims: 15 01 (type-2, 32 strips) / 01 frame (block
        K = frame0 + 16K) / 08 0 (X = WORLD_X) / 0A 0 (Y = WORLD_Y) /
        0F 7F + jmp self (the SMD idiom: hold the frame)."""
        out = bytearray(); addr = []
        for k in range(nblk):
            addr.append(a0 + len(out))
            assert frame0 + k * 16 < 256, (frame0, k)
            a = bytes((0x15, 0x01, 0x01, (frame0 + k * 16) & 0xFF, 0x08, 0, 0, 0x0A, 0, 0))
            loop_at = a0 + len(out) + len(a)
            out += a + bytes((0x0F, 0x7F, 0x03)) + struct.pack("<H", loop_at)
        return bytes(out), addr
    # pass 1: the code length with a dummy anim map
    dummy = {E0_SMD_ANIM_BASE: 0, **{a: 0 for a in E0_SMD_ANIMS}}
    code, _ = SL.port_blob([E0_SMD_MAIN], 4, base, edits=edits, drops=E0_SFX_DROPS, op19_map=dummy)
    a_bytes, a_addr = anims(base + len(code))
    amap = {E0_SMD_ANIM_BASE: a_addr[0], **{a: a_addr[k] for k, a in enumerate(E0_SMD_ANIMS)}}
    code, _ = SL.port_blob([E0_SMD_MAIN], 4, base, edits=edits, drops=E0_SFX_DROPS, op19_map=amap)
    return code + a_bytes

def restore_1c6(scratch):
    """The scene script 1C6 goes back to the canonical text (earlier runs
    patched its D8 duration and appended the letter blob for the scenes,
    which now live on their own templates); the world ladder patch is
    reapplied by patch_1c6_ladder right after."""
    src = os.path.join(AC.RAW, "lvs", "1C6.lvsf")
    dst = os.path.join(scratch, "level_scripts", "1C6.lvsf")
    txt = open(src).read()
    with open(dst + ".tmp", "w") as f:
        f.write(txt)
    os.replace(dst + ".tmp", dst)
    sj = os.path.join(scratch, "level_scripts", "01C6.size.json")
    with open(sj + ".tmp", "w") as f:
        json.dump({"payload_len": 40787}, f)
    os.replace(sj + ".tmp", sj)


def patch_1c6_d8_timer(scratch):
    """S_8A90 (the D8 timed-scene controller): default duration 0x3C ticks
    is too short for the choreography — retarget the DEFAULT branch constant
    to 300 ticks. Text edit of the .lvsf (same literal width); idempotent.
    The logo screens (39/40) spawn D8 through the HEAD path whose ANIM_SUB
    is nonzero, so they ride the field[16] branch, not this default —
    verified by the logo pageflip timing staying identical after the patch."""
    p = os.path.join(scratch, "level_scripts", "1C6.lvsf")
    txt = open(p).read()
    old = "o 78 16 S_8AAB\no 51 3c00\no 56 1c"
    new = "o 78 16 S_8AAB\no 51 2c01\no 56 1c"
    if new in txt:
        return                       # already patched (idempotent rerun)
    if old not in txt:
        raise SystemExit("S_8A90 default-timer sequence not found")
    assert txt.count(old) == 1
    txt = txt.replace(old, new)
    tmp = p + ".tmp"
    with open(tmp, "w") as f:
        f.write(txt)
    os.replace(tmp, p)
    print("  1C6 S_8A90: D8 default timer 60 -> 300 ticks")


def do_integrate(scratch, music=None):
    """Full console-content integration: 5 SNES levels + 6 SMD scenes."""
    lvx = []
    for e in PLAN:
        b = e["base"]
        cids = {"hdr": b, "map": b + 1, "tiles": b + 2,
                "gtld": b + 4, "pal": b + 5}   # masks = tiles+1 = b+3
        print(f"slot {e['slot']} ({e['pw'].decode()}):")
        info = SP.convert_level(e["snes"], e["donor"], scratch,
                                new_cids=cids, next_level=e["next"],
                                music=music)
        print(f"  music track {info['music']} "
              f"({MUSIC_TRACKS.get(info['music'], ('?', '?'))[1]})")
        lvx.append({"slot": e["slot"], "hdr": b, "pw": e["pw"]})
    # SMD scenes (D8 timed cutscenes on the 1C6 scene script)
    import smd2pc as SMD
    frames = {}
    for e in PLAN_SMD:
        b = e["base"]
        print(f"slot {e['slot']} ({e['pw'].decode()}, SMD scene):")
        info = SMD.convert_scene(e["smd"], e["donor"], scratch,
                                 {"hdr": b, "map": b + 1, "tiles": b + 2,
                                  "gtld": b + 4, "pal": b + 5,
                                  "banner": 0x258 + (e["slot"] - 53),
                                  "bubbles": 0x262 + (e["slot"] - 53)},
                                 next_level=e["next"], scene_mode=True,
                                 de_bg=e.get("de_bg"),
                                 gen_bg=e.get("gen_bg"))
        if info.get("decor_frames"):
            frames[e["slot"]] = info["decor_frames"]
        # UX stage 0/1: scenes play full-screen with the camera parked at
        # the map's (pin_x, pin_y) = EXT_L room columns + the Genesis camera
        # mod 16 (genesis_scene.layout); bits 4-11 / 12-15 of the flags
        pin_x = pin_y = 0
        if e.get("gen_bg"):
            import genesis_scene as GS
            pin_x, pin_y = GS.layout(e["gen_bg"]["world"], e["gen_bg"])["pin"]
        flags = LVX_FULLSCREEN | LVX_CAMLOCK | (pin_x << 4) | (pin_y << 12)
        if e.get("gen_bg"):
            flags |= LVX_NOGATE
        trio = None
        if info.get("trio"):
            # UX stage 1: per-viking placement = the mode-2 table rows of
            # sub_11446/sub_11569 (Erik, Baleog, Olaf = code_seg 1/0/2)
            trio = b"".join(struct.pack("<hHH", x, y, a)
                            for (x, y, a) in info["trio"])
            flags |= LVX_TRIO
        lvx.append({"slot": e["slot"], "hdr": b, "pw": e["pw"],
                    "demo": DEMO_CID.get(e["slot"], 0), "flags": flags,
                    "trio": trio})
    write_demo_chunks(scratch)
    # canonical predecessors point into the insert chains
    print("progression patch:")
    for e in PLAN + PLAN_SMD:
        if e["prev_hdr"]:
            patch_next(scratch, e["prev_hdr"], e["slot"])
    # UX stage 1 route B: the scenes run on their own world-template copies
    # (build_scene_templates); 1C6 keeps only the world-ladder retarget
    restore_1c6(scratch)
    patch_1c6_ladder(scratch)
    text_idx = write_scene_texts(scratch)   # lines 389..406 -> SCENE_TEXT_IDX0+k (e1_talk_blob's delta)
    build_scene_templates(scratch, frames=frames, text_idx=text_idx)
    # SNDS (slot 49) lives in a NEW header — its next=50 already baked;
    # its predecessor JMNN (0053) got next=49 above.
    build_lvx(scratch, lvx)
    # UX stage 2: the SNES parallax layer of every level (the five exclusives'
    # heads exist from this point on)
    import parallax_snes
    parallax_snes.integrate(scratch)
    # UX stage 7: the SNES-balance variants (after the parallax step: their
    # heads copy the canonical heads' pair refs) — the trailer is rebuilt
    balance_integrate(scratch, lvx)
    build_lvx(scratch, lvx)
    # UX stage 6: the language banks (BAC translations, Press Start 2P glyph pages)
    import build_locale
    build_locale.integrate(scratch)
    return [e["slot"] for e in PLAN + PLAN_SMD]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scratch", default="/tmp/lv_edit_scratch")
    ap.add_argument("--pack", action="store_true")
    ap.add_argument("--music", type=int, default=None,
                    help="force this music track on the five levels "
                         "(default: the SNES head's own = the world theme). "
                         "Track 8 is the unused full theme, 9 the short one.")
    ap.add_argument("--list-music", action="store_true")
    args = ap.parse_args()

    if args.list_music:
        print("track chunk  notes  secs  used  description")
        for t, (c, d, n, s, u) in MUSIC_TRACKS.items():
            print(f"  {t:2d}  {c}  {n:5d} {s:6.1f}  {u:4d}  {d}")
        return

    do_integrate(args.scratch, music=args.music)
    if args.pack:
        AC.ASSETS = args.scratch
        AC.pack()


if __name__ == "__main__":
    main()
