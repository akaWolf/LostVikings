#!/usr/bin/env python3
"""Bake the DE world-entry fields straight from the walkthrough video.

The DE builds these fields procedurally (no map chunk in the ROM) and the
quad-match restoration hit its ceiling on the compressed video (cell-level
mismatches — user-visible). This path skips quads entirely: crop the field
between the yellow frame lines at the measured scale, composite several
frames with an overlay-suppression rule (speech bubbles are saturated
red/green + white text, floating bubbles are bright — pick the least
saturated+bright candidate per pixel), quantize to a 48-color palette
(median cut) and emit pixels + palette per world. The port turns them
into tiles directly.
"""
import sys, os, json, zlib, struct
SCR = os.path.dirname(os.path.abspath(__file__))
VID = os.path.join(SCR, 'vid')

WORLDS = {
    'preh':    ['preh_02', 'preh_03'],
    'egypt':   ['egypt_03', 'egypt_08', 'egypt_14', 'egypt_18'],
    'factory': ['factory_05', 'factory_10', 'factory_15', 'factory_20'],
    'wacky':   ['wacky_04', 'wacky_08', 'wacky_14', 'wacky_18'],
    'ship':    ['ship_04', 'ship_08', 'ship_14', 'ship_18'],
}
# burn-in zones (vikings stand still through the whole scene, one
# bubble parks over Wacky's center in every frame): rectangles in
# field coords, refilled by a tile-aligned horizontal clone
MASKS = {
    'egypt':   [(40, 36, 68, 74, 'v'), (12, 86, 160, 110, 'v')],
    'factory': [(52, 12, 104, 56, 'v'), (160, 56, 208, 100, 'v')],
    'wacky':   [(0, 52, 64, 96, 'h'), (48, 24, 176, 40, 'v')],
    'ship':    [(0, 8, 52, 56, 'h')],
}
FW, FH = 256, 112


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
                pr = a if (pa <= pb and pa <= pc) else \
                    (b if pb <= pc else c)
                row[i] = (row[i] + pr) & 0xFF
        img[y*stride:(y+1)*stride] = row
        prev = row
    return img, W, H


def field_box(img, W, H):
    stride = W * 3

    def yellow_xs(y):
        return [x for x in range(150, 1130)
                if img[y*stride+x*3] > 170 and img[y*stride+x*3+1] > 150
                and img[y*stride+x*3+2] < 110]

    best = (0, None)
    for y in range(150, 260):
        xs = yellow_xs(y)
        if len(xs) > best[0]:
            best = (len(xs), (y, min(xs), max(xs)))
    if not best[1]:
        return None
    ty, x0, x1 = best[1]
    scale = (x1 - x0 + 1) / 256.0
    top = ty
    while len(yellow_xs(top)) > 60:
        top += 1
    return x0, top, scale


def crop(img, W, H, box):
    x0, y0, scale = box
    out = []
    stride = W * 3
    for fy in range(FH):
        row = []
        for fx in range(FW):
            vx = int(x0 + fx*scale + scale/2)
            vy = int(y0 + fy*scale + scale/2)
            o = vy*stride + vx*3
            row.append((img[o], img[o+1], img[o+2]))
        out.append(row)
    return out


def overlay_penalty(c):
    r, g, b = c
    sat = max(r, g, b) - min(r, g, b)
    lum = r + g + b
    p = 0
    if r > 130 and r - (g + b) // 2 > 40:
        p += 400                      # red bubble body
    if g > 130 and g - (r + b) // 2 > 40:
        p += 400                      # green bubble body
    if lum > 560:
        p += 300                      # white text / bubble shine
    return p + sat // 4 + lum // 8


def composite(crops):
    out = []
    for fy in range(FH):
        row = []
        for fx in range(FW):
            cand = [c[fy][fx] for c in crops]
            row.append(min(cand, key=overlay_penalty))
        out.append(row)
    return out


def median_cut(pixels, n):
    boxes = [pixels]
    while len(boxes) < n:
        boxes.sort(key=lambda px: -max(
            max(c[i] for c in px) - min(c[i] for c in px)
            for i in range(3)) * len(px) if px else 0)
        px = boxes.pop(0)
        if len(px) < 2:
            boxes.append(px)
            break
        ch = max(range(3), key=lambda i: max(c[i] for c in px) -
                 min(c[i] for c in px))
        px = sorted(px, key=lambda c: c[ch])
        boxes.append(px[:len(px)//2])
        boxes.append(px[len(px)//2:])
    pal = []
    for px in boxes:
        if not px:
            continue
        pal.append(tuple(sum(c[i] for c in px)//len(px) for i in range(3)))
    return pal


def auto_mask(crops, comp):
    """Burn-in detector: vikings idle-animate and bubbles drift, so
    pixels that differ across frames are 'live' overlays; bubble bodies
    and text are saturated red/green/white even when static. Dilate to
    cover sprite outlines."""
    live = [[False]*FW for _ in range(FH)]
    for fy in range(FH):
        for fx in range(FW):
            cand = [c[fy][fx] for c in crops]
            spread = max(max(c[i] for c in cand) - min(c[i] for c in cand)
                         for i in range(3))
            if spread > 60 or overlay_penalty(comp[fy][fx]) >= 300:
                live[fy][fx] = True
    # dilate by 6
    R = 6
    mask = [[False]*FW for _ in range(FH)]
    for fy in range(FH):
        for fx in range(FW):
            if not live[fy][fx]:
                continue
            for dy in range(-R, R+1):
                for dx in range(-R, R+1):
                    yy, xx = fy+dy, fx+dx
                    if 0 <= yy < FH and 0 <= xx < FW:
                        mask[yy][xx] = True
    return mask




def rects_mask(rects):
    mask = [[None]*FW for _ in range(FH)]
    for (mx0, my0, mx1, my1, d) in rects:
        for y in range(my0, min(my1, FH)):
            for x in range(mx0, min(mx1, FW)):
                mask[y][x] = d
    return mask

def inpaint(comp, mask):
    """Fill masked pixels by a tile-aligned VERTICAL clone (the fields
    are wall/drip patterned — a horizontal clone multiplied columns,
    seen on the Wacky bake), falling back to a horizontal one."""
    for fy in range(FH):
        for fx in range(FW):
            if not mask[fy][fx]:
                continue
            if mask[fy][fx] == 'h':
                sx = fx
                while sx >= 0 and mask[fy][sx]:
                    sx -= 16
                if sx < 0:
                    sx = fx
                    while sx < FW and mask[fy][sx]:
                        sx += 16
                if 0 <= sx < FW and not mask[fy][sx]:
                    comp[fy][fx] = comp[fy][sx]
                continue
            sy = fy
            while sy >= 0 and mask[sy][fx]:
                sy -= 16
            if sy < 0:
                sy = fy
                while sy < FH and mask[sy][fx]:
                    sy += 16
            if 0 <= sy < FH and not mask[sy][fx]:
                comp[fy][fx] = comp[sy][fx]
                continue
            sx = fx
            while sx >= 0 and mask[fy][sx]:
                sx -= 16
            if sx < 0:
                sx = fx
                while sx < FW and mask[fy][sx]:
                    sx += 16
            if 0 <= sx < FW and not mask[fy][sx]:
                comp[fy][fx] = comp[fy][sx]
    return comp


def bake(name, frames):
    crops = []
    for fr in frames:
        p = os.path.join(VID, fr + '.png')
        if not os.path.exists(p):
            continue
        img, W, H = read_png_rgb(p)
        box = field_box(img, W, H)
        if box:
            crops.append(crop(img, W, H, box))
    if not crops:
        return None
    comp = composite(crops)
    comp = inpaint(comp, rects_mask(MASKS.get(name, [])))
    pixels = [comp[y][x] for y in range(FH) for x in range(FW)]
    # 47 colors + black at index 0 (transparent-through slot)
    pal = median_cut(pixels, 47)
    pal = [(0, 0, 0)] + pal

    def nearest(c):
        return min(range(len(pal)),
                   key=lambda i: (pal[i][0]-c[0])**2 +
                   (pal[i][1]-c[1])**2 + (pal[i][2]-c[2])**2)

    # small LUT cache on the 5-bit grid for speed
    lut = {}
    idx = bytearray(FW * FH)
    for y in range(FH):
        for x in range(FW):
            c = comp[y][x]
            k = (c[0] >> 3, c[1] >> 3, c[2] >> 3)
            if k not in lut:
                lut[k] = nearest(c)
            idx[y*FW + x] = lut[k]
    return dict(pal=[list(c) for c in pal], w=FW, h=FH,
                pix=bytes(idx).hex())


if __name__ == '__main__':
    targets = sys.argv[1:] or list(WORLDS)
    outp = os.path.join(SCR, 'scene_bakes.json')
    out = {}
    if os.path.exists(outp):
        out = json.load(open(outp))
    for name in targets:
        print('== bake', name)
        r = bake(name, WORLDS[name])
        if r:
            out[name] = r
            print('   ok (%d colors)' % len(r['pal']))
    json.dump(out, open(outp, 'w'))
    print('saved', outp)
