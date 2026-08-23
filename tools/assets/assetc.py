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

CONVERTERS = {}   # role -> (extract_fn(cid, data) -> [(relpath, bytes)], compile_fn(files) -> bytes)

def register(role):
    def deco(cls):
        CONVERTERS[role] = cls
        return cls
    return deco

# ---------------------------------------------------------------- palette --
@register("palette")
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
@register("bg_tileset")
class BgTileset(TilesBase): subdir = "bg_tilesets"

# ---------------------------------------------------------------- tilemap --
@register("tilemap")
class Tilemap:
    """Array of u16 tile entries (verified by the renderer):
    bits 15..6 tile byte-offset (multiple of 64), bit4 hflip, bit5 vflip,
    low bits 0..3 pass through to collision/attr lookups. Stored as JSON
    words to stay diffable; exact odd tail preserved."""
    @staticmethod
    def extract(cid, data):
        n = len(data) // 2
        words = [data[i*2] | (data[i*2+1] << 8) for i in range(n)]
        js = {"format": "tilemap_u16", "chunk": f"{cid:04X}",
              "tail": data[n*2:].hex(), "words": words}
        return [(f"tilemaps/{cid:04X}.json", json.dumps(js, indent=0).encode())]
    @staticmethod
    def compile(files):
        js = json.loads(files[0][1])
        out = bytearray()
        for w in js["words"]:
            out += bytes((w & 0xFF, (w >> 8) & 0xFF))
        out += bytes.fromhex(js.get("tail", ""))
        return bytes(out)

# ---------------------------------------------------------------- roundtrip --
def roundtrip(role, cid):
    conv = converter_for(role)
    data = read_dec(cid)
    files = conv.extract(cid, data)
    back = conv.compile(files)
    ok = back == data
    return ok, files

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

def main():
    cmap = json.load(open(os.path.join(RAW, "chunk_map.json")))
    todo = []
    for cid_hex, info in cmap.items():
        role = info["roles"][0]
        converter_for(role)
        todo.append((role, int(cid_hex, 16)))
    passn = 0
    for role, cid in sorted(todo):
        ok, files = roundtrip(role, cid)
        if not ok:
            print(f"FAIL roundtrip {role} {cid:04X}")
            sys.exit(1)
        for rel, blob in files:
            p = os.path.join(ASSETS, rel)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            open(p, "wb").write(blob)
        passn += 1
    print(f"converted+verified: {passn} chunks "
          f"({len(set(r for r, _ in todo))} roles)")

if __name__ == "__main__":
    main()
