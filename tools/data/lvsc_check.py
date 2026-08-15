#!/usr/bin/env python3
"""Stage 2.3 seed: per-instruction recompile-identity check.

For every decoded instruction of a chunk, re-encode from its .lvs v0
form (opcode suffix + raw operand hex + label target) and compare with
the original bytes. This is the compiler's identity core: once every
instruction round-trips, whole-chunk emission is just layout.

Usage: lvsc_check.py CHUNK_HEX [--dyn glob]
"""
import sys, struct, glob, importlib.util

spec = importlib.util.spec_from_file_location('dz', 'tools/data/disasm.py')
dz = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dz)
spec2 = importlib.util.spec_from_file_location('dc', 'tools/data/decompile.py')
dc = importlib.util.module_from_spec(spec2)
spec2.loader.exec_module(dc)

def main():
    cid = int(sys.argv[1], 16)
    table = dz.load_draft()
    pt = dz.spawn_entries()
    dyn = set()
    for path in glob.glob('/tmp/pcdump/*.txt'):
        for line in open(path):
            t, pc, op = (int(x, 16) for x in line.split())
            if t == cid:
                dyn.add(pc)
    d, entries, seen, stops, parent, addrs, anim_e = dz.walk(
        cid, pt.get(cid, set()), table, extra_entries=dyn)
    bad = 0
    total = 0
    for pc in sorted(seen):
        op = seen[pc][0]
        ln, kind, tgt = dc.decode_info(d, pc, op, table)
        # re-encode: opcode byte + operand raw + (target word if any)
        ob_end = pc + ln - (2 if (kind in ('br', 'jmp') and tgt is not None) else 0)
        enc = bytes([op]) + d[pc+1:ob_end]
        if kind in ('br', 'jmp') and tgt is not None:
            enc += struct.pack('<H', tgt)
        total += 1
        if enc != d[pc:pc+ln]:
            bad += 1
            if bad <= 5:
                print(f'  MISMATCH pc={pc:04X}: enc={enc.hex()} orig={d[pc:pc+ln].hex()}')
    print(f'0x{cid:X}: instructions={total} recompile_identity_fail={bad}')
    return 0 if bad == 0 else 1

if __name__ == '__main__':
    sys.exit(main())
