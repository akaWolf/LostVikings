// v2_midi.cpp — see v2_midi.h.
#include "v2_midi.h"
#include "v2_sc55.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

extern "C" uint64_t v2_nopl_ticks_done(void);   // v2_native_opl.cpp: the driver ticks pumped so far
extern "C" double   v2_nopl_get_tick_hz(void);

namespace {
std::mutex g_mtx;                                   // the verify build's audible driver (the sink) dispatches on the audio thread
uint64_t (*g_clock)(void) = v2_nopl_ticks_done;     // the audible instance's tick counter (the sink installs its own)
struct Ev { uint64_t tick; uint8_t b[3]; uint8_t n; };
std::vector<Ev> g_dump; const char* g_dump_path = nullptr; int g_dump_state = -1; uint64_t g_first_tick = 0; bool g_have_first = false;
void put_vlq(std::vector<uint8_t>& o, uint32_t v) {
    uint8_t buf[5]; int n = 0;
    do { buf[n++] = (uint8_t)(v & 0x7F); v >>= 7; } while (v);
    while (n--) o.push_back((uint8_t)(buf[n] | (n ? 0x80 : 0)));
}
}

void v2_midi_set_clock(uint64_t (*ticks)(void)) { std::lock_guard<std::mutex> lk(g_mtx); g_clock = ticks; }

void v2_midi_event(uint16_t status, uint16_t d1, uint16_t d2) {
    const uint8_t fam = (uint8_t)(status & 0xF0);
    if (fam == 0xB0 && (d1 == 0x70 || d1 == 0x71 || d1 == 0x72)) return;   // AIL's protect / lock / bank: not MIDI
    if (v2_sc55_enabled()) v2_sc55_midi(status, d1, d2);
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_dump_state < 0) {
        g_dump_path = getenv("V2_MIDI_DUMP"); g_dump_state = (g_dump_path && *g_dump_path) ? 1 : 0;
        if (g_dump_state == 1) atexit(v2_midi_shutdown);   // every exit() path writes the file; the _exit paths call it explicitly
    }
    if (g_dump_state == 1) {
        uint64_t t = g_clock();
        if (!g_have_first) { g_first_tick = t; g_have_first = true; }
        Ev e; e.tick = t - g_first_tick; e.b[0] = (uint8_t)status; e.b[1] = (uint8_t)(d1 & 0x7F); e.b[2] = (uint8_t)(d2 & 0x7F);
        e.n = (fam == 0xC0 || fam == 0xD0) ? 2 : 3;
        g_dump.push_back(e);
    }
}

void v2_midi_shutdown(void) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_dump_state != 1 || !g_dump_path) return;
    g_dump_state = 2;
    double hz = v2_nopl_get_tick_hz(); if (hz <= 0.0) hz = 125.0;
    const uint16_t division = (uint16_t)(hz + 0.5);           // one driver tick = one MIDI tick; a quarter = one second
    std::vector<uint8_t> trk;
    put_vlq(trk, 0); trk.insert(trk.end(), {0xFF, 0x51, 0x03, 0x0F, 0x42, 0x40});   // tempo 1 000 000 us per quarter
    uint64_t last = 0;
    for (const Ev& e : g_dump) {
        put_vlq(trk, (uint32_t)(e.tick - last)); last = e.tick;
        trk.insert(trk.end(), e.b, e.b + e.n);
    }
    put_vlq(trk, 0); trk.insert(trk.end(), {0xFF, 0x2F, 0x00});
    FILE* f = fopen(g_dump_path, "wb");
    if (!f) { fprintf(stderr, "V2-MIDI: cannot write %s\n", g_dump_path); return; }
    const uint8_t hdr[14] = { 'M','T','h','d', 0,0,0,6, 0,0, 0,1, (uint8_t)(division >> 8), (uint8_t)division };
    fwrite(hdr, 1, 14, f);
    const uint32_t len = (uint32_t)trk.size();
    const uint8_t th[8] = { 'M','T','r','k', (uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len };
    fwrite(th, 1, 8, f); fwrite(trk.data(), 1, trk.size(), f); fclose(f);
    fprintf(stderr, "V2-MIDI: %zu events, %llu ticks at %.1f Hz -> %s\n", g_dump.size(), (unsigned long long)last, hz, g_dump_path);
}
