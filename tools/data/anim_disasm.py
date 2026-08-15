#!/usr/bin/env python3
"""Stage 1.2b: anim-VM (27 commands) stream walker.

Grammar source: v2_vm_exec_anim_cmd (v2_vm.cpp) — handler table at
ds:0x86E6, command byte indexes it. Fixed lengths below are from the
handler bodies; VAR commands (per-sub-sprite loops, conditional
payloads) take their consumed length from the live V2_ANIM_DUMP corpus
(bx_before->bx_after pairs) when files are supplied.

Entries: op_19 operands harvested by the object-code walker, plus any
dynamic anim PCs from the corpus.
Usage: anim_disasm.py [dumpfile ...]
"""
import sys, struct, glob, collections, importlib.util

spec = importlib.util.spec_from_file_location('dz', 'tools/data/disasm.py')
dz = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dz)

# cmd -> ('fixed', operand_bytes) | ('jump',) | ('loopstart',)
# | ('loopback',) | ('stop', operand_bytes) | ('var',)
ANIM_SCHEME = {
    0x00: ('fixed', 1),   # advance sprite data offset
    0x01: ('var',),       # advance sprite, conditional payload (live: 1..24B)
    0x02: ('fixed', 2),   # play sound
    0x03: ('jump',),      # bx = word
    0x04: ('fixed', 1),   # skip 1 byte
    0x05: ('loopstart',), # save bx+2, jump word
    0x06: ('loopback',),  # bx = saved
    0x07: ('fixed', 1),   # X signed offset
    0x08: ('var',),       # X absolute, 2B per sub-sprite
    0x09: ('fixed', 1),   # Y signed offset
    0x0A: ('var',),       # Y absolute, 2B per sub-sprite
    0x0B: ('fixed', 0),   # int3 (debug nop)
    0x0C: ('var',),       # palette/layer bits
    0x0D: ('fixed', 1),   # set mask
    0x0E: ('stop', 0),    # end frame
    0x0F: ('stop', 1),    # set delay timer, exit
    0x10: ('fixed', 0),   # XOR flags 0x200 (family)
    0x11: ('fixed', 0),   # XOR flags 0x400
    0x12: ('fixed', 0),   # XOR flags 0x600
    0x13: ('var',),       # set sub-sprite mask
    0x14: ('fixed', 1),   # sprite decompression
    0x15: ('var',),       # sprite type/data setup
    0x16: ('fixed', 1),   # skip (dup of 4)
    0x17: ('fixed', 2),   # sprite resource lookup
    0x18: ('fixed', 0),   # 339F (live: always 0 operand bytes)
    0x19: ('fixed', 0),   # 33DE (live: always 0 operand bytes)
    0x1A: ('stop', 0),    # end animation (bx=FFFF)
}

def load_dyn(paths):
    lens = collections.defaultdict(lambda: collections.defaultdict(set))
    pcs = collections.defaultdict(set)
    for path in paths:
        for line in open(path):
            t, b0, cmd, b1 = (int(x, 16) for x in line.split())
            pcs[t].add(b0)
            lens[t][b0].add(b1)
    return lens, pcs

def walk_anim(cid, entries, dynlens):
    d = open(f'assets_raw/chunks/dec/{cid:04d}.bin', 'rb').read()
    seen = {}
    stops = collections.Counter()
    q = list(entries)
    while q:
        pc = q.pop()
        if pc in seen or pc + 1 > len(d):
            continue
        cmd = d[pc]
        sch = ANIM_SCHEME.get(cmd)
        if sch is None:
            stops[cmd] += 1
            continue
        kind = sch[0]
        if kind == 'fixed':
            seen[pc] = cmd
            q.append(pc + 1 + sch[1])
        elif kind == 'jump' or kind == 'loopstart':
            seen[pc] = cmd
            if pc + 3 <= len(d):
                t = struct.unpack_from('<H', d, pc + 1)[0]
                if 0 < t < len(d):
                    q.append(t)
            if kind == 'loopstart':
                q.append(pc + 3)
        elif kind == 'loopback':
            seen[pc] = cmd   # dynamic target (loop head) — reached via loopstart
        elif kind == 'stop':
            seen[pc] = cmd
        elif kind == 'var':
            nxts = dynlens.get(pc)
            seen[pc] = cmd
            if nxts:
                for b1 in nxts:
                    if 0 < b1 < len(d):
                        q.append(b1)
            else:
                stops[cmd] += 1   # unmeasured VAR — needs corpus coverage
    return d, seen, stops

def main():
    dynlens, dynpcs = load_dyn(sys.argv[1:] or glob.glob('/tmp/animdump/*.txt'))
    table = dz.load_draft()
    pt = dz.spawn_entries()
    for cid in range(0x1C1, 0x1C7):
        _, _, _, _, _, _, anim_entries = dz.walk(cid, pt.get(cid, set()), table)
        entries = set(anim_entries) | dynpcs.get(cid, set())
        d, seen, stops = walk_anim(cid, entries, dynlens.get(cid, {}))
        print(f'0x{cid:X}: anim entries={len(entries)} cmds={len(seen)} '
              f'stops={dict(sorted(stops.items())[:8])}')

if __name__ == '__main__':
    main()
