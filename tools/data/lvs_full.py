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
    # group states by owning template (single-owner), then by the exact
    # owner set for shared code, then dyn-only strands.
    own = ls.owners_of_states(cid)
    groups = {}
    for h in states:
        os_ = frozenset(own.get(h, set()))
        if len(os_) == 1:
            key = ('t', min(os_), None)
        elif os_:
            key = ('shared', min(os_), os_)
        else:
            key = ('orphan', 0, None)
        groups.setdefault(key, []).append(h)
    def emit_state(h):
        body, guards, end = states[h]
        ek = end[0] if end else 'runoff'
        ent = ('  ; entry ' + ','.join(entry_of[h])) if h in entry_of else ''
        out.append(f'state S_{h:04X} {{  ; end={ek}{ent}')
        for pc in body:
            out.append(op_line(pc))
        out.append('}')
    order = {'t': 0, 'shared': 1, 'orphan': 2}
    for key in sorted(groups, key=lambda k: (order[k[0]], k[1])):
        heads = sorted(groups[key])
        if key[0] == 't':
            t = key[1]
            o = t * dz.REC
            spr = struct.unpack_from('<H', d, o)[0]
            sub = d[o + 2] & 0x7F
            out.append(f'; ==== template t{t:02X} (sprite={spr:04X} '
                       f'subsprites={sub}) — {len(heads)} states ====')
        elif key[0] == 'shared':
            names = ','.join(f't{t:02X}' for t in sorted(key[2]))
            if len(names) > 60:
                names = names[:57] + '...'
            out.append(f'; ==== shared by {names} — {len(heads)} states ====')
        else:
            out.append(f'; ==== dyn-only / unowned states — {len(heads)} ====')
        for h in heads:
            emit_state(h)
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
    aown = ls.anim_owners(cid, anims)
    agroups = {}
    for pc in sorted(anims):
        cmd, ln, kind, tgt = anims[pc]
        if any((pc + i) in covered for i in range(ln)):
            continue
        for i in range(pc, pc + ln):
            covered.add(i)
        os_ = frozenset(aown.get(pc, set()))
        if len(os_) == 1:
            key = ('t', min(os_), None)
        elif os_:
            key = ('shared', min(os_), os_)
        else:
            key = ('orphan', 0, None)
        body = d[pc+1:pc+ln]
        agroups.setdefault(key, []).append(
            (pc, f'an @{pc:04X} {cmd:02X} {body.hex()}  ; {an_expr(cmd, body, tgt)}'))
    aorder = {'t': 0, 'shared': 1, 'orphan': 2}
    for key in sorted(agroups, key=lambda k: (aorder[k[0]], k[1])):
        rows = sorted(agroups[key])
        if key[0] == 't':
            out.append(f'; ==== anim of t{key[1]:02X} — {len(rows)} cmds ====')
        elif key[0] == 'shared':
            names = ','.join(f't{t:02X}' for t in sorted(key[2]))
            if len(names) > 60:
                names = names[:57] + '...'
            out.append(f'; ==== anim shared by {names} — {len(rows)} cmds ====')
        else:
            out.append(f'; ==== anim dyn-only/unowned — {len(rows)} cmds ====')
        out.extend(r for _, r in rows)
    # data annotations: op 13 sub D9 points at a 48-byte palette block
    pal_at = {}
    for pc in seen:
        if seen[pc][0] == 0x13 and pc + 4 <= len(d) and d[pc+1] == 0xD9:
            ptr = struct.unpack_from('<H', d, pc + 2)[0]
            if ptr + 48 <= len(d):
                pal_at[ptr] = pc
    i = rec_end
    while i < len(d):
        if i in covered:
            i += 1
            continue
        j = i
        while j < len(d) and j not in covered:
            j += 1
        ann = ''
        pals = [a for a in pal_at if i <= a and a + 48 <= j]
        if pals:
            ann = '  ; ' + ', '.join(
                f'palette48 @{a:04X} (op13@{pal_at[a]:04X})' for a in sorted(pals))
        out.append(f'blob @{i:04X} {d[i:j].hex()}{ann}')
        i = j
    return '\n'.join(out)

def emit_free(cid):
    """v2 free-form: NO @ anchors on code. Every byte of the chunk is
    emitted in address order (records, op/an lines, blobs); branch and
    jump targets are symbolic (S_xxxx / A_xxxx); op 19 anim operands
    are symbolic too. compile_free lays elements out sequentially and
    resolves labels in a second pass — an unedited emit_free text must
    rebuild the chunk byte-identically."""
    text = emit_structured(cid)
    d, seen, table = full_walk(cid)
    # collect label sets: object targets (S_) and anim targets (A_)
    obj_lbl = set()
    an_lbl = set()
    items = []   # (addr, kind, payload)
    for line in text.splitlines():
        raw = line.split(';', 1)[0].strip()
        if not raw:
            continue
        p = raw.split()
        if p[0] == 'chunk':
            items.append((0, 'chunk', raw))
        elif p[0] == 'record':
            items.append((int(p[1], 16) * dz.REC, 'record', raw))
        elif p[0] == 'op':
            pc = int(p[1][1:], 16)
            op = int(p[2], 16)
            body = b''
            tgt = None
            for tok in p[3:]:
                if tok.startswith('T'):
                    tgt = int(tok[1:], 16)
                else:
                    body = bytes.fromhex(tok)
            if tgt is not None:
                obj_lbl.add(tgt)
            if op == 0x19:
                an_lbl.add(struct.unpack_from('<H', body, 0)[0])
            items.append((pc, 'op', (pc, op, body, tgt)))
        elif p[0] == 'an':
            pc = int(p[1][1:], 16)
            cmd = int(p[2], 16)
            body = bytes.fromhex(p[3]) if len(p) > 3 else b''
            if cmd in (0x03, 0x05) and len(body) == 2:
                an_lbl.add(struct.unpack_from('<H', body, 0)[0])
            items.append((pc, 'an', (pc, cmd, body)))
        elif p[0] == 'blob':
            items.append((int(p[1][1:], 16), 'blob', raw))
    an_nodes = {addr for addr, kind, _ in items if kind == 'an'}
    # record code= targets are labels too
    recs = [it for it in items if it[1] == 'record']
    for _, _, raw in recs:
        code = int(raw.split()[4].split('=')[1], 16)
        if code in seen:
            obj_lbl.add(code)
    items.sort(key=lambda x: x[0])
    # overlap resolution: keep the first frame per byte; overlapped
    # secondary frames are not emitted (their bytes are already owned) —
    # referenced ones become alias labels into the owning line.
    owner = {}       # byte addr -> (owner_addr, owner_kind)
    kept = []
    aliases = []     # (label_prefix, addr, owner_addr)
    def span_of(addr, kind, payload):
        if kind == 'op':
            pc, op, body, tgt = payload
            return 1 + len(body) + (2 if tgt is not None else 0)
        if kind == 'an':
            pc, cmd, body = payload
            return 1 + len(body)
        return 0
    dropped_tail = set()   # bytes of dropped frames beyond their owner
    for addr, kind, payload in items:
        if kind in ('op', 'an'):
            ln = span_of(addr, kind, payload)
            if any((addr + i) in owner for i in range(ln)):
                lblset = obj_lbl if kind == 'op' else an_lbl
                if addr in lblset:
                    aliases.append(('S' if kind == 'op' else 'A',
                                    addr, owner[addr]))
                for i in range(ln):   # tail bytes the owner chain misses
                    if (addr + i) not in owner:
                        dropped_tail.add(addr + i)
                continue
            for i in range(ln):
                owner[addr + i] = addr
                dropped_tail.discard(addr + i)
        kept.append((addr, kind, payload))
    # re-emit still-uncovered dropped-tail bytes as anchored mini-blobs
    for a in sorted(dropped_tail):
        if a in owner:
            continue
        b = a
        while b + 1 in dropped_tail and b + 1 not in owner:
            b += 1
        span = bytes(d[a:b+1])
        kept.append((a, 'blob', f'blob @{a:04X} {span.hex()}'))
        for i in range(a, b + 1):
            owner[i] = a
    kept.sort(key=lambda x: x[0])
    items = kept
    out = []
    alias_lines = []
    pal_lbls = set()
    base_kind = {a: k for a, k, _ in items}
    for pref, addr, base in aliases:
        bpref = 'S' if base_kind.get(base) == 'op' else 'A'
        alias_lines.append(f'{pref}_{addr:04X} = {bpref}_{base:04X}+{addr-base}')
        if base_kind.get(base) == 'op':
            obj_lbl.add(base)
        else:
            an_lbl.add(base)
    for addr, kind, payload in items:
        if kind == 'chunk':
            out.append(payload)
        elif kind == 'record':
            p = payload.split()
            code = int(p[4].split('=')[1], 16)
            p[4] = f'code=S_{code:04X}' if code in seen else f'code=={code:04X}'
            out.append(' '.join(p))
        elif kind == 'blob':
            out.append(payload)
        elif kind == 'op':
            pc, op, body, tgt = payload
            if pc in obj_lbl:
                out.append(f'S_{pc:04X}:')
            toks = [f'o {op:02X}']
            if op == 0x19:
                a = struct.unpack_from('<H', body, 0)[0]
                toks.append(f'A_{a:04X}' if a in an_nodes else f'={a:04X}')
            elif op == 0x13 and len(body) == 3 and body[0] == 0xD9:
                # D9 sub-command: symbolic pointer to the 48-byte palette
                ptr = struct.unpack_from('<H', body, 1)[0]
                if ptr + 48 <= len(d):
                    toks.append('d9')
                    toks.append(f'P_{ptr:04X}')
                    pal_lbls.add(ptr)
                else:
                    toks.append(body.hex())
            elif body:
                toks.append(body.hex())
            if tgt is not None:
                toks.append(f'S_{tgt:04X}' if tgt in seen else f'={tgt:04X}')
            out.append(' '.join(toks))
        elif kind == 'an':
            pc, cmd, body = payload
            if pc in an_lbl:
                out.append(f'A_{pc:04X}:')
            toks = [f'a {cmd:02X}']
            if cmd in (0x03, 0x05) and len(body) == 2:
                a = struct.unpack_from('<H', body, 0)[0]
                toks.append(f'A_{a:04X}' if a in an_nodes else f'={a:04X}')
            elif body:
                toks.append(body.hex())
            out.append(' '.join(toks))
    pal_lines = [f'P_{a:04X} = @{a:04X}  ; 48-byte palette block'
                 for a in sorted(pal_lbls)]
    return '\n'.join(out[:1] + alias_lines + pal_lines + out[1:])

def compile_free(text):
    """Two-pass sequential assembler for emit_free output."""
    # pass 1: layout — walk lines in order, assign addresses
    lines = []
    for line in text.splitlines():
        raw = line.split(';', 1)[0].strip()
        if raw:
            lines.append(raw)
    size = 0
    labels = {}
    cursor = 0
    parsed = []   # (kind, data, length)
    for raw in lines:
        p = raw.split()
        if p[0] == 'chunk':
            size = int(p[3])
            parsed.append(('chunk', None, 0))
        elif p[0] == 'record':
            t = int(p[1], 16)
            cursor = max(cursor, (t + 1) * dz.REC)
            parsed.append(('record', p, 0))
        elif raw.endswith(':') and len(p) == 1:
            labels[p[0][:-1]] = None   # resolved when next code line lands
            parsed.append(('label', p[0][:-1], 0))
        elif len(p) == 3 and p[1] == '=' and p[2].startswith('@'):
            labels[p[0]] = int(p[2][1:], 16)   # absolute label (data anchor)
        elif len(p) == 3 and p[1] == '=' and '+' in p[2]:
            base, off = p[2].split('+')
            parsed.append(('alias', (p[0], base, int(off)), 0))
        elif p[0] == 'blob':
            addr = int(p[1][1:], 16)
            b = bytes.fromhex(p[2]) if len(p) > 2 else b''
            cursor = max(cursor, addr + len(b))
            parsed.append(('blob', (addr, b), len(b)))
        elif p[0] in ('o', 'a'):
            parsed.append((p[0], p[1:], 0))
        else:
            raise ValueError(f'free-form: unknown line {raw!r}')
    # sequential address assignment: records occupy the table; code and
    # blobs advance a cursor in file order. blobs are anchored (data),
    # so the cursor jumps to their addr; code fills the gaps in between.
    img = bytearray(size)
    cursor = 0
    pend_labels = []
    enc_items = []
    alias_defs = []
    for kind, data, _ in parsed:
        if kind == 'chunk':
            continue
        if kind == 'alias':
            alias_defs.append(data)
            continue
        if kind == 'record':
            t = int(data[1], 16)
            cursor = max(cursor, (t + 1) * dz.REC)
            enc_items.append(('record', data, None))
            continue
        if kind == 'label':
            pend_labels.append(data)
            continue
        if kind == 'blob':
            addr, b = data
            cursor = max(cursor, addr + len(b))
            enc_items.append(('bytes', addr, b))
            continue
        # code line: length = 1 + operands (symbolic/raw word = 2)
        toks = data
        opb = int(toks[0], 16)
        ln = 1
        parts = []
        for tok in toks[1:]:
            if tok.startswith(('S_', 'A_', 'P_')):
                ln += 2
                parts.append(tok)
            elif tok.startswith('='):
                ln += 2
                parts.append(struct.pack('<H', int(tok[1:], 16)))
            else:
                b = bytes.fromhex(tok)
                ln += len(b)
                parts.append(b)
        addr = cursor
        for L in pend_labels:
            labels[L] = addr
        pend_labels = []
        enc_items.append(('code', addr, (opb, parts)))
        cursor += ln
    for name, base, off in alias_defs:
        labels[name] = labels[base] + off
    # collision check: sequential code must never run into an anchored
    # element (records/blobs) — that means an edit overflowed a gap.
    taken = bytearray(size)
    for item in enc_items:
        if item[0] == 'record':
            t = int(item[1][1], 16)
            a, ln = t * dz.REC, dz.REC
        elif item[0] == 'bytes':
            a, ln = item[1], len(item[2])
        else:
            a = item[1]
            opb, parts = item[2]
            ln = 1 + sum(2 if isinstance(p, str) else len(p) for p in parts)
        for i in range(a, a + ln):
            if i >= size:
                raise ValueError(f'element @{a:04X}+{ln} beyond chunk size')
            if taken[i]:
                raise ValueError(f'layout collision at 0x{i:04X} '
                                 f'(code overflowed into an anchored element?)')
            taken[i] = 1
    # pass 2: encode
    for item in enc_items:
        if item[0] == 'record':
            p = item[1]
            t = int(p[1], 16)
            o = t * dz.REC
            spr = int(p[2].split('=')[1], 16)
            fl = int(p[3].split('=')[1], 16)
            cs = p[4].split('=', 1)[1]
            if cs.startswith('S_'):
                code = labels[cs]
            elif cs.startswith('='):
                code = int(cs[1:], 16)
            else:
                code = int(cs, 16)
            rest = bytes.fromhex(p[5].split('=')[1])
            struct.pack_into('<HB', img, o, spr, fl)
            struct.pack_into('<H', img, o + 3, code)
            img[o+5:o+5+len(rest)] = rest
        elif item[0] == 'bytes':
            _, addr, b = item
            img[addr:addr+len(b)] = b
        else:
            _, addr, (opb, parts) = item
            enc = bytes([opb])
            for prt in parts:
                if isinstance(prt, bytes):
                    enc += prt
                else:
                    enc += struct.pack('<H', labels[prt])
            img[addr:addr+len(enc)] = enc
    return bytes(img)

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
    elif mode == 'emit3':
        print(emit_free(int(sys.argv[2], 16)))
    elif mode == 'roundtrip3':
        cid = int(sys.argv[2], 16)
        text = emit_free(cid)
        img = compile_free(text)
        orig = open(f'assets_raw/chunks/dec/{cid:04d}.bin', 'rb').read()
        ok = img == orig
        ndiff = sum(1 for a, b in zip(img, orig) if a != b)
        print(f'0x{cid:X}: free_roundtrip={ok} diff_bytes={ndiff} '
              f'lines={len(text.splitlines())}')
        return 0 if ok else 1
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
