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
}

def render(op, body, lay):
    """body = raw operand bytes (target word excluded). Returns str or None."""
    e = EXPR.get(op)
    if e is None:
        return None
    kind, tpl = e
    def imm(v):
        return str(v) if v <= 9 else f'0x{v:X}'
    try:
        if kind == 'imm16':
            return tpl.format(imm(struct.unpack_from('<H', body, 0)[0]))
        if kind == 'imm8':
            return tpl.format(imm(body[0]))
        if kind in ('fld', 'pfld'):
            return tpl.format(field_name(body[0]))
        if kind == 'mem16':
            return tpl.format(mem_name(struct.unpack_from('<H', body, 0)[0], lay))
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
        if kind in ('bit_fld', 'bit_pfld', 'bit_pfld_br'):
            return tpl.format(f'0x{bit_mask(body[0]):X}', field_name(body[1]))
        if kind == 'maskfld':
            return tpl.format(f'0x{bit_mask(body[0]):X}',
                              f'0x{bit_clear(body[0]):X}', field_name(body[1]))
    except (struct.error, IndexError):
        return None
    return None
