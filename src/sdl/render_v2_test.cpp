// ============================================================================
// Callback для второго окна (render_v2)
// ============================================================================

#include <cstdio>
#include <cstdint>
#include <cstring>
#include "render_v2.h"

// Оригинальная структура VGA памяти (совпадает с vikings.exe_seg000.cpp)
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

    // Собираем линейный stableBuffer из live drawBuffer.
    // VGA planar buf[a*4+p] == buf[y*344+x] (a=y*86+x/4, p=x%4), поэтому memcpy достаточен.
    //   game area (rows 0-175): livebuf[P*4 .. P*4+176*344)  — активная VGA страница
    //   UI   area (rows 176+):  livebuf[0  .. 64*344)        — фиксированный VGA addr 0
    const uint8_t* livebuf = myDrawInfo->drawBuffer;
    uint32_t game_off = myDrawInfo->myOffset * 4u + myDrawInfo->myPixelOffset;

    if (game_off + 176u * 344u <= 262144u)
        memcpy(sbuf, livebuf + game_off, 176u * 344u);
    else
        memset(sbuf, 0, 176u * 344u);

    uint32_t game_end = game_off + 176u * 344u;
    if (game_off >= 64u * 344u) {
        // Нормальный геймплей: HUD живёт по VGA addr 0
        memcpy(sbuf + 176u * 344u, livebuf, 64u * 344u);
    } else if (game_end + 64u * 344u <= 262144u) {
        // Меню/заставка: продолжаем читать игровую страницу
        memcpy(sbuf + 176u * 344u, livebuf + game_end, 64u * 344u);
    } else {
        memset(sbuf + 176u * 344u, 0, 64u * 344u);
    }
}
