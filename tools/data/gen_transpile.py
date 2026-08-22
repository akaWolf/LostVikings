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

# Wave 3: control flow (yield/jmp/call/ret), bittest family with the
# mask/clear LUTs folded (proven immutable: no operand address reaches
# the 0x93xx zone — map max 0x7804 — and the per-opcode parity of waves
# 1-2 already validated the field-LUT folding the same way), flips,
# vel setters, spawn-record stores and the small fixed-effect ops.
def _mask(idx):
    return ex.bit_mask(idx)

def _clear(idx):
    return ex.bit_clear(idx)

_BT_FETCH = {
    # opcode -> (helper_kind) for the bittest fetch step
    # '153ea': [maskIdx][imm16], si_track=maskIdx
    # '15403': [maskIdx][fldIdx] self, si_track=maskIdx
    # '1542a': [maskIdx][addr16], si_track=maskIdx
    # '15445': [maskIdx][fldIdx] partner, si_track=maskIdx, di_track=slot
}

def _bt_fetch(kind, body):
    """Returns (lines, value_expr) reproducing the bittest fetch helpers."""
    idx1 = body[0]
    L = []
    if kind == '153ea':
        imm = int.from_bytes(body[1:3], 'little')
        L.append(f'  vm.si_track = 0x{idx1:04X};  // 153ea tail: POP si = mask byte')
        return L, f'((0x{imm:04X} & 0x{_mask(idx1):04X}) ? 1 : 0)'
    if kind == '15403':
        a = _self_addr(body[1])
        L.append(f'  uint16_t _fv = vm.ds_read({a});')
        L.append(f'  vm.si_track = 0x{idx1:04X};  // 15403 tail: POP si = mask byte')
        return L, f'((_fv & 0x{_mask(idx1):04X}) ? 1 : 0)'
    if kind == '1542a':
        a = int.from_bytes(body[1:3], 'little')
        L.append(f'  uint16_t _fv = vm.ds_read(0x{a:04X});')
        L.append(f'  vm.si_track = 0x{idx1:04X};  // 1542a tail: POP si = mask byte')
        return L, f'((_fv & 0x{_mask(idx1):04X}) ? 1 : 0)'
    if kind == '15445':
        L.append('  uint16_t _obj = vm.global_r(DS_CUR_OBJ);')
        L.append(f'  uint16_t _di = (uint16_t)(vm.ds_read((uint16_t)(_obj + OBJ_PARTNER)) + 0x{_fcol(body[1]) - 0x14E5:04X});')
        L.append(f'  // partner.{ex.field_name(body[1])}')
        L.append('  vm.di_track = _di;')
        L.append('  uint16_t _fv = vm.ds_read((uint16_t)(_di + OBJ_FIELD_BASE));')
        L.append(f'  vm.si_track = 0x{idx1:04X};  // 15445 tail: POP si = first byte')
        return L, f'((_fv & 0x{_mask(idx1):04X}) ? 1 : 0)'
    raise KeyError(kind)

_BT_LOAD = {0x97: '153ea', 0x98: '15403', 0x99: '1542a', 0x9A: '15445'}
_BT_BR = {
    # op: (fetch, cond ('eq'/'ne'), call?)   — polarity read from the bodies
    0xA8: ('153ea', 'eq', False), 0xA9: ('15403', 'eq', False),
    0xAA: ('1542a', 'eq', False), 0xAB: ('15445', 'eq', False),
    0xAD: ('153ea', 'ne', False), 0xAE: ('15403', 'ne', False),
    0xB0: ('15445', 'ne', False),
    0xB2: ('153ea', 'eq', True),  0xB3: ('15403', 'eq', True),
    0xB5: ('15445', 'eq', True),
}
# AF/B8/B9/BB: standalone bodies (no si_track at all — they read idx/addr
# inline, unlike the helper fetches).
def _bt_raw(op, body):
    idx1 = body[0]
    if op in (0xAF, 0xB9):
        a = int.from_bytes(body[1:3], 'little')
        return ([f'  uint16_t _r = (vm.ds_read(0x{a:04X}) & 0x{_mask(idx1):04X}) ? 1 : 0;'],
                '_r')
    if op == 0xB8:
        a = _self_addr(body[1])
        return ([f'  uint16_t _r = (vm.ds_read({a}) & 0x{_mask(idx1):04X}) ? 1 : 0;'],
                '_r')
    if op == 0xBB:
        return (['  uint16_t _r = v2_vm_read_random(vm) & 1;'], '_r')
    raise KeyError(op)

def inline_wave3(op, body, kind, tgt, nxt, pc):
    L = []
    def jump_or(cond_expr, call):
        if call:
            L.append(f'  if ({cond_expr}) {{')
            L.append('      ObjRef{vm, vm.global_r(DS_CUR_OBJ)}'
                     f'.w16(OBJ_ALT_PC, 0x{nxt:04X});')
            L.append(f'      vm.pc = 0x{tgt:04X};')
            L.append(f'  }} else vm.pc = 0x{nxt:04X};')
        else:
            L.append(f'  vm.pc = ({cond_expr}) ? 0x{tgt:04X} : 0x{nxt:04X};')
    # --- control flow ---
    if op == 0x00:   # yield: OBJ_PC = pc (already PC+1), stop
        return ['ObjRef{vm, vm.global_r(DS_CUR_OBJ)}'
                f'.w16(OBJ_PC, 0x{pc + 1:04X});',
                'vm.running = false;']
    if op == 0x01:
        return [f'vm.pc = 0x{nxt:04X};']
    if op == 0x03 and tgt is not None:
        return [f'vm.pc = 0x{tgt:04X};']
    if op == 0x05 and tgt is not None:   # save_alt_pc = do_call_jump
        return ['ObjRef{vm, vm.global_r(DS_CUR_OBJ)}'
                f'.w16(OBJ_ALT_PC, 0x{pc + 3:04X});',
                f'vm.pc = 0x{tgt:04X};']
    if op == 0x06:   # ret: dynamic pc from ALT_PC — dispatch re-enters
        return ['vm.pc = ObjRef{vm, vm.global_r(DS_CUR_OBJ)}.u16(OBJ_ALT_PC);']
    # --- fixed-effect simple ops ---
    if op == 0x4B:
        return ['{ uint16_t _o = vm.global_r(DS_CUR_OBJ);',
                '  ObjRef{vm, _o}.w16(OBJ_FLAGS, (uint16_t)(ObjRef{vm, _o}.u16(OBJ_FLAGS) | 0x2000)); }',
                f'vm.pc = 0x{nxt:04X};']
    if op == 0x96:
        return ['vm.field_w(OBJ_PARTNER, v2_vm_accumulator);',
                f'vm.pc = 0x{nxt:04X};']
    if op == 0x19:
        imm = _imm16(body)
        return [f'vm.field_w(OBJ_ANIM_PC, 0x{imm:04X});',
                'vm.field_w(OBJ_ANIM_TIMER, 1);',
                f'vm.pc = 0x{nxt:04X};']
    if op == 0x18:   # anim_tbl=partner; vel = two signed bytes
        vx = body[0] - 256 if body[0] >= 128 else body[0]
        vy = body[1] - 256 if body[1] >= 128 else body[1]
        return ['vm.field_w(OBJ_ANIM_TABLE, vm.field_r(OBJ_PARTNER));',
                f'vm.field_w(OBJ_VEL_X, (uint16_t)(int16_t){vx});',
                f'vm.field_w(OBJ_VEL_Y, (uint16_t)(int16_t){vy});',
                f'vm.pc = 0x{nxt:04X};']
    if op == 0x17:   # anim_tbl=0; vel
        vx = body[0] - 256 if body[0] >= 128 else body[0]
        vy = body[1] - 256 if body[1] >= 128 else body[1]
        return ['{ uint16_t _o = vm.global_r(DS_CUR_OBJ);',
                '  ObjRef{vm, _o}.w16(OBJ_ANIM_TABLE, 0);',
                f'  ObjRef{{vm, _o}}.w16(OBJ_VEL_X, (uint16_t)(int16_t){vx});',
                f'  ObjRef{{vm, _o}}.w16(OBJ_VEL_Y, (uint16_t)(int16_t){vy}); }}',
                f'vm.pc = 0x{nxt:04X};']
    if op == 0x1B:   # obj0: [OBJ_ANIM_TABLE global] = cur; obj0.vel
        vx = body[0] - 256 if body[0] >= 128 else body[0]
        vy = body[1] - 256 if body[1] >= 128 else body[1]
        return ['vm.ds_write(OBJ_ANIM_TABLE, vm.global_r(DS_CUR_OBJ));',
                f'ObjRef{{vm, 0}}.w16(OBJ_VEL_X, (uint16_t)(int16_t){vx});',
                f'ObjRef{{vm, 0}}.w16(OBJ_VEL_Y, (uint16_t)(int16_t){vy});',
                f'vm.pc = 0x{nxt:04X};']
    if op in (0x07, 0x08, 0x0B):   # hflip variants (primitive call)
        cond = {0x07: '!(ObjRef{vm, _o}.u16(OBJ_FLAGS) & 0x40)',
                0x08: '(ObjRef{vm, _o}.u16(OBJ_FLAGS) & 0x40)',
                0x0B: 'true'}[op]
        return ['{ uint16_t _o = vm.global_r(DS_CUR_OBJ);',
                f'  if ({cond}) v2_vm_hflip_body_136a0(vm, _o); }}',
                f'vm.pc = 0x{nxt:04X};']
    if op in (0x09, 0x0A, 0x0C):   # vflip variants
        cond = {0x09: '!(ObjRef{vm, _o}.u16(OBJ_FLAGS) & 0x80)',
                0x0A: '(ObjRef{vm, _o}.u16(OBJ_FLAGS) & 0x80)',
                0x0C: 'true'}[op]
        return ['{ uint16_t _o = vm.global_r(DS_CUR_OBJ);',
                f'  if ({cond}) v2_vm_vflip_body_13757(vm, _o); }}',
                f'vm.pc = 0x{nxt:04X};']
    if op in (0x11, 0x12, 0x3A):   # res_deduct(partner)
        return ['{ uint16_t _di = vm.global_r(DS_CUR_OBJ);',
                '  uint16_t _si = ObjRef{vm, _di}.u16(OBJ_PARTNER);',
                '  v2_vm_res_deduct_15505(vm, _si, _di); }',
                f'vm.pc = 0x{nxt:04X};']
    if op == 0x2F:
        return ['v2_vm_anim_interp_1303a(vm);',
                'v2_vm_anim_tail_135cf(vm);',
                f'vm.pc = 0x{nxt:04X};']
    # --- bittest loads ---
    if op in _BT_LOAD:
        f, v = _bt_fetch(_BT_LOAD[op], body)
        return ['{'] + f + [f'  v2_vm_accumulator = {v};', '}',
                            f'vm.pc = 0x{nxt:04X};']
    # --- bittest branches ---
    if op in _BT_BR and tgt is not None:
        fk, pol, call = _BT_BR[op]
        f, v = _bt_fetch(fk, body)
        L.append('{')
        L.extend(f)
        cond = (f'{v} == v2_vm_accumulator' if pol == 'eq'
                else f'{v} != v2_vm_accumulator')
        jump_or(cond, call)
        L.append('}')
        return L
    if op in (0xAF, 0xB8, 0xB9, 0xBB) and tgt is not None:
        f, v = _bt_raw(op, body)
        pol_call = {0xAF: ('ne', False), 0xB8: ('ne', True),
                    0xB9: ('ne', True), 0xBB: ('ne', True)}[op]
        L.append('{')
        L.extend(f)
        cond = (f'{v} != v2_vm_accumulator' if pol_call[0] == 'ne'
                else f'{v} == v2_vm_accumulator')
        jump_or(cond, pol_call[1])
        L.append('}')
        return L
    # --- mask-merge RMW ---
    if op in (0x9C, 0x9D, 0x9E, 0xA0, 0xA3, 0xA4, 0xA5, 0xA7):
        idx1 = body[0]
        m, c = _mask(idx1), _clear(idx1)
        L.append('{')
        L.append('  if (v2_vm_accumulator != 0)')
        L.append(f'      v2_vm_accumulator = 0x{m:04X};')
        if op == 0x9C:      # self.F = F&clear | acc
            a = _self_addr(body[1])
            L.append(f'  uint16_t _a = {a};')
            L.append(f'  vm.ds_write(_a, (uint16_t)((vm.ds_read(_a) & 0x{c:04X}) | v2_vm_accumulator));')
        elif op == 0x9D:    # [addr] = &clear | acc
            a = int.from_bytes(body[1:3], 'little')
            L.append(f'  vm.ds_write(0x{a:04X}, (uint16_t)((vm.ds_read(0x{a:04X}) & 0x{c:04X}) | v2_vm_accumulator));')
        elif op == 0x9E:    # partner.F via 1995_target (si/di tracks!)
            L.append('  uint16_t _si = vm.global_r(DS_CUR_OBJ);')
            L.append(f'  uint16_t _di = (uint16_t)(vm.ds_read((uint16_t)(_si + OBJ_PARTNER)) + 0x{_fcol(body[1]) - 0x14E5:04X});')
            L.append('  vm.si_track = _si; vm.di_track = _di;')
            L.append('  uint16_t _a = (uint16_t)(_di + OBJ_FIELD_BASE);')
            L.append(f'  vm.ds_write(_a, (uint16_t)((vm.ds_read(_a) & 0x{c:04X}) | v2_vm_accumulator));')
        elif op == 0xA0:    # [addr] &= acc  (no clear)
            a = int.from_bytes(body[1:3], 'little')
            L.append(f'  vm.ds_write(0x{a:04X}, (uint16_t)(vm.ds_read(0x{a:04X}) & v2_vm_accumulator));')
        elif op == 0xA3:
            a = int.from_bytes(body[1:3], 'little')
            L.append(f'  vm.ds_write(0x{a:04X}, (uint16_t)(vm.ds_read(0x{a:04X}) | v2_vm_accumulator));')
        elif op == 0xA5:    # self.F ^= acc
            a = _self_addr(body[1])
            L.append(f'  uint16_t _a = {a};')
            L.append('  vm.ds_write(_a, (uint16_t)(vm.ds_read(_a) ^ v2_vm_accumulator));')
        elif op in (0xA4, 0xA7):   # partner.F |=/^= acc (BOTH tracks per body)
            oper = '|' if op == 0xA4 else '^'
            L.append('  uint16_t _obj = vm.global_r(DS_CUR_OBJ);')
            L.append(f'  uint16_t _di = (uint16_t)(vm.ds_read((uint16_t)(_obj + OBJ_PARTNER)) + 0x{_fcol(body[1]) - 0x14E5:04X});')
            L.append('  vm.si_track = _obj; vm.di_track = _di;')
            L.append('  uint16_t _a = (uint16_t)(_di + OBJ_FIELD_BASE);')
            L.append(f'  vm.ds_write(_a, (uint16_t)(vm.ds_read(_a) {oper} v2_vm_accumulator));')
        L.append('}')
        L.append(f'vm.pc = 0x{nxt:04X};')
        return L
    # --- acc<<8 stores ---
    if op == 0xBC:
        return ['v2_vm_accumulator <<= 8;',
                f'vm.ds_write({_self_addr(body[0])}, v2_vm_accumulator);',
                f'vm.pc = 0x{nxt:04X};']
    if op == 0xBD:
        a = _imm16(body)
        return ['v2_vm_accumulator <<= 8;',
                f'vm.ds_write(0x{a:04X}, v2_vm_accumulator);',
                f'vm.pc = 0x{nxt:04X};']
    if op == 0xBE:   # via 1995_target (si/di tracks)
        return ['v2_vm_accumulator <<= 8;',
                '{ uint16_t _si = vm.global_r(DS_CUR_OBJ);',
                f'  uint16_t _di = (uint16_t)(vm.ds_read((uint16_t)(_si + OBJ_PARTNER)) + 0x{_fcol(body[0]) - 0x14E5:04X});',
                '  vm.si_track = _si; vm.di_track = _di;',
                '  vm.ds_write((uint16_t)(_di + OBJ_FIELD_BASE), v2_vm_accumulator); }',
                f'vm.pc = 0x{nxt:04X};']
    return None

# Wave 4: collision family (via the new _f cores), viking scan family,
# probe family with the runtime 30C8E sub-dispatch reproduced verbatim.
def _dispatch_c8e(L, tbl_index, tgt, nxt, carry='vm.carry'):
    """Reproduce the off_30C8E[tbl_index] runtime dispatch used by the
    probe opcodes: 44E9 no-carry-skip/carry-jump, 44F3 inverse, 42CF
    unconditional jump; anything else -> the shared runtime_dispatch
    (which the loop path also uses)."""
    L.append(f'  uint16_t _cs = v2gs(vm.shadow).vm_subdispatch_tbl({tbl_index});')
    L.append('  if (_cs == 0x44E9) {')
    L.append(f'      vm.pc = (!{carry}) ? 0x{nxt:04X} : 0x{tgt:04X};')
    L.append('  } else if (_cs == 0x44F3) {')
    L.append(f'      vm.pc = ({carry}) ? 0x{nxt:04X} : 0x{tgt:04X};')
    L.append('  } else if (_cs == 0x42CF) {')
    L.append(f'      vm.pc = 0x{tgt:04X};')
    L.append('  } else {')
    L.append(f'      vm.pc = 0x{nxt - 2:04X};  // T-word position for runtime_dispatch')
    L.append(f'      v2_vm_runtime_dispatch(vm, 0x87AE, {tbl_index});')
    L.append('  }')

_COLL = {
    # op: (core_f, filter_width, vik_limit)
    0x1A: ('v2_vm_collision_155d6_f', 1, True),
    0x1D: ('v2_vm_collision_156c0_f', 2, True),
    0x32: ('v2_vm_collision_15788_f', 1, False),
    0x33: ('v2_vm_collision_157eb_f', 1, False),
    0x37: ('v2_vm_collision_155d6_f', 1, False),
    0x38: ('v2_vm_collision_156c0_f', 2, False),
    0x3C: ('v2_vm_collision_1584e_f', 1, False),
}

_SCAN = {
    # viking scan family: explicit filter byte + primitive + branch shape.
    # op: (call_expr(filter), branch)  branch: 'jump' carry->TGT,
    # 'skip' carry->NEXT, 'flip_rl'/'flip_lr' pick the primitive by hflip.
    0xBF: ('v2_vm_obj_search_up_15fb1(vm, {f}, _di)', 'jump'),
    0xC0: ('v2_vm_obj_search_down_15fbe(vm, {f}, _di)', 'jump'),
    0xC3: ('v2_vm_obj_search_up_15fb1(vm, {f}, _di)', 'skip'),
    0xC4: ('v2_vm_obj_search_down_15fbe(vm, {f}, _di)', 'skip'),
}

def inline_wave4(op, body, kind, tgt, nxt, pc):
    L = []
    if op in _COLL and tgt is not None:
        core, w, vik = _COLL[op]
        filt = _imm16(body) if w == 2 else body[0]
        L.append('{')
        if vik:
            L.append('  uint16_t _saved = vm.ds_read(DS_OBJ_COUNT);')
            L.append('  vm.ds_write(DS_OBJ_COUNT, 6);   // viking-only scan limit')
        L.append(f'  bool _c = {core}(vm, 0x{filt:0{w*2}X});')
        if vik:
            L.append('  vm.ds_write(DS_OBJ_COUNT, _saved);')
        L.append('  vm.ds_write(DS_COLL_BIT_IDX, (uint16_t)(vm.ds_read(DS_COLL_BIT_IDX) + 2));  // ALWAYS')
        L.append('  if (_c) {')
        L.append('      ObjRef{vm, vm.global_r(DS_CUR_OBJ)}'
                 f'.w16(OBJ_ALT_PC, 0x{nxt:04X});  // do_call_jump')
        L.append(f'      vm.pc = 0x{tgt:04X};')
        L.append(f'  }} else vm.pc = 0x{nxt:04X};')
        L.append('}')
        return L
    if op in (0x4E, 0x4F) and tgt is not None:
        L.append('{')
        L.append('  vm.carry = v2_vm_platform_check_163ac(vm);')
        if op == 0x4F:   # fixed: no-carry -> jump, carry -> skip
            L.append(f'  vm.pc = (!vm.carry) ? 0x{tgt:04X} : 0x{nxt:04X};')
        else:            # 4E: 30C8E[0] dispatch
            _dispatch_c8e(L, 0, tgt, nxt)
        L.append('}')
        return L
    if op in _SCAN and tgt is not None:
        call, br = _SCAN[op]
        f = body[0]
        L.append('{')
        L.append('  vm.ds_write(DS_SEARCH_RES_SLOT, 0xFFFF);')
        L.append('  uint16_t _saved = vm.ds_read(DS_OBJ_COUNT);')
        L.append('  vm.ds_write(DS_OBJ_COUNT, 6);')
        L.append('  uint16_t _di = vm.global_r(DS_CUR_OBJ);')
        if op == 0xC0:   # only the C0 body seeds si/di tracks
            L.append(f'  vm.si_track = 0x{f:02X}; vm.di_track = _di;')
        L.append(f'  vm.carry = {call.format(f=f"0x{f:02X}")};')
        L.append('  vm.ds_write(DS_OBJ_COUNT, _saved);')
        if br == 'jump':
            L.append(f'  vm.pc = vm.carry ? 0x{tgt:04X} : 0x{nxt:04X};')
        else:
            L.append(f'  vm.pc = vm.carry ? 0x{nxt:04X} : 0x{tgt:04X};')
        L.append('}')
        return L
    if op in (0xC1, 0xC2, 0xC5, 0xC6) and tgt is not None:
        # side scans: hflip picks the primitive; C1/C2 carry->jump,
        # C5/C6 carry->skip; C1/C5: flip->right-scan, C2/C6: !flip->right.
        f = body[0]
        right_on_flip = op in (0xC1, 0xC5)
        carry_jump = op in (0xC1, 0xC2)
        L.append('{')
        L.append('  vm.ds_write(DS_SEARCH_RES_SLOT, 0xFFFF);')
        L.append('  uint16_t _saved = vm.ds_read(DS_OBJ_COUNT);')
        L.append('  vm.ds_write(DS_OBJ_COUNT, 6);')
        L.append('  uint16_t _di = vm.global_r(DS_CUR_OBJ);')
        L.append('  bool _flip = (ObjRef{vm, _di}.u16(OBJ_FLAGS) & 0x40) != 0;')
        cond = '_flip' if right_on_flip else '!_flip'
        L.append(f'  if ({cond})')
        L.append(f'      vm.carry = v2_vm_obj_scan_x_15dfd(vm, 0x{f:02X}, _di, '
                 '(uint16_t)(ObjRef{vm, _di}.u16(OBJ_BBOX_X1) + 1));')
        L.append('  else')
        L.append(f'      vm.carry = v2_vm_obj_search_left_15de5(vm, 0x{f:02X}, _di);')
        L.append('  vm.ds_write(DS_OBJ_COUNT, _saved);')
        if carry_jump:
            L.append(f'  vm.pc = vm.carry ? 0x{tgt:04X} : 0x{nxt:04X};')
        else:
            L.append(f'  vm.pc = vm.carry ? 0x{nxt:04X} : 0x{tgt:04X};')
        L.append('}')
        return L
    # probe family: explicit anim/filter byte; carry semantics per body.
    if op in (0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x30, 0x31) and tgt is not None:
        f = body[0]
        L.append('{')
        L.append('  uint16_t _di = vm.global_r(DS_CUR_OBJ);')
        if op in (0x20, 0x21, 0x24, 0x25):
            L.append('  bool _flip = (ObjRef{vm, _di}.u16(OBJ_FLAGS) & 0x40) != 0;')
            first_left = op in (0x20, 0x24)   # !flip -> left for 20/24
            a, b = ('v2_vm_probe_left_158aa', 'v2_vm_probe_right_158b9')
            if not first_left:
                a, b = b, a
            L.append(f'  if (!_flip) {a}(vm, 0x{f:02X}, _di);')
            L.append(f'  else        {b}(vm, 0x{f:02X}, _di);')
        elif op in (0x1F, 0x23):
            L.append(f'  v2_vm_probe_down_158d7(vm, 0x{f:02X}, _di);')
        elif op in (0x1E, 0x22):
            L.append(f'  v2_vm_probe_up_158c8(vm, 0x{f:02X}, _di);')
        elif op in (0x30, 0x31):
            L.append(f'  v2_vm_probe_front_158e6(vm, 0x{f:02X}, _di);')
        if op in (0x24, 0x30):   # fixed carry branch
            L.append(f'  vm.pc = vm.carry ? 0x{nxt:04X} : 0x{tgt:04X};'
                     if op == 0x24 else
                     f'  vm.pc = vm.carry ? 0x{tgt:04X} : 0x{nxt:04X};')
        else:
            # table index per body: 1E/1F/20/21 use off_30C8E[0],
            # 22/23/25/31 use off_30C8E[2] (byte offset -> index 1)
            tbl = 0 if op in (0x1E, 0x1F, 0x20, 0x21) else 1
            _dispatch_c8e(L, tbl, tgt, nxt)
        L.append('}')
        return L
    return None

# Wave 5: search cores + full unrolling of the 30C98 channel operands.
# Getter channels (dispatch semantics reproduced): every dispatch first
# clears ch4_mul_clobber; ch4 (random) sets it inside the primitive.
# ch5 returns the constant 0x000A (the dispatch prologue's (mode&7)<<1).
# ch6/7 sites keep their handler call (orig-UB escape guards).
def _ch_get(L, var, chan, body, o):
    """Emit getter channel code; returns bytes consumed or None if the
    site must stay on the handler (UB channels)."""
    L.append('  vm.ch4_mul_clobber = false;')
    if chan == 0:
        v = int.from_bytes(body[o:o+2], 'little')
        L.append(f'  const uint16_t {var} = 0x{v:04X};')
        return 2
    if chan == 1:
        a = _self_addr(body[o])
        L.append(f'  uint16_t {var}_a = {a};')
        L.append(f'  vm.si_track = (uint16_t)({var}_a - OBJ_FIELD_BASE);')
        L.append(f'  const uint16_t {var} = vm.ds_read({var}_a);')
        return 1
    if chan == 2:
        a = int.from_bytes(body[o:o+2], 'little')
        L.append(f'  vm.si_track = 0x{a:04X};')
        L.append(f'  const uint16_t {var} = vm.ds_read(0x{a:04X});')
        return 2
    if chan == 3:
        L.append('  { uint16_t _o3 = vm.global_r(DS_CUR_OBJ);')
        L.append(f'    _t3 = (uint16_t)(vm.ds_read((uint16_t)(_o3 + OBJ_PARTNER)) + 0x{_fcol(body[o]) - 0x14E5:04X});')
        L.append('    vm.si_track = _o3; vm.di_track = _t3; }')
        L.append(f'  const uint16_t {var} = vm.ds_read((uint16_t)(_t3 + OBJ_FIELD_BASE));')
        return 1
    if chan == 4:
        L.append(f'  const uint16_t {var} = v2_vm_read_random(vm);')
        return 0
    if chan == 5:
        L.append(f'  const uint16_t {var} = 0x000A;  // locret_15504 getter')
        return 0
    return None

def _ch_set(L, chan, body, o, val_expr):
    """Emit setter channel (154bf) code; returns bytes consumed or None."""
    if chan == 1:
        a = _self_addr(body[o])
        L.append(f'  {{ uint16_t _sa = {a};')
        L.append('    vm.si_track = _sa;   // 154cb: slot addr in SI')
        L.append(f'    vm.ds_write(_sa, {val_expr}); }}')
        return 1
    if chan == 2:
        a = int.from_bytes(body[o:o+2], 'little')
        L.append(f'  vm.si_track = 0x{a:04X};')
        L.append(f'  vm.ds_write(0x{a:04X}, {val_expr});')
        return 2
    if chan == 3:
        L.append('  { uint16_t _so = vm.global_r(DS_CUR_OBJ);')
        L.append(f'    uint16_t _sd = (uint16_t)(vm.ds_read((uint16_t)(_so + OBJ_PARTNER)) + 0x{_fcol(body[o]) - 0x14E5:04X});')
        L.append('    vm.si_track = _so; vm.di_track = _sd;')
        L.append(f'    vm.ds_write((uint16_t)(_sd + OBJ_FIELD_BASE), {val_expr}); }}')
        return 1
    if chan == 5:
        # setter ch5 stores the CURRENT anim pc — depends on live vm.pc,
        # which inline sites do not maintain mid-instruction: keep handler.
        return None
    return None

def inline_wave5(op, body, kind, tgt, nxt, pc):
    import os as _os
    _skip = set(int(x,16) for x in _os.environ.get('W5SKIP','').split(',') if x)
    if op in _skip:
        return None
    L = []
    # --- search family ---
    if op in (0x2C, 0x35) and kind == 'srch':
        core = 'v2_vm_search_2c_core' if op == 0x2C else 'v2_vm_search_35_core'
        # The core relies on vm.pc sitting at the continuation byte (pc+4):
        # found -> ALT_PC = pc, pc = target; miss -> pc += 1 (skip 2D/36).
        return [f'vm.pc = 0x{pc + 4:04X};  // continuation position',
                f'{core}(vm, 0x{body[0]:02X}, 0x{tgt:04X});']
    if op == 0xD1 and kind == 'srch':
        return [f'vm.pc = 0x{pc + 4:04X};  // continuation position',
                f'v2_vm_search_d1_core(vm, 0x{body[0]:02X}, 0x{tgt:04X});']
    if op in (0x2D, 0x36):
        # continuations: 0 operands; the body starts with pc-=1 and owns
        # all pc outcomes (SEARCH_JUMP / +1 / +2) — open primitive call.
        h = 'v2_vm_op_2D' if op == 0x2D else 'v2_vm_op_36'
        return [f'{h}(vm);   // open search continuation (no stream reads)']
    if op == 0xD0 and tgt is not None:
        return ['{ uint16_t _si = vm.global_r(DS_CUR_OBJ);',
                '  vm.ds_write(DS_SEARCH_Y, (uint16_t)(ObjRef{vm, _si}.u16(OBJ_BBOX_Y1) + 1));',
                f'  vm.ds_write(DS_SEARCH_FILTER, 0x{body[0]:02X});',
                f'  vm.ds_write(DS_SEARCH_JUMP, 0x{tgt:04X});',
                f'  vm.pc = 0x{pc + 4:04X};  // post-operand position for the loop',
                '  v2_vm_collision_search_loop(vm, 0xFFFE); }']
    # --- channel ops ---
    if op == 0x48:   # vel_to: [m][chX][chY]
        m = body[0]
        ca, cb = m & 7, (m >> 3) & 7
        if ca > 5 or cb > 5:
            return None
        L.append('{ uint16_t _t3 = 0; (void)_t3;')
        o = 1
        used = _ch_get(L, '_x', ca, body, o)
        if used is None: return None
        o += used
        L.append('  { uint16_t _o = vm.global_r(DS_CUR_OBJ);')
        L.append('    ObjRef{vm, _o}.w16(OBJ_VEL_X, (uint16_t)(_x - ObjRef{vm, _o}.u16(OBJ_WORLD_X))); }')
        used = _ch_get(L, '_y', cb, body, o)
        if used is None: return None
        L.append('  { uint16_t _o = vm.global_r(DS_CUR_OBJ);')
        L.append('    ObjRef{vm, _o}.w16(OBJ_VEL_Y, (uint16_t)(_y - ObjRef{vm, _o}.u16(OBJ_WORLD_Y)));')
        L.append('    ObjRef{vm, _o}.w16(OBJ_ANIM_TABLE, 0x100); }   // MOV [si+141D],100h')
        L.append('}')
        L.append(f'vm.pc = 0x{nxt:04X};')
        return L
    if op in (0x15, 0x16, 0x34):
        # delta writers: compute rel x/y per body, then setter pair.
        m = body[0]
        sa, sb = m & 7, (m >> 3) & 7
        probe = []
        u1 = _ch_set(probe, sa, body, 1, '0')
        if u1 is None: return None
        u2 = _ch_set(probe, sb, body, 1 + u1, '0')
        if u2 is None: return None
        L.append('{ uint16_t _di = vm.global_r(DS_CUR_OBJ);')
        if op == 0x15:
            L.append('  uint16_t _ss = vm.ds_read(DS_ACTIVE_VIKING);')
        elif op == 0x16:
            L.append('  uint16_t _ss = ObjRef{vm, _di}.u16(OBJ_PARTNER);')
        else:   # 0x34: nearest-viking scan (task #94 exact semantics)
            L.append('  uint16_t _bd = 0xFFFF;')
            L.append('  for (uint16_t _si = 0; _si < 6; _si += 2) {')
            L.append('      if (ObjRef{vm, _si}.u16(OBJ_RES_HANDLE) == 0) continue;')
            L.append('      int16_t _dx = (int16_t)(ObjRef{vm, _di}.u16(OBJ_WORLD_X) - ObjRef{vm, _si}.u16(OBJ_WORLD_X));')
            L.append('      if (_dx < 0) _dx = -_dx;')
            L.append('      int16_t _dy = (int16_t)(ObjRef{vm, _di}.u16(OBJ_WORLD_Y) - ObjRef{vm, _si}.u16(OBJ_WORLD_Y));')
            L.append('      if (_dy < 0) _dy = -_dy;')
            L.append('      uint16_t _d = (uint16_t)((uint16_t)_dx + (uint16_t)_dy);')
            L.append('      if (_d < _bd) { _bd = _d; vm.ds_write(DS_SEARCH_BEST, _si); }')
            L.append('  }')
            L.append('  uint16_t _ss = vm.ds_read(DS_SEARCH_BEST);')
        L.append('  int16_t _rx;')
        L.append('  if (ObjRef{vm, _di}.u16(OBJ_FLAGS) & 0x40)')
        L.append('      _rx = (int16_t)(ObjRef{vm, _di}.u16(OBJ_WORLD_X) - ObjRef{vm, _ss}.u16(OBJ_WORLD_X));')
        L.append('  else')
        L.append('      _rx = (int16_t)(ObjRef{vm, _ss}.u16(OBJ_WORLD_X) - ObjRef{vm, _di}.u16(OBJ_WORLD_X));')
        L.append('  vm.ds_write(DS_TEXT_COL, (uint16_t)_rx);')
        L.append('  int16_t _ry;')
        L.append('  if (ObjRef{vm, _di}.u16(OBJ_FLAGS) & 0x80)')
        L.append('      _ry = (int16_t)(ObjRef{vm, _di}.u16(OBJ_WORLD_Y) - ObjRef{vm, _ss}.u16(OBJ_WORLD_Y));')
        L.append('  else')
        L.append('      _ry = (int16_t)(ObjRef{vm, _ss}.u16(OBJ_WORLD_Y) - ObjRef{vm, _di}.u16(OBJ_WORLD_Y));')
        L.append('  vm.ds_write(DS_TEXT_ROW, (uint16_t)_ry);')
        o = 1
        # 16 re-reads the freshly written 6C/6E for the setters; 15/34
        # pass the local values — copied per body.
        va = 'vm.ds_read(DS_TEXT_COL)' if op == 0x16 else '(uint16_t)_rx'
        vb = 'vm.ds_read(DS_TEXT_ROW)' if op == 0x16 else '(uint16_t)_ry'
        u = _ch_set(L, sa, body, o, va)
        o += u
        _ch_set(L, sb, body, o, vb)
        if op in (0x16, 0x34):
            L.append('  vm.di_track = _di;   // orig: di=[0x42]; setters leave DI')
        L.append('}')
        L.append(f'vm.pc = 0x{nxt:04X};')
        return L
    return None

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
        try:
            inl = inline_wave4(op, body, kind, tgt, pc + ln, pc)
        except Exception:
            inl = None
        if inl is None:
            try:
                inl = inline_wave5(op, body, kind, tgt, pc + ln, pc)
            except Exception:
                inl = None
        if inl is None:
            try:
                inl = inline_wave3(op, body, kind, tgt, pc + ln, pc)
            except Exception:
                inl = None
        if inl is None and kind == 'fall':
            try:
                inl = inline_wave1(op, body, pc + ln)
            except Exception:
                inl = None
        if inl is None and kind == 'br' and tgt is not None:
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
