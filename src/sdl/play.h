// SDL/adlmidi audio API for Lost Vikings port.
// =============================================================================
// Defines AudioPool class (per-instance audio state) and two global instances:
//   orig_pool — driven by orig path SDL inlines in seg000.cpp (audible).
//   v2_pool   — driven by v2 mirror code in v2_vm.cpp.
// In default (verify) mode, v2_pool.globally_muted=true: slot tracking continues
// for DS verify symmetry, but no audio output. In V2_ONLY mode, v2_pool is the
// only producer (audible) and orig_pool is unused.
//
// Free-function legacy entries (play_xmidi_external, stop_xmidi_external, etc.)
// delegate to orig_pool.method() for back-compat with seg000.cpp SDL inlines.
// v2 callers use v2_pool.method() directly.
// =============================================================================
#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>

// Forward decl for adlmidi player handle (defined in adlmidi.h).
struct ADL_MIDIPlayer;

// SPSC ring buffer for int16 samples. Lock-free single-producer/single-consumer.
// CAPACITY = 8192 int16 = 4096 stereo frames = ~92ms @ 44100Hz.
template<size_t Capacity>
class SpscRing {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
    static constexpr size_t MASK = Capacity - 1;
    int16_t buffer[Capacity];
    alignas(64) std::atomic<size_t> write_pos{0};
    alignas(64) std::atomic<size_t> read_pos{0};
public:
    size_t write(const int16_t* data, size_t count) {
        size_t w = write_pos.load(std::memory_order_relaxed);
        size_t r = read_pos.load(std::memory_order_acquire);
        size_t used = w - r;
        size_t free_space = Capacity - 1 - used;
        size_t n = (count < free_space) ? count : free_space;
        for (size_t i = 0; i < n; i++) buffer[(w + i) & MASK] = data[i];
        write_pos.store(w + n, std::memory_order_release);
        return n;
    }
    size_t read(int16_t* dest, size_t count) {
        size_t r = read_pos.load(std::memory_order_relaxed);
        size_t w = write_pos.load(std::memory_order_acquire);
        size_t avail = w - r;
        size_t n = (count < avail) ? count : avail;
        for (size_t i = 0; i < n; i++) dest[i] = buffer[(r + i) & MASK];
        read_pos.store(r + n, std::memory_order_release);
        return n;
    }
    size_t free_space() const {
        size_t w = write_pos.load(std::memory_order_relaxed);
        size_t r = read_pos.load(std::memory_order_acquire);
        return Capacity - 1 - (w - r);
    }
    size_t available() const {
        size_t r = read_pos.load(std::memory_order_relaxed);
        size_t w = write_pos.load(std::memory_order_acquire);
        return w - r;
    }
    void reset() {
        read_pos.store(0, std::memory_order_relaxed);
        write_pos.store(0, std::memory_order_relaxed);
    }
};

// AudioPool: ONE class for an independent audio instance. Two globals:
// orig_pool, v2_pool — same class, different state. Methods called via instance:
//   orig_pool.play_xmidi_external(...)
//   v2_pool.play_xmidi_external(...)
// audio_callback iterates BOTH pools and mixes outputs (skipping globally_muted).
class AudioPool {
public:
    // ---- per-slot state (each pool has its own arrays of 100 slots) ----

    // Per-slot ring buffer (4096 stereo frames = ~92ms ahead). Producer thread
    // writes adlmidi samples; audio_callback drains. Decoupled mixer pattern.
    SpscRing<8192> g_rings[100];

    // Per-slot state for decoupled mixer:
    //   stop_requested: set by stop_xmidi()/main thread; worker checks each loop.
    //   producer_alive: true while worker thread running; false after worker exits.
    //   player_active:  true while logical playback active (set by play_xmidi,
    //                   cleared by audio_callback after ring drained AND producer dead).
    std::atomic<bool> g_stop_requested[100] = {};
    std::atomic<bool> g_producer_alive[100] = {};
    std::atomic<bool> g_player_active[100] = {};

    // Muted slots: reserved for tracking only (no producer thread, no audio).
    // Used in default mode for DS verify symmetry. audio_callback skips
    // natural-end SLOT-DRAIN for muted slots — they only close on explicit
    // stop_xmidi (need_close=true).
    std::atomic<bool> g_slot_muted[100] = {};

    // First-tick diagnostic: tracks when audio_callback first sees samples from
    // each slot. Reset on close so reused slots re-log latency.
    bool _sound_first_tick[100] = {};

    // Per-slot saved XMI buffer pointer + length, captured at play_xmidi time.
    // Used to RE-OPEN the XMI when music track ends naturally (samples=0). Orig
    // DOS AIL handles XMI Branch (CC 0x77) loop events; adlmidi doesn't support
    // them for these files. Workaround: full adl_openData re-init = same effect
    // as restarting music from beginning, mimicking AIL's loop behavior.
    const void* _sound_xmi_buf[100] = {};
    uint32_t    _sound_xmi_len[100] = {};
    int         _sound_xmi_seq[100] = {};

    // Per-slot fade-out state. fade_volume is current scale (1.0 = full, 0.0 = silent).
    // fade_step is per-callback decrement (positive). When fade_volume reaches 0.0,
    // slot is marked for close. Used by fade_music() to gradually fade out
    // (orig sub_178f1 — AIL fade over 1 sec for level transitions).
    std::atomic<float> _sound_fade_volume[100];
    std::atomic<float> _sound_fade_step[100];

    // Per-slot async close flag. Set by stop_xmidi(handle) or stop_all_sfx().
    // Audio callback reads this per-iteration; on true: closes player and resets.
    // Atomic since written from main game thread, read from SDL audio thread.
    std::atomic<bool> need_close[100] = {};

    // Currently-playing music handle (unique ID, set by set_dontstop()). 0 = no music.
    // Used by stop_all_sfx (to skip music slot) and audio_callback volume scaling.
    std::atomic<uint16_t> dontstop_handle{0};

    // Unique handle table — mirrors orig AIL design. Each play_xmidi assigns a
    // fresh handle (1..0xFFFE; 0 and 0xFFFF reserved as "free" sentinels).
    // slot_handle[i] is the handle currently playing in slot i, or 0 if free.
    // After natural-end or stop, slot_handle[i] = 0 — any subsequent stop_xmidi
    // with stale handle finds no match, no-op (mirrors AIL "stop accepts stale
    // handle silently" behavior).
    std::atomic<uint16_t> slot_handle[100] = {};
    std::atomic<uint16_t> g_next_handle{1};

    // ADLMIDI player instances per slot. nullptr = no player (slot free or muted).
    struct ADL_MIDIPlayer* midi_players[100] = {};

    // If true, audio_callback skips mixing for this pool (still runs lifecycle).
    // v2_pool=true in default mode (verify symmetry only); orig_pool always false.
    bool globally_muted = false;

    // ---- public API — methods. Same signature on orig_pool and v2_pool. ----

    // Init/reset all per-slot state. globally_muted_flag controls audio output.
    void init(bool globally_muted_flag);

    // Play XMI. Auto-allocates handle (returns it). Spawns producer thread.
    int play_xmidi_external(const void* xmidi, uint32_t len, int seq_num);

    // Play XMI with EXTERNAL handle (from deterministic audit). mute=true →
    // reserve slot for tracking but no producer thread / no audio (used in
    // default mode by v2 for DS verify symmetry without sound output).
    int play_xmidi_external_with_handle_and_mute(const void* xmidi, uint32_t len, int seq_num,
                                                  uint16_t handle, bool mute);

    // Stop a specific player by handle. Non-blocking. Stale handle → no-op.
    void stop_xmidi(uint16_t handle);

    // Stop ALL non-music SFX. Music (matching dontstop_handle) preserved.
    void stop_all_sfx();

    // Fade music out over duration_ms. <=0 = immediate stop. Per-slot volume ramp.
    void fade_music(int duration_ms);

    // Mark a player as the "music" player (60% volume, skipped by stop_all_sfx).
    void set_dontstop(uint16_t handle);

    // True if any slot matching handle has live producer (real audio, not muted).
    bool is_player_active(uint16_t handle);

    // True if any slot in this pool tracks `handle` (including mute slots).
    // Used to detect stale DS slot entries — sub_177bb treats inactive handles
    // as free, otherwise the 4-entry DS slot table fills up and stops break.
    bool is_handle_active(uint16_t handle);

    // Returns current music handle (or 0).
    uint16_t get_music_handle() { return dontstop_handle.load(); }

    // ---- internal helpers (called from audio_callback's per-slot loop) ----
    uint16_t alloc_handle();
    template<class Fn> int for_each_slot_with_handle(uint16_t h, Fn&& fn);
    int play_xmidi_with_handle_and_mute(const void* xmidi, uint32_t len, int seq_num,
                                         uint16_t handle_in, bool mute);
};

// Two independent audio pools — defined in play.cpp. Same class, different state.
extern AudioPool orig_pool;
extern AudioPool v2_pool;

// Legacy free-function API — orig path inlines in seg000.cpp call by these names.
// Each delegates to orig_pool.method(). v2 callers use v2_pool.method() directly.
int  play_xmidi(const void* xmidi, uint32_t len, int seq_num);
int  play_xmidi_external(const void* xmidi, uint32_t len, int seq_num);
int  play_xmidi_with_handle_and_mute(const void* xmidi, uint32_t len, int seq_num,
                                      uint16_t handle_in, bool mute);
int  play_xmidi_external_with_handle_and_mute(const void* xmidi, uint32_t len, int seq_num,
                                               uint16_t handle, bool mute);
void stop_xmidi_external(uint16_t handle);
void stop_xmidi_external();   // legacy "stop all" alias
void stop_all_sfx();
void fade_music(int duration_ms);
void set_dontstop_external(uint16_t handle);
bool is_player_active(uint16_t handle);
uint16_t get_music_handle();

void sound_init();
void _sound_reset_first_tick(int i);
