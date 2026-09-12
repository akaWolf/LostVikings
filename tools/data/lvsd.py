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
  lvsd.py ref [out.md]                 write the statement reference (LVD_REFERENCE.md)
  lvsd.py rename CID NAME|@ADDR NEW [--all]   rename a state / anim / palette label (by address in the dictionary)
  lvsd.py rename-class XX NEW [CID]    rename a class (global, or in one script)
  lvsd.py propagate SRC [DST ...]      copy state names to the other scripts by owner set + code signature
"""
import sys, os, re, json, struct, importlib.util, collections

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
        self.d = {'states': {}, 'classes': {}, 'anims': {}, 'sfx': {}, 'mem': {}, 'pals': {}, 'fields': {}}
        if os.path.exists(dict_path):
            self.d.update(json.load(open(dict_path, encoding='utf-8')))
        alias = self.d.get('fields', {})               # dictionary names of object fields over the OBJ_* names ("state_18a5": "event")
        self.fld_orig = dict(self.fld_of)
        for idx, n in list(self.fld_of.items()):
            if n in alias: self.fld_of[idx] = alias[n]
        self.idx_of_fld = {}
        for idx, n in self.fld_of.items(): self.idx_of_fld.setdefault(n, []).append(idx)
        self.idx_of_mask = {}
        for idx, m in self.mask_of.items():
            if m is not None: self.idx_of_mask.setdefault(m, []).append(idx)
        for k, n in self.d.get('mem', {}).items():     # dictionary names of DS words ("HHHH": name) over the layout names
            self.lay[int(k, 16)] = n
        self.addr_of_mem = {}
        for a, n in self.lay.items(): self.addr_of_mem.setdefault(n, []).append(a)

    # fields -------------------------------------------------------------
    def fld(self, idx):
        n = self.fld_of[idx]
        return n if len(self.idx_of_fld.get(n, [])) == 1 else f'{n}#{idx:02X}'
    def fld_token(self, orig):
        """the token the text shows for the field whose OBJ_* name is `orig`
        (its dictionary alias, `#idx` when the name is not unique)."""
        idx = next(i for i, n in self.fld_orig.items() if n == orig)
        return self.fld(idx)
    # sounds -------------------------------------------------------------
    def sfx(self, i):
        """sequence id -> dictionary name ('sfx': "1C": "arrow_break"), else the number."""
        return self.d.get('sfx', {}).get(f'{i:02X}', imm(i))
    def sfx_parse(self, tok):
        if re.fullmatch(r'0x[0-9A-Fa-f]+|\d+', tok): return int(tok, 0)
        c = [int(k, 16) for k, v in self.d.get('sfx', {}).items() if v == tok]
        if len(c) != 1: raise ValueError(f'unknown sound {tok!r}')
        return c[0]
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

    def anim(self, cid, lbl):
        """A_xxxx -> dictionary name, else the automatic owner name
        (`<class>_a<k>`: the k-th anim label, by address, of the class whose
        code reaches it; `shared_<t1>_<t2>_a<k>`), else A_xxxx; a label the
        author named (A_walk from `anim walk:`) shows as `walk`."""
        if not re.fullmatch(r'A_[0-9A-Fa-f]{4}', lbl):
            return lbl[2:] if lbl.startswith('A_') else lbl
        n = self.d['anims'].get(f'{cid:X}:{lbl[2:]}')
        if n: return n
        return self.auto_anim_names(cid).get(int(lbl[2:], 16), lbl)

    def pal(self, cid, lbl):
        """P_xxxx -> dictionary name ('pals': "CID:xxxx"), else P_xxxx (no
        owner to name it after); a label the author named (P_gold from
        `palette gold:`) shows as `gold`."""
        if re.fullmatch(r'P_[0-9A-Fa-f]{4}', lbl):
            return self.d.get('pals', {}).get(f'{cid:X}:{lbl[2:]}', lbl)
        return lbl[2:] if lbl.startswith('P_') else lbl

    _aauto = {}
    def auto_anim_names(self, cid):
        """{anim label addr: name}. Owners: the op 19 sites of the object code
        donate their owners (lvs_struct.owners_of_states) to their operand,
        propagated over the anim graph of lvs_full.anim_layer (fall, the frame
        ends 0E/0F, goto, call + its continuation; `return` and `stop` end)."""
        if cid in self._aauto: return self._aauto[cid]
        ls = mod('lvs_struct'); lf = mod('lvs_full')
        cwd = os.getcwd(); os.chdir(ROOT)
        try:
            anims = lf.anim_layer(cid)
            d, seen, table, entries = lf.full_walk(cid, with_entries=True)
            own = ls.owners_of_states(cid)
            text = canonical_text(cid)
        finally: os.chdir(cwd)
        aown = collections.defaultdict(set); work = collections.deque()
        for pc in seen:
            if seen[pc][0] == 0x19 and pc + 3 <= len(d):
                a = struct.unpack_from('<H', d, pc + 1)[0]
                for t in own.get(pc, ()):
                    if t not in aown[a]: aown[a].add(t); work.append((a, t))
        def succ(pc):
            e = anims.get(pc)
            if e is None: return []
            cmd, ln, kind, tgt = e; out = []
            if kind == 'fall' or (kind == 'stop' and cmd != 0x1A): out.append(pc + ln)
            if kind in ('jump', 'loopstart') and tgt is not None: out.append(tgt)
            if kind == 'loopstart': out.append(pc + 3)
            return [x for x in out if x in anims]
        while work:
            pc, t = work.popleft()
            for n2 in succ(pc):
                if t not in aown[n2]: aown[n2].add(t); work.append((n2, t))
        labels = sorted(int(m.group(1), 16) for m in re.finditer(r'^A_([0-9A-F]{4}):', text, re.M))
        groups = {}
        for a in labels: groups.setdefault(frozenset(aown.get(a, ())), []).append(a)
        names = {}
        for os_, addrs in groups.items():
            if not os_: continue
            base = self.cls(cid, min(os_)) if len(os_) == 1 else 'shared_' + '_'.join(f'{t:02X}' for t in sorted(os_))
            k = 0
            for a in sorted(addrs):
                if f'{cid:X}:{a:04X}' in self.d['anims']: continue
                k += 1; names[a] = f'{base}_a{k}'
        self._aauto[cid] = names
        return names


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
    0x3F: ('', 'sprites_hide'), 0x40: ('', 'sprites_show'),   # 3F ORs 0x4000 into every sub-sprite's flags — every draw pass skips flags & 0x6000 (v2_draw_sprites, 1DD9C); 40 clears bits 13-14
    0x42: ('', 'cmdq_push(2)'), 0x43: ('', 'cmdq_push(4)'), 0xCB: ('', 'cmdq_push(4) #CB'),
    0x46: ('w', 'cmdq_push(6, {0})'), 0x47: ('', 'nop47'),
    0x4B: ('', 'flags_set 0x2000'),           # its own op (the generic `self.flags |= 0x2000` is 51+62)
    0x4C: ('rrr', 'pal_shade2({0},{1},{2})'), 0x4D: ('', 'pal_shade2_off'),   # the second shade channel (DS_PAL_SHADE_*2, values << 1)
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
    0xAC: ('', 'if acc == random()&1', 'T'), 0xAD: ('mw', 'if acc != bit({1} & {0})', 'T'),   # v2_vm_op_AD: eq -> skip, ne -> jump
    0xAE: ('mf', 'if acc != bit(self.{1} & {0})', 'T'), 0xAF: ('mg', 'if acc != bit([{1}] & {0})', 'T'),
    0xB0: ('mp', 'if acc != bit(partner.{1} & {0})', 'T'), 0xB1: ('', 'if acc != random()&1', 'T'),
    0xB2: ('mw', 'if acc == bit({1} & {0})', 'C'), 0xB3: ('mf', 'if acc == bit(self.{1} & {0})', 'C'),
    0xB4: ('mg', 'if acc == bit([{1}] & {0})', 'C'), 0xB5: ('mp', 'if acc == bit(partner.{1} & {0})', 'C'),   # v2_vm_op_B5: ne -> skip, eq -> call
    0xB7: ('mw', 'if acc != bit({1} & {0})', 'C'), 0xBA: ('mp', 'if acc != bit(partner.{1} & {0})', 'C'),
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
# op 13 sub-commands (v2_vm_op_13: al = the first operand byte; d9 reads a palette pointer
# behind it, 01 = the DOS exit of the menu's QUIT, 11 = the HUD picture copied up into the
# viewport and the HUD cleared; the two bytes behind 01/11 are never read — written as `pad`)
OP13 = {0x01: 'quit_to_dos', 0x11: 'hud_to_viewport'}
OP13_RX = re.compile(r'^(quit_to_dos|hud_to_viewport) pad (0x[0-9A-Fa-f]+|\d+),(0x[0-9A-Fa-f]+|\d+)$')

# ------------------------------------------------------ anim statements --
# The anim VM (v2_vm_exec_anim_cmd, dispatch off_30BC6): one statement per
# command. Operand kinds: '' none; 'b' one byte; 'sb' one signed byte;
# 'b*' a byte per sub-sprite (the count is the decoder's — masked sites take
# one per matching slot); 'sw*' a signed word per sub-sprite; 'w' a word;
# 'L' an anim label; 'sfx' sequence byte + the console's volume byte (the
# PC reads the low byte only); 'b16' cmd 16 = the same skip as 04, kept apart
# by the `#16` suffix.
ANIM = {
    0x00: ('frame +=', 'b'),     # sub-sprite data offset += N*72 (advance N frames; masked: matching slots)
    0x01: ('frame', 'b*'),       # sub-sprite data offset = base + N*72, one N per slot
    0x02: ('sfx', 'sfx'),        # play sequence N (low byte); the high byte is the console's volume
    0x03: ('goto', 'L'), 0x04: ('skip', 'b'), 0x05: ('call', 'L'), 0x06: ('return', ''),
    0x07: ('dx', 'sb'),          # masked: sub-sprite x += N; unmasked: object velocity x += N (pixels)
    0x08: ('x', 'sw*'),          # sub-sprite x = object x + N, one per slot
    0x09: ('dy', 'sb'), 0x0A: ('y', 'sw*'),
    0x0B: ('int3', ''), 0x0C: ('pal', 'b*'),   # colour bank bits 4-6 of the sprite flags = (N << 3) & 0x70
    0x0D: ('mask', 'b'),         # sub-sprite class mask for the rest of this frame
    0x0E: ('yield', ''),         # end of frame (the next tick continues here)
    0x0F: ('wait', 'b'),         # end of frame, N ticks
    0x10: ('flip_x', ''), 0x11: ('flip_y', ''), 0x12: ('flip_xy', ''),   # XOR 0x200 / 0x400 / 0x600
    0x13: ('class', 'b*'),       # sub-sprite classes (what `mask` selects)
    0x14: ('sprite', 'b'),       # decompress image N of the bank into the sub-sprite buffer
    0x15: ('type', 'b'),         # sprite type (renderer) + strip count from the type table
    0x16: ('skip', 'b16'), 0x17: ('bank', 'w'),   # sprite bank = chunk id N (DS_ANIM_CHUNK_IDS lookup)
    0x18: ('hide', ''), 0x19: ('show', ''),      # OR 0x4000 / AND 0x9FFF on the sub-sprite flags: every draw pass skips flags & 0x6000
    0x1A: ('stop', ''),          # anim pc = FFFF
}
ANIM_KW = {}
for _c, (_kw, _kd) in ANIM.items(): ANIM_KW.setdefault(_kw, []).append(_c)
_N = r'(0x[0-9A-Fa-f]+|\d+)'; _SN = r'(-?0x[0-9A-Fa-f]+|-?\d+)'
ANIM_RX = {
    0x00: re.compile(rf'^frame \+= {_N}$'), 0x02: re.compile(rf'^sfx {_N} vol {_N}$'),
    0x04: re.compile(rf'^skip {_N}$'), 0x16: re.compile(rf'^skip {_N} #16$'),
    'b': re.compile(rf'^(mask|wait|sprite|type) {_N}$'), 'sb': re.compile(rf'^(dx|dy) {_SN}$'),
    'b*': re.compile(rf'^(frame|pal|class)(?: {_N}(?:, {_N})*)?$'), 'sw*': re.compile(rf'^(x|y)(?: {_SN}(?:, {_SN})*)?$'),
    'w': re.compile(rf'^bank {_N}$'), 'L': re.compile(r'^(goto|call) (\S+)$'),
}


def anim_render(cmd, toks, aname, names=None):
    """`a XX <hex|label>` tokens -> statement text (aname: label token -> name; names: the sound names)."""
    kw, kd = ANIM[cmd]
    body = bytes.fromhex(toks[0]) if toks and not toks[0].startswith(('A_', '=')) else b''
    def need(n):
        if len(body) != n: raise ValueError(f'anim cmd {cmd:02X}: {len(body)} operand bytes, {n} expected')
    if kd == '': need(0); return kw
    if kd == 'L': return f'{kw} {aname(toks[0])}'
    if kd == 'b': need(1); return f'{kw} {imm(body[0])}'
    if kd == 'b16': need(1); return f'{kw} {imm(body[0])} #16'
    if kd == 'sb': need(1); return f'{kw} {body[0] - 256 if body[0] >= 128 else body[0]}'
    if kd == 'w': need(2); return f'{kw} {imm(struct.unpack_from("<H", body)[0])}'
    if kd == 'sfx': need(2); return f'{kw} {names.sfx(body[0]) if names else imm(body[0])} vol {imm(body[1])}'
    if kd == 'b*': return kw + (' ' + ', '.join(imm(b) for b in body) if body else '')
    if kd == 'sw*':
        if len(body) % 2: raise ValueError(f'anim cmd {cmd:02X}: odd operand length {len(body)}')
        vals = [struct.unpack_from('<h', body, o)[0] for o in range(0, len(body), 2)]
        return kw + (' ' + ', '.join(str(v) for v in vals) if vals else '')
    raise ValueError(kd)


def _ab(v, signed=False):
    """a byte operand: 0..255, or -128..127 when signed"""
    if not (-128 <= v <= 255 if signed else 0 <= v <= 255): raise ValueError(f'{v} does not fit a byte')
    return v & 0xFF


def _aw(v, signed=False):
    if not (-32768 <= v <= 65535 if signed else 0 <= v <= 65535): raise ValueError(f'{v} does not fit a word')
    return v & 0xFFFF


def anim_parse(text, alabel, names=None):
    """statement text -> `a XX ...` line (alabel: name -> label token; names: the sound names)."""
    m = re.match(r'^sfx (\S+) vol (0x[0-9A-Fa-f]+|\d+)$', text)
    if m:
        sid = names.sfx_parse(m.group(1)) if names else int(m.group(1), 0)
        return 'a 02 ' + bytes([_ab(sid), _ab(int(m.group(2), 0))]).hex()
    for cmd in (0x00, 0x04, 0x16):
        m = ANIM_RX[cmd].match(text)
        if m:
            vals = [_ab(int(x, 0)) for x in m.groups()]
            return f'a {cmd:02X} ' + bytes(vals).hex()
    for kw, cmds in ANIM_KW.items():
        if text == kw and ANIM[cmds[0]][1] == '': return f'a {cmds[0]:02X}'
    m = ANIM_RX['L'].match(text)
    if m: return f'a {0x03 if m.group(1) == "goto" else 0x05:02X} {alabel(m.group(2))}'
    m = ANIM_RX['b'].match(text)
    if m: return f'a {ANIM_KW[m.group(1)][0]:02X} {_ab(int(m.group(2), 0)):02x}'
    m = ANIM_RX['sb'].match(text)
    if m: return f'a {ANIM_KW[m.group(1)][0]:02X} {_ab(int(m.group(2), 0), True):02x}'
    m = ANIM_RX['w'].match(text)
    if m: return f'a 17 ' + struct.pack('<H', _aw(int(m.group(1), 0))).hex()
    m = ANIM_RX['b*'].match(text)
    if m:
        cmd = ANIM_KW[m.group(1)][0]; rest = text[len(m.group(1)):].strip()
        vals = [_ab(int(x, 0)) for x in rest.split(',')] if rest else []
        return f'a {cmd:02X}' + (' ' + bytes(vals).hex() if vals else '')
    m = ANIM_RX['sw*'].match(text)
    if m:
        cmd = ANIM_KW[m.group(1)][0]; rest = text[len(m.group(1)):].strip()
        vals = [_aw(int(x, 0), True) for x in rest.split(',')] if rest else []
        return f'a {cmd:02X}' + (' ' + b''.join(struct.pack('<H', v) for v in vals).hex() if vals else '')
    raise ValueError(f'cannot parse anim statement: {text!r}')
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
    stats = {'stmt': 0, 'sugar': 0, 'states': 0, 'named': 0, 'anim': 0, 'anims': 0, 'anamed': 0, 'pal': 0}
    def tgt_name(tok):
        if tok.startswith('='): return tok
        return names.state(cid, tok)
    def anim_name(tok):
        if tok.startswith('='): return tok
        return names.anim(cid, tok)
    dmode = None      # 'code' after a code label, 'anim' after an anim label: the two never interleave (asserted)
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
                out.append(f'state {nm}:' + (f'   ; @{a:04X}' if a is not None else '')); dmode = 'code'
            elif lbl.startswith('A_'):
                nm = names.anim(cid, lbl); stats['anims'] += 1; stats['anamed'] += (nm != lbl)
                a = addr_after.get(lineno)
                out.append(f'anim {nm}:' + (f'   ; @{a:04X}' if a is not None else '')); dmode = 'anim'
            elif lbl.startswith('P_'):
                a = addr_after.get(lineno)
                out.append(f'palette {names.pal(cid, lbl)}:' + (f'   ; @{a:04X}' if a is not None else '')); dmode = 'pal'; stats['pal'] += 1
            else:
                out.append(raw); dmode = None
            continue
        if len(p) == 3 and p[1] == '=' and p[0].startswith('S_') and '+' in p[2]:
            flush()                                   # alias between code labels: both sides get the state names
            base, off = p[2].split('+')
            out.append(f'alias {tgt_name(p[0])} = {tgt_name(base) if base.startswith("S_") else base}+{off}')
            continue
        if p[0] == 'a':
            flush()
            if dmode != 'anim': raise ValueError(f'{cid:X}.lvsf:{lineno}: anim code under a code label ({raw!r})')
            out.append('    ' + anim_render(int(p[1], 16), p[2:], anim_name, names)); stats['anim'] += 1; continue
        if p[0] == 'blob' and dmode == 'pal':                 # the palette block behind a P_ label: 16 colours of 3 DAC bytes
            flush(); b = bytes.fromhex(p[1]) if len(p) > 1 else b''
            for k in range(len(b) // 3):
                out.append(f'    rgb {b[3*k]}, {b[3*k+1]}, {b[3*k+2]}   ; {k % 16}')
            if len(b) % 3: out.append('blob ' + b[len(b) - len(b) % 3:].hex())
            dmode = None; continue
        if p[0] == 'blob' or (len(p) == 3 and p[1] == '='):
            flush(); out.append(raw)
            if p[0] == 'blob': dmode = None
            continue
        if p[0] != 'o':
            raise ValueError(f'{cid:X}.lvsf:{lineno}: unexpected line {raw!r}')
        op, toks = parse_o_line(raw)
        if dmode != 'code': raise ValueError(f'{cid:X}.lvsf:{lineno}: object code under an anim label ({raw!r})')
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
            flush(); out.append(f'    anim {anim_name(sym[0])}'); continue
        if op == 0x02 and len(body) == 2:      # sfx: the low byte is the sequence, the high byte the console's volume (the PC reads the low byte)
            flush(); out.append(f'    sfx {names.sfx(body[0])} vol {imm(body[1])}'); continue
        if op == 0x04 and len(body) == 1:
            flush(); out.append(f'    sfx_stop {names.sfx(body[0])}'); continue
        if op == 0x13:
            flush()
            if sym and sym[0] == 'd9': out.append(f'    palette {names.pal(cid, sym[1])}')
            elif len(body) == 3 and body[0] in OP13: out.append(f'    {OP13[body[0]]} pad {imm(body[1])},{imm(body[2])}')
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
    out, nsay = fold_say(out, names)
    stats['say'] = nsay
    out, stats['bits'] = fold_bits(out, names)
    out, stats['facing'] = fold_facing(out, names)
    out, stats['gates'] = fold_gates(out, names)
    out, stats['hurt'] = fold_hurt(out, names)
    out, stats['abv'] = fold_abv(out, names)
    out, nfn, ncall = fold_funcs(out)
    stats['func'] = nfn; stats['callargs'] = ncall
    out, stats['loops'] = fold_loops(out)
    out.append(f'; stats: {stats["stmt"]} statements, {stats["sugar"]} folded loads, {nsw} switch blocks, {nsay} say lines, {nfn} funcs, {ncall} calls with args, {stats["states"]} states ({stats["named"]} named), {stats["anim"]} anim statements, {stats["anims"]} anim labels ({stats["anamed"]} named), {stats["pal"]} palettes, '
               f'idioms: {stats["bits"]} bit tests, {stats["facing"]} same_facing, {stats["gates"]} mask gates, {stats["hurt"]} hurt, {stats["abv"]} anim_by_viking, {stats["loops"]} loops')
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


# func: a subroutine. The VM keeps ONE return address per object (OBJ_ALT_PC:
# `call` writes it, `return` reads it), so a func is a run of blocks that is
# entered by `call` only, holds no other call or call-branch, whose inner
# labels are entered from inside only and whose last block does not fall
# through — Cfg.func_extent. Declared `func NAME:`, its further blocks as
# inner labels `  NAME:`; a hand-written one is checked for the same rules.
# Parameters: `call F(self.f = N, [g] = N, partner.f = N, acc = X)` is the
# literal stores (each `acc = N; store`) then the optional `acc = X`, then the
# call — written back in that order, so acc holds X at entry.
NONFALL = {'goto', 'return', 'exit', 'despawn'}
STORE_ARG_RX = re.compile(r'^(self\.[A-Za-z_][A-Za-z0-9_?]*(?:#[0-9A-Fa-f]{2})?|partner\.[A-Za-z_][A-Za-z0-9_?]*(?:#[0-9A-Fa-f]{2})?|\[(?:[A-Za-z_][A-Za-z0-9_]*|[0-9A-Fa-f]{4})\]) = (0x[0-9A-Fa-f]+|-?\d+)$')
ACC_ARG_RX = re.compile(r'^acc = (.+)$')


def label_of(line):
    """the name a header/label line defines: `state X:`, `loop X:`, `func X:`, or an inner label `  X:`"""
    m = re.match(r'^(?:(?:state|func|loop) (\S+)|  (\S+)):', line.split(';', 1)[0].rstrip())
    return (m.group(1) or m.group(2)) if m else None


class Cfg:
    """The control-flow graph of a .lvd text (the decompiler and the compiler
    read the same one, so a func the decompiler writes passes the checks and
    a func that passes the checks is re-detected with the same extent).
    Blocks: a `state`/`func` header or an inner label plus the indented lines
    up to the next non-indented line; a block falls through into the next
    block only when nothing but blank/comment lines lies between them.
    Edges (non-call): goto, the branch and search families, switch/select
    cases, fallthrough, class entries (`entry` pseudo-source). Call edges are
    kept apart (call, if … call, call F(args))."""
    def __init__(self, lines):
        self.lines = lines
        self.blocks = []          # [name, line index of the header, [(line index, stmt)], adjacent to the next block]
        self.kind = {}            # name -> 'state' | 'func' | 'inner'
        self.entries = {}         # class entry name -> line index
        self.loops = set()        # `loop X:` headers: the block ends with an implicit `goto X`
        cur = None; in_anim = False
        for i, l in enumerate(lines):
            code = l.split(';', 1)[0].rstrip()
            if not code.strip(): continue
            if re.match(r'^(anim|palette) \S+:$', code) or re.match(r'^[AP]_[0-9A-Fa-f]{4}:$', code):    # an anim / palette label: its indented lines are not code
                in_anim = True; cur = None; continue
            if code.startswith(('    ', '\t')) and in_anim: continue
            if not code.startswith(' '): in_anim = False
            m = re.match(r'^(state|func|loop) (\S+):$', code)
            if m:
                if cur is not None: cur[3] = True
                cur = [m.group(2), i, [], False]; self.blocks.append(cur); self.kind[m.group(2)] = 'state' if m.group(1) == 'loop' else m.group(1)
                if m.group(1) == 'loop': self.loops.add(m.group(2))
                continue
            m = re.match(r'^  (\S+):$', code)
            if m:
                if cur is not None: cur[3] = True
                cur = [m.group(1), i, [], False]; self.blocks.append(cur); self.kind[m.group(1)] = 'inner'; continue
            if code.startswith(('    ', '\t')):
                if cur is None: cur = [f'<orphan@{i}>', i, [], False]; self.blocks.append(cur); self.kind[cur[0]] = 'orphan'
                cur[2].append((i, code.strip())); continue
            if code.startswith('class '):
                mm = re.search(r'entry=(\S+)', code)
                if mm: self.entries[mm.group(1)] = i
            cur = None                                   # class / blob / anim / alias / chunk: no fallthrough across it
        self.index = {b[0]: k for k, b in enumerate(self.blocks)}
        self.succ = {}; self.calls = {}; self.falls = {}; self.call_lines = {}
        for k, (name, hi, stmts, adj) in enumerate(self.blocks):
            succ = []; calls = []
            for li, st in stmts:
                m = re.match(r'^call (\S+?)(\(.*\))?$', st)
                if m: calls.append((m.group(1), li)); continue
                m = re.match(r'^if .* call (\S+)$', st)
                if m: calls.append((m.group(1), li)); continue
                m = re.match(r'^(0x[0-9A-Fa-f]+|\d+) -> (\S+)$', st)
                if m: succ.append((m.group(2), li)); continue
                m = re.search(r'\bgoto (\S+)$', st)
                if m: succ.append((m.group(1), li))
            last = stmts[-1][1].split()[0] if stmts else ''
            falls = last not in NONFALL
            if last == 'anim_by_viking': falls = stmts[-1][1].endswith(' fallthrough')   # the olaf branch falls through only in that variant
            if name in self.loops:
                succ.append((name, stmts[-1][0] if stmts else hi)); falls = False      # the implicit `goto X` of a loop
            if falls and adj and k + 1 < len(self.blocks):
                succ.append((self.blocks[k + 1][0], stmts[-1][0] if stmts else hi))
            self.succ[name] = succ; self.calls[name] = calls; self.falls[name] = falls
        self.preds = collections.defaultdict(list)       # name -> [(source block, line)]
        self.callers = collections.defaultdict(list)
        for name in self.succ:
            for t, li in self.succ[name]: self.preds[t].append((name, li))
            for t, li in self.calls[name]: self.callers[t].append((name, li))

    def closure(self, start):
        seen = {start}; st = [start]
        while st:
            n = st.pop()
            for t, _ in self.succ.get(n, ()):
                if t in self.index and t not in seen: seen.add(t); st.append(t)
        return seen

    def func_extent(self, E):
        """The blocks of a func headed by E: the longest run of adjacent blocks
        from E, every one reachable from E, such that (a) no block holds a
        call or call-branch, (b) no block but E is called and none is a class
        entry, (c) every block but E is entered from inside the run only, (d)
        E is entered from outside the run by calls only (an internal loop back
        to E is fine; a fallthrough from the block before is not), (e) the
        last block does not fall through. None when no run qualifies."""
        if E not in self.index or E not in self.callers: return None
        k0 = self.index[E]; C = self.closure(E); run = [E]
        while self.blocks[k0 + len(run) - 1][3] and k0 + len(run) < len(self.blocks):
            nxt = self.blocks[k0 + len(run)][0]
            if nxt not in C or self.kind.get(nxt) == 'orphan': break
            run.append(nxt)
        for k in range(len(run), 0, -1):
            P = run[:k]; Ps = set(P)
            if any(self.calls[n] for n in P): continue
            if any(n in self.callers for n in P[1:]) or any(n in self.entries for n in P): continue
            if any(p not in Ps for n in P[1:] for p, _ in self.preds.get(n, ())): continue
            if any(p not in Ps for p, _ in self.preds.get(E, ())): continue
            if self.falls[P[-1]]: continue
            return P
        return None


def fold_funcs(lines):
    """-> (lines, funcs, folded calls). `func NAME:` for every call target
    whose extent qualifies (Cfg.func_extent); the blocks after the header
    inside the extent become inner labels `  NAME:`."""
    cfg = Cfg(lines); out = list(lines); funcs = set(); nfn = 0
    for E in cfg.callers:
        P = cfg.func_extent(E)
        if P is None or cfg.kind.get(E) != 'state': continue
        funcs.add(E); nfn += 1
        hi = cfg.blocks[cfg.index[E]][1]
        out[hi] = out[hi].replace('state ', 'func ', 1)
        for n in P[1:]:
            li = cfg.blocks[cfg.index[n]][1]
            out[li] = '  ' + out[li][len('state '):]
    # call sugar: literal stores (+ one acc load) right before `call F` of a func
    res = []; ncall = 0; k = 0
    while k < len(out):
        l = out[k]
        m = re.match(r'^    call (\S+)$', l.split(';', 1)[0].rstrip()) if l.startswith('    ') else None
        if m and m.group(1) in funcs:
            args = []; b = len(res) - 1
            if b >= 0 and res[b].startswith('    ') and ACC_ARG_RX.match(res[b].strip()):
                args.insert(0, res[b].strip()); b -= 1
            while b >= 0 and res[b].startswith('    ') and STORE_ARG_RX.match(res[b].strip()):
                args.insert(0, res[b].strip()); b -= 1
            if args:
                del res[b + 1:]
                res.append(f'    call {m.group(1)}({", ".join(args)})'); ncall += 1; k += 1; continue
        res.append(l); k += 1
    return res, nfn, ncall


# say: the speech-bubble idiom — seven statements in a row (no label between):
#   set_partner P                        acc = P; op 96
#   [0206], [0208] = delta(partner)      op 16 into the text column/row globals
#   [0208] -= N   (or += N)              acc = N; op 5D (or 5A) on [0208]
#   cmdq_push(6, X)                      op 46
#   text(id=ID, edge=E, x=[0206], y=[0208])   op 41 (id a literal, E a literal or a field)
#   cmdq_push(4)                         op 43
#   cmdq_push(2)                         op 42
# -> say partner=P dy=-N cmd=X id=ID edge=E     (dy=+N for the += form)
def live(names):
    """The tokens the idiom folds match: the text column/row scratch words
    0206/0208 and the fields/masks by their CURRENT display names (the
    dictionary may rename them), so a renamed word never silences a fold."""
    return {'ta': '[' + names.mem(0x206) + ']', 'tb': '[' + names.mem(0x208) + ']',
            'flags': 'self.' + names.fld(0x08), 'pflags': 'partner.' + names.fld(0x08),
            'facing': names.mask(0x0C), 'bit1': names.mask(0x00), 'hi': names.mask(0x1E),
            'event': names.fld_token('state_18a5'), 'arg': names.fld_token('state_18cd'), 'vik': names.fld_token('anim_idx')}


def say_rx(names):
    L = live(names); ta, tb = re.escape(L['ta']), re.escape(L['tb'])
    return [
        re.compile(r'^    set_partner (0x[0-9A-Fa-f]+|\d+)$'),
        re.compile(rf'^    {ta}, {tb} = delta\(partner\)$'),
        re.compile(rf'^    {tb} (-=|\+=) (0x[0-9A-Fa-f]+|\d+)$'),
        re.compile(r'^    cmdq_push\(6, (0x[0-9A-Fa-f]+|\d+)\)$'),
        re.compile(rf'^    text\(id=(0x[0-9A-Fa-f]+|\d+), edge=(0x[0-9A-Fa-f]+|\d+|self\.[A-Za-z_][A-Za-z0-9_?]*(?:#[0-9A-Fa-f]{{2}})?), x={ta}, y={tb}\)$'),
        re.compile(r'^    cmdq_push\(4\)$'),
        re.compile(r'^    cmdq_push\(2\)$'),
    ]
SAY_LINE_RX = re.compile(r'^say partner=(0x[0-9A-Fa-f]+|\d+) dy=([-+])(0x[0-9A-Fa-f]+|\d+) cmd=(0x[0-9A-Fa-f]+|\d+) id=(0x[0-9A-Fa-f]+|\d+) edge=(0x[0-9A-Fa-f]+|\d+|self\.[A-Za-z_][A-Za-z0-9_?]*(?:#[0-9A-Fa-f]{2})?)$')


def fold_say(lines, names):
    out = []; i = 0; n = 0; SAY_RX = say_rx(names)
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


# ------------------------------------------------ idioms (level C DSL) --
# Every idiom below is an exact two-way rewrite of a fixed statement run: the
# decompiler folds the run into one line, the compiler writes the run back,
# byte for byte. Operands stay the tokens of the low form.
OPD_RX = r'(self\.[A-Za-z_][A-Za-z0-9_?]*(?:#[0-9A-Fa-f]{2})?|partner\.[A-Za-z_][A-Za-z0-9_?]*(?:#[0-9A-Fa-f]{2})?|\[(?:[A-Za-z_][A-Za-z0-9_]*|[0-9A-Fa-f]{4})\])'
MASK_RX = r'(0x[0-9A-Fa-f]+(?:#[0-9A-Fa-f]{2})?)'
KIND_RX = r'(goto|call)'
LIT2_RX = r'(0x[0-9A-Fa-f]+|-?\d+)'

# bit test: `acc = bit(K & 1)` (op 97, K 0/1, mask index 00) + a bit-test branch on acc (A9-AB / AE-B0 / B3-B5 / B8-BA)
#   K=1 ==  ->  if X & M goto L          K=1 !=  ->  if !(X & M) goto L
#   K=0 ==  ->  if bit(X & M) == 0 goto L   K=0 !=  ->  if bit(X & M) != 0 goto L
# (the four spellings keep the four opcode pairs apart; `call` for the call forms)
BIT_HI_RX = {}
def fold_bits(lines, names):
    b1 = re.escape(live(names)['bit1'])
    A = re.compile(rf'^    acc = bit\(([01]) & {b1}\)$')
    B = re.compile(rf'^    if acc (==|!=) bit\({OPD_RX} & {MASK_RX}\) {KIND_RX} {TARGET_RX}$')
    out = []; i = 0; n = 0
    while i < len(lines):
        ma = A.match(lines[i]); mb = B.match(lines[i + 1]) if ma and i + 1 < len(lines) else None
        if ma and mb:
            k, (cmp, x, m, kind, tgt) = ma.group(1), mb.groups()
            if k == '1': cond = f'{x} & {m}' if cmp == '==' else f'!({x} & {m})'
            else: cond = f'bit({x} & {m}) {cmp} 0'
            out.append(f'    if {cond} {kind} {tgt}'); i += 2; n += 1; continue
        out.append(lines[i]); i += 1
    return out, n

BIT_LINE_RX = [
    (re.compile(rf'^if !\({OPD_RX} & {MASK_RX}\) {KIND_RX} {TARGET_RX}$'), '1', '!='),
    (re.compile(rf'^if {OPD_RX} & {MASK_RX} {KIND_RX} {TARGET_RX}$'), '1', '=='),
    (re.compile(rf'^if bit\({OPD_RX} & {MASK_RX}\) (==|!=) 0 {KIND_RX} {TARGET_RX}$'), '0', None),
]
def expand_bits(raw, names):
    for rx, k, cmp in BIT_LINE_RX:
        m = rx.match(raw)
        if not m: continue
        g = m.groups()
        if cmp is None: x, mk, cmp, kind, tgt = g
        else: x, mk, kind, tgt = g
        return [f'acc = bit({k} & {live(names)["bit1"]})', f'if acc {cmp} bit({x} & {mk}) {kind} {tgt}']
    return None

# same facing: `acc = bit(self.flags & 0x40)` + `if acc ==/!= bit(partner.flags & 0x40) goto L`
def fold_facing(lines, names):
    L = live(names); A = re.compile(rf'^    acc = bit\({re.escape(L["flags"])} & {re.escape(L["facing"])}\)$')
    B = re.compile(rf'^    if acc (==|!=) bit\({re.escape(L["pflags"])} & {re.escape(L["facing"])}\) {KIND_RX} {TARGET_RX}$')
    out = []; i = 0; n = 0
    while i < len(lines):
        ma = A.match(lines[i]); mb = B.match(lines[i + 1]) if ma and i + 1 < len(lines) else None
        if ma and mb:
            cmp, kind, tgt = mb.groups()
            out.append(f'    if {"" if cmp == "==" else "!"}same_facing(partner) {kind} {tgt}'); i += 2; n += 1; continue
        out.append(lines[i]); i += 1
    return out, n

FACING_LINE_RX = re.compile(rf'^if (!?)same_facing\(partner\) {KIND_RX} {TARGET_RX}$')
def expand_facing(raw, names):
    m = FACING_LINE_RX.match(raw)
    if not m: return None
    L = live(names); neg, kind, tgt = m.groups()
    return [f'acc = bit({L["flags"]} & {L["facing"]})', f'if acc {"!=" if neg else "=="} bit({L["pflags"]} & {L["facing"]}) {kind} {tgt}']

# mask gate: `[0206] = A` / `[0206] &= B` / `if A ==/!= [0206] goto L`  ->  if A & B == A goto L
# (== : every bit of A is set in B — the switch words [switches] / [switches_b] / [hints_done] against the object's pool)
def fold_gates(lines, names):
    ta = re.escape(live(names)['ta'])
    A = re.compile(rf'^    {ta} = {OPD_RX}$'); B = re.compile(rf'^    {ta} &= {OPD_RX}$')
    C = re.compile(rf'^    if {OPD_RX} (==|!=) {ta} {KIND_RX} {TARGET_RX}$')
    out = []; i = 0; n = 0
    while i < len(lines):
        ma = A.match(lines[i]); mb = B.match(lines[i + 1]) if ma and i + 1 < len(lines) else None
        mc = C.match(lines[i + 2]) if mb and i + 2 < len(lines) else None
        if ma and mb and mc and mc.group(1) == ma.group(1):
            out.append(f'    if {ma.group(1)} & {mb.group(1)} {mc.group(2)} {ma.group(1)} {mc.group(3)} {mc.group(4)}'); i += 3; n += 1; continue
        out.append(lines[i]); i += 1
    return out, n

GATE_LINE_RX = re.compile(rf'^if {OPD_RX} & {OPD_RX} (==|!=) {OPD_RX} {KIND_RX} {TARGET_RX}$')
def expand_gate(raw):
    m = GATE_LINE_RX.match(raw)
    if not m or m.group(1) != m.group(4): return None
    a, b, cmp, _, kind, tgt = m.groups()
    return [f'[0206] = {a}', f'[0206] &= {b}', f'if {a} {cmp} [0206] {kind} {tgt}']

# hurt: `partner.event = K` / `partner.event_arg = A` / `partner.event_arg = setbit(partner.event_arg, 0x8000, bit(self.flags & 0x40))`
#   -> hurt partner event=K amount=A facing     (the direction bit of the amount = the hurter's facing)
def fold_hurt(lines, names):
    L = live(names); ev, ar = re.escape(L['event']), re.escape(L['arg'])
    A = re.compile(rf'^    partner\.{ev} = {LIT2_RX}$'); B = re.compile(rf'^    partner\.{ar} = {LIT2_RX}$')
    C = re.compile(rf'^    partner\.{ar} = setbit\(partner\.{ar}, {re.escape(L["hi"])}, bit\({re.escape(L["flags"])} & {re.escape(L["facing"])}\)\)$')
    out = []; i = 0; n = 0
    while i < len(lines):
        ma = A.match(lines[i]); mb = B.match(lines[i + 1]) if ma and i + 1 < len(lines) else None
        mc = C.match(lines[i + 2]) if mb and i + 2 < len(lines) else None
        if ma and mb and mc:
            out.append(f'    hurt partner event={ma.group(1)} amount={mb.group(1)} facing'); i += 3; n += 1; continue
        out.append(lines[i]); i += 1
    return out, n

HURT_LINE_RX = re.compile(rf'^hurt partner event={LIT2_RX} amount={LIT2_RX} facing$')
def expand_hurt(raw, names):
    m = HURT_LINE_RX.match(raw)
    if not m: return None
    L = live(names); k, a = m.groups()
    return [f'partner.{L["event"]} = {k}', f'partner.{L["arg"]} = {a}', f'partner.{L["arg"]} = setbit(partner.{L["arg"]}, {L["hi"]}, bit({L["flags"]} & {L["facing"]}))']

# anim by viking: a state that ends with
#   select self.anim_idx:  0 -> B   2 -> C  /  anim A  /  goto L
# followed directly by the states B: `anim ...; goto L` and C: `anim ...; goto L` (or `anim ...` falling
# through), both entered from that select only  ->  anim_by_viking erik=A, baleog=B, olaf=C goto L [fallthrough]
def fold_abv(lines, names):
    vik = re.escape(live(names)['vik'])
    hdr = re.compile(r'^state (\S+):(\s*;.*)?$')
    def stmt(i): return lines[i].split(';', 1)[0].rstrip() if i < len(lines) else None
    def refs(name):
        return sum(len(re.findall(rf'(?:goto|call|->|entry=) {re.escape(name)}\b|entry={re.escape(name)}\b', l.split(';', 1)[0])) for l in lines)
    out = []; i = 0; n = 0; skip = set()
    while i < len(lines):
        if i in skip: i += 1; continue
        s = stmt(i)
        if s == f'    select self.{live(names)["vik"]}:' and i + 4 < len(lines):
            m0 = re.match(r'^        0 -> (\S+)$', stmt(i + 1) or ''); m2 = re.match(r'^        2 -> (\S+)$', stmt(i + 2) or '')
            ma = re.match(r'^    anim (\S+)$', stmt(i + 3) or ''); mg = re.match(r'^    goto (\S+)$', stmt(i + 4) or '')
            hb = hdr.match(stmt(i + 5) or '');
            if m0 and m2 and ma and mg and hb and hb.group(1) == m0.group(1):
                ab = re.match(r'^    anim (\S+)$', stmt(i + 6) or ''); gb = stmt(i + 7) == f'    goto {mg.group(1)}'
                hc = hdr.match(stmt(i + 8) or '')
                if ab and gb and hc and hc.group(1) == m2.group(1):
                    ac = re.match(r'^    anim (\S+)$', stmt(i + 9) or '')
                    gc = stmt(i + 10) == f'    goto {mg.group(1)}'
                    nxt = stmt(i + 10 if gc else i + 10)
                    end_ok = gc or (nxt is None or not nxt.startswith(' ') or nxt == '')
                    if ac and end_ok and refs(m0.group(1)) == 1 and refs(m2.group(1)) == 1:
                        out.append(f'    anim_by_viking erik={ma.group(1)}, baleog={ab.group(1)}, olaf={ac.group(1)} goto {mg.group(1)}' + ('' if gc else ' fallthrough'))
                        i += 11 if gc else 10; n += 1; continue
        out.append(lines[i]); i += 1
    return out, n

ABV_LINE_RX = re.compile(rf'^anim_by_viking erik=(\S+), baleog=(\S+), olaf=(\S+) goto {TARGET_RX}( fallthrough)?$')

# loop: a state whose last statement is `goto <itself>`  ->  `loop X:` header, the goto implied
def fold_loops(lines):
    out = list(lines); n = 0; i = 0
    while i < len(out):
        m = re.match(r'^state (\S+):', out[i].split(';', 1)[0].rstrip())
        if not m: i += 1; continue
        j = i + 1; last = None
        while j < len(out) and (out[j].startswith(('    ', '\t')) or not out[j].split(';', 1)[0].strip()):
            if out[j].split(';', 1)[0].strip(): last = j
            j += 1
        if last is not None and out[last].split(';', 1)[0].rstrip() == f'    goto {m.group(1)}':
            out[i] = out[i].replace('state ', 'loop ', 1); del out[last]; n += 1
        i = j
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

    def plabel(self, tok):
        """palette name -> compile_free label token (the P_ namespace)."""
        if tok.startswith('='): return tok
        if tok.startswith(('P_', 'A_', 'S_')): return tok
        return 'P_' + tok

    def alabel(self, tok):
        """anim name -> compile_free label token (the anim namespace)."""
        if tok.startswith('='): return tok
        if tok.startswith(('A_', 'S_', 'P_')): return tok
        return 'A_' + tok

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
        if w[0] == 'anim' and len(w) == 2: return [f'o 19 {self.alabel(w[1])}']
        if w[0] == 'palette' and len(w) == 2: return ['o 13 d9 ' + self.plabel(w[1])]
        ms = re.match(r'^sfx (\S+) vol (0x[0-9A-Fa-f]+|\d+)$', s)
        if ms: return ['o 02 ' + struct.pack('<H', (int(ms.group(2), 0) << 8) | self.N.sfx_parse(ms.group(1))).hex()]
        ms = re.match(r'^sfx_stop (\S+)$', s)
        if ms: return ['o 04 %02x' % self.N.sfx_parse(ms.group(1))]
        m13 = OP13_RX.match(s)
        if m13:
            sub = next(k for k, v in OP13.items() if v == m13.group(1))
            return ['o 13 %02x%02x%02x' % (sub, int(m13.group(2), 0), int(m13.group(3), 0))]
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

    def check_funcs(self, text):
        """The rules a `func` must satisfy (Cfg.func_extent, the VM keeps one
        return address per object), each violation with its line."""
        lines = text.splitlines()
        cfg = Cfg(lines)
        for name, kind in cfg.kind.items():
            if kind != 'inner': continue
            k = cfg.index[name]; q = k - 1
            while q >= 0 and cfg.kind[cfg.blocks[q][0]] == 'inner': q -= 1
            if q < 0 or cfg.kind[cfg.blocks[q][0]] != 'func' or not all(cfg.blocks[x][3] for x in range(q, k)):
                raise ValueError(f'line {cfg.blocks[k][1] + 1}: inner label {name} must follow a func (or its inner labels) directly')
        for E, kind in cfg.kind.items():
            if kind != 'func': continue
            k = cfg.index[E]; P = [E]
            while k + len(P) < len(cfg.blocks) and cfg.kind[cfg.blocks[k + len(P)][0]] == 'inner': P.append(cfg.blocks[k + len(P)][0])
            Ps = set(P); hl = cfg.blocks[k][1] + 1
            for n in P:
                for t, li in cfg.calls[n]:
                    raise ValueError(f'line {li + 1}: func {E}: a call inside a func loses the return address (the VM keeps one per object)')
                if n in cfg.entries:
                    raise ValueError(f'line {cfg.entries[n] + 1}: func {E}: {n} is a class entry — a func is entered by call only')
            for n in P[1:]:
                for c, li in cfg.callers.get(n, ()):
                    raise ValueError(f'line {li + 1}: {n} is a label inside func {E} — call the func, not a label inside it')
                for p, li in cfg.preds.get(n, ()):
                    if p not in Ps: raise ValueError(f'line {li + 1}: {n} is a label inside func {E} and is entered from outside it')
            for p, li in cfg.preds.get(E, ()):
                if p in Ps: continue
                how = 'fallen into' if (cfg.index.get(p) == k - 1 and not any(t == E for t, _ in cfg.succ[p][:-1])) and cfg.falls[p] else 'entered by goto/branch'
                if how == 'fallen into' and any(t == E for t, l2 in cfg.succ[p] if l2 != li): how = 'entered by goto/branch'
                raise ValueError(f'line {hl}: func {E} is {how} from line {li + 1} — a func is entered by call only' +
                                 (' (end the previous code with goto/return/exit/despawn)' if how == 'fallen into' else ''))
            if E not in cfg.callers:
                raise ValueError(f'line {hl}: func {E} is never called')
            last = cfg.blocks[cfg.index[P[-1]]]
            if cfg.falls[P[-1]]:
                raise ValueError(f'line {(last[2][-1][0] if last[2] else last[1]) + 1}: func {E} falls through past its last block — end it with return/goto/exit/despawn')

    def expand_idiom(self, raw):
        """a level-C idiom line -> the low statements it stands for (label
        lines as 'NAME:'), or None when the line is not an idiom."""
        for f in (expand_gate, lambda r: expand_bits(r, self.N), lambda r: expand_facing(r, self.N), lambda r: expand_hurt(r, self.N)):
            ex = f(raw)
            if ex is not None: return ex
        m = ABV_LINE_RX.match(raw)
        if m:
            a, b, c, tgt, fall = m.groups(); vik = self.N.fld_token('anim_idx')
            self._abv += 1; lb, lc = f'_abv{self._abv}_baleog', f'_abv{self._abv}_olaf'
            return [f'if 0 == self.{vik} goto {lb}', f'if 2 == self.{vik} goto {lc}', f'anim {a}', f'goto {tgt}',
                    f'{lb}:', f'anim {b}', f'goto {tgt}', f'{lc}:', f'anim {c}'] + ([] if fall else [f'goto {tgt}'])
        return None

    def lower(self, text):
        self.check_funcs(text)
        out = []; self._abv = 0
        block = None            # ('switch'|'select', value) while inside a case block
        mode = None             # 'code' under a state/func/inner label, 'anim' under an anim label, 'pal' under a palette label
        pal = []                # the rgb bytes of the palette block being read
        loop = None             # the name of the open `loop X:` block: its implicit `goto X` is written when the block ends
        def flush_pal():
            if pal: out.append('blob ' + bytes(pal).hex()); pal.clear()
        def flush_loop():
            nonlocal loop
            if loop: out.extend(self.statement(f'goto {loop}')); loop = None
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
                if not (code.startswith('    ') or code.startswith('\t')): flush_loop()   # a label / header / data line ends the loop block
                mi = re.match(r'^  ([A-Za-z_][A-Za-z0-9_]*):$', code)     # an inner label of a func
                if mi:
                    out.append(self.label(mi.group(1)) + ':'); mode = 'code'; continue
                if (code.startswith('    ') or code.startswith('\t')) and mode == 'anim':   # an anim statement
                    out.append(anim_parse(raw, self.alabel, self.N)); continue
                if (code.startswith('    ') or code.startswith('\t')) and mode == 'pal':    # a palette colour
                    mp = re.match(r'^rgb (\d+), (\d+), (\d+)$', raw)
                    if not mp: raise ValueError(f'expected `rgb r, g, b` under a palette label: {raw!r}')
                    pal.extend(_ab(int(x)) for x in mp.groups()); continue
                flush_pal()
                if (code.startswith('    ') or code.startswith('\t')) and mode is None:
                    raise ValueError(f'statement outside a state/func/anim: {raw!r}')
                if code.startswith('    ') or code.startswith('\t'):   # indented = a statement (checked first:
                    mb = re.match(r'^(switch|select) (\S+):$', raw)     # `x = y` statements look like aliases)
                    if mb:
                        block = (mb.group(1), mb.group(2)); continue
                    if raw.startswith('say '):
                        ex = expand_say(raw)
                        if ex is None: raise ValueError(f'bad say line {raw!r}')
                        for s in ex: out.extend(self.statement(s))
                        continue
                    mc = re.match(r'^call (\S+?)\((.*)\)$', raw)
                    if mc:                                              # call F(args): the stores, the acc load, the call
                        args = [a.strip() for a in mc.group(2).split(',') if a.strip()]
                        seen_acc = False
                        for a in args:
                            if ACC_ARG_RX.match(a):
                                if seen_acc: raise ValueError('call arguments: only one `acc = X`, and last')
                                seen_acc = True
                            elif STORE_ARG_RX.match(a):
                                if seen_acc: raise ValueError('call arguments: `acc = X` must come last (the stores use acc)')
                            else:
                                raise ValueError(f'call argument {a!r}: expected `self.f = N`, `[g] = N`, `partner.f = N` or `acc = X`')
                            out.extend(self.statement(a))
                        out.extend(self.statement(f'call {mc.group(1)}')); continue
                    ex = self.expand_idiom(raw)
                    if ex is not None:
                        for s in ex:
                            if s.endswith(':'): out.append(self.label(s[:-1]) + ':')
                            else: out.extend(self.statement(s))
                        continue
                    out.extend(self.statement(raw)); continue
                if p[0] == 'chunk': out.append(raw); mode = None
                elif p[0] == 'class':
                    kv = dict(x.split('=', 1) for x in p[2:])
                    entry = kv['entry']
                    out.append(f'record {kv["record"]} sprite={kv["sprite"]} flags={kv["flags"]} code={self.label(entry)} rest={kv["rest"]}'); mode = None
                elif p[0] in ('state', 'func', 'loop') and raw.endswith(':'):
                    out.append(self.label(p[1][:-1]) + ':'); mode = 'code'
                    if p[0] == 'loop': loop = p[1][:-1]
                elif p[0] == 'anim' and len(p) == 2 and raw.endswith(':'):
                    out.append(self.alabel(p[1][:-1]) + ':'); mode = 'anim'
                elif p[0] == 'palette' and len(p) == 2 and raw.endswith(':'):
                    out.append(self.plabel(p[1][:-1]) + ':'); mode = 'pal'
                elif raw.endswith(':') and len(p) == 1:
                    out.append(raw); mode = 'anim' if raw.startswith('A_') else ('code' if raw.startswith('S_') else ('pal' if raw.startswith('P_') else None))
                elif p[0] == 'alias' and len(p) == 4 and p[2] == '=':
                    base, off = p[3].split('+')
                    out.append(f'{self.label(p[1])} = {self.label(base)}+{off}')
                elif p[0] == 'a': out.append(raw)                    # a raw anim line still passes
                elif p[0] == 'blob' or (len(p) == 3 and p[1] == '='):
                    out.append(raw)
                    if p[0] == 'blob': mode = None
                else:
                    raise ValueError(f'unexpected line {raw!r}')
            except Exception as e:
                raise ValueError(f'line {lineno}: {e}') from e
        flush_loop(); flush_pal()
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
        labels = re.findall(r'^(?:state|func|loop|anim|palette|  )\s*([A-Za-z_]\w*):', text, re.M)
        dup = sorted({x for x in labels if labels.count(x) > 1})
        if dup:
            print(f'{cid:X}: DUPLICATE label names (a state, anim or palette share a name — confusing in the text and for the editor anchors): {dup[:8]}'); ok = False
        tails = sorted({x for x in labels if x.endswith('_w') or '__' in x})
        if tails:
            print(f'{cid:X}: SUSPICIOUS names (a dedupe tail `_w` or a double underscore — a batch rename went wrong): {tails[:8]}'); ok = False
    if os.path.exists(REF_PATH):
        fresh = open(REF_PATH, encoding='utf-8').read() == reference_md()
        print('LVD_REFERENCE.md ' + ('up to date' if fresh else 'STALE — run lvsd.py ref')); ok &= fresh
    return ok


CHANNEL_FORMS = [    # (op, form, meaning) — the channel ops (typed operands; Channels.render); meanings from the v2_vm_op_XX headers
    (0x14, 'spawn(t=TT, x=V, y=V, pool=V, fl=V)', 'create an object of class TT at x,y (sub_14f59 -> sub_13809)'),
    (0x15, 'a, b = delta(active_vik)', 'a,b = the position relative to the active viking (sub_150fc)'),
    (0x16, 'a, b = delta(partner)', 'a,b = the position delta to the partner, x then y (sub_15106)'),
    (0x34, 'a, b = delta(nearest_vik)', 'the nearest of the three vikings by Manhattan distance, then as 0x16 (sub_150b5)'),
    (0x26, 'a, b = quad(x, y)', 'a,b = x >> 4, y >> 4: the tile column/row of world x,y (sub_14edd)'),
    (0x28, 'a, b = cell8(x, y)', 'a,b = (x & ~15) | 8, (y & ~15) | 8: the centre of the 16-px tile (sub_14f27)'),
    (0x27, 'l = tile_type(x, y)', 'l = the collision type of the tile at x,y (sub_141a7)'),
    (0x29, 'tile[x,y] = v', 'write the map cell and mark it dirty (sub_141e0 + sub_13fc2)'),
    (0x2A, 'tile[x,y] = hi10 | v', 'keep the upper 6 bits of the cell, replace the lower 10 (the tile index); dirty'),
    (0x2B, 'tile[x,y] = lo | swap(v)', 'keep the lower 10 bits, replace the upper 6 flags with v byte-swapped (<< 2 & 0xFC00); no dirty mark'),
    (0x41, 'text(id=V, edge=V, x=V, y=V)', 'show text id in a box at x,y (sub_1242e)'),
    (0x44, 'text_menu(id=V, edge=V, x=V, y=V)', 'the menu variant of 0x41'),
    (0x45, 'cmdq_push(0xA, id=V, x=V, y=V)', 'text display variant: command-buffer entry type 0xA (sub_12634)'),
    (0x48, 'vel_to(x=V, y=V)', 'x/y velocity = the delta to x,y; the anim-table word = 0x100 (sub_14fec)'),
    (0x49, 'if probe_at(TT, x=V, y=V) goto L', 'search for an object at x,y with filter TT (sub_1589b), branch on the result'),
    (0x4A, 'if probe_at2(TT, x=V, y=V) goto L', 'the same through the second dispatch slot (sub_14fc8)'),
    (0x50, 'cmdq_push(8, x=V, y=V, p=V)', 'text position: command-buffer entry type 8 (sub_126a9)'),
    (0xD4, 'aim(x=V, y=V, thr=N)', 'x/y velocity = the direction to x,y, threshold N (sub_1531c)'),
]
ANIM_DESC = {
    0x00: 'advance every (masked: matching) sub-sprite by N frames (data offset += N*72)',
    0x01: 'sub-sprite frame(s): data offset = base + N*72, one N per sub-sprite (masked: per matching one)',
    0x02: 'play sequence N; V is the console\'s volume byte (the PC reads N only)',
    0x03: 'jump', 0x04: 'one dead byte', 0x05: 'run another stream; its `return` comes back here',
    0x06: 'back to the continuation of the last `call`',
    0x07: 'masked: sub-sprite x += N; unmasked: the object\'s x velocity += N pixels',
    0x08: 'sub-sprite x = object x + N, one per sub-sprite (signed)',
    0x09: 'masked: sub-sprite y += N; unmasked: the object\'s y velocity += N pixels',
    0x0A: 'sub-sprite y = object y + N, one per sub-sprite (signed)',
    0x0B: 'INT 3 (nothing)', 0x0C: 'colour bank bits of the sprite flags: (N << 3) & 0x70, one N per sub-sprite (masked: gated)',
    0x0D: 'sub-sprite class mask for the rest of this frame (0 = all)',
    0x0E: 'end of frame; the next tick continues here', 0x0F: 'end of frame for N ticks',
    0x10: 'XOR 0x200 (x flip) on the sub-sprite flags', 0x11: 'XOR 0x400 (y flip)', 0x12: 'XOR 0x600 (both)',
    0x13: 'set the sub-sprite classes, one per sub-sprite (masked: per matching one)',
    0x14: 'decompress image N of the bank into the sub-sprite buffer (skipped when N is current)',
    0x15: 'sprite type (renderer bits) and strip count from the type table',
    0x16: 'one dead byte (command 16, the same as 04)', 0x17: 'sprite bank = chunk id N',
    0x18: 'OR 0x4000 on the sub-sprite flags: not drawn (every draw pass skips flags & 0x6000)',
    0x19: 'AND 0x9FFF: drawn again', 0x1A: 'the anim ends (anim pc = FFFF)',
}
KIND_DOC = {'w': 'N (16-bit literal)', 'ws': 'N (signed 16-bit literal)', 'b': 'N (byte)', 'r': 'N (raw byte)',
            'v': 'N (signed byte)', 'f': 'self.field (OBJ_* name, `#idx` when ambiguous)', 'p': 'partner.field',
            'g': '[global] (layout name or 4-hex address)', 'm': '0xMASK (`#idx` when ambiguous)'}


def reference_md():
    """The statement reference, generated from the tables (LVD_REFERENCE.md)."""
    L = ['# .lvd statement reference', '',
         'Generated by `python3 tools/data/lvsd.py ref` from the tables in lvsd.py — do not edit; the language',
         'itself is described in LVD_LANGUAGE.md. Every statement lowers to exactly one opcode with the',
         'operand bytes shown; the opcode semantics are the engine\'s (src/sdl/v2_vm.cpp).', '',
         '## Object code', '', '### Control', '',
         '| op | statement | notes |', '|---|---|---|']
    for op, kw in sorted(CONTROL.items()): L.append(f'| {op:02X} | `{kw}` | |')
    L += ['| 03 | `goto L` | L a state name (or `=HHHH`) |', '| 05 | `call L` | the return address goes to OBJ_ALT_PC (one per object) |',
          '| 19 | `anim L` | start anim stream L |', '| 13 d9 | `palette L` | copy the 48-byte palette block L into the dialogue DAC rows |']
    for sub, kw in sorted(OP13.items()): L.append(f'| 13 {sub:02X} | `{kw} pad a,b` | the two bytes behind the sub-command are never read |')
    L += ['', '### Fixed-operand statements', '',
          'Operand kinds: ' + '; '.join(f'`{k}` = {v}' for k, v in KIND_DOC.items()) + '.',
          'A branch statement (`if …`, `search_*`) ends in `goto L`; a call-branch in `call L`. `acc` is the accumulator.', '',
          '| op | statement | operand bytes | kind |', '|---|---|---|---|']
    for op in sorted(OPS):
        ent = OPS[op]; slots, tpl = ent[0], ent[1]; fl = ent[2] if len(ent) > 2 else ''
        kinds = slot_kinds(slots)
        tail = ' goto L' if 'T' in fl else (' call L' if 'C' in fl else '')
        L.append(f'| {op:02X} | `{tpl}{tail}` | {" ".join(kinds) or "—"} | {"branch" if "T" in fl else ("call-branch" if "C" in fl else "")} |')
    L += ['', '### Channel statements', '', 'V = a channel value: literal, `self.f`, `[g]`, `partner.f`, `random()`, `ch5`, `ub6(XX)`, `ub7(XXXX)`;',
          'a, b, l = channel targets: `self.f`, `[g]`, `partner.f`, `drop`.', '', '| op | statement | meaning |', '|---|---|---|']
    for op, form, mean in sorted(CHANNEL_FORMS): L.append(f'| {op:02X} | `{form}` | {mean} |')
    L += ['', '### Folded forms', '',
          '* `self.f = X`, `[g] += X`, `if X == Y goto L` … — `acc = X` folded into the next statement (LVD_LANGUAGE.md, Statements).',
          '* `switch X:` / `select X:` with `N -> L` cases — runs of field-loaded / literal-loaded compare-and-branch statements.',
          '* `say partner=P dy=±N cmd=X id=ID edge=E` — the seven-statement speech-bubble idiom.',
          '* `if X & M goto L` / `if !(X & M) goto L` — `acc = bit(1 & 0x1#00)` + the bit-test branch `if acc == / != bit(X & M)`; '
          '`if bit(X & M) == 0 goto L` / `!= 0` — the same with `acc = bit(0 & 0x1#00)` (four spellings, four opcode pairs); `call` forms alike.',
          '* `if same_facing(partner) goto L` / `if !same_facing(partner) goto L` — `acc = bit(self.flags & 0x40)` + `if acc == / != bit(partner.flags & 0x40)`.',
          '* `if A & B == A goto L` / `!= A` — `[0206] = A`, `[0206] &= B`, `if A == / != [0206] goto L`: every bit of A is (not) set in B (the switch words against the pool).',
          '* `hurt partner event=K amount=A facing` — `partner.event = K`, `partner.event_arg = A`, `partner.event_arg = setbit(partner.event_arg, 0x8000, bit(self.flags & 0x40))`.',
          '* `anim_by_viking erik=A, baleog=B, olaf=C goto L [fallthrough]` — `select self.anim_idx: 0 -> b, 2 -> c`, `anim A`, `goto L`, then the states `b: anim B; goto L` and `c: anim C; goto L` (or `anim C` falling through); the two side states are written by the compiler.',
          '* `loop X:` — a state whose last statement is `goto X`; the header implies it.',
          '* `sfx NAME vol V` (op 02: the word V<<8 | sequence — the PC plays the low byte, V is the console\'s volume byte) and `sfx_stop NAME` (op 04); '
          'NAME is the `sfx` dictionary name of the sequence or its number; the anim command `sfx NAME vol V` uses the same names.',
          '* `func NAME:` with inner labels `  NAME:`; `call F(self.f = N, [g] = N, acc = X)` — Functions.', '',
          '## Anim code', '', 'Under an `anim NAME:` header. Lists carry one value per sub-sprite (masked: per matching one).', '',
          '| cmd | statement | operands | meaning |', '|---|---|---|---|']
    forms = {'': '', 'b': ' N', 'sb': ' ±N', 'b*': ' N, N, …', 'sw*': ' ±N, ±N, …', 'w': ' 0xNNNN', 'L': ' L', 'sfx': ' N vol V', 'b16': ' N #16'}
    opd = {'': '—', 'b': '1 byte', 'sb': '1 signed byte', 'b*': 'a byte per sub-sprite', 'sw*': 'a signed word per sub-sprite', 'w': 'word',
           'L': 'label (word)', 'sfx': '2 bytes', 'b16': '1 byte'}
    for cmd in sorted(ANIM):
        kw, kd = ANIM[cmd]
        L.append(f'| {cmd:02X} | `{kw}{forms[kd]}` | {opd[kd]} | {ANIM_DESC[cmd]} |')
    L += ['', '## Palettes', '', 'Under a `palette NAME:` header: `rgb r, g, b` per colour (6-bit DAC values), the block an op 13 d9 statement copies.', '']
    return '\n'.join(L)


REF_PATH = os.path.join(HERE, 'LVD_REFERENCE.md')


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


def save_names(d):
    json.dump(d, open(NAMES_PATH, 'w', encoding='utf-8'), indent=1, ensure_ascii=False)


def label_table(cid, names):
    """{display name: [(section, addr)]} of the labels of chunk cid — states
    (S_xxxx -> 'states'), anims (A_ -> 'anims'), palettes (P_ -> 'pals') — as
    the decompiler shows them (dictionary name, else the automatic one)."""
    text = canonical_text(cid); out = {}
    for m in re.finditer(r'^([SAP])_([0-9A-F]{4}):', text, re.M):
        kind, a = m.group(1), int(m.group(2), 16); lbl = f'{kind}_{a:04X}'
        nm = names.state(cid, lbl) if kind == 'S' else names.anim(cid, lbl) if kind == 'A' else names.pal(cid, lbl)
        out.setdefault(nm, []).append(({'S': 'states', 'A': 'anims', 'P': 'pals'}[kind], a))
    return out


def rename(cid, old, new, all_scripts=False):
    """Rename one label of chunk cid — a state/func/inner label, an anim or a
    palette — by its displayed name or by `@ADDR`; the dictionary entry is
    written by address, so the automatic numbering of the other labels cannot
    shift it. `all_scripts`: the same rename in every script that shows the
    name (each resolved by its own address). Refuses a name another label of
    the script already carries. Returns the number of scripts changed."""
    if not re.fullmatch(r'[A-Za-z_]\w*', new): raise SystemExit(f'bad label name {new!r}')
    names = Names(); d = names.d; n = 0
    for c in (range(0x1C1, 0x1C7) if all_scripts else [cid]):
        tab = label_table(c, names)
        if old.startswith('@'):
            want = int(old[1:], 16); hits = [(sec, a) for lst in tab.values() for sec, a in lst if a == want]
        else: hits = tab.get(old, [])
        if not hits:
            if not all_scripts: print(f'{c:X}: no label {old}')
            continue
        if len(hits) > 1: print(f'{c:X}: {old} names {len(hits)} labels — use @ADDR'); continue
        if new in tab and tab[new] != hits: print(f'{c:X}: {new} already names {tab[new]}'); continue
        sec, a = hits[0]; d[sec][f'{c:X}:{a:04X}'] = new; n += 1
        print(f'{c:X}: {old} @{a:04X} -> {new} ({sec})')
    if n: save_names(d)
    return n


def rename_class(t, new, cid=None):
    """Class name (global `*:XX`, or `CID:XX` when cid is given); refuses a name
    another class carries in the same scope."""
    if not re.fullmatch(r'[A-Za-z_]\w*', new): raise SystemExit(f'bad class name {new!r}')
    names = Names(); d = names.d; key = f'{cid:X}:{t:02X}' if cid is not None else f'*:{t:02X}'
    clash = [k for k, v in d['classes'].items() if v == new and k != key and (k.startswith('*:') or cid is None or k.startswith(f'{cid:X}:'))]
    if clash: print(f'{new} already names class {clash[0]}'); return 0
    print(f'class {key}: {d["classes"].get(key, f"t{t:02X}")} -> {new}'); d['classes'][key] = new; save_names(d); return 1


def state_sigs(cid, names):
    """[(display name, addr, signature)] of the states of chunk cid in text
    order; the signature is the statement list with jump targets and anim
    labels normalised, so equal code compares equal across scripts."""
    text, _ = decompile_text(cid, canonical_text(cid), names); out = []; cur = None
    for l in text.splitlines():
        m = re.match(r'^(state|func|loop|  )\s*(\S+):\s*;\s*@([0-9A-F]{4})', l)
        if m: cur = [m.group(2), int(m.group(3), 16), []]; out.append(cur); continue
        if l.startswith(('anim ', 'class ', 'blob', 'palette')): cur = None; continue
        if cur and l.startswith('    '):
            s = l.split(';')[0].strip()
            s = re.sub(r'\b(goto|call) \S+', r'\1 L', s); s = re.sub(r'-> \S+', '-> L', s); s = re.sub(r'\banim \S+', 'anim A', s)
            cur[2].append(s)
    return [(n, a, tuple(b)) for n, a, b in out]


def propagate(src, dsts):
    """Copy the dictionary state names of script src to the scripts dsts: a
    state of a destination that has no dictionary name yet takes the name of
    the source state with the same owner set (the classes whose code reaches
    it) and the same signature, when both sides have exactly one such state
    and the name is free there. Bodies of one statement are never matched."""
    names = Names(); d = names.d; ls = mod('lvs_struct')
    cwd = os.getcwd(); os.chdir(ROOT)
    try: own = {c: ls.owners_of_states(c) for c in [src] + list(dsts)}
    finally: os.chdir(cwd)
    hand = {}
    for n, a, sig in state_sigs(src, names):
        if f'{src:X}:{a:04X}' in d['states'] and len(sig) >= 2:
            hand.setdefault((frozenset(own[src].get(a, ())), sig), set()).add(n)
    total = 0
    for c in dsts:
        T = state_sigs(c, names); have = {n for n, a, s in T}; added = skipped = 0
        for n, a, sig in T:
            if f'{c:X}:{a:04X}' in d['states'] or len(sig) < 2: continue
            cands = hand.get((frozenset(own[c].get(a, ())), sig))
            if not cands or len(cands) != 1: continue
            nm = next(iter(cands))
            if nm in have: skipped += 1; continue
            d['states'][f'{c:X}:{a:04X}'] = nm; have.add(nm); added += 1
        total += added
        print(f'{c:X}: +{added} names from {src:X}' + (f', {skipped} skipped (name taken)' if skipped else ''))
    if total: save_names(d)
    return total


def main():
    a = sys.argv[1:]
    if not a: print(__doc__); return 1
    cmd = a[0]
    if cmd == 'seed': seed_names(); return 0
    if cmd == 'rename':
        all_ = '--all' in a; args = [x for x in a[1:] if x != '--all']
        if len(args) != 3: print('usage: lvsd.py rename CID NAME|@ADDR NEW [--all]'); return 1
        return 0 if rename(int(args[0], 16), args[1], args[2], all_) else 1
    if cmd == 'rename-class':
        if len(a) < 3: print('usage: lvsd.py rename-class XX NEW [CID]'); return 1
        return 0 if rename_class(int(a[1], 16), a[2], int(a[3], 16) if len(a) > 3 else None) else 1
    if cmd == 'propagate':
        if len(a) < 2: print('usage: lvsd.py propagate SRC_CID [DST_CID ...]'); return 1
        src = int(a[1], 16); dsts = [int(x, 16) for x in a[2:]] or [c for c in range(0x1C1, 0x1C7) if c != src]
        propagate(src, dsts); return 0
    if cmd == 'ref':
        out = a[1] if len(a) > 1 else REF_PATH
        open(out, 'w', encoding='utf-8').write(reference_md()); print('wrote', out); return 0
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
