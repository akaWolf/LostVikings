// play.cpp — SDL audio device layer.
//
// #79: the AudioPool/adlmidi channel (per-side pools, XMID player threads,
// SPSC rings, deterministic-handle bookkeeping — the old "decoupled mixer")
// is REMOVED. Sound is the native interpreted AIL driver (v2_ail /
// v2_ail_interp) rendered by the Nuked OPL3 core in v2_native_opl. This file
// only owns the SDL audio device: open it, hand the obtained rate to the OPL
// mixer, and pump v2_nopl_mix() into the stream, keeping the long-lived
// diagnostics (SOUND-UNDERRUN, NO_AUDIO_WORK, V2_AUDIO_DUMP tap,
// clipping/overrun stats).

#include <SDL2/SDL.h>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <cstdint>

#include "play.h"

// #61 native AIL channel (v2_native_opl.cpp)
extern "C" void v2_nopl_set_mix_rate(uint32_t);
void v2_mt32_set_mix_rate(uint32_t);   // v2_mt32.cpp: the device rate v2_mt32_prepare opens Munt at
extern "C" void v2_nopl_mix(int16_t*, uint32_t);

static auto _sound_t0 = std::chrono::steady_clock::now();
static uint32_t _sound_now_ms() {
    using namespace std::chrono;
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now() - _sound_t0).count();
}

const uint32_t MYFREQ = 44100;
static SDL_AudioFormat myFormat;
static uint8_t myBuffer[16384];   // device mix buffer (cherry-pick 3f2114a size)

#include "v2_snes_sound.h"
#include "v2_sc55.h"       // UX stage 11: the SC-55 option
#include "v2_mt32.h"       // UX stage 11: the MT-32 world
static uint32_t g_snes_mix_rate = 0;   // UX stage 10: the device rate for the SNES resampler
void my_audio_callback(void* argument, Uint8* stream, int len)
{
    auto _cb_start = std::chrono::steady_clock::now();
    static int _cb_call_count = 0;
    static int _cb_overrun_count = 0;
    _cb_call_count++;

    // Underrun detector: a gap between callbacks much longer than the buffer
    // duration means the device starved (host hiccup) — audible silence.
    static auto _cb_last_start = std::chrono::steady_clock::time_point{};
    static int _cb_underrun_count = 0;
    static double _cb_underrun_total_ms = 0.0;
    const double expected_ms = ((double)len / 4.0) * 1000.0 / (double)MYFREQ;
    if (_cb_last_start.time_since_epoch().count() != 0) {
        double interval_ms = std::chrono::duration<double, std::milli>(_cb_start - _cb_last_start).count();
        if (interval_ms > expected_ms * 1.5) {
            double gap_ms = interval_ms - expected_ms;
            _cb_underrun_count++;
            _cb_underrun_total_ms += gap_ms;
            printf("[%ums] SOUND-UNDERRUN: gap=%.1fms (expected=%.1fms, +%.1fms missing) "
                   "underruns=%d total_silence=%.1fms\n",
                   _sound_now_ms(), interval_ms, expected_ms, gap_ms,
                   _cb_underrun_count, _cb_underrun_total_ms);
        }
    }
    _cb_last_start = _cb_start;

    const double _cb_budget_us = ((double)len / 4.0) * 1e6 / (double)MYFREQ;

    if (len > (int)sizeof(myBuffer)) {
        printf("SOUND ERROR, len = %d!\n", len);
        len = sizeof(myBuffer);
    }
    memset(myBuffer, 0, len);

    static int _no_audio_work = -1;
    if (_no_audio_work == -1) {
        _no_audio_work = (getenv("NO_AUDIO_WORK") != nullptr) ? 1 : 0;
        if (_no_audio_work) printf("[%ums] SOUND-DEBUG: NO_AUDIO_WORK=1 — silence-only mode\n", _sound_now_ms());
    }
    (void)argument;
    if (!_no_audio_work) {
        // Native AIL channel: the interpreted-driver OPL3 render. Inert until
        // the driver boots (no ticks pumped → the chip stays silent).
        v2_nopl_mix((int16_t*)myBuffer, (uint32_t)(len / (2 * sizeof(int16_t))));
        // UX stage 10: with the SNES sound option on, the console's mix
        // replaces the OPL render (the AIL machine keeps running unheard)
        v2_snes_sound_mix((int16_t*)myBuffer, (uint32_t)(len / (2 * sizeof(int16_t))), g_snes_mix_rate);
        // UX stage 11: the SC-55 option — the module's output replaces the OPL render
        v2_sc55_mix((int16_t*)myBuffer, (uint32_t)(len / (2 * sizeof(int16_t))), MYFREQ);
        v2_mt32_mix((int16_t*)myBuffer, (uint32_t)(len / (2 * sizeof(int16_t))), MYFREQ);   // UX stage 11: the MT-32 world (Munt) replaces the OPL render
    }

    // Diagnostic tap: V2_AUDIO_DUMP=<path> writes the exact device-bound mix
    // (raw s16le stereo at the obtained rate) — "what the user heard" for
    // offline comparison against the register-trace render.
    { static FILE* _dump = nullptr; static int _dump_init = 0;
      if (!_dump_init) { _dump_init = 1;
          const char* e = getenv("V2_AUDIO_DUMP");
          if (e && e[0]) _dump = fopen(e, "wb"); }
      if (_dump) fwrite(myBuffer, 1, len, _dump); }

    // Peak/clipping stats on the final mix.
    int16_t* samples = (int16_t*)myBuffer;
    int16_t peak_pos = 0, peak_neg = 0;
    int clipped = 0;
    int sample_count_int16 = len / 2;
    for (int s = 0; s < sample_count_int16; s++) {
        if (samples[s] > peak_pos) peak_pos = samples[s];
        if (samples[s] < peak_neg) peak_neg = samples[s];
        if (samples[s] == 32767 || samples[s] == -32768) clipped++;
    }
    static int _clip_log = 0;
    if (clipped && _clip_log++ < 20)
        printf("[%ums] SOUND-CLIP: %d/%d samples at rail (peak %+d/%d)\n",
               _sound_now_ms(), clipped, sample_count_int16, peak_pos, peak_neg);

    SDL_memcpy(stream, myBuffer, len);

    auto _cb_end = std::chrono::steady_clock::now();
    double _cb_us = std::chrono::duration<double, std::micro>(_cb_end - _cb_start).count();
    if (_cb_us > _cb_budget_us * 0.9) {
        _cb_overrun_count++;
        printf("[%ums] SOUND-CB-OVERRUN: took %.0fus (budget=%.0fus, %.1f%%) "
               "overruns=%d/total=%d\n",
               _sound_now_ms(), _cb_us, _cb_budget_us, 100.0 * _cb_us / _cb_budget_us,
               _cb_overrun_count, _cb_call_count);
    }
}

void sound_init()
{
    printf("init sound\n");
    static SDL_AudioSpec spec, obtained;

    if (SDL_Init(SDL_INIT_AUDIO) < 0)
        return;

    spec.freq = MYFREQ;
    spec.format = AUDIO_S16SYS;
    spec.channels = 2;
    spec.samples = 1024;  // cherry-pick 3f2114a — was 64, larger reduces audio glitches

    spec.callback = my_audio_callback;
    spec.userdata = nullptr;

    if (SDL_OpenAudio(&spec, &obtained) < 0) {
        fprintf(stderr, "Couldn't open audio: %s\n", SDL_GetError());
        return;
    }

    printf("SOUND-INIT: requested freq=%d format=0x%X channels=%d samples=%d\n",
           spec.freq, spec.format, spec.channels, spec.samples);
    printf("SOUND-INIT: obtained  freq=%d format=0x%X channels=%d samples=%d size=%u\n",
           obtained.freq, obtained.format, obtained.channels, obtained.samples, obtained.size);

    myFormat = obtained.format;
    if (myFormat != AUDIO_S16SYS)
        fprintf(stderr, "SOUND-INIT: WARNING obtained format 0x%X != S16SYS — "
                "native mix assumes s16 stereo\n", myFormat);

    // #61 native AIL channel renders at the obtained device rate.
    v2_nopl_set_mix_rate((uint32_t)obtained.freq);
    g_snes_mix_rate = (uint32_t)obtained.freq;
    v2_mt32_set_mix_rate((uint32_t)obtained.freq);   // the MT-32 world opens Munt at this rate before the game boots (v2_mt32_prepare)

    SDL_PauseAudio(0);
}
