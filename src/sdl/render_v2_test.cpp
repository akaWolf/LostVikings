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
    }
    // Copy HUD (rows 176-239) from v2_display_hud_buf SNAPSHOT (captured atomically
    // with v2_display_buf + v2_display_palette at v2_swap_render_buf time). Live
    // v2_hud_buf without snapshot caused HUD icon flicker on level transitions —
    // HUD pixels updated mid-frame while palette snapshot was from earlier swap.
    extern uint8_t v2_display_hud_buf[];
    {
        std::lock_guard<std::mutex> lock(v2_display_mutex);
        for (int y = 0; y < 64; y++) {
            memcpy(sbuf + (176 + y) * 344, v2_display_hud_buf + y * 320, 320);
            memset(sbuf + (176 + y) * 344 + 320, 0, 24); // padding
        }
    }
}
