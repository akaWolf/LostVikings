#!/usr/bin/env python3
"""BAC (Blizzard Arcade Collection) Genesis save-state reader: the EMUS v3
blobs inside lv_cinematics.rpl are complete Genesis machine states; this
module decodes their VDP part and renders the frame the console showed —
the GROUND TRUTH for the 'Definitive Edition' inter-world scenes, which BAC
plays through its Genesis core (docs2/VERSIONS_DIFF_ANALYSIS.md §7).

Layout (measured on the states, see project memory project_de_interlude_absent):
  0x0A6  VDP registers, u32 LE x24 (low byte = the register)
  0x126  CRAM, u16 LE x64 (----BBB-GGG-RRR-); entry 0 = the backdrop
  0x1A6  VSRAM, u16 LE x40 ([0] = plane A, [1] = plane B; measured on the
         five scene states: [1] == [0] * parallax ratio, exactly)
  0x226  VRAM, 64 KB (68k byte order)
Registers used: 2 plane A base, 3 window base, 4 plane B base, 5 sprite
table, 11 scroll modes, 13 HScroll table, 16 plane size, 17/18 window.

Commands:
  python3 bac_state.py info  <state.bin>
  python3 bac_state.py render <state.bin> <out.png>          # planes + sprites
  python3 bac_state.py fit <world> <state.bin> [out_prefix]  # exact camera of
      genesis_scene.GenesisScene against the state (plane A + plane B), prints
      cam/bg and the sprite list (the vikings' spots), writes state/model/diff
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from genesis_scene import (GenesisScene, gen_rgb, png_write, SCREEN_W,  # noqa: E402
                           SCREEN_H, WIN_ROWS)

REGS_OFF, CRAM_OFF, VSRAM_OFF, VRAM_OFF = 0xA6, 0x126, 0x1A6, 0x226


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


class VdpState:
    def __init__(self, path):
        d = open(path, "rb").read()
        assert len(d) >= VRAM_OFF + 0x10000, "not an EMUS Genesis state"
        self.path = path
        self.regs = [struct.unpack_from("<I", d, REGS_OFF + 4 * i)[0] & 0xFF for i in range(24)]
        self.cram = [struct.unpack_from("<H", d, CRAM_OFF + 2 * i)[0] for i in range(64)]
        self.vsram = [struct.unpack_from("<H", d, VSRAM_OFF + 2 * i)[0] for i in range(40)]
        self.vram = d[VRAM_OFF:VRAM_OFF + 0x10000]
        r = self.regs
        self.nt_a = (r[2] & 0x38) << 10
        self.nt_w = (r[3] & 0x3E) << 10
        self.nt_b = (r[4] & 7) << 13
        self.sat = (r[5] & 0x7F) << 9
        self.hs_tab = (r[13] & 0x3F) << 10
        sz = {0: 32, 1: 64, 3: 128}
        self.pw, self.ph = sz[r[16] & 3], sz[(r[16] >> 4) & 3]
        self.pal = [gen_rgb(w) for w in self.cram]
        self._tiles = {}

    # -- raw --------------------------------------------------------------
    def be16(self, off):
        return struct.unpack_from(">H", self.vram, off)[0]

    def tile(self, t):
        px = self._tiles.get(t)
        if px is None:
            base = (t * 32) & 0xFFFF
            px = []
            for y in range(8):
                for x in range(8):
                    b = self.vram[(base + y * 4 + (x >> 1)) & 0xFFFF]
                    px.append(b >> 4 if (x & 1) == 0 else b & 15)
            self._tiles[t] = px
        return px

    def hscroll(self, line):
        mode = self.regs[11] & 3
        off = self.hs_tab + (0 if mode == 0 else (line >> 3) * 32 if mode == 2 else line * 4)
        return s16(self.be16(off)), s16(self.be16(off + 2))

    def vscroll(self, col):
        if self.regs[11] & 4:
            i = 2 * (col >> 4)
            return s16(self.vsram[i]), s16(self.vsram[i + 1])
        return s16(self.vsram[0]), s16(self.vsram[1])

    def in_window(self, x, y):
        whp, wvp = self.regs[17], self.regs[18]
        h = (whp & 0x1F) != 0 and ((x >= (whp & 0x1F) * 16) if whp & 0x80 else (x < (whp & 0x1F) * 16))
        v = (wvp & 0x1F) != 0 and ((y >= (wvp & 0x1F) * 8) if wvp & 0x80 else (y < (wvp & 0x1F) * 8))
        return h or v

    def nt_pixel(self, base, width, col, row, x, y):
        """-> (cram index or None, priority) of name-table cell (col,row) pixel (x,y)."""
        e = self.be16(base + (row * width + col) * 2)
        t, hf, vf, prow, pri = e & 0x7FF, (e >> 11) & 1, (e >> 12) & 1, (e >> 13) & 3, e >> 15
        if hf:
            x = 7 - x
        if vf:
            y = 7 - y
        v = self.tile(t)[y * 8 + x]
        return (None if v == 0 else prow * 16 + v), pri

    # -- planes as the screen sees them ----------------------------------
    def screen_plane(self, which):
        """which: 'A' (window applied) or 'B' -> rows x 320 of (idx|None, pri)."""
        out = []
        base = self.nt_a if which == "A" else self.nt_b
        for sy in range(SCREEN_H):
            hsa, hsb = self.hscroll(sy)
            hs = hsa if which == "A" else hsb
            row = []
            for sx in range(SCREEN_W):
                if which == "A" and self.in_window(sx, sy):
                    row.append(self.nt_pixel(self.nt_w, 64, sx >> 3, sy >> 3, sx & 7, sy & 7))
                    continue
                vsa, vsb = self.vscroll(sx)
                vs = vsa if which == "A" else vsb
                px = (sx - hs) % (self.pw * 8)
                py = (sy + vs) % (self.ph * 8)
                row.append(self.nt_pixel(base, self.pw, px >> 3, py >> 3, px & 7, py & 7))
            out.append(row)
        return out

    # -- sprites ----------------------------------------------------------
    def sprites(self):
        """link-ordered sprite list: dict(x, y, w, h, tile, prow, hf, vf, pri)."""
        out, i, seen = [], 0, set()
        while i not in seen and len(out) < 80:
            seen.add(i)
            o = self.sat + i * 8
            w0, b2, b3, attr, w3 = (self.be16(o), self.vram[o + 2], self.vram[o + 3],
                                     self.be16(o + 4), self.be16(o + 6))
            out.append(dict(idx=i, x=(w3 & 0x1FF) - 128, y=(w0 & 0x3FF) - 128,
                            w=((b2 >> 2) & 3) + 1, h=(b2 & 3) + 1, tile=attr & 0x7FF,
                            prow=(attr >> 13) & 3, hf=(attr >> 11) & 1, vf=(attr >> 12) & 1,
                            pri=attr >> 15))
            i = b3 & 0x7F
            if i == 0:
                break
        return out

    def sprite_layer(self):
        """320 x 224 of (idx|None, pri); the first sprite in link order wins."""
        lay = [[None] * SCREEN_W for _ in range(SCREEN_H)]
        for sp in self.sprites():
            for cx in range(sp["w"]):
                for cy in range(sp["h"]):
                    tcx = sp["w"] - 1 - cx if sp["hf"] else cx
                    tcy = sp["h"] - 1 - cy if sp["vf"] else cy
                    t = (sp["tile"] + tcx * sp["h"] + tcy) & 0x7FF
                    px = self.tile(t)
                    for y in range(8):
                        sy = sp["y"] + cy * 8 + y
                        if not (0 <= sy < SCREEN_H):
                            continue
                        for x in range(8):
                            sx = sp["x"] + cx * 8 + x
                            if not (0 <= sx < SCREEN_W) or lay[sy][sx] is not None:
                                continue
                            v = px[(7 - y if sp["vf"] else y) * 8 + (7 - x if sp["hf"] else x)]
                            if v:
                                lay[sy][sx] = (sp["prow"] * 16 + v, sp["pri"])
        return lay

    # -- the frame ----------------------------------------------------------
    def render(self, with_sprites=True):
        A, B = self.screen_plane("A"), self.screen_plane("B")
        S = self.sprite_layer() if with_sprites else None
        back = self.pal[0]
        img = []
        for y in range(SCREEN_H):
            row = []
            for x in range(SCREEN_W):
                c = 0
                for lay, want in ((B, 0), (A, 0), (S, 0), (B, 1), (A, 1), (S, 1)):
                    if lay is None:
                        continue
                    p = lay[y][x]
                    if p is not None and p[0] is not None and p[1] == want:
                        c = p[0]
                row.append(self.pal[c] if c else back)
            img.append(row)
        return img


def fit(world, state_path, out_prefix=None):
    st = VdpState(state_path)
    sc = GenesisScene(world)
    hsA, hsB = st.hscroll(100)
    vsA, vsB = st.vscroll(0)
    print(f"{world}: state {os.path.basename(state_path)} hsA={hsA} hsB={hsB} vsA={vsA} vsB={vsB} "
          f"planes {st.pw}x{st.ph} cells, window rows {st.regs[18] & 0x1F}, bg flags {sc.bg_flags}")
    A = st.screen_plane("A")
    B = st.screen_plane("B")
    bg_idx, room_idx = sc._plane_index_images()
    rw, rh = sc.qw * 16, sc.qh * 16
    Y0, Y1 = WIN_ROWS * 8, 187

    def score_a(cx, cy, step):
        bad = 0
        for sy in range(Y0, Y1, step):
            ry = sy + cy
            rrow = room_idx[ry] if 0 <= ry < rh else None
            arow = A[sy]
            for sx in range(0, SCREEN_W, step):
                rx = sx + cx
                m = rrow[rx] if (rrow is not None and 0 <= rx < rw) else None
                if m != arow[sx][0]:
                    bad += 1
        return bad

    # ring-buffered name table: the room->plane offset is tile aligned, so
    # cam_x == -hsA (mod 8) and cam_y == vsA (mod 8)
    best = None
    for cx in range((-hsA) % 8 - 320, rw, 8):
        for cy in range(vsA % 8 - 224, rh, 8):
            b = score_a(cx, cy, 8)
            if best is None or b < best[0]:
                best = (b, cx, cy)
    _, cam_x, cam_y = best
    full = score_a(cam_x, cam_y, 1)
    print(f"  plane A: cam=({cam_x}, {cam_y}) sample mismatches {best[0]}, "
          f"full-res mismatches {full} of {(Y1 - Y0) * SCREEN_W}")

    bw, bh = sc.bg_w * 16, sc.bg_h * 16
    bgfit = None
    if bg_idx:
        def score_b(bx, by, step):
            bad = 0
            for sy in range(Y0, Y1, step):
                brow = bg_idx[(sy + by) % bh]
                srow = B[sy]
                for sx in range(0, SCREEN_W, step):
                    if brow[(sx + bx) % bw] != srow[sx][0]:
                        bad += 1
            return bad
        bb = None
        for bx in range((-hsB) % 8, bw, 8):
            for by in range(vsB % 8, bh, 8):
                b = score_b(bx, by, 4)
                if bb is None or b < bb[0]:
                    bb = (b, bx, by)
        bgfit = (bb[1], bb[2])
        print(f"  plane B: bg=({bb[1]}, {bb[2]}) sample mismatches {bb[0]}, "
              f"full-res mismatches {score_b(bb[1], bb[2], 1)}; "
              f"ratio check cam*flags: x {cam_x * (sc.bg_flags[0] >> 6 & 3) / 4:.2f} "
              f"y {cam_y * (sc.bg_flags[1] >> 6 & 3) / 4:.2f}")
        # rows where plane B carries anything at all (the 64-px bands)
        rows_b = [sy for sy in range(SCREEN_H) if any(p[0] is not None for p in B[sy])]
        print(f"  plane B content rows: {rows_b[0] if rows_b else None}..{rows_b[-1] if rows_b else None}")
    print("  sprites (link order):")
    for sp in st.sprites():
        print(f"    #{sp['idx']:2d} x={sp['x']:4d} y={sp['y']:4d} {sp['w']}x{sp['h']} tile {sp['tile']:4d} "
              f"pal {sp['prow']} hf {sp['hf']} vf {sp['vf']} pri {sp['pri']}")
    if out_prefix:
        state_img = st.render()
        bx, by = bgfit if bgfit else (0, 0)
        model = sc.render(cam_x, cam_y, bx, by, rows=SCREEN_H)
        diff = []
        for y in range(SCREEN_H):
            diff.append([(255, 0, 0) if state_img[y][x] != model[y][x] else model[y][x]
                         for x in range(SCREEN_W)])
        png_write(out_prefix + "_state.png", state_img)
        png_write(out_prefix + "_model.png", model)
        sbs = [state_img[y] + [(64, 64, 64)] * 4 + model[y] + [(64, 64, 64)] * 4 + diff[y]
               for y in range(SCREEN_H)]
        png_write(out_prefix + "_sbs.png", sbs)
        print("  wrote", out_prefix + "_{state,model,sbs}.png")
    return dict(cam=(cam_x, cam_y), bg=bgfit, sprites=st.sprites())


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return
    cmd = sys.argv[1]
    if cmd == "info":
        st = VdpState(sys.argv[2])
        print("regs:", " ".join("%02X" % r for r in st.regs))
        print(f"ntA {st.nt_a:04X} ntW {st.nt_w:04X} ntB {st.nt_b:04X} SAT {st.sat:04X} HS {st.hs_tab:04X} "
              f"planes {st.pw}x{st.ph}")
        print("hscroll(0):", st.hscroll(0), "vscroll(0):", st.vscroll(0))
        print("cram:", " ".join("%04X" % w for w in st.cram))
        for sp in st.sprites():
            print(f"  sprite #{sp['idx']:2d} x={sp['x']:4d} y={sp['y']:4d} {sp['w']}x{sp['h']} "
                  f"tile {sp['tile']:4d} pal {sp['prow']} pri {sp['pri']}")
    elif cmd == "render":
        st = VdpState(sys.argv[2])
        png_write(sys.argv[3], st.render())
        print("wrote", sys.argv[3])
    elif cmd == "fit":
        fit(sys.argv[2], sys.argv[3], sys.argv[4] if len(sys.argv) > 4 else None)


if __name__ == "__main__":
    main()
