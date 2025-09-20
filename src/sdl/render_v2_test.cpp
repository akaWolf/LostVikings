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
#ifdef V2_ONLY
    else {
        // V2_ONLY: read palette directly from shadow DS at 0x8202.
        // Format: 256 × 3 bytes RGB, 6-bit values (shift left 2 to get 8-bit).
        extern uint8_t* v2_vm_get_shadow_ds();
        uint8_t* shad = v2_vm_get_shadow_ds();
        if (shad) {
            uint8_t* pal = shad + 0x8202;
            for (int i = 0; i < 256; i++) {
                myDrawInfo_v2->drawPalette[i].r = pal[i*3 + 0] << 2;
                myDrawInfo_v2->drawPalette[i].g = pal[i*3 + 1] << 2;
                myDrawInfo_v2->drawPalette[i].b = pal[i*3 + 2] << 2;
                myDrawInfo_v2->drawPalette[i].a = 255;
            }
            // Debug: log first few palette entries + display buf state every 60 frames.
            static int _pal_dbg = 0; _pal_dbg++;
            if (_pal_dbg <= 3 || _pal_dbg % 120 == 0) {
                int non_black = 0;
                for (int i = 0; i < 256; i++)
                    if (pal[i*3] || pal[i*3+1] || pal[i*3+2]) non_black++;
                int buf_non_zero = 0;
                int render_non_zero = 0;
                int hud_non_zero = 0;
                extern bool v2_vm_in_frame;
                {
                    extern uint8_t v2_display_buf[];
                    extern uint8_t v2_render_buf[];
                    extern uint8_t v2_hud_buf[];
                    for (int i = 0; i < 320*176; i++) {
                        if (v2_display_buf[i]) buf_non_zero++;
                        if (v2_render_buf[i]) render_non_zero++;
                    }
                    for (int i = 0; i < 320*64; i++)
                        if (v2_hud_buf[i]) hud_non_zero++;
                }
                fprintf(stderr,
                  "V2-PAL-DBG[%d]: pal_nb=%d display_nz=%d render_nz=%d hud_nz=%d in_frame=%d\n",
                  _pal_dbg, non_black, buf_non_zero, render_non_zero, hud_non_zero, v2_vm_in_frame ? 1 : 0);
            }
        }
    }
#endif

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
    // Copy HUD (rows 176-239) from v2_hud_buf
    for (int y = 0; y < 64; y++) {
        memcpy(sbuf + (176 + y) * 344, v2_hud_buf + y * 320, 320);
        memset(sbuf + (176 + y) * 344 + 320, 0, 24); // padding
    }
}
