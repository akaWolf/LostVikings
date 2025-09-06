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

    // Копируем палитру каждый кадр (игра может менять её)
    if (myDrawInfo) {
        if (!palette_copied) {
            int non_black = 0;
            for (int i = 0; i < 256; i++) {
                if (myDrawInfo->drawPalette[i].r != 0 ||
                    myDrawInfo->drawPalette[i].g != 0 ||
                    myDrawInfo->drawPalette[i].b != 0)
                    non_black++;
            }
            if (non_black > 10) {
                memcpy(myDrawInfo_v2->drawPalette, myDrawInfo->drawPalette, 256 * sizeof(SDL_Color));
                palette_copied = true;
            }
        } else {
            memcpy(myDrawInfo_v2->drawPalette, myDrawInfo->drawPalette, 256 * sizeof(SDL_Color));
        }
    }

    (void)state;

    uint8_t* sbuf = myDrawInfo_v2->stableBuffer;

    // Copy from display buffer under lock (game thread writes here at swap)
    {
        std::lock_guard<std::mutex> lock(v2_display_mutex);
        const uint8_t* src = v2_display_buf;
        for (int y = 0; y < 200; y++) {
            memcpy(sbuf + y * 344, src + y * 320, 320);
            memset(sbuf + y * 344 + 320, 0, 24); // padding
        }
    }
    // Clear rows 200-239
    memset(sbuf + 200 * 344, 0, 40 * 344);
}
