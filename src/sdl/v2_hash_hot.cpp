// See v2_hash_hot.h — pure verify-hash kernels on a per-file -O2 island.
#include "v2_hash_hot.h"
#include <string.h>

static bool g_skip_bm[0x10000];
static bool g_ready = false;

// Non-skip dword runs on the i+=4 grid: [start, start+len) in bytes.
// Built once; the run walk replaces the per-dword branch of the old loop.
static uint32_t g_run_start[256];
static uint32_t g_run_len[256];
static int g_runs = 0;

void v2h_skip_build(const V2hRange* rs, int n) {
    if (g_ready) return;
    for (int k = 0; k < n; k++)
        for (uint32_t a = rs[k].start; a <= rs[k].end && a < 0x10000; a++)
            g_skip_bm[a] = true;
    // dword grid: the old hash tested skip(i) for i = 0,4,8,… — a dword is
    // skipped iff its FIRST byte is in the skip set. Reproduce exactly.
    g_runs = 0;
    uint32_t i = 0;
    while (i < 0x10000) {
        while (i < 0x10000 && g_skip_bm[i]) i += 4;
        if (i >= 0x10000) break;
        uint32_t s = i;
        while (i < 0x10000 && !g_skip_bm[i]) i += 4;
        if (g_runs >= 256) {
            // Overflow would silently DROP hash coverage — never weaken the
            // verify channel quietly. Loud abort instead.
            __builtin_trap();
        }
        g_run_start[g_runs] = s;
        g_run_len[g_runs] = i - s;
        g_runs++;
    }
    g_ready = true;
}

bool v2h_ds_skip(uint32_t i) {
    return i < 0x10000 ? g_skip_bm[i] : false;
}

uint32_t v2h_mem_hash(const uint8_t* p, uint32_t bytes) {
    uint32_t h = 0;
    for (uint32_t i = 0; i + 4 <= bytes; i += 4) {
        uint32_t w;
        memcpy(&w, p + i, 4);           // alias-safe; compiles to one mov
        h = h * 131 + w;
    }
    return h;
}

uint32_t v2h_ds_hash(const uint8_t* ds) {
    uint32_t h = 0;
    for (int r = 0; r < g_runs; r++) {
        const uint8_t* p = ds + g_run_start[r];
        uint32_t len = g_run_len[r];
        for (uint32_t i = 0; i < len; i += 4) {
            uint32_t w;
            memcpy(&w, p + i, 4);
            h = h * 131 + w;
        }
    }
    return h;
}
