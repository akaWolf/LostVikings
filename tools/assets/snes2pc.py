#!/usr/bin/env python3
"""snes2pc.py — SNES DE exclusive levels -> the PC assets tree (task #104).

Everything below is verified against primary sources (the ROM itself and
our engine), not taken from the LostVikingsDE notes on faith:
  - chunk table @ROM 0x58000: 4B entries (addr u16, bank u16), SNES addr
    = (bank+0x8B):(addr+0x8000), LoROM file offset; 386 chunks, offsets
    monotonic (verified);
  - LZSS identical to our v2_lzss_decompress model, EXCEPT the 2-byte
    header stores the size itself (PC stores size-1) — verified: the
    header chunk of TLPT decodes to a valid stripe only without +1;
  - the level-header STRIPE GRAMMAR IS IDENTICAL to PC: our parse_stripe
    round-trips SNES stripes byte-exact; spawns of shared levels are
    byte-identical to PC; map TYPE bits (10-15) are byte-identical too;
  - the per-class config layout matches ours (sub_13e52 0x15-byte
    records) SEMANTICALLY (sprite ref FFFF/FFFE/chunk, w/h) but NOT
    byte-wise (different chunk ids / code offsets) — the DE notes
    overstate "identical VM";
  - KEY consequence: every TR33 spawn class exists in the PC Caves
    .lvs (01C2) with the same w/h — the port uses PC-native classes,
    banks and anim chunks (no SNES sprite conversion needed);
  - SNES tiles: 32B 4bpp planar (bp0/1 interleaved rows, then bp2/3);
    prefabs: PPU nametable words (tile 0-9, pal 10-12, prio 13, hf 14,
    vf 15); palettes: BGR555 words.

Conversion model (level -> a donor PC slot, same world):
  - tileset: each unique (tile, palette) pair bakes into one PC 64B tile
    with pixel = nibble ? nibble + pal*16 : pal*16 (backdrop slot; the
    PC engine blacks out colors 16k exactly like the SNES transparent-0
    convention shows the dark backdrop);
  - prefabs: PC draw words (pair<<6 | vf<<5 | hf<<4 | prio<<3); the
    SNES priority bit maps onto PC bit 3 (masked foreground overdraw,
    sub_1c8f1) with a generated tile-mask chunk (tileset id + 1):
    mask bit set iff the SNES nibble != 0 — same cover semantics;
  - map: unchanged (prefab numbering preserved; type bits already
    PC-semantics);
  - palettes: the level's SNES palette list composes a CGRAM image;
    colors 0..127 (BG) convert BGR555 -> VGA6 into one new palette
    chunk written over an 'unreferenced' archive id; sprite colors
    128+ come from the donor's own palette entries (PC sprites always
    draw at 0x80+ — proven by the 34DC codec flags model);
  - header: donor head (music/scroll fields) + TR33 dims and viking
    start; TR33 spawns and palette anims; donor banks/anim chunk
    sections (they cover all classes); SNES-only head refs @+0x39/+0x3D
    are reset to 0xFFFF like every PC level.

Usage:
  snes2pc.py convert 0x16C --donor 002A --scratch /tmp/lv_edit_scratch
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import assetc as AC  # noqa: E402
import level_render as LR  # noqa: E402

# The SNES DE ROM: LV_SNES_ROM=<path> (tools/assets/build_content.py sets it
# from --snes-rom or its search), else roms/LostVikingsDE.sfc under the repo
# root (roms/ is git-ignored — the console images are the user's).
ROM_PATH = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
                        "roms", "LostVikingsDE.sfc")
# BG palettes land in 'unreferenced' archive ids (>=384B each), one per
# converted level so several exclusives can coexist in one tree.
PAL_CHUNK_DEFAULT = 0x0155


class SnesRom:
    def __init__(self, path=None):
        path = path or os.environ.get("LV_SNES_ROM") or ROM_PATH
        with open(path, "rb") as f:
            self.rom = f.read()
        self.base = 0x58000
        self.n = (self.off(0) - self.base) // 4

    def off(self, i):
        o = self.base + i * 4
        a = self.rom[o] | (self.rom[o + 1] << 8)
        b = self.rom[o + 2] | (self.rom[o + 3] << 8)
        return (((b + 0x8B) & 0x7F) * 0x8000) + (((a + 0x8000) & 0xFFFF) - 0x8000)

    def chunk(self, i):
        o = self.off(i)
        end = self.off(i + 1) if i + 1 < self.n else len(self.rom)
        size = self.rom[o] | (self.rom[o + 1] << 8)   # SNES: size, not size-1
        if size == 0:
            size = 0x10000                            # the 64 KB frame sets (DE viking anim chunks 0x89..) wrap the u16
        return self._lzss(self.rom[o + 2:end], size)

    @staticmethod
    def _lzss(src, size):
        ring = bytearray(4096)
        bx = si = 0
        out = bytearray()
        while len(out) < size:
            flags = src[si]
            si += 1
            for _ in range(8):
                if len(out) >= size:
                    break
                if flags & 1:
                    v = src[si]
                    si += 1
                    ring[bx] = v
                    bx = (bx + 1) & 0xFFF
                    out.append(v)
                else:
                    ref = src[si] | (src[si + 1] << 8)
                    si += 2
                    ln = ((ref >> 12) & 0xF) + 3
                    o = ref & 0xFFF
                    for _ in range(ln):
                        v = ring[o]
                        ring[bx] = v
                        bx = (bx + 1) & 0xFFF
                        out.append(v)
                        o = (o + 1) & 0xFFF
                        if len(out) >= size:
                            break
                flags >>= 1
        return bytes(out)


def snes_tile_decode(t32):
    """32B 4bpp planar -> 64 pixel values 0-15.
    Rows: bytes r*2 = bp0, r*2+1 = bp1 (first 16B); +16 same for bp2/3."""
    px = bytearray(64)
    for y in range(8):
        b0, b1 = t32[y * 2], t32[y * 2 + 1]
        b2, b3 = t32[16 + y * 2], t32[16 + y * 2 + 1]
        for x in range(8):
            m = 0x80 >> x
            px[y * 8 + x] = ((1 if b0 & m else 0) | (2 if b1 & m else 0) |
                             (4 if b2 & m else 0) | (8 if b3 & m else 0))
    return bytes(px)


def bgr555_to_vga6(word):
    r = word & 31
    g = (word >> 5) & 31
    b = (word >> 10) & 31
    s = lambda c: (c * 63 + 15) // 31
    return s(r), s(g), s(b)


def compose_cgram(rom, pal_list):
    """Apply the stripe palette list into a 256-color BGR555 CGRAM image
    (2 bytes per color, start is a color index)."""
    cg = [0] * 256
    for e in pal_list:
        data = rom.chunk(e["chunk"])
        for i in range(len(data) // 2):
            c = e["start"] + i
            if c < 256:
                cg[c] = data[i * 2] | (data[i * 2 + 1] << 8)
    return cg


def convert_level(snes_hdr_id, donor_cid, scratch, pal_chunk=PAL_CHUNK_DEFAULT,
                  new_cids=None, next_level=None, music=None):
    """new_cids: dict(hdr, map, tiles, gtld, pal) of NEW archive ids —
    progression-integration mode (task #104): the donor slot stays
    untouched, the level lands in its own chunks (masks id = tiles+1 by
    the engine rule word_2AAC3+1) and its header carries next_level in
    the +0x16 field (the verified in-stripe progression pointer: the
    level-load decompression writes it straight into DS_LEVEL_LOAD)."""
    rom = SnesRom()
    tr = rom.chunk(snes_hdr_id)
    st = LR.parse_stripe(tr)
    assert LR.serialize_stripe(st) == tr, "SNES stripe grammar mismatch"

    def w16(buf, o):
        return buf[o] | (buf[o + 1] << 8)
    dims = (w16(tr, 0x29), w16(tr, 0x2B))
    smap_id, sts_id, sgt_id = w16(tr, 0x2E), w16(tr, 0x30), w16(tr, 0x32)
    smap, stset, sgt = rom.chunk(smap_id), rom.chunk(sts_id), rom.chunk(sgt_id)
    ntiles, nprefab = len(stset) // 32, len(sgt) // 8
    print(f"SNES {snes_hdr_id:04X}: {dims[0]}x{dims[1]}, {ntiles} tiles, "
          f"{nprefab} prefabs, {len(st['spawns'])} spawns")

    donor_raw = LR.header_raw(donor_cid, scratch)
    dst = LR.parse_stripe(donor_raw)
    dhead = bytearray(bytes.fromhex(dst["head"]))
    if new_cids:
        hdr_id = new_cids["hdr"]
        tm_id, ts_id, gt_id = new_cids["map"], new_cids["tiles"], new_cids["gtld"]
        pal_chunk = new_cids["pal"]
    else:
        hdr_id = int(donor_cid, 16)
        tm_id = w16(dhead, 0x2E)
        ts_id = w16(dhead, 0x30)
        gt_id = w16(dhead, 0x32)

    # sanity: every spawn class must exist in the donor world's .lvs with
    # the same dimensions as the SNES per-type config @ROM 0x010000
    script_id = LR.script_for_header(int(donor_cid, 16))
    script_raw = LR.open_payload(script_id, "lzss", scratch)
    for cls in sorted({sp["cls"] for sp in st["spawns"]}):
        rec = LR.class_record(script_raw, cls)
        srec = rom.rom[0x010000 + cls * 0x15:0x010000 + (cls + 1) * 0x15]
        assert rec, f"class {cls:02X} missing in PC {script_id:04X}"
        assert (rec["w"], rec["h"]) == (srec[9], srec[10]), \
            f"class {cls:02X} dims differ PC {rec['w']}x{rec['h']} vs " \
            f"SNES {srec[9]}x{srec[10]}"

    # ---- (tile, palette) pairs -> baked PC tiles ----
    # The SNES priority bit maps 1:1 onto the PC foreground model: bit 3 of
    # the draw word flags the cell for the masked overdraw pass (sub_1c8f1 /
    # v2_draw_flagged_tiles, runs AFTER sprites), and the tile-mask chunk
    # (engine rule: tileset id + 1, 8B/tile) says which pixels overdraw.
    # SNES: a priority BG pixel covers the sprite iff its nibble != 0 —
    # exactly the mask bit we emit. Flips only move the screen position on
    # PC (mask indexed unflipped), same as the PPU.
    pairs = []
    pair_idx = {}
    prio_ported = 0
    pc_gtld = bytearray()
    for p in range(nprefab):
        for k in range(4):
            v = w16(sgt, p * 8 + k * 2)
            t, pal = v & 0x3FF, (v >> 10) & 7
            hf, vf, prio = (v >> 14) & 1, (v >> 15) & 1, (v >> 13) & 1
            prio_ported += prio
            key = (t, pal)
            if key not in pair_idx:
                pair_idx[key] = len(pairs)
                pairs.append(key)
            pcv = (pair_idx[key] << 6) | (vf << 5) | (hf << 4) | (prio << 3)
            pc_gtld += bytes((pcv & 0xFF, pcv >> 8))
    # the draw word carries the tile offset in bits 6-15 (entry & 0xFFC0 =
    # index*64): 1024 indices, the full 64 KB tileset segment (engine buffers
    # V2_GS_TILEDATA_SIZE/V2_GS_SHADOW_SIZE = 0x10000; the LZSS lead u16 is
    # size-1 = 0xFFFF, the loop runs the count down through the wrap). The
    # SNES Factory table bakes to exactly 1024 pairs (UX stage 7).
    assert len(pairs) <= 1024, f"{len(pairs)} baked tiles > 10-bit offset"
    print(f"baked tiles: {len(pairs)}; priority bits ported to bit3: "
          f"{prio_ported}")

    pc_tiles = bytearray()
    pc_masks = bytearray()
    for (t, pal) in pairs:
        px = snes_tile_decode(stset[t * 32:(t + 1) * 32])
        baked = bytes((v + pal * 16) if v else (pal * 16) for v in px)
        pc_tiles += AC.tile_encode(baked)
        # mask: bit layout of v2_render_tile_masked — pixel (tx,ty) lives at
        # byte (tx&3)*2+(ty>>2), bit 7-((ty&3)*2+(tx>>2)); set iff nibble!=0
        m = bytearray(8)
        for ty in range(8):
            for tx in range(8):
                if px[ty * 8 + tx]:
                    m[(tx & 3) * 2 + (ty >> 2)] |= \
                        1 << (7 - ((ty & 3) * 2 + (tx >> 2)))
        pc_masks += m

    # ---- palettes: BG 0..127 from SNES, sprites 128+ from the donor ----
    cg = compose_cgram(rom, st["pal_list"])
    bg = bytearray()
    for c in range(128):
        r, g, b = bgr555_to_vga6(cg[c])
        bg += bytes((r, g, b))
    new_pal_list = [{"chunk": pal_chunk, "start": 0}]
    for e in dst["pal_list"]:
        if e["start"] >= 128:
            new_pal_list.append(dict(e))

    # ---- assemble the converted stripe over the donor head ----
    head = bytearray(dhead)
    # music block +0x04..+0x06 straight from the SNES head: +0x04 = the
    # dispatch action byte (0 = load+play; ds:0x25B7), +0x05 = the TRACK id
    # (ds:0x25B8 -> table @ds:0xA384 -> XMID chunk; the DE notes call this
    # field "World" — it doubles as both, one theme per world), +0x06 = the
    # exit-time action (ds:0x25B9). No level ever overrides these from
    # bytecode (checked: zero writes to 25B7/25B8 in all six .lvs), so the
    # header is the whole music story. `music` overrides the track id.
    head[0x04], head[0x05], head[0x06] = tr[0x04], tr[0x05], tr[0x06]
    if music is not None:
        head[0x05] = music & 0xFF
    for o in range(0x07, 0x12):          # viking start block (sel/X/Y/flags/arg)
        head[o] = tr[o]
    head[0x29:0x2D] = tr[0x29:0x2D]      # dims (+0x2D byte kept from SNES)
    if new_cids:
        # own chunk refs (the donor head carried the donor's)
        head[0x2E], head[0x2F] = tm_id & 0xFF, tm_id >> 8
        head[0x30], head[0x31] = ts_id & 0xFF, ts_id >> 8
        head[0x32], head[0x33] = gt_id & 0xFF, gt_id >> 8
    if next_level is not None:
        # +0x16 = next level (verified: the stripe decompresses over
        # DS 0x25B3 and this word lands on DS_LEVEL_LOAD 0x25C9)
        head[0x16], head[0x17] = next_level & 0xFF, next_level >> 8
    # SNES-only refs @+0x39/+0x3D -> 0xFFFF exactly like PC levels
    # UX stage 2/4: the donor heads carry the parallax pair refs now — a
    # converted head must NOT inherit them (+0x39/+0x3B/+0x3D -> FFFF: a
    # scene field carries its own baked plan B; an exclusive level gets
    # its own pair from parallax_snes right after). fx/fy stay the donor's.
    for o in (0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E):
        head[o] = 0xFF
    for o in (0x3F, 0x40):
        head[o] = donor_raw[o]
    out = dict(dst)
    out["head"] = bytes(head).hex()
    out["spawns"] = st["spawns"]
    out["pal_list"] = new_pal_list
    out["pal_anim_en"] = st["pal_anim_en"]
    out["pal_anims"] = st["pal_anims"]
    # banks / anim chunks / rest: donor's own (cover all classes)
    new_raw = LR.serialize_stripe(out)

    # ---- write into the scratch tree ----
    def wjson(path, obj):
        tmp = path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(obj, f, indent=1)
        os.replace(tmp, path)

    hdr_path = os.path.join(scratch, "level_headers", f"{hdr_id:04X}.json")
    hj = {"format": "level_header_stripe", "chunk": f"{hdr_id:04X}",
          "width": dims[0], "height": dims[1],
          "tilemap": f"{tm_id:04X}", "tileset": f"{ts_id:04X}",
          "bg_tileset": f"{gt_id:04X}", "raw": new_raw.hex()}
    wjson(hdr_path, hj)

    if new_cids:
        # register the fresh archive ids so assetc.pack picks them up
        ex_path = os.path.join(scratch, "extras.json")
        extras = {}
        if os.path.exists(ex_path):
            with open(ex_path) as f:
                extras = json.load(f)
        for cid2, role2 in ((hdr_id, "level_header_stripe"),
                            (tm_id, "tilemap"),
                            (ts_id, "tileset"),
                            (ts_id + 1, "tile_masks"),
                            (gt_id, "bg_tileset"),
                            (pal_chunk, "unreferenced")):
            extras[f"{cid2:04X}"] = {"role": role2}
        wjson(ex_path, extras)

    rows = []
    for y in range(dims[1]):
        row = []
        for x in range(dims[0]):
            wv = w16(smap, (y * dims[0] + x) * 2)
            row.append(f"{wv:04X}")
        rows.append(" ".join(row))
    wjson(os.path.join(scratch, "tilemaps", f"{tm_id:04X}.json"),
          {"format": "tilemap_u16", "chunk": f"{tm_id:04X}",
           "width": dims[0], "height": dims[1], "tail": "", "rows": rows})

    for role, cid, payload in (("tileset", ts_id, bytes(pc_tiles)),
                               ("tile_masks", ts_id + 1, bytes(pc_masks)),
                               ("bg_tileset", gt_id, bytes(pc_gtld))):
        for rel, blob in AC.converter_for(role).extract(cid, payload):
            path = os.path.join(scratch, rel)
            tmp = path + ".tmp"
            with open(tmp, "wb") as f:
                f.write(blob)
            os.replace(tmp, path)

    pal_path = os.path.join(scratch, "unreferenced", f"{pal_chunk:04X}.bin")
    tmp = pal_path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(bytes(bg))
    os.replace(tmp, pal_path)

    print(f"written into {scratch}: header {hdr_id:04X}, map {tm_id:04X}, "
          f"tiles {ts_id:04X} ({len(pc_tiles)}B), masks {ts_id+1:04X} "
          f"({len(pc_masks)}B), prefabs {gt_id:04X}, "
          f"BG palette -> {pal_chunk:04X}"
          + (f", next={next_level}" if next_level is not None else ""))
    return {"donor": donor_cid, "header": f"{hdr_id:04X}",
            "tilemap": f"{tm_id:04X}",
            "tileset": f"{ts_id:04X}", "prefabs": f"{gt_id:04X}",
            "pal_chunk": f"{pal_chunk:04X}", "tiles": len(pairs),
            "spawns": len(st["spawns"]), "prio_ported": prio_ported,
            "music": head[0x05], "dims": list(dims)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["convert"])
    ap.add_argument("snes_hdr", type=lambda v: int(v, 16))
    ap.add_argument("--donor", default="002A")
    ap.add_argument("--scratch", default="/tmp/lv_edit_scratch")
    ap.add_argument("--pal-chunk", type=lambda v: int(v, 16),
                    default=PAL_CHUNK_DEFAULT)
    args = ap.parse_args()
    convert_level(args.snes_hdr, args.donor.upper(), args.scratch,
                  args.pal_chunk)


if __name__ == "__main__":
    main()
