// v2_vm_gen.h — the part of v2_vm.cpp that the generated executors share.
//
// The transpiled world-script executors (src/sdl/gen/chunk_01cN.gen.inc, the
// anim executors anim_01cN.gen.inc; tools/data/gen_transpile.py) used to be
// #included into v2_vm.cpp. That single translation unit (54 MB of generated
// source) needs more than 7 GB under gcc -O2 — the GitHub runners lost the
// Windows builds to it, and a local gcc build was impossible. They now compile
// per chunk in src/sdl/gen/exec_01cN.cpp, which include this header: the VM
// state (struct V2VM), the accumulator proxy, the trace rings, the executor
// macros (G_PRE/G_POST) and the declarations of every v2_vm.cpp helper the
// generated code calls (v2_vm_gen_decls.gen.inc, kept by
// tools/data/gen_vm_decls.py). Nothing here changes behaviour: the helpers
// merely gained external linkage, the executors are the same instruction
// blocks in piece functions.
#ifndef V2_VM_GEN_H
#define V2_VM_GEN_H
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "render_v2.h"        // v2_m2c_base
#include "v2_ds_layout.h"
#include "v2_gamestate.h"     // v2gs, g_gs_evac, the evac mirror
#include "v2_obj_view.h"      // ObjRef (its u16/w16 are defined in v2_vm.cpp)

extern int v2_dbg_pre_vm_iter;        // game-frame counter (v2_vm.cpp)
extern uint8_t* v2_vm_acc_base;       // the active shadow the accumulator lives in (v2_vm.cpp)

// Shadow of DS segment (animation table + globals): the full 64KB DS segment.
constexpr uint32_t V2_VM_SHADOW_SIZE = 0x10000;

// stage-4: the accumulator lives on the typed carrier. The proxy keeps the
// 159 call sites reading/writing naturally while every store goes
// member+image (write-through) via the mirror. acc_base may point at a
// fn-test shadow — evac_on() is false there and the flat path is used.
struct V2AccProxy {
    // FLIPPED (wave 9): reads come from the carrier member; the fn-test
    // shadows are not canonical and keep the flat path.
    operator uint16_t() const {
        const uint8_t* b = v2_vm_acc_base;
        if (v2_gs_evac_on(b)) {
            uint16_t img = *(const uint16_t*)(b + DS_ACCUMULATOR);
            if (g_gs_evac.accumulator != img)
                v2_gs_evac_read_desync("accumulator", DS_ACCUMULATOR, g_gs_evac.accumulator, img);
            return g_gs_evac.accumulator;
        }
        return *(const uint16_t*)(b + DS_ACCUMULATOR);
    }
    V2AccProxy& operator=(uint16_t v) {
        uint8_t* b = v2_vm_acc_base;
        if (v2_gs_evac_on(b)) g_gs_evac.accumulator = v;
        *(uint16_t*)(b + DS_ACCUMULATOR) = v;
        return *this;
    }
    V2AccProxy& operator&=(uint16_t v)  { return *this = (uint16_t)((uint16_t)*this & v); }
    V2AccProxy& operator<<=(int sh)     { return *this = (uint16_t)((uint16_t)*this << sh); }
};
extern V2AccProxy v2_vm_accumulator;

struct V2VMTraceEntry {
    uint8_t  opcode;
    uint16_t pc_before;  // PC before opcode dispatch (after reading opcode byte)
    uint16_t pc_after;   // PC after opcode execution
    uint16_t acc_before; // accumulator BEFORE opcode
    uint16_t acc_after;  // accumulator AFTER opcode
    uint16_t es_seg;     // es segment value at this opcode
};
constexpr int V2_VM_TRACE_MAX = 512;
extern V2VMTraceEntry v2_vm_trace[128][V2_VM_TRACE_MAX]; // per object slot
extern int v2_vm_trace_count[128];
extern uint16_t v2_trace_object;      // V2_TRACE_OBJECT=<hex>: G_POST prints that object's steps

extern int v2_v2_anim_cmd_count;      // incremented by v2's anim cmd loop
extern uint64_t v2_op_anim_count[32]; // anim cmds (cmd <= 0x1A)
// Debug ring buffer for anim cmd trace (last N commands before error)
struct AnimCmdTrace {
    uint8_t cmd;
    uint16_t handler;
    uint16_t bx_before;
    uint16_t bx_after;
};
extern AnimCmdTrace v2_anim_trace[16];
extern int v2_anim_trace_idx;

// ============================================================================
// VM State
// ============================================================================
struct V2VM {
    uint8_t* ds;          // Real DS segment (for reading data outside shadow range)
    uint8_t* shadow;      // Shadow DS (for reads/writes within animation table)
    uint8_t* es;          // Current code segment pointer
    uint8_t* cs_base;     // CS segment base (seg000 = m2c_base + 0x1A20)
    uint16_t obj;         // Current object index
    uint16_t pc;          // Bytecode pointer
    bool running;         // False when opcode 0x00 yields
    bool carry;           // Carry flag (set by animation load functions)
    int slot;             // Object slot index (obj / 2)
    // Shadow SI register (task #15): NOT carried across opcodes — the orig
    // dispatcher rewrites si = opcode*2 at every fetch (loc_142a6). Handlers'
    // local si writers (channel getters, 12515 tail) update it; the op
    // 41/44/45 ch6/7 escapes store it into [bx+0x1DA9].
    uint16_t si_track = 0;
    // Shadow DI register (task #15): mirrors the 8086 DI value the orig VM
    // carries across opcodes/objects. Written by the deterministic DI writers
    // (ch3 field addressing, search scans, per-opcode MOV di sites) and read
    // by the ch6/7 escape paths of op 41/44/45, which store the STALE di into
    // the command buffer word [bx+0x1DAB] (the orig continues through the
    // wrapper writes with whatever di held at escape time).
    uint16_t di_track = 0;
    // DX clobber model for the ch4 getter (sub_12312 Path1, divergence №40):
    // MOV edx,15A4E35h (0x231E) + MUL edx (0x2324) leave EDX = high32 of
    // seed*0x15A4E35 — the real 386 destroys DX inside the RNG fetch. Opcode
    // bodies that keep a value in DX across a later dispatch (op27 X @0x4F11,
    // op29 Y @0x5025) must replace it with ch4_mul_dx when the LAST dispatch
    // took the ch4 Path1 route. Path2 (LFSR, [0x42C]!=0) clobbers only AX.
    // (EAX's high half also survives as low16(new_seed) after ROR — no 16-bit
    // VM path ever reads it, so it is not modelled.)
    // The dispatcher clears the flag before every dispatch.
    bool     ch4_mul_clobber = false;
    uint16_t ch4_mul_dx = 0;

    // Bytecode read helpers
    uint8_t  read_u8()  { uint8_t  v = es[pc]; pc += 1; return v; }
    uint16_t read_u16() { uint16_t v = *(uint16_t*)(es + pc); pc += 2; return v; }

    // DS read: use shadow if within range, otherwise real DS
    uint16_t ds_read(uint16_t addr) {
        if (addr < V2_VM_SHADOW_SIZE - 1)
            return *(uint16_t*)(shadow + addr);
        return *(uint16_t*)(ds + addr);
    }

    // DS write: write to shadow if within range (never write to real DS)
    void ds_write(uint16_t addr, uint16_t val) {
        if (addr < V2_VM_SHADOW_SIZE - 1) {
            // V2_NEXTLVL_TRACE=1 (#104 progression): log every VM write into
            // DS_LEVEL_LOAD with the executing object + bytecode pc — the
            // "who sets the next level" forensics channel.
            if (addr == DS_LEVEL_LOAD) {
                static int trace = -1;
                if (trace < 0) { const char* e = getenv("V2_NEXTLVL_TRACE"); trace = (e && e[0]=='1') ? 1 : 0; }
                if (trace) {
                    // v2_dbg_pre_vm_iter: file-scope extern (top of file)
                    fprintf(stderr, "V2-NEXTLVL: f%d obj=%02X pc=%04X val=%04X (level=%04X)\n",
                            v2_dbg_pre_vm_iter, obj, pc,
                            val, *(uint16_t*)(shadow + DS_LEVEL));
                }
            }
            // stage-4 II.c: the operand write path mirrors evacuated fields
            // (interpreter bodies/helpers reach them with computed addresses).
            v2_gs_evac_mirror_w(shadow, addr, val);
            *(uint16_t*)(shadow + addr) = val;
        }
    }

    // DS byte write
    void ds_write_b(uint16_t addr, uint8_t val) {
        if (addr < V2_VM_SHADOW_SIZE) {
            // stage-4 II.c: byte operand path mirrors evacuated fields too
            v2_gs_evac_mirror_b(shadow, addr, val);
            shadow[addr] = val;
        }
    }

    // Field access for current object
    uint16_t field_r(uint16_t offset) { return ds_read(obj + offset); }
    void     field_w(uint16_t offset, uint16_t val) { ds_write(obj + offset, val); }

    // Global access
    uint16_t global_r(uint16_t offset) { return ds_read(offset); }
    void     global_w(uint16_t offset, uint16_t val) { ds_write(offset, val); }
};

// v2_vm.cpp helpers the generated code calls (external linkage; the list is
// derived from the generated files by tools/data/gen_vm_decls.py).
#include "gen/v2_vm_gen_decls.gen.inc"

// Entry points of the per-chunk translation units (src/sdl/gen/exec_01cN.cpp).
bool v2_gen_exec_1c1_entry(V2VM& vm, int& max_ops);
bool v2_gen_exec_1c2_entry(V2VM& vm, int& max_ops);
bool v2_gen_exec_1c3_entry(V2VM& vm, int& max_ops);
bool v2_gen_exec_1c4_entry(V2VM& vm, int& max_ops);
bool v2_gen_exec_1c5_entry(V2VM& vm, int& max_ops);
bool v2_gen_exec_1c6_entry(V2VM& vm, int& max_ops);
bool v2_gen_anim_1c1_entry(V2VM& vm, uint16_t& anim_bx, int& max);
bool v2_gen_anim_1c2_entry(V2VM& vm, uint16_t& anim_bx, int& max);
bool v2_gen_anim_1c3_entry(V2VM& vm, uint16_t& anim_bx, int& max);
bool v2_gen_anim_1c4_entry(V2VM& vm, uint16_t& anim_bx, int& max);
bool v2_gen_anim_1c5_entry(V2VM& vm, uint16_t& anim_bx, int& max);
bool v2_gen_anim_1c6_entry(V2VM& vm, uint16_t& anim_bx, int& max);

#ifdef V2_GENCODE
// The executor macros: every generated instruction is G_PRE(pc, op) ... G_POST(pc, op).
#ifndef V2_ONLY
#define V2_GEN_HASHSNAP \
        const uint32_t _h1 = v2_ds_hash(vm.shadow); \
        const uint32_t _h2 = v2_obj_hash(vm.shadow, vm.obj);
#define V2_GEN_TRACES(PC, OP, PCB, ACCB) \
        { extern void v2_vm_trace_record_v2_ext(uint16_t, uint16_t, uint8_t, \
              uint16_t, uint16_t, uint16_t, uint16_t, uint8_t*, uint32_t, uint32_t); \
          extern uint16_t v2_vm_step_per_obj[128]; \
          v2_vm_trace_record_v2_ext(vm.obj, v2_vm_step_per_obj[vm.slot]++, (OP), \
                                    (PCB), vm.pc, (ACCB), v2_vm_accumulator, \
                                    vm.shadow, _h1, _h2); } \
        { int& _cnt = v2_vm_trace_count[vm.slot]; \
          if (_cnt < V2_VM_TRACE_MAX) { \
              v2_vm_trace[vm.slot][_cnt].opcode = (OP); \
              v2_vm_trace[vm.slot][_cnt].pc_before = (uint16_t)((PCB) + 1); \
              v2_vm_trace[vm.slot][_cnt].pc_after = vm.pc; \
              v2_vm_trace[vm.slot][_cnt].acc_before = (ACCB); \
              v2_vm_trace[vm.slot][_cnt].acc_after = v2_vm_accumulator; \
              v2_vm_trace[vm.slot][_cnt].es_seg = \
                  (uint16_t)((vm.es - v2_m2c_base) >> 4); \
              _cnt++; } }
#else
#define V2_GEN_HASHSNAP
#define V2_GEN_TRACES(PC, OP, PCB, ACCB) \
        { extern uint16_t v2_vm_step_per_obj[128]; \
          v2_vm_step_per_obj[vm.slot]++; } \
        { int& _cnt = v2_vm_trace_count[vm.slot]; \
          if (_cnt < V2_VM_TRACE_MAX) { \
              v2_vm_trace[vm.slot][_cnt].opcode = (OP); \
              v2_vm_trace[vm.slot][_cnt].pc_before = (uint16_t)((PCB) + 1); \
              v2_vm_trace[vm.slot][_cnt].pc_after = vm.pc; \
              v2_vm_trace[vm.slot][_cnt].acc_before = (ACCB); \
              v2_vm_trace[vm.slot][_cnt].acc_after = v2_vm_accumulator; \
              v2_vm_trace[vm.slot][_cnt].es_seg = \
                  (uint16_t)((vm.es - v2_m2c_base) >> 4); \
              _cnt++; } }
#endif
#define G_PRE(PC, OP) \
        if (--max_ops < 0) return true; \
        { const uint16_t _pcb = (PC); \
          const uint16_t _accb = v2_vm_accumulator; \
          V2_GEN_HASHSNAP \
          v2_pcdump_note(vm.shadow, (PC), (OP)); \
          vm.pc = (uint16_t)((PC) + 1); \
          vm.si_track = (uint16_t)((OP) << 1);
#define G_POST(PC, OP) \
          V2_GEN_TRACES(PC, OP, _pcb, _accb) \
          if (vm.obj == v2_trace_object) \
              fprintf(stderr, "V2-OBJ-TRACE: obj=%02X op=%02X pc=%04X\xe2\x86\x92%04X " \
                      "acc=%04X\xe2\x86\x92%04X es=%04X\n", vm.obj, (OP), _pcb, vm.pc, \
                      _accb, v2_vm_accumulator, \
                      *(uint16_t*)(vm.shadow + vm.obj + OBJ_CODE_SEG)); \
          if (!vm.running) return true; }
#endif // V2_GENCODE

#endif // V2_VM_GEN_H
