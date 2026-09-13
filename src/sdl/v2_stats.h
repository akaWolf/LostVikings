// v2_stats.h — the figures behind the STATS overlay (F1, 2026-09-11): frame
// pacing, sub-frame delivery and audio health, each written by the thread that
// knows it and read by the presenter. Diagnostics only — no game state, no
// effect on the canon (the headless build has no presenter and no audio device).
#pragma once
#include <atomic>
#include <cstdint>

struct V2Stats {
    // presenter (render_v2.cpp)
    std::atomic<int>      vsync_locked{0};        // the game's vsync follows the display
    std::atomic<int>      display_hz_x100{0};     // the refresh the lock runs on
    std::atomic<int>      present_ms_x100{0};     // measured interval between presents
    std::atomic<uint32_t> present_late{0};        // presents that took more than 1.5 refreshes
    std::atomic<uint32_t> flip_drops{0};          // sub-frames the game flipped that were never presented
    std::atomic<uint32_t> flip_doubles{0};        // presents that showed the previous sub-frame again while the game was flipping (60 Hz lock, no interpolation)
    // game thread (v2_smooth.cpp capture / v2_main.cpp)
    std::atomic<uint32_t> subframes{0};           // distinct sub-frames flipped so far
    std::atomic<int>      subframes_per_frame{0}; // of the last complete game frame (3 in play)
    std::atomic<int>      frame_ms_x100{0};       // wall time of the last game frame
    std::atomic<int>      work_ms_x100{0};        // its work, the waits excluded
    std::atomic<uint32_t> slow_frames{0};         // frames whose work alone exceeded the budget
    // audio (play.cpp)
    std::atomic<int>      audio_rate{0};
    std::atomic<int>      audio_samples{0};       // the device buffer SDL gave (frames)
    std::atomic<uint32_t> audio_underruns{0};     // callback gaps longer than 1.5 buffers
    std::atomic<uint32_t> audio_clips{0};         // callbacks with samples at the rail
    std::atomic<uint32_t> audio_cb_overruns{0};   // callbacks that used more than 90 % of their budget
};
extern V2Stats v2_stats;   // render_v2.cpp
