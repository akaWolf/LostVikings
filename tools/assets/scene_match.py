#!/usr/bin/env python3
"""Restore the DE world-entry scene field from walkthrough video frames.

The DE builds the field procedurally (no map chunk exists — full-ROM scan),
out of the world's unified quad table (gt == level gt == bg gt) on the
world tileset. We reverse the OUTPUT: grab the field area between the
yellow frame lines, downscale to native 256px, and match every 16x16 cell
against every quad of the world gt rendered in candidate palettes.
Cells hidden by speech bubbles / vikings differ between frames — take the
per-cell minimum across several frames.
"""
import sys, os, json, zlib, struct
sys.path.insert(0, '/home/akawolf/projects/own/LostVikings/tools/assets')
import snes2pc as SP
import level_render as LR

SCR = os.path.dirname(os.path.abspath(__file__))
VID = os.path.join(SCR, 'vid')
rom = SP.SnesRom()

WORLDS = {
    'preh':    dict(lvl=0x01A, frames=['preh_02', 'preh_03'],
                    extra_pals=[0x3B, 0x3C, 0x3D]),
    'egypt':   dict(lvl=0x02F, frames=['egypt_03', 'egypt_08', 'egypt_14'],
                    extra_pals=[0x2C]),
    'factory': dict(lvl=0x041, frames=['factory_05', 'factory_10',
                                       'factory_15'], extra_pals=[]),
    'wacky':   dict(lvl=0x05B, frames=['wacky_04', 'wacky_08', 'wacky_14'],
                    extra_pals=[]),
    'ship':    dict(lvl=0x077, frames=['ship_04', 'ship_08', 'ship_14'],
                    extra_pals=[]),
}


def read_png_rgb(path):
    d = open(path, 'rb').read()
    o = 8
    W = H = None
    idat = b''
    while o < len(d):
        ln = struct.unpack('>I', d[o:o+4])[0]
        typ = d[o+4:o+8]
        if typ == b'IHDR':
            W, H = struct.unpack('>II', d[o+8:o+16])
        elif typ == b'IDAT':
            idat += d[o+8:o+8+ln]
        o += 12 + ln
    raw = zlib.decompress(idat)
    stride = W * 3
    img = bytearray(W * H * 3)
    prev = bytearray(stride)
    pos = 0
    for y in range(H):
        f = raw[pos]; pos += 1
        row = bytearray(raw[pos:pos+stride]); pos += stride
        if f == 1:
            for i in range(3, stride):
                row[i] = (row[i] + row[i-3]) & 0xFF
        elif f == 2:
            for i in range(stride):
                row[i] = (row[i] + prev[i]) & 0xFF
        elif f == 3:
            for i in range(stride):
                a = row[i-3] if i >= 3 else 0
                row[i] = (row[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif f == 4:
            for i in range(stride):
                a = row[i-3] if i >= 3 else 0
                b = prev[i]
                c = prev[i-3] if i >= 3 else 0
                pp = a + b - c
                pa, pb, pc = abs(pp-a), abs(pp-b), abs(pp-c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                row[i] = (row[i] + pr) & 0xFF
        img[y*stride:(y+1)*stride] = row
        prev = row
    return img, W, H


def find_field(img, W, H):
    """Yellow frame lines -> field box; scale from the box height."""
    stride = W * 3
    ys = []
    for y in range(H):
        cnt = 0
        for x in range(200, 1080, 4):
            r, g, b = img[y*stride+x*3:y*stride+x*3+3]
            if r > 170 and g > 150 and b < 110:
                cnt += 1
        if cnt > 60:
            ys.append(y)
    if not ys:
        return None
    top = min(ys) + 4
    bot = max(y for y in ys if y > top + 100)
    # x-извлечение: у видео скейл одинаков по осям (проверено по спрайту
    # Эрика ~97px = 32*3.035); поле = 256 SNES-px шириной, центрировано
    scale = 3.035
    fh = int((bot - top) / scale)
    # центр поля по х: жёлтая линия тянется по всему полю: найдём её span
    y0 = min(ys)
    xs = [x for x in range(W)
          if img[y0*stride+x*3] > 170 and img[y0*stride+x*3+1] > 150
          and img[y0*stride+x*3+2] < 110]
    x0 = min(xs)
    return x0, top, scale, fh


def native_field(img, W, H, box):
    x0, y0, scale, fh = box
    FW = 256
    out = bytearray(FW * fh * 3)
    stride = W * 3
    for fy in range(fh):
        for fx in range(FW):
            vx = int(x0 + fx*scale + scale/2)
            vy = int(y0 + fy*scale + scale/2)
            if vx < W and vy < H:
                out[(fy*FW+fx)*3:(fy*FW+fx)*3+3] = \
                    img[vy*stride+vx*3:vy*stride+vx*3+3]
    return out, FW, fh


def mkpal_chunk(cid):
    pc = rom.chunk(cid)
    pal = []
    for i in range(min(len(pc)//2, 128)):
        w = pc[i*2] | (pc[i*2+1] << 8)
        r, g, b = SP.bgr555_to_vga6(w)
        pal.append(((r*255)//63, (g*255)//63, (b*255)//63))
    while len(pal) < 128:
        pal.append((0, 0, 0))
    return pal


def mkpal_cgram(lvl):
    st = LR.parse_stripe(rom.chunk(lvl))
    cg = SP.compose_cgram(rom, st['pal_list'])
    return [((SP.bgr555_to_vga6(w)[0]*255)//63,
             (SP.bgr555_to_vga6(w)[1]*255)//63,
             (SP.bgr555_to_vga6(w)[2]*255)//63) for w in cg[:128]]


def match_world(name, spec):
    st = LR.parse_stripe(rom.chunk(spec['lvl']))
    h = bytes.fromhex(st['head'])
    w16 = lambda o: h[o] | (h[o+1] << 8)
    ts = rom.chunk(w16(0x30))
    gt = rom.chunk(w16(0x32))
    nq = len(gt) // 8
    pals = [('cgram', mkpal_cgram(spec['lvl']))]
    for pc in spec['extra_pals']:
        pals.append(('%03X' % pc, mkpal_chunk(pc)))

    fields = []
    fh_min = 10**9
    for fr in spec['frames']:
        p = os.path.join(VID, fr + '.png')
        if not os.path.exists(p):
            print('  skip missing frame', fr)
            continue
        img, W, H = read_png_rgb(p)
        box = find_field(img, W, H)
        if not box:
            print('  no field in', fr)
            continue
        fld, FW, fh = native_field(img, W, H, box)
        fields.append((fld, FW, fh))
        fh_min = min(fh_min, fh)
        print('  frame %s: field 256x%d (x0=%d y0=%d)' % (fr, fh, box[0],
                                                          box[1]))
    if not fields:
        return None

    tile_cache = {}

    def tile_px(t):
        if t not in tile_cache:
            tile_cache[t] = SP.snes_tile_decode(
                ts[t*32:(t+1)*32].ljust(32, b'\x00'))
        return tile_cache[t]

    sub_cache = {}

    def render_sub(q, pi):
        key = (q, pi)
        if key in sub_cache:
            return sub_cache[key]
        pal = pals[pi][1]
        e = gt[q*8:q*8+8]
        sub = []
        for k, (dy, dx) in enumerate(((0, 0), (0, 1), (1, 0), (1, 1))):
            dw = e[k*2] | (e[k*2+1] << 8)
            t, prow = dw & 0x3FF, (dw >> 10) & 7
            hf, vf = (dw >> 14) & 1, (dw >> 15) & 1
            px = tile_px(t)
            for yy in range(0, 8, 2):
                sy = 7-yy if vf else yy
                for xx in range(0, 8, 2):
                    sx = 7-xx if hf else xx
                    v = px[sy*8+sx]
                    c = pal[(v + prow*16)] if v else pal[0]
                    sub.append((dy*8+yy, dx*8+xx, c))
        sub_cache[key] = sub
        return sub

    def sad_cell(fld, FW, fy0, fx0, q, pi):
        s = 0
        for (y, x, (r, g, b)) in render_sub(q, pi):
            fo = ((fy0+y)*FW + fx0+x) * 3
            s += abs(r-fld[fo]) + abs(g-fld[fo+1]) + abs(b-fld[fo+2])
        return s

    # quads that render fully transparent (all-zero pixels) may only win
    # on near-black video cells — else the dark JPEG mush swallows the
    # mountains into quad 000
    def quad_is_blank(q):
        e = gt[q*8:q*8+8]
        for k in range(4):
            dw = e[k*2] | (e[k*2+1] << 8)
            if any(tile_px(dw & 0x3FF)):
                return False
        return True

    blank = [quad_is_blank(q) for q in range(nq)]

    def cell_lum(fld, FW, fy0, fx0):
        s = 0
        for y in range(0, 16, 4):
            for x in range(0, 16, 4):
                o = ((fy0+y)*FW + fx0+x) * 3
                s += fld[o] + fld[o+1] + fld[o+2]
        return s / 16 / 3

    rows = fh_min // 16

    # pass 1: pick ONE palette for the whole scene (the field is drawn
    # in a single CGRAM state) by total best-SAD over a sample of cells
    pal_tot = [0] * len(pals)
    for pi in range(len(pals)):
        for qy in range(0, rows, 2):
            for qx in range(0, 16, 3):
                best = min(min(sad_cell(fld, FW, qy*16, qx*16, q, pi)
                               for (fld, FW, fh) in fields)
                           for q in range(0, nq, 2))
                pal_tot[pi] += best
    pw = pal_tot.index(min(pal_tot))
    print('  palette totals %s -> %s' % (pal_tot, pals[pw][0]))

    # pass 1.5: grid phase — the frame-line estimate is a few px off;
    # probe (dx, dy) around zero on a sample row by total best-SAD
    def probe_phase(dx, dy):
        tot = 0
        for qy in (1, rows - 1):
            for qx in range(1, 15, 3):
                fy0, fx0 = qy*16 + dy, qx*16 + dx
                if fy0 < 0 or fy0 + 16 > fh_min or fx0 < 0:
                    return 10**9
                tot += min(min(sad_cell(fld, FW, fy0, fx0, q, pw)
                               for (fld, FW, fh) in fields)
                           for q in range(0, nq, 2))
        return tot
    best_ph = (0, 0, probe_phase(0, 0))
    for dy in (-4, -2, 0, 2, 4):
        for dx in (-4, -2, 0, 2, 4):
            t = probe_phase(dx, dy)
            if t < best_ph[2]:
                best_ph = (dx, dy, t)
    pdx, pdy = best_ph[0], best_ph[1]
    print('  phase dx=%d dy=%d' % (pdx, pdy))

    def bubble_frac(fld, FW, fy0, fx0):
        n = hit = 0
        for y in range(0, 16, 2):
            for x in range(0, 16, 2):
                o = ((fy0+y)*FW + fx0+x) * 3
                r, g, b = fld[o], fld[o+1], fld[o+2]
                if (r > 140 and g < 70 and b < 70) or \
                   (r > 190 and g > 190 and b > 190):
                    hit += 1
                n += 1
        return hit / n

    grid = []
    covered = []
    for qy in range(rows):
        rowg = []
        rowc = []
        for qx in range(16):
            fy0 = min(max(qy*16 + pdy, 0), fh_min - 16)
            fx0 = min(max(qx*16 + pdx, 0), 256 - 16)
            # speech bubbles (red box + white text) sit over the field
            # in most frames — a cell is usable only via a frame where
            # the bubble is absent
            fs = [(fld, FW) for (fld, FW, fh) in fields
                  if bubble_frac(fld, FW, fy0, fx0) < 0.30]
            if not fs:
                rowg.append((0, pw, -1))
                rowc.append(True)
                continue
            rowc.append(False)
            dark = min(cell_lum(fld, FW, fy0, fx0)
                       for (fld, FW) in fs) < 22
            best = (0, pw, 10**9)
            for q in range(nq):
                if blank[q] and not dark:
                    continue
                s = min(sad_cell(fld, FW, fy0, fx0, q, pw)
                        for (fld, FW) in fs)
                if s < best[2]:
                    best = (q, pw, s)
            rowg.append(best)
        grid.append(rowg)
        covered.append(rowc)
    # bubble-covered cells take the dominant quad of their row's clean
    # cells (the sky/fill pattern), else the global clean mode
    from collections import Counter
    glob = Counter(q for qy in range(rows) for qx in range(16)
                   if not covered[qy][qx]
                   for q in [grid[qy][qx][0]])
    for qy in range(rows):
        clean = [grid[qy][qx][0] for qx in range(16) if not covered[qy][qx]]
        fill = Counter(clean).most_common(1)[0][0] if clean else \
            glob.most_common(1)[0][0]
        for qx in range(16):
            if covered[qy][qx]:
                grid[qy][qx] = (fill, pw, -1)
    # cleanup: a cell whose SAD is far above the row median gets the
    # row's dominant quad (sky/floor fills) when one dominates
    from collections import Counter
    for qy in range(rows):
        row = grid[qy]
        meds = sorted(s for (q, pi, s) in row)[len(row)//2]
        cnt = Counter(q for (q, pi, s) in row)
        dom, domn = cnt.most_common(1)[0]
        for qx in range(16):
            q, pi, s = row[qx]
            if s > max(3*meds, 2500) and domn >= 8 and not blank[q]:
                row[qx] = (dom, pi, s)
        print('  row %d: %s' % (qy, ' '.join('%03X' % q
                                             for (q, pi, s) in row)))
    return dict(world=name, lvl=spec['lvl'], rows=rows,
                pals=[p[0] for p in pals], pal_win=pw, phase=[pdx, pdy],
                grid=[[[q, pi, s] for (q, pi, s) in row] for row in grid])


if __name__ == '__main__':
    targets = sys.argv[1:] or list(WORLDS)
    out = {}
    outp = os.path.join(SCR, 'scene_fields.json')
    if os.path.exists(outp):
        out = json.load(open(outp))
    for name in targets:
        print('== %s' % name)
        r = match_world(name, WORLDS[name])
        if r:
            out[name] = r
    json.dump(out, open(outp, 'w'))
    print('saved', outp)
