#!/usr/bin/env python3
"""anim_static — static operand lengths of the anim-VM commands.

The five VAR commands (01 08 0A 0C 13) consume bytes per sub-sprite slot
(v2_vm.cpp v2_vm_exec_anim_cmd; every loop is a do-while over the object's
[OBJ_SUB_SLOT, OBJ_SUB_END) window, so n = max(1, sub-sprite count)):
  08 / 0A   : 2 bytes x n                                (mask ignored)
  01 / 13   : unmasked (ds:38C == 0): 1 byte x n
              masked: 1 byte per slot whose OBJ_SUB_CLASS & mask != 0
  0C        : unmasked: 1 byte x n; masked: n bytes if OBJ_SUB_CLASS of the
              FIXED slot 0x18 (di = cmd*2, an original-code quirk) & mask,
              else 0 — a global runtime gate, not a property of the stream
The mask is ds:38C: reset to 0 at every frame start (v2_vm_anim_interp_1303a),
set by command 0D inside the frame. The classes are per-slot object fields:
0 at spawn (v2_slot_init), rewritten by command 13 (per slot that received a
byte) — they persist across frames and anims.

This module walks every anim stream from its entries (op 19 operands of the
object code, attributed to the owning class records for n) with that state
(pc, mask, classes, loop-save) and collects the possible byte lengths of
every VAR site. The V2_ANIM_DUMP corpus (/tmp/animdump/*.txt) is the oracle:
a measured length must be among the static ones.

Usage:  anim_static.py stats [CID ...]      per-chunk coverage + oracle check
        anim_static.py lens CID             -> {pc: length} for unique sites
"""
import sys, os, glob, struct, collections, importlib.util

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
REC = 0x15
UNK = -1      # unknown class value

_mods = {}
def mod(name):
    if name not in _mods:
        sp = importlib.util.spec_from_file_location(name, os.path.join(HERE, name + '.py'))
        m = importlib.util.module_from_spec(sp)
        cwd = os.getcwd(); os.chdir(ROOT)
        try: sp.loader.exec_module(m)
        finally: os.chdir(cwd)
        _mods[name] = m
    return _mods[name]


def anim_entries_with_owners(cid):
    """-> {anim_pc: set(n)} : op 19 operands of the walked object code, each
    with the sub-sprite counts of the class records that own the site."""
    lf = mod('lvs_full'); ls = mod('lvs_struct')
    cwd = os.getcwd(); os.chdir(ROOT)
    try:
        d, seen, table = lf.full_walk(cid)
        own = ls.owners_of_states(cid)
    finally: os.chdir(cwd)
    ns = {}
    for pc, (op, ln) in seen.items():
        if op != 0x19: continue
        a = struct.unpack_from('<H', d, pc + 1)[0]
        if not (0x600 <= a < len(d)): continue
        for t in own.get(pc, ()):
            n = d[t * REC + 2] & 0x7F
            ns.setdefault(a, set()).add(n)
        ns.setdefault(a, set())
    return d, ns


FIXED = {0x00: 1, 0x02: 2, 0x04: 1, 0x07: 1, 0x09: 1, 0x0B: 0, 0x0D: 1, 0x0E: 0, 0x0F: 1,
         0x10: 0, 0x11: 0, 0x12: 0, 0x14: 1, 0x15: 1, 0x16: 1, 0x17: 2, 0x18: 0, 0x19: 0, 0x1A: 0}
VAR = {0x01, 0x08, 0x0A, 0x0C, 0x13}


def walk_stream(d, entry, n, lens, states_seen, limit=200000, cls0=None, cls_out=None):
    """Abstract run from `entry` for an owner with n sub-sprites. lens[pc] gets
    the set of possible operand lengths ('?' when unknown) of VAR sites.
    cls0: the class tuple at entry (default: all 0 = the spawn state);
    cls_out: a set collecting every class tuple that occurs on the run (the
    classes persist across frames AND anims of the same object, so they feed
    the entry states of the owner's other anims — see analyze())."""
    it = max(1, n)
    start = (entry, 0, cls0 if cls0 is not None else (0,) * it, None)      # pc, mask, classes, loop save
    stack = [start]
    steps = 0
    while stack and steps < limit:
        steps += 1
        st = stack.pop()
        if st in states_seen: continue
        states_seen.add(st)
        pc, mask, cls, cont = st
        if cls_out is not None: cls_out.add(cls)
        if pc < 0x600 or pc >= len(d): continue     # anim code never lives in the class table
        cmd = d[pc]
        if cmd > 0x1A: continue                       # not a command: dead path
        if cmd in FIXED:
            ln = FIXED[cmd]
            if cmd == 0x0D:
                stack.append((pc + 2, d[pc + 1] if pc + 1 < len(d) else 0, cls, cont)); continue
            if cmd in (0x0E, 0x0F):                   # frame end: next frame resumes after it, mask reset
                stack.append((pc + 1 + ln, 0, cls, cont)); continue
            if cmd == 0x1A: continue
            stack.append((pc + 1 + ln, mask, cls, cont)); continue
        if cmd == 0x03:
            if pc + 3 <= len(d): stack.append((struct.unpack_from('<H', d, pc + 1)[0], mask, cls, cont))
            continue
        if cmd == 0x05:
            if pc + 3 <= len(d):
                t = struct.unpack_from('<H', d, pc + 1)[0]
                stack.append((t, mask, cls, pc + 3))
            continue
        if cmd == 0x06:
            if cont is not None: stack.append((cont, mask, cls, cont))
            else: lens.setdefault(pc, set()).add('?loop')
            continue
        # VAR commands
        if cmd in (0x08, 0x0A):
            L = 2 * it; lens.setdefault(pc, set()).add(L); stack.append((pc + 1 + L, mask, cls, cont)); continue
        if cmd in (0x01, 0x13):
            if mask == 0:
                L = it
                ncls = cls
                if cmd == 0x13:
                    ncls = tuple(d[pc + 1 + i] if pc + 1 + i < len(d) else UNK for i in range(it))
                lens.setdefault(pc, set()).add(L); stack.append((pc + 1 + L, mask, ncls, cont)); continue
            if any(c == UNK for c in cls):
                lens.setdefault(pc, set()).add('?cls'); continue
            hits = [i for i in range(it) if cls[i] & mask]
            L = len(hits)
            ncls = cls
            if cmd == 0x13:
                lst = list(cls); k = 0
                for i in hits:
                    lst[i] = d[pc + 1 + k] if pc + 1 + k < len(d) else UNK; k += 1
                ncls = tuple(lst)
            lens.setdefault(pc, set()).add(L); stack.append((pc + 1 + L, mask, ncls, cont)); continue
        if cmd == 0x0C:
            if mask == 0:
                L = it; lens.setdefault(pc, set()).add(L); stack.append((pc + 1 + L, mask, cls, cont)); continue
            # masked: the gate is slot 0x18's class — global runtime state: both outcomes
            lens.setdefault(pc, set()).update({0, it, '?gate'})
            stack.append((pc + 1, mask, cls, cont)); stack.append((pc + 1 + it, mask, cls, cont)); continue
    return steps


def analyze(cid, dyn=None):
    """Per owner n: the class state is shared by all anims the object may run
    (the object code switches anims at will), so iterate: the class tuples
    seen anywhere in the owner's anims become entry states of all of them,
    until no new tuple appears."""
    d, entries = anim_entries_with_owners(cid)
    lens = {}
    by_n = collections.defaultdict(set)
    for a, ns in entries.items():
        for n in ns: by_n[n].add(a)
    for n, anims in by_n.items():
        it = max(1, n)
        known = {(0,) * it}
        done = set()                       # (entry, cls0) pairs already run
        while True:
            new = set()
            for a in anims:
                for c0 in list(known):
                    if (a, c0) in done: continue
                    done.add((a, c0))
                    walk_stream(d, a, n, lens, set(), cls0=c0, cls_out=new)
            new -= known
            if not new: break
            known |= new
            if len(known) > 64: break      # safety: an anim family with an exploding class space
    return d, entries, lens


def resolved(cid, dyn=None):
    """-> {pc: operand length} for every VAR site the model settles:
    unique static length; else the corpus measurement when unique; else the
    single parse whose continuation starts with a valid command byte (the
    others die at once on a byte > 0x1A — such a stream could never run).
    dyn: {pc: set(lengths incl. the cmd byte)} from load_dyn (optional)."""
    d, entries, lens = analyze(cid)
    out = {}
    for pc, s in lens.items():
        ints = sorted(x for x in s if isinstance(x, int))
        if len(ints) == 1 and not any(isinstance(x, str) for x in s):
            out[pc] = ints[0]; continue
        if dyn and pc in dyn and len(dyn[pc]) == 1:
            m = next(iter(dyn[pc])) - 1
            if m in ints: out[pc] = m; continue
        alive = [L for L in ints if pc + 1 + L < len(d) and d[pc + 1 + L] <= 0x1A]
        if len(alive) == 1: out[pc] = alive[0]
    return out


def load_dyn(cid):
    ad = mod('anim_disasm')
    lens, pcs = ad.load_dyn(glob.glob('/tmp/animdump/*.txt'))
    return {pc: {b1 - pc for b1 in s} for pc, s in lens.get(cid, {}).items()}, pcs.get(cid, set())


def stats(cids):
    for cid in cids:
        d, entries, lens = analyze(cid)
        dyn, dynpcs = load_dyn(cid)
        var_sites = {pc for pc in lens}
        uniq = {pc for pc, s in lens.items() if len(s) == 1 and isinstance(next(iter(s)), int)}
        amb = {pc for pc, s in lens.items() if len(s) > 1 and all(isinstance(x, int) for x in s)}
        unk = {pc for pc, s in lens.items() if any(isinstance(x, str) for x in s)}
        multi = sum(1 for a, ns in entries.items() if len(ns) > 1)
        noown = sum(1 for a, ns in entries.items() if not ns)
        # oracle: measured VAR sites (cmd pc with a VAR command byte)
        meas = {pc: s for pc, s in dyn.items() if pc < len(d) and d[pc] in VAR}
        ok = bad = miss = 0; badlist = []
        for pc, ms in meas.items():
            ss = lens.get(pc)
            if ss is None: miss += 1; continue
            ints = {x for x in ss if isinstance(x, int)}
            # dyn lengths are (bx_after - cmd pc) = 1 + operand bytes
            if all((m - 1) in ints for m in ms): ok += 1
            else:
                bad += 1
                if len(badlist) < 8: badlist.append((f'{pc:04X}', f'{d[pc]:02X}', sorted(m - 1 for m in ms), sorted(ss, key=str)))
        print(f'{cid:X}: anim entries {len(entries)} (multi-n {multi}, no owner {noown}); VAR sites reached {len(var_sites)}: '
              f'unique {len(uniq)}, ambiguous {len(amb)}, unknown {len(unk)} | oracle: measured {len(meas)}, agree {ok}, DISAGREE {bad}, not reached {miss}')
        if badlist: print('   disagreements (pc, cmd, measured, static):', badlist)
        unk_by = collections.Counter()
        for pc in unk:
            for x in lens[pc]:
                if isinstance(x, str): unk_by[(f'{d[pc]:02X}', x)] += 1
        if unk_by: print('   unknown by (cmd, reason):', dict(unk_by))


def main():
    a = sys.argv[1:]
    if not a: print(__doc__); return 1
    if a[0] == 'stats':
        stats([int(x, 16) for x in a[1:]] or list(range(0x1C1, 0x1C7))); return 0
    if a[0] == 'lens':
        cid = int(a[1], 16); d, entries, lens = analyze(cid)
        for pc in sorted(lens):
            s = lens[pc]
            if len(s) == 1 and isinstance(next(iter(s)), int): print(f'{pc:04X} {d[pc]:02X} {next(iter(s))}')
        return 0
    print(__doc__); return 1


if __name__ == '__main__':
    sys.exit(main())
