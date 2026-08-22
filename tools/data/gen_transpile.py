#!/usr/bin/env python3
"""Stage 3A: transpile a template chunk's object bytecode to C++.

Emits src/sdl/gen/chunk_XXXX.gen.inc — included from v2_vm.cpp (same TU,
so the static v2_vm_op_* handlers are directly callable). The generated
function replaces ONLY the fetch-decode loop: every instruction becomes
a direct call to its verified handler with vm.pc pre-set past the opcode
byte (handlers read their own operands from the chunk stream exactly as
under the VM), followed by statically-known control flow:

  fall  -> goto next label
  br    -> if (vm.pc == TGT) goto TGT else goto FALL
  srch  -> 3-way (TGT / continuation / skip)
  jmp   -> goto TGT (05 call-jump included; ALT_PC returns re-enter the
           dispatch switch because 06 loads a dynamic pc)
  stop  -> running=false -> return (yield/exit/despawn), or 06: continue

Unknown pc (dead code the STRICT walker cannot see, future dyn) falls
back to the interpreter loop: the function returns false and the caller
resumes the classic while-loop seamlessly (vm state is always coherent).

Verify parity: the G_PRE/G_POST macros in v2_vm.cpp reproduce the exact
per-opcode side channels of the loop (si_track seed, hash-before snaps,
trace_record_v2_ext, the per-slot trace ring) so a gencode binary stays
comparable against the real side inside the standard headless canon.

Usage: gen_transpile.py CHUNK_HEX [outdir]
"""
import sys, re, struct, importlib.util, os

def load(n, p):
    sp = importlib.util.spec_from_file_location(n, p)
    m = importlib.util.module_from_spec(sp)
    sp.loader.exec_module(m)
    return m

lf = load('lf', 'tools/data/lvs_full.py')
dz = lf.dz
dc = lf.dc
ex = load('ex', 'tools/data/expr.py')

# ============================================================================
# Stage B wave 1: inline emitters for the acc family. Each template is a
# line-by-line copy of its verified handler body with the stream operands
# folded to constants (field LUT resolved via the static DS, exactly what
# the handler computed at runtime). si_track/di_track shadow-register
# effects (task #15) are reproduced verbatim; DS access stays on the
# vm.ds_read/ds_write methods so every verify ring keeps firing.
# Returns list of C++ lines or None (-> keep the handler call).
# ============================================================================
def _fcol(idx):
    """LUT16[(idx-0x6CBA)&0xFFFF] + OBJ_FIELD_BASE — the folded field column."""
    import struct as st
    lut = st.unpack_from('<H', ex.ds_static(), (idx - 0x6CBA) & 0xFFFF)[0]
    return (lut + 0x14E5) & 0xFFFF

def _self_addr(idx):
    return (f'(uint16_t)(vm.global_r(DS_CUR_OBJ) + 0x{_fcol(idx):04X})'
            f' /* self.{ex.field_name(idx)} */')

def _partner_addr(idx):
    # 58/67/5B/5E/64 pattern: di = LUT[idx]; di += [cur+OBJ_PARTNER]; addr=di+14E5
    return (f'(uint16_t)(vm.ds_read((uint16_t)(vm.global_r(DS_CUR_OBJ) + OBJ_PARTNER))'
            f' + 0x{_fcol(idx):04X}) /* partner.{ex.field_name(idx)} */')

def _imm16(body):
    import struct as st
    return st.unpack_from('<H', body, 0)[0]

def inline_wave1(op, body, nxt):
    L = []
    if op == 0x51:                       # v2_vm_op_load_acc_literal
        L.append(f'v2_vm_accumulator = 0x{_imm16(body):04X};')
    elif op == 0x52:                     # read_indexed_field: si_track = field addr
        a = _self_addr(body[0])
        L.append(f'{{ uint16_t _a = {a};')
        L.append('  vm.si_track = (uint16_t)(_a - OBJ_FIELD_BASE);  // orig ch1: slot base in SI')
        L.append('  v2_vm_accumulator = vm.ds_read(_a); }')
    elif op == 0x53:                     # read_indirect: si_track = addr
        a = _imm16(body)
        L.append(f'vm.si_track = 0x{a:04X};')
        L.append(f'v2_vm_accumulator = vm.ds_read(0x{a:04X});')
    elif op == 0x54:                     # load_acc_indexed_1995: di_track only
        L.append(f'{{ uint16_t _di = {_partner_addr(body[0])};')
        L.append('  v2_vm_accumulator = vm.ds_read(_di);')
        L.append('  vm.di_track = (uint16_t)(_di - OBJ_FIELD_BASE); }')
    elif op == 0x56:                     # self.F = acc (no tracks)
        L.append(f'vm.ds_write({_self_addr(body[0])}, v2_vm_accumulator);')
    elif op == 0x57:                     # [addr] = acc (+ trap logging, folded)
        a = _imm16(body)
        if 0x3E4 <= a <= 0x413 or a in (0x302, 0x304):
            return None                  # keep the handler: trap diagnostics
        L.append(f'vm.ds_write(0x{a:04X}, v2_vm_accumulator);')
    elif op == 0x58:                     # partner.F = acc, di_track
        L.append(f'{{ uint16_t _a = {_partner_addr(body[0])};')
        L.append('  vm.ds_write(_a, v2_vm_accumulator);')
        L.append('  vm.di_track = (uint16_t)(_a - OBJ_FIELD_BASE); }')
    elif op in (0x59, 0x5C, 0x5F, 0x62, 0x65):   # self.F op= acc (no tracks)
        oper = {0x59: '+', 0x5C: '-', 0x5F: '&', 0x62: '|', 0x65: '^'}[op]
        L.append(f'{{ uint16_t _a = {_self_addr(body[0])};')
        L.append(f'  vm.ds_write(_a, (uint16_t)(vm.ds_read(_a) {oper} v2_vm_accumulator)); }}')
    elif op in (0x5A, 0x5D, 0x60, 0x63, 0x66):   # [addr] op= acc
        oper = {0x5A: '+', 0x5D: '-', 0x60: '&', 0x63: '|', 0x66: '^'}[op]
        a = _imm16(body)
        L.append(f'vm.ds_write(0x{a:04X}, (uint16_t)(vm.ds_read(0x{a:04X}) {oper} v2_vm_accumulator));')
    elif op in (0x5B, 0x5E, 0x64):       # partner.F op= acc via 1995_target:
        oper = {0x5B: '+', 0x5E: '-', 0x64: '|'}[op]   # si_track=cur, di_track=slot
        L.append(f'{{ uint16_t _si = vm.global_r(DS_CUR_OBJ);')
        L.append(f'  uint16_t _di = (uint16_t)(vm.ds_read((uint16_t)(_si + OBJ_PARTNER)) + 0x{_fcol(body[0]) - 0x14E5:04X});')
        L.append(f'  // partner.{ex.field_name(body[0])} (indexed_1995_target)')
        L.append('  vm.si_track = _si; vm.di_track = _di;')
        L.append('  uint16_t _a = (uint16_t)(_di + OBJ_FIELD_BASE);')
        L.append(f'  vm.ds_write(_a, (uint16_t)(vm.ds_read(_a) {oper} v2_vm_accumulator)); }}')
    elif op == 0x61:                     # field_addr_A: PARTNER, both tracks
        L.append(f'{{ uint16_t _si = vm.global_r(DS_CUR_OBJ);')
        L.append(f'  uint16_t _di = (uint16_t)(vm.ds_read((uint16_t)(_si + OBJ_PARTNER)) + 0x{_fcol(body[0]) - 0x14E5:04X});')
        L.append(f'  // partner.{ex.field_name(body[0])} (field_addr_A)')
        L.append('  vm.si_track = _si; vm.di_track = _di;')
        L.append('  uint16_t _a = (uint16_t)(_di + OBJ_FIELD_BASE);')
        L.append('  vm.ds_write(_a, (uint16_t)(vm.ds_read(_a) & v2_vm_accumulator)); }')
    elif op == 0x67:                     # partner.F ^= acc, di_track only
        L.append(f'{{ uint16_t _a = {_partner_addr(body[0])};')
        L.append('  vm.ds_write(_a, (uint16_t)(vm.ds_read(_a) ^ v2_vm_accumulator));')
        L.append('  vm.di_track = (uint16_t)(_a - OBJ_FIELD_BASE); }')
    else:
        return None
    L.append(f'vm.pc = 0x{nxt:04X};')
    return L

# Wave 2: the whole compare-branch family. Fetch (with its exact
# si/di_track effects) + comparison + pc=TGT/NEXT, verbatim per handler.
# do_call_jump forms store OBJ_ALT_PC = NEXT (orig: vm.pc+2 at the
# T-word position) on the CURRENT object before jumping.
_CMPBR = {
    # op: (fetch, cond)  — cond is a C expression over acc and _v;
    # fetch in {'lit','self','mem','partner','rand'};
    # kind 'j' = do_jump on cond else skip; 'c' = do_call_jump on cond.
    0x68: ('lit',     'j', 'v2_vm_accumulator >= _v'),
    0x69: ('self',    'j', 'v2_vm_accumulator >= _v'),
    0x6A: ('mem',     'j', 'v2_vm_accumulator >= _v'),
    0x6B: ('partner', 'j', 'v2_vm_accumulator >= _v'),
    0x6C: ('rand',    'j', 'v2_vm_accumulator >= _v'),
    0x6D: ('lit',     'j', 'v2_vm_accumulator < _v'),
    0x6E: ('self',    'j', 'v2_vm_accumulator < _v'),
    0x6F: ('mem',     'j', 'v2_vm_accumulator < _v'),
    0x70: ('partner', 'j', 'v2_vm_accumulator < _v'),
    0x71: ('rand',    'j', 'v2_vm_accumulator < _v'),
    0x72: ('lit',     'j', '_v == v2_vm_accumulator'),
    0x73: ('self',    'j', '_v == v2_vm_accumulator'),
    0x74: ('mem',     'j', '_v == v2_vm_accumulator'),
    0x75: ('partner', 'j', '_v == v2_vm_accumulator'),
    0x76: ('rand',    'j', '_v == v2_vm_accumulator'),
    0x77: ('lit',     'j', '_v != v2_vm_accumulator'),
    0x78: ('self',    'j', '_v != v2_vm_accumulator'),
    0x79: ('mem',     'j', '_v != v2_vm_accumulator'),
    0x7A: ('partner', 'j', '_v != v2_vm_accumulator'),
    0x7B: ('rand',    'j', '_v != v2_vm_accumulator'),
    0x7C: ('lit',     'j', '(int16_t)v2_vm_accumulator >= (int16_t)_v'),
    0x7D: ('self',    'j', '(int16_t)v2_vm_accumulator >= (int16_t)_v'),
    0x7E: ('mem',     'j', '(int16_t)v2_vm_accumulator >= (int16_t)_v'),
    0x7F: ('partner', 'j', '(int16_t)v2_vm_accumulator >= (int16_t)_v'),
    0x80: ('rand',    'j', '(int16_t)v2_vm_accumulator >= (int16_t)_v'),
    0x81: ('lit',     'j', '(int16_t)v2_vm_accumulator < (int16_t)_v'),
    0x82: ('self',    'j', '(int16_t)v2_vm_accumulator < (int16_t)_v'),
    0x83: ('mem',     'j', '(int16_t)v2_vm_accumulator < (int16_t)_v'),
    0x84: ('partner', 'j', '(int16_t)v2_vm_accumulator < (int16_t)_v'),
    0x85: ('rand',    'j', '(int16_t)v2_vm_accumulator < (int16_t)_v'),
    0x86: ('lit',     'c', '_v == v2_vm_accumulator'),
    0x87: ('self',    'c', '_v == v2_vm_accumulator'),
    0x88: ('mem',     'c', '_v == v2_vm_accumulator'),
    0x89: ('partner', 'c', '_v == v2_vm_accumulator'),
    0x8B: ('lit',     'c', '_v != v2_vm_accumulator'),
    0x8C: ('self',    'c', '_v != v2_vm_accumulator'),
    0x8D: ('mem',     'c', '_v != v2_vm_accumulator'),
    0x8E: ('partner', 'c', '_v != v2_vm_accumulator'),
    0x8F: ('rand',    'c', '_v != v2_vm_accumulator'),
}

def inline_wave2(op, body, tgt, nxt):
    if op == 0x1C:   # sub_1443d: [cur+OBJ_ANIM_TIMER] != 0 ? skip : jump
        return ['{ uint16_t _t = vm.field_r(OBJ_ANIM_TIMER);',
                f'  vm.pc = (_t != 0) ? 0x{nxt:04X} : 0x{tgt:04X}; }}']
    e = _CMPBR.get(op)
    if e is None:
        return None
    fetch, kind, cond = e
    L = ['{']
    if fetch == 'lit':
        L.append(f'  const uint16_t _v = 0x{_imm16(body):04X};')
    elif fetch == 'self':
        a = _self_addr(body[0])
        L.append(f'  uint16_t _a = {a};')
        L.append('  vm.si_track = (uint16_t)(_a - OBJ_FIELD_BASE);  // ch1: slot base in SI')
        L.append('  const uint16_t _v = vm.ds_read(_a);')
    elif fetch == 'mem':
        a = _imm16(body)
        L.append(f'  vm.si_track = 0x{a:04X};  // ch2: address in SI')
        L.append(f'  const uint16_t _v = vm.ds_read(0x{a:04X});')
    elif fetch == 'partner':
        # read_indexed_field_1995: si_track = cur obj, di_track = slot
        L.append('  uint16_t _si = vm.global_r(DS_CUR_OBJ);')
        L.append(f'  uint16_t _di = (uint16_t)(vm.ds_read((uint16_t)(_si + OBJ_PARTNER)) + 0x{_fcol(body[0]) - 0x14E5:04X});')
        L.append(f'  // partner.{ex.field_name(body[0])}')
        L.append('  vm.si_track = _si; vm.di_track = _di;')
        L.append('  const uint16_t _v = vm.ds_read((uint16_t)(_di + OBJ_FIELD_BASE));')
    elif fetch == 'rand':
        L.append('  const uint16_t _v = v2_vm_read_random(vm);')
    if kind == 'j':
        L.append(f'  vm.pc = ({cond}) ? 0x{tgt:04X} : 0x{nxt:04X};')
    else:
        L.append(f'  if ({cond}) {{')
        L.append('      ObjRef{vm, vm.global_r(DS_CUR_OBJ)}'
                 f'.w16(OBJ_ALT_PC, 0x{nxt:04X});  // do_call_jump: return pc')
        L.append(f'      vm.pc = 0x{tgt:04X};')
        L.append(f'  }} else vm.pc = 0x{nxt:04X};')
    L.append('}')
    return L

def handler_map():
    src = open('src/sdl/v2_vm.cpp').read()
    tbl = {}
    for m in re.finditer(r'v2_vm_optable\[0x([0-9A-Fa-f]{2})\]\s*=\s*(\w+);', src):
        tbl[int(m.group(1), 16)] = m.group(2)
    return tbl

def transpile(cid, outdir='src/sdl/gen'):
    d, seen, table = lf.full_walk(cid)
    tbl = handler_map()
    LAY = dz.load_layout_names()
    lines = []
    w = lines.append
    w(f'// AUTO-GENERATED by tools/data/gen_transpile.py from chunk 0x{cid:X}.')
    w(f'// {len(seen)} instructions. Do not edit — regenerate.')
    w(f'static bool v2_gen_exec_{cid:x}(V2VM& vm, int& max_ops) {{')
    w('    uint16_t g_last_pc = 0xFFFF;   // diagnostics: last executed insn')
    w('    (void)g_last_pc;')
    w('    while (vm.running) {')
    w('        switch (vm.pc) {')
    for pc in sorted(seen):
        w(f'        case 0x{pc:04X}: goto L_{pc:04X};')
    w('        default:')
    w('            { static int _gdbg = 0; if (_gdbg < 20) { _gdbg++;')
    w('              fprintf(stderr, "V2-GEN-FALLBACK: pc=%04X after insn %04X '
      'obj=%02X\\n", vm.pc, g_last_pc, vm.obj); } }')
    w('            return false;   // unknown pc -> interpreter fallback')
    w('        }')
    # Instruction blocks. Control flow trusts ONLY vm.pc: after every
    # handler we re-enter the dispatch switch (`continue`). Handlers own
    # their pc semantics — including dynamic ones the static model cannot
    # prove (2D/36 search continuations jump via DS_SEARCH_JUMP, 06 loads
    # ALT_PC). What dies versus the interpreter is the fetch+decode+table
    # dispatch; static goto shortcuts can come back later per proven op.
    for pc in sorted(seen):
        op = seen[pc][0]
        ln, kind, tgt = dc.decode_info(d, pc, op, table)
        h = tbl.get(op)
        if h is None:
            raise SystemExit(f'no handler for op {op:02X}')
        ob_end = pc + ln - (2 if (kind in ('br', 'jmp', 'srch') and tgt is not None) else 0)
        body = d[pc+1:ob_end]
        e = ex.render(op, body, LAY)
        cmt = f'  // {e}' if e else ''
        w(f'    L_{pc:04X}:')
        w(f'        g_last_pc = 0x{pc:04X};')
        w(f'        G_PRE(0x{pc:04X}, 0x{op:02X});')
        inl = None
        if kind == 'fall':
            try:
                inl = inline_wave1(op, body, pc + ln)
            except Exception:
                inl = None
        elif kind == 'br' and tgt is not None:
            try:
                inl = inline_wave2(op, body, tgt, pc + ln)
            except Exception:
                inl = None
        if inl is not None:
            if e:
                w(f'        // inlined {h}: {e}')
            for line in inl:
                w(f'        {line}')
        else:
            w(f'        {h}(vm);{cmt}')
        w(f'        G_POST(0x{pc:04X}, 0x{op:02X});')
        w('        continue;')
    w('    }')
    w('    return true;')
    w('}')
    os.makedirs(outdir, exist_ok=True)
    path = f'{outdir}/chunk_{cid:04x}.gen.inc'
    open(path, 'w').write('\n'.join(lines) + '\n')
    print(f'{path}: {len(lines)} lines, {len(seen)} instructions')
    return path

def transpile_anim(cid, outdir='src/sdl/gen'):
    """Anim-VM executor: cmd fetch + table read unrolled per known bx.
    The handler word still comes from the DS table (runtime semantics —
    exec_anim_cmd dispatches on it exactly like the loop); operand
    consumption stays inside exec_anim_cmd. Unknown bx -> false ->
    the caller resumes the interpreter loop."""
    anims = lf.anim_layer(cid)
    lines = []
    w = lines.append
    w(f'// AUTO-GENERATED anim executor for chunk 0x{cid:X} '
      f'({len(anims)} commands).')
    w(f'static bool v2_gen_anim_{cid:x}(V2VM& vm, uint16_t& anim_bx, int& max) {{')
    w('    while (max > 0) {')
    w('        switch (anim_bx) {')
    for pc in sorted(anims):
        w(f'        case 0x{pc:04X}: goto A_{pc:04X};')
    w('        default: return false;   // unknown bx -> interpreter fallback')
    w('        }')
    for pc in sorted(anims):
        cmd, ln, kind, tgt = anims[pc]
        w(f'    A_{pc:04X}:')
        w('        max--;')
        w('        {')
        w('        v2_v2_anim_cmd_count++;')
        if cmd <= 0x1A:
            w(f'        v2_op_anim_count[0x{cmd:02X}]++;')
        w(f'        const uint16_t _bxb = 0x{pc:04X};')
        w(f'        anim_bx = 0x{pc + 1:04X};')
        w(f'        const uint16_t _h = *(uint16_t*)(vm.shadow + '
          f'DS_CMD_HANDLER_TBL + 0x{cmd:02X} * 2);')
        w(f'        bool _ok = v2_vm_exec_anim_cmd(vm, _h, anim_bx, 0x{cmd:02X});')
        w(f'        v2_animdump_note(vm.shadow, 0x{pc + 1:04X}, 0x{cmd:02X}, anim_bx);')
        w('        AnimCmdTrace& _t = v2_anim_trace[v2_anim_trace_idx & 15];')
        w(f'        _t.cmd = 0x{cmd:02X}; _t.handler = _h; '
          f'_t.bx_before = _bxb; _t.bx_after = anim_bx;')
        w('        v2_anim_trace_idx++;')
        w('        if (!_ok) return true;')
        w('        }')
        w('        continue;')
    w('    }')
    w('    return true;')
    w('}')
    os.makedirs(outdir, exist_ok=True)
    path = f'{outdir}/anim_{cid:04x}.gen.inc'
    open(path, 'w').write('\n'.join(lines) + '\n')
    print(f'{path}: {len(lines)} lines, {len(anims)} commands')
    return path

if __name__ == '__main__':
    cid = int(sys.argv[1], 16)
    outdir = sys.argv[2] if len(sys.argv) > 2 else 'src/sdl/gen'
    transpile(cid, outdir)
    transpile_anim(cid, outdir)
