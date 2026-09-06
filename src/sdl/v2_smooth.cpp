// UX stage 9 (2026-09-05): smooth scrolling — presenter-side interpolation.
//
// A GAME frame of the DOS engine is three vsync waits long (pre-VM tick,
// render1 + flip, post-flip1, render2 + flip, post-flip2: ~48 ms at the
// port's 16 ms vsync, ~43 ms on the 70 Hz original) — object positions and
// the camera move once per game frame, i.e. at ~20 Hz, while the window is
// presented at the display's refresh (60 Hz with PRESENTVSYNC): every
// position step lands on one of three presented frames and the scroll
// visibly stutters. Here the presenter renders its OWN frame: the camera and
// every active sub-sprite's world position are interpolated between the two
// newest GAME-frame snapshots by the wall-clock fraction elapsed since the
// newer one (so the three presented frames of a game frame show 1/3, 2/3,
// 3/3 of the step — one game frame of display latency, no extrapolation
// artefacts), everything else (sprite animation frames, UI, palette) comes
// from the newest page flip, and the ordinary render passes (tiles/
// parallax, sprites, foreground tiles, UI) paint into a presenter buffer
// through the thread-local overrides of v2_render_funcs.cpp. The game
// state is never touched: the snapshots are copies, the passes read them and
// paint elsewhere, so the canon (headless, no presenter) is unaffected and
// the interpolation can be switched off live (F1 SMOOTH) — the presenter
// then shows the flip frame as before.
//
// Sub-sprite positions are lerped in WORLD space (OBJ_SPRITE_X/Y are world
// coordinates; the passes subtract the camera), which keeps a camera-locked
// viking still on screen while the map glides. A slot that changed by more
// than V2_SMOOTH_MAX_STEP px (spawn, teleport, wrap) or is inactive in
// either snapshot keeps its newer position.
#include "render_v2.h"
#include "v2_ds_layout.h"
#include "v2_ui.h"
#include <SDL2/SDL.h>
#include <atomic>
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
void v2_draw_tiles(uint16_t ds_val);
void v2_draw_sprites(uint16_t ds_val);
void v2_draw_sprites_late(uint16_t ds_val);
void v2_draw_flagged_tiles(uint16_t ds_val);
void v2_draw_ui(uint16_t ds_val);

namespace {
constexpr uint32_t DS_SIZE = 0x10000;
constexpr uint32_t FS_SIZE = 0x10000;
constexpr int V2_SMOOTH_MAX_STEP = 48;       // px per tick beyond which a slot is not interpolated

struct Snap {
    uint8_t ds[DS_SIZE];
    uint8_t fs[FS_SIZE];
    uint32_t par_acc_x, par_acc_y;
    uint64_t t;                 // SDL_GetPerformanceCounter at capture
    bool valid, tile_frame, fullscreen;
    int w;                      // the frame width the tick rendered at (v2_fbw; UX stage 9 step 4)
};
// g_frame[0/1] = the two newest GAME-frame snapshots (positions/camera),
// g_flip = the newest page flip (sprite frames, UI, FS map)
Snap g_frame[2];
int g_fcur = -1;
Snap g_flip;
int g_last_iter = -1;
std::mutex g_mx;

// presenter-local copies (the lock is held only for the memcpy)
Snap g_prev_local, g_cur_local, g_flip_local;
uint8_t g_work[DS_SIZE];

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
    S.valid = true;
}

// Game thread, at every page flip (v2_swap_render_buf): the flip snapshot
// always; a GAME-frame snapshot at the first flip of a new game frame
// (render1 — the first image drawn from the tick's new positions).
void v2_smooth_capture(void) {
    uint8_t* s = v2_vm_get_shadow_ds();
    if (!s) return;
    std::lock_guard<std::mutex> lock(g_mx);
    fill(g_flip, s);
    if (v2_dbg_pre_vm_iter != g_last_iter) {
        g_last_iter = v2_dbg_pre_vm_iter;
        int nxt = (g_fcur + 1) & 1;
        memcpy(&g_frame[nxt], &g_flip, sizeof(Snap));
        g_fcur = nxt;
    }
}

// Presenter thread: paint an interpolated frame into `out` (rows per v2_view_rows,
// width = the snapshot's v2_fbw, reported in v2_smooth_last_w).
// false = nothing to interpolate (option off, chunk screens, level change,
// first tick) — the caller shows the tick frame as before.
bool v2_smooth_render(uint8_t* out) {
    v2_options_ensure_loaded();
    if (!v2_options.smooth.load()) { v2_smooth_last_reason = 1; return false; }
    {
        std::lock_guard<std::mutex> lock(g_mx);
        if (g_fcur < 0 || !g_flip.valid) { v2_smooth_last_reason = 2; return false; }
        const Snap& C = g_frame[g_fcur];
        const Snap& P = g_frame[g_fcur ^ 1];
        if (!C.valid || !P.valid) { v2_smooth_last_reason = 3; return false; }
        if (!C.tile_frame || !P.tile_frame || !g_flip.tile_frame) { v2_smooth_last_reason = 4; return false; }
        if (C.fullscreen != P.fullscreen || C.t <= P.t) { v2_smooth_last_reason = 5; return false; }
        if (C.w != P.w || C.w != g_flip.w) { v2_smooth_last_reason = 5; return false; }   // a width change = a level change
        if (rd16(C.ds, DS_LEVEL) != rd16(P.ds, DS_LEVEL) || rd16(g_flip.ds, DS_LEVEL) != rd16(C.ds, DS_LEVEL)) {
            v2_smooth_last_reason = 6; return false;                  // same level throughout
        }
        memcpy(&g_cur_local, &C, sizeof(Snap));
        memcpy(&g_prev_local, &P, sizeof(Snap));
        memcpy(&g_flip_local, &g_flip, sizeof(Snap));
    }
    const Snap& C = g_cur_local;
    const Snap& P = g_prev_local;
    const Snap& L = g_flip_local;
    const double freq = (double)SDL_GetPerformanceFrequency();
    const double period = (double)(C.t - P.t) / freq;               // s between the two game frames
    if (period < 0.020 || period > 0.120) { v2_smooth_last_reason = 7; return false; }   // a stall or a pause: no lerp
    double t = (double)(SDL_GetPerformanceCounter() - C.t) / freq / period;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    v2_smooth_last_t = (float)t;
    v2_smooth_last_reason = 0;

    memcpy(g_work, L.ds, DS_SIZE);      // the newest flip: sprite frames, UI, everything not interpolated
    // camera: viewport + the tile-scroll state derived from it (vp >> 3)
    int16_t cx = rd16(C.ds, DS_VIEWPORT_X), px = rd16(P.ds, DS_VIEWPORT_X);
    int16_t cy = rd16(C.ds, DS_VIEWPORT_Y), py = rd16(P.ds, DS_VIEWPORT_Y);
    if (cx != px || cy != py) {
        if (cx - px <= V2_SMOOTH_MAX_STEP && px - cx <= V2_SMOOTH_MAX_STEP &&
            cy - py <= V2_SMOOTH_MAX_STEP && py - cy <= V2_SMOOTH_MAX_STEP) {
            int16_t vx = lerp16(px, cx, t), vy = lerp16(py, cy, t);
            if (vx < 0) vx = 0;
            if (vy < 0) vy = 0;
            wr16(g_work, DS_VIEWPORT_X, vx);
            wr16(g_work, DS_VIEWPORT_Y, vy);
            wr16(g_work, DS_SCROLL_COL, (int16_t)((uint16_t)vx >> 3));
            wr16(g_work, DS_SCROLL_ROW, (int16_t)((uint16_t)vy >> 3));
        }
    }
    // sub-sprites: world positions of the slots active in both snapshots
    for (uint32_t obj = 0; obj <= 0xFE; obj += 2) {
        uint16_t fc = (uint16_t)rd16(C.ds, obj + OBJ_SPRITE_FLAGS);
        uint16_t fp = (uint16_t)rd16(P.ds, obj + OBJ_SPRITE_FLAGS);
        if (!(fc & 0x8000) || !(fp & 0x8000)) continue;
        int16_t ax = rd16(C.ds, obj + OBJ_SPRITE_X), bx = rd16(P.ds, obj + OBJ_SPRITE_X);
        int16_t ay = rd16(C.ds, obj + OBJ_SPRITE_Y), by = rd16(P.ds, obj + OBJ_SPRITE_Y);
        if (ax == bx && ay == by) continue;
        if (ax - bx > V2_SMOOTH_MAX_STEP || bx - ax > V2_SMOOTH_MAX_STEP ||
            ay - by > V2_SMOOTH_MAX_STEP || by - ay > V2_SMOOTH_MAX_STEP) continue;
        wr16(g_work, obj + OBJ_SPRITE_X, lerp16(bx, ax, t));
        wr16(g_work, obj + OBJ_SPRITE_Y, lerp16(by, ay, t));
    }
    // parallax autoscroll accumulators (units of 1/1792 px): lerp unless wrapped
    uint32_t acc[2] = { C.par_acc_x, C.par_acc_y };
    if (C.par_acc_x >= P.par_acc_x && C.par_acc_x - P.par_acc_x < 1792u * 64u)
        acc[0] = P.par_acc_x + (uint32_t)((double)(C.par_acc_x - P.par_acc_x) * t);
    if (C.par_acc_y >= P.par_acc_y && C.par_acc_y - P.par_acc_y < 1792u * 64u)
        acc[1] = P.par_acc_y + (uint32_t)((double)(C.par_acc_y - P.par_acc_y) * t);

    // debug: V2_SMOOTH_DUMP=<dir> — one line per interpolated frame: game
    // frame, fraction, camera prev/cur/lerp, sub-sprite slot 0 prev/cur/lerp
    {
        static FILE* lf = nullptr; static int init = 0;
        if (!init) { init = 1; const char* dd = getenv("V2_SMOOTH_DUMP");
            if (dd && *dd) { char path[512]; snprintf(path, sizeof path, "%s/smooth_log.txt", dd); lf = fopen(path, "w"); } }
        if (lf) fprintf(lf, "f%d t=%.3f period=%.1fms vp %d,%d -> %d,%d = %d,%d | slot0 %d,%d -> %d,%d = %d,%d\n",
                        v2_dbg_pre_vm_iter, t, period * 1000.0, px, py, cx, cy, rd16(g_work, DS_VIEWPORT_X), rd16(g_work, DS_VIEWPORT_Y),
                        rd16(P.ds, OBJ_SPRITE_X), rd16(P.ds, OBJ_SPRITE_Y), rd16(C.ds, OBJ_SPRITE_X), rd16(C.ds, OBJ_SPRITE_Y),
                        rd16(g_work, OBJ_SPRITE_X), rd16(g_work, OBJ_SPRITE_Y));
    }
    // the passes, in the gameplay frame's order, on the presenter's buffers
    v2_tls_ds = g_work;
    v2_tls_out = out;
    v2_fbw = L.w;                   // the presenter thread renders at the snapshot's width
    v2_smooth_last_w = L.w;
    v2_tls_fs = L.fs;
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
