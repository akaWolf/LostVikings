#!/usr/bin/env python3
"""snes_sfx_map.py — the PC -> SNES effect-id map (UX stage 10).

The world scripts are the same programs on the PC and on the SNES DE, but the
effect ids in op 2 (play effect: id, vol), op 4 (stop) and the animation
command 2 are each machine's own: the PC's XMIDI sequence numbers (0x03..0x5D)
against the console driver's sequences (0x80..0xEA; the ids below 0x80 are the
songs). This tool pairs the sites:
  1. classes: the spawn rows of the shared levels (identical coordinates) name
     the PC record on one side and the SNES global class on the other; the
     three vikings are records 0..2 of every world; a class's code bank on the
     SNES (3 or 4) is the one whose walk aligns best;
  2. the class code is walked with the PC decoder on both sides, the opcode
     sequences aligned (difflib), op-2 sites in equal runs paired;
  3. op-19 (set-anim) operands in equal runs pair the animation scripts, which
     are walked in lockstep — the PC corpus /tmp/animdump/*.txt (V2_ANIM_DUMP
     over the canon replays) supplies the consumed length of every VAR command
     and the SNES command at the same position takes the same length; the
     command-2 sites are paired.
Votes are tallied per PC id (the volume byte is a check, not a key); the
majority wins, conflicts are listed. Output: tools/assets/snes_sfx_map.json
{"map": {pc_hex: snes_hex}, "votes": {...}, "conflicts": [...]} — packed by
integrate_snes.py as chunk 0x317 (256 bytes, 0 = no twin).
Usage (repo root, corpus present): python3 tools/assets/snes_sfx_map.py"""

import sys, struct, importlib.util, collections, difflib
sys.path.insert(0, 'tools/assets'); sys.path.insert(0, 'tools/data')
def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path); m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m
dz = load('dz', 'tools/data/disasm.py'); dc = load('dc', 'tools/data/decompile.py')
import level_render as LR, parallax_snes as PX, snes2pc as SP
de = SP.SnesRom(); SNES = de.rom
import json
CLASS_BANK = json.load(open('/tmp/lv_class_banks.json'))
BANKS = {b: SNES[b*0x8000:(b+1)*0x8000] for b in range(2, 12)}
TABLE = dz.load_draft()
tables = LR.level_tables()
de_of = {s: s for s in range(0x25)}; de_of.update(PX.PC_STORY); de_of[0x2F] = 0x34

def key(r): return (r['x'], r['y'], r['half_w'], r['half_h'])
cls_pairs = collections.Counter(); rowstats = collections.Counter()
for slot, de_slot in sorted(de_of.items()):
    if slot >= len(tables): continue
    hdr, script = tables[slot]
    if hdr == 0xFFFF or script == 0xFFFF: continue
    try:
        pc_st = LR.parse_stripe(LR.header_raw(f'{hdr:04X}')); hid, h = PX.de_head(de, de_slot); sn_st = LR.parse_stripe(h)
    except Exception: rowstats['level parse fail'] += 1; continue
    a, b = pc_st['spawns'], sn_st['spawns']
    sm = difflib.SequenceMatcher(a=[key(r) for r in a], b=[key(r) for r in b], autojunk=False)
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag != 'equal': rowstats['rows unmatched'] += (i2 - i1); continue
        for k in range(i2 - i1): cls_pairs[(script, a[i1 + k]['cls'], b[j1 + k]['cls'])] += 1; rowstats['rows matched'] += 1
for sc in (0x1C1, 0x1C2, 0x1C3, 0x1C4, 0x1C5, 0x1C6):
    for c in (0, 1, 2): cls_pairs[(sc, c, c)] += 1
print('rows:', dict(rowstats), 'class pairs (+vikings):', len(cls_pairs))
# the anim layer: ANIM_SCHEME from anim_disasm; a walker over arbitrary bytes
ad = load('ad', 'tools/data/anim_disasm.py')
print('anim cmd 2 scheme:', ad.ANIM_SCHEME.get(2), '| cmd 0/1:', ad.ANIM_SCHEME.get(0), ad.ANIM_SCHEME.get(1))
def walk_anim_bytes(d, entries, limit=4000):
    seen = {}; q = list(entries)
    while q and len(seen) < limit:
        pc = q.pop()
        if pc in seen or not (0 <= pc < len(d)): continue
        cmd = d[pc]; sch = ad.ANIM_SCHEME.get(cmd)
        if sch is None: continue
        kind = sch[0]
        if kind == 'fixed': seen[pc] = (cmd, 1 + sch[1]); q.append(pc + 1 + sch[1])
        elif kind in ('jump', 'loopstart'):
            seen[pc] = (cmd, 3)
            if pc + 3 <= len(d):
                tg = struct.unpack_from('<H', d, pc + 1)[0]
                q.append(tg)
            if kind == 'loopstart': q.append(pc + 3)
        elif kind == 'loopback': seen[pc] = (cmd, 1)
        elif kind == 'stop': seen[pc] = (cmd, 1)
        else: seen[pc] = (cmd, 1)   # var: length unknown -> the strand ends here
    return seen
def anim_seq(d, entry, off=0):
    # the anim entry is a 16-bit code address: PC = chunk offset; SNES = $8000-based -> bank offset
    seen = walk_anim_bytes(d, [entry - off])
    return [(seen[pc][0], bytes(d[pc + 1:pc + seen[pc][1]])) for pc in sorted(seen)]


def walk_snes(entries, BANK4, limit=20000):
    seen = {}; todo = list(entries); notes = []
    while todo:
        pc = todo.pop()
        while pc not in seen and len(seen) < limit:
            if not (0 <= pc < len(BANK4)): notes.append(f'{pc:X} outside'); break
            op = BANK4[pc]
            if op in dz.CUSTOM:                     # template-referencing ops (op 14 & co): the PC walker
                try: ln, tmpls = dz.CUSTOM[op](BANK4, pc)   # seeds the referenced records' code P and P+3
                except Exception as e: notes.append(f'{pc:X}: op {op:02X} {e!r}'); break
                for t in tmpls:
                    npc = struct.unpack_from('<H', SNES, 0x10000 + t * 0x15 + 3)[0] - 0x8000
                    if 0 <= npc < len(BANK4): todo.append(npc); todo.append(npc + 3)
                seen[pc] = (op, ln, 'fall', None); pc += ln; continue
            try: ln, kind, tgt = dc.decode_info(BANK4, pc, op, TABLE)
            except Exception as e: notes.append(f'{pc:X}: op {op:02X} {e!r}'); break
            if ln is None: notes.append(f'{pc:X}: op {op:02X} {kind}'); break
            toff = None
            if tgt is not None:
                toff = tgt - 0x8000
                if not (0 <= toff < len(BANK4)): toff = None
            seen[pc] = (op, ln, kind, toff)
            if kind in ('br', 'srch') and toff is not None: todo.append(toff)
            if kind == 'jmp':
                if toff is None: break
                pc = toff; continue
            if kind == 'stop': break
            pc += ln
    return seen, notes
def snes_ops(c, bank):
    o = 0x10000 + c * 0x15; code = struct.unpack_from('<H', SNES, o + 3)[0]
    if not (0x8000 <= code <= 0xFFFF): return None
    e = code - 0x8000; BANK4 = BANKS[bank]
    seen, notes = walk_snes([e, e + 3], BANK4)
    return [(seen[pc][0], bytes(BANK4[pc + 1:pc + seen[pc][1]])) for pc in sorted(seen)]
def snes_ops_best(sc, pcc, snc, p):
    b = CLASS_BANK.get(f'{sc:X}:{pcc:02X}')
    cands = [b] if b else list(range(2, 12))   # unknown: every LoROM bank the class table could point into
    best = None
    for bb in cands:
        s = snes_ops(snc, bb)
        if not s: continue
        eq = sum(i2 - i1 for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(a=[op for op, _ in p], b=[op for op, _ in s], autojunk=False).get_opcodes() if tag == 'equal')
        if best is None or eq > best[0]: best = (eq, s, bb)
    if best: best_bank[(sc, pcc, snc)] = best[2]
    return best[1] if best else None
best_bank = {}
def pc_ops(cid, c):
    try: d, entries, seen, stops, parent, addrs, anim_e = dz.walk(cid, {c}, TABLE)
    except Exception: return None
    return [(seen[pc][0], bytes(d[pc + 1:pc + seen[pc][1]])) for pc in sorted(seen)]

votes = collections.defaultdict(collections.Counter); st = collections.Counter(); unal = []; anim_pairs = set(); bank_of = {}; anim_cands = []
cache_p, cache_s = {}, {}
for (sc, pcc, snc), n in cls_pairs.items():
    if (sc, pcc) not in cache_p: cache_p[(sc, pcc)] = pc_ops(sc, pcc)
    p = cache_p[(sc, pcc)]
    if (sc, pcc, snc) not in cache_s: cache_s[(sc, pcc, snc)] = snes_ops_best(sc, pcc, snc, p) if p else None
    s = cache_s[(sc, pcc, snc)]
    if (sc, pcc, snc) not in bank_of: bank_of[(sc, pcc, snc)] = best_bank.get((sc, pcc, snc), 3)
    if not p or not s: st['class walk fail'] += 1; continue
    if not any(op == 2 for op, _ in p): continue
    sm = difflib.SequenceMatcher(a=[op for op, _ in p], b=[op for op, _ in s], autojunk=False)
    got = 0
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag != 'equal': continue
        for k in range(i2 - i1):
            if p[i1 + k][0] == 2 and len(p[i1 + k][1]) >= 2 and len(s[j1 + k][1]) >= 2:
                pid, pv = p[i1 + k][1][0], p[i1 + k][1][1]; sid, sv = s[j1 + k][1][0], s[j1 + k][1][1]
                votes[pid][(sid, pv == sv)] += 1; got += 1
    st['classes with op2 aligned' if got else 'classes with op2 UNaligned'] += 1
    # the animations: op-19 (set-anim) operands of the two codes in address order —
    # equal counts pair k-th with k-th (an opcode-run alignment may slip by one site)
    P19 = sorted({struct.unpack_from('<H', ob, 0)[0] for op, ob in p if op == 0x19 and len(ob) >= 2})
    S19 = sorted({struct.unpack_from('<H', ob, 0)[0] for op, ob in s if op == 0x19 and len(ob) >= 2})
    if P19 and S19: anim_cands.append((sc, pcc, snc, bank_of[(sc, pcc, snc)], P19, S19))
    if not got:
        eq = sum(i2 - i1 for tag, i1, i2, j1, j2 in sm.get_opcodes() if tag == 'equal')
        unal.append((sc, pcc, snc, len(p), len(s), eq, [x[1][:2].hex() for x in p if x[0] == 2][:4], [x[1][:2].hex() for x in s if x[0] == 2][:4]))
print(dict(st), 'anim entry pairs:', len(anim_pairs))
pc_chunks = {}
for cid in (0x1C1, 0x1C2, 0x1C3, 0x1C4, 0x1C5, 0x1C6):
    d, entries, seen, stops, parent, addrs, anim_e = dz.walk(cid, set(), TABLE, rec_scan=True); pc_chunks[cid] = d
import glob
dynlens, dynpcs = ad.load_dyn(glob.glob('/tmp/animdump/*.txt'))
print('anim corpus:', {hex(k): len(v) for k, v in dynlens.items()})
# Lockstep walk: the PC and SNES anim scripts of a paired entry are the same command
# stream (the SNES port kept the animations); the PC corpus supplies the consumed
# length of every VAR command (cmd 01/08/0A/0C/13 depend on the sub-sprite window)
# and the SNES command at the same position takes the same length. Diverging command
# bytes end the strand.
def lockstep(D, B, lens, pa, sp0, limit=5000):
    """Walk the PC anim script at pa and the SNES one at sp0 together; -> (matched
    commands, [(pc_id, pc_vol, sn_id, sn_vol)] of the command-2 sites met)."""
    q = [(pa, sp0)]; visited = set(); pairs = []; matched = 0; steps = 0
    while q and steps < limit:
        pc, sp = q.pop()
        if (pc, sp) in visited or not (0 <= pc < len(D)) or not (0 <= sp < len(B)): continue
        visited.add((pc, sp)); steps += 1
        cmd = D[pc]
        if B[sp] != cmd: continue
        sch = ad.ANIM_SCHEME.get(cmd)
        if sch is None: continue
        matched += 1; kind = sch[0]
        if cmd == 2 and pc + 2 < len(D) and sp + 2 < len(B): pairs.append((D[pc + 1], D[pc + 2], B[sp + 1], B[sp + 2]))
        if kind == 'fixed':
            if cmd != 0x1A: q.append((pc + 1 + sch[1], sp + 1 + sch[1]))
        elif kind in ('jump', 'loopstart'):
            if pc + 3 <= len(D) and sp + 3 <= len(B):
                q.append((struct.unpack_from('<H', D, pc + 1)[0], struct.unpack_from('<H', B, sp + 1)[0] - 0x8000))
            if kind == 'loopstart': q.append((pc + 3, sp + 3))
        elif kind == 'var':
            nxts = [nxt for nxt in lens.get(pc, ()) if 0 < nxt - pc < 64]
            if not nxts:
                ks = range(2, 41, 2) if cmd in (0x08, 0x0A) else range(0, 25)
                for k in ks:
                    a, b2 = pc + 1 + k, sp + 1 + k
                    if a >= len(D) or b2 >= len(B) or D[a] != B[b2] or D[a] > 0x1A: continue
                    sch2 = ad.ANIM_SCHEME.get(D[a])
                    if sch2 and sch2[0] in ('fixed', 'stop'):
                        a2, b3 = a + 1 + sch2[1], b2 + 1 + sch2[1]
                        if a2 < len(D) and b3 < len(B) and (D[a2] != B[b3] or D[a2] > 0x1A): continue
                    nxts.append(a)
            for nxt in nxts: q.append((nxt, sp + (nxt - pc)))
        elif kind == 'stop':
            if cmd != 0x1A: q.append((pc + 1 + sch[1], sp + 1 + sch[1]))
    return matched, pairs
# pair the set-anim targets of the two classes by content: the PC anim and the SNES anim
# that walk together the longest (at least 6 matched commands, a unique best on both sides)
avotes = collections.defaultdict(collections.Counter); ast = collections.Counter(); rejected = collections.defaultdict(collections.Counter)
for (sc, pcc, snc, bank, P19, S19) in anim_cands:
    D = pc_chunks[sc]; B = BANKS[bank]; lens = dynlens.get(sc, {})
    scores = {}
    for pa in P19:
        for sa in S19:
            sp0 = sa - 0x8000
            if not (0 <= pa < len(D) and 0 <= sp0 < len(B)) or D[pa] != B[sp0]: continue
            m, _ = lockstep(D, B, lens, pa, sp0, limit=400)
            if m >= 6: scores[(pa, sa)] = m
    best_p = {}; best_s = {}
    for (pa, sa), m in scores.items():
        if pa not in best_p or m > best_p[pa][1]: best_p[pa] = (sa, m)
        if sa not in best_s or m > best_s[sa][1]: best_s[sa] = (pa, m)
    for pa, (sa, m) in best_p.items():
        if best_s.get(sa, (None,))[0] == pa: anim_pairs.add((sc, pa, snc, sa, bank))
    ast['classes with anim pairs' if any(best_s.get(sa, (None,))[0] == pa for pa, (sa, m) in best_p.items()) else 'classes without anim pairs'] += 1
for (sc, pa, snc, sa, bank) in anim_pairs:
    D = pc_chunks[sc]; B = BANKS[bank]; lens = dynlens.get(sc, {})
    m, pairs = lockstep(D, B, lens, pa, sa - 0x8000)
    got = 0
    for (pid, pv, sid, sv) in pairs:
        if sid >= 0x80 and pv == sv:          # a real driver id, the volume kept: a vote
            avotes[pid][(sid, True)] += 1; got += 1
        else:
            ast['anim cmd2 rejected (id < 80 or vol differs)'] += 1
            if sid >= 0x80: rejected[pid][(sid, pv, sv)] += 1
    ast['anim entries with cmd2 pairs' if got else 'anim entries without'] += 1
print('anim:', dict(ast))
for k in sorted(rejected):
    if k not in votes and k not in avotes:
        print(f'  ANIM-REJECTED PC {k:02X} (unmapped): ' + ', '.join(f'{sid:02X} pv{pv:02X}/sv{sv:02X} x{n}' for (sid, pv, sv), n in rejected[k].most_common(5)))
for k in sorted(avotes):
    print(f'  ANIM PC {k:02X} -> ' + ', '.join(f'{sid:02X}{"" if ok else "?vol"}x{n}' for (sid, ok), n in avotes[k].most_common()))
for k, v in avotes.items():
    for kk, n in v.items(): votes[k][kk] += n
for u in unal[:12]: print('  UNAL', hex(u[0]), f'pc {u[1]:02X} sn {u[2]:02X}', 'ops', u[3], u[4], 'eq', u[5], 'pc op2', u[6], 'sn op2', u[7])
conf = 0
for k in sorted(votes):
    items = votes[k].most_common(); ids = {sid for (sid, _), _ in items}
    if len(ids) > 1: conf += 1
    print(f'  PC {k:02X} -> ' + ', '.join(f'{sid:02X}{"" if ok else "?vol"}x{n}' for (sid, ok), n in items))
print('pc ids mapped:', len(votes), 'with conflicts:', conf)
seen_pc = [0x13,0x1B,0x1D,0x1E,0x21,0x26,0x2F,0x30,0x3B,0x3F,0x40,0x44,0x50,0x52]
print('trace ids covered:', [f'{k:02X}' for k in seen_pc if k in votes], 'missing:', [f'{k:02X}' for k in seen_pc if k not in votes])
import json; json.dump({f'{k:02X}': {f'{sid:02X}': n for (sid, ok), n in v.items()} for k, v in votes.items()}, open('/tmp/lv_sfx_votes.json', 'w'), indent=1)

final = {}; conflicts = []; vote_out = {}
for k in sorted(votes):
    tally = collections.Counter()
    for (sid, ok), n in votes[k].items(): tally[sid] += n
    best, nbest = tally.most_common(1)[0]
    if best < 0x80 or (len(tally) > 1 and tally.most_common(2)[1][1] * 2 > nbest):
        conflicts.append({"pc": f"{k:02X}", "tally": {f"{s:02X}": n for s, n in tally.most_common()}})
    if best >= 0x80: final[f"{k:02X}"] = f"{best:02X}"
    vote_out[f"{k:02X}"] = {f"{s:02X}": n for s, n in tally.most_common()}
import os
out = os.path.join('tools', 'assets', 'snes_sfx_map.json')
json.dump({"map": final, "votes": vote_out, "conflicts": conflicts}, open(out, 'w'), indent=1)
print(f"{out}: {len(final)} ids mapped, {len(conflicts)} conflicts")
