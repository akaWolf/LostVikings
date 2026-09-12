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
//   * the timer tick is pumped ON THE GAME THREAD from frame_begin /
//     blocking-loop ticks: v2_nopl_pump() computes how many driver ticks
//     are due and calls fn67 for each, stamping every OPL write with the
//     tick's sample position — sample-accurate music from a
//     frame-granular pump (the DOS INT8 interleaving collapses to
//     batched, correctly-timestamped writes);
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

// SDL mixer rate. play.cpp sets the obtained device rate after SDL_OpenAudio;
// the 44100 default keeps HEADLESS builds (no play.cpp, no audio device)
// self-contained — there the queue is never drained, only the fn67 ticks
// matter (they advance the driver's DS state deterministically).
static uint32_t g_rate = 44100;
extern "C" void v2_nopl_set_mix_rate(uint32_t rate) { if (rate) g_rate = rate; }

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------
static opl3_chip    g_chip;                 // single OPL3 (bank 1 via 0x222/3)
static bool         g_inited  = false;
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
static int          g_force_frame = 0;      // a lockstep game: the sequencer ticks by frames on every peer (v2_nopl_force_frame_ticks)

static double       g_tick_hz = 0.0;

// Sample clock: audio thread advances; game thread schedules ticks against it.
static std::atomic<uint64_t> g_samples_played{0};
static uint64_t     g_ticks_done = 0;
static uint64_t     g_cur_ts    = 0;        // ts stamped on writes of the tick being pumped

// Command ring: game thread produces, audio thread consumes.
struct NoplCmd { uint64_t ts; uint8_t chip; uint8_t reg; uint8_t val; };
static const size_t NOPL_RING = 1 << 14;
static NoplCmd      g_ring[NOPL_RING];
static std::atomic<size_t> g_wr{0}, g_rd{0};

static void nopl_lazy_init() {
    if (g_inited) return;
    OPL3_Reset(&g_chip, g_rate);      // Nuked resamples its 49716 core to g_rate
    g_inited = true;
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
        uint64_t ts = g_cur_ts ? g_cur_ts
                               : g_samples_played.load(std::memory_order_relaxed);
        size_t wr = g_wr.load(std::memory_order_relaxed);
        size_t nx = (wr + 1) & (NOPL_RING - 1);
        if (nx == g_rd.load(std::memory_order_acquire)) {
            // full: drop (never block the game thread) — but LOUDLY: a
            // dropped register write audibly corrupts patches/notes.
            static uint64_t drops = 0;
            if ((++drops & (drops - 1)) == 0)   // log at 1,2,4,8,...
                fprintf(stderr, "v2_native_opl: RING FULL — %llu writes dropped so far\n",
                        (unsigned long long)drops);
            return;
        }
        g_ring[wr] = { ts, (uint8_t)chip, reg, val };
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
}

extern "C" void v2_nopl_force_frame_ticks(void) {
    if (!g_force_frame) fprintf(stderr, "v2_native_opl: lockstep — frame-accumulator tick mode on every peer\n");
    g_force_frame = 1;
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
    g_tick_hz = hz;
    g_base_samples = g_samples_played.load(std::memory_order_relaxed);
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

// Tick pump. The DOS INT8 fired at [desc+0x14]+5 Hz by REAL TIME regardless
// of the frame rate — and the V2_ONLY game loop is NOT 60 Hz (the phase
// chain blocks on VSYNC waits inside sub_10130; ~19 game frames/s, see
// v2_main.cpp timing notes). Pacing ticks per frame therefore ran the
// sequencer ~3x slow (user-audible: background music crawled while short
// SFX still sounded okay). Default: pace by the AUDIO clock — samples the
// device has consumed ARE wall time, and every queued write keeps its exact
// per-tick timestamp, so batching at frame granularity stays inaudible.
//
// V2_AIL_FRAME_TICKS=1 keeps the frame-accumulator mode (125/60 per pump):
// fully deterministic driver DS state for replay/verify experiments, at the
// cost of tempo tracking the frame rate.
static const double NOPL_FRAME_HZ = 60.0;
// (g_tick_acc lives in g_oplr, see the register file above)

static int nopl_frame_mode(void);
extern "C" void v2_ail_sink_pump(uint64_t);   // (#83) audible sink driver (v2_ail.cpp)

extern "C" void v2_nopl_pump(void) {
#if defined(V2_ONLY) && !defined(HEADLESS)
    // №59 windowed enforcement: when the presenter loop has already left on
    // need_quit (max-frames / window close), the game thread can sit inside
    // a blocking wait loop forever — every such loop pumps, so this is the
    // single choke point. Mirror the headless clean exit.
    {
        extern bool need_quit;
        extern int g_v2only_max_frames;
        // v2_dbg_pre_vm_iter: file-scope extern (top of file)
        if (need_quit ||
            (g_v2only_max_frames > 0 && v2_dbg_pre_vm_iter >= g_v2only_max_frames)) {
            fprintf(stderr, "V2_ONLY: max-frames/quit reached in the game "
                    "thread (frame %d), exiting cleanly\n", v2_dbg_pre_vm_iter);
            extern void headless_golden_dump(void);
            headless_golden_dump();
            v2_midi_shutdown();   // UX stage 11: V2_MIDI_DUMP (_exit skips atexit)
            fflush(stdout); fflush(stderr);
            _exit(0);
        }
    }
#endif
    if (g_tick_hz <= 0.0) return;
    double spt = (double)g_rate / g_tick_hz;   // samples per tick (queue ts)
    int frame_mode = nopl_frame_mode();
    int guard = 0;
    if (frame_mode) {
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
            v2_ail_tick();
            g_ticks_done++;
        }
        g_cur_ts = 0;
        return;
    }
    // real-time mode: ticks due by the audio cursor (relative to the boot
    // base), small lead so freshly queued commands land slightly ahead of
    // it instead of in its past. Queue timestamps carry the same base so
    // the mixer's absolute sample position lines up.
    uint64_t played = g_samples_played.load(std::memory_order_relaxed);
    uint64_t rel = (played > g_base_samples) ? played - g_base_samples : 0;
    uint64_t due = (uint64_t)((double)rel / spt) + 1;
    // Catch-up cap: if the game thread stalled long enough to owe more than
    // ~0.5 s of ticks (level loads, rare host hiccups), slide the base
    // forward instead of burst-replaying the backlog. A burst stamps events
    // spread over hundreds of musical ms into one mixer instant — short
    // notes vanish (key-on+off in one buffer), held notes overstay. A clean
    // PAUSE is the right degradation; DOS never lagged its INT8.
    if (due > g_ticks_done + 64) {
        uint64_t excess = due - g_ticks_done - 64;
        g_base_samples += (uint64_t)((double)excess * spt);
        due -= excess;
        fprintf(stderr, "v2_native_opl: tick debt %llu — paused (base slid)\n",
                (unsigned long long)excess);
    }
    while (g_ticks_done < due && guard++ < 96) {
        g_cur_ts = g_base_samples + (uint64_t)((double)g_ticks_done * spt);
        v2_ail_tick();
        g_ticks_done++;
    }
    g_cur_ts = 0;
}

// Tick pacing mode, decided once.
// (#83) default/verify: ALWAYS frame-paced again — real+shadow are the
// deterministic verify pair and no longer feed the chip (the audible path
// is the sink instance, ticked on the sample clock in v2_nopl_mix).
// V2_ONLY: the single (audible) instance paces by the audio clock;
// V2_AIL_FRAME_TICKS=1 keeps its deterministic frame mode for replays.
static int nopl_frame_mode(void) {
    if (g_force_frame) return 1;    // UX stage 8 tails: a network game ticks by frames (the audio clock is not shared)
    static int frame_mode = -1;
    if (frame_mode < 0) {
#ifdef V2_ONLY
        const char* e = getenv("V2_AIL_FRAME_TICKS");
        frame_mode = (e && e[0] == '1') ? 1 : 0;
#else
        frame_mode = 1;
#endif
        if (frame_mode) fprintf(stderr, "v2_native_opl: frame-accumulator tick mode\n");
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
    if (!g_inited) { v2_mt32_pump(g_samples_played.load(std::memory_order_relaxed), 0, g_rate); return; }   // UX stage 11: the MT-32 world runs even before the chip is up
    uint64_t pos = g_samples_played.load(std::memory_order_relaxed);
    uint32_t donef = 0;
    int16_t buf[256 * 2];
    while (donef < frames) {
        // (#83) per-chunk sink service: sample-clock fn67 ticks + call drain
        // right before generating this chunk — DOS INT8 semantics.
        v2_ail_sink_pump(pos);
        v2_mt32_pump(pos, donef, g_rate);   // UX stage 11: the MT-32 world (its driver ticks on the same sample clock)
        size_t rd = g_rd.load(std::memory_order_relaxed);
        uint32_t chunk = frames - donef;
        if (chunk > 256) chunk = 256;
        while (rd != g_wr.load(std::memory_order_acquire)) {
            const NoplCmd& c = g_ring[rd];
            if (c.ts > pos) {
                uint64_t gap = c.ts - pos;
                if (gap < chunk) chunk = (uint32_t)gap;
                break;
            }
            // bank 1 (ports 0x222/3) = OPL3 register range 0x100+.
            OPL3_WriteRegBuffered(&g_chip, (uint16_t)((c.chip << 8) | c.reg), c.val);
            rd = (rd + 1) & (NOPL_RING - 1);
        }
        g_rd.store(rd, std::memory_order_release);
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
}
