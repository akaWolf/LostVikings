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
    """Stripe tail: pal entries (4B) -> FFFF, then pal-anim grammar (BE)."""
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
    return pal_list, en, anims


def convert_scene(smd_id, donor_cid, scratch, new_cids, next_level=None,
                  keep_vikings=True, scene_mode=False):
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
    pal_list, pal_en, pal_anims = parse_tail(c, o + 2)
    print(f"SMD {smd_id:03X}: {W}x{H}, {len(sts)//32} tiles, {nprefab} "
          f"prefabs, {len(spawns)} spawns, {len(pal_list)} pal entries, "
          f"{len(pal_anims)} pal anims (en={pal_en:04X})")

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
            # Dropped rows: 48/4A (level controller / 13DB stub), E0/E1 (the
            # PC-side code draws wipe circles, bank 1B4 — not figures),
            # 4B/4C/4E (finale props of the 08C scene — that scene itself is
            # NOT ported: the PC finale 00DA already exists at slot 46).
            if sp["cls"] in (0x48, 0x4A, 0xE0, 0xE1, 0x4B, 0x4C, 0x4E):
                continue
        if not keep_vikings and sp["cls"] in (0, 1, 2):
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

    # ---- bake (tile, pal) pairs; prio bit 15 -> PC bit 3 + masks ----
    # The SMD prefab table is SHARED per world (up to ~950 prefabs); a
    # 33x33 scene uses a small subset — renumber the map to just the used
    # prefabs, else the baked tileset blows the 10-bit offset ceiling.
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
            pcv = (pair_idx[key] << 6) | (vf << 5) | (hf << 4) | (prio << 3)
            pc_gtld += bytes((pcv & 0xFF, pcv >> 8))
    assert len(pairs) <= 1023, f"{len(pairs)} baked tiles > 10-bit offset"
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

    # ---- map: BE -> LE words, prefab index renumbered to the used set,
    # type bits (10-15) unchanged ----
    mmap = bytearray()
    for i in range(W * H):
        v = be16(smap, i * 2)
        v = used_idx[v & 0x3FF] | (v & 0xFC00)
        mmap += bytes((v & 0xFF, v >> 8))

    # ---- palette: CRAM 64 colors -> VGA6 into colors 0..63; the donor's
    # sprite entries (128+) stay ----
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
    new_pal_list = [{"chunk": pal_chunk, "start": 0}]
    import os as _os
    if scene_mode and _os.environ.get("SMD_SCENE_PAL_DIAG"):
        # diagnostic: full 00DA palette (backdrop turns forest-colored,
        # vikings must turn correct if their pixels sit below 0x80)
        new_pal_list = [{"chunk": 214, "start": 0}, {"chunk": 3, "start": 0},
                        {"chunk": 3, "start": 128}, {"chunk": 291, "start": 144},
                        {"chunk": 292, "start": 160}, {"chunk": 229, "start": 176},
                        {"chunk": 383, "start": 208}, {"chunk": 220, "start": 240}]
    elif scene_mode:
        # sprite colors (128+) must match the scene viking frames — clone
        # the 00DA finale's sprite palette rows (its 128+ entries carry
        # the scene-viking coloring; the donor world's rows paint them
        # green — seen live)
        new_pal_list += [{"chunk": 3, "start": 128},
                         {"chunk": 291, "start": 144},
                         {"chunk": 292, "start": 160},
                         {"chunk": 229, "start": 176},
                         {"chunk": 383, "start": 208},
                         {"chunk": 220, "start": 240}]
    else:
        for e in dst["pal_list"]:
            if e["start"] >= 128:
                new_pal_list.append(dict(e))

    # ---- head: donor base + SMD fields ----
    head = bytearray(dhead)
    head[0x04], head[0x05], head[0x06] = c[4], c[5], c[6]  # music block
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
        head[0x07] = 0                                     # sel = 0
        head[0x0C], head[0x0D] = 0xD8, 0x00                # timed controller
        head[0x0E], head[0x0F] = 0x20, 0x00                # flags 0x0020
        head[0x10], head[0x11] = 0x78, 0x00                # arg = 120 ticks
        # (live viking actors need the sel=6 head channel, which trips the
        # stage-4 strict evac guard in the CURRENT engine build — parked;
        # the scenes ship with the D3-D5 statue figures, the respawn-screen
        # recipe, until that guard is taught the scene path)
        # lvflags stay the donor's: 0x0C (the PC scenes' value) kills the
        # D8 timer exit — bisect-proven
        # viewport anchor: the camera parks on the head spawn — aim it at
        # the SMD actors (the scene's action happens around its spawns;
        # skip the (0,0) controller row and the off-screen E0 banners),
        # falling back to the non-background map cells
        sx = sy = n = 0
        for sp in spawns:
            if sp["cls"] == 0x48 or not (0 < sp["y"] < H * 16):
                continue
            sx += sp["x"]; sy += sp["y"]; n += 1
        if not n:
            bgp = be16(smap, 0) & 0x3FF
            for i in range(W * H):
                if (be16(smap, i * 2) & 0x3FF) != bgp:
                    sx += (i % W) * 16 + 8
                    sy += (i // W) * 16 + 8
                    n += 1
        if n:
            cx, cy = sx // n, sy // n
            vx = max(0x90, min(cx, W * 16 - 344 + 0x90))
            vy = max(0x58, min(cy, H * 16 - 176 - 64 + 0x58))
            head[0x08], head[0x09] = vx & 0xFF, vx >> 8
            head[0x0A], head[0x0B] = vy & 0xFF, vy >> 8
    head[0x29], head[0x2A] = W & 0xFF, W >> 8
    head[0x2B], head[0x2C] = H & 0xFF, H >> 8
    head[0x2E], head[0x2F] = tm_id & 0xFF, tm_id >> 8
    head[0x30], head[0x31] = ts_id & 0xFF, ts_id >> 8
    head[0x32], head[0x33] = gt_id & 0xFF, gt_id >> 8
    if next_level is not None:
        head[0x16], head[0x17] = next_level & 0xFF, next_level >> 8
    for o2 in (0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40):
        head[o2] = donor_raw[o2]

    out = dict(dst)
    out["head"] = bytes(head).hex()
    out["spawns"] = out_spawns
    out["pal_list"] = new_pal_list
    out["pal_anim_en"] = pal_en
    out["pal_anims"] = pal_anims
    if scene_mode:
        # resource sections cloned wholesale from the PC ship finale 00DA —
        # the proven "vikings as scene actors on the 1C6 script" recipe
        # (its banks carry the scene viking sprites; FFFE pool resolution
        # rides on them)
        out["sprite_banks"] = [
            {"chunk": 371, "pad": "c000080c"},
            {"chunk": 373, "pad": "c800080c"},
            {"chunk": 372, "pad": "80011008"},
            {"chunk": 306, "pad": "c8010202"},
        ]
        out["anim_chunks"] = [
            {"chunk": c, "pad": "040401"}
            for c in (375, 376, 377, 378, 414, 415, 416, 417, 368)]
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

    ex_path = os.path.join(scratch, "extras.json")
    extras = json.load(open(ex_path)) if os.path.exists(ex_path) else {}
    for cid2, role2 in ((hdr_id, "level_header_stripe"), (tm_id, "tilemap"),
                        (ts_id, "tileset"), (ts_id + 1, "tile_masks"),
                        (gt_id, "bg_tileset"), (pal_chunk, "unreferenced")):
        extras[f"{cid2:04X}"] = {"role": role2}
    wjson(ex_path, extras)

    print(f"written: header {hdr_id:04X}, map {tm_id:04X}, tiles {ts_id:04X} "
          f"({len(pc_tiles)}B), masks {ts_id+1:04X}, prefabs {gt_id:04X}, "
          f"palette -> {pal_chunk:04X}"
          + (f", next={next_level}" if next_level is not None else ""))
    return {"header": f"{hdr_id:04X}", "dims": [W, H],
            "tiles": len(pairs), "spawns": len(out_spawns),
            "prio_ported": prio_ported, "pal_anims": len(pal_anims)}


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
