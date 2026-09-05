#!/usr/bin/env python3
"""SMD (Genesis) bytecode disassembler: the PC VM dialect (216 ops, same
operand grammar: tools/data/disasm.py) with big-endian operands (LE, movep trick @0x38D8); jump
targets are SIGNED 16-bit offsets from the class bank base (68k @0x398C:
d6 = [0x3970 + bank*4] -> 0x20000/0x28000/0x30000; record +2 low byte
= bank 2/3/4). Class table @ROM 0x18000, 20 bytes/record, code word at +4
(68k @0x2102: +3 on spawn = skip the 03 prolog)."""
import sys, struct, json, importlib.util, os
os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
sys.path.insert(0, "tools/assets"); import smd2pc
spec = importlib.util.spec_from_file_location("dz", "tools/data/disasm.py"); dz = importlib.util.module_from_spec(spec); spec.loader.exec_module(dz)
R = smd2pc.SmdRom().rom
TABLE = dz.load_draft()
# SMD setter channels (68k table @0x3920: 0..4 valid) — lengths verified from the bodies
SMD_SET_LEN = {0: 0, 1: 1, 2: 2, 3: 1, 4: 0}   # 0x3940 rts / 0x3942 1B / 0x3950 2B / 0x395E 1B / 0x396E rts
def _setpair_smd(d, q, second=True):
    m = d[q]; q += 1
    q += SMD_SET_LEN[m & 7]
    if second: q += SMD_SET_LEN[(m >> 3) & 7]
    return q
dz._setpair = _setpair_smd
BANK = {2: 0x20000, 3: 0x28000, 4: 0x30000}

def record(cls):
    o = 0x18000 + cls * 20
    w = [(R[o+i] << 8) | R[o+i+1] for i in range(0, 20, 2)]
    bank = w[1] & 0xFF
    return dict(sprite=w[0], flags=w[1] >> 8, bank=bank, code=BANK[bank] + struct.unpack(">h", R[o+4:o+6])[0], rest=w[3:])

# operands are LITTLE-endian: the 68k reader uses movep.w $1(a6),d0 + move.b (a6)+,d0 (0x38D8)
def le16(a): return R[a] | (R[a+1] << 8)
def s16(a): return struct.unpack("<h", R[a:a+2])[0]

def decode(pc, base):
    """-> (length, kind, target_abs or None)"""
    op = R[pc]
    if op in dz.CUSTOM:
        ln, _ = dz.CUSTOM[op](R, pc); return ln, 'fall', None
    if op in dz.CUSTOM_BR:
        ln, _ = dz.CUSTOM_BR[op](R, pc); return ln, 'br', base + s16(pc + ln - 2)
    if op in dz.OPERAND_SCHEMES:
        ln, kind, toff = dz.OPERAND_SCHEMES[op]
        return ln, kind, (base + s16(pc + toff)) if toff is not None else None
    t = TABLE.get(op)
    if t is None: return None, 'unknown', None
    if t[0] == 'fixed': return t[1], 'fall', None
    if t[0] == 'scheme':
        ln, kind, toff = t[1]; return ln, kind, (base + s16(pc + toff)) if toff is not None else None
    return None, 'complex', None

def walk(entries, base, limit=60000):
    seen = {}; todo = list(entries); notes = []
    while todo:
        pc = todo.pop()
        while pc not in seen and len(seen) < limit:
            if not (base - 0x8000 <= pc < base + 0x8000): notes.append(f"pc {pc:X} outside bank"); break
            try: ln, kind, tgt = decode(pc, base)
            except KeyError as e: notes.append(f'{pc:X}: op {R[pc]:02X} decode error {e}'); seen[pc] = (R[pc], 1, 'err', None); break
            if ln is None: notes.append(f"{pc:X}: op {R[pc]:02X} {kind}"); seen[pc] = (R[pc], 1, kind, None); break
            seen[pc] = (R[pc], ln, kind, tgt)
            if kind in ('br', 'srch') and tgt is not None: todo.append(tgt)
            if kind == 'jmp': pc = tgt; continue
            if kind == 'stop': break
            pc += ln
    return seen, notes

def listing(seen, base, names=True):
    out = []
    for pc in sorted(seen):
        op, ln, kind, tgt = seen[pc]
        raw = R[pc+1:pc+ln].hex(' ')
        mn = dz.mnemonic(op, TABLE) if names else ''
        t = f"  -> {tgt:X}" if tgt is not None else ''
        out.append(f"{pc:05X}: {op:02X} {raw:<24} {mn}{t}")
    return "\n".join(out)

def port_blob(regions, bank, base, edits=None, inserts=None, drops=(), op19_map=None, prolog=True):
    """Port SMD class code to a PC blob at `base`: the instructions of the
    ROM regions [(start, end)] in address order, verbatim, with
    - jump/call/branch targets relocated (every target op keeps its word as
      the last operand; targets must fall inside the regions),
    - `edits` {smd_pc_of_byte: bytes} = operand byte overrides (platform
      constants: DS addresses, level ids, sfx ids),
    - `inserts` {smd_pc: bytes} = extra bytes emitted right AFTER that
      instruction (relocations account for the shift),
    - `drops` = SMD pcs of instructions left out,
    - `op19_map` {smd_anim_addr: pc_anim_addr} for the anim-load operands
      of op 0x19 (their word is an anim address, not code),
    - a `03 base+3` prolog (sub_1424c re-enters records at the raw pointer)."""
    edits = edits or {}; inserts = inserts or {}; op19_map = op19_map or {}
    b = BANK[bank]
    seen, notes = walk([r[0] for r in regions], b)
    assert not notes, notes
    for pc in seen:
        assert any(r0 <= pc < r1 for r0, r1 in regions), hex(pc)
    order = [pc for r0, r1 in regions for pc in sorted(seen) if r0 <= pc < r1]
    covered = set()
    for pc in order:
        covered.update(range(pc, pc + seen[pc][1]))
    for r0, r1 in regions:
        gaps = [x for x in range(r0, r1) if x not in covered]
        assert not gaps, ("unreached bytes in region", hex(r0), [hex(g) for g in gaps[:8]])
    # layout pass: new address of every instruction
    new = {}; cur = base + (3 if prolog else 0)
    for pc in order:
        if pc in drops:
            continue
        new[pc] = cur
        cur += seen[pc][1] + len(inserts.get(pc, b""))
    out = bytearray()
    if prolog:
        out += bytes((0x03,)) + struct.pack("<H", base + 3)
    for pc in order:
        if pc in drops:
            continue
        op, ln, kind, tgt = seen[pc]
        ins = bytearray(R[pc:pc + ln])
        for eo, val in edits.items():
            if pc <= eo < pc + ln:
                ins[eo - pc:eo - pc + len(val)] = val
        if tgt is not None:
            assert tgt in new, ("target dropped/outside", hex(pc), hex(tgt))
            ins[ln - 2:ln] = struct.pack("<H", new[tgt])
        if op == 0x19:
            a = b + s16(pc + 1)
            assert a in op19_map, ("op 19 anim not mapped", hex(pc), hex(a))
            ins[1:3] = struct.pack("<H", op19_map[a])
        out += ins + inserts.get(pc, b"")
    return bytes(out), new


if __name__ == "__main__":
    cls = int(sys.argv[1], 16)
    r = record(cls); base = BANK[r['bank']]
    print(f"class {cls:02X}: sprite={r['sprite']:04X} flags={r['flags']:02X} bank={r['bank']} code={r['code']:X} rest={[hex(x) for x in r['rest']]}")
    seen, notes = walk([r['code'] + 3], base)   # +3: skip the '03 despawn' prolog (68k @0x2104 addq #3)
    print(listing(seen, base)); print("notes:", notes)
