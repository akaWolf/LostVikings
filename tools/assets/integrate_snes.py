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


# UX stage 9 (2026-09-05): the console finale. The SNES DE finale head 0x082
# (slot 0x34 = PC 0x2F, the rock concert) carries what the DOS build left
# out: a second layer BG2 (map 0083 x the level's own quad table 0081 on the
# finale tileset 080, 18x15 quads, fx/fy 1.0 = it stands still behind the
# stage: the dragon head) and two crowd objects (class 4E at (96,208) and
# (224,208), pools 0/2; the PC script 1C6 has class 4E with sprite bank 00F0
# — the "THE END" letters, 12 frames of 1152 B, which no PC head ever loads —
# so the SNES crowd sheet 0x99 becomes a PC bank of the same shape and the
# variant's copy of the script points class 4E at it). Torches are the
# palette animations both heads already share ((1,81,89) (6,26,28) (1,17,17)
# (2,29,31)). Port: an LVX_CONSOLE variant head of slot 0x2F = the PC head
# 0xDA + the BG pair refs (stage-2 parallax format, chunks 0x2FB/0x2FC) +
# the two 4E rows + bank 00F0 in the sprite-bank list; the option CONSOLE
# FINALE (default on) makes v2_load_template take it.
FINALE_SLOT, FINALE_DE_SLOT = 0x2F, 0x34
# archive ids 0x2E8..0x2EF (after the 12 balance levels 0x2A0..0x2E7)
FINALE_HEAD, FINALE_TILEMAP, FINALE_TILESET, FINALE_GTLD, FINALE_PAL = 0x2E8, 0x2E9, 0x2EA, 0x2EC, 0x2ED
FINALE_BGMAP, FINALE_BGTILES = 0x2EE, 0x2EF          # masks = FINALE_TILESET + 1 = 0x2EB
FINALE_CROWD_PAL = 0x2FA                             # SNES palette row 192 (chunk 0x9A): the crowd
FINALE_CROWD_BANK = 0x2FB                            # SNES bank 0x99 (12 x 32x32 4bpp) as a PC type-2 bank
FINALE_SCRIPT = 0x2FC                                # copy of the world script 1C6: class 4E -> that bank


def finale_integrate(scratch, lvx):
    """The console finale as an LVX_CONSOLE | LVX_TALL224 variant of slot 0x2F.

    Base = the SNES DE finale converted like a balance level (convert_level:
    the SNES level map BG1, its tileset/quads baked, its palette with the
    crowd's row 192): the SNES keeps only the stage platforms and the dragon
    head's top in BG1 and the whole backdrop in BG2, the PC baked one opaque
    map instead — so the PC map cannot show the dragon. The SNES world is
    18 quads (288 px) wide; the 320-px PC view gets the PC map's own columns
    18-19 (the right speakers; 20-21 are empty) appended with their tiles and
    quads, so nothing wraps. Then: the BG2 pair as a live layer (fx/fy 1.0,
    padded to 40 tiles), the PC spawn rows (Erik, Baleog, 4B, Olaf, 4C — the
    SNES table has no Baleog and a class-48 controller the PC script does not
    want here) + the two crowd rows 4E, the converted SNES crowd bank first in
    the sprite-bank list (the SNES pad c0000c10 kept; the engine skips the pad)
    with the palette row 192 and a script copy whose class 4E names that bank,
    the PC head's mode/flags/music/next/start point."""
    import json as _json
    import parallax_snes as PX
    de = SP.SnesRom(); R = de.rom
    de_hid = R[0x7686 + FINALE_DE_SLOT * 2] | (R[0x7686 + FINALE_DE_SLOT * 2 + 1] << 8)
    h = de.chunk(de_hid)
    pc_hid = PX.exe_level_head(FINALE_SLOT)
    pc_raw, _ = AC.read_payload(pc_hid, "lzss")
    ex_path = os.path.join(scratch, "extras.json")
    extras = _json.load(open(ex_path)) if os.path.exists(ex_path) else {}
    for stale in (0x2FB, 0x2FC, 0x2FD):                # ids of the first attempt
        extras.pop(f"{stale:04X}", None)
        for rel in (f"unreferenced/{stale:04X}.bin", f"level_headers/{stale:04X}.json", f"tilemaps/{stale:04X}.json"):
            if os.path.exists(os.path.join(scratch, rel)): os.remove(os.path.join(scratch, rel))
    with open(ex_path + ".tmp", "w") as f: _json.dump(extras, f, indent=1)
    os.replace(ex_path + ".tmp", ex_path)
    PX.ensure_head_json(scratch, pc_hid, extras)        # the donor (story head) into the scratch
    nxt = pc_raw[0x16] | (pc_raw[0x17] << 8)
    cids = {"hdr": FINALE_HEAD, "map": FINALE_TILEMAP, "tiles": FINALE_TILESET, "gtld": FINALE_GTLD, "pal": FINALE_PAL}
    exe = open(os.path.join(LR.ROOT, "exe_static.bin"), "rb").read()
    sc_o = LR._EXE_DS + LR._SCRIPT_TBL + FINALE_SLOT * 2
    TMPL_CHUNK[FINALE_SLOT] = exe[sc_o] | (exe[sc_o + 1] << 8)
    # convert_level checks the SNES spawn classes against the donor world's
    # script; the story heads are not in LR's 42-level table, so hand it the
    # exe script-table entry of slot 0x2F (1C6) for the donor head.
    _sfh = LR.script_for_header
    LR.script_for_header = lambda hid, _o=_sfh: TMPL_CHUNK[FINALE_SLOT] if hid == pc_hid else _o(hid)
    try:
        SP.convert_level(de_hid, f"{pc_hid:04X}", scratch, new_cids=cids, next_level=nxt, music=pc_raw[0x05])
    finally:
        LR.script_for_header = _sfh

    # ---- widen the converted level by the PC columns 18-19 ----
    hp = os.path.join(scratch, "level_headers", f"{FINALE_HEAD:04X}.json")
    hj = _json.load(open(hp)); raw = bytearray(bytes.fromhex(hj["raw"]))
    sw, sh = raw[0x29] | (raw[0x2A] << 8), raw[0x2B] | (raw[0x2C] << 8)
    tmj = _json.load(open(os.path.join(scratch, "tilemaps", f"{FINALE_TILEMAP:04X}.json")))
    rows = [[int(w, 16) for w in r.split()] for r in tmj["rows"]]
    # the converted tileset / masks / quads back from the scratch files
    def compile_role(role, cid):
        conv = AC.converter_for(role)
        sample = conv.extract(cid, bytes(64 * 4))         # only to learn the file names
        files = []
        for rel, _ in sample:
            with open(os.path.join(scratch, rel), "rb") as f: files.append((rel, f.read()))
        return bytearray(conv.compile(files))
    tiles = compile_role("tileset", FINALE_TILESET)
    masks = compile_role("tile_masks", FINALE_TILESET + 1)
    gtld = compile_role("bg_tileset", FINALE_GTLD)
    pc_qw = pc_raw[0x29] | (pc_raw[0x2A] << 8)
    pc_tm = AC.read_payload(pc_raw[0x2E] | (pc_raw[0x2F] << 8), "lzss")[0]
    pc_ts = AC.read_payload(pc_raw[0x30] | (pc_raw[0x31] << 8), "lzss")[0]
    pc_gt = AC.read_payload(pc_raw[0x32] | (pc_raw[0x33] << 8), "lzss")[0]
    tile_index = {}
    for i in range(len(tiles) // 64):
        tile_index.setdefault(bytes(tiles[i * 64:(i + 1) * 64]), i)
    quad_index = {}
    for i in range(len(gtld) // 8):
        quad_index.setdefault(bytes(gtld[i * 8:(i + 1) * 8]), i)
    def add_tile(t64):
        k = bytes(t64)
        if k in tile_index: return tile_index[k]
        idx = len(tiles) // 64
        tiles.extend(k)
        tp = LR.tile_decode(k)
        m = bytearray(8)
        for ty in range(8):
            for tx in range(8):
                if tp[ty * 8 + tx]:
                    m[(tx & 3) * 2 + (ty >> 2)] |= 1 << (7 - ((ty & 3) * 2 + (tx >> 2)))
        masks.extend(m)
        tile_index[k] = idx
        return idx
    EXTRA_COLS = (18, 19)
    for y in range(sh):
        for x in EXTRA_COLS:
            wv = pc_tm[(y * pc_qw + x) * 2] | (pc_tm[(y * pc_qw + x) * 2 + 1] << 8)
            e = pc_gt[(wv & 0x3FF) * 8:(wv & 0x3FF) * 8 + 8]
            words = []
            for o in (0, 2, 4, 6):
                dw = e[o] | (e[o + 1] << 8)
                idx = add_tile(pc_ts[(dw & 0xFFC0):(dw & 0xFFC0) + 64].ljust(64, b"\0"))
                words.append((idx << 6) | (dw & 0x003F))
            qk = bytes(v for w in words for v in (w & 0xFF, w >> 8))
            if qk not in quad_index:
                quad_index[qk] = len(gtld) // 8
                gtld.extend(qk)
            rows[y].append(quad_index[qk] | (wv & 0xFC00))
    assert len(tiles) // 64 <= 1024, len(tiles) // 64
    NW = sw + len(EXTRA_COLS)
    tmj["width"] = NW; tmj["rows"] = [" ".join(f"{w:04X}" for w in r) for r in rows]
    with open(os.path.join(scratch, "tilemaps", f"{FINALE_TILEMAP:04X}.json"), "w") as f: _json.dump(tmj, f, indent=1)
    for role, cid, payload in (("tileset", FINALE_TILESET, bytes(tiles)), ("tile_masks", FINALE_TILESET + 1, bytes(masks)),
                               ("bg_tileset", FINALE_GTLD, bytes(gtld))):
        for rel, blob in AC.converter_for(role).extract(cid, payload):
            with open(os.path.join(scratch, rel), "wb") as f: f.write(blob)
    raw[0x29], raw[0x2A] = NW & 0xFF, NW >> 8
    hj["width"] = NW

    # ---- the BG2 pair as a live layer, padded to 40 tiles ----
    mbytes, tbytes, stt = PX.convert_pair(de, h)
    TW, TH = mbytes[0] | (mbytes[1] << 8), mbytes[2] | (mbytes[3] << 8)
    n_t = tbytes[0] | (tbytes[1] << 8)
    tbytes = bytes(((n_t + 1) & 0xFF, (n_t + 1) >> 8)) + tbytes[2:] + bytes(64)   # + one blank tile
    pm = bytearray(bytes((40 & 0xFF, 0, TH & 0xFF, TH >> 8)))
    for ty in range(TH):
        pm += mbytes[4 + ty * TW * 2: 4 + (ty + 1) * TW * 2]
        pm += bytes((n_t & 0xFF, n_t >> 8)) * (40 - TW)
    d = os.path.join(scratch, "unreferenced"); os.makedirs(d, exist_ok=True)
    for cid, blob in ((FINALE_BGMAP, bytes(pm)), (FINALE_BGTILES, tbytes)):
        with open(os.path.join(d, f"{cid:04X}.bin"), "wb") as f: f.write(blob)

    # ---- the head: PC fixed part, SNES BG fields -> our pair, spawns, banks ----
    st = LR.parse_stripe(bytes(raw))
    pcs = LR.parse_stripe(pc_raw); de_st = LR.parse_stripe(h)
    head = bytearray(st["head"] if isinstance(st["head"], (bytes, bytearray)) else bytes.fromhex(st["head"]))
    head[0x00:0x29] = pc_raw[0x00:0x29]                 # mode, music, next, viking start point, lvflags: the PC's
    head[0x16], head[0x17] = nxt & 0xFF, nxt >> 8
    head[0x34:0x38] = h[0x34:0x38]                      # BG size 18 x 15 quads
    head[0x39], head[0x3A] = FINALE_BGMAP & 0xFF, FINALE_BGMAP >> 8
    head[0x3B], head[0x3C] = FINALE_BGTILES & 0xFF, FINALE_BGTILES >> 8
    head[0x3D], head[0x3E] = 0xFF, 0xFF
    head[0x3F:0x43] = h[0x3F:0x43]                      # fx/fy 0x0100
    st["head"] = bytes(head) if isinstance(st["head"], (bytes, bytearray)) else bytes(head).hex()
    crowd = [s for s in de_st["spawns"] if s["cls"] == 0x4E]
    assert len(crowd) == 2, crowd
    st["spawns"] = list(pcs["spawns"]) + crowd
    de_first = de_st["sprite_banks"][0]
    assert de_first["pad"] == "c0000c10" and [b["pad"] for b in de_st["sprite_banks"][1:]] == [b["pad"] for b in pcs["sprite_banks"]]
    # the crowd bank: SNES chunk 0x99 is a 4bpp sheet of 16 tile rows x 12
    # tiles (4 poses x 3 frames of 32x32, see frame_px) whose nibbles 13-15
    # are the blues of palette row 192. The PC class 4E of script 1C6 names
    # bank 00F0 — the same crowd art in PC type 2 (12 frames of 1152 B, the
    # pixels 205-207 = row 192 colours 13-15) shifted 3 rows up; the SNES
    # sheet is used so the figures land on the console's rows. Frame = type
    # 2 (32 wide, 32 rows, 4 planes x 32 strips x (mask + 8 bytes) = 1152 B),
    # pixel = 192 + nibble; 16 frames so that frame index 4p + j is 16 units
    sheet = de.chunk(de_first["chunk"])
    assert de_first["chunk"] == 0x99 and len(sheet) == 12 * 512, (de_first, len(sheet))
    def snes_tile(b):
        px = [[0] * 8 for _ in range(8)]
        for y in range(8):
            p0, p1, p2, p3 = b[y * 2], b[y * 2 + 1], b[16 + y * 2], b[16 + y * 2 + 1]
            for x in range(8):
                bit = 7 - x
                px[y][x] = ((p0 >> bit) & 1) | (((p1 >> bit) & 1) << 1) | (((p2 >> bit) & 1) << 2) | (((p3 >> bit) & 1) << 3)
        return px
    # The crowd animation (identical bytes in the SNES ROM at $B7AA and in the
    # PC script at 0x83F4): three sub-sprites at x -48/-16/+16, y -16, then
    # four poses "01 n n+4 n+8" with n = 00/40/80/C0 (delays 2/3/2/3 ticks —
    # Mesen frames 4000-4040: four crowd states of 6/9/6/9 frames). On the
    # console the operands are OAM tile numbers over the 16-tile-wide VRAM
    # sheet: pose p, sub-sprite j = the 4x4 tile block at row 4p, column 4j —
    # three DIFFERENT frames side by side, rows 0-15 = the whole bank. The
    # PC's anim op 1 (sub_130ef) turns the same bytes into byte offsets, bank
    # base + n*72: with 1152-B frames the +4/+8 sub-sprites would read the
    # previous frame's plane sections and pose 3 the next bank (00F3, the
    # THE END letters) — the crowd never spawns on the PC, so its bank 00F0
    # and this anim were never reconciled. The variant lays the frames out
    # at 16 units each (frame index 4p + j -> unit 16(4p + j)) and patches
    # the twelve operands in its copy of the script accordingly.
    def frame_px(fr):
        # chunk 0x99 = 16 tile rows x 12 tiles (4 poses x 3 frames of 4x4
        # tiles, 6144 B); in VRAM the rows sit at a 16-tile stride from tile
        # 0xC0 (the pad's base) with the letters bank 0x9C (base 0xCC) in
        # columns 12-15. Frame index 4p + j -> sheet rows 4p.., columns 4j..;
        # j = 3 has no frame (blank). Verified: the PC bank 00F0 holds the
        # same pixels shifted 3 rows up (F0 row y = sheet row y + 3).
        px = [[0] * 32 for _ in range(32)]
        p, j = fr // 4, fr % 4
        if j == 3:
            return px
        for ty in range(4):
            for tx in range(4):
                tp = snes_tile(sheet[((4 * p + ty) * 12 + 4 * j + tx) * 32:][:32])
                for y in range(8):
                    for x in range(8):
                        px[ty * 8 + y][tx * 8 + x] = (192 + tp[y][x]) if tp[y][x] else 0
        return px
    benc = bytearray()
    for fr in range(16):
        px = frame_px(fr)
        for plane in range(4):
            for strip in range(32):
                mask = 0; dd = bytearray(8)
                for i in range(8):
                    v = px[strip][i * 4 + plane]
                    if v:
                        mask |= 0x80 >> i; dd[i] = v
                benc += bytes((mask,)) + bytes(dd)
    assert len(benc) == 16 * 1152, len(benc)
    with open(os.path.join(d, f"{FINALE_CROWD_BANK:04X}.bin"), "wb") as f: f.write(bytes(benc))
    # the variant's script: the world script with class 4E's sprite chunk
    # pointed at the converted bank (the lookup sub_12f82 goes by chunk id)
    # and the crowd anim's frame operands on the 16-unit frame grid
    # the mod's 1C6 (the scratch copy already carries the S_8C00 ladder patch
    # of UX stage 1: vortex -> scene slots 0x35..0x39), not the canonical one
    sc = bytearray(LR.open_payload(TMPL_CHUNK[FINALE_SLOT], "lzss", scratch))
    assert sc[0x8C38] == 0x35 and sc[0x8C54] == 0x39, "1C6 ladder patch missing — run the scene templates first"
    o = 0x4E * 0x15
    assert sc[o] | (sc[o + 1] << 8) == 0xF0, sc[o:o + 2].hex()
    sc[o], sc[o + 1] = FINALE_CROWD_BANK & 0xFF, FINALE_CROWD_BANK >> 8
    anim = bytes.fromhex("08d0fff0ff10000af0fff0fff0ff01000408 0f02 01404448 0f03 01808488 0f02 01c0c4c8 0f03".replace(" ", ""))
    a = sc.find(anim)
    assert a == 0x83F4 and sc.find(anim, a + 1) < 0, hex(a)
    for p, at in enumerate((0x8402, 0x8408, 0x840E, 0x8414)):
        assert sc[at] == 0x01 and sc[at + 1] == 0x40 * p, sc[at:at + 4].hex()
        for j in range(3):
            sc[at + 1 + j] = 16 * (4 * p + j)
    with open(os.path.join(d, f"{FINALE_SCRIPT:04X}.bin"), "wb") as f: f.write(bytes(sc))
    st["sprite_banks"] = [{"chunk": FINALE_CROWD_BANK, "pad": de_first["pad"]}] + list(pcs["sprite_banks"])
    # the crowd's palette row: the SNES list carries chunk 0x9A at colour 192,
    # the PC list has no row 192 at all (convert_level keeps the donor's rows)
    row192 = [e for e in de_st["pal_list"] if e["start"] == 192]
    assert len(row192) == 1 and not [e for e in pcs["pal_list"] if e["start"] == 192], (row192, pcs["pal_list"])
    cp = de.chunk(row192[0]["chunk"])
    pal192 = bytes(v for i in range(len(cp) // 2) for v in SP.bgr555_to_vga6(cp[i * 2] | (cp[i * 2 + 1] << 8)))
    with open(os.path.join(d, f"{FINALE_CROWD_PAL:04X}.bin"), "wb") as f: f.write(pal192)
    st["pal_list"] = list(st["pal_list"]) + [{"chunk": FINALE_CROWD_PAL, "start": 192}]
    st["anim_chunks"] = pcs["anim_chunks"]
    new_raw = LR.serialize_stripe(st)
    hj["raw"] = new_raw.hex()
    with open(hp + ".tmp", "w") as f: _json.dump(hj, f, indent=1)
    os.replace(hp + ".tmp", hp)
    extras = _json.load(open(ex_path))
    extras[f"{FINALE_BGMAP:04X}"] = {"role": "unreferenced"}
    extras[f"{FINALE_BGTILES:04X}"] = {"role": "unreferenced"}
    extras[f"{FINALE_CROWD_PAL:04X}"] = {"role": "unreferenced"}
    extras[f"{FINALE_CROWD_BANK:04X}"] = {"role": "unreferenced"}
    extras[f"{FINALE_SCRIPT:04X}"] = {"role": "unreferenced"}
    with open(ex_path + ".tmp", "w") as f: _json.dump(extras, f, indent=1)
    os.replace(ex_path + ".tmp", ex_path)
    TMPL_CHUNK[FINALE_SLOT] = FINALE_SCRIPT           # the LVX record's script (the canonical slot keeps 1C6)
    lvx.append({"slot": FINALE_SLOT, "hdr": FINALE_HEAD, "pw": b"\0\0\0\0", "flags": LVX_CONSOLE | LVX_TALL224})
    print(f"console finale: SNES {de_hid:03X} + PC {pc_hid:04X} cols 18-19 -> head {FINALE_HEAD:04X} map {FINALE_TILEMAP:04X} "
          f"{NW}x{sh} ({len(tiles)//64} tiles, {len(gtld)//8} quads); BG pair {FINALE_BGMAP:04X}/{FINALE_BGTILES:04X} 40x{TH} tiles; "
          f"crowd {[(c['x'], c['y']) for c in crowd]}; crowd bank {FINALE_CROWD_BANK:04X} (12 x 1152 B) + palette row 192 "
          f"{FINALE_CROWD_PAL:04X}; script {FINALE_SCRIPT:04X} (1C6 with 4E -> {FINALE_CROWD_BANK:04X})")


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
LVX_CONSOLE = 0x0040      # UX stage 9: an ALTERNATIVE head of a canonical slot gated by the
                          # CONSOLE FINALE option (the SNES finale's BG2 dragon layer + crowd)
LVX_TALL224 = 0x0080      # UX stage 9: the level is viewed 224 rows tall without a HUD band
                          # (map rows 16..239 on the finale, as the console shows it)
LVX_PALTICK3 = 0x0020     # the palette-animation timers tick once per 3 console
                          # frames (20 Hz; Mednafen 60 fps recordings: Preh grass
                          # reload 2 = a step every 6 frames, Factory letter light
                          # reload 1 = every 3) — the engine decrements them on a
                          # 2/7-per-tick accumulator instead of every 70 Hz tick
                          # (v2_pal_ui_cycle_101be); canonical slots never carry it
                          # (3 x {x,y,anim} for Erik/Baleog/Olaf, ds:0x8508):
                          # the scene head's +0x07 mode byte is 2 (UX stage 1)


def build_lvx(scratch, entries):
    """exe_static.bin + LVX5 trailer: [magic][u16 n][36B records:
    level u16, hdr_cid u16, tmpl_cid u16, pw 4B, demo_cid u16, flags u16,
    trio 18B (3 x {x i16, y u16, anim u16}; zeros unless LVX_TRIO),
    pin_x u16, pin_y u16 (the LVX_CAMLOCK camera pin in px; 2026-09-06: fields
    of their own — LVX3/LVX4 packed the pin into flags bits 4-11 / 12-15 and
    the later flag bits LVX_ALT..LVX_TALL224 collided with it: slot 53's pin_x
    84 = 0x54 read as LVX_CONSOLE and the engine's canonical lookup skipped
    the scene; the engine still decodes the old images)].
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
    for magic in (b"LVX5", b"LVX4", b"LVX3", b"LVX2", b"LVX1"):
        m = img.rfind(magic)
        if m >= 0 and m >= len(img) - 4 - 2 - 36 * 64:
            img = img[:m]
    tr = b"LVX5" + struct.pack("<H", len(entries))
    for e in entries:
        tr += struct.pack("<HHH", e["slot"], e["hdr"], TMPL_CHUNK[e["slot"]])
        tr += e["pw"]
        tr += struct.pack("<HH", e.get("demo", 0), e.get("flags", 0))
        tr += e.get("trio") or bytes(18)
        pin = e.get("pin") or (0, 0)
        tr += struct.pack("<HH", pin[0], pin[1])
    out = os.path.join(scratch, "exe_static.bin")
    tmp = out + ".tmp"
    with open(tmp, "wb") as f:
        f.write(img + tr)
    os.replace(tmp, out)
    print(f"  exe_static: {len(img)}B + LVX5 trailer {len(tr)}B -> {out}")


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


# UX stage 10 (2026-09-06): the SNES DE sound engine's data — the SPC700 driver
# (0x15A2 bytes at $05:8FAE, IPL-loaded to APU $1000 by $05:8D14) and the
# block store at $05:A550 ([u24 dir size][dir: [type][id][len u16]... FF]
# [blocks in dir order]: 0 samples, 1 instruments, 2 small blocks, 3 sequences,
# 4 song sets) — see docs2/SNES_SOUND_ENGINE.md. The store (169 KB) is split
# into <= 0xD800-byte parts (assetc streams are u16-sized; BRR expands under LZSS). Plus a table of the
# SNES level head bytes +5 (music set), +4 (music mode at level entry, the
# $87E1 dispatcher by $19DB) and +6 (mode at exit/death, $87F9 by $19DD) per PC
# slot (0..0x2F and the SNES-exclusive LVX slots) — the console's glue reads
# them from the head copy at $19D7.
SND_DRIVER, SND_DIR, SND_DATA0, SND_NDATA, SND_LEVELS = 0x310, 0x311, 0x312, 4, 0x316
SND_SFXMAP = 0x317         # PC effect number -> console sequence id (tools/assets/snes_sfx_map.json)
# The archive loader (sub_1c8f1 mirror v2_read_chunk) copies a chunk's compressed block
# into FS:0x1000 of a 64 KB segment: a record may hold at most 0xF000 bytes of
# stream. LZSS expands the BRR sample data ~1.13x (a 0xD800 part came out as 0xF302
# and overran the window — the FORTIFY abort of 2026-09-06), so the parts stay at
# 0xC000: 0xD830 compressed, 0x7D0 under the limit.
SND_PART = 0xC000


def snes_sound_integrate(scratch):
    import parallax_snes as PX
    de = SP.SnesRom(); R = de.rom
    def lorom(bank, addr): return (bank << 15) | (addr - 0x8000)
    drv_off = lorom(5, 0x8FAE); driver = R[drv_off:drv_off + 0x15A2]
    base = lorom(5, 0xA550)
    dsz = R[base] | (R[base + 1] << 8) | (R[base + 2] << 16)
    p = base + 3; total = 0; n = 0
    while R[p] != 0xFF:
        total += R[p + 2] | (R[p + 3] << 8); p += 4; n += 1
    assert p + 1 == base + dsz, (hex(p), hex(base + dsz))
    directory = R[base:base + dsz]            # header + entries + FF
    data = R[base + dsz:base + dsz + total]
    parts = [data[i:i + SND_PART] for i in range(0, len(data), SND_PART)]
    assert len(parts) <= SND_NDATA, len(parts)
    d = os.path.join(scratch, "unreferenced"); os.makedirs(d, exist_ok=True)
    ex_path = os.path.join(scratch, "extras.json")
    extras = json.load(open(ex_path)) if os.path.exists(ex_path) else {}
    blobs = [(SND_DRIVER, driver), (SND_DIR, directory)] + [(SND_DATA0 + k, b) for k, b in enumerate(parts)]
    # per-slot SNES head bytes +5 (set) / +6 (mode); 0xFF = no SNES level
    tbl = bytearray(b"\xff\xff\xff\xff" * 64)   # per slot: [+5 set][+4 mode at entry][+6 mode at exit][0]
    # PC slot -> DE ROM slot: levels 0..0x24 as is, the story heads with a
    # SNES twin (PC_STORY: 0x2B/0x2E), the finale 0x2F -> DE 0x34, the five
    # SNES exclusives 48..52 -> DE 0x25.. (parallax_snes.de_slot_of covers
    # the first and last groups only)
    de_of = {s: s for s in range(0x25)}
    de_of.update(PX.PC_STORY); de_of[0x2F] = FINALE_DE_SLOT
    de_of.update({s: 0x25 + (s - 48) for s in PX.LVX_HEADS})
    for slot, de_slot in de_of.items():
        hid, h = PX.de_head(de, de_slot)
        tbl[slot * 4], tbl[slot * 4 + 1], tbl[slot * 4 + 2], tbl[slot * 4 + 3] = h[5], h[4], h[6], 0
    blobs.append((SND_LEVELS, bytes(tbl)))
    # PC effect number -> console sequence id. The scripts carry each machine's own
    # numbering in op 2 / anim command 2 (PC: XMIDI sequence numbers, SNES: driver
    # sequences 0x80..); snes_sfx_map.py pairs the sites of the same classes and
    # animations and writes the table as JSON — packed here as 256 bytes (0 = no twin).
    mp = os.path.join(os.path.dirname(os.path.abspath(__file__)), "snes_sfx_map.json")
    with open(mp) as f: sm = json.load(f)["map"]
    sfx = bytearray(256)
    for k, v in sm.items(): sfx[int(k, 16)] = int(v, 16)
    blobs.append((SND_SFXMAP, bytes(sfx)))
    for cid, blob in blobs:
        with open(os.path.join(d, f"{cid:04X}.bin"), "wb") as f: f.write(blob)
        extras[f"{cid:04X}"] = {"role": "unreferenced"}
    with open(ex_path + ".tmp", "w") as f: json.dump(extras, f, indent=1)
    os.replace(ex_path + ".tmp", ex_path)
    print(f"SNES sound: driver {len(driver)} B -> {SND_DRIVER:04X}, directory {len(directory)} B ({n} blocks) -> {SND_DIR:04X}, "
          f"data {len(data)} B -> {len(parts)} parts from {SND_DATA0:04X}, effect map ({sum(1 for b in sfx if b)} ids) -> {SND_SFXMAP:04X}, level sets -> {SND_LEVELS:04X} "
          f"(slot 1: set {tbl[4]:02X} entry {tbl[5]:02X} exit {tbl[6]:02X}; finale: set {tbl[0x2F*4]:02X} entry {tbl[0x2F*4+1]:02X})")


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
                                  "bubbles": 0x262 + (e["slot"] - 53),
                                  # live plane B pair (Starship starfield)
                                  "par_map": 0x2F0 + 2 * (e["slot"] - 53),
                                  "par_tiles": 0x2F1 + 2 * (e["slot"] - 53)},
                                 next_level=e["next"], scene_mode=True,
                                 de_bg=e.get("de_bg"),
                                 gen_bg=e.get("gen_bg"))
        if info.get("decor_frames"):
            frames[e["slot"]] = info["decor_frames"]
        # UX stage 0/1: scenes play full-screen with the camera parked at
        # the map's (pin_x, pin_y) = EXT_L room columns + the Genesis camera
        # mod 16 (genesis_scene.layout); the record's own pin fields (LVX5)
        pin_x = pin_y = 0
        if e.get("gen_bg"):
            import genesis_scene as GS
            pin_x, pin_y = GS.layout(e["gen_bg"]["world"], e["gen_bg"])["pin"]
        flags = LVX_FULLSCREEN | LVX_CAMLOCK
        if e.get("gen_bg"):
            # + the console's 224-row frame (the room art under its lower
            # band shown, 2026-09-06 — genesis_scene.layout / smd2pc)
            flags |= LVX_NOGATE | LVX_PALTICK3 | LVX_TALL224
        trio = None
        if info.get("trio"):
            # UX stage 1: per-viking placement = the mode-2 table rows of
            # sub_11446/sub_11569 (Erik, Baleog, Olaf = code_seg 1/0/2)
            trio = b"".join(struct.pack("<hHH", x, y, a)
                            for (x, y, a) in info["trio"])
            flags |= LVX_TRIO
        lvx.append({"slot": e["slot"], "hdr": b, "pw": e["pw"],
                    "demo": DEMO_CID.get(e["slot"], 0), "flags": flags,
                    "trio": trio, "pin": (pin_x, pin_y)})
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
    finale_integrate(scratch, lvx)
    snes_sound_integrate(scratch)
    build_lvx(scratch, lvx)
    # UX stage 6: the language banks (BAC translations, Press Start 2P glyph pages)
    import build_locale
    # the scene lines (write_scene_texts: text indices 0x3BA0+ past the EXE table)
    # get their records into the language banks too — the XTRA table of the bank
    rom_smd = SM.SmdRom().rom
    extra = {idx: smd_text_record(rom_smd, ln) for ln, idx in text_idx.items()}
    build_locale.integrate(scratch, extra)
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
