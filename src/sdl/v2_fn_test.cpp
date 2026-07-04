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

extern int v2_dbg_pre_vm_iter;  // game frame counter — context for FAIL logs

namespace {

enum FtId { FT_SUB_15972 = 0, FT_COUNT };

struct FtRegs { uint16_t ax, bx, cx, dx, si, di, bp; };

struct FtSlot {
    bool     enabled = false;
    bool     armed   = false;   // entry seen, waiting for the RETN hook
    FtRegs   regs{};
    long     calls = 0, pass = 0, fail = 0;
    uint8_t  ds_in[0x10000];
};

FtSlot      g_slot[FT_COUNT];
const char* g_name[FT_COUNT] = { "sub_15972" };

uint8_t g_scratch[0x10000];   // v2 executes here; never the live shadow
bool    g_init_done   = false;
bool    g_any_enabled = false;

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
}
