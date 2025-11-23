// HEADLESS audio stub — replaces play.cpp under HEADLESS build.
//
// Provides AudioPool API + global instances WITHOUT adlmidi / SDL audio device.
// Audio audit (v2_audit_*) uses deterministic handles, doesn't need real audio
// for matching orig + v2 SFX event sequences.
//
// Behavior:
//   play_xmidi_*  : assigns deterministic handle, records frame issued
//   stop_xmidi    : clears slot
//   is_handle_active: true while (current_frame - issued < SFX_DURATION_FRAMES)
//                    mimics ~1s natural SFX lifetime → game logic gets
//                    consistent answers about whether sound still plays.
//   fade_music    : immediate stop (no fade duration meaningful without audio)

#include "../play.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <atomic>

extern int v2_dbg_pre_vm_iter;  // current frame counter

// Two pool instances, same as real play.cpp.
AudioPool orig_pool;
AudioPool v2_pool;

namespace {
constexpr int SFX_DURATION_FRAMES = 60;  // ~1 sec at 60fps — mimics typical SFX
constexpr int MUSIC_DURATION_FRAMES = 100000;  // music loops indefinitely
// Per-slot frame at which handle was issued. Used for is_handle_active deterministic.
int g_slot_frame_issued_orig[100] = {};
int g_slot_frame_issued_v2[100] = {};

int* slot_frames_for(AudioPool* p) {
    return (p == &orig_pool) ? g_slot_frame_issued_orig : g_slot_frame_issued_v2;
}
} // namespace

void AudioPool::init(bool globally_muted_flag) {
    globally_muted = globally_muted_flag;
    for (int i = 0; i < 100; i++) {
        slot_handle[i].store(0, std::memory_order_relaxed);
        g_slot_muted[i].store(false, std::memory_order_relaxed);
        g_player_active[i].store(false, std::memory_order_relaxed);
        g_producer_alive[i].store(false, std::memory_order_relaxed);
        g_stop_requested[i].store(false, std::memory_order_relaxed);
        need_close[i].store(false, std::memory_order_relaxed);
    }
    g_next_handle.store(1, std::memory_order_relaxed);
    dontstop_handle.store(0, std::memory_order_relaxed);
}

uint16_t AudioPool::alloc_handle() {
    uint16_t h = g_next_handle.fetch_add(1, std::memory_order_relaxed);
    if (h == 0 || h == 0xFFFF) h = g_next_handle.fetch_add(1, std::memory_order_relaxed);
    return h;
}

int AudioPool::play_xmidi_with_handle_and_mute(const void* xmidi, uint32_t len, int seq_num,
                                                uint16_t handle_in, bool mute) {
    (void)xmidi; (void)len; (void)mute;
    uint16_t h = handle_in ? handle_in : alloc_handle();
    int* frames = slot_frames_for(this);
    // Find free slot
    for (int i = 0; i < 100; i++) {
        if (slot_handle[i].load() == 0) {
            slot_handle[i].store(h, std::memory_order_relaxed);
            g_player_active[i].store(true, std::memory_order_relaxed);
            frames[i] = v2_dbg_pre_vm_iter;
            _sound_xmi_seq[i] = seq_num;
            return h;
        }
    }
    return h;  // no free slot — return handle anyway (will be stale)
}

int AudioPool::play_xmidi_external(const void* xmidi, uint32_t len, int seq_num) {
    return play_xmidi_with_handle_and_mute(xmidi, len, seq_num, 0, false);
}

int AudioPool::play_xmidi_external_with_handle_and_mute(const void* xmidi, uint32_t len,
                                                         int seq_num, uint16_t handle, bool mute) {
    return play_xmidi_with_handle_and_mute(xmidi, len, seq_num, handle, mute);
}

void AudioPool::stop_xmidi(uint16_t handle) {
    if (!handle || handle == 0xFFFF) return;
    for (int i = 0; i < 100; i++) {
        if (slot_handle[i].load() == handle) {
            slot_handle[i].store(0, std::memory_order_relaxed);
            g_player_active[i].store(false, std::memory_order_relaxed);
        }
    }
}

void AudioPool::stop_all_sfx() {
    uint16_t music = dontstop_handle.load();
    for (int i = 0; i < 100; i++) {
        uint16_t h = slot_handle[i].load();
        if (h && h != music) {
            slot_handle[i].store(0, std::memory_order_relaxed);
            g_player_active[i].store(false, std::memory_order_relaxed);
        }
    }
}

void AudioPool::fade_music(int duration_ms) {
    (void)duration_ms;
    uint16_t music = dontstop_handle.load();
    if (music) stop_xmidi(music);
    dontstop_handle.store(0);
}

void AudioPool::set_dontstop(uint16_t handle) {
    dontstop_handle.store(handle);
}

bool AudioPool::is_player_active(uint16_t handle) {
    return is_handle_active(handle);
}

bool AudioPool::is_handle_active(uint16_t handle) {
    if (!handle || handle == 0xFFFF) return false;
    int* frames = slot_frames_for(this);
    for (int i = 0; i < 100; i++) {
        if (slot_handle[i].load() != handle) continue;
        int issued = frames[i];
        int duration = (handle == dontstop_handle.load())
                       ? MUSIC_DURATION_FRAMES : SFX_DURATION_FRAMES;
        if (v2_dbg_pre_vm_iter - issued < duration) return true;
        // Natural-end: clear slot
        slot_handle[i].store(0, std::memory_order_relaxed);
        g_player_active[i].store(false, std::memory_order_relaxed);
        return false;
    }
    return false;
}

template<class Fn>
int AudioPool::for_each_slot_with_handle(uint16_t h, Fn&& fn) {
    int n = 0;
    for (int i = 0; i < 100; i++) {
        if (slot_handle[i].load() == h) { fn(i); n++; }
    }
    return n;
}
// Explicit instantiations not needed in headless — no callers use templates here.

void _sound_reset_first_tick(int) {}

// ============================================================================
// Legacy free-function delegates (orig sub_177bb / sub_1782a inlines call these)
// ============================================================================
int  play_xmidi(const void* x, uint32_t l, int s) { return orig_pool.play_xmidi_with_handle_and_mute(x, l, s, 0, false); }
int  play_xmidi_external(const void* x, uint32_t l, int s) { return orig_pool.play_xmidi_external(x, l, s); }
int  play_xmidi_with_handle_and_mute(const void* x, uint32_t l, int s, uint16_t h, bool m) { return orig_pool.play_xmidi_with_handle_and_mute(x, l, s, h, m); }
int  play_xmidi_external_with_handle_and_mute(const void* x, uint32_t l, int s, uint16_t h, bool m) { return orig_pool.play_xmidi_external_with_handle_and_mute(x, l, s, h, m); }
void stop_xmidi_external(uint16_t h) { orig_pool.stop_xmidi(h); }
void stop_xmidi_external() { orig_pool.stop_all_sfx(); }
void stop_all_sfx() { orig_pool.stop_all_sfx(); }
void fade_music(int d) { orig_pool.fade_music(d); }
void set_dontstop_external(uint16_t h) { orig_pool.set_dontstop(h); }
bool is_player_active(uint16_t h) { return orig_pool.is_player_active(h); }
uint16_t get_music_handle() { return orig_pool.get_music_handle(); }

void sound_init() {
    orig_pool.init(false);
    v2_pool.init(true);  // muted in default mode (verify symmetry only)
    fprintf(stderr, "[headless] sound_init: stub pools initialized (no real audio)\n");
}
