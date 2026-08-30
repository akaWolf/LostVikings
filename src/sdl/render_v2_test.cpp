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

    // Copy viewport (rows 0-175) from display buffer under lock
    {
        std::lock_guard<std::mutex> lock(v2_display_mutex);
        const uint8_t* src = v2_display_buf;
        for (int y = 0; y < 176; y++) {
            memcpy(sbuf + y * 344, src + y * 320, 320);
            memset(sbuf + y * 344 + 320, 0, 24); // padding
        }
        // F12 PGM dump (v2 path). Clear flag AFTER both orig+v2 saved.
        extern std::atomic<bool> g_dump_pgm_request;
        if (g_dump_pgm_request.load(std::memory_order_acquire)) {
            FILE* f = fopen("/tmp/v2_ladder.ppm", "wb");
            if (f) {
                const int H = v2_display_fullscreen ? 200 : 176;   // UX stage 0
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
            // UX stage 0: LVX full-screen scene — rows 176..199 come from the
            // map render (v2_display_buf is 320x200), rows 200..239 stay black.
            // The HUD band is never painted on these slots ([25CF] bit0 = 0).
            for (int y = 0; y < 64; y++) {
                if (y < 24) memcpy(sbuf + (176 + y) * 344, v2_display_buf + (176 + y) * 320, 320);
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
