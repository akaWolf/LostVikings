#!/usr/bin/env python3
"""Stage 2.5: operand expressions for the structured .lvs emitter.

Every entry here was verified line-by-line against the v2 handler body
(which itself is the verified mirror of the original asm):
  ch1/field index: addr = LUT16[(idx-0x6CBA)&0xFFFF] + obj + 0x14E5
                   (LUT words live in static DS at 0x9346+idx)
  bit mask index:  mask = LUT16[(idx1-0x6C34)&0xFFFF]      (0x93CC+idx1)
  partner forms add ds:[obj+0x1995] instead of the object base.
Comment-level only: the compiler strips comments, so byte identity is
untouched by anything this module renders.
"""
import struct, re, os

LUT_FIELD_OFF = 0x6CBA
LUT_BIT_MASK = 0x6C34
LUT_BIT_CLEAR = 0x6C14
OBJ_FIELD_BASE = 0x14E5

_ds_static = None
def ds_static():
    global _ds_static
    if _ds_static is None:
        _ds_static = open('ds_static.bin', 'rb').read()
    return _ds_static

_obj_names = None
def obj_names():
    """OBJ_* constants from v2_ds_layout.h: column addr -> lowercase name."""
    global _obj_names
    if _obj_names is None:
        _obj_names = {}
        rx = re.compile(r'constexpr uint16_t OBJ_(\w+)\s*=\s*0x([0-9A-Fa-f]+)')
        for line in open('src/sdl/v2_ds_layout.h'):
            m = rx.search(line)
            if m:
                _obj_names.setdefault(int(m.group(2), 16), m.group(1).lower())
    return _obj_names

def field_name(idx):
    """ch1-style field index -> 'name' or 'fld_XXXX' (column address)."""
    lut = struct.unpack_from('<H', ds_static(), (idx - LUT_FIELD_OFF) & 0xFFFF)[0]
    col = (lut + OBJ_FIELD_BASE) & 0xFFFF
    return obj_names().get(col, f'fld_{col:04X}')

def bit_mask(idx1):
    return struct.unpack_from('<H', ds_static(), (idx1 - LUT_BIT_MASK) & 0xFFFF)[0]

def bit_clear(idx1):
    return struct.unpack_from('<H', ds_static(), (idx1 - LUT_BIT_CLEAR) & 0xFFFF)[0]

def mem_name(addr, lay):
    return lay.get(addr, f'{addr:04X}')

# op -> (operand kind, render template).  kinds:
#   imm16 w | imm8 b | fld b | mem16 w | pfld b (partner field)
#   bit_lit b,w | bit_mem b,w | bit_fld b,b | bit_pfld b,b
#   maskfld b,b (op 9C: mask idx + field idx) | vel b,b (two signed bytes)
# Branch ops render the condition only; the emitter appends '-> S_xxxx'.
EXPR = {
    0x02: ('imm16', 'sfx {0}'),                       # v2_vm_op_sound: seq&0xFF
    0x04: ('imm8', 'sfx_stop {0}'),                   # v2_vm_op_sound1
    0x18: ('vel', 'anim_tbl=partner; vel {0},{1}'),   # v2_vm_op_18
    0x19: ('imm16', 'anim {0}'),                      # OBJ_ANIM_PC = imm, timer=1
    0x51: ('imm16', 'acc = {0}'),
    0x52: ('fld', 'acc = self.{0}'),
    0x53: ('mem16', 'acc = [{0}]'),
    0x56: ('fld', 'self.{0} = acc'),
    0x57: ('mem16', '[{0}] = acc'),
    0x58: ('pfld', 'partner.{0} = acc'),
    0x59: ('fld', 'self.{0} += acc'),
    0x5A: ('mem16', '[{0}] += acc'),
    0x5B: ('pfld', 'partner.{0} += acc'),
    0x5C: ('fld', 'self.{0} -= acc'),
    0x5D: ('mem16', '[{0}] -= acc'),
    0x72: ('imm16', 'when acc == {0}'),
    0x73: ('fld', 'when acc == self.{0}'),
    0x74: ('mem16', 'when acc == [{0}]'),
    0x78: ('fld', 'when acc != self.{0}'),
    0x79: ('mem16', 'when acc != [{0}]'),
    0x88: ('mem16', 'call when acc == [{0}]'),        # ne skip / eq call-jump
    0x8C: ('fld', 'call when acc != self.{0}'),       # eq skip / ne call-jump
    0x97: ('bit_lit', 'acc = bit({1} & {0})'),
    0x99: ('bit_mem', 'acc = bit([{1}] & {0})'),
    0x9A: ('bit_pfld', 'acc = bit(partner.{1} & {0})'),
    0x9C: ('maskfld', 'self.{2} = self.{2} & {1} | (acc? {0} : 0)'),
    0xA9: ('bit_fld', 'when acc == bit(self.{1} & {0})'),
    0xAB: ('bit_pfld_br', 'when acc == bit(partner.{1} & {0})'),
    0xAD: ('bit_lit', 'when acc == bit({1} & {0})'),
    # wave 2 — every body read from v2_vm.cpp before entry:
    0x08: ('none', 'hflip if self.flags&0x40'),       # sub_1367c
    0x0B: ('none', 'hflip'),                          # sub_1369c unconditional
    0x1C: ('none', 'when self.anim_timer == 0'),      # sub_1443d (br, 0 operand)
    0x2F: ('none', 'anim_step'),                      # interp_1303a + tail_135cf
    0x46: ('imm16', 'cmdq_push(6, {0})'),             # v2_vm_op_46
    0x4B: ('none', 'self.flags |= 0x2000'),
    0x54: ('pfld', 'acc = partner.{0}'),
    0x5F: ('fld', 'self.{0} &= acc'),
    0x60: ('mem16', '[{0}] &= acc'),
    0x62: ('fld', 'self.{0} |= acc'),
    0x66: ('mem16', '[{0}] ^= acc'),
    0x6D: ('imm16', 'when acc <u {0}'),               # unsigned <
    0x7C: ('imm16s', 'when acc >=s {0}'),             # signed >= jumps
    0x81: ('imm16s', 'when acc <s {0}'),              # signed >= skips
    0x82: ('fld', 'when acc <s self.{0}'),
    0x8B: ('imm16', 'call when acc != {0}'),
    0x96: ('none', 'self.partner = acc'),
    0x98: ('bit_fld_ld', 'acc = bit(self.{1} & {0})'),  # bittest_15403 load
    0xA8: ('bit_lit', 'when acc == bit({1} & {0})'),
    0xB2: ('bit_lit', 'call when acc == bit({1} & {0})'),
    # collision family: 1 filter byte, always bit_idx += 2, hit -> call-jump
    0x1A: ('imm8', 'call when coll_155d6_vik(f={0})'),
    0x1D: ('imm8', 'call when coll_156c0_vik(f={0})'),
    0x32: ('imm8', 'call when coll_15788(f={0})'),
    0x33: ('imm8', 'call when coll_up_157eb(f={0})'),
    0x37: ('imm8', 'call when coll_155d6(f={0})'),
    0x38: ('imm8', 'call when coll_156c0(f={0})'),
    0x3C: ('imm8', 'call when coll_down_1584e(f={0})'),
    # wave 3:
    0x0E: ('none', 'bits[0x356+sub>>3] |= 1<<(sub&7)'),  # OBJ_ANIM_SUB mark
    0x12: ('none', 'res_deduct(partner)'),               # sub_15505
    0x1F: ('imm8', 'when probe_down({0})'),              # 158d7 + 30C8E[0]
    0x20: ('imm8', 'when probe_l/r({0})'),               # flip-aware 158aa/158b9
    0x21: ('imm8', 'when probe_r/l({0})'),               # anti-flip 158b9/158aa
    0x23: ('imm8', 'when probe_down2({0})'),             # 158d7 + 30C8E[2]
    0x2C: ('imm8', 'search_vik_up(f={0})'),              # y=bbox_y0-1, si<6
    0x2D: ('none', 'search_next_vik'),
    0x35: ('imm8', 'search_all_up(f={0})'),              # si<obj_count
    0x36: ('none', 'search_next_all'),
    0xD1: ('imm8', 'search_vik_down(f={0})'),            # y=bbox_y1+1
    0x3D: ('rgb', 'pal_shade({0},{1},{2})'),
    0x42: ('none', 'cmdq_push(2)'),
    0x68: ('imm16', 'when acc >=u {0}'),
    0xAA: ('bit_mem', 'when acc == bit([{1}] & {0})'),
    0xB3: ('bit_fld', 'call when acc == bit(self.{1} & {0})'),
    # wave 4:
    0x07: ('none', 'hflip if !(self.flags&0x40)'),    # sub_13675 inverse of 08
    0x22: ('imm8', 'when probe_up({0})'),             # 158c8 + 30C8E[2]
    0x2E: ('imm16', 'shake_x({0})'),                  # src/gate split of the word
    0x30: ('imm8', 'when probe_front(f={0})'),        # 158e6, carry -> jump
    0x3A: ('none', 'res_deduct(partner)'),            # same body as 12
    0x4F: ('imm8', 'when !platform(f={0})'),          # 163ac, no-carry -> jump
    0x55: ('none', 'acc = random()'),
    0x63: ('mem16', '[{0}] |= acc'),
    0x64: ('pfld', 'partner.{0} |= acc'),
    0x6A: ('mem16', 'when acc >=u [{0}]'),
    0x77: ('imm16', 'when acc != {0}'),
    0x7D: ('fld', 'when acc >=s self.{0}'),
    0x84: ('pfld', 'when acc <s partner.{0}'),
    0x91: ('mem16', '[{0}] ±= acc by hflip'),
    0x9D: ('maskmem', '[{2}] = [{2}] & {1} | (acc? {0} : 0)'),
}

# ---- channel-operand ops (30C98 getter pairs) --------------------------
CH_LEN = {0: 2, 1: 1, 2: 2, 3: 1, 4: 0, 5: 0, 6: 1, 7: 2}

def ch_render(chan, body, o, lay):
    """Render one getter channel from body at offset o.
    Returns (text, bytes_used) or (None, None) on unrenderable."""
    if chan == 0:
        v = struct.unpack_from('<H', body, o)[0]
        return (str(v) if v <= 9 else f'0x{v:X}'), 2
    if chan == 1:
        return f'self.{field_name(body[o])}', 1
    if chan == 2:
        return f'[{mem_name(struct.unpack_from("<H", body, o)[0], lay)}]', 2
    if chan == 3:
        return f'partner.{field_name(body[o])}', 1
    if chan == 4:
        return 'random()', 0
    if chan == 5:
        return 'ch5', 0
    if chan == 6:
        return f'ub6({body[o]:02X})', 1
    if chan == 7:
        return f'ub7({struct.unpack_from("<H", body, o)[0]:04X})', 2
    return None, None

def ch_pair(body, o, lay):
    """mode byte at o, then channels (m&7) and (m>>3)&7. -> (a, b, next_off)."""
    m = body[o]
    o += 1
    a, la = ch_render(m & 7, body, o, lay)
    if a is None:
        return None, None, None
    o += la
    b, lb = ch_render((m >> 3) & 7, body, o, lay)
    if b is None:
        return None, None, None
    return a, b, o + lb

def render_channels(op, body, lay):
    """op 14 (spawn) / 49,4A (probe at pos): channel-pair operands."""
    try:
        if op == 0x14:
            x, y, o = ch_pair(body, 0, lay)
            if x is None: return None
            pool, fl, o = ch_pair(body, o, lay)
            if pool is None or o >= len(body) + 1: return None
            t = body[o] if o < len(body) else None
            if t is None or o + 1 != len(body): return None
            return f'spawn(t={t:02X}, x={x}, y={y}, pool={pool}, fl={fl}&0x801)'
        if op in (0x49, 0x4A):
            x, y, o = ch_pair(body, 0, lay)
            if x is None or o + 1 != len(body): return None
            tag = '' if op == 0x49 else '2'
            return f'when probe_at{tag}({body[o]:02X}, x={x}, y={y})'
    except (IndexError, struct.error):
        return None
    return None

def render(op, body, lay):
    """body = raw operand bytes (target word excluded). Returns str or None."""
    if op in (0x14, 0x49, 0x4A):
        return render_channels(op, body, lay)
    e = EXPR.get(op)
    if e is None:
        return None
    kind, tpl = e
    def imm(v):
        return str(v) if v <= 9 else f'0x{v:X}'
    try:
        if kind == 'none':
            return tpl
        if kind == 'imm16':
            return tpl.format(imm(struct.unpack_from('<H', body, 0)[0]))
        if kind == 'imm16s':
            v = struct.unpack_from('<H', body, 0)[0]
            return tpl.format(v - 0x10000 if v >= 0x8000 else v)
        if kind == 'imm8':
            return tpl.format(imm(body[0]))
        if kind in ('fld', 'pfld'):
            return tpl.format(field_name(body[0]))
        if kind == 'mem16':
            return tpl.format(mem_name(struct.unpack_from('<H', body, 0)[0], lay))
        if kind == 'rgb':
            return tpl.format(body[0], body[1], body[2])
        if kind == 'vel':
            sx = body[0] - 256 if body[0] >= 128 else body[0]
            sy = body[1] - 256 if body[1] >= 128 else body[1]
            return tpl.format(sx, sy)
        if kind == 'bit_lit':
            return tpl.format(f'0x{bit_mask(body[0]):X}',
                              struct.unpack_from('<H', body, 1)[0])
        if kind == 'bit_mem':
            return tpl.format(f'0x{bit_mask(body[0]):X}',
                              mem_name(struct.unpack_from('<H', body, 1)[0], lay))
        if kind in ('bit_fld', 'bit_pfld', 'bit_pfld_br', 'bit_fld_ld'):
            return tpl.format(f'0x{bit_mask(body[0]):X}', field_name(body[1]))
        if kind == 'maskfld':
            return tpl.format(f'0x{bit_mask(body[0]):X}',
                              f'0x{bit_clear(body[0]):X}', field_name(body[1]))
        if kind == 'maskmem':
            return tpl.format(f'0x{bit_mask(body[0]):X}',
                              f'0x{bit_clear(body[0]):X}',
                              mem_name(struct.unpack_from('<H', body, 1)[0], lay))
    except (struct.error, IndexError):
        return None
    return None
