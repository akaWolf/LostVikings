// v2_native_opl.cpp — #61: native AIL sound channel, OPL3 model.
//
// The interpreted SBPFM.ADV driver (v2_ail_interp/v2_ail) addresses
// base+0/1 and base+2/3 (base = 0x220 from the DATA.DAT config tail) plus
// the SBPro mixer at base+4/5. The card model is a single OPL3 — what a
// Sound Blaster Pro 2 / SB16 provides and what DOSBox emulates:
// base+2/3 is the SECOND REGISTER BANK of one chip, not a separate right
// OPL2. This matters because the driver actively uses OPL3 features:
// it writes reg 0x105 = 1 (NEW bit) during init and streams hundreds of
// reg 0x104 writes (Connection Select) during playback — its timbres are
// FOUR-OPERATOR patches. The earlier dual-OPL2 (SBPro-1) model sent those
// into the second chip's timer register, so every 4-op instrument
// collapsed into two unrelated 2-op voices (user-audible: some
// instruments played wrong while others were fine).
// In DOS the ports were hardware; here:
//   * OUT lands in v2_nopl_sbpro_out(): per-bank register-index latches,
//     shadow register files (the fn65 detect probe READS timer status
//     back), and each data write is queued as (sample_ts, bank, reg, val);
//   * IN lands in v2_nopl_sbpro_in(): AdLib timer-status model over the
//     bank-0 shadow regs (reg4 bit0 T1-start → status 0xC0|0x40, bit7
//     reset → 0x00 — an OPL3 returns the same status on both port pairs)
//     and honest mixer readback — the model the standalone smoke rig
//     validated against the live detect code;
//   * the timer tick (fn67, the DOS INT8) runs ON THE AUDIO THREAD on the
//     sample clock: v2_nopl_mix cuts its chunks at the ticks' sample
//     positions and runs each tick right there, under the driver's CLI-model
//     lock, so the tick's OPL writes are stamped with its position and drain
//     into the same chunk — the note timing of the DOS machine. The game
//     thread's driver calls (starts, stops, fades, SFX) stamp their writes
//     with the played position and land in the next chunk (one buffer of
//     latency, like the DOS OUT after the device buffer). The game-thread
//     pump v2_nopl_pump() keeps the FRAME mode only: the deterministic
//     125/60-per-frame accumulator of the test / headless / lockstep worlds
//     (and of a game build without an audio device), where the ticks are a
//     pure function of the frame counter. Until 2026-10-07 the game-thread
//     pump ticked the real-time mode too, due by the played position: every
//     stamp was then at or before the mixer's cursor, the stamps were never
//     honoured, and a pump's 2–3 ticks collapsed onto the next buffer
//     boundary (measured on intro.inp: 0.7 % of the writes at their stamp,
//     the rest 4–30 ms late, 28 % of the mixer instants with ≥ 2 ticks merged
//     — audible as hurried / dragging parts and bunched short notes);
//   * the audio thread renders ONE Nuked OPL3 chip; C0 pan bits and 4-op
//     routing behave exactly as on the DOSBox reference.
//
// Tick rate comes from the driver descriptor ([desc+0x14]+5 = 125 Hz for
// SBPFM) via v2_nopl_set_tick_hz — there is no PIT here to intercept.
//
// Verification channel ("shadow-VGA for sound"): V2_OPL_TRACE=<file> logs
// every register write as "<tick> <chip> <reg> <val>" — byte-comparable
// across runs and against the standalone interpreter smoke rig.
//
// Activation: V2_NATIVE_AIL=1 in the environment (checked by v2_ail_native_on
// on the glue side; this file's entry points are inert until ticked/written).

#include "v2_midi.h"   // UX stage 11: V2_MIDI_DUMP is written before the _exit paths
#include "v2_mt32.h"   // UX stage 11: the MT-32 world pump
#include <SDL.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <unistd.h>
#include <cstring>

extern int v2_dbg_pre_vm_iter;   // game-frame counter (v2_vm.cpp), C++ linkage — declared once at file scope (clang rejects block externs inside extern "C" functions)
extern "C" {
#include "../adlmidi/src/chips/nuked/nukedopl3.h"
}

extern "C" void v2_ail_tick(void);          // v2_ail.cpp — one fn67 driver tick
extern "C" void* v2_ail_interp_lock(void);  // v2_ail_interp.cpp — the driver's CLI-model lock (recursive; every fn entry holds it)
extern "C" void  v2_ail_interp_unlock(void);

// SDL mixer rate. play.cpp sets the obtained device rate after SDL_OpenAudio;
// the 44100 default keeps HEADLESS builds (no play.cpp, no audio device)
// self-contained — there the queue is never drained, only the fn67 ticks
// matter (they advance the driver's DS state deterministically).
// g_device: the device is open (set before it starts) — without one there is
// no sample clock, and the ticks fall back to the frame mode (nopl_frame_mode).
static uint32_t g_rate = 44100;
static std::atomic<int> g_device{0};
extern "C" void v2_nopl_set_mix_rate(uint32_t rate) { if (rate) g_rate = rate; g_device.store(1, std::memory_order_release); }

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------
static opl3_chip    g_chip;                 // single OPL3 (bank 1 via 0x222/3)
static std::atomic<bool> g_inited{false};   // set by the first port write (the fn65 probe, game thread, under the driver lock); the mixer reads it
static FILE*        g_trace   = nullptr;
static int          g_trace_resolved = 0;

// The game-thread register file: the per-bank index latches, the two shadow
// register files (the detect probe reads them) and the SBPro mixer. One packed
// array (UX stage 8 tails): the state image carries it as the "OPLR" block, so
// a loaded state — a lockstep joiner, the host's LOAD, the debug slots — puts
// the chip back to the registers the copied world had (v2_nopl_regs_replay).
// ... plus the frame-mode tick accumulator at +776 (8-aligned): the phase of
// the sequencer's 125/60-per-frame counting is game state too — a joiner
// with another phase ticks the driver a frame early or late and its DS words
// drift from the host's.
alignas(8) static uint8_t g_oplr[2 + 2 * 256 + 1 + 256 + 5 + 8];
#define g_index        (g_oplr)                                            // [2] per-bank register-index latch
#define g_regs         (reinterpret_cast<uint8_t (*)[256]>(g_oplr + 2))    // [2][256] shadow register files
#define g_mixer_index  (g_oplr[2 + 2 * 256])
#define g_mixer        (g_oplr + 2 + 2 * 256 + 1)                          // [256]
#define g_tick_acc     (*reinterpret_cast<double*>(g_oplr + 776))          // the frame-mode tick accumulator
extern "C" uint8_t* v2_nopl_regs_data(uint32_t* size) { if (size) *size = (uint32_t)sizeof g_oplr; return g_oplr; }
static int          g_last_frame = -1;      // the render frame the accumulator was last advanced to (re-based after a restore)
static std::atomic<int> g_force_frame{0};   // a lockstep game: the sequencer ticks by frames on every peer (v2_nopl_force_frame_ticks)

// The tick clock. g_tick_hz and g_base_samples are set once at the driver's boot
// (v2_nopl_set_tick_hz, game thread) and read by whichever thread ticks — both
// under the driver lock; g_ticks_done / g_cur_ts / g_in_tick belong to the
// ticking thread (the audio thread in the real-time mode, the game thread in
// the frame mode) and are read elsewhere only under the lock (the MIDI lane's
// clock, the port model's stamps).
static double       g_tick_hz = 0.0;

// Sample clock: the audio thread advances it at the end of every callback; the
// game thread's writes are stamped with it, the real-time ticks are scheduled
// against it inside the callback (tick n at g_base_samples + n * samples per tick).
static std::atomic<uint64_t> g_samples_played{0};
static uint64_t     g_ticks_done = 0;
static uint64_t     g_cur_ts    = 0;        // ts stamped on the writes of the tick being run ...
static bool         g_in_tick   = false;    // ... while this is set (outside a tick a write takes the played position)

// Command ring: produced under the driver lock (the game thread's calls, the
// ticking thread's fn67), consumed by the audio thread. `tick` marks a write of
// a tick (the V2_NOPL_LATE diagnostic tells the two kinds apart).
struct NoplCmd { uint64_t ts; uint8_t chip; uint8_t reg; uint8_t val; uint8_t tick; };
static const size_t NOPL_RING = 1 << 14;
static NoplCmd      g_ring[NOPL_RING];
static std::atomic<size_t> g_wr{0}, g_rd{0};

static void nopl_lazy_init() {
    if (g_inited.load(std::memory_order_relaxed)) return;
    OPL3_Reset(&g_chip, g_rate);      // Nuked resamples its 49716 core to g_rate
    g_inited.store(true, std::memory_order_release);
    fprintf(stderr, "v2_native_opl: OPL3 ACTIVE (mix rate=%u)\n", g_rate);
}

// ---------------------------------------------------------------------------
// SBPro port model — called from the interpreter's OUT/IN hooks (game thread)
// ---------------------------------------------------------------------------
extern "C" void v2_nopl_sbpro_out(uint16_t port, uint8_t val) {
    if (port >= 0x220 && port <= 0x223) {
        nopl_lazy_init();
        int chip = (port - 0x220) >> 1;   // 0 = bank 0, 1 = bank 1 (reg 0x1xx)
        if ((port & 1) == 0) { g_index[chip] = (uint8_t)val; return; }
        uint8_t reg = g_index[chip];
        g_regs[chip][reg] = val;
        if (!g_trace_resolved) {
            g_trace_resolved = 1;
            const char* t = getenv("V2_OPL_TRACE");
            if (t && t[0]) g_trace = fopen(t, "w");
        }
        if (g_trace)
            fprintf(g_trace, "%llu %d %02X %02X\n",
                    (unsigned long long)g_ticks_done, chip, reg, val);
        // a tick's write carries the tick's position; a game-thread call's write the
        // played position (the start of the callback running or next to run — at or
        // before the mixer's cursor, so it is applied in the next chunk drained)
        uint64_t ts = g_in_tick ? g_cur_ts
                                : g_samples_played.load(std::memory_order_relaxed);
        size_t wr = g_wr.load(std::memory_order_relaxed);
        size_t nx = (wr + 1) & (NOPL_RING - 1);
        if (nx == g_rd.load(std::memory_order_acquire)) {
            // full: drop (never block the producer) — but LOUDLY: a
            // dropped register write audibly corrupts patches/notes.
            static uint64_t drops = 0;
            if ((++drops & (drops - 1)) == 0)   // log at 1,2,4,8,...
                fprintf(stderr, "v2_native_opl: RING FULL — %llu writes dropped so far\n",
                        (unsigned long long)drops);
            return;
        }
        g_ring[wr] = { ts, (uint8_t)chip, reg, val, (uint8_t)(g_in_tick ? 1 : 0) };
        g_wr.store(nx, std::memory_order_release);
        return;
    }
    if (port == 0x224) { g_mixer_index = val; return; }
    if (port == 0x225) {
        // SBPro mixer model: shadow registers only. Verified over full runs
        // (init + music + SFX + level): the driver touches ONLY reg 0x0A
        // (Mic volume, the fn65 detect probe writes 00/06); the volume
        // registers 0x22 (Master) / 0x26 (FM) are never programmed by the
        // game or the driver, so the hardware stays at its power-on default
        // — exactly what an unscaled mix models. Log anything beyond the
        // known probe so future titles/tracks can't silently need more.
        if (g_mixer_index != 0x0A && g_mixer[g_mixer_index] != val)
            fprintf(stderr, "v2_native_opl: MIXER[%02X] <- %02X (beyond detect probe)\n",
                    g_mixer_index, val);
        g_mixer[g_mixer_index] = val;
        return;
    }
    // Anything else the driver touches is a survey gap — log, don't guess.
    static int warn = 0;
    if (warn++ < 8) fprintf(stderr, "v2_native_opl: OUT %04X <- %02X (unmodeled port)\n", port, val);
}

// After a state image replaced the register file (UX stage 8 tails): send it
// to the chip through the same port model, so the audible chip holds the
// registers of the copied world — operators and timbres first, the key-on
// registers B0..B8 last (a note starts on a configured operator pair), the
// index latch restored at the end; the mixer's one live register (0x0A).
extern "C" void v2_nopl_regs_replay(void) {
    // under the driver lock: the port writes below enter the command ring,
    // whose producer side is the lock's (the audio thread's ticks write it too)
    v2_ail_interp_lock();
    uint8_t saved[sizeof g_oplr];
    memcpy(saved, g_oplr, sizeof g_oplr);
    const uint8_t (*regs)[256] = reinterpret_cast<const uint8_t (*)[256]>(saved + 2);
    for (int chip = 0; chip < 2; chip++) {
        const uint16_t pi = (uint16_t)(0x220 + chip * 2), pd = (uint16_t)(pi + 1);
        for (int reg = 1; reg < 256; reg++) {
            if (reg >= 0xB0 && reg <= 0xB8) continue;
            v2_nopl_sbpro_out(pi, (uint8_t)reg); v2_nopl_sbpro_out(pd, regs[chip][reg]);
        }
        for (int reg = 0xB0; reg <= 0xB8; reg++) { v2_nopl_sbpro_out(pi, (uint8_t)reg); v2_nopl_sbpro_out(pd, regs[chip][reg]); }
        v2_nopl_sbpro_out(pi, saved[chip]);                 // the index latch as the image had it
    }
    v2_nopl_sbpro_out(0x224, 0x0A); v2_nopl_sbpro_out(0x225, saved[2 + 2 * 256 + 1 + 0x0A]);
    v2_nopl_sbpro_out(0x224, saved[2 + 2 * 256]);
    // the accumulator came with the image; the frame it was advanced to is
    // this world's current one (the image was taken at a main read, before
    // the frame's first vsync wait — nothing is pending on either side)
    { extern int v2_render_frame; g_last_frame = v2_render_frame; }
    v2_ail_interp_unlock();
}

extern "C" void v2_nopl_force_frame_ticks(void) {
    if (!g_force_frame.load(std::memory_order_relaxed)) fprintf(stderr, "v2_native_opl: lockstep — frame-accumulator tick mode on every peer\n");
    g_force_frame.store(1, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// (#83) SINK port model — audio thread. Same SBPro translation, but the
// register write goes STRAIGHT into the chip (we are already on the mixer
// thread, right before sample generation) — no ring, no timestamps, sample-
// accurate like DOSBox. Separate index latches/regs: the game-thread model
// above belongs to the (now silent) verify pair and the V2_ONLY world.
// ---------------------------------------------------------------------------
static uint8_t g_sink_index[2] = {0, 0};
static uint8_t g_sink_regs[2][256] = {{0}};
static uint8_t g_sink_mixer_index = 0;
static uint8_t g_sink_mixer[256] = {0};

extern "C" void v2_nopl_sink_out(uint16_t port, uint8_t val) {
    if (port >= 0x220 && port <= 0x223) {
        nopl_lazy_init();
        int chip = (port - 0x220) >> 1;
        if ((port & 1) == 0) { g_sink_index[chip] = (uint8_t)val; return; }
        uint8_t reg = g_sink_index[chip];
        g_sink_regs[chip][reg] = val;
        if (!g_trace_resolved) {
            g_trace_resolved = 1;
            const char* t = getenv("V2_OPL_TRACE");
            if (t && t[0]) g_trace = fopen(t, "w");
        }
        if (g_trace)
            fprintf(g_trace, "%llu %d %02X %02X\n",
                    (unsigned long long)g_ticks_done, chip, reg, val);
        OPL3_WriteRegBuffered(&g_chip, (uint16_t)((chip << 8) | reg), val);
        return;
    }
    if (port == 0x224) { g_sink_mixer_index = val; return; }
    if (port == 0x225) {
        if (g_sink_mixer_index != 0x0A && g_sink_mixer[g_sink_mixer_index] != val)
            fprintf(stderr, "v2_native_opl: SINK MIXER[%02X] <- %02X (beyond detect probe)\n",
                    g_sink_mixer_index, val);
        g_sink_mixer[g_sink_mixer_index] = val;
        return;
    }
    static int warn = 0;
    if (warn++ < 8) fprintf(stderr, "v2_native_opl: SINK OUT %04X <- %02X (unmodeled port)\n", port, val);
}

extern "C" uint8_t v2_nopl_sink_in(uint16_t port) {
    if (port >= 0x220 && port <= 0x223) {
        uint8_t ctl = g_sink_regs[0][4];
        if (ctl & 0x80) return 0x00;
        if (ctl & 0x01) return 0xC0 | 0x40;
        return 0x00;
    }
    if (port == 0x225) return g_sink_mixer[g_sink_mixer_index];
    return 0xFF;
}

extern "C" double   v2_nopl_get_tick_hz(void) { return g_tick_hz; }
extern "C" uint64_t v2_nopl_ticks_done(void)  { return g_ticks_done; }   // the MIDI lane's clock (v2_midi.cpp)
extern "C" uint32_t v2_nopl_get_rate(void)    { return g_rate; }

extern "C" uint8_t v2_nopl_sbpro_in(uint16_t port) {
    if (port >= 0x220 && port <= 0x223) {
        // AdLib detect model (validated by the smoke rig): after T1 start
        // (reg4 bit0, mask clear) status reads 0xC0|0x40; after reset (bit7)
        // it reads 0x00. Timer expiry is immediate in this model — the
        // driver's 6/8-IN delay loops satisfy the settle time. An OPL3 has
        // ONE status register (bank-0 timers) visible on both port pairs.
        uint8_t ctl = g_regs[0][4];
        if (ctl & 0x80) return 0x00;
        if (ctl & 0x01) return 0xC0 | 0x40;
        return 0x00;
    }
    if (port == 0x225) return g_mixer[g_mixer_index];
    return 0xFF;
}

// ---------------------------------------------------------------------------
// tick pump (game thread)
// ---------------------------------------------------------------------------
// Base of the tick clock: samples already played when the driver booted.
// Without it the sequencer would owe ticks for all the audio time that
// passed before the first music start and slam through them at once.
static uint64_t g_base_samples = 0;

extern "C" void v2_nopl_set_tick_hz(double hz) {
    // under the driver lock: the audio thread schedules its ticks from these two
    // (v2_ail_boot calls this between two driver calls, outside their locks)
    v2_ail_interp_lock();
    g_base_samples = g_samples_played.load(std::memory_order_relaxed);
    g_tick_hz = hz;
    v2_ail_interp_unlock();
    fprintf(stderr, "v2_native_opl: tick rate %.1f Hz (driver descriptor)\n", hz);
}

// Legacy PIT capture (asm2C_OUT hook in asm.cpp): in default/verify mode the
// m2c seg002 timer service reprograms PIT channel 0 with its tick divisor.
// The interpreted-driver path derives the rate from the descriptor instead
// (v2_nopl_set_tick_hz above); this entry just logs the observation — the
// pump only runs once the v2_ail glue boots, which never happens in the
// modes where seg002 programs a PIT.
extern "C" void v2_nopl_set_pit_divisor(uint32_t divisor) {
    if (divisor == 0) divisor = 0x10000;
    fprintf(stderr, "v2_native_opl: PIT divisor=%u observed (%.2f Hz)\n",
            divisor, 1193182.0 / (double)divisor);
}

// Legacy AdLib port capture (asm2C_OUT hook): the m2c seg002 path emits
// OUT 0x388/0x389 when its (never-executed-here) OPL code runs. The
// interpreted driver uses the SBPro ports above instead; keep the symbol as
// an explicit no-op so the asm.cpp routing stays documented and linkable.
extern "C" void v2_nopl_out(uint16_t port, uint8_t val) {
    (void)port; (void)val;
}

// Tick pacing. The DOS INT8 fired at [desc+0x14]+5 Hz by REAL TIME regardless
// of the frame rate — and the V2_ONLY game loop is NOT 60 Hz (the phase
// chain blocks on VSYNC waits inside sub_10130; ~19 game frames/s, see
// v2_main.cpp timing notes). Pacing ticks per frame therefore ran the
// sequencer ~3x slow (user-audible: background music crawled while short
// SFX still sounded okay). Default (the real-time mode): the ticks run on the
// AUDIO thread, on the sample clock — samples the device has consumed ARE
// wall time — each at its exact sample inside v2_nopl_mix (nopl_ticks_on_clock
// below); this game-thread pump then only keeps the quit choke.
//
// The frame mode (V2_AIL_FRAME_TICKS=1, a lockstep game, the test / headless
// builds and a game build without an audio device) ticks HERE, 125/60 per
// frame: a fully deterministic driver DS state (the ticks are a pure function
// of the frame counter), at the cost of tempo tracking the frame rate.
static const double NOPL_FRAME_HZ = 60.0;
// (g_tick_acc lives in g_oplr, see the register file above)

static int nopl_frame_mode(void);
extern "C" void v2_ail_sink_pump(uint64_t);   // (#83) audible sink driver (v2_ail.cpp)

// debug (V2_NOPL_LATE=1): the timing of the queued writes against the mixer — a tick's writes
// must be applied exactly at the tick's sample (late = 0, one tick per mixer instant); a call's
// writes land in the next chunk drained (late by up to one device buffer). The audio thread
// prints a summary line every 1000 callbacks.
static int      g_late_on = -1;
static uint64_t g_late_cbs = 0;
static uint64_t g_late_tick_applied = 0, g_late_tick_late = 0, g_late_tick_max = 0;   // tick writes: applied, applied after their stamp, the worst lateness (samples)
static uint64_t g_late_call_applied = 0, g_late_call_hist[8] = {0};                  // call writes, late: 0 | ≤4 | ≤8 | ≤12 | ≤16 | ≤24 | ≤32 | >32 ms
static uint64_t g_late_instants[2] = {0, 0};                                         // mixer instants with tick writes of 1 | ≥2 distinct stamps
static int late_on(void) { if (g_late_on < 0) { const char* e = getenv("V2_NOPL_LATE"); g_late_on = (e && e[0] == '1') ? 1 : 0; } return g_late_on; }
#if defined(V2_ONLY) && !defined(HEADLESS)
void v2_only_clean_exit(const char* why);      // v2_main.cpp — C++ linkage, declared here at file scope:
                                               // a block-scope extern inside the C-linkage pump below would be C
#endif

extern "C" void v2_nopl_pump(void) {
#if defined(V2_ONLY) && !defined(HEADLESS)
    // №59 windowed enforcement: when the presenter loop has already left on
    // need_quit (window close, or the max-frames fallback of v2_main.cpp for a game
    // thread that never reaches a frame boundary), the game thread can sit inside
    // a blocking wait loop forever — every such loop pumps, so this is the
    // single choke point. Mirror the headless clean exit. The --max-frames stop itself
    // is taken at the frame boundary (v2_phase_post_flip3 / v2_blocking_loop_tick), the
    // same point as the headless builds', so a replay's tail does not depend on where
    // this pump happened to run.
    {
        extern bool need_quit;
        // v2_dbg_pre_vm_iter: file-scope extern (top of file)
        if (need_quit) v2_only_clean_exit("quit");
    }
#endif
    if (!nopl_frame_mode()) return;            // the real-time mode ticks on the audio thread (nopl_ticks_on_clock)
    if (g_tick_hz <= 0.0) return;
    double spt = (double)g_rate / g_tick_hz;   // samples per tick (queue ts)
    int guard = 0;
    // Deterministic invariant: ticks accrue per FRAME COUNTER delta,
    // not per pump call — wait loops pump once per iteration and the
    // iteration count depends on pacing/scheduling (a NOVSYNC spin ran
    // thousands of iterations per frame and multiplied the ticks).
    // Counter-based accrual keeps the tick schedule a pure function of
    // the frame number on every pacing mode.
    // v2_render_frame (#32) is the DETERMINISTIC per-game-frame index;
    // v2_dbg_pre_vm_iter is documented-inflated by the blocking-loop
    // wall-clock spins and multiplied the ticks ~600x under NOVSYNC.
    extern int v2_render_frame;
    int& last_frame = g_last_frame;
    int cur = v2_render_frame;
    {   // V2_TICKDBG=1: pump/counter forensics (one line per 1000 pumps)
        static int dbg = -1; static long pumps = 0;
        if (dbg < 0) dbg = getenv("V2_TICKDBG") ? 1 : 0;
        if (dbg && (++pumps % 1000) == 1) {
            // v2_dbg_pre_vm_iter: file-scope extern (top of file)
            fprintf(stderr, "TICKDBG pumps=%ld rframe=%d pvi=%d ticks=%llu acc=%.2f hz=%.1f\n",
                    pumps, cur, v2_dbg_pre_vm_iter,
                    (unsigned long long)g_ticks_done, g_tick_acc, g_tick_hz);
        }
    }
    if (last_frame < 0) last_frame = cur;
    if (cur != last_frame) {
        g_tick_acc += (double)(cur - last_frame) * g_tick_hz / NOPL_FRAME_HZ;
        last_frame = cur;
    }
    while (g_tick_acc >= 1.0 && guard++ < 64) {
        g_tick_acc -= 1.0;
        g_cur_ts = (uint64_t)((double)g_ticks_done * spt);
        g_in_tick = true;
        v2_ail_tick();
        g_in_tick = false;
        g_ticks_done++;
    }
    g_cur_ts = 0;
}

// The real-time mode's ticks — audio thread, inside v2_nopl_mix, at the start of
// a chunk beginning at sample `pos`: every tick due at or before pos runs now
// (tick n is due at g_base_samples + n * samples per tick; its writes, stamped
// with that position, are drained into this very chunk, ahead of the chunk's
// samples), and the chunk is cut at the next tick's position, so that tick runs
// exactly at its sample on the next round. Returns the chunk length to render.
// Under the driver lock, like every fn entry: a game-thread call is atomic
// against the tick, as the PUSHF/CLI of the DOS API was against its INT8; the
// lock is held by that thread only for the length of one driver call (or a
// state image's copy), the audio thread's wait stays far below its budget.
// A tick debt (more than 64 ticks owed — only if the sample clock jumped, it
// does not advance while no callback runs) slides the base instead of bursting:
// the clean PAUSE of the old pump.
static uint32_t nopl_ticks_on_clock(uint64_t pos, uint32_t chunk) {
    if (nopl_frame_mode()) return chunk;      // the frame mode ticks on the game thread (v2_nopl_pump)
    v2_ail_interp_lock();
    if (g_tick_hz > 0.0) {
        const double spt = (double)g_rate / g_tick_hz;
        uint64_t next = g_base_samples + (uint64_t)((double)g_ticks_done * spt);
        if (pos > next && (uint64_t)((double)(pos - next) / spt) > 64) {
            const uint64_t excess = (uint64_t)((double)(pos - next) / spt) - 64;
            g_base_samples += (uint64_t)((double)excess * spt);
            next = g_base_samples + (uint64_t)((double)g_ticks_done * spt);
            fprintf(stderr, "v2_native_opl: tick debt %llu — paused (base slid)\n", (unsigned long long)excess);
        }
        int guard = 0;
        while (next <= pos && guard++ < 96) {
            g_cur_ts = next;
            g_in_tick = true;
            v2_ail_tick();
            g_in_tick = false;
            g_ticks_done++;
            next = g_base_samples + (uint64_t)((double)g_ticks_done * spt);
        }
        g_cur_ts = 0;
        if (next > pos && next - pos < chunk) chunk = (uint32_t)(next - pos);
    }
    v2_ail_interp_unlock();
    return chunk;
}

// Tick pacing mode, decided once.
// (#83) default/verify: ALWAYS frame-paced again — real+shadow are the
// deterministic verify pair and no longer feed the chip (the audible path
// is the sink instance, ticked on the sample clock in v2_nopl_mix).
// V2_ONLY: the single (audible) instance ticks on the audio clock (the audio
// thread, nopl_ticks_on_clock); V2_AIL_FRAME_TICKS=1 keeps its deterministic
// frame mode for replays, and a build without an audio device (HEADLESS, or a
// device that failed to open) has no sample clock and takes the frame mode too.
static int nopl_frame_mode(void) {
    if (g_force_frame.load(std::memory_order_relaxed)) return 1;    // UX stage 8 tails: a network game ticks by frames (the audio clock is not shared)
    static int frame_mode = -1;
    if (frame_mode < 0) {
        const char* why = "";
#ifdef V2_ONLY
        const char* e = getenv("V2_AIL_FRAME_TICKS");
        frame_mode = (e && e[0] == '1') ? 1 : 0;
        if (frame_mode) why = " (V2_AIL_FRAME_TICKS)";
        else if (!g_device.load(std::memory_order_acquire)) { frame_mode = 1; why = " (no audio device)"; }
#else
        frame_mode = 1;
#endif
        if (frame_mode) fprintf(stderr, "v2_native_opl: frame-accumulator tick mode%s\n", why);
    }
    return frame_mode;
}

// ---------------------------------------------------------------------------
// audio-thread mix: apply queued writes at their timestamps, generate both
// chips, ADD into the caller's stereo buffer (chip0 → L, chip1 → R).
// ---------------------------------------------------------------------------
extern "C" void v2_nopl_mix(int16_t* stereo, uint32_t frames) {
    // (#83) audible sink: BEFORE the g_inited gate — the chip now comes up
    // via the sink's own OPL writes (fn65 probe during the drained init
    // chain), so the sink must get its first pump while the chip is still
    // down or neither ever starts. No-op until the bridge publishes
    // (and always in V2_ONLY, which has no bridge).
    v2_ail_sink_pump(g_samples_played.load(std::memory_order_relaxed));
    // (the chip comes up with the driver's first port write — the fn65 probe of its boot, before
    // the tick rate exists — so no tick is ever due while it is down)
    if (!g_inited.load(std::memory_order_acquire)) { v2_mt32_pump(g_samples_played.load(std::memory_order_relaxed), 0, g_rate); return; }   // UX stage 11: the MT-32 world runs even before the chip is up
    uint64_t pos = g_samples_played.load(std::memory_order_relaxed);
    uint32_t donef = 0;
    int16_t buf[256 * 2];
    while (donef < frames) {
        // (#83) per-chunk sink service: sample-clock fn67 ticks + call drain
        // right before generating this chunk — DOS INT8 semantics.
        v2_ail_sink_pump(pos);
        v2_mt32_pump(pos, donef, g_rate);   // UX stage 11: the MT-32 world (its driver ticks on the same sample clock)
        uint32_t chunk = frames - donef;
        if (chunk > 256) chunk = 256;
        // the audible driver's own ticks at their samples (the real-time mode): the ticks due
        // at pos run now, their writes are in the ring behind this line, the chunk ends at the
        // next tick's sample
        chunk = nopl_ticks_on_clock(pos, chunk);
        size_t rd = g_rd.load(std::memory_order_relaxed);
        const int late = late_on();                 // debug (V2_NOPL_LATE)
        int distinct = 0; uint64_t last_ts = ~0ull;
        while (rd != g_wr.load(std::memory_order_acquire)) {
            const NoplCmd& c = g_ring[rd];
            if (c.ts > pos) {
                uint64_t gap = c.ts - pos;
                if (gap < chunk) chunk = (uint32_t)gap;
                break;
            }
            if (late) {
                const uint64_t l = pos - c.ts;
                if (c.tick) {
                    g_late_tick_applied++;
                    if (l) { g_late_tick_late++; if (l > g_late_tick_max) g_late_tick_max = l; }
                    if (c.ts != last_ts) { distinct++; last_ts = c.ts; }
                } else {
                    g_late_call_applied++;
                    if (l == 0) g_late_call_hist[0]++;
                    else { const double ms = (double)l * 1000.0 / (double)g_rate;
                           g_late_call_hist[ms <= 4 ? 1 : ms <= 8 ? 2 : ms <= 12 ? 3 : ms <= 16 ? 4 : ms <= 24 ? 5 : ms <= 32 ? 6 : 7]++; }
                }
            }
            // bank 1 (ports 0x222/3) = OPL3 register range 0x100+.
            OPL3_WriteRegBuffered(&g_chip, (uint16_t)((c.chip << 8) | c.reg), c.val);
            rd = (rd + 1) & (NOPL_RING - 1);
        }
        g_rd.store(rd, std::memory_order_release);
        if (late && distinct) g_late_instants[distinct >= 2 ? 1 : 0]++;
        if (chunk == 0) chunk = 1;
        OPL3_GenerateStream(&g_chip, buf, chunk);
        for (uint32_t i = 0; i < chunk; i++) {
            // x2 level match: raw Nuked output sits ~10 dB below the DOSBox
            // reference capture (their mixer applies OPL gain); clamped add.
            int32_t l = (int32_t)stereo[(donef + i) * 2]     + buf[i * 2] * 2;
            int32_t r = (int32_t)stereo[(donef + i) * 2 + 1] + buf[i * 2 + 1] * 2;
            if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
            if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
            stereo[(donef + i) * 2]     = (int16_t)l;
            stereo[(donef + i) * 2 + 1] = (int16_t)r;
        }
        donef += chunk;
        pos   += chunk;
    }
    g_samples_played.store(pos, std::memory_order_release);
    if (late_on() && (++g_late_cbs % 1000) == 0) {
        fprintf(stderr, "V2-NOPL-LATE cbs=%llu tick_writes=%llu late=%llu max=%.2fms instants[1|2+]=%llu/%llu"
                        " call_writes=%llu late_ms[0|<=4|<=8|<=12|<=16|<=24|<=32|>32]=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu buf=%u\n",
                (unsigned long long)g_late_cbs, (unsigned long long)g_late_tick_applied, (unsigned long long)g_late_tick_late,
                (double)g_late_tick_max * 1000.0 / (double)g_rate, (unsigned long long)g_late_instants[0], (unsigned long long)g_late_instants[1],
                (unsigned long long)g_late_call_applied,
                (unsigned long long)g_late_call_hist[0], (unsigned long long)g_late_call_hist[1], (unsigned long long)g_late_call_hist[2], (unsigned long long)g_late_call_hist[3],
                (unsigned long long)g_late_call_hist[4], (unsigned long long)g_late_call_hist[5], (unsigned long long)g_late_call_hist[6], (unsigned long long)g_late_call_hist[7],
                frames);
    }
}
