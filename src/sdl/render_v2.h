#ifndef RENDER_V2_H
#define RENDER_V2_H

#include <cstdint>
#include <atomic>
#include <mutex>
#include <SDL2/SDL.h>

// ============================================================================
// Заголовочный файл для второго окна (render_v2)
// ============================================================================

// Структура данных отрисовки (дубликат для v2)
struct myDrawInfoS_v2
{
  uint8_t drawBuffer[65536*4];   // не используется напрямую
  uint8_t stableBuffer[65536*4]; // рендер-поток: линейный буфер 344x240
  SDL_Color drawPalette[256];
  uint32_t myOffset;
  uint8_t myPixelOffset;
};

// Pointer to emulated memory base (m2c::m), set once at startup
extern uint8_t* v2_m2c_base;

// V2 rendering buffer (game thread writes, render thread reads)
// Linear format: [y * 320 + x] = palette index, 320x200
extern uint8_t  v2_render_buf[2][320*200];
extern std::atomic<int> v2_render_fill;  // which buffer game thread writes to

// Display buffer — game copies completed frame here under lock,
// render thread reads it under the same lock. No race.
extern uint8_t  v2_display_buf[320*200];
extern std::mutex v2_display_mutex;

// Глобальные переменные (extern)
extern struct myDrawInfoS_v2* myDrawInfo_v2;
extern uint16_t input_keys_v2;
extern bool need_quit_v2;

// Функции API
extern void render_init_v2(void* state);
extern void render_callback_v2(void* state);

// V2 rendering functions — called from seg000
extern void v2_draw_tiles(uint16_t ds_val);
extern void v2_draw_sprites(uint16_t ds_val);
extern void v2_draw_ui(uint16_t ds_val);
extern void v2_swap_render_buf();
extern void v2_set_m2c_base(void* base);

#endif // RENDER_V2_H
