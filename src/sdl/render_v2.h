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

// V2 rendering buffer — single persistent buffer, like drawBuffer in the original.
// Game thread writes, v2_swap_render_buf copies to v2_display_buf for render thread.
// Linear format: [y * 320 + x] = palette index, 320x200
extern uint8_t  v2_render_buf[320*200];

// Display buffer — game copies completed frame here under lock,
// render thread reads it under the same lock. No race.
extern uint8_t  v2_display_buf[320*200];
extern std::mutex v2_display_mutex;

// HUD buffer — rendered independently from game data, 320x64
// Screen rows 176-239 (VGA split screen: always from VGA address 0)
extern uint8_t  v2_hud_buf[320*64];

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
extern void v2_draw_flagged_tiles(uint16_t ds_val);
extern void v2_draw_ui(uint16_t ds_val);
extern void v2_swap_render_buf();
extern void v2_set_m2c_base(void* base);

// HUD rendering — called from seg000
extern void v2_draw_hud_background(uint16_t ds_val, uint16_t chunk_seg, uint16_t plane_size);
extern void v2_draw_viewport_chunk(uint16_t chunk_seg, uint16_t plane_size);
extern void v2_clear_viewport_chunk();
extern void v2_draw_hud_item(uint16_t ds_val, uint16_t slot_di, uint16_t item_ax);
extern void v2_draw_hud_portrait(uint16_t ds_val, uint16_t viking_di, uint16_t portrait_si);
extern void v2_draw_hud_selector(uint16_t ds_val, uint16_t slot_di);
extern void v2_draw_hud_healthbar(uint16_t ds_val, uint16_t health_ax, uint16_t viking_bx, uint16_t pos_di);

#endif // RENDER_V2_H
