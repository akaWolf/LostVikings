#ifndef RENDER_V2_H
#define RENDER_V2_H

#include <cstdint>
#include <SDL2/SDL.h>

// ============================================================================
// Заголовочный файл для второго окна (render_v2)
// ============================================================================

// Структура данных отрисовки (дубликат для v2)
struct myDrawInfoS_v2
{
  uint8_t drawBuffer[65536*4];   // игровой поток пишет (mirror + replay source)
  uint8_t stableBuffer[65536*4]; // только рендер-поток: snapshot + replay результат
  SDL_Color drawPalette[256];
  uint32_t myOffset;
  uint8_t myPixelOffset;
};

// Глобальные переменные (extern)
extern struct myDrawInfoS_v2* myDrawInfo_v2;
extern uint16_t input_keys_v2;
extern bool need_quit_v2;

// Функции API
extern void render_init_v2(void* state);
extern void render_callback_v2(void* state);

#endif // RENDER_V2_H
