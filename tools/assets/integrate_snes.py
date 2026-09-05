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

R, L, U, D, ACT = 0x100, 0x200, 0x800, 0x400, 0x8000
NEXT, PREV = 0x10, 0x20      # viking switch edges (sub_12e84)

# UX stage 1: the walk-ins. The Genesis scene brings its vikings in ONE BY
# ONE from off-screen (the room's E1 rows are their stops); the PC scene
# spawns the trio at screen x -12 (Factory Erik: 332, facing left) through
# the mode-2 head table (genesis_scene.WORLD_CAMERA['trio']) and the demo
# stream walks each one to its stop. Timing/stops measured on the DE
# walkthrough (2 fps montages): (viking, seconds after scene start, stop
# screen x). Object slots: Erik 0, Baleog 2, Olaf 4 (spawn order code_seg
# 1/0/2 = classes Erik/Baleog/Olaf — class 1 is Erik: the first spawn is
# object slot 0, the HUD's first portrait; the scene strips proved the
# colours live); NEXT (bit 0x10) cycles 0 -> 2 -> 4 -> 0 (sub_12e84).
# Input reaches only the ACTIVE viking, and only while it stands inside
# vp_x-12 .. vp_x+332 (sub_10813 clears the input words otherwise) — the
# waiting spots sit exactly on that edge.
# Positions read off the DE walkthrough clips (scratchpad vid/*_in.mp4,
# 1 fps contact sheets) and the rooms' E1 rows (the bubble anchors):
#  Preh    Erik enters first (x~20 on the ledge), Olaf second (~36, E1
#          389 "HEY ERIC, WATCH THIS" is his olive bubble); Baleog is not
#          in the 14 s clip -> late entry ASSUMED (verify on a full clip).
#  Egypt   Erik 40 and Baleog 76 (E1 394 / 392); Olaf never enters the
#          22 s clip (the gag: "have Olaf go first" while he is not
#          there) -> no Olaf entry.
#  Factory Olaf ~44 and Erik ~64 on the left beam (SMD spawns Erik/Olaf
#          at (-32, 80/88)); Baleog (class 1, SMD spawn +360) comes from
#          the RIGHT along the bottom floor to ~304 (E1 396 "COME ON
#          OVER, OLAF" at (304,144)) about 8 s in.
#  Wacky   Olaf ~20 (E1 400 at 16), Erik 64 (E1 402); Baleog absent for
#          the whole 22 s clip -> late entry ASSUMED.
#  Ship    Olaf 24 (E1 406), Baleog 56 (E1 404), Erik 38.
SCENE_WALK = {
    53: [("erik", 1.8, 20), ("olaf", 4.5, 36), ("baleog", 27.0, 52)],
    54: [("erik", 1.0, 40), ("baleog", 2.5, 76)],
    55: [("olaf", 0.8, 44), ("erik", 2.0, 64), ("baleog", 8.0, 304)],
    56: [("erik", 0.5, 64), ("olaf", 1.5, 20), ("baleog", 26.0, 48)],
    57: [("olaf", 1.0, 24), ("baleog", 3.5, 56), ("erik", 8.5, 38)],
}
# waiting spots that are not the default screen x -12 (genesis_scene
# WORLD_CAMERA trio order Erik/Baleog/Olaf)
START_X = {55: {"baleog": 332}}
VIK_SLOT = {"erik": 0, "baleog": 2, "olaf": 4}
# scripted events after the walk-ins: (viking, seconds, raw RLE). Egypt:
# Erik boasts, runs right and jumps the spike pit (clip ~13 s -> ~16 s):
# ACTION while running = Erik's jump (S_4A25: vel_y by the run speed),
# takeoff ~5 ticks after the press, ~110 px of flight at full speed —
# pressed at x~93 (16 ticks of run-up) he takes off at ~135 and lands at
# ~245, just short of
# the room's sensor/cage objects at 256 (the cage drop itself is the
# Genesis script's, not ported yet).
SCENE_EVENTS = {54: [("erik", 16.0, [(R, 16), (R | ACT, 2), (R, 12)])],
                # Wacky: after his last line Erik walks right to the candy-cane
                # ladder (room type-3 columns at screen x 112..143, rows 1-6 —
                # it runs up into the banner band, whose rows carry no type
                # bits on the scene map) and climbs out of the scene (clip
                # ~20 s -> ~23 s). The ladder hangs 16 px above the floor, so
                # walking into it with UP held does not latch (measured);
                # a jump (ACTION) under it and UP held latch it — the engine
                # snaps him to x 128 and climbs 4 px/tick; with UP held he
                # keeps climbing off the top of the screen and stays there
                56: [("erik", 23.0, [(R, 8), (0, 4), (R, 7), (0, 6), (ACT, 2), (U, 60)])]}
TICK_HZ = 18.2          # DOS INT8 rate = one game tick
# displacement of one held-RIGHT/LEFT burst of n ticks, in px, measured on
# the per-frame trajectories (V2_VIK_DBG=2, Egypt scene): the viking
# accelerates ~+0.6 px/tick^2 from a standstill and slides 1-2 ticks after
# the release; a 1-tick press does not register at all. Erik/Olaf land
# 1-2 px short of Baleog on the long bursts. Longer holds break into the
# ~6 px/tick run, so the choreography is stitched from bursts <= 8 ticks
# with a 4-tick stop between them.
BURST_PX = {"baleog": {2: 3, 3: 4, 4: 8, 5: 11, 6: 16, 7: 22, 8: 29},
            "erik":   {2: 3, 3: 4, 4: 8, 5: 11, 6: 15, 7: 21, 8: 27},
            "olaf":   {2: 3, 3: 4, 4: 8, 5: 11, 6: 15, 7: 21, 8: 27}}
GAP = 4

def walk_bursts(vik, dist):
    """(key, ticks) bursts moving viking `vik` by `dist` px (sign = dir):
    greedy over BURST_PX, the remainder under 3 px is dropped."""
    key = R if dist > 0 else L
    tbl = BURST_PX[vik]
    out = []
    left = abs(dist)
    while left >= min(tbl.values()):
        n = max(k for k, px in tbl.items() if px <= left)
        out += [(key, n), (0, GAP)]
        left -= tbl[n]
    return out

def walk_script(slot, start_x=None):
    """The demo RLE for scene `slot` from SCENE_WALK."""
    start_x = dict(start_x or {})
    t, active, s = 0, 0, []
    def emit(k, n):
        nonlocal t
        s.append((k, n)); t += n
    plan = [(sec, vik, stop_x, None) for (vik, sec, stop_x) in SCENE_WALK[slot]]
    plan += [(sec, vik, None, rle) for (vik, sec, rle) in SCENE_EVENTS.get(slot, [])]
    plan.sort(key=lambda e: e[0])
    for sec, vik, stop_x, rle in plan:
        start = int(round(sec * TICK_HZ))
        if start > t:
            emit(0, start - t)
        while active != VIK_SLOT[vik]:
            emit(NEXT, 1); emit(0, 4)
            active = (active + 2) % 6
        # (Factory ladder, measured for the step-2 choreography: LEFT+UP
        # held from 332 runs a viking along the bottom floor and latches
        # the ladder — room type-3 columns at screen x 256..287, the PC
        # snaps to its centre 272 — after 14 ticks; UP climbs 4 px/tick,
        # 22 ticks top the 64 px onto the type-4 platform cells.)
        if rle is not None:
            for k, n in rle: emit(k, n)
            continue
        x0 = start_x.get(vik, -12)
        for k, n in walk_bursts(vik, stop_x - x0):
            emit(k, n)
    emit(0, 0x7FFF)                                # silence till the timer
    return s

def write_demo_chunks(scratch):
    import json as _json
    ex_path = os.path.join(scratch, "extras.json")
    extras = _json.load(open(ex_path)) if os.path.exists(ex_path) else {}
    for slot, cid in DEMO_CID.items():
        script = walk_script(slot, START_X.get(slot))
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

def letterfall_blob(nblk, base=LETTER_BASE, frame0=1):
    """(blob_bytes, dispatcher_addr) — dispatcher first, then 8 anims.
    `base` = the absolute address the blob lands at (the D9 record's code
    is the block: it opens with the prolog jmp, spawn enters at pc + 3).
    `frame0` = the pool
    frame of block 0: 1 when the banner bank stands alone at sprite base 0
    (the 1C6 scenes), 65 when it rides behind the world's pool bank 0x12F
    (route B: 4608 B = 64 units, then the 71-byte prefix — smd2pc)."""
    # Every block starts with the canonical 3-byte prolog `03 <base+3>`:
    # the spawn enters a class at record pc + 3, but the level-end frame
    # (sub_1424c with ds:32F != 0) re-enters EVERY object at the record's
    # raw code pointer — canonical classes carry that jmp; a record pointed
    # 3 bytes BEFORE the block ran the previous block's tail as code (the
    # D8's `0F` = level end: level 4 jumped straight to 5; the Ship scene
    # died on a ch6/7 FATAL) — seen live.
    prolog = bytes((0x03, (base + 3) & 0xFF, ((base + 3) >> 8) & 0xFF))
    LETTER_BASE = base + 3
    # pass 1: measure the dispatcher: per row: 3 (51 k) + 4 (73 16 addr)
    # ... + 1 (10 destroy default) ; branch targets: 19 addr (3) + idle jmp (3).
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
        assert frame0 + k * 16 < 256, (frame0, k)
        a += bytes((0x01, (frame0 + k * 16) & 0xFF))  # frame = block K
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
    return prolog + bytes(disp) + bytes(anims)


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
    # The 1C6 loop ends the scene on two input edges: `aa 18 b803` (ESC,
    # LUT_BIT_MASK idx 0x18 = 0x1000) and `99 1e b803 / a8 000100` (ACTION
    # 0x8000 = the jump/attack button). The interludes are DEMO-driven and
    # the choreography presses ACTION (Erik's jumps), so the ACTION exit
    # goes (six 0x01 NOPs keep every offset); the ESC skip stays — and the
    # engine's own demo path (v2_transition_kick_102ad) skips on any key
    # anyway, the intro mask turns a keypress into 0xFFFF.
    a8 = D8_CANON.find(bytes.fromhex("a8000100cf8a"))   # canon offset (relocated above)
    assert a8 > 0, "D8 canon: ACTION exit not found"
    b[a8:a8 + 6] = bytes([0x01] * 6)
    return bytes(b)


def bubble_blob(base, rows, frame0, rise=168, step=4, ticks=3, period=170):
    """Class DA for the Preh scene: the Genesis bubbles (frames frame0 =
    bubble, +16 = wobble, +32 = pop, from the SMD scene bank 0x0C0 riding
    the scene bank behind the letters). Dispatcher on the spawn row (the
    same `51 k / 73 16` ladder as the letters; unknown row -> 10 destroy),
    per row a phase wait on the object timer (flags |= 0x1000, field[1C] =
    phase, yield until 0 — the D8 idiom), then the shared anim: type-2 32
    strips, X = WORLD_X, and a loop of `0A y` steps up from the spawn point
    (y = -step*i, `ticks` apart -> rise/ticks*step px per tick: 4 px per 3
    ticks = 1.33 px/tick, the DE video's 80 px per 3.5 s) with a +-3 px `08`
    wobble and the wobble frame every other step, the pop frame at the
    top, then parked 300 px below the spawn point for the rest of the
    `period`, and around again. Lanes stagger by period/len(rows)."""
    prolog = bytes((0x03, (base + 3) & 0xFF, ((base + 3) >> 8) & 0xFF))   # see letterfall_blob
    base += 3
    n = len(rows)
    disp = bytearray()
    branches = []
    for r in rows:
        disp += bytes((0x51, r & 0xFF, (r >> 8) & 0xFF))
        branches.append(len(disp) + 2)
        disp += bytes((0x73, 0x16, 0x00, 0x00))
    disp += bytes((0x10,))                       # foreign row: destroy
    bodies = []
    go_refs = []
    for k in range(n):
        bodies.append(len(disp))
        phase = (period * k) // n
        disp += bytes((0x51, 0x00, 0x10, 0x62, 0x08))            # flags |= 0x1000 (timer runs)
        disp += bytes((0x51, phase & 0xFF, phase >> 8, 0x56, 0x1C))
        wait = len(disp)
        disp += bytes((0x00, 0x01, 0x51, 0x00, 0x00, 0x73, 0x1C, 0x00, 0x00))
        go_refs.append(len(disp) - 2)
        disp += bytes((0x03,)) + (base + wait).to_bytes(2, "little")
    go = len(disp)
    disp += bytes((0x19, 0x00, 0x00))            # anim (patched below)
    idle = len(disp)
    disp += bytes((0x2F, 0x00, 0x01, 0x03)) + (base + idle).to_bytes(2, "little")
    for k in range(n):
        tgt = base + bodies[k]
        disp[branches[k]] = tgt & 0xFF
        disp[branches[k] + 1] = tgt >> 8
        disp[go_refs[k]] = (base + go) & 0xFF
        disp[go_refs[k] + 1] = (base + go) >> 8
    anim_at = base + len(disp)
    disp[go + 1] = anim_at & 0xFF
    disp[go + 2] = anim_at >> 8
    a = bytearray()
    a += bytes((0x15, 0x01))                     # type-2, 32 strips
    a += bytes((0x01, frame0 & 0xFF))            # bubble frame
    a += bytes((0x08, 0x00, 0x00))               # X = WORLD_X
    loop_at = anim_at + len(a)
    y = 0
    i = 0
    spent = 0
    while y > -rise:
        y -= step
        a += bytes((0x0A,)) + int(y).to_bytes(2, "little", signed=True)
        dx = 3 if (i // 4) % 2 == 0 else -3
        a += bytes((0x08,)) + int(dx).to_bytes(2, "little", signed=True)
        a += bytes((0x01, (frame0 + (16 if i % 2 else 0)) & 0xFF))
        a += bytes((0x0F, ticks))
        spent += ticks
        i += 1
    a += bytes((0x01, (frame0 + 32) & 0xFF, 0x0F, 0x04))    # pop
    spent += 4
    a += bytes((0x0A,)) + int(300).to_bytes(2, "little", signed=True)  # park out of sight
    a += bytes((0x01, frame0 & 0xFF))
    rest = max(2, period - spent)
    while rest > 0:
        d = min(rest, 250)
        a += bytes((0x0F, d))
        rest -= d
    a += bytes((0x03,)) + loop_at.to_bytes(2, "little")
    return prolog + bytes(disp) + bytes(a)


D8_REST = "04000000101000000500000000000000"   # 1C6's D8 record body
D9_REST = "04000000202000000500000000000010"   # the D7-shaped 32x32 body
# Scene durations in game ticks (DOS 18.2 Hz; the D8 timer is a 16-bit
# tick count, d8_timer_blob). Ship: the clip shows the fade to the level
# 17 s after its start, ~20 s into the scene. The other four are
# ESTIMATES from the earlier walkthrough timing (Preh ~44 s, Egypt ~40,
# Factory ~52, Wacky ~46) — their clips end before the fade.
SCENE_TICKS = {53: 801, 54: 728, 55: 946, 56: 837, 57: 370}
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


def build_scene_templates(scratch, nblk=8, ticks=SCENE_TICKS, decor=None, text_idx=None):
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
        # the banner blocks ride behind the world's pool bank 0x12F in the
        # scene's bank 0 (smd2pc.convert_scene, gen_bg): block K = pool
        # frame len(0x12F)/72 + 1 + 16K
        pool_len = len(LR.read_payload(0x12F, "lzss")[0])
        assert pool_len % 72 == 0, pool_len
        frame0 = pool_len // 72 + 1
        dec = (decor or {}).get(slot)                # (rows, frame0) of the DA decor class
        dia = SCENE_DIALOGUE.get(slot) if text_idx else None
        t_slot = ticks[slot] if isinstance(ticks, dict) else ticks
        # the blocks, sized at base 0 (rebuilt at their final addresses)
        sz_timer = len(d8_timer_blob(0, t_slot))
        sz_letters = len(letterfall_blob(nblk, base=0, frame0=frame0))
        sz_bub = len(bubble_blob(0, dec[0], dec[1])) if dec else 0
        sz_dia = len(dialogue_blob(0, dia, text_idx)) if dia else 0
        need = sz_timer + sz_letters + sz_bub + sz_dia
        # placement: everything appended past the payload when the 48K
        # buffer has room; else the letters (the big block) overlay
        # FREE_REGION and the small controllers still append — the Ship
        # template 1C1 leaves exactly 180 B past its payload
        if n + need <= TMPL_BUF:
            tail_base, region = n, None
            order = ["timer", "letters"] + (["bub"] if dec else []) + (["dia"] if dia else [])
            where = "appended"
        else:
            rbase, rend, victim = FREE_REGION[src]
            assert sz_letters + sz_bub <= rend - rbase, (src, sz_letters + sz_bub, rend - rbase)
            assert n + sz_timer + sz_dia <= TMPL_BUF, (src, n + sz_timer + sz_dia, TMPL_BUF)
            tail_base, region = n, (rbase, rend, victim)
            order = ["timer"] + (["dia"] if dia else [])
            where = f"letters overlaid on {victim} {rbase:04X}-{rend:04X}, controllers appended"
        # build the tail (appended) and the region blob at their addresses
        addr = {}
        tail = b""
        for name in order:
            a = tail_base + len(tail); addr[name] = a
            if name == "timer":   tail += d8_timer_blob(a, t_slot)
            elif name == "letters": tail += letterfall_blob(nblk, base=a, frame0=frame0)
            elif name == "bub":   tail += bubble_blob(a, dec[0], dec[1])
            elif name == "dia":   tail += dialogue_blob(a, dia, text_idx)
        region_blob = b""
        if region:
            rbase, rend, victim = region
            a = rbase; addr["letters"] = a
            region_blob = letterfall_blob(nblk, base=a, frame0=frame0)
            if dec:
                addr["bub"] = rbase + len(region_blob)
                region_blob += bubble_blob(addr["bub"], dec[0], dec[1])
        new_n = n + len(tail)
        recs = [("D8", "record D8 sprite=FFFF flags=00 code==%04X rest=%s" % (addr["timer"], D8_REST)),
                ("D9", "record D9 sprite=FFFE flags=01 code==%04X rest=%s" % (addr["letters"], D9_REST))]
        if dec:
            recs.append(("DA", "record DA sprite=FFFE flags=01 code==%04X rest=%s" % (addr["bub"], D9_REST)))
        if dia:
            recs.append(("DC", "record DC sprite=FFFF flags=00 code==%04X rest=%s" % (addr["dia"], D8_REST)))
        for cls, new in recs:
            txt, k = re.subn(r"^record %s .*$" % cls, new, txt, count=1, flags=re.M)
            if k != 1:
                raise SystemExit(f"template {src:X}: record {cls} not found")
        txt = txt.replace(m.group(0), "chunk %04X size %d" % (dst, new_n), 1)
        if region:
            rbase, rend, victim = region
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
            # the overlaid class must never resolve into our code
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
              + (f" + DA decor rows {dec[0]} frame {dec[1]} @{addr['bub']:04X}" if dec else "")
              + (f" + DC dialogue {len(dia)} lines @{addr['dia']:04X}" if dia else "")
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


# The spoken lines per scene: (seconds after scene start, SMD line, bubble
# anchor screen x, y, seconds shown). Read off the DE clips (scratchpad
# vid/*_in.mp4, 2 fps frames, bubble colour detection + the contact sheets):
# the box body's centre x and bottom y. The clips of Egypt/Factory/Wacky/
# Ship start with the vikings already in place, ~3 s into the scene
# (ASSUMED offset), Preh's ~1 s in (banner already up, ledge still empty).
# Speakers by bubble colour: red Erik (394/395/402/403), green Baleog
# (392/393/396/404/405), olive Olaf (389/398/400/401/406).
# PC box geometry (measured live, val2 = 0): the box is centred on the
# anchor x and its bottom border sits 4 px above the anchor y (the tail);
# sub_12613 clamps it onto the screen. Genesis boxes are ~3% narrower.
SCENE_DIALOGUE = {
    53: [(7.0, 389, 100, 111, 3.5)],
    54: [(3.0, 394, 86, 131, 1.5), (5.0, 392, 153, 139, 3.5), (10.0, 395, 99, 149, 3.5),
         (20.0, 393, 240, 137, 4.0)],
    55: [(14.5, 396, 220, 95, 3.5), (20.0, 398, 109, 112, 3.5)],
    56: [(4.0, 400, 124, 95, 3.5), (9.0, 402, 183, 95, 3.5), (14.5, 401, 104, 82, 2.5),
         (18.0, 403, 169, 87, 3.5)],
    57: [(7.0, 404, 119, 119, 3.5), (12.0, 406, 123, 112, 3.5), (16.5, 405, 138, 104, 3.0)],
}


def dialogue_blob(base, lines, text_idx):
    """Class DC — the scene's speaker. Bytecode in the 1C6 finale's own
    idiom (its D8 prolog + the S_9620 wait loop, ops verified in v2_vm):
    prolog jmp base+3 (spawn enters at record pc + 3) / 19 anim (the 0E
    byte at the end) / 2F / acc = 0x1000, 62 08 (OR field 08: field 1C
    counts down) ; per line: acc = delay, 56 1C, 05 WAIT ; 41 00 <idx>
    <val2 = 0> 00 <x> <y> (text index literal, tail bottom-middle, literal
    anchor) ; acc = hold, 56 1C, 05 WAIT ; 42 (cmd type 2: box erased) ;
    then idle: 00 01 03 idle. WAIT: 00 01 / 51 0000 / 73 1C exit / 03 WAIT
    / exit: 06 (op 73: jump when field == acc)."""
    b = bytearray()
    fix = []                                   # (offset, blob-relative target)
    def emit(*xs):
        b.extend(xs)
    def ref(target_getter):
        fix.append((len(b), target_getter)); emit(0, 0)
    emit(0x03); ref(lambda: 3)                 # prolog jmp -> base+3
    emit(0x19); ref(lambda: anim_off)          # anim -> the 0E byte
    emit(0x2F, 0x51, 0x00, 0x10, 0x62, 0x08)
    t_prev = 0
    for (sec, line, x, y, hold) in lines:
        t = int(round(sec * TICK_HZ))
        delay = max(1, t - t_prev)
        emit(0x51, delay & 0xFF, delay >> 8, 0x56, 0x1C, 0x05); ref(lambda: wait_off)
        idx = text_idx[line]
        emit(0x41, 0x00, idx & 0xFF, idx >> 8, 0x00, 0x00, 0x00,
             x & 0xFF, (x >> 8) & 0xFF, y & 0xFF, (y >> 8) & 0xFF)
        h = int(round(hold * TICK_HZ))
        emit(0x51, h & 0xFF, h >> 8, 0x56, 0x1C, 0x05); ref(lambda: wait_off)
        emit(0x42)
        t_prev = t + h
    idle_off = len(b)
    emit(0x00, 0x01, 0x03); ref(lambda: idle_off)
    wait_off = len(b)
    emit(0x00, 0x01, 0x51, 0x00, 0x00, 0x73, 0x1C); ref(lambda: exit_off)
    emit(0x03); ref(lambda: wait_off)
    exit_off = len(b)
    emit(0x06)
    anim_off = len(b)
    emit(0x0E)
    for off, getter in fix:
        a = base + getter()
        b[off], b[off + 1] = a & 0xFF, a >> 8
    return bytes(b)


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
    decor = {}
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
        if info.get("decor_rows"):
            decor[e["slot"]] = (info["decor_rows"], info["decor_frame0"])
        # UX stage 0/1: scenes play full-screen with the camera parked at
        # the map's (pin_x, pin_y) = EXT_L room columns + the Genesis camera
        # mod 16 (genesis_scene.layout); bits 4-11 / 12-15 of the flags
        pin_x = pin_y = 0
        if e.get("gen_bg"):
            import genesis_scene as GS
            pin_x, pin_y = GS.layout(e["gen_bg"]["world"], e["gen_bg"])["pin"]
        flags = LVX_FULLSCREEN | LVX_CAMLOCK | (pin_x << 4) | (pin_y << 12)
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
    text_idx = write_scene_texts(scratch)
    build_scene_templates(scratch, decor=decor, text_idx=text_idx)
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
