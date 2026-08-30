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


def build_lvx(scratch, entries):
    """exe_static.bin + LVX3 trailer: [magic][u16 n][14B records:
    level u16, hdr_cid u16, tmpl_cid u16, pw 4B, demo_cid u16, flags u16].
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
    for magic in (b"LVX3", b"LVX2", b"LVX1"):
        m = img.rfind(magic)
        if m >= 0 and m >= len(img) - 4 - 2 - 14 * 64:
            img = img[:m]
    tr = b"LVX3" + struct.pack("<H", len(entries))
    for e in entries:
        tr += struct.pack("<HHH", e["slot"], e["hdr"], TMPL_CHUNK[e["slot"]])
        tr += e["pw"]
        tr += struct.pack("<HH", e.get("demo", 0), e.get("flags", 0))
    out = os.path.join(scratch, "exe_static.bin")
    tmp = out + ".tmp"
    with open(tmp, "wb") as f:
        f.write(img + tr)
    os.replace(tmp, out)
    print(f"  exe_static: {len(img)}B + LVX3 trailer {len(tr)}B -> {out}")


# ---------------------------------------------------------------------------
# #113 scene choreography: the vikings on the SNES/SMD interludes MOVE on a
# scripted demo (the video: Olaf rides his shield past, Erik answers and
# shows his sprint). PC plays it through the canonical attract machinery:
# ac=0x8000 -> sub_12d72 pops RLE (keys u16, count u16) pairs from the chunk
# loaded at ds:2193 and ORs them into the input word — so the choreography
# is DATA: one small chunk per scene. Key bits: RIGHT 0x100, LEFT 0x200,
# UP(jump) 0x800, DOWN 0x400, TAB(switch) 0x2000. No ACTION 0x8000 bit —
# that is the D8 skip button. The tail (0, 0x7FFF) parks the input silent
# until the D8 timer ends the scene.
DEMO_CID = {53: 0x253, 54: 0x254, 55: 0x255, 56: 0x256, 57: 0x257}

R, L, U, TAB = 0x100, 0x200, 0x800, 0x2000

def demo_script(step=8):
    """The v4 choreography (~279 ticks), active viking only (TAB does
    not cycle vikings inside the interludes — seen live; the video also
    moves one viking at a time). Erik ACCELERATES: 6 ticks walk
    ~2.7px/t but 14 ticks hit run speed ~5.8px/t and threw him off the
    ledge (seen live twice) — every stride stays <= `step` ticks and
    net-zero, hops between strides. `step` scales the walk amplitude to
    the scene's ledge width (the DE level-crop ledges are narrow: LLM0's
    start ledge is 6 quads)."""
    s2 = max(step - 1, 2)
    s = [(0, 44),                                  # drop in + settle
         (R, step), (0, 14), (L, step), (0, 16),   # stroll right and back
         (U, 3), (0, 30),                          # hop
         (R, s2), (0, 12), (L, s2), (0, 16),       # short steps
         (U, 3), (0, 30),
         (L, s2), (0, 12), (R, s2), (0, 16),       # the other way
         (U, 3), (0, 36),
         (0, 0x7FFF)]                              # silence till the timer
    return s

def write_demo_chunks(scratch, steps=None):
    import json as _json
    ex_path = os.path.join(scratch, "extras.json")
    extras = _json.load(open(ex_path)) if os.path.exists(ex_path) else {}
    for slot, cid in DEMO_CID.items():
        step = (steps or {}).get(slot, 8)
        blob = b"".join(struct.pack("<HH", k, n)
                        for k, n in demo_script(step))
        d = os.path.join(scratch, "unreferenced")
        os.makedirs(d, exist_ok=True)
        p = os.path.join(d, f"{cid:04X}.bin")
        tmp = p + ".tmp"
        with open(tmp, "wb") as f:
            f.write(blob)
        os.replace(tmp, p)
        extras[f"{cid:04X}"] = {"role": "unreferenced"}
    tmp = ex_path + ".tmp"
    with open(tmp, "w") as f:
        _json.dump(extras, f, indent=1)
    os.replace(tmp, ex_path)
    print(f"  demo chunks: {len(DEMO_CID)} x {len(demo_script())*4}B "
          f"(ids {min(DEMO_CID.values()):04X}..{max(DEMO_CID.values()):04X})")



# #113 letter-fall: the SMD/SNES interludes DROP the banner letters from the
# sky (video ref; the SMD E0 rows carry per-letter phase in `pool`). Ours is
# pure data appended PAST the canonical 1C6 payload (0x9F53; the template
# buffer holds 48K, payload 40787 — room to spare, absolute addresses mean
# nothing shifts):
#   - class D9 is an ORPHAN on the PC (no canonical stripe spawns it — full
#     scan) — its record is retargeted: spr FFFE (pool sprite, base 0),
#     32x32 body (the D7 rest bytes), code -> the dispatcher below;
#   - dispatcher: acc=field[16] (OBJ_ANIM_SUB = the spawn ROW INDEX) -> the
#     canonical `51 k / 73 16 target` ladder (same grammar the S_8C61 row
#     dispatcher uses) -> op 19 arms the letter's anim; rows 1..8 are the
#     letters (row 0 stays the D8 timer: its index feeds its OWN duration);
#   - anim per letter K: 0F phase-delay (the SMD pool cascade), 15 01
#     (type-2, 32 strips — cs:32D7 table), 01 K*16 (frame = block K: the
#     banner bank is the FIRST bank of the scene => sprite base 0, and a
#     32x32 type-2 block is exactly 16 x 72B units), 08 0 (X=WORLD_X),
#     then a ladder of 0A y-absolutes from -(drop) up to 0 (the first 0A
#     also RAISES the sprite — canonical A_9F25 pattern), idle loop.
LETTER_BASE = 0x9F53

def letterfall_blob(nblk, base=LETTER_BASE):
    """(blob_bytes, dispatcher_addr) — dispatcher first, then 8 anims.
    `base` = the absolute address the blob lands at (the D9 record's code
    points at base-3: spawn enters at record pc + 3)."""
    LETTER_BASE = base
    # pass 1: measure the dispatcher: 3 (prolog jmp is NOT used: the 13DB
    # prolog calls the shared setup; D9 objects need none of it — start
    # straight at the ladder) + per row: 3 (51 k) + 4 (73 16 addr) ... + 1
    # (10 destroy default) ; branch targets: 19 addr (3) + idle jmp (3).
    disp = bytearray()
    branches = []
    for k in range(1, nblk + 1):
        disp += bytes((0x51, k & 0xFF, 0x00))
        branches.append(len(disp) + 2)           # patch spot for 73-target
        disp += bytes((0x73, 0x16, 0x00, 0x00))
    fall_through = len(disp) + 1
    disp += bytes((0x03, 0x00, 0x00))            # foreign index: -> letter 1
                                                 # (diagnosed live: never
                                                 # destroy — see below)
    # branch bodies: 19 <anim_k> ; 03 <idle>
    body_off = []
    for k in range(nblk):
        body_off.append(len(disp))
        disp += bytes((0x19, 0x00, 0x00, 0x03, 0x00, 0x00))
    idle_off = len(disp)
    disp += bytes((0x2F, 0x00, 0x01, 0x03, 0x00, 0x00))   # show/idle loop
    # anims start after the dispatcher
    anims = bytearray()
    anim_off = []
    for k in range(nblk):
        anim_off.append(len(disp) + len(anims))
        a = bytearray()
        a += bytes((0x0F, 4 + k * 7))            # cascade phase (SMD pools)
        a += bytes((0x15, 0x01))                 # type-2, 32 strips
        # frame 1+K*16: the render fetches mask@off-1/data@off (1-based,
        # code-read), the anim 01 gives off = frm*72 from pool base 0 —
        # the bank ships with a 71-byte zero prefix so off=72 lands the
        # masks exactly on the encoded stream (block K at 71+K*1152).
        # ('15 05' was NOT the fix: its reset also rewrites SPRITE_SEG
        # from the sub's SRC_SEG — killed the draw, seen live.)
        a += bytes((0x01, (1 + k * 16) & 0xFF))  # frame = block K
        a += bytes((0x08, 0x00, 0x00))           # X = WORLD_X
        y = -(88 + k * 4)
        while y < 0:
            a += bytes((0x0A,)) + int(y).to_bytes(2, "little", signed=True)
            a += bytes((0x0F, 0x02))
            y += 8
        a += bytes((0x0A, 0x00, 0x00))           # settle at WORLD_Y
        loop_at = LETTER_BASE + len(disp) + len(anims) + len(a)
        a += bytes((0x0F, 0x28))
        a += bytes((0x03,)) + loop_at.to_bytes(2, "little")
        anims += a
    # pass 2: patch dispatcher targets (absolute addresses)
    ft = LETTER_BASE + body_off[0]
    disp[fall_through] = ft & 0xFF
    disp[fall_through + 1] = ft >> 8
    for k in range(nblk):
        tgt = LETTER_BASE + body_off[k]
        disp[branches[k]] = tgt & 0xFF
        disp[branches[k] + 1] = tgt >> 8
        ao = LETTER_BASE + anim_off[k]
        disp[body_off[k] + 1] = ao & 0xFF
        disp[body_off[k] + 2] = ao >> 8
        io = LETTER_BASE + idle_off
        disp[body_off[k] + 4] = io & 0xFF
        disp[body_off[k] + 5] = io >> 8
    io = LETTER_BASE + idle_off
    disp[idle_off + 4] = io & 0xFF
    disp[idle_off + 5] = io >> 8
    return bytes(disp) + bytes(anims)


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
    b = bytearray(D8_CANON)
    assert len(b) == 0x41, len(b)
    for off, canon, rel in D8_RELOC:
        assert b[off] | (b[off + 1] << 8) == canon, (off, b[off:off + 2].hex())
        tgt = base + rel
        b[off], b[off + 1] = tgt & 0xFF, tgt >> 8
    assert b[19:22] == bytes((0x51, 0x3C, 0x00)), b[19:22].hex()
    b[20], b[21] = ticks & 0xFF, ticks >> 8
    return bytes(b)


D8_REST = "04000000101000000500000000000000"   # 1C6's D8 record body
D9_REST = "04000000202000000500000000000010"   # the D7-shaped 32x32 body
SCENE_TICKS = 300
TMPL_BUF = 0xC00 * 16    # the template lives in the animdata segment: 0xC00 paragraphs
# Where the blocks go when the template has no room past its payload: the
# Ship template 1C1 is 48972 B (180 B short of the buffer), so its scene
# copy overlays the blocks onto a class region the scene never runs —
# class D2 (S_3311..S_3694, 899 B): no Ship level spawns D2, no label in
# the region is referenced from outside it (the .lvsf reference graph: 0
# incoming; only its own `X = X+0` aliases), and the D2 record is pointed
# at the 13DB stub like 1C6 does. The region keeps its exact length (the
# blob is zero-padded), so nothing else in the template shifts.
FREE_REGION = {0x1C1: (0x3311, 0x3694, "D2")}


def build_scene_templates(scratch, nblk=8, ticks=SCENE_TICKS):
    """One template per scene slot: the canonical world template text
    (assets_raw/lvs/<src>.lvsf) under a new chunk id, its orphan D8/D9
    records retargeted at the two blocks — appended past the payload when
    the 48K template buffer has room, else overlaid on FREE_REGION.
    Registered in extras.json as role level_script."""
    ex_path = os.path.join(scratch, "extras.json")
    extras = json.load(open(ex_path)) if os.path.exists(ex_path) else {}
    for slot, (src, dst) in sorted(SCENE_TMPL.items()):
        txt = open(os.path.join(AC.RAW, "lvs", f"{src:X}.lvsf")).read()
        m = re.search(r"^chunk %04X size (\d+)$" % src, txt, re.M)
        if not m:
            raise SystemExit(f"template {src:X}: size header not found")
        n = int(m.group(1))
        timer = d8_timer_blob(0, ticks)              # sized only; rebuilt below
        need = len(timer) + len(letterfall_blob(nblk, base=0))
        if n + need <= TMPL_BUF:
            base, new_n, where = n, n + need, "appended"
        else:
            base, end, victim = FREE_REGION[src]
            assert need <= end - base, (src, need, end - base)
            new_n, where = n, f"overlaid on {victim} {base:04X}-{end:04X}"
        timer = d8_timer_blob(base, ticks)
        letters = letterfall_blob(nblk, base=base + len(timer))
        blob = timer + letters
        for cls, new in (("D8", "record D8 sprite=FFFF flags=00 code==%04X rest=%s" % (base, D8_REST)),
                         ("D9", "record D9 sprite=FFFE flags=01 code==%04X rest=%s" % (base + len(timer) - 3, D9_REST))):
            txt, k = re.subn(r"^record %s .*$" % cls, new, txt, count=1, flags=re.M)
            if k != 1:
                raise SystemExit(f"template {src:X}: record {cls} not found")
        txt = txt.replace(m.group(0), "chunk %04X size %d" % (dst, new_n), 1)
        if where == "appended":
            txt = txt.rstrip("\n") + "\nblob @%04X %s\n" % (base, blob.hex())
        else:
            lines = txt.split("\n")
            i0 = lines.index("S_%04X:" % base)
            i1 = lines.index("S_%04X:" % end)
            inside = {ln[:-1] for ln in lines[i0:i1] if re.match(r"^[SA]_[0-9A-F]{4}:$", ln)}
            for k in range(i0, i1):
                for lab in re.findall(r"\b[SA]_[0-9A-F]{4}\b", lines[k]):
                    if lab not in inside and lines[k].startswith("o ") is False and lab != "S_%04X" % end:
                        pass
            # outside the region only the region's own aliases mention its
            # labels (verified when the region was chosen) — drop those
            keep = [ln for k, ln in enumerate(lines)
                    if not (i0 <= k < i1) and not (re.match(r"^[SA]_[0-9A-F]{4} = ", ln) and ln.split(" = ")[0] in inside)]
            j = keep.index("S_%04X:" % end)
            pad = bytes(end - base - len(blob))
            keep.insert(j, "blob @%04X %s" % (base, (blob + pad).hex()))
            txt = "\n".join(keep)
            # the overlaid class must never resolve into our code
            txt, k = re.subn(r"^record %s sprite=(\S+) flags=(\S+) code=\S+ rest=(\S+)$" % victim,
                             r"record %s sprite=\1 flags=\2 code=S_13DB rest=\3" % victim, txt, count=1, flags=re.M)
            if k != 1:
                raise SystemExit(f"template {src:X}: record {victim} not found")
            stray = [ln for ln in keep if not ln.startswith("record") and any(
                     re.search(r"\b%s\b" % lab, ln) for lab in inside)]
            if stray:
                raise SystemExit(f"template {src:X}: {len(stray)} lines still reference the overlaid region: {stray[:3]}")
        d = os.path.join(scratch, "level_scripts")
        os.makedirs(d, exist_ok=True)
        for name, body in ((f"{dst:X}.lvsf", txt),
                           (f"{dst:04X}.size.json", json.dumps({"payload_len": new_n}))):
            p = os.path.join(d, name)
            with open(p + ".tmp", "w") as f:
                f.write(body)
            os.replace(p + ".tmp", p)
        extras[f"{dst:04X}"] = {"role": "level_script"}
        print(f"  scene template {dst:04X} = {src:04X} ({n}B): D8 timer @{base:04X} "
              f"({ticks} ticks) + {nblk} letters @{base + len(timer):04X}, {len(blob)}B {where}")
    with open(ex_path + ".tmp", "w") as f:
        json.dump(extras, f, indent=1)
    os.replace(ex_path + ".tmp", ex_path)


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


def patch_1c6_letterfall(scratch, nblk=8):
    p = os.path.join(scratch, "level_scripts", "1C6.lvsf")
    txt = open(p).read()
    if "blob @9F53" in txt:
        return                                    # idempotent rerun
    blob = letterfall_blob(nblk)
    # retarget the orphan D9 record: FFFE pool sprite, D7's 32x32 body,
    # code = our dispatcher (raw address form `code==`)
    old_rec = ("record D9 sprite=0180 flags=0E code==8AD4 "
               "rest=040000007f4000000500000000100010")
    new_rec = ("record D9 sprite=FFFE flags=01 code==%04X "
               "rest=04000000202000000500000000000010" % (LETTER_BASE - 3))
    if old_rec not in txt:
        if "record D9 sprite=FFFE" in txt:
            return                                # already retargeted
        raise SystemExit("1C6 D9 record not in the expected canonical form")
    txt = txt.replace(old_rec, new_rec)
    # the .lvsf header line carries the chunk size — grow it by the append
    old_size = "chunk 01C6 size 40787"
    if old_size not in txt:
        raise SystemExit("1C6 size header not found")
    txt = txt.replace(old_size, f"chunk 01C6 size {40787 + len(blob)}")
    txt = txt.rstrip("\n") + "\nblob @9F53 " + blob.hex() + "\n"
    tmp = p + ".tmp"
    with open(tmp, "w") as f:
        f.write(txt)
    os.replace(tmp, p)
    sj = os.path.join(scratch, "level_scripts", "01C6.size.json")
    meta = json.load(open(sj))
    meta["payload_len"] = 0x9F53 + len(blob)
    with open(sj + ".tmp", "w") as f:
        json.dump(meta, f)
    os.replace(sj + ".tmp", sj)
    print(f"  1C6 letter-fall: D9 -> dispatcher @{LETTER_BASE:04X}, "
          f"{nblk} anims, +{len(blob)}B payload")


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
    demo_steps = {}
    for e in PLAN_SMD:
        b = e["base"]
        print(f"slot {e['slot']} ({e['pw'].decode()}, SMD scene):")
        info = SMD.convert_scene(e["smd"], e["donor"], scratch,
                                 {"hdr": b, "map": b + 1, "tiles": b + 2,
                                  "gtld": b + 4, "pal": b + 5,
                                  "banner": 0x258 + (e["slot"] - 53)},
                                 next_level=e["next"], scene_mode=True,
                                 de_bg=e.get("de_bg"),
                                 gen_bg=e.get("gen_bg"))
        if info.get("ledge"):
            # walk amplitude scaled to the ledge: <112px -> gentle 4-tick
            # strides, <176px -> 6, else the full v4 8-tick stroll
            w = info["ledge"][1] - info["ledge"][0]
            demo_steps[e["slot"]] = 4 if w < 112 else (6 if w < 176 else 8)
        # UX stage 0/1: scenes play full-screen with the camera parked at
        # the map's (pin_x, pin_y) = EXT_L room columns + the Genesis camera
        # mod 16 (genesis_scene.layout); bits 4-11 / 12-15 of the flags
        pin_x = pin_y = 0
        if e.get("gen_bg"):
            import genesis_scene as GS
            pin_x, pin_y = GS.layout(e["gen_bg"]["world"], e["gen_bg"])["pin"]
        lvx.append({"slot": e["slot"], "hdr": b, "pw": e["pw"],
                    "demo": DEMO_CID.get(e["slot"], 0),
                    "flags": LVX_FULLSCREEN | LVX_CAMLOCK | (pin_x << 4) | (pin_y << 12)})
    write_demo_chunks(scratch, demo_steps)
    # canonical predecessors point into the insert chains
    print("progression patch:")
    for e in PLAN + PLAN_SMD:
        if e["prev_hdr"]:
            patch_next(scratch, e["prev_hdr"], e["slot"])
    # UX stage 1 route B: the scenes run on their own world-template copies
    # (build_scene_templates); 1C6 keeps only the world-ladder retarget
    restore_1c6(scratch)
    patch_1c6_ladder(scratch)
    build_scene_templates(scratch)
    # SNDS (slot 49) lives in a NEW header — its next=50 already baked;
    # its predecessor JMNN (0053) got next=49 above.
    build_lvx(scratch, lvx)
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
