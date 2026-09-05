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

    // UX stage 9: an interpolated frame between the two newest ticks (own
    // passes on a snapshot, v2_smooth.cpp) — else the tick frame as before.
    static uint8_t v2_smooth_frame[320 * 240];
    const bool smooth = v2_smooth_render(v2_smooth_frame);
    // debug: V2_SMOOTH_DUMP=<dir> writes the first 48 presented frames after
    // game frame 100 as <dir>/pf_<n>_f<game frame>_t<fraction>.ppm (+ the
    // tick frame it interpolates towards) — proves the sub-tick positions.
    {
        static int dump = -1; static const char* dd = nullptr; static int n = 0; static int from = 100;
        if (dump < 0) { dd = getenv("V2_SMOOTH_DUMP"); dump = (dd && *dd) ? 1 : 0;
                        const char* f0 = getenv("V2_SMOOTH_DUMP_FROM"); if (f0 && *f0) from = atoi(f0); }
        // v2_dbg_pre_vm_iter: file-scope extern (top of file)
        if (dump == 1 && v2_dbg_pre_vm_iter >= from && n < 48) {
            extern int v2_smooth_last_reason;
            char path[512]; snprintf(path, sizeof path, "%s/pf_%02d_f%d_t%.2f_%s%d_%u.ppm", dd, n, v2_dbg_pre_vm_iter,
                                     smooth ? v2_smooth_last_t : -1.0f, smooth ? "sm" : "tick", v2_smooth_last_reason,
                                     (unsigned)SDL_GetTicks());
            FILE* f = fopen(path, "wb");
            if (f) {
                const uint8_t* src = smooth ? v2_smooth_frame : v2_display_buf;
                fprintf(f, "P6\n320 200\n255\n");
                for (int i = 0; i < 320 * 200; i++) { const SDL_Color& c = myDrawInfo_v2->drawPalette[src[i]]; fputc(c.r, f); fputc(c.g, f); fputc(c.b, f); }
                fclose(f);
            }
            n++;
        }
    }
    // Copy viewport (rows 0-175) from the chosen frame under lock
    {
        std::lock_guard<std::mutex> lock(v2_display_mutex);
        const uint8_t* src = smooth ? v2_smooth_frame : v2_display_buf;
        for (int y = 0; y < 176; y++) {
            memcpy(sbuf + y * 344, src + y * 320, 320);
            memset(sbuf + y * 344 + 320, 0, 24); // padding
        }
        // F12 PGM dump (v2 path). Clear flag AFTER both orig+v2 saved.
        extern std::atomic<bool> g_dump_pgm_request;
        if (g_dump_pgm_request.load(std::memory_order_acquire)) {
            FILE* f = fopen("/tmp/v2_ladder.ppm", "wb");
            if (f) {
                const int H = v2_display_fullscreen ? v2_display_fullscreen : 176;   // UX stage 0/9: map rows shown
                fprintf(f, "P6\n320 %d\n255\n", H);
                for (int y = 0; y < H; y++) {
                    for (int x = 0; x < 320; x++) {
                        uint8_t c = v2_display_buf[y * 320 + x];
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
        if (v2_display_fullscreen) {
            const uint8_t* src2 = smooth ? v2_smooth_frame : v2_display_buf;   // UX stage 9
            // UX stage 0/9: LVX full-screen scene (200 rows) or an LVX_TALL224
            // level (224 rows) — rows 176.. come from the map render, the rest
            // stay black. The HUD band is never painted on these slots.
            const int shown = v2_display_fullscreen - 176;
            for (int y = 0; y < 64; y++) {
                if (y < shown) memcpy(sbuf + (176 + y) * 344, src2 + (176 + y) * 320, 320);
                else        memset(sbuf + (176 + y) * 344, 0, 320);
                memset(sbuf + (176 + y) * 344 + 320, 0, 24); // padding
            }
        } else {
            for (int y = 0; y < 64; y++) {
                memcpy(sbuf + (176 + y) * 344, v2_display_hud_buf + y * 320, 320);
                memset(sbuf + (176 + y) * 344 + 320, 0, 24); // padding
            }
        }
    }
}
