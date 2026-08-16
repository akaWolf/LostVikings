#!/usr/bin/env python3
"""Stage 1.2: object-bytecode disassembler, v1 (reachability walker).

Entry points: template records referenced by level spawn tables
(+8 field = template index; record +3 = start_pc), expanded by the
walker through jumps/calls found in code. Instruction widths come from
tools/data/optable_draft.json for simple fixed-length ops plus the
hand-verified OPERAND_SCHEMES below (filled iteratively: the walk stops
on any opcode whose scheme is unknown and reports it — widths are then
taken from the v2 handler bodies, the proven semantic source).

Usage: disasm.py [chunk_id_hex ...]  (default: all six 0x1C1..0x1C6)
Output: assets_raw/disasm/<chunk>.lst + summary on stdout.
"""
import json, struct, sys, os
from collections import deque

REC = 0x15

# Hand-verified operand schemes (op -> (total_len, kind, note)).
# kind: 'flow' ops also push extra targets via TARGETS handlers below.
# Filled iteratively against v2_vm.cpp handlers; the walker refuses to
# guess: unknown op => stop branch, report, extend here after reading
# the handler.
# scheme: op -> (length, kind) where kind in {'fall','jmp','br','stop'}
#   'fall' — continue at pc+len only
#   'jmp'  — unconditional: continue at word-target only
#   'br'   — conditional: word-target AND fall-through
#   'stop' — terminates the strand (exit/yield/ret-class)
# Word target is always the operand at pc+1 unless a custom extractor is
# added later. Verified against the v2 handlers one by one.
# format: op -> (length, kind, target_word_offset_from_op)
OPERAND_SCHEMES = {
    0x00: (1, 'fall', None),  # yield: saves PC, strand resumes next tick
    0x03: (3, 'jmp', 1),      # jump word
    0x06: (1, 'stop', None),  # load_alt_pc: dynamic return (pair of 0x05)
    0x0F: (1, 'stop', None),  # exit VM + [334]|=1
    0x10: (1, 'stop', None),  # despawn + exit VM
    0x1A: (4, 'br', 2),       # collision probe (155d6, always 1B) + call-jump
    0x32: (4, 'br', 2),       # collision probe (15788, always 1B) + call-jump
    0x1D: (4, 'br', 2),       # collision probe variant
    0x37: (4, 'br', 2),       # collision probe (155d6)
    0x38: (4, 'br', 2),       # collision probe (156c0)
    0x2C: (4, 'srch', 2),     # obj search fwd: 1B filter + word tgt; no-match skips the 2D continuation byte
    0xD0: (4, 'br', 2),       # viking search (no extra INC)
    0xD1: (4, 'srch', 2),     # viking search: same skip-the-continuation shape
    0x2D: (1, 'fall', None),  # search continue (dynamic target = 2C's)
    0x35: (4, 'srch', 2),     # obj search (Y): same skip-the-continuation shape
    0x36: (1, 'fall', None),  # search continue (dynamic)
    0xBF: (4, 'br', 2),       # search up: 1B filter + word target
    0xC0: (4, 'br', 2),       # search family
    0xC1: (4, 'br', 2),       # search family
    0x13: (4, 'fall', None),  # sub-cmd byte + 2 param bytes; body always pc+=3
    0x61: (2, 'fall', None),  # partner.field &= acc (v2_field_addr_A: 1 idx byte)
    0x65: (2, 'fall', None),  # self.field ^= acc (v2_field_addr_B: 1 idx byte)
}

CH_LEN = {0:2, 1:1, 2:2, 3:1, 4:0, 5:0, 6:1, 7:2}
# setter channels (sub_154bf via off_30CA2): 1=self.field 2=[addr]
# 3=partner.field 5=drop(0B); 0/4/6/7 are orig-UB escape setters that the
# v2 guard aborts on — no byte model, treat as strand stop if ever seen.
SET_LEN = {1:1, 2:2, 3:1, 5:0}

def decode_op14(d, pc):
    """[14][mode1][ch(m1&7)][ch(m1>>3&7)][mode2][ch..][ch..][anim_type].
    Returns (length, extra_templates)."""
    q = pc + 1
    m1 = d[q]; q += 1
    q += CH_LEN[m1 & 7]; q += CH_LEN[(m1 >> 3) & 7]
    m2 = d[q]; q += 1
    q += CH_LEN[m2 & 7]; q += CH_LEN[(m2 >> 3) & 7]
    tmpl = d[q]; q += 1
    return q - pc, [tmpl]

def decode_op49(d, pc):
    # [op][mode][ch(m&7)][ch(m>>3&7)][anim_idx][word target] -> br
    q = pc + 1
    m = d[q]; q += 1
    q += CH_LEN[m & 7]; q += CH_LEN[(m >> 3) & 7]
    q += 1              # anim_idx
    return q + 2 - pc, []

def _chpair(d, q):
    m = d[q]; q += 1
    return q + CH_LEN[m & 7] + CH_LEN[(m >> 3) & 7]

def _setpair(d, q, second=True):
    m = d[q]; q += 1
    a = SET_LEN.get(m & 7)
    if a is None:
        raise KeyError(f'UB setter ch{m & 7}')
    q += a
    if second:
        b = SET_LEN.get((m >> 3) & 7)
        if b is None:
            raise KeyError(f'UB setter ch{(m >> 3) & 7}')
        q += b
    return q

def decode_setpair_only(d, pc):
    # 15/16/34 (sub_150xx delta writers): [op][m][set(m&7)][set(m>>3&7)]
    return _setpair(d, pc + 1) - pc, []

def decode_ch2_set2(d, pc):
    # 26/28: [op][m1][ch][ch][m2][set][set]
    q = _chpair(d, pc + 1)
    return _setpair(d, q) - pc, []

def decode_ch2_set1(d, pc):
    # 27: [op][m1][ch][ch][m2][set(m2&7)] — single setter
    q = _chpair(d, pc + 1)
    return _setpair(d, q, second=False) - pc, []

def decode_ch2_ch1(d, pc):
    # 29/2A/2B/50: [op][m1][ch][ch][m2][ch(m2&7)]
    q = _chpair(d, pc + 1)
    m2 = d[q]; q += 1
    return q + CH_LEN[m2 & 7] - pc, []

def decode_ch2(d, pc):
    # 48: [op][m][ch][ch]
    return _chpair(d, pc + 1) - pc, []

def decode_ch2_thr(d, pc):
    # D4: [op][m][ch][ch][threshold_byte]
    return _chpair(d, pc + 1) + 1 - pc, []

def decode_op41_44(d, pc):
    # 41/44: [op][w0][ch(w0&7)] + 12543-channel ch(w0>>3&7) NO extra mode
    # byte + [w1][ch(w1&7)][ch(w1>>3&7)]  (41 tail glyph_clamp eats 0)
    q = pc + 1
    w0 = d[q]; q += 1
    q += CH_LEN[w0 & 7]
    q += CH_LEN[(w0 >> 3) & 7]      # sub_12543 re-reads DS_MODE_WORD>>3
    return _chpair(d, q) - pc, []

def decode_op45(d, pc):
    # 45: [op][w0][ch(w0&7)] + 125fa: [w1][ch][ch]   (no 12543 channel)
    q = pc + 1
    w0 = d[q]; q += 1
    q += CH_LEN[w0 & 7]
    return _chpair(d, q) - pc, []

CUSTOM = {
    0x14: decode_op14,   # spawner: fall + collects template operand
    0x15: decode_setpair_only,
    0x16: decode_setpair_only,
    0x34: decode_setpair_only,
    0x26: decode_ch2_set2,
    0x28: decode_ch2_set2,
    0x27: decode_ch2_set1,
    0x29: decode_ch2_ch1,
    0x2A: decode_ch2_ch1,
    0x2B: decode_ch2_ch1,
    0x50: decode_ch2_ch1,
    0x48: decode_ch2,
    0xD4: decode_ch2_thr,
    0x41: decode_op41_44,
    0x44: decode_op41_44,
    0x45: decode_op45,
}
# probe ops 49/4A: variable channels then conditional word target
CUSTOM_BR = {
    0x49: decode_op49,
    0x4A: decode_op49,
}

# ops whose operand is an ABSOLUTE DS word address (phase-D router map).
# value = offset of the addr16 operand from the opcode byte.
ABS_ADDR_OPS = {
    0x53:1, 0x57:1, 0x5A:1, 0x5D:1, 0x60:1, 0x63:1, 0x66:1, 0x6A:1,
    0x6F:1, 0x74:1, 0x79:1, 0x7E:1, 0x83:1, 0x88:1, 0x8D:1, 0x91:1,
    0x94:1, 0x9D:1, 0xA0:1, 0xA3:1, 0xA6:1, 0xAA:2, 0xAF:1, 0xB4:1,
    0xB9:1, 0xBD:1, 0x99:2,
}

def load_draft():
    d = json.load(open('tools/data/optable_draft.json'))
    table = {}
    for k, v in d.items():
        op = int(k, 16)
        dj, dcj = v.get('do_jump', 0), v.get('do_call_jump', 0)
        ob = v.get('op_bytes', 0)
        # branch family: [op][operands][word target]; skip-2 fall or call-return
        if dcj or (dj and v['pc_adds'] == [2]):
            table[op] = ('scheme', (1 + ob + 2, 'br', 1 + ob), v['handler'])
        elif dj and not v['pc_adds']:
            table[op] = ('scheme', (1 + ob + 2, 'jmp', 1 + ob), v['handler'])
        elif v['pc_sets'] == 0 and not v['exits'] and len(v['pc_adds']) <= 1:
            tail = v['pc_adds'][0] if v['pc_adds'] else 0
            table[op] = ('fixed', 1 + ob + tail, v['handler'])
        elif v['pc_sets'] == 0 and v['exits'] and len(v['pc_adds']) <= 1:
            # exit-class op with operands: strand stops (PC saved by yield-family)
            tail = v['pc_adds'][0] if v['pc_adds'] else 0
            table[op] = ('scheme', (1 + ob + tail, 'stop', None), v['handler'])
        else:
            table[op] = ('complex', None, v['handler'])
    return table

def spawn_entries(manifest_dir='assets_raw'):
    """Collect template indices from all level chunks' spawn tables."""
    import glob
    ds = open('ds_static.bin', 'rb').read()
    lc = [struct.unpack_from('<H', ds, 0x940C + i*2)[0] for i in range(48)]
    lt = [struct.unpack_from('<H', ds, 0x946C + i*2)[0] for i in range(48)]
    per_template = {}
    for lvl, (cid, tid) in enumerate(zip(lc, lt)):
        if tid == 0xFFFF:
            continue
        try:
            d = open(f'{manifest_dir}/chunks/dec/{cid:04d}.bin', 'rb').read()
        except FileNotFoundError:
            continue
        off = 0x25F6 - 0x25B3   # spawn table offset inside the level chunk
        while off + 14 <= len(d):
            x = struct.unpack_from('<H', d, off)[0]
            if x == 0xFFFF:
                break
            tmpl = struct.unpack_from('<H', d, off + 8)[0]
            per_template.setdefault(tid, set()).add(tmpl)
            off += 14
    return per_template

def walk(chunk_id, tmpl_indices, table, extra_entries=()):
    d = open(f'assets_raw/chunks/dec/{chunk_id:04d}.bin', 'rb').read()
    entries = {}
    # ALL template/anim records are entry points: objects live by switching
    # animations, each record's +3 PC becomes OBJ_PC (v2_vm.cpp:17559).
    # Record-table extent is unknown a priori — take every index whose PC
    # lands beyond the record area and inside the chunk; the walk itself
    # validates (bad PCs derail into unknown opcodes and are reported).
    t = 0
    while (t + 1) * REC <= len(d):
        off = t * REC + 3
        pc = struct.unpack_from('<H', d, off)[0]
        if pc >= 0x600 and pc < len(d):
            entries.setdefault(pc, f'rec_{t:02X}')
        elif t > 0 and pc == 0:
            break
        t += 1
        if t > 0x400:
            break
    for t2 in sorted(tmpl_indices):
        off = t2 * REC + 3
        if off + 2 <= len(d):
            pc = struct.unpack_from('<H', d, off)[0]
            if pc < len(d):
                entries[pc] = f'tmpl_{t2:02X}'
                if pc + 3 < len(d):
                    entries.setdefault(pc + 3, f'tmpl_{t2:02X}+3')
    for pc in extra_entries:
        entries.setdefault(pc, 'dyn')
    seen = {}
    stops = {}
    parent = {}
    addrs = {}
    anim_entries = set()
    q = deque(entries.keys())
    def push(npc, src):
        if npc not in parent:
            parent[npc] = src
        q.append(npc)
    while q:
        pc = q.popleft()
        if pc in seen or pc >= len(d):
            continue
        op = d[pc]
        info = table.get(op)
        if op in ABS_ADDR_OPS and pc + ABS_ADDR_OPS[op] + 2 <= len(d):
            a = struct.unpack_from('<H', d, pc + ABS_ADDR_OPS[op])[0]
            addrs.setdefault(a, set()).add(op)
        if op == 0x19 and pc + 3 <= len(d):   # set-anim: operand = anim PC
            anim_entries.add(struct.unpack_from('<H', d, pc + 1)[0])
        if op in CUSTOM:
            try:
                ln, tmpls = CUSTOM[op](d, pc)
            except (KeyError, IndexError) as e:
                stops.setdefault(pc, (op, str(e)))
                continue
            seen[pc] = (op, ln)
            push(pc + ln, pc)
            for t in tmpls:
                off = t * REC + 3
                if off + 2 <= len(d):
                    npc = struct.unpack_from('<H', d, off)[0]
                    if npc < len(d):
                        push(npc, pc)
            continue
        if op in CUSTOM_BR:
            ln, _ = CUSTOM_BR[op](d, pc)
            seen[pc] = (op, ln)
            tgt = struct.unpack_from('<H', d, pc + ln - 2)[0]
            if 0 < tgt < len(d):
                push(tgt, pc)
            push(pc + ln, pc)
            continue
        if op in OPERAND_SCHEMES:
            ln, kind, toff = OPERAND_SCHEMES[op]
            seen[pc] = (op, ln)
            if kind in ('jmp', 'br', 'srch') and toff is not None and pc + toff + 2 <= len(d):
                tgt = struct.unpack_from('<H', d, pc + toff)[0]
                if 0 < tgt < len(d):
                    push(tgt, pc)
            if kind in ('fall', 'br'):
                push(pc + ln, pc)
            if kind == 'srch':
                push(pc + ln, pc)       # the 2D/36 continuation instruction
                push(pc + ln + 1, pc)   # no-match path (skips it)
            continue
        if info and info[0] == 'scheme':
            ln, kind, toff = info[1]
            seen[pc] = (op, ln)
            if kind in ('jmp', 'br') and toff is not None and pc + toff + 2 <= len(d):
                tgt = struct.unpack_from('<H', d, pc + toff)[0]
                if 0 < tgt < len(d):
                    push(tgt, pc)
            if kind in ('fall', 'br'):
                push(pc + ln, pc)
            continue
        if info and info[0] == 'fixed':
            ln = info[1]
            seen[pc] = (op, ln)
            push(pc + ln, pc)
        else:
            stops.setdefault(op, []).append(pc)
    return d, entries, seen, stops, parent, addrs, anim_entries

def load_layout_names():
    names = {}
    import re
    for line in open('src/sdl/v2_ds_layout.h'):
        m = re.match(r'constexpr uint16_t (DS|LUT)_([A-Z_0-9]+)\s*=\s*0x([0-9A-Fa-f]+);', line)
        if m:
            names[int(m.group(3), 16)] = m.group(2).lower()
    return names

LAYOUT = None

MNEM_CACHE = {}

def mnemonic(op, table):
    if op in MNEM_CACHE:
        return MNEM_CACHE[op]
    info = table.get(op)
    h = info[2] if info else '?'
    name = h.replace('v2_vm_op_', '')
    if len(name) <= 2:   # bare hex handler name: derive from the table comment
        import json
        d = json.load(open('tools/data/optable_draft.json'))
        c = d.get(f'{op:02X}', {}).get('comment', '')
        c = c.split(':', 1)[-1].strip() if ':' in c else c
        c = c.split('.')[0].split(',')[0].strip()
        if c:
            name = c[:26].replace(' ', '_').lower()
    MNEM_CACHE[op] = name
    return name

def write_listing(cid, d, entries, seen, table, dyn_pcs=()):
    global LAYOUT
    if LAYOUT is None:
        LAYOUT = load_layout_names()
    dyn = set(dyn_pcs)
    rev = {}
    for pc, name in entries.items():
        rev.setdefault(pc, []).append(name)

    def annot(pc, op, ln):
        parts = []
        if op in ABS_ADDR_OPS and pc + ABS_ADDR_OPS[op] + 2 <= len(d):
            a = struct.unpack_from('<H', d, pc + ABS_ADDR_OPS[op])[0]
            parts.append('[' + (LAYOUT.get(a) or f'{a:04X}') + ']')
        sch = OPERAND_SCHEMES.get(op)
        info = table.get(op)
        toff = None
        if sch and sch[1] in ('jmp', 'br'):
            toff = sch[2]
        elif info and info[0] == 'scheme' and info[1][1] in ('jmp', 'br'):
            toff = info[1][2]
        if toff is not None and pc + toff + 2 <= len(d):
            t = struct.unpack_from('<H', d, pc + toff)[0]
            parts.append(f'-> {t:04X}')
        return ' '.join(parts)

    path = f'assets_raw/disasm/{cid:04X}.lst'
    with open(path, 'w') as f:
        f.write(f'; chunk {cid:04X}: {len(seen)} instructions\n')
        for pc in sorted(seen):
            op, ln = seen[pc]
            if pc in rev:
                names = ','.join(sorted(rev[pc]))
                f.write(f'\n{pc:04X} <{names}>:\n')
            b = ' '.join(f'{x:02X}' for x in d[pc:pc+ln])
            mark = '*' if pc in dyn else ' '
            f.write(f'{mark}{pc:04X}: {b:<15} {mnemonic(op, table):<22} {annot(pc, op, ln)}\n')
    return path

def main():
    table = load_draft()
    per_template = spawn_entries()
    args = [int(a, 16) for a in sys.argv[1:]] or list(range(0x1C1, 0x1C7))
    os.makedirs('assets_raw/disasm', exist_ok=True)
    for cid in args:
        tmpls = per_template.get(cid, set())
        d, entries, seen, stops, parent, addrs, anim_entries = walk(cid, tmpls, table)
        cov = sum(l for _, l in seen.values())
        write_listing(cid, d, entries, seen, table)
        print(f'0x{cid:X}: templates={len(tmpls)} entries={len(entries)} '
              f'insns={len(seen)} bytes~{cov} stops={{'
              + ', '.join(f'{op:02X}:{len(v)}' for op, v in
                          sorted(stops.items())[:8]) + '}')
    return 0

if __name__ == '__main__':
    sys.exit(main())
