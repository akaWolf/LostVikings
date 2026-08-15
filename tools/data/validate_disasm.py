#!/usr/bin/env python3
"""Stage 1.2: validate the static walker against live V2_PC_DUMP streams.

Input: one or more dump files of "TMPL PC OP" hex triples (from the v2
dispatcher under V2_PC_DUMP). For every template chunk present:
  - run the static walk;
  - report DYN-not-in-STATIC pcs (missed entries / broken reach);
  - report OP MISMATCHES: pc decoded statically as a different opcode
    (the tell-tale of a wrong operand width upstream);
  - report static coverage vs dynamic set size.
Usage: validate_disasm.py dump1.txt [dump2.txt ...]
"""
import sys, importlib.util

spec = importlib.util.spec_from_file_location('dz', 'tools/data/disasm.py')
dz = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dz)

def main():
    allad = {}
    dyn = {}   # tmpl -> {pc: op}
    for path in sys.argv[1:]:
        for line in open(path):
            t, pc, op = (int(x, 16) for x in line.split())
            dyn.setdefault(t, {})[pc] = op
    table = dz.load_draft()
    pt = dz.spawn_entries()
    for tmpl in sorted(dyn):
        stream = dyn[tmpl]
        d, entries, seen, stops, parent, addrs = dz.walk(tmpl, pt.get(tmpl, set()), table, extra_entries=stream.keys())
        missed = [pc for pc in stream if pc not in seen]
        mism = [(pc, stream[pc], seen[pc][0]) for pc in stream
                if pc in seen and seen[pc][0] != stream[pc]]
        print(f'tmpl 0x{tmpl:X}: dyn={len(stream)} static={len(seen)} '
              f'dyn_missed={len(missed)} op_mismatch={len(mism)}')
        for pc in sorted(missed)[:6]:
            print(f'  MISSED pc={pc:04X} dyn_op={stream[pc]:02X} '
                  f'bytes={" ".join(f"{x:02X}" for x in d[pc:pc+6])}')
        for pc, dop, sop in sorted(mism)[:6]:
            print(f'  MISMATCH pc={pc:04X} dyn={dop:02X} static={sop:02X}')
        allad.setdefault(tmpl, addrs)

    merged = {}
    for t, am in allad.items():
        for a, ops in am.items():
            merged.setdefault(a, set()).update(ops)
    import json
    json.dump({f'{a:04X}': sorted(f'{o:02X}' for o in ops)
               for a, ops in sorted(merged.items())},
              open('assets_raw/operand_addr_map.json', 'w'), indent=1)
    print(f'operand addr map: {len(merged)} unique DS addresses -> assets_raw/operand_addr_map.json')

if __name__ == '__main__':
    main()
