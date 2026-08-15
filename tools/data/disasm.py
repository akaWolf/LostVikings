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
    0x2C: (4, 'br', 2),       # obj search fwd: 1B filter + word target
    0x2D: (1, 'fall', None),  # search continue (dynamic target = 2C's)
    0x35: (4, 'br', 2),       # obj search (Y): 1B filter + word target
    0x36: (1, 'fall', None),  # search continue (dynamic)
    0xBF: (4, 'br', 2),       # search up: 1B filter + word target
    0xC0: (4, 'br', 2),       # search family
    0xC1: (4, 'br', 2),       # search family
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

def walk(chunk_id, tmpl_indices, table):
    d = open(f'assets_raw/chunks/dec/{chunk_id:04d}.bin', 'rb').read()
    entries = {}
    for t in sorted(tmpl_indices):
        off = t * REC + 3
        if off + 2 > len(d):
            continue
        pc = struct.unpack_from('<H', d, off)[0]
        if pc < len(d):
            entries[pc] = f'tmpl_{t:02X}'
    seen = {}
    stops = {}
    q = deque(entries.keys())
    while q:
        pc = q.popleft()
        if pc in seen or pc >= len(d):
            continue
        op = d[pc]
        info = table.get(op)
        if op in OPERAND_SCHEMES:
            ln, kind, toff = OPERAND_SCHEMES[op]
            seen[pc] = (op, ln)
            if kind in ('jmp', 'br') and toff is not None and pc + toff + 2 <= len(d):
                tgt = struct.unpack_from('<H', d, pc + toff)[0]
                if tgt < len(d):
                    q.append(tgt)
            if kind in ('fall', 'br'):
                q.append(pc + ln)
            continue
        if info and info[0] == 'scheme':
            ln, kind, toff = info[1]
            seen[pc] = (op, ln)
            if kind in ('jmp', 'br') and toff is not None and pc + toff + 2 <= len(d):
                tgt = struct.unpack_from('<H', d, pc + toff)[0]
                if tgt < len(d):
                    q.append(tgt)
            if kind in ('fall', 'br'):
                q.append(pc + ln)
            continue
        if info and info[0] == 'fixed':
            ln = info[1]
            seen[pc] = (op, ln)
            q.append(pc + ln)
        else:
            stops.setdefault(op, []).append(pc)
    return d, entries, seen, stops

def main():
    table = load_draft()
    per_template = spawn_entries()
    args = [int(a, 16) for a in sys.argv[1:]] or list(range(0x1C1, 0x1C7))
    os.makedirs('assets_raw/disasm', exist_ok=True)
    for cid in args:
        tmpls = per_template.get(cid, set())
        d, entries, seen, stops = walk(cid, tmpls, table)
        cov = sum(l for _, l in seen.values())
        print(f'0x{cid:X}: templates={len(tmpls)} entries={len(entries)} '
              f'insns={len(seen)} bytes~{cov} stops={{'
              + ', '.join(f'{op:02X}:{len(v)}' for op, v in
                          sorted(stops.items())[:8]) + '}')
    return 0

if __name__ == '__main__':
    sys.exit(main())
