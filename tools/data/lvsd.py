#!/usr/bin/env python3
"""lvsd — the .lvd logic language (level C of the logic editor, minimum).

A .lvd file is a readable layer over the free-form .lvsf (DSL_SPEC §v1.5):
every object-code line `o XX <operands> [S_target]` becomes ONE statement
rendered from a bidirectional template (op -> slots in byte order), and
the compiler turns each statement back into exactly that `o` line before
handing the text to lvs_full.compile_free — the only place that lays bytes
out. Nothing else changes: records, anim code (`a`), data blobs, aliases
and palette anchors pass through verbatim, in the same order, so the
layout and the opcode semantics are the verified v1.5 ones.

Statement forms (all parse back to the same opcode + operand bytes):
  yield | nop | return | exit | despawn
  goto L | call L
  if <cond> goto L | if <cond> call L        (branch families, srch)
  anim A_xxxx | sfx N | self.f = acc | [g] += acc | ...
  self.f = X | [g] = X | if X == Y goto L    (acc sugar: `acc = X` folded
        into the next statement when X is a plain load — literal,
        self.f, [g], partner.f, random(), bit(...) — and that statement
        is not a jump target; the compiler always expands it back to
        the load op + the consumer op)
Operand tokens: fields by their OBJ_* names (v2_ds_layout.h) — `name#idx`
when the name maps to several field indices; globals by layout names or
4-hex addresses; bit masks as `0xMASK` (`#idx` when several indices carry
the same mask); labels by state names from the dictionary
(tools/data/lvs_names.json) or the machine S_xxxx.

Usage (repo root):
  lvsd.py decompile CID [out.lvd]      assets_raw/lvs/CID.lvsf -> .lvd
  lvsd.py compile in.lvd [out.bin]     .lvd -> chunk bytes (via .lvsf)
  lvsd.py lower in.lvd [out.lvsf]      .lvd -> .lvsf text only
  lvsd.py check [CID ...]              decompile+compile == reference bytes
  lvsd.py seed                         write the initial names dictionary
"""
import sys, os, re, json, struct, importlib.util

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
NAMES_PATH = os.path.join(HERE, 'lvs_names.json')


def _load(name):
    sp = importlib.util.spec_from_file_location(name, os.path.join(HERE, name + '.py'))
    m = importlib.util.module_from_spec(sp)
    sp.loader.exec_module(m)
    return m


_mods = {}
def mod(name):
    if name not in _mods:
        cwd = os.getcwd(); os.chdir(ROOT)
        try: _mods[name] = _load(name)
        finally: os.chdir(cwd)
    return _mods[name]


# --------------------------------------------------------------- names ----
class Names:
    """Operand name tables. Fields and masks are derived from the static
    DS LUTs (expr.py), globals from the phase-D layout; states/classes/anims
    come from the dictionary file. Every table is checked for invertibility
    and the renderer adds `#idx` where a name is not unique."""
    def __init__(self, dict_path=NAMES_PATH):
        ex = mod('expr'); dz = mod('disasm')
        cwd = os.getcwd(); os.chdir(ROOT)
        try:
            self.fld_of = {}
            for idx in range(256):
                try: self.fld_of[idx] = ex.field_name(idx)
                except Exception: self.fld_of[idx] = f'fld?{idx:02X}'
            self.mask_of = {}
            for idx in range(256):
                try: self.mask_of[idx] = ex.bit_mask(idx)
                except Exception: self.mask_of[idx] = None
            self.lay = dz.load_layout_names()
        finally: os.chdir(cwd)
        self.idx_of_fld = {}
        for idx, n in self.fld_of.items(): self.idx_of_fld.setdefault(n, []).append(idx)
        self.idx_of_mask = {}
        for idx, m in self.mask_of.items():
            if m is not None: self.idx_of_mask.setdefault(m, []).append(idx)
        self.addr_of_mem = {}
        for a, n in self.lay.items(): self.addr_of_mem.setdefault(n, []).append(a)
        self.d = {'states': {}, 'classes': {}, 'anims': {}, 'sfx': {}}
        if os.path.exists(dict_path):
            self.d.update(json.load(open(dict_path, encoding='utf-8')))

    # fields -------------------------------------------------------------
    def fld(self, idx):
        n = self.fld_of[idx]
        return n if len(self.idx_of_fld.get(n, [])) == 1 else f'{n}#{idx:02X}'
    def fld_parse(self, tok):
        if '#' in tok:
            n, i = tok.split('#', 1); idx = int(i, 16)
            if self.fld_of.get(idx) != n: raise ValueError(f'field {tok}: index/name mismatch')
            return idx
        c = self.idx_of_fld.get(tok)
        if not c: raise ValueError(f'unknown field {tok!r}')
        if len(c) != 1: raise ValueError(f'ambiguous field {tok!r}: indices {[hex(x) for x in c]} — write name#idx')
        return c[0]
    # masks ---------------------------------------------------------------
    def mask(self, idx):
        m = self.mask_of[idx]
        if m is None: return f'mask?{idx:02X}'
        return f'0x{m:X}' if len(self.idx_of_mask.get(m, [])) == 1 else f'0x{m:X}#{idx:02X}'
    def mask_parse(self, tok):
        if tok.startswith('mask?'): return int(tok[5:], 16)
        if '#' in tok:
            m, i = tok.split('#', 1); idx = int(i, 16)
            if self.mask_of.get(idx) != int(m, 16): raise ValueError(f'mask {tok}: index/value mismatch')
            return idx
        c = self.idx_of_mask.get(int(tok, 16))
        if not c: raise ValueError(f'unknown mask {tok}')
        if len(c) != 1: raise ValueError(f'ambiguous mask {tok}: indices {[hex(x) for x in c]} — write 0xMASK#idx')
        return c[0]
    # globals -------------------------------------------------------------
    def mem(self, addr):
        n = self.lay.get(addr)
        return n if n and len(self.addr_of_mem[n]) == 1 else f'{addr:04X}'
    def mem_parse(self, tok):
        if re.fullmatch(r'[0-9A-Fa-f]{4}', tok): return int(tok, 16)
        c = self.addr_of_mem.get(tok)
        if not c or len(c) != 1: raise ValueError(f'unknown global {tok!r}')
        return c[0]
    # states / classes / anims -------------------------------------------
    def state(self, cid, lbl):
        """S_xxxx -> dictionary name, else the automatic owner name
        (`<class>_<k>`: the k-th state of the class that owns the code, by
        address; `shared_<t1>_<t2>_<k>` when several classes share it), else
        S_xxxx; a label the author named (S_walk, written by the lowerer from
        `state walk:`) shows as `walk`."""
        if not re.fullmatch(r'S_[0-9A-Fa-f]{4}', lbl):
            return lbl[2:] if lbl.startswith('S_') else lbl
        n = self.d['states'].get(f'{cid:X}:{lbl[2:]}')
        if n: return n
        return self.auto_names(cid).get(int(lbl[2:], 16), lbl)

    _auto = {}
    def auto_names(self, cid):
        """{label addr: name} for the labels of chunk cid, from the ownership
        propagation of lvs_struct (record P/P+3 roots over the code graph)."""
        if cid in self._auto: return self._auto[cid]
        ls = mod('lvs_struct'); lf = mod('lvs_full')
        cwd = os.getcwd(); os.chdir(ROOT)
        try:
            own = ls.owners_of_states(cid)
            text = canonical_text(cid)
        finally: os.chdir(cwd)
        labels = sorted(int(m.group(1), 16) for m in re.finditer(r'^S_([0-9A-F]{4}):', text, re.M))
        groups = {}
        for a in labels:
            os_ = frozenset(own.get(a, ()))
            groups.setdefault(os_, []).append(a)
        names = {}
        for os_, addrs in groups.items():
            if not os_: continue
            base = self.cls(cid, min(os_)) if len(os_) == 1 else 'shared_' + '_'.join(f'{t:02X}' for t in sorted(os_))
            k = 0
            for a in sorted(addrs):
                if f'{cid:X}:{a:04X}' in self.d['states']: continue    # a dictionary name keeps its own
                k += 1; names[a] = f'{base}_{k}'
        self._auto[cid] = names
        return names
    def cls(self, cid, t):
        return self.d['classes'].get(f'{cid:X}:{t:02X}', self.d['classes'].get(f'*:{t:02X}', f't{t:02X}'))


# ------------------------------------------------------- statement table --
# op -> (slots, template, flags). slots = operand kinds in BYTE order:
#   w  imm16 (unsigned)   ws imm16 signed    b  imm8
#   f  field index (1B)   p  partner field (1B, same LUT)   g  global addr (2B)
#   m  mask index (1B)    r  raw byte        v  signed byte (velocity)
# The template refers to slots as {0},{1},... in slot order. flags:
#   T = takes a code target (branch/jump/srch); the statement gets
#       ' goto L' (kind br/srch/jmp) or ' call L' (call-branches).
# Control ops (00 01 03 05 06 0F 10 13 19) and channel ops are handled apart.
OPS = {
    0x02: ('w', 'sfx {0}'), 0x04: ('b', 'sfx_stop {0}'),
    0x07: ('', 'hflip if !(self.flags&0x40)'), 0x08: ('', 'hflip if self.flags&0x40'),
    0x09: ('', 'vflip if !(self.flags&0x80)'), 0x0A: ('', 'vflip if self.flags&0x80'),
    0x0B: ('', 'hflip'), 0x0C: ('', 'vflip'),
    0x0E: ('', 'mark_anim_sub'), 0x11: ('', 'res_deduct(partner)'), 0x12: ('', 'res_deduct(partner) #12'),
    0x17: ('vv', 'vel {0},{1} anim_tbl=0'), 0x18: ('vv', 'vel {0},{1} anim_tbl=partner'),
    0x1B: ('vv', 'obj0.vel {0},{1} obj0.anim_tbl=self'),
    0x1A: ('b', 'if coll_155d6_vik(f={0})', 'C'), 0x1D: ('w', 'if coll_156c0_vik(class={0})', 'C'),
    0x1C: ('', 'if anim_timer_zero', 'T'),      # its own op (the generic `if self.anim_timer == 0` is 52+72)
    0x1E: ('b', 'if probe_up0({0})', 'T'), 0x1F: ('b', 'if probe_down({0})', 'T'),
    0x20: ('b', 'if probe_lr({0})', 'T'), 0x21: ('b', 'if probe_rl({0})', 'T'),
    0x22: ('b', 'if probe_up({0})', 'T'), 0x23: ('b', 'if probe_down2({0})', 'T'),
    0x24: ('b', 'if probe_lr_fix({0})', 'T'), 0x25: ('b', 'if probe_rl2({0})', 'T'),
    0x2C: ('b', 'search_vik_up(f={0})', 'T'), 0x2D: ('', 'search_next_vik'),
    0x2E: ('w', 'shake_x({0})'), 0x2F: ('', 'anim_step'),
    0x30: ('b', 'if probe_front(f={0})', 'T'), 0x31: ('b', 'if probe_front2({0})', 'T'),
    0x32: ('b', 'if coll_15788(f={0})', 'C'), 0x33: ('b', 'if coll_up_157eb(f={0})', 'C'),
    0x35: ('b', 'search_all_up(f={0})', 'T'), 0x36: ('', 'search_next_all'),
    0x37: ('b', 'if coll_155d6(f={0})', 'C'), 0x38: ('w', 'if coll_156c0(class={0})', 'C'),
    0x39: ('rrr', 'discard3 {0},{1},{2}'), 0x3A: ('', 'res_deduct(partner) #3A'),
    0x3B: ('w', 'shake_y({0})'), 0x3C: ('b', 'if coll_down_1584e(f={0})', 'C'),
    0x3D: ('rrr', 'pal_shade({0},{1},{2})'), 0x3E: ('', 'pal_shade_off'),
    0x3F: ('', 'subsprites_on'), 0x40: ('', 'subsprites_off'),
    0x42: ('', 'cmdq_push(2)'), 0x43: ('', 'cmdq_push(4)'), 0xCB: ('', 'cmdq_push(4) #CB'),
    0x46: ('w', 'cmdq_push(6, {0})'), 0x47: ('', 'nop47'),
    0x4B: ('', 'flags_set 0x2000'),           # its own op (the generic `self.flags |= 0x2000` is 51+62) 0x4C: ('rrr', 'pal_shade2({0},{1},{2})'), 0x4D: ('', 'pal_shade2_off'),
    0x4E: ('', 'if !platform0', 'T'), 0x4F: ('', 'if !platform', 'T'),
    0x51: ('w', 'acc = {0}'), 0x52: ('f', 'acc = self.{0}'), 0x53: ('g', 'acc = [{0}]'),
    0x54: ('p', 'acc = partner.{0}'), 0x55: ('', 'acc = random()'),
    0x56: ('f', 'self.{0} = acc'), 0x57: ('g', '[{0}] = acc'), 0x58: ('p', 'partner.{0} = acc'),
    0x59: ('f', 'self.{0} += acc'), 0x5A: ('g', '[{0}] += acc'), 0x5B: ('p', 'partner.{0} += acc'),
    0x5C: ('f', 'self.{0} -= acc'), 0x5D: ('g', '[{0}] -= acc'), 0x5E: ('p', 'partner.{0} -= acc'),
    0x5F: ('f', 'self.{0} &= acc'), 0x60: ('g', '[{0}] &= acc'), 0x61: ('p', 'partner.{0} &= acc'),
    0x62: ('f', 'self.{0} |= acc'), 0x63: ('g', '[{0}] |= acc'), 0x64: ('p', 'partner.{0} |= acc'),
    0x65: ('f', 'self.{0} ^= acc'), 0x66: ('g', '[{0}] ^= acc'), 0x67: ('p', 'partner.{0} ^= acc'),
    0x68: ('w', 'if acc >=u {0}', 'T'), 0x69: ('f', 'if acc >=u self.{0}', 'T'),
    0x6A: ('g', 'if acc >=u [{0}]', 'T'), 0x6B: ('p', 'if acc >=u partner.{0}', 'T'),
    0x6C: ('', 'if acc >=u random()', 'T'), 0x6D: ('w', 'if acc <u {0}', 'T'),
    0x6E: ('f', 'if acc <u self.{0}', 'T'), 0x6F: ('g', 'if acc <u [{0}]', 'T'),
    0x70: ('p', 'if acc <u partner.{0}', 'T'), 0x71: ('', 'if acc <u random()', 'T'),
    0x72: ('w', 'if acc == {0}', 'T'), 0x73: ('f', 'if acc == self.{0}', 'T'),
    0x74: ('g', 'if acc == [{0}]', 'T'), 0x75: ('p', 'if acc == partner.{0}', 'T'),
    0x76: ('', 'if acc == random()', 'T'), 0x77: ('w', 'if acc != {0}', 'T'),
    0x78: ('f', 'if acc != self.{0}', 'T'), 0x79: ('g', 'if acc != [{0}]', 'T'),
    0x7A: ('p', 'if acc != partner.{0}', 'T'), 0x7B: ('', 'if acc != random()', 'T'),
    0x7C: ('ws', 'if acc >=s {0}', 'T'), 0x7D: ('f', 'if acc >=s self.{0}', 'T'),
    0x7E: ('g', 'if acc >=s [{0}]', 'T'), 0x7F: ('p', 'if acc >=s partner.{0}', 'T'),
    0x80: ('', 'if acc >=s random()', 'T'), 0x81: ('ws', 'if acc <s {0}', 'T'),
    0x82: ('f', 'if acc <s self.{0}', 'T'), 0x83: ('g', 'if acc <s [{0}]', 'T'),
    0x84: ('p', 'if acc <s partner.{0}', 'T'), 0x85: ('', 'if acc <s random()', 'T'),
    0x86: ('w', 'if acc == {0}', 'C'), 0x87: ('f', 'if acc == self.{0}', 'C'),
    0x88: ('g', 'if acc == [{0}]', 'C'), 0x89: ('p', 'if acc == partner.{0}', 'C'),
    0x8B: ('w', 'if acc != {0}', 'C'), 0x8C: ('f', 'if acc != self.{0}', 'C'),
    0x8D: ('g', 'if acc != [{0}]', 'C'), 0x8E: ('p', 'if acc != partner.{0}', 'C'),
    0x8F: ('', 'if acc != random()', 'C'),
    0x90: ('f', 'self.{0} +=hflip acc'), 0x91: ('g', '[{0}] +=hflip acc'), 0x92: ('p', 'partner.{0} +=hflip acc'),
    0x93: ('f', 'self.{0} -=hflip acc'), 0x94: ('g', '[{0}] -=hflip acc'), 0x95: ('p', 'partner.{0} -=hflip acc'),
    0x96: ('', 'set_partner acc'),          # its own op (not the generic store 56 into the `partner` field)
    0x97: ('mw', 'acc = bit({1} & {0})'), 0x98: ('mf', 'acc = bit(self.{1} & {0})'),
    0x99: ('mg', 'acc = bit([{1}] & {0})'), 0x9A: ('mp', 'acc = bit(partner.{1} & {0})'),
    0x9B: ('', 'acc = random()&1'),
    0x9C: ('mf', 'self.{1} = setbit(self.{1}, {0}, acc)'), 0x9D: ('mg', '[{1}] = setbit([{1}], {0}, acc)'),
    0x9E: ('mp', 'partner.{1} = setbit(partner.{1}, {0}, acc)'),
    0xA0: ('mg', '[{1}] &= (acc ? {0} : 0)'), 0xA3: ('mg', '[{1}] |= (acc ? {0} : 0)'),
    0xA4: ('mp', 'partner.{1} |= (acc ? {0} : 0)'), 0xA5: ('mf', 'self.{1} ^= (acc ? {0} : 0)'),
    0xA7: ('mp', 'partner.{1} ^= (acc ? {0} : 0)'),
    0xA8: ('mw', 'if acc == bit({1} & {0})', 'T'), 0xA9: ('mf', 'if acc == bit(self.{1} & {0})', 'T'),
    0xAA: ('mg', 'if acc == bit([{1}] & {0})', 'T'), 0xAB: ('mp', 'if acc == bit(partner.{1} & {0})', 'T'),
    0xAC: ('', 'if acc == random()&1', 'T'), 0xAD: ('mw', 'if acc == bit({1} & {0}) #AD', 'T'),
    0xAE: ('mf', 'if acc != bit(self.{1} & {0})', 'T'), 0xAF: ('mg', 'if acc != bit([{1}] & {0})', 'T'),
    0xB0: ('mp', 'if acc != bit(partner.{1} & {0})', 'T'),
    0xB2: ('mw', 'if acc == bit({1} & {0})', 'C'), 0xB3: ('mf', 'if acc == bit(self.{1} & {0})', 'C'),
    0xB4: ('mg', 'if acc == bit([{1}] & {0})', 'C'), 0xB5: ('mp', 'if acc != bit(partner.{1} & {0})', 'C'),
    0xB6: ('', 'if acc == rng17()&1', 'T'), 0xB8: ('mf', 'if acc != bit(self.{1} & {0})', 'C'),
    0xB9: ('mg', 'if acc != bit([{1}] & {0})', 'C'), 0xBB: ('', 'if acc != random()&1', 'C'),
    0xBC: ('f', 'self.{0} = (acc <<= 8)'), 0xBD: ('g', '[{0}] = (acc <<= 8)'), 0xBE: ('p', 'partner.{0} = (acc <<= 8)'),
    0xBF: ('b', 'if scan_up_vik(f={0})', 'T'), 0xC0: ('b', 'if scan_c0(f={0})', 'T'),
    0xC1: ('b', 'if scan_x_vik_fwd(f={0})', 'T'), 0xC2: ('b', 'if scan_side_vik({0})', 'T'),
    0xC3: ('b', 'if !scan_up_vik(f={0})', 'T'), 0xC4: ('b', 'if !scan_down_vik(f={0})', 'T'),
    0xC5: ('b', 'if !scan_x_vik_fwd(f={0})', 'T'), 0xC6: ('b', 'if !scan_x_vik_back(f={0})', 'T'),
    0xC7: ('', 'spawn_rec[+0] = acc'), 0xC8: ('', 'spawn_rec[+2] = acc'),
    0xC9: ('', 'spawn_rec[+A] = acc &= 0xCDFF'), 0xCA: ('', 'spawn_rec[+C] = acc'),
    0xCD: ('', 'if in_viewport(partner)', 'T'), 0xCE: ('', 'if !in_viewport(self)', 'T'),
    0xCF: ('', 'if !in_viewport(partner)', 'T'),
    0xD0: ('b', 'if search_vik_d0(f={0})', 'T'), 0xD1: ('b', 'search_vik_down(f={0})', 'T'),
    0xD2: ('', 'pw_chars = password[level]'), 0xD3: ('', 'level_load = find_password(pw)'),
    0xD5: ('r', 'music_start {0}'), 0xD7: ('brr', 'sfx_stop_slots({0}) pad {1},{2}'),
}
CHANNEL_OPS = {0x14, 0x15, 0x16, 0x34, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x50, 0x41, 0x44, 0x45, 0x48, 0x49, 0x4A, 0xD4}
CONTROL = {0x00: 'yield', 0x01: 'nop', 0x06: 'return', 0x0F: 'exit', 0x10: 'despawn'}
# consumer ops that take `acc` as their single right-hand value: sugar
SUGAR_CONSUMERS = ({0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x60, 0x61, 0x62, 0x63,
                    0x64, 0x65, 0x66, 0x67, 0x96, 0xC7, 0xC8, 0xCA,
                    0x9C, 0x9D, 0x9E, 0xA0, 0xA3, 0xA4, 0xA5, 0xA7, 0xBC, 0xBD, 0xBE}   # setbit / mask-merge / <<8 stores
                   | set(range(0x68, 0x90)))
LOAD_OPS = {0x51, 0x52, 0x53, 0x54, 0x55, 0x97, 0x98, 0x99, 0x9A}

SLOT_RX = {
    'w': r'(0x[0-9A-Fa-f]+|\d+)', 'ws': r'(-?0x[0-9A-Fa-f]+|-?\d+)', 'b': r'(0x[0-9A-Fa-f]+|\d+)',
    'f': r'([A-Za-z_][A-Za-z0-9_?]*(?:#[0-9A-Fa-f]{2})?)', 'p': r'([A-Za-z_][A-Za-z0-9_?]*(?:#[0-9A-Fa-f]{2})?)',
    'g': r'([A-Za-z_][A-Za-z0-9_]*|[0-9A-Fa-f]{4})', 'm': r'(0x[0-9A-Fa-f]+(?:#[0-9A-Fa-f]{2})?|mask\?[0-9A-Fa-f]{2})',
    'r': r'(0x[0-9A-Fa-f]+|\d+)', 'v': r'(-?\d+)',
}
ACC_RX = (r'(0x[0-9A-Fa-f]+|\d+|self\.[A-Za-z_][A-Za-z0-9_?]*(?:#[0-9A-Fa-f]{2})?|partner\.[A-Za-z_][A-Za-z0-9_?]*(?:#[0-9A-Fa-f]{2})?'
          r'|\[(?:[A-Za-z_][A-Za-z0-9_]*|[0-9A-Fa-f]{4})\]|random\(\)|bit\([^()]*\))')
TARGET_RX = r'([A-Za-z_][A-Za-z0-9_]*|=[0-9A-Fa-f]{4})'


def imm(v):
    return str(v) if v <= 9 else f'0x{v:X}'


class Table:
    """Render/parse of the fixed-slot statements (OPS)."""
    def __init__(self, names):
        self.N = names
        self.rx = {}       # op -> compiled regex (plain form)
        self.rx_sugar = {} # op -> compiled regex with `acc` replaced by an acc-expression
        self.order = {}    # op -> slot index of each regex group, in template order
        for op, ent in OPS.items():
            slots, tpl = ent[0], ent[1]
            rx, order = self._tpl_rx(tpl, slots)
            self.rx[op] = re.compile('^' + rx + self._tail_rx(ent) + '$'); self.order[op] = order
            if op in SUGAR_CONSUMERS and tpl.count('acc') == 1:
                rx2, _ = self._tpl_rx(tpl.replace('acc', '\x00'), slots)
                self.rx_sugar[op] = re.compile('^' + rx2.replace('\x00', ACC_RX) + self._tail_rx(ent) + '$')

    @staticmethod
    def _tail_rx(ent):
        fl = ent[2] if len(ent) > 2 else ''
        if 'T' in fl: return r' goto ' + TARGET_RX
        if 'C' in fl: return r' call ' + TARGET_RX
        return ''

    @staticmethod
    def _tpl_rx(tpl, slots):
        """template -> (regex text, [slot index per group in template order])"""
        out = ''; i = 0; order = []; group_of = {}
        while i < len(tpl):
            if tpl[i] == '{':
                j = tpl.index('}', i); k = int(tpl[i+1:j]); i = j + 1
                if k in group_of:                      # a repeated slot must carry the same text: backreference
                    out += '\\%d' % group_of[k]; continue
                group_of[k] = len(order) + 1; out += SLOT_RX[slots_kind(slots, k)]; order.append(k)
            else:
                out += re.escape(tpl[i]); i += 1
        return out, order

    def render_slots(self, op, body):
        slots = OPS[op][0]; vals = []; o = 0
        for kd in slot_kinds(slots):
            if kd == 'w': vals.append(imm(struct.unpack_from('<H', body, o)[0])); o += 2
            elif kd == 'ws':
                v = struct.unpack_from('<H', body, o)[0]; vals.append(str(v - 0x10000 if v >= 0x8000 else v)); o += 2
            elif kd in ('b', 'r'): vals.append(imm(body[o])); o += 1
            elif kd == 'v': vals.append(str(body[o] - 256 if body[o] >= 128 else body[o])); o += 1
            elif kd in ('f', 'p'): vals.append(self.N.fld(body[o])); o += 1
            elif kd == 'g': vals.append(self.N.mem(struct.unpack_from('<H', body, o)[0])); o += 2
            elif kd == 'm': vals.append(self.N.mask(body[o])); o += 1
        if o != len(body): raise ValueError(f'op {op:02X}: operand length {len(body)} != slots')
        return vals

    def parse_slots(self, op, vals):
        slots = OPS[op][0]; out = b''
        for k, v in enumerate(vals):
            kd = slots_kind(slots, k)
            if kd == 'w': out += struct.pack('<H', int(v, 0))
            elif kd == 'ws': out += struct.pack('<H', int(v, 0) & 0xFFFF)
            elif kd in ('b', 'r'): out += bytes([int(v, 0)])
            elif kd == 'v': out += bytes([int(v) & 0xFF])
            elif kd in ('f', 'p'): out += bytes([self.N.fld_parse(v)])
            elif kd == 'g': out += struct.pack('<H', self.N.mem_parse(v))
            elif kd == 'm': out += bytes([self.N.mask_parse(v)])
        return out

    def render(self, op, body):
        ent = OPS[op]
        return ent[1].format(*self.render_slots(op, body))


def slot_kinds(slots):
    """slots string -> list of kinds ('ws' is two chars)."""
    kinds = []; i = 0
    while i < len(slots):
        if slots[i] == 'w' and i + 1 < len(slots) and slots[i+1] == 's': kinds.append('ws'); i += 2
        else: kinds.append(slots[i]); i += 1
    return kinds


def slots_kind(slots, k):
    return slot_kinds(slots)[k]


# ------------------------------------------------------- channel operands --
CH_LEN = {0: 2, 1: 1, 2: 2, 3: 1, 4: 0, 5: 0, 6: 1, 7: 2}
SET_LEN = {1: 1, 2: 2, 3: 1, 5: 0}
CHV_RX = (r'(0x[0-9A-Fa-f]+|\d+|self\.[A-Za-z_][A-Za-z0-9_?]*(?:#[0-9A-Fa-f]{2})?|partner\.[A-Za-z_][A-Za-z0-9_?]*(?:#[0-9A-Fa-f]{2})?'
          r'|\[(?:[A-Za-z_][A-Za-z0-9_]*|[0-9A-Fa-f]{4})\]|random\(\)|ch5|ub6\([0-9A-Fa-f]{2}\)|ub7\([0-9A-Fa-f]{4}\))')
SETV_RX = r'(self\.[A-Za-z_][A-Za-z0-9_?]*(?:#[0-9A-Fa-f]{2})?|partner\.[A-Za-z_][A-Za-z0-9_?]*(?:#[0-9A-Fa-f]{2})?|\[(?:[A-Za-z_][A-Za-z0-9_]*|[0-9A-Fa-f]{4})\]|drop)'


class Channels:
    def __init__(self, names): self.N = names

    def ch_render(self, chan, body, o):
        if chan == 0: return imm(struct.unpack_from('<H', body, o)[0]), 2
        if chan == 1: return 'self.' + self.N.fld(body[o]), 1
        if chan == 2: return '[' + self.N.mem(struct.unpack_from('<H', body, o)[0]) + ']', 2
        if chan == 3: return 'partner.' + self.N.fld(body[o]), 1
        if chan == 4: return 'random()', 0
        if chan == 5: return 'ch5', 0
        if chan == 6: return f'ub6({body[o]:02X})', 1
        if chan == 7: return f'ub7({struct.unpack_from("<H", body, o)[0]:04X})', 2
        raise ValueError('bad channel')
    def ch_parse(self, tok):
        """-> (chan, bytes)"""
        if tok.startswith('self.'): return 1, bytes([self.N.fld_parse(tok[5:])])
        if tok.startswith('partner.'): return 3, bytes([self.N.fld_parse(tok[8:])])
        if tok.startswith('['): return 2, struct.pack('<H', self.N.mem_parse(tok[1:-1]))
        if tok == 'random()': return 4, b''
        if tok == 'ch5': return 5, b''
        if tok.startswith('ub6('): return 6, bytes([int(tok[4:-1], 16)])
        if tok.startswith('ub7('): return 7, struct.pack('<H', int(tok[4:-1], 16))
        return 0, struct.pack('<H', int(tok, 0))
    def set_render(self, chan, body, o):
        if chan == 1: return 'self.' + self.N.fld(body[o]), 1
        if chan == 2: return '[' + self.N.mem(struct.unpack_from('<H', body, o)[0]) + ']', 2
        if chan == 3: return 'partner.' + self.N.fld(body[o]), 1
        if chan == 5: return 'drop', 0
        raise ValueError('bad setter channel')
    def set_parse(self, tok):
        if tok.startswith('self.'): return 1, bytes([self.N.fld_parse(tok[5:])])
        if tok.startswith('partner.'): return 3, bytes([self.N.fld_parse(tok[8:])])
        if tok.startswith('['): return 2, struct.pack('<H', self.N.mem_parse(tok[1:-1]))
        if tok == 'drop': return 5, b''
        raise ValueError(f'bad setter {tok!r}')

    def pair_render(self, body, o, setter=False):
        m = body[o]; o += 1
        if m & 0xC0: raise ValueError(f'channel mode byte {m:02X} has high bits')
        f = self.set_render if setter else self.ch_render
        a, la = f(m & 7, body, o); o += la
        b, lb = f((m >> 3) & 7, body, o); o += lb
        return a, b, o
    def pair_parse(self, a, b, setter=False):
        f = self.set_parse if setter else self.ch_parse
        ca, ba = f(a); cb, bb = f(b)
        return bytes([ca | (cb << 3)]) + ba + bb

    # per-op forms -------------------------------------------------------
    def render(self, op, body):
        if op in (0x15, 0x16, 0x34):
            a, b, o = self.pair_render(body, 0, setter=True); self._end(o, body)
            src = {0x15: 'active_vik', 0x16: 'partner', 0x34: 'nearest_vik'}[op]
            return f'{a}, {b} = delta({src})'
        if op in (0x29, 0x2A, 0x2B, 0x50):
            x, y, o = self.pair_render(body, 0); m2 = body[o]; o += 1
            if m2 & 0xF8: raise ValueError('single channel mode has extra bits')
            v, lv = self.ch_render(m2 & 7, body, o); self._end(o + lv, body)
            return {0x29: f'tile[{x},{y}] = {v}', 0x2A: f'tile[{x},{y}] = hi10 | {v}',
                    0x2B: f'tile[{x},{y}] = lo | swap({v})', 0x50: f'cmdq_push(8, x={x}, y={y}, p={v})'}[op]
        if op == 0xD4:
            x, y, o = self.pair_render(body, 0); self._end(o + 1, body)
            return f'aim(x={x}, y={y}, thr={imm(body[o])})'
        if op in (0x41, 0x44):
            w0 = body[0]; o = 1
            if w0 & 0xC0: raise ValueError('mode bits')
            a, la = self.ch_render(w0 & 7, body, o); o += la
            b, lb = self.ch_render((w0 >> 3) & 7, body, o); o += lb
            x, y, o = self.pair_render(body, o); self._end(o, body)
            return f'{"text" if op == 0x41 else "text_menu"}(id={a}, edge={b}, x={x}, y={y})'
        if op == 0x45:
            w0 = body[0]; o = 1
            if w0 & 0xF8: raise ValueError('mode bits')
            a, la = self.ch_render(w0 & 7, body, o); o += la
            x, y, o = self.pair_render(body, o); self._end(o, body)
            return f'cmdq_push(0xA, id={a}, x={x}, y={y})'
        if op == 0x48:
            x, y, o = self.pair_render(body, 0); self._end(o, body)
            return f'vel_to(x={x}, y={y})'
        if op in (0x26, 0x28):
            a, b, o = self.pair_render(body, 0); l1, l2, o = self.pair_render(body, o, setter=True); self._end(o, body)
            return f'{l1}, {l2} = {"quad" if op == 0x26 else "cell8"}({a}, {b})'
        if op == 0x27:
            a, b, o = self.pair_render(body, 0); m2 = body[o]; o += 1
            if m2 & 0xF8: raise ValueError('setter mode bits')
            l1, u1 = self.set_render(m2 & 7, body, o); self._end(o + u1, body)
            return f'{l1} = tile_type({a}, {b})'
        if op == 0x14:
            x, y, o = self.pair_render(body, 0); pool, fl, o = self.pair_render(body, o)
            self._end(o + 1, body)
            return f'spawn(t={body[o]:02X}, x={x}, y={y}, pool={pool}, fl={fl})'
        if op in (0x49, 0x4A):
            x, y, o = self.pair_render(body, 0); self._end(o + 1, body)
            return f'if probe_at{"" if op == 0x49 else "2"}({body[o]:02X}, x={x}, y={y})'
        raise ValueError(f'channel op {op:02X} unhandled')

    @staticmethod
    def _end(o, body):
        if o != len(body): raise ValueError(f'channel operands: {o} != {len(body)}')

    RX = {
        'delta': re.compile(rf'^{SETV_RX}, {SETV_RX} = delta\((active_vik|partner|nearest_vik)\)$'),
        'tile': re.compile(rf'^tile\[{CHV_RX},{CHV_RX}\] = (hi10 \| |lo \| swap\()?{CHV_RX}\)?$'),
        'cmdq8': re.compile(rf'^cmdq_push\(8, x={CHV_RX}, y={CHV_RX}, p={CHV_RX}\)$'),
        'aim': re.compile(rf'^aim\(x={CHV_RX}, y={CHV_RX}, thr=(0x[0-9A-Fa-f]+|\d+)\)$'),
        'text': re.compile(rf'^(text|text_menu)\(id={CHV_RX}, edge={CHV_RX}, x={CHV_RX}, y={CHV_RX}\)$'),
        'cmdqA': re.compile(rf'^cmdq_push\(0xA, id={CHV_RX}, x={CHV_RX}, y={CHV_RX}\)$'),
        'vel_to': re.compile(rf'^vel_to\(x={CHV_RX}, y={CHV_RX}\)$'),
        'quad': re.compile(rf'^{SETV_RX}, {SETV_RX} = (quad|cell8)\({CHV_RX}, {CHV_RX}\)$'),
        'ttype': re.compile(rf'^{SETV_RX} = tile_type\({CHV_RX}, {CHV_RX}\)$'),
        'spawn': re.compile(rf'^spawn\(t=([0-9A-Fa-f]{{2}}), x={CHV_RX}, y={CHV_RX}, pool={CHV_RX}, fl={CHV_RX}\)$'),
        'probe_at': re.compile(rf'^if probe_at(2?)\(([0-9A-Fa-f]{{2}}), x={CHV_RX}, y={CHV_RX}\) goto {TARGET_RX}$'),
    }

    def parse(self, text):
        """-> (op, body_bytes, target_token_or_None) or None if not a channel statement."""
        R = self.RX
        m = R['delta'].match(text)
        if m:
            op = {'active_vik': 0x15, 'partner': 0x16, 'nearest_vik': 0x34}[m.group(3)]
            return op, self.pair_parse(m.group(1), m.group(2), setter=True), None
        m = R['tile'].match(text)
        if m:
            kind = m.group(3) or ''
            op = 0x29 if not kind else (0x2A if kind.startswith('hi10') else 0x2B)
            c, b = self.ch_parse(m.group(4))
            return op, self.pair_parse(m.group(1), m.group(2)) + bytes([c]) + b, None
        m = R['cmdq8'].match(text)
        if m:
            c, b = self.ch_parse(m.group(3)); return 0x50, self.pair_parse(m.group(1), m.group(2)) + bytes([c]) + b, None
        m = R['aim'].match(text)
        if m: return 0xD4, self.pair_parse(m.group(1), m.group(2)) + bytes([int(m.group(3), 0)]), None
        m = R['text'].match(text)
        if m:
            ca, ba = self.ch_parse(m.group(2)); cb, bb = self.ch_parse(m.group(3))
            return (0x41 if m.group(1) == 'text' else 0x44), bytes([ca | (cb << 3)]) + ba + bb + self.pair_parse(m.group(4), m.group(5)), None
        m = R['cmdqA'].match(text)
        if m:
            ca, ba = self.ch_parse(m.group(1)); return 0x45, bytes([ca]) + ba + self.pair_parse(m.group(2), m.group(3)), None
        m = R['vel_to'].match(text)
        if m: return 0x48, self.pair_parse(m.group(1), m.group(2)), None
        m = R['quad'].match(text)
        if m:
            op = 0x26 if m.group(3) == 'quad' else 0x28
            return op, self.pair_parse(m.group(4), m.group(5)) + self.pair_parse(m.group(1), m.group(2), setter=True), None
        m = R['ttype'].match(text)
        if m:
            c, b = self.set_parse(m.group(1)); return 0x27, self.pair_parse(m.group(2), m.group(3)) + bytes([c]) + b, None
        m = R['spawn'].match(text)
        if m:
            return 0x14, self.pair_parse(m.group(2), m.group(3)) + self.pair_parse(m.group(4), m.group(5)) + bytes([int(m.group(1), 16)]), None
        m = R['probe_at'].match(text)
        if m:
            op = 0x4A if m.group(1) else 0x49
            return op, self.pair_parse(m.group(3), m.group(4)) + bytes([int(m.group(2), 16)]), m.group(5)
        return None


# ------------------------------------------------------------ decompiler --
LINE_O = re.compile(r'^o ([0-9A-Fa-f]{2})((?: \S+)*)$')


def parse_o_line(raw):
    """'o XX [hex] [S_x|A_x|P_x|=HHHH|d9]' -> (op, body_hex_tokens, symbolic tokens in order)."""
    m = LINE_O.match(raw)
    if not m: raise ValueError(raw)
    op = int(m.group(1), 16); toks = m.group(2).split()
    return op, toks


def decompile_text(cid, text, names):
    """canonical .lvsf text -> .lvd text (statements + verbatim rest)."""
    T = Table(names); C = Channels(names)
    out = [f'; .lvd v0 — chunk {cid:04X} (level C of the logic editor; lvsd.py)',
           f'; statements are a 1:1 layer over the free-form .lvsf: compile == the v1.5 bytes']
    lines = text.splitlines()
    # the address of every label = the address of the code line that follows it,
    # from the assembler's own layout of this very text (so an edited text shows
    # its new addresses and an unedited one the original ones)
    lf = mod('lvs_full'); line_map = []
    cwd = os.getcwd(); os.chdir(ROOT)
    try: lf.compile_free(text, line_map)
    except Exception: line_map = []
    finally: os.chdir(cwd)
    addr_after = {}
    for ln, a in reversed(line_map): addr_after[ln] = a
    nxt = None
    for ln in range(len(lines), 0, -1):
        if ln in addr_after: nxt = addr_after[ln]
        addr_after[ln] = nxt
    stats = {'stmt': 0, 'sugar': 0, 'states': 0, 'named': 0}
    def tgt_name(tok):
        if tok.startswith('='): return tok
        return names.state(cid, tok)
    pending = None   # (op, body, text) of an `acc = X` load awaiting a consumer
    def flush():
        nonlocal pending
        if pending is not None:
            out.append('    ' + pending[2]); pending = None
    for lineno, line in enumerate(lines, 1):
        code, _, comment = line.partition(';')
        raw = code.strip()
        if not raw:
            continue
        p = raw.split()
        if p[0] == 'chunk':
            flush(); out.append(raw); continue
        if p[0] == 'record':
            flush()
            t = int(p[1], 16); cs = p[4].split('=', 1)[1]
            entry = tgt_name(cs) if cs.startswith('S_') else cs
            out.append(f'class {names.cls(cid, t)} record={t:02X} {p[2]} {p[3]} entry={entry} {p[5]}')
            continue
        if raw.endswith(':') and len(p) == 1:
            flush()
            lbl = p[0][:-1]
            if lbl.startswith('S_'):
                nm = names.state(cid, lbl); stats['states'] += 1; stats['named'] += (nm != lbl)
                a = addr_after.get(lineno)
                out.append(f'state {nm}:' + (f'   ; @{a:04X}' if a is not None else ''))
            else:
                out.append(raw)
            continue
        if len(p) == 3 and p[1] == '=' and p[0].startswith('S_') and '+' in p[2]:
            flush()                                   # alias between code labels: both sides get the state names
            base, off = p[2].split('+')
            out.append(f'alias {tgt_name(p[0])} = {tgt_name(base) if base.startswith("S_") else base}+{off}')
            continue
        if p[0] in ('blob', 'a') or (len(p) == 3 and p[1] == '='):
            flush(); out.append(raw); continue
        if p[0] != 'o':
            raise ValueError(f'{cid:X}.lvsf:{lineno}: unexpected line {raw!r}')
        op, toks = parse_o_line(raw)
        stats['stmt'] += 1
        sym = [t for t in toks if t.startswith(('S_', 'A_', 'P_', '=')) or t == 'd9']
        hexs = [t for t in toks if not (t.startswith(('S_', 'A_', 'P_', '=')) or t == 'd9')]
        body = bytes.fromhex(''.join(hexs))
        # --- control / special ops
        if op in CONTROL:
            flush(); out.append('    ' + CONTROL[op]); continue
        if op == 0x03 or op == 0x05:
            flush(); out.append(f'    {"goto" if op == 0x03 else "call"} {tgt_name(sym[0])}'); continue
        if op == 0x19:
            flush(); out.append(f'    anim {sym[0]}'); continue
        if op == 0x13:
            flush()
            if sym and sym[0] == 'd9': out.append(f'    op13 d9 {sym[1]}')
            else: out.append(f'    op13 {body.hex()}')
            continue
        if op in CHANNEL_OPS:
            stext = C.render(op, body)
            if op in (0x49, 0x4A): stext += f' goto {tgt_name(sym[0])}'
            flush(); out.append('    ' + stext); continue
        if op not in OPS:
            flush(); out.append(f'    op{op:02X} {body.hex()}' + (f' goto {tgt_name(sym[0])}' if sym else '')); continue
        ent = OPS[op]; fl = ent[2] if len(ent) > 2 else ''
        stext = T.render(op, body)
        if 'T' in fl: stext += f' goto {tgt_name(sym[0])}'
        elif 'C' in fl: stext += f' call {tgt_name(sym[0])}'
        # --- acc sugar: a load immediately followed by a single-acc consumer
        if op in LOAD_OPS:
            flush(); pending = (op, body, stext); continue
        if pending is not None and op in SUGAR_CONSUMERS and op in T.rx_sugar and ent[1].count('acc') == 1:
            xval = pending[2][len('acc = '):]
            out.append('    ' + stext.replace('acc', xval, 1)); pending = None; stats['sugar'] += 1
            continue
        flush(); out.append('    ' + stext)
    flush()
    out, nsw = fold_switches(out)
    stats['switch'] = nsw
    out, nsay = fold_say(out)
    stats['say'] = nsay
    out.append(f'; stats: {stats["stmt"]} statements, {stats["sugar"]} folded loads, {nsw} switch blocks, {nsay} say lines, {stats["states"]} states ({stats["named"]} named)')
    return '\n'.join(out) + '\n', stats


# switch / select: a run of two or more consecutive `if` statements that compare
# the SAME value with literals and branch — the field-loaded form
#   acc = X; if acc == N goto L      (52/53/54 + 72)      ->  switch X:
# and the literal-loaded form
#   acc = N; if acc == X goto L      (51 + 73/74/75)      ->  select X:
# Both are the same test; the two keywords keep the two opcode sequences apart
# so the fold is exact. Cases are `N -> L` lines indented by eight spaces.
VAL_RX = r'(self\.[A-Za-z_][A-Za-z0-9_?]*(?:#[0-9A-Fa-f]{2})?|partner\.[A-Za-z_][A-Za-z0-9_?]*(?:#[0-9A-Fa-f]{2})?|\[(?:[A-Za-z_][A-Za-z0-9_]*|[0-9A-Fa-f]{4})\])'
LIT_RX = r'(0x[0-9A-Fa-f]+|\d+)'
SW_RX = re.compile(rf'^    if {VAL_RX} == {LIT_RX} goto ([A-Za-z_][A-Za-z0-9_]*|=[0-9A-Fa-f]{{4}})$')
SEL_RX = re.compile(rf'^    if {LIT_RX} == {VAL_RX} goto ([A-Za-z_][A-Za-z0-9_]*|=[0-9A-Fa-f]{{4}})$')


# say: the speech-bubble idiom — seven statements in a row (no label between):
#   set_partner P                        acc = P; op 96
#   [0206], [0208] = delta(partner)      op 16 into the text column/row globals
#   [0208] -= N   (or += N)              acc = N; op 5D (or 5A) on [0208]
#   cmdq_push(6, X)                      op 46
#   text(id=ID, edge=E, x=[0206], y=[0208])   op 41 (id a literal, E a literal or a field)
#   cmdq_push(4)                         op 43
#   cmdq_push(2)                         op 42
# -> say partner=P dy=-N cmd=X id=ID edge=E     (dy=+N for the += form)
SAY_RX = [
    re.compile(r'^    set_partner (0x[0-9A-Fa-f]+|\d+)$'),
    re.compile(r'^    \[0206\], \[0208\] = delta\(partner\)$'),
    re.compile(r'^    \[0208\] (-=|\+=) (0x[0-9A-Fa-f]+|\d+)$'),
    re.compile(r'^    cmdq_push\(6, (0x[0-9A-Fa-f]+|\d+)\)$'),
    re.compile(r'^    text\(id=(0x[0-9A-Fa-f]+|\d+), edge=(0x[0-9A-Fa-f]+|\d+|self\.[A-Za-z_][A-Za-z0-9_?]*(?:#[0-9A-Fa-f]{2})?), x=\[0206\], y=\[0208\]\)$'),
    re.compile(r'^    cmdq_push\(4\)$'),
    re.compile(r'^    cmdq_push\(2\)$'),
]
SAY_LINE_RX = re.compile(r'^say partner=(0x[0-9A-Fa-f]+|\d+) dy=([-+])(0x[0-9A-Fa-f]+|\d+) cmd=(0x[0-9A-Fa-f]+|\d+) id=(0x[0-9A-Fa-f]+|\d+) edge=(0x[0-9A-Fa-f]+|\d+|self\.[A-Za-z_][A-Za-z0-9_?]*(?:#[0-9A-Fa-f]{2})?)$')


def fold_say(lines):
    out = []; i = 0; n = 0
    while i < len(lines):
        if i + 7 <= len(lines):
            ms = [rx.match(lines[i + k]) for k, rx in enumerate(SAY_RX)]
            if all(ms):
                sign = '-' if ms[2].group(1) == '-=' else '+'
                out.append(f'    say partner={ms[0].group(1)} dy={sign}{ms[2].group(2)} cmd={ms[3].group(1)} id={ms[4].group(1)} edge={ms[4].group(2)}')
                i += 7; n += 1; continue
        out.append(lines[i]); i += 1
    return out, n


def expand_say(raw):
    m = SAY_LINE_RX.match(raw)
    if not m: return None
    P, sign, N, X, ID, E = m.groups()
    return [f'set_partner {P}', '[0206], [0208] = delta(partner)', f'[0208] {"-=" if sign == "-" else "+="} {N}',
            f'cmdq_push(6, {X})', f'text(id={ID}, edge={E}, x=[0206], y=[0208])', 'cmdq_push(4)', 'cmdq_push(2)']


def fold_switches(lines):
    out = []; i = 0; n = 0
    while i < len(lines):
        m = SW_RX.match(lines[i]); kw = 'switch'
        if not m:
            m = SEL_RX.match(lines[i]); kw = 'select'
        if not m:
            out.append(lines[i]); i += 1; continue
        val = m.group(1) if kw == 'switch' else m.group(2)
        j = i; cases = []
        while j < len(lines):
            mm = (SW_RX if kw == 'switch' else SEL_RX).match(lines[j])
            if not mm: break
            v = mm.group(1) if kw == 'switch' else mm.group(2)
            if v != val: break
            cases.append((mm.group(2) if kw == 'switch' else mm.group(1), mm.group(3))); j += 1
        if len(cases) < 2:
            out.append(lines[i]); i += 1; continue
        out.append(f'    {kw} {val}:')
        for lit, tgt in cases: out.append(f'        {lit} -> {tgt}')
        n += 1; i = j
    return out, n


# --------------------------------------------------------------- compiler --
class Lowerer:
    def __init__(self, names):
        self.N = names; self.T = Table(names); self.C = Channels(names)
        self.ctl = {v: k for k, v in CONTROL.items()}
        # candidate regexes are picked by the template's literal prefix (up to
        # its first slot, or up to `acc` for the sugar form) — a cheap filter
        # before the anchored match
        self.cands = []
        for op, rx in self.T.rx.items():
            tpl = OPS[op][1]; cut = tpl.find('{')
            self.cands.append((tpl if cut < 0 else tpl[:cut], op, rx, False))
        for op, rx in self.T.rx_sugar.items():
            tpl = OPS[op][1]; cut = min(x for x in (tpl.find('{'), tpl.find('acc')) if x >= 0)
            self.cands.append((tpl[:cut], op, rx, True))

    def label(self, tok):
        """state name -> compile_free label token."""
        if tok.startswith('='): return tok
        if tok.startswith(('S_', 'A_', 'P_')): return tok
        return 'S_' + tok

    def acc_load(self, x):
        """acc-expression text -> (op, body) of the load that produced it."""
        if x.startswith('self.'): return 0x52, bytes([self.N.fld_parse(x[5:])])
        if x.startswith('partner.'): return 0x54, bytes([self.N.fld_parse(x[8:])])
        if x.startswith('['): return 0x53, struct.pack('<H', self.N.mem_parse(x[1:-1]))
        if x == 'random()': return 0x55, b''
        if x.startswith('bit('):
            for op in (0x97, 0x98, 0x99, 0x9A):
                m = self.T.rx[op].match('acc = ' + x)
                if m:
                    g = list(m.groups()); order = self.T.order[op]; by_slot = [None] * len(order)
                    for gi, si in enumerate(order): by_slot[si] = g[gi]
                    return op, self.T.parse_slots(op, by_slot)
            raise ValueError(f'bad bit load {x!r}')
        return 0x51, struct.pack('<H', int(x, 0))

    def statement(self, s):
        """one statement -> list of 'o ...' lines"""
        if s in self.ctl: return [f'o {self.ctl[s]:02X}']
        w = s.split(' ', 1)
        if w[0] in ('goto', 'call') and len(w) == 2:
            return [f'o {0x03 if w[0] == "goto" else 0x05:02X} {self.label(w[1])}']
        if w[0] == 'anim': return [f'o 19 {w[1]}']
        if w[0] == 'op13':
            rest = w[1].split()
            return ['o 13 d9 ' + rest[1]] if rest[0] == 'd9' else ['o 13 ' + rest[0]]
        if re.match(r'^op[0-9A-Fa-f]{2}\b', w[0]):
            op = int(w[0][2:], 16); rest = (w[1] if len(w) > 1 else '').split()
            hexs = rest[0] if rest and not rest[0] == 'goto' else ''
            tg = rest[rest.index('goto') + 1] if 'goto' in rest else None
            return ['o %02X%s%s' % (op, (' ' + hexs) if hexs else '', (' ' + self.label(tg)) if tg else '')]
        ch = self.C.parse(s)
        if ch is not None:
            op, body, tg = ch
            return ['o %02X%s%s' % (op, (' ' + body.hex()) if body else '', (' ' + self.label(tg)) if tg else '')]
        for pref, op, rx, sugar in self.cands:
            if not s.startswith(pref): continue
            m = rx.match(s)
            if not m: continue
            g = list(m.groups()); ent = OPS[op]; fl = ent[2] if len(ent) > 2 else ''
            tg = g.pop() if ('T' in fl or 'C' in fl) else None
            lines = []
            if sugar:
                # the acc-expression sits where `acc` is in the template: its group index = number of slots before it
                tpl = ent[1]; pos = tpl.index('acc')
                nb = len({int(k) for k in re.findall(r'\{(\d+)\}', tpl[:pos])})   # distinct slots before `acc` = groups before it (a repeat is a backreference)
                x = g.pop(nb)
                lop, lbody = self.acc_load(x)
                lines.append('o %02X%s' % (lop, (' ' + lbody.hex()) if lbody else ''))
            # regex groups come in template order; parse_slots wants byte (slot) order
            order = self.T.order[op]; by_slot = [None] * len(order)
            for gi, si in enumerate(order): by_slot[si] = g[gi]
            body = self.T.parse_slots(op, by_slot)
            lines.append('o %02X%s%s' % (op, (' ' + body.hex()) if body else '', (' ' + self.label(tg)) if tg else ''))
            return lines
        raise ValueError(f'cannot parse statement: {s!r}')

    def lower(self, text):
        out = []
        block = None            # ('switch'|'select', value) while inside a case block
        for lineno, line in enumerate(text.splitlines(), 1):
            code = line.partition(';')[0].rstrip()
            raw = code.strip()
            if not raw: continue
            try:
                p = raw.split()
                if block and code.startswith('        '):               # a case line: `N -> L`
                    mc = re.match(r'^(0x[0-9A-Fa-f]+|\d+) -> ([A-Za-z_][A-Za-z0-9_]*|=[0-9A-Fa-f]{4})$', raw)
                    if not mc: raise ValueError(f'bad case line {raw!r}')
                    kw, val = block
                    stmt = f'if {val} == {mc.group(1)} goto {mc.group(2)}' if kw == 'switch' else f'if {mc.group(1)} == {val} goto {mc.group(2)}'
                    out.extend(self.statement(stmt)); continue
                block = None
                if code.startswith('    ') or code.startswith('\t'):   # indented = a statement (checked first:
                    mb = re.match(r'^(switch|select) (\S+):$', raw)     # `x = y` statements look like aliases)
                    if mb:
                        block = (mb.group(1), mb.group(2)); continue
                    if raw.startswith('say '):
                        ex = expand_say(raw)
                        if ex is None: raise ValueError(f'bad say line {raw!r}')
                        for s in ex: out.extend(self.statement(s))
                        continue
                    out.extend(self.statement(raw)); continue
                if p[0] == 'chunk': out.append(raw)
                elif p[0] == 'class':
                    kv = dict(x.split('=', 1) for x in p[2:])
                    entry = kv['entry']
                    out.append(f'record {kv["record"]} sprite={kv["sprite"]} flags={kv["flags"]} code={self.label(entry)} rest={kv["rest"]}')
                elif p[0] == 'state' and raw.endswith(':'):
                    out.append(self.label(p[1][:-1]) + ':')
                elif raw.endswith(':') and len(p) == 1: out.append(raw)
                elif p[0] == 'alias' and len(p) == 4 and p[2] == '=':
                    base, off = p[3].split('+')
                    out.append(f'{self.label(p[1])} = {self.label(base)}+{off}')
                elif p[0] in ('blob', 'a') or (len(p) == 3 and p[1] == '='): out.append(raw)
                else:
                    raise ValueError(f'unexpected line {raw!r}')
            except Exception as e:
                raise ValueError(f'line {lineno}: {e}') from e
        return '\n'.join(out) + '\n'


# ------------------------------------------------------------------- cli --
def reference_bytes(cid):
    return open(os.path.join(ROOT, f'assets_raw/chunks/dec/{cid:04d}.bin'), 'rb').read()


def canonical_text(cid):
    """assets_raw/lvs/<cid>.lvsf — a derived file (assets_raw is not in git):
    regenerated from the chunk by lvs_full.emit_free when missing."""
    p = os.path.join(ROOT, 'assets_raw', 'lvs', f'{cid:X}.lvsf')
    if not os.path.exists(p):
        lf = mod('lvs_full')
        cwd = os.getcwd(); os.chdir(ROOT)
        try: text = lf.emit_free(cid)
        finally: os.chdir(cwd)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, 'w', encoding='utf-8').write(text)
        return text
    return open(p, encoding='utf-8').read()


def compile_lvd(text, names):
    lf = mod('lvs_full')
    lowered = Lowerer(names).lower(text)
    cwd = os.getcwd(); os.chdir(ROOT)
    try: img = lf.compile_free(lowered)
    finally: os.chdir(cwd)
    return img, lowered


def check(cids):
    names = Names(); ok = True
    for cid in cids:
        text, stats = decompile_text(cid, canonical_text(cid), names)
        img, lowered = compile_lvd(text, names)
        ref = reference_bytes(cid)
        same = img == ref
        diff = next((i for i in range(min(len(img), len(ref))) if img[i] != ref[i]), None) if not same else None
        print(f'{cid:X}: {stats["stmt"]} statements, {stats["sugar"]} folded, {stats["states"]} states — '
              f'{"IDENTICAL" if same else f"DIFF at 0x{diff:04X} (len {len(img)} vs {len(ref)})"}')
        ok &= same
    return ok


def seed_names():
    """Initial dictionary: record entries -> <class>_anim / <class>_spawn."""
    dz = mod('disasm')
    d = {'classes': {'*:00': 'baleog', '*:01': 'erik', '*:02': 'olaf',
                     # from the scene / game-over work (docs2/SCENE_PORT_ENGINE_FACTS, GENESIS_ROM_INTERNALS)
                     '*:48': 'scene_ctl', '*:4A': 'geyser', '*:4E': 'crowd', '*:61': 'water_bubble',
                     '*:89': 'ship_pod', '*:D2': 'gameover_ctl', '*:D8': 'cutscene_ctl', '*:DA': 'bubble_track',
                     '*:E0': 'banner_letter', '*:E1': 'talk_spot'},
         'states': {}, 'anims': {}, 'sfx': {}}
    if os.path.exists(NAMES_PATH):
        d.update(json.load(open(NAMES_PATH, encoding='utf-8')))
    json.dump(d, open(NAMES_PATH, 'w', encoding='utf-8'), indent=1, ensure_ascii=False)   # the class names first: the state names derive from them
    nm = Names(NAMES_PATH)
    for cid in range(0x1C1, 0x1C7):
        ref = reference_bytes(cid)
        t = 0; seen = set()
        while (t + 1) * dz.REC <= len(ref):
            code = struct.unpack_from('<H', ref, t * dz.REC + 3)[0]
            if 0x600 <= code < len(ref):
                cn = nm.cls(cid, t)
                for off, suf in ((0, 'anim'), (3, 'spawn')):
                    key = f'{cid:X}:{code + off:04X}'
                    if key not in d['states'] and (code + off) not in seen:
                        d['states'][key] = f'{cn}_{suf}'; seen.add(code + off)
            elif t > 0 and code == 0: break
            t += 1
            if t > 0x400: break
    json.dump(d, open(NAMES_PATH, 'w', encoding='utf-8'), indent=1, ensure_ascii=False)
    print(f'names: {len(d["states"])} states, {len(d["classes"])} classes -> {NAMES_PATH}')


def main():
    a = sys.argv[1:]
    if not a: print(__doc__); return 1
    cmd = a[0]
    if cmd == 'seed': seed_names(); return 0
    if cmd == 'check':
        cids = [int(x, 16) for x in a[1:]] or list(range(0x1C1, 0x1C7))
        return 0 if check(cids) else 1
    names = Names()
    if cmd == 'decompile':
        cid = int(a[1], 16); text, stats = decompile_text(cid, canonical_text(cid), names)
        out = a[2] if len(a) > 2 else f'/tmp/{cid:X}.lvd'
        open(out, 'w', encoding='utf-8').write(text); print(f'wrote {out}: {stats}'); return 0
    if cmd in ('compile', 'lower'):
        text = open(a[1], encoding='utf-8').read()
        if cmd == 'lower':
            low = Lowerer(names).lower(text); out = a[2] if len(a) > 2 else a[1] + '.lvsf'
            open(out, 'w', encoding='utf-8').write(low); print('wrote', out); return 0
        img, _ = compile_lvd(text, names); out = a[2] if len(a) > 2 else a[1] + '.bin'
        open(out, 'wb').write(img); print(f'wrote {out} ({len(img)} bytes)'); return 0
    print(__doc__); return 1


if __name__ == '__main__':
    sys.exit(main())
