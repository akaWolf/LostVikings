// v2_native_opl.cpp — #61: native AIL sound channel.
//
// The converted AIL driver (seg002) talks to an AdLib chip through
// OUT 0x388/0x389 and paces itself with its own INT8 timer hook. In DOS
// both were hardware; here:
//   * OUT 0x388/0x389 land in v2_nopl_out() (routed from asm2C_OUT) and are
//     queued as (sample_timestamp, reg, val) commands;
//   * the timer tick is pumped ON THE GAME THREAD (the m2c machine is not
//     thread-safe) from frame_begin / blocking-loop ticks: v2_nopl_pump()
//     computes how many driver ticks are due by the AUDIO clock and calls
//     the driver's tick handler for each, stamping every OPL write with the
//     tick's sample position — so the music is sample-accurate even though
//     the pump itself is frame-granular (the DOS INT8 interleaving collapses
//     to batched, correctly-timestamped writes);
//   * the audio thread renders the Nuked OPL3 chip, applying queued writes
//     at their timestamps (standard OPL-player technique).
//
// Verification channel ("shadow-VGA for sound"): V2_OPL_TRACE=<file> logs
// every register write as "<tick> <reg> <val>" — byte-comparable across
// runs and against golden sequencer output.
//
// Activation: V2_NATIVE_AIL=1 in the environment. Off by default — the SDL
// adlmidi channel keeps playing as before.

#include <SDL.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" {
#include "../adlmidi/src/chips/nuked/nukedopl3.h"
}

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------
static opl3_chip    g_nopl_chip;
static bool         g_nopl_inited  = false;
static int          g_nopl_active  = -1;      // env gate, resolved lazily
static uint8_t      g_nopl_addr    = 0;       // OPL address latch (port 0x388)
static FILE*        g_nopl_trace   = nullptr;

// Driver timer: the AIL driver programs the PIT itself (divisor captured by
// asm2C_OUT hook → v2_nopl_set_pit_divisor). 0 = timer not installed yet.
static uint32_t     g_nopl_pit_divisor = 0;
static double       g_nopl_tick_hz     = 0.0;

// Sample clock (audio rate of the native channel).
static const uint32_t NOPL_RATE = 49716;      // native OPL3 rate (Nuked resamples)
static std::atomic<uint64_t> g_nopl_samples_played{0};   // audio thread advances
static uint64_t     g_nopl_ticks_done = 0;    // game thread: driver ticks issued
static uint64_t     g_nopl_cur_ts    = 0;     // timestamp stamped on writes of the tick being pumped

// Command ring: game thread produces, audio thread consumes.
struct NoplCmd { uint64_t ts; uint16_t reg; uint8_t val; };
static const size_t NOPL_RING = 1 << 14;
static NoplCmd      g_nopl_ring[NOPL_RING];
static std::atomic<size_t> g_nopl_wr{0}, g_nopl_rd{0};

extern "C" int v2_nopl_enabled(void) {
    if (g_nopl_active < 0) {
        const char* e = getenv("V2_NATIVE_AIL");
        g_nopl_active = (e && e[0] == '1') ? 1 : 0;
        if (g_nopl_active) {
            OPL3_Reset(&g_nopl_chip, NOPL_RATE);
            const char* t = getenv("V2_OPL_TRACE");
            if (t && t[0]) g_nopl_trace = fopen(t, "w");
            g_nopl_inited = true;
            fprintf(stderr, "v2_native_opl: ACTIVE (rate=%u, trace=%s)\n",
                    NOPL_RATE, g_nopl_trace ? "on" : "off");
        }
    }
    return g_nopl_active;
}

// ---------------------------------------------------------------------------
// OUT 0x388/0x389 routing (called from asm2C_OUT on the game thread)
// ---------------------------------------------------------------------------
extern "C" void v2_nopl_out(uint16_t port, uint8_t val) {
    if (!v2_nopl_enabled()) return;
    if (port == 0x388) { g_nopl_addr = val; return; }
    if (port != 0x389) return;
    // data write → queue (timestamped with the tick being pumped; writes
    // issued outside a tick — driver init, timbre install — use "now").
    uint64_t ts = g_nopl_cur_ts ? g_nopl_cur_ts
                                : g_nopl_samples_played.load(std::memory_order_relaxed);
    if (g_nopl_trace)
        fprintf(g_nopl_trace, "%llu %02X %02X\n",
                (unsigned long long)g_nopl_ticks_done, g_nopl_addr, val);
    size_t wr = g_nopl_wr.load(std::memory_order_relaxed);
    size_t nx = (wr + 1) & (NOPL_RING - 1);
    if (nx == g_nopl_rd.load(std::memory_order_acquire)) return;  // full: drop (never blocks game thread)
    g_nopl_ring[wr] = { ts, g_nopl_addr, val };
    g_nopl_wr.store(nx, std::memory_order_release);
}

// PIT divisor capture (asm2C_OUT hook, see asm.cpp): the AIL driver
// reprograms channel 0 for its tick rate.
extern "C" void v2_nopl_set_pit_divisor(uint32_t divisor) {
    if (!v2_nopl_enabled()) return;
    if (divisor == 0) divisor = 0x10000;
    g_nopl_pit_divisor = divisor;
    g_nopl_tick_hz = 1193182.0 / (double)divisor;
    fprintf(stderr, "v2_native_opl: PIT divisor=%u -> tick=%.2f Hz\n",
            divisor, g_nopl_tick_hz);
}

// ---------------------------------------------------------------------------
// tick pump (game thread): call the driver's INT8 handler for every tick due
// by the audio clock. The handler entry is provided by v2_vm glue (it needs
// the m2c _STATE machinery available only there).
// ---------------------------------------------------------------------------
extern "C" void v2_nopl_driver_tick(void);   // v2_vm.cpp glue → seg002 handler

extern "C" void v2_nopl_pump(void) {
    if (!v2_nopl_enabled() || g_nopl_tick_hz <= 0.0) return;
    double spt = (double)NOPL_RATE / g_nopl_tick_hz;   // samples per tick
    uint64_t played = g_nopl_samples_played.load(std::memory_order_relaxed);
    // small lead so freshly queued commands land slightly ahead of the
    // audio cursor instead of in its past
    uint64_t due = (uint64_t)((double)played / spt) + 1;
    int guard = 0;
    while (g_nopl_ticks_done < due && guard++ < 64) {
        g_nopl_cur_ts = (uint64_t)((double)g_nopl_ticks_done * spt);
        v2_nopl_driver_tick();
        g_nopl_ticks_done++;
    }
    g_nopl_cur_ts = 0;
}

// ---------------------------------------------------------------------------
// audio-thread render: apply queued writes at their timestamps, generate.
// ---------------------------------------------------------------------------
extern "C" void v2_nopl_render(int16_t* stereo, uint32_t frames) {
    if (!g_nopl_inited) { memset(stereo, 0, frames * 2 * sizeof(int16_t)); return; }
    uint64_t pos = g_nopl_samples_played.load(std::memory_order_relaxed);
    uint32_t donef = 0;
    while (donef < frames) {
        // apply all commands due at/before pos
        size_t rd = g_nopl_rd.load(std::memory_order_relaxed);
        uint32_t chunk = frames - donef;
        while (rd != g_nopl_wr.load(std::memory_order_acquire)) {
            const NoplCmd& c = g_nopl_ring[rd];
            if (c.ts > pos) {
                uint64_t gap = c.ts - pos;
                if (gap < chunk) chunk = (uint32_t)gap;
                break;
            }
            OPL3_WriteRegBuffered(&g_nopl_chip, c.reg, c.val);
            rd = (rd + 1) & (NOPL_RING - 1);
        }
        g_nopl_rd.store(rd, std::memory_order_release);
        if (chunk == 0) chunk = 1;
        OPL3_GenerateStream(&g_nopl_chip, stereo + donef * 2, chunk);
        donef += chunk;
        pos   += chunk;
    }
    g_nopl_samples_played.store(pos, std::memory_order_release);
}
