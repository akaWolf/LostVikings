// ============================================================================
// FN-TEST — differential golden tests for orig↔v2 functions (Phase 0, Mode A).
// Design: FN_TEST_ANALYSIS.md. Strategy rationale: REWRITE_STRATEGY.md.
//
// Mode A (inline differential): a hook at the ENTRY of an orig function
// captures {full DS, regs} (= ds_in); hooks before each RETN capture DS again
// (= ds_out, the golden). The v2 rewrite of the same function is then run on a
// scratch copy of ds_in and byte-compared against ds_out. The orig function is
// NEVER re-invoked — its golden output comes from the live run where it
// executed anyway (no synthetic inputs, no re-driving the m2c machinery).
//
// Enable per function via env: FNTEST=sub_15972[,name...]  or  FNTEST=all.
// Disabled (no env) = one bool check per hook, zero copies.
//
// Threading: hooks run on the game thread only. In default/HEADLESS the v2
// mirror thread is idle while orig executes between barriers (lock-step
// v2_signal_phase), so running v2 code here does not race the live mirror.
// The test uses its own scratch shadow and never touches v2_vm_shadow_ds.
//
// Re-entrancy: one in-flight slot per function. Registered functions must not
// recurse into themselves (nested DIFFERENT functions each use their own
// slot). sub_15972: leaf, no calls — trivially safe.
// ============================================================================
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Thin exports from v2_vm.cpp (wrappers over file-static v2 functions/tables).
extern "C" void v2_fntest_call_sub_15972(uint8_t* test_shadow, uint16_t ax, uint16_t di);
extern "C" int  v2_fntest_ds_skip(uint32_t addr);
extern "C" void v2_fntest_report(void);  // defined below; fwd for atexit registration

// Exports from vikings.exe_seg000.cpp: isolated orig-function execution
// (SYNTHETIC_DIFF_ANALYSIS.md). No game/SDL/threads required.
// io_regs[8]: in/out ax,bx,cx,dx,si,di,bp + [7]=CF on exit.
extern "C" uint32_t v2_fntest_game_ds_linear(void);
extern "C" void     v2_fntest_snap_game_ds(uint8_t* out64k);
extern "C" void*    v2_fntest_orig_fnptr(int id);
extern "C" bool     v2_fntest_orig_isolated(void* fn, uint8_t* ds_image, uint16_t* io_regs);
extern "C" int      v2_fntest_call_sub_161a1(uint8_t* test_shadow, uint16_t di, uint16_t si);
extern "C" void     v2_fntest_call_sub_15da8(uint8_t* test_shadow, uint16_t ax, uint16_t si, uint16_t di);
extern "C" void     v2_fntest_call_sub_15d6b(uint8_t* test_shadow, uint16_t ax, uint16_t si, uint16_t di);
extern "C" int      v2_fntest_call_sub_13d68(uint8_t* test_shadow, uint16_t si);
extern "C" void     v2_fntest_call_sub_13dd6(uint8_t* test_shadow, uint16_t si);
extern "C" void     v2_fntest_call_sub_13e15(uint8_t* test_shadow, uint16_t si);
extern "C" void     v2_fntest_call_sub_13c0c(uint8_t* test_shadow);
extern "C" void     v2_fntest_call_sub_1064b(uint8_t* test_shadow);
extern "C" void     v2_fntest_call_sub_10704(uint8_t* test_shadow);
extern "C" void     v2_fntest_call_sub_10753(uint8_t* test_shadow);
extern "C" void     v2_fntest_call_sub_17496(uint8_t* test_shadow, uint16_t si_speed);
extern "C" void     v2_fntest_call_sub_1746c(uint8_t* test_shadow, uint16_t si_speed);
extern "C" void     v2_fntest_call_sub_101be(uint8_t* test_shadow);
extern "C" void     v2_fntest_call_vm_exec(uint8_t* test_shadow, uint16_t si);
extern "C" void     v2_fntest_call_sub_10255(uint8_t* test_shadow, uint16_t si, uint16_t dx);
extern "C" void     v2_fntest_call_sub_1020f(uint8_t* test_shadow, uint16_t si, uint16_t dx);
extern "C" void     v2_fntest_call_sub_12fc6(uint8_t* test_shadow);
extern "C" void     v2_fntest_call_sub_12fcb(uint8_t* test_shadow);
extern "C" void     v2_fntest_call_sub_12fd0(uint8_t* test_shadow);
extern "C" int      v2_fntest_call_search(uint8_t* test_shadow, int which,
                                          uint16_t filter, uint16_t obj);
extern "C" int32_t  v2_fntest_call_scan(uint8_t* test_shadow, int which,
                                        uint16_t filter, uint16_t obj);
extern "C" void     v2_fntest_call_anim(uint8_t* test_shadow, uint16_t obj, int which);
extern "C" long     v2_fntest_ret_mismatches(void);
#include <setjmp.h>
extern "C" sigjmp_buf* v2_fntest_jb(void);
extern "C" void     v2_fntest_alarm_ms(long ms);
extern "C" void     v2_fntest_arm_signals(void);
extern "C" void*    v2_fntest_m2c_base(void);
extern void         v2_set_m2c_base(void* base);   // v2 resolve fallback target

extern int v2_dbg_pre_vm_iter;  // game frame counter — context for FAIL logs

// Shared with the orig side (seg000): isolated-mode flag + start:-escape
// counter. Some synthetic inputs hit orig-UB paths (e.g. VM op 0x15 with
// arg&7==0 dispatches to locret_15504, a bare RETN that pops PUSH(si)'s
// value as a return address → __disp 0x01A20000 → start:). The start: and
// dispatcher-default guards bail out of the group and bump the counter; the
// hang watchdog (SIGALRM in v2_fntest_orig_isolated) bumps it too. Runners
// treat a bumped counter as "orig-UB input — not a comparable case".
extern "C" {
int  v2_fntest_isolated_active = 0;
long v2_fntest_start_escapes = 0;
int  v2_fntest_watchdog_enable = 0;   // =1 in the selftest process only
int  v2_fntest_vm_soft = 0;           // 1: v2 VM FATALs become soft aborts; 2: one fired
}

namespace {

enum FtId { FT_SUB_15972 = 0, FT_SUB_161A1 = 1, FT_SUB_15DA8 = 2, FT_SUB_15D6B = 3,
            FT_SUB_13D68 = 4, FT_SUB_13DD6 = 5, FT_SUB_13E15 = 6, FT_SUB_13C0C = 7,
            FT_SUB_1064B = 8, FT_SUB_10704 = 9, FT_SUB_10753 = 10,
            FT_SUB_17496 = 11, FT_SUB_1746C = 12, FT_SUB_101BE = 13,
            FT_SUB_1424C = 14, FT_SUB_10255 = 15, FT_SUB_1020F = 16,
            FT_SUB_12FC6 = 17, FT_SUB_12FCB = 18, FT_SUB_12FD0 = 19,
            FT_SUB_158AA = 20, FT_SUB_158B9 = 21, FT_SUB_158C8 = 22,
            FT_SUB_158D7 = 23, FT_SUB_158E6 = 24,
            FT_SUB_1303A = 25, FT_SUB_13031 = 26,
            FT_SUB_1614E = 27, FT_SUB_15C37 = 28, FT_SUB_15C93 = 29,
            FT_SUB_15AFD = 30, FT_COUNT };

struct FtRegs { uint16_t ax, bx, cx, dx, si, di, bp; };

struct FtSlot {
    bool     enabled = false;
    bool     armed   = false;   // entry seen, waiting for the RETN hook
    FtRegs   regs{};
    long     calls = 0, pass = 0, fail = 0;
    uint8_t  ds_in[0x10000];
};

FtSlot      g_slot[FT_COUNT];
const char* g_name[FT_COUNT] = { "sub_15972", "sub_161a1", "sub_15da8", "sub_15d6b",
                                 "sub_13d68", "sub_13dd6", "sub_13e15", "sub_13c0c",
                                 "sub_1064b", "sub_10704", "sub_10753",
                                 "sub_17496", "sub_1746c", "sub_101be",
                                 "sub_1424c", "sub_10255", "sub_1020f",
                                 "sub_12fc6", "sub_12fcb", "sub_12fd0",
                                 "sub_158aa", "sub_158b9", "sub_158c8",
                                 "sub_158d7", "sub_158e6",
                                 "sub_1303a", "sub_13031",
                                 "sub_1614e", "sub_15c37", "sub_15c93",
                                 "sub_15afd" };

// Buffers carry a 16-byte tail past the 64KB window: a WORD read at offset
// 0xFFFF touches byte 0x10000, which the m2c oracle reads LINEARLY from the
// real m2c::m (no 8086 segment wrap in the port). The tail is filled with
// those same real bytes before each case so both sides read identical
// out-of-window values; the tail itself is never diffed.
uint8_t g_scratch[0x10010];   // v2 executes here; never the live shadow
bool    g_init_done   = false;
bool    g_any_enabled = false;

// Synthetic-diff buffers (selftest + xcheck). Game-thread only.
uint8_t g_synth_base[0x10010];   // pristine game DS image (static EXE data) + tail
uint8_t g_synth_in[0x10010];     // constructed case input
uint8_t g_synth_orig[0x10010];   // isolated-orig output (the oracle)

void ft_fill_tail(uint8_t* buf) {
    memcpy(buf + 0x10000,
           (uint8_t*)v2_fntest_m2c_base() + v2_fntest_game_ds_linear() + 0x10000, 0x10);
}
long    g_xchk_ok[FT_COUNT]      = {};
long    g_xchk_diverge[FT_COUNT] = {};

// Combined orig-UB marker: start:/default-guard escapes + shadow-stack ret
// mismatches (POP-through-frame paths that survive on a valid case label).
long ft_ub_marks() { return v2_fntest_start_escapes + v2_fntest_ret_mismatches(); }

// Sharding for heavy exhaustive series: FNSELFTEST_SHARD="i/N" splits the
// 65536-value axis across N parallel processes (one per core). grid+fuzz
// run only in shard 0.
int g_shard_i = 0, g_shard_n = 1;
bool ft_shard_mine(uint32_t v) { return (int)(v % (uint32_t)g_shard_n) == g_shard_i; }

struct FtWr { uint16_t addr, val; };   // extra DS word for directed cases

// Shared 64KB test code/tile segment (class B + search units). See the
// class-B section for the placement rationale (heap area of m2c::m).
const uint16_t FT_VM_TESTSEG = 0x4000;
const uint32_t FT_VM_ZONE = 0x10000;        // full 64KB segment
const uint16_t FT_VM_PC = 0x0100;
uint8_t g_vm_es_in[FT_VM_ZONE], g_vm_es_orig[FT_VM_ZONE];
uint8_t g_vm_es_patch[8]; int g_vm_es_patch_n = 0;   // zone[0..n) bytes for crafted headers

void ft_run_v2(FtId id, uint8_t* shadow, const FtRegs& r) {
    switch (id) {
    case FT_SUB_15972: v2_fntest_call_sub_15972(shadow, r.ax, r.di); break;
    default: break;
    }
}

void ft_init() {
    g_init_done = true;
    const char* env = getenv("FNTEST");
    if (!env || !env[0]) return;
    bool all = (strcmp(env, "all") == 0);
    for (int i = 0; i < FT_COUNT; i++) {
        if (all || strstr(env, g_name[i])) {
            g_slot[i].enabled = true;
            g_any_enabled = true;
            fprintf(stderr, "FNTEST: differential test enabled for %s\n", g_name[i]);
        }
    }
    // Normal-exit summary. Game quit paths use _exit() which SKIPS atexit —
    // headless_check_exit() calls v2_fntest_report() explicitly for that case.
    if (g_any_enabled) atexit(v2_fntest_report);
}

// Eager init at process start: the "enabled" banner prints even if the hooked
// function is never reached (SUMMARY with calls=0 then distinguishes "function
// never ran" from "tests disabled"). pre() keeps the lazy call as a fallback.
struct FtBoot { FtBoot() { ft_init(); } } g_boot;

} // namespace

extern "C" void v2_fntest_report(void) {
    for (int i = 0; i < FT_COUNT; i++) {
        if (!g_slot[i].enabled) continue;
        fprintf(stderr, "FNTEST-SUMMARY[%s]: calls=%ld pass=%ld fail=%ld%s\n",
                g_name[i], g_slot[i].calls, g_slot[i].pass, g_slot[i].fail,
                g_slot[i].fail ? "  <<< DIVERGENCE" : "");
        if (g_xchk_ok[i] || g_xchk_diverge[i])
            fprintf(stderr, "FNTEST-XCHECK-SUMMARY[%s]: ok=%ld diverge=%ld%s\n",
                    g_name[i], g_xchk_ok[i], g_xchk_diverge[i],
                    g_xchk_diverge[i] ? "  <<< ORACLE BROKEN" : "");
    }
}

// Called from the orig function ENTRY (seg000 hook). ds_base = raddr(ds,0).
extern "C" void v2_fntest_pre(int id, const uint8_t* ds_base,
                              uint16_t ax, uint16_t bx, uint16_t cx, uint16_t dx,
                              uint16_t si, uint16_t di, uint16_t bp)
{
    if (!g_init_done) ft_init();
    if (id < 0 || id >= FT_COUNT) return;
    FtSlot& s = g_slot[id];
    if (!s.enabled) return;
    memcpy(s.ds_in, ds_base, sizeof(s.ds_in));
    s.regs = { ax, bx, cx, dx, si, di, bp };
    s.armed = true;
}

// Called right before each RETN of the orig function (seg000 hook).
// ds_base now holds the golden output state.
extern "C" void v2_fntest_post(int id, const uint8_t* ds_base)
{
    if (id < 0 || id >= FT_COUNT) return;
    FtSlot& s = g_slot[id];
    if (!s.enabled || !s.armed) return;
    s.armed = false;
    s.calls++;

    memcpy(g_scratch, s.ds_in, sizeof(g_scratch));
    ft_run_v2((FtId)id, g_scratch, s.regs);

    long diffs = 0;
    for (uint32_t a = 0; a < 0x10000; a++) {
        if (g_scratch[a] == ds_base[a]) continue;
        if (v2_fntest_ds_skip(a)) continue;   // documented exclusions (AIL, input race, ...)
        if (diffs < 8)
            fprintf(stderr,
                "FNTEST-DIFF[%s call=%ld f=%d]: addr=%04X orig=%02X v2=%02X (in=%02X) "
                "ax=%04X di=%04X\n",
                g_name[id], s.calls, v2_dbg_pre_vm_iter, a, ds_base[a], g_scratch[a],
                s.ds_in[a], s.regs.ax, s.regs.di);
        diffs++;
    }
    if (diffs) {
        s.fail++;
        fprintf(stderr, "FNTEST-FAIL[%s call=%ld f=%d]: %ld byte(s) diverge\n",
                g_name[id], s.calls, v2_dbg_pre_vm_iter, diffs);
    } else {
        s.pass++;
    }

    // Oracle cross-check (env FNTEST_XCHECK): the isolated orig call on the
    // captured live input must reproduce the live orig output byte-for-byte.
    // Validates the whole isolation machinery (state/stack/trap/DS mapping)
    // against ground truth. Diagnostic mode: the render thread may async-write
    // the input word (ds:0x86DE) while game DS is temporarily swapped — that
    // address is in the verify skip table, so no false diffs; a lost input
    // edge during the swap window is theoretically possible, so keep XCHECK
    // off in precision replay runs.
    static int xchk = -1;
    if (xchk < 0) xchk = getenv("FNTEST_XCHECK") ? 1 : 0;
    if (xchk) {
        void* fn = v2_fntest_orig_fnptr(id);
        if (fn) {
            s.enabled = false;   // guard: hooks inside the function re-enter
            memcpy(g_synth_orig, s.ds_in, sizeof(g_synth_orig));
            uint16_t xregs[8] = { s.regs.ax, s.regs.bx, s.regs.cx, s.regs.dx,
                                  s.regs.si, s.regs.di, s.regs.bp, 0 };
            v2_fntest_orig_isolated(fn, g_synth_orig, xregs);
            s.enabled = true;
            long xd = 0;
            for (uint32_t a = 0; a < 0x10000; a++) {
                if (g_synth_orig[a] == ds_base[a]) continue;
                if (v2_fntest_ds_skip(a)) continue;
                if (xd < 8)
                    fprintf(stderr, "FNTEST-XCHECK-DIVERGE[%s call=%ld]: addr=%04X "
                            "live=%02X isolated=%02X\n",
                            g_name[id], s.calls, a, ds_base[a], g_synth_orig[a]);
                xd++;
            }
            if (xd) g_xchk_diverge[id]++; else g_xchk_ok[id]++;
        }
    }
}

// ============================================================================
// Synthetic-diff selftest (SYNTHETIC_DIFF_ANALYSIS.md, S0).
// Env: FNSELFTEST=sub_15972|all — run BEFORE game init (no SDL/threads/game),
// then exit with 0 (all pass) / 1 (divergence). Inputs are GENERATED (branch
// grid, boundaries, exhaustive 16-bit sweep, seeded fuzz); the oracle is the
// isolated orig function itself — no hand-written expectations anywhere.
// ============================================================================
namespace {

// Deterministic PRNG (fixed seed per group) — reproducible fuzz cases.
struct FtRng {
    uint32_t s;
    explicit FtRng(uint32_t seed) : s(seed) {}
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    uint16_t w() { return (uint16_t)(next() & 0xFFFF); }
};

inline void ft_wr16(uint8_t* p, uint32_t off, uint16_t v) {
    p[off] = (uint8_t)(v & 0xFF); p[off + 1] = (uint8_t)(v >> 8);
}

struct FtSynthStats { long cases = 0, pass = 0, fail = 0; };

// One sub_15972 case: build input from the pristine base, run isolated orig
// (oracle) and v2 on identical copies, byte-compare full DS.
// Contract (verified line-by-line against orig eips 0x5972-0x59C5):
//   reads  ax, di, DS[di+0x150D](Y_end), DS[di+0x14E5](Y_start), DS[di+0x1765](Y)
//   writes DS[di+0x150D], DS[di+0x14E5], DS[di+0x1765], DS[di+0x19E5]=0
// The canary 0xAAAA pre-filled at di+0x19E5 proves BOTH sides clear it.
bool ft_synth_case_15972(uint16_t ax, uint16_t di,
                         uint16_t y, uint16_t y_end, uint16_t y_start,
                         const char* group, FtSynthStats& st, long& diff_budget)
{
    st.cases++;
    memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
    ft_wr16(g_synth_in, (uint16_t)(di + 0x1765), y);
    ft_wr16(g_synth_in, (uint16_t)(di + 0x150D), y_end);
    ft_wr16(g_synth_in, (uint16_t)(di + 0x14E5), y_start);
    ft_wr16(g_synth_in, (uint16_t)(di + 0x19E5), 0xAAAA);

    memcpy(g_synth_orig, g_synth_in, sizeof(g_synth_orig));
    uint16_t regs[8] = { ax, 0, 0, 0, 0, di, 0, 0 };
    v2_fntest_orig_isolated(v2_fntest_orig_fnptr(FT_SUB_15972), g_synth_orig, regs);

    memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
    v2_fntest_call_sub_15972(g_scratch, ax, di);

    long diffs = 0;
    for (uint32_t a = 0; a < 0x10000; a++) {
        if (g_scratch[a] == g_synth_orig[a]) continue;
        if (v2_fntest_ds_skip(a)) continue;
        if (diff_budget > 0) {
            diff_budget--;
            fprintf(stderr, "FNSELFTEST-DIFF[sub_15972 %s]: addr=%04X orig=%02X v2=%02X "
                    "(in=%02X) | ax=%04X di=%04X y=%04X y_end=%04X y_start=%04X\n",
                    group, a, g_synth_orig[a], g_scratch[a], g_synth_in[a],
                    ax, di, y, y_end, y_start);
        }
        diffs++;
    }
    if (diffs) { st.fail++; return false; }
    st.pass++; return true;
}

int ft_selftest_sub_15972() {
    FtSynthStats grid, exh, fuzz;
    long diff_budget = 24;   // cap detailed prints; counters keep totals

    // --- Group 1: branch × boundary grid ---------------------------------
    // ax: both signs of every branch (ax<0 low-byte masked path, ax==0 snap,
    //     ax>0 snap-up), sign borders, byte borders, wrap.
    static const uint16_t AXS[] = { 0x0000, 0x0001, 0x0002, 0x000F, 0x0010,
                                    0x7FFF, 0x8000, 0x8001, 0xFF00, 0xFF01,
                                    0xFFF0, 0xFFFF };
    static const uint16_t DIS[] = { 0x0000, 0x0002, 0x0026 };
    static const uint16_t YSETS[][3] = {   // {y, y_end, y_start}
        { 0x0000, 0x0000, 0x0000 },
        { 0xFFFF, 0xFFFF, 0xFFFF },
        { 0x0124, 0x0131, 0x0116 },        // real values from attract f468
        { 0x0010, 0x000F, 0x0011 },        // tile-boundary cluster
        { 0x1FF0, 0x1FEF, 0x1FF1 },
        { 0x8000, 0x7FFF, 0x8001 },        // sign border
        { 0x0001, 0x0010, 0xFFF0 },        // wrap candidates
        { 0x0124, 0x0130, 0x0116 },        // y_end exactly on tile boundary
    };
    for (uint16_t ax : AXS)
        for (uint16_t di : DIS)
            for (auto& ys : YSETS)
                ft_synth_case_15972(ax, di, ys[0], ys[1], ys[2],
                                    "grid", grid, diff_budget);

    // --- Group 2: exhaustive 16-bit sweep of ax (all three branches fully) --
    static const uint16_t EXH_YSETS[][3] = {
        { 0x0124, 0x0131, 0x0116 },
        { 0x0010, 0x000F, 0x0011 },
        { 0xFFFF, 0x0000, 0x8000 },
        { 0x4000, 0x3FF0, 0x4010 },
    };
    for (uint32_t ax = 0; ax <= 0xFFFF; ax++)
        for (auto& ys : EXH_YSETS)
            ft_synth_case_15972((uint16_t)ax, 0, ys[0], ys[1], ys[2],
                                "exhaustive", exh, diff_budget);

    // --- Group 3: seeded fuzz over all contract fields ---------------------
    FtRng rng(0xC0FFEE01);
    for (int i = 0; i < 20000; i++) {
        uint16_t di = (uint16_t)((rng.next() % 20) * 2);   // object slots 0..0x26
        ft_synth_case_15972(rng.w(), di, rng.w(), rng.w(), rng.w(),
                            "fuzz", fuzz, diff_budget);
    }

    fprintf(stderr,
        "FNSELFTEST-SUMMARY[sub_15972]: grid %ld/%ld, exhaustive %ld/%ld, "
        "fuzz %ld/%ld — total cases=%ld fail=%ld%s\n",
        grid.pass, grid.cases, exh.pass, exh.cases, fuzz.pass, fuzz.cases,
        grid.cases + exh.cases + fuzz.cases,
        grid.fail + exh.fail + fuzz.fail,
        (grid.fail + exh.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (grid.fail + exh.fail + fuzz.fail) ? 1 : 0;
}

// One sub_161a1 case (bbox check, di=self si=target).
// Contract (verified line-by-line against orig eips 0x61A1-0x6234):
//   reads  di, si, DS[di+0x1535/0x155D/0x150D/0x1765/0x13CD],
//                  DS[si+0x1535/0x155D/0x14E5/0x1765/0x13CD]
//   writes ds:0x32/0x34/0x36 (scratch — only on paths past the X-overlap
//          rejects; canary 0xBBBB proves path-exact write behavior)
//   returns CF: STC = collision, CLC = no. Register outputs beyond CF are not
//   part of the v2 contract (v2 mirrors register effects inline at callers).
struct Ft161a1Fields {
    uint16_t di_x_lo, di_x_hi;    // [di+0x1535], [di+0x155D]
    uint16_t si_x_lo, si_x_hi;    // [si+0x1535], [si+0x155D]
    uint16_t di_y_end, di_y, di_yc;   // [di+0x150D], [di+0x1765], [di+0x13CD]
    uint16_t si_y_st, si_y, si_yc;    // [si+0x14E5], [si+0x1765], [si+0x13CD]
};

bool ft_synth_case_161a1(uint16_t di, uint16_t si, const Ft161a1Fields& f,
                         const char* group, FtSynthStats& st, long& diff_budget)
{
    st.cases++;
    memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
    ft_wr16(g_synth_in, (uint16_t)(di + 0x1535), f.di_x_lo);
    ft_wr16(g_synth_in, (uint16_t)(di + 0x155D), f.di_x_hi);
    ft_wr16(g_synth_in, (uint16_t)(si + 0x1535), f.si_x_lo);
    ft_wr16(g_synth_in, (uint16_t)(si + 0x155D), f.si_x_hi);
    ft_wr16(g_synth_in, (uint16_t)(di + 0x150D), f.di_y_end);
    ft_wr16(g_synth_in, (uint16_t)(di + 0x1765), f.di_y);
    ft_wr16(g_synth_in, (uint16_t)(di + 0x13CD), f.di_yc);
    ft_wr16(g_synth_in, (uint16_t)(si + 0x14E5), f.si_y_st);
    ft_wr16(g_synth_in, (uint16_t)(si + 0x1765), f.si_y);
    ft_wr16(g_synth_in, (uint16_t)(si + 0x13CD), f.si_yc);
    ft_wr16(g_synth_in, 0x32, 0xBBBB);   // scratch canaries: written only on
    ft_wr16(g_synth_in, 0x34, 0xBBBB);   // post-X-overlap paths — path-exact
    ft_wr16(g_synth_in, 0x36, 0xBBBB);   // behavior must match

    memcpy(g_synth_orig, g_synth_in, sizeof(g_synth_orig));
    uint16_t regs[8] = { 0, 0, 0, 0, si, di, 0, 0 };
    v2_fntest_orig_isolated(v2_fntest_orig_fnptr(FT_SUB_161A1), g_synth_orig, regs);
    int cf_orig = (int)regs[7];

    memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
    int cf_v2 = v2_fntest_call_sub_161a1(g_scratch, di, si);

    long diffs = 0;
    if (cf_orig != cf_v2) diffs++;
    for (uint32_t a = 0; a < 0x10000; a++) {
        if (g_scratch[a] == g_synth_orig[a]) continue;
        if (v2_fntest_ds_skip(a)) continue;
        diffs++;
    }
    if (diffs && diff_budget > 0) {
        diff_budget--;
        fprintf(stderr, "FNSELFTEST-DIFF[sub_161a1 %s]: cf orig=%d v2=%d | di=%04X si=%04X "
                "dX=[%04X,%04X] sX=[%04X,%04X] dY={%04X,%04X,%04X} sY={%04X,%04X,%04X}\n",
                group, cf_orig, cf_v2, di, si, f.di_x_lo, f.di_x_hi, f.si_x_lo, f.si_x_hi,
                f.di_y_end, f.di_y, f.di_yc, f.si_y_st, f.si_y, f.si_yc);
        for (uint32_t a = 0, shown = 0; a < 0x10000 && shown < 4; a++) {
            if (g_scratch[a] == g_synth_orig[a] || v2_fntest_ds_skip(a)) continue;
            fprintf(stderr, "  addr=%04X orig=%02X v2=%02X (in=%02X)\n",
                    a, g_synth_orig[a], g_scratch[a], g_synth_in[a]);
            shown++;
        }
    }
    if (diffs) { st.fail++; return false; }
    st.pass++; return true;
}

int ft_selftest_sub_161a1() {
    FtSynthStats grid, exh, fuzz;
    long diff_budget = 24;

    // --- Group 1: X-overlap configs × Y branch/boundary sets ---------------
    // X configs: overlap, reject#1 (JGE false), reject#2 (JL true), and both
    // EQUALITY borders of those signed compares.
    struct XCfg { uint16_t dlo, dhi, slo, shi; };
    static const XCfg XC[] = {
        { 0x0100, 0x0120, 0x0110, 0x0130 },  // overlap
        { 0x0100, 0x0100, 0x0110, 0x0130 },  // reject#1: [di+155D] < [si+1535]
        { 0x0100, 0x0120, 0x0110, 0x00F0 },  // reject#2: [si+155D] < [di+1535]
        { 0x0100, 0x0110, 0x0110, 0x0130 },  // border: [di+155D] == [si+1535] (JGE taken)
        { 0x0100, 0x0120, 0x0110, 0x0100 },  // border: [si+155D] == [di+1535] (JL not taken)
    };
    // Y sets aimed at each signed compare/clamp border in the tail.
    static const uint16_t YS[][6] = {   // {di_y_end, di_y, di_yc, si_y_st, si_y, si_yc}
        { 0x8000, 0x0010, 0x0010, 0x0100, 0x0100, 0x0100 },  // y_end<0 clamp
        { 0x0050, 0x0060, 0x0010, 0x0100, 0x0100, 0x0100 },  // adj_y == 0 (JG false)
        { 0x0050, 0x0060, 0x0011, 0x0100, 0x0100, 0x0100 },  // adj_y == 1 (JG true)
        { 0x0100, 0x0080, 0x0000, 0x0100, 0x0100, 0x0100 },  // adj_y >= ds32 path
        { 0x0020, 0x0100, 0x0000, 0x0100, 0x0100, 0x0100 },  // adj_y < ds32 path
        { 0x0100, 0x0100, 0x0100, 0x0200, 0x0100, 0x0050 },  // target_adj < si_y_st
        { 0x0100, 0x0100, 0x0100, 0x0200, 0x0050, 0x0050 },  // target_adj == si_y_st (JGE)
        { 0x0100, 0x0100, 0x0100, 0x0200, 0x0040, 0x0050 },  // target_adj > si_y_st
        { 0x7FFF, 0x8000, 0x0001, 0x7FFF, 0x8000, 0x0001 },  // signed extremes
        { 0x0100, 0x0000, 0x0000, 0x0100, 0x0000, 0x0000 },  // equal mid-values
        // JG-overflow corners of the adj_y clamp (orig 0x61cb SUB;ADD;JG — 8086 JG
        // is !ZF && SF==OF, i.e. the TRUE sum of the ADD operands vs 0):
        { 0x7FF0, 0x0010, 0x0100, 0x0100, 0x0100, 0x0100 },  // true sum +32992 → keep wrapped 0x80E0
        { 0x8010, 0x0010, 0xFF00, 0x0100, 0x0100, 0x0100 },  // true sum −33024 → clamp 0 (wrapped +0x7F00)
    };
    static const uint16_t DISI[][2] = { {0,2}, {2,0}, {0,0x26}, {4,4} };
    for (auto& xc : XC)
        for (auto& ys : YS)
            for (auto& p : DISI) {
                Ft161a1Fields f = { xc.dlo, xc.dhi, xc.slo, xc.shi,
                                    ys[0], ys[1], ys[2], ys[3], ys[4], ys[5] };
                ft_synth_case_161a1(p[0], p[1], f, "grid", grid, diff_budget);
            }

    // --- Group 2: exhaustive sweeps of the two hottest compare operands ----
    for (uint32_t v = 0; v <= 0xFFFF; v++) {   // [si+14E5]: in 3 signed compares
        Ft161a1Fields f = { 0x0100, 0x0120, 0x0110, 0x0130,
                            0x0100, 0x0080, 0x0020, (uint16_t)v, 0x0100, 0x0150 };
        ft_synth_case_161a1(0, 2, f, "exh-siYst", exh, diff_budget);
    }
    for (uint32_t v = 0; v <= 0xFFFF; v++) {   // [di+150D]: clamp + adj_y source
        Ft161a1Fields f = { 0x0100, 0x0120, 0x0110, 0x0130,
                            (uint16_t)v, 0x0080, 0x0020, 0x0100, 0x0100, 0x0150 };
        ft_synth_case_161a1(0, 2, f, "exh-diYend", exh, diff_budget);
    }

    // --- Group 3: seeded fuzz over the full contract ------------------------
    FtRng rng(0xB0B0CA71);
    for (int i = 0; i < 30000; i++) {
        uint16_t di = (uint16_t)((rng.next() % 20) * 2);
        uint16_t si = (uint16_t)((rng.next() % 20) * 2);
        Ft161a1Fields f = { rng.w(), rng.w(), rng.w(), rng.w(),
                            rng.w(), rng.w(), rng.w(), rng.w(), rng.w(), rng.w() };
        ft_synth_case_161a1(di, si, f, "fuzz", fuzz, diff_budget);
    }

    fprintf(stderr,
        "FNSELFTEST-SUMMARY[sub_161a1]: grid %ld/%ld, exhaustive %ld/%ld, "
        "fuzz %ld/%ld — total cases=%ld fail=%ld%s\n",
        grid.pass, grid.cases, exh.pass, exh.cases, fuzz.pass, fuzz.cases,
        grid.cases + exh.cases + fuzz.cases,
        grid.fail + exh.fail + fuzz.fail,
        (grid.fail + exh.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (grid.fail + exh.fail + fuzz.fail) ? 1 : 0;
}

// sub_15da8 (Y) / sub_15d6b (X): post-collision position snap twins.
// Contract (verified line-by-line, orig eips 0x5DA8-0x5DE4 / 0x5D6B-0x5DA7):
//   reads  ax (0 = snap negative / !0 = snap positive), si (partner), di (self),
//          Y twin: [di+0x150D],[si+0x14E5] (ax==0) or [si+0x150D],[di+0x14E5] (ax!=0)
//          X twin: [di+0x155D],[si+0x1535] (ax==0) or [si+0x155D],[di+0x1535] (ax!=0)
//   writes 3 self fields (pos, span_lo, span_hi) -= or += delta; clears the
//          fractional accumulator: [di+0x19E5] (Y) / [di+0x19BD] (X).
// Parameterized runner: field offsets differ, logic is identical.
struct FtSnapLayout {
    FtId id; uint16_t pos, lo, hi, clr;
    void (*v2call)(uint8_t*, uint16_t, uint16_t, uint16_t);
};
const FtSnapLayout FT_SNAP_Y = { FT_SUB_15DA8, 0x1765, 0x14E5, 0x150D, 0x19E5, v2_fntest_call_sub_15da8 };
const FtSnapLayout FT_SNAP_X = { FT_SUB_15D6B, 0x173D, 0x1535, 0x155D, 0x19BD, v2_fntest_call_sub_15d6b };

bool ft_synth_case_snap(const FtSnapLayout& L, uint16_t ax, uint16_t di, uint16_t si,
                        uint16_t d_pos, uint16_t d_lo, uint16_t d_hi,
                        uint16_t s_lo, uint16_t s_hi,
                        const char* group, FtSynthStats& st, long& diff_budget)
{
    st.cases++;
    memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
    ft_wr16(g_synth_in, (uint16_t)(di + L.pos), d_pos);
    ft_wr16(g_synth_in, (uint16_t)(di + L.lo),  d_lo);
    ft_wr16(g_synth_in, (uint16_t)(di + L.hi),  d_hi);
    ft_wr16(g_synth_in, (uint16_t)(si + L.lo),  s_lo);
    ft_wr16(g_synth_in, (uint16_t)(si + L.hi),  s_hi);
    ft_wr16(g_synth_in, (uint16_t)(di + L.clr), 0xAAAA);   // canary: must be cleared

    memcpy(g_synth_orig, g_synth_in, sizeof(g_synth_orig));
    uint16_t regs[8] = { ax, 0, 0, 0, si, di, 0, 0 };
    v2_fntest_orig_isolated(v2_fntest_orig_fnptr(L.id), g_synth_orig, regs);

    memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
    L.v2call(g_scratch, ax, si, di);

    long diffs = 0;
    for (uint32_t a = 0; a < 0x10000; a++) {
        if (g_scratch[a] == g_synth_orig[a]) continue;
        if (v2_fntest_ds_skip(a)) continue;
        if (diff_budget > 0) {
            diff_budget--;
            fprintf(stderr, "FNSELFTEST-DIFF[%s %s]: addr=%04X orig=%02X v2=%02X (in=%02X) | "
                    "ax=%04X di=%04X si=%04X dPos=%04X dLo=%04X dHi=%04X sLo=%04X sHi=%04X\n",
                    g_name[L.id], group, a, g_synth_orig[a], g_scratch[a], g_synth_in[a],
                    ax, di, si, d_pos, d_lo, d_hi, s_lo, s_hi);
        }
        diffs++;
    }
    if (diffs) { st.fail++; return false; }
    st.pass++; return true;
}

int ft_selftest_snap(const FtSnapLayout& L, uint32_t fuzz_seed) {
    FtSynthStats grid, exh, fuzz;
    long diff_budget = 24;

    // --- Group 1: branch grid — ax zero/non-zero variants × field boundaries
    static const uint16_t AXS[] = { 0x0000, 0x0001, 0x0002, 0x7FFF, 0x8000, 0xFFFF };
    static const uint16_t FS[][5] = {   // {d_pos, d_lo, d_hi, s_lo, s_hi}
        { 0x0100, 0x00F0, 0x0110, 0x0105, 0x0130 },  // overlap: delta small positive
        { 0x0100, 0x00F0, 0x0110, 0x0110, 0x0130 },  // delta == 1 (equality border)
        { 0x0100, 0x00F0, 0x0110, 0x0111, 0x0130 },  // delta == 0
        { 0x0100, 0x00F0, 0x0110, 0x0120, 0x0130 },  // negative delta (wraps)
        { 0x0000, 0x0000, 0x0000, 0x0000, 0x0000 },  // all zero
        { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF },  // all max (INC wrap)
        { 0x8000, 0x7FFF, 0x8001, 0x7FFF, 0x8000 },  // sign borders
    };
    static const uint16_t DISI[][2] = { {0,2}, {2,0}, {0,0x26}, {4,4} };
    for (uint16_t ax : AXS)
        for (auto& f : FS)
            for (auto& p : DISI)
                ft_synth_case_snap(L, ax, p[0], p[1], f[0], f[1], f[2], f[3], f[4],
                                   "grid", grid, diff_budget);

    // --- Group 2: exhaustive sweep of the delta source operand in each branch
    for (uint32_t v = 0; v <= 0xFFFF; v++)   // ax==0: delta = [di+hi] - [si+lo] + 1
        ft_synth_case_snap(L, 0, 0, 2, 0x0100, 0x00F0, (uint16_t)v, 0x0105, 0x0130,
                           "exh-dHi", exh, diff_budget);
    for (uint32_t v = 0; v <= 0xFFFF; v++)   // ax!=0: delta = [si+hi] - [di+lo] + 1
        ft_synth_case_snap(L, 1, 0, 2, 0x0100, 0x00F0, 0x0110, 0x0105, (uint16_t)v,
                           "exh-sHi", exh, diff_budget);

    // --- Group 3: seeded fuzz over the full contract
    FtRng rng(fuzz_seed);
    for (int i = 0; i < 20000; i++) {
        uint16_t di = (uint16_t)((rng.next() % 20) * 2);
        uint16_t si = (uint16_t)((rng.next() % 20) * 2);
        ft_synth_case_snap(L, rng.w(), di, si, rng.w(), rng.w(), rng.w(), rng.w(), rng.w(),
                           "fuzz", fuzz, diff_budget);
    }

    fprintf(stderr,
        "FNSELFTEST-SUMMARY[%s]: grid %ld/%ld, exhaustive %ld/%ld, "
        "fuzz %ld/%ld — total cases=%ld fail=%ld%s\n",
        g_name[L.id], grid.pass, grid.cases, exh.pass, exh.cases, fuzz.pass, fuzz.cases,
        grid.cases + exh.cases + fuzz.cases,
        grid.fail + exh.fail + fuzz.fail,
        (grid.fail + exh.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (grid.fail + exh.fail + fuzz.fail) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// sub_13d68: sub-sprite pool scan. Input: si ([si+0x1AD5] = run length),
// ds:0x374 (pool select), pool occupancy = [d+0x44D]/[d+0x114D] word pairs
// for d in [0,0x100). Output: CF + ds:0x32 (limit), ds:0x3A/0x38 (candidate
// start/end — written per candidate, 0xBBBB canaries verify).
// occ128[d>>1] bits: 1 -> [d+0x44D]=0x8000, 2 -> [d+0x114D]=0x204.
bool ft_synth_case_13d68(uint16_t mode, uint16_t si, uint16_t count,
                         const uint8_t* occ128,
                         const char* group, FtSynthStats& st, long& diff_budget)
{
    st.cases++;
    memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
    ft_wr16(g_synth_in, 0x374, mode);
    ft_wr16(g_synth_in, (uint16_t)(si + 0x1AD5), count);
    for (uint16_t d = 0; d < 0x100; d += 2) {
        uint8_t o = occ128[d >> 1];
        if (o & 1) ft_wr16(g_synth_in, (uint16_t)(d + 0x44D),  0x8000);
        if (o & 2) ft_wr16(g_synth_in, (uint16_t)(d + 0x114D), 0x204);
    }
    ft_wr16(g_synth_in, 0x32, 0xBBBB);
    ft_wr16(g_synth_in, 0x3A, 0xBBBB);
    ft_wr16(g_synth_in, 0x38, 0xBBBB);

    memcpy(g_synth_orig, g_synth_in, sizeof(g_synth_orig));
    uint16_t regs[8] = { 0, 0, 0, 0, si, 0, 0, 0 };
    v2_fntest_orig_isolated(v2_fntest_orig_fnptr(FT_SUB_13D68), g_synth_orig, regs);
    int cf_orig = (int)regs[7];

    memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
    int cf_v2 = v2_fntest_call_sub_13d68(g_scratch, si);

    long diffs = (cf_orig != cf_v2) ? 1 : 0;
    for (uint32_t a = 0; a < 0x10000; a++) {
        if (g_scratch[a] == g_synth_orig[a]) continue;
        if (v2_fntest_ds_skip(a)) continue;
        diffs++;
    }
    if (diffs && diff_budget > 0) {
        diff_budget--;
        fprintf(stderr, "FNSELFTEST-DIFF[sub_13d68 %s]: cf orig=%d v2=%d | mode=%04X si=%04X count=%04X\n",
                group, cf_orig, cf_v2, mode, si, count);
        for (uint32_t a = 0, shown = 0; a < 0x10000 && shown < 4; a++) {
            if (g_scratch[a] == g_synth_orig[a] || v2_fntest_ds_skip(a)) continue;
            fprintf(stderr, "  addr=%04X orig=%02X v2=%02X (in=%02X)\n",
                    a, g_synth_orig[a], g_scratch[a], g_synth_in[a]);
            shown++;
        }
    }
    if (diffs) { st.fail++; return false; }
    st.pass++; return true;
}

int ft_selftest_sub_13d68() {
    FtSynthStats grid, fuzz;
    long diff_budget = 24;
    uint8_t occ[128];
    static const uint16_t MODES[] = { 0, 1, 2, 0x8000 };   // else-branch twice
    static const uint16_t SIS[]   = { 0x0000, 0x0028 };

    for (uint16_t mode : MODES) for (uint16_t si : SIS) {
        // all free — first candidate wins
        memset(occ, 0, sizeof(occ));
        static const uint16_t COUNTS[] = { 0, 1, 2, 6, 0x30, 0x7FFF };
        for (uint16_t c : COUNTS)
            ft_synth_case_13d68(mode, si, c, occ, "grid", grid, diff_budget);
        // all occupied (via 0x44D) — immediate STC path
        memset(occ, 1, sizeof(occ));
        ft_synth_case_13d68(mode, si, 2, occ, "grid", grid, diff_budget);
        // all occupied via 0x114D only — OR semantics
        memset(occ, 2, sizeof(occ));
        ft_synth_case_13d68(mode, si, 2, occ, "grid", grid, diff_budget);
        // hole of exactly count-1 then a hole of count (restart + 3A/38 rewrite)
        memset(occ, 1, sizeof(occ));
        for (int d = 0x10; d < 0x16; d += 2) occ[d >> 1] = 0;   // 3-slot hole
        for (int d = 0x40; d < 0x48; d += 2) occ[d >> 1] = 0;   // 4-slot hole
        ft_synth_case_13d68(mode, si, 4, occ, "grid", grid, diff_budget);
        // free run touching the pool limit exactly (JZ limit border)
        memset(occ, 1, sizeof(occ));
        {
            uint16_t lim = (mode == 0) ? 0x100 : (mode == 1 ? 0x50 : 0x30);
            for (int d = lim - 8; d < lim; d += 2) occ[d >> 1] = 0;
            ft_synth_case_13d68(mode, si, 4, occ, "grid", grid, diff_budget);  // fits exactly
            ft_synth_case_13d68(mode, si, 5, occ, "grid", grid, diff_budget);  // overruns -> STC
        }
    }

    FtRng rng(0x13D68001);
    for (int i = 0; i < 40000; i++) {
        uint16_t mode = (uint16_t)(rng.next() % 3);
        uint16_t si = (uint16_t)((rng.next() % 20) * 2);
        uint16_t count = (uint16_t)(rng.next() & 0x3F);
        if ((rng.next() & 0xFF) == 0) count = rng.w();          // rare wild counts
        for (int d = 0; d < 128; d++) occ[d] = (uint8_t)(rng.next() & 3);
        ft_synth_case_13d68(mode, si, count, occ, "fuzz", fuzz, diff_budget);
    }

    fprintf(stderr,
        "FNSELFTEST-SUMMARY[sub_13d68]: grid %ld/%ld, fuzz %ld/%ld — total cases=%ld fail=%ld%s\n",
        grid.pass, grid.cases, fuzz.pass, fuzz.cases, grid.cases + fuzz.cases,
        grid.fail + fuzz.fail, (grid.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (grid.fail + fuzz.fail) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// sub_13dd6 / sub_13e15: slot-range init loops (do-while, signed JL bottom
// test — an inverted/empty range still writes ONE slot). sub_13dd6 re-reads
// ds:0x2E73 and [si+0x1855] every iteration: directed alias cases make the
// loop overwrite its own sources, which catches hoisted-read rewrites.
bool ft_synth_case_1345init(FtId id, uint16_t si, uint16_t start, uint16_t end,
                            uint16_t flags, uint16_t spr_base, uint16_t spr_seg,
                            const char* group, FtSynthStats& st, long& diff_budget)
{
    st.cases++;
    memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
    ft_wr16(g_synth_in, (uint16_t)(si + 0x1A85), start);
    ft_wr16(g_synth_in, (uint16_t)(si + 0x1AAD), end);
    ft_wr16(g_synth_in, (uint16_t)(si + 0x1585), flags);
    ft_wr16(g_synth_in, (uint16_t)(si + 0x1855), spr_base);
    ft_wr16(g_synth_in, 0x2E73, spr_seg);
    // Pre-fill [d+0x44D] pattern so sub_13e15's OR has varied prior bits.
    for (uint16_t d = start, n = 0; n < 8; d += 2, n++)
        ft_wr16(g_synth_in, (uint16_t)(d + 0x44D), (uint16_t)(0x1110 * n));

    memcpy(g_synth_orig, g_synth_in, sizeof(g_synth_orig));
    uint16_t regs[8] = { 0, 0, 0, 0, si, 0, 0, 0 };
    v2_fntest_orig_isolated(v2_fntest_orig_fnptr(id), g_synth_orig, regs);

    memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
    if (id == FT_SUB_13DD6) v2_fntest_call_sub_13dd6(g_scratch, si);
    else                    v2_fntest_call_sub_13e15(g_scratch, si);

    long diffs = 0;
    for (uint32_t a = 0; a < 0x10000; a++) {
        if (g_scratch[a] == g_synth_orig[a]) continue;
        if (v2_fntest_ds_skip(a)) continue;
        if (diff_budget > 0) {
            diff_budget--;
            fprintf(stderr, "FNSELFTEST-DIFF[%s %s]: addr=%04X orig=%02X v2=%02X (in=%02X) | "
                    "si=%04X start=%04X end=%04X flags=%04X base=%04X seg=%04X\n",
                    g_name[id], group, a, g_synth_orig[a], g_scratch[a], g_synth_in[a],
                    si, start, end, flags, spr_base, spr_seg);
        }
        diffs++;
    }
    if (diffs) { st.fail++; return false; }
    st.pass++; return true;
}

int ft_selftest_1345init(FtId id, uint32_t fuzz_seed) {
    FtSynthStats grid, fuzz;
    long diff_budget = 24;
    static const uint16_t FLAGS[] = { 0x0000, 0x0001, 0x00CE, 0x00CF, 0xFFFF, 0x8001 };

    for (uint16_t fl : FLAGS) {
        // normal ranges incl. single-slot
        ft_synth_case_1345init(id, 0, 0x48, 0x50, fl, 0x1234, 0x5678, "grid", grid, diff_budget);
        ft_synth_case_1345init(id, 0, 0x48, 0x4A, fl, 0x1234, 0x5678, "grid", grid, diff_budget);
        // EMPTY and INVERTED ranges: orig do-while still writes one slot
        ft_synth_case_1345init(id, 0, 0x48, 0x48, fl, 0x1234, 0x5678, "grid", grid, diff_budget);
        ft_synth_case_1345init(id, 0, 0x50, 0x30, fl, 0x1234, 0x5678, "grid", grid, diff_budget);
        // alias: range covers si+0x1855 (write [d+0x84D] hits the spr_base source)
        ft_synth_case_1345init(id, 0, 0x1000, 0x1020, fl, 0x1234, 0x5678, "grid", grid, diff_budget);
        // alias: range covers ds:0x2E73 (write [d+0x54D]=0 zeroes the seg source)
        ft_synth_case_1345init(id, 0, 0x2920, 0x2940, fl, 0x1234, 0x5678, "grid", grid, diff_budget);
        // signed-border range ends
        ft_synth_case_1345init(id, 0, 0x7FF0, 0x8000, fl, 0x1234, 0x5678, "grid", grid, diff_budget);
    }

    FtRng rng(fuzz_seed);
    for (int i = 0; i < 30000; i++) {
        uint16_t si = (uint16_t)((rng.next() % 20) * 2);
        uint16_t start = rng.w();
        uint16_t end = (uint16_t)(start + (rng.next() & 0x3F) * 2);
        ft_synth_case_1345init(id, si, start, end, rng.w(), rng.w(), rng.w(),
                               "fuzz", fuzz, diff_budget);
    }

    fprintf(stderr,
        "FNSELFTEST-SUMMARY[%s]: grid %ld/%ld, fuzz %ld/%ld — total cases=%ld fail=%ld%s\n",
        g_name[id], grid.pass, grid.cases, fuzz.pass, fuzz.cases, grid.cases + fuzz.cases,
        grid.fail + fuzz.fail, (grid.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (grid.fail + fuzz.fail) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// sub_13c0c: viewport despawn bounds + offscreen marking.
// Contract (verified line-by-line, orig eips 0x3C0C-0x3C92):
//   reads  ds:0x44/0x46 (viewport), ds:0x372 (table end), and per object
//          si=6..te: [si+0x1355] alive, [si+0x1585] flags (0x800 = permanent),
//          [si+0x173D/0x14BD] X/half-width, [si+0x1765/0x1495] Y/half-height
//   writes ds:0x34 = clamp0(vp_x-0x10); ds:0x36 = RAW(vp_x-0x10)+0x160 (X ax
//          stays unclamped!); ds:0x38 = clamp0(vp_y-0x10); ds:0x3A =
//          CLAMPED+0xD0 (Y ax IS clamped — the asymmetry that was wrong in
//          the dead v2 function); OR [si+0x1585] |= 0x200 for objects outside.
struct FtCullObj { uint16_t alive, flags, x, hw, y, hh; };

bool ft_synth_case_13c0c(uint16_t vpx, uint16_t vpy,
                         const FtCullObj* objs, int n_objs,
                         const char* group, FtSynthStats& st, long& diff_budget)
{
    st.cases++;
    memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
    ft_wr16(g_synth_in, 0x44, vpx);
    ft_wr16(g_synth_in, 0x46, vpy);
    ft_wr16(g_synth_in, 0x372, (uint16_t)(6 + n_objs * 2));
    for (int i = 0; i < n_objs; i++) {
        uint16_t si = (uint16_t)(6 + i * 2);
        ft_wr16(g_synth_in, (uint16_t)(si + 0x1355), objs[i].alive);
        ft_wr16(g_synth_in, (uint16_t)(si + 0x1585), objs[i].flags);
        ft_wr16(g_synth_in, (uint16_t)(si + 0x173D), objs[i].x);
        ft_wr16(g_synth_in, (uint16_t)(si + 0x14BD), objs[i].hw);
        ft_wr16(g_synth_in, (uint16_t)(si + 0x1765), objs[i].y);
        ft_wr16(g_synth_in, (uint16_t)(si + 0x1495), objs[i].hh);
    }
    ft_wr16(g_synth_in, 0x34, 0xBBBB); ft_wr16(g_synth_in, 0x36, 0xBBBB);
    ft_wr16(g_synth_in, 0x38, 0xBBBB); ft_wr16(g_synth_in, 0x3A, 0xBBBB);

    memcpy(g_synth_orig, g_synth_in, sizeof(g_synth_orig));
    uint16_t regs[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    v2_fntest_orig_isolated(v2_fntest_orig_fnptr(FT_SUB_13C0C), g_synth_orig, regs);

    memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
    v2_fntest_call_sub_13c0c(g_scratch);

    long diffs = 0;
    for (uint32_t a = 0; a < 0x10000; a++) {
        if (g_scratch[a] == g_synth_orig[a]) continue;
        if (v2_fntest_ds_skip(a)) continue;
        if (diff_budget > 0) {
            diff_budget--;
            fprintf(stderr, "FNSELFTEST-DIFF[sub_13c0c %s]: addr=%04X orig=%02X v2=%02X (in=%02X) "
                    "| vpx=%04X vpy=%04X n=%d\n",
                    group, a, g_synth_orig[a], g_scratch[a], g_synth_in[a], vpx, vpy, n_objs);
        }
        diffs++;
    }
    if (diffs) { st.fail++; return false; }
    st.pass++; return true;
}

int ft_selftest_sub_13c0c() {
    FtSynthStats grid, exh, fuzz;
    long diff_budget = 24;

    // Objects sitting exactly on each bound for a mid-screen viewport, plus
    // dead / permanent(0x800) / already-0x200 ones.
    static const FtCullObj OBJS_MID[] = {
        { 1, 0x0000, 0x0100, 0x10, 0x0100, 0x10 },  // inside
        { 1, 0x0000, 0x0000, 0x00, 0x0100, 0x10 },  // left of ds:0x34
        { 1, 0x0000, 0x7FFF, 0x00, 0x0100, 0x10 },  // right of ds:0x36
        { 1, 0x0000, 0x0100, 0x10, 0x0000, 0x00 },  // above ds:0x38
        { 1, 0x0000, 0x0100, 0x10, 0x7FFF, 0x00 },  // below ds:0x3A
        { 0, 0x0000, 0x0000, 0x00, 0x0000, 0x00 },  // dead — skipped
        { 1, 0x0800, 0x0000, 0x00, 0x0000, 0x00 },  // permanent — skipped
        { 1, 0x0200, 0x0100, 0x10, 0x0100, 0x10 },  // already marked, inside
    };
    static const uint16_t VPS[] = { 0x0000, 0x000F, 0x0010, 0x0011, 0x0100,
                                    0x8000, 0xFFF0, 0xFFFF };
    for (uint16_t vx : VPS)
        for (uint16_t vy : VPS)
            ft_synth_case_13c0c(vx, vy, OBJS_MID, 8, "grid", grid, diff_budget);

    // Exhaustive sweeps of both viewport axes (the Y-clamp asymmetry corner
    // lives entirely in vpy < 0x10 / negative territory).
    for (uint32_t v = 0; v <= 0xFFFF; v++)
        ft_synth_case_13c0c(0x0100, (uint16_t)v, OBJS_MID, 5, "exh-vpy", exh, diff_budget);
    for (uint32_t v = 0; v <= 0xFFFF; v++)
        ft_synth_case_13c0c((uint16_t)v, 0x0100, OBJS_MID, 5, "exh-vpx", exh, diff_budget);

    FtRng rng(0x13C0C001);
    FtCullObj fo[16];
    for (int i = 0; i < 20000; i++) {
        int n = (int)(rng.next() % 16);
        for (int k = 0; k < n; k++)
            fo[k] = { (uint16_t)(rng.next() & 1), rng.w(), rng.w(), rng.w(), rng.w(), rng.w() };
        ft_synth_case_13c0c(rng.w(), rng.w(), fo, n, "fuzz", fuzz, diff_budget);
    }

    fprintf(stderr,
        "FNSELFTEST-SUMMARY[sub_13c0c]: grid %ld/%ld, exhaustive %ld/%ld, fuzz %ld/%ld — "
        "total cases=%ld fail=%ld%s\n",
        grid.pass, grid.cases, exh.pass, exh.cases, fuzz.pass, fuzz.cases,
        grid.cases + exh.cases + fuzz.cases, grid.fail + exh.fail + fuzz.fail,
        (grid.fail + exh.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (grid.fail + exh.fail + fuzz.fail) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Camera/scroll cluster. Shared DS layout of one case:
//   vp ds:0x44/0x46, gates ds:0x394/0x396, limits ds:0x25A4/0x25A6,
//   outputs ds:0x257F/0x2581 (vp>>3) and ds:0x34E/0x350 (delta) — canaried.
// sub_17496/sub_1746c take the speed in SI (io_regs[4] for the oracle).
// sub_10704/10753 read pending amounts ds:0x3D8/0x3DA/0x3DE/0x3DC and a speed
// table at 0x2B82/0x2B80; sub_1064b computes the amounts from the active
// viking's position and uses table 0x2B84 (movers covered transitively).
void ft_cam_base(uint16_t vpx, uint16_t vpy, uint16_t gx, uint16_t gy,
                 uint16_t limx, uint16_t limy)
{
    memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
    ft_wr16(g_synth_in, 0x44, vpx);   ft_wr16(g_synth_in, 0x46, vpy);
    ft_wr16(g_synth_in, 0x394, gx);   ft_wr16(g_synth_in, 0x396, gy);
    ft_wr16(g_synth_in, 0x25A4, limx); ft_wr16(g_synth_in, 0x25A6, limy);
    ft_wr16(g_synth_in, 0x257F, 0xBBBB); ft_wr16(g_synth_in, 0x2581, 0xBBBB);
    ft_wr16(g_synth_in, 0x34E, 0xBBBB);  ft_wr16(g_synth_in, 0x350, 0xBBBB);
}

bool ft_cam_run(FtId id, uint16_t si_reg, const char* group,
                FtSynthStats& st, long& diff_budget)
{
    st.cases++;
    memcpy(g_synth_orig, g_synth_in, sizeof(g_synth_orig));
    uint16_t regs[8] = { 0, 0, 0, 0, si_reg, 0, 0, 0 };
    v2_fntest_orig_isolated(v2_fntest_orig_fnptr(id), g_synth_orig, regs);

    memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
    switch (id) {
    case FT_SUB_1064B: v2_fntest_call_sub_1064b(g_scratch); break;
    case FT_SUB_10704: v2_fntest_call_sub_10704(g_scratch); break;
    case FT_SUB_10753: v2_fntest_call_sub_10753(g_scratch); break;
    case FT_SUB_17496: v2_fntest_call_sub_17496(g_scratch, si_reg); break;
    case FT_SUB_1746C: v2_fntest_call_sub_1746c(g_scratch, si_reg); break;
    default: break;
    }

    long diffs = 0;
    for (uint32_t a = 0; a < 0x10000; a++) {
        if (g_scratch[a] == g_synth_orig[a]) continue;
        if (v2_fntest_ds_skip(a)) continue;
        if (diff_budget > 0) {
            diff_budget--;
            fprintf(stderr, "FNSELFTEST-DIFF[%s %s]: addr=%04X orig=%02X v2=%02X (in=%02X) si=%04X\n",
                    g_name[id], group, a, g_synth_orig[a], g_scratch[a], g_synth_in[a], si_reg);
        }
        diffs++;
    }
    if (diffs) { st.fail++; return false; }
    st.pass++; return true;
}

int ft_selftest_mover(FtId id, uint32_t fuzz_seed) {
    FtSynthStats grid, exh, fuzz;
    long diff_budget = 24;
    static const uint16_t VPS[]  = { 0, 1, 0xF, 0x10, 0x100, 0x7FF0, 0x7FFF, 0x8000, 0xFFF0, 0xFFFF };
    static const uint16_t SPDS[] = { 0, 1, 2, 8, 0x10, 0x7FFF, 0x8000, 0xFFFF };
    static const uint16_t LIMS[] = { 0, 0x100, 0x7FFF, 0x8000, 0xFFFF };
    for (uint16_t vp : VPS) for (uint16_t sp : SPDS) for (uint16_t lim : LIMS) {
        ft_cam_base(vp, vp, 0, 0, lim, lim);
        ft_cam_run(id, sp, "grid", grid, diff_budget);
    }
    // gate set → mover must be a no-op (canaries stay)
    ft_cam_base(0x100, 0x100, 1, 1, 0x1000, 0x1000);
    ft_cam_run(id, 8, "grid", grid, diff_budget);
    // exhaustive speed sweep at two viewport positions
    for (uint32_t sp = 0; sp <= 0xFFFF; sp++) {
        ft_cam_base(0x0100, 0x0100, 0, 0, 0x2000, 0x2000);
        ft_cam_run(id, (uint16_t)sp, "exh-speed", exh, diff_budget);
    }
    // exhaustive viewport sweep at fixed speed
    for (uint32_t vp = 0; vp <= 0xFFFF; vp++) {
        ft_cam_base((uint16_t)vp, (uint16_t)vp, 0, 0, 0x8000, 0x8000);
        ft_cam_run(id, 8, "exh-vp", exh, diff_budget);
    }
    FtRng rng(fuzz_seed);
    for (int i = 0; i < 20000; i++) {
        ft_cam_base(rng.w(), rng.w(), (uint16_t)(rng.next() & 1), (uint16_t)(rng.next() & 1),
                    rng.w(), rng.w());
        ft_cam_run(id, rng.w(), "fuzz", fuzz, diff_budget);
    }
    fprintf(stderr,
        "FNSELFTEST-SUMMARY[%s]: grid %ld/%ld, exhaustive %ld/%ld, fuzz %ld/%ld — "
        "total cases=%ld fail=%ld%s\n",
        g_name[id], grid.pass, grid.cases, exh.pass, exh.cases, fuzz.pass, fuzz.cases,
        grid.cases + exh.cases + fuzz.cases, grid.fail + exh.fail + fuzz.fail,
        (grid.fail + exh.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (grid.fail + exh.fail + fuzz.fail) ? 1 : 0;
}

int ft_selftest_scroll_apply(FtId id, uint16_t table_off, uint32_t fuzz_seed) {
    FtSynthStats grid, fuzz;
    long diff_budget = 24;
    // Speed table entries for amounts 0..0x10 (and negative-wrapped indices in
    // fuzz reach arbitrary DS bytes — same for both sides by construction).
    auto fill_table = [&](uint16_t base_val) {
        for (uint16_t a = 0; a <= 0x10; a++)
            ft_wr16(g_synth_in, (uint16_t)(a * 2 + table_off), (uint16_t)(base_val + a));
    };
    static const uint16_t AMTS[] = { 0, 1, 2, 0x10, 0x7FFF, 0x8000, 0xFFFF };
    // All priority combos: left vs right (left wins), up vs down (up wins).
    for (uint16_t l : AMTS) for (uint16_t r : AMTS) {
        ft_cam_base(0x0400, 0x0400, 0, 0, 0x2000, 0x2000);
        fill_table(3);
        ft_wr16(g_synth_in, 0x3D8, l); ft_wr16(g_synth_in, 0x3DA, r);
        ft_wr16(g_synth_in, 0x3DE, (uint16_t)(l ^ 1)); ft_wr16(g_synth_in, 0x3DC, r);
        ft_cam_run(id, 0, "grid", grid, diff_budget);
    }
    FtRng rng(fuzz_seed);
    for (int i = 0; i < 30000; i++) {
        ft_cam_base(rng.w(), rng.w(), (uint16_t)(rng.next() & 1), (uint16_t)(rng.next() & 1),
                    rng.w(), rng.w());
        fill_table(rng.w());
        // amounts mostly in-range (table hits), occasionally wild (wrapped index)
        auto amt = [&]() -> uint16_t {
            uint32_t x = rng.next();
            return (x & 0xF0) ? (uint16_t)(x & 0x1F) : rng.w();
        };
        ft_wr16(g_synth_in, 0x3D8, amt()); ft_wr16(g_synth_in, 0x3DA, amt());
        ft_wr16(g_synth_in, 0x3DE, amt()); ft_wr16(g_synth_in, 0x3DC, amt());
        ft_cam_run(id, 0, "fuzz", fuzz, diff_budget);
    }
    fprintf(stderr,
        "FNSELFTEST-SUMMARY[%s]: grid %ld/%ld, fuzz %ld/%ld — total cases=%ld fail=%ld%s\n",
        g_name[id], grid.pass, grid.cases, fuzz.pass, fuzz.cases,
        grid.cases + fuzz.cases, grid.fail + fuzz.fail,
        (grid.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (grid.fail + fuzz.fail) ? 1 : 0;
}

int ft_selftest_sub_1064b() {
    FtSynthStats grid, exh, fuzz;
    long diff_budget = 24;
    // One case: viking di=0 at (ox,oy), viewport at (vpx,vpy), table 0x2B84
    // filled 1..0x11, pending-amount fields canaried (function zeroes them).
    auto build = [&](uint16_t vpx, uint16_t vpy, uint16_t ox, uint16_t oy) {
        ft_cam_base(vpx, vpy, 0, 0, 0x4000, 0x4000);
        ft_wr16(g_synth_in, 0x3C2, 0);                       // active viking slot 0
        ft_wr16(g_synth_in, 0x173D, ox);
        ft_wr16(g_synth_in, 0x1765, oy);
        for (uint16_t a = 0; a <= 0x10; a++)
            ft_wr16(g_synth_in, (uint16_t)(a * 2 + 0x2B84), (uint16_t)(a + 1));
        ft_wr16(g_synth_in, 0x3D8, 0xBBBB); ft_wr16(g_synth_in, 0x3DA, 0xBBBB);
        ft_wr16(g_synth_in, 0x3DE, 0xBBBB); ft_wr16(g_synth_in, 0x3DC, 0xBBBB);
    };
    // Directed: dead zone, both follow directions, clamp corners, overflow zone.
    static const uint16_t POS[] = { 0, 0x40, 0x90, 0xA0, 0xB0, 0x100, 0x7FF0,
                                    0x7FFF, 0x8000, 0x8080, 0xFFF0, 0xFFFF };
    for (uint16_t vp : POS) for (uint16_t obj : POS) {
        build(vp, 0x0200, obj, 0x0230);   // X varies, Y in dead zone
        ft_cam_run(FT_SUB_1064B, 0, "grid", grid, diff_budget);
        build(0x0200, vp, 0x0290, obj);   // Y varies, X in dead zone
        ft_cam_run(FT_SUB_1064B, 0, "grid", grid, diff_budget);
    }
    // Exhaustive: each axis coordinate swept fully, the other held mid-range.
    for (uint32_t v = 0; v <= 0xFFFF; v++) {
        build((uint16_t)v, 0x0200, 0x0290, 0x0230);
        ft_cam_run(FT_SUB_1064B, 0, "exh-vpx", exh, diff_budget);
    }
    for (uint32_t v = 0; v <= 0xFFFF; v++) {
        build(0x0200, 0x0200, (uint16_t)v, 0x0230);
        ft_cam_run(FT_SUB_1064B, 0, "exh-objx", exh, diff_budget);
    }
    for (uint32_t v = 0; v <= 0xFFFF; v++) {
        build(0x0200, (uint16_t)v, 0x0290, 0x0230);
        ft_cam_run(FT_SUB_1064B, 0, "exh-vpy", exh, diff_budget);
    }
    for (uint32_t v = 0; v <= 0xFFFF; v++) {
        build(0x0200, 0x0200, 0x0290, (uint16_t)v);
        ft_cam_run(FT_SUB_1064B, 0, "exh-objy", exh, diff_budget);
    }
    FtRng rng(0x1064B001);
    for (int i = 0; i < 20000; i++) {
        build(rng.w(), rng.w(), rng.w(), rng.w());
        uint16_t di = (uint16_t)((rng.next() % 3) * 2);      // slots 0/2/4
        ft_wr16(g_synth_in, 0x3C2, di);
        ft_wr16(g_synth_in, (uint16_t)(di + 0x173D), rng.w());
        ft_wr16(g_synth_in, (uint16_t)(di + 0x1765), rng.w());
        ft_cam_run(FT_SUB_1064B, 0, "fuzz", fuzz, diff_budget);
    }
    fprintf(stderr,
        "FNSELFTEST-SUMMARY[sub_1064b]: grid %ld/%ld, exhaustive %ld/%ld, fuzz %ld/%ld — "
        "total cases=%ld fail=%ld%s\n",
        grid.pass, grid.cases, exh.pass, exh.cases, fuzz.pass, fuzz.cases,
        grid.cases + exh.cases + fuzz.cases, grid.fail + exh.fail + fuzz.fail,
        (grid.fail + exh.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (grid.fail + exh.fail + fuzz.fail) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// sub_101be: palette-animation timers + table rotation.
// Contract (verified line-by-line, orig eips 0x1BE-0x20E + callees sub_10255
// eips 0x255-0x2xx / sub_1020f 0x20F-0x254):
//   reads  mask ds:0x2583; per channel si=7..0: enable byte at
//          (uint16_t)(si-0x6C44) (= 0x93BC+si), timer [si+0x258C],
//          cur [si+0x259C], end [si+0x2594]; palette tables 0x8202/0x7F02
//   writes timer DEC; on 0: rotates BOTH tables' entry ranges (3-byte RGB,
//          REP MOVSB up/down by cur<=>end), scratch ds:0x8504-0x8506; after a
//          full non-early-exit pass: ds:0x7EFE = 2. Canaries on scratch+7EFE.
bool ft_synth_case_101be(uint16_t mask, const uint8_t* en8, const uint8_t* tm8,
                         const uint8_t* cur8, const uint8_t* end8, uint32_t tbl_seed,
                         const char* group, FtSynthStats& st, long& diff_budget)
{
    st.cases++;
    memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
    g_synth_in[0x2583] = (uint8_t)mask;
    for (int i = 0; i < 8; i++) {
        g_synth_in[(uint16_t)(i - 0x6C44)] = en8[i];
        g_synth_in[(uint16_t)(i + 0x258C)] = tm8[i];
        g_synth_in[(uint16_t)(i + 0x259C)] = cur8[i];
        g_synth_in[(uint16_t)(i + 0x2594)] = end8[i];
    }
    FtRng trng(tbl_seed);
    for (uint32_t off = 0; off < 0x300; off++) {        // 256 entries x 3 bytes
        g_synth_in[(uint16_t)(0x8202 + off)] = (uint8_t)trng.next();
        g_synth_in[(uint16_t)(0x7F02 + off)] = (uint8_t)trng.next();
    }
    ft_wr16(g_synth_in, 0x8504, 0xBBBB);
    g_synth_in[0x8506] = 0xBB;
    ft_wr16(g_synth_in, 0x7EFE, 0xBBBB);

    memcpy(g_synth_orig, g_synth_in, sizeof(g_synth_orig));
    uint16_t regs[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    v2_fntest_orig_isolated(v2_fntest_orig_fnptr(FT_SUB_101BE), g_synth_orig, regs);

    memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
    v2_fntest_call_sub_101be(g_scratch);

    long diffs = 0;
    for (uint32_t a = 0; a < 0x10000; a++) {
        if (g_scratch[a] == g_synth_orig[a]) continue;
        if (v2_fntest_ds_skip(a)) continue;
        if (diff_budget > 0) {
            diff_budget--;
            fprintf(stderr, "FNSELFTEST-DIFF[sub_101be %s]: addr=%04X orig=%02X v2=%02X (in=%02X) "
                    "| mask=%02X cur0=%02X end0=%02X tm0=%02X\n",
                    group, a, g_synth_orig[a], g_scratch[a], g_synth_in[a],
                    (uint8_t)mask, cur8[0], end8[0], tm8[0]);
        }
        diffs++;
    }
    if (diffs) { st.fail++; return false; }
    st.pass++; return true;
}

int ft_selftest_sub_101be() {
    FtSynthStats grid, exh, fuzz;
    long diff_budget = 24;
    uint8_t en[8], tm[8], cur[8], end[8];

    // Directed: mask off / mask-vs-enable misses / timer chains / rotation
    // both directions on every channel simultaneously.
    static const uint8_t MASKS[] = { 0x00, 0x01, 0x80, 0xFF };
    for (uint8_t m : MASKS) {
        for (int i = 0; i < 8; i++) { en[i] = (uint8_t)(1 << i); tm[i] = (uint8_t)(i % 3); }
        for (int i = 0; i < 8; i++) { cur[i] = (uint8_t)(0x10 + i); end[i] = (uint8_t)(0x14 - i); }
        ft_synth_case_101be(m, en, tm, cur, end, 0xA1000000u + m, "grid", grid, diff_budget);
        for (int i = 0; i < 8; i++) { en[i] = 0xFF; tm[i] = 1; }        // all fire
        ft_synth_case_101be(m, en, tm, cur, end, 0xA2000000u + m, "grid", grid, diff_budget);
        for (int i = 0; i < 8; i++) tm[i] = 0;                          // all zero timers
        ft_synth_case_101be(m, en, tm, cur, end, 0xA3000000u + m, "grid", grid, diff_budget);
    }

    // Exhaustive: channel 0 cur x end full 256x256 (timer=1, fires each case).
    for (uint32_t c = 0; c <= 0xFF; c++) for (uint32_t e = 0; e <= 0xFF; e++) {
        for (int i = 0; i < 8; i++) { en[i] = 0; tm[i] = 0; cur[i] = 0; end[i] = 0; }
        en[0] = 0x01; tm[0] = 1; cur[0] = (uint8_t)c; end[0] = (uint8_t)e;
        ft_synth_case_101be(0x01, en, tm, cur, end, 0xE0000000u | (c << 8) | e,
                            "exh-curXend", exh, diff_budget);
    }

    FtRng rng(0x101BE001);
    for (int i = 0; i < 20000; i++) {
        uint16_t m = (uint16_t)(rng.next() & 0xFF);
        for (int k = 0; k < 8; k++) {
            en[k] = (uint8_t)rng.next(); tm[k] = (uint8_t)(rng.next() & 3);
            cur[k] = (uint8_t)rng.next(); end[k] = (uint8_t)rng.next();
        }
        ft_synth_case_101be(m, en, tm, cur, end, rng.next(), "fuzz", fuzz, diff_budget);
    }

    fprintf(stderr,
        "FNSELFTEST-SUMMARY[sub_101be]: grid %ld/%ld, exhaustive %ld/%ld, fuzz %ld/%ld — "
        "total cases=%ld fail=%ld%s\n",
        grid.pass, grid.cases, exh.pass, exh.cases, fuzz.pass, fuzz.cases,
        grid.cases + exh.cases + fuzz.cases, grid.fail + exh.fail + fuzz.fail,
        (grid.fail + exh.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (grid.fail + exh.fail + fuzz.fail) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// sub_10255 / sub_1020f <-> v2_sub_10255 / v2_sub_1020f (palette entry
// rotates, the two callees of sub_101be — registered as standalone units).
// Contract: si = slot (byte fields [si+0x259C]=cur, [si+0x2594]=end),
// dx = table base. Both REP MOVSB with a WRAPPED uint16 count, so the
// "wrong-order" half-plane (10255: end<cur, 1020f: cur<end) is orig-UB-ish
// but fully deterministic memory-wise (64KB wrap) — compared exactly.

bool ft_synth_case_palrot(FtId id, uint16_t si, uint16_t dx,
                          uint8_t cur, uint8_t endi, uint32_t tbl_seed,
                          const char* group, FtSynthStats& st, long& diff_budget)
{
    st.cases++;
    memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
    g_synth_in[(uint16_t)(si + 0x259C)] = cur;
    g_synth_in[(uint16_t)(si + 0x2594)] = endi;
    FtRng trng(tbl_seed);
    for (uint32_t off = 0; off < 0x300; off++) {        // 256 entries x 3 bytes
        g_synth_in[(uint16_t)(0x8202 + off)] = (uint8_t)trng.next();
        g_synth_in[(uint16_t)(0x7F02 + off)] = (uint8_t)trng.next();
    }
    ft_wr16(g_synth_in, 0x8504, 0xBBBB);                // scratch statics canary
    g_synth_in[0x8506] = 0xBB;

    memcpy(g_synth_orig, g_synth_in, sizeof(g_synth_orig));
    uint16_t regs[8] = { 0, 0, 0, dx, si, 0, 0, 0 };
    long esc0 = ft_ub_marks();
    v2_fntest_orig_isolated(v2_fntest_orig_fnptr(id), g_synth_orig, regs);
    if (ft_ub_marks() != esc0) {
        st.cases--;                                      // not comparable (unexpected here)
        fprintf(stderr, "FNSELFTEST-UB[%s %s]: si=%u dx=%04X cur=%02X end=%02X escaped\n",
                g_name[id], group, si, dx, cur, endi);
        return true;
    }

    memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
    if (id == FT_SUB_10255) v2_fntest_call_sub_10255(g_scratch, si, dx);
    else                    v2_fntest_call_sub_1020f(g_scratch, si, dx);

    long diffs = 0;
    for (uint32_t a = 0; a < 0x10000; a++) {
        if (g_scratch[a] == g_synth_orig[a]) continue;
        if (v2_fntest_ds_skip(a)) continue;
        if (diff_budget > 0) {
            diff_budget--;
            fprintf(stderr, "FNSELFTEST-DIFF[%s %s]: addr=%04X orig=%02X v2=%02X (in=%02X) "
                    "| si=%u dx=%04X cur=%02X end=%02X\n",
                    g_name[id], group, a, g_synth_orig[a], g_scratch[a], g_synth_in[a],
                    si, dx, cur, endi);
        }
        diffs++;
    }
    if (diffs) { st.fail++; return false; }
    st.pass++; return true;
}

int ft_selftest_palrot(FtId id) {
    FtSynthStats exh, wrap, fuzz;
    long diff_budget = 24;
    const bool fwd = (id == FT_SUB_10255);   // 10255: valid half-plane cur<=end

    // Exhaustive valid half-plane at si=0, dx=0x8202 (game base).
    for (uint32_t c = 0; c <= 0xFF; c++) for (uint32_t e = 0; e <= 0xFF; e++) {
        if (fwd ? (c > e) : (c < e)) continue;
        ft_synth_case_palrot(id, 0, 0x8202, (uint8_t)c, (uint8_t)e,
                             0xC0000000u | (c << 8) | e, "exh", exh, diff_budget);
    }
    // Slot addressing: si=0..7 over a small cur/end grid, both game bases.
    for (uint16_t si = 0; si < 8; si++)
        for (uint32_t c = 0; c <= 0xF0; c += 0x3C) for (uint32_t e = 0; e <= 0xF0; e += 0x3C) {
            if (fwd ? (c > e) : (c < e)) continue;
            ft_synth_case_palrot(id, si, 0x7F02, (uint8_t)c, (uint8_t)e,
                                 0xC1000000u | (si << 16) | (c << 8) | e, "exh", exh, diff_budget);
        }

    // Wrong-order half-plane (wrapped REP MOVSB count) — sampled diagonals.
    static const uint8_t DELTA[] = { 1, 2, 3, 5, 17, 85, 255 };
    for (uint8_t d : DELTA) for (uint32_t c = 0; c <= 0xF0; c += 0x10) {
        uint8_t cur = (uint8_t)c, endi;
        if (fwd) endi = (uint8_t)(cur - d);   // end<cur → wrapped count for 10255
        else     endi = (uint8_t)(cur + d);   // cur<end → wrapped count for 1020f
        ft_synth_case_palrot(id, (uint16_t)(c & 7), 0x8202, cur, endi,
                             0xC2000000u | (d << 8) | c, "wrap", wrap, diff_budget);
    }
    // Base-address wrap: dx near segment end / zero.
    static const uint16_t DXS[] = { 0x0000, 0xFF00, 0xFFFD };
    for (uint16_t dxv : DXS) for (uint32_t c = 0; c <= 0xFF; c += 0x33) {
        uint8_t cur = (uint8_t)c, endi = (uint8_t)(fwd ? (uint8_t)(c + 9) : (uint8_t)(c - 9));
        ft_synth_case_palrot(id, 3, dxv, cur, endi,
                             0xC3000000u | ((uint32_t)dxv << 8) | c, "wrap", wrap, diff_budget);
    }

    // Fuzz: random everything (both game bases).
    FtRng rng(fwd ? 0x10255001u : 0x1020F001u);
    for (int i = 0; i < 5000; i++) {
        uint16_t si = (uint16_t)(rng.next() & 7);
        uint16_t dxv = (rng.next() & 1) ? 0x8202 : 0x7F02;
        ft_synth_case_palrot(id, si, dxv, (uint8_t)rng.next(), (uint8_t)rng.next(),
                             rng.next(), "fuzz", fuzz, diff_budget);
    }

    fprintf(stderr,
        "FNSELFTEST-SUMMARY[%s]: exhaustive %ld/%ld, wrap %ld/%ld, fuzz %ld/%ld — "
        "total cases=%ld fail=%ld%s\n",
        g_name[id], exh.pass, exh.cases, wrap.pass, wrap.cases, fuzz.pass, fuzz.cases,
        exh.cases + wrap.cases + fuzz.cases, exh.fail + wrap.fail + fuzz.fail,
        (exh.fail + wrap.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (exh.fail + wrap.fail + fuzz.fail) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// sub_12fc6 / sub_12fcb / sub_12fd0 <-> v2_sub_12fc6/12fcb/12fd0 (sub-sprite
// catch-up passes; delta fns sub_1227e/sub_122c0/sub_122f3 via off_30BC0).
// Contract: walk objects [ds:372h]-2 down to 0 — DO-WHILE: with [372h]==0 the
// first call still runs with di=0xFFFE (gates then read wrapped addresses).
// Per object (sub_12fe5): gates [di+1355h]!=0, [di+1AD5h]!=0; deltas from
// [1765]-[13CD] (Y) and [173D]-[13A5] (X) through the type's delta fn;
// (tx|ty)==0 → skip; else DO-WHILE over slots [1A85..1AAD): [64D]+=tx,
// [74D]+=ty, [114D]=0x202.

struct FtDeltaObj {
    uint16_t alive, count;          // [1355], [1AD5]
    uint16_t x, xs, y, ys;          // [173D], [13A5], [1765], [13CD]
    uint16_t sa, se;                // [1A85], [1AAD]
};

bool ft_synth_case_deltafam(FtId id, uint16_t obj_top, const FtDeltaObj* objs, int n_objs,
                            uint32_t bg_seed, const char* group,
                            FtSynthStats& st, long& diff_budget)
{
    st.cases++;
    memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
    ft_wr16(g_synth_in, 0x372, obj_top);
    for (int i = 0; i < n_objs; i++) {
        uint16_t di = (uint16_t)(i * 2);
        ft_wr16(g_synth_in, (uint16_t)(di + 0x1355), objs[i].alive);
        ft_wr16(g_synth_in, (uint16_t)(di + 0x1AD5), objs[i].count);
        ft_wr16(g_synth_in, (uint16_t)(di + 0x173D), objs[i].x);
        ft_wr16(g_synth_in, (uint16_t)(di + 0x13A5), objs[i].xs);
        ft_wr16(g_synth_in, (uint16_t)(di + 0x1765), objs[i].y);
        ft_wr16(g_synth_in, (uint16_t)(di + 0x13CD), objs[i].ys);
        ft_wr16(g_synth_in, (uint16_t)(di + 0x1A85), objs[i].sa);
        ft_wr16(g_synth_in, (uint16_t)(di + 0x1AAD), objs[i].se);
    }
    // Slot-area background noise so += effects are visible on any slot.
    FtRng bg(bg_seed);
    for (uint32_t a = 0; a < 0x100; a += 2) {
        ft_wr16(g_synth_in, (uint16_t)(a + 0x64D), bg.w());
        ft_wr16(g_synth_in, (uint16_t)(a + 0x74D), bg.w());
        ft_wr16(g_synth_in, (uint16_t)(a + 0x114D), bg.w());
    }

    memcpy(g_synth_orig, g_synth_in, sizeof(g_synth_orig));
    uint16_t regs[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    long esc0 = ft_ub_marks();
    v2_fntest_orig_isolated(v2_fntest_orig_fnptr(id), g_synth_orig, regs);
    if (ft_ub_marks() != esc0) {
        st.cases--;
        fprintf(stderr, "FNSELFTEST-UB[%s %s]: top=%04X escaped\n", g_name[id], group, obj_top);
        return true;
    }

    memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
    if (id == FT_SUB_12FC6)      v2_fntest_call_sub_12fc6(g_scratch);
    else if (id == FT_SUB_12FCB) v2_fntest_call_sub_12fcb(g_scratch);
    else                         v2_fntest_call_sub_12fd0(g_scratch);

    long diffs = 0;
    for (uint32_t a = 0; a < 0x10000; a++) {
        if (g_scratch[a] == g_synth_orig[a]) continue;
        if (v2_fntest_ds_skip(a)) continue;
        if (diff_budget > 0) {
            diff_budget--;
            fprintf(stderr, "FNSELFTEST-DIFF[%s %s]: addr=%04X orig=%02X v2=%02X (in=%02X) "
                    "| top=%04X o0={x=%04X xs=%04X y=%04X ys=%04X sa=%04X se=%04X}\n",
                    g_name[id], group, a, g_synth_orig[a], g_scratch[a], g_synth_in[a],
                    obj_top, n_objs ? objs[0].x : 0, n_objs ? objs[0].xs : 0,
                    n_objs ? objs[0].y : 0, n_objs ? objs[0].ys : 0,
                    n_objs ? objs[0].sa : 0, n_objs ? objs[0].se : 0);
        }
        diffs++;
    }
    if (diffs) { st.fail++; return false; }
    st.pass++; return true;
}

int ft_selftest_deltafam(FtId id, uint32_t seed) {
    FtSynthStats grid, exh, fuzz;
    long diff_budget = 24;

    // Directed: delta boundaries on one object, incl. the IDIV edge d=-0x8000
    // (orig negative branch divides 0:|d| with DX=0, not CWD) and the empty/
    // inverted slot ranges (slot loop is a DO-WHILE).
    static const uint16_t DX[] = { 0, 1, 2, 3, 4, 5, 6, 0xFFFF, 0xFFFE, 0xFFFD,
                                   0xFFFC, 0xFFFB, 0x7FFF, 0x8000, 0x8001 };
    for (uint16_t dxv : DX) for (uint16_t dyv : DX) {
        FtDeltaObj o = { 1, 1, (uint16_t)(0x100 + dxv), 0x100, (uint16_t)(0x200 + dyv), 0x200,
                         0x48, 0x4C };
        ft_synth_case_deltafam(id, 2, &o, 1, seed ^ (dxv << 16) ^ dyv, "grid", grid, diff_budget);
    }
    {   // empty range (sa==se), inverted range (sa>se), count==0 gate, dead obj
        FtDeltaObj o1 = { 1, 1, 0x105, 0x100, 0x200, 0x200, 0x48, 0x48 };
        ft_synth_case_deltafam(id, 2, &o1, 1, seed + 1, "grid", grid, diff_budget);
        FtDeltaObj o2 = { 1, 1, 0x105, 0x100, 0x200, 0x200, 0x4C, 0x48 };
        ft_synth_case_deltafam(id, 2, &o2, 1, seed + 2, "grid", grid, diff_budget);
        FtDeltaObj o3 = { 1, 0, 0x105, 0x100, 0x200, 0x200, 0x48, 0x4C };
        ft_synth_case_deltafam(id, 2, &o3, 1, seed + 3, "grid", grid, diff_budget);
        FtDeltaObj o4 = { 0, 1, 0x105, 0x100, 0x200, 0x200, 0x48, 0x4C };
        ft_synth_case_deltafam(id, 2, &o4, 1, seed + 4, "grid", grid, diff_budget);
        // top==0: orig object walk is a DO-WHILE — one call with di=0xFFFE
        FtDeltaObj o5 = { 1, 1, 0x105, 0x100, 0x200, 0x200, 0x48, 0x4C };
        ft_synth_case_deltafam(id, 0, &o5, 1, seed + 5, "grid", grid, diff_budget);
        // three objects with different deltas
        FtDeltaObj m3[3] = {
            { 1, 1, 0x109, 0x100, 0x1F9, 0x200, 0x48, 0x4C },
            { 0, 1, 0x120, 0x100, 0x220, 0x200, 0x50, 0x54 },
            { 1, 2, 0x0FD, 0x100, 0x203, 0x200, 0x58, 0x5E },
        };
        ft_synth_case_deltafam(id, 6, m3, 3, seed + 6, "grid", grid, diff_budget);
    }

    // Exhaustive X-delta sweep (all 65536 values of [173D] against xs=0x8000
    // fixed midpoint) on one object — full coverage of both IDIV branches.
    for (uint32_t xv = 0; xv <= 0xFFFF; xv++) {
        FtDeltaObj o = { 1, 1, (uint16_t)xv, 0x8000, 0x200, 0x200, 0x48, 0x4C };
        ft_synth_case_deltafam(id, 2, &o, 1, seed ^ 0xE0000000u ^ xv, "exh", exh, diff_budget);
    }

    FtRng rng(seed);
    for (int i = 0; i < 15000; i++) {
        FtDeltaObj os[3];
        int n = 1 + (int)(rng.next() % 3);
        for (int k = 0; k < n; k++) {
            os[k].alive = rng.next() & 1;
            os[k].count = (uint16_t)(rng.next() & 3);
            os[k].x = rng.w(); os[k].xs = rng.w();
            os[k].y = rng.w(); os[k].ys = rng.w();
            uint16_t sa = (uint16_t)((rng.next() % 0x60) & ~1u);
            os[k].sa = sa;
            os[k].se = (uint16_t)((sa + ((rng.next() % 5) * 2)) & ~1u);
            if ((rng.next() & 7) == 0) { uint16_t t = os[k].sa; os[k].sa = os[k].se; os[k].se = t; }
        }
        ft_synth_case_deltafam(id, (uint16_t)(n * 2), os, n, rng.next(), "fuzz", fuzz, diff_budget);
    }

    fprintf(stderr,
        "FNSELFTEST-SUMMARY[%s]: grid %ld/%ld, exhaustive %ld/%ld, fuzz %ld/%ld — "
        "total cases=%ld fail=%ld%s\n",
        g_name[id], grid.pass, grid.cases, exh.pass, exh.cases, fuzz.pass, fuzz.cases,
        grid.cases + exh.cases + fuzz.cases, grid.fail + exh.fail + fuzz.fail,
        (grid.fail + exh.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (grid.fail + exh.fail + fuzz.fail) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// sub_158aa/158b9/158c8/158d7/158e6 <-> v2 search family (units 20-24).
// Each = tile stage (loc_15a93 core: sub_14199 lookup at world coords /16,
// filter chain at [f-0x6B34], X walk +0x10 clamped) then object stage
// (bounds vs every live object). Inputs: si=filter, di=obj. Output: CF.
// The tilemap is crafted INSIDE the 64KB test zone: [0x2E63]=TESTSEG,
// [0x25DC]=W (tiles), [0x25DE]=H, row offsets at [y*2-0x7098], tile words
// zone[row+x*2] with the type in bits 15..10.

bool ft_synth_case_search(FtId id, uint16_t filter, uint16_t obj,
                          const FtWr* w, int nw,
                          const uint16_t* tiles, int tw, int th,
                          uint32_t bg_seed, const char* group,
                          FtSynthStats& st, long& diff_budget)
{
    st.cases++;
    uint8_t* mbase = (uint8_t*)v2_fntest_m2c_base();
    uint8_t* zone = mbase + (uint32_t)FT_VM_TESTSEG * 16;
    memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
    ft_wr16(g_synth_in, 0x372, 8);
    for (uint32_t a = 0x2E5C; a <= 0x2E7C; a += 2) ft_wr16(g_synth_in, a, 0);
    ft_wr16(g_synth_in, 0x2E63, FT_VM_TESTSEG);          // tilemap segment
    ft_wr16(g_synth_in, 0x25DC, (uint16_t)tw);           // map width (tiles)
    ft_wr16(g_synth_in, 0x25DE, (uint16_t)th);           // map height
    for (int y = 0; y < th; y++)
        ft_wr16(g_synth_in, (uint16_t)(y * 2 - 0x7098), (uint16_t)(y * tw * 2)); // row offsets
    // canaries on the scratch protocol
    ft_wr16(g_synth_in, 0x34, 0xBBBB); ft_wr16(g_synth_in, 0x36, 0xBBBB);
    ft_wr16(g_synth_in, 0x38, 0xBBBB); ft_wr16(g_synth_in, 0x3A, 0xBBBB);
    ft_wr16(g_synth_in, 0x3B2, 0xBBBB); ft_wr16(g_synth_in, 0x3B4, 0xBBBB);
    FtRng bg(bg_seed);
    static const uint16_t SF[] = { 0x1535, 0x155D, 0x14E5, 0x150D, 0x1585, 0x17DD, 0x196D };
    for (uint16_t f : SF)
        ft_wr16(g_synth_in, (uint16_t)(obj + f), bg.w());
    for (int i = 0; i < nw; i++) ft_wr16(g_synth_in, w[i].addr, w[i].val);

    // Build the tile zone
    memset(g_vm_es_in, 0, sizeof(g_vm_es_in));
    for (int i = 0; i < tw * th; i++) {
        g_vm_es_in[i * 2]     = (uint8_t)(tiles[i] & 0xFF);
        g_vm_es_in[i * 2 + 1] = (uint8_t)(tiles[i] >> 8);
    }
    memcpy(zone, g_vm_es_in, FT_VM_ZONE);

    memcpy(g_synth_orig, g_synth_in, sizeof(g_synth_orig));
    uint16_t regs[8] = { 0, 0, 0, 0, filter, obj, 0, 0 };
    long esc0 = ft_ub_marks();
    v2_fntest_orig_isolated(v2_fntest_orig_fnptr(id), g_synth_orig, regs);
    memcpy(zone, g_vm_es_in, FT_VM_ZONE);                // zone is read-only here
    if (ft_ub_marks() != esc0) {
        st.cases--;
        fprintf(stderr, "FNSELFTEST-UB[%s %s]: filter=%04X seed=%08X escaped\n",
                g_name[id], group, filter, bg_seed);
        return true;
    }
    int cf_orig = regs[7] & 1;

    memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
    // v2 under its own watchdog: a mirror that spins forever (clamp jumped
    // over at an int16 boundary etc.) must surface as a FAIL, not a hang.
    int cf_v2 = -1; int v2_hung = 0;
    v2_fntest_arm_signals();
    if (sigsetjmp(*v2_fntest_jb(), 1) == 0) {
        v2_fntest_alarm_ms(4000);
        cf_v2 = v2_fntest_call_search(g_scratch, (int)(id - FT_SUB_158AA), filter, obj);
        v2_fntest_alarm_ms(0);
    } else {
        v2_fntest_alarm_ms(0);
        v2_hung = 1;
    }
    if (v2_hung) {
        fprintf(stderr, "FNSELFTEST-DIFF[%s %s]: v2 HUNG (watchdog) | filter=%04X obj=%04X seed=%08X"
                " | o6: fl=%04X X=[%04X..%04X] Y=[%04X..%04X] v=%04X cX=%04X"
                " | o2: alive=%04X t=%04X X=[%04X..%04X] Y=[%04X..%04X]\n",
                g_name[id], group, filter, obj, bg_seed,
                *(uint16_t*)(g_synth_in + obj + 0x1585),
                *(uint16_t*)(g_synth_in + obj + 0x1535), *(uint16_t*)(g_synth_in + obj + 0x155D),
                *(uint16_t*)(g_synth_in + obj + 0x14E5), *(uint16_t*)(g_synth_in + obj + 0x150D),
                *(uint16_t*)(g_synth_in + obj + 0x196D), *(uint16_t*)(g_synth_in + obj + 0x173D),
                *(uint16_t*)(g_synth_in + 2 + 0x1355), *(uint16_t*)(g_synth_in + 2 + 0x17DD),
                *(uint16_t*)(g_synth_in + 2 + 0x1535), *(uint16_t*)(g_synth_in + 2 + 0x155D),
                *(uint16_t*)(g_synth_in + 2 + 0x14E5), *(uint16_t*)(g_synth_in + 2 + 0x150D));
        st.fail++; return false;
    }

    long diffs = 0;
    if (cf_v2 != cf_orig) {
        if (diff_budget > 0) {
            diff_budget--;
            fprintf(stderr, "FNSELFTEST-DIFF[%s %s]: CF orig=%d v2=%d | filter=%04X obj=%04X\n",
                    g_name[id], group, cf_orig, cf_v2, filter, obj);
        }
        diffs++;
    }
    for (uint32_t a = 0; a < 0x10000; a++) {
        if (g_scratch[a] == g_synth_orig[a]) continue;
        if (v2_fntest_ds_skip(a)) continue;
        if (diff_budget > 0) {
            diff_budget--;
            fprintf(stderr, "FNSELFTEST-DIFF[%s %s]: addr=%04X orig=%02X v2=%02X (in=%02X) "
                    "| filter=%04X obj=%04X\n",
                    g_name[id], group, a, g_synth_orig[a], g_scratch[a], g_synth_in[a],
                    filter, obj);
        }
        diffs++;
    }
    if (diffs) { st.fail++; return false; }
    st.pass++; return true;
}

int ft_selftest_search(FtId id, uint32_t seed) {
    FtSynthStats grid, exh, fuzz;
    long diff_budget = 24;
    const uint16_t OBJ = 6;
    v2_set_m2c_base(v2_fntest_m2c_base());   // tilemap parity needs v2_m2c_base
    const bool shard0 = (g_shard_i == 0);    // grid+fuzz only once across shards
    // TEMP diag: FNSELFTEST_FZ_ONLY=<i> runs just fuzz case #i of this shard
    // (skips grid/exhaustive) — for isolating a hung case under perf.
    long fz_only = -1;
    if (const char* fo = getenv("FNSELFTEST_FZ_ONLY")) fz_only = atol(fo);
    // 4x4 tile map: type = tile>>10. Row 0: types 0,1,2,3; row 1: 4,5,6,7...
    uint16_t T[16];
    for (int i = 0; i < 16; i++) T[i] = (uint16_t)((i & 0x3F) << 10);
    // filter table at [filter-0x6B34]: filter=0 → address 0x94CC
    const uint16_t FT_ADDR = (uint16_t)(0 - 0x6B34);

    // Directed: tile-hit / obj-hit / none / both (tile stage wins), flip
    // variants, filter chain walk (match at 2nd entry), obj JS-edge bounds.
    struct SCase { uint16_t flags, xs, xe, ys, ye; uint16_t f0, f1;
                   uint16_t o2alive, o2t, o2xs, o2xe, o2ys, o2ye; };
    static const SCase SC[] = {
        // in-map bbox, tile type 5 under Y_end+1 probe (row1), filter matches 5 → tile hit
        { 0x8000, 0x0010, 0x0020, 0x0008, 0x000E, 0x0005, 0x00FF, 0, 0, 0, 0, 0, 0 },
        // filter stops before (first entry > type) → no tile; no obj → none
        { 0x8000, 0x0010, 0x0020, 0x0008, 0x000E, 0x0006, 0x00FF, 0, 0, 0, 0, 0, 0 },
        // filter chain: first entry smaller, second matches (INC walk)
        { 0x8000, 0x0010, 0x0020, 0x0008, 0x000E, 0x0004, 0x0005, 0, 0, 0, 0, 0, 0 },
        // out-of-map probe (X beyond W*16) → tile stage sees 0x400-type path; obj hit
        { 0x8000, 0x0100, 0x0200, 0x0100, 0x0140, 0x0000, 0x00FF,
          1, 0x0100, 0x00F0, 0x0300, 0x0100, 0x0200 },
        // obj bounds JS edge: target X range straddles 0x8000
        { 0x8000, 0x7FF0, 0x7FFF, 0x0100, 0x0140, 0x0000, 0x00FF,
          1, 0x0100, 0x8000, 0x8010, 0x0100, 0x0200 },
        // flip set (affects 158e6/158aa/158b9 axis choice)
        { 0x8040, 0x0010, 0x0020, 0x0008, 0x000E, 0x0005, 0x00FF, 0, 0, 0, 0, 0, 0 },
    };
    int ci = 0;
    if (shard0 && fz_only < 0) for (const SCase& c : SC) {
        FtWr w[16]; int nw = 0;
        w[nw++] = { (uint16_t)(OBJ + 0x1585), c.flags };
        w[nw++] = { (uint16_t)(OBJ + 0x1535), c.xs };
        w[nw++] = { (uint16_t)(OBJ + 0x155D), c.xe };
        w[nw++] = { (uint16_t)(OBJ + 0x14E5), c.ys };
        w[nw++] = { (uint16_t)(OBJ + 0x150D), c.ye };
        w[nw++] = { FT_ADDR, (uint16_t)(c.f0 | ((c.f1 & 0xFF) << 8)) };
        if (c.o2alive) {
            w[nw++] = { (uint16_t)(2 + 0x1355), FT_VM_TESTSEG };
            w[nw++] = { (uint16_t)(2 + 0x17DD), c.o2t };
            w[nw++] = { (uint16_t)(2 + 0x1535), c.o2xs };
            w[nw++] = { (uint16_t)(2 + 0x155D), c.o2xe };
            w[nw++] = { (uint16_t)(2 + 0x14E5), c.o2ys };
            w[nw++] = { (uint16_t)(2 + 0x150D), c.o2ye };
        }
        ft_synth_case_search(id, 0, OBJ, w, nw, T, 4, 4,
                             seed ^ (0xA0000000u + ci), "grid", grid, diff_budget);
        ci++;
    }

    // Exhaustive: the unit's own axis field over all 65536 values.
    uint16_t axis_field =
        (id == FT_SUB_158AA) ? (uint16_t)(OBJ + 0x1535) :
        (id == FT_SUB_158B9) ? (uint16_t)(OBJ + 0x155D) :
        (id == FT_SUB_158C8) ? (uint16_t)(OBJ + 0x14E5) :
        (id == FT_SUB_158D7) ? (uint16_t)(OBJ + 0x150D) :
                               (uint16_t)(OBJ + 0x155D);   // 158e6, flip=0 → X_end
    long exh_done = 0;
    if (fz_only < 0) for (uint32_t v = 0; v <= 0xFFFF; v++) {
        if (!ft_shard_mine(v)) continue;             // FNSELFTEST_SHARD split
        if ((++exh_done % 1000) == 0)
            fprintf(stderr, "FNSELFTEST-PROG[%s]: exh %ld (v=%04X)\n", g_name[id], exh_done, v);
        FtWr w[7] = {
            { (uint16_t)(OBJ + 0x1585), 0x8000 },
            { (uint16_t)(OBJ + 0x1535), 0x0010 },
            { (uint16_t)(OBJ + 0x155D), 0x0020 },
            { (uint16_t)(OBJ + 0x14E5), 0x0008 },
            { (uint16_t)(OBJ + 0x150D), 0x000E },
            { FT_ADDR, 0xFF05 },                     // match type 5, stop at 0xFF
            { axis_field, (uint16_t)v },
        };
        ft_synth_case_search(id, 0, OBJ, w, 7, T, 4, 4,
                             seed ^ 0xE0000000u ^ v, "exh", exh, diff_budget);
    }

    // Fuzz: random bbox/flags/filter-pair/second object. Sharded: each shard
    // runs its slice with its own deterministic stream (seed salted by shard).
    FtRng rng(seed ^ (0x51ED0000u * (uint32_t)(g_shard_i + 1)));
    int fz_count = 10000 / g_shard_n + (g_shard_i < (10000 % g_shard_n) ? 1 : 0);
    for (int i = 0; i < fz_count; i++) {
        if ((i % 100) == 0)
            fprintf(stderr, "FNSELFTEST-PROG[%s]: fuzz %d\n", g_name[id], i);
        bool fz_dbg = (i < 3);   // TEMP: dump первых кейсов (вис-локализация)
        if (fz_only >= 0 && i != fz_only) {          // consume the same RNG stream
            FtWr wskip[13]; int nws = 0; (void)wskip; (void)nws;
            (void)(rng.next()); (void)rng.w(); (void)rng.w(); (void)rng.w(); (void)rng.w();
            (void)(rng.next());
            if (rng.next() & 1) { (void)(rng.next()); (void)rng.w(); (void)rng.w(); (void)rng.w(); (void)rng.w(); }
            (void)(rng.next());
            continue;
        }
        FtWr w[13]; int nw = 0;
        w[nw++] = { (uint16_t)(OBJ + 0x1585), (uint16_t)(0x8000 | (rng.next() & 0x40)) };
        w[nw++] = { (uint16_t)(OBJ + 0x1535), rng.w() };
        w[nw++] = { (uint16_t)(OBJ + 0x155D), rng.w() };
        w[nw++] = { (uint16_t)(OBJ + 0x14E5), rng.w() };
        w[nw++] = { (uint16_t)(OBJ + 0x150D), rng.w() };
        w[nw++] = { FT_ADDR, (uint16_t)(0xFF00 | (rng.next() & 0x3F)) };
        if (rng.next() & 1) {
            w[nw++] = { (uint16_t)(2 + 0x1355), FT_VM_TESTSEG };
            w[nw++] = { (uint16_t)(2 + 0x17DD), (uint16_t)(rng.next() & 0x013F) };
            w[nw++] = { (uint16_t)(2 + 0x1535), rng.w() };
            w[nw++] = { (uint16_t)(2 + 0x155D), rng.w() };
            w[nw++] = { (uint16_t)(2 + 0x14E5), rng.w() };
            w[nw++] = { (uint16_t)(2 + 0x150D), rng.w() };
        }
        if (fz_dbg) {
            fprintf(stderr, "FNSELFTEST-FZCASE[%s i=%d]:", g_name[id], i);
            for (int k = 0; k < nw; k++) fprintf(stderr, " %04X=%04X", w[k].addr, w[k].val);
            fprintf(stderr, "\n");
        }
        ft_synth_case_search(id, 0, OBJ, w, nw, T, 4, 4,
                             rng.next(), "fuzz", fuzz, diff_budget);
    }

    fprintf(stderr,
        "FNSELFTEST-SUMMARY[%s]: grid %ld/%ld, exhaustive %ld/%ld, fuzz %ld/%ld — "
        "total cases=%ld fail=%ld%s\n",
        g_name[id], grid.pass, grid.cases, exh.pass, exh.cases, fuzz.pass, fuzz.cases,
        grid.cases + exh.cases + fuzz.cases, grid.fail + exh.fail + fuzz.fail,
        (grid.fail + exh.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (grid.fail + exh.fail + fuzz.fail) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Vel/collision scan family (units 27-30):
//   sub_1614e — Y-vel object scan, JL gate (divergence 17) + sub_161a1 bbox
//   sub_15c37 — X-vel object scan, JG gate (divergence 20) + cef/cf5 bbox
//   sub_15c93 — Y-vel object scan, JNS gate (SF-class, wrapped-exact)
//   sub_15afd — downward tile collision, JZ+JGE gate (divergence 19),
//               slope probe + horizontal walk (needs the crafted tilemap
//               and the slope table at [sidx-0x7684]).
// Inputs: si=filter, di=obj. Output: CF + AX (compared only when CF=1:
// 1614e→0, 15c37/15c93→direction, 15afd→snap value 0x8000|x or 0).

bool ft_synth_case_scan(FtId id, uint16_t filter, uint16_t obj,
                        const FtWr* w, int nw,
                        const uint16_t* tiles, int tw, int th,
                        uint32_t bg_seed, const char* group,
                        FtSynthStats& st, long& diff_budget)
{
    st.cases++;
    uint8_t* mbase = (uint8_t*)v2_fntest_m2c_base();
    uint8_t* zone = mbase + (uint32_t)FT_VM_TESTSEG * 16;
    memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
    ft_wr16(g_synth_in, 0x372, 8);                        // 4 slots: 0,2,4,6
    ft_wr16(g_synth_in, 0x42, obj);                       // self
    for (uint16_t s = 0; s < 8; s += 2)                   // controlled slot table
        ft_wr16(g_synth_in, (uint16_t)(s + 0x1355), 0);
    for (uint32_t a = 0x2E5C; a <= 0x2E7C; a += 2) ft_wr16(g_synth_in, a, 0);
    ft_wr16(g_synth_in, 0x2E63, FT_VM_TESTSEG);           // tilemap segment (15afd)
    ft_wr16(g_synth_in, 0x25DC, (uint16_t)tw);
    ft_wr16(g_synth_in, 0x25DE, (uint16_t)th);
    for (int y = 0; y < th; y++)
        ft_wr16(g_synth_in, (uint16_t)(y * 2 - 0x7098), (uint16_t)(y * tw * 2));
    // canaries on the scratch protocol (0x6C/0x6E belong to sub_15afd only)
    ft_wr16(g_synth_in, 0x32, 0xBBBB); ft_wr16(g_synth_in, 0x34, 0xBBBB);
    ft_wr16(g_synth_in, 0x36, 0xBBBB); ft_wr16(g_synth_in, 0x38, 0xBBBB);
    ft_wr16(g_synth_in, 0x3A, 0xBBBB);
    ft_wr16(g_synth_in, 0x6C, 0xBBBB); ft_wr16(g_synth_in, 0x6E, 0xBBBB);
    FtRng bg(bg_seed);
    static const uint16_t SF[] = { 0x1535, 0x155D, 0x14E5, 0x150D, 0x1585, 0x17DD,
                                   0x196D, 0x1945, 0x1765, 0x13CD, 0x173D };
    for (uint16_t f : SF)
        ft_wr16(g_synth_in, (uint16_t)(obj + f), bg.w());
    for (int i = 0; i < nw; i++) ft_wr16(g_synth_in, w[i].addr, w[i].val);

    // Build the tile zone (only sub_15afd reads it; harmless for the others)
    memset(g_vm_es_in, 0, sizeof(g_vm_es_in));
    for (int i = 0; i < tw * th; i++) {
        g_vm_es_in[i * 2]     = (uint8_t)(tiles[i] & 0xFF);
        g_vm_es_in[i * 2 + 1] = (uint8_t)(tiles[i] >> 8);
    }
    memcpy(zone, g_vm_es_in, FT_VM_ZONE);

    memcpy(g_synth_orig, g_synth_in, sizeof(g_synth_orig));
    uint16_t regs[8] = { 0, 0, 0, 0, filter, obj, 0, 0 };
    long esc0 = ft_ub_marks();
    v2_fntest_orig_isolated(v2_fntest_orig_fnptr(id), g_synth_orig, regs);
    memcpy(zone, g_vm_es_in, FT_VM_ZONE);
    if (ft_ub_marks() != esc0) {
        st.cases--;
        fprintf(stderr, "FNSELFTEST-UB[%s %s]: filter=%04X seed=%08X escaped\n",
                g_name[id], group, filter, bg_seed);
        return true;
    }
    int cf_orig = regs[7] & 1;
    int32_t eff_orig = cf_orig ? (int32_t)regs[0] : -1;   // AX only meaningful on STC

    memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
    int32_t eff_v2 = -1; int v2_hung = 0;
    v2_fntest_arm_signals();
    if (sigsetjmp(*v2_fntest_jb(), 1) == 0) {
        v2_fntest_alarm_ms(4000);
        eff_v2 = v2_fntest_call_scan(g_scratch, (int)(id - FT_SUB_1614E), filter, obj);
        v2_fntest_alarm_ms(0);
    } else {
        v2_fntest_alarm_ms(0);
        v2_hung = 1;
    }
    if (v2_hung) {
        fprintf(stderr, "FNSELFTEST-DIFF[%s %s]: v2 HUNG (watchdog) | filter=%04X obj=%04X seed=%08X"
                " | self: vY=%04X vX=%04X Y=%04X Yp=%04X Ye=%04X X=%04X"
                " | o2: alive=%04X t=%04X vY=%04X vX=%04X\n",
                g_name[id], group, filter, obj, bg_seed,
                *(uint16_t*)(g_synth_in + obj + 0x196D), *(uint16_t*)(g_synth_in + obj + 0x1945),
                *(uint16_t*)(g_synth_in + obj + 0x1765), *(uint16_t*)(g_synth_in + obj + 0x13CD),
                *(uint16_t*)(g_synth_in + obj + 0x150D), *(uint16_t*)(g_synth_in + obj + 0x173D),
                *(uint16_t*)(g_synth_in + 2 + 0x1355), *(uint16_t*)(g_synth_in + 2 + 0x17DD),
                *(uint16_t*)(g_synth_in + 2 + 0x196D), *(uint16_t*)(g_synth_in + 2 + 0x1945));
        st.fail++; return false;
    }

    long diffs = 0;
    if (eff_v2 != eff_orig) {
        if (diff_budget > 0) {
            diff_budget--;
            fprintf(stderr, "FNSELFTEST-DIFF[%s %s]: CF/AX orig=%ld v2=%ld | filter=%04X obj=%04X seed=%08X\n",
                    g_name[id], group, (long)eff_orig, (long)eff_v2, filter, obj, bg_seed);
        }
        diffs++;
    }
    for (uint32_t a = 0; a < 0x10000; a++) {
        if (g_scratch[a] == g_synth_orig[a]) continue;
        if (v2_fntest_ds_skip(a)) continue;
        if (diff_budget > 0) {
            diff_budget--;
            fprintf(stderr, "FNSELFTEST-DIFF[%s %s]: addr=%04X orig=%02X v2=%02X (in=%02X) "
                    "| filter=%04X obj=%04X seed=%08X\n",
                    g_name[id], group, a, g_synth_orig[a], g_scratch[a], g_synth_in[a],
                    filter, obj, bg_seed);
        }
        diffs++;
    }
    if (diffs) { st.fail++; return false; }
    st.pass++; return true;
}

int ft_selftest_scan(FtId id, uint32_t seed) {
    FtSynthStats grid, exh, fuzz;
    long diff_budget = 24;
    const uint16_t OBJ = 6;
    v2_set_m2c_base(v2_fntest_m2c_base());
    const bool shard0 = (g_shard_i == 0);
    const uint16_t FT_ADDR = (uint16_t)(0 - 0x6B34);      // filter=0 chain
    const uint16_t SLOPE_BASE = (uint16_t)(0 - 0x7684);   // slope table base
    // 4x4 tile map for sub_15afd: row-major types in bits 15..10.
    // Row 0-1: plain low types; row 2: slope types 0x30/0x31; row 3: 0x05 (match row).
    uint16_t T[16];
    for (int i = 0; i < 8;  i++) T[i] = (uint16_t)((i & 0x3F) << 10);
    T[8] = (uint16_t)(0x30 << 10); T[9]  = (uint16_t)(0x31 << 10);
    T[10] = (uint16_t)(0x30 << 10); T[11] = (uint16_t)(0x31 << 10);
    for (int i = 12; i < 16; i++) T[i] = (uint16_t)(0x05 << 10);

    const bool is_afd = (id == FT_SUB_15AFD);
    // vel field pair per unit: self field / partner field (the gate operands)
    const uint16_t VF = is_afd ? 0x1765 :
                        (id == FT_SUB_15C37) ? 0x1945 : 0x196D;
    const uint16_t VP = is_afd ? 0x13CD : VF;             // 15afd gate is self-only

    // --- Grid: directed branch/corner cases -------------------------------
    // Velocity gate values: zero, small +/-, int16 corners, OVERFLOW pairs
    // (|true diff| > 0x7FFF — the divergence-17/19/20 corners).
    struct VPair { uint16_t a, b; };
    static const VPair VG[] = {
        { 0x0000, 0x0000 },  // JZ gate
        { 0x0005, 0x0002 },  // small positive diff
        { 0x0002, 0x0005 },  // small negative diff
        { 0x8000, 0x7FFF },  // wrapped diff = 1, true diff = -65535 → overflow corner
        { 0x7FFF, 0x8000 },  // wrapped diff = -1, true diff = +65535 → overflow corner
        { 0xC216, 0x4220 },  // true -32778, wrapped +32758 (divergence-16 numbers)
        { 0x4220, 0xC216 },  // true +32778, wrapped -32758
        { 0x8000, 0x0001 },  // true -32769, wrapped +32767
        { 0x0001, 0x8000 },  // true +32769, wrapped -32767
    };
    // Partner bbox variants: full overlap (deep path), X-reject, Y-adj reject.
    struct PBox { uint16_t xs, xe, ys, ye, vy; };
    static const PBox PB[] = {
        { 0x0100, 0x0200, 0x0100, 0x0200, 0x0000 },  // overlap → STC path
        { 0x0180, 0x0200, 0x0100, 0x0200, 0x0000 },  // X-point reject (loc_15cf9 JL)
        { 0x0100, 0x0200, 0x01F0, 0x0200, 0x0040 },  // Y-adj corner
    };
    int ci = 0;
    if (shard0) for (const VPair& vg : VG) for (const PBox& pb : PB) {
        FtWr w[20]; int nw = 0;
        w[nw++] = { (uint16_t)(OBJ + 0x1535), 0x0140 };   // self X in partner range
        w[nw++] = { (uint16_t)(OBJ + 0x155D), 0x0150 };
        w[nw++] = { (uint16_t)(OBJ + 0x14E5), 0x0140 };   // self Y overlapping
        w[nw++] = { (uint16_t)(OBJ + 0x150D), 0x0150 };
        w[nw++] = { (uint16_t)(OBJ + 0x196D), 0x0000 };   // default vels
        w[nw++] = { (uint16_t)(OBJ + 0x1945), 0x0000 };
        w[nw++] = { (uint16_t)(OBJ + VF), vg.a };
        w[nw++] = { (uint16_t)(OBJ + 0x173D), 0x0148 };   // X for 15afd probes
        w[nw++] = { FT_ADDR, 0xFF05 };
        if (!is_afd) {
            w[nw++] = { (uint16_t)(2 + 0x1355), 1 };      // live partner in slot 2
            w[nw++] = { (uint16_t)(2 + 0x17DD), 0x0005 }; // type matches filter
            w[nw++] = { (uint16_t)(2 + VP), vg.b };
            w[nw++] = { (uint16_t)(2 + 0x1535), pb.xs };
            w[nw++] = { (uint16_t)(2 + 0x155D), pb.xe };
            w[nw++] = { (uint16_t)(2 + 0x14E5), pb.ys };
            w[nw++] = { (uint16_t)(2 + 0x150D), pb.ye };
            w[nw++] = { (uint16_t)(2 + 0x196D), pb.vy };
        } else {
            w[nw++] = { (uint16_t)(OBJ + VP), vg.b };     // 15afd: gate = self [1765]-[13CD]
        }
        ft_synth_case_scan(id, 0, OBJ, w, nw, T, 4, 4,
                           seed ^ (0xA0000000u + ci), "grid", grid, diff_budget);
        ci++;
    }
    // 15afd-specific directed: slope probe + walk corners.
    if (shard0 && is_afd) {
        struct ACase { uint16_t y, yp, ye, x, xs, xe; uint16_t f0f1; uint16_t slope; };
        static const ACase AC[] = {
            // moved down (y>yp), slope filter 0x30, probe over slope row 2 (y=0x20-0x2F)
            { 0x0030, 0x0020, 0x002E, 0x0015, 0x0010, 0x0020, 0x0030 | (0xFF<<8), 0x0004 },
            // same but slope value larger than (temp&0xF) → sr<0 → probe B/walk
            { 0x0030, 0x0020, 0x002E, 0x0015, 0x0010, 0x0020, 0x0030 | (0xFF<<8), 0x000F },
            // plain filter (no slope in chain) → straight to walk over match row 3
            { 0x0040, 0x0030, 0x003E, 0x0015, 0x0010, 0x0020, 0x0005 | (0xFF<<8), 0x0000 },
            // walk with same 16px row (old_ye & F0 == ye & F0) → early CLC
            { 0x0032, 0x0030, 0x0031, 0x0015, 0x0010, 0x0020, 0x0005 | (0xFF<<8), 0x0000 },
            // walk clamp: X range not multiple of 16
            { 0x0040, 0x0030, 0x003E, 0x0015, 0x0012, 0x002D, 0x0005 | (0xFF<<8), 0x0000 },
        };
        int ai = 0;
        for (const ACase& c : AC) {
            FtWr w[16]; int nw = 0;
            w[nw++] = { (uint16_t)(OBJ + 0x1765), c.y };
            w[nw++] = { (uint16_t)(OBJ + 0x13CD), c.yp };
            w[nw++] = { (uint16_t)(OBJ + 0x150D), c.ye };
            w[nw++] = { (uint16_t)(OBJ + 0x173D), c.x };
            w[nw++] = { (uint16_t)(OBJ + 0x1535), c.xs };
            w[nw++] = { (uint16_t)(OBJ + 0x155D), c.xe };
            w[nw++] = { FT_ADDR, c.f0f1 };
            // slope table entry for tile type 0x30, column (x & 0xF)
            w[nw++] = { (uint16_t)(SLOPE_BASE + ((0x30 & 0xF) << 4) + (c.x & 0xF)), c.slope };
            ft_synth_case_scan(id, 0, OBJ, w, nw, T, 4, 4,
                               seed ^ (0xAF000000u + ai), "grid", grid, diff_budget);
            ai++;
        }
    }

    // --- Exhaustive: the gate's partner operand over all 65536 values ------
    // Fixed self operand 0x4220 (+16928): sweeps cross both the JZ point and
    // both overflow corners of the JL/JG/JGE/JNS gate.
    long exh_done = 0;
    for (uint32_t v = 0; v <= 0xFFFF; v++) {
        if (!ft_shard_mine(v)) continue;
        if ((++exh_done % 4000) == 0)
            fprintf(stderr, "FNSELFTEST-PROG[%s]: exh %ld (v=%04X)\n", g_name[id], exh_done, v);
        FtWr w[18]; int nw = 0;
        w[nw++] = { (uint16_t)(OBJ + 0x1535), 0x0140 };
        w[nw++] = { (uint16_t)(OBJ + 0x155D), 0x0150 };
        w[nw++] = { (uint16_t)(OBJ + 0x14E5), 0x0140 };
        w[nw++] = { (uint16_t)(OBJ + 0x150D), 0x0150 };
        w[nw++] = { (uint16_t)(OBJ + 0x196D), 0x0000 };
        w[nw++] = { (uint16_t)(OBJ + 0x1945), 0x0000 };
        w[nw++] = { (uint16_t)(OBJ + 0x173D), 0x0148 };
        w[nw++] = { FT_ADDR, 0xFF05 };
        if (!is_afd) {
            w[nw++] = { (uint16_t)(OBJ + VF), 0x4220 };
            w[nw++] = { (uint16_t)(2 + 0x1355), 1 };
            w[nw++] = { (uint16_t)(2 + 0x17DD), 0x0005 };
            w[nw++] = { (uint16_t)(2 + VP), (uint16_t)v };
            w[nw++] = { (uint16_t)(2 + 0x1535), 0x0100 };
            w[nw++] = { (uint16_t)(2 + 0x155D), 0x0200 };
            w[nw++] = { (uint16_t)(2 + 0x14E5), 0x0100 };
            w[nw++] = { (uint16_t)(2 + 0x150D), 0x0200 };
            w[nw++] = { (uint16_t)(2 + 0x196D), 0x0000 };
        } else {
            w[nw++] = { (uint16_t)(OBJ + 0x1765), 0x4220 };
            w[nw++] = { (uint16_t)(OBJ + 0x13CD), (uint16_t)v };
        }
        ft_synth_case_scan(id, 0, OBJ, w, nw, T, 4, 4,
                           seed ^ 0xE0000000u ^ v, "exh", exh, diff_budget);
    }

    // --- Fuzz ---------------------------------------------------------------
    FtRng rng(seed ^ (0x51ED0000u * (uint32_t)(g_shard_i + 1)));
    int fz_count = 10000 / g_shard_n + (g_shard_i < (10000 % g_shard_n) ? 1 : 0);
    for (int i = 0; i < fz_count; i++) {
        if ((i % 500) == 0)
            fprintf(stderr, "FNSELFTEST-PROG[%s]: fuzz %d\n", g_name[id], i);
        FtWr w[24]; int nw = 0;
        w[nw++] = { (uint16_t)(OBJ + 0x1535), rng.w() };
        w[nw++] = { (uint16_t)(OBJ + 0x155D), rng.w() };
        w[nw++] = { (uint16_t)(OBJ + 0x14E5), rng.w() };
        w[nw++] = { (uint16_t)(OBJ + 0x150D), rng.w() };
        w[nw++] = { (uint16_t)(OBJ + 0x196D), rng.w() };
        w[nw++] = { (uint16_t)(OBJ + 0x1945), rng.w() };
        w[nw++] = { (uint16_t)(OBJ + 0x1765), rng.w() };
        w[nw++] = { (uint16_t)(OBJ + 0x13CD), rng.w() };
        w[nw++] = { (uint16_t)(OBJ + 0x173D), rng.w() };
        w[nw++] = { FT_ADDR, (uint16_t)(0xFF00 | (rng.next() & 0x3F)) };
        // two random slope-table entries (15afd probes; harmless otherwise)
        w[nw++] = { (uint16_t)(SLOPE_BASE + (rng.next() & 0xFF)), (uint16_t)(rng.next() & 0xF) };
        w[nw++] = { (uint16_t)(SLOPE_BASE + (rng.next() & 0xFF)), (uint16_t)(rng.next() & 0xF) };
        int live = (int)(rng.next() % 3);                  // 0-2 live partners
        for (int k = 0; k < live; k++) {
            uint16_t slot = (uint16_t)((k == 0) ? 2 : 4);
            w[nw++] = { (uint16_t)(slot + 0x1355), 1 };
            w[nw++] = { (uint16_t)(slot + 0x17DD), (uint16_t)(rng.next() & 0x013F) };
            w[nw++] = { (uint16_t)(slot + 0x196D), rng.w() };
            w[nw++] = { (uint16_t)(slot + 0x1945), rng.w() };
        }
        ft_synth_case_scan(id, 0, OBJ, w, nw, T, 4, 4,
                           rng.next(), "fuzz", fuzz, diff_budget);
    }

    fprintf(stderr,
        "FNSELFTEST-SUMMARY[%s]: grid %ld/%ld, exhaustive %ld/%ld, fuzz %ld/%ld — "
        "total cases=%ld fail=%ld%s\n",
        g_name[id], grid.pass, grid.cases, exh.pass, exh.cases, fuzz.pass, fuzz.cases,
        grid.cases + exh.cases + fuzz.cases, grid.fail + exh.fail + fuzz.fail,
        (grid.fail + exh.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (grid.fail + exh.fail + fuzz.fail) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// sub_1303a / sub_13031 <-> v2_vm_sub_1303a (+ v2_vm_sub_135cf) — the anim
// frame interpreter (units 25/26). The anim script is planted INSIDE the DS
// image at FT_ANIM_PC (the oracle enters with es==ds, so es:[bx] reads hit
// the DS image; v2 gets vm.es = the same image). Object state: [di+0x1A0D]
// script PC, [0x1A35]/[0x1A5D] timers, [0x1A85]/[0x1AAD] slot range.
// Terminal cmds: 0x0E (END FRAME) / 0x0F (set delay + exit).
const uint16_t FT_ANIM_PC = 0xE000;

bool ft_synth_case_anim(FtId id, uint16_t obj, const uint8_t* script, int slen,
                        uint16_t t1, uint16_t t2, uint16_t sa, uint16_t se,
                        const FtWr* w, int nw, uint32_t bg_seed,
                        const char* group, FtSynthStats& st, long& diff_budget)
{
    st.cases++;
    memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
    ft_wr16(g_synth_in, 0x372, (uint16_t)(obj + 2));
    ft_wr16(g_synth_in, 0x42, obj);
    ft_wr16(g_synth_in, 0x304, 1);                        // SFX muted (sound cmd gate)
    ft_wr16(g_synth_in, 0x302, 1);                        // music muted
    ft_wr16(g_synth_in, (uint16_t)(obj + 0x1355), FT_VM_TESTSEG);
    ft_wr16(g_synth_in, (uint16_t)(obj + 0x1A0D), FT_ANIM_PC);
    ft_wr16(g_synth_in, (uint16_t)(obj + 0x1A35), t1);
    ft_wr16(g_synth_in, (uint16_t)(obj + 0x1A5D), t2);
    ft_wr16(g_synth_in, (uint16_t)(obj + 0x1A85), sa);
    ft_wr16(g_synth_in, (uint16_t)(obj + 0x1AAD), se);
    // slot background noise (sprite offsets, flags, dirty)
    FtRng bg(bg_seed);
    for (uint32_t a = 0; a < 0x60; a += 2) {
        ft_wr16(g_synth_in, (uint16_t)(a + 0x44D), bg.w());
        ft_wr16(g_synth_in, (uint16_t)(a + 0x54D), bg.w());
        ft_wr16(g_synth_in, (uint16_t)(a + 0x84D), bg.w());
        ft_wr16(g_synth_in, (uint16_t)(a + 0x114D), bg.w());
    }
    // canaries on interpreter globals
    ft_wr16(g_synth_in, 0x78, 0xBBBB); ft_wr16(g_synth_in, 0x7A, 0xBBBB);
    ft_wr16(g_synth_in, 0x7C, 0xBBBB); ft_wr16(g_synth_in, 0x80, 0xBBBB);
    ft_wr16(g_synth_in, 0x38C, 0xBBBB);
    for (int i = 0; i < slen; i++) g_synth_in[(uint16_t)(FT_ANIM_PC + i)] = script[i];
    for (int i = 0; i < 8; i++)                            // safety END pad: consumption
        g_synth_in[(uint16_t)(FT_ANIM_PC + slen + i)] = 0x0E;  // drifts still terminate both sides
    for (int i = 0; i < nw; i++) ft_wr16(g_synth_in, w[i].addr, w[i].val);

    memcpy(g_synth_orig, g_synth_in, sizeof(g_synth_orig));
    uint16_t regs[8] = { 0, FT_ANIM_PC, 0, 0, 0, obj, 0, 0 };  // bx unused by 1303a (it loads its own)
    long esc0 = ft_ub_marks();
    v2_fntest_orig_isolated(v2_fntest_orig_fnptr(id), g_synth_orig, regs);
    if (ft_ub_marks() != esc0) {
        st.cases--;
        fprintf(stderr, "FNSELFTEST-UB[%s %s]: script0=%02X escaped\n",
                g_name[id], group, script[0]);
        return true;
    }

    memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
    v2_fntest_call_anim(g_scratch, obj, id == FT_SUB_13031 ? 1 : 0);
    if (v2_fntest_vm_soft == 2) {
        fprintf(stderr, "FNSELFTEST-DIFF[%s %s]: v2 SOFT-FAULT (anim cmd) script0=%02X\n",
                g_name[id], group, script[0]);
        v2_fntest_vm_soft = 0;
        st.fail++; return false;
    }
    v2_fntest_vm_soft = 0;

    long diffs = 0;
    for (uint32_t a = 0; a < 0x10000; a++) {
        if (g_scratch[a] == g_synth_orig[a]) continue;
        if (v2_fntest_ds_skip(a)) continue;
        if (diff_budget > 0) {
            diff_budget--;
            fprintf(stderr, "FNSELFTEST-DIFF[%s %s]: addr=%04X orig=%02X v2=%02X (in=%02X) "
                    "| s0=%02X s1=%02X t1=%04X sa=%04X se=%04X\n",
                    g_name[id], group, a, g_synth_orig[a], g_scratch[a], g_synth_in[a],
                    script[0], slen > 1 ? script[1] : 0, t1, sa, se);
        }
        diffs++;
    }
    if (diffs) { st.fail++; return false; }
    st.pass++; return true;
}

int ft_selftest_anim(FtId id, uint32_t seed) {
    FtSynthStats grid, fuzz;
    long diff_budget = 32;
    const uint16_t OBJ = 6;
    v2_set_m2c_base(v2_fntest_m2c_base());

    // Directed scripts: each cmd family once, plus timer paths and loops.
    struct AScript { uint8_t b[12]; int n; uint16_t t1, t2, sa, se; };
    static const AScript AS[] = {
        { { 0x0E }, 1, 0, 0, 0x48, 0x4C },                       // END only
        { { 0x0F, 0x07 }, 2, 0, 0, 0x48, 0x4C },                 // delay+exit
        { { 0x00, 0x03, 0x0E }, 3, 0, 0, 0x48, 0x4C },           // advance sprite (frame 3)
        { { 0x01, 0x02, 0x0E }, 3, 0, 0, 0x48, 0x4C },           // cond advance
        { { 0x0D, 0x05, 0x00, 0x01, 0x0E }, 5, 0, 0, 0x48, 0x4C },// mask + advance
        { { 0x07, 0xF9, 0x0E }, 3, 0, 0, 0x48, 0x4C },           // X offset -7
        { { 0x09, 0x11, 0x0E }, 3, 0, 0, 0x48, 0x4C },           // Y offset +0x11
        { { 0x08, 0x05, 0x00, 0x0E }, 4, 0, 0, 0x48, 0x4A },     // abs X per-slot (1 slot)
        { { 0x0A, 0xFE, 0xFF, 0x0E }, 4, 0, 0, 0x48, 0x4A },     // abs Y per-slot
        { { 0x0C, 0x35, 0x0E }, 3, 0, 0, 0x48, 0x4C },           // palette bits
        { { 0x04, 0xAA, 0x0E }, 3, 0, 0, 0x48, 0x4C },           // skip 1 byte
        { { 0x0B, 0x0E }, 2, 0, 0, 0x48, 0x4C },                 // INT3 nop
        { { 0x0F, 0x00 }, 2, 0, 0, 0x48, 0x4C },                 // delay 0 edge
        { { 0x0E }, 1, 1, 0, 0x48, 0x4C },                       // timer=1 → DEC to 0 → run
        { { 0x0E }, 1, 2, 7, 0x48, 0x4C },                       // timer=2 → DEC, exit (no cmds)
        { { 0x15, 0x02, 0x0E }, 3, 0, 0, 0x48, 0x4A },           // sprite type/data setup
        { { 0x14, 0x01, 0x0E }, 3, 0, 0, 0x48, 0x4A },           // decompression cmd
        { { 0x02, 0x33, 0x44, 0x0E }, 4, 0, 0, 0x48, 0x4C },     // play sound (muted in-case)
        { { 0x03, 0x06, 0xE0, 0x0E, 0x00, 0x00, 0x0E }, 7, 0, 0, 0x48, 0x4C },  // JUMP → E006
        { { 0x05, 0x07, 0xE0, 0x0E, 0x00, 0x00, 0x00, 0x06 }, 8, 0, 0, 0x48, 0x4C }, // LOOP once: START→E007, BACK→E003
        { { 0x10, 0x0E }, 2, 0, 0, 0x48, 0x4C },                 // loc_13480
        { { 0x11, 0x0E }, 2, 0, 0, 0x48, 0x4C },                 // loc_13485
        { { 0x12, 0x0E }, 2, 0, 0, 0x48, 0x4C },                 // loc_1348a
        { { 0x13, 0x0E }, 2, 0, 0, 0x48, 0x4C },                 // loc_1341f
        { { 0x16, 0x55, 0x0E }, 3, 0, 0, 0x48, 0x4C },           // dup of cmd 04 (skip byte)
        { { 0x17, 0x0E }, 2, 0, 0, 0x48, 0x4C },                 // loc_1356e
        { { 0x18, 0x0E }, 2, 0, 0, 0x48, 0x4C },                 // loc_1339f
        { { 0x19, 0x0E }, 2, 0, 0, 0x48, 0x4C },                 // loc_133de (AND 0x9FFF pass)
        { { 0x1A }, 1, 0, 0, 0x48, 0x4C },                       // loc_1346f: bx=FFFF reset + exit
    };
    int ci = 0;
    for (const AScript& a : AS) {
        ft_synth_case_anim(id, OBJ, a.b, a.n, a.t1, a.t2, a.sa, a.se,
                           nullptr, 0, seed ^ (0xA0000000u + ci), "grid", grid, diff_budget);
        ci++;
    }
    // [0x1A0D]==0xFFFF: whole call is a no-op
    {
        static const uint8_t nb[1] = { 0x0E };
        FtWr wn[1] = { { (uint16_t)(OBJ + 0x1A0D), 0xFFFF } };
        ft_synth_case_anim(id, OBJ, nb, 1, 0, 0, 0x48, 0x4C,
                           wn, 1, seed + 0x999, "grid", grid, diff_budget);
    }
    // Slot-length cmds (0x01 / 0x0C): consumption = one byte per slot
    // (unmasked; both slot loops are do-while). Dedicated stream with an
    // exact slot count so the stream stays in sync.
    {
        FtRng r2(seed ^ 0x5107C0DE);
        for (int i = 0; i < 2000; i++) {
            uint8_t sc[12]; int n = 0;
            uint8_t c = (r2.next() & 1) ? 0x01 : 0x0C;
            int slots = 1 + (int)(r2.next() % 3);          // 1..3 slots
            sc[n++] = c;
            for (int k = 0; k < slots; k++) sc[n++] = (uint8_t)r2.next();
            sc[n++] = 0x0E;
            uint16_t sa = (uint16_t)((0x40 + (r2.next() % 0x10)) & ~1u);
            uint16_t se = (uint16_t)(sa + slots * 2);
            ft_synth_case_anim(id, OBJ, sc, n, 0, 0, sa, se,
                               nullptr, 0, r2.next(), "slotfz", fuzz, diff_budget);
        }
    }

    // Fuzz: random scripts from the safe subset (fixed arg lengths), always
    // terminated; random slot ranges and timers.
    FtRng rng(seed);
    // NB: 0x01/0x0C are slot-length cmds (byte per slot) — excluded here,
    // covered by the dedicated slot-exact stream above.
    static const uint8_t FZ_CMD[]  = { 0x00, 0x04, 0x07, 0x09, 0x0B, 0x0D };
    static const int     FZ_LEN[]  = { 1,    1,    1,    1,    0,    1    };
    for (int i = 0; i < 8000; i++) {
        uint8_t sc[12]; int n = 0;
        int cmds = 1 + (int)(rng.next() % 4);
        for (int k = 0; k < cmds && n < 9; k++) {
            int idx = (int)(rng.next() % (sizeof(FZ_CMD)));
            sc[n++] = FZ_CMD[idx];
            for (int j = 0; j < FZ_LEN[idx]; j++) sc[n++] = (uint8_t)rng.next();
        }
        sc[n++] = (rng.next() & 1) ? 0x0E : 0x0F;
        if (sc[n-1] == 0x0F) sc[n++] = (uint8_t)(rng.next() & 0x1F);
        uint16_t sa = (uint16_t)((0x40 + (rng.next() % 0x20)) & ~1u);
        uint16_t se = (uint16_t)(sa + ((rng.next() % 4) * 2));
        ft_synth_case_anim(id, OBJ, sc, n, (uint16_t)(rng.next() % 3),
                           (uint16_t)(rng.next() % 5), sa, se,
                           nullptr, 0, rng.next(), "fuzz", fuzz, diff_budget);
    }

    fprintf(stderr,
        "FNSELFTEST-SUMMARY[%s]: grid %ld/%ld, fuzz %ld/%ld — total cases=%ld fail=%ld%s\n",
        g_name[id], grid.pass, grid.cases, fuzz.pass, fuzz.cases,
        grid.cases + fuzz.cases, grid.fail + fuzz.fail,
        (grid.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (grid.fail + fuzz.fail) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Class B: per-object VM exec (sub_1424c <-> v2_vm_execute_object).
// Unit: ONE object whose bytecode lives in a scratch code segment inside
// m2c::m at TESTSEG (linear TESTSEG*16 — heap area, free in the selftest
// process). The oracle reads it via es=[si+0x1355]=TESTSEG; v2 reads the SAME
// bytes via v2_resolve_segment's fallback (all shadow segment fields in
// ds:0x2E5C.. are zeroed in the case input, v2_set_m2c_base points at m2c::m).
// Bytecode layout: a 0x00 (yield) carpet with [op][args...] planted at PC —
// any argument mis-consumption or forward jump lands on yield immediately,
// identically on both sides. The ES zone is snapshotted/compared/restored
// around each side, since both physically share it.
// Known first-iteration limits: the accumulator is compared only through its
// DS effects (both sides start at 0: memset'ed oracle state / explicit v2
// reset); anim-update path (flags&0x200 / ds:0x32F) is exercised with the
// anim header zone also inside the carpet segment.
// TESTSEG placement: m2c::m layout is [static EXE image ~0x243B0][stack 64KB
// @~0x243B0][heap ~1MB]. The oracle's emulated stack pushes land at
// stack_base+sp (sp starts at STACK_SIZE/2) — segment 0x2A00 overlapped that,
// so oracle pushes trashed the code zone. 0x4000 (linear 0x40000..0x50000)
// sits safely inside the heap, which is untouched in the selftest process
// (no m2c::init / DOS allocs there).
// The zone is the FULL 64KB segment: control-flow opcodes jump anywhere in
// it (uint16 PC), and both sides must read/write identical bytes.
// (FT_VM_TESTSEG/FT_VM_ZONE/FT_VM_PC and the zone buffers are defined near
// the top of this namespace — shared with the search units.)

struct FtVmObj { uint16_t flags, anim, timer, x, y, yvel, ystart, yend; };

long g_vmop_ub = 0;   // vmop cases skipped as orig-UB

bool ft_synth_case_vmop(uint8_t op, const uint8_t* args, int n_args,
                        const FtVmObj& o, uint32_t bg_seed,
                        const char* group, FtSynthStats& st, long& diff_budget,
                        long* op_fail,
                        const FtWr* extra = nullptr, int n_extra = 0)
{
    st.cases++;
    uint8_t* mbase = (uint8_t*)v2_fntest_m2c_base();
    uint8_t* zone = mbase + (uint32_t)FT_VM_TESTSEG * 16;
    const uint16_t si = 6;   // object slot under test

    // Build DS input
    memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
    ft_wr16(g_synth_in, 0x372, (uint16_t)(si + 2));
    for (uint32_t a = 0x2E5C; a <= 0x2E7C; a += 2) ft_wr16(g_synth_in, a, 0); // no shadow segs
    ft_wr16(g_synth_in, 0x32F, 0);
    ft_wr16(g_synth_in, (uint16_t)(si + 0x1355), FT_VM_TESTSEG);
    ft_wr16(g_synth_in, (uint16_t)(si + 0x132D), FT_VM_PC);
    ft_wr16(g_synth_in, (uint16_t)(si + 0x1585), o.flags);
    ft_wr16(g_synth_in, (uint16_t)(si + 0x16ED), o.anim);
    ft_wr16(g_synth_in, (uint16_t)(si + 0x1715), o.timer);
    ft_wr16(g_synth_in, (uint16_t)(si + 0x173D), o.x);
    ft_wr16(g_synth_in, (uint16_t)(si + 0x1765), o.y);
    ft_wr16(g_synth_in, (uint16_t)(si + 0x196D), o.yvel);
    ft_wr16(g_synth_in, (uint16_t)(si + 0x14E5), o.ystart);
    ft_wr16(g_synth_in, (uint16_t)(si + 0x150D), o.yend);
    // Light background noise over misc object fields (deterministic)
    FtRng bg(bg_seed);
    static const uint16_t OF[] = { 0x1305, 0x13CD, 0x1495, 0x14BD, 0x1535, 0x155D,
                                   0x1855, 0x18AD, 0x1995, 0x19E5, 0x1AD5 };
    for (uint16_t f : OF) ft_wr16(g_synth_in, (uint16_t)(si + f), bg.w());
    // Directed-case overrides (applied last — may override anything above)
    for (int i = 0; i < n_extra; i++) ft_wr16(g_synth_in, extra[i].addr, extra[i].val);

    // Build the code zone: yield carpet + [op][args] at PC
    memset(g_vm_es_in, 0, sizeof(g_vm_es_in));
    g_vm_es_in[FT_VM_PC] = op;
    for (int i = 0; i < n_args; i++) g_vm_es_in[FT_VM_PC + 1 + i] = args[i];
    for (int i = 0; i < g_vm_es_patch_n; i++) g_vm_es_in[i] = g_vm_es_patch[i];

    // --- oracle ---
    memcpy(zone, g_vm_es_in, FT_VM_ZONE);
    memcpy(g_synth_orig, g_synth_in, sizeof(g_synth_orig));
    uint16_t regs[8] = { 0, 0, 0, 0, si, 0, 0, 0 };
    long esc0 = ft_ub_marks();
    v2_fntest_orig_isolated(v2_fntest_orig_fnptr(FT_SUB_1424C), g_synth_orig, regs);
    if (ft_ub_marks() != esc0) {
        // orig-UB input: the oracle escaped through start: (see guard there).
        // Real bytecode never reaches these paths — skip, don't compare.
        g_vmop_ub++;
        st.cases--;
        memcpy(zone, g_vm_es_in, FT_VM_ZONE);
        return true;
    }
    memcpy(g_vm_es_orig, zone, FT_VM_ZONE);

    // --- v2 (same zone, restored to the input first) ---
    memcpy(zone, g_vm_es_in, FT_VM_ZONE);
    memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
    v2_fntest_call_vm_exec(g_scratch, si);
    if (v2_fntest_vm_soft == 3) {
        // ch6/7 register-history guard: the orig continues through a stale-DI
        // DS write (deterministic for the oracle, whose registers persist);
        // the register-less v2 has no DI model yet — counted separately, not
        // as a divergence. Only the three wrapper-Y sites can raise this.
        fprintf(stderr, "FNSELFTEST-GUARD[sub_1424c %s]: op=%02X ch6/7 register-history site\n",
                group, op);
        v2_fntest_vm_soft = 0;
        memcpy(zone, g_vm_es_in, FT_VM_ZONE);
        st.cases--;               // model-gap skip (like orig-UB), visible via the GUARD print
        return true;
    }
    if (v2_fntest_vm_soft == 2) {
        // v2 hit a VM FATAL (op>0xD7 / unimplemented) on a case the oracle
        // completed — that's a divergence in its own right.
        fprintf(stderr, "FNSELFTEST-DIFF[sub_1424c %s]: op=%02X v2 SOFT-FAULT (VM FATAL path)\n",
                group, op);
        v2_fntest_vm_soft = 0;
        memcpy(zone, g_vm_es_in, FT_VM_ZONE);
        st.fail++; if (op_fail) op_fail[op]++;
        return false;
    }
    v2_fntest_vm_soft = 0;

    static long op_print[256];   // per-opcode diff-print quota (readable full map)
    long diffs = 0;
    for (uint32_t a = 0; a < 0x10000; a++) {
        if (g_scratch[a] == g_synth_orig[a]) continue;
        if (v2_fntest_ds_skip(a)) continue;
        if (op_print[op] < 10) {
            op_print[op]++;
            fprintf(stderr, "FNSELFTEST-DIFF[sub_1424c %s]: op=%02X addr=%04X orig=%02X v2=%02X (in=%02X) args=%02X%02X%02X%02X\n",
                    group, op, a, g_synth_orig[a], g_scratch[a], g_synth_in[a],
                    n_args > 0 ? args[0] : 0, n_args > 1 ? args[1] : 0,
                    n_args > 2 ? args[2] : 0, n_args > 3 ? args[3] : 0);
        }
        diffs++;
    }
    for (uint32_t a = 0; a < FT_VM_ZONE; a++) {
        if (zone[a] == g_vm_es_orig[a]) continue;
        if (op_print[op] < 10) {
            op_print[op]++;
            fprintf(stderr, "FNSELFTEST-DIFF[sub_1424c %s]: op=%02X ES+%04X orig=%02X v2=%02X (in=%02X)\n",
                    group, op, a, g_vm_es_orig[a], zone[a], g_vm_es_in[a]);
        }
        diffs++;
    }
    memcpy(zone, g_vm_es_in, FT_VM_ZONE);   // leave the zone clean

    if (diffs) { st.fail++; if (op_fail) op_fail[op]++; return false; }
    st.pass++; return true;
}

// Oracle-driven argument-length probe: run [op][0x00 carpet]; with zero args
// every non-consumed byte is a yield, so the final saved PC ([si+0x132D])
// reveals how many bytes the opcode consumed. len = final_pc - (PC+1);
// -1 -> terminal (op yielded/exited itself), 0..16 -> data op, else CTRL
// (jump/dispatch landed elsewhere in the carpet — arg fuzz unsafe for it).
// NOTE: bytes above 0xD7 are FORBIDDEN anywhere reachable: orig dispatches
// through a 0xD8-entry table and reads garbage beyond it (jumps into start:).
const int FT_VMOP_ESCAPED = -1000;   // probe attempt hit an orig-UB escape

// One oracle probe with a FULL 16-byte arg vector. Returns the DATA length
// consumed (final saved PC tells: carpet yield saves PC past itself, so
// d = final - (PC+1) - 1), or -1 terminal, -2 control-flow, FT_VMOP_ESCAPED.
int ft_vmop_probe_vec(uint8_t op, const FtVmObj& o, const uint8_t* args16) {
    uint8_t* mbase = (uint8_t*)v2_fntest_m2c_base();
    uint8_t* zone = mbase + (uint32_t)FT_VM_TESTSEG * 16;
    const uint16_t si = 6;
    memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
    ft_wr16(g_synth_in, 0x372, (uint16_t)(si + 2));
    for (uint32_t a = 0x2E5C; a <= 0x2E7C; a += 2) ft_wr16(g_synth_in, a, 0);
    ft_wr16(g_synth_in, 0x32F, 0);
    ft_wr16(g_synth_in, (uint16_t)(si + 0x1355), FT_VM_TESTSEG);
    ft_wr16(g_synth_in, (uint16_t)(si + 0x132D), FT_VM_PC);
    ft_wr16(g_synth_in, (uint16_t)(si + 0x1585), o.flags);
    ft_wr16(g_synth_in, (uint16_t)(si + 0x16ED), o.anim);
    memset(g_vm_es_in, 0, sizeof(g_vm_es_in));
    g_vm_es_in[FT_VM_PC] = op;
    for (int i = 0; i < 16; i++) g_vm_es_in[FT_VM_PC + 1 + i] = args16[i];
    memcpy(zone, g_vm_es_in, FT_VM_ZONE);
    memcpy(g_synth_orig, g_synth_in, sizeof(g_synth_orig));
    uint16_t regs[8] = { 0, 0, 0, 0, si, 0, 0, 0 };
    long esc0 = ft_ub_marks();
    v2_fntest_orig_isolated(v2_fntest_orig_fnptr(FT_SUB_1424C), g_synth_orig, regs);
    memcpy(zone, g_vm_es_in, FT_VM_ZONE);
    if (ft_ub_marks() != esc0) return FT_VMOP_ESCAPED;
    uint16_t fin = (uint16_t)(g_synth_orig[(uint16_t)(si + 0x132D)]
                 | (g_synth_orig[(uint16_t)(si + 0x132D + 1)] << 8));
    int len = (int)fin - (int)(FT_VM_PC + 1);
    if (len == -1) return -1;              // terminal
    if (len == 0) return 0;                // op saved PC right after itself
    if (len >= 1 && len <= 17) return len - 1;  // data bytes (carpet yield adds 1)
    return -2;                             // control-flow / non-local PC
}

// Per-opcode generation plan, self-calibrated against the oracle:
// - base: the first arg vector that completes without an orig-UB escape.
//   V0 = all zero (off_30C98 channels 0,0 = literals). Ops whose zero path
//   dies (sub_154bf setters: channel 0 is a bare-RETN table overlap) get
//   V1..V3 with 0x09 (channels 1,1) planted at the plausible mode spots.
// - data_len: consumption measured ON THE BASE VECTOR.
// - mode_mask: positions whose value changes consumption (flipping a byte to
//   a different channel pair changes how many bytes the getters eat) — these
//   are mode bytes; sweep/fuzz keep them at base values, and dedicated
//   channel variations exercise them within the DEFINED channel sets
//   (off_30C98: 0-4; overlap channels 5-7 are the POP-through class, modeled
//   separately — see the channels-5/7 task).
struct FtOpPlan {
    int     data_len;      // -1 terminal, -2 ctrl, -3 UB-with-all-bases, >=0 data
    uint8_t base[16];
    uint16_t mode_mask;
};

void ft_vmop_make_plan(uint8_t op, const FtVmObj& o, FtOpPlan& p) {
    static const uint8_t V0[16] = { 0 };
    static const uint8_t V1[16] = { 0x09 };
    static const uint8_t V2[16] = { 0x09, 0x09 };
    static const uint8_t V3[16] = { 0x09, 0x00, 0x00, 0x09 };
    const uint8_t* BASES[4] = { V0, V1, V2, V3 };
    p.data_len = -3; p.mode_mask = 0;
    memset(p.base, 0, sizeof(p.base));
    for (const uint8_t* bv : BASES) {
        int len = ft_vmop_probe_vec(op, o, bv);
        if (len == FT_VMOP_ESCAPED) continue;
        p.data_len = len;
        memcpy(p.base, bv, 16);
        break;
    }
    if (p.data_len < 0) return;            // terminal(-1)/ctrl(-2)/UB(-3): no scan
    // Mode scan: flip each in-range byte to a different channel pair and see
    // if consumption changes (or the path escapes) — that marks a mode byte.
    uint8_t v[16];
    for (int k = 0; k < p.data_len && k < 16; k++) {
        memcpy(v, p.base, 16);
        v[k] = (p.base[k] == 0x09) ? 0x12 : 0x09;   // (1,1)<->(2,2) channel pairs
        int len = ft_vmop_probe_vec(op, o, v);
        if (len != p.data_len) p.mode_mask |= (uint16_t)(1u << k);
    }
}

int ft_selftest_vmops() {
    FtSynthStats sweep, fuzz;
    long diff_budget = 32;
    static long op_fail[256];
    memset(op_fail, 0, sizeof(op_fail));
    v2_set_m2c_base(v2_fntest_m2c_base());

    const FtVmObj BASE = { 0x8000, 0, 0, 0x0120, 0x0140, 0, 0x0130, 0x0150 };

    // Phase 1: per-opcode plan (base vector + data length + mode positions).
    static FtOpPlan plan[0xD8];
    for (int op = 0; op <= 0xD7; op++) ft_vmop_make_plan((uint8_t)op, BASE, plan[op]);
    {
        char buf[1024]; int n = 0;
        for (int op = 0; op <= 0xD7 && n < 1000; op++)
            if (plan[op].data_len == -2) n += snprintf(buf + n, sizeof(buf) - (size_t)n, " %02X", op);
        fprintf(stderr, "FNSELFTEST-VMOP-CTRL:%s\n", n ? buf : " (none)");
        n = 0;
        for (int op = 0; op <= 0xD7 && n < 1000; op++)
            if (plan[op].data_len == -3) n += snprintf(buf + n, sizeof(buf) - (size_t)n, " %02X", op);
        fprintf(stderr, "FNSELFTEST-VMOP-UBPROBE:%s\n", n ? buf : " (none)");
        n = 0;
        for (int op = 0; op <= 0xD7 && n < 1000; op++)
            if (plan[op].data_len >= 0 && plan[op].mode_mask)
                n += snprintf(buf + n, sizeof(buf) - (size_t)n, " %02X:%X", op, plan[op].mode_mask);
        fprintf(stderr, "FNSELFTEST-VMOP-MODE:%s\n", n ? buf : " (none)");
    }

    // Phase 2: sweep. Base case for every comparable opcode; PAT variations
    // on NON-mode data positions (mode bytes stay at base so consumption is
    // stable); channel variations (defined channels only) on mode positions
    // with zeroed data.
    static const uint8_t PAT[4] = { 0x11, 0xFF, 0x80, 0x27 };
    static const uint8_t CHAN[14] = { 0x00, 0x01, 0x02, 0x03, 0x04,  // (c,0)
                                      0x09, 0x1B, 0x24,              // (1,1) (3,3) (4,4)
                                      // Table-overlap channels (off_30C98→off_30CA2):
                                      // 5 = clean passthrough (ax=0x0A), 6/7 = POP-through
                                      // site-ip writes; X-sites RETN onto data (orig-UB →
                                      // runner skips), Y-sites exit the opcode cleanly.
                                      0x05, 0x06, 0x07,              // (5..7, 0)
                                      0x2D, 0x30, 0x38 };            // (5,5) (0,6) (0,7)
    uint8_t a[16];
    for (int op = 0; op <= 0xD7; op++) {
        const FtOpPlan& pl = plan[op];
        if (pl.data_len == -3) continue;                     // orig-UB: not comparable
        memcpy(a, pl.base, 16);
        ft_synth_case_vmop((uint8_t)op, a, 16, BASE,
                           0xB0000000u | (op << 8), "sweep", sweep, diff_budget, op_fail);
        if (pl.data_len == -2) {
            // Control-flow ops: the base run plus a few in-carpet target words
            // planted at the first arg positions — jumps land on yields either
            // way, both sides follow the same PC. Escaping variants are
            // UB-skipped by the runner.
            static const uint16_t TGT[] = { 0x0120, 0x0080, 0x0000 };
            for (uint16_t t : TGT) {
                memcpy(a, pl.base, 16);
                a[0] = (uint8_t)(t & 0xFF);
                a[1] = (uint8_t)(t >> 8);
                ft_synth_case_vmop((uint8_t)op, a, 16, BASE,
                                   0xB4000000u | (op << 8) | (t & 0xFF),
                                   "ctrl", sweep, diff_budget, op_fail);
            }
            continue;
        }
        if (pl.data_len <= 0) continue;
        for (auto ptn : PAT) {                       // data-position patterns
            memcpy(a, pl.base, 16);
            for (int k = 0; k < pl.data_len; k++)
                if (!(pl.mode_mask & (1u << k))) a[k] = ptn;
            ft_synth_case_vmop((uint8_t)op, a, 16, BASE,
                               0xB1000000u | (op << 8) | ptn, "sweep", sweep, diff_budget, op_fail);
        }
        int last_mode = -1;
        for (int k = 0; k < pl.data_len; k++)
            if (pl.mode_mask & (1u << k)) last_mode = k;
        for (int k = 0; k < pl.data_len; k++) {      // mode-position channel combos
            if (!(pl.mode_mask & (1u << k))) continue;
            for (auto ch : CHAN) {
                memcpy(a, pl.base, 16);
                for (int j = k; j < 16; j++) a[j] = 0;   // zero data past the mode byte
                a[k] = ch;
                ft_synth_case_vmop((uint8_t)op, a, 16, BASE,
                                   0xB2000000u | (op << 12) | (k << 8) | ch,
                                   "sweep", sweep, diff_budget, op_fail);
                // For the LAST mode position also feed non-zero data of the
                // exact per-channel consumption ({literal:2, field:1,
                // indirect:2, partner:1, rng:0} per 3-bit field) — earlier
                // positions would shift the base geometry.
                if (k == last_mode) {
                    // Per-channel consumption incl. overlap channels: 5 eats
                    // nothing (plain RETN), 6 eats 1 (idx byte), 7 eats 2 (addr).
                    static const int CH_CONS[8] = { 2, 1, 2, 1, 0, 0, 1, 2 };
                    int a1 = ch & 7, b1 = (ch >> 3) & 7;
                    int need = CH_CONS[a1] + CH_CONS[b1];
                    if (need > 0 && k + 1 + need <= 16) {
                        FtRng cr(0xB3000000u | (op << 12) | (k << 8) | ch);
                        for (int j = 0; j < need; j++) a[k + 1 + j] = (uint8_t)cr.next();
                        ft_synth_case_vmop((uint8_t)op, a, 16, BASE,
                                           0xB3000000u | (op << 12) | (k << 8) | ch,
                                           "sweep", sweep, diff_budget, op_fail);
                    }
                }
            }
        }
    }

    // Phase 3: fuzz — random data on NON-mode positions (mode bytes stay at
    // base so consumption is exact), random object fields (anim-update flag
    // 0x200 kept off; crafted anim headers are a later iteration).
    FtRng rng(0x1424C001);
    for (int i = 0; i < 20000; i++) {
        uint8_t op = (uint8_t)(rng.next() % 0xD8);
        const FtOpPlan& pl = plan[op];
        if (pl.data_len < 0) { i--; continue; }    // terminal/ctrl/UB skip
        memcpy(a, pl.base, 16);
        for (int k = 0; k < pl.data_len; k++)
            if (!(pl.mode_mask & (1u << k))) a[k] = (uint8_t)rng.next();
        FtVmObj o = { (uint16_t)(rng.w() & (uint16_t)~0x0200), rng.w(), rng.w(),
                      rng.w(), rng.w(), rng.w(), rng.w(), rng.w() };
        ft_synth_case_vmop(op, a, 16, o, rng.next(), "fuzz", fuzz, diff_budget, op_fail);
    }

    // Phase 4: directed cases.
    // (a) op 0x14 with ALL main object slots occupied — sub_13d52 STC path.
    //     Orig writes the sub_13809 prologue (0x34=type, 0x36=di, 0x38=flags)
    //     BEFORE the 13d30/13d52 gates, so the scratch trio must appear in
    //     DS even on the fail path.
    {
        FtWr occ[0x14 + 1];
        for (int k = 0; k < 0x14; k++)
            occ[k] = { (uint16_t)(k * 2 + 0x1355), 0x4000 };   // slots 0..0x26 alive
        occ[0x14] = { (uint16_t)(6 + 0x1355), FT_VM_TESTSEG }; // keep test obj's code seg
        uint8_t a4[16]; memcpy(a4, plan[0x14].base, 16);
        ft_synth_case_vmop(0x14, a4, 16, BASE, 0xD0000001u, "directed",
                           sweep, diff_budget, op_fail, occ, 0x14 + 1);
    }
    // (b) op 0x14 with ds:0x32F != 0 — sub_13d30 STC path (prologue order too).
    {
        FtWr tr[1] = { { 0x32F, 1 } };
        uint8_t a4[16]; memcpy(a4, plan[0x14].base, 16);
        // NB: 0x32F!=0 also flips sub_1424c to the transition fetch (PC and
        // ES from the anim header at [0x2E67]:[anim*0x15]); with anim=0 and
        // [0x2E67]=0 both sides execute the same low-memory bytes — the op14
        // handler itself is then reached only if those bytes lead to it, so
        // this case primarily covers the transition-fetch parity.
        ft_synth_case_vmop(0x14, a4, 16, BASE, 0xD0000002u, "directed",
                           sweep, diff_budget, op_fail, tr, 1);
    }
    // (c) transition fetch INTO the test zone: [0x2E67]=TESTSEG and a crafted
    //     anim header (anim=0 → header at zone[0..]: byte[2]=subcount flags,
    //     WORD[3]=PC-3 → saved PC becomes FT_VM_PC) so the planted opcode
    //     executes THROUGH the 0x32F transition path for every data op.
    {
        for (int op = 0; op <= 0xD7; op++) {
            const FtOpPlan& pl = plan[op];
            if (pl.data_len < 0 && pl.data_len != -1) continue;
            FtWr tr[2] = { { 0x32F, 1 }, { 0x2E67, FT_VM_TESTSEG } };
            uint8_t a4[16]; memcpy(a4, pl.base, 16);
            // crafted header lives at zone[0..4] (anim=0): applied by the
            // runner via g_vm_es_patch after the carpet is built.
            g_vm_es_patch[0] = 0; g_vm_es_patch[1] = 0; g_vm_es_patch[2] = 0;
            g_vm_es_patch[3] = (uint8_t)((FT_VM_PC - 3) & 0xFF);
            g_vm_es_patch[4] = (uint8_t)((FT_VM_PC - 3) >> 8);
            g_vm_es_patch_n = 5;
            ft_synth_case_vmop((uint8_t)op, a4, 16, BASE,
                               0xD1000000u | (op << 8), "trans", sweep, diff_budget, op_fail, tr, 2);
            g_vm_es_patch_n = 0;
        }
    }
    // (d) the same transition fetch via the object flag 0x200 (loc_14283 is
    //     entered when flags&0x200 OR ds:0x32F != 0) — flag variant.
    {
        FtVmObj fo = BASE; fo.flags = 0x8200;
        for (int op = 0; op <= 0xD7; op++) {
            const FtOpPlan& pl = plan[op];
            if (pl.data_len < 0 && pl.data_len != -1) continue;
            FtWr tr[1] = { { 0x2E67, FT_VM_TESTSEG } };
            uint8_t a4[16]; memcpy(a4, pl.base, 16);
            g_vm_es_patch[0] = 0; g_vm_es_patch[1] = 0; g_vm_es_patch[2] = 0;
            g_vm_es_patch[3] = (uint8_t)((FT_VM_PC - 3) & 0xFF);
            g_vm_es_patch[4] = (uint8_t)((FT_VM_PC - 3) >> 8);
            g_vm_es_patch_n = 5;
            ft_synth_case_vmop((uint8_t)op, a4, 16, fo,
                               0xD2000000u | (op << 8), "trans200", sweep, diff_budget, op_fail, tr, 1);
            g_vm_es_patch_n = 0;
        }
    }

    // (e2) anim-search opcodes with a SECOND live object in range: covers the
    //      object-found paths of the sub_158xx family (word-write of
    //      [si+17DDh] into ds:0x3B2, bounds models). Filter table entry at
    //      [0 - 0x6B34] is set to 0x00 so filter byte 0 matches type 0x0100's
    //      low byte; [si+17DD]=0x0100 exposes byte-vs-word 0x3B2 writes.
    {
        static const uint8_t SOPS[] = { 0x1F, 0x20, 0x21, 0x22, 0x23, 0x31 };
        for (uint8_t sop : SOPS) {
            FtWr second[12] = {
                { 0x372, 8 },
                { (uint16_t)(2 + 0x1355), FT_VM_TESTSEG },   // slot 2 alive
                { (uint16_t)(2 + 0x17DD), 0x0100 },          // type word (hi byte set!)
                { (uint16_t)(2 + 0x1535), 0x0100 },          // target X range
                { (uint16_t)(2 + 0x155D), 0x0200 },
                { (uint16_t)(2 + 0x14E5), 0x0140 },          // target Y range
                { (uint16_t)(2 + 0x150D), 0x0160 },
                { (uint16_t)(6 + 0x1535), 0x0110 },          // self X range (overrides noise)
                { (uint16_t)(6 + 0x155D), 0x0130 },
                { (uint16_t)(0x94CC), 0x0000 },              // filter table [0-0x6B34]: match 0
                { (uint16_t)(6 + 0x14E5), 0x0130 },          // self Y start (bounds sanity)
                { (uint16_t)(6 + 0x150D), 0x0150 },
            };
            uint8_t a4[16]; memset(a4, 0, 16);               // filter byte 0 + zero args
            ft_synth_case_vmop(sop, a4, 16, BASE, 0xD3000000u | (sop << 8),
                               "objfound", sweep, diff_budget, op_fail, second, 12);
        }
    }

    long bad_ops = 0;
    for (int op = 0; op < 256; op++) {
        if (!op_fail[op]) continue;
        bad_ops++;
        fprintf(stderr, "FNSELFTEST-VMOP-FAIL: op=%02X len=%d mode=%X fails=%ld\n",
                op, op < 0xD8 ? plan[op].data_len : -9,
                op < 0xD8 ? plan[op].mode_mask : 0, op_fail[op]);
    }
    fprintf(stderr,
        "FNSELFTEST-SUMMARY[sub_1424c]: sweep %ld/%ld, fuzz %ld/%ld — total cases=%ld "
        "fail=%ld, failing opcodes=%ld, orig-UB skipped=%ld%s\n",
        sweep.pass, sweep.cases, fuzz.pass, fuzz.cases,
        sweep.cases + fuzz.cases, sweep.fail + fuzz.fail, bad_ops, g_vmop_ub,
        (sweep.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (sweep.fail + fuzz.fail) ? 1 : 0;
}

} // namespace

// Entry point, called from main() BEFORE m2c::init (no game/SDL/threads).
// Returns -1 when FNSELFTEST is not set (normal game startup continues),
// else the process exit code (0 = all pass, 1 = divergence).
extern "C" int v2_fntest_selftest_env(void) {
    const char* env = getenv("FNSELFTEST");
    if (!env || !env[0]) return -1;

    uint32_t ds_lin = v2_fntest_game_ds_linear();
    if (ds_lin & 0xF) {
        fprintf(stderr, "FNSELFTEST: game DS base 0x%X not paragraph-aligned — abort\n", ds_lin);
        return 1;
    }
    fprintf(stderr, "FNSELFTEST: game DS at linear 0x%X (seg 0x%X), oracle = isolated m2c orig\n",
            ds_lin, ds_lin >> 4);
    v2_fntest_watchdog_enable = 1;   // single-threaded here: hang watchdog is safe
    v2_fntest_snap_game_ds(g_synth_base);
    ft_fill_tail(g_synth_base);      // out-of-window WORD reads at 0xFFFF (see tail note)
    if (const char* sh = getenv("FNSELFTEST_SHARD")) {   // "i/N" exhaustive split
        int i = 0, n = 1;
        if (sscanf(sh, "%d/%d", &i, &n) == 2 && n > 0 && i >= 0 && i < n) {
            g_shard_i = i; g_shard_n = n;
            fprintf(stderr, "FNSELFTEST: shard %d/%d\n", i, n);
        }
    }

    int rc = 0; bool matched = false;
    bool all = (strcmp(env, "all") == 0);
    if (all || strstr(env, "sub_15972")) { matched = true; rc |= ft_selftest_sub_15972(); }
    if (all || strstr(env, "sub_161a1")) { matched = true; rc |= ft_selftest_sub_161a1(); }
    if (all || strstr(env, "sub_15da8")) { matched = true; rc |= ft_selftest_snap(FT_SNAP_Y, 0x5DA80001); }
    if (all || strstr(env, "sub_15d6b")) { matched = true; rc |= ft_selftest_snap(FT_SNAP_X, 0x5D6B0001); }
    if (all || strstr(env, "sub_13d68")) { matched = true; rc |= ft_selftest_sub_13d68(); }
    if (all || strstr(env, "sub_13dd6")) { matched = true; rc |= ft_selftest_1345init(FT_SUB_13DD6, 0x13DD6001); }
    if (all || strstr(env, "sub_13e15")) { matched = true; rc |= ft_selftest_1345init(FT_SUB_13E15, 0x13E15001); }
    if (all || strstr(env, "sub_13c0c")) { matched = true; rc |= ft_selftest_sub_13c0c(); }
    if (all || strstr(env, "sub_17496")) { matched = true; rc |= ft_selftest_mover(FT_SUB_17496, 0x17496001); }
    if (all || strstr(env, "sub_1746c")) { matched = true; rc |= ft_selftest_mover(FT_SUB_1746C, 0x1746C001); }
    if (all || strstr(env, "sub_10704")) { matched = true; rc |= ft_selftest_scroll_apply(FT_SUB_10704, 0x2B82, 0x10704001); }
    if (all || strstr(env, "sub_10753")) { matched = true; rc |= ft_selftest_scroll_apply(FT_SUB_10753, 0x2B80, 0x10753001); }
    if (all || strstr(env, "sub_1064b")) { matched = true; rc |= ft_selftest_sub_1064b(); }
    if (all || strstr(env, "sub_101be")) { matched = true; rc |= ft_selftest_sub_101be(); }
    if (all || strstr(env, "sub_1424c") || strstr(env, "vmops")) { matched = true; rc |= ft_selftest_vmops(); }
    if (all || strstr(env, "sub_10255")) { matched = true; rc |= ft_selftest_palrot(FT_SUB_10255); }
    if (all || strstr(env, "sub_1020f")) { matched = true; rc |= ft_selftest_palrot(FT_SUB_1020F); }
    if (all || strstr(env, "sub_12fc6")) { matched = true; rc |= ft_selftest_deltafam(FT_SUB_12FC6, 0x12FC6001u); }
    if (all || strstr(env, "sub_12fcb")) { matched = true; rc |= ft_selftest_deltafam(FT_SUB_12FCB, 0x12FCB001u); }
    if (all || strstr(env, "sub_12fd0")) { matched = true; rc |= ft_selftest_deltafam(FT_SUB_12FD0, 0x12FD0001u); }
    if (all || strstr(env, "sub_158aa")) { matched = true; rc |= ft_selftest_search(FT_SUB_158AA, 0x158AA001u); }
    if (all || strstr(env, "sub_158b9")) { matched = true; rc |= ft_selftest_search(FT_SUB_158B9, 0x158B9001u); }
    if (all || strstr(env, "sub_158c8")) { matched = true; rc |= ft_selftest_search(FT_SUB_158C8, 0x158C8001u); }
    if (all || strstr(env, "sub_158d7")) { matched = true; rc |= ft_selftest_search(FT_SUB_158D7, 0x158D7001u); }
    if (all || strstr(env, "sub_158e6")) { matched = true; rc |= ft_selftest_search(FT_SUB_158E6, 0x158E6001u); }
    if (all || strstr(env, "sub_1303a")) { matched = true; rc |= ft_selftest_anim(FT_SUB_1303A, 0x1303A001u); }
    if (all || strstr(env, "sub_13031")) { matched = true; rc |= ft_selftest_anim(FT_SUB_13031, 0x13031001u); }
    if (all || strstr(env, "sub_1614e")) { matched = true; rc |= ft_selftest_scan(FT_SUB_1614E, 0x1614E001u); }
    if (all || strstr(env, "sub_15c37")) { matched = true; rc |= ft_selftest_scan(FT_SUB_15C37, 0x15C37001u); }
    if (all || strstr(env, "sub_15c93")) { matched = true; rc |= ft_selftest_scan(FT_SUB_15C93, 0x15C93001u); }
    if (all || strstr(env, "sub_15afd")) { matched = true; rc |= ft_selftest_scan(FT_SUB_15AFD, 0x15AFD001u); }
    if (!matched) {
        fprintf(stderr, "FNSELFTEST: no registered function matches '%s'\n", env);
        return 1;
    }
    return rc;
}
