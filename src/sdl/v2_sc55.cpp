// v2_sc55.cpp — see v2_sc55.h.
#include "v2_sc55.h"
#include "v2_ui.h"
#include <SDL2/SDL.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <cstddef>

extern "C" {
int  nsc55_load(const char* dir);
int  nsc55_start(int ring_frames);
void nsc55_stop(void);
void nsc55_read(short* out, int frames);
int  nsc55_running(void);
void nsc55_post(uint8_t b);
int  nsc55_sample_rate(void);
void nsc55_stats(uint32_t* uart_pending, int* rx_enabled, uint64_t* samples_out);
int  nsc55_lcd_enabled(void);
const char* nsc55_model_name(void);
const char* nsc55_last_error(void);
}

static std::atomic<bool> g_on{false};     // the module runs (ROMs loaded, work thread up)
static uint8_t g_fifo[0x10000]; static uint32_t g_fifo_wr = 0, g_fifo_rd = 0; static bool g_fifo_overflow = false;   // the MT-32 world's bytes while the firmware boots
static std::atomic<bool> g_ready{false};  // its firmware has booted (lcd_stub's signal) and the channel state is replayed
static int g_trace = -1;                 // V2_SC55_TRACE=1: boot / UART / ring diagnostics on stderr
static uint32_t g_t0 = 0;                // SDL ticks at start
static std::atomic<uint64_t> g_events{0}, g_frames_pulled{0};
static bool trace_on() { if (g_trace < 0) { const char* e = getenv("V2_SC55_TRACE"); g_trace = (e && *e) ? 1 : 0; } return g_trace == 1; }
static std::mutex g_mix_mtx;             // start/stop (game thread) vs mix (audio thread): the ring lives only while running
static bool g_loaded = false;
static char g_status[160] = "SC-55: off";

bool v2_sc55_enabled() { return v2_options.sound_mode.load() == 2; }
bool v2_sc55_running() { return g_on.load(std::memory_order_acquire); }
const char* v2_sc55_status() { return g_status; }

// ---------------------------------------------------------------------------
// Channel state — what the driver has told the channels so far (bank, program,
// controllers, pitch wheel, pressure), tracked ALWAYS, whatever the option:
// a module that comes up later (the option switched on mid-game, or its own
// boot time — 2 s on the SC-55 v1.21) must hear the current state, not just
// the notes from then on. Notes are not state (a sounding note is lost at the
// switch — inherent). CC 121 resets what the MIDI spec says it resets.
// ---------------------------------------------------------------------------
struct ChState { int16_t prog, bend, pressure; int16_t cc[128]; };
static ChState g_ch[16];
static bool g_ch_init = false;
static void ch_defaults(ChState& c, bool all) {
    if (all) { c.prog = -1; for (int i = 0; i < 128; i++) c.cc[i] = -1; }
    c.bend = -1; c.pressure = -1;
    c.cc[1] = -1; c.cc[11] = -1; for (int i = 64; i <= 69; i++) c.cc[i] = -1;   // Reset All Controllers (RP-015)
    c.cc[6] = -1; c.cc[38] = -1; c.cc[98] = -1; c.cc[99] = -1; c.cc[100] = -1; c.cc[101] = -1;
}
void v2_sc55_observe(uint16_t status, uint16_t d1, uint16_t d2) {
    if (!g_ch_init) { for (auto& c : g_ch) ch_defaults(c, true); g_ch_init = true; }
    ChState& c = g_ch[status & 0x0F]; d1 &= 0x7F; d2 &= 0x7F;
    switch (status & 0xF0) {
    case 0xB0:
        if (d1 == 121) ch_defaults(c, false);
        else if (d1 != 120 && d1 != 123) c.cc[d1] = (int16_t)d2;   // All Sound Off / All Notes Off carry no state
        break;
    case 0xC0: c.prog = (int16_t)d1; break;
    case 0xD0: c.pressure = (int16_t)d1; break;
    case 0xE0: c.bend = (int16_t)(d1 | (d2 << 7)); break;
    default: break;
    }
}
static void post3(uint8_t s, uint8_t a, uint8_t b) { nsc55_post(s); nsc55_post(a); nsc55_post(b); }
static int replay_channel_state() {
    int n = 0;
    if (!g_ch_init) return 0;
    for (int ch = 0; ch < 16; ch++) {
        const ChState& c = g_ch[ch]; const uint8_t st = (uint8_t)ch;
        if (c.cc[0] >= 0)  { post3(0xB0 | st, 0, (uint8_t)c.cc[0]); n++; }
        if (c.cc[32] >= 0) { post3(0xB0 | st, 32, (uint8_t)c.cc[32]); n++; }
        if (c.prog >= 0)   { nsc55_post(0xC0 | st); nsc55_post((uint8_t)c.prog); n++; }
        for (int i = 1; i < 128; i++) {
            if (i == 32 || i == 120 || i == 121 || i == 123 || c.cc[i] < 0) continue;
            post3(0xB0 | st, (uint8_t)i, (uint8_t)c.cc[i]); n++;
        }
        if (c.bend >= 0)     { post3(0xE0 | st, (uint8_t)(c.bend & 0x7F), (uint8_t)(c.bend >> 7)); n++; }
        if (c.pressure >= 0) { nsc55_post(0xD0 | st); nsc55_post((uint8_t)c.pressure); n++; }
    }
    return n;
}

// game thread, every tick: the boot gate — until the firmware is up the OPL
// stays audible (v2_sc55_mix falls through) and events are only tracked;
// then the channel state goes first, the live events follow.
void v2_sc55_service() {
    if (!g_on.load(std::memory_order_acquire) || g_ready.load(std::memory_order_acquire)) return;
    if (!nsc55_lcd_enabled()) return;
    // no channel-state replay: the MT-32 world's stream (queued in v2_sc55_uart_bytes since its own
    // start) opens with the MT-32 reset and carries every program and controller itself
    (void)replay_channel_state;
    g_ready.store(true, std::memory_order_release);
    fprintf(stderr, "V2-SC55: %s booted after %u ms — the MT-32 world's stream flows (%u bytes queued)\n", nsc55_model_name(), SDL_GetTicks() - g_t0, (g_fifo_wr - g_fifo_rd) & 0xFFFF);
}

bool v2_sc55_start() {
    if (g_on.load()) return true;
    if (!g_loaded) {
        v2_options_ensure_loaded();
        const char* dir = v2_options_sc55_roms[0] ? v2_options_sc55_roms : "roms/sc55";
        if (!nsc55_load(dir)) {
            snprintf(g_status, sizeof g_status, "SC-55: %s", nsc55_last_error());
            fprintf(stderr, "V2-SC55: %s\n", g_status);
            return false;
        }
        g_loaded = true;
        fprintf(stderr, "V2-SC55: ROM set %s from %s (%d Hz)\n", nsc55_model_name(), dir, nsc55_sample_rate());
    }
    {
        std::lock_guard<std::mutex> lk(g_mix_mtx);
        if (!nsc55_start(8192)) {           // the standalone's default ring: 8192 frames = 124 ms of latency
            snprintf(g_status, sizeof g_status, "SC-55: %s", nsc55_last_error());
            fprintf(stderr, "V2-SC55: %s\n", g_status);
            return false;
        }
    }
    // No GS reset here: the module comes up in its power-on (GS) state anyway,
    // and a GS reset sysex sent while the SC-55mk2 boots stalls it for ~5 s of
    // silence (boot probe 2026-09-10: with it the first sounded note came at
    // 5.0 s, without it at 0 ms). The channel state follows at v2_sc55_service.
    snprintf(g_status, sizeof g_status, "SC-55: %s running", nsc55_model_name());
    g_t0 = SDL_GetTicks();
    g_fifo_wr = g_fifo_rd = 0; g_fifo_overflow = false;
    g_ready.store(false, std::memory_order_release);
    g_on.store(true, std::memory_order_release);
    fprintf(stderr, "V2-SC55: started (%s)\n", nsc55_model_name());
    return true;
}

void v2_sc55_stop() {
    if (!g_on.load()) return;
    g_on.store(false, std::memory_order_release);
    g_ready.store(false, std::memory_order_release);
    { std::lock_guard<std::mutex> lk(g_mix_mtx); nsc55_stop(); }   // no callback inside nsc55_read while the ring goes away
    snprintf(g_status, sizeof g_status, "SC-55: off");
    fprintf(stderr, "V2-SC55: stopped\n");
}

// The MT-32 world's bytes (audio thread). Single producer of the module's UART: nothing else posts.
static void fifo_flush() {
    while (g_fifo_rd != g_fifo_wr) { nsc55_post(g_fifo[g_fifo_rd]); g_fifo_rd = (g_fifo_rd + 1) & 0xFFFF; }
}
void v2_sc55_uart_bytes(const uint8_t* b, size_t n) {
    if (!g_on.load(std::memory_order_acquire)) return;
    if (!g_ready.load(std::memory_order_acquire)) {                    // booting: keep everything in order for the flush
        for (size_t i = 0; i < n; i++) {
            const uint32_t nx = (g_fifo_wr + 1) & 0xFFFF;
            if (nx == g_fifo_rd) { if (!g_fifo_overflow) { g_fifo_overflow = true; fprintf(stderr, "V2-SC55: UART FIFO overflow while booting — bytes lost\n"); } return; }
            g_fifo[g_fifo_wr] = b[i]; g_fifo_wr = nx;
        }
        return;
    }
    if (g_fifo_rd != g_fifo_wr) fifo_flush();                          // the module came up: the backlog first
    for (size_t i = 0; i < n; i++) nsc55_post(b[i]);
}

void v2_sc55_midi(uint16_t status, uint16_t d1, uint16_t d2) {
    // UX stage 11 tail: retired. The SC-55 is no longer a GM reading of the FM driver's channel messages
    // (the music is written for the MT-32: wrong patch map, drum kits switching) — it is fed the MT-32
    // world's MPU stream through v2_sc55_uart_bytes. The lane keeps calling in (the observe/dump path).
    (void)status; (void)d1; (void)d2;
    if (!g_on.load(std::memory_order_acquire)) return;
    if (!g_ready.load(std::memory_order_acquire)) v2_sc55_service();   // the gate is checked on the event path too (the first music comes from the init chain before any tick)
    return;
}

// audio thread: pull the module's frames at its own rate, linear-resample to `rate`
bool v2_sc55_mix(int16_t* out, uint32_t frames, uint32_t rate) {
    if (!g_on.load(std::memory_order_acquire) || !rate) return false;
    std::unique_lock<std::mutex> lk(g_mix_mtx, std::try_to_lock);
    if (!lk.owns_lock()) return false;    // a start/stop in progress: this callback keeps the OPL buffer
    // The emulation is paced by this consumer (the work thread stalls when it
    // has filled the ring up to the read pointer), so the frames are pulled in
    // every case — while the firmware boots they are discarded and the OPL
    // buffer stays as it is (the boot gate, v2_sc55_service).
    const bool ready = g_ready.load(std::memory_order_acquire);
    static int16_t discard[2048 * 2];
    if (!ready) { if (frames > 2048) frames = 2048; out = discard; }
    static short pull[512 * 2]; static int pulled = 0, pull_pos = 0;
    static short cur[2] = {0, 0}, nxt[2] = {0, 0}; static double frac = 0.0; static bool primed = false;
    const double step = (double)nsc55_sample_rate() / (double)rate;
    auto next_src = [&](short* dst) {
        if (pull_pos >= pulled) {
            nsc55_read(pull, 512); pulled = 512; pull_pos = 0;
            const uint64_t fp = g_frames_pulled.fetch_add(512) + 512;
            if (trace_on() && (fp % (512 * 32)) == 0) {      // ~every 0.25 s of module time
                uint32_t pend = 0; int re = 0; uint64_t so = 0; nsc55_stats(&pend, &re, &so);
                int nz = 0; for (int i = 0; i < 512 * 2; i++) if (pull[i]) nz++;
                (void)re;
                fprintf(stderr, "V2-SC55-TRACE mix t=%ums pulled=%llu frames_out=%llu lead=%lld nonzero=%d uart_pending=%u\n",
                        SDL_GetTicks() - g_t0, (unsigned long long)fp, (unsigned long long)so, (long long)so - (long long)fp, nz, pend);
            }
        }
        dst[0] = pull[pull_pos * 2]; dst[1] = pull[pull_pos * 2 + 1]; pull_pos++;
    };
    if (!primed) { next_src(cur); next_src(nxt); primed = true; }
    for (uint32_t i = 0; i < frames; i++) {
        out[i * 2]     = (int16_t)(cur[0] + (nxt[0] - cur[0]) * frac);
        out[i * 2 + 1] = (int16_t)(cur[1] + (nxt[1] - cur[1]) * frac);
        frac += step;
        while (frac >= 1.0) { frac -= 1.0; cur[0] = nxt[0]; cur[1] = nxt[1]; next_src(nxt); }
    }
    return ready;
}
