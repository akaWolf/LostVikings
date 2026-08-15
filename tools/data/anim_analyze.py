#!/usr/bin/env python3
"""Stage 1.2b: anim-VM stream analysis from V2_ANIM_DUMP corpus.

Input lines: TMPL BX_BEFORE CMD BX_AFTER (hex). Derives:
  - per-cmd consumed-length histogram (bx_after - bx_before, excluding
    control-flow cmds whose bx jumps);
  - per-template set of visited anim PCs (entries for the anim lister);
  - flags variable-length cmds (multiple distinct deltas).
Usage: anim_analyze.py /tmp/animdump/*.txt
"""
import sys, struct, collections

def main():
    deltas = collections.defaultdict(collections.Counter)
    visited = collections.defaultdict(set)
    for path in sys.argv[1:]:
        for line in open(path):
            t, b0, cmd, b1 = (int(x, 16) for x in line.split())
            visited[t].add(b0)
            deltas[cmd][(b1 - b0) & 0xFFFF] += 1
    print('cmd  deltas (len:count)')
    for cmd in sorted(deltas):
        ds = deltas[cmd]
        s = ', '.join(f'{d}:{n}' for d, n in sorted(ds.items())[:8])
        var = ' VAR' if len(ds) > 1 else ''
        print(f' {cmd:02X}  {s}{var}')
    for t in sorted(visited):
        print(f'tmpl {t:04X}: {len(visited[t])} anim pcs')

if __name__ == '__main__':
    main()
