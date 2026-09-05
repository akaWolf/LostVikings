#!/usr/bin/env python3
"""genesis_scene.py — render a Genesis (SMD) inter-world scene EXACTLY the way
the Genesis VDP composes it, from ROM data only (UX plan stage 1).

Composition (all facts measured on the BAC Genesis save-states, see
docs2/VERSIONS_DIFF_ANALYSIS.md §7 and tools/assets/bac_state.py):
  plane B  = the world's parallax quad map (scene head +0x40; its size in
             quads at +0x3A/+0x3C: Preh 16x32, Egypt/Factory 8x8, Wacky
             16x16, Ship 16x8; wraps both ways) laid through the bg quad
             table (+0x44) on the world tileset (+0x36); scrolled by
             (bg_x, bg_y) = the camera times the parallax ratio of the head
             flags +0x46/+0x48 (see GenesisScene.parallax);
  plane A  = the scene room (33x33 quads) scrolled by the camera (cam_x,
             cam_y); pixel 0 of a tile is transparent (plane B shows);
  window   = the top WIN_ROWS tile rows (VDP reg 0x12 = 6): tile 0 of chunk
             0x07 (solid black, index 15) with tile 1 (the yellow line,
             index 9 on tile rows 5-6) on window row 5;
  backdrop = CRAM color 0.
Palette: the scene's CRAM (palette list of the stripe tail, chunk 0x18 for
Prehistoria; 64 x 9-bit BE words). Sprites (banner letters, vikings,
bubbles) are NOT part of the static composition.

The scene camera of every world is READ off the BAC save-state 30 s into
the scene (HScroll table + VSRAM, tools/assets/bac_state.py fit): the model
reproduces the console's plane A and plane B pixel for pixel there (0
mismatches; the only differing pixels are the speech bubble the game draws
into plane A). WORLD_CAMERA below carries those cameras. The DE walkthrough
video shows the same scenes with the camera one tile row (8 px) higher in
four worlds — a different recording of the same scene; the save-states are
the ground truth this port follows. (`fit <frame.png>` against a video frame
is kept for reference only.)
"""
import os
import sys
import struct
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import smd2pc as M  # noqa: E402

WIN_ROWS = 6          # VDP reg 0x12 = 6 -> window covers tile rows 0..5
WIN_LINE_ROW = 5      # the yellow line tile sits on window row 5
SCREEN_W, SCREEN_H = 320, 224

# world -> (scene chunk, palette chunk override or None)
SCENES = {"preh": 0x13D, "egypt": 0x13E, "factory": 0x13F,
          "wacky": 0x140, "ship": 0x141}


def be16(b, o):
    return (b[o] << 8) | b[o + 1]


def gen_rgb(word):
    """9-bit VDP color (BE word) -> 8-bit RGB (3-bit channels x 36 = 0..252)."""
    r = (word >> 1) & 7
    g = (word >> 5) & 7
    b = (word >> 9) & 7
    return (r * 36, g * 36, b * 36)


class GenesisScene:
    def __init__(self, world, rom=None):
        self.rom = rom or M.SmdRom()
        self.cid = SCENES[world]
        c = self.rom.chunk(self.cid)
        assert c[:4] == bytes.fromhex("010000f8"), "not an SMD level chunk"
        self.head = c
        self.qw, self.qh = be16(c, 0x2E), be16(c, 0x30)
        self.map = self.rom.chunk(be16(c, 0x34))
        self.tiles = self.rom.chunk(be16(c, 0x36))
        self.quads = self.rom.chunk(be16(c, 0x38))
        # parallax: +0x40 bg quad map, +0x44 its quad table (== level's)
        self.bg_map_id = be16(c, 0x40)
        self.bg_gt_id = be16(c, 0x44)
        self.bg_flags = (be16(c, 0x46), be16(c, 0x48))
        self.bg_map = self.rom.chunk(self.bg_map_id) if self.bg_map_id != 0xFFFF else b""
        self.bg_quads = self.rom.chunk(self.bg_gt_id) if self.bg_gt_id != 0xFFFF else self.quads
        # +0x3A/+0x3C: the parallax map size in quads (Preh 16x32, Egypt and
        # Factory 8x8, Wacky 16x16, Ship 16x8 — the console's plane B name
        # table repeats the 8x8 maps every 128 px both ways, measured on the
        # BAC states); the chunk holds exactly w*h words
        self.bg_w, self.bg_h = be16(c, 0x3A), be16(c, 0x3C)
        if self.bg_map:
            assert len(self.bg_map) == self.bg_w * self.bg_h * 2, \
                f"bg map {self.bg_map_id:03X}: {len(self.bg_map)} bytes != {self.bg_w}x{self.bg_h} quads"
        else:
            self.bg_w = self.bg_h = 0
        # spawns + tail (palette list etc.)
        self.spawns = []
        o = 0x4A
        while be16(c, o) != 0xFFFF:
            self.spawns.append(dict(x=be16(c, o), y=be16(c, o + 2),
                                    half_w=be16(c, o + 4), half_h=be16(c, o + 6),
                                    cls=be16(c, o + 8), anim=be16(c, o + 10),
                                    pool=be16(c, o + 12)))
            o += 14
        self.pal_list, self.pal_en, self.pal_anims, self.banks, self.achunks = \
            M.parse_tail(c, o + 2)
        cram = [0] * 64
        for e in self.pal_list:
            pd = self.rom.chunk(e["chunk"])
            for i in range(len(pd) // 2):
                if e["start"] + i < 64:
                    cram[e["start"] + i] = be16(pd, i * 2)
        self.cram = cram
        self.pal = [gen_rgb(w) for w in cram]
        # window tiles: chunk 0x07 (VRAM 0x7E4+ on the console)
        self.win_tiles = self.rom.chunk(0x07)
        self._tile_cache = {}

    # -- tiles ---------------------------------------------------------
    def tile_px(self, tiles, t):
        key = (id(tiles), t)
        px = self._tile_cache.get(key)
        if px is None:
            px = M.smd_tile_nibs(tiles, t) or [0] * 64
            self._tile_cache[key] = px
        return px

    def quad_cell(self, quads, tiles, qidx, x, y):
        """pixel (x,y in 0..15) of quad qidx -> (color index 0..15, pal row)"""
        k = (y // 8) * 2 + (x // 8)
        tw = be16(quads, qidx * 8 + k * 2)
        t = tw & 0x7FF
        hf, vf, prow = (tw >> 11) & 1, (tw >> 12) & 1, (tw >> 13) & 3
        px = self.tile_px(tiles, t)
        sx, sy = x % 8, y % 8
        if hf:
            sx = 7 - sx
        if vf:
            sy = 7 - sy
        return px[sy * 8 + sx], prow

    # -- planes --------------------------------------------------------
    def plane_a_pixel(self, wx, wy):
        """room pixel at world coords; None outside the room (transparent)."""
        qx, qy = wx // 16, wy // 16
        if not (0 <= qx < self.qw and 0 <= qy < self.qh):
            return None
        wv = be16(self.map, (qy * self.qw + qx) * 2)
        v, prow = self.quad_cell(self.quads, self.tiles, wv & 0x3FF, wx % 16, wy % 16)
        return None if v == 0 else self.pal[prow * 16 + v]

    def plane_b_pixel(self, bx, by):
        if not self.bg_h:
            return None
        bx %= self.bg_w * 16
        by %= self.bg_h * 16
        qx, qy = bx // 16, by // 16
        wv = be16(self.bg_map, (qy * self.bg_w + qx) * 2)
        v, prow = self.quad_cell(self.bg_quads, self.tiles, wv & 0x3FF, bx % 16, by % 16)
        return None if v == 0 else self.pal[prow * 16 + v]

    def window_pixel(self, x, y):
        """window plane rows 0..WIN_ROWS*8-1 (measured on the console VRAM:
        tile 0x7E4 = solid color index 15, tile 0x7E5 = index 15 with index 9
        on its tile rows 5-6, both on palette row 0 -> black band with one
        2-px yellow line at screen rows 45-46)."""
        if (y // 8) == WIN_LINE_ROW and (y % 8) in (5, 6):
            return self.pal[9]
        return self.pal[15]

    def _plane_images(self):
        if getattr(self, "_bg_img", None) is None:
            bw, bh = self.bg_w * 16, self.bg_h * 16
            self._bg_img = [[self.plane_b_pixel(x, y) for x in range(bw)] for y in range(bh)] if bh else None
            rw, rh = self.qw * 16, self.qh * 16
            self._room_img = [[self.plane_a_pixel(x, y) for x in range(rw)] for y in range(rh)]
        return self._bg_img, self._room_img

    def render(self, cam_x, cam_y, bg_x, bg_y, rows=SCREEN_H, with_window=True):
        """-> list of rows of RGB tuples, 320 x rows."""
        bg_img, room_img = self._plane_images()
        back = self.pal[0]
        bw = self.bg_w * 16 if bg_img else 0
        bh = self.bg_h * 16 if bg_img else 0
        rw, rh = self.qw * 16, self.qh * 16
        img = []
        for y in range(rows):
            brow = bg_img[(y + bg_y) % bh] if bg_img else None
            ry = y + cam_y
            rrow = room_img[ry] if 0 <= ry < rh else None
            win = with_window and y < WIN_ROWS * 8
            row = []
            for x in range(SCREEN_W):
                c = (brow[(x + bg_x) % bw] if brow else None) or back
                if win:
                    a = self.window_pixel(x, y)
                else:
                    rx = x + cam_x
                    a = rrow[rx] if (rrow is not None and 0 <= rx < rw) else None
                if a is not None:
                    c = a
                row.append(c)
            img.append(row)
        return img


    # -- index-level composition for the PC port ------------------------
    def render_indices(self, cam_x, cam_y, bg_x, bg_y, rows=SCREEN_H,
                       lower_line=187, with_bg=True):
        """Same composition as render() but as CRAM indices (0..63) and with
        plane B left OUT when with_bg is False (the port then draws it as a
        live parallax layer — the Starship starfield scrolls by itself) and
        the LOWER band the console draws by a raster split (HInt enabled in
        the scene, reg0 bit4): rows lower_line..+1 = the yellow line (index
        9), everything below = black (index 15) — measured on the DE video at
        Genesis resolution (lines at 45-46 and 187-188)."""
        bg_img, room_img = self._plane_index_images()
        bw = self.bg_w * 16 if bg_img else 0
        bh = self.bg_h * 16 if bg_img else 0
        rw, rh = self.qw * 16, self.qh * 16
        out = []
        for y in range(rows):
            if y >= lower_line:
                out.append(bytearray([9 if y < lower_line + 2 else 15] * SCREEN_W))
                continue
            brow = bg_img[(y + bg_y) % bh] if (bg_img and with_bg) else None
            ry = y + cam_y
            rrow = room_img[ry] if 0 <= ry < rh else None
            win = y < WIN_ROWS * 8
            row = bytearray(SCREEN_W)
            for x in range(SCREEN_W):
                c = (brow[(x + bg_x) % bw] if brow else None)
                c = 0 if c is None else c
                if win:
                    a = 9 if ((y // 8) == WIN_LINE_ROW and (y % 8) in (5, 6)) else 15
                else:
                    rx = x + cam_x
                    a = rrow[rx] if (rrow is not None and 0 <= rx < rw) else None
                row[x] = c if a is None else a
            out.append(row)
        return out

    def _plane_index_images(self):
        if getattr(self, "_bg_idx", None) is None:
            def a_idx(wx, wy):
                qx, qy = wx // 16, wy // 16
                if not (0 <= qx < self.qw and 0 <= qy < self.qh):
                    return None
                wv = be16(self.map, (qy * self.qw + qx) * 2)
                v, prow = self.quad_cell(self.quads, self.tiles, wv & 0x3FF, wx % 16, wy % 16)
                return None if v == 0 else prow * 16 + v

            def b_idx(bx, by):
                bx %= self.bg_w * 16
                by %= self.bg_h * 16
                qx, qy = bx // 16, by // 16
                wv = be16(self.bg_map, (qy * self.bg_w + qx) * 2)
                v, prow = self.quad_cell(self.bg_quads, self.tiles, wv & 0x3FF, bx % 16, by % 16)
                return None if v == 0 else prow * 16 + v
            bw, bh = self.bg_w * 16, self.bg_h * 16
            self._bg_idx = [[b_idx(x, y) for x in range(bw)] for y in range(bh)] if bh else None
            rw, rh = self.qw * 16, self.qh * 16
            self._room_idx = [[a_idx(x, y) for x in range(rw)] for y in range(rh)]
        return self._bg_idx, self._room_idx

    def parallax(self, cam_x, cam_y, auto_x=None):
        """plane B scroll (bg_x, bg_y) for the scene camera. Measured on the
        five BAC states (HScroll B == HScroll A * r, VSRAM[1] == VSRAM[0] * r,
        exactly): r = ((flags >> 6) & 7) / 4 per axis from the head flags
        +0x46 (x) / +0x48 (y) — 0x0040 = 1/4 (Preh), 0x0080 = 1/2 (Wacky),
        0x00C0 = 3/4 (Egypt, Factory), 0x0100 = 1 (Ship y). Flag bit 15 on x
        (Ship: 0x8500) = the plane's x is driven by the scene itself (the
        starfield), not by the camera: the caller passes the measured value
        (auto_x); without one the ratio still applies."""
        fx, fy = self.bg_flags
        bx = (cam_x * ((fx >> 6) & 7)) // 4
        by = (cam_y * ((fy >> 6) & 7)) // 4
        if (fx & 0x8000) and auto_x is not None:
            bx = auto_x
        if self.bg_w:
            bx %= self.bg_w * 16
            by %= self.bg_h * 16
        return bx, by

    def type_bits(self, cam_x, cam_y, qx, qy):
        """type bits (map word >> 10) of the room cell under screen quad
        (qx, qy) — the cell covering the quad's center; 0 outside the room."""
        wx, wy = qx * 16 + 8 + cam_x, qy * 16 + 8 + cam_y
        rx, ry = wx // 16, wy // 16
        if not (0 <= rx < self.qw and 0 <= ry < self.qh):
            return 0
        return be16(self.map, (ry * self.qw + rx) * 2) >> 10


# Per-world scene camera = the plane A scroll READ off the BAC Genesis
# save-state 30 s into the scene (HScroll table word 0, VSRAM word 0;
# tools/assets/bac_state.py fit — the model matches the console's planes
# pixel for pixel at these values; all four scroll values are multiples of
# 8, the camera is static for the whole scene). `bg_x` (Ship only): the
# starfield's own x at that moment (head flag 0x8500 = self-driven x).
# Viking spots (screen px: x center, y = top edge of the platform they stand
# on): where the state's viking sprites stand (sprite bottom + 1 lands on a
# floor-typed room row) and, for the viking that has already walked off by
# then, the platform the DE video shows it on.
# The SMD scene itself spawns the trio OFF-SCREEN on that platform (the
# room's spawn rows: Preh screen x -68, Egypt -32, Wacky/Ship -64; Factory
# Baleog/Olaf -32 and Erik +360 on the right) and walks them in one by one.
# The PC port spawns the trio from THOSE ROWS (smd2pc.build_genesis_backdrop:
# classes 1/0/2 = Erik/Baleog/Olaf, screen = room - camera, the row's own
# y) through the engine's mode-2 head recipe (sub_11446 -> sub_11569: three
# vikings from a DS position table, code_seg order 1/0/2, class 1 = Erik =
# object slot 0; dormant in the DOS build, its table is zero) fed per scene
# by the LVX4 record, and drives them with the scene's own Genesis input
# recording (integrate_snes.smd_recording). sub_10813 would clear the input
# words while the ACTIVE viking is outside vp_x-12 .. vp_x+332 (the
# camera-catch-up rule) — the LVX_NOGATE flag lifts that gate on the scene
# slots (the SMD engine has no such gate: docs2/GENESIS_ROM_INTERNALS.md).
# `spots`/`walk` (screen px) serve the DE-field path only. Engine landing
# types 1/2/4/5/0x20 and slopes >= 0x30 (sub_16260); the Wacky candy floor
# (0x13 — no ground on the PC, walked on the real Genesis) is translated to
# plain ground on the scene map (smd2pc GENESIS_FLOOR_TYPES); the Preh bottom
# (0x10) is the world's lethal substance on both platforms and stays so.
# The map carries EXT_L quad columns of the room LEFT of the screen (the
# off-screen part of the platform, real room cells) so the leftmost viking
# stands on real geometry — see layout().
# `banner` = the world-name letter blocks as the console's sprite table has
# them (32x32 sprites, tile 1217+, x0 + 32*k at y0, palette row prow).
EXT_L = 5   # quad columns of room kept left of the screen (80 px): the trio
            # waits there out of sight (screen x -12, body -28..4) and walks
            # in one by one like the SMD scene's own spawn rows
WORLD_CAMERA = {
    # Erik on the left ledge (room row 15, screen x -4..59 — 64 px: two
    # vikings on screen, the third off-screen left on the same ledge),
    # Olaf on the right shelf (row 17); the bottom grass (row 18) is 0x10
    # The room's 4A row (the SMD bubble geyser) is dropped: on the PC 4A is
    # the per-level spawner, invisible, and its periodic spawns only fill
    # the 20-slot object table (the scene held exactly 20 objects with it;
    # a failed spawn on the level-end frame drained stale slots and re-fired
    # the level-end flag — level 4 jumped to 5, seen live). Since the E0/E1
    # ports the table holds D8 + 8 letters + 2 spots + 48 + trio + 4A = 16.
    "preh":    dict(cam=(132, 120), spots=[(28, 120), (156, 168), (296, 152)], walk=(0, 56),
                    banner=dict(x0=32, y0=11, prow=3),
                    # The bubbles are the room's own: the SMD geyser 4A row
                    # (192,344 = screen 60,224, under the ledge edge) runs its
                    # scene lane cycle (pool 0x46..0x4E, one bubble per 20
                    # ticks: big 0x25 / medium 0x24 / small 0x23) and the
                    # bubbles push a touching viking upward (VEL_Y += -1 per
                    # tick) — Olaf steps off the ledge onto one and rides it
                    # to the right shelf on the real Genesis. All four classes
                    # are the SMD bytecode (integrate_snes.bubble_blobs); the
                    # frames come from the SMD sprite chunks 0C0/0BF/0BE
                    # (32x32/16x16/8x8, 3 frames each: bubble, wobble, pop)
                    # shipped as pool units behind the letters (`decor`).
                    decor=dict(sprites=[(0x0C0, 3, 32), (0x0BF, 3, 16), (0x0BE, 3, 8)])),
    # the left ledge, room row 17 (x -80..159); the pit with the spikes lies
    # at x 160..223
    # SMD class 0A (128,56, half 256x48) has NO PC counterpart in the #110
    # bijection (no Egypt level uses it): identity-mapped it is a PC 0A
    # that spawns a patrolling hazard (seen live: x 96..160 / y 83..120,
    # knocks a standing viking back, kills Erik mid-jump) — nothing of the
    # kind happens on the DE clip. Left out until the SMD class is
    # identified (sprite match against the PC Egypt classes).
    "egypt":   dict(cam=(80, 144), spots=[(40, 128), (76, 128), (128, 128)], drop_cls=[0x0A], walk=(24, 140),
                    banner=dict(x0=80, y0=12, prow=2)),
    # Olaf + Baleog on the left beam (row 17: flat x 48..79, slopes 0x35/0x34
    # at x 16..47), Erik on its right part (x 208..255) — the video shows him
    # climbing the right ladder later. Head at 64: 64 beam, 32 slope, 0 drops
    # onto the lower-left bricks (row 18, y 112)
    "factory": dict(cam=(112, 176), spots=[(232, 96), (76, 96), (57, 96)], walk=(52, 76),
                    banner=dict(x0=50, y0=10, prow=2)),
    # the candy floor, room row 23: x -144..79 type 01 (ground), 80..319 type
    # 0x13 (not ground on the PC) — the trio and the stroll stay left of 80
    "wacky":   dict(cam=(144, 216), spots=[(64, 152), (208, 152), (26, 152)], walk=(24, 72),
                    banner=dict(x0=80, y0=10, prow=2)),
    # the left console platform, row 21 (x -144..63) — the trio crowds on it
    # bg_x0: plane B x at the FIRST visible frame of the scene on the console
    # (Mednafen 60 fps recording of the attract scene: phase 207 at frame
    # first+51, 222 at first+54 = +5 px/frame, i.e. 208 at the first frame;
    # the head's +0x46 = 0x8500 drives the plane by itself, 68k 0x1402 ->
    # 0x44F0 adds 5.0 px per frame to $1720). The live-layer port starts at
    # this phase (map trailer off_x); bg_x=242 = the state's value 30 s in,
    # used only by the static composite of the other worlds' check.
    "ship":    dict(cam=(144, 208), bg_x=242, bg_x0=208,
                    spots=[(40, 128), (56, 128), (20, 128)], walk=(16, 48),
                    banner=dict(x0=32, y0=10, prow=2)),
}


def layout(world, gen_bg=None):
    """The PC scene map geometry for a world (single source of truth for
    smd2pc.build_genesis_backdrop / convert_scene and the LVX4 flags in
    integrate_snes): the map holds EXT_L quad columns of room left of the
    screen plus the 320x200 screen, padded so the room quads stay 16-aligned
    with their type bits; the engine parks the viewport at (pin_x, pin_y)
    (LVX4 flags bits 4-11 / 12-15, v2_lvx_pin_camera) — screen pixel (sx,
    sy) is map pixel (sx + pin_x, sy + pin_y)."""
    wc = WORLD_CAMERA[world]
    cam_x, cam_y = (gen_bg or {}).get("cam") or wc["cam"]
    pin_x = EXT_L * 16 + cam_x % 16
    pin_y = cam_y % 16
    cw = EXT_L + 20 + (1 if cam_x % 16 else 0)
    ch = (200 + pin_y + 15) // 16
    # the room's props that sit past the screen (the Preh bubble geyser 4A
    # is parked 24 px below the bottom edge) need real map cells under
    # them: grow the map to their boxes (never shown — the viewport is
    # parked at the pin); vikings/controller/letters/actor spots excluded
    for sp in GenesisScene(world).spawns:
        if sp["cls"] in (0, 1, 2, 0x48, 0xE0, 0xE1):
            continue
        x, y = sp["x"] - cam_x + pin_x, sp["y"] - cam_y + pin_y
        cw = max(cw, (x + sp["half_w"]) // 16 + 1)
        ch = max(ch, (y + sp["half_h"]) // 16 + 1)
    # the trio's own rows too: Factory Erik starts PAST the right edge (room
    # x 472 = screen 360) and needs the room's floor cells under him
    for sp in GenesisScene(world).spawns:
        if sp["cls"] in (0, 1, 2):
            x, y = sp["x"] - cam_x + pin_x, sp["y"] - cam_y + pin_y
            cw = max(cw, (x + 16) // 16 + 1)
            ch = max(ch, (y + 16) // 16 + 1)
    assert pin_x < 256 and pin_y < 16
    return dict(cam=(cam_x, cam_y), pin=(pin_x, pin_y), cw=cw, ch=ch)


def png_write(path, img):
    H, W = len(img), len(img[0])

    def ch(t, x):
        c = struct.pack('>I', len(x)) + t + x
        return c + struct.pack('>I', zlib.crc32(t + x) & 0xFFFFFFFF)
    raw = b''.join(b'\x00' + bytes(v for p in row for v in p) for row in img)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0))
                           + ch(b'IDAT', zlib.compress(raw)) + ch(b'IEND', b''))


def read_png(path):
    d = open(path, "rb").read()
    pos, idat, W, H, ct = 8, b"", 0, 0, 2
    while pos < len(d):
        ln = struct.unpack(">I", d[pos:pos + 4])[0]
        t = d[pos + 4:pos + 8]
        body = d[pos + 8:pos + 8 + ln]
        if t == b"IHDR":
            W, H, _, ct = struct.unpack(">IIBB", body[:10])
        elif t == b"IDAT":
            idat += body
        elif t == b"IEND":
            break
        pos += 12 + ln
    raw = zlib.decompress(idat)
    bpp = 4 if ct == 6 else 3
    stride = W * bpp
    rows, prev, p = [], bytearray(stride), 0
    for _ in range(H):
        f = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        rows.append(bytes(line))
        prev = line
    return W, H, bpp, rows


def video_to_genesis(frame_png, x0=242.0, y0=70.0, scale=794.0 / 320.0):
    """Box-resample the video's Genesis area to 320x224 RGB (geometry given
    for a 1280-wide frame; scaled by the actual frame width)."""
    W, H, bpp, rows = read_png(frame_png)
    k = W / 1280.0
    x0, y0, scale = x0 * k, y0 * k, scale * k
    out = []
    for gy in range(SCREEN_H):
        vy0, vy1 = y0 + gy * scale, y0 + (gy + 1) * scale
        ys = range(max(0, int(vy0)), min(H, int(vy1) + 1))
        row = []
        for gx in range(SCREEN_W):
            vx0, vx1 = x0 + gx * scale, x0 + (gx + 1) * scale
            xs = range(max(0, int(vx0)), min(W, int(vx1) + 1))
            acc = [0, 0, 0]
            n = 0
            for vy in ys:
                r = rows[vy]
                for vx in xs:
                    o = vx * bpp
                    acc[0] += r[o]
                    acc[1] += r[o + 1]
                    acc[2] += r[o + 2]
                    n += 1
            row.append(tuple(a // max(1, n) for a in acc))
        out.append(row)
    return out


def sad(a, b, y0, y1, step=3):
    s = 0
    for y in range(y0, y1, step):
        ra, rb = a[y], b[y]
        for x in range(0, SCREEN_W, step):
            pa, pb = ra[x], rb[x]
            s += abs(pa[0] - pb[0]) + abs(pa[1] - pb[1]) + abs(pa[2] - pb[2])
    return s


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    cmd = sys.argv[1]
    world = sys.argv[2] if len(sys.argv) > 2 and sys.argv[2] in SCENES else "preh"
    sc = GenesisScene(world)
    print(f"{world}: room {sc.cid:03X} {sc.qw}x{sc.qh}, bg map {sc.bg_map_id:03X} "
          f"{sc.bg_w}x{sc.bg_h}, bg quads {sc.bg_gt_id:03X}, bg flags {sc.bg_flags}, "
          f"palette {[(hex(e['chunk']), e['start']) for e in sc.pal_list]}")
    if cmd == "render":
        cam_x, cam_y, bg_x, bg_y = (int(v) for v in sys.argv[3:7])
        img = sc.render(cam_x, cam_y, bg_x, bg_y)
        out = sys.argv[7] if len(sys.argv) > 7 else f"/tmp/genesis_scene_{world}.png"
        png_write(out, img)
        print("wrote", out)
    elif cmd == "fit":
        frame = sys.argv[3]
        ref = video_to_genesis(frame)
        png_write(f"/tmp/genesis_ref_{world}.png", ref)
        # plane B fit first (rows below the window, coarse): bg_x from the
        # HScroll table (-33 -> 33) is a strong prior; search bg_y and cam_y.
        best = None
        for cam_y in range(40, 141, 2):
            for bg_y in range(-40, 101, 4):
                img = sc.render(132, cam_y, 33, bg_y, rows=200, with_window=True)
                s = sad(img, ref, 48, 200, 4)
                if best is None or s < best[0]:
                    best = (s, cam_y, bg_y)
        print("coarse best (sad, cam_y, bg_y):", best, flush=True)
        s0, cy0, by0 = best
        best = None
        for cam_y in range(cy0 - 2, cy0 + 3):
            for bg_y in range(by0 - 4, by0 + 5):
                for cam_x in (130, 131, 132, 133, 134):
                    img = sc.render(cam_x, cam_y, 33, bg_y, with_window=True)
                    s = sad(img, ref, 48, 200, 2)
                    if best is None or s < best[0]:
                        best = (s, cam_x, cam_y, bg_y)
        print("fine best (sad, cam_x, cam_y, bg_y):", best)
        _, cx, cy, by = best
        img = sc.render(cx, cy, 33, by)
        png_write(f"/tmp/genesis_scene_{world}_fit.png", img)
        # side by side
        sbs = [img[y] + [(64, 64, 64)] * 4 + ref[y] for y in range(SCREEN_H)]
        png_write(f"/tmp/genesis_scene_{world}_vs_video.png", sbs)
        print("wrote /tmp/genesis_scene_%s_fit.png and _vs_video.png" % world)


if __name__ == "__main__":
    main()
