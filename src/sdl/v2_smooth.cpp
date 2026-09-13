// UX stage 9 (2026-09-05), reworked 2026-09-11: presenter-side interpolation
// between the two newest FLIPS — the sub-frames of the DOS game.
//
// The DOS engine renders three sub-frames per game frame (render1 / render2 /
// render3), one per 60 Hz refresh of Mode X, and moves the camera and the
// sprites on every one of them (the object's world position steps once per
// game frame; the sub-sprites follow it in thirds, the viewport scrolls per
// refresh — a DOSBox capture and the flip log agree: 1..3 px per refresh). So
// the original's motion is already 60 Hz smooth, and with the game's vsync
// locked to the display's (render_v2.cpp) every sub-frame is shown once, as on
// the VGA. Nothing is left to interpolate on a 60 or 120 Hz display.
//
// The first version of this file interpolated between game FRAMES: it took the
// positions of consecutive ticks and lerped them over the ~50 ms — throwing the
// original's sub-frame positions away, a game frame of latency, and a ±1 px
// shimmer where the camera and a sprite were rounded separately. Gone.
//
// What remains useful is the display whose refresh is no multiple of 60 (75,
// 90, 144, 165 Hz): the 60 Hz schedule of the game's vsyncs is uneven there
// (a sub-frame held for two refreshes, the next for three), and interpolating
// the camera and the sprite positions between the two newest flips by the
// wall-clock fraction of the sub-frame period gives even motion at the
// display's rate — one sub-frame (16.7 ms) of latency. SMOOTH < NONE | AUTO |
// ON >: AUTO = exactly that case, ON = always (a 60 Hz display then shows
// positions between flips too, at the same latency), NONE = never.
//
// Model: the presenter renders its OWN frame from a snapshot of the newest flip
// (sprite frames, UI, palette, FS map come from it), with the viewport and the
// world positions of the sub-sprites active in both flips moved back towards
// the previous flip by (1 - t). Positions are rounded ONCE, relative to the
// exact interpolated camera (x = cam_i + round(x_f - cam_f)): the sprite keeps
// its distance to the background, no shimmer. A slot that moved more than
// V2_SMOOTH_MAX_STEP px (spawn, teleport, wrap) or is inactive in either flip
// keeps its newest position; two flips closer than 4 ms are one sub-frame (the
// game flips again in the post-flip phases) — the later one wins. The passes
// (tiles/parallax, sprites, foreground tiles, UI) paint into the presenter's
// buffer through the thread-local overrides of v2_render_funcs.cpp; the game
// state is never touched, the canon (headless, no presenter) is unaffected.
#include "render_v2.h"
#include "v2_ds_layout.h"
#include "v2_ui.h"
#include "v2_coop.h"        // UX stage 8 step 2: the local player's camera
#include "v2_stats.h"       // the STATS overlay's sub-frame counters
#include "v2_timing.h"      // v2_tick_wait_ticks: the game thread's vsync waits (STATS: work = frame - waits)
#include <SDL2/SDL.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

extern uint8_t* v2_vm_get_shadow_ds();
extern uint8_t v2_vm_shadow_fs[];
extern bool v2_last_frame_tiles;     // v2_render_funcs.cpp: the last v2_draw_tiles took the tile path
extern thread_local int v2_fbw;      // v2_render_funcs.cpp: the frame width this thread renders
extern "C" int v2_scene_fullscreen(void);
extern "C" uint16_t v2_lvx_flags(uint16_t level);
extern bool v2_vsync_auto_smooth(void);   // render_v2.cpp: the display's refresh is no multiple of 60 (the lock on)
void v2_draw_tiles(uint16_t ds_val);
void v2_draw_sprites(uint16_t ds_val);
void v2_draw_sprites_late(uint16_t ds_val);
void v2_draw_flagged_tiles(uint16_t ds_val);
void v2_draw_ui(uint16_t ds_val);

namespace {
constexpr uint32_t DS_SIZE = 0x10000;
constexpr uint32_t FS_SIZE = 0x10000;
constexpr int V2_SMOOTH_MAX_STEP = 48;       // px per sub-frame beyond which a slot is not interpolated
constexpr double SAME_SUBFRAME_MS = 4.0;     // flips closer than this are one sub-frame (the later wins)

struct Snap {
    uint8_t ds[DS_SIZE];
    uint8_t fs[FS_SIZE];
    uint32_t par_acc_x, par_acc_y;
    uint64_t t;                 // SDL_GetPerformanceCounter at capture
    bool valid, tile_frame, fullscreen;
    int w;                      // the frame width the flip rendered at (v2_fbw; UX stage 9 step 4)
    // UX stage 8 step 2: the logical cameras of players 2..3 at this flip
    // (game state, captured with the DS on the game thread)
    int players;
    uint16_t cam_x[V2_COOP_MAX], cam_y[V2_COOP_MAX];
    bool cam_valid[V2_COOP_MAX];
};
// the two newest sub-frames: g_cur = the newest flip, g_prev = the flip before it
Snap g_cur, g_prev;
std::mutex g_mx;

// presenter-local copies (the lock is held only for the memcpy)
Snap g_prev_local, g_cur_local;
uint8_t g_work[DS_SIZE];
std::atomic<bool> g_effective{false};   // the last render interpolated
std::atomic<uint32_t> g_subframe_seq{0};      // distinct sub-frames captured (STATS, the VRR presenter)
std::atomic<uint64_t> g_last_flip_ticks{0};   // the newest flip's time

inline int16_t rd16(const uint8_t* b, uint32_t o) { return (int16_t)(b[o] | (b[o + 1] << 8)); }
inline void wr16(uint8_t* b, uint32_t o, int16_t v) { b[o] = (uint8_t)v; b[o + 1] = (uint8_t)((uint16_t)v >> 8); }
inline int16_t lerp16(int16_t a, int16_t b, double t) {
    return (int16_t)(a + (int)((double)(b - a) * t + ((b >= a) ? 0.5 : -0.5)));
}
}  // namespace

float v2_smooth_last_t = -1.0f;    // debug: fraction of the last interpolated frame
int v2_smooth_last_w = 320;         // the width of the last interpolated frame (its row stride)
int v2_smooth_last_reason = 0;     // debug: why the last call returned false (0 = it did not)

extern int v2_dbg_pre_vm_iter;   // the game-frame counter (pre-VM barrier)

static void fill(Snap& S, const uint8_t* s) {
    memcpy(S.ds, s, DS_SIZE);
    memcpy(S.fs, v2_vm_shadow_fs, FS_SIZE);
    S.par_acc_x = v2_parallax.acc_x;
    S.par_acc_y = v2_parallax.acc_y;
    S.t = SDL_GetPerformanceCounter();
    S.tile_frame = v2_last_frame_tiles;
    S.fullscreen = v2_scene_fullscreen() != 0;
    S.w = v2_fbw;
    S.players = g_v2_coop_players;
    for (int k = 0; k < V2_COOP_MAX; k++) {
        S.cam_x[k] = g_coop.p[k].cam_x;
        S.cam_y[k] = g_coop.p[k].cam_y;
        S.cam_valid[k] = (k > 0) && g_coop.p[k].cam_valid;
    }
    S.valid = true;
}

// Game thread, at every page flip (v2_swap_render_buf): the flip becomes the
// newest sub-frame; a flip within SAME_SUBFRAME_MS of the newest replaces it
// (the post-flip phases flip again without a vsync wait), any other shifts it
// into the previous slot.
void v2_smooth_capture(void) {
    uint8_t* s = v2_vm_get_shadow_ds();
    if (!s) return;
    std::lock_guard<std::mutex> lock(g_mx);
    // debug: V2_SMOOTH_DUMP — every page flip as the game thread made it: the
    // viewport and the active viking's object / first sub-sprite positions, so the
    // motion WITHIN a game frame (the sub-frames) can be read
    { static FILE* ff = nullptr; static int init = 0; static int nflip = 0;
      if (!init) { init = 1; const char* dd = getenv("V2_SMOOTH_DUMP");
          if (dd && *dd) { char path[512]; snprintf(path, sizeof path, "%s/flip_log.txt", dd); ff = fopen(path, "w"); } }
      if (ff) { const uint16_t vk = (uint16_t)rd16(s, DS_ACTIVE_VIKING); const uint16_t sub = (uint16_t)rd16(s, vk + OBJ_SUB_SLOT);
          fprintf(ff, "FLIP #%d f%d t=%.1fms vp %d,%d | vik %02X obj %d,%d | sub %02X %d,%d\n", nflip++, v2_dbg_pre_vm_iter,
                  (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency() * 1000.0, rd16(s, DS_VIEWPORT_X), rd16(s, DS_VIEWPORT_Y),
                  vk, rd16(s, vk + OBJ_WORLD_X), rd16(s, vk + OBJ_WORLD_Y), sub,
                  sub <= 0xFE ? rd16(s, sub + OBJ_SPRITE_X) : 0, sub <= 0xFE ? rd16(s, sub + OBJ_SPRITE_Y) : 0); } }
    const uint64_t now = SDL_GetPerformanceCounter();
    const double since_cur_ms = g_cur.valid ? (double)(now - g_cur.t) / (double)SDL_GetPerformanceFrequency() * 1000.0 : 1e9;
    const bool new_subframe = !g_cur.valid || since_cur_ms >= SAME_SUBFRAME_MS;
    if (g_cur.valid && new_subframe) memcpy(&g_prev, &g_cur, sizeof(Snap));
    fill(g_cur, s);
    g_last_flip_ticks.store(g_cur.t, std::memory_order_relaxed);
    if (new_subframe) {
        g_subframe_seq.fetch_add(1, std::memory_order_relaxed);
        v2_stats.subframes.fetch_add(1, std::memory_order_relaxed);
        // sub-frames per game frame: the count of the frame just finished (3 in play); its wall time
        // (first flip to first flip) and its work = that time minus what the game thread spent in
        // its vsync waits (v2_tick_wait_ticks, v2_timing.h); slow = the work alone exceeded a refresh
        static int last_iter = -1, in_frame = 0; static uint64_t frame_t0 = 0, wait_t0 = 0;
        if (v2_dbg_pre_vm_iter != last_iter) {
            const uint64_t waited = v2_tick_wait_ticks.load(std::memory_order_relaxed);
            if (last_iter >= 0 && frame_t0) {
                const double freq = (double)SDL_GetPerformanceFrequency();
                const double frame_ms = (double)(g_cur.t - frame_t0) / freq * 1000.0;
                const double work_ms = frame_ms - (double)(waited - wait_t0) / freq * 1000.0;
                v2_stats.subframes_per_frame.store(in_frame, std::memory_order_relaxed);
                v2_stats.frame_ms_x100.store((int)(frame_ms * 100.0 + 0.5), std::memory_order_relaxed);
                v2_stats.work_ms_x100.store((int)((work_ms > 0.0 ? work_ms : 0.0) * 100.0 + 0.5), std::memory_order_relaxed);
                if (work_ms > 16.7) v2_stats.slow_frames.fetch_add(1, std::memory_order_relaxed);
            }
            last_iter = v2_dbg_pre_vm_iter; in_frame = 0; frame_t0 = g_cur.t; wait_t0 = waited;
        }
        in_frame++;
    }
}
uint32_t v2_smooth_subframe_seq(void) { return g_subframe_seq.load(std::memory_order_relaxed); }
uint64_t v2_smooth_last_flip_ticks(void) { return g_last_flip_ticks.load(std::memory_order_relaxed); }

// the SMOOTH option: 0 NONE, 1 AUTO (the display's refresh is no multiple of 60), 2 ON
static bool smooth_wanted(void) {
    switch (v2_options.smooth.load()) {
        case 2: return true;
        case 1: return v2_vsync_auto_smooth();
        default: return false;
    }
}
bool v2_smooth_effective(void) { return g_effective.load(std::memory_order_relaxed); }

// Presenter thread: paint an interpolated frame into `out` (rows per v2_view_rows,
// width = the snapshot's v2_fbw, reported in v2_smooth_last_w).
// false = nothing to interpolate (option off, chunk screens, level change,
// first flips, the pair is no sub-frame) — the caller shows the flip as before.
bool v2_smooth_render(uint8_t* out) {
    v2_options_ensure_loaded();
    // UX stage 8 step 2: a client whose player has his own camera renders his
    // own frame even without interpolation (then t = 1: the newest flip's
    // positions behind his camera); the flip the game thread drew is player 1's.
    const int local = g_v2_local_player;
    bool own_cam = false;
    {
        std::lock_guard<std::mutex> lock(g_mx);
        own_cam = g_cur.valid && local > 0 && local < g_cur.players && g_cur.cam_valid[local];
    }
    const bool smooth_on = smooth_wanted();
    if (!smooth_on && !own_cam) { g_effective = false; v2_smooth_last_reason = 1; return false; }
    {
        std::lock_guard<std::mutex> lock(g_mx);
        if (!g_cur.valid) { g_effective = false; v2_smooth_last_reason = 2; return false; }
        if (smooth_on && !g_prev.valid) { g_effective = false; v2_smooth_last_reason = 3; return false; }
        memcpy(&g_cur_local, &g_cur, sizeof(Snap));
        if (g_prev.valid) memcpy(&g_prev_local, &g_prev, sizeof(Snap));
        else memcpy(&g_prev_local, &g_cur, sizeof(Snap));
    }
    const Snap& C = g_cur_local;
    const Snap& P = g_prev_local;
    const double freq = (double)SDL_GetPerformanceFrequency();
    const double period = (double)(C.t - P.t) / freq;               // s between the two newest flips
    double t = 1.0;                                                 // own camera, no interpolation: the newest flip
    bool interp = false;
    if (smooth_on) {
        // a sub-frame pair: the same level and width, both tile frames, 6..40 ms apart
        // (the DOS game flips once per game frame on some screens: 50 ms, not a sub-frame)
        const bool pair = C.tile_frame && P.tile_frame && C.fullscreen == P.fullscreen && C.w == P.w &&
                          rd16(C.ds, DS_LEVEL) == rd16(P.ds, DS_LEVEL) && period >= 0.006 && period <= 0.040;
        if (!pair) {
            if (!own_cam) { g_effective = false; v2_smooth_last_reason = 5; return false; }
        } else {
            t = (double)(SDL_GetPerformanceCounter() - C.t) / freq / period;
            if (t < 0.0) t = 0.0;
            if (t > 1.0) t = 1.0;
            interp = true;
        }
    }
    if (!C.tile_frame) { g_effective = false; v2_smooth_last_reason = 4; return false; }
    v2_smooth_last_t = (float)t;
    v2_smooth_last_reason = 0;
    g_effective = interp;

    memcpy(g_work, C.ds, DS_SIZE);      // the newest flip: sprite frames, UI, everything not interpolated
    // camera: viewport + the tile-scroll state derived from it (vp >> 3) —
    // the DS viewport, or (UX stage 8 step 2) the local player's own camera
    const bool own_valid = own_cam && C.cam_valid[local] && P.cam_valid[local];
    int16_t cx = own_valid ? (int16_t)C.cam_x[local] : rd16(C.ds, DS_VIEWPORT_X);
    int16_t px = own_valid ? (int16_t)P.cam_x[local] : rd16(P.ds, DS_VIEWPORT_X);
    int16_t cy = own_valid ? (int16_t)C.cam_y[local] : rd16(C.ds, DS_VIEWPORT_Y);
    int16_t py = own_valid ? (int16_t)P.cam_y[local] : rd16(P.ds, DS_VIEWPORT_Y);
    if (own_cam && !own_valid) { cx = px = rd16(C.ds, DS_VIEWPORT_X); cy = py = rd16(C.ds, DS_VIEWPORT_Y); }
    // the exact interpolated camera (the sprites are rounded relative to it) and
    // the integer one written to the frame (the tiles scroll by it)
    double camx_f = cx, camy_f = cy;
    int16_t vx = cx, vy = cy;
    {
        bool write = own_cam;
        if (interp && (cx != px || cy != py) &&
            cx - px <= V2_SMOOTH_MAX_STEP && px - cx <= V2_SMOOTH_MAX_STEP &&
            cy - py <= V2_SMOOTH_MAX_STEP && py - cy <= V2_SMOOTH_MAX_STEP) {
            camx_f = px + (double)(cx - px) * t; camy_f = py + (double)(cy - py) * t;
            vx = lerp16(px, cx, t); vy = lerp16(py, cy, t);
            write = true;
        }
        if (vx < 0) { vx = 0; camx_f = 0.0; }
        if (vy < 0) { vy = 0; camy_f = 0.0; }
        if (write) {
            wr16(g_work, DS_VIEWPORT_X, vx);
            wr16(g_work, DS_VIEWPORT_Y, vy);
            wr16(g_work, DS_SCROLL_COL, (int16_t)((uint16_t)vx >> 3));
            wr16(g_work, DS_SCROLL_ROW, (int16_t)((uint16_t)vy >> 3));
        }
    }
    // sub-sprites active in both flips: the world position moved back towards the
    // previous flip by (1 - t), rounded once relative to the exact camera so the
    // distance to the background is the rounded exact one (a camera-locked viking
    // stays still on the screen, as in the original; no ±1 px shimmer)
    if (interp) {
        for (uint32_t obj = 0; obj <= 0xFE; obj += 2) {
            const uint16_t fc = (uint16_t)rd16(C.ds, obj + OBJ_SPRITE_FLAGS);
            const uint16_t fp = (uint16_t)rd16(P.ds, obj + OBJ_SPRITE_FLAGS);
            if (!(fc & 0x8000) || !(fp & 0x8000)) continue;
            const int16_t ax = rd16(C.ds, obj + OBJ_SPRITE_X), bx = rd16(P.ds, obj + OBJ_SPRITE_X);
            const int16_t ay = rd16(C.ds, obj + OBJ_SPRITE_Y), by = rd16(P.ds, obj + OBJ_SPRITE_Y);
            if (ax == bx && ay == by) continue;
            if (ax - bx > V2_SMOOTH_MAX_STEP || bx - ax > V2_SMOOTH_MAX_STEP ||
                ay - by > V2_SMOOTH_MAX_STEP || by - ay > V2_SMOOTH_MAX_STEP) continue;   // spawn / teleport / wrap
            const double xf = bx + (double)(ax - bx) * t, yf = by + (double)(ay - by) * t;
            wr16(g_work, obj + OBJ_SPRITE_X, (int16_t)(vx + lround(xf - camx_f)));
            wr16(g_work, obj + OBJ_SPRITE_Y, (int16_t)(vy + lround(yf - camy_f)));
        }
    }
    // parallax autoscroll accumulators (units of 1/1792 px): lerp unless wrapped
    uint32_t acc[2] = { C.par_acc_x, C.par_acc_y };
    if (interp) {
        if (C.par_acc_x >= P.par_acc_x && C.par_acc_x - P.par_acc_x < 1792u * 64u)
            acc[0] = P.par_acc_x + (uint32_t)((double)(C.par_acc_x - P.par_acc_x) * t);
        if (C.par_acc_y >= P.par_acc_y && C.par_acc_y - P.par_acc_y < 1792u * 64u)
            acc[1] = P.par_acc_y + (uint32_t)((double)(C.par_acc_y - P.par_acc_y) * t);
    }

    // debug: V2_SMOOTH_DUMP=<dir> — one line per interpolated frame: game frame,
    // fraction, the sub-frame period, the wall-clock since the newest flip, camera
    // prev/cur/lerp, the active viking's object X/Y and its first sub-sprite's X/Y
    // prev/cur/lerp
    {
        static FILE* lf = nullptr; static int init = 0;
        if (!init) { init = 1; const char* dd = getenv("V2_SMOOTH_DUMP");
            if (dd && *dd) { char path[512]; snprintf(path, sizeof path, "%s/smooth_log.txt", dd); lf = fopen(path, "w"); } }
        if (lf) {
            const uint16_t vk = (uint16_t)rd16(C.ds, DS_ACTIVE_VIKING);
            const uint16_t sub = (uint16_t)rd16(C.ds, vk + OBJ_SUB_SLOT);
            fprintf(lf, "f%d t=%.3f period=%.1fms since=%.1fms vp %d,%d -> %d,%d = %d,%d | vik %02X obj %d,%d -> %d,%d | sub %02X x %d -> %d = %d y %d -> %d = %d\n",
                    v2_dbg_pre_vm_iter, t, period * 1000.0, (double)(SDL_GetPerformanceCounter() - C.t) / freq * 1000.0,
                    px, py, cx, cy, rd16(g_work, DS_VIEWPORT_X), rd16(g_work, DS_VIEWPORT_Y),
                    vk, rd16(P.ds, vk + OBJ_WORLD_X), rd16(P.ds, vk + OBJ_WORLD_Y), rd16(C.ds, vk + OBJ_WORLD_X), rd16(C.ds, vk + OBJ_WORLD_Y),
                    sub,
                    sub <= 0xFE ? rd16(P.ds, sub + OBJ_SPRITE_X) : 0, sub <= 0xFE ? rd16(C.ds, sub + OBJ_SPRITE_X) : 0, sub <= 0xFE ? rd16(g_work, sub + OBJ_SPRITE_X) : 0,
                    sub <= 0xFE ? rd16(P.ds, sub + OBJ_SPRITE_Y) : 0, sub <= 0xFE ? rd16(C.ds, sub + OBJ_SPRITE_Y) : 0, sub <= 0xFE ? rd16(g_work, sub + OBJ_SPRITE_Y) : 0);
        }
    }
    // the passes, in the gameplay frame's order, on the presenter's buffers
    v2_tls_ds = g_work;
    v2_tls_out = out;
    v2_fbw = C.w;                   // the presenter thread renders at the snapshot's width
    v2_smooth_last_w = C.w;
    v2_tls_fs = C.fs;
    v2_tls_par_acc = acc;
    v2_tls_presenter = true;
    v2_draw_tiles(0);
    v2_draw_sprites(0);
    v2_draw_sprites_late(0);
    v2_draw_flagged_tiles(0);
    v2_draw_ui(0);
    v2_tls_presenter = false;
    v2_tls_par_acc = nullptr;
    v2_tls_fs = nullptr;
    v2_tls_out = nullptr;
    v2_tls_ds = nullptr;
    return true;
}
