#!/usr/bin/env python3
"""Stage 2.1: bytecode -> .lvs decompiler, v0 (CFG + basic structure).

Builds basic blocks over the validated walker decode, then emits .lvs
draft text per template record: linear runs, `if ... goto` for branch
ops, `loop`/`goto` labels for back-edges, `yield`/`exit` terminators.
Structure recovery is deliberately conservative in v0 — anything not
matched becomes an explicit labeled goto, which the compiler (stage
2.3) can always re-emit byte-identically.

Usage: decompile.py CHUNK_HEX [TEMPLATE_IDX_HEX] [--dyn glob]
"""
import sys, struct, glob, importlib.util
from collections import defaultdict

spec = importlib.util.spec_from_file_location('dz', 'tools/data/disasm.py')
dz = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dz)

def decode_info(d, pc, op, table):
    """(len, kind, target|None) for a decoded instruction."""
    if op in dz.CUSTOM:
        ln, _ = dz.CUSTOM[op](d, pc)
        return ln, 'fall', None
    if op in dz.CUSTOM_BR:
        ln, _ = dz.CUSTOM_BR[op](d, pc)
        tgt = struct.unpack_from('<H', d, pc + ln - 2)[0]
        return ln, 'br', tgt
    if op in dz.OPERAND_SCHEMES:
        ln, kind, toff = dz.OPERAND_SCHEMES[op]
        tgt = None
        if kind in ('jmp', 'br', 'srch') and toff is not None:
            tgt = struct.unpack_from('<H', d, pc + toff)[0]
        return ln, kind, tgt
    info = table.get(op)
    if info and info[0] == 'scheme':
        ln, kind, toff = info[1]
        tgt = None
        if kind in ('jmp', 'br') and toff is not None:
            tgt = struct.unpack_from('<H', d, pc + toff)[0]
        return ln, kind, tgt
    if info and info[0] == 'fixed':
        return info[1], 'fall', None
    return 1, 'stop', None

def build_blocks(d, seen, table):
    """Split decoded instruction set into basic blocks."""
    leaders = set()
    succs = {}
    for pc in seen:
        op = seen[pc][0]
        ln, kind, tgt = decode_info(d, pc, op, table)
        nxt = pc + ln
        out = []
        if kind in ('fall', 'br') and nxt in seen:
            out.append(nxt)
        if tgt is not None and tgt in seen:
            out.append(tgt)
            leaders.add(tgt)
        if kind in ('jmp', 'br') or kind == 'stop':
            if nxt in seen:
                leaders.add(nxt)
        succs[pc] = (kind, out)
    blocks = {}
    for pc in sorted(seen):
        op, ln = seen[pc]
        prev = None
    # linear block assembly
    order = sorted(seen)
    cur = None
    for pc in order:
        if cur is None or pc in leaders or pc != cur[-1][0] + seen[cur[-1][0]][1]:
            cur = []
            blocks[pc] = cur
        cur.append((pc, seen[pc][0]))
    return blocks, succs

def emit(cid, tmpl, dyn_glob='/tmp/pcdump/*.txt'):
    table = dz.load_draft()
    pt = dz.spawn_entries()
    dyn = set()
    for path in glob.glob(dyn_glob):
        for line in open(path):
            t, pc, op = (int(x, 16) for x in line.split())
            if t == cid:
                dyn.add(pc)
    d, entries, seen, stops, parent, addrs, anim_e = dz.walk(
        cid, pt.get(cid, set()), table, extra_entries=dyn)
    rec = tmpl * dz.REC
    P = struct.unpack_from('<H', d, rec + 3)[0]
    roots = {P: 'anim_entry', P + 3: 'on_spawn'}
    # collect reachable-from-roots subset
    sub = {}
    work = list(roots)
    while work:
        pc = work.pop()
        if pc in sub or pc not in seen:
            continue
        op = seen[pc][0]
        ln, kind, tgt = decode_info(d, pc, op, table)
        sub[pc] = (op, ln, kind, tgt)
        if kind in ('fall', 'br') and pc + ln in seen:
            work.append(pc + ln)
        if tgt is not None and tgt in seen:
            work.append(tgt)
    # label targets
    targets = {t for (_, _, k, t) in sub.values() if t is not None}
    lab = {t: f'L_{t:04X}' for t in sorted(targets)}
    LAY = dz.load_layout_names()
    out = []
    hdr = struct.unpack_from('<HB', d, rec)
    out.append(f'template t{tmpl:02X} {{  # chunk {cid:04X}, record @{rec:04X}')
    out.append(f'    sprite_chunk 0x{hdr[0]:X}')
    out.append(f'    subsprites   {hdr[1] & 0x7F}'
               + ('  bigpool' if hdr[1] & 0x80 else ''))
    for name, root in (('anim_entry', P), ('on_spawn', P + 3)):
        out.append(f'    {name} {{')
        for pc in sorted(sub):
            if not (pc == root or True):
                continue
        emitted = set()
        stack = [root]
        while stack:
            pc = stack.pop()
            while pc in sub and pc not in emitted:
                emitted.add(pc)
                op, ln, kind, tgt = sub[pc]
                if pc in lab:
                    out.append(f'      {lab[pc]}:')
                mn = dz.mnemonic(op, table)
                ann = ''
                if op in dz.ABS_ADDR_OPS:
                    a = struct.unpack_from('<H', d, pc + dz.ABS_ADDR_OPS[op])[0]
                    ann = f' [{LAY.get(a, f"{a:04X}")}]'
                # raw operand bytes (excluding the target word for br/jmp) —
                # v0 keeps byte identity; readability layers come later.
                ob_end = pc + ln - (2 if (kind in ('br', 'jmp') and tgt is not None) else 0)
                raw = d[pc+1:ob_end]
                raws = (' ' + raw.hex()) if raw else ''
                if kind == 'br' and tgt is not None:
                    out.append(f'      if {mn}.{op:02X}{raws}{ann} goto {lab.get(tgt, hex(tgt))}')
                    if tgt in sub and tgt not in emitted:
                        stack.append(tgt)
                    pc += ln
                elif kind == 'jmp' and tgt is not None:
                    out.append(f'      goto.{op:02X} {lab.get(tgt, hex(tgt))}')
                    pc = tgt
                elif kind == 'stop':
                    out.append(f'      {mn}.{op:02X}{raws}{ann}')
                    break
                else:
                    out.append(f'      {mn}.{op:02X}{raws}{ann}')
                    pc += ln
        out.append('    }')
    out.append('}')
    return '\n'.join(out)

def main():
    cid = int(sys.argv[1], 16)
    tmpl = int(sys.argv[2], 16) if len(sys.argv) > 2 else None
    if tmpl is None:
        print('need template idx')
        return 1
    print(emit(cid, tmpl))
    return 0

if __name__ == '__main__':
    sys.exit(main())
