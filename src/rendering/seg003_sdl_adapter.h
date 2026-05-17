#ifndef SEG003_SDL_ADAPTER_H
#define SEG003_SDL_ADAPTER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Адаптер для интеграции seg003 с SDL буфером
// Устанавливает SDL буфер как "VGA память" для наших функций
void seg003_set_sdl_buffer(uint8_t* buffer, int width, int height);

// Вызывает sub_1dd9c_main_render_loop с использованием SDL буфера
void seg003_render_to_sdl(void);

#ifdef __cplusplus
}
#endif

#endif // SEG003_SDL_ADAPTER_H
