#!/usr/bin/env python3
"""Stage 2: whole-chunk .lvs emission + compile-back, byte-identity diff.

emit:    lvs_full.py emit CHUNK_HEX  > file.lvs
compile: lvs_full.py compile file.lvs ORIG_CHUNK_HEX   (diff vs original)
roundtrip: lvs_full.py roundtrip CHUNK_HEX             (both in one go)

.lvs full-chunk layout (v0, anchor-based):
  chunk 01C1 size 48972
  record 00 sprite=FFFE flags=01 code=3850 w5=0003 rest=<hex>
  ...
  code @13DB:            # one block per basic-block leader
    goto.03 L_5C87
  blob @0562 <hex>       # undecoded bytes (data, unreached code)
Anchors (@addr) pin every element to its original offset — layout
freedom comes later; identity comes first.
"""
import sys, struct, glob, importlib.util

spec = importlib.util.spec_from_file_location('dz', 'tools/data/disasm.py')
dz = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dz)
spec2 = importlib.util.spec_from_file_location('dc', 'tools/data/decompile.py')
dc = importlib.util.module_from_spec(spec2)
spec2.loader.exec_module(dc)

def full_walk(cid, with_entries=False):
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
    if with_entries:
        return d, seen, table, entries
    return d, seen, table

def emit(cid):
    d, seen, table = full_walk(cid)
    out = [f'chunk {cid:04X} size {len(d)}']
    # record table: emit until first decoded instruction offset
    first_code = min(seen) if seen else len(d)
    nrec = first_code // dz.REC
    for t in range(nrec):
        o = t * dz.REC
        spr, fl = struct.unpack_from('<HB', d, o)
        code = struct.unpack_from('<H', d, o + 3)[0]
        rest = d[o+5:o+dz.REC].hex()
        out.append(f'record {t:02X} sprite={spr:04X} flags={fl:02X} '
                   f'code={code:04X} rest={rest}')
    rec_end = nrec * dz.REC
    covered = set()
    for pc in seen:
        for i in range(pc, pc + seen[pc][1]):
            covered.add(i)
    # code lines with anchors
    lines = []
    for pc in sorted(seen):
        op = seen[pc][0]
        ln, kind, tgt = dc.decode_info(d, pc, op, table)
        ob_end = pc + ln - (2 if (kind in ('br', 'jmp', 'srch') and tgt is not None) else 0)
        raw = d[pc+1:ob_end].hex()
        mn = dz.mnemonic(op, table)
        if kind in ('br', 'jmp', 'srch') and tgt is not None:
            lines.append(f'op @{pc:04X} {op:02X} {raw} T{tgt:04X}')
        else:
            lines.append(f'op @{pc:04X} {op:02X} {raw}')
    out.extend(lines)
    # blobs: uncovered spans (record table excluded)
    i = rec_end
    while i < len(d):
        if i in covered:
            i += 1
            continue
        j = i
        while j < len(d) and j not in covered:
            j += 1
        out.append(f'blob @{i:04X} {d[i:j].hex()}')
        i = j
    return '\n'.join(out)

ANIM_MN = {
    0x00: 'frame+', 0x01: 'frame_set(masked)', 0x02: 'sfx', 0x03: 'jump',
    0x04: 'skip1', 0x05: 'loop_start', 0x06: 'loop_back', 0x07: 'dx',
    0x08: 'x_abs[]', 0x09: 'dy', 0x0A: 'y_abs[]', 0x0B: 'int3',
    0x0C: 'layer_bits', 0x0D: 'mask', 0x0E: 'end_frame', 0x0F: 'delay_exit',
    0x10: 'xor200', 0x11: 'xor400', 0x12: 'xor600', 0x13: 'submask_set',
    0x14: 'sprite_load', 0x15: 'sprite_setup', 0x16: 'skip1b', 0x17: 'res_lookup',
    0x18: 'f339F', 0x19: 'f33DE', 0x1A: 'end_anim',
}

def anim_layer(cid):
    """Anim-VM instructions with resolved lengths.
    Returns {pc: (cmd, length, kind, tgt)}; unresolved VARs are absent."""
    import importlib.util as iu
    sp = iu.spec_from_file_location('ad', 'tools/data/anim_disasm.py')
    ad = iu.module_from_spec(sp)
    sp.loader.exec_module(ad)
    import glob as g
    dynlens, dynpcs = ad.load_dyn(g.glob('/tmp/animdump/*.txt'))
    table = dz.load_draft()
    pt = dz.spawn_entries()
    _, _, _, _, _, _, anim_entries = dz.walk(cid, pt.get(cid, set()), table)
    entries = set(anim_entries) | dynpcs.get(cid, set())
    d, seen, stops = ad.walk_anim(cid, entries, dynlens.get(cid, {}))
    out = {}
    lens = dynlens.get(cid, {})
    for pc, cmd in seen.items():
        sch = ad.ANIM_SCHEME.get(cmd)
        if sch is None:
            continue
        kind = sch[0]
        if kind == 'fixed':
            out[pc] = (cmd, 1 + sch[1], 'fall', None)
        elif kind in ('jump', 'loopstart'):
            tgt = struct.unpack_from('<H', d, pc + 1)[0] if pc + 3 <= len(d) else None
            out[pc] = (cmd, 3, kind, tgt)
        elif kind == 'loopback':
            out[pc] = (cmd, 1, 'loopback', None)
        elif kind == 'stop':
            out[pc] = (cmd, 1 + sch[1], 'stop', None)
        elif kind == 'var':
            nxts = lens.get(pc)
            if nxts and len(nxts) == 1:
                ln = next(iter(nxts)) - pc
                if 0 < ln <= 64:
                    out[pc] = (cmd, ln, 'fall', None)
            # ambiguous / unmeasured VAR lengths stay as blob bytes
    return out

def emit_structured(cid):
    import importlib.util as iu
    sp = iu.spec_from_file_location('ls', 'tools/data/lvs_struct.py')
    ls = iu.module_from_spec(sp)
    sp.loader.exec_module(ls)
    sp2 = iu.spec_from_file_location('ex', 'tools/data/expr.py')
    ex = iu.module_from_spec(sp2)
    sp2.loader.exec_module(ex)
    d, seen, table = full_walk(cid)
    n, states, cov = ls.lift_states(cid)
    LAY = dz.load_layout_names()
    out = [f'chunk {cid:04X} size {len(d)}']
    first_code = min(seen) if seen else len(d)
    nrec = first_code // dz.REC
    for t in range(nrec):
        o = t * dz.REC
        spr, fl = struct.unpack_from('<HB', d, o)
        code = struct.unpack_from('<H', d, o + 3)[0]
        rest = d[o+5:o+dz.REC].hex()
        out.append(f'record {t:02X} sprite={spr:04X} flags={fl:02X} '
                   f'code={code:04X} rest={rest}')
    rec_end = nrec * dz.REC
    covered = set()
    for pc in seen:
        for i in range(pc, pc + seen[pc][1]):
            covered.add(i)
    in_state = set()
    for h, (body, guards, end) in states.items():
        for pc in body:
            in_state.add(pc)
    def op_line(pc, indent='  '):
        op = seen[pc][0]
        ln, kind, tgt = dc.decode_info(d, pc, op, table)
        ob_end = pc + ln - (2 if (kind in ('br', 'jmp', 'srch') and tgt is not None) else 0)
        body = d[pc+1:ob_end]
        raw = body.hex()
        e = ex.render(op, body, LAY)
        mn = e if e is not None else dz.mnemonic(op, table)
        if kind in ('br', 'srch') and tgt is not None:
            pre = mn if e is not None else f'when {mn}'
            return f'{indent}op @{pc:04X} {op:02X} {raw} T{tgt:04X}  ; {pre} -> S_{tgt:04X}'
        if kind == 'jmp' and tgt is not None:
            return f'{indent}op @{pc:04X} {op:02X} {raw} T{tgt:04X}  ; -> S_{tgt:04X}'
        return f'{indent}op @{pc:04X} {op:02X} {raw}  ; {mn}'
    # entry annotations: record P -> anim redirect, P+3 -> spawn entry.
    # Scan the whole potential record table (same filters as the walker),
    # not just the nrec prefix in front of the first decoded instruction.
    entry_of = {}
    t = 0
    while (t + 1) * dz.REC <= len(d):
        pcode = struct.unpack_from('<H', d, t * dz.REC + 3)[0]
        if 0x600 <= pcode < len(d):
            entry_of.setdefault(pcode, []).append(f't{t:02X}.anim')
            entry_of.setdefault(pcode + 3, []).append(f't{t:02X}.spawn')
        elif t > 0 and pcode == 0:
            break
        t += 1
        if t > 0x400:
            break
    for h in sorted(states):
        body, guards, end = states[h]
        ek = end[0] if end else 'runoff'
        ent = ('  ; entry ' + ','.join(entry_of[h])) if h in entry_of else ''
        out.append(f'state S_{h:04X} {{  ; end={ek}{ent}')
        for pc in body:
            out.append(op_line(pc))
        out.append('}')
    # stray decoded ops outside any state body
    for pc in sorted(seen):
        if pc not in in_state:
            out.append(op_line(pc, indent=''))
    # anim-VM layer: instructions whose bytes are not already claimed by
    # object code (overlap impossible in practice; guard anyway)
    def an_expr(cmd, body, tgt):
        if cmd == 0x00 and body:
            return f'frame += {body[0]}'
        if cmd == 0x02 and len(body) == 2:
            return f'sfx {struct.unpack_from("<H", body)[0] & 0xFF}'
        if cmd == 0x03:
            return f'-> A_{tgt:04X}'
        if cmd == 0x05:
            return f'loop {{ -> A_{tgt:04X}'
        if cmd == 0x06:
            return '} loop_back'
        if cmd in (0x04, 0x16) and body:
            return f'skip({body[0]:02X})'
        if cmd == 0x07 and body:
            v = body[0] - 256 if body[0] >= 128 else body[0]
            return f'dx {v:+d}'
        if cmd == 0x09 and body:
            v = body[0] - 256 if body[0] >= 128 else body[0]
            return f'dy {v:+d}'
        if cmd in (0x08, 0x0A) and len(body) % 2 == 0 and body:
            vals = struct.unpack_from(f'<{len(body)//2}h', body)
            ax = 'x' if cmd == 0x08 else 'y'
            return f'{ax}_abs {list(vals)}'
        if cmd == 0x0D and body:
            return f'mask = 0x{body[0]:X}'
        if cmd == 0x0F and body:
            return f'delay {body[0]}; end_frame'
        if cmd == 0x14 and body:
            return f'sprite {body[0]}'
        if cmd == 0x15 and body:
            return f'subtype {body[0]}'
        if cmd == 0x17 and len(body) == 2:
            return f'res_lookup {struct.unpack_from("<H", body)[0]:04X}'
        if cmd == 0x01 and body:
            return f'frame_set(masked) [{",".join(str(b) for b in body)}]'
        if cmd == 0x13 and body:
            return f'submask_set [{",".join(f"0x{b:X}" for b in body)}]'
        if cmd == 0x0C and body:
            return f'layer_bits [{",".join(str(b) for b in body)}]'
        return ANIM_MN.get(cmd, f'a{cmd:02X}')
    anims = anim_layer(cid)
    an_lines = []
    for pc in sorted(anims):
        cmd, ln, kind, tgt = anims[pc]
        if any((pc + i) in covered for i in range(ln)):
            continue
        for i in range(pc, pc + ln):
            covered.add(i)
        body = d[pc+1:pc+ln]
        an_lines.append(f'an @{pc:04X} {cmd:02X} {body.hex()}  ; {an_expr(cmd, body, tgt)}')
    out.extend(an_lines)
    i = rec_end
    while i < len(d):
        if i in covered:
            i += 1
            continue
        j = i
        while j < len(d) and j not in covered:
            j += 1
        out.append(f'blob @{i:04X} {d[i:j].hex()}')
        i = j
    return '\n'.join(out)

def compile_lvs(text):
    img = None
    size = 0
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        if line.startswith(';'):
            continue
        if ';' in line:
            line = line.split(';', 1)[0].strip()
        if line.startswith('state ') or line == '}' or not line:
            continue
        p = line.split()
        if p[0] == 'chunk':
            size = int(p[3])
            img = bytearray(size)
        elif p[0] == 'record':
            t = int(p[1], 16)
            o = t * dz.REC
            spr = int(p[2].split('=')[1], 16)
            fl = int(p[3].split('=')[1], 16)
            code = int(p[4].split('=')[1], 16)
            rest = bytes.fromhex(p[5].split('=')[1])
            struct.pack_into('<HB', img, o, spr, fl)
            struct.pack_into('<H', img, o + 3, code)
            img[o+5:o+5+len(rest)] = rest
        elif p[0] == 'op' or p[0] == 'an':
            pc = int(p[1][1:], 16)
            op = int(p[2], 16)
            body = b''
            tgt = None
            for tok in p[3:]:
                if tok.startswith('T'):
                    tgt = int(tok[1:], 16)
                else:
                    body = bytes.fromhex(tok)
            enc = bytes([op]) + body
            if tgt is not None:
                enc += struct.pack('<H', tgt)
            img[pc:pc+len(enc)] = enc
        elif p[0] == 'blob':
            o = int(p[1][1:], 16)
            b = bytes.fromhex(p[2]) if len(p) > 2 else b''
            img[o:o+len(b)] = b
    return bytes(img)

def main():
    mode = sys.argv[1]
    if mode == 'emit':
        print(emit(int(sys.argv[2], 16)))
    elif mode == 'emit2':
        print(emit_structured(int(sys.argv[2], 16)))
    elif mode == 'roundtrip2':
        cid = int(sys.argv[2], 16)
        text = emit_structured(cid)
        img = compile_lvs(text)
        orig = open(f'assets_raw/chunks/dec/{cid:04d}.bin', 'rb').read()
        ok = img == orig
        ndiff = sum(1 for a, b in zip(img, orig) if a != b)
        print(f'0x{cid:X}: structured_roundtrip={ok} diff_bytes={ndiff} '
              f'lines={len(text.splitlines())}')
        return 0 if ok else 1
    elif mode == 'roundtrip':
        cid = int(sys.argv[2], 16)
        text = emit(cid)
        img = compile_lvs(text)
        orig = open(f'assets_raw/chunks/dec/{cid:04d}.bin', 'rb').read()
        ok = img == orig
        ndiff = sum(1 for a, b in zip(img, orig) if a != b)
        print(f'0x{cid:X}: roundtrip_identical={ok} diff_bytes={ndiff} '
              f'lvs_lines={len(text.splitlines())}')
        return 0 if ok else 1
    return 0

if __name__ == '__main__':
    sys.exit(main())
