#!/usr/bin/env python3
"""smd2pc.py — the six SMD/Genesis-only scenes -> the PC assets tree.

The SMD exclusives are NOT gameplay levels: five 33x33 between-world
cutscenes (the Spaceship one is the game-completion finale) plus the
24x16 Candy bonus room. Ported the same way as the SNES levels
(snes2pc.py), from formats verified in task #110:

  - chunk table @ROM 0x30000 (348 BE u32 entries, flag byte ignored);
    LZSS = our model, header stores the size itself;
  - level header: BE words, dims @0x2E/0x30, chunks @0x34/36/38,
    music/viking block +0x04..0x11 in PC field order; spawns from
    +0x4A as 14B BE records in PC field order;
  - tiles: 32B VDP 4bpp (high nibble = left pixel); prefabs: VDP
    nametable BE words — tile 0-10, hflip 11, vflip 12, pal 13-14,
    prio 15; map words: prefab 0-9 + PC-semantics type bits 10-15;
  - stripe tail: palette entries are 4B (chunk BE u16, base u8, pad)
    to FFFF, then the SNES/PC palette-anim grammar in BE (en word,
    {reload,start,end,frames->FFFF} entries, reload==0 terminator);
    CRAM chunks are 128B = 64 BGR555-like 9-bit colors (bits 1-3 R,
    5-7 G, 9-11 B);
  - spawns: classes/anims carry the SMD numbering; the majority maps
    onto PC ids via the position-matched pairs of the 42 shared levels
    (tools: the mapping ships in smd2pc_map.json built by #110 code);
    scene actors E0/E1 and bonus props 4B/4C/4E exist in the PC .lvs
    class tables under the SAME ids (sprites 0x1B4/0x1B5/0xF0), so
    they pass through unmapped — their PC bytecode is the shared
    handler @0x13DB, behavior verified in-engine, not assumed.

Usage:
  smd2pc.py convert 0x13D --donor 002A --scratch /tmp/lv_edit_scratch \
            --base 0x235 [--next N]
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import assetc as AC  # noqa: E402
import level_render as LR  # noqa: E402

ROM_PATH = "/home/akawolf/projects/own/LostVikingsDE/LostVikingsSMD/LV.gen"
MAP_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "smd2pc_map.json")


class SmdRom:
    def __init__(self, path=ROM_PATH):
        with open(path, "rb") as f:
            self.rom = f.read()
        self.base = 0x30000
        self.n = (self.be32(self.base) & 0xFFFFFF) // 4

    def be32(self, o):
        return int.from_bytes(self.rom[o:o + 4], "big")

    def off(self, i):
        return (self.be32(self.base + i * 4) & 0xFFFFFF) + self.base

    def chunk(self, i):
        o = self.off(i)
        end = self.off(i + 1) if i + 1 < self.n else len(self.rom)
        size = self.rom[o] | (self.rom[o + 1] << 8)   # header = size (LE u16)
        src = self.rom[o + 2:end]
        ring = bytearray(4096)
        bx = si = 0
        out = bytearray()
        while len(out) < size and si < len(src):
            fl = src[si]; si += 1
            for _ in range(8):
                if len(out) >= size:
                    break
                if fl & 1:
                    v = src[si]; si += 1
                    ring[bx] = v; bx = (bx + 1) & 0xFFF
                    out.append(v)
                else:
                    ref = src[si] | (src[si + 1] << 8); si += 2
                    ln = ((ref >> 12) & 0xF) + 3
                    o2 = ref & 0xFFF
                    for _ in range(ln):
                        v = ring[o2]
                        ring[bx] = v; bx = (bx + 1) & 0xFFF
                        out.append(v); o2 = (o2 + 1) & 0xFFF
                        if len(out) >= size:
                            break
                fl >>= 1
        return bytes(out)


def be16(b, o):
    return (b[o] << 8) | b[o + 1]


def smd_tile_nibs(ts, t):
    t32 = ts[t * 32:(t + 1) * 32]
    if len(t32) < 32:
        return None
    px = [0] * 64
    for y in range(8):
        for bx in range(4):
            v = t32[y * 4 + bx]
            px[y * 8 + bx * 2] = v >> 4
            px[y * 8 + bx * 2 + 1] = v & 0xF
    return px


def smd_color_to_vga6(word):
    """9-bit VDP color word -> 6-bit VGA rgb."""
    r = (word >> 1) & 7
    g = (word >> 5) & 7
    b = (word >> 9) & 7
    s = lambda c: (c * 63 + 3) // 7
    return s(r), s(g), s(b)


def parse_tail(c, spawn_end):
    """Stripe tail: pal entries (4B) -> FFFF, then pal-anim grammar (BE),
    then TWO more sections mirroring the PC stripe grammar (found on the
    scene chunks — the first sprite bank of an interlude IS its banner
    letters chunk): sprite banks (6B: [chunk u16 BE][pad 4B]) -> FFFF,
    anim chunks (6B) -> FFFF."""
    o = spawn_end
    pal_list = []
    while be16(c, o) != 0xFFFF:
        pal_list.append({"chunk": be16(c, o), "start": c[o + 2]})
        o += 4
    o += 2
    en = be16(c, o); o += 2
    anims = []
    while c[o] != 0:
        e = {"reload": c[o], "start": c[o + 1], "end": c[o + 2], "frames": []}
        o += 3
        while be16(c, o) != 0xFFFF:
            e["frames"].append(be16(c, o))
            o += 2
        o += 2
        anims.append(e)
    o += 1
    if o & 1:
        o += 1            # FF pad: 68k word-aligns the bank section
    banks = []
    while o + 1 < len(c) and be16(c, o) != 0xFFFF:
        banks.append({"chunk": be16(c, o), "pad": c[o + 2:o + 6].hex()})
        o += 6
    o += 2
    achunks = []
    while o + 1 < len(c) and be16(c, o) != 0xFFFF:
        achunks.append({"chunk": be16(c, o), "pad": c[o + 2:o + 6].hex()})
        o += 6
    return pal_list, en, anims, banks, achunks


def build_de_backdrop(de_bg, CW, CH):
    """DE (SNES Definitive Edition) world-entry field -> the scene map.

    Task #114 final: the DE builds these fields PROCEDURALLY (no field
    map chunk exists anywhere in the ROM — full scans) out of the
    world's unified quad table, so the ground truth for the composition
    is the game's own output. tools/assets/scene_bake.py grabs the
    field between the yellow frame lines of the walkthrough video at
    the measured scale (794px span / 256 = 3.1016; a guessed scale
    drifted half a cell by the right edge — the user-visible tile
    errors), suppresses the overlays (speech bubbles, floating bubbles,
    idle vikings) via a multi-frame least-saturated composite plus
    rectangle inpaint, and quantizes to a 48-color palette. Here the
    baked pixels become tiles directly: DAC 0..47 carries the bake
    palette, 64 the yellow frame lines, 65..79 the SMD banner letters,
    128+ the donor's viking rows — no remapping at all.

    Layout: rows 0-2 title band (banner code repaints 0-1), rows 3..9
    the 112px field centered on black letterbox (SNES look), yellow
    lines over row 3 and on row 10, floor = the field's last row
    (solid 0x0400 on the field columns).

    Returns (mmap, pc_tiles, pc_masks, pc_gtld, pairs, pal128, floor_y,
    vik_xs, ledge) in the exact shapes convert_scene ships downstream.
    """
    import json as _json
    bakes_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                              "de_scene_bakes.json")
    bake = _json.load(open(bakes_path))[de_bg["world"]]
    FWB, FHB = bake["w"], bake["h"]          # 256 x 112
    pix = bytes.fromhex(bake["pix"])
    R = FHB // 16                            # 7 field rows
    # per-world viking placement, read off the video (the trio must
    # stand WHERE the DE scene stands it — Egypt on the masonry ledge,
    # Factory on the beam, Ship on the left step; a centered bottom-row
    # trio sat on the inpaint scars and off the reference spots, user
    # report): scene row whose TOP edge carries the feet, trio center
    # x, demo half-span (feeds the stride scaling)
    # the map is exactly 11 rows = 176px: the scroll limits collapse to
    # (0,0) and the camera is NAILED — a 12-row map parked the camera at
    # y=0 on the high-floor worlds and the map's bottom edge sat exactly
    # on the viewport edge, smearing the frame-line row into the HUD
    # band (Factory 'plaques', DAC-64 pixels measured in the band)
    WORLD_SPOTS = {
        "preh":    (8, 128, 88),
        "egypt":   (7, 104, 52),
        "factory": (5, 96, 52),
        "wacky":   (8, 88, 88),
        "ship":    (6, 64, 52),
    }
    FLOOR, VCENTER, VSPAN = WORLD_SPOTS[de_bg["world"]]
    YROW = 2 + R                     # yellow bottom line rides this row
    YELLOW = 64                      # DAC 64: banner nib-0 slot, unused
    #                                  by the letters -> frame color

    tiles = []
    tile_idx = {}
    prefabs = []
    prefab_idx = {}
    mmap = bytearray()
    for y in range(CH):
        for x in range(CW):
            ltype = 0
            in_field = 2 <= x <= 17          # the 256px SNES field sits
            #                                  centered on black letterbox
            if y < 2 or y > YROW or not in_field:
                cell = [0] * 256
            elif y == YROW:
                # black row with the yellow bottom frame line on top
                cell = [YELLOW if yy < 2 else 0
                        for yy in range(16) for xx in range(16)]
            else:
                fy0, fx0 = (y - 2) * 16, (x - 2) * 16
                cell = [pix[(fy0 + yy) * FWB + fx0 + xx]
                        for yy in range(16) for xx in range(16)]
                if y == 2:
                    # yellow top frame line over the field's first row
                    for xx in range(16):
                        cell[xx] = YELLOW
                        cell[16 + xx] = YELLOW
                if y == FLOOR:
                    ltype = 0x0400
            # 4 straight 8x8 tiles out of the cell
            pcvs = []
            for dy, dx in ((0, 0), (0, 1), (1, 0), (1, 1)):
                tp = bytes(cell[(dy * 8 + yy) * 16 + dx * 8 + xx]
                           for yy in range(8) for xx in range(8))
                if tp not in tile_idx:
                    tile_idx[tp] = len(tiles)
                    tiles.append(tp)
                pcvs.append(tile_idx[tp] << 6)
            pk = tuple(pcvs)
            if pk not in prefab_idx:
                prefab_idx[pk] = len(prefabs)
                prefabs.append(pk)
            mmap.extend(((prefab_idx[pk] | ltype) & 0xFF,
                         (prefab_idx[pk] | ltype) >> 8))
    assert len(tiles) <= 1023, f"{len(tiles)} baked tiles > 10-bit offset"

    pc_gtld = bytearray()
    for pk in prefabs:
        for pcv in pk:
            pc_gtld.extend((pcv & 0xFF, pcv >> 8))
    pc_tiles = bytearray()
    pc_masks = bytearray()
    for tp in tiles:
        pc_tiles += AC.tile_encode(tp)
        m = bytearray(8)
        for ty in range(8):
            for tx in range(8):
                if tp[ty * 8 + tx]:
                    m[(tx & 3) * 2 + (ty >> 2)] |= \
                        1 << (7 - ((ty & 3) * 2 + (tx >> 2)))
        pc_masks += m

    # ---- 128-color palette image: 0..47 = the bake palette (VGA6),
    # 64 = frame yellow, 65..79 = the banner letters (convert_scene),
    # 128+ = the donor sprite rows ----
    pal128 = [(0, 0, 0)] * 128
    for i, (r, g, b) in enumerate(bake["pal"][:48]):
        pal128[i] = (r >> 2, g >> 2, b >> 2)
    pal128[YELLOW] = (63, 55, 0)     # the frame lines (VGA6 yellow)

    # ---- viking spots: the video positions (WORLD_SPOTS) ----
    floor_y = FLOOR * 16
    center = VCENTER
    a, b = center - VSPAN, center + VSPAN
    vik_xs = (center - 0x20, center, center + 0x20)
    print(f"  DE field: world '{de_bg['world']}' baked {FWB}x{FHB}, "
          f"{len(tiles)} tiles / {len(prefabs)} prefabs, "
          f"{len(bake['pal'])} colors")
    return (mmap, pc_tiles, pc_masks, pc_gtld,
            [("T", i) for i in range(len(tiles))], pal128, floor_y, vik_xs,
            (a, b))


# Genesis-scene surface types that are ground for the interlude's actors but
# not on the PC engine: 0x13 (Wacky candy floor — Erik walks it to the ladder
# on the DE clip) and 0x10 (the Preh bottom grass — Olaf glides down onto it
# for "GOOD AND SLOW"; on the PC a viking that lands on 0x10 dies, seen live:
# anim_idx -1, the dead idle loop). Plain ground on the scene map.
GENESIS_FLOOR_TYPES = {0x13: 0x01, 0x10: 0x01}


def build_genesis_backdrop(gen_bg, CW, CH):
    """Genesis inter-world scene -> the scene map, composed EXACTLY the way
    the Genesis VDP shows it (UX plan stage 1; docs2/VERSIONS_DIFF_ANALYSIS
    §7): plane B = the world parallax quad map (scene head +0x40) at 1/4 of
    the camera, plane A = the scene room at the camera, the window band
    (black, yellow line at rows 45-46), the raster-split lower band (yellow
    line at rows 187-188, black below), palette = the scene CRAM. The BAC
    'Definitive Edition' plays these very frames through its Genesis core,
    so this IS the reference look. Pixels become straight 8x8 tiles (dedup),
    CRAM 0..63 -> DAC 0..63, 65..79 the banner letters (convert_scene),
    128+ the donor viking rows. Type bits are the room's own (cell under
    each screen quad's center): the trio stands and walks on the Genesis
    ledge/floor/shelf geometry. The map is CW x CH quads (20 x 13 = 320 x
    208; rows 200..207 ride below the 200-row full-screen scene viewport).

    Camera pin (genesis_scene.layout): the map carries EXT_L quad columns of
    room LEFT of the screen (the off-screen part of the platform the SMD
    scene spawns the trio on — real room cells under the leftmost viking)
    and, because the state cameras sit at a half-quad offset in two worlds
    (Preh 132,120 / Wacky 144,216), cam_x % 16 padding px on the left and
    cam_y % 16 padding rows on top so the room quads stay 16-aligned with
    their type bits; the engine parks the viewport at (pin_x, pin_y) (LVX3
    flags bits 4-11 / 12-15, v2_lvx_pin_camera) — screen pixel (sx, sy) is
    map pixel (sx + pin_x, sy + pin_y). Pixels outside the screen are black
    (never shown); type bits everywhere are the room's own.

    Returns (mmap, pc_tiles, pc_masks, pc_gtld, pairs, pal128, vik_pos,
    walk, trio) — vik_pos = [(x, y)] x3 for Erik/Baleog/Olaf in MAP px (x
    center, the row above the floor top), walk = (x0, x1) map-px span of the
    floor for the demo stroll, trio = [(x, y, anim)] x3 map px for Erik,
    Baleog, Olaf = the mode-2 spawn table rows (sub_11569 code_seg 1/0/2):
    x center, y 48 px above the platform top (drop-in)."""
    from genesis_scene import GenesisScene, WORLD_CAMERA, layout
    world = gen_bg["world"]
    wc = WORLD_CAMERA[world]
    lay = layout(world, gen_bg)
    cam_x, cam_y = lay["cam"]
    pin_x, pin_y = lay["pin"]
    spots = gen_bg.get("spots") or wc["spots"]
    walk = gen_bg.get("walk") or wc["walk"]
    trio = gen_bg.get("trio") or wc["trio"]
    assert (CW, CH) == (lay["cw"], lay["ch"]), (CW, CH, lay)
    sc = GenesisScene(world)
    bg_x, bg_y = sc.parallax(cam_x, cam_y, gen_bg.get("bg_x", wc.get("bg_x")))
    canvas = sc.render_indices(cam_x, cam_y, bg_x, bg_y, rows=200)

    def gpx(gx, gy):
        """Genesis screen pixel -> CRAM index; off the frame = the window's
        black (index 15) — the padding rows/cols never reach the screen."""
        return canvas[gy][gx] if (0 <= gy < 200 and 0 <= gx < 320) else 15

    tiles, tile_idx, prefabs, prefab_idx = [], {}, [], {}
    mmap = bytearray()
    for y in range(CH):
        for x in range(CW):
            cell = [gpx(x * 16 + xx - pin_x, y * 16 + yy - pin_y)
                    for yy in range(16) for xx in range(16)]
            # room cell under this (16-aligned) map quad; the window band
            # (Genesis rows 0..47 — the console's window plane, priority
            # over sprites: a viking can never be seen standing there) and
            # the lower band (rows 187+) are presentation, not room — no
            # type bits on the map rows that lie entirely inside them
            # (seen live: a dropped viking parked on a room ledge hidden
            # behind the black band of the Factory scene)
            tb = sc.type_bits(cam_x - pin_x, cam_y - pin_y, x, y)
            # the interlude rooms use surface type 0x13 (the Wacky candy
            # floor) as walkable ground — the DE clip shows Erik walking
            # it from x 64 to the ladder at 125 — while the PC landing
            # filters (LUT_SCAN_FILTER lists 23/68) know only 1/2/4/5/6/
            # 0x20 and the slopes: the trio fell through it live. Plain
            # ground for the scene map.
            tb = GENESIS_FLOOR_TYPES.get(tb, tb)
            ltype = (tb << 10) & 0xFC00
            if y * 16 - pin_y >= 187 or y * 16 - pin_y + 16 <= 48:
                ltype = 0
            pcvs = []
            for dy, dx in ((0, 0), (0, 1), (1, 0), (1, 1)):
                tp = bytes(cell[(dy * 8 + yy) * 16 + dx * 8 + xx]
                           for yy in range(8) for xx in range(8))
                if tp not in tile_idx:
                    tile_idx[tp] = len(tiles)
                    tiles.append(tp)
                pcvs.append(tile_idx[tp] << 6)
            pk = tuple(pcvs)
            if pk not in prefab_idx:
                prefab_idx[pk] = len(prefabs)
                prefabs.append(pk)
            v = prefab_idx[pk] | ltype
            mmap.extend((v & 0xFF, v >> 8))
    assert len(tiles) <= 1023, f"{len(tiles)} baked tiles > 10-bit offset"
    pc_gtld = bytearray()
    for pk in prefabs:
        for pcv in pk:
            pc_gtld.extend((pcv & 0xFF, pcv >> 8))
    pc_tiles = bytearray()
    pc_masks = bytearray()
    for tp in tiles:
        pc_tiles += AC.tile_encode(tp)
        m = bytearray(8)
        for ty in range(8):
            for tx in range(8):
                if tp[ty * 8 + tx]:
                    m[(tx & 3) * 2 + (ty >> 2)] |= \
                        1 << (7 - ((ty & 3) * 2 + (tx >> 2)))
        pc_masks += m
    pal128 = [(0, 0, 0)] * 128
    for i, w in enumerate(sc.cram):
        pal128[i] = smd_color_to_vga6(w)
    vik_pos = [(x + pin_x, y + pin_y - 1) for (x, y) in spots]
    walk = (walk[0] + pin_x, walk[1] + pin_x)
    # spawn 48 px above the platform top (the SNES-style drop-in, out of
    # sight at screen x -12 / 332), clamped under the window band (0x30)
    trio = [(t[0] + pin_x, max(0x30, t[1] + pin_y - 1 - 48),
             (t[2] if len(t) > 2 else 0)) for t in trio]
    print(f"  Genesis scene: world '{world}' room {sc.cid:03X} cam ({cam_x},{cam_y}) "
          f"pin ({pin_x},{pin_y}) map {CW}x{CH}, parallax ({bg_x},{bg_y}); "
          f"{len(tiles)} tiles / {len(prefabs)} prefabs, "
          f"CRAM {sum(1 for w in sc.cram if w)} colors; trio {trio}")
    return (mmap, pc_tiles, pc_masks, pc_gtld,
            [("T", i) for i in range(len(tiles))], pal128, vik_pos, walk, trio)


def convert_scene(smd_id, donor_cid, scratch, new_cids, next_level=None,
                  keep_vikings=True, scene_mode=False, de_bg=None,
                  gen_bg=None):
    """scene_mode: shape the head like the PC logo/intro scenes (0186/
    018C/017D on the 1C6 script): sel=0, head spawn = class 0xD8 — the
    timed-scene controller that shows the screen for `arg` ticks and
    then transitions BY THE HEAD'S OWN +0x16 NEXT FIELD (the intro
    chain 39->40->38 runs on exactly this), flags 0x0020, arg 0x0078
    (120 ticks) as on 017D. The spawn table is dropped whole — the PC
    logo scenes carry zero spawns; the SMD actors (E0/E1 etc) run on
    their own SMD 68k timing/anims that have no PC counterpart, so the
    scene ships as a live backdrop (map + palette anims + music).
    (The DA controller variant — scene 41's wait-for-key + prev ladder
    S_8C00 — also works, proven by the pilot: prev=3 -> level_load=4;
    D8 wins because the head-next chain needs no prev bookkeeping.)"""
    rom = SmdRom()
    c = rom.chunk(smd_id)
    assert c[:4] == bytes.fromhex("010000f8"), "not an SMD level chunk"
    W, H = be16(c, 0x2E), be16(c, 0x30)
    smap = rom.chunk(be16(c, 0x34))
    sts = rom.chunk(be16(c, 0x36))
    sgt = rom.chunk(be16(c, 0x38))
    nprefab = len(sgt) // 8

    # spawns + tail
    spawns = []
    o = 0x4A
    while be16(c, o) != 0xFFFF:
        spawns.append(dict(x=be16(c, o), y=be16(c, o + 2),
                           half_w=be16(c, o + 4), half_h=be16(c, o + 6),
                           cls=be16(c, o + 8), anim=be16(c, o + 10),
                           pool=be16(c, o + 12)))
        o += 14
    pal_list, pal_en, pal_anims, smd_banks, smd_anims = parse_tail(c, o + 2)
    print(f"SMD {smd_id:03X}: {W}x{H}, {len(sts)//32} tiles, {nprefab} "
          f"prefabs, {len(spawns)} spawns, {len(pal_list)} pal entries, "
          f"{len(pal_anims)} pal anims (en={pal_en:04X})")

    if scene_mode:
        # ---- FIXED-SCREEN crop (the SNES/SMD interludes play on a locked
        # camera; on the video the field never scrolls). Cut the 33x33 room
        # down to one 22x12-quad window around the viking floor: the map
        # then barely exceeds the 344x176 viewport, the scroll limits pin
        # the camera, and the top rows become the black title band. All the
        # downstream code (used-prefab renumber, map, spawns, banner) works
        # on the cropped map transparently. ----
        # 20x11 quads = 320x176 px: the scroll limits collapse to (0,0)
        # (the engine computes them against the 320x176 window — seen live:
        # a 22x12 map let the camera drift to (32,16)), so the screen is
        # HARD-locked like the SNES interlude.
        # 12th row rides BELOW the visible window (11 rows = 176px): the
        # renderer smears garbage into the last on-screen row when it sits
        # exactly on the map's bottom edge (seen live), and the vikings park
        # at the TOP of this layout so the camera stays clamped at y=0.
        CW, CH = 20, 11
        if gen_bg:
            # UX stage 1: the Genesis composition fills the 200-row
            # full-screen scene viewport (LVX_FULLSCREEN); the map geometry
            # (EXT_L room columns left of the screen + pin padding) comes
            # from genesis_scene.layout — see build_genesis_backdrop
            from genesis_scene import layout as _gs_layout
            _lay = _gs_layout(gen_bg["world"], gen_bg)
            CW, CH = _lay["cw"], _lay["ch"]
    gen_trio = None
    if scene_mode and gen_bg:
        # UX stage 1: the scene as the Genesis VDP composes it (what the
        # BAC Definitive Edition shows) — see build_genesis_backdrop
        de_map, de_tiles, de_masks, de_gtld, de_pairs, de_pal128, \
            vik_pos_de, de_ledge, gen_trio = build_genesis_backdrop(gen_bg, CW, CH)
        smap, W, H = bytes(de_map), CW, CH
        # route B (user decision 2026-09-02): the room's own objects ride
        # along — recoded below through the SMD->PC class bijection, they
        # run their native world code on the scene's world-template copy
        # (integrate_snes.build_scene_templates). Room px -> map px = room
        # - camera + pin (genesis_scene.layout); the level controller 48
        # sits at (0,0) as in every PC level (its pool = the world index,
        # same convention on both sides); the SMD scene classes E0 (the
        # letters — ours are the D9 rows) and E1 (actor spots: the viking
        # walks there and speaks the line in `pool` — not ported yet) drop.
        from genesis_scene import layout as _gs_layout2
        _lay2 = _gs_layout2(gen_bg["world"], gen_bg)
        (_cx, _cy), (_px, _py) = _lay2["cam"], _lay2["pin"]
        kept = []
        from genesis_scene import WORLD_CAMERA as _WC0g
        _WC0 = _WC0g[gen_bg["world"]]
        for sp in spawns:
            if sp["cls"] in (0xE0, 0xE1):
                continue
            sp = dict(sp)
            if sp["cls"] == 0x48:
                sp["x"], sp["y"] = 0, 0
            elif sp["cls"] in _WC0.get("drop_cls", []):
                continue           # room prop without a PC counterpart (see WORLD_CAMERA)
            elif sp["cls"] not in (0, 1, 2):
                sp["x"] = (sp["x"] - _cx + _px) & 0xFFFF
                sp["y"] = (sp["y"] - _cy + _py) & 0xFFFF
            kept.append(sp)
        # world-class rows the port adds itself (genesis_scene.WORLD_CAMERA
        # 'extra', SCREEN px): PC classes already — they skip the bijection
        # below by carrying the marker key
        from genesis_scene import WORLD_CAMERA as _WC
        for ex in _WC[gen_bg["world"]].get("extra", []):
            sp = dict(ex)
            sp["x"] = (sp["x"] + _px) & 0xFFFF
            sp["y"] = (sp["y"] + _py) & 0xFFFF
            sp["_pc"] = True
            kept.append(sp)
        # the scene's speaker (class DC, integrate_snes.dialogue_blob):
        # a permanent controller row like D8, parked at the map origin
        kept.append(dict(cls=0xDC, x=0, y=0, half_w=16, half_h=16,
                         anim=0x0800, pool=0, _pc=True))
        spawns = kept
        de_bg = gen_bg       # downstream: the 'shipped in PC shapes' path
    elif scene_mode and de_bg:
        # DE world-entry scenes (task #114): the field is the START AREA
        # of the world's first DE level composited over its backdrop
        # pair; the SMD room is dropped whole (only the banner bank +
        # head music bytes survive from the SMD chunk)
        de_map, de_tiles, de_masks, de_gtld, de_pairs, de_pal128, \
            de_floor_y, de_vik_xs, de_ledge = build_de_backdrop(
                de_bg, CW, CH)
        smap, W, H = bytes(de_map), CW, CH
        spawns = []          # SMD room actors/vikings mean nothing here
        vik_pos_de = [(vx, de_floor_y) for vx in de_vik_xs]
    elif scene_mode:
        _vx = [sp["x"] for sp in spawns if sp["cls"] in (0, 1, 2)] or [80]
        _vy = [sp["y"] for sp in spawns if sp["cls"] in (0, 1, 2)] or [224]
        floor_row = min(_vy) // 16
        c0 = max(1, min(min(_vx) // 16 - 2, W - 1 - CW))
        # window layout mirrors the video: 2 rows of black title band, the
        # viking ledge RIGHT under it, and the room composition (the lake
        # pit / lower floor) filling the rest of the frame downward. The
        # old floor-8 crop showed only sky above the ledge and cut the
        # whole scene body away (full-map render compared to the video).
        r0 = max(0, min(floor_row - 3, H - CH))
        crop = bytearray()
        for y in range(CH):
            row_off = ((r0 + y) * W + c0) * 2
            crop += smap[row_off:row_off + CW * 2]
        smap = bytes(crop)
        W, H = CW, CH
        # shift every spawn into crop coordinates (the E0 banner rows are
        # dropped later anyway; vikings/figures need the shift)
        for sp in spawns:
            sp["x"] = (sp["x"] - c0 * 16) & 0xFFFF
            sp["y"] = (sp["y"] - r0 * 16) & 0xFFFF
        print(f"  scene crop: {CW}x{CH} at quad ({c0},{r0}), "
              f"floor row {floor_row}")

    # class/anim recode via the shared-level mapping (unknown ids pass
    # through — the scene actors exist on PC under the same numbers)
    world = c[5]   # the SMD world byte doubles as the track id
    mp = json.load(open(MAP_PATH)) if os.path.exists(MAP_PATH) else \
        {"cls": {}, "anim": {}}
    # Scene viking figures: the PC forest finale (00DA, script 1C6) spawns
    # the REAL viking classes 00/01/02 as talking scene actors. The anim id
    # selects the FRAME SET (i.e. which viking is drawn), so the mapping is
    # BY CLASS, straight from the 00DA rows: Erik 086F, Baleog 0827,
    # Olaf 082F. (First take mapped by the SMD anim value and painted two
    # Olafs — seen live.)
    # NO 0x800 permanent bit: the permanent pass would spawn them with
    # ANIM_SUB = the table index (0..2), which flips the 1C6 viking code
    # into its intro waypoint choreography (markers absent here -> frozen
    # green stand-ins, seen live). The finale's LIVE vikings sit with
    # ANIM_SUB=FFFF — the scene controller code spawns them itself from
    # the table rows.
    SCENE_VIK_ANIM = {0: 0x086F, 1: 0x0827, 2: 0x082F}
    recoded = passed = 0
    out_spawns = []
    vik_pos = []
    for sp in spawns:
        if scene_mode:
            if sp["cls"] in (0, 1, 2):
                vik_pos.append((sp["x"], sp["y"]))
                continue
            # Nothing but the vikings survives on a 1C6 scene: the 1C6
            # SCENE script resolves gameplay mob classes to unrelated
            # machines (the Egypt scene's leftover rows wedged the VM pass
            # and the D8 timer never fired — seen live); 48/4A/E0/E1/4B/4C/
            # 4E are controller/stub/wipe/prop rows with no figure role
            # there. On a world-template copy (gen_bg, route B) the props
            # are real: fall through to the class bijection.
            if not gen_bg:
                continue
        if not keep_vikings and sp["cls"] in (0, 1, 2):
            continue
        if sp.get("_pc"):
            # a PC-class row the port added itself (WORLD_CAMERA 'extra'):
            # no recoding
            sp = dict(sp)
            sp.pop("_pc")
            out_spawns.append(sp)
            passed += 1
            continue
        k_cls = f"{world}:{sp['cls']:02X}"
        k_anim = f"{world}:{sp['cls']:02X}:{sp['anim']:04X}"
        sp = dict(sp)
        if k_cls in mp["cls"]:
            ncls = mp["cls"][k_cls]
            nanim = mp["anim"].get(k_anim, sp["anim"])
            if ncls != sp["cls"] or nanim != sp["anim"]:
                recoded += 1
            sp["cls"], sp["anim"] = ncls, nanim
        else:
            passed += 1
        out_spawns.append(sp)
    if scene_mode and de_bg:
        # synthetic viking spots: standing ON the walk floor row (the
        # SMD room's viking rows are gone with the room)
        vik_pos = vik_pos_de
    if scene_mode:
        # the timed exit controller as a PERMANENT spawn-table row (bit
        # 0x800 in the anim word drives sub_13ba5)
        # D8's duration = its own ANIM_SUB = ITS ROW INDEX in the spawn
        # table (field[16] in the 8A93 code; 0 -> the 60-tick default,
        # early exit by button edges). FIRST row => index 0 => 60 ticks.
        out_spawns.insert(0, dict(x=0, y=0, half_w=16, half_h=16,
                                  cls=0xD8, anim=0x0800, pool=0))
    if scene_mode and vik_pos and os.environ.get("SMD_SCENE_FIGURES"):
        # D3/D4/D5 statue figures (the respawn-screen recipe: 13DB stub +
        # sprites 0x173-0x175 + anim 0x82F + the 0171 bank rows).
        # OPT-IN while the figure work is unfinished: the figures render
        # (wrong palette rows resolved), but the frame bytes still decode
        # as the intro-machine look and the exit timer regressed in the
        # same round — parked, see project memory.
        xs = sorted(set(x for x, _ in vik_pos))
        if len(xs) < 3:
            bx, by = vik_pos[0]
            vik_pos = [(bx, by), (bx + 56, by), (bx + 112, by)]
        for i, (vx2, vy2) in enumerate(vik_pos[:3]):
            out_spawns.append(dict(x=vx2, y=(vy2 + 28 - 96) & 0xFFFF,
                                   half_w=16, half_h=16,
                                   cls=0xD3 + i, anim=0x082F, pool=0))
    print(f"spawns: {len(out_spawns)} kept, {recoded} recoded via the "
          f"mapping, {passed} passed through"
          + (f", figures D3-D5 at {vik_pos[:3]}"
             if scene_mode and vik_pos else ""))

    # donor stripe (banks/anims/.lvs world context)
    donor_raw = LR.header_raw(donor_cid, scratch)
    dst = LR.parse_stripe(donor_raw)
    dhead = bytearray(bytes.fromhex(dst["head"]))

    hdr_id = new_cids["hdr"]
    tm_id, ts_id, gt_id = new_cids["map"], new_cids["tiles"], new_cids["gtld"]
    pal_chunk = new_cids["pal"]

    if de_bg:
        # DE-backdrop path: build_de_backdrop shipped everything in the
        # PC shapes already (map is LE, tiles carry the palette rows)
        mmap = bytearray(de_map)
        pc_tiles, pc_masks = bytearray(de_tiles), bytearray(de_masks)
        pc_gtld, pairs = bytearray(de_gtld), list(de_pairs)
        prio_ported = 0      # SNES bit-13 layering is not ported (see
        #                      build_de_backdrop)
    else:
        # ---- bake (tile, pal) pairs; prio bit 15 -> PC bit 3 + masks ----
        # The SMD prefab table is SHARED per world (up to ~950 prefabs); a
        # 33x33 scene uses a small subset — renumber the map to just the
        # used prefabs, else the baked tileset blows the 10-bit ceiling.
        used = []
        used_idx = {}
        for i in range(W * H):
            p = be16(smap, i * 2) & 0x3FF
            if p not in used_idx:
                used_idx[p] = len(used)
                used.append(p)
        pairs = []
        pair_idx = {}
        prio_ported = 0
        pc_gtld = bytearray()
        for p in used:
            for k in range(4):
                v = be16(sgt, p * 8 + k * 2)
                t, pal = v & 0x7FF, (v >> 13) & 3
                hf, vf, prio = (v >> 11) & 1, (v >> 12) & 1, (v >> 15) & 1
                prio_ported += prio
                key = (t, pal)
                if key not in pair_idx:
                    pair_idx[key] = len(pairs)
                    pairs.append(key)
                pcv = (pair_idx[key] << 6) | (vf << 5) | (hf << 4) | \
                    (prio << 3)
                pc_gtld += bytes((pcv & 0xFF, pcv >> 8))
        assert len(pairs) <= 1023, f"{len(pairs)} baked > 10-bit offset"
        print(f"used prefabs: {len(used)} of {nprefab}; baked tiles: "
              f"{len(pairs)}; priority bits ported to bit3: {prio_ported}")

        pc_tiles = bytearray()
        pc_masks = bytearray()
        for (t, pal) in pairs:
            px = smd_tile_nibs(sts, t) or [0] * 64
            baked = bytes((v + pal * 16) if v else (pal * 16) for v in px)
            pc_tiles += AC.tile_encode(baked)
            m = bytearray(8)
            for ty in range(8):
                for tx in range(8):
                    if px[ty * 8 + tx]:
                        m[(tx & 3) * 2 + (ty >> 2)] |= \
                            1 << (7 - ((ty & 3) * 2 + (tx >> 2)))
            pc_masks += m

        # ---- map: BE -> LE words, prefab index renumbered to the used
        # set, type bits (10-15) unchanged ----
        mmap = bytearray()
        for i in range(W * H):
            v = be16(smap, i * 2)
            v = used_idx[v & 0x3FF] | (v & 0xFC00)
            mmap += bytes((v & 0xFF, v >> 8))

    # ---- world-title banner: the ORIGINAL SMD letters. The interlude's
    # FIRST sprite bank in the stripe tail IS the banner chunk (0x144
    # PREHISTORIA / 0x145 EGYPT / 0x146 FACTORY / 0x147 WACKY /
    # 0x148 STARSHIP): N blocks of 32x32, 16 nibble tiles per block in
    # COLUMN-major order (proven by render), drawn by the SMD E0 class at
    # the spawn-table E0 rows. The letters use CRAM row 0 of the scene
    # palette (gold 63,45,27 body + white highlights — the same chunk the
    # BG rows come from). Port = data: bake the blocks into map prefabs
    # (2x2 quads per block) above the vikings, letter pixels on DAC slots
    # 64+nib (a private copy of CRAM row 0), plus a slow rotate over the
    # body/highlight slots for the SNES-style shimmer. ----
    title_pal_tail = []
    title_anim = None
    banner_bank = None
    banner_cols = []
    banner_frame0 = 1
    decor_rows = []
    if scene_mode and smd_banks:
        banner = rom.chunk(smd_banks[0]["chunk"])
        nblk = len(banner) // 512
        # the top TWO quad rows become the black title band (the SNES look:
        # the name floats on black above the field) — blank them
        if not gen_bg:
            # (DE-bake path only: the Genesis composition already carries
            # the console's black window band on top and the raster band
            # at the bottom — blanking its last map row would cut the
            # field's lowest rows and the yellow line on the pinned worlds)
            blk_tile = len(pairs)
            pc_tiles += AC.tile_encode(bytes(64))
            pc_masks += bytes(8)
            pairs.append(("BLACK", 0))
            for t in (0, 1, 2, 3):
                pcv = blk_tile << 6
                pc_gtld += bytes((pcv & 0xFF, pcv >> 8))
            black_prefab = len(pc_gtld) // 8 - 1
            for cell in range(W * 2):
                mmap[cell * 2] = black_prefab & 0xFF
                mmap[cell * 2 + 1] = black_prefab >> 8
            # the hidden support row (11) goes black too: the renderer leaks
            # the map's bottom edge into the 24px HUD-less band below the
            # viewport (seen live) — keep whatever it smears black
            for x in range(W):
                cell = (CH - 1) * W + x
                mmap[cell * 2] = black_prefab & 0xFF
                mmap[cell * 2 + 1] = black_prefab >> 8
        # The letters themselves are SPRITES that FALL into the band (the
        # video shows the name dropping in; the SMD E0 rows do the same,
        # pool = phase): encode each 32x32 block as a type-2 strip sprite
        # (4 plane sections x 32 strips x [mask + 8 data]; pixel(x=j*4+p,
        # y=strip)=data[j], mask bit 7-j gates the write) — one block is
        # exactly 16 x 72B units, so anim cmd 01 addresses block K as
        # frame K*16 from sprite base 0 (the bank ships FIRST in the
        # scene's bank list). Letter pixels ride DAC 64+nib (CRAM row 1).
        assert nblk <= 8
        enc = bytearray()
        for blk in range(nblk):
            px = [[0] * 32 for _ in range(32)]
            for t in range(16):
                tx, ty = t // 4, t % 4          # column-major in the block
                nib = smd_tile_nibs(banner, blk * 16 + t) or [0] * 64
                for y in range(8):
                    for x in range(8):
                        v = nib[y * 8 + x]
                        px[ty * 8 + y][tx * 8 + x] = (64 + v) if v else 0
            for plane in range(4):
                for strip in range(32):
                    mask = 0
                    d = bytearray(8)
                    for j in range(8):
                        v = px[strip][j * 4 + plane]
                        if v:
                            mask |= 0x80 >> j
                            d[j] = v
                    enc += bytes((mask,)) + bytes(d)
        # 71-byte zero prefix: the type-2 render reads mask@off-1/data@off
        # (1-based, code-read in v2_render_funcs), and the pool anims can
        # only produce off = frm*72 from base 0 — with the prefix, frame
        # 1+K*16 lands mask-aligned on block K (at 71 + K*1152).
        banner_bank = bytes(71) + bytes(enc)
        banner_frame0 = 1
        if gen_bg:
            # Route B: the scene's bank 0 must stay the world's POOL bank
            # 0x12F (4608 B = 64 x 72 B units, bank 0 of every world level):
            # every pool-sprite class (record sprite FFFE — the Preh bubbles
            # 61 among them) addresses its frames from sprite base 0, so a
            # banner-first list fed them banner bytes (seen live: garbage
            # where a bubble should be). Bank 0 = 0x12F + the 71-byte
            # prefix + the letter stream: the pool frames keep their
            # indices, block K becomes frame 65 + 16K (4608 + 71 = 72*65 - 1,
            # mask-aligned exactly like the 71-prefix at base 0 was).
            pool = LR.read_payload(0x12F, "lzss")[0]
            assert len(pool) % 72 == 0, len(pool)
            # scene decor blocks (WORLD_CAMERA 'decor': an SMD sprite bank
            # of 32x32 blocks, e.g. the Preh bubble frames) follow the
            # letters in the same stream: block nblk+K = frame
            # frame0 + 16*(nblk+K); pixels = the tile nibbles as they are
            # (CRAM row 0 = DAC 0..15, the row the console draws them with)
            from genesis_scene import WORLD_CAMERA as _WC2
            _decor = _WC2[gen_bg["world"]].get("decor")
            if _decor:
                dbank = rom.chunk(_decor["bank"])
                assert len(dbank) >= _decor["blocks"] * 512, (len(dbank), _decor)
                for blk in range(_decor["blocks"]):
                    px = [[0] * 32 for _ in range(32)]
                    for t in range(16):
                        tx, ty = t // 4, t % 4
                        nib = smd_tile_nibs(dbank, blk * 16 + t) or [0] * 64
                        for y in range(8):
                            for x in range(8):
                                px[ty * 8 + y][tx * 8 + x] = nib[y * 8 + x]
                    for plane in range(4):
                        for strip in range(32):
                            mask = 0
                            d = bytearray(8)
                            for j in range(8):
                                v = px[strip][j * 4 + plane]
                                if v:
                                    mask |= 0x80 >> j
                                    d[j] = v
                            enc += bytes((mask,)) + bytes(d)
            banner_bank = bytes(pool) + bytes(71) + bytes(enc)
            banner_frame0 = len(pool) // 72 + 1
        banner_y = 0
        banner_prow = 1
        if gen_bg:
            # UX stage 1: the blocks sit where the console's sprite table
            # puts them (bac_state.py: x0 + 32k at y0, palette row prow —
            # WORLD_CAMERA[world]["banner"]), shifted by the map's camera
            # pin like everything else on the map
            from genesis_scene import WORLD_CAMERA, layout as _gs_layout
            _wc = WORLD_CAMERA[gen_bg["world"]]
            _bn = gen_bg.get("banner") or _wc["banner"]
            _pin_x, _pin_y = _gs_layout(gen_bg["world"], gen_bg)["pin"]
            banner_cols = [_bn["x0"] + _pin_x + blk * 32 for blk in range(nblk)]
            banner_y = _bn["y0"] + _pin_y
            banner_prow = _bn["prow"]
        else:
            col0 = max(0, (W - nblk * 2) // 2)
            banner_cols = [col0 * 16 + blk * 32 for blk in range(nblk)]
        # DAC slots 64..79 = a CRAM row of the scene palette. The letters
        # are SPRITES on the SMD; rows 0/1 share the first 8 colors (the
        # PREHISTORIA nibbles), but the >7 nibbles of FACTORY/STARSHIP/
        # EGYPT/WACKY only look right on row 1 (row 0 paints FACTORY a
        # striped beige, row 1 the yellow neon sign / orange gloss —
        # render-compared across all four rows). The console's own sprite
        # attributes (BAC states) say row 3 for PREHISTORIA and row 2 for
        # the other four — the Genesis-composition path uses those. NO
        # rotate: a [65..70] rotate recolored the lettering white for most
        # of the cycle (seen live); the video shows a steady banner (the
        # SMD shine is a separate sprite overlay, chunk 0x142 — not ported).
        cram_pd = rom.chunk(pal_list[0]["chunk"])
        for i in range(16):
            o2 = banner_prow * 32 + i * 2        # CRAM row banner_prow
            v = be16(cram_pd, o2) if o2 + 1 < len(cram_pd) else 0
            title_pal_tail.append(smd_color_to_vga6(v))
        title_anim = None

    # ---- palette ----
    if de_bg:
        # DE path: 128-color image from the backdrop bake (rows on their
        # remap bases); the SMD banner letters keep their reserved
        # 64..79 window inside it
        pal128 = list(de_pal128)
        # letters paint 64+nibble with nibble >= 1: slots 65..79; slot
        # 64 stays the yellow frame color set by build_de_backdrop
        for i, (r, g, b) in enumerate(title_pal_tail):
            if i >= 1:
                pal128[64 + i] = (r, g, b)
        bg = bytearray()
        for (r, g, b) in pal128:
            bg += bytes((r, g, b))
    else:
        # SMD path: CRAM 64 colors -> VGA6 into colors 0..63; the
        # donor's sprite entries (128+) stay
        cram = [0] * 64
        for e in pal_list:
            pd = rom.chunk(e["chunk"])
            for i in range(len(pd) // 2):
                if e["start"] + i < 64:
                    cram[e["start"] + i] = be16(pd, i * 2)
        bg = bytearray()
        for v in cram:
            r, g, b = smd_color_to_vga6(v)
            bg += bytes((r, g, b))
        # title banner colors ride the same BG pal chunk: slots 64..64+N
        for (r, g, b) in title_pal_tail:
            bg += bytes((r, g, b))
    new_pal_list = [{"chunk": pal_chunk, "start": 0}]
    import os as _os
    if scene_mode and _os.environ.get("SMD_SCENE_PAL_DIAG"):
        # diagnostic: full 00DA palette (backdrop turns forest-colored,
        # vikings must turn correct if their pixels sit below 0x80)
        new_pal_list = [{"chunk": 214, "start": 0}, {"chunk": 3, "start": 0},
                        {"chunk": 3, "start": 128}, {"chunk": 291, "start": 144},
                        {"chunk": 292, "start": 160}, {"chunk": 229, "start": 176},
                        {"chunk": 383, "start": 208}, {"chunk": 220, "start": 240}]
    else:
        # sprite palette rows (128+): the DONOR world's — the scene vikings
        # are the plain gameplay trio now (mode 0x10), colored exactly as on
        # the donor's levels. (The 00DA actor-row set belonged to the retired
        # sel=6 actor experiment; with it + the respawn banks the "vikings"
        # rendered as 64x64 portrait fragments — horned-helmet pile, user
        # report.)
        for e in dst["pal_list"]:
            if e["start"] >= 128:
                new_pal_list.append(dict(e))

    # ---- head: donor base + SMD fields ----
    head = bytearray(dhead)
    head[0x04], head[0x05], head[0x06] = c[4], c[5], c[6]  # music block
    # +0x05 = the track id, and the SMD numbers its world themes
    # differently from the PC: measured on every level head of both ROMs —
    # SMD  02 Ship (079-087, scene 141)  03 Preh (01E-02C, 13D)  04 Egypt
    # (034-042, 13E)  05 Factory (049-059, 13F)  06 Wacky (063-073, 140)
    # 07 ending 08C;  PC  02 Ship (0C6-0D4)  03 Preh (028-034)  04 Egypt
    # (04B-055)  05 Factory (072-080)  07 Wacky (09C-0AA)  06 = the
    # Timewarp vortex 0192 only. Copying the SMD byte verbatim made the
    # Wacky scene play the vortex tune; map through the world theme.
    SMD_TO_PC_TRACK = {0x02: 0x02, 0x03: 0x03, 0x04: 0x04, 0x05: 0x05, 0x06: 0x07}
    head[0x05] = SMD_TO_PC_TRACK.get(c[5], c[5])
    for off2, so in ((0x07, 0x07),):                       # start selector
        head[off2] = c[so]
    for off2, so in ((0x08, 0x08), (0x0A, 0x0A), (0x0C, 0x0C),
                     (0x0E, 0x0E), (0x10, 0x10)):          # viking block (BE)
        v = be16(c, so)
        head[off2], head[off2 + 1] = v & 0xFF, v >> 8
    if scene_mode:
        # head shaped EXACTLY like the PC forest finale 00DA — its scene
        # (controller 48 + viking actor classes) is the proven live recipe:
        # the class codes animate/voice the vikings; a D8 head instead
        # leaves the scene globals cold and the viking frames decode wrong
        # (three green look-alikes — seen live)
        # LIVE vikings: the mode-0x10 gameplay recipe (three vikings spread
        # around the spawn point, Y offsets -0x10/-0x08, ANIM_SUB=FFFF ->
        # the interactive 1C6 viking branch). This is the state the LIVE
        # walking-viking proofs were captured in (walkviks/insc4 states:
        # 25BA=10, spawn=(80,-176), anim=2F — the 0028/LLM0 header shape).
        # The later sel=6 form (copied from the SMD heads, which DO carry
        # sel=6 + anim=0) only works on the SMD engine: on the PC 1C6 the
        # mode-6 spawn anim never raises the viking sprites — invisible
        # "ghost" vikings (user report, reproduced via the vortex chain).
        # Spawn point: from the SMD vik rows (the SMD placed its vikings
        # where the scene floor is). The trio spreads x, x-0x20, x-0x40
        # (anim 0x2F has bit6 clear -> minus), so bias +0x40 keeps all
        # three off the left frame wall; Y = above the SMD floor spot so
        # they drop in — the SNES interlude look. A raw Y=-176 sky drop
        # lands them on the scene's TOP frame wall (33x33 rooms are
        # closed boxes — seen live), so stay inside the room.
        # UX stage 1 (Genesis scenes): mode 2 = the engine's table-driven
        # trio (sub_11446 -> sub_11569, three vikings at INDIVIDUAL
        # positions from ds:0x8508; the LVX4 record fills that table) —
        # the SMD scene places its vikings one by one off-screen too
        head[0x07] = 0x02 if gen_trio else 0x10
        head[0x0C], head[0x0D] = 0x00, 0x00
        head[0x0E], head[0x0F] = 0x2F, 0x00                # spawn anim 0x2F
        head[0x10], head[0x11] = 0x00, 0x00                # spawn pool0 = 0
        vxs = [p[0] for p in vik_pos] or [80]
        vys = [p[1] for p in vik_pos] or [224]
        sxv = max(0x60, min(min(vxs) + 0x40, W * 16 - 0x30))
        # short hop only: a 128px drop stuns the vikings on landing (the
        # "horned" pile = lying vikings with the dizzy-stars loop — user
        # spotted it), keep the fall under the fall-damage threshold
        syv = max(0x20, min(vys) - 48)
        if gen_trio:
            # UX stage 1: mode 2 spawns from the table, not from the head
            # point; keep the head point on the first table row anyway
            sxv, syv = gen_trio[0][0], gen_trio[0][1]
        head[0x08], head[0x09] = sxv & 0xFF, (sxv >> 8) & 0xFF
        head[0x0A], head[0x0B] = syv & 0xFF, (syv >> 8) & 0xFF
        # head+0x1C -> ds:25CF (byte_2AAAF level flags): the donor is a
        # GAMEPLAY level, its bit0 keeps the HUD machinery alive and the
        # scene shows interface garbage in the bottom band (user report).
        # Every PC interlude runs with bit0 clear (vortex 0x08, respawn
        # 0x0C, menu 0x0A); take the vortex's 0x08 — bit3 also routes
        # F10/Alt-X to DOS-quit there, same as the other interludes.
        head[0x1C] = 0x08
        # lvflags stay the donor's: 0x0C (the PC scenes' value) kills the
        # D8 timer exit — bisect-proven
        # NO viewport anchor: in mode 0x10 the head spawn IS the viking
        # drop point and v2_viewport_init_113d8 parks the camera on the
        # vikings itself (the old sel=6 anchor overwrote the spawn with a
        # map-center average — that was part of the ghost-viking regress).
    head[0x29], head[0x2A] = W & 0xFF, W >> 8
    head[0x2B], head[0x2C] = H & 0xFF, H >> 8
    head[0x2E], head[0x2F] = tm_id & 0xFF, tm_id >> 8
    head[0x30], head[0x31] = ts_id & 0xFF, ts_id >> 8
    head[0x32], head[0x33] = gt_id & 0xFF, gt_id >> 8
    if next_level is not None:
        head[0x16], head[0x17] = next_level & 0xFF, next_level >> 8
    for o2 in (0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40):
        head[o2] = donor_raw[o2]

    if de_bg:
        # the SMD palette anims animated the SMD room (water shimmer
        # etc.) — that map is gone; the DE backdrops are static
        pal_en, pal_anims = 0, []
    out = dict(dst)
    out["head"] = bytes(head).hex()
    out["spawns"] = out_spawns
    out["pal_list"] = new_pal_list
    out["pal_anim_en"] = pal_en
    out["pal_anims"] = pal_anims
    if title_anim is not None:
        # rotate the title body slots — en gains the next record's bit
        out["pal_anims"] = list(pal_anims) + [title_anim]
        out["pal_anim_en"] = pal_en | (1 << len(pal_anims))
    # resource sections: the DONOR level's banks/anims stay (out = dict(dst)).
    # The gameplay trio resolves its pool sprites through the stripe banks —
    # the donor world set carries the viking frames on the right pool slots
    # (the walkviks live proof ran on exactly this). The former 00DA/respawn
    # clone made the vikings render as portrait fragments.
    if banner_bank is not None and "banner" in new_cids:
        # the banner bank ships FIRST: pool sprites (class spr FFFE) address
        # frames from sprite base 0, so block K = anim frame K*16. The pad
        # bytes are dead at load time (1167a reads only the chunk id).
        rest_banks = list(out["sprite_banks"])
        if gen_bg:
            # the pool bank rides inside our bank 0 now — drop the donor's
            # own 0x12F entry so it is not loaded twice
            rest_banks = [b for b in rest_banks if b["chunk"] != 0x12F]
        out["sprite_banks"] = ([{"chunk": new_cids["banner"],
                                 "pad": "c0000000"}] + rest_banks)
        # letter spawn rows: AFTER the D8 controller (row 0 keeps the
        # timer's ANIM_SUB=0 default), rows 1..N — the row index IS the
        # dispatcher key (OBJ_ANIM_SUB = spawn row index)
        for blk in range(len(banner_cols)):
            out_spawns.insert(1 + blk, dict(
                x=banner_cols[blk], y=banner_y, half_w=16, half_h=16,
                cls=0xD9, anim=0x0800, pool=1 + blk))
            # pool -> ds:374 -> OBJ_SPAWN_POOL = the dispatcher key:
            # field[16] resolves through the runtime LUT to column 0x1B8
            # (OBJ_SPAWN_POOL) — diagnosed live via the op_73 probe
        # scene decor rows (class DA, WORLD_CAMERA 'extra' with cls DA):
        # same convention — the row index keys the DA dispatcher that
        # integrate_snes.bubble_blob appends to the scene template
        decor_rows = []
        for i, sp in enumerate(out_spawns):
            if sp["cls"] == 0xDA:
                sp["pool"] = i
                decor_rows.append(i)
        out["spawns"] = out_spawns
    new_raw = LR.serialize_stripe(out)

    # ---- write into the scratch tree + extras ----
    def wjson(path, obj):
        tmp = path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(obj, f, indent=1)
        os.replace(tmp, path)

    wjson(os.path.join(scratch, "level_headers", f"{hdr_id:04X}.json"),
          {"format": "level_header_stripe", "chunk": f"{hdr_id:04X}",
           "width": W, "height": H, "tilemap": f"{tm_id:04X}",
           "tileset": f"{ts_id:04X}", "bg_tileset": f"{gt_id:04X}",
           "raw": new_raw.hex()})
    rows = []
    for y in range(H):
        rows.append(" ".join(f"{mmap[(y*W+x)*2] | (mmap[(y*W+x)*2+1]<<8):04X}"
                             for x in range(W)))
    wjson(os.path.join(scratch, "tilemaps", f"{tm_id:04X}.json"),
          {"format": "tilemap_u16", "chunk": f"{tm_id:04X}",
           "width": W, "height": H, "tail": "", "rows": rows})
    for role, cid2, payload in (("tileset", ts_id, bytes(pc_tiles)),
                                ("tile_masks", ts_id + 1, bytes(pc_masks)),
                                ("bg_tileset", gt_id, bytes(pc_gtld))):
        for rel, blob in AC.converter_for(role).extract(cid2, payload):
            path = os.path.join(scratch, rel)
            with open(path + ".tmp", "wb") as f:
                f.write(blob)
            os.replace(path + ".tmp", path)
    pal_path = os.path.join(scratch, "unreferenced", f"{pal_chunk:04X}.bin")
    with open(pal_path + ".tmp", "wb") as f:
        f.write(bytes(bg))
    os.replace(pal_path + ".tmp", pal_path)

    if banner_bank is not None and "banner" in new_cids:
        bp = os.path.join(scratch, "unreferenced",
                          f"{new_cids['banner']:04X}.bin")
        with open(bp + ".tmp", "wb") as f:
            f.write(banner_bank)
        os.replace(bp + ".tmp", bp)

    ex_path = os.path.join(scratch, "extras.json")
    extras = json.load(open(ex_path)) if os.path.exists(ex_path) else {}
    for cid2, role2 in ((hdr_id, "level_header_stripe"), (tm_id, "tilemap"),
                        (ts_id, "tileset"), (ts_id + 1, "tile_masks"),
                        (gt_id, "bg_tileset"), (pal_chunk, "unreferenced")):
        extras[f"{cid2:04X}"] = {"role": role2}
    if banner_bank is not None and "banner" in new_cids:
        extras[f"{new_cids['banner']:04X}"] = {"role": "unreferenced"}
    wjson(ex_path, extras)

    print(f"written: header {hdr_id:04X}, map {tm_id:04X}, tiles {ts_id:04X} "
          f"({len(pc_tiles)}B), masks {ts_id+1:04X}, prefabs {gt_id:04X}, "
          f"palette -> {pal_chunk:04X}"
          + (f", next={next_level}" if next_level is not None else ""))
    return {"header": f"{hdr_id:04X}", "dims": [W, H],
            "tiles": len(pairs), "spawns": len(out_spawns),
            "prio_ported": prio_ported, "pal_anims": len(pal_anims),
            "ledge": de_ledge if de_bg else None,
            "trio": gen_trio,
            # route B: the DA decor rows' indices + the pool frame of the
            # first decor block (integrate_snes.build_scene_templates)
            "decor_rows": decor_rows if scene_mode else [],
            "decor_frame0": (banner_frame0 + 16 * len(banner_cols)) if scene_mode and banner_bank is not None else None}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["convert"])
    ap.add_argument("smd_id", type=lambda v: int(v, 16))
    ap.add_argument("--donor", required=True)
    ap.add_argument("--scratch", default="/tmp/lv_edit_scratch")
    ap.add_argument("--base", type=lambda v: int(v, 16), required=True,
                    help="first of 6 fresh chunk ids (hdr,map,tiles,masks,"
                         "gtld,pal)")
    ap.add_argument("--next", type=int, default=None)
    ap.add_argument("--drop-vikings", action="store_true")
    ap.add_argument("--scene", action="store_true",
                    help="scene-41 head shape (sel=0, DA controller spawn)")
    args = ap.parse_args()
    b = args.base
    convert_scene(args.smd_id, args.donor.upper(), args.scratch,
                  {"hdr": b, "map": b + 1, "tiles": b + 2,
                   "gtld": b + 4, "pal": b + 5},
                  next_level=args.next,
                  keep_vikings=not args.drop_vikings,
                  scene_mode=args.scene)


if __name__ == "__main__":
    main()
