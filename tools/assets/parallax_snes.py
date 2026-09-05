#!/usr/bin/env python3
"""UX stage 2: the SNES DE parallax layer for every level (docs2/VERSIONS_DIFF_ANALYSIS
§6.3, memory snes-parallax-model).

Every level head — PC and SNES alike — carries the layer's fields: +0x34/+0x36 BG
map size in quads, +0x39 BG map chunk, +0x3B/+0x3D tile/quad-table chunks, +0x3F
and +0x41 the 8.8 scroll multipliers fx/fy (bit 15 = autoscroll at f/256 px per
console frame).  The PC heads keep the multipliers but hold 0xFFFF where the SNES
refers to its map/tiles (the DOS engine never reads them, DS 0x25EC..0x25F5).
This converter fills them: for each distinct (bgmap, quad table, tileset) triple
of the SNES DE ROM it writes two v2-private chunks

    map   [W u16][H u16][W*H u16]   W/H in 8x8 tiles; cell = tile index (0-9) |
                                    palette row (10-12) | priority (13, kept) |
                                    hflip (14) | vflip (15) — the SNES tilemap
                                    word with the index remapped into `tiles`
    tiles [n u16][n * 64 B]         8x8 pixels row-major, nibbles 0..15 (0 =
                                    transparent); the level palette rows 0-7 of
                                    the PC equal the SNES CGRAM, so the renderer
                                    paints row*16+nibble straight into the DAC

and points the heads of the PC levels 0..0x24 (= DE slots 0..0x24) and of the
five SNES exclusives (LVX slots 48..52 = DE slots 0x25..0x29) at them.  The
renderer (v2_draw_tiles) reads the fields back from the DS head copy.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import level_render as LR          # noqa: E402
import snes2pc as SP               # noqa: E402

BASE_CID = 0x270                   # map = BASE + 2k, tiles = BASE + 2k + 1
DE_LEVEL_TABLE = 0x7686            # ROM: 53 words = head ids of slots 0..0x34
LVX_HEADS = {48: 0x217, 49: 0x21D, 50: 0x223, 51: 0x229, 52: 0x22F}
# UX stage 4: the DOS intro/ending village levels ARE the SNES ones (same
# spawn tables, same scripts: PC 0x2B = DE 0x30 village, 0x2E = DE 0x33 the
# village return) — on the DOS the sky is black, the console draws the sky +
# mountains as the parallax pair 153 (fx/fy 0040 on both heads). PC 0x2C
# (abduction) takes NO pair: the SNES draws Tomator's ship hull as the BG
# layer (map 154) and moves it by fx/fy writes of its DC controller, the DOS
# draws the whole ship as the class-DE sprite object; PC 0x2F (the final
# stage 00DA) paints its backdrop in the level map itself (the SNES BG 083
# differs there — a dragon podium instead of the deck).
PC_STORY = {0x2B: 0x30, 0x2E: 0x33}


def le16(b, o):
    return b[o] | (b[o + 1] << 8)


def p16(b, o, v):
    b[o] = v & 0xFF
    b[o + 1] = (v >> 8) & 0xFF


def de_slot_of(slot):
    """Our level slot -> DE ROM slot (the same order; exclusives appended)."""
    if slot in PC_STORY:
        return PC_STORY[slot]
    if slot < 0x25:
        return slot
    return 0x25 + (slot - 48)


def exe_level_head(slot):
    """The EXE level table entry (level_tables() stops at 42 entries; the
    story levels 0x2A..0x2F sit right behind them)."""
    exe = os.path.join(LR.ROOT, "exe_static.bin")
    d = open(exe, "rb").read()
    o = LR._EXE_DS + LR._LVL_TBL + slot * 2
    return d[o] | (d[o + 1] << 8)


def pc_head_cid(slot):
    if slot in PC_STORY:
        return exe_level_head(slot)
    if slot < 0x25:
        return LR.level_tables()[slot][0]
    return LVX_HEADS[slot]


def ensure_head_json(scratch, cid, extras):
    """The story heads are not part of the extracted level tree — bring the
    archive payload in as a level_header_stripe extra (same json form as
    assetc's LevelHeader.extract) so the mod can carry the patched copy."""
    hp = os.path.join(scratch, "level_headers", f"{cid:04X}.json")
    if os.path.exists(hp):
        return hp
    import assetc as AC
    data, _ = AC.read_payload(cid, "lzss")
    files = AC.converter_for("level_header_stripe").extract(cid, data)
    js = json.loads(files[0][1])
    os.makedirs(os.path.dirname(hp), exist_ok=True)
    with open(hp + ".tmp", "w") as f:
        json.dump(js, f, indent=1)
    os.replace(hp + ".tmp", hp)
    extras[f"{cid:04X}"] = {"role": "level_header_stripe"}
    return hp


def de_head(R, de_slot):
    hid = le16(R.rom, DE_LEVEL_TABLE + de_slot * 2)
    h = R.chunk(hid)
    assert h[:4] == bytes.fromhex("0001f800"), (de_slot, hid)
    return hid, h


def expand_bg(R, h):
    """The BG map (+0x39, W x H quads, quad table +0x3D) -> 2W x 2H SNES tile
    words, exactly like validate_snes_render / the console's VRAM ring."""
    bw, bh = le16(h, 0x34), le16(h, 0x36)
    bgmap = R.chunk(le16(h, 0x39))
    gtid = le16(h, 0x3D)
    if gtid == 0xFFFF:                 # the story heads: the level's own quad table
        gtid = le16(h, 0x32)
    gt = R.chunk(gtid)
    assert len(bgmap) == bw * bh * 2, (len(bgmap), bw, bh)
    TW, TH = bw * 2, bh * 2
    words = [[0] * TW for _ in range(TH)]
    for qy in range(bh):
        for qx in range(bw):
            wv = le16(bgmap, (qy * bw + qx) * 2)
            e = gt[(wv & 0x3FF) * 8:(wv & 0x3FF) * 8 + 8]
            for (dy, dx, o) in ((0, 0, 0), (0, 1, 2), (1, 0, 4), (1, 1, 6)):
                words[qy * 2 + dy][qx * 2 + dx] = le16(e, o)
    return TW, TH, words


def convert_pair(R, h):
    """-> (map_chunk, tiles_chunk, stats) for one head's BG pair."""
    TW, TH, words = expand_bg(R, h)
    ts = R.chunk(le16(h, 0x30))          # the level's tileset (32 B 4bpp tiles)
    used = sorted({w & 0x3FF for row in words for w in row})
    remap = {t: i for i, t in enumerate(used)}
    tiles = bytearray()
    for t in used:
        t32 = ts[t * 32:t * 32 + 32]
        assert len(t32) == 32, (t, len(ts))
        tiles += SP.snes_tile_decode(t32)
    m = bytearray(4)
    p16(m, 0, TW)
    p16(m, 2, TH)
    prio = 0
    for row in words:
        for w in row:
            if w & 0x2000:
                prio += 1
            cell = remap[w & 0x3FF] | (w & 0xFC00)
            m += bytes((cell & 0xFF, cell >> 8))
    tc = bytearray()
    tc += bytes((len(used) & 0xFF, len(used) >> 8))
    tc += tiles
    return bytes(m), bytes(tc), dict(w=TW, h=TH, ntiles=len(used), prio=prio)


def integrate(scratch, slots=None):
    """Write the chunks + patch the heads in `scratch` (the mod tree)."""
    R = SP.SnesRom()
    ex_path = os.path.join(scratch, "extras.json")
    extras = json.load(open(ex_path)) if os.path.exists(ex_path) else {}
    d = os.path.join(scratch, "unreferenced")
    os.makedirs(d, exist_ok=True)
    if slots is None:
        slots = list(range(0x25)) + list(LVX_HEADS) + list(PC_STORY)
    pairs = {}                       # (bgmap, bggt, ts) -> (map cid, tiles cid)
    print("parallax layer (SNES DE BG pairs):")
    for slot in slots:
        de_slot = de_slot_of(slot)
        hid, h = de_head(R, de_slot)
        key = (le16(h, 0x39), le16(h, 0x3D) if le16(h, 0x3D) != 0xFFFF else le16(h, 0x32), le16(h, 0x30))
        if 0xFFFF in key:
            print(f"  slot {slot}: DE {de_slot:02X} has no BG pair, skipped")
            continue
        if key not in pairs:
            k = len(pairs)
            mc, tc = BASE_CID + 2 * k, BASE_CID + 2 * k + 1
            mbytes, tbytes, st = convert_pair(R, h)
            for cid, blob in ((mc, mbytes), (tc, tbytes)):
                p = os.path.join(d, f"{cid:04X}.bin")
                with open(p + ".tmp", "wb") as f:
                    f.write(blob)
                os.replace(p + ".tmp", p)
                extras[f"{cid:04X}"] = {"role": "unreferenced"}
            pairs[key] = (mc, tc)
            print(f"  pair {key[0]:03X}/{key[1]:03X}/{key[2]:03X}: map {mc:04X} "
                  f"{st['w']}x{st['h']} tiles, tileset {tc:04X} {st['ntiles']} tiles"
                  f"{', PRIORITY cells: %d' % st['prio'] if st['prio'] else ''}")
        mc, tc = pairs[key]
        # patch the PC head (scratch copy) — refs + the console's own fields
        cid = pc_head_cid(slot)
        hp = ensure_head_json(scratch, cid, extras)
        js = json.load(open(hp))
        raw = bytearray(bytes.fromhex(js["raw"]))
        fx_pc, fy_pc = le16(raw, 0x3F), le16(raw, 0x41)
        fx, fy = le16(h, 0x3F), le16(h, 0x41)
        if (slot < 0x25 or slot in PC_STORY) and (fx_pc, fy_pc) != (fx, fy):
            print(f"  slot {slot}: PC head {cid:04X} fx/fy {fx_pc:04X}/{fy_pc:04X} != DE {fx:04X}/{fy:04X}")
        p16(raw, 0x34, le16(h, 0x34))
        p16(raw, 0x36, le16(h, 0x36))
        p16(raw, 0x39, mc)
        p16(raw, 0x3B, tc)
        p16(raw, 0x3F, fx)
        p16(raw, 0x41, fy)
        js["raw"] = bytes(raw).hex()
        js["parallax"] = {"map": f"{mc:04X}", "tiles": f"{tc:04X}",
                          "fx": f"{fx:04X}", "fy": f"{fy:04X}"}
        with open(hp + ".tmp", "w") as f:
            json.dump(js, f, indent=1)
        os.replace(hp + ".tmp", hp)
    with open(ex_path + ".tmp", "w") as f:
        json.dump(extras, f, indent=1)
    os.replace(ex_path + ".tmp", ex_path)
    print(f"  {len(pairs)} BG pairs -> chunks {BASE_CID:04X}..{BASE_CID + 2 * len(pairs) - 1:04X}, "
          f"{len(slots)} heads patched")
    return pairs


if __name__ == "__main__":
    integrate(sys.argv[1] if len(sys.argv) > 1 else "/tmp/lv_edit_scratch")
