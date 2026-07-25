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
#include "v2_ds_layout.h"

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
extern "C" void     v2_fntest_call_sub_11192(uint8_t* test_shadow);
extern "C" void     v2_fntest_call_sub_111a1(uint8_t* test_shadow);
extern "C" void     v2_fntest_call_sub_111df(uint8_t* test_shadow);
extern "C" void     v2_fntest_call_sub_11784(uint8_t* test_shadow);
extern "C" void     v2_fntest_call_sub_137f1(uint8_t* test_shadow);
extern "C" void     v2_fntest_call_sub_12fb3(uint8_t* test_shadow);
extern "C" void     v2_fntest_call_sub_12ca3(uint8_t* test_shadow);
extern "C" void     v2_fntest_call_sub_12ce4(uint8_t* test_shadow);
extern "C" void     v2_fntest_call_sub_108b8(uint8_t* test_shadow);
extern "C" void     v2_fntest_call_sub_11397(uint8_t* test_shadow);
extern "C" void     v2_fntest_call_sub_113b0(uint8_t* test_shadow);
extern "C" void     v2_fntest_call_sub_113d8(uint8_t* test_shadow);
extern "C" void     v2_fntest_call_sub_116e3(uint8_t* test_shadow);
extern "C" uint16_t v2_fntest_call_sub_11383(uint8_t* test_shadow);
extern "C" uint16_t v2_fntest_call_sub_1133a(uint8_t* test_shadow, uint16_t di);
extern "C" uint16_t v2_fntest_call_sub_1241e(uint8_t* test_shadow, uint16_t al, uint16_t si, uint16_t di);
extern "C" void     v2_fntest_call_sub_12816(uint8_t* test_shadow);
extern "C" void     v2_fntest_call_sub_12515(uint8_t* test_shadow, uint16_t ax);
extern "C" uint16_t v2_fntest_call_sub_12529(uint8_t* test_shadow, uint16_t bx);
extern "C" void     v2_fntest_call_sub_13a0e(uint8_t* test_shadow);
extern "C" void     v2_fntest_set_animdata(const uint8_t* data, uint32_t len);
extern "C" void     v2_fntest_ensure_drawinfo(void);
extern "C" void     v2_fntest_call_sub_173c7(uint8_t* test_shadow);
extern "C" void     v2_fntest_set_tilemap(const uint8_t* data, uint32_t len);
extern "C" uint8_t* v2_fntest_tilemap_ptr(void);
extern "C" void     v2_fntest_set_gs_tiledata(const uint8_t* data, uint32_t len);
extern "C" uint8_t* v2_fntest_fs_ptr(void);
extern "C" void     v2_fntest_clear_fs(uint8_t fill);
extern "C" void     v2_fntest_call_sub_14207(uint8_t* test_shadow);
extern "C" uint16_t v2_fntest_call_sub_112ae(uint8_t* test_shadow, uint16_t di);
extern "C" uint16_t v2_fntest_call_sub_1167a(uint8_t* test_shadow, uint16_t di);
extern "C" uint16_t v2_fntest_call_sub_116ae(uint8_t* test_shadow, uint16_t di);
extern "C" uint8_t* v2_fntest_sprite_shadow_win(uint32_t off);
extern uint8_t* v2_vm_get_shadow_chunk();   // 0x2ABA0-byte chunk-buffer shadow
extern "C" void v2_fntest_call_sub_11b0b(uint8_t* test_shadow);
extern "C" void v2_fntest_call_sub_10813(uint8_t* test_shadow);
extern "C" void     v2_fntest_call_sub_12d2c(uint8_t* test_shadow);
extern "C" void     v2_fntest_call_sub_13ba5(uint8_t* test_shadow);
extern "C" void     v2_fntest_call_sub_11446(uint8_t* test_shadow);
extern "C" void     v2_fntest_call_sub_11569(uint8_t* test_shadow, uint16_t di);
extern "C" int      v2_fntest_call_sub_15911(uint8_t* test_shadow, uint16_t di, uint16_t si);
extern "C" void     v2_fntest_call_sub_12549(uint8_t* test_shadow, uint16_t ax);
extern "C" int      v2_fntest_call_sub_11cbb(uint8_t* test_shadow);
extern "C" void     v2_fntest_call_sub_1450b(uint8_t* test_shadow, uint16_t al, uint16_t si, uint16_t di);
extern "C" void     v2_fntest_call_sub_10e99(uint8_t* test_shadow);
extern "C" void     v2_fntest_call_sub_11c52(uint8_t* test_shadow);
extern "C" int      v2_fntest_call_sub_15d3c(uint8_t* test_shadow, uint16_t si, uint16_t di);
extern "C" int      v2_fntest_call_sub_15d42(uint8_t* test_shadow, uint16_t si, uint16_t di);
extern "C" int      v2_fntest_call_sub_15cef(uint8_t* test_shadow, uint16_t si, uint16_t di);
extern "C" int      v2_fntest_call_sub_15cf5(uint8_t* test_shadow, uint16_t si, uint16_t di);
extern "C" int      v2_fntest_call_sub_15de5(uint8_t* test_shadow, uint16_t filter, uint16_t di);
extern "C" int      v2_fntest_call_sub_15df2(uint8_t* test_shadow, uint16_t filter, uint16_t di);
extern "C" int      v2_fntest_call_sub_15fb1(uint8_t* test_shadow, uint16_t filter, uint16_t di);
extern "C" int      v2_fntest_call_sub_15fbe(uint8_t* test_shadow, uint16_t filter, uint16_t di);
extern "C" int      v2_fntest_call_sub_1603e(uint8_t* test_shadow, uint16_t filter, uint16_t di);
extern "C" int32_t  v2_fntest_call_sub_158f5(uint8_t* test_shadow, uint16_t filter, uint16_t di);
extern "C" void     v2_fntest_call_sub_1592d(uint8_t* test_shadow, uint16_t ax, uint16_t di);
extern "C" void     v2_fntest_call_sub_15505(uint8_t* test_shadow, uint16_t si, uint16_t di);
extern "C" void     v2_fntest_call_sub_15517(uint8_t* test_shadow);
extern "C" void     v2_fntest_call_sub_1555c(uint8_t* test_shadow, uint16_t si);
extern "C" void     v2_fntest_call_sub_15569(uint8_t* test_shadow, uint16_t si);
extern "C" void     v2_fntest_call_sub_15530(uint8_t* test_shadow);
extern "C" void     v2_fntest_call_sub_15546(uint8_t* test_shadow);
extern "C" int      v2_fntest_call_sub_155d6(uint8_t* test_shadow, uint16_t pc);
extern "C" int      v2_fntest_call_sub_156c0(uint8_t* test_shadow, uint16_t pc);
extern "C" int      v2_fntest_call_sub_1584e(uint8_t* test_shadow, uint16_t pc);
extern "C" int      v2_fntest_call_sub_157eb(uint8_t* test_shadow, uint16_t pc);
extern "C" void     v2_fntest_call_sub_16235(uint8_t* test_shadow, uint16_t si, uint16_t di);
extern "C" uint16_t v2_fntest_call_sub_16243(uint8_t* test_shadow, uint16_t di);
extern "C" int16_t  v2_fntest_call_sub_16390(uint8_t* test_shadow, uint16_t ax, uint16_t si, uint16_t di);
extern "C" int      v2_fntest_call_search(uint8_t* test_shadow, int which,
                                          uint16_t filter, uint16_t obj);
extern "C" int32_t  v2_fntest_call_scan(uint8_t* test_shadow, int which,
                                        uint16_t filter, uint16_t obj);
extern "C" uint32_t v2_fntest_call_read_chunk(uint16_t chunk_id, uint8_t* dest,
                                              uint8_t* ring_out, uint8_t* hdr10_out);
extern "C" uint32_t v2_fntest_call_raw_chunk(uint16_t chunk_id, uint8_t* dest,
                                             uint8_t* hdr10_out);
extern "C" void     v2_fntest_alloc_drawinfo(void);
extern "C" void     v2_fntest_call_pal(uint8_t* test_shadow, int which, uint8_t* dac_out768);
extern "C" void     v2_fntest_reset_v2_dac(void);
extern "C" void     v2_fntest_reset_orig_dac(void);
extern "C" void     v2_fntest_orig_call_pal(int which, uint8_t* ds_image);
extern "C" void     v2_fetch_orig_dac(uint8_t* rgb768);
extern "C" uint16_t v2_fntest_get_word_10980(void);
extern "C" void     v2_fntest_put_word_10980(uint16_t v);
extern "C" uint16_t v2_fntest_es_override;
extern "C" int      v2_fntest_set_data_file(const char* path);
extern "C" int      v2_fntest_set_data_file_v2(const char* path);
extern "C" uint8_t  v2_vm_shadow_fs[];
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
            FT_SUB_15AFD = 30, FT_SUB_10982 = 31, FT_SUB_10CD8 = 32,
            FT_SUB_10FE6 = 33, FT_SUB_10FFC = 34,
            FT_SUB_11192 = 35, FT_SUB_111A1 = 36, FT_SUB_111DF = 37,
            FT_SUB_11784 = 38, FT_SUB_137F1 = 39, FT_SUB_12FB3 = 40,
            FT_SUB_12CA3 = 41, FT_SUB_12CE4 = 42, FT_SUB_108B8 = 43,
            FT_SUB_11397 = 44, FT_SUB_113B0 = 45, FT_SUB_113D8 = 46,
            FT_SUB_116E3 = 47,
            FT_SUB_11383 = 48, FT_SUB_1133A = 49, FT_SUB_1241E = 50,
            FT_SUB_12816 = 51, FT_SUB_12515 = 52, FT_SUB_12529 = 53,
            FT_SUB_13A0E = 54, FT_SUB_1450B = 55, FT_SUB_10E99 = 56,
            FT_SUB_15D3C = 57, FT_SUB_15D42 = 58, FT_SUB_11C52 = 59,
            FT_SUB_13BA5 = 60, FT_SUB_11446 = 61, FT_SUB_11569 = 62,
            FT_SUB_15911 = 63, FT_SUB_12549 = 64, FT_SUB_11CBB = 65,
            FT_SUB_173C7 = 66, FT_SUB_14207 = 67,
            FT_SUB_112AE = 68, FT_SUB_12D2C = 69,
            FT_SUB_1167A = 70, FT_SUB_116AE = 71, FT_SUB_11B0B = 72,
            FT_SUB_10813 = 73,
            FT_SUB_15CEF = 74, FT_SUB_15CF5 = 75,
            FT_SUB_15DE5 = 76, FT_SUB_15DF2 = 77,
            FT_SUB_15FB1 = 78, FT_SUB_15FBE = 79,
            FT_SUB_1603E = 80,
            FT_SUB_160CF = 81, FT_SUB_15AE9 = 82, FT_SUB_1589B = 83,
            FT_SUB_159C6 = 84, FT_SUB_159D3 = 85, FT_SUB_159DF = 86,
            FT_SUB_15A57 = 87, FT_SUB_15AC4 = 88,
            FT_SUB_158F5 = 89, FT_SUB_1592D = 90,
            FT_SUB_15505 = 91, FT_SUB_15517 = 92,
            FT_SUB_1555C = 93, FT_SUB_15569 = 94,
            FT_SUB_15530 = 95, FT_SUB_15546 = 96,
            FT_SUB_155D6 = 97, FT_SUB_156C0 = 98,
            FT_SUB_1584E = 99, FT_SUB_157EB = 100,
            FT_SUB_16235 = 101, FT_SUB_16243 = 102,
            FT_SUB_16390 = 103,
            FT_COUNT };

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
                                 "sub_15afd", "sub_10982", "sub_10cd8",
                                 "sub_10fe6", "sub_10ffc",
                                 "sub_11192", "sub_111a1", "sub_111df",
                                 "sub_11784", "sub_137f1", "sub_12fb3",
                                 "sub_12ca3", "sub_12ce4", "sub_108b8",
                                 "sub_11397", "sub_113b0", "sub_113d8",
                                 "sub_116e3",
                                 "sub_11383", "sub_1133a", "sub_1241e",
                                 "sub_12816", "sub_12515", "sub_12529",
                                 "sub_13a0e", "sub_1450b", "sub_10e99",
                                 "sub_15d3c", "sub_15d42", "sub_11c52",
                                 "sub_13ba5", "sub_11446", "sub_11569",
                                 "sub_15911", "sub_12549", "sub_11cbb",
                                 "sub_173c7", "sub_14207",
                                 "sub_112ae", "sub_12d2c",
                                 "sub_1167a", "sub_116ae", "sub_11b0b",
                                 "sub_10813",
                                 "sub_15cef", "sub_15cf5",
                                 "sub_15de5", "sub_15df2",
                                 "sub_15fb1", "sub_15fbe",
                                 "sub_1603e",
                                 "sub_160cf", "sub_15ae9", "sub_1589b",
                                 "sub_159c6", "sub_159d3", "sub_159df",
                                 "sub_15a57", "sub_15ac4",
                                 "sub_158f5", "sub_1592d",
                                 "sub_15505", "sub_15517",
                                 "sub_1555c", "sub_15569",
                                 "sub_15530", "sub_15546",
                                 "sub_155d6", "sub_156c0",
                                 "sub_1584e", "sub_157eb",
                                 "sub_16235", "sub_16243",
                                 "sub_16390" };

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
    ft_wr16(g_synth_in, (uint16_t)(di + OBJ_WORLD_Y), y);
    ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_Y1), y_end);
    ft_wr16(g_synth_in, (uint16_t)(di + 0x14E5), y_start);
    ft_wr16(g_synth_in, (uint16_t)(di + OBJ_FRAC_Y), 0xAAAA);

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
    ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_X0), f.di_x_lo);
    ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_X1), f.di_x_hi);
    ft_wr16(g_synth_in, (uint16_t)(si + OBJ_BBOX_X0), f.si_x_lo);
    ft_wr16(g_synth_in, (uint16_t)(si + OBJ_BBOX_X1), f.si_x_hi);
    ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_Y1), f.di_y_end);
    ft_wr16(g_synth_in, (uint16_t)(di + OBJ_WORLD_Y), f.di_y);
    ft_wr16(g_synth_in, (uint16_t)(di + 0x13CD), f.di_yc);
    ft_wr16(g_synth_in, (uint16_t)(si + 0x14E5), f.si_y_st);
    ft_wr16(g_synth_in, (uint16_t)(si + OBJ_WORLD_Y), f.si_y);
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
const FtSnapLayout FT_SNAP_Y = { FT_SUB_15DA8, OBJ_WORLD_Y, 0x14E5, OBJ_BBOX_Y1, OBJ_FRAC_Y, v2_fntest_call_sub_15da8 };
const FtSnapLayout FT_SNAP_X = { FT_SUB_15D6B, OBJ_WORLD_X, OBJ_BBOX_X0, OBJ_BBOX_X1, OBJ_FRAC_X, v2_fntest_call_sub_15d6b };

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
    ft_wr16(g_synth_in, (uint16_t)(si + OBJ_SUB_COUNT), count);
    for (uint16_t d = 0; d < 0x100; d += 2) {
        uint8_t o = occ128[d >> 1];
        if (o & 1) ft_wr16(g_synth_in, (uint16_t)(d + 0x44D),  0x8000);
        if (o & 2) ft_wr16(g_synth_in, (uint16_t)(d + OBJ_DIRTY_MODE), 0x204);
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
    ft_wr16(g_synth_in, (uint16_t)(si + OBJ_SUB_SLOT), start);
    ft_wr16(g_synth_in, (uint16_t)(si + OBJ_SUB_END), end);
    ft_wr16(g_synth_in, (uint16_t)(si + OBJ_FLAGS), flags);
    ft_wr16(g_synth_in, (uint16_t)(si + 0x1855), spr_base);
    ft_wr16(g_synth_in, DS_SEG_SPRITE, spr_seg);
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
        ft_wr16(g_synth_in, (uint16_t)(si + OBJ_CODE_SEG), objs[i].alive);
        ft_wr16(g_synth_in, (uint16_t)(si + OBJ_FLAGS), objs[i].flags);
        ft_wr16(g_synth_in, (uint16_t)(si + OBJ_WORLD_X), objs[i].x);
        ft_wr16(g_synth_in, (uint16_t)(si + OBJ_HALF_W), objs[i].hw);
        ft_wr16(g_synth_in, (uint16_t)(si + OBJ_WORLD_Y), objs[i].y);
        ft_wr16(g_synth_in, (uint16_t)(si + OBJ_HALF_H), objs[i].hh);
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
        ft_wr16(g_synth_in, OBJ_WORLD_X, ox);
        ft_wr16(g_synth_in, OBJ_WORLD_Y, oy);
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
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_WORLD_X), rng.w());
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_WORLD_Y), rng.w());
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
        g_synth_in[(uint16_t)(DS_PAL_OUT + off)] = (uint8_t)trng.next();
        g_synth_in[(uint16_t)(DS_PAL_SRC + off)] = (uint8_t)trng.next();
    }
    ft_wr16(g_synth_in, 0x8504, 0xBBBB);
    g_synth_in[0x8506] = 0xBB;
    ft_wr16(g_synth_in, DS_PAL_REQ, 0xBBBB);

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
// sub_10255 / sub_1020f <-> v2_pal_rotate_fwd_10255 / v2_pal_rotate_back_1020f (palette entry
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
        g_synth_in[(uint16_t)(DS_PAL_OUT + off)] = (uint8_t)trng.next();
        g_synth_in[(uint16_t)(DS_PAL_SRC + off)] = (uint8_t)trng.next();
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
        ft_synth_case_palrot(id, 0, DS_PAL_OUT, (uint8_t)c, (uint8_t)e,
                             0xC0000000u | (c << 8) | e, "exh", exh, diff_budget);
    }
    // Slot addressing: si=0..7 over a small cur/end grid, both game bases.
    for (uint16_t si = 0; si < 8; si++)
        for (uint32_t c = 0; c <= 0xF0; c += 0x3C) for (uint32_t e = 0; e <= 0xF0; e += 0x3C) {
            if (fwd ? (c > e) : (c < e)) continue;
            ft_synth_case_palrot(id, si, DS_PAL_SRC, (uint8_t)c, (uint8_t)e,
                                 0xC1000000u | (si << 16) | (c << 8) | e, "exh", exh, diff_budget);
        }

    // Wrong-order half-plane (wrapped REP MOVSB count) — sampled diagonals.
    static const uint8_t DELTA[] = { 1, 2, 3, 5, 17, 85, 255 };
    for (uint8_t d : DELTA) for (uint32_t c = 0; c <= 0xF0; c += 0x10) {
        uint8_t cur = (uint8_t)c, endi;
        if (fwd) endi = (uint8_t)(cur - d);   // end<cur → wrapped count for 10255
        else     endi = (uint8_t)(cur + d);   // cur<end → wrapped count for 1020f
        ft_synth_case_palrot(id, (uint16_t)(c & 7), DS_PAL_OUT, cur, endi,
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
        uint16_t dxv = (rng.next() & 1) ? DS_PAL_OUT : DS_PAL_SRC;
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
// sub_12fc6 / sub_12fcb / sub_12fd0 <-> v2_subsprite_walk_12fc6/12fcb/12fd0 (sub-sprite
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
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_CODE_SEG), objs[i].alive);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_SUB_COUNT), objs[i].count);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_WORLD_X), objs[i].x);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_X_PREV), objs[i].xs);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_WORLD_Y), objs[i].y);
        ft_wr16(g_synth_in, (uint16_t)(di + 0x13CD), objs[i].ys);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_SUB_SLOT), objs[i].sa);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_SUB_END), objs[i].se);
    }
    // Slot-area background noise so += effects are visible on any slot.
    FtRng bg(bg_seed);
    for (uint32_t a = 0; a < 0x100; a += 2) {
        ft_wr16(g_synth_in, (uint16_t)(a + 0x64D), bg.w());
        ft_wr16(g_synth_in, (uint16_t)(a + 0x74D), bg.w());
        ft_wr16(g_synth_in, (uint16_t)(a + OBJ_DIRTY_MODE), bg.w());
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
    ft_wr16(g_synth_in, DS_SEG_TILEMAP, FT_VM_TESTSEG);          // tilemap segment
    ft_wr16(g_synth_in, 0x25DC, (uint16_t)tw);           // map width (tiles)
    ft_wr16(g_synth_in, 0x25DE, (uint16_t)th);           // map height
    for (int y = 0; y < th; y++)
        ft_wr16(g_synth_in, (uint16_t)(y * 2 - LUT_ROW_BASE), (uint16_t)(y * tw * 2)); // row offsets
    // canaries on the scratch protocol
    ft_wr16(g_synth_in, 0x34, 0xBBBB); ft_wr16(g_synth_in, 0x36, 0xBBBB);
    ft_wr16(g_synth_in, 0x38, 0xBBBB); ft_wr16(g_synth_in, 0x3A, 0xBBBB);
    ft_wr16(g_synth_in, 0x3B2, 0xBBBB); ft_wr16(g_synth_in, 0x3B4, 0xBBBB);
    FtRng bg(bg_seed);
    static const uint16_t SF[] = { OBJ_BBOX_X0, OBJ_BBOX_X1, 0x14E5, OBJ_BBOX_Y1, OBJ_FLAGS, OBJ_TYPE_ID, OBJ_VEL_Y };
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
                *(uint16_t*)(g_synth_in + obj + OBJ_FLAGS),
                *(uint16_t*)(g_synth_in + obj + OBJ_BBOX_X0), *(uint16_t*)(g_synth_in + obj + OBJ_BBOX_X1),
                *(uint16_t*)(g_synth_in + obj + 0x14E5), *(uint16_t*)(g_synth_in + obj + OBJ_BBOX_Y1),
                *(uint16_t*)(g_synth_in + obj + OBJ_VEL_Y), *(uint16_t*)(g_synth_in + obj + OBJ_WORLD_X),
                *(uint16_t*)(g_synth_in + 2 + OBJ_CODE_SEG), *(uint16_t*)(g_synth_in + 2 + OBJ_TYPE_ID),
                *(uint16_t*)(g_synth_in + 2 + OBJ_BBOX_X0), *(uint16_t*)(g_synth_in + 2 + OBJ_BBOX_X1),
                *(uint16_t*)(g_synth_in + 2 + 0x14E5), *(uint16_t*)(g_synth_in + 2 + OBJ_BBOX_Y1));
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
    const uint16_t FT_ADDR = (uint16_t)(0 - LUT_SCAN_FILTER);

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
        w[nw++] = { (uint16_t)(OBJ + OBJ_FLAGS), c.flags };
        w[nw++] = { (uint16_t)(OBJ + OBJ_BBOX_X0), c.xs };
        w[nw++] = { (uint16_t)(OBJ + OBJ_BBOX_X1), c.xe };
        w[nw++] = { (uint16_t)(OBJ + 0x14E5), c.ys };
        w[nw++] = { (uint16_t)(OBJ + OBJ_BBOX_Y1), c.ye };
        w[nw++] = { FT_ADDR, (uint16_t)(c.f0 | ((c.f1 & 0xFF) << 8)) };
        if (c.o2alive) {
            w[nw++] = { (uint16_t)(2 + OBJ_CODE_SEG), FT_VM_TESTSEG };
            w[nw++] = { (uint16_t)(2 + OBJ_TYPE_ID), c.o2t };
            w[nw++] = { (uint16_t)(2 + OBJ_BBOX_X0), c.o2xs };
            w[nw++] = { (uint16_t)(2 + OBJ_BBOX_X1), c.o2xe };
            w[nw++] = { (uint16_t)(2 + 0x14E5), c.o2ys };
            w[nw++] = { (uint16_t)(2 + OBJ_BBOX_Y1), c.o2ye };
        }
        ft_synth_case_search(id, 0, OBJ, w, nw, T, 4, 4,
                             seed ^ (0xA0000000u + ci), "grid", grid, diff_budget);
        ci++;
    }

    // Exhaustive: the unit's own axis field over all 65536 values.
    uint16_t axis_field =
        (id == FT_SUB_158AA) ? (uint16_t)(OBJ + OBJ_BBOX_X0) :
        (id == FT_SUB_158B9) ? (uint16_t)(OBJ + OBJ_BBOX_X1) :
        (id == FT_SUB_158C8) ? (uint16_t)(OBJ + 0x14E5) :
        (id == FT_SUB_158D7) ? (uint16_t)(OBJ + OBJ_BBOX_Y1) :
                               (uint16_t)(OBJ + OBJ_BBOX_X1);   // 158e6, flip=0 → X_end
    long exh_done = 0;
    if (fz_only < 0) for (uint32_t v = 0; v <= 0xFFFF; v++) {
        if (!ft_shard_mine(v)) continue;             // FNSELFTEST_SHARD split
        if ((++exh_done % 1000) == 0)
            fprintf(stderr, "FNSELFTEST-PROG[%s]: exh %ld (v=%04X)\n", g_name[id], exh_done, v);
        FtWr w[7] = {
            { (uint16_t)(OBJ + OBJ_FLAGS), 0x8000 },
            { (uint16_t)(OBJ + OBJ_BBOX_X0), 0x0010 },
            { (uint16_t)(OBJ + OBJ_BBOX_X1), 0x0020 },
            { (uint16_t)(OBJ + 0x14E5), 0x0008 },
            { (uint16_t)(OBJ + OBJ_BBOX_Y1), 0x000E },
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
        w[nw++] = { (uint16_t)(OBJ + OBJ_FLAGS), (uint16_t)(0x8000 | (rng.next() & 0x40)) };
        w[nw++] = { (uint16_t)(OBJ + OBJ_BBOX_X0), rng.w() };
        w[nw++] = { (uint16_t)(OBJ + OBJ_BBOX_X1), rng.w() };
        w[nw++] = { (uint16_t)(OBJ + 0x14E5), rng.w() };
        w[nw++] = { (uint16_t)(OBJ + OBJ_BBOX_Y1), rng.w() };
        w[nw++] = { FT_ADDR, (uint16_t)(0xFF00 | (rng.next() & 0x3F)) };
        if (rng.next() & 1) {
            w[nw++] = { (uint16_t)(2 + OBJ_CODE_SEG), FT_VM_TESTSEG };
            w[nw++] = { (uint16_t)(2 + OBJ_TYPE_ID), (uint16_t)(rng.next() & 0x013F) };
            w[nw++] = { (uint16_t)(2 + OBJ_BBOX_X0), rng.w() };
            w[nw++] = { (uint16_t)(2 + OBJ_BBOX_X1), rng.w() };
            w[nw++] = { (uint16_t)(2 + 0x14E5), rng.w() };
            w[nw++] = { (uint16_t)(2 + OBJ_BBOX_Y1), rng.w() };
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
        ft_wr16(g_synth_in, (uint16_t)(s + OBJ_CODE_SEG), 0);
    for (uint32_t a = 0x2E5C; a <= 0x2E7C; a += 2) ft_wr16(g_synth_in, a, 0);
    ft_wr16(g_synth_in, DS_SEG_TILEMAP, FT_VM_TESTSEG);           // tilemap segment (15afd)
    ft_wr16(g_synth_in, 0x25DC, (uint16_t)tw);
    ft_wr16(g_synth_in, 0x25DE, (uint16_t)th);
    for (int y = 0; y < th; y++)
        ft_wr16(g_synth_in, (uint16_t)(y * 2 - LUT_ROW_BASE), (uint16_t)(y * tw * 2));
    // canaries on the scratch protocol (0x6C/0x6E belong to sub_15afd only)
    ft_wr16(g_synth_in, 0x32, 0xBBBB); ft_wr16(g_synth_in, 0x34, 0xBBBB);
    ft_wr16(g_synth_in, 0x36, 0xBBBB); ft_wr16(g_synth_in, 0x38, 0xBBBB);
    ft_wr16(g_synth_in, 0x3A, 0xBBBB);
    ft_wr16(g_synth_in, 0x6C, 0xBBBB); ft_wr16(g_synth_in, 0x6E, 0xBBBB);
    FtRng bg(bg_seed);
    static const uint16_t SF[] = { OBJ_BBOX_X0, OBJ_BBOX_X1, 0x14E5, OBJ_BBOX_Y1, OBJ_FLAGS, OBJ_TYPE_ID,
                                   OBJ_VEL_Y, OBJ_VEL_X, OBJ_WORLD_Y, 0x13CD, OBJ_WORLD_X };
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
                *(uint16_t*)(g_synth_in + obj + OBJ_VEL_Y), *(uint16_t*)(g_synth_in + obj + OBJ_VEL_X),
                *(uint16_t*)(g_synth_in + obj + OBJ_WORLD_Y), *(uint16_t*)(g_synth_in + obj + 0x13CD),
                *(uint16_t*)(g_synth_in + obj + OBJ_BBOX_Y1), *(uint16_t*)(g_synth_in + obj + OBJ_WORLD_X),
                *(uint16_t*)(g_synth_in + 2 + OBJ_CODE_SEG), *(uint16_t*)(g_synth_in + 2 + OBJ_TYPE_ID),
                *(uint16_t*)(g_synth_in + 2 + OBJ_VEL_Y), *(uint16_t*)(g_synth_in + 2 + OBJ_VEL_X));
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
    const uint16_t FT_ADDR = (uint16_t)(0 - LUT_SCAN_FILTER);      // filter=0 chain
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
    const uint16_t VF = is_afd ? OBJ_WORLD_Y :
                        (id == FT_SUB_15C37) ? OBJ_VEL_X : OBJ_VEL_Y;
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
        w[nw++] = { (uint16_t)(OBJ + OBJ_BBOX_X0), 0x0140 };   // self X in partner range
        w[nw++] = { (uint16_t)(OBJ + OBJ_BBOX_X1), 0x0150 };
        w[nw++] = { (uint16_t)(OBJ + 0x14E5), 0x0140 };   // self Y overlapping
        w[nw++] = { (uint16_t)(OBJ + OBJ_BBOX_Y1), 0x0150 };
        w[nw++] = { (uint16_t)(OBJ + OBJ_VEL_Y), 0x0000 };   // default vels
        w[nw++] = { (uint16_t)(OBJ + OBJ_VEL_X), 0x0000 };
        w[nw++] = { (uint16_t)(OBJ + VF), vg.a };
        w[nw++] = { (uint16_t)(OBJ + OBJ_WORLD_X), 0x0148 };   // X for 15afd probes
        w[nw++] = { FT_ADDR, 0xFF05 };
        if (!is_afd) {
            w[nw++] = { (uint16_t)(2 + OBJ_CODE_SEG), 1 };      // live partner in slot 2
            w[nw++] = { (uint16_t)(2 + OBJ_TYPE_ID), 0x0005 }; // type matches filter
            w[nw++] = { (uint16_t)(2 + VP), vg.b };
            w[nw++] = { (uint16_t)(2 + OBJ_BBOX_X0), pb.xs };
            w[nw++] = { (uint16_t)(2 + OBJ_BBOX_X1), pb.xe };
            w[nw++] = { (uint16_t)(2 + 0x14E5), pb.ys };
            w[nw++] = { (uint16_t)(2 + OBJ_BBOX_Y1), pb.ye };
            w[nw++] = { (uint16_t)(2 + OBJ_VEL_Y), pb.vy };
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
            w[nw++] = { (uint16_t)(OBJ + OBJ_WORLD_Y), c.y };
            w[nw++] = { (uint16_t)(OBJ + 0x13CD), c.yp };
            w[nw++] = { (uint16_t)(OBJ + OBJ_BBOX_Y1), c.ye };
            w[nw++] = { (uint16_t)(OBJ + OBJ_WORLD_X), c.x };
            w[nw++] = { (uint16_t)(OBJ + OBJ_BBOX_X0), c.xs };
            w[nw++] = { (uint16_t)(OBJ + OBJ_BBOX_X1), c.xe };
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
        w[nw++] = { (uint16_t)(OBJ + OBJ_BBOX_X0), 0x0140 };
        w[nw++] = { (uint16_t)(OBJ + OBJ_BBOX_X1), 0x0150 };
        w[nw++] = { (uint16_t)(OBJ + 0x14E5), 0x0140 };
        w[nw++] = { (uint16_t)(OBJ + OBJ_BBOX_Y1), 0x0150 };
        w[nw++] = { (uint16_t)(OBJ + OBJ_VEL_Y), 0x0000 };
        w[nw++] = { (uint16_t)(OBJ + OBJ_VEL_X), 0x0000 };
        w[nw++] = { (uint16_t)(OBJ + OBJ_WORLD_X), 0x0148 };
        w[nw++] = { FT_ADDR, 0xFF05 };
        if (!is_afd) {
            w[nw++] = { (uint16_t)(OBJ + VF), 0x4220 };
            w[nw++] = { (uint16_t)(2 + OBJ_CODE_SEG), 1 };
            w[nw++] = { (uint16_t)(2 + OBJ_TYPE_ID), 0x0005 };
            w[nw++] = { (uint16_t)(2 + VP), (uint16_t)v };
            w[nw++] = { (uint16_t)(2 + OBJ_BBOX_X0), 0x0100 };
            w[nw++] = { (uint16_t)(2 + OBJ_BBOX_X1), 0x0200 };
            w[nw++] = { (uint16_t)(2 + 0x14E5), 0x0100 };
            w[nw++] = { (uint16_t)(2 + OBJ_BBOX_Y1), 0x0200 };
            w[nw++] = { (uint16_t)(2 + OBJ_VEL_Y), 0x0000 };
        } else {
            w[nw++] = { (uint16_t)(OBJ + OBJ_WORLD_Y), 0x4220 };
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
        w[nw++] = { (uint16_t)(OBJ + OBJ_BBOX_X0), rng.w() };
        w[nw++] = { (uint16_t)(OBJ + OBJ_BBOX_X1), rng.w() };
        w[nw++] = { (uint16_t)(OBJ + 0x14E5), rng.w() };
        w[nw++] = { (uint16_t)(OBJ + OBJ_BBOX_Y1), rng.w() };
        w[nw++] = { (uint16_t)(OBJ + OBJ_VEL_Y), rng.w() };
        w[nw++] = { (uint16_t)(OBJ + OBJ_VEL_X), rng.w() };
        w[nw++] = { (uint16_t)(OBJ + OBJ_WORLD_Y), rng.w() };
        w[nw++] = { (uint16_t)(OBJ + 0x13CD), rng.w() };
        w[nw++] = { (uint16_t)(OBJ + OBJ_WORLD_X), rng.w() };
        w[nw++] = { FT_ADDR, (uint16_t)(0xFF00 | (rng.next() & 0x3F)) };
        // two random slope-table entries (15afd probes; harmless otherwise)
        w[nw++] = { (uint16_t)(SLOPE_BASE + (rng.next() & 0xFF)), (uint16_t)(rng.next() & 0xF) };
        w[nw++] = { (uint16_t)(SLOPE_BASE + (rng.next() & 0xFF)), (uint16_t)(rng.next() & 0xF) };
        int live = (int)(rng.next() % 3);                  // 0-2 live partners
        for (int k = 0; k < live; k++) {
            uint16_t slot = (uint16_t)((k == 0) ? 2 : 4);
            w[nw++] = { (uint16_t)(slot + OBJ_CODE_SEG), 1 };
            w[nw++] = { (uint16_t)(slot + OBJ_TYPE_ID), (uint16_t)(rng.next() & 0x013F) };
            w[nw++] = { (uint16_t)(slot + OBJ_VEL_Y), rng.w() };
            w[nw++] = { (uint16_t)(slot + OBJ_VEL_X), rng.w() };
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
// Unit 31 (class D): sub_10982 read_chunk <-> v2_read_chunk (DATA.DAT seek/
// read + LZSS decompress). Inputs: ax=chunk_id, es:di=dest (di=0). The oracle
// writes: dest zone (ES via v2_fntest_es_override), the LZSS ring + compressed
// staging in the FS segment ([ds:0x2E69] — pointed at FT_RING_SEG inside the
// case image), ds:0x2BB4..0x2BBD (table entry + size), cs-global word_10980.
// v2 side: v2_fntest_call_read_chunk fills a dest buffer + ring copy + the
// 10-byte header mirror. Corpus: every real DATA.DAT chunk + synthetic LZSS
// fixtures via v2_fntest_set_data_file{,_v2}.
const uint16_t FT_RING_SEG = 0x4000;    // 64KB zone at linear 0x40000 (reused test zone)
const uint16_t FT_DEST_SEG = 0x5800;    // 64KB zone at linear 0x58000

bool ft_synth_case_chunk(uint16_t chunk_id, const char* group,
                         FtSynthStats& st, long& diff_budget)
{
    st.cases++;
    uint8_t* mbase = (uint8_t*)v2_fntest_m2c_base();
    uint8_t* ring_zone = mbase + (uint32_t)FT_RING_SEG * 16;
    uint8_t* dest_zone = mbase + (uint32_t)FT_DEST_SEG * 16;
    static uint8_t saved_ring[0x10000], saved_dest[0x10000];
    static uint8_t v2_dest[0x10000], v2_ring[0x1000], v2_hdr[10];
    memcpy(saved_ring, ring_zone, 0x10000);
    memcpy(saved_dest, dest_zone, 0x10000);
    memset(ring_zone, 0xCC, 0x10000);       // recognizable baseline both sides of diff
    memset(dest_zone, 0xCC, 0x10000);

    memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
    ft_wr16(g_synth_in, DS_SEG_FS, FT_RING_SEG);        // FS segment = ring zone
    for (int i = 0; i < 10; i++) g_synth_in[DS_CHUNK_HDR + i] = 0;  // header window baseline

    uint16_t saved_10980 = v2_fntest_get_word_10980();

    memcpy(g_synth_orig, g_synth_in, sizeof(g_synth_orig));
    uint16_t regs[8] = { chunk_id, 0, 0, 0, 0, 0, 0, 0 };
    v2_fntest_es_override = FT_DEST_SEG;
    long esc0 = ft_ub_marks();
    v2_fntest_orig_isolated(v2_fntest_orig_fnptr(FT_SUB_10982), g_synth_orig, regs);
    v2_fntest_es_override = 0;
    uint16_t orig_10980 = v2_fntest_get_word_10980();
    v2_fntest_put_word_10980(saved_10980);
    if (ft_ub_marks() != esc0) {
        st.cases--;
        fprintf(stderr, "FNSELFTEST-UB[sub_10982 %s]: chunk=%04X escaped (error path)\n",
                group, chunk_id);
        memcpy(ring_zone, saved_ring, 0x10000);
        memcpy(dest_zone, saved_dest, 0x10000);
        return true;
    }
    uint16_t orig_di_out = regs[5];

    // Capture oracle zones, restore m2c::m
    static uint8_t orig_ring[0x10000], orig_dest[0x10000];
    memcpy(orig_ring, ring_zone, 0x10000);
    memcpy(orig_dest, dest_zone, 0x10000);
    memcpy(ring_zone, saved_ring, 0x10000);
    memcpy(dest_zone, saved_dest, 0x10000);

    // --- v2 ---
    memset(v2_dest, 0xCC, sizeof(v2_dest));
    uint32_t v2_size = v2_fntest_call_read_chunk(chunk_id, v2_dest, v2_ring, v2_hdr);

    long diffs = 0;
    if ((uint32_t)orig_di_out != (v2_size & 0xFFFF)) {
        if (diff_budget > 0) { diff_budget--;
            fprintf(stderr, "FNSELFTEST-DIFF[sub_10982 %s]: size orig(di)=%04X v2=%04X | chunk=%04X\n",
                    group, orig_di_out, (uint16_t)v2_size, chunk_id); }
        diffs++;
    }
    // dest zone: compare the written prefix (orig baseline was 0xCC everywhere)
    for (uint32_t a = 0; a < 0x10000; a++) {
        uint8_t ov = orig_dest[a];
        uint8_t vv = (a < v2_size) ? v2_dest[a] : 0xCC;
        if (ov == vv) continue;
        if (diff_budget > 0) { diff_budget--;
            fprintf(stderr, "FNSELFTEST-DIFF[sub_10982 %s]: dest+%04X orig=%02X v2=%02X | chunk=%04X\n",
                    group, a, ov, vv, chunk_id); }
        diffs++;
        if (diffs > 40) break;
    }
    // ring window 0..0xFFF (the FS staging at 0x1000+ holds the raw compressed
    // bytes — identical by construction on both sides since both read the same
    // file; compare it too, bounded by the compressed size from the header).
    // chunk 0xFFFA is the documented no-op: neither side touches the ring —
    // the v2 wrapper's baseline zeroing is prep, not a write; skip the window.
    uint32_t comp_sz = *(uint32_t*)(v2_hdr + 4) - *(uint32_t*)(v2_hdr + 0);
    uint32_t stage_end = 0x1000 + ((comp_sz < 0xEFF0) ? (uint16_t)comp_sz : 0xEFF0);
    if (chunk_id == 0xFFFA) stage_end = 0;
    for (uint32_t a = 0; a < stage_end; a++) {
        uint8_t ov = orig_ring[a];
        uint8_t vv;
        if (a < 0x1000) vv = v2_ring[a];
        else vv = v2_vm_shadow_fs[a];
        if (ov == vv) continue;
        if (diff_budget > 0) { diff_budget--;
            fprintf(stderr, "FNSELFTEST-DIFF[sub_10982 %s]: fs+%04X orig=%02X v2=%02X | chunk=%04X\n",
                    group, a, ov, vv, chunk_id); }
        diffs++;
        if (diffs > 80) break;
    }
    // DS: plant the v2 header mirror, then full-image diff
    memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
    memcpy(g_scratch + DS_CHUNK_HDR, v2_hdr, 10);
    for (uint32_t a = 0; a < 0x10000; a++) {
        if (g_scratch[a] == g_synth_orig[a]) continue;
        if (v2_fntest_ds_skip(a)) continue;
        if (diff_budget > 0) { diff_budget--;
            fprintf(stderr, "FNSELFTEST-DIFF[sub_10982 %s]: ds addr=%04X orig=%02X v2=%02X | chunk=%04X\n",
                    group, a, g_synth_orig[a], g_scratch[a], chunk_id); }
        diffs++;
    }
    // cs-global word_10980 == decompressed size (v2 mirror lives at hdr[8..9])
    if (orig_10980 != *(uint16_t*)(v2_hdr + 8)) {
        if (diff_budget > 0) { diff_budget--;
            fprintf(stderr, "FNSELFTEST-DIFF[sub_10982 %s]: word_10980 orig=%04X v2=%04X | chunk=%04X\n",
                    group, orig_10980, *(uint16_t*)(v2_hdr + 8), chunk_id); }
        diffs++;
    }
    if (diffs) { st.fail++; return false; }
    st.pass++; return true;
}

int ft_selftest_chunk(uint32_t seed) {
    (void)seed;
    FtSynthStats corpus, synth;
    long diff_budget = 60;
    v2_set_m2c_base(v2_fntest_m2c_base());

    // --- Corpus: every real DATA.DAT chunk -------------------------------
    if (!v2_fntest_set_data_file("DATA.DAT") || !v2_fntest_set_data_file_v2("DATA.DAT")) {
        fprintf(stderr, "FNSELFTEST-SUMMARY[sub_10982]: DATA.DAT missing — total cases=0 fail=1\n");
        return 1;
    }
    uint32_t first_off = 0;
    { FILE* f = fopen("DATA.DAT", "rb");
      if (f) { if (fread(&first_off, 4, 1, f) != 1) first_off = 0; fclose(f); } }
    uint32_t nchunks = first_off / 4;
    fprintf(stderr, "FNSELFTEST-PROG[sub_10982]: DATA.DAT chunks=%u\n", nchunks);
    for (uint32_t id = 0; id < nchunks; id++) {
        if ((id % 64) == 0)
            fprintf(stderr, "FNSELFTEST-PROG[sub_10982]: corpus %u/%u\n", id, nchunks);
        if (!ft_shard_mine(id)) continue;
        ft_synth_case_chunk((uint16_t)id, "corpus", corpus, diff_budget);
    }
    // 0xFFFA special no-op + one out-of-table id (error path → oracle UB-skip)
    if (g_shard_i == 0) {
        ft_synth_case_chunk(0xFFFA, "corpus", corpus, diff_budget);
        ft_synth_case_chunk((uint16_t)(nchunks + 8), "corpus", corpus, diff_budget);
    }

    // --- Synthetic LZSS fixtures ------------------------------------------
    // One fixture file, several crafted chunks: pure literals; backrefs into
    // the zeroed ring; max-length refs; offset wrap at 0xFFF; termination
    // mid-backref; size==1 (the DEC dx underflow edge: one byte IS written).
    if (g_shard_i == 0) {
        const char* fx = "/tmp/fnst_lzss_fixture.dat";
        {
            FILE* f = fopen(fx, "wb");
            if (f) {
                struct Blob { uint8_t bytes[64]; int n; uint16_t dsize; };
                Blob b[6];
                // 0: 8 literals "ABCDEFGH"
                { Blob& x = b[0]; x.n = 0; x.dsize = 8; x.bytes[x.n++] = 0xFF;
                  for (int i = 0; i < 8; i++) x.bytes[x.n++] = (uint8_t)('A' + i); }
                // 1: literal 'Z', then a backref len 3 into the zeroed ring @0x800
                { Blob& x = b[1]; x.n = 0; x.dsize = 4; x.bytes[x.n++] = 0x01;
                  x.bytes[x.n++] = 'Z';
                  x.bytes[x.n++] = 0x00; x.bytes[x.n++] = 0x08; } // ref word 0x0800: off=0x800 len=3
                // 2: 2 literals then max-length (18) self-overlapping ref off=0
                { Blob& x = b[2]; x.n = 0; x.dsize = 20; x.bytes[x.n++] = 0x03;
                  x.bytes[x.n++] = 0x55; x.bytes[x.n++] = 0xAA;
                  x.bytes[x.n++] = 0x00; x.bytes[x.n++] = 0xF0; } // off=0 len=15+3=18
                // 3: ref crossing the ring wrap: off=0xFFE len=5
                { Blob& x = b[3]; x.n = 0; x.dsize = 5; x.bytes[x.n++] = 0x00;
                  x.bytes[x.n++] = 0xFE; x.bytes[x.n++] = 0x5F; } // word 0x5FFE: off=0xFFE len=5+3=8→size stops at 5
                // 4: size 1 via a backref (termination mid-copy)
                { Blob& x = b[4]; x.n = 0; x.dsize = 1; x.bytes[x.n++] = 0x00;
                  x.bytes[x.n++] = 0x00; x.bytes[x.n++] = 0x30; } // off=0 len=6, stops after 1
                // 5: size 1 via literal (DEC dx from 1)
                { Blob& x = b[5]; x.n = 0; x.dsize = 1; x.bytes[x.n++] = 0x01;
                  x.bytes[x.n++] = 0x7E; }
                uint32_t off = 6 * 4;
                uint32_t offs[7];
                for (int i = 0; i < 6; i++) { offs[i] = off; off += 2 + (uint32_t)b[i].n; }
                offs[6] = off;
                // NOTE the oracle reads chunk i's table entry as TWO dwords
                // (offset, next) at id*4 — entry i+1 must be chunk i's end.
                for (int i = 0; i < 6; i++) fwrite(&offs[i], 4, 1, f);
                // (no 7th entry needed for ids 0..4; id 5 reads offs[5]+offs[6]
                // where offs[6] would be chunk-5 data — craft: append a dword
                // equal to the file end as a trailing pseudo-entry INSIDE the
                // first chunk's padding is impossible → keep ids 0..4 only.)
                for (int i = 0; i < 6; i++) {
                    fwrite(&b[i].dsize, 2, 1, f);
                    fwrite(b[i].bytes, 1, (size_t)b[i].n, f);
                }
                fclose(f);
            }
        }
        if (v2_fntest_set_data_file(fx) && v2_fntest_set_data_file_v2(fx)) {
            for (uint16_t id = 0; id < 5; id++)   // id 5's "next" entry is data, skip
                ft_synth_case_chunk(id, "synth", synth, diff_budget);
        }
        // restore the real file for any later units in the same process
        v2_fntest_set_data_file("DATA.DAT");
        v2_fntest_set_data_file_v2("DATA.DAT");
    }

    fprintf(stderr,
        "FNSELFTEST-SUMMARY[sub_10982]: corpus %ld/%ld, synth %ld/%ld — "
        "total cases=%ld fail=%ld%s\n",
        corpus.pass, corpus.cases, synth.pass, synth.cases,
        corpus.cases + synth.cases, corpus.fail + synth.fail,
        (corpus.fail + synth.fail) ? "  <<< DIVERGENCE" : "");
    return (corpus.fail + synth.fail) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Unit 32 (class D): sub_10cd8 read_and_display_raw_chunk <-> v2_read_raw_chunk.
// Orig: 0xFFFA no-op; seek id*4 (32-bit `ax*4` here, unlike sub_10982's SHL
// dx,2); ds:0x2BB4(8) table entry; seek; ds:0x2BBC(2)=plane_size;
// word_10980=plane_size; fread plane_size*4 into the CHUNK segment
// ([ds:0x2E77] → FT_RING_SEG); then 4 plane loops into raddr(0xA000,
// display_offset+i) — in the port's LINEAR model the planes land on top of
// each other, plane 3 survives; plus a drawPixel pass into myDrawInfo
// (legacy layer, outside the v2 contract — allocated so the oracle doesn't
// NULL-deref, contents not compared). Errors → sub_10dba fatal (UB-skip).
const uint32_t FT_A000_LIN  = 0xA0000;
// raddr_ takes a dw offset — every plane read/write wraps at 64K (8086
// segment semantics). The staging fread, by contrast, is a LINEAR libc call:
// plane_size*4 can spill up to 256K past the chunk zone — save/compare that
// whole span.
const uint32_t FT_A000_SPAN  = 0x10000;
const uint32_t FT_CHUNK_SPAN = 0x40000;

bool ft_synth_case_rawchunk(uint16_t chunk_id, uint16_t disp_off, const char* group,
                            FtSynthStats& st, long& diff_budget)
{
    st.cases++;
    uint8_t* mbase = (uint8_t*)v2_fntest_m2c_base();
    uint8_t* chunk_zone = mbase + (uint32_t)FT_RING_SEG * 16;
    uint8_t* a000_zone  = mbase + FT_A000_LIN;
    static uint8_t saved_chunk[FT_CHUNK_SPAN], saved_a000[FT_A000_SPAN];
    static uint8_t orig_chunk[FT_CHUNK_SPAN], orig_a000[FT_A000_SPAN];
    static uint8_t v2_dest[0x40000], v2_hdr[10];
    memcpy(saved_chunk, chunk_zone, FT_CHUNK_SPAN);
    memcpy(saved_a000, a000_zone, FT_A000_SPAN);
    memset(chunk_zone, 0xCC, FT_CHUNK_SPAN);
    memset(a000_zone, 0xCC, FT_A000_SPAN);

    memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
    ft_wr16(g_synth_in, DS_SEG_CHUNK, FT_RING_SEG);        // chunk segment = test zone
    for (int i = 0; i < 10; i++) g_synth_in[DS_CHUNK_HDR + i] = 0;

    uint16_t saved_10980 = v2_fntest_get_word_10980();
    memcpy(g_synth_orig, g_synth_in, sizeof(g_synth_orig));
    uint16_t regs[8] = { chunk_id, 0, 0, 0, 0, disp_off, 0, 0 };
    long esc0 = ft_ub_marks();
    v2_fntest_orig_isolated(v2_fntest_orig_fnptr(FT_SUB_10CD8), g_synth_orig, regs);
    uint16_t orig_10980 = v2_fntest_get_word_10980();
    v2_fntest_put_word_10980(saved_10980);
    if (ft_ub_marks() != esc0) {
        st.cases--;
        fprintf(stderr, "FNSELFTEST-UB[sub_10cd8 %s]: chunk=%04X escaped (error path)\n",
                group, chunk_id);
        memcpy(chunk_zone, saved_chunk, FT_CHUNK_SPAN);
        memcpy(a000_zone, saved_a000, FT_A000_SPAN);
        return true;
    }
    memcpy(orig_chunk, chunk_zone, FT_CHUNK_SPAN);
    memcpy(orig_a000, a000_zone, FT_A000_SPAN);
    memcpy(chunk_zone, saved_chunk, FT_CHUNK_SPAN);
    memcpy(a000_zone, saved_a000, FT_A000_SPAN);

    // --- v2 ---
    memset(v2_dest, 0xCC, sizeof(v2_dest));
    uint32_t v2_ps = v2_fntest_call_raw_chunk(chunk_id, v2_dest, v2_hdr);

    long diffs = 0;
    uint32_t data_len = (uint32_t)v2_ps * 4;
    // chunk zone: the staging fread is LINEAR — the written prefix can span
    // up to 256K; the rest stays at the 0xCC baseline.
    for (uint32_t a = 0; a < FT_CHUNK_SPAN; a++) {
        uint8_t ov = orig_chunk[a];
        uint8_t vv = (a < data_len) ? v2_dest[a] : 0xCC;
        if (ov == vv) continue;
        if (diff_budget > 0) { diff_budget--;
            fprintf(stderr, "FNSELFTEST-DIFF[sub_10cd8 %s]: chunk+%05X orig=%02X v2=%02X | id=%04X ps=%04X\n",
                    group, a, ov, vv, chunk_id, (uint16_t)v2_ps); }
        diffs++;
        if (diffs > 40) break;
    }
    // A000 window: each plane pass writes A000[(uint16_t)(disp_off+i)] =
    // chunk[(uint16_t)(ps*k+i)] — 16-bit wraps on BOTH sides (raddr takes a dw
    // offset). Plane 3 runs last, and within 64K each woff maps to exactly one
    // i, so the survivor at woff is plane 3's byte at i=(uint16_t)(woff-off).
    for (uint32_t a = 0; a < FT_A000_SPAN; a++) {
        uint8_t ov = orig_a000[a];
        uint8_t vv = 0xCC;
        if (v2_ps) {
            uint16_t i = (uint16_t)((uint16_t)a - disp_off);
            if (i < v2_ps) {
                uint16_t widx = (uint16_t)((uint32_t)v2_ps * 3 + i);
                vv = (widx < data_len) ? v2_dest[widx] : 0xCC;  // wrapped read past the staging = zone baseline
            }
        }
        if (ov == vv) continue;
        if (diff_budget > 0) { diff_budget--;
            fprintf(stderr, "FNSELFTEST-DIFF[sub_10cd8 %s]: a000+%05X orig=%02X v2=%02X | id=%04X off=%04X ps=%04X\n",
                    group, a, ov, vv, chunk_id, disp_off, (uint16_t)v2_ps); }
        diffs++;
        if (diffs > 80) break;
    }
    // DS window + full image
    memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
    memcpy(g_scratch + DS_CHUNK_HDR, v2_hdr, 8);
    *(uint16_t*)(g_scratch + 0x2BBC) = (uint16_t)v2_ps;
    for (uint32_t a = 0; a < 0x10000; a++) {
        if (g_scratch[a] == g_synth_orig[a]) continue;
        if (v2_fntest_ds_skip(a)) continue;
        if (diff_budget > 0) { diff_budget--;
            fprintf(stderr, "FNSELFTEST-DIFF[sub_10cd8 %s]: ds addr=%04X orig=%02X v2=%02X | id=%04X\n",
                    group, a, g_synth_orig[a], g_scratch[a], chunk_id); }
        diffs++;
    }
    if (chunk_id != 0xFFFA && orig_10980 != (uint16_t)v2_ps) {
        if (diff_budget > 0) { diff_budget--;
            fprintf(stderr, "FNSELFTEST-DIFF[sub_10cd8 %s]: word_10980 orig=%04X v2=%04X | id=%04X\n",
                    group, orig_10980, (uint16_t)v2_ps, chunk_id); }
        diffs++;
    }
    if (diffs) { st.fail++; return false; }
    st.pass++; return true;
}

int ft_selftest_rawchunk(uint32_t seed) {
    (void)seed;
    FtSynthStats corpus, synth;
    long diff_budget = 60;
    v2_set_m2c_base(v2_fntest_m2c_base());
    v2_fntest_alloc_drawinfo();   // oracle's drawPixel pass needs the buffer

    if (!v2_fntest_set_data_file("DATA.DAT") || !v2_fntest_set_data_file_v2("DATA.DAT")) {
        fprintf(stderr, "FNSELFTEST-SUMMARY[sub_10cd8]: DATA.DAT missing — total cases=0 fail=1\n");
        return 1;
    }
    uint32_t first_off = 0;
    { FILE* f = fopen("DATA.DAT", "rb");
      if (f) { if (fread(&first_off, 4, 1, f) != 1) first_off = 0; fclose(f); } }
    uint32_t nchunks = first_off / 4;
    fprintf(stderr, "FNSELFTEST-PROG[sub_10cd8]: DATA.DAT chunks=%u\n", nchunks);
    static const uint16_t OFFS[2] = { 0x0000, 0x20C8 };   // real caller offsets
    for (uint32_t id = 0; id < nchunks; id++) {
        if ((id % 64) == 0)
            fprintf(stderr, "FNSELFTEST-PROG[sub_10cd8]: corpus %u/%u\n", id, nchunks);
        if (!ft_shard_mine(id)) continue;
        ft_synth_case_rawchunk((uint16_t)id, OFFS[id & 1], "corpus", corpus, diff_budget);
    }
    if (g_shard_i == 0) {
        ft_synth_case_rawchunk(0xFFFA, 0, "corpus", corpus, diff_budget);
        ft_synth_case_rawchunk((uint16_t)(nchunks + 8), 0, "corpus", corpus, diff_budget);
        // real intro/HUD offsets on a couple of known-RAW ids (the unit-31
        // UB list: those chunks ARE the raw class)
        ft_synth_case_rawchunk(0x017C, 0x66A8, "corpus", corpus, diff_budget);
        ft_synth_case_rawchunk(0x0212, 0xAC88, "corpus", corpus, diff_budget);
    }

    // Synthetic raw fixtures: tiny planes + an A000 spill past 64K.
    if (g_shard_i == 0) {
        const char* fx = "/tmp/fnst_raw_fixture.dat";
        {
            FILE* f = fopen(fx, "wb");
            if (f) {
                // chunk 0: plane_size 1 (4 bytes: 11 22 33 44)
                // chunk 1: plane_size 3 (12 bytes: 3 per plane)
                // chunk 2: plane_size 0x3000 spill test (planes = ramp bytes)
                uint32_t offs[4];
                uint32_t off = 4 * 4;
                offs[0] = off; off += 2 + 4;
                offs[1] = off; off += 2 + 12;
                offs[2] = off; off += 2 + 0x3000u * 4;
                offs[3] = off;
                for (int i = 0; i < 4; i++) fwrite(&offs[i], 4, 1, f);
                uint16_t ps0 = 1;  fwrite(&ps0, 2, 1, f);
                const uint8_t p0[4] = { 0x11, 0x22, 0x33, 0x44 };
                fwrite(p0, 1, 4, f);
                uint16_t ps1 = 3;  fwrite(&ps1, 2, 1, f);
                const uint8_t p1[12] = { 1,2,3, 4,5,6, 7,8,9, 10,11,12 };
                fwrite(p1, 1, 12, f);
                uint16_t ps2 = 0x3000; fwrite(&ps2, 2, 1, f);
                for (uint32_t i = 0; i < 0x3000u * 4; i++) {
                    uint8_t b = (uint8_t)(i * 7 + 13);
                    fwrite(&b, 1, 1, f);
                }
                fclose(f);
            }
        }
        if (v2_fntest_set_data_file(fx) && v2_fntest_set_data_file_v2(fx)) {
            ft_synth_case_rawchunk(0, 0x0000, "synth", synth, diff_budget);
            ft_synth_case_rawchunk(0, 0x1234, "synth", synth, diff_budget);
            ft_synth_case_rawchunk(1, 0x20C8, "synth", synth, diff_budget);
            ft_synth_case_rawchunk(2, 0xE000, "synth", synth, diff_budget);  // 0xE000+0x3000 spills past 64K
        }
        v2_fntest_set_data_file("DATA.DAT");
        v2_fntest_set_data_file_v2("DATA.DAT");
    }

    fprintf(stderr,
        "FNSELFTEST-SUMMARY[sub_10cd8]: corpus %ld/%ld, synth %ld/%ld — "
        "total cases=%ld fail=%ld%s\n",
        corpus.pass, corpus.cases, synth.pass, synth.cases,
        corpus.cases + synth.cases, corpus.fail + synth.fail,
        (corpus.fail + synth.fail) ? "  <<< DIVERGENCE" : "");
    return (corpus.fail + synth.fail) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// sub_1303a / sub_13031 <-> v2_vm_anim_interp_1303a (+ v2_vm_anim_tail_135cf) — the anim
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
    ft_wr16(g_synth_in, (uint16_t)(obj + OBJ_CODE_SEG), FT_VM_TESTSEG);
    ft_wr16(g_synth_in, (uint16_t)(obj + OBJ_ANIM_PC), FT_ANIM_PC);
    ft_wr16(g_synth_in, (uint16_t)(obj + OBJ_ANIM_TIMER), t1);
    ft_wr16(g_synth_in, (uint16_t)(obj + OBJ_ANIM_CONT), t2);
    ft_wr16(g_synth_in, (uint16_t)(obj + OBJ_SUB_SLOT), sa);
    ft_wr16(g_synth_in, (uint16_t)(obj + OBJ_SUB_END), se);
    // slot background noise (sprite offsets, flags, dirty)
    FtRng bg(bg_seed);
    for (uint32_t a = 0; a < 0x60; a += 2) {
        ft_wr16(g_synth_in, (uint16_t)(a + 0x44D), bg.w());
        ft_wr16(g_synth_in, (uint16_t)(a + 0x54D), bg.w());
        ft_wr16(g_synth_in, (uint16_t)(a + 0x84D), bg.w());
        ft_wr16(g_synth_in, (uint16_t)(a + OBJ_DIRTY_MODE), bg.w());
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
        FtWr wn[1] = { { (uint16_t)(OBJ + OBJ_ANIM_PC), 0xFFFF } };
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
    ft_wr16(g_synth_in, (uint16_t)(si + OBJ_CODE_SEG), FT_VM_TESTSEG);
    ft_wr16(g_synth_in, (uint16_t)(si + OBJ_PC), FT_VM_PC);
    ft_wr16(g_synth_in, (uint16_t)(si + OBJ_FLAGS), o.flags);
    ft_wr16(g_synth_in, (uint16_t)(si + OBJ_ANIM_IDX), o.anim);
    ft_wr16(g_synth_in, (uint16_t)(si + OBJ_TIMER), o.timer);
    ft_wr16(g_synth_in, (uint16_t)(si + OBJ_WORLD_X), o.x);
    ft_wr16(g_synth_in, (uint16_t)(si + OBJ_WORLD_Y), o.y);
    ft_wr16(g_synth_in, (uint16_t)(si + OBJ_VEL_Y), o.yvel);
    ft_wr16(g_synth_in, (uint16_t)(si + 0x14E5), o.ystart);
    ft_wr16(g_synth_in, (uint16_t)(si + OBJ_BBOX_Y1), o.yend);
    // Light background noise over misc object fields (deterministic)
    FtRng bg(bg_seed);
    static const uint16_t OF[] = { 0x1305, 0x13CD, OBJ_HALF_H, OBJ_HALF_W, OBJ_BBOX_X0, OBJ_BBOX_X1,
                                   0x1855, 0x18AD, OBJ_PARTNER, OBJ_FRAC_Y, OBJ_SUB_COUNT };
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
    ft_wr16(g_synth_in, (uint16_t)(si + OBJ_CODE_SEG), FT_VM_TESTSEG);
    ft_wr16(g_synth_in, (uint16_t)(si + OBJ_PC), FT_VM_PC);
    ft_wr16(g_synth_in, (uint16_t)(si + OBJ_FLAGS), o.flags);
    ft_wr16(g_synth_in, (uint16_t)(si + OBJ_ANIM_IDX), o.anim);
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
    uint16_t fin = (uint16_t)(g_synth_orig[(uint16_t)(si + OBJ_PC)]
                 | (g_synth_orig[(uint16_t)(si + OBJ_PC + 1)] << 8));
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
            occ[k] = { (uint16_t)(k * 2 + OBJ_CODE_SEG), 0x4000 };   // slots 0..0x26 alive
        occ[0x14] = { (uint16_t)(6 + OBJ_CODE_SEG), FT_VM_TESTSEG }; // keep test obj's code seg
        uint8_t a4[16]; memcpy(a4, plan[0x14].base, 16);
        ft_synth_case_vmop(0x14, a4, 16, BASE, 0xD0000001u, "directed",
                           sweep, diff_budget, op_fail, occ, 0x14 + 1);
    }
    // (a2) task #15 DI-inheritance: op 0x54 (ch3-style field address load —
    // leaves di = [idx-0x6CBA]+[obj+0x1995]) immediately followed by op 0x41
    // with a ch6 Y-escape. The orig stores that STALE di (through the
    // sub_12613 clamps) into [bx+0x1DAB]; v2 must reproduce it via di_track.
    // Byte stream after the planted 0x54: idx, then the full op41 tail:
    //   41 | mode1=0x00 (1250b ch0: 2 lit bytes; 12543 ch0: 2 lit bytes)
    //      | lit lit | lit lit | mode125a3=0x30 (X=ch0: 2 lit; Y=ch6: 1 idx)
    //      | lit lit | y6idx
    {
        uint8_t chain[16] = {
            /*op54 idx*/ 0x10,
            /*op*/ 0x41,
            /*mode1*/ 0x00, /*1250b lit*/ 0x02, 0x00,
            /*12543 lit*/ 0x03, 0x00,
            /*125a3 mode*/ 0x30, /*X lit*/ 0x50, 0x00,
            /*Y ch6 idx*/ 0x11,
            0, 0, 0, 0, 0 };
        ft_synth_case_vmop(0x54, chain, 16, BASE, 0xD0000015u, "directed",
                           sweep, diff_budget, op_fail);
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
            FtWr tr[2] = { { 0x32F, 1 }, { DS_SEG_ANIM, FT_VM_TESTSEG } };
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
            FtWr tr[1] = { { DS_SEG_ANIM, FT_VM_TESTSEG } };
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
                { (uint16_t)(2 + OBJ_CODE_SEG), FT_VM_TESTSEG },   // slot 2 alive
                { (uint16_t)(2 + OBJ_TYPE_ID), 0x0100 },          // type word (hi byte set!)
                { (uint16_t)(2 + OBJ_BBOX_X0), 0x0100 },          // target X range
                { (uint16_t)(2 + OBJ_BBOX_X1), 0x0200 },
                { (uint16_t)(2 + 0x14E5), 0x0140 },          // target Y range
                { (uint16_t)(2 + OBJ_BBOX_Y1), 0x0160 },
                { (uint16_t)(6 + OBJ_BBOX_X0), 0x0110 },          // self X range (overrides noise)
                { (uint16_t)(6 + OBJ_BBOX_X1), 0x0130 },
                { (uint16_t)(0x94CC), 0x0000 },              // filter table [0-0x6B34]: match 0
                { (uint16_t)(6 + 0x14E5), 0x0130 },          // self Y start (bounds sanity)
                { (uint16_t)(6 + OBJ_BBOX_Y1), 0x0150 },
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

// ============================================================================
// Units 33-34: DAC dispatch pair — sub_10fe6 (full upload, source hardcoded
// ds:0x8202) / sub_10ffc (per-channel animation bursts from ds:[word_303E0]).
// NEW compare channel: oracle drawPalette (reset to zero baseline, then
// v2_fetch_orig_dac after the call) vs v2_dac_shadow<<2 (reset the same way).
// Plus the standard full-DS diff (0x7EFE clear, [bx+0x258C] timer reloads).
// ============================================================================
struct FtPalCh { uint8_t timer, reload, start, end; };   // [bx+0x258C/0x2584/0x2594/0x259C]

bool ft_synth_case_pal(FtId id, uint8_t chan_mask, const FtPalCh* ch /*8 or null*/,
                       uint16_t w7f00, uint32_t pal_seed, const char* group,
                       FtSynthStats& st, long& diff_budget)
{
    st.cases++;
    memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
    FtRng pr(pal_seed);
    for (int i = 0; i < 768; i++) g_synth_in[DS_PAL_OUT + i] = (uint8_t)pr.w();
    for (int i = 0; i < 768; i++) g_synth_in[DS_PAL_SRC + i] = (uint8_t)pr.w();
    g_synth_in[0x2583] = chan_mask;                       // byte_2AA63 enable bits
    for (int b = 0; b < 8; b++) {
        g_synth_in[b + 0x258C] = ch ? ch[b].timer  : 0;
        g_synth_in[b + 0x2584] = ch ? ch[b].reload : 0;
        g_synth_in[b + 0x2594] = ch ? ch[b].start  : 0;
        g_synth_in[b + 0x259C] = ch ? ch[b].end    : 0;
    }
    ft_wr16(g_synth_in, DS_PAL_REQ, 0xBBBB);                  // both sides clear to 0
    ft_wr16(g_synth_in, DS_PAL_SRC_PTR, w7f00);                   // word_303E0 burst source

    memcpy(g_synth_orig, g_synth_in, sizeof(g_synth_orig));
    // Direct-call isolator (no CALL_): the pair's port bodies end in a C
    // return (RETN commented at the SDL inline) — see seg000 note.
    v2_fntest_reset_orig_dac();
    v2_fntest_orig_call_pal((id == FT_SUB_10FE6) ? 0 : 1, g_synth_orig);
    uint8_t dac_orig[768];
    v2_fetch_orig_dac(dac_orig);

    memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
    uint8_t dac_v2[768]; int v2_hung = 0;
    v2_fntest_reset_v2_dac();
    v2_fntest_arm_signals();
    if (sigsetjmp(*v2_fntest_jb(), 1) == 0) {
        v2_fntest_alarm_ms(4000);
        v2_fntest_call_pal(g_scratch, (id == FT_SUB_10FE6) ? 0 : 1, dac_v2);
        v2_fntest_alarm_ms(0);
    } else { v2_fntest_alarm_ms(0); v2_hung = 1; }
    if (v2_hung) {
        fprintf(stderr, "FNSELFTEST-DIFF[%s %s]: v2 HUNG (watchdog) | mask=%02X seed=%08X\n",
                g_name[id], group, chan_mask, pal_seed);
        st.fail++; return false;
    }

    long diffs = 0;
    for (int i = 0; i < 768; i++) {
        uint8_t ov = dac_orig[i];
        uint8_t vv = (uint8_t)(dac_v2[i] << 2);
        if (ov == vv) continue;
        if (diff_budget > 0) {
            diff_budget--;
            fprintf(stderr, "FNSELFTEST-DIFF[%s %s]: DAC slot=%02X comp=%d orig=%02X v2=%02X "
                    "| mask=%02X 7F00=%04X seed=%08X\n",
                    g_name[id], group, i / 3, i % 3, ov, vv, chan_mask, w7f00, pal_seed);
        }
        diffs++;
    }
    for (uint32_t a = 0; a < 0x10000; a++) {
        if (g_scratch[a] == g_synth_orig[a]) continue;
        if (v2_fntest_ds_skip(a)) continue;
        if (diff_budget > 0) {
            diff_budget--;
            fprintf(stderr, "FNSELFTEST-DIFF[%s %s]: addr=%04X orig=%02X v2=%02X (in=%02X) "
                    "| mask=%02X seed=%08X\n",
                    g_name[id], group, a, g_synth_orig[a], g_scratch[a], g_synth_in[a],
                    chan_mask, pal_seed);
        }
        diffs++;
    }
    if (diffs) { st.fail++; return false; }
    st.pass++; return true;
}

int ft_selftest_pal(FtId id, uint32_t seed) {
    FtSynthStats grid, fuzz;
    long diff_budget = 24;
    v2_fntest_alloc_drawinfo();          // oracle setPalette needs myDrawInfo
    const bool is_ffc = (id == FT_SUB_10FFC);

    // --- grid ---
    if (!is_ffc) {
        // sub_10fe6 has no branches: a few palette seeds + wild 0x7F00 values.
        for (uint32_t s = 0; s < 6; s++)
            ft_synth_case_pal(id, 0, nullptr, (s & 1) ? DS_PAL_SRC : DS_PAL_OUT,
                              seed + s, "grid", grid, diff_budget);
    } else {
        // Directed channel configs. LUT [bx-0x6C44] holds the enable bit per
        // channel; mask 0xFF enables all.
        struct C { uint8_t mask; FtPalCh ch[8]; uint16_t w7f00; };
        auto mk = [](uint8_t st_, uint8_t en, uint8_t tm = 0, uint8_t rl = 0) {
            return FtPalCh{ tm, rl, st_, en };
        };
        C cases[] = {
            // cur==end → JZ skip (no DAC, timer still reloads)
            { 0xFF, { mk(0x10,0x10), {}, {}, {}, {}, {}, {}, {} }, DS_PAL_SRC },
            // positive small range, source 0x7F02 (the fix-#22 scenario)
            { 0xFF, { mk(0x71,0x74), {}, {}, {}, {}, {}, {}, {} }, DS_PAL_SRC },
            // positive from 0x8202
            { 0xFF, { mk(0x00,0x0F), {}, {}, {}, {}, {}, {}, {} }, DS_PAL_OUT },
            // positive full range 0..0xFF
            { 0xFF, { mk(0x00,0xFF), {}, {}, {}, {}, {}, {}, {} }, DS_PAL_SRC },
            // negative (byte diff bit7=1): count*3 source quirk
            { 0xFF, { mk(0x20,0x00), {}, {}, {}, {}, {}, {}, {} }, DS_PAL_SRC },
            // POSITIVE with byte-diff underflow: end=0x00 start=0xFF → diff8=0x01,
            // SF=0 → positive path, count=2, DAC slots 0xFF→0x00 (index wrap).
            // The old int16 range model sent this to the negative path.
            { 0xFF, { mk(0xFF,0x00), {}, {}, {}, {}, {}, {}, {} }, DS_PAL_SRC },
            // negative BIG: start=0x10 end=0x90 → diff8=0x80 (bit7=1), count=0x81,
            // slots 0x90..0xFF then wrap 0x00..0x10.
            { 0xFF, { mk(0x10,0x90), {}, {}, {}, {}, {}, {}, {} }, DS_PAL_SRC },
            // timer≠0 → whole channel skipped (no reload of others)
            { 0xFF, { mk(0x10,0x20,5), {}, {}, {}, {}, {}, {}, {} }, DS_PAL_SRC },
            // mask off → gate rejects even with data
            { 0x00, { mk(0x10,0x20), {}, {}, {}, {}, {}, {}, {} }, DS_PAL_SRC },
            // all 8 channels active with different ranges incl. overlaps
            { 0xFF, { mk(0,3), mk(4,7), mk(8,15), mk(16,16), mk(0x40,0x20),
                      mk(0x80,0x9F), mk(0xF0,0xFF), mk(2,5) }, DS_PAL_SRC },
            // wild 303E0 (both sides read the same DS garbage)
            { 0xFF, { mk(0x10,0x2F), {}, {}, {}, {}, {}, {}, {} }, 0x4321 },
        };
        int ci = 0;
        for (auto& c : cases)
            ft_synth_case_pal(id, c.mask, c.ch, c.w7f00, seed + 100 + ci++,
                              "grid", grid, diff_budget);
    }

    // --- fuzz ---
    FtRng rng(seed ^ 0xDACDACDAu);
    int n_fuzz = is_ffc ? 20000 : 2000;
    for (int i = 0; i < n_fuzz; i++) {
        FtPalCh ch[8];
        for (int b = 0; b < 8; b++)
            ch[b] = { (uint8_t)(rng.w() & ((rng.w() & 3) ? 0 : 0xFF)),  // mostly timer=0
                      (uint8_t)rng.w(), (uint8_t)rng.w(), (uint8_t)rng.w() };
        uint16_t w7 = (rng.w() & 1) ? DS_PAL_SRC : ((rng.w() & 1) ? DS_PAL_OUT : rng.w());
        ft_synth_case_pal(id, (uint8_t)rng.w(), is_ffc ? ch : nullptr, w7,
                          rng.w() * 65536u + rng.w(), "fuzz", fuzz, diff_budget);
    }

    fprintf(stderr,
        "FNSELFTEST-SUMMARY[%s]: grid %ld/%ld, fuzz %ld/%ld — total cases=%ld fail=%ld%s\n",
        g_name[id], grid.pass, grid.cases, fuzz.pass, fuzz.cases,
        grid.cases + fuzz.cases, grid.fail + fuzz.fail,
        (grid.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (grid.fail + fuzz.fail) ? 1 : 0;
}

// ---- K1 units (35-43): parameterless DS clear/init leaves ------------------
// sub_11080 level-load chain leaves with NO input registers: the whole DS is
// the contract (they read nothing but constants and write fixed ranges), so
// one shared runner covers all of them. Input images: the pristine base,
// all-00, all-FF, 0xA5/0x5A checkerboards, and seeded-random fuzz — both
// sides run identical copies, full-DS byte compare.
//   35 sub_11192: bytes ds:356..365 = 0
//   36 sub_111a1: REP STOSW 0xE00 words ds:44D..204C = 0 (ES=DS)
//   37 sub_111df: bytes ds:342..347 = 0, word ds:348 = 0
//   38 sub_11784: words ds:3C2/3C4 = 0
//   39 sub_137f1: 20 slots: word [si+1355]=0, word [si+1A0D]=FFFF (si!=0x28 loop)
//   40 sub_12fb3: words ds:44D..54B = 0 (si<0x100 JL loop)
//   41 sub_12ca3: bytes ds:32E/28 = 0, words 3D4/3CC/336/34A/34C/334/331 = 0,
//                 words 25AF/3C2 = FFFF (NB word at 0x331 overlaps 0x332)
//   42 sub_12ce4: words 414/416/418 = 0, words [3E4..3FB] = 0 (si<0x18),
//                 words 423/425/427 = 6, words 429/42B/42D = 0
//   43 sub_108b8: sub_12ce4 + word 25C9 = 0x27, word 3C2 = 0
typedef void (*FtClearCall)(uint8_t*);
struct FtClearSpec { FtId id; FtClearCall call; };
const FtClearSpec FT_CLEARS[] = {
    { FT_SUB_11192, v2_fntest_call_sub_11192 },
    { FT_SUB_111A1, v2_fntest_call_sub_111a1 },
    { FT_SUB_111DF, v2_fntest_call_sub_111df },
    { FT_SUB_11784, v2_fntest_call_sub_11784 },
    { FT_SUB_137F1, v2_fntest_call_sub_137f1 },
    { FT_SUB_12FB3, v2_fntest_call_sub_12fb3 },
    { FT_SUB_12CA3, v2_fntest_call_sub_12ca3 },
    { FT_SUB_12CE4, v2_fntest_call_sub_12ce4 },
    { FT_SUB_108B8, v2_fntest_call_sub_108b8 },
    { FT_SUB_12816, v2_fntest_call_sub_12816 },
};

bool ft_synth_case_clear(const FtClearSpec& cs, const char* group,
                         FtSynthStats& st, long& diff_budget)
{
    st.cases++;
    // g_synth_in is prepared by the caller (image pattern); tail refreshed
    // here so out-of-window reads stay identical after pattern fills.
    ft_fill_tail(g_synth_in);

    memcpy(g_synth_orig, g_synth_in, sizeof(g_synth_orig));
    uint16_t regs[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    v2_fntest_orig_isolated(v2_fntest_orig_fnptr(cs.id), g_synth_orig, regs);

    memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
    cs.call(g_scratch);

    long diffs = 0;
    for (uint32_t a = 0; a < 0x10000; a++) {
        if (g_scratch[a] == g_synth_orig[a]) continue;
        if (v2_fntest_ds_skip(a)) continue;
        if (diff_budget > 0) {
            diff_budget--;
            fprintf(stderr, "FNSELFTEST-DIFF[%s %s]: addr=%04X orig=%02X v2=%02X (in=%02X)\n",
                    g_name[cs.id], group, a, g_synth_orig[a], g_scratch[a], g_synth_in[a]);
        }
        diffs++;
    }
    if (diffs) { st.fail++; return false; }
    st.pass++; return true;
}

// ---- K2a units (44-47): level-init leaves with data-directed axes ---------
// Same full-DS contract as K1 (no input registers; all inputs are DS fields),
// plus a directed grid over up to two DS word fields that steer the branches
// (clamp bounds, transition-state values). All contracts read line-by-line:
//   44 sub_11397: [3A6]=[([25CB]-0x7AC2)&FFFF]&FF, [3A8]=[([25CB]-0x7ABC)&FFFF]&FF
//   45 sub_113b0: scroll limits from map dims [25DC]/[25DE] (SHL1/SHL3-0x140/0xB0)
//   46 sub_113d8: viewport center-clamp on viking [si+173D]/[si+1765]
//                 (si = [25BA]?[3C2]:0); SUB 0xA0/0x58 + JGE = SIGNED OPERAND
//                 compare (class #16 overflow zone X in [0x8000..0x809F])
//   47 sub_116e3: transition state machine on [25C9] level / [3CC] state
struct FtLeafAxis { uint16_t addr; const uint16_t* vals; int n; };
typedef void (*FtLeafNorm)(uint8_t* img);
struct FtLeafSpec { FtId id; FtClearCall call; FtLeafAxis ax1, ax2;
                    bool needs_file;      // unit's orig tail reads DATA.DAT
                    FtLeafNorm norm;      // input-domain fixup (call contract)
                    bool fuzz_base_only; };  // fuzz = base image + axes only
                    // (for units whose orig callees walk DS LUTs — full-DS
                    // noise would only measure those renderers on garbage
                    // tables, outside this function's contract; the axis grid
                    // already covers every branch of the function itself)

const uint16_t FT_AX_11397_DI[] = { 0, 2, 4, 8, 0x10, 0x14, 0x7AC2, 0x7AC4,
                                    0x8000, 0xFFFE };
const uint16_t FT_AX_DIMS[]     = { 0, 1, 2, 0x28, 0x2B, 0x100, 0x4000,
                                    0x7FFF, 0x8000, 0xFFFF };
const uint16_t FT_AX_VIKX[]     = { 0, 0x9F, 0xA0, 0xA1, 0x140, 0x7FFF,
                                    0x8000, 0x809F, 0x80A0, 0xFFFF };
const uint16_t FT_AX_VIKY[]     = { 0, 0x57, 0x58, 0x59, 0xB0, 0x7FFF,
                                    0x8000, 0x8057, 0x8058, 0xFFFF };
const uint16_t FT_AX_LEVEL[]    = { 0, 0x27, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
                                    0x30, 0x3E };
const uint16_t FT_AX_STATE[]    = { 0, 1, 2, 0x8000, 0x8001, 0x8002, 0xFFFF };
const uint16_t FT_AX_FADE[]     = { 0x0000, 0x0101, 0x3F3F, 0x4040, 0x7F7F,
                                    0xFF00, 0x00FF, 0xFFFF };
const uint16_t FT_AX_BLINK[]    = { 0, 1, 0x10, 0x11, 0x1F, 0x20, 0x21, 0xFF };
const uint16_t FT_AX_STATE01[]  = { 0, 1, 2 };

// sub_13a0e spawn unit: the tail runs the 13ae0 spawn loop over the [25F6]
// table; the base image carries the EXE-static table. Fuzz is base-only —
// noise in the spawn table drives sub_13809 (VM object init) on garbage
// code_seg indices, which is the vmops unit's territory.
void ft_norm_13a0e(uint8_t* img) {
    ft_wr16(img, 0x032F, 0);   // creation gate open (orig checks it in 13809)
}

// sub_11c52 fuzz domain: the orig callees sub_1183d/sub_120d1/sub_118ad are
// renderers indexing LUTs by DS fields — the call contract is HUD slot 0..11
// ([443]), item id 0..0x17 ([441]), viking selector indices 0..5
// ([414/416/418]). Random values there walk garbage pointers inside the
// oracle's renderer (aborting it mid-way) — outside the function's domain;
// pin them into range after each noise fill.
void ft_norm_11c52(uint8_t* img) {
    ft_wr16(img, 0x0443, (uint16_t)(*(uint16_t*)(img + 0x0443) % 12));
    ft_wr16(img, 0x0441, (uint16_t)(*(uint16_t*)(img + 0x0441) % 0x18));
    ft_wr16(img, 0x0414, (uint16_t)(*(uint16_t*)(img + 0x0414) % 6));
    ft_wr16(img, 0x0416, (uint16_t)(*(uint16_t*)(img + 0x0416) % 6));
    ft_wr16(img, 0x0418, (uint16_t)(*(uint16_t*)(img + 0x0418) % 6));
}

const FtLeafSpec FT_LEAVES[] = {
    { FT_SUB_11397, v2_fntest_call_sub_11397,
      { 0x25CB, FT_AX_11397_DI, 10 }, { 0, nullptr, 0 }, false, nullptr, false },
    { FT_SUB_113B0, v2_fntest_call_sub_113b0,
      { 0x25DC, FT_AX_DIMS, 10 }, { 0x25DE, FT_AX_DIMS, 10 }, false, nullptr, false },
    { FT_SUB_113D8, v2_fntest_call_sub_113d8,
      { OBJ_WORLD_X, FT_AX_VIKX, 10 }, { OBJ_WORLD_Y, FT_AX_VIKY, 10 }, false, nullptr, false },
    // 116e3's transition tail CALLs sub_10982 (DATA.DAT read into ds:2193) —
    // both sides need the same file context (class-D setup); fuzz images are
    // skipped for it (random [3D4] -> random chunk ids would just measure the
    // file-reader units 31/32 again on garbage ids, and a random [2BB2]
    // handle would diverge on the DOS-handle emulation, not this function).
    { FT_SUB_116E3, v2_fntest_call_sub_116e3,
      { 0x25C9, FT_AX_LEVEL, 9 }, { 0x3CC, FT_AX_STATE, 7 }, true, nullptr, false },
    // K3a/K3b leaves. (13a0e parked until the unit builds the class-B
    // template context: orig reads object templates via es=[2E67] while
    // v2_spawn_object_13809 needs v2_vm_shadow_animdata — task #29 wires both to one
    // synthetic template segment, like the vmops unit does with its
    // FT_VM_TESTSEG code segment.)
    { FT_SUB_10E99, v2_fntest_call_sub_10e99,
      { 0x0342, FT_AX_FADE, 8 }, { 0x0344, FT_AX_FADE, 8 }, false, nullptr, false },
    { FT_SUB_11C52, v2_fntest_call_sub_11c52,
      { 0x0445, FT_AX_BLINK, 8 }, { 0x0447, FT_AX_STATE01, 3 }, false,
      ft_norm_11c52, true },
    { FT_SUB_12D2C, v2_fntest_call_sub_12d2c,
      { 0x03A2, FT_AX_BLINK, 8 }, { 0x03A4, FT_AX_BLINK, 8 }, false,
      nullptr, false },
};

int ft_selftest_leaf(const FtLeafSpec& ls, uint32_t seed) {
    FtSynthStats grid, fuzz;
    long diff_budget = 24;
    FtClearSpec cs{ ls.id, ls.call };

    if (ls.needs_file) {
        v2_set_m2c_base(v2_fntest_m2c_base());
        if (!v2_fntest_set_data_file("DATA.DAT") ||
            !v2_fntest_set_data_file_v2("DATA.DAT")) {
            fprintf(stderr, "FNSELFTEST-SUMMARY[%s]: DATA.DAT missing — total cases=0 fail=1\n",
                    g_name[ls.id]);
            return 1;
        }
    }

    // Directed axis grid on the pristine base image.
    int n1 = ls.ax1.n ? ls.ax1.n : 1;
    int n2 = ls.ax2.n ? ls.ax2.n : 1;
    for (int i = 0; i < n1; i++)
        for (int j = 0; j < n2; j++) {
            memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
            if (ls.ax1.n) ft_wr16(g_synth_in, ls.ax1.addr, ls.ax1.vals[i]);
            if (ls.ax2.n) ft_wr16(g_synth_in, ls.ax2.addr, ls.ax2.vals[j]);
            if (ls.norm) ls.norm(g_synth_in);
            ft_synth_case_clear(cs, "grid", grid, diff_budget);
        }

    // Directed axes on random images (branch values with noisy context),
    // alternating with pure-noise fuzz. Skipped for file-context units (see
    // the FT_LEAVES note).
    FtRng rng(seed);
    for (int i = 0; i < (ls.needs_file ? 0 : 48); i++) {
        if (ls.fuzz_base_only)
            memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
        else
        for (uint32_t a = 0; a < 0x10000; a += 2) {
            uint16_t w = rng.w();
            g_synth_in[a] = (uint8_t)w; g_synth_in[a + 1] = (uint8_t)(w >> 8);
        }
        if (ls.ax1.n) ft_wr16(g_synth_in, ls.ax1.addr,
                              ls.ax1.vals[rng.next() % (uint32_t)ls.ax1.n]);
        if (ls.ax2.n) ft_wr16(g_synth_in, ls.ax2.addr,
                              ls.ax2.vals[rng.next() % (uint32_t)ls.ax2.n]);
        if (ls.norm) ls.norm(g_synth_in);
        ft_synth_case_clear(cs, "fuzz-dir", fuzz, diff_budget);
        if (ls.fuzz_base_only) continue;   // base+axes only (see FT_LEAVES note)
        for (uint32_t a = 0; a < 0x10000; a += 2) {
            uint16_t w = rng.w();
            g_synth_in[a] = (uint8_t)w; g_synth_in[a + 1] = (uint8_t)(w >> 8);
        }
        if (ls.norm) ls.norm(g_synth_in);
        ft_synth_case_clear(cs, "fuzz", fuzz, diff_budget);
    }

    fprintf(stderr,
        "FNSELFTEST-SUMMARY[%s]: grid %ld/%ld, fuzz %ld/%ld — total cases=%ld fail=%ld%s\n",
        g_name[ls.id], grid.pass, grid.cases, fuzz.pass, fuzz.cases,
        grid.cases + fuzz.cases, grid.fail + fuzz.fail,
        (grid.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (grid.fail + fuzz.fail) ? 1 : 0;
}

// ---- K2b units (48-50, 52-53): spawn parsers / glyph / seg001 text --------
// 48 sub_11383: scan [di+25F6] step 0x0E until 0xFFFF, return di+2. No DS
//    writes. Oracle di-out compared via io_regs[5].
// 49 sub_1133a: HUD init parser — [2583]=first byte; per-viking: [si+2584/
//    258C]=type, [si+2594]=[di+25F7], [si+259C]=[di+25F8]; skip sub-list to
//    0xFFFF; terminator byte 0 -> INC di, return di.
// 50 sub_1241e: glyph cell write [ (si + [di*2-6CBA]) - 6A94 ] = al; returns
//    si+1 (PUSH/POP si; INC si).
// 52 sub_12515: word_2850A = seg001[ax*2] (const EXE data, both sides read
//    the same bytes).
// 53 sub_12529: word_28514/16 = seg001[bx]/[bx+1] bytes; bx += 2 (io_regs).
bool ft_synth_case_regs(FtId id, const FtRegs& in, uint16_t v2_ret,
                        int ret_reg /*-1 none, 4=si, 5=di, 1=bx*/,
                        const char* group, FtSynthStats& st, long& diff_budget)
{
    // g_synth_in prepared by caller; g_scratch already run by caller (v2_ret).
    st.cases++;
    memcpy(g_synth_orig, g_synth_in, sizeof(g_synth_orig));
    uint16_t regs[8] = { in.ax, in.bx, in.cx, in.dx, in.si, in.di, in.bp, 0 };
    long se0 = v2_fntest_start_escapes, rm0 = v2_fntest_ret_mismatches();
    v2_fntest_orig_isolated(v2_fntest_orig_fnptr(id), g_synth_orig, regs);
    long se_d = v2_fntest_start_escapes - se0;
    long rm_d = v2_fntest_ret_mismatches() - rm0;

    long diffs = 0;
    if ((se_d || rm_d) && diff_budget > 0)
        fprintf(stderr, "FNSELFTEST-UB[%s %s]: oracle start-escapes=%ld ret-mismatches=%ld\n",
                g_name[id], group, se_d, rm_d);
    if (ret_reg >= 0 && regs[ret_reg] != v2_ret) {
        if (diff_budget > 0) {
            diff_budget--;
            fprintf(stderr, "FNSELFTEST-DIFF[%s %s]: reg[%d] orig=%04X v2=%04X | "
                    "ax=%04X bx=%04X si=%04X di=%04X\n",
                    g_name[id], group, ret_reg, regs[ret_reg], v2_ret,
                    in.ax, in.bx, in.si, in.di);
        }
        diffs++;
    }
    for (uint32_t a = 0; a < 0x10000; a++) {
        if (g_scratch[a] == g_synth_orig[a]) continue;
        if (v2_fntest_ds_skip(a)) continue;
        if (diff_budget > 0) {
            diff_budget--;
            fprintf(stderr, "FNSELFTEST-DIFF[%s %s]: addr=%04X orig=%02X v2=%02X (in=%02X) | "
                    "ax=%04X bx=%04X si=%04X di=%04X\n",
                    g_name[id], group, a, g_synth_orig[a], g_scratch[a], g_synth_in[a],
                    in.ax, in.bx, in.si, in.di);
        }
        diffs++;
    }
    if (diffs) { st.fail++; return false; }
    st.pass++; return true;
}

int ft_selftest_sub_11383() {
    FtSynthStats grid, fuzz;
    long diff_budget = 24;
    // k entries of 0x0E garbage bytes, then the 0xFFFF terminator.
    FtRng rng(0x11383001u);
    for (int k = 0; k <= 6; k++) {
        for (int rep = 0; rep < 8; rep++) {
            memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
            for (int e = 0; e < k; e++)
                for (int b = 0; b < 0x0E; b += 2) {
                    uint16_t w = rng.w();
                    if (w == 0xFFFF) w = 0xFFFE;   // no early terminator
                    ft_wr16(g_synth_in, (uint16_t)(e * 0x0E + b + 0x25F6), w);
                }
            ft_wr16(g_synth_in, (uint16_t)(k * 0x0E + 0x25F6), 0xFFFF);
            ft_fill_tail(g_synth_in);
            memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
            uint16_t di_v2 = v2_fntest_call_sub_11383(g_scratch);
            FtRegs in{}; ft_synth_case_regs(FT_SUB_11383, in, di_v2, 5,
                                            "grid", grid, diff_budget);
        }
    }
    fprintf(stderr,
        "FNSELFTEST-SUMMARY[sub_11383]: grid %ld/%ld, fuzz %ld/%ld — total cases=%ld fail=%ld%s\n",
        grid.pass, grid.cases, fuzz.pass, fuzz.cases, grid.cases + fuzz.cases,
        grid.fail + fuzz.fail, (grid.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (grid.fail + fuzz.fail) ? 1 : 0;
}

int ft_selftest_sub_1133a() {
    FtSynthStats grid, fuzz;
    long diff_budget = 24;
    FtRng rng(0x1133A001u);
    // nv vikings (0..3), each with a sub-list of ns words then 0xFFFF; the
    // viking list ends with a 0 type byte. di start varied.
    static const uint16_t DIS[] = { 0, 2, 0x10, 0x100 };
    for (uint16_t di0 : DIS)
        for (int nv = 0; nv <= 3; nv++)
            for (int rep = 0; rep < 6; rep++) {
                memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
                uint16_t p = (uint16_t)(di0 + 0x25F6);
                uint16_t hdr = rng.w();
                ft_wr16(g_synth_in, p, hdr); p += 2;
                for (int v = 0; v < nv; v++) {
                    uint8_t type = (uint8_t)(1 + (rng.next() % 0xFE));  // nonzero
                    g_synth_in[p] = type;
                    g_synth_in[(uint16_t)(p + 1)] = (uint8_t)rng.w();
                    g_synth_in[(uint16_t)(p + 2)] = (uint8_t)rng.w();
                    p = (uint16_t)(p + 3);
                    int ns = (int)(rng.next() % 4);
                    for (int e = 0; e < ns; e++) {
                        uint16_t w = rng.w(); if (w == 0xFFFF) w = 0;
                        ft_wr16(g_synth_in, p, w); p = (uint16_t)(p + 2);
                    }
                    ft_wr16(g_synth_in, p, 0xFFFF); p = (uint16_t)(p + 2);
                }
                g_synth_in[p] = 0;   // list terminator (byte 0)
                ft_fill_tail(g_synth_in);
                memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
                uint16_t di_v2 = v2_fntest_call_sub_1133a(g_scratch, di0);
                FtRegs in{}; in.di = di0;
                ft_synth_case_regs(FT_SUB_1133A, in, di_v2, 5,
                                   "grid", grid, diff_budget);
            }
    fprintf(stderr,
        "FNSELFTEST-SUMMARY[sub_1133a]: grid %ld/%ld, fuzz %ld/%ld — total cases=%ld fail=%ld%s\n",
        grid.pass, grid.cases, fuzz.pass, fuzz.cases, grid.cases + fuzz.cases,
        grid.fail + fuzz.fail, (grid.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (grid.fail + fuzz.fail) ? 1 : 0;
}

int ft_selftest_sub_1241e() {
    FtSynthStats grid, fuzz;
    long diff_budget = 24;
    static const uint16_t ALS[] = { 0x00, 0x12, 0x41, 0x7F, 0xFF };
    static const uint16_t SIS[] = { 0, 1, 0x27, 0x100, 0x8000, 0xFFFF };
    static const uint16_t DIS[] = { 0, 1, 2, 0x0B, 0x17 };
    for (uint16_t al : ALS)
        for (uint16_t si : SIS)
            for (uint16_t di : DIS) {
                memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
                ft_fill_tail(g_synth_in);
                memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
                uint16_t si_v2 = v2_fntest_call_sub_1241e(g_scratch, al, si, di);
                FtRegs in{}; in.ax = al; in.si = si; in.di = di;
                ft_synth_case_regs(FT_SUB_1241E, in, si_v2, 4,
                                   "grid", grid, diff_budget);
            }
    FtRng rng(0x1241E001u);
    for (int i = 0; i < 4000; i++) {
        memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
        ft_fill_tail(g_synth_in);
        memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
        uint16_t al = (uint16_t)(rng.w() & 0xFF), si = rng.w(),
                 di = (uint16_t)(rng.next() % 0x18);
        uint16_t si_v2 = v2_fntest_call_sub_1241e(g_scratch, al, si, di);
        FtRegs in{}; in.ax = al; in.si = si; in.di = di;
        ft_synth_case_regs(FT_SUB_1241E, in, si_v2, 4, "fuzz", fuzz, diff_budget);
    }
    fprintf(stderr,
        "FNSELFTEST-SUMMARY[sub_1241e]: grid %ld/%ld, fuzz %ld/%ld — total cases=%ld fail=%ld%s\n",
        grid.pass, grid.cases, fuzz.pass, fuzz.cases, grid.cases + fuzz.cases,
        grid.fail + fuzz.fail, (grid.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (grid.fail + fuzz.fail) ? 1 : 0;
}

int ft_selftest_textcfg(FtId id, uint32_t seed) {
    FtSynthStats grid, fuzz;
    long diff_budget = 24;
    // ax (12515) / bx (12529) sweep over the real seg001 table zone + edges.
    // bx capped below 0xFF00: bx+1 at 0xFFFF is a port-linear (non-8086)
    // corner on BOTH sides; real data never goes there.
    static const uint16_t VS[] = { 0, 1, 2, 5, 0x10, 0x40, 0x100, 0x400,
                                   0x1000, 0x7FFE, 0x8000, 0xFF00 };
    for (uint16_t v : VS) {
        memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
        ft_fill_tail(g_synth_in);
        memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
        FtRegs in{};
        uint16_t ret = 0; int rr = -1;
        if (id == FT_SUB_12515) { v2_fntest_call_sub_12515(g_scratch, v); in.ax = v; }
        else { ret = v2_fntest_call_sub_12529(g_scratch, v); in.bx = v; rr = 1; }
        ft_synth_case_regs(id, in, ret, rr, "grid", grid, diff_budget);
    }
    FtRng rng(seed);
    for (int i = 0; i < 2000; i++) {
        memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
        ft_fill_tail(g_synth_in);
        memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
        uint16_t v = (uint16_t)(rng.w() & 0x7FFF);
        FtRegs in{};
        uint16_t ret = 0; int rr = -1;
        if (id == FT_SUB_12515) { v2_fntest_call_sub_12515(g_scratch, v); in.ax = v; }
        else { ret = v2_fntest_call_sub_12529(g_scratch, v); in.bx = v; rr = 1; }
        ft_synth_case_regs(id, in, ret, rr, "fuzz", fuzz, diff_budget);
    }
    fprintf(stderr,
        "FNSELFTEST-SUMMARY[%s]: grid %ld/%ld, fuzz %ld/%ld — total cases=%ld fail=%ld%s\n",
        g_name[id], grid.pass, grid.cases, fuzz.pass, fuzz.cases,
        grid.cases + fuzz.cases, grid.fail + fuzz.fail,
        (grid.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (grid.fail + fuzz.fail) ? 1 : 0;
}

// ---- K3a: sub_1450b (save header regs + sub_10e99 fade tail) --------------
int ft_selftest_sub_1450b() {
    FtSynthStats grid, fuzz;
    long diff_budget = 24;
    static const uint16_t VALS[] = { 0, 1, 2, 5, 0x0F, 0x10, 0x7F, 0x80, 0xFF };
    for (uint16_t a : VALS) for (uint16_t si : VALS) for (uint16_t di : VALS) {
        memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
        ft_fill_tail(g_synth_in);
        memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
        v2_fntest_call_sub_1450b(g_scratch, a, si, di);
        FtRegs in{}; in.ax = a; in.si = si; in.di = di;
        ft_synth_case_regs(FT_SUB_1450B, in, 0, -1, "grid", grid, diff_budget);
    }
    FtRng rng(0x1450B001u);
    for (int i = 0; i < 2000; i++) {
        for (uint32_t a2 = 0x7EF0; a2 < 0x8300; a2 += 2) {   // noisy DAC zone
            uint16_t w = rng.w();
            g_synth_in[a2] = (uint8_t)w; g_synth_in[a2 + 1] = (uint8_t)(w >> 8);
        }
        ft_fill_tail(g_synth_in);
        memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
        uint16_t a = rng.w(), si2 = rng.w(), di2 = rng.w();
        v2_fntest_call_sub_1450b(g_scratch, a, si2, di2);
        FtRegs in{}; in.ax = a; in.si = si2; in.di = di2;
        ft_synth_case_regs(FT_SUB_1450B, in, 0, -1, "fuzz", fuzz, diff_budget);
    }
    fprintf(stderr,
        "FNSELFTEST-SUMMARY[sub_1450b]: grid %ld/%ld, fuzz %ld/%ld — total cases=%ld fail=%ld%s\n",
        grid.pass, grid.cases, fuzz.pass, fuzz.cases, grid.cases + fuzz.cases,
        grid.fail + fuzz.fail, (grid.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (grid.fail + fuzz.fail) ? 1 : 0;
}

// ---- K3a: sub_15d3c / sub_15d42 (bbox CF twins; CF via io_regs[7]) ---------
int ft_selftest_bbox2(FtId id, uint32_t seed) {
    FtSynthStats grid, fuzz;
    long diff_budget = 24;
    // Directed field sets for [obj+14E5/150D/1535/155D] pairs: overlapping,
    // touching (the DEC ax edge), disjoint, sign borders.
    static const uint16_t P[][4] = {   // {start, end} for di and si objects
        { 0x0100, 0x0110, 0x0100, 0x0110 },   // identical
        { 0x0100, 0x0110, 0x0110, 0x0120 },   // touching (end==start; DEC edge)
        { 0x0100, 0x0110, 0x0111, 0x0120 },   // disjoint by 1
        { 0x0100, 0x0110, 0x010F, 0x0120 },   // overlap by 1
        { 0x0000, 0x0000, 0x0000, 0x0000 },   // degenerate zeros
        { 0x7FF0, 0x8010, 0x7FF8, 0x8008 },   // sign border spans
        { 0xFFF0, 0x0010, 0x0000, 0x0008 },   // wrap-looking values
    };
    uint16_t di = 2, si = 4;
    for (auto& px : P) for (auto& py : P) {
        memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
        ft_wr16(g_synth_in, (uint16_t)(di + 0x14E5), px[0]);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_Y1), px[1]);
        ft_wr16(g_synth_in, (uint16_t)(si + 0x14E5), px[2]);
        ft_wr16(g_synth_in, (uint16_t)(si + OBJ_BBOX_Y1), px[3]);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_X0), py[0]);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_X1), py[1]);
        ft_wr16(g_synth_in, (uint16_t)(si + OBJ_BBOX_X0), py[2]);
        ft_wr16(g_synth_in, (uint16_t)(si + OBJ_BBOX_X1), py[3]);
        ft_fill_tail(g_synth_in);
        memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
        int cf = (id == FT_SUB_15D3C)
                 ? v2_fntest_call_sub_15d3c(g_scratch, si, di)
                 : v2_fntest_call_sub_15d42(g_scratch, si, di);
        FtRegs in{}; in.si = si; in.di = di;
        ft_synth_case_regs(id, in, (uint16_t)cf, 7, "grid", grid, diff_budget);
    }
    FtRng rng(seed);
    for (int i = 0; i < 6000; i++) {
        memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
        uint16_t d2 = (uint16_t)((rng.next() % 20) * 2);
        uint16_t s2 = (uint16_t)((rng.next() % 20) * 2);
        static const uint16_t BASES[4] = { 0x14E5, OBJ_BBOX_Y1, OBJ_BBOX_X0, OBJ_BBOX_X1 };
        for (int b = 0; b < 4; b++) {
            ft_wr16(g_synth_in, (uint16_t)(d2 + BASES[b]), rng.w());
            ft_wr16(g_synth_in, (uint16_t)(s2 + BASES[b]), rng.w());
        }
        ft_fill_tail(g_synth_in);
        memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
        int cf = (id == FT_SUB_15D3C)
                 ? v2_fntest_call_sub_15d3c(g_scratch, s2, d2)
                 : v2_fntest_call_sub_15d42(g_scratch, s2, d2);
        FtRegs in{}; in.si = s2; in.di = d2;
        ft_synth_case_regs(id, in, (uint16_t)cf, 7, "fuzz", fuzz, diff_budget);
    }
    fprintf(stderr,
        "FNSELFTEST-SUMMARY[%s]: grid %ld/%ld, fuzz %ld/%ld — total cases=%ld fail=%ld%s\n",
        g_name[id], grid.pass, grid.cases, fuzz.pass, fuzz.cases,
        grid.cases + fuzz.cases, grid.fail + fuzz.fail,
        (grid.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (grid.fail + fuzz.fail) ? 1 : 0;
}

// ---- Units 75-76: sub_15cef / sub_15cf5 (X bbox probe, Y-vel adjust) ------
// The bbox check of the sub_15c37 X-vel search. Entry ax = self.X_start
// (sub_15cef) / self.X_end (sub_15cf5), shared tail loc_15cf9. CF via
// regs[7]; ds:0x32 = Y-adj scratch written ONLY after both X gates pass
// (0xBBBB canary — the full-DS diff checks both value and written-at-all).
int ft_selftest_bbox_vel(FtId id, uint32_t seed) {
    FtSynthStats grid, exh, fuzz;
    long diff_budget = 24;
    uint16_t di = 2, si = 4;
    // X spans {self.X0, self.X1, cand.X0, cand.X1}: JL edge, entry==cand.X1
    // (the DEC edge), JGE edge, ax=0 (DEC wraps to 0xFFFF), sign borders.
    static const uint16_t PX[][4] = {
        { 0x0100, 0x0110, 0x0100, 0x0110 },   // identical spans
        { 0x0100, 0x0110, 0x0110, 0x0120 },   // entry below cand.X0 (JL edge)
        { 0x0110, 0x0120, 0x0100, 0x0110 },   // entry == cand.X1 (DEC edge hits)
        { 0x0111, 0x0121, 0x0100, 0x0110 },   // entry-1 == cand.X1 (JGE edge)
        { 0x0000, 0x0000, 0x0000, 0x0001 },   // ax=0 → DEC wraps to 0xFFFF
        { 0x7FFF, 0x8001, 0x7FF0, 0x8010 },   // sign borders
        { 0xFFF0, 0x0010, 0xFFF8, 0x0008 },   // wrap-looking values
    };
    // Y/vel sets {self.Y0, self.Y1, self.vel, cand.Y0, cand.Y1, cand.vel}:
    // vel=0 overlap/touch/miss, ± velocity shifts, SUB-wrap (0x8000-0x7FFF
    // class), adjusted values crossing the sign border.
    static const uint16_t PY[][6] = {
        { 0x0100, 0x0110, 0x0000, 0x0100, 0x0110, 0x0000 },
        { 0x0100, 0x0110, 0x0000, 0x0110, 0x0120, 0x0000 },
        { 0x0100, 0x0110, 0x0000, 0x0111, 0x0120, 0x0000 },
        { 0x0100, 0x0110, 0x0010, 0x0100, 0x0110, 0xFFF0 },
        { 0x8000, 0x8010, 0x7FFF, 0x0100, 0x0110, 0x0000 },
        { 0x0100, 0x0110, 0x0000, 0x8000, 0x8010, 0x7FFF },
        { 0x7FFF, 0x7FFF, 0xFFFF, 0x8000, 0x8000, 0x0001 },
    };
    for (auto& px : PX) for (auto& py : PY) {
        memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
        ft_wr16(g_synth_in, 0x0032, 0xBBBB);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_X0), px[0]);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_X1), px[1]);
        ft_wr16(g_synth_in, (uint16_t)(si + OBJ_BBOX_X0), px[2]);
        ft_wr16(g_synth_in, (uint16_t)(si + OBJ_BBOX_X1), px[3]);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_Y0), py[0]);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_Y1), py[1]);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_VEL_Y),   py[2]);
        ft_wr16(g_synth_in, (uint16_t)(si + OBJ_BBOX_Y0), py[3]);
        ft_wr16(g_synth_in, (uint16_t)(si + OBJ_BBOX_Y1), py[4]);
        ft_wr16(g_synth_in, (uint16_t)(si + OBJ_VEL_Y),   py[5]);
        ft_fill_tail(g_synth_in);
        memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
        int cf = (id == FT_SUB_15CEF)
                 ? v2_fntest_call_sub_15cef(g_scratch, si, di)
                 : v2_fntest_call_sub_15cf5(g_scratch, si, di);
        FtRegs in{}; in.si = si; in.di = di;
        ft_synth_case_regs(id, in, (uint16_t)cf, 7, "grid", grid, diff_budget);
    }
    // Exhaustive 1: the entry-X axis over all 65536 values (both X fields set
    // to the axis value — only one is read per twin), cand span mid-range,
    // passing Y context. Covers every JL/DEC/JGE decision point bit-exactly.
    // Exhaustive 2: the self Y-velocity axis over all 65536 values with the
    // X gates passing — sweeps both SUB-wrap adjust points through JL edges.
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t x = 0; x <= 0xFFFF; x++) {
            memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
            ft_wr16(g_synth_in, 0x0032, 0xBBBB);
            if (pass == 0) {
                ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_X0), (uint16_t)x);
                ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_X1), (uint16_t)x);
                ft_wr16(g_synth_in, (uint16_t)(si + OBJ_BBOX_X0), 0x0100);
                ft_wr16(g_synth_in, (uint16_t)(si + OBJ_BBOX_X1), 0x0110);
                ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_Y0), 0x0100);
                ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_Y1), 0x0110);
                ft_wr16(g_synth_in, (uint16_t)(si + OBJ_BBOX_Y0), 0x0100);
                ft_wr16(g_synth_in, (uint16_t)(si + OBJ_BBOX_Y1), 0x0110);
            } else {
                ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_X0), 0x0105);
                ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_X1), 0x0105);
                ft_wr16(g_synth_in, (uint16_t)(si + OBJ_BBOX_X0), 0x0100);
                ft_wr16(g_synth_in, (uint16_t)(si + OBJ_BBOX_X1), 0x0110);
                ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_Y0), 0x0100);
                ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_Y1), 0x0110);
                ft_wr16(g_synth_in, (uint16_t)(di + OBJ_VEL_Y), (uint16_t)x);
                ft_wr16(g_synth_in, (uint16_t)(si + OBJ_BBOX_Y0), 0x0100);
                ft_wr16(g_synth_in, (uint16_t)(si + OBJ_BBOX_Y1), 0x0110);
            }
            ft_fill_tail(g_synth_in);
            memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
            int cf = (id == FT_SUB_15CEF)
                     ? v2_fntest_call_sub_15cef(g_scratch, si, di)
                     : v2_fntest_call_sub_15cf5(g_scratch, si, di);
            FtRegs in{}; in.si = si; in.di = di;
            ft_synth_case_regs(id, in, (uint16_t)cf, 7, "exh", exh, diff_budget);
        }
    }
    FtRng rng(seed);
    for (int i = 0; i < 20000; i++) {
        memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
        ft_wr16(g_synth_in, 0x0032, 0xBBBB);
        uint16_t d2 = (uint16_t)((rng.next() % 20) * 2);
        uint16_t s2 = (uint16_t)((rng.next() % 20) * 2);
        static const uint16_t BASES[5] = { OBJ_BBOX_X0, OBJ_BBOX_X1,
                                           OBJ_BBOX_Y0, OBJ_BBOX_Y1, OBJ_VEL_Y };
        for (int b = 0; b < 5; b++) {
            ft_wr16(g_synth_in, (uint16_t)(d2 + BASES[b]), rng.w());
            ft_wr16(g_synth_in, (uint16_t)(s2 + BASES[b]), rng.w());
        }
        ft_fill_tail(g_synth_in);
        memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
        int cf = (id == FT_SUB_15CEF)
                 ? v2_fntest_call_sub_15cef(g_scratch, s2, d2)
                 : v2_fntest_call_sub_15cf5(g_scratch, s2, d2);
        FtRegs in{}; in.si = s2; in.di = d2;
        ft_synth_case_regs(id, in, (uint16_t)cf, 7, "fuzz", fuzz, diff_budget);
    }
    fprintf(stderr,
        "FNSELFTEST-SUMMARY[%s]: grid %ld/%ld, exh %ld/%ld, fuzz %ld/%ld — total cases=%ld fail=%ld%s\n",
        g_name[id], grid.pass, grid.cases, exh.pass, exh.cases, fuzz.pass, fuzz.cases,
        grid.cases + exh.cases + fuzz.cases, grid.fail + exh.fail + fuzz.fail,
        (grid.fail + exh.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (grid.fail + exh.fail + fuzz.fail) ? 1 : 0;
}

// ---- Units 77-78: sub_15de5 / sub_15df2 (X-axis object search) ------------
// Shared core loc_15dfd: probe X = self.X0-1 (15de5) / self.X1+1 (15df2);
// slot loop: [s+1355]!=0, s!=ds:42, ds:3A=s; filter chain al=[s+17DD].lo vs
// bytes at [f-0x6B34] (JB miss / JZ match / INC); X gates JL + DEC/JGE on
// target span; Y gates: self.Y1-vel >= target.Y0 (JL) and self.Y0-vel <=
// target.Y1 (JZ hit + JGE skip). Hit: ds:3B2=[s+17DD] FULL WORD, ds:3B4=s,
// STC. DS: 0x34=filter, 0x36=probe (written unconditionally up front).
static void ft_objscan_ctx(uint16_t di, uint16_t filter,
                           uint16_t self_x0, uint16_t self_x1,
                           uint16_t self_y0, uint16_t self_y1, uint16_t self_vel,
                           uint16_t table_end, uint16_t cur_obj) {
    memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
    ft_wr16(g_synth_in, 0x372, table_end);
    ft_wr16(g_synth_in, 0x42, cur_obj);
    // scratch canaries (0x38 must stay untouched by this family)
    ft_wr16(g_synth_in, 0x34, 0xBBBB); ft_wr16(g_synth_in, 0x36, 0xBBBB);
    ft_wr16(g_synth_in, 0x38, 0xBBBB); ft_wr16(g_synth_in, 0x3A, 0xBBBB);
    ft_wr16(g_synth_in, 0x3B2, 0xBBBB); ft_wr16(g_synth_in, 0x3B4, 0xBBBB);
    // filter chain at [filter-0x6B34]: single type 0x30, terminator 0xFF
    g_synth_in[(uint16_t)(filter - LUT_SCAN_FILTER)]     = 0x30;
    g_synth_in[(uint16_t)(filter - LUT_SCAN_FILTER + 1)] = 0xFF;
    ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_X0), self_x0);
    ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_X1), self_x1);
    ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_Y0), self_y0);
    ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_Y1), self_y1);
    ft_wr16(g_synth_in, (uint16_t)(di + OBJ_VEL_Y),   self_vel);
}
static void ft_objscan_target(uint16_t slot, uint16_t alive, uint16_t type_word,
                              uint16_t x0, uint16_t x1, uint16_t y0, uint16_t y1) {
    ft_wr16(g_synth_in, (uint16_t)(slot + OBJ_CODE_SEG), alive);
    ft_wr16(g_synth_in, (uint16_t)(slot + 0x17DD),       type_word);
    ft_wr16(g_synth_in, (uint16_t)(slot + OBJ_BBOX_X0),  x0);
    ft_wr16(g_synth_in, (uint16_t)(slot + OBJ_BBOX_X1),  x1);
    ft_wr16(g_synth_in, (uint16_t)(slot + OBJ_BBOX_Y0),  y0);
    ft_wr16(g_synth_in, (uint16_t)(slot + OBJ_BBOX_Y1),  y1);
}
int ft_selftest_objscan_x(FtId id, uint32_t seed) {
    FtSynthStats grid, exh, fuzz;
    long diff_budget = 24;
    const uint16_t di = 8, FLT = 0x7000;   // chain bytes at ds:0x4CC
    auto run1 = [&](const char* group, FtSynthStats& st) {
        ft_fill_tail(g_synth_in);
        memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
        int cf = (id == FT_SUB_15DE5)
                 ? v2_fntest_call_sub_15de5(g_scratch, FLT, di)
                 : v2_fntest_call_sub_15df2(g_scratch, FLT, di);
        FtRegs in{}; in.si = FLT; in.di = di;
        ft_synth_case_regs(id, in, (uint16_t)cf, 7, group, st, diff_budget);
    };
    // Probe geometry: for 15de5 probe = self_x0-1, for 15df2 probe = self_x1+1.
    // Self span picked so probe==0x0105 in both twins.
    const uint16_t SX0 = 0x0106, SX1 = 0x0104;   // 15de5: 0x0106-1; 15df2: 0x0104+1
    struct GC { uint16_t alive, type, tx0, tx1, ty0, ty1, s_y0, s_y1, s_vel, tend, cur; };
    static const GC G[] = {
        // basic hit: probe 0x105 in [0x100,0x110), Y overlap, vel 0
        { 1, 0x0030, 0x0100, 0x0110, 0x0100, 0x0110, 0x0100, 0x0110, 0x0000, 4, 8 },
        // dead slot → CLC
        { 0, 0x0030, 0x0100, 0x0110, 0x0100, 0x0110, 0x0100, 0x0110, 0x0000, 4, 8 },
        // self-skip: cur_obj == slot 2 → CLC
        { 1, 0x0030, 0x0100, 0x0110, 0x0100, 0x0110, 0x0100, 0x0110, 0x0000, 4, 2 },
        // filter miss: type below chain head (JB)
        { 1, 0x0010, 0x0100, 0x0110, 0x0100, 0x0110, 0x0100, 0x0110, 0x0000, 4, 8 },
        // filter miss: type above terminator path (INC walk into 0xFF stop)
        { 1, 0x0031, 0x0100, 0x0110, 0x0100, 0x0110, 0x0100, 0x0110, 0x0000, 4, 8 },
        // X JL edge: target.X0 == probe+1 → JL rejects
        { 1, 0x0030, 0x0106, 0x0110, 0x0100, 0x0110, 0x0100, 0x0110, 0x0000, 4, 8 },
        // X JL edge pass: target.X0 == probe
        { 1, 0x0030, 0x0105, 0x0110, 0x0100, 0x0110, 0x0100, 0x0110, 0x0000, 4, 8 },
        // X JGE edge: probe-1 == target.X1 → JGE rejects
        { 1, 0x0030, 0x0100, 0x0104, 0x0100, 0x0110, 0x0100, 0x0110, 0x0000, 4, 8 },
        // Y gate 1 edge: self.Y1-vel == target.Y0-1 → JL rejects
        { 1, 0x0030, 0x0100, 0x0110, 0x0111, 0x0120, 0x0100, 0x0110, 0x0000, 4, 8 },
        // Y gate 1 edge pass: self.Y1-vel == target.Y0
        { 1, 0x0030, 0x0100, 0x0110, 0x0110, 0x0120, 0x0100, 0x0110, 0x0000, 4, 8 },
        // Y gate 2 JZ hit: self.Y0-vel == target.Y1 (equality passes via JZ)
        { 1, 0x0030, 0x0100, 0x0110, 0x0100, 0x0110, 0x0110, 0x0120, 0x0000, 4, 8 },
        // Y gate 2 JGE reject: self.Y0-vel == target.Y1+1
        { 1, 0x0030, 0x0100, 0x0110, 0x0100, 0x010F, 0x0110, 0x0120, 0x0000, 4, 8 },
        // vel SUB-wrap: self.Y0=0x8000 vel=0x7FFF → adj wraps to +1
        { 1, 0x0030, 0x0100, 0x0110, 0x0000, 0x0110, 0x8000, 0x8010, 0x7FFF, 4, 8 },
        // full-word 0x3B2 channel: type high byte set, hit
        { 1, 0x1230, 0x0100, 0x0110, 0x0100, 0x0110, 0x0100, 0x0110, 0x0000, 4, 8 },
        // empty table
        { 1, 0x0030, 0x0100, 0x0110, 0x0100, 0x0110, 0x0100, 0x0110, 0x0000, 0, 8 },
    };
    for (auto& g : G) {
        ft_objscan_ctx(di, FLT, SX0, SX1, g.s_y0, g.s_y1, g.s_vel, g.tend, g.cur);
        ft_objscan_target(2, g.alive, g.type, g.tx0, g.tx1, g.ty0, g.ty1);
        run1("grid", grid);
    }
    // two-slot restore-path case: slot 0 filter-miss (walk), slot 2 hits;
    // then slot 0 X-miss, slot 2 hits (3A-протокол restore).
    {
        ft_objscan_ctx(di, FLT, SX0, SX1, 0x0100, 0x0110, 0, 4, 8);
        ft_objscan_target(0, 1, 0x0010, 0x0100, 0x0110, 0x0100, 0x0110); // JB miss
        ft_objscan_target(2, 1, 0x0030, 0x0100, 0x0110, 0x0100, 0x0110);
        run1("grid", grid);
        ft_objscan_ctx(di, FLT, SX0, SX1, 0x0100, 0x0110, 0, 4, 8);
        ft_objscan_target(0, 1, 0x0030, 0x0200, 0x0210, 0x0100, 0x0110); // X miss
        ft_objscan_target(2, 1, 0x0030, 0x0100, 0x0110, 0x0100, 0x0110);
        run1("grid", grid);
        // probe wrap: 15de5 self.X0=0 → probe 0xFFFF; 15df2 self.X1=0xFFFF → probe 0
        ft_objscan_ctx(di, FLT, 0x0000, 0xFFFF, 0x0100, 0x0110, 0, 4, 8);
        ft_objscan_target(2, 1, 0x0030, 0xFFF0, 0x0010, 0x0100, 0x0110);
        run1("grid", grid);
    }
    // Exhaustive 1: target.X0 axis over 65536 (probe fixed at 0x0105).
    // Exhaustive 2: self Y-velocity axis over 65536 (X pass, Y spans fixed).
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t x = 0; x <= 0xFFFF; x++) {
            if (pass == 0) {
                ft_objscan_ctx(di, FLT, SX0, SX1, 0x0100, 0x0110, 0, 4, 8);
                ft_objscan_target(2, 1, 0x0030, (uint16_t)x, 0x0110, 0x0100, 0x0110);
            } else {
                ft_objscan_ctx(di, FLT, SX0, SX1, 0x0100, 0x0110, (uint16_t)x, 4, 8);
                ft_objscan_target(2, 1, 0x0030, 0x0100, 0x0110, 0x0100, 0x0110);
            }
            run1("exh", exh);
        }
    }
    FtRng rng(seed);
    for (int i = 0; i < 20000; i++) {
        ft_objscan_ctx(di, FLT, rng.w(), rng.w(), rng.w(), rng.w(), rng.w(),
                       (uint16_t)((rng.next() % 5) * 2), (uint16_t)((rng.next() % 6) * 2));
        for (uint16_t slot = 0; slot < 8; slot += 2)
            ft_objscan_target(slot, (uint16_t)(rng.next() & 1),
                              (uint16_t)(rng.w() & 0x0FFF ? rng.w() : 0x0030),
                              rng.w(), rng.w(), rng.w(), rng.w());
        run1("fuzz", fuzz);
    }
    fprintf(stderr,
        "FNSELFTEST-SUMMARY[%s]: grid %ld/%ld, exh %ld/%ld, fuzz %ld/%ld — total cases=%ld fail=%ld%s\n",
        g_name[id], grid.pass, grid.cases, exh.pass, exh.cases, fuzz.pass, fuzz.cases,
        grid.cases + exh.cases + fuzz.cases, grid.fail + exh.fail + fuzz.fail,
        (grid.fail + exh.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (grid.fail + exh.fail + fuzz.fail) ? 1 : 0;
}

// ---- Units 79-80: sub_15fb1 / sub_15fbe (Y-axis object search) ------------
// Shared core loc_15fc9: probe Y = self.Y0-1 (15fb1) / self.Y1+1 (15fbe);
// same slot/filter protocol as loc_15dfd; Y gates JL + DEC/JGE on the target
// span; X overlap gates are the JS CLASS (bit 15 of the WRAPPED difference,
// no vel adjust): [di+155D]-[si+1535] and [si+155D]-[di+1535]. Hit: ds:3B2 =
// [s+17DD] full word, ds:3B4 = slot, STC.
int ft_selftest_objscan_y(FtId id, uint32_t seed) {
    FtSynthStats grid, exh, fuzz;
    long diff_budget = 24;
    const uint16_t di = 8, FLT = 0x7000;
    auto run1 = [&](const char* group, FtSynthStats& st) {
        ft_fill_tail(g_synth_in);
        memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
        int cf = (id == FT_SUB_15FB1)
                 ? v2_fntest_call_sub_15fb1(g_scratch, FLT, di)
                 : v2_fntest_call_sub_15fbe(g_scratch, FLT, di);
        FtRegs in{}; in.si = FLT; in.di = di;
        ft_synth_case_regs(id, in, (uint16_t)cf, 7, group, st, diff_budget);
    };
    // Self spans picked so probe==0x0105 in both twins (Y0=0x0106 / Y1=0x0104).
    const uint16_t SY0 = 0x0106, SY1 = 0x0104;
    struct GC { uint16_t alive, type, ty0, ty1, tx0, tx1, s_x0, s_x1, tend, cur; };
    static const GC G[] = {
        // basic hit
        { 1, 0x0030, 0x0100, 0x0110, 0x0100, 0x0110, 0x0100, 0x0110, 4, 8 },
        // dead / self-skip / filter JB / filter walk-to-terminator
        { 0, 0x0030, 0x0100, 0x0110, 0x0100, 0x0110, 0x0100, 0x0110, 4, 8 },
        { 1, 0x0030, 0x0100, 0x0110, 0x0100, 0x0110, 0x0100, 0x0110, 4, 2 },
        { 1, 0x0010, 0x0100, 0x0110, 0x0100, 0x0110, 0x0100, 0x0110, 4, 8 },
        { 1, 0x0031, 0x0100, 0x0110, 0x0100, 0x0110, 0x0100, 0x0110, 4, 8 },
        // Y JL edge reject / pass, DEC/JGE edge
        { 1, 0x0030, 0x0106, 0x0110, 0x0100, 0x0110, 0x0100, 0x0110, 4, 8 },
        { 1, 0x0030, 0x0105, 0x0110, 0x0100, 0x0110, 0x0100, 0x0110, 4, 8 },
        { 1, 0x0030, 0x0100, 0x0104, 0x0100, 0x0110, 0x0100, 0x0110, 4, 8 },
        // X JS gate 1: self.X1 - target.X0 wraps bit15 SET (true diff +32768!)
        //   self.X1=0x7FFF, target.X0=0xFFFF: wrapped 0x8000 → JS skips
        //   (JL would NOT skip — the divergence point of the JS class)
        { 1, 0x0030, 0x0100, 0x0110, 0xFFFF, 0x0010, 0x0100, 0x7FFF, 4, 8 },
        // X JS gate 1 mirror: bit15 CLEAR via wrap (self.X1=0x8000, t.X0=1)
        { 1, 0x0030, 0x0100, 0x0110, 0x0001, 0x0110, 0x0100, 0x8000, 4, 8 },
        // X JS gate 2: target.X1 - self.X0 bit15 set
        { 1, 0x0030, 0x0100, 0x0110, 0x0100, 0x7FFF, 0xFFFF, 0x0010, 4, 8 },
        // touching spans (diff 0 → bit15 clear → pass)
        { 1, 0x0030, 0x0100, 0x0110, 0x0110, 0x0110, 0x0110, 0x0110, 4, 8 },
        // full-word 0x3B2, empty table
        { 1, 0x1230, 0x0100, 0x0110, 0x0100, 0x0110, 0x0100, 0x0110, 4, 8 },
        { 1, 0x0030, 0x0100, 0x0110, 0x0100, 0x0110, 0x0100, 0x0110, 0, 8 },
    };
    for (auto& g : G) {
        memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
        ft_wr16(g_synth_in, 0x372, g.tend);
        ft_wr16(g_synth_in, 0x42, g.cur);
        ft_wr16(g_synth_in, 0x34, 0xBBBB); ft_wr16(g_synth_in, 0x36, 0xBBBB);
        ft_wr16(g_synth_in, 0x38, 0xBBBB); ft_wr16(g_synth_in, 0x3A, 0xBBBB);
        ft_wr16(g_synth_in, 0x3B2, 0xBBBB); ft_wr16(g_synth_in, 0x3B4, 0xBBBB);
        g_synth_in[(uint16_t)(FLT - LUT_SCAN_FILTER)]     = 0x30;
        g_synth_in[(uint16_t)(FLT - LUT_SCAN_FILTER + 1)] = 0xFF;
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_Y0), SY0);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_Y1), SY1);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_X0), g.s_x0);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_X1), g.s_x1);
        ft_objscan_target(2, g.alive, g.type, g.tx0, g.tx1, g.ty0, g.ty1);
        // ft_objscan_target writes (x0,x1,y0,y1) in that order — fix Y fields:
        ft_wr16(g_synth_in, (uint16_t)(2 + OBJ_BBOX_Y0), g.ty0);
        ft_wr16(g_synth_in, (uint16_t)(2 + OBJ_BBOX_Y1), g.ty1);
        ft_wr16(g_synth_in, (uint16_t)(2 + OBJ_BBOX_X0), g.tx0);
        ft_wr16(g_synth_in, (uint16_t)(2 + OBJ_BBOX_X1), g.tx1);
        run1("grid", grid);
    }
    // Exhaustive 1: target.Y0 axis 65536 (probe fixed 0x0105, X pass).
    // Exhaustive 2: target.X0 axis 65536 (Y pass; sweeps the JS gate-1 wrap
    // point across the whole 16-bit ring with self.X1=0x0110).
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t x = 0; x <= 0xFFFF; x++) {
            memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
            ft_wr16(g_synth_in, 0x372, 4);
            ft_wr16(g_synth_in, 0x42, 8);
            ft_wr16(g_synth_in, 0x34, 0xBBBB); ft_wr16(g_synth_in, 0x36, 0xBBBB);
            ft_wr16(g_synth_in, 0x38, 0xBBBB); ft_wr16(g_synth_in, 0x3A, 0xBBBB);
            ft_wr16(g_synth_in, 0x3B2, 0xBBBB); ft_wr16(g_synth_in, 0x3B4, 0xBBBB);
            g_synth_in[(uint16_t)(FLT - LUT_SCAN_FILTER)]     = 0x30;
            g_synth_in[(uint16_t)(FLT - LUT_SCAN_FILTER + 1)] = 0xFF;
            ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_Y0), SY0);
            ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_Y1), SY1);
            ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_X0), 0x0100);
            ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_X1), 0x0110);
            if (pass == 0) {
                ft_objscan_target(2, 1, 0x0030, 0x0100, 0x0110, (uint16_t)x, 0x0110);
            } else {
                ft_objscan_target(2, 1, 0x0030, (uint16_t)x, 0x0110, 0x0100, 0x0110);
            }
            run1("exh", exh);
        }
    }
    FtRng rng(seed);
    for (int i = 0; i < 20000; i++) {
        memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
        ft_wr16(g_synth_in, 0x372, (uint16_t)((rng.next() % 5) * 2));
        ft_wr16(g_synth_in, 0x42, (uint16_t)((rng.next() % 6) * 2));
        ft_wr16(g_synth_in, 0x34, 0xBBBB); ft_wr16(g_synth_in, 0x36, 0xBBBB);
        ft_wr16(g_synth_in, 0x38, 0xBBBB); ft_wr16(g_synth_in, 0x3A, 0xBBBB);
        ft_wr16(g_synth_in, 0x3B2, 0xBBBB); ft_wr16(g_synth_in, 0x3B4, 0xBBBB);
        g_synth_in[(uint16_t)(FLT - LUT_SCAN_FILTER)]     = 0x30;
        g_synth_in[(uint16_t)(FLT - LUT_SCAN_FILTER + 1)] = 0xFF;
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_Y0), rng.w());
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_Y1), rng.w());
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_X0), rng.w());
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_X1), rng.w());
        for (uint16_t slot = 0; slot < 8; slot += 2)
            ft_objscan_target(slot, (uint16_t)(rng.next() & 1),
                              (uint16_t)(rng.w() & 0x0FFF ? rng.w() : 0x0030),
                              rng.w(), rng.w(), rng.w(), rng.w());
        run1("fuzz", fuzz);
    }
    fprintf(stderr,
        "FNSELFTEST-SUMMARY[%s]: grid %ld/%ld, exh %ld/%ld, fuzz %ld/%ld — total cases=%ld fail=%ld%s\n",
        g_name[id], grid.pass, grid.cases, exh.pass, exh.cases, fuzz.pass, fuzz.cases,
        grid.cases + exh.cases + fuzz.cases, grid.fail + exh.fail + fuzz.fail,
        (grid.fail + exh.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (grid.fail + exh.fail + fuzz.fail) ? 1 : 0;
}

// ---- Unit 81: sub_1603e (flip-aware single-point object search) -----------
// Own prologue: ds:34=filter; probe X = self.X0-1 (flags&0x40) / self.X1+1;
// ds:36=probe X; ds:38 = self.Y1+1. Slot do-while loop + filter chain; gates
// on the TARGET span only (X: JL + DEC/JGE vs [s+1535]/[s+155D]; Y: JL +
// DEC/JGE vs [s+14E5]/[s+150D]). Hit: ds:3B2=[s+17DD] full word, 3B4, STC.
int ft_selftest_probe_1603e(uint32_t seed) {
    FtSynthStats grid, exh, fuzz;
    long diff_budget = 24;
    const uint16_t di = 8, FLT = 0x7000;
    auto ctx = [&](uint16_t flags, uint16_t sx0, uint16_t sx1, uint16_t sy1,
                   uint16_t tend, uint16_t cur) {
        memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
        ft_wr16(g_synth_in, 0x372, tend);
        ft_wr16(g_synth_in, 0x42, cur);
        ft_wr16(g_synth_in, 0x34, 0xBBBB); ft_wr16(g_synth_in, 0x36, 0xBBBB);
        ft_wr16(g_synth_in, 0x38, 0xBBBB); ft_wr16(g_synth_in, 0x3A, 0xBBBB);
        ft_wr16(g_synth_in, 0x3B2, 0xBBBB); ft_wr16(g_synth_in, 0x3B4, 0xBBBB);
        g_synth_in[(uint16_t)(FLT - LUT_SCAN_FILTER)]     = 0x30;
        g_synth_in[(uint16_t)(FLT - LUT_SCAN_FILTER + 1)] = 0xFF;
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_FLAGS),   flags);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_X0), sx0);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_X1), sx1);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_Y1), sy1);
    };
    auto run1 = [&](const char* group, FtSynthStats& st) {
        ft_fill_tail(g_synth_in);
        memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
        int cf = v2_fntest_call_sub_1603e(g_scratch, FLT, di);
        FtRegs in{}; in.si = FLT; in.di = di;
        ft_synth_case_regs(FT_SUB_1603E, in, (uint16_t)cf, 7, group, st, diff_budget);
    };
    // flip=0: probe = X1+1 = 0x0105 (X1=0x0104); flip=0x40: probe = X0-1 =
    // 0x0105 (X0=0x0106). Y probe = Y1+1 = 0x0105 (Y1=0x0104).
    struct GC { uint16_t flags, alive, type, tx0, tx1, ty0, ty1, tend, cur; };
    static const GC G[] = {
        { 0x0000, 1, 0x0030, 0x0100, 0x0110, 0x0100, 0x0110, 4, 8 },  // hit, no flip
        { 0x0040, 1, 0x0030, 0x0100, 0x0110, 0x0100, 0x0110, 4, 8 },  // hit, flip
        { 0x0000, 0, 0x0030, 0x0100, 0x0110, 0x0100, 0x0110, 4, 8 },  // dead
        { 0x0000, 1, 0x0030, 0x0100, 0x0110, 0x0100, 0x0110, 4, 2 },  // self-skip
        { 0x0000, 1, 0x0010, 0x0100, 0x0110, 0x0100, 0x0110, 4, 8 },  // filter JB
        { 0x0000, 1, 0x0031, 0x0100, 0x0110, 0x0100, 0x0110, 4, 8 },  // filter walk
        { 0x0000, 1, 0x0030, 0x0106, 0x0110, 0x0100, 0x0110, 4, 8 },  // X JL reject
        { 0x0000, 1, 0x0030, 0x0105, 0x0110, 0x0100, 0x0110, 4, 8 },  // X JL edge pass
        { 0x0000, 1, 0x0030, 0x0100, 0x0104, 0x0100, 0x0110, 4, 8 },  // X JGE reject
        { 0x0000, 1, 0x0030, 0x0100, 0x0110, 0x0106, 0x0110, 4, 8 },  // Y JL reject
        { 0x0000, 1, 0x0030, 0x0100, 0x0110, 0x0105, 0x0110, 4, 8 },  // Y JL edge pass
        { 0x0000, 1, 0x0030, 0x0100, 0x0110, 0x0100, 0x0104, 4, 8 },  // Y JGE reject
        { 0x0000, 1, 0x1230, 0x0100, 0x0110, 0x0100, 0x0110, 4, 8 },  // word 0x3B2
        { 0x0000, 1, 0x0030, 0x0100, 0x0110, 0x0100, 0x0110, 0, 8 },  // empty table
    };
    for (auto& g : G) {
        ctx(g.flags, 0x0106, 0x0104, 0x0104, g.tend, g.cur);
        ft_objscan_target(2, g.alive, g.type, g.tx0, g.tx1, g.ty0, g.ty1);
        run1("grid", grid);
    }
    // probe wrap: flip=0, X1=0xFFFF → probe 0; flip=0x40, X0=0 → probe 0xFFFF
    ctx(0x0000, 0x0106, 0xFFFF, 0x0104, 4, 8);
    ft_objscan_target(2, 1, 0x0030, 0xFFF0, 0x0010, 0x0100, 0x0110);
    run1("grid", grid);
    ctx(0x0040, 0x0000, 0x0104, 0x0104, 4, 8);
    ft_objscan_target(2, 1, 0x0030, 0xFFF0, 0x0010, 0x0100, 0x0110);
    run1("grid", grid);
    // Exhaustive: target.X0 axis + target.Y0 axis (65536 each), probe fixed.
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t x = 0; x <= 0xFFFF; x++) {
            ctx(0x0000, 0x0106, 0x0104, 0x0104, 4, 8);
            if (pass == 0)
                ft_objscan_target(2, 1, 0x0030, (uint16_t)x, 0x0110, 0x0100, 0x0110);
            else
                ft_objscan_target(2, 1, 0x0030, 0x0100, 0x0110, (uint16_t)x, 0x0110);
            run1("exh", exh);
        }
    }
    FtRng rng(seed);
    for (int i = 0; i < 20000; i++) {
        ctx((uint16_t)(rng.next() & 1 ? 0x40 : 0), rng.w(), rng.w(), rng.w(),
            (uint16_t)((rng.next() % 5) * 2), (uint16_t)((rng.next() % 6) * 2));
        for (uint16_t slot = 0; slot < 8; slot += 2)
            ft_objscan_target(slot, (uint16_t)(rng.next() & 1),
                              (uint16_t)(rng.w() & 0x0FFF ? rng.w() : 0x0030),
                              rng.w(), rng.w(), rng.w(), rng.w());
        run1("fuzz", fuzz);
    }
    fprintf(stderr,
        "FNSELFTEST-SUMMARY[sub_1603e]: grid %ld/%ld, exh %ld/%ld, fuzz %ld/%ld — total cases=%ld fail=%ld%s\n",
        grid.pass, grid.cases, exh.pass, exh.cases, fuzz.pass, fuzz.cases,
        grid.cases + exh.cases + fuzz.cases, grid.fail + exh.fail + fuzz.fail,
        (grid.fail + exh.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (grid.fail + exh.fail + fuzz.fail) ? 1 : 0;
}

// ---- Units 82-84: sub_160cf / sub_15ae9 / sub_1589b (at-pos probes) -------
// Probe point = ds:0x6C / ds:0x6E. sub_15ae9: single tile at (x>>4, y>>4) via
// sub_14199, filter chain; hit writes ds:0x3B2 = type AND 0xFF ONLY (no 3B4).
// sub_160cf: object scan (34/36/38 prologue, do-while slots, X/Y JL+DEC/JGE
// gates on the target span, hit 3B2 word + 3B4). sub_1589b: 3B4=0xFFFF, tile
// probe first (JC -> ret), else object probe. v2 which-map: 5/6/7.
int ft_selftest_atpos(FtId id, uint32_t seed) {
    FtSynthStats grid, exh, fuzz;
    long diff_budget = 24;
    const uint16_t FLT = 0x7000;
    int which = (id == FT_SUB_15AE9) ? 5 : (id == FT_SUB_160CF) ? 6 : 7;
    // Tile zone: 8x8 map, all tiles type 0 except (1,1)=0x30 (bits 15..10).
    auto ctx = [&](uint16_t px, uint16_t py, uint16_t tile_type,
                   uint16_t tend, uint16_t cur) {
        memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
        ft_wr16(g_synth_in, 0x372, tend);
        ft_wr16(g_synth_in, 0x42, cur);
        ft_wr16(g_synth_in, 0x6C, px);
        ft_wr16(g_synth_in, 0x6E, py);
        ft_wr16(g_synth_in, 0x34, 0xBBBB); ft_wr16(g_synth_in, 0x36, 0xBBBB);
        ft_wr16(g_synth_in, 0x38, 0xBBBB); ft_wr16(g_synth_in, 0x3A, 0xBBBB);
        ft_wr16(g_synth_in, 0x3B2, 0xBBBB); ft_wr16(g_synth_in, 0x3B4, 0xBBBB);
        g_synth_in[(uint16_t)(FLT - LUT_SCAN_FILTER)]     = 0x30;
        g_synth_in[(uint16_t)(FLT - LUT_SCAN_FILTER + 1)] = 0xFF;
        for (uint32_t a = 0x2E5C; a <= 0x2E7C; a += 2) ft_wr16(g_synth_in, a, 0);
        ft_wr16(g_synth_in, DS_SEG_TILEMAP, FT_VM_TESTSEG);
        ft_wr16(g_synth_in, 0x25DC, 8);
        ft_wr16(g_synth_in, 0x25DE, 8);
        for (int y = 0; y < 8; y++)
            ft_wr16(g_synth_in, (uint16_t)(y * 2 - LUT_ROW_BASE), (uint16_t)(y * 16));
        memset(g_vm_es_in, 0, sizeof(g_vm_es_in));
        uint16_t tw = (uint16_t)(tile_type << 10);
        g_vm_es_in[(1 * 8 + 1) * 2]     = (uint8_t)(tw & 0xFF);
        g_vm_es_in[(1 * 8 + 1) * 2 + 1] = (uint8_t)(tw >> 8);
    };
    auto run1 = [&](const char* group, FtSynthStats& st) {
        ft_fill_tail(g_synth_in);
        uint8_t* zone = (uint8_t*)v2_fntest_m2c_base() + (uint32_t)FT_VM_TESTSEG * 16;
        memcpy(zone, g_vm_es_in, FT_VM_ZONE);
        memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
        int cf = v2_fntest_call_search(g_scratch, which, FLT, 8);
        memcpy(zone, g_vm_es_in, FT_VM_ZONE);   // zone read-only for this family
        FtRegs in{}; in.si = FLT;
        ft_synth_case_regs(id, in, (uint16_t)cf, 7, group, st, diff_budget);
    };
    // grid: tile hit at (0x10..0x1F, 0x10..0x1F) → type 0x30 in cell (1,1);
    // tile miss (type 0 cell); obj hit/miss combos; probe edges.
    struct GC { uint16_t px, py, tt, alive, otype, tx0, tx1, ty0, ty1, tend, cur; };
    static const GC G[] = {
        { 0x0015, 0x0015, 0x30, 0, 0x0030, 0x0100, 0x0110, 0x0100, 0x0110, 4, 8 }, // tile hit
        { 0x0015, 0x0015, 0x00, 0, 0x0030, 0x0100, 0x0110, 0x0100, 0x0110, 4, 8 }, // tile miss, no obj
        { 0x0105, 0x0105, 0x00, 1, 0x0030, 0x0100, 0x0110, 0x0100, 0x0110, 4, 8 }, // obj hit
        { 0x0105, 0x0105, 0x00, 1, 0x0010, 0x0100, 0x0110, 0x0100, 0x0110, 4, 8 }, // obj filter JB
        { 0x0105, 0x0105, 0x00, 1, 0x0030, 0x0106, 0x0110, 0x0100, 0x0110, 4, 8 }, // X JL reject
        { 0x0105, 0x0105, 0x00, 1, 0x0030, 0x0100, 0x0105, 0x0100, 0x0110, 4, 8 }, // X JGE reject
        { 0x0105, 0x0105, 0x00, 1, 0x0030, 0x0100, 0x0110, 0x0106, 0x0110, 4, 8 }, // Y JL reject
        { 0x0105, 0x0105, 0x00, 1, 0x0030, 0x0100, 0x0110, 0x0100, 0x0105, 4, 8 }, // Y JGE reject
        { 0x0105, 0x0105, 0x00, 1, 0x1230, 0x0100, 0x0110, 0x0100, 0x0110, 4, 8 }, // word 3B2 (obj)
        { 0x0015, 0x0015, 0x30, 1, 0x1230, 0x0000, 0xFFFF, 0x0000, 0xFFFF, 4, 8 }, // tile hit shadows obj (1589b order)
        { 0x0105, 0x0105, 0x00, 1, 0x0030, 0x0100, 0x0110, 0x0100, 0x0110, 0, 8 }, // empty table
        { 0x0000, 0x0000, 0x00, 1, 0x0030, 0x0000, 0x0010, 0x0000, 0x0010, 4, 8 }, // probe 0/0 (DEC wrap)
    };
    for (auto& g : G) {
        ctx(g.px, g.py, g.tt, g.tend, g.cur);
        ft_objscan_target(2, g.alive, g.otype, g.tx0, g.tx1, g.ty0, g.ty1);
        run1("grid", grid);
    }
    // Exhaustive: probe-X axis 65536 vs a fixed object span (tile cell empty);
    // second pass: probe-Y axis.
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t x = 0; x <= 0xFFFF; x++) {
            if (pass == 0) ctx((uint16_t)x, 0x0105, 0, 4, 8);
            else           ctx(0x0105, (uint16_t)x, 0, 4, 8);
            ft_objscan_target(2, 1, 0x0030, 0x0100, 0x0110, 0x0100, 0x0110);
            run1("exh", exh);
        }
    }
    FtRng rng(seed);
    for (int i = 0; i < 12000; i++) {
        ctx(rng.w(), rng.w(), (uint16_t)(rng.next() & 1 ? 0x30 : 0),
            (uint16_t)((rng.next() % 5) * 2), (uint16_t)((rng.next() % 6) * 2));
        for (uint16_t slot = 0; slot < 8; slot += 2)
            ft_objscan_target(slot, (uint16_t)(rng.next() & 1),
                              (uint16_t)(rng.w() & 0x0FFF ? rng.w() : 0x0030),
                              rng.w(), rng.w(), rng.w(), rng.w());
        run1("fuzz", fuzz);
    }
    fprintf(stderr,
        "FNSELFTEST-SUMMARY[%s]: grid %ld/%ld, exh %ld/%ld, fuzz %ld/%ld — total cases=%ld fail=%ld%s\n",
        g_name[id], grid.pass, grid.cases, exh.pass, exh.cases, fuzz.pass, fuzz.cases,
        grid.cases + exh.cases + fuzz.cases, grid.fail + exh.fail + fuzz.fail,
        (grid.fail + exh.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (grid.fail + exh.fail + fuzz.fail) ? 1 : 0;
}

// ---- Units 85-89: tile-walk entries 159c6/159d3/159df/15a57/15ac4 ---------
// X-walk core loc_159f6 (entries: X0-1 / X0 / X1+1): slope pre-probe at
// (WORLD_X, clamp(Y1-vel)) >= 0x30 -> CLC; walk probe column from
// clamp0(Y0-vel) down to clamp0(Y1-vel) step 0x10 with last-step clamp;
// filter chain per tile; hit ds:3B2 = type byte. Y-walk core loc_15a70
// (15a57: Y0-1 entry) walks X across [X0..X1]. 15ac4: flip single point at
// (flip-X, Y1+1). Exhaustive completeness lives in parent units 20-24
// (full 158xx sweeps); these direct units isolate every branch per entry.
int ft_selftest_tilewalk(FtId id, uint32_t seed) {
    FtSynthStats grid, fuzz;
    long diff_budget = 24;
    const uint16_t di = 8, FLT = 0x7000;
    int which = 8 + (int)(id - FT_SUB_159C6);
    // 16x16 map; type-0x30 stripe on tile column 4 (x 0x40..0x4F) and tile
    // row 4 (y 0x40..0x4F) for the Y-walk twin; slope tile placed on demand.
    auto ctx = [&](uint16_t sx0, uint16_t sx1, uint16_t sy0, uint16_t sy1,
                   uint16_t vel, uint16_t flags, uint16_t wx, int slope_cell) {
        memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
        ft_wr16(g_synth_in, 0x372, 0);
        ft_wr16(g_synth_in, 0x42, 0xFFFE);
        ft_wr16(g_synth_in, 0x34, 0xBBBB); ft_wr16(g_synth_in, 0x36, 0xBBBB);
        ft_wr16(g_synth_in, 0x38, 0xBBBB); ft_wr16(g_synth_in, 0x3A, 0xBBBB);
        ft_wr16(g_synth_in, 0x3B2, 0xBBBB); ft_wr16(g_synth_in, 0x3B4, 0xBBBB);
        g_synth_in[(uint16_t)(FLT - LUT_SCAN_FILTER)]     = 0x30;
        g_synth_in[(uint16_t)(FLT - LUT_SCAN_FILTER + 1)] = 0xFF;
        for (uint32_t a = 0x2E5C; a <= 0x2E7C; a += 2) ft_wr16(g_synth_in, a, 0);
        ft_wr16(g_synth_in, DS_SEG_TILEMAP, FT_VM_TESTSEG);
        ft_wr16(g_synth_in, 0x25DC, 16);
        ft_wr16(g_synth_in, 0x25DE, 16);
        for (int y = 0; y < 16; y++)
            ft_wr16(g_synth_in, (uint16_t)(y * 2 - LUT_ROW_BASE), (uint16_t)(y * 32));
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_X0), sx0);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_X1), sx1);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_Y0), sy0);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_Y1), sy1);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_VEL_Y),   vel);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_FLAGS),   flags);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_WORLD_X), wx);
        memset(g_vm_es_in, 0, sizeof(g_vm_es_in));
        for (int y = 0; y < 16; y++) {   // stripe column 4
            uint16_t tw = (uint16_t)(0x30 << 10);
            g_vm_es_in[(y * 16 + 4) * 2]     = (uint8_t)(tw & 0xFF);
            g_vm_es_in[(y * 16 + 4) * 2 + 1] = (uint8_t)(tw >> 8);
        }
        for (int x = 0; x < 16; x++) {   // stripe row 4
            uint16_t tw = (uint16_t)(0x30 << 10);
            g_vm_es_in[(4 * 16 + x) * 2]     = (uint8_t)(tw & 0xFF);
            g_vm_es_in[(4 * 16 + x) * 2 + 1] = (uint8_t)(tw >> 8);
        }
        if (slope_cell >= 0) {           // slope tile >= 0x30 for the pre-probe
            uint16_t tw = (uint16_t)(0x31 << 10);
            g_vm_es_in[slope_cell * 2]     = (uint8_t)(tw & 0xFF);
            g_vm_es_in[slope_cell * 2 + 1] = (uint8_t)(tw >> 8);
        }
    };
    auto run1 = [&](const char* group, FtSynthStats& st) {
        ft_fill_tail(g_synth_in);
        uint8_t* zone = (uint8_t*)v2_fntest_m2c_base() + (uint32_t)FT_VM_TESTSEG * 16;
        memcpy(zone, g_vm_es_in, FT_VM_ZONE);
        memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
        int cf = v2_fntest_call_search(g_scratch, which, FLT, di);
        memcpy(zone, g_vm_es_in, FT_VM_ZONE);
        FtRegs in{}; in.si = FLT; in.di = di;
        ft_synth_case_regs(id, in, (uint16_t)cf, 7, group, st, diff_budget);
    };
    // Grid: probe on/off the stripe; hit at first/mid/last walk step; walk
    // clamp (start past top: single clamped attempt); slope pre-probe kill;
    // vel shifting the window; zero window; flip for 15ac4.
    struct GC { uint16_t sx0, sx1, sy0, sy1, vel, flags, wx; int slope; };
    static const GC G[] = {
        { 0x0041, 0x0030, 0x0080, 0x00A0, 0x0000, 0x0000, 0x0090, -1 },  // 159c6: probe 0x40 on stripe, hit mid-walk
        { 0x0051, 0x0040, 0x0080, 0x00A0, 0x0000, 0x0000, 0x0090, -1 },  // probe off stripe (0x50) → CLC walk-out
        { 0x0041, 0x0030, 0x00A0, 0x0080, 0x0000, 0x0000, 0x0090, -1 },  // start below top → clamp path
        { 0x0041, 0x0030, 0x0080, 0x00A0, 0x0000, 0x0000, 0x0048, 8*16+4 }, // slope pre-probe kills (X-walk twins)
        { 0x0041, 0x0030, 0x0080, 0x00A0, 0x0030, 0x0000, 0x0090, -1 },  // vel shifts window up
        { 0x0041, 0x0030, 0x0010, 0x0010, 0x0100, 0x0000, 0x0090, -1 },  // clamp0 both (vel > Y)
        { 0x0041, 0x0030, 0x0080, 0x0080, 0x0000, 0x0000, 0x0090, -1 },  // single-step window
        { 0x0041, 0x0040, 0x0080, 0x0041, 0x0000, 0x0000, 0x0090, -1 },  // 15a57: X-range over stripe row 4
        { 0x0041, 0x0040, 0x0080, 0x0041, 0x0000, 0x0040, 0x0090, -1 },  // flip set (15ac4 X0-1 path)
        { 0x8000, 0x8000, 0x8000, 0x8010, 0x7FFF, 0x0000, 0x0090, -1 },  // SUB-wrap class on clamps
    };
    for (auto& g : G) { ctx(g.sx0, g.sx1, g.sy0, g.sy1, g.vel, g.flags, g.wx, g.slope); run1("grid", grid); }
    FtRng rng(seed);
    for (int i = 0; i < 4000; i++) {
        // bounded ranges keep the oracle walk short (<= 32 steps)
        ctx((uint16_t)(rng.w() & 0x1FF), (uint16_t)(rng.w() & 0x1FF),
            (uint16_t)(rng.w() & 0x1FF), (uint16_t)(rng.w() & 0x1FF),
            (uint16_t)((rng.w() & 0x3F) - 0x20), (uint16_t)(rng.next() & 1 ? 0x40 : 0),
            (uint16_t)(rng.w() & 0x1FF), (rng.next() & 3) == 0 ? (int)(rng.next() % 256) : -1);
        run1("fuzz", fuzz);
    }
    fprintf(stderr,
        "FNSELFTEST-SUMMARY[%s]: grid %ld/%ld, fuzz %ld/%ld — total cases=%ld fail=%ld%s\n",
        g_name[id], grid.pass, grid.cases, fuzz.pass, fuzz.cases,
        grid.cases + fuzz.cases, grid.fail + fuzz.fail,
        (grid.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (grid.fail + fuzz.fail) ? 1 : 0;
}

// ---- Units 90-91: sub_158f5 (X-dir dispatcher) / sub_1592d (X snap) -------
// 158f5: WORLD_X vs prev-X: JZ -> CLC with ax=WORLD_X; JL -> X-walk at X0,
// ax=1; JG -> X-walk at X1 (loc_159EC, no INC), ax=0. Dual channel (CF+AX)
// needs a custom checker. 1592d: the X twin of unit 1's Y-snap.
int ft_selftest_sub_158f5(uint32_t seed) {
    FtSynthStats grid, fuzz;
    long diff_budget = 24;
    const uint16_t di = 8, FLT = 0x7000;
    auto ctx = [&](uint16_t wx, uint16_t px, uint16_t sx0, uint16_t sx1,
                   uint16_t sy0, uint16_t sy1, uint16_t vel) {
        memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
        ft_wr16(g_synth_in, 0x34, 0xBBBB); ft_wr16(g_synth_in, 0x36, 0xBBBB);
        ft_wr16(g_synth_in, 0x3B2, 0xBBBB);
        g_synth_in[(uint16_t)(FLT - LUT_SCAN_FILTER)]     = 0x30;
        g_synth_in[(uint16_t)(FLT - LUT_SCAN_FILTER + 1)] = 0xFF;
        for (uint32_t a = 0x2E5C; a <= 0x2E7C; a += 2) ft_wr16(g_synth_in, a, 0);
        ft_wr16(g_synth_in, DS_SEG_TILEMAP, FT_VM_TESTSEG);
        ft_wr16(g_synth_in, 0x25DC, 16);
        ft_wr16(g_synth_in, 0x25DE, 16);
        for (int y = 0; y < 16; y++)
            ft_wr16(g_synth_in, (uint16_t)(y * 2 - LUT_ROW_BASE), (uint16_t)(y * 32));
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_WORLD_X), wx);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_X_PREV),  px);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_X0), sx0);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_X1), sx1);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_Y0), sy0);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_Y1), sy1);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_VEL_Y),   vel);
        memset(g_vm_es_in, 0, sizeof(g_vm_es_in));
        for (int y = 0; y < 16; y++) {
            uint16_t tw = (uint16_t)(0x30 << 10);
            g_vm_es_in[(y * 16 + 4) * 2]     = (uint8_t)(tw & 0xFF);
            g_vm_es_in[(y * 16 + 4) * 2 + 1] = (uint8_t)(tw >> 8);
        }
    };
    auto run1 = [&](const char* group, FtSynthStats& st) {
        ft_fill_tail(g_synth_in);
        uint8_t* zone = (uint8_t*)v2_fntest_m2c_base() + (uint32_t)FT_VM_TESTSEG * 16;
        memcpy(zone, g_vm_es_in, FT_VM_ZONE);
        memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
        int32_t packed = v2_fntest_call_sub_158f5(g_scratch, FLT, di);
        memcpy(zone, g_vm_es_in, FT_VM_ZONE);
        int cf_v2 = (int)(packed & 1);
        uint16_t ax_v2 = (uint16_t)((uint32_t)packed >> 1);
        st.cases++;
        memcpy(g_synth_orig, g_synth_in, sizeof(g_synth_orig));
        uint16_t regs[8] = { 0, 0, 0, 0, FLT, di, 0, 0 };
        long esc0 = ft_ub_marks();
        v2_fntest_orig_isolated(v2_fntest_orig_fnptr(FT_SUB_158F5), g_synth_orig, regs);
        memcpy(zone, g_vm_es_in, FT_VM_ZONE);
        if (ft_ub_marks() != esc0) { st.cases--; return; }
        long diffs = 0;
        if ((regs[7] & 1) != (uint16_t)cf_v2) {
            if (diff_budget-- > 0)
                fprintf(stderr, "FNSELFTEST-DIFF[sub_158f5 %s]: CF orig=%d v2=%d\n",
                        group, regs[7] & 1, cf_v2);
            diffs++;
        }
        if (regs[0] != ax_v2) {
            if (diff_budget-- > 0)
                fprintf(stderr, "FNSELFTEST-DIFF[sub_158f5 %s]: AX orig=%04X v2=%04X\n",
                        group, regs[0], ax_v2);
            diffs++;
        }
        for (uint32_t a = 0; a < 0x10000; a++) {
            if (g_scratch[a] == g_synth_orig[a]) continue;
            if (v2_fntest_ds_skip(a)) continue;
            if (diff_budget-- > 0)
                fprintf(stderr, "FNSELFTEST-DIFF[sub_158f5 %s]: addr=%04X orig=%02X v2=%02X\n",
                        group, a, g_synth_orig[a], g_scratch[a]);
            diffs++;
        }
        if (diffs) st.fail++; else st.pass++;
    };
    // JZ path (ax = WORLD_X, even a wild value)
    ctx(0x0123, 0x0123, 0x0041, 0x0050, 0x0080, 0x00A0, 0); run1("grid", grid);
    ctx(0x8000, 0x8000, 0x0041, 0x0050, 0x0080, 0x00A0, 0); run1("grid", grid);
    // moved left (cur<old): probe X0 on/off stripe
    ctx(0x0080, 0x0090, 0x0041, 0x0050, 0x0080, 0x00A0, 0); run1("grid", grid);
    ctx(0x0080, 0x0090, 0x0051, 0x0060, 0x0080, 0x00A0, 0); run1("grid", grid);
    // moved right (cur>old): probe X1 (no INC!) on/off stripe
    ctx(0x0090, 0x0080, 0x0030, 0x0041, 0x0080, 0x00A0, 0); run1("grid", grid);
    ctx(0x0090, 0x0080, 0x0030, 0x0051, 0x0080, 0x00A0, 0); run1("grid", grid);
    // signed compare edges of cur vs old (JL/JG true-diff class)
    ctx(0x7FFF, 0x8000, 0x0041, 0x0050, 0x0080, 0x00A0, 0); run1("grid", grid);
    ctx(0x8000, 0x7FFF, 0x0030, 0x0041, 0x0080, 0x00A0, 0); run1("grid", grid);
    FtRng rng(seed);
    for (int i = 0; i < 4000; i++) {
        ctx(rng.w(), rng.w(), (uint16_t)(rng.w() & 0x1FF), (uint16_t)(rng.w() & 0x1FF),
            (uint16_t)(rng.w() & 0x1FF), (uint16_t)(rng.w() & 0x1FF),
            (uint16_t)((rng.w() & 0x3F) - 0x20));
        run1("fuzz", fuzz);
    }
    fprintf(stderr,
        "FNSELFTEST-SUMMARY[sub_158f5]: grid %ld/%ld, fuzz %ld/%ld — total cases=%ld fail=%ld%s\n",
        grid.pass, grid.cases, fuzz.pass, fuzz.cases, grid.cases + fuzz.cases,
        grid.fail + fuzz.fail, (grid.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (grid.fail + fuzz.fail) ? 1 : 0;
}

int ft_selftest_sub_1592d(uint32_t seed) {
    FtSynthStats grid, exh, fuzz;
    long diff_budget = 24;
    const uint16_t di = 8;
    auto ctx = [&](uint16_t x0, uint16_t x1, uint16_t wx) {
        memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_X0), x0);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_X1), x1);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_WORLD_X), wx);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_FRAC_X), 0xAAAA);   // canary: must become 0
    };
    auto run1 = [&](uint16_t ax, const char* group, FtSynthStats& st) {
        ft_fill_tail(g_synth_in);
        memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
        v2_fntest_call_sub_1592d(g_scratch, ax, di);
        FtRegs in{}; in.ax = ax; in.di = di;
        ft_synth_case_regs(FT_SUB_1592D, in, 0, -1, group, st, diff_budget);
    };
    static const uint16_t B[] = { 0, 1, 0xF, 0x10, 0x11, 0x7FF0, 0x7FFF,
                                  0x8000, 0x800F, 0xFFF0, 0xFFFF };
    for (uint16_t x : B) for (int axv = 0; axv <= 1; axv++) {
        ctx(x, x, 0x0100); run1((uint16_t)axv, "grid", grid);
    }
    for (uint32_t x = 0; x <= 0xFFFF; x++) {       // exhaustive both entries
        ctx(0x0123, (uint16_t)x, 0x0100); run1(0, "exh", exh);
    }
    for (uint32_t x = 0; x <= 0xFFFF; x++) {
        ctx((uint16_t)x, 0x0123, 0x0100); run1(1, "exh", exh);
    }
    FtRng rng(seed);
    for (int i = 0; i < 20000; i++) {
        ctx(rng.w(), rng.w(), rng.w());
        run1((uint16_t)(rng.next() & 1), "fuzz", fuzz);
    }
    fprintf(stderr,
        "FNSELFTEST-SUMMARY[sub_1592d]: grid %ld/%ld, exh %ld/%ld, fuzz %ld/%ld — total cases=%ld fail=%ld%s\n",
        grid.pass, grid.cases, exh.pass, exh.cases, fuzz.pass, fuzz.cases,
        grid.cases + exh.cases + fuzz.cases, grid.fail + exh.fail + fuzz.fail,
        (grid.fail + exh.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (grid.fail + exh.fail + fuzz.fail) ? 1 : 0;
}

// ---- Units 92-93: sub_15505 (resource deduct) / sub_15517 (vel clear) -----
int ft_selftest_sub_15505(uint32_t seed) {
    FtSynthStats exh, fuzz;
    long diff_budget = 24;
    const uint16_t si = 4, di = 8;
    auto run1 = [&](uint16_t a, uint16_t b, const char* group, FtSynthStats& st) {
        memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
        ft_wr16(g_synth_in, (uint16_t)(si + OBJ_RES_HANDLE), a);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_RES_COST),   b);
        ft_fill_tail(g_synth_in);
        memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
        v2_fntest_call_sub_15505(g_scratch, si, di);
        FtRegs in{}; in.si = si; in.di = di;
        ft_synth_case_regs(FT_SUB_15505, in, 0, -1, group, st, diff_budget);
    };
    // exhaustive a-axis at borrow-edge costs, plus the full b-axis at fixed a
    static const uint16_t COSTS[] = { 0, 1, 0x8000, 0xFFFF };
    for (uint16_t b : COSTS)
        for (uint32_t a = 0; a <= 0xFFFF; a += 7)   // step-7 lattice x4 costs
            run1((uint16_t)a, b, "exh", exh);
    for (uint32_t b = 0; b <= 0xFFFF; b++)
        run1(0x8000, (uint16_t)b, "exh", exh);
    FtRng rng(seed);
    for (int i = 0; i < 20000; i++) run1(rng.w(), rng.w(), "fuzz", fuzz);
    fprintf(stderr,
        "FNSELFTEST-SUMMARY[sub_15505]: exh %ld/%ld, fuzz %ld/%ld — total cases=%ld fail=%ld%s\n",
        exh.pass, exh.cases, fuzz.pass, fuzz.cases, exh.cases + fuzz.cases,
        exh.fail + fuzz.fail, (exh.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (exh.fail + fuzz.fail) ? 1 : 0;
}

int ft_selftest_sub_15517(uint32_t seed) {
    FtSynthStats grid, fuzz;
    long diff_budget = 24;
    auto run1 = [&](uint16_t te, const char* group, FtSynthStats& st) {
        memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
        ft_wr16(g_synth_in, 0x372, te);
        // canaries in the wrap zone hit by the [372]=0 first-slot write
        ft_wr16(g_synth_in, 0x1943, 0xBBBB);
        ft_wr16(g_synth_in, 0x196B, 0xBBBB);
        for (uint16_t s = 0; s < 12; s += 2) {
            ft_wr16(g_synth_in, (uint16_t)(s + OBJ_VEL_X), 0xCCCC);
            ft_wr16(g_synth_in, (uint16_t)(s + OBJ_VEL_Y), 0xCCCC);
        }
        ft_fill_tail(g_synth_in);
        memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
        v2_fntest_call_sub_15517(g_scratch);
        FtRegs in{};
        ft_synth_case_regs(FT_SUB_15517, in, 0, -1, group, st, diff_budget);
    };
    static const uint16_t TE[] = { 0, 2, 4, 8, 12, 1, 3 };   // odd te: wrap lattice
    for (uint16_t te : TE) run1(te, "grid", grid);
    FtRng rng(seed);
    for (int i = 0; i < 200; i++) run1((uint16_t)(rng.next() % 16), "fuzz", fuzz);
    fprintf(stderr,
        "FNSELFTEST-SUMMARY[sub_15517]: grid %ld/%ld, fuzz %ld/%ld — total cases=%ld fail=%ld%s\n",
        grid.pass, grid.cases, fuzz.pass, fuzz.cases, grid.cases + fuzz.cases,
        grid.fail + fuzz.fail, (grid.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (grid.fail + fuzz.fail) ? 1 : 0;
}

// ---- Units 94-95: sub_1555c / sub_15569 (per-object collision-VM pass) ----
// Bytecode carpet in FT_VM_TESTSEG (shared with the oracle via es=[1355]):
// terminator opcode 0x01; PC is NOT written back. Gate cases + a small
// data-op prefix (op 0x11 = resource deduct, DS-only, proven by unit 92).
int ft_selftest_coll_pass(FtId id, uint32_t seed) {
    FtSynthStats grid, fuzz;
    long diff_budget = 24;
    const uint16_t si = 4;
    auto ctx = [&](uint16_t anim_tbl, uint16_t code_seg, uint16_t pc,
                   const uint8_t* code, int code_len) {
        memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
        ft_wr16(g_synth_in, 0x372, 8);
        ft_wr16(g_synth_in, 0x42, 0xBBBB);   // canary: set to si only when the VM runs
        ft_wr16(g_synth_in, 0x38E, 0xBBBB);
        ft_wr16(g_synth_in, (uint16_t)(si + OBJ_COLL_BITS),  0xCCCC);
        ft_wr16(g_synth_in, (uint16_t)(si + OBJ_ANIM_TABLE), anim_tbl);
        ft_wr16(g_synth_in, (uint16_t)(si + OBJ_CODE_SEG),   code_seg);
        ft_wr16(g_synth_in, (uint16_t)(si + OBJ_PC),         pc);
        // op 0x11 operand fields (self res handle / obj[1995] cost)
        ft_wr16(g_synth_in, (uint16_t)(si + OBJ_RES_HANDLE), 0x0123);
        ft_wr16(g_synth_in, (uint16_t)(si + 0x1995), 8);
        ft_wr16(g_synth_in, (uint16_t)(8 + OBJ_RES_COST), 0x0023);
        memset(g_vm_es_in, 0x01, 0x200);     // 0x01 carpet (terminator)
        if (code_len > 0) memcpy(g_vm_es_in + pc, code, (size_t)code_len);
    };
    auto run1 = [&](const char* group, FtSynthStats& st) {
        ft_fill_tail(g_synth_in);
        uint8_t* zone = (uint8_t*)v2_fntest_m2c_base() + (uint32_t)FT_VM_TESTSEG * 16;
        memcpy(zone, g_vm_es_in, FT_VM_ZONE);
        memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
        if (id == FT_SUB_1555C) v2_fntest_call_sub_1555c(g_scratch, si);
        else                    v2_fntest_call_sub_15569(g_scratch, si);
        memcpy(zone, g_vm_es_in, FT_VM_ZONE);
        FtRegs in{}; in.si = si;
        ft_synth_case_regs(id, in, 0, -1, group, st, diff_budget);
    };
    static const uint8_t OP11[] = { 0x11, 0x01 };
    // gates: anim!=FFFF (1555c: only 13F5 clear; 15569: runs anyway),
    // dead (code_seg 0), live minimal carpet, live with op-11 prefix.
    ctx(0x0000, FT_VM_TESTSEG, 0x0100, nullptr, 0); run1("grid", grid);
    ctx(0xFFFF, 0x0000,        0x0100, nullptr, 0); run1("grid", grid);
    ctx(0xFFFF, FT_VM_TESTSEG, 0x0100, nullptr, 0); run1("grid", grid);
    ctx(0xFFFF, FT_VM_TESTSEG, 0x0100, OP11, 2);    run1("grid", grid);
    ctx(0xFFFF, FT_VM_TESTSEG, 0x01FE, nullptr, 0); run1("grid", grid);  // pc at carpet tail
    FtRng rng(seed);
    for (int i = 0; i < 400; i++) {
        uint16_t at = (rng.next() & 1) ? 0xFFFF : rng.w();
        uint16_t cs2 = (rng.next() & 3) ? FT_VM_TESTSEG : 0;
        const uint8_t* cd = (rng.next() & 1) ? OP11 : nullptr;
        ctx(at, cs2, (uint16_t)(0x0100 + (rng.next() % 0xF0)), cd, cd ? 2 : 0);
        run1("fuzz", fuzz);
    }
    fprintf(stderr,
        "FNSELFTEST-SUMMARY[%s]: grid %ld/%ld, fuzz %ld/%ld — total cases=%ld fail=%ld%s\n",
        g_name[id], grid.pass, grid.cases, fuzz.pass, fuzz.cases,
        grid.cases + fuzz.cases, grid.fail + fuzz.fail,
        (grid.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (grid.fail + fuzz.fail) ? 1 : 0;
}

// ---- Units 96-97: sub_15530 / sub_15546 (collision sweeps) ----------------
// 15530: [390]=0xFFFF + slot do-while over sub_15569; 15546: [390]=1 + slot
// do-while over sub_1555c. Two-slot tables exercise the gate mix per slot;
// te=0 exercises the unconditional first slot (#29 class).
int ft_selftest_coll_sweep(FtId id, uint32_t seed) {
    FtSynthStats grid, fuzz;
    long diff_budget = 24;
    auto ctx = [&](uint16_t te, uint16_t at0, uint16_t cs0, uint16_t at1, uint16_t cs1) {
        memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
        ft_wr16(g_synth_in, 0x372, te);
        ft_wr16(g_synth_in, 0x390, 0xBBBB);
        ft_wr16(g_synth_in, 0x42, 0xBBBB);
        ft_wr16(g_synth_in, 0x38E, 0xBBBB);
        ft_wr16(g_synth_in, (uint16_t)(0 + OBJ_COLL_BITS),  0xCCCC);
        ft_wr16(g_synth_in, (uint16_t)(0 + OBJ_ANIM_TABLE), at0);
        ft_wr16(g_synth_in, (uint16_t)(0 + OBJ_CODE_SEG),   cs0);
        ft_wr16(g_synth_in, (uint16_t)(0 + OBJ_PC),         0x0100);
        ft_wr16(g_synth_in, (uint16_t)(2 + OBJ_COLL_BITS),  0xCCCC);
        ft_wr16(g_synth_in, (uint16_t)(2 + OBJ_ANIM_TABLE), at1);
        ft_wr16(g_synth_in, (uint16_t)(2 + OBJ_CODE_SEG),   cs1);
        ft_wr16(g_synth_in, (uint16_t)(2 + OBJ_PC),         0x0110);
        memset(g_vm_es_in, 0x01, 0x200);
    };
    auto run1 = [&](const char* group, FtSynthStats& st) {
        ft_fill_tail(g_synth_in);
        uint8_t* zone = (uint8_t*)v2_fntest_m2c_base() + (uint32_t)FT_VM_TESTSEG * 16;
        memcpy(zone, g_vm_es_in, FT_VM_ZONE);
        memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
        if (id == FT_SUB_15530) v2_fntest_call_sub_15530(g_scratch);
        else                    v2_fntest_call_sub_15546(g_scratch);
        memcpy(zone, g_vm_es_in, FT_VM_ZONE);
        FtRegs in{};
        ft_synth_case_regs(id, in, 0, -1, group, st, diff_budget);
    };
    const uint16_t TS = FT_VM_TESTSEG;
    ctx(4, 0xFFFF, TS, 0xFFFF, TS); run1("grid", grid);   // both run
    ctx(4, 0x0000, TS, 0xFFFF, TS); run1("grid", grid);   // slot0 gated (15546) / runs (15530)
    ctx(4, 0xFFFF, 0,  0xFFFF, TS); run1("grid", grid);   // slot0 dead
    ctx(0, 0xFFFF, TS, 0xFFFF, TS); run1("grid", grid);   // te=0: first slot unconditional
    ctx(2, 0xFFFF, TS, 0xFFFF, TS); run1("grid", grid);   // one slot only
    FtRng rng(seed);
    for (int i = 0; i < 300; i++) {
        ctx((uint16_t)((rng.next() % 4) * 2),
            (rng.next() & 1) ? 0xFFFF : rng.w(), (rng.next() & 3) ? TS : 0,
            (rng.next() & 1) ? 0xFFFF : rng.w(), (rng.next() & 3) ? TS : 0);
        run1("fuzz", fuzz);
    }
    fprintf(stderr,
        "FNSELFTEST-SUMMARY[%s]: grid %ld/%ld, fuzz %ld/%ld — total cases=%ld fail=%ld%s\n",
        g_name[id], grid.pass, grid.cases, fuzz.pass, fuzz.cases,
        grid.cases + fuzz.cases, grid.fail + fuzz.fail,
        (grid.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (grid.fail + fuzz.fail) ? 1 : 0;
}

// ---- Units 98-99: sub_155d6 / sub_156c0 (per-op collision checks) ---------
// Three-state triage on [0x390]: ==0 → consume filter, mask-check
// [di+13F5] & LUT[38E-0x6C34]: hit → sub_16243 partner fetch + [di+1995] +
// STC; >0 → consume + CLC; <0 (JS) → typed slot scan (155d6: byte equality
// vs [15FD]; 156c0: word TEST vs [1625]) with 4 JL bbox gates vs the 34-3A
// scratches; hit → OR bit + sub_16235 stash + CLC (first hit exits).
// Return channel: packed (pc<<1)|CF — checks byte consumption too.
int ft_selftest_coll_check(FtId id, uint32_t seed) {
    FtSynthStats grid, fuzz;
    long diff_budget = 24;
    const uint16_t di = 8, PC0 = 0x0100;
    bool d6 = (id == FT_SUB_155D6);
    auto ctx = [&](uint16_t st390, uint16_t cur, uint16_t bit38e,
                   uint16_t coll_bits, uint16_t filt_val) {
        memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
        ft_wr16(g_synth_in, 0x372, 4);
        ft_wr16(g_synth_in, 0x390, st390);
        ft_wr16(g_synth_in, 0x42, cur);
        ft_wr16(g_synth_in, 0x38E, bit38e);
        ft_wr16(g_synth_in, 0x34, 0xBBBB); ft_wr16(g_synth_in, 0x36, 0xBBBB);
        ft_wr16(g_synth_in, 0x38, 0xBBBB); ft_wr16(g_synth_in, 0x3A, 0xBBBB);
        // bit-mask LUT at [38E-0x6C34]: single bit per index
        ft_wr16(g_synth_in, (uint16_t)(bit38e - 0x6C34), (uint16_t)(1u << (bit38e / 2)));
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_COLL_BITS), coll_bits);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_X0), 0x0100);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_X1), 0x0110);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_Y0), 0x0100);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_Y1), 0x0110);
        // partner table entry for the ==0 hit path (sub_16243 reads it)
        ft_wr16(g_synth_in, (uint16_t)((di << 4) + bit38e + OBJ_COLL_TABLE), 0x0002);
        // slot 2 candidate
        ft_wr16(g_synth_in, (uint16_t)(2 + OBJ_CODE_SEG), 1);
        ft_wr16(g_synth_in, (uint16_t)(2 + (d6 ? OBJ_STATE_IDX : 0x1625)), filt_val);
        ft_wr16(g_synth_in, (uint16_t)(2 + OBJ_BBOX_X0), 0x0100);
        ft_wr16(g_synth_in, (uint16_t)(2 + OBJ_BBOX_X1), 0x0110);
        ft_wr16(g_synth_in, (uint16_t)(2 + OBJ_BBOX_Y0), 0x0100);
        ft_wr16(g_synth_in, (uint16_t)(2 + OBJ_BBOX_Y1), 0x0110);
        memset(g_vm_es_in, 0, 0x200);
        if (d6) g_vm_es_in[PC0] = 0x30;                      // byte filter
        else { g_vm_es_in[PC0] = 0x04; g_vm_es_in[PC0+1] = 0x00; } // word filter 0x0004
    };
    auto run1 = [&](const char* group, FtSynthStats& st) {
        ft_fill_tail(g_synth_in);
        uint8_t* zone = (uint8_t*)v2_fntest_m2c_base() + (uint32_t)FT_VM_TESTSEG * 16;
        memcpy(zone, g_vm_es_in, FT_VM_ZONE);
        memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
        int32_t packed = d6 ? v2_fntest_call_sub_155d6(g_scratch, PC0)
                            : v2_fntest_call_sub_156c0(g_scratch, PC0);
        memcpy(zone, g_vm_es_in, FT_VM_ZONE);
        int cf_v2 = (int)(packed & 1);
        uint16_t pc_v2 = (uint16_t)((uint32_t)packed >> 1);
        st.cases++;
        memcpy(g_synth_orig, g_synth_in, sizeof(g_synth_orig));
        uint16_t regs[8] = { 0, PC0, 0, 0, 0, 0, 0, 0 };   // bx = PC
        v2_fntest_es_override = FT_VM_TESTSEG;
        long esc0 = ft_ub_marks();
        v2_fntest_orig_isolated(v2_fntest_orig_fnptr(id), g_synth_orig, regs);
        v2_fntest_es_override = 0;
        memcpy(zone, g_vm_es_in, FT_VM_ZONE);
        if (ft_ub_marks() != esc0) { st.cases--; return; }
        long diffs = 0;
        if ((regs[7] & 1) != (uint16_t)cf_v2) {
            if (diff_budget-- > 0)
                fprintf(stderr, "FNSELFTEST-DIFF[%s %s]: CF orig=%d v2=%d\n",
                        g_name[id], group, regs[7] & 1, cf_v2);
            diffs++;
        }
        if (regs[1] != pc_v2) {
            if (diff_budget-- > 0)
                fprintf(stderr, "FNSELFTEST-DIFF[%s %s]: BX/pc orig=%04X v2=%04X\n",
                        g_name[id], group, regs[1], pc_v2);
            diffs++;
        }
        for (uint32_t a = 0; a < 0x10000; a++) {
            if (g_scratch[a] == g_synth_orig[a]) continue;
            if (v2_fntest_ds_skip(a)) continue;
            if (diff_budget-- > 0)
                fprintf(stderr, "FNSELFTEST-DIFF[%s %s]: addr=%04X orig=%02X v2=%02X\n",
                        g_name[id], group, a, g_synth_orig[a], g_scratch[a]);
            diffs++;
        }
        if (diffs) st.fail++; else st.pass++;
    };
    // state==0: mask hit (partner fetch + STC) / mask miss (CLC)
    ctx(0x0000, di, 4, 0xFFFF, d6 ? 0x0030 : 0x0004); run1("grid", grid);
    ctx(0x0000, di, 4, 0x0000, d6 ? 0x0030 : 0x0004); run1("grid", grid);
    // state>0: consume + CLC
    ctx(0x0001, di, 4, 0xCCCC, d6 ? 0x0030 : 0x0004); run1("grid", grid);
    // state<0 (JS): scan hit (OR + stash + CLC)
    ctx(0xFFFF, di, 4, 0x0000, d6 ? 0x0030 : 0x0004); run1("grid", grid);
    // scan: filter mismatch / bbox reject / self-skip / dead
    ctx(0xFFFF, di, 4, 0x0000, d6 ? 0x0031 : 0x0008); run1("grid", grid);
    ctx(0xFFFF, 2,  4, 0x0000, d6 ? 0x0030 : 0x0004); run1("grid", grid);
    FtRng rng(seed);
    for (int i = 0; i < 500; i++) {
        uint16_t stv = (uint16_t)((rng.next() % 3 == 0) ? 0
                       : (rng.next() & 1) ? 0xFFFF : 1);
        ctx(stv, (rng.next() & 3) ? di : 2, (uint16_t)((rng.next() % 8) * 2),
            rng.w(), d6 ? (uint16_t)(rng.next() & 1 ? 0x30 : 0x31)
                        : (uint16_t)(rng.next() & 1 ? 0x0004 : 0x0008));
        run1("fuzz", fuzz);
    }
    fprintf(stderr,
        "FNSELFTEST-SUMMARY[%s]: grid %ld/%ld, fuzz %ld/%ld — total cases=%ld fail=%ld%s\n",
        g_name[id], grid.pass, grid.cases, fuzz.pass, fuzz.cases,
        grid.cases + fuzz.cases, grid.fail + fuzz.fail,
        (grid.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (grid.fail + fuzz.fail) ? 1 : 0;
}

// ---- Units 100-101: sub_1584e / sub_157eb (directional collision ops) -----
// Same [390] triage; state>0 composes verified callees: 1584e = sub_15afd
// (down tile) -> sub_15972 snap | sub_1614e (obj) -> sub_15da8 snap; 157eb =
// sub_15911 (Y-dir walk dispatcher) -> 15972 | sub_15c93 -> 15da8. Hit ORs
// the [38E] bit; ALL state>0 exits are CLC. state==0: bare mask STC (no
// 16243 here — unlike 155d6). Uses the unit-30 tilemap craft for the tile
// stage plus a partner slot for the object stage.
int ft_selftest_coll_dir(FtId id, uint32_t seed) {
    FtSynthStats grid, fuzz;
    long diff_budget = 24;
    const uint16_t di = 8, PC0 = 0x0100;
    bool down = (id == FT_SUB_1584E);
    auto ctx = [&](uint16_t st390, uint16_t coll_bits, uint16_t bit38e,
                   uint16_t wy, uint16_t py, int with_tile, int with_obj) {
        memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
        ft_wr16(g_synth_in, 0x372, 4);
        ft_wr16(g_synth_in, 0x390, st390);
        ft_wr16(g_synth_in, 0x42, di);
        ft_wr16(g_synth_in, 0x38E, bit38e);
        ft_wr16(g_synth_in, (uint16_t)(bit38e - 0x6C34), (uint16_t)(1u << (bit38e / 2)));
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_COLL_BITS), coll_bits);
        // self geometry: X 0x100..0x110, Y around the probe rows
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_X0), 0x0100);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_X1), 0x0110);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_Y0), 0x0100);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_Y1), 0x0110);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_WORLD_X), 0x0108);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_WORLD_Y), wy);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_Y_PREV),  py);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_X_PREV),  0x0108);
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_VEL_Y),   0);
        // tilemap 16x16, stripe type 0x30 on row 4 (world y 0x40..0x4F)
        for (uint32_t a = 0x2E5C; a <= 0x2E7C; a += 2) ft_wr16(g_synth_in, a, 0);
        ft_wr16(g_synth_in, DS_SEG_TILEMAP, FT_VM_TESTSEG);
        ft_wr16(g_synth_in, 0x25DC, 16);
        ft_wr16(g_synth_in, 0x25DE, 16);
        for (int y = 0; y < 16; y++)
            ft_wr16(g_synth_in, (uint16_t)(y * 2 - LUT_ROW_BASE), (uint16_t)(y * 32));
        memset(g_vm_es_in, 0, 0x400);
        if (with_tile)
            for (int x = 0; x < 16; x++) {
                uint16_t tw = (uint16_t)(0x30 << 10);
                g_vm_es_in[0x400 + (4 * 16 + x) * 2]     = (uint8_t)(tw & 0xFF);
                g_vm_es_in[0x400 + (4 * 16 + x) * 2 + 1] = (uint8_t)(tw >> 8);
            }
        if (with_obj) {
            ft_wr16(g_synth_in, (uint16_t)(2 + OBJ_CODE_SEG), 1);
            ft_wr16(g_synth_in, (uint16_t)(2 + OBJ_TYPE_ID), 0x0030);
            ft_wr16(g_synth_in, (uint16_t)(2 + OBJ_BBOX_X0), 0x0100);
            ft_wr16(g_synth_in, (uint16_t)(2 + OBJ_BBOX_X1), 0x0110);
            ft_wr16(g_synth_in, (uint16_t)(2 + OBJ_BBOX_Y0), 0x0100);
            ft_wr16(g_synth_in, (uint16_t)(2 + OBJ_BBOX_Y1), 0x0110);
            ft_wr16(g_synth_in, (uint16_t)(2 + OBJ_VEL_Y), 0);
            ft_wr16(g_synth_in, (uint16_t)(2 + OBJ_STATE_IDX), 0x0030);
        }
        // filter chain for the vel/search families at [0x7000-0x6B34]
        g_synth_in[(uint16_t)(0x7000 - LUT_SCAN_FILTER)]     = 0x30;
        g_synth_in[(uint16_t)(0x7000 - LUT_SCAN_FILTER + 1)] = 0xFF;
        g_vm_es_in[PC0] = 0x00;   // filter byte 0x00 → chain at [0-0x6B34]... use direct byte:
        g_vm_es_in[PC0] = 0x70;   // NB: orig AND si,0xFF then chain at [si-0x6B34];
                                   // 0x70 → ds:[0x70-0x6B34] wrap — craft chain there:
    };
    auto run1 = [&](const char* group, FtSynthStats& st) {
        // chain for filter byte 0x70 at wrapped ds:(0x70-0x6B34)
        uint16_t ca = (uint16_t)(0x70 - LUT_SCAN_FILTER);
        g_synth_in[ca] = 0x30; g_synth_in[(uint16_t)(ca + 1)] = 0xFF;
        ft_fill_tail(g_synth_in);
        uint8_t* zone = (uint8_t*)v2_fntest_m2c_base() + (uint32_t)FT_VM_TESTSEG * 16;
        memcpy(zone, g_vm_es_in, FT_VM_ZONE);
        // tilemap zone lives at +0x400 inside the same segment
        memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
        int32_t packed = down ? v2_fntest_call_sub_1584e(g_scratch, PC0)
                              : v2_fntest_call_sub_157eb(g_scratch, PC0);
        memcpy(zone, g_vm_es_in, FT_VM_ZONE);
        int cf_v2 = (int)(packed & 1);
        uint16_t pc_v2 = (uint16_t)((uint32_t)packed >> 1);
        st.cases++;
        memcpy(g_synth_orig, g_synth_in, sizeof(g_synth_orig));
        uint16_t regs[8] = { 0, PC0, 0, 0, 0, 0, 0, 0 };
        v2_fntest_es_override = FT_VM_TESTSEG;
        long esc0 = ft_ub_marks();
        v2_fntest_orig_isolated(v2_fntest_orig_fnptr(id), g_synth_orig, regs);
        v2_fntest_es_override = 0;
        memcpy(zone, g_vm_es_in, FT_VM_ZONE);
        if (ft_ub_marks() != esc0) { st.cases--; return; }
        long diffs = 0;
        if ((regs[7] & 1) != (uint16_t)cf_v2) {
            if (diff_budget-- > 0)
                fprintf(stderr, "FNSELFTEST-DIFF[%s %s]: CF orig=%d v2=%d\n",
                        g_name[id], group, regs[7] & 1, cf_v2);
            diffs++;
        }
        if (regs[1] != pc_v2) {
            if (diff_budget-- > 0)
                fprintf(stderr, "FNSELFTEST-DIFF[%s %s]: BX/pc orig=%04X v2=%04X\n",
                        g_name[id], group, regs[1], pc_v2);
            diffs++;
        }
        for (uint32_t a = 0; a < 0x10000; a++) {
            if (g_scratch[a] == g_synth_orig[a]) continue;
            if (v2_fntest_ds_skip(a)) continue;
            if (diff_budget-- > 0)
                fprintf(stderr, "FNSELFTEST-DIFF[%s %s]: addr=%04X orig=%02X v2=%02X\n",
                        g_name[id], group, a, g_synth_orig[a], g_scratch[a]);
            diffs++;
        }
        if (diffs) st.fail++; else st.pass++;
    };
    // state==0 mask hit / miss; state<0; state>0 both-miss
    ctx(0x0000, 0xFFFF, 4, 0x0100, 0x0100, 0, 0); run1("grid", grid);
    ctx(0x0000, 0x0000, 4, 0x0100, 0x0100, 0, 0); run1("grid", grid);
    ctx(0xFFFF, 0xCCCC, 4, 0x0100, 0x0100, 0, 0); run1("grid", grid);
    ctx(0x0001, 0x0000, 4, 0x0100, 0x0100, 0, 0); run1("grid", grid);
    // state>0 with an object partner (vel-scan family path)
    ctx(0x0001, 0x0000, 4, 0x0100, 0x0100, 0, 1); run1("grid", grid);
    // state>0 with moving self (dir gates engaged)
    ctx(0x0001, 0x0000, 4, 0x0108, 0x0100, 0, 1); run1("grid", grid);
    ctx(0x0001, 0x0000, 4, 0x0100, 0x0108, 0, 1); run1("grid", grid);
    FtRng rng(seed);
    for (int i = 0; i < 300; i++) {
        uint16_t stv = (uint16_t)((rng.next() % 3 == 0) ? 0
                       : (rng.next() & 1) ? 0xFFFF : 1);
        ctx(stv, rng.w(), (uint16_t)((rng.next() % 8) * 2),
            (uint16_t)(0x0F0 + (rng.next() % 0x30)),
            (uint16_t)(0x0F0 + (rng.next() % 0x30)),
            0, (int)(rng.next() & 1));
        run1("fuzz", fuzz);
    }
    fprintf(stderr,
        "FNSELFTEST-SUMMARY[%s]: grid %ld/%ld, fuzz %ld/%ld — total cases=%ld fail=%ld%s\n",
        g_name[id], grid.pass, grid.cases, fuzz.pass, fuzz.cases,
        grid.cases + fuzz.cases, grid.fail + fuzz.fail,
        (grid.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (grid.fail + fuzz.fail) ? 1 : 0;
}

// ---- Units 102-103: sub_16235 / sub_16243 (collision partner table) -------
int ft_selftest_colltab(FtId id, uint32_t seed) {
    FtSynthStats grid, fuzz;
    long diff_budget = 24;
    bool wr = (id == FT_SUB_16235);
    auto run1 = [&](uint16_t si, uint16_t di, uint16_t e38, const char* group, FtSynthStats& st) {
        memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
        ft_wr16(g_synth_in, 0x38E, e38);
        uint16_t addr = (uint16_t)((uint16_t)(di << 4) + e38 + 0x1B25);
        ft_wr16(g_synth_in, addr, 0xCAFE);
        ft_fill_tail(g_synth_in);
        memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
        uint16_t ax_v2 = 0;
        if (wr) v2_fntest_call_sub_16235(g_scratch, si, di);
        else    ax_v2 = v2_fntest_call_sub_16243(g_scratch, di);
        FtRegs in{}; in.si = si; in.di = di;
        ft_synth_case_regs(id, in, ax_v2, wr ? -1 : 0, group, st, diff_budget);
    };
    static const uint16_t DI[] = { 0, 2, 4, 0x100, 0x0FFF, 0x8000, 0xFFFF };
    static const uint16_t E38[] = { 0, 2, 14, 0x8000, 0xFFFF };
    for (uint16_t d : DI) for (uint16_t e : E38) run1(0x1234, d, e, "grid", grid);
    FtRng rng(seed);
    for (int i = 0; i < 20000; i++) run1(rng.w(), rng.w(), rng.w(), "fuzz", fuzz);
    fprintf(stderr,
        "FNSELFTEST-SUMMARY[%s]: grid %ld/%ld, fuzz %ld/%ld — total cases=%ld fail=%ld%s\n",
        g_name[id], grid.pass, grid.cases, fuzz.pass, fuzz.cases,
        grid.cases + fuzz.cases, grid.fail + fuzz.fail,
        (grid.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (grid.fail + fuzz.fail) ? 1 : 0;
}

// ---- Unit 104: sub_16390 (slope height diff) ------------------------------
int ft_selftest_sub_16390(uint32_t seed) {
    FtSynthStats exh, fuzz;
    long diff_budget = 24;
    const uint16_t di = 8;
    auto run1 = [&](uint16_t ax, uint16_t si, uint16_t y150d, uint8_t lutv,
                    const char* group, FtSynthStats& st) {
        memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
        ft_wr16(g_synth_in, (uint16_t)(di + OBJ_BBOX_Y1), y150d);
        uint16_t sidx = (uint16_t)((((ax & 0xF) << 4) + (si & 0xF)) - 0x7684);
        g_synth_in[sidx] = lutv;
        ft_fill_tail(g_synth_in);
        memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
        int16_t ax_v2 = v2_fntest_call_sub_16390(g_scratch, ax, si, di);
        FtRegs in{}; in.ax = ax; in.si = si; in.di = di;
        ft_synth_case_regs(FT_SUB_16390, in, (uint16_t)ax_v2, 0, group, st, diff_budget);
    };
    for (uint16_t a = 0; a < 16; a++)
        for (uint16_t s = 0; s < 16; s++)
            for (uint16_t y = 0; y < 16; y++)
                run1(a, s, (uint16_t)(0x0100 | y), (uint8_t)(y * 7 + s), "exh", exh);
    FtRng rng(seed);
    for (int i = 0; i < 20000; i++)
        run1(rng.w(), rng.w(), rng.w(), (uint8_t)rng.next(), "fuzz", fuzz);
    fprintf(stderr,
        "FNSELFTEST-SUMMARY[sub_16390]: exh %ld/%ld, fuzz %ld/%ld — total cases=%ld fail=%ld%s\n",
        exh.pass, exh.cases, fuzz.pass, fuzz.cases, exh.cases + fuzz.cases,
        exh.fail + fuzz.fail, (exh.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (exh.fail + fuzz.fail) ? 1 : 0;
}

// ---- Unit 54 full tree: sub_13a0e = viewport clamps + 13ae0 spawn loop ----
// Both sides read object templates from ONE synthetic block: the oracle via
// es=[2E67] -> FT_VM_TESTSEG (templates copied into m2c::m at SEG*16), v2 via
// v2_fntest_set_animdata. Template (0x15 bytes per code_seg_idx):
//   +0 chunk(2: FFFF=no sprite, FFFE=[374]+=1, else [12AD] lookup)
//   +2 ss_byte(1: low7=sub-sprite count, bit7=[374]+=2)  +3 w16([132D]=v+3)
//   +7 w16([15AD])  +9 b([1445]) +A b([146D])  +B/+D/+F/+11/+13 w16 fields
struct FtSpawnRec { uint16_t x, y, hw, hh, code, anim, extra; };
void ft_spawn_build(const FtSpawnRec* recs, int n,
                    const uint8_t* tmpl, int tmpl_len,
                    const FtWr* wr, int nw)
{
    memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
    // Level context: template segment, empty object table, gates open.
    ft_wr16(g_synth_in, DS_SEG_ANIM, FT_VM_TESTSEG);
    ft_wr16(g_synth_in, 0x033C, 0);
    ft_wr16(g_synth_in, 0x0372, 0);
    ft_wr16(g_synth_in, 0x032F, 0);
    for (int i = 0; i < 0x10; i++) g_synth_in[0x356 + i] = 0;
    // Clear the object-slot zone the spawn writes into (deterministic diff).
    for (uint32_t a = OBJ_CODE_SEG; a < 0x1B60; a++) g_synth_in[a] = 0;
    // Spawn table.
    uint16_t off = 0x25F6;
    for (int i = 0; i < n; i++) {
        ft_wr16(g_synth_in, off + 0x0, recs[i].x);
        ft_wr16(g_synth_in, off + 0x2, recs[i].y);
        ft_wr16(g_synth_in, off + 0x4, recs[i].hw);
        ft_wr16(g_synth_in, off + 0x6, recs[i].hh);
        ft_wr16(g_synth_in, off + 0x8, recs[i].code);
        ft_wr16(g_synth_in, off + 0xA, recs[i].anim);
        ft_wr16(g_synth_in, off + 0xC, recs[i].extra);
        off += 0x0E;
    }
    ft_wr16(g_synth_in, off, 0xFFFF);
    for (int i = 0; i < nw; i++) ft_wr16(g_synth_in, wr[i].addr, wr[i].val);
    // Template block into the shared oracle segment + the v2 shadow.
    uint8_t* zone = (uint8_t*)v2_fntest_m2c_base() + (uint32_t)FT_VM_TESTSEG * 16;
    memset(zone, 0, 0x200);
    memcpy(zone, tmpl, tmpl_len);
    v2_fntest_set_animdata(tmpl, (uint32_t)tmpl_len);
    ft_fill_tail(g_synth_in);
}

bool ft_spawn_case(const char* tag, FtSynthStats& st, long& diff_budget) {
    memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
    v2_fntest_call_sub_13a0e(g_scratch);
    FtRegs in{};
    return ft_synth_case_regs(FT_SUB_13A0E, in, 0, -1, tag, st, diff_budget);
}

int ft_selftest_spawn() {
    FtSynthStats grid;
    long diff_budget = 40;
    // Base template pair: idx0 = plain sprite-less object, idx1 = with fields.
    uint8_t T[0x15 * 3];
    memset(T, 0, sizeof(T));
    auto w16 = [&](int off, uint16_t v){ T[off] = (uint8_t)v; T[off+1] = (uint8_t)(v>>8); };
    // idx 0
    w16(0x00, 0xFFFF); T[0x02] = 0; w16(0x03, 0x10);
    w16(0x07, 0x1111); T[0x09] = 0x10; T[0x0A] = 0x18;
    w16(0x0B, 0x2222); w16(0x0D, 0x3333); w16(0x0F, 0x4444);
    w16(0x11, 0x5555); w16(0x13, 0x6666);
    // idx 1
    int b = 0x15;
    w16(b+0x00, 0xFFFF); T[b+0x02] = 0; w16(b+0x03, 0x20);
    w16(b+0x07, 0x7777); T[b+0x09] = 0x20; T[b+0x0A] = 0x08;
    w16(b+0x0B, 0x0102); w16(b+0x0D, 0x0304); w16(b+0x0F, 0x0506);
    w16(b+0x11, 0x0708); w16(b+0x13, 0x090A);
    // idx 2: FFFE chunk ([374]+=1) + ss_byte bit7 ([374]+=2)
    b = 0x2A;
    w16(b+0x00, 0xFFFE); T[b+0x02] = 0x80; w16(b+0x03, 0x08);
    T[b+0x09] = 0x08; T[b+0x0A] = 0x08;

    // Viewport for [44]=0x100,[46]=0x100: bounds [34]=F0,[36]=250,[38]=F0,[3A]=1C0.
    const FtWr VP[] = { {0x0044, 0x0100}, {0x0046, 0x0100} };
    FtSpawnRec both_vis[] = {
        { 0x0120, 0x0120, 8, 8, 0, 0x11, 0xAA55 },
        { 0x0200, 0x0150, 8, 8, 1, 0x22, 0x1234 },
    };
    ft_spawn_build(both_vis, 2, T, sizeof(T), VP, 2);
    ft_spawn_case("both-vis", grid, diff_budget);

    FtSpawnRec one_vis[] = {
        { 0x0120, 0x0120, 8, 8, 0, 0x11, 0 },
        { 0x0500, 0x0150, 8, 8, 1, 0x22, 0 },   // right of [36]
    };
    ft_spawn_build(one_vis, 2, T, sizeof(T), VP, 2);
    ft_spawn_case("one-vis", grid, diff_budget);

    FtSpawnRec none_vis[] = {
        { 0x0500, 0x0120, 8, 8, 0, 0, 0 },
        { 0x0120, 0x0500, 8, 8, 1, 0, 0 },
    };
    ft_spawn_build(none_vis, 2, T, sizeof(T), VP, 2);
    ft_spawn_case("none-vis", grid, diff_budget);

    // Edge-touch cases on every bound (signed-operand JGE/JL edges).
    FtSpawnRec edges[] = {
        { 0x00E8, 0x0120, 8, 8, 0, 0, 0 },   // x+hw == [34] (0xF0): visible
        { 0x00E7, 0x0120, 8, 8, 0, 0, 0 },   // x+hw == [34]-1: out
        { 0x0258, 0x0120, 8, 8, 0, 0, 0 },   // x-hw == [36] (0x250): out
        { 0x0257, 0x0120, 8, 8, 0, 0, 0 },   // x-hw == [36]-1: visible
    };
    ft_spawn_build(edges, 4, T, sizeof(T), VP, 2);
    ft_spawn_case("edges", grid, diff_budget);

    // Dedup: slot 0 already spawned from spawn index 0.
    {
        FtSpawnRec recs[] = { { 0x0120, 0x0120, 8, 8, 0, 0, 0 } };
        const FtWr wr[] = { {0x0044,0x0100},{0x0046,0x0100},
                            {0x0372, 2},              // one live slot
                            {(uint16_t)(0 + OBJ_CODE_SEG), 0x1234},   // slot 0 active
                            {(uint16_t)(0 + OBJ_ANIM_SUB), 0} };      // from spawn idx 0
        ft_spawn_build(recs, 1, T, sizeof(T), wr, 5);
        ft_spawn_case("dedup", grid, diff_budget);
    }
    // Creation gate closed.
    {
        FtSpawnRec recs[] = { { 0x0120, 0x0120, 8, 8, 0, 0, 0 } };
        const FtWr wr[] = { {0x0044,0x0100},{0x0046,0x0100},{0x032F,1} };
        ft_spawn_build(recs, 1, T, sizeof(T), wr, 3);
        ft_spawn_case("gate-32F", grid, diff_budget);
    }
    // Spawn-bit block: [356]=FF blocks indices 0..7.
    {
        FtSpawnRec recs[] = { { 0x0120, 0x0120, 8, 8, 0, 0, 0 } };
        ft_spawn_build(recs, 1, T, sizeof(T), VP, 2);
        g_synth_in[0x356] = 0xFF;
        ft_spawn_case("bit-356", grid, diff_budget);
    }
    // All 20 slots busy.
    {
        FtSpawnRec recs[] = { { 0x0120, 0x0120, 8, 8, 0, 0, 0 } };
        ft_spawn_build(recs, 1, T, sizeof(T), VP, 2);
        for (uint16_t s2 = 0; s2 < 0x28; s2 += 2)
            ft_wr16(g_synth_in, (uint16_t)(s2 + OBJ_CODE_SEG), 0x1111);
        ft_wr16(g_synth_in, 0x0372, 0x28);
        ft_spawn_case("slots-full", grid, diff_budget);
    }
    // FFFE chunk + ss bit7 template (idx 2): [374] accumulation path.
    {
        FtSpawnRec recs[] = { { 0x0120, 0x0120, 8, 8, 2, 0x33, 0xBEEF } };
        ft_spawn_build(recs, 1, T, sizeof(T), VP, 2);
        ft_spawn_case("fffe-bit7", grid, diff_budget);
    }
    // Sprite chunk lookup: template idx0 with chunk=5; [12AD] carries 5->0x4321.
    {
        uint8_t T2[0x15]; memcpy(T2, T, 0x15);
        T2[0] = 5; T2[1] = 0;
        FtSpawnRec recs[] = { { 0x0120, 0x0120, 8, 8, 0, 0, 0 } };
        const FtWr wr[] = { {0x0044,0x0100},{0x0046,0x0100},
                            {DS_SPRITE_RES_ID, 5}, {DS_SPRITE_RES_BASE, 0x4321} };
        ft_spawn_build(recs, 1, T2, sizeof(T2), wr, 4);
        ft_spawn_case("chunk-hit", grid, diff_budget);
    }
    // Sprite chunk with NO resource entry: creation fails mid-init.
    {
        uint8_t T2[0x15]; memcpy(T2, T, 0x15);
        T2[0] = 7; T2[1] = 0;
        FtSpawnRec recs[] = { { 0x0120, 0x0120, 8, 8, 0, 0, 0 } };
        ft_spawn_build(recs, 1, T2, sizeof(T2), VP, 2);
        ft_spawn_case("chunk-miss", grid, diff_budget);
    }

    fprintf(stderr,
        "FNSELFTEST-SUMMARY[sub_13a0e]: grid %ld/%ld — total cases=%ld fail=%ld%s\n",
        grid.pass, grid.cases, grid.cases, grid.fail,
        grid.fail ? "  <<< DIVERGENCE" : "");
    return grid.fail ? 1 : 0;
}

// ---- Units 60-62: the rest of the spawn family -----------------------------
bool ft_spawn_case_id(FtId id, uint16_t reg_di, const char* tag,
                      FtSynthStats& st, long& diff_budget) {
    memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
    if (id == FT_SUB_13BA5)      v2_fntest_call_sub_13ba5(g_scratch);
    else if (id == FT_SUB_11446) v2_fntest_call_sub_11446(g_scratch);
    else                         v2_fntest_call_sub_11569(g_scratch, reg_di);
    FtRegs in{}; in.di = reg_di;
    return ft_synth_case_regs(id, in, 0, -1, tag, st, diff_budget);
}

int ft_selftest_spawn2() {
    FtSynthStats grid;
    long diff_budget = 40;
    uint8_t T[0x15 * 3];
    memset(T, 0, sizeof(T));
    auto w16 = [&](int off, uint16_t v){ T[off] = (uint8_t)v; T[off+1] = (uint8_t)(v>>8); };
    for (int i = 0; i < 3; i++) {   // three plain templates
        int b = i * 0x15;
        w16(b+0x00, 0xFFFF); T[b+0x02] = 0; w16(b+0x03, 0x10 + i);
        T[b+0x09] = 0x10; T[b+0x0A] = 0x10;
        w16(b+0x0B, (uint16_t)(0x1000 + i)); w16(b+0x0D, (uint16_t)(0x2000 + i));
    }
    const FtWr VP[] = { {0x0044, 0x0100}, {0x0046, 0x0100} };
    int rc = 0;

    // 60 sub_13ba5: permanent-flag sweep (anim & 0x800).
    {
        FtSynthStats g2; 
        FtSpawnRec recs[] = {
            { 0x0120, 0x0120, 8, 8, 0, 0x0811, 0x0001 },   // permanent
            { 0x0200, 0x0150, 8, 8, 1, 0x0022, 0x0002 },   // not
            { 0x0500, 0x0500, 8, 8, 2, 0x0800, 0x0003 },   // permanent, off-screen (still spawns!)
        };
        ft_spawn_build(recs, 3, T, sizeof(T), VP, 2);
        ft_spawn_case_id(FT_SUB_13BA5, 0, "mixed", g2, diff_budget);
        FtSpawnRec none[] = { { 0x0120, 0x0120, 8, 8, 0, 0x0011, 0 } };
        ft_spawn_build(none, 1, T, sizeof(T), VP, 2);
        ft_spawn_case_id(FT_SUB_13BA5, 0, "no-permanent", g2, diff_budget);
        fprintf(stderr, "FNSELFTEST-SUMMARY[sub_13ba5]: grid %ld/%ld — total cases=%ld fail=%ld%s\n",
                g2.pass, g2.cases, g2.cases, g2.fail, g2.fail ? "  <<< DIVERGENCE" : "");
        rc |= g2.fail ? 1 : 0; grid.cases += g2.cases; grid.fail += g2.fail;
    }
    // 62 sub_11569: 3 vikings from an 18-byte table at [di].
    {
        FtSynthStats g2;
        static const uint16_t ANIM_OR[] = { 0, 0x4000 };
        for (uint16_t aor : ANIM_OR) {
            FtSpawnRec none[] = { { 0x0500, 0x0500, 8, 8, 0, 0, 0 } };
            const FtWr wr[] = { {0x0044,0x0100},{0x0046,0x0100},{0x25C1, aor},
                                {0x0100, 0x0130},{0x0102, 0x0140},{0x0104, 0x0007},
                                {0x0106, 0x0150},{0x0108, 0x0140},{0x010A, 0x0008},
                                {0x010C, 0x0170},{0x010E, 0x0140},{0x0110, 0x0009} };
            ft_spawn_build(none, 1, T, sizeof(T), wr, 12);
            ft_spawn_case_id(FT_SUB_11569, 0x0100, aor ? "or4000" : "plain",
                             g2, diff_budget);
        }
        fprintf(stderr, "FNSELFTEST-SUMMARY[sub_11569]: grid %ld/%ld — total cases=%ld fail=%ld%s\n",
                g2.pass, g2.cases, g2.cases, g2.fail, g2.fail ? "  <<< DIVERGENCE" : "");
        rc |= g2.fail ? 1 : 0; grid.cases += g2.cases; grid.fail += g2.fail;
    }
    // 61 sub_11446: mode dispatcher (modes 0/2/4/5/0x10/other).
    {
        FtSynthStats g2;
        static const uint16_t MODES[] = { 0, 2, 4, 5, 0x10, 3 };
        for (uint16_t m : MODES) {
            FtSpawnRec none[] = { { 0x0500, 0x0500, 8, 8, 0, 0, 0 } };
            const FtWr wr[] = { {0x0044,0x0100},{0x0046,0x0100},
                                {0x25BB, 0x0140},{0x25BD, 0x0120},   // mode-0 pos
                                {0x25BF, 0x0001},{0x25C1, 0x0000},   // code idx / anim
                                {0x25C3, 0x0042},                    // [374] cfg
                                {0x25C5, 0x0180},{0x25C7, 0x0110} }; // default-path pos
            ft_spawn_build(none, 1, T, sizeof(T), wr, 9);
            g_synth_in[0x25BA] = (uint8_t)m;
            ft_spawn_case_id(FT_SUB_11446, 0, "mode", g2, diff_budget);
        }
        fprintf(stderr, "FNSELFTEST-SUMMARY[sub_11446]: grid %ld/%ld — total cases=%ld fail=%ld%s\n",
                g2.pass, g2.cases, g2.cases, g2.fail, g2.fail ? "  <<< DIVERGENCE" : "");
        rc |= g2.fail ? 1 : 0; grid.cases += g2.cases; grid.fail += g2.fail;
    }
    return rc;
}

// ---- Unit 63: sub_15911 — Y-move search dispatcher --------------------------
// cur=[di+1765] vs old=[di+13CD]: == -> CLC, ax=cur (untouched path);
// < -> loc_15a64 tail (Y_start-1 probe), ax=1; > -> loc_15a7d tail, ax=0.
// The tails are the same search machinery as units 20-24 — reuse their
// synthetic 4x4 tile map + filter table environment.
bool ft_synth_case_15911(uint16_t cur_y, uint16_t old_y, uint16_t filter,
                         const char* group, FtSynthStats& st, long& diff_budget)
{
    st.cases++;
    const uint16_t OBJ = 6;
    uint8_t* zone = (uint8_t*)v2_fntest_m2c_base() + (uint32_t)FT_VM_TESTSEG * 16;
    memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
    ft_wr16(g_synth_in, 0x372, 8);
    for (uint32_t a = 0x2E5C; a <= 0x2E7C; a += 2) ft_wr16(g_synth_in, a, 0);
    ft_wr16(g_synth_in, DS_SEG_TILEMAP, FT_VM_TESTSEG);
    ft_wr16(g_synth_in, 0x25DC, 4);
    ft_wr16(g_synth_in, 0x25DE, 4);
    for (int y = 0; y < 4; y++)
        ft_wr16(g_synth_in, (uint16_t)(y * 2 - LUT_ROW_BASE), (uint16_t)(y * 8));
    uint16_t T[16];
    for (int i = 0; i < 16; i++) T[i] = (uint16_t)((i & 0x3F) << 10);
    memset(g_vm_es_in, 0, sizeof(g_vm_es_in));
    for (int i = 0; i < 16; i++) {
        g_vm_es_in[i * 2] = (uint8_t)(T[i] & 0xFF);
        g_vm_es_in[i * 2 + 1] = (uint8_t)(T[i] >> 8);
    }
    memcpy(zone, g_vm_es_in, FT_VM_ZONE);
    ft_wr16(g_synth_in, (uint16_t)(OBJ + OBJ_FLAGS), 0x8000);
    ft_wr16(g_synth_in, (uint16_t)(OBJ + OBJ_BBOX_X0), 0x0010);
    ft_wr16(g_synth_in, (uint16_t)(OBJ + OBJ_BBOX_X1), 0x0020);
    ft_wr16(g_synth_in, (uint16_t)(OBJ + 0x14E5), 0x0008);
    ft_wr16(g_synth_in, (uint16_t)(OBJ + OBJ_BBOX_Y1), 0x000E);
    ft_wr16(g_synth_in, (uint16_t)(0 - LUT_SCAN_FILTER), 0xFF05);   // filter chain
    ft_wr16(g_synth_in, (uint16_t)(OBJ + OBJ_WORLD_Y), cur_y);
    ft_wr16(g_synth_in, (uint16_t)(OBJ + 0x13CD), old_y);
    ft_fill_tail(g_synth_in);

    memcpy(g_synth_orig, g_synth_in, sizeof(g_synth_orig));
    uint16_t regs[8] = { 0xBEEF, 0, 0, 0, filter, OBJ, 0, 0 };
    v2_fntest_orig_isolated(v2_fntest_orig_fnptr(FT_SUB_15911), g_synth_orig, regs);

    memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
    int pack = v2_fntest_call_sub_15911(g_scratch, OBJ, filter);
    int v2_carry = (pack >> 1) & 1;
    int v2_od    = (pack >> 4);        // 0 = untouched, 1 = dir0, 2 = dir1

    long diffs = 0;
    uint16_t ax_expect = (v2_od == 0) ? cur_y : (v2_od == 2 ? 1 : 0);
    if (regs[0] != ax_expect) {
        if (diff_budget-- > 0)
            fprintf(stderr, "FNSELFTEST-DIFF[sub_15911 %s]: ax orig=%04X v2model=%04X | cur=%04X old=%04X f=%04X\n",
                    group, regs[0], ax_expect, cur_y, old_y, filter);
        diffs++;
    }
    if ((int)regs[7] != v2_carry) {
        if (diff_budget-- > 0)
            fprintf(stderr, "FNSELFTEST-DIFF[sub_15911 %s]: CF orig=%d v2=%d | cur=%04X old=%04X f=%04X\n",
                    group, regs[7], v2_carry, cur_y, old_y, filter);
        diffs++;
    }
    for (uint32_t a = 0; a < 0x10000; a++) {
        if (g_scratch[a] == g_synth_orig[a]) continue;
        if (v2_fntest_ds_skip(a)) continue;
        if (diff_budget > 0) {
            diff_budget--;
            fprintf(stderr, "FNSELFTEST-DIFF[sub_15911 %s]: addr=%04X orig=%02X v2=%02X (in=%02X) | cur=%04X old=%04X\n",
                    group, a, g_synth_orig[a], g_scratch[a], g_synth_in[a], cur_y, old_y);
        }
        diffs++;
    }
    if (diffs) { st.fail++; return false; }
    st.pass++; return true;
}

int ft_selftest_sub_15911() {
    FtSynthStats grid, fuzz;
    long diff_budget = 24;
    static const uint16_t YS[] = { 0x0008, 0x000C, 0x000E, 0x0010, 0x0100,
                                   0x7FFF, 0x8000, 0xFFFF };
    for (uint16_t cur : YS)
        for (uint16_t old : YS)
            ft_synth_case_15911(cur, old, 0, "grid", grid, diff_budget);
    FtRng rng(0x15911001u);
    for (int i = 0; i < 4000; i++) {
        uint16_t cur = rng.w(), old = (rng.next() & 1) ? cur : rng.w();
        ft_synth_case_15911(cur, old, 0, "fuzz", fuzz, diff_budget);
    }
    fprintf(stderr,
        "FNSELFTEST-SUMMARY[sub_15911]: grid %ld/%ld, fuzz %ld/%ld — total cases=%ld fail=%ld%s\n",
        grid.pass, grid.cases, fuzz.pass, fuzz.cases,
        grid.cases + fuzz.cases, grid.fail + fuzz.fail,
        (grid.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (grid.fail + fuzz.fail) ? 1 : 0;
}

// ---- Units 64-65: pause-screen logic with the sound stub --------------------
// sub_12549(ax): corner/frame glyph pick per box mode; calls the SFX channel
// dispatcher sub_15473 in some branches (headless: AIL stubbed, DS channel
// effects compared as data). sub_11cbb: pause item interaction ([3B8] input
// bits, [447] mode) — HUD renderer callees need the same LUT domains as
// sub_11c52 (ft_norm_11c52); fuzz is base-image-only.
int ft_selftest_sub_12549() {
    FtSynthStats grid;
    long diff_budget = 24;
    static const uint16_t AXS[] = { 0, 1, 2, 3, 4, 5, 6, 0x10, 0xFFFF };
    static const uint16_t WH[][2] = { {2,2}, {3,4}, {0x12,5}, {0x28,0x19} };
    for (uint16_t ax : AXS)
        for (auto& wh : WH) {
            memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
            ft_wr16(g_synth_in, 0x34, wh[0]);
            ft_wr16(g_synth_in, 0x36, wh[1]);
            ft_fill_tail(g_synth_in);
            memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
            v2_fntest_call_sub_12549(g_scratch, ax);
            FtRegs in{}; in.ax = ax;
            ft_synth_case_regs(FT_SUB_12549, in, 0, -1, "grid", grid, diff_budget);
        }
    fprintf(stderr,
        "FNSELFTEST-SUMMARY[sub_12549]: grid %ld/%ld — total cases=%ld fail=%ld%s\n",
        grid.pass, grid.cases, grid.cases, grid.fail,
        grid.fail ? "  <<< DIVERGENCE" : "");
    return grid.fail ? 1 : 0;
}

int ft_selftest_sub_11cbb() {
    FtSynthStats grid;
    long diff_budget = 24;
    static const uint16_t MODES[] = { 0, 1, 2 };
    static const uint16_t INPUTS[] = { 0, 0x0100, 0x0200, 0x0300, 0x8000, 0x0080 };
    for (uint16_t m : MODES)
        for (uint16_t inp : INPUTS) {
            memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
            ft_wr16(g_synth_in, 0x447, m);
            ft_wr16(g_synth_in, 0x3B8, inp);
            ft_wr16(g_synth_in, 0x304, 1);   // SFX mute: the orig sub_177bb
                                             // AIL chain is not walkable in
                                             // the isolator (sound handles are
                                             // a documented verify exception)
            ft_norm_11c52(g_synth_in);   // same HUD LUT domains
            ft_fill_tail(g_synth_in);
            memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
            v2_fntest_call_sub_11cbb(g_scratch);
            FtRegs in{};
            ft_synth_case_regs(FT_SUB_11CBB, in, 0, -1, "grid", grid, diff_budget);
        }
    fprintf(stderr,
        "FNSELFTEST-SUMMARY[sub_11cbb]: grid %ld/%ld — total cases=%ld fail=%ld%s\n",
        grid.pass, grid.cases, grid.cases, grid.fail,
        grid.fail ? "  <<< DIVERGENCE" : "");
    return grid.fail ? 1 : 0;
}

// ---- Unit 66: sub_173c7 — render-tilemap (FS) builder -----------------------
// Zones: tilemap = FT_VM_TESTSEG, GS tiledata = FT_GS_SEG, FS = FT_FS_SEG
// (m2c::m scratch, save/restored per case). v2 reads/writes its shadow
// buffers via the v2_fntest_set_* helpers; the FS zone comparison is a NEW
// compare channel (poison-protocol tested).
const uint16_t FT_GS_SEG = 0x5000;   // 32K zone 0x50000..0x57FFF
const uint16_t FT_FS_SEG = 0x5800;   // reuses the chunk DEST zone (saved/restored)

bool ft_synth_case_173c7(uint16_t w, uint16_t h, uint8_t flag25cf,
                         uint32_t seed, const char* group,
                         FtSynthStats& st, long& diff_budget)
{
    st.cases++;
    uint8_t* mbase = (uint8_t*)v2_fntest_m2c_base();
    uint8_t* tm_zone = mbase + (uint32_t)FT_VM_TESTSEG * 16;
    uint8_t* gs_zone = mbase + (uint32_t)FT_GS_SEG * 16;
    uint8_t* fs_zone = mbase + (uint32_t)FT_FS_SEG * 16;
    static uint8_t saved_fs[0x10000], saved_gs[0x8000];
    memcpy(saved_fs, fs_zone, 0x10000);
    memcpy(saved_gs, gs_zone, 0x8000);
    memset(fs_zone, 0xCC, 0x10000);

    // Tile map W*H words + GS entries (8 bytes per tile type).
    FtRng rng(seed);
    static uint8_t tm[0x10000], gsd[0x8000];
    memset(tm, 0, sizeof(tm)); memset(gsd, 0, sizeof(gsd));
    for (int i = 0; i < w * h; i++) {
        uint16_t t = (uint16_t)(rng.w() & 0x3FF) | (uint16_t)(rng.w() & 0xFC00);
        tm[i * 2] = (uint8_t)t; tm[i * 2 + 1] = (uint8_t)(t >> 8);
    }
    for (int i = 0; i < 0x8000; i++) gsd[i] = (uint8_t)rng.w();
    memset(tm_zone, 0, FT_VM_ZONE);
    memcpy(tm_zone, tm, sizeof(tm));
    memset(gs_zone, 0, 0x8000);
    memcpy(gs_zone, gsd, sizeof(gsd));

    memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
    ft_wr16(g_synth_in, DS_SEG_TILEMAP, FT_VM_TESTSEG);
    ft_wr16(g_synth_in, DS_SEG_FS, FT_FS_SEG);
    ft_wr16(g_synth_in, 0x2E5D, FT_GS_SEG);
    ft_wr16(g_synth_in, 0x25DC, w);
    ft_wr16(g_synth_in, 0x25DE, h);
    ft_wr16(g_synth_in, 0x2E65, (uint16_t)(w * h * 2));   // GS tail dest offset
    g_synth_in[0x25CF] = flag25cf;
    ft_fill_tail(g_synth_in);

    memcpy(g_synth_orig, g_synth_in, sizeof(g_synth_orig));
    uint16_t regs[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    v2_fntest_orig_isolated(v2_fntest_orig_fnptr(FT_SUB_173C7), g_synth_orig, regs);
    static uint8_t orig_fs[0x10000], orig_tm[0x10000];
    memcpy(orig_fs, fs_zone, 0x10000);
    memcpy(orig_tm, tm_zone, 0x10000);
    memcpy(fs_zone, saved_fs, 0x10000);
    memcpy(gs_zone, saved_gs, 0x8000);

    // v2: same inputs into the shadows; FS baseline matches the 0xCC zone.
    v2_fntest_set_tilemap(tm, sizeof(tm));
    v2_fntest_set_gs_tiledata(gsd, sizeof(gsd));
    v2_fntest_clear_fs(0xCC);
    memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
    v2_fntest_call_sub_173c7(g_scratch);

    long diffs = 0;
    uint8_t* v2fs = v2_fntest_fs_ptr();
    for (uint32_t a = 0; a < 0x10000; a++) {
        if (orig_fs[a] == v2fs[a]) continue;
        if (diff_budget > 0) { diff_budget--;
            fprintf(stderr, "FNSELFTEST-DIFF[sub_173c7 %s]: fs+%04X orig=%02X v2=%02X | w=%d h=%d f=%02X\n",
                    group, a, orig_fs[a], v2fs[a], w, h, flag25cf); }
        diffs++;
        if (diffs > 60) break;
    }
    // The GS tail lands in the tilemap zone (ES) — compare it too.
    uint8_t* v2tm = v2_fntest_tilemap_ptr();
    for (uint32_t a = 0; a < 0x10000; a++) {
        if (orig_tm[a] == v2tm[a]) continue;
        if (diff_budget > 0) { diff_budget--;
            fprintf(stderr, "FNSELFTEST-DIFF[sub_173c7 %s]: tm+%04X orig=%02X v2=%02X | w=%d h=%d\n",
                    group, a, orig_tm[a], v2tm[a], w, h); }
        diffs++;
        if (diffs > 90) break;
    }
    for (uint32_t a = 0; a < 0x10000; a++) {
        if (g_scratch[a] == g_synth_orig[a]) continue;
        if (v2_fntest_ds_skip(a)) continue;
        if (diff_budget > 0) { diff_budget--;
            fprintf(stderr, "FNSELFTEST-DIFF[sub_173c7 %s]: ds addr=%04X orig=%02X v2=%02X\n",
                    group, a, g_synth_orig[a], g_scratch[a]); }
        diffs++;
    }
    if (diffs) { st.fail++; return false; }
    st.pass++; return true;
}

int ft_selftest_sub_173c7() {
    FtSynthStats grid;
    long diff_budget = 40;
    static const uint16_t WH[][2] = { {1,1}, {2,2}, {4,4}, {8,3}, {0x2B,0x19},
                                      {0x40,0x20} };
    int ci = 0;
    for (auto& wh : WH)
        ft_synth_case_173c7(wh[0], wh[1], 0, 0x173C7000u + ci++, "grid",
                            grid, diff_budget);
    ft_synth_case_173c7(4, 4, 0x42, 0x173C70F0u, "chunk-clear", grid, diff_budget);
    ft_synth_case_173c7(4, 4, 0x02, 0x173C70F1u, "flag02", grid, diff_budget);
    fprintf(stderr,
        "FNSELFTEST-SUMMARY[sub_173c7]: grid %ld/%ld — total cases=%ld fail=%ld%s\n",
        grid.pass, grid.cases, grid.cases, grid.fail,
        grid.fail ? "  <<< DIVERGENCE" : "");
    return grid.fail ? 1 : 0;
}

// ---- Unit 67: sub_14207 — full VM sweep + priority drain queue --------------
// vmops-style context: bytecode in the shared test segment (yield carpet),
// two live objects in slots 0/2. Directed queue cases pre-load [376]/[378]
// (the same bytes the spawn op writes) — the drain bound must be re-read
// live (task #31: three v2 inlines hoisted it).
bool ft_synth_case_14207(int nobj, uint16_t prio_n, const uint8_t* prio_q,
                         const char* group, FtSynthStats& st, long& diff_budget)
{
    // (case counting happens inside ft_synth_case_regs)
    uint8_t* zone = (uint8_t*)v2_fntest_m2c_base() + (uint32_t)FT_VM_TESTSEG * 16;
    memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
    ft_wr16(g_synth_in, 0x372, (uint16_t)(nobj * 2));
    for (uint32_t a = 0x2E5C; a <= 0x2E7C; a += 2) ft_wr16(g_synth_in, a, 0);
    ft_wr16(g_synth_in, 0x32F, 0);
    ft_wr16(g_synth_in, 0x42, 0xFFFF);
    for (int i = 0; i < nobj; i++) {
        ft_wr16(g_synth_in, (uint16_t)(i * 2 + OBJ_CODE_SEG), FT_VM_TESTSEG);
        ft_wr16(g_synth_in, (uint16_t)(i * 2 + OBJ_PC), (uint16_t)(FT_VM_PC + i * 8));
        ft_wr16(g_synth_in, (uint16_t)(i * 2 + OBJ_FLAGS), 0x8000);
        ft_wr16(g_synth_in, (uint16_t)(i * 2 + OBJ_VEL_X), 0x1111);  // 15517 must clear
        ft_wr16(g_synth_in, (uint16_t)(i * 2 + OBJ_VEL_Y), 0x2222);
    }
    ft_wr16(g_synth_in, 0x376, prio_n);
    for (int i = 0; i < (int)prio_n; i++) g_synth_in[0x378 + i] = prio_q[i];
    ft_fill_tail(g_synth_in);

    memset(g_vm_es_in, 0, sizeof(g_vm_es_in));   // yield carpet (op 0x00)
    memcpy(zone, g_vm_es_in, FT_VM_ZONE);

    memcpy(g_synth_orig, g_synth_in, sizeof(g_synth_orig));
    uint16_t regs[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    FtRegs in{};
    memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
    v2_fntest_call_sub_14207(g_scratch);
    return ft_synth_case_regs(FT_SUB_14207, in, 0, -1, group, st, diff_budget);
}

int ft_selftest_sub_14207() {
    FtSynthStats grid;
    long diff_budget = 24;
    static const uint8_t Q0[1] = { 0 };
    static const uint8_t Q2[2] = { 2, 0 };
    ft_synth_case_14207(1, 0, Q0, "one-obj", grid, diff_budget);
    ft_synth_case_14207(2, 0, Q0, "two-obj", grid, diff_budget);
    ft_synth_case_14207(2, 1, Q0, "queue-1", grid, diff_budget);
    ft_synth_case_14207(2, 2, Q2, "queue-2", grid, diff_budget);
    ft_synth_case_14207(0, 0, Q0, "empty", grid, diff_budget);
    // Divergence #28 directed: a ch3 opcode inside the FIRST drained object
    // skews the orig DI counter (INC of a slot address → CMP di,[0x376]
    // signed-less fails) — the orig drains ONLY queue[0] and leaves queue[1]
    // unexecuted. Marker: slot 2 runs an ACCUMULATING op 0x5B (+acc to a
    // field) — drained+main-loop double execution would differ from the
    // main-loop-only single hit. v2's register-model loop must match.
    {
        uint8_t* zone = (uint8_t*)v2_fntest_m2c_base() + (uint32_t)FT_VM_TESTSEG * 16;
        memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
        ft_wr16(g_synth_in, 0x372, 4);              // 2 objects
        for (uint32_t a = 0x2E5C; a <= 0x2E7C; a += 2) ft_wr16(g_synth_in, a, 0);
        ft_wr16(g_synth_in, 0x32F, 0);
        ft_wr16(g_synth_in, 0x42, 0xFFFF);
        ft_wr16(g_synth_in, 0x8A, 5);               // accumulator for the op5B marker
        for (int i = 0; i < 2; i++) {
            ft_wr16(g_synth_in, (uint16_t)(i * 2 + OBJ_CODE_SEG), FT_VM_TESTSEG);
            ft_wr16(g_synth_in, (uint16_t)(i * 2 + OBJ_PC), (uint16_t)(FT_VM_PC + i * 8));
            ft_wr16(g_synth_in, (uint16_t)(i * 2 + OBJ_FLAGS), 0x8000);
        }
        ft_wr16(g_synth_in, 0x376, 2);              // queue of TWO entries
        g_synth_in[0x378] = 0;                      // queue[0] = slot 0 (ch3 bytecode)
        g_synth_in[0x379] = 2;                      // queue[1] = slot 2 (marker)
        ft_fill_tail(g_synth_in);
        memset(g_vm_es_in, 0, sizeof(g_vm_es_in));  // yield carpet
        // slot 0 @ FT_VM_PC: op54 (ch3-style address load — di=slot addr) + yield
        g_vm_es_in[FT_VM_PC]     = 0x54;
        g_vm_es_in[FT_VM_PC + 1] = 0x10;            // idx → [0x10-0x6CBA] table word
        g_vm_es_in[FT_VM_PC + 2] = 0x00;            // yield
        // slot 2 @ FT_VM_PC+8: accumulating op5B marker + yield
        g_vm_es_in[FT_VM_PC + 8] = 0x5B;
        g_vm_es_in[FT_VM_PC + 9] = 0x12;            // idx → distinct field
        g_vm_es_in[FT_VM_PC + 10] = 0x00;           // yield
        memcpy(zone, g_vm_es_in, FT_VM_ZONE);
        memcpy(g_synth_orig, g_synth_in, sizeof(g_synth_orig));
        FtRegs in{};
        memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
        v2_fntest_call_sub_14207(g_scratch);
        ft_synth_case_regs(FT_SUB_14207, in, 0, -1, "queue-ch3-#28", grid, diff_budget);
    }
    fprintf(stderr,
        "FNSELFTEST-SUMMARY[sub_14207]: grid %ld/%ld — total cases=%ld fail=%ld%s\n",
        grid.pass, grid.cases, grid.cases, grid.fail,
        grid.fail ? "  <<< DIVERGENCE" : "");
    return grid.fail ? 1 : 0;
}

// ---- Unit 68: sub_112ae — viking-config chunk loads into DS -----------------
int ft_selftest_sub_112ae() {
    FtSynthStats grid;
    long diff_budget = 24;
    v2_set_m2c_base(v2_fntest_m2c_base());
    if (!v2_fntest_set_data_file("DATA.DAT") || !v2_fntest_set_data_file_v2("DATA.DAT")) {
        fprintf(stderr, "FNSELFTEST-SUMMARY[sub_112ae]: DATA.DAT missing — total cases=0 fail=1\n");
        return 1;
    }
    // Entries: chunk_id word + type byte; types map to DS dest 0x7F02+type*3.
    // Real small chunks: the game's own viking-config ids live in the level
    // tables; ids 3..6 are small palette-ish chunks — enough to walk the
    // loop, the palette-clear tail and the sub_10e99 jump.
    struct E { uint16_t id; uint8_t type; };
    static const E CASES[][3] = {
        { {3, 0}, {0xFFFF, 0}, {0, 0} },
        { {3, 0}, {4, 1}, {0xFFFF, 0} },
        { {5, 2}, {6, 40}, {0xFFFF, 0} },
    };
    static const int NC[] = { 1, 2, 2 };
    for (int c = 0; c < 3; c++) {
        memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
        uint16_t p = 0x25F6;
        for (int e = 0; e < NC[c]; e++) {
            ft_wr16(g_synth_in, p, CASES[c][e].id);
            g_synth_in[(uint16_t)(p + 2)] = CASES[c][e].type;
            p = (uint16_t)(p + 3);
        }
        ft_wr16(g_synth_in, p, 0xFFFF);
        ft_fill_tail(g_synth_in);
        memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
        uint16_t di_v2 = v2_fntest_call_sub_112ae(g_scratch, 0);
        FtRegs in{};
        ft_synth_case_regs(FT_SUB_112AE, in, di_v2, 5, "grid", grid, diff_budget);
    }
    fprintf(stderr,
        "FNSELFTEST-SUMMARY[sub_112ae]: grid %ld/%ld — total cases=%ld fail=%ld%s\n",
        grid.pass, grid.cases, grid.cases, grid.fail,
        grid.fail ? "  <<< DIVERGENCE" : "");
    return grid.fail ? 1 : 0;
}


// ---- Units 70/71: sub_1167a / sub_116ae — chunk-loader loops into segments --
// sub_1167a (unit 70): walks 6-byte level entries at [di+0x25F6]. Per entry:
//   chunk id → [si+0x12AD]; dest offset+1 → [si+0x12ED]; es = word_2B353
//   (ds:0x2E73); sub_10982 loads the chunk at es:bx; bx = 10982's out-di
//   (64K-wrap chain). Terminator 0xFFFF → di += 2, RETN.
// sub_116ae (unit 71): 5-byte entries. chunk id → [si+0x124D]; les di,
//   dword_2B359 (ds:0x2E79 lo / 0x2E7B hi) → [si+0x126D]/[si+0x128D];
//   sub_10982; sub_10e85 normalizes es += (di>>4)+1, di = 0; far pointer
//   written back. Terminator: RETN with NO di advance.
// Compare: full DS image (incl. sub_10982's ds:0x2BB4 header side effects —
// the ds_ctx channel), return di, and dest-zone CONTENT: the oracle writes
// the FT_DEST_SEG zone of m2c::m; v2 writes v2_sprite_shadow (70) /
// v2_vm_shadow_chunk (71) at the same relative offsets (both start the case
// 0xCC-baselined). word_10980 (cs-global) is saved/restored around the
// oracle and not re-compared: its write is inside sub_10982, covered
// byte-exact by unit 31 over the whole DATA.DAT corpus.
static bool ft_seg_loader_case(int which /*0=1167a,1=116ae*/,
                               const uint16_t* ids, int n, uint16_t di_in,
                               uint16_t start_off, const char* group,
                               FtSynthStats& st, long& diff_budget, bool porch)
{
    const char* nm = which ? "sub_116ae" : "sub_1167a";
    st.cases++;
    uint8_t* mbase = (uint8_t*)v2_fntest_m2c_base();
    uint8_t* dest_zone = mbase + (uint32_t)FT_DEST_SEG * 16;
    static uint8_t saved_dest[0x10000], orig_dest[0x10000];
    memcpy(saved_dest, dest_zone, 0x10000);
    memset(dest_zone, 0xCC, 0x10000);

    memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
    const uint16_t step = which ? 5 : 6;
    uint16_t p = (uint16_t)(di_in + 0x25F6);
    for (int e = 0; e < n; e++) { ft_wr16(g_synth_in, p, ids[e]); p = (uint16_t)(p + step); }
    ft_wr16(g_synth_in, p, 0xFFFF);
    if (which == 0) {
        ft_wr16(g_synth_in, DS_SEG_SPRITE, FT_DEST_SEG);   // word_2B353: sprite segment
    } else {
        ft_wr16(g_synth_in, DS_SEG_CHUNK, FT_DEST_SEG);   // chunk-buffer base segment
        ft_wr16(g_synth_in, DS_ANIM_PTR_LO, start_off);     // dword_2B359 lo (offset)
        ft_wr16(g_synth_in, DS_ANIM_PTR_HI, FT_DEST_SEG);   // dword_2B359 hi (segment)
    }
    for (int i = 0; i < 10; i++) g_synth_in[DS_CHUNK_HDR + i] = 0;  // 10982 header window
    ft_fill_tail(g_synth_in);

    // --- oracle ---
    uint16_t saved_10980 = v2_fntest_get_word_10980();
    memcpy(g_synth_orig, g_synth_in, sizeof(g_synth_orig));
    uint16_t regs[8] = { 0, 0, 0, 0, 0, di_in, 0, 0 };
    long esc0 = ft_ub_marks();
    v2_fntest_orig_isolated(v2_fntest_orig_fnptr(which ? FT_SUB_116AE : FT_SUB_1167A),
                            g_synth_orig, regs);
    v2_fntest_put_word_10980(saved_10980);
    if (ft_ub_marks() != esc0) {
        st.cases--;
        fprintf(stderr, "FNSELFTEST-UB[%s %s]: escaped\n", nm, group);
        memcpy(dest_zone, saved_dest, 0x10000);
        return true;
    }
    uint16_t orig_di_out = regs[5];
    memcpy(orig_dest, dest_zone, 0x10000);
    memcpy(dest_zone, saved_dest, 0x10000);

    // --- v2 ---
    uint8_t* v2_zone = which ? v2_vm_get_shadow_chunk()
                             : v2_fntest_sprite_shadow_win(0);
    memset(v2_zone, 0xCC, 0x10000);
    memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
    uint16_t di_v2 = which ? v2_fntest_call_sub_116ae(g_scratch, di_in)
                           : v2_fntest_call_sub_1167a(g_scratch, di_in);

    if (porch) v2_zone[(uint16_t)(start_off + 3)] ^= 0xFF;  // porch-protocol injection

    long diffs = 0;
    if (orig_di_out != di_v2) {
        if (diff_budget > 0) { diff_budget--;
            fprintf(stderr, "FNSELFTEST-DIFF[%s %s]: di orig=%04X v2=%04X\n",
                    nm, group, orig_di_out, di_v2); }
        diffs++;
    }
    for (uint32_t a = 0; a < 0x10000; a++) {
        if (orig_dest[a] == v2_zone[a]) continue;
        if (diff_budget > 0) { diff_budget--;
            fprintf(stderr, "FNSELFTEST-DIFF[%s %s]: dest+%04X orig=%02X v2=%02X\n",
                    nm, group, a, orig_dest[a], v2_zone[a]); }
        diffs++;
        if (diffs > 40) break;
    }
    for (uint32_t a = 0; a < 0x10000; a++) {
        if (g_scratch[a] == g_synth_orig[a]) continue;
        if (v2_fntest_ds_skip(a)) continue;
        if (diff_budget > 0) { diff_budget--;
            fprintf(stderr, "FNSELFTEST-DIFF[%s %s]: ds addr=%04X orig=%02X v2=%02X\n",
                    nm, group, a, g_synth_orig[a], g_scratch[a]); }
        diffs++;
    }
    if (diffs) { st.fail++; return false; }
    st.pass++; return true;
}

static int ft_selftest_seg_loader(int which) {
    const char* nm = which ? "sub_116ae" : "sub_1167a";
    FtSynthStats grid;
    long diff_budget = 24;
    v2_set_m2c_base(v2_fntest_m2c_base());
    if (!v2_fntest_set_data_file("DATA.DAT") || !v2_fntest_set_data_file_v2("DATA.DAT")) {
        fprintf(stderr, "FNSELFTEST-SUMMARY[%s]: DATA.DAT missing — total cases=0 fail=1\n", nm);
        return 1;
    }
    // ids 3..6: small palette-class chunks (same corpus the sub_112ae unit
    // uses — they fit the 64K dest window even chained; sizes verified by
    // unit 31 corpus). Case axes: empty table (sentinel-only), single, chain
    // of two (exercises the bx / dword_2B359 hand-over), di_in offset, and a
    // non-paragraph start_off for unit 71's sub_10e85 rounding.
    static const uint16_t C1[] = { 3 };
    static const uint16_t C2[] = { 3, 4 };
    static const uint16_t C3[] = { 5, 6 };
    ft_seg_loader_case(which, nullptr, 0, 0,     0,     "grid", grid, diff_budget, false);
    ft_seg_loader_case(which, C1, 1,    0,     0,     "grid", grid, diff_budget, false);
    ft_seg_loader_case(which, C2, 2,    0,     0,     "grid", grid, diff_budget, false);
    ft_seg_loader_case(which, C3, 2,    0x0C,  0,     "grid", grid, diff_budget, false);
    if (which == 1) {
        ft_seg_loader_case(which, C2, 2, 0,    0x123, "grid", grid, diff_budget, false);
        ft_seg_loader_case(which, C1, 1, 4,    0x0FF, "grid", grid, diff_budget, false);
    }
    // Porch protocol: corrupt one v2 dest byte — the compare MUST fail.
    {
        FtSynthStats porch;
        long pb = 2;
        ft_seg_loader_case(which, C1, 1, 0, 0, "porch", porch, pb, true);
        if (porch.fail != 1) {
            fprintf(stderr, "FNSELFTEST-SUMMARY[%s]: PORCH-PROTOCOL BROKEN "
                    "(injected corruption not caught, fail=%ld)\n", nm, porch.fail);
            grid.fail++;
        }
    }
    fprintf(stderr,
        "FNSELFTEST-SUMMARY[%s]: grid %ld/%ld — total cases=%ld fail=%ld%s\n",
        nm, grid.pass, grid.cases, grid.cases, grid.fail,
        grid.fail ? "  <<< DIVERGENCE" : "");
    return grid.fail ? 1 : 0;
}


// ---- Unit 72: sub_11b0b — portrait/sound sync (DS effects) ------------------
// Oracle renders via sub_11AA4 (drawBuffer only, no DS writes — verified);
// v2's render call is gated off by v2_vm_in_frame=false here. Compared: the
// skip/render condition pairs and the prev-tracking writes [0x42F+2k]/[0x423+2k].
// Portrait index domain kept to the real LUT range (small even values) — the
// oracle's sub_11AA4 reads ds:[table] rows derived from si.
int ft_selftest_sub_11b0b(uint32_t seed) {
    FtSynthStats grid, fuzz;
    long diff_budget = 24;
    v2_set_m2c_base(v2_fntest_m2c_base());
    static const uint16_t POR[] = { 0, 2, 4, 6 };
    static const uint16_t SND[] = { 0, 1 };
    // grid: viking 0 walks all (snd==psnd)x(por==ppor)x(snd 0/1) combos while
    // vikings 1-2 hold skip state; then a mixed case flips all three.
    for (int spm = 0; spm < 2; spm++)
    for (int ppm = 0; ppm < 2; ppm++)
    for (int sv = 0; sv < 2; sv++) {
        memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
        for (int vk = 0; vk < 3; vk++) {
            ft_wr16(g_synth_in, 0x0429 + vk * 2, 0);
            ft_wr16(g_synth_in, 0x042F + vk * 2, 0);
            ft_wr16(g_synth_in, 0x15AD + vk * 2, POR[vk]);
            ft_wr16(g_synth_in, 0x0423 + vk * 2, POR[vk]);
        }
        ft_wr16(g_synth_in, 0x0429, SND[sv]);
        ft_wr16(g_synth_in, 0x042F, spm ? SND[sv] : (uint16_t)(SND[sv] ^ 1));
        ft_wr16(g_synth_in, 0x15AD, POR[0]);
        ft_wr16(g_synth_in, 0x0423, ppm ? POR[0] : POR[2]);
        ft_fill_tail(g_synth_in);
        memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
        v2_fntest_call_sub_11b0b(g_scratch);
        FtRegs in{};
        ft_synth_case_regs(FT_SUB_11B0B, in, 0, -1, "grid", grid, diff_budget);
    }
    { // mixed: all three vikings dirty in different ways
        memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
        for (int vk = 0; vk < 3; vk++) {
            ft_wr16(g_synth_in, 0x0429 + vk * 2, vk == 1 ? 1 : 0);
            ft_wr16(g_synth_in, 0x042F + vk * 2, vk == 0 ? 1 : 0);
            ft_wr16(g_synth_in, 0x15AD + vk * 2, POR[vk]);
            ft_wr16(g_synth_in, 0x0423 + vk * 2, vk == 2 ? POR[3] : POR[vk]);
        }
        ft_fill_tail(g_synth_in);
        memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
        v2_fntest_call_sub_11b0b(g_scratch);
        FtRegs in{};
        ft_synth_case_regs(FT_SUB_11B0B, in, 0, -1, "grid", grid, diff_budget);
    }
    // fuzz: random tracking words, portrait idx bounded to the LUT domain
    FtRng rng(seed ? seed : 0x11B0B001u);
    for (int i = 0; i < 200; i++) {
        memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
        for (int vk = 0; vk < 3; vk++) {
            ft_wr16(g_synth_in, 0x0429 + vk * 2, (uint16_t)(rng.w() & 3));
            ft_wr16(g_synth_in, 0x042F + vk * 2, (uint16_t)(rng.w() & 3));
            ft_wr16(g_synth_in, 0x15AD + vk * 2, POR[rng.w() & 3]);
            ft_wr16(g_synth_in, 0x0423 + vk * 2, POR[rng.w() & 3]);
        }
        ft_fill_tail(g_synth_in);
        memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
        v2_fntest_call_sub_11b0b(g_scratch);
        FtRegs in{};
        ft_synth_case_regs(FT_SUB_11B0B, in, 0, -1, "fuzz", fuzz, diff_budget);
    }
    fprintf(stderr,
        "FNSELFTEST-SUMMARY[sub_11b0b]: grid %ld/%ld fuzz %ld/%ld — total cases=%ld fail=%ld%s\n",
        grid.pass, grid.cases, fuzz.pass, fuzz.cases,
        grid.cases + fuzz.cases, grid.fail + fuzz.fail,
        (grid.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (grid.fail + fuzz.fail) ? 1 : 0;
}


// ---- Unit 73: sub_10813 (+loc_107A2) — viking blink, pure DS effects --------
// Axes: byte_2AA9A gate; active slot signed CMP,6 (incl. 0xFFFF and 6);
// X/Y bounds at the exact +-0x0C/0x14C/0xB0 edges (JS = sign of the 16-bit
// wrap result); wy==0 fast path; prev switch (signed <6, [prev+0x16ED] sign);
// blink counter 0/1/2/3/0x15 (DEC + TEST&2 double-DEC branch). Objects table
// [vk+0x1A85] points at small even slots so [obj+0x44D]/[obj+0x114D] writes
// stay in distinct cells.
int ft_selftest_sub_10813(uint32_t seed) {
    FtSynthStats grid, fuzz;
    long diff_budget = 24;
    v2_set_m2c_base(v2_fntest_m2c_base());
    struct C { uint8_t flag; uint16_t act, prev, vx, wx, vy, wy, anim_prev, blink; };
    static const C CASES[] = {
        // flag gate off
        { 0, 0, 0, 100, 100, 100, 100, 0, 5 },
        // slot >= 6 (signed): clear path
        { 1, 6, 0, 100, 100, 100, 100, 0, 5 },
        // slot 0xFFFF (signed -1): enters bounds with wrap reads
        { 1, 0xFFFF, 0, 100, 100, 100, 100, 0, 5 },
        // X bounds edges: vx-wx+0xC sign flips at vx = wx-0xC
        { 1, 0, 0, (uint16_t)(200-0x0C), 200, 50, 0, 0, 5 },
        { 1, 0, 0, (uint16_t)(200-0x0D), 200, 50, 0, 0, 5 },
        { 1, 0, 0, (uint16_t)(200+0x14C), 200, 50, 0, 0, 5 },
        { 1, 0, 0, (uint16_t)(200+0x14D), 200, 50, 0, 0, 5 },
        // wy!=0: Y edges
        { 1, 0, 0, 200, 200, (uint16_t)(90-0x0C), 90, 0, 5 },
        { 1, 0, 0, 200, 200, (uint16_t)(90-0x0D), 90, 0, 5 },
        { 1, 0, 0, 200, 200, (uint16_t)(90+0xB0), 90, 0, 5 },
        { 1, 0, 0, 200, 200, (uint16_t)(90+0xB1), 90, 0, 5 },
        // prev switch: prev<6 anim>=0 (clears prev obj), anim<0 (skips), prev=0xFFFF
        { 1, 2, 0, 200, 200, 90, 90, 0, 0 },
        { 1, 2, 0, 200, 200, 90, 90, 0x8000, 0 },
        { 1, 2, 0xFFFF, 200, 200, 90, 90, 0, 0 },
        { 1, 2, 6, 200, 200, 90, 90, 0, 0 },
        // blink counter branches: 0 (skip), 1 (DEC->0, show), 2 (DEC->1, show),
        // 3 (DEC->2, TEST&2 -> hide + extra DEC), 0x15 (fresh)
        { 1, 0, 0, 200, 200, 90, 90, 0, 0 },
        { 1, 0, 0, 200, 200, 90, 90, 0, 1 },
        { 1, 0, 0, 200, 200, 90, 90, 0, 2 },
        { 1, 0, 0, 200, 200, 90, 90, 0, 3 },
        { 1, 0, 0, 200, 200, 90, 90, 0, 0x15 },
    };
    for (const C& c : CASES) {
        memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
        g_synth_in[0x25BA] = c.flag;
        ft_wr16(g_synth_in, 0x03C2, c.act);
        ft_wr16(g_synth_in, 0x03C4, c.prev);
        ft_wr16(g_synth_in, 0x03C6, c.blink);
        // world pos for the ACTIVE slot (wrap target address like the orig)
        ft_wr16(g_synth_in, (uint16_t)(c.act + OBJ_WORLD_X), c.vx);
        ft_wr16(g_synth_in, (uint16_t)(c.act + OBJ_WORLD_Y), c.vy);
        ft_wr16(g_synth_in, 0x0044, c.wx);
        ft_wr16(g_synth_in, 0x0046, c.wy);
        // prev viking anim + object slots
        ft_wr16(g_synth_in, (uint16_t)(c.prev + OBJ_ANIM_IDX), c.anim_prev);
        for (int vk = 0; vk < 3; vk++)
            ft_wr16(g_synth_in, OBJ_SUB_SLOT + vk * 2, (uint16_t)(0x10 + vk * 2));
        ft_wr16(g_synth_in, (uint16_t)(c.prev + OBJ_SUB_SLOT), 0x20);
        ft_wr16(g_synth_in, (uint16_t)(c.act + OBJ_SUB_SLOT), 0x24);
        ft_fill_tail(g_synth_in);
        memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
        v2_fntest_call_sub_10813(g_scratch);
        FtRegs in{};
        ft_synth_case_regs(FT_SUB_10813, in, 0, -1, "grid", grid, diff_budget);
    }
    // fuzz
    FtRng rng(seed ? seed : 0x10813001u);
    for (int i = 0; i < 400; i++) {
        memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
        g_synth_in[0x25BA] = (uint8_t)(rng.w() & 1);
        uint16_t act = (uint16_t)(rng.w() & 7); if (rng.w() & 8) act = 0xFFFF;
        uint16_t prv = (uint16_t)(rng.w() & 7); if (rng.w() & 8) prv = 0xFFFF;
        ft_wr16(g_synth_in, 0x03C2, act);
        ft_wr16(g_synth_in, 0x03C4, prv);
        ft_wr16(g_synth_in, 0x03C6, (uint16_t)(rng.w() & 0x1F));
        ft_wr16(g_synth_in, (uint16_t)(act + OBJ_WORLD_X), (uint16_t)rng.w());
        ft_wr16(g_synth_in, (uint16_t)(act + OBJ_WORLD_Y), (uint16_t)rng.w());
        ft_wr16(g_synth_in, 0x0044, (uint16_t)rng.w());
        ft_wr16(g_synth_in, 0x0046, (uint16_t)(rng.w() & ((rng.w() & 1) ? 0xFFFF : 0)));
        ft_wr16(g_synth_in, (uint16_t)(prv + OBJ_ANIM_IDX), (uint16_t)rng.w());
        for (int vk = 0; vk < 3; vk++)
            ft_wr16(g_synth_in, OBJ_SUB_SLOT + vk * 2, (uint16_t)(0x10 + vk * 2));
        ft_wr16(g_synth_in, (uint16_t)(prv + OBJ_SUB_SLOT), 0x20);
        ft_wr16(g_synth_in, (uint16_t)(act + OBJ_SUB_SLOT), 0x24);
        ft_fill_tail(g_synth_in);
        memcpy(g_scratch, g_synth_in, sizeof(g_scratch));
        v2_fntest_call_sub_10813(g_scratch);
        FtRegs in{};
        ft_synth_case_regs(FT_SUB_10813, in, 0, -1, "fuzz", fuzz, diff_budget);
    }
    fprintf(stderr,
        "FNSELFTEST-SUMMARY[sub_10813]: grid %ld/%ld fuzz %ld/%ld — total cases=%ld fail=%ld%s\n",
        grid.pass, grid.cases, fuzz.pass, fuzz.cases,
        grid.cases + fuzz.cases, grid.fail + fuzz.fail,
        (grid.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (grid.fail + fuzz.fail) ? 1 : 0;
}

int ft_selftest_clear(const FtClearSpec& cs, uint32_t seed) {
    FtSynthStats grid, fuzz;
    long diff_budget = 24;

    // Pattern images: pristine, all-00, all-FF, checkerboards.
    memcpy(g_synth_in, g_synth_base, sizeof(g_synth_in));
    ft_synth_case_clear(cs, "base", grid, diff_budget);
    memset(g_synth_in, 0x00, 0x10000);
    ft_synth_case_clear(cs, "zeros", grid, diff_budget);
    memset(g_synth_in, 0xFF, 0x10000);
    ft_synth_case_clear(cs, "ones", grid, diff_budget);
    memset(g_synth_in, 0xA5, 0x10000);
    ft_synth_case_clear(cs, "a5", grid, diff_budget);
    memset(g_synth_in, 0x5A, 0x10000);
    ft_synth_case_clear(cs, "5a", grid, diff_budget);

    // Seeded random images (full-DS noise).
    FtRng rng(seed);
    for (int i = 0; i < 64; i++) {
        for (uint32_t a = 0; a < 0x10000; a += 2) {
            uint16_t w = rng.w();
            g_synth_in[a] = (uint8_t)w; g_synth_in[a + 1] = (uint8_t)(w >> 8);
        }
        ft_synth_case_clear(cs, "fuzz", fuzz, diff_budget);
    }

    fprintf(stderr,
        "FNSELFTEST-SUMMARY[%s]: grid %ld/%ld, fuzz %ld/%ld — total cases=%ld fail=%ld%s\n",
        g_name[cs.id], grid.pass, grid.cases, fuzz.pass, fuzz.cases,
        grid.cases + fuzz.cases, grid.fail + fuzz.fail,
        (grid.fail + fuzz.fail) ? "  <<< DIVERGENCE" : "");
    return (grid.fail + fuzz.fail) ? 1 : 0;
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
    v2_fntest_ensure_drawinfo();     // oracle HUD/VGA inlines need a draw target
    v2_set_m2c_base(v2_fntest_m2c_base());   // v2 const-data reads (seg001 text
                                             // config, CS jump tables) — same
                                             // bytes the oracle reads
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
    if (all || strstr(env, "sub_10982")) { matched = true; rc |= ft_selftest_chunk(0x10982001u); }
    if (all || strstr(env, "sub_10cd8")) { matched = true; rc |= ft_selftest_rawchunk(0x10CD8001u); }
    if (all || strstr(env, "sub_10fe6")) { matched = true; rc |= ft_selftest_pal(FT_SUB_10FE6, 0x10FE6001u); }
    if (all || strstr(env, "sub_10ffc")) { matched = true; rc |= ft_selftest_pal(FT_SUB_10FFC, 0x10FFC001u); }
    for (const FtClearSpec& cs : FT_CLEARS)   // K1 units 35-43
        if (all || strstr(env, g_name[cs.id])) {
            matched = true;
            rc |= ft_selftest_clear(cs, 0xC1EA0000u + (uint32_t)cs.id);
        }
    for (const FtLeafSpec& ls : FT_LEAVES)    // K2a units 44-47
        if (all || strstr(env, g_name[ls.id])) {
            matched = true;
            rc |= ft_selftest_leaf(ls, 0x1EAF0000u + (uint32_t)ls.id);
        }
    if (all || strstr(env, "sub_11383")) { matched = true; rc |= ft_selftest_sub_11383(); }
    if (all || strstr(env, "sub_1133a")) { matched = true; rc |= ft_selftest_sub_1133a(); }
    if (all || strstr(env, "sub_1241e")) { matched = true; rc |= ft_selftest_sub_1241e(); }
    if (all || strstr(env, "sub_12515")) { matched = true; rc |= ft_selftest_textcfg(FT_SUB_12515, 0x12515001u); }
    if (all || strstr(env, "sub_12529")) { matched = true; rc |= ft_selftest_textcfg(FT_SUB_12529, 0x12529001u); }
    if (all || strstr(env, "sub_1450b")) { matched = true; rc |= ft_selftest_sub_1450b(); }
    if (all || strstr(env, "sub_13a0e")) { matched = true; rc |= ft_selftest_spawn(); }
    if (all || strstr(env, "sub_13ba5") || strstr(env, "sub_11446") || strstr(env, "sub_11569")) {
        matched = true; rc |= ft_selftest_spawn2();
    }
    if (all || strstr(env, "sub_15911")) { matched = true; rc |= ft_selftest_sub_15911(); }
    if (all || strstr(env, "sub_12549")) { matched = true; rc |= ft_selftest_sub_12549(); }
    if (all || strstr(env, "sub_11cbb")) { matched = true; rc |= ft_selftest_sub_11cbb(); }
    if (all || strstr(env, "sub_173c7")) { matched = true; rc |= ft_selftest_sub_173c7(); }
    if (all || strstr(env, "sub_14207")) { matched = true; rc |= ft_selftest_sub_14207(); }
    if (all || strstr(env, "sub_112ae")) { matched = true; rc |= ft_selftest_sub_112ae(); }
    if (all || strstr(env, "sub_1167a")) { matched = true; rc |= ft_selftest_seg_loader(0); }
    if (all || strstr(env, "sub_116ae")) { matched = true; rc |= ft_selftest_seg_loader(1); }
    if (all || strstr(env, "sub_11b0b")) { matched = true; rc |= ft_selftest_sub_11b0b(0); }
    if (all || strstr(env, "sub_10813")) { matched = true; rc |= ft_selftest_sub_10813(0); }
    if (all || strstr(env, "sub_15cef")) { matched = true; rc |= ft_selftest_bbox_vel(FT_SUB_15CEF, 0x15CEF001u); }
    if (all || strstr(env, "sub_15cf5")) { matched = true; rc |= ft_selftest_bbox_vel(FT_SUB_15CF5, 0x15CF5001u); }
    if (all || strstr(env, "sub_15de5")) { matched = true; rc |= ft_selftest_objscan_x(FT_SUB_15DE5, 0x15DE5001u); }
    if (all || strstr(env, "sub_15df2")) { matched = true; rc |= ft_selftest_objscan_x(FT_SUB_15DF2, 0x15DF2001u); }
    if (all || strstr(env, "sub_15fb1")) { matched = true; rc |= ft_selftest_objscan_y(FT_SUB_15FB1, 0x15FB1001u); }
    if (all || strstr(env, "sub_15fbe")) { matched = true; rc |= ft_selftest_objscan_y(FT_SUB_15FBE, 0x15FBE001u); }
    if (all || strstr(env, "sub_1603e")) { matched = true; rc |= ft_selftest_probe_1603e(0x1603E001u); }
    if (all || strstr(env, "sub_160cf")) { matched = true; rc |= ft_selftest_atpos(FT_SUB_160CF, 0x160CF001u); }
    if (all || strstr(env, "sub_15ae9")) { matched = true; rc |= ft_selftest_atpos(FT_SUB_15AE9, 0x15AE9001u); }
    if (all || strstr(env, "sub_1589b")) { matched = true; rc |= ft_selftest_atpos(FT_SUB_1589B, 0x1589B001u); }
    if (all || strstr(env, "sub_159c6")) { matched = true; rc |= ft_selftest_tilewalk(FT_SUB_159C6, 0x159C6001u); }
    if (all || strstr(env, "sub_159d3")) { matched = true; rc |= ft_selftest_tilewalk(FT_SUB_159D3, 0x159D3001u); }
    if (all || strstr(env, "sub_159df")) { matched = true; rc |= ft_selftest_tilewalk(FT_SUB_159DF, 0x159DF001u); }
    if (all || strstr(env, "sub_15a57")) { matched = true; rc |= ft_selftest_tilewalk(FT_SUB_15A57, 0x15A57001u); }
    if (all || strstr(env, "sub_15ac4")) { matched = true; rc |= ft_selftest_tilewalk(FT_SUB_15AC4, 0x15AC4001u); }
    if (all || strstr(env, "sub_158f5")) { matched = true; rc |= ft_selftest_sub_158f5(0x158F5001u); }
    if (all || strstr(env, "sub_1592d")) { matched = true; rc |= ft_selftest_sub_1592d(0x1592D001u); }
    if (all || strstr(env, "sub_15505")) { matched = true; rc |= ft_selftest_sub_15505(0x15505001u); }
    if (all || strstr(env, "sub_15517")) { matched = true; rc |= ft_selftest_sub_15517(0x15517001u); }
    if (all || strstr(env, "sub_1555c")) { matched = true; rc |= ft_selftest_coll_pass(FT_SUB_1555C, 0x1555C001u); }
    if (all || strstr(env, "sub_15569")) { matched = true; rc |= ft_selftest_coll_pass(FT_SUB_15569, 0x15569001u); }
    if (all || strstr(env, "sub_15530")) { matched = true; rc |= ft_selftest_coll_sweep(FT_SUB_15530, 0x15530001u); }
    if (all || strstr(env, "sub_15546")) { matched = true; rc |= ft_selftest_coll_sweep(FT_SUB_15546, 0x15546001u); }
    if (all || strstr(env, "sub_155d6")) { matched = true; rc |= ft_selftest_coll_check(FT_SUB_155D6, 0x155D6001u); }
    if (all || strstr(env, "sub_156c0")) { matched = true; rc |= ft_selftest_coll_check(FT_SUB_156C0, 0x156C0001u); }
    if (all || strstr(env, "sub_1584e")) { matched = true; rc |= ft_selftest_coll_dir(FT_SUB_1584E, 0x1584E001u); }
    if (all || strstr(env, "sub_157eb")) { matched = true; rc |= ft_selftest_coll_dir(FT_SUB_157EB, 0x157EB001u); }
    if (all || strstr(env, "sub_16235")) { matched = true; rc |= ft_selftest_colltab(FT_SUB_16235, 0x16235001u); }
    if (all || strstr(env, "sub_16243")) { matched = true; rc |= ft_selftest_colltab(FT_SUB_16243, 0x16243001u); }
    if (all || strstr(env, "sub_16390")) { matched = true; rc |= ft_selftest_sub_16390(0x16390001u); }
    if (all || strstr(env, "sub_15d3c")) { matched = true; rc |= ft_selftest_bbox2(FT_SUB_15D3C, 0x15D3C001u); }
    if (all || strstr(env, "sub_15d42")) { matched = true; rc |= ft_selftest_bbox2(FT_SUB_15D42, 0x15D42001u); }
    if (!matched) {
        fprintf(stderr, "FNSELFTEST: no registered function matches '%s'\n", env);
        return 1;
    }
    return rc;
}
