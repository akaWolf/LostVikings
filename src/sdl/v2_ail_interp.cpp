// v2_ail_interp.cpp — 8086 real-mode interpreter for the AIL .ADV driver blob
// (task #61, "interpreter as oracle" architecture).
//
// The OPL synthesis code of the game is NOT in seg002 (that is only the AIL
// timer/dispatcher layer) — it lives in a .ADV driver blob loaded AS DATA
// from DATA.DAT (chunk 0x1C7 + [86B6]; the user's config selects dev5 =
// SBPFM.ADV, "Creative Labs Sound Blaster Pro FM", dual OPL2 at base+0..3
// with the mixer at base+4/5 — ports 0x220-0x225, NOT 0x388).
//
// Instead of hand-porting ~4.8K instructions, this module EXECUTES the blob
// with a small 8086 interpreter: semantics are correct by construction, all
// nine .ADV drivers work for free, and the interpreter doubles as the oracle
// for a later (optional) C replica via synthetic-diff on the OPL register
// trace + segment state.
//
// Blob properties (verified by disassembly survey, memory
// project-v2-native-ail-adv-blob):
//   * no INT instructions, no self-modifying CODE (cs-writes hit data cells);
//   * the only external call is `call far [cs:0x2957]` (the AIL timer
//     callback pointer installed by fn64) — the interpreter routes it to a
//     host hook;
//   * mnemonic census over the 0x1380..0x3F86 code range: 59 distinct ops
//     (mov/push/pop/add/cmp/call/jmp/jcc family, or/and/xor, inc/dec,
//     shl/shr/sar/rcl/rcr, adc/sbb, mul/imul/div, lds/les/lea, pushf/popf,
//     cli/sti/cld, in/out/outsb/outsw, rep/repe/repne movs/stos/cmps, xchg,
//     test, not/neg, cbw/cwd, loop/jcxz, retf/ret) — jpe means the parity
//     flag must be real.
//
// Activation: like v2_native_opl.cpp this is inert unless V2_NATIVE_AIL=1.

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>

// ---------------------------------------------------------------------------
// Machine state
// ---------------------------------------------------------------------------
// The driver is a single segment (16262 bytes for SBPFM.ADV) addressed
// cs-relative; it reads/writes its own data cells and touches the timbre
// bank through lds/les far pointers. The interpreter therefore models a small
// flat memory of 8086 segments:
//   seg[AIL_SEG_DRV]   — working copy of the blob (code + mutable data)
//   seg[AIL_SEG_BANK]  — timbre bank chunk
//   seg[AIL_SEG_STACK] — private stack (the real driver runs on the caller's)
// Far pointers outside these segments trap (they would mean a survey gap).

struct AilRegs {
    uint16_t ax = 0, bx = 0, cx = 0, dx = 0;
    uint16_t si = 0, di = 0, bp = 0, sp = 0;
    uint16_t cs = 0, ds = 0, es = 0, ss = 0;
    uint16_t ip = 0;
    // Flags kept unpacked for speed and clarity.
    bool cf = false, zf = false, sf = false, of = false;
    bool af = false, pf = false, df = false, if_ = true;
};

class AilInterp {
public:
    // Segment registry: paragraph -> host pointer + limit. The blob segment
    // value is arbitrary (we pick stable fake paragraphs); what matters is
    // that far pointers the driver stores and reloads round-trip exactly.
    struct Seg { uint16_t para; uint8_t* mem; uint32_t size; };

    static constexpr uint16_t DRV_PARA   = 0x2000;  // fake paragraph of the blob
    static constexpr uint16_t BANK_PARA  = 0x3000;  // timbre bank
    static constexpr uint16_t STACK_PARA = 0x4000;  // private stack segment
    static constexpr uint32_t STACK_SIZE = 0x1000;

    AilRegs r;
    uint8_t* drv = nullptr;      uint32_t drv_size = 0;
    uint8_t* bank = nullptr;     uint32_t bank_size = 0;
    uint8_t  stack_mem[STACK_SIZE] = {0};

    // Host hooks -------------------------------------------------------------
    // OPL/mixer port I/O. The SBPFM driver computes dx = base + 2*bh for the
    // two OPL2 chips (0x220/0x221 left, 0x222/0x223 right) and base+4/5 for
    // the SBPro mixer. IN from the data/status ports feeds the busy-wait
    // delays and the fn65 detect probe.
    void (*out_hook)(uint16_t port, uint8_t val) = nullptr;
    uint8_t (*in_hook)(uint16_t port) = nullptr;
    // `call far [cs:0x2957]` — AIL timer-callback pointer cell. The driver
    // never fabricates far code pointers elsewhere (survey), so this is the
    // single far-call escape hatch.
    void (*ail_callback_hook)() = nullptr;
    uint16_t callback_ptr_off = 0x2957;   // dword cell inside the blob

    bool trace = false;          // V2_AIL_TRACE=1 — per-instruction log
    int  fault = 0;              // non-zero: interpreter hit something unmodeled
    char fault_msg[128] = {0};

    // ------------------------------------------------------------------ setup
    void load(const uint8_t* blob, uint32_t blob_size,
              const uint8_t* bank_data, uint32_t bank_sz) {
        static uint8_t drv_copy[0x8000];
        static uint8_t bank_copy[0x4000];
        if (blob_size > sizeof(drv_copy) || bank_sz > sizeof(bank_copy)) {
            fail("blob/bank larger than the interpreter arenas");
            return;
        }
        memcpy(drv_copy, blob, blob_size);
        memcpy(bank_copy, bank_data, bank_sz);
        drv = drv_copy;   drv_size = blob_size;
        bank = bank_copy; bank_size = bank_sz;
    }

    // Resolve a far address to host memory. Traps on unknown segments —
    // any hit here is a survey gap that must be modeled, never guessed.
    uint8_t* mem(uint16_t seg, uint16_t off, uint32_t len) {
        if (seg == DRV_PARA) {
            if ((uint32_t)off + len <= 0x10000) return drv + off;  // blob addresses wrap in 64K like real DS
        }
        if (seg == BANK_PARA)  { return bank + off; }
        if (seg == STACK_PARA) { return stack_mem + (off % STACK_SIZE); }
        fail("far access to unmodeled segment %04X:%04X", seg, off);
        static uint8_t sink[4] = {0};
        return sink;
    }

    uint8_t  rd8 (uint16_t seg, uint16_t off) { return *mem(seg, off, 1); }
    uint16_t rd16(uint16_t seg, uint16_t off) { return (uint16_t)(rd8(seg, off) | (rd8(seg, (uint16_t)(off + 1)) << 8)); }
    void wr8 (uint16_t seg, uint16_t off, uint8_t v)  { *mem(seg, off, 1) = v; }
    void wr16(uint16_t seg, uint16_t off, uint16_t v) { wr8(seg, off, (uint8_t)v); wr8(seg, (uint16_t)(off + 1), (uint8_t)(v >> 8)); }

    void push(uint16_t v) { r.sp = (uint16_t)(r.sp - 2); wr16(r.ss, r.sp, v); }
    uint16_t pop() { uint16_t v = rd16(r.ss, r.sp); r.sp = (uint16_t)(r.sp + 2); return v; }

    // ------------------------------------------------------------------ flags
    static bool parity8(uint8_t v) { return __builtin_parity(v) == 0; }  // PF set on EVEN population

    void flags_logic16(uint16_t res) { r.cf = false; r.of = false; set_szp16(res); }
    void flags_logic8 (uint8_t  res) { r.cf = false; r.of = false; set_szp8(res); }
    void set_szp16(uint16_t res) { r.zf = res == 0; r.sf = (res & 0x8000) != 0; r.pf = parity8((uint8_t)res); }
    void set_szp8 (uint8_t  res) { r.zf = res == 0; r.sf = (res & 0x80)   != 0; r.pf = parity8(res); }

    uint16_t add16(uint16_t a, uint16_t b, bool carry_in = false) {
        uint32_t c = carry_in ? 1u : 0u;
        uint32_t res = (uint32_t)a + b + c;
        r.cf = res > 0xFFFF;
        r.af = ((a ^ b ^ res) & 0x10) != 0;
        r.of = (~(a ^ b) & (a ^ res) & 0x8000) != 0;
        set_szp16((uint16_t)res);
        return (uint16_t)res;
    }
    uint8_t add8(uint8_t a, uint8_t b, bool carry_in = false) {
        uint16_t c = carry_in ? 1 : 0;
        uint16_t res = (uint16_t)a + b + c;
        r.cf = res > 0xFF;
        r.af = ((a ^ b ^ res) & 0x10) != 0;
        r.of = (~(a ^ b) & (a ^ res) & 0x80) != 0;
        set_szp8((uint8_t)res);
        return (uint8_t)res;
    }
    uint16_t sub16(uint16_t a, uint16_t b, bool borrow_in = false) {
        uint32_t c = borrow_in ? 1u : 0u;
        uint32_t res = (uint32_t)a - b - c;
        r.cf = (uint32_t)a < (uint32_t)b + c;
        r.af = ((a ^ b ^ res) & 0x10) != 0;
        r.of = ((a ^ b) & (a ^ res) & 0x8000) != 0;
        set_szp16((uint16_t)res);
        return (uint16_t)res;
    }
    uint8_t sub8(uint8_t a, uint8_t b, bool borrow_in = false) {
        uint16_t c = borrow_in ? 1 : 0;
        uint16_t res = (uint16_t)a - b - c;
        r.cf = (uint16_t)a < (uint16_t)b + c;
        r.af = ((a ^ b ^ res) & 0x10) != 0;
        r.of = ((a ^ b) & (a ^ res) & 0x80) != 0;
        set_szp8((uint8_t)res);
        return (uint8_t)res;
    }

    uint16_t pack_flags() const {
        return (uint16_t)((r.cf ? 1 : 0) | 0x0002 | (r.pf ? 4 : 0) | (r.af ? 0x10 : 0) |
                          (r.zf ? 0x40 : 0) | (r.sf ? 0x80 : 0) | (r.if_ ? 0x200 : 0) |
                          (r.df ? 0x400 : 0) | (r.of ? 0x800 : 0) | 0xF000);
    }
    void unpack_flags(uint16_t f) {
        r.cf = f & 1; r.pf = f & 4; r.af = f & 0x10; r.zf = f & 0x40;
        r.sf = f & 0x80; r.if_ = f & 0x200; r.df = f & 0x400; r.of = f & 0x800;
    }

    // ------------------------------------------------------------- reg access
    uint16_t& reg16(int idx) {
        switch (idx) {
            case 0: return r.ax; case 1: return r.cx; case 2: return r.dx; case 3: return r.bx;
            case 4: return r.sp; case 5: return r.bp; case 6: return r.si; default: return r.di;
        }
    }
    uint8_t get_reg8(int idx) {
        uint16_t& w = reg16(idx & 3);
        return (idx & 4) ? (uint8_t)(w >> 8) : (uint8_t)w;
    }
    void set_reg8(int idx, uint8_t v) {
        uint16_t& w = reg16(idx & 3);
        if (idx & 4) w = (uint16_t)((w & 0x00FF) | (v << 8));
        else         w = (uint16_t)((w & 0xFF00) | v);
    }
    uint16_t& sreg(int idx) {
        switch (idx) { case 0: return r.es; case 1: return r.cs; case 2: return r.ss; default: return r.ds; }
    }

    // ------------------------------------------------------------------ modrm
    // Decoded effective address for the current instruction.
    struct EA {
        bool is_reg;
        int  reg;          // when is_reg
        uint16_t seg, off; // when memory
        int  reg_field;    // the /r middle field
    };

    int seg_override = -1;   // -1 none, else sreg index

    uint8_t fetch8()  { uint8_t v = rd8(r.cs, r.ip); r.ip = (uint16_t)(r.ip + 1); return v; }
    uint16_t fetch16() { uint16_t v = rd16(r.cs, r.ip); r.ip = (uint16_t)(r.ip + 2); return v; }

    EA modrm() {
        uint8_t m = fetch8();
        EA ea;
        ea.reg_field = (m >> 3) & 7;
        int mod = m >> 6, rm = m & 7;
        if (mod == 3) { ea.is_reg = true; ea.reg = rm; ea.seg = 0; ea.off = 0; return ea; }
        ea.is_reg = false;
        uint16_t disp = 0;
        if (mod == 1) disp = (uint16_t)(int16_t)(int8_t)fetch8();
        else if (mod == 2) disp = fetch16();
        uint16_t base = 0;
        int default_seg = 3;  // ds
        switch (rm) {
            case 0: base = (uint16_t)(r.bx + r.si); break;
            case 1: base = (uint16_t)(r.bx + r.di); break;
            case 2: base = (uint16_t)(r.bp + r.si); default_seg = 2; break;
            case 3: base = (uint16_t)(r.bp + r.di); default_seg = 2; break;
            case 4: base = r.si; break;
            case 5: base = r.di; break;
            case 6:
                if (mod == 0) { base = 0; disp = fetch16(); }
                else { base = r.bp; default_seg = 2; }
                break;
            default: base = r.bx; break;
        }
        ea.off = (uint16_t)(base + disp);
        ea.seg = sreg(seg_override >= 0 ? seg_override : default_seg);
        return ea;
    }

    uint16_t ea_rd16(const EA& ea) { return ea.is_reg ? reg16(ea.reg) : rd16(ea.seg, ea.off); }
    void     ea_wr16(const EA& ea, uint16_t v) { if (ea.is_reg) reg16(ea.reg) = v; else wr16(ea.seg, ea.off, v); }
    uint8_t  ea_rd8 (const EA& ea) { return ea.is_reg ? get_reg8(ea.reg) : rd8(ea.seg, ea.off); }
    void     ea_wr8 (const EA& ea, uint8_t v) { if (ea.is_reg) set_reg8(ea.reg, v); else wr8(ea.seg, ea.off, v); }

    // ------------------------------------------------------------------ fault
    void fail(const char* fmt, ...) {
        if (fault) return;
        fault = 1;
        va_list ap; va_start(ap, fmt);
        vsnprintf(fault_msg, sizeof(fault_msg), fmt, ap);
        va_end(ap);
        fprintf(stderr, "AIL-INTERP FAULT @%04X:%04X: %s\n", r.cs, r.ip, fault_msg);
    }

    // ------------------------------------------------------------- entrypoint
    // Call a driver function the way seg002's sub_1bec2 dispatcher does:
    // caller pushes args right-to-left, then does `call far` into the fn
    // handler found in the blob's fn table. We emulate the far call frame and
    // run until the matching RETF pops our sentinel.
    static constexpr uint16_t SENTINEL_CS = 0xFFFF;
    static constexpr uint16_t SENTINEL_IP = 0xFFFF;

    uint16_t call_fn(uint16_t fn_off, const uint16_t* args, int argc) {
        r.cs = DRV_PARA; r.ds = DRV_PARA; r.es = DRV_PARA;
        r.ss = STACK_PARA; r.sp = (uint16_t)(STACK_SIZE - 2);
        for (int i = argc - 1; i >= 0; i--) push(args[i]);
        push(SENTINEL_CS); push(SENTINEL_IP);   // far return frame
        r.ip = fn_off;
        run();
        return r.ax;   // AIL fns return in AX (dx:ax for far values)
    }

    void run(int max_insns = 2000000) {
        while (!fault && max_insns-- > 0) {
            if (r.cs == SENTINEL_CS && r.ip == SENTINEL_IP) return;  // fn returned
            step();
        }
        if (!fault && r.cs != SENTINEL_CS) fail("instruction budget exhausted");
    }

    void step();   // the opcode dispatcher — defined below
};

// ---------------------------------------------------------------------------
// Opcode dispatcher.
//
// Coverage strategy: implement exactly the mnemonic census of the SBPFM code
// range (see header). Anything not in the census traps loudly via fail() —
// per project rules an unimplemented path must never be silently skipped.
// ---------------------------------------------------------------------------
void AilInterp::step() {
    seg_override = -1;
    uint16_t ip0 = r.ip;
    uint8_t op;

prefix:
    op = fetch8();
    switch (op) {
        case 0x26: seg_override = 0; goto prefix;   // es:
        case 0x2E: seg_override = 1; goto prefix;   // cs:
        case 0x36: seg_override = 2; goto prefix;   // ss:
        case 0x3E: seg_override = 3; goto prefix;   // ds:
        default: break;
    }

    if (trace)
        fprintf(stderr, "AIL-T %04X: op=%02X ax=%04X bx=%04X cx=%04X dx=%04X si=%04X di=%04X sp=%04X\n",
                ip0, op, r.ax, r.bx, r.cx, r.dx, r.si, r.di, r.sp);

    switch (op) {
        // ---- MOV -----------------------------------------------------------
        case 0x88: { EA ea = modrm(); ea_wr8(ea, get_reg8(ea.reg_field)); break; }
        case 0x89: { EA ea = modrm(); ea_wr16(ea, reg16(ea.reg_field)); break; }
        case 0x8A: { EA ea = modrm(); set_reg8(ea.reg_field, ea_rd8(ea)); break; }
        case 0x8B: { EA ea = modrm(); reg16(ea.reg_field) = ea_rd16(ea); break; }
        case 0x8C: { EA ea = modrm(); ea_wr16(ea, sreg(ea.reg_field & 3)); break; }
        case 0x8E: { EA ea = modrm(); sreg(ea.reg_field & 3) = ea_rd16(ea); break; }
        case 0xA0: { uint16_t off = fetch16(); r.ax = (uint16_t)((r.ax & 0xFF00) | rd8(sreg(seg_override >= 0 ? seg_override : 3), off)); break; }
        case 0xA1: { uint16_t off = fetch16(); r.ax = rd16(sreg(seg_override >= 0 ? seg_override : 3), off); break; }
        case 0xA2: { uint16_t off = fetch16(); wr8(sreg(seg_override >= 0 ? seg_override : 3), off, (uint8_t)r.ax); break; }
        case 0xA3: { uint16_t off = fetch16(); wr16(sreg(seg_override >= 0 ? seg_override : 3), off, r.ax); break; }
        case 0xB0: case 0xB1: case 0xB2: case 0xB3:
        case 0xB4: case 0xB5: case 0xB6: case 0xB7:
            set_reg8(op - 0xB0, fetch8()); break;
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF:
            reg16(op - 0xB8) = fetch16(); break;
        case 0xC6: { EA ea = modrm(); ea_wr8(ea, fetch8()); break; }
        case 0xC7: { EA ea = modrm(); ea_wr16(ea, fetch16()); break; }

        // ---- PUSH/POP ------------------------------------------------------
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:
            push(reg16(op - 0x50)); break;
        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            reg16(op - 0x58) = pop(); break;
        case 0x06: push(r.es); break;   case 0x07: r.es = pop(); break;
        case 0x0E: push(r.cs); break;
        case 0x16: push(r.ss); break;   case 0x17: r.ss = pop(); break;
        case 0x1E: push(r.ds); break;   case 0x1F: r.ds = pop(); break;
        case 0x9C: push(pack_flags()); break;                       // pushf
        case 0x9D: unpack_flags(pop()); break;                      // popf
        case 0x8F: { EA ea = modrm(); ea_wr16(ea, pop()); break; }  // pop r/m16

        // ---- ALU r/m,r + r,r/m + acc,imm ----------------------------------
        #define ALU_FAMILY(base, op8, op16) \
            case base+0: { EA ea = modrm(); ea_wr8 (ea, op8 (ea_rd8(ea),  get_reg8(ea.reg_field))); break; } \
            case base+1: { EA ea = modrm(); ea_wr16(ea, op16(ea_rd16(ea), reg16(ea.reg_field)));   break; } \
            case base+2: { EA ea = modrm(); set_reg8(ea.reg_field, op8 (get_reg8(ea.reg_field), ea_rd8(ea)));  break; } \
            case base+3: { EA ea = modrm(); reg16(ea.reg_field) =  op16(reg16(ea.reg_field),   ea_rd16(ea)); break; } \
            case base+4: r.ax = (uint16_t)((r.ax & 0xFF00) | op8((uint8_t)r.ax, fetch8())); break; \
            case base+5: r.ax = op16(r.ax, fetch16()); break;

        #define OP_ADD8(a,b)  add8(a,b)
        #define OP_ADD16(a,b) add16(a,b)
        #define OP_ADC8(a,b)  add8(a,b,r.cf)
        #define OP_ADC16(a,b) add16(a,b,r.cf)
        #define OP_SUB8(a,b)  sub8(a,b)
        #define OP_SUB16(a,b) sub16(a,b)
        #define OP_SBB8(a,b)  sub8(a,b,r.cf)
        #define OP_SBB16(a,b) sub16(a,b,r.cf)
        #define OP_OR8(a,b)   ([&]{ uint8_t  v = (uint8_t)(a|b);  flags_logic8(v);  return v; }())
        #define OP_OR16(a,b)  ([&]{ uint16_t v = (uint16_t)(a|b); flags_logic16(v); return v; }())
        #define OP_AND8(a,b)  ([&]{ uint8_t  v = (uint8_t)(a&b);  flags_logic8(v);  return v; }())
        #define OP_AND16(a,b) ([&]{ uint16_t v = (uint16_t)(a&b); flags_logic16(v); return v; }())
        #define OP_XOR8(a,b)  ([&]{ uint8_t  v = (uint8_t)(a^b);  flags_logic8(v);  return v; }())
        #define OP_XOR16(a,b) ([&]{ uint16_t v = (uint16_t)(a^b); flags_logic16(v); return v; }())

        ALU_FAMILY(0x00, OP_ADD8, OP_ADD16)   // add
        ALU_FAMILY(0x08, OP_OR8,  OP_OR16)    // or
        ALU_FAMILY(0x10, OP_ADC8, OP_ADC16)   // adc
        ALU_FAMILY(0x18, OP_SBB8, OP_SBB16)   // sbb
        ALU_FAMILY(0x20, OP_AND8, OP_AND16)   // and
        ALU_FAMILY(0x28, OP_SUB8, OP_SUB16)   // sub
        ALU_FAMILY(0x30, OP_XOR8, OP_XOR16)   // xor

        // cmp — sub without writeback
        case 0x38: { EA ea = modrm(); sub8 (ea_rd8(ea),  get_reg8(ea.reg_field)); break; }
        case 0x39: { EA ea = modrm(); sub16(ea_rd16(ea), reg16(ea.reg_field));   break; }
        case 0x3A: { EA ea = modrm(); sub8 (get_reg8(ea.reg_field), ea_rd8(ea)); break; }
        case 0x3B: { EA ea = modrm(); sub16(reg16(ea.reg_field), ea_rd16(ea));   break; }
        case 0x3C: sub8((uint8_t)r.ax, fetch8()); break;
        case 0x3D: sub16(r.ax, fetch16()); break;

        // test
        case 0x84: { EA ea = modrm(); flags_logic8 ((uint8_t)(ea_rd8(ea)  & get_reg8(ea.reg_field))); break; }
        case 0x85: { EA ea = modrm(); flags_logic16((uint16_t)(ea_rd16(ea) & reg16(ea.reg_field)));   break; }
        case 0xA8: flags_logic8((uint8_t)((uint8_t)r.ax & fetch8())); break;
        case 0xA9: flags_logic16((uint16_t)(r.ax & fetch16())); break;

        // group 0x80/0x81/0x83: ALU r/m, imm
        case 0x80: case 0x81: case 0x83: {
            EA ea = modrm();
            uint16_t imm = (op == 0x81) ? fetch16()
                         : (op == 0x83) ? (uint16_t)(int16_t)(int8_t)fetch8()
                                        : fetch8();
            if (op == 0x80) {
                uint8_t a = ea_rd8(ea), b = (uint8_t)imm, v = 0;
                switch (ea.reg_field) {
                    case 0: v = add8(a, b); break;
                    case 1: v = (uint8_t)(a | b); flags_logic8(v); break;
                    case 2: v = add8(a, b, r.cf); break;
                    case 3: v = sub8(a, b, r.cf); break;
                    case 4: v = (uint8_t)(a & b); flags_logic8(v); break;
                    case 5: v = sub8(a, b); break;
                    case 6: v = (uint8_t)(a ^ b); flags_logic8(v); break;
                    case 7: sub8(a, b); goto g80_done;
                }
                ea_wr8(ea, v);
            g80_done:;
            } else {
                uint16_t a = ea_rd16(ea), b = imm, v = 0;
                switch (ea.reg_field) {
                    case 0: v = add16(a, b); break;
                    case 1: v = (uint16_t)(a | b); flags_logic16(v); break;
                    case 2: v = add16(a, b, r.cf); break;
                    case 3: v = sub16(a, b, r.cf); break;
                    case 4: v = (uint16_t)(a & b); flags_logic16(v); break;
                    case 5: v = sub16(a, b); break;
                    case 6: v = (uint16_t)(a ^ b); flags_logic16(v); break;
                    case 7: sub16(a, b); goto g81_done;
                }
                ea_wr16(ea, v);
            g81_done:;
            }
            break;
        }

        // ---- INC/DEC (CF preserved) ---------------------------------------
        case 0x40: case 0x41: case 0x42: case 0x43:
        case 0x44: case 0x45: case 0x46: case 0x47: {
            bool cf = r.cf; reg16(op - 0x40) = add16(reg16(op - 0x40), 1); r.cf = cf; break;
        }
        case 0x48: case 0x49: case 0x4A: case 0x4B:
        case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
            bool cf = r.cf; reg16(op - 0x48) = sub16(reg16(op - 0x48), 1); r.cf = cf; break;
        }

        // ---- XCHG ----------------------------------------------------------
        case 0x86: { EA ea = modrm(); uint8_t t = ea_rd8(ea); ea_wr8(ea, get_reg8(ea.reg_field)); set_reg8(ea.reg_field, t); break; }
        case 0x87: { EA ea = modrm(); uint16_t t = ea_rd16(ea); ea_wr16(ea, reg16(ea.reg_field)); reg16(ea.reg_field) = t; break; }
        case 0x90: break;   // nop (xchg ax,ax)
        case 0x91: case 0x92: case 0x93: case 0x94:
        case 0x95: case 0x96: case 0x97: {
            uint16_t t = r.ax; r.ax = reg16(op - 0x90); reg16(op - 0x90) = t; break;
        }

        // ---- jumps ---------------------------------------------------------
        case 0xEB: { int8_t d = (int8_t)fetch8(); r.ip = (uint16_t)(r.ip + d); break; }
        case 0xE9: { uint16_t d = fetch16(); r.ip = (uint16_t)(r.ip + d); break; }
        case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75:
        case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B:
        case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
            int8_t d = (int8_t)fetch8();
            bool take = false;
            switch (op & 0x0F) {
                case 0x0: take = r.of; break;                  // jo
                case 0x1: take = !r.of; break;                 // jno
                case 0x2: take = r.cf; break;                  // jc/jb
                case 0x3: take = !r.cf; break;                 // jnc/jae
                case 0x4: take = r.zf; break;                  // jz
                case 0x5: take = !r.zf; break;                 // jnz
                case 0x6: take = r.cf || r.zf; break;          // jbe/jna
                case 0x7: take = !r.cf && !r.zf; break;        // ja
                case 0x8: take = r.sf; break;                  // js
                case 0x9: take = !r.sf; break;                 // jns
                case 0xA: take = r.pf; break;                  // jpe
                case 0xB: take = !r.pf; break;                 // jpo
                case 0xC: take = r.sf != r.of; break;          // jl
                case 0xD: take = r.sf == r.of; break;          // jnl/jge
                case 0xE: take = r.zf || (r.sf != r.of); break; // jle/jng
                case 0xF: take = !r.zf && (r.sf == r.of); break; // jg
            }
            if (take) r.ip = (uint16_t)(r.ip + d);
            break;
        }
        case 0xE2: { int8_t d = (int8_t)fetch8(); if (--r.cx != 0) r.ip = (uint16_t)(r.ip + d); break; }         // loop
        case 0xE1: { int8_t d = (int8_t)fetch8(); if (--r.cx != 0 && r.zf) r.ip = (uint16_t)(r.ip + d); break; } // loope
        case 0xE0: { int8_t d = (int8_t)fetch8(); if (--r.cx != 0 && !r.zf) r.ip = (uint16_t)(r.ip + d); break; }// loopne
        case 0xE3: { int8_t d = (int8_t)fetch8(); if (r.cx == 0) r.ip = (uint16_t)(r.ip + d); break; }           // jcxz

        // ---- call/ret ------------------------------------------------------
        case 0xE8: { uint16_t d = fetch16(); push(r.ip); r.ip = (uint16_t)(r.ip + d); break; }   // call near
        case 0xC3: r.ip = pop(); break;                                                          // ret near
        case 0xC2: { uint16_t n = fetch16(); r.ip = pop(); r.sp = (uint16_t)(r.sp + n); break; } // ret n
        case 0xCB: { r.ip = pop(); r.cs = pop(); break; }                                        // retf
        case 0xCF: { r.ip = pop(); r.cs = pop(); unpack_flags(pop()); break; }                   // iret (timer ISR tail)
        case 0xCA: { uint16_t n = fetch16(); r.ip = pop(); r.cs = pop(); r.sp = (uint16_t)(r.sp + n); break; } // retf n
        case 0x9A: { uint16_t off = fetch16(), seg = fetch16(); push(r.cs); push(r.ip); r.cs = seg; r.ip = off; break; } // call far imm

        // ---- lds/les/lea ---------------------------------------------------
        case 0xC5: { EA ea = modrm(); reg16(ea.reg_field) = rd16(ea.seg, ea.off); r.ds = rd16(ea.seg, (uint16_t)(ea.off + 2)); break; } // lds
        case 0xC4: { EA ea = modrm(); reg16(ea.reg_field) = rd16(ea.seg, ea.off); r.es = rd16(ea.seg, (uint16_t)(ea.off + 2)); break; } // les
        case 0x8D: { EA ea = modrm(); reg16(ea.reg_field) = ea.off; break; }                                                            // lea

        // ---- flags/misc ----------------------------------------------------
        case 0xF8: r.cf = false; break;   // clc
        case 0xF9: r.cf = true; break;    // stc
        case 0xFA: r.if_ = false; break;  // cli
        case 0xFB: r.if_ = true; break;   // sti
        case 0xFC: r.df = false; break;   // cld
        case 0xFD: r.df = true; break;    // std
        case 0x98: r.ax = (uint16_t)(int16_t)(int8_t)r.ax; break;                        // cbw
        case 0x99: r.dx = (r.ax & 0x8000) ? 0xFFFF : 0x0000; break;                      // cwd

        // ---- I/O -----------------------------------------------------------
        case 0xE4: { uint8_t p = fetch8(); r.ax = (uint16_t)((r.ax & 0xFF00) | (in_hook ? in_hook(p) : 0xFF)); break; }
        case 0xEC: r.ax = (uint16_t)((r.ax & 0xFF00) | (in_hook ? in_hook(r.dx) : 0xFF)); break;
        case 0xE6: { uint8_t p = fetch8(); if (out_hook) out_hook(p, (uint8_t)r.ax); break; }
        case 0xEE: if (out_hook) out_hook(r.dx, (uint8_t)r.ax); break;

        // ---- string ops (rep-prefixed forms handled here) ------------------
        case 0xF2: case 0xF3: {   // repne / rep(e)
            bool is_repe = (op == 0xF3);
            uint8_t sop = fetch8();
            while (r.cx != 0) {
                r.cx--;
                switch (sop) {
                    case 0xA4: { wr8(r.es, r.di, rd8(sreg(seg_override >= 0 ? seg_override : 3), r.si));
                                 r.si = (uint16_t)(r.si + (r.df ? -1 : 1)); r.di = (uint16_t)(r.di + (r.df ? -1 : 1)); break; } // movsb
                    case 0xA5: { wr16(r.es, r.di, rd16(sreg(seg_override >= 0 ? seg_override : 3), r.si));
                                 r.si = (uint16_t)(r.si + (r.df ? -2 : 2)); r.di = (uint16_t)(r.di + (r.df ? -2 : 2)); break; } // movsw
                    case 0xAA: { wr8(r.es, r.di, (uint8_t)r.ax); r.di = (uint16_t)(r.di + (r.df ? -1 : 1)); break; }            // stosb
                    case 0xAB: { wr16(r.es, r.di, r.ax); r.di = (uint16_t)(r.di + (r.df ? -2 : 2)); break; }                    // stosw
                    case 0xA6: { sub8(rd8(sreg(seg_override >= 0 ? seg_override : 3), r.si), rd8(r.es, r.di));
                                 r.si = (uint16_t)(r.si + (r.df ? -1 : 1)); r.di = (uint16_t)(r.di + (r.df ? -1 : 1));
                                 if (r.zf != is_repe) goto rep_done; break; }                                                    // cmpsb
                    case 0x6E: { if (out_hook) out_hook(r.dx, rd8(sreg(seg_override >= 0 ? seg_override : 3), r.si));
                                 r.si = (uint16_t)(r.si + (r.df ? -1 : 1)); break; }                                            // outsb
                    case 0x6F: { if (out_hook) { uint16_t v = rd16(sreg(seg_override >= 0 ? seg_override : 3), r.si);
                                 out_hook(r.dx, (uint8_t)v); out_hook(r.dx, (uint8_t)(v >> 8)); }
                                 r.si = (uint16_t)(r.si + (r.df ? -2 : 2)); break; }                                            // outsw
                    default: fail("rep with unmodeled string op %02X", sop); return;
                }
            }
        rep_done:;
            break;
        }
        // bare string ops
        case 0xA4: { wr8(r.es, r.di, rd8(sreg(seg_override >= 0 ? seg_override : 3), r.si));
                     r.si = (uint16_t)(r.si + (r.df ? -1 : 1)); r.di = (uint16_t)(r.di + (r.df ? -1 : 1)); break; }
        case 0xA5: { wr16(r.es, r.di, rd16(sreg(seg_override >= 0 ? seg_override : 3), r.si));
                     r.si = (uint16_t)(r.si + (r.df ? -2 : 2)); r.di = (uint16_t)(r.di + (r.df ? -2 : 2)); break; }
        case 0x6E: { if (out_hook) out_hook(r.dx, rd8(sreg(seg_override >= 0 ? seg_override : 3), r.si));
                     r.si = (uint16_t)(r.si + (r.df ? -1 : 1)); break; }

        // ---- shifts (group 2) ---------------------------------------------
        case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
            EA ea = modrm();
            int count = (op & 2) ? (uint8_t)r.cx & 0x1F : 1;
            bool wide = op & 1;
            uint16_t v = wide ? ea_rd16(ea) : ea_rd8(ea);
            int bits = wide ? 16 : 8;
            uint16_t msb = (uint16_t)(1u << (bits - 1));
            for (int i = 0; i < count; i++) {
                switch (ea.reg_field) {
                    case 0: { bool c = (v & msb) != 0; v = (uint16_t)(((v << 1) | (c ? 1 : 0)) & (wide ? 0xFFFF : 0xFF)); r.cf = c; break; }             // rol
                    case 1: { bool c = v & 1; v = (uint16_t)((v >> 1) | (c ? msb : 0)); r.cf = c; break; }                                               // ror
                    case 2: { bool c = (v & msb) != 0; v = (uint16_t)(((v << 1) | (r.cf ? 1 : 0)) & (wide ? 0xFFFF : 0xFF)); r.cf = c; break; }          // rcl
                    case 3: { bool c = v & 1; v = (uint16_t)((v >> 1) | (r.cf ? msb : 0)); r.cf = c; break; }                                            // rcr
                    case 4: { r.cf = (v & msb) != 0; v = (uint16_t)((v << 1) & (wide ? 0xFFFF : 0xFF)); break; }                                          // shl
                    case 5: { r.cf = v & 1; v = (uint16_t)(v >> 1); break; }                                                                             // shr
                    case 7: { r.cf = v & 1; v = (uint16_t)((v >> 1) | (v & msb)); break; }                                                                // sar
                    default: fail("shift /6 unmodeled"); return;
                }
            }
            if (count && ea.reg_field >= 4) { if (wide) set_szp16(v); else set_szp8((uint8_t)v); }
            if (wide) ea_wr16(ea, v); else ea_wr8(ea, (uint8_t)v);
            break;
        }

        // ---- group 3 (F6/F7): test/not/neg/mul/imul/div --------------------
        case 0xF6: case 0xF7: {
            EA ea = modrm();
            bool wide = op & 1;
            switch (ea.reg_field) {
                case 0:   // test r/m, imm
                    if (wide) flags_logic16((uint16_t)(ea_rd16(ea) & fetch16()));
                    else      flags_logic8 ((uint8_t)(ea_rd8(ea) & fetch8()));
                    break;
                case 2:   // not
                    if (wide) ea_wr16(ea, (uint16_t)~ea_rd16(ea));
                    else      ea_wr8 (ea, (uint8_t)~ea_rd8(ea));
                    break;
                case 3:   // neg
                    if (wide) { uint16_t v = ea_rd16(ea); ea_wr16(ea, sub16(0, v)); }
                    else      { uint8_t  v = ea_rd8(ea);  ea_wr8 (ea, sub8(0, v)); }
                    break;
                case 4:   // mul
                    if (wide) { uint32_t p = (uint32_t)r.ax * ea_rd16(ea); r.ax = (uint16_t)p; r.dx = (uint16_t)(p >> 16);
                                r.cf = r.of = r.dx != 0; }
                    else      { uint16_t p = (uint16_t)((uint8_t)r.ax * ea_rd8(ea)); r.ax = p;
                                r.cf = r.of = (p & 0xFF00) != 0; }
                    break;
                case 5:   // imul
                    if (wide) { int32_t p = (int32_t)(int16_t)r.ax * (int16_t)ea_rd16(ea); r.ax = (uint16_t)p; r.dx = (uint16_t)((uint32_t)p >> 16);
                                r.cf = r.of = p != (int32_t)(int16_t)p; }
                    else      { int16_t p = (int16_t)(int8_t)r.ax * (int8_t)ea_rd8(ea); r.ax = (uint16_t)p;
                                r.cf = r.of = p != (int16_t)(int8_t)p; }
                    break;
                case 6:   // div
                    if (wide) { uint32_t n = ((uint32_t)r.dx << 16) | r.ax; uint16_t d = ea_rd16(ea);
                                if (!d) { fail("div16 by zero"); return; }
                                r.ax = (uint16_t)(n / d); r.dx = (uint16_t)(n % d); }
                    else      { uint16_t n = r.ax; uint8_t d = ea_rd8(ea);
                                if (!d) { fail("div8 by zero"); return; }
                                r.ax = (uint16_t)(((n % d) << 8) | (uint8_t)(n / d)); }
                    break;
                default: fail("group3 /%d unmodeled", ea.reg_field); return;
            }
            break;
        }

        // ---- group 4/5 (FE/FF) --------------------------------------------
        case 0xFE: { EA ea = modrm(); bool cf = r.cf;
                     if (ea.reg_field == 0) ea_wr8(ea, add8(ea_rd8(ea), 1));
                     else if (ea.reg_field == 1) ea_wr8(ea, sub8(ea_rd8(ea), 1));
                     else { fail("FE /%d unmodeled", ea.reg_field); return; }
                     r.cf = cf; break; }
        case 0xFF: { EA ea = modrm();
            switch (ea.reg_field) {
                case 0: { bool cf = r.cf; ea_wr16(ea, add16(ea_rd16(ea), 1)); r.cf = cf; break; }  // inc
                case 1: { bool cf = r.cf; ea_wr16(ea, sub16(ea_rd16(ea), 1)); r.cf = cf; break; }  // dec
                case 2: push(r.ip); r.ip = ea_rd16(ea); break;                                     // call near r/m
                case 3: {                                                                          // call far m
                    // `call far [cs:0x2957]` — the AIL callback escape hatch.
                    if (!ea.is_reg && ea.off == callback_ptr_off && ea.seg == DRV_PARA) {
                        if (ail_callback_hook) ail_callback_hook();
                        break;
                    }
                    uint16_t off = rd16(ea.seg, ea.off), seg = rd16(ea.seg, (uint16_t)(ea.off + 2));
                    push(r.cs); push(r.ip); r.cs = seg; r.ip = off;
                    if (seg != DRV_PARA) fail("call far to unmodeled segment %04X:%04X", seg, off);
                    break;
                }
                case 4: r.ip = ea_rd16(ea); break;                                                 // jmp near r/m
                case 5: { uint16_t off = rd16(ea.seg, ea.off), seg = rd16(ea.seg, (uint16_t)(ea.off + 2));
                          r.cs = seg; r.ip = off;
                          if (seg != DRV_PARA) fail("jmp far to unmodeled segment"); break; }
                case 6: push(ea_rd16(ea)); break;                                                  // push r/m
                default: fail("FF /%d unmodeled", ea.reg_field); return;
            }
            break;
        }

        default:
            fail("unmodeled opcode %02X at %04X", op, ip0);
            return;
    }
}

// ---------------------------------------------------------------------------
// Singleton + C entry points (wired to the game in a later step; everything
// stays inert unless V2_NATIVE_AIL=1, same contract as v2_native_opl.cpp).
// ---------------------------------------------------------------------------
static AilInterp g_ail;

extern "C" int v2_ail_interp_enabled() {
    static int en = -1;
    if (en < 0) { const char* e = getenv("V2_NATIVE_AIL"); en = (e && e[0] == '1') ? 1 : 0; }
    return en;
}

extern "C" void v2_ail_interp_load(const uint8_t* blob, uint32_t blob_size,
                                   const uint8_t* bank, uint32_t bank_size) {
    g_ail.load(blob, blob_size, bank, bank_size);
}

extern "C" void v2_ail_interp_set_io(void (*out_fn)(uint16_t, uint8_t),
                                     uint8_t (*in_fn)(uint16_t)) {
    g_ail.out_hook = out_fn;
    g_ail.in_hook = in_fn;
}

extern "C" uint16_t v2_ail_interp_call(uint16_t fn_off, const uint16_t* args, int argc) {
    if (getenv("V2_AIL_TRACE")) g_ail.trace = true;
    uint16_t ret = g_ail.call_fn(fn_off, args, argc);
    if (g_ail.fault) {
        fprintf(stderr, "AIL-INTERP: fn @%04X faulted: %s\n", fn_off, g_ail.fault_msg);
        g_ail.fault = 0;   // one report per call; state may be inconsistent
    }
    return ret;
}
