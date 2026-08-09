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

// 4-lane ILP polynomial (user-approved change, 2026-08-14): the single
// h=h*131+w chain serializes on the multiply latency. Four independent
// accumulators over interleaved words + a final 131-combine keep the same
// hash class (linear polynomial, 32-bit) and the same hard guarantee — a
// single changed word changes the result (its weight is a power of 131,
// odd, nonzero mod 2^32). The VALUE differs from the old formula; that is
// fine: the channel only ever compares orig-vs-v2 computed by this same
// function inside one process — nothing is persisted.
static inline uint32_t hash_span(uint32_t h, const uint8_t* p, uint32_t bytes) {
    uint32_t i = 0;
    if (bytes >= 16) {
        uint32_t h0 = h, h1 = 0, h2 = 0, h3 = 0;
        for (; i + 16 <= bytes; i += 16) {
            uint32_t w0, w1, w2, w3;
            memcpy(&w0, p + i +  0, 4);
            memcpy(&w1, p + i +  4, 4);
            memcpy(&w2, p + i +  8, 4);
            memcpy(&w3, p + i + 12, 4);
            h0 = h0 * 131 + w0;
            h1 = h1 * 131 + w1;
            h2 = h2 * 131 + w2;
            h3 = h3 * 131 + w3;
        }
        h = ((h0 * 131 + h1) * 131 + h2) * 131 + h3;
    }
    for (; i + 4 <= bytes; i += 4) {
        uint32_t w;
        memcpy(&w, p + i, 4);
        h = h * 131 + w;
    }
    return h;
}

uint32_t v2h_mem_hash(const uint8_t* p, uint32_t bytes) {
    return hash_span(0, p, bytes);
}

uint32_t v2h_ds_hash(const uint8_t* ds) {
    uint32_t h = 0;
    for (int r = 0; r < g_runs; r++)
        h = hash_span(h, ds + g_run_start[r], g_run_len[r]);
    return h;
}
