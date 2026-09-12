// v2_sc55.cpp — see v2_sc55.h.
#include "v2_sc55.h"
#include "v2_ui.h"
#include <SDL2/SDL.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

extern "C" {
int  nsc55_load(const char* dir);
int  nsc55_start(int ring_frames);
void nsc55_stop(void);
void nsc55_read(short* out, int frames);
int  nsc55_running(void);
void nsc55_post(uint8_t b);
int  nsc55_sample_rate(void);
const char* nsc55_model_name(void);
const char* nsc55_last_error(void);
}

static std::atomic<bool> g_on{false};
static std::mutex g_mix_mtx;             // start/stop (game thread) vs mix (audio thread): the ring lives only while running
static bool g_loaded = false;
static char g_status[160] = "SC-55: off";

bool v2_sc55_enabled() { return v2_options.sound_mode.load() == 2; }
bool v2_sc55_running() { return g_on.load(std::memory_order_acquire); }
const char* v2_sc55_status() { return g_status; }

static void post_sysex(const uint8_t* b, size_t n) { for (size_t i = 0; i < n; i++) nsc55_post(b[i]); }

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
        if (!nsc55_start(16384)) {
            snprintf(g_status, sizeof g_status, "SC-55: %s", nsc55_last_error());
            fprintf(stderr, "V2-SC55: %s\n", g_status);
            return false;
        }
    }
    // GS reset: the module in its known state before the game's first event
    static const uint8_t gs_reset[] = { 0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7 };
    post_sysex(gs_reset, sizeof gs_reset);
    snprintf(g_status, sizeof g_status, "SC-55: %s running", nsc55_model_name());
    g_on.store(true, std::memory_order_release);
    fprintf(stderr, "V2-SC55: started (%s)\n", nsc55_model_name());
    return true;
}

void v2_sc55_stop() {
    if (!g_on.load()) return;
    g_on.store(false, std::memory_order_release);
    { std::lock_guard<std::mutex> lk(g_mix_mtx); nsc55_stop(); }   // no callback inside nsc55_read while the ring goes away
    snprintf(g_status, sizeof g_status, "SC-55: off");
    fprintf(stderr, "V2-SC55: stopped\n");
}

void v2_sc55_midi(uint16_t status, uint16_t d1, uint16_t d2) {
    if (!g_on.load(std::memory_order_acquire)) return;
    const uint8_t fam = (uint8_t)(status & 0xF0);
    nsc55_post((uint8_t)status);
    nsc55_post((uint8_t)(d1 & 0x7F));
    if (fam != 0xC0 && fam != 0xD0) nsc55_post((uint8_t)(d2 & 0x7F));
}

// audio thread: pull the module's frames at its own rate, linear-resample to `rate`
bool v2_sc55_mix(int16_t* out, uint32_t frames, uint32_t rate) {
    if (!g_on.load(std::memory_order_acquire) || !rate) return false;
    std::unique_lock<std::mutex> lk(g_mix_mtx, std::try_to_lock);
    if (!lk.owns_lock()) return false;    // a start/stop in progress: this callback keeps the OPL buffer
    static short pull[512 * 2]; static int pulled = 0, pull_pos = 0;
    static short cur[2] = {0, 0}, nxt[2] = {0, 0}; static double frac = 0.0; static bool primed = false;
    const double step = (double)nsc55_sample_rate() / (double)rate;
    auto next_src = [&](short* dst) {
        if (pull_pos >= pulled) { nsc55_read(pull, 512); pulled = 512; pull_pos = 0; }
        dst[0] = pull[pull_pos * 2]; dst[1] = pull[pull_pos * 2 + 1]; pull_pos++;
    };
    if (!primed) { next_src(cur); next_src(nxt); primed = true; }
    for (uint32_t i = 0; i < frames; i++) {
        out[i * 2]     = (int16_t)(cur[0] + (nxt[0] - cur[0]) * frac);
        out[i * 2 + 1] = (int16_t)(cur[1] + (nxt[1] - cur[1]) * frac);
        frac += step;
        while (frac >= 1.0) { frac -= 1.0; cur[0] = nxt[0]; cur[1] = nxt[1]; next_src(nxt); }
    }
    return true;
}
