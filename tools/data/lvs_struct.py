#!/usr/bin/env python3
"""Stage 2.2: structured lift v1 — if-then[-else] recovery over the CFG.

Conservative pattern lift on top of the anchored identity layer:
  br T; A... ; (merge at T)              ->  if !cond { A }        (then=fall)
  br T; A...; jmp M; T: B...; (merge M)  ->  if !cond { A } else { B }
Anything else stays a labeled goto. Loops (back-edges) are left as
goto in v1. The structured text keeps block anchors, so compilation
is deterministic and the byte round-trip must stay green.

Usage: lvs_struct.py stats CHUNK_HEX   — lift coverage metrics
"""
import sys, struct, glob, importlib.util
from collections import defaultdict

spec = importlib.util.spec_from_file_location('dz', 'tools/data/disasm.py')
dz = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dz)
spec2 = importlib.util.spec_from_file_location('dc', 'tools/data/decompile.py')
dc = importlib.util.module_from_spec(spec2)
spec2.loader.exec_module(dc)
spec3 = importlib.util.spec_from_file_location('lf', 'tools/data/lvs_full.py')
lf = importlib.util.module_from_spec(spec3)
spec3.loader.exec_module(lf)

def analyze(cid):
    d, seen, table = lf.full_walk(cid)
    info = {}
    preds = defaultdict(set)
    for pc in seen:
        op = seen[pc][0]
        ln, kind, tgt = dc.decode_info(d, pc, op, table)
        info[pc] = (op, ln, kind, tgt)
        nxt = pc + ln
        if kind in ('fall', 'br') and nxt in seen:
            preds[nxt].add(pc)
        if kind == 'srch':
            for n in (pc + ln, pc + ln + 1):
                if n in seen:
                    preds[n].add(pc)
        if tgt is not None and tgt in seen:
            preds[tgt].add(pc)
    lifted = 0
    if_then = 0
    if_else = 0
    total_br = 0
    for pc, (op, ln, kind, tgt) in info.items():
        if kind != 'br' or tgt is None or tgt not in seen:
            continue
        total_br += 1
        # walk the fall (then) side linearly until we hit tgt or flow break
        cur = pc + ln
        body = 0
        ok = False
        while cur in info and body < 64:
            o2, l2, k2, t2 = info[cur]
            if cur == tgt:
                ok = True
                break
            if k2 == 'fall':
                cur += l2
                body += 1
                continue
            if k2 == 'jmp' and t2 is not None:
                # candidate else: fall-part ends jumping to merge M;
                # tgt-side runs until M
                m = t2
                cur2 = tgt
                b2 = 0
                ok2 = False
                while cur2 in info and b2 < 64:
                    o3, l3, k3, t3 = info[cur2]
                    if cur2 == m:
                        ok2 = True
                        break
                    if k3 == 'fall':
                        cur2 += l3
                        b2 += 1
                        continue
                    break
                if ok2 and len(preds[tgt]) == 1:
                    if_else += 1
                    lifted += 1
                break
            break
        else:
            ok = False
        if ok and len(preds[tgt]) >= 1 and body > 0:
            if_then += 1
            lifted += 1
    return len(info), total_br, if_then, if_else

def main():
    if sys.argv[1] == 'stats':
        args = [int(a, 16) for a in sys.argv[2:]] or list(range(0x1C1, 0x1C7))
        for cid in args:
            n, br, it, ie = analyze(cid)
            print(f'0x{cid:X}: insns={n} branches={br} lift_if_then={it} '
                  f'lift_if_else={ie} lift%={100.0*(it+ie)/max(br,1):.1f}')
    return 0

if __name__ == '__main__':
    sys.exit(main())
