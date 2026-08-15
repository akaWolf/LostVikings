#!/usr/bin/env python3
"""Stage 1.2 (ENGINE_REWRITE_ROADMAP): opcode-table draft generator.

Parses src/sdl/v2_vm.cpp (the semantic source of truth for all 216 VM
opcodes) and emits tools/data/optable_draft.json:
  op -> { handler, comment, pc_adds (fixed operand widths seen),
          pc_sets (jump-class writes to pc), exits (vm.running=false),
          operand_reads (vm.es+vm.pc fetches) }

This is a DRAFT for the disassembler: simple fixed-length ops are fully
described by pc_adds; jump/variable ops get hand-written entries in the
disassembler itself, validated by walking all real scripts (chunks
0x1C1..0x1C6) — any width mismatch derails the walk into garbage opcodes
immediately, so the table is self-validating against the real data.
"""
import re, json, sys

def main():
    src = open('src/sdl/v2_vm.cpp').read()
    assigns = {}
    for m in re.finditer(r'v2_vm_optable\[0x([0-9A-Fa-f]{2})\]\s*=\s*'
                         r'([a-zA-Z_0-9]+);(?:\s*//\s*(.*))?', src):
        assigns[int(m.group(1), 16)] = (m.group(2), (m.group(3) or '').strip())
    bodies = {}
    for m in re.finditer(r'static void (v2_vm_op_[A-Za-z_0-9]+)\(V2VM& vm\)\s*\{', src):
        name = m.group(1); i = m.end(); depth = 1
        while depth and i < len(src):
            c = src[i]
            if c == '{': depth += 1
            elif c == '}': depth -= 1
            i += 1
        bodies[name] = src[m.end():i-1]
    out = {}
    for op, (h, comment) in sorted(assigns.items()):
        b = bodies.get(h, '')
        out[f'{op:02X}'] = {
            'handler': h,
            'comment': comment,
            'pc_adds': sorted(set(int(x) for x in re.findall(r'vm\.pc\s*\+=\s*(\d+)', b))),
            'pc_sets': len(re.findall(r'vm\.pc\s*=\s*[^+=]', b)),
            'exits': ('vm.running = false' in b),
            'operand_reads': len(re.findall(r'vm\.es\s*\+\s*vm\.pc', b)),
            'do_jump': len(re.findall(r'v2_vm_do_jump\(vm\)', b)),
            'do_call_jump': len(re.findall(r'v2_vm_do_call_jump\(vm\)', b)),
            # operand fetches advance pc INSIDE these helpers/methods —
            # invisible to pc_adds, must be counted separately:
            'op_bytes': (2*len(re.findall(r'read_u16\(\)|v2_vm_read_literal\(vm\)|v2_vm_read_indirect\(vm\)', b))
                        +1*len(re.findall(r'read_u8\(\)|v2_vm_read_indexed_field\(vm\)|v2_vm_read_indexed_field_1995\(vm\)', b))),
            'body_found': h in bodies,
        }
    json.dump(out, open('tools/data/optable_draft.json', 'w'), indent=1)
    n_missing = sum(1 for v in out.values() if not v['body_found'])
    print(f"ops={len(out)} missing_bodies={n_missing}")
    return 0 if n_missing == 0 else 1

if __name__ == '__main__':
    sys.exit(main())
