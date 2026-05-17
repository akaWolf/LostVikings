#include "seg003_sdl_adapter.h"
#include "seg003_implementation.h"
#include <string.h>

// Глобальный указатель на SDL буфер
static uint8_t* g_sdl_buffer = NULL;
static int g_sdl_width = 0;
static int g_sdl_height = 0;

void seg003_set_sdl_buffer(uint8_t* buffer, int width, int height) {
    g_sdl_buffer = buffer;
    g_sdl_width = width;
    g_sdl_height = height;
}

// Адаптированная версия seg003_f9e_proc для SDL
static void seg003_f9e_proc_sdl(
    uint8_t* mask_stream,
    uint8_t* pixel_stream,
    int dest_x, int dest_y,
    uint16_t width,
    uint16_t height
) {
    if (!g_sdl_buffer) return;
    
    uint8_t* mask_ptr = mask_stream;
    uint8_t* pixel_ptr = pixel_stream;
    
    // Для каждой строки спрайта
    for (int row = 0; row < height; row++) {
        int screen_y = dest_y + row;
        if (screen_y < 0 || screen_y >= g_sdl_height) {
            mask_ptr += 8;  // Пропускаем маски для этой строки
            continue;
        }
        
        // Читаем 8 масок для этой строки
        uint8_t masks[8];
        for (int i = 0; i < (width / 8); i++) {
            masks[i] = mask_ptr[i];
        }
        
        // Обрабатываем каждую группу из 8 пикселей
        for (int group = 0; group < (width / 8); group++) {
            uint8_t mask = masks[group];
            
            // Для каждого бита в маске
            for (int bit = 0; bit < 8; bit++) {
                if (mask & (1 << bit)) {
                    int screen_x = dest_x + group * 8 + bit;
                    if (screen_x >= 0 && screen_x < g_sdl_width) {
                        // Вычисляем offset в SDL linear буфере
                        uint32_t offset = screen_y * g_sdl_width + screen_x;
                        uint8_t pixel_value = *pixel_ptr;
                        
                        // Просто пишем значение цвета (индекс в палитре)
                        g_sdl_buffer[offset] = pixel_value;
                    }
                    pixel_ptr++;
                }
            }
        }
        
        mask_ptr += 8;  // Следующая строка масок (8 байт)
    }
}

// Адаптированная версия seg003_648_proc для SDL
static void seg003_648_proc_sdl(GameObject* object) {
    if (!object || !object->graphics_data_ptr || !g_sdl_buffer) return;
    
    GraphicsData* gfx = object->graphics_data_ptr;
    
    seg003_f9e_proc_sdl(
        gfx->mask_stream, 
        gfx->pixel_stream,
        object->x_coord, 
        object->y_coord,
        object->width, 
        object->height
    );
}

void seg003_render_to_sdl(void) {
    if (!g_sdl_buffer) return;
    
    // Вызываем главный цикл рендеринга
    // Но используем нашу SDL-адаптированную версию
    extern GameObject* g_game_objects;
    extern int g_object_count;
    
    // Главный цикл рендеринга всех объектов
    for (int i = g_object_count - 1; i >= 0; i--) {
        GameObject* obj = &g_game_objects[i];
        
        // Проверяем флаг активности (бит 15 = 0x8000)
        if (!(obj->flags & 0x8000)) continue;
        
        // Проверяем что объект не скрыт (биты 13-14)
        if (obj->flags & 0x6000) continue;
        
        // Вызываем рендеринг объекта
        seg003_648_proc_sdl(obj);
    }
}
