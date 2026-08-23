#!/usr/bin/env python3
"""Stage 5.1: asset converter framework.

Direction A (extract): chunks/dec/NNNN.bin -> assets/<role>/<name>.<ext>
Direction B (compile): assets/... -> bin; MUST equal chunks/dec/NNNN.bin
byte-for-byte (the wave's judge; a converter without a green round-trip
does not land).

Converters register per role; ids come from assets_raw/chunk_map.json.
"""
import json, os, sys, hashlib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RAW = os.path.join(ROOT, "assets_raw")
ASSETS = os.path.join(ROOT, "assets")

def dec_path(cid): return os.path.join(RAW, "chunks", "dec", f"{cid:04d}.bin")
def read_dec(cid): return open(dec_path(cid), "rb").read()

def comp_path(cid): return os.path.join(RAW, "chunks", "comp", f"{cid:04d}.bin")

def read_payload(cid, container):
    """The engine-visible bytes of a chunk. LZSS records: the decompressor
    output (dec/). RAW records (sub_10cd8 family — screens): the archive
    stores [u16 plane_size][4*plane_size bytes] UNCOMPRESSED; dec/ holds
    LZSS garbage for those — read the true bytes from comp/."""
    if container == "raw":
        c = open(comp_path(cid), "rb").read()
        ps = c[0] | (c[1] << 8)
        return c[2:2 + ps*4], ps
    d = read_dec(cid)
    return d, None

CONVERTERS = {}   # role -> (extract_fn(cid, data) -> [(relpath, bytes)], compile_fn(files) -> bytes)

def register(role):
    def deco(cls):
        CONVERTERS[role] = cls
        return cls
    return deco

# ---------------------------------------------------------------- palette --
@register("palette")
@register("palette16_by_sig")
class Palette:
    """768 bytes, 256 x RGB, 6-bit components (0..63) — verified layout
    (v2 DAC pipeline reads exactly these triplets)."""
    @staticmethod
    def extract(cid, data):
        n = len(data) // 3
        pal = [[data[i*3], data[i*3+1], data[i*3+2]] for i in range(n)]
        js = {"format": "vga_pal_6bit", "chunk": f"{cid:04X}",
              "tail": data[n*3:].hex(),   # exact remainder, if any
              "colors": pal}
        return [(f"palettes/{cid:04X}.pal.json",
                 json.dumps(js, indent=0).encode())]
    @staticmethod
    def compile(files):
        js = json.loads(files[0][1])
        out = bytearray()
        for c in js["colors"]:
            out += bytes(c)
        out += bytes.fromhex(js.get("tail", ""))
        return bytes(out)

# ------------------------------------------------------------------- png ----
import zlib, struct

def png_write(width, height, idx_pixels, palette256):
    """8-bit indexed PNG. idx_pixels: bytes len=w*h. palette: 256*[r,g,b] 0..255."""
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 3, 0, 0, 0)
    plte = b"".join(bytes(c) for c in palette256)
    raw = b"".join(b"\x00" + idx_pixels[y*width:(y+1)*width] for y in range(height))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"PLTE", plte)
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))

def png_read_indexed(blob):
    """Reads back the exact index bytes of an 8-bit indexed PNG we wrote."""
    assert blob[:8] == b"\x89PNG\r\n\x1a\n"
    pos = 8; w = h = None; idat = b""
    while pos < len(blob):
        ln = struct.unpack(">I", blob[pos:pos+4])[0]
        tag = blob[pos+4:pos+8]; data = blob[pos+8:pos+8+ln]
        if tag == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", data[:10])
            assert depth == 8 and ctype == 3
        elif tag == b"IDAT": idat += data
        pos += 12 + ln
    raw = zlib.decompress(idat)
    out = bytearray()
    for y in range(h):
        row = raw[y*(w+1):(y+1)*(w+1)]
        assert row[0] == 0, "only filter 0 written"
        out += row[1:]
    return w, h, bytes(out)

GRAY_PAL = [[i, i, i] for i in range(256)]

# ---------------------------------------------------------------- tileset --
def tile_decode(t64):
    """64B tile -> 8x8 index bytes. pixel(x,y) = t[(x&3)*16 + y*2 + (x>>2)]
    (verified: v2_draw_tile mirror of sub_1689e)."""
    px = bytearray(64)
    for y in range(8):
        for x in range(8):
            px[y*8+x] = t64[(x & 3)*16 + y*2 + (x >> 2)]
    return bytes(px)

def tile_encode(px):
    t = bytearray(64)
    for y in range(8):
        for x in range(8):
            t[(x & 3)*16 + y*2 + (x >> 2)] = px[y*8+x]
    return bytes(t)

class TilesBase:
    """Chunk = dense array of 64B tiles (+ exact tail if not multiple of 64).
    Atlas: 16 tiles per row, 8x8 each -> width 128."""
    subdir = "tilesets"
    @classmethod
    def extract(cls, cid, data):
        n = len(data) // 64
        tail = data[n*64:]
        cols = 16
        rows = (n + cols - 1) // cols
        img = bytearray(cols*8 * rows*8)
        for i in range(n):
            px = tile_decode(data[i*64:(i+1)*64])
            bx = (i % cols)*8; by = (i // cols)*8
            for y in range(8):
                img[(by+y)*cols*8 + bx : (by+y)*cols*8 + bx + 8] = px[y*8:(y+1)*8]
        meta = {"chunk": f"{cid:04X}", "tiles": n, "tail": tail.hex(),
                "format": "tiles8x8_modex_byteplane"}
        return [(f"{cls.subdir}/{cid:04X}.png",
                 png_write(cols*8, rows*8, bytes(img), GRAY_PAL)),
                (f"{cls.subdir}/{cid:04X}.json",
                 json.dumps(meta, indent=0).encode())]
    @classmethod
    def compile(cls, files):
        png = next(b for r, b in files if r.endswith(".png"))
        meta = json.loads(next(b for r, b in files if r.endswith(".json")))
        w, h, img = png_read_indexed(png)
        cols = w // 8
        out = bytearray()
        for i in range(meta["tiles"]):
            bx = (i % cols)*8; by = (i // cols)*8
            px = bytearray(64)
            for y in range(8):
                px[y*8:(y+1)*8] = img[(by+y)*w + bx : (by+y)*w + bx + 8]
            out += tile_encode(px)
        out += bytes.fromhex(meta.get("tail", ""))
        return bytes(out)

@register("tileset")
class Tileset(TilesBase): subdir = "tilesets"

@register("tile_masks")
class TileMasks:
    """8 bytes per tile — per-pixel transparency bits of the masked-tile
    pass (v2_render_tile_masked): bit(tx,ty) lives at byte (tx&3)*2+(ty>>2),
    bit 7-((ty&3)*2+(tx>>2)). Atlas mirrors the tileset layout (16/row);
    index 1 = opaque pixel."""
    @staticmethod
    def extract(cid, data):
        n = len(data) // 8
        tail = data[n*8:]
        cols = 16
        rows = (n + cols - 1) // cols
        img = bytearray(cols*8 * rows*8)
        for i in range(n):
            m = data[i*8:(i+1)*8]
            bx = (i % cols)*8; by = (i // cols)*8
            for ty in range(8):
                for tx in range(8):
                    mb = (tx & 3)*2 + (ty >> 2)
                    mbit = 7 - ((ty & 3)*2 + (tx >> 2))
                    img[(by+ty)*cols*8 + bx+tx] = 1 if (m[mb] >> mbit) & 1 else 0
        meta = {"chunk": f"{cid:04X}", "tiles": n, "tail": tail.hex(),
                "format": "tile_masks_1bit"}
        pal = [[0,0,0]] + [[255,255,255]] + [[i,i,i] for i in range(2,256)]
        return [(f"tile_masks/{cid:04X}.png",
                 png_write(cols*8, rows*8, bytes(img), pal)),
                (f"tile_masks/{cid:04X}.json",
                 json.dumps(meta, indent=0).encode())]
    @staticmethod
    def compile(files):
        png = next(b for r, b in files if r.endswith(".png"))
        meta = json.loads(next(b for r, b in files if r.endswith(".json")))
        w, h, img = png_read_indexed(png)
        cols = w // 8
        out = bytearray()
        for i in range(meta["tiles"]):
            bx = (i % cols)*8; by = (i // cols)*8
            m = bytearray(8)
            for ty in range(8):
                for tx in range(8):
                    if img[(by+ty)*w + bx+tx]:
                        mb = (tx & 3)*2 + (ty >> 2)
                        mbit = 7 - ((ty & 3)*2 + (tx >> 2))
                        m[mb] |= (1 << mbit)
            out += m
        out += bytes.fromhex(meta.get("tail", ""))
        return bytes(out)
@register("bg_tileset")
class BgTileset(TilesBase): subdir = "bg_tilesets"

# ---------------------------------------------------------------- tilemap --
def _cmap():
    global _CMAP_CACHE
    try: return _CMAP_CACHE
    except NameError:
        _CMAP_CACHE = json.load(open(os.path.join(RAW, "chunk_map.json")))
        return _CMAP_CACHE

@register("tilemap")
class Tilemap:
    """Array of u16 tile entries (verified by the renderer):
    bits 15..6 tile byte-offset (multiple of 64), bit4 hflip, bit5 vflip,
    low bits pass to collision/attr lookups. Rows of `width` words (width
    from the level header stripe via chunk_map) — diffable per map row;
    exact odd tail preserved."""
    @staticmethod
    def extract(cid, data):
        n = len(data) // 2
        words = [data[i*2] | (data[i*2+1] << 8) for i in range(n)]
        info = _cmap().get(f"{cid:04X}", {})
        width = info.get("width", 0)
        js = {"format": "tilemap_u16", "chunk": f"{cid:04X}",
              "width": width, "height": info.get("height", 0),
              "tail": data[n*2:].hex()}
        if width:
            js["rows"] = [" ".join(f"{w:04X}" for w in words[r*width:(r+1)*width])
                          for r in range((n + width - 1) // width)]
        else:
            js["rows"] = [" ".join(f"{w:04X}" for w in words)]
        return [(f"tilemaps/{cid:04X}.json", json.dumps(js, indent=1).encode())]
    @staticmethod
    def compile(files):
        js = json.loads(files[0][1])
        out = bytearray()
        for row in js["rows"]:
            for tok in row.split():
                w = int(tok, 16)
                out += bytes((w & 0xFF, (w >> 8) & 0xFF))
        out += bytes.fromhex(js.get("tail", ""))
        return bytes(out)

@register("level_header_stripe")
class LevelHeader:
    """The per-level DS stripe @25B3. Verified fields decoded (width/
    height @+0x29/+0x2B, resource chunk ids @+0x2E..+0x32); the rest is
    kept as exact hex until each field earns a verified name."""
    @staticmethod
    def extract(cid, data):
        def w16(o): return data[o] | (data[o+1] << 8)
        js = {"format": "level_header_stripe", "chunk": f"{cid:04X}",
              "width": w16(0x29), "height": w16(0x2B),
              "tilemap": f"{w16(0x2E):04X}", "tileset": f"{w16(0x30):04X}",
              "bg_tileset": f"{w16(0x32):04X}",
              "raw": data.hex()}
        return [(f"level_headers/{cid:04X}.json", json.dumps(js, indent=1).encode())]
    @staticmethod
    def compile(files):
        js = json.loads(files[0][1])
        raw = bytearray(bytes.fromhex(js["raw"]))
        # named fields override the raw image (hand edits win)
        def p16(o, v): raw[o] = v & 0xFF; raw[o+1] = (v >> 8) & 0xFF
        p16(0x29, js["width"]); p16(0x2B, js["height"])
        p16(0x2E, int(js["tilemap"], 16)); p16(0x30, int(js["tileset"], 16))
        p16(0x32, int(js["bg_tileset"], 16))
        return bytes(raw)

# ---------------------------------------------------------------- roundtrip --
def roundtrip(role, cid, container="lzss"):
    conv = converter_for(role)
    data, _ps = read_payload(cid, container)
    files = conv.extract(cid, data)
    back = conv.compile(files)
    ok = back == data
    return ok, files

# ---------------------------------------------------------------- screens --
@register("screen_gfx")
@register("screen_hud_only")
@register("screen_by_sig")
@register("screen_intro2")
class Screen:
    """Raw chunk: u16 plane_size + 4 planes x plane_size bytes.
    VGA copy (v2_load_chunk_10cd8 mirror of seg000 0xD50..0xD8A):
    plane p byte i -> pixel x=(i%86)*4+p, y=i/86 — 344px-wide image."""
    PITCH = 86
    @classmethod
    def extract(cls, cid, data):
        ps = len(data) // 4
        body = data[:ps*4]
        tail = data[ps*4:]
        h = ps // cls.PITCH
        rem = ps - h*cls.PITCH   # partial last row bytes per plane
        w = cls.PITCH * 4
        img = bytearray(w * (h + (1 if rem else 0)))
        for p in range(4):
            pl = body[p*ps:(p+1)*ps]
            for i in range(ps):
                x = (i % cls.PITCH)*4 + p
                y = i // cls.PITCH
                img[y*w + x] = pl[i]
        meta = {"chunk": f"{cid:04X}", "plane_size": ps, "tail": tail.hex(),
                "format": "vga_planar_344"}
        out_h = h + (1 if rem else 0)
        return [(f"screens/{cid:04X}.png", png_write(w, out_h, bytes(img), GRAY_PAL)),
                (f"screens/{cid:04X}.json", json.dumps(meta, indent=0).encode())]
    @classmethod
    def compile(cls, files):
        png = next(b for r, b in files if r.endswith(".png"))
        meta = json.loads(next(b for r, b in files if r.endswith(".json")))
        w, hh, img = png_read_indexed(png)
        ps = meta["plane_size"]
        out = bytearray(ps*4)
        for p in range(4):
            for i in range(ps):
                x = (i % cls.PITCH)*4 + p
                y = i // cls.PITCH
                out[p*ps + i] = img[y*w + x]
        out += bytes.fromhex(meta.get("tail", ""))
        return bytes(out)

# ---------------------------------------------------------------- xmid ------
def xmid_to_midi(data):
    """Derived-view converter (NOT part of the round-trip): XMID EVNT ->
    SMF type 0. XMID facts: intervals = bytes <0x80 summed; note-on carries
    a VLQ duration instead of a paired note-off; PPQN model: 60 ticks/quarter
    at 120 BPM = 120 ticks/sec (Miles default the AIL driver clocks at)."""
    i = data.find(b"EVNT")
    if i < 0: return None
    ln = int.from_bytes(data[i+4:i+8], "big")
    ev = data[i+8:i+8+ln]
    pos = 0; t = 0
    events = []   # (time, order, midi-bytes)
    order = 0
    while pos < len(ev):
        b = ev[pos]
        if b < 0x80:
            t += b; pos += 1; continue
        if b == 0xFF:
            meta = ev[pos+1]
            l = ev[pos+2]
            events.append((t, order, ev[pos:pos+3+l])); order += 1
            if meta == 0x2F: break
            pos += 3 + l; continue
        st = b & 0xF0
        if st == 0x90:
            note, vel = ev[pos+1], ev[pos+2]
            pos += 3
            dur = 0
            while True:
                c = ev[pos]; dur = (dur << 7) | (c & 0x7F); pos += 1
                if not (c & 0x80): break
            events.append((t, order, bytes((b, note, vel)))); order += 1
            events.append((t + dur, order, bytes((0x80 | (b & 0x0F), note, 0x40)))); order += 1
        elif st in (0xC0, 0xD0):
            events.append((t, order, ev[pos:pos+2])); order += 1; pos += 2
        else:
            events.append((t, order, ev[pos:pos+3])); order += 1; pos += 3
    events.sort(key=lambda e: (e[0], e[1]))
    def vlq(n):
        out = [n & 0x7F]; n >>= 7
        while n: out.append(0x80 | (n & 0x7F)); n >>= 7
        return bytes(reversed(out))
    trk = bytearray()
    last = 0
    for tt, _, eb in events:
        trk += vlq(tt - last) + eb
        last = tt
    if not trk.endswith(b"\xff\x2f\x00"):
        trk += vlq(0) + b"\xff\x2f\x00"
    hdr = b"MThd" + (6).to_bytes(4, "big") + (0).to_bytes(2, "big") \
        + (1).to_bytes(2, "big") + (60).to_bytes(2, "big")
    return hdr + b"MTrk" + len(trk).to_bytes(4, "big") + bytes(trk)

@register("xmid_track")
@register("xmid_extra")
class XmidTrack:
    """Canonical asset = byte-exact .xmi; .mid is a derived listen/DAW view
    regenerated on every extract (never read back)."""
    @staticmethod
    def extract(cid, data):
        files = [(f"music/{cid:04X}.xmi", data)]
        mid = xmid_to_midi(data)
        if mid: files.append((f"music/{cid:04X}.derived.mid", mid))
        return files
    @staticmethod
    def compile(files):
        return next(b for r, b in files if r.endswith(".xmi"))

# ---------------------------------------------------------------- lvs -------
@register("level_script")
class LevelScript:
    """5.3: the level/VM bytecode keeps its Stage-2 DSL as the open form.
    Canonical text = assets_raw/lvs/<ID>.lvsf (generated by lvs_full.py,
    proven IDENTICAL by the lvsc compiler for every level chunk); compile
    goes through lvsc so the round-trip judge holds on the payload."""
    @staticmethod
    def extract(cid, data):
        src = os.path.join(RAW, "lvs", f"{cid:X}.lvsf")
        if not os.path.exists(src):
            return [(f"level_scripts/{cid:04X}.bin", data)]   # no DSL yet: exact bytes
        return [(f"level_scripts/{cid:X}.lvsf", open(src, "rb").read()),
                (f"level_scripts/{cid:04X}.size.json",
                 json.dumps({"payload_len": len(data)}).encode())]
    @staticmethod
    def compile(files):
        lvsf = [b for r, b in files if r.endswith(".lvsf")]
        if not lvsf:
            return files[0][1]
        import subprocess, tempfile
        meta = json.loads(next(b for r, b in files if r.endswith(".size.json")))
        rel = next(r for r, b in files if r.endswith(".lvsf"))
        cid = rel.split("/")[-1].split(".")[0]
        with tempfile.NamedTemporaryFile(suffix=".lvsf", delete=False) as tf:
            tf.write(lvsf[0]); tmp = tf.name
        try:
            out = subprocess.run(
                [sys.executable, os.path.join(ROOT, "tools/data/lvsc.py"),
                 "build", tmp, cid, "--emit-bin"],
                capture_output=True, cwd=ROOT)
            binp = tmp + ".bin"
            if os.path.exists(binp):
                data = open(binp, "rb").read()
                os.unlink(binp)
                return data[:meta["payload_len"]]
        finally:
            os.unlink(tmp)
        raise RuntimeError("lvsc compile failed")

# ---------------------------------------------------------------- sprites ---
def sprite_decode(data, off, typ, strips):
    """One sprite frame -> (w, h, idx bytes, 255 = masked-out). Unified
    strip model (all three seg003 renderers): 4 sections (plane 0..3) x
    N strips x (1 mask + bpr*rps data); pixel(x=j*4+plane,
    y=strip*rps+r) = data[r*bpr+j]; mask bit 7-(r*bpr+j) gates the WRITE
    (colour 0 paints when the bit is set)."""
    if typ == 1: nstr, rps, bpr = 2, 4, 2
    elif typ == 4: nstr, rps, bpr = 8, 2, 4
    else: nstr, rps, bpr = strips, 1, 8
    w = bpr*4; h = nstr*rps
    img = bytearray([255]) * (w*h)
    img = bytearray([255]*(w*h))
    ptr = off - 1          # off is 1-based to the first data byte; mask at off-1
    for plane in range(4):
        for st in range(nstr):
            if ptr < 0 or ptr + 1 + bpr*rps > len(data): return None
            mask = data[ptr]; d = data[ptr+1:ptr+1+bpr*rps]
            for r in range(rps):
                for j in range(bpr):
                    if (mask >> (7 - (r*bpr + j))) & 1:
                        img[(st*rps + r)*w + j*4 + plane] = d[r*bpr + j]
            ptr += 1 + bpr*rps
    return w, h, bytes(img)

SPRITE_PAL = GRAY_PAL[:255] + [[255, 0, 255]]   # 255 = masked-out (magenta)

def sprite_bank_derived(cid, data):
    """Derived contact-sheet PNG for a sprite bank (usage map from the
    replay corpus trace: assets_raw/sprite_map.json). NOT read back — the
    canonical asset stays the byte-exact .bin until the lvs anim model
    provides the authoritative frame table."""
    import math
    smap = _smap()
    frames = smap.get(f"{cid:04X}")
    if not frames: return None
    cells = []
    for f in frames:
        r = sprite_decode(data, f["off"], f["type"], f["strips"])
        if r: cells.append((f, r))
    if not cells: return None
    cw = max(r[0] for _, r in cells)
    ch = max(r[1] for _, r in cells)
    cols = max(1, min(16, int(math.ceil(len(cells) ** 0.5))))
    rows = (len(cells) + cols - 1) // cols
    W, H = cols*(cw+1), rows*(ch+1)
    img = bytearray([255]) * 0
    img = bytearray([254]*(W*H))       # 254 = grid background
    for i, (f, (w, h, px)) in enumerate(cells):
        bx = (i % cols)*(cw+1); by = (i // cols)*(ch+1)
        for y in range(h):
            img[(by+y)*W + bx : (by+y)*W + bx + w] = px[y*w:(y+1)*w]
    pal = SPRITE_PAL[:254] + [[32, 32, 48]] + [[255, 0, 255]]
    return png_write(W, H, bytes(img), pal)

def _smap():
    global _SMAP_CACHE
    try: return _SMAP_CACHE
    except NameError:
        import json as _j
        pth = os.path.join(RAW, "sprite_map.json")
        _SMAP_CACHE = _j.load(open(pth)) if os.path.exists(pth) else {}
        return _SMAP_CACHE

@register("sprite_gfx")
class SpriteBank:
    """Canonical = byte-exact .bin; .derived.png contact sheet from the
    dynamic usage map (regenerated on extract, never read back)."""
    @staticmethod
    def extract(cid, data):
        files = [(f"sprite_banks/{cid:04X}.bin", data)]
        png = sprite_bank_derived(cid, data)
        if png: files.append((f"sprite_banks/{cid:04X}.derived.png", png))
        return files
    @staticmethod
    def compile(files):
        return next(b for r, b in files if r.endswith(".bin"))

# ------------------------------------------------------------- bin fallback --
class BinPassthrough:
    """Roles whose deep format lands in a later wave keep byte-exact .bin
    in the open tree (sprite banks slice via anim scripts; screens/XMID/
    text get dedicated converters next)."""
    def __init__(self, subdir): self.subdir = subdir
    def extract(self, cid, data):
        return [(f"{self.subdir}/{cid:04X}.bin", data)]
    def compile(self, files):
        return files[0][1]

BIN_ROLES = {
    "sprite_gfx": "sprite_banks", "screen_gfx": "screens",
    "collision_masks": "collision_masks", "level_header_stripe": "level_headers",
    "transition_text": "texts", "level_script": "level_scripts",
    "sound_driver_or_bank": "sound", "builtin_palette": "palettes_builtin",
    "level_password_table": "tables", "level_palette_chunks": "palettes_builtin",
    "hud_item_gfx": "hud", "unreferenced": "unreferenced",
}
for role, sub in BIN_ROLES.items():
    if role not in CONVERTERS:
        CONVERTERS[role] = BinPassthrough(sub)

def converter_for(role):
    """Exact role, else its [param]-stripped base, else a misc slug —
    every chunk lands in the open tree no matter how roles evolve."""
    if role in CONVERTERS: return CONVERTERS[role]
    base = role.split('[')[0]
    if base in CONVERTERS: return CONVERTERS[base]
    for known, sub in (("sound_driver", "sound_drivers"),
                       ("xmid_track", "music"),
                       ("sound_bank", "sound_banks"),
                       ("screen_", "screens")):
        if base.startswith(known):
            CONVERTERS[role] = BinPassthrough(sub)
            return CONVERTERS[role]
    CONVERTERS[role] = BinPassthrough("misc/" + base)
    return CONVERTERS[role]

def lzss_store(payload: bytes) -> bytes:
    """All-literal LZSS stream (flag 0xFF per 8 literals) — a valid stream
    for the engine decompressor; used when an open asset was hand-edited
    (the archive stream then no longer applies)."""
    out = bytearray()
    for i in range(0, len(payload), 8):
        out.append(0xFF)
        out += payload[i:i+8]
    return bytes(out)

def pack():
    """5.2: build the engine-facing tree assets/.compiled/NNNN.bin =
    [8B table header][the chunk's COMP block exactly as the archive stores
    it]. The engine then runs its own decompressor over it — every side
    effect (the FS ring scratch, the comp bytes at FS+0x1000, the header
    mirror) is identical BY CONSTRUCTION. compile() over the open files is
    the judge: if it equals the archive payload, the ORIGINAL comp block is
    reused (bit-exact A/B); a hand-edited asset gets a store-mode stream."""
    cmap = json.load(open(os.path.join(RAW, "chunk_map.json")))
    man = json.load(open(os.path.join(RAW, "manifest.json")))
    entries = {e["id"]: e for e in man["entries"]}
    outdir = os.path.join(ASSETS, ".compiled")
    os.makedirs(outdir, exist_ok=True)
    import struct as _st
    for cid_hex, info in sorted(cmap.items()):
        cid = int(cid_hex, 16)
        role = info["roles"][0]
        container = info.get("container", "lzss")
        conv = converter_for(role)
        data, ps = read_payload(cid, container)
        files = conv.extract(cid, data)
        # read back from the OPEN tree if present (assets edited by hand win)
        loaded = []
        for rel, blob in files:
            pth = os.path.join(ASSETS, rel)
            loaded.append((rel, open(pth, "rb").read() if os.path.exists(pth) else blob))
        payload = conv.compile(loaded)
        e = entries[cid]
        hdr = _st.pack("<II", e["offset"], e["offset"] + e["comp_size"])
        orig_comp = open(comp_path(cid), "rb").read()
        if container == "raw":
            ps_now = len(payload) // 4
            comp_block = _st.pack("<H", ps_now) + payload
            if payload == orig_comp[2:2 + (orig_comp[0] | (orig_comp[1] << 8)) * 4]:
                comp_block = orig_comp   # keep the exact archive block (incl. tail pad)
        else:
            if payload == read_dec(cid):
                comp_block = orig_comp   # untouched asset: the original stream
            else:
                comp_block = _st.pack("<H", (len(payload) - 1) & 0xFFFF) + lzss_store(payload)
        open(os.path.join(outdir, f"{cid:04d}.bin"), "wb").write(hdr + comp_block)
    print(f"packed: {len(cmap)} -> {outdir}")

def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--pack":
        pack(); return
    cmap = json.load(open(os.path.join(RAW, "chunk_map.json")))
    todo = []
    for cid_hex, info in cmap.items():
        role = info["roles"][0]
        converter_for(role)
        todo.append((role, int(cid_hex, 16), info.get("container", "lzss")))
    passn = 0
    for role, cid, container in sorted(todo):
        ok, files = roundtrip(role, cid, container)
        if not ok:
            print(f"FAIL roundtrip {role} {cid:04X}")
            sys.exit(1)
        for rel, blob in files:
            p = os.path.join(ASSETS, rel)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            open(p, "wb").write(blob)
        passn += 1
    print(f"converted+verified: {passn} chunks "
          f"({len(set(r for r, _cid, _c in todo))} roles)")

if __name__ == "__main__":
    main()
