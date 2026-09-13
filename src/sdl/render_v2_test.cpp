// ============================================================================
// Callback для второго окна (render_v2)
// Рендер-поток: копирует v2_render_buf в stableBuffer с палитрой
// ============================================================================

#include <cstdio>
#include <cstdint>
#include <cstring>
#include "render_v2.h"

// Оригинальная структура VGA памяти (для палитры)
#include <SDL2/SDL.h>
extern int v2_dbg_pre_vm_iter;   // game-frame counter (v2_vm.cpp), C++ linkage — declared once at file scope (clang rejects block externs inside extern "C" functions)
struct myDrawInfoS_orig {
    uint8_t drawBuffer[65536*4];
    SDL_Color drawPalette[256];
    uint32_t myOffset;
    uint8_t myPixelOffset;
};
extern struct myDrawInfoS_orig* myDrawInfo;

// Функция callback вызывается каждый кадр рендер-потоком
int v2_present_w = 320;   // UX stage 9 step 4: the width of the frame in stableBuffer (read by v2_present_frame)
const V2PresentLayers* v2_present_layers_cur = nullptr;   // render_v2.h: the sub-pixel layers of the frame being presented (nullptr: the flat frame)
bool v2_present_ref_valid = false;                        // render_v2.h: stableBuffer holds the flat frame beside the layers (V2_KX_SELFTEST)
void render_callback_v2(void* state)
{
    static bool palette_copied = false;

    if (!myDrawInfo_v2) return;

    // Read palette from v2_display_palette SNAPSHOT (captured from SHADOW DS
    // atomically with v2_display_buf at v2_swap_render_buf time). v2 stays
    // independent from orig per CLAUDE.md (no real→shadow copy), and pixels
    // + palette stay matched across level transitions.
    {
        extern SDL_Color v2_display_palette[256];
        extern bool v2_display_palette_valid;
        if (v2_display_palette_valid) {
            std::lock_guard<std::mutex> lock(v2_display_mutex);
            memcpy(myDrawInfo_v2->drawPalette, v2_display_palette, 256 * sizeof(SDL_Color));
            palette_copied = true;
        }
        // Pre-game (no swap yet) → palette stays zero (window black) until first swap
    }

    (void)state;

    uint8_t* sbuf = myDrawInfo_v2->stableBuffer;

    // The frame source. The game build (2026-09-15): the presenter is the only renderer — every
    // frame is composed from the snapshots the game thread publishes at its flips
    // (v2_present_compose, v2_smooth.cpp): the newest flip, or an interpolation between the two
    // newest under SMOOTH. The test build presents the shadow-VGA page the game thread
    // published (v2_display_buf), as before.
    // debug: V2_SMOOTH_TIME=1 — the presenter's own composition cost per present
    // (v2_present_compose: the passes), avg/max ms every 2 s
    static int sm_time = -1; if (sm_time < 0) sm_time = getenv("V2_SMOOTH_TIME") ? 1 : 0;
    const uint64_t sm_t0 = sm_time ? SDL_GetPerformanceCounter() : 0;
    const uint8_t* frame_src; int FW; int fs_rows; const uint8_t* hud_src; const V2DisplayBadge* badge_src; bool smooth;
#ifdef V2_ONLY
    static uint8_t black[V2_FB_MAX_W * 240];
    V2PresentFrame pf; pf.layers = nullptr;
    // the sub-pixel presentation (render_v2.h V2PresentLayers): a tile frame comes as layers for
    // updateDraw_v2's GPU composition and no 1x frame (frame_src nullptr: the copies and the 1x
    // dumps below are skipped) — unless the selftest asked for the flat frame beside them
    if (v2_present_compose(&pf)) { frame_src = pf.map; FW = pf.w; fs_rows = pf.rows; hud_src = pf.hud; badge_src = pf.badges; smooth = pf.smooth; }
    else { frame_src = black; FW = 320; fs_rows = 0; hud_src = black; badge_src = nullptr; smooth = false; }
    v2_present_layers_cur = pf.layers;
    v2_present_ref_valid = pf.layers != nullptr && frame_src != nullptr;
    v2_display_w = FW; v2_display_fullscreen = fs_rows;   // the presenter owns these in the game build (updateDraw_v2 reads the rows)
#else
    extern uint8_t v2_display_hud_buf[];
    frame_src = v2_display_buf; FW = v2_display_w; fs_rows = v2_display_fullscreen; hud_src = v2_display_hud_buf; badge_src = v2_display_badge; smooth = false;
#endif
    if (sm_time) {
        static double acc = 0.0, mx = 0.0; static int n = 0, composed = 0; static uint32_t last_ms = 0;
        const double ms = (double)(SDL_GetPerformanceCounter() - sm_t0) / (double)SDL_GetPerformanceFrequency() * 1000.0;
        acc += ms; if (ms > mx) mx = ms; n++; if (smooth) composed++;
        const uint32_t now = SDL_GetTicks();
        if (last_ms == 0) last_ms = now;
        if (now - last_ms >= 2000) {
            fprintf(stderr, "V2-SMOOTH-TIME presents=%d composed=%d avg=%.2fms max=%.2fms\n", n, composed, n ? acc / n : 0.0, mx);
            acc = 0.0; mx = 0.0; n = 0; composed = 0; last_ms = now;
        }
    }
    v2_present_w = FW;   // UX stage 9 step 4: the frame's width (its row stride)
    // debug: V2_PRESENT_DUMP=<dir>:<from>-<to> writes EVERY presented frame whose
    // game frame lies in [from, to] as <dir>/pf_<n>_f<game frame>_<ms>.ppm (the
    // 320x240 canvas the presenter shows: the interpolated frame or the published
    // flip, through the palette in effect) — what the user saw, present by present.
    {
        static int dump = -1; static char dir[480]; static int from = 0, to = -1, n = 0;
        if (dump < 0) {
            const char* e = getenv("V2_PRESENT_DUMP"); dump = 0;
            if (e && *e) { snprintf(dir, sizeof dir, "%s", e); char* c = strrchr(dir, ':');
                           if (c && sscanf(c + 1, "%d-%d", &from, &to) == 2) { *c = 0; dump = 1; } }
        }
        if (dump == 1 && frame_src && v2_dbg_pre_vm_iter >= from && v2_dbg_pre_vm_iter <= to && n < 4000) {
            extern int v2_smooth_last_reason;   // the file name tells whether the presenter composed the frame itself (sm) or showed the flip (tick<reason>)
            char path[560]; snprintf(path, sizeof path, "%s/pf_%04d_f%d_%u_%s%d.ppm", dir, n, v2_dbg_pre_vm_iter, (unsigned)SDL_GetTicks(),
                                     smooth ? "sm" : "tick", v2_smooth_last_reason);
            FILE* f = fopen(path, "wb");
            if (f) {
                const uint8_t* src = frame_src;
                fprintf(f, "P6\n%d 240\n255\n", FW);
                for (int i = 0; i < FW * 240; i++) { const SDL_Color& c = myDrawInfo_v2->drawPalette[src[i]]; fputc(c.r, f); fputc(c.g, f); fputc(c.b, f); }
                fclose(f);
            }
            n++;
        }
    }
    // debug: V2_SMOOTH_DUMP=<dir> writes the first 48 presented frames after
    // game frame 100 as <dir>/pf_<n>_f<game frame>_t<fraction>.ppm (+ the
    // tick frame it interpolates towards) — proves the sub-tick positions.
    {
        static int dump = -1; static const char* dd = nullptr; static int n = 0; static int from = 100;
        if (dump < 0) { dd = getenv("V2_SMOOTH_DUMP"); dump = (dd && *dd) ? 1 : 0;
                        const char* f0 = getenv("V2_SMOOTH_DUMP_FROM"); if (f0 && *f0) from = atoi(f0); }
        // v2_dbg_pre_vm_iter: file-scope extern (top of file)
        if (dump == 1 && frame_src && v2_dbg_pre_vm_iter >= from && n < 48) {
            extern int v2_smooth_last_reason;
            char path[512]; snprintf(path, sizeof path, "%s/pf_%02d_f%d_t%.2f_%s%d_%u.ppm", dd, n, v2_dbg_pre_vm_iter,
                                     smooth ? v2_smooth_last_t : -1.0f, smooth ? "sm" : "tick", v2_smooth_last_reason,
                                     (unsigned)SDL_GetTicks());
            FILE* f = fopen(path, "wb");
            if (f) {
                const uint8_t* src = frame_src;
                fprintf(f, "P6\n%d 200\n255\n", FW);
                for (int i = 0; i < FW * 200; i++) { const SDL_Color& c = myDrawInfo_v2->drawPalette[src[i]]; fputc(c.r, f); fputc(c.g, f); fputc(c.b, f); }
                fclose(f);
            }
            n++;
        }
    }
    if (!frame_src) return;   // the layers alone: nothing to lay out in stableBuffer (updateDraw_v2 composes them)
    // Copy viewport (rows 0-175) from the chosen frame under lock
    {
        std::lock_guard<std::mutex> lock(v2_display_mutex);
        const uint8_t* src = frame_src;
        for (int y = 0; y < 176; y++) {
            memcpy(sbuf + y * V2_FB_MAX_W, src + y * FW, FW);
            memset(sbuf + y * V2_FB_MAX_W + FW, 0, V2_FB_MAX_W - FW); // padding
        }
        // F12 PGM dump (v2 path). Clear flag AFTER both orig+v2 saved.
        extern std::atomic<bool> g_dump_pgm_request;
        if (g_dump_pgm_request.load(std::memory_order_acquire)) {
            FILE* f = fopen("/tmp/v2_ladder.ppm", "wb");
            if (f) {
                const int H = fs_rows ? fs_rows : 176;   // UX stage 0/9: map rows shown
                fprintf(f, "P6\n%d %d\n255\n", FW, H);
                for (int y = 0; y < H; y++) {
                    for (int x = 0; x < FW; x++) {
                        uint8_t c = frame_src[y * FW + x];
                        fputc(myDrawInfo_v2->drawPalette[c].r, f);
                        fputc(myDrawInfo_v2->drawPalette[c].g, f);
                        fputc(myDrawInfo_v2->drawPalette[c].b, f);
                    }
                }
                fclose(f);
                fprintf(stderr, "PGM-DUMP: saved /tmp/v2_ladder.ppm\n");
            }
            // Clear flag — orig render side might've already saved before this point
            g_dump_pgm_request.store(false, std::memory_order_release);
        }
    }
    // Copy HUD (rows 176-239) from v2_display_hud_buf SNAPSHOT (captured atomically
    // with v2_display_buf + v2_display_palette at v2_swap_render_buf time). Live
    // v2_hud_buf without snapshot caused HUD icon flicker on level transitions —
    // HUD pixels updated mid-frame while palette snapshot was from earlier swap.
    extern uint8_t v2_display_hud_buf[];
    {
        std::lock_guard<std::mutex> lock(v2_display_mutex);
        if (fs_rows) {
            const uint8_t* src2 = frame_src;   // UX stage 9
            // UX stage 0/9: LVX full-screen scene (200 rows) or an LVX_TALL224
            // level (224 rows) — rows 176.. come from the map render, the rest
            // stay black. The HUD band is never painted on these slots.
            const int shown = fs_rows - 176;
            for (int y = 0; y < 64; y++) {
                if (y < shown) memcpy(sbuf + (176 + y) * V2_FB_MAX_W, src2 + (176 + y) * FW, FW);
                else        memset(sbuf + (176 + y) * V2_FB_MAX_W, 0, FW);
                memset(sbuf + (176 + y) * V2_FB_MAX_W + FW, 0, V2_FB_MAX_W - FW); // padding
            }
        } else {
            // the 320-px HUD art centred on a wide frame, the stone wall mirrored outward on
            // the wings, the co-op badges on the portraits (v2_layout_hud_band, v2_smooth.cpp —
            // the flip dump paints the same canvas)
            v2_layout_hud_band(sbuf + 176 * V2_FB_MAX_W, V2_FB_MAX_W, FW, hud_src, badge_src);
        }
    }
}
