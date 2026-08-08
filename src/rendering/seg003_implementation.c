// Восстанавливаем базовую версию
#include "seg003_implementation.h"

/* (#84) fn-test isolation: the RECREATED seg003 layer is port scaffolding
 * (a duplicate renderer next to the original CALLF path). Inside the unit
 * isolator it would paint extras into the oracle drawBuffer that the v2
 * model legitimately does not have — silence it there. */
extern int v2_fntest_isolated_active;

#include <string.h>
#include <stdio.h>

// Подключаем vikings.exe.h и _data.h для доступа к m2c типам и памяти
#ifdef __cplusplus
extern "C++" {
#include "../vikings.exe.h"
#include "../_data.h"
}
#else
struct _STATE;
#endif

// Глобальные переменные
GameObject* g_game_objects = NULL;
int g_object_count = 0;

// Внешние переменные SDL
extern struct myDrawInfoS {
    uint8_t drawBuffer[65536*4];
    void* drawPalette;
    uint32_t myOffset;
    uint8_t myPixelOffset;
}* myDrawInfo;

extern struct myDrawInfoS* myDrawInfo_v2;

// Инициализация
void seg003_init(GameObject* objects, int object_count, uint16_t* tilemap, uint16_t tilemap_width) {
    g_game_objects = objects;
    g_object_count = object_count;
}

// ============================================================================
// seg003_f9e_proc - Pixel Mask Drawing Engine
// ============================================================================
// Draws sprite using pixel masks (bitmasks)
// Each mask byte indicates which of 8 pixels to draw (bit=1) or skip (bit=0)
void seg003_f9e_proc(uint8_t* mask_stream, uint8_t* pixel_stream, uint8_t* vga_dest, uint16_t width, uint16_t height) {
    if (!mask_stream || !pixel_stream || !vga_dest) return;
    
    uint8_t* pixel_ptr = pixel_stream;
    
    // For each row
    for (int row = 0; row < height; row++) {
        uint8_t* dest_ptr = vga_dest + row * 344;  // 344 = RENDER_WIDTH
        
        // Read 8 mask bytes for this row (64 pixels total)
        uint8_t masks[8];
        for (int i = 0; i < 8; i++) {
            masks[i] = mask_stream[row * 8 + i];
        }
        
        // Process each group of 8 pixels
        for (int group = 0; group < (width / 8); group++) {
            uint8_t mask = masks[group];
            
            // For each bit in mask
            for (int bit = 0; bit < 8; bit++) {
                if (mask & (1 << bit)) {
                    // Draw pixel
                    dest_ptr[group * 8 + bit] = *pixel_ptr;
                    pixel_ptr++;
                }
            }
        }
    }
}

// ============================================================================
// seg003_470_proc - Graphics Command Dispatcher
// ============================================================================
void seg003_470_proc(uint8_t* command_stream, uint8_t* source_data, uint8_t* vga_dest, uint16_t dx_port) {
    (void)command_stream; (void)source_data; (void)vga_dest; (void)dx_port;
    // TODO: Implement command dispatcher if needed
}

// ============================================================================
// seg003_648_proc - Main Object Drawing Dispatcher
// ============================================================================
void seg003_648_proc(GameObject* object) {
    (void)object;
    // TODO: Implement object drawing dispatcher
}

// ============================================================================
// Dirty Rectangle Functions
// ============================================================================
void sub_1cd7b_dirty_rectangle_wrapper(int16_t x1, int16_t y1, int16_t x2, int16_t y2) {
    (void)x1; (void)y1; (void)x2; (void)y2;
}

void sub_1cd7d_dirty_rectangle_core(int16_t x1, int16_t y1, int16_t x2, int16_t y2) {
    (void)x1; (void)y1; (void)x2; (void)y2;
}

// ============================================================================
// sub_1cdef - Culling and Dirty Check
// ============================================================================
bool sub_1cdef_culling_check(GameObject* object) {
    (void)object;
    return true;  // Always render for now
}

// ============================================================================
// sub_1d3b2 - Static Object Drawing
// ============================================================================
void sub_1d3b2_static_object_drawing(GameObject* object) {
    (void)object;
}

// ============================================================================
// sub_1de05 - Dirty Update Position
// ============================================================================
void sub_1de05_dirty_update_position(GameObject* object) {
    if (v2_fntest_isolated_active) return;  /* (#84) unit-world: scaffolding off */
    (void)object;
}

// ============================================================================
// sub_1df6a - State Change Redraw
// ============================================================================
void sub_1df6a_state_change_redraw(GameObject* object) {
    (void)object;
}

// ============================================================================
// sub_1e0c7 - UI Drawing Loop
// ============================================================================
void sub_1e0c7_ui_drawing_loop(void) {
    // TODO: Implement UI drawing
}

// ============================================================================
// sub_1e16d - UI Element Drawing
// ============================================================================
void sub_1e16d_ui_element_drawing(void* ui_element) {
    (void)ui_element;
}

// ============================================================================
// sub_1dd9c - Main Render Loop (basic stub)
// ============================================================================
void sub_1dd9c_main_render_loop(void) {
    // Basic version without state access
}

// ============================================================================
// sub_1c8f1 - Door Rendering
// ============================================================================
void sub_1c8f1_door_rendering(void) {
    // TODO: Implement door rendering
}

void sub_1c8f1_door_rendering_with_state(void* _state) {
    if (v2_fntest_isolated_active) return;  /* (#84) unit-world: scaffolding off */ 
    (void)_state; 
}

// ============================================================================
// sub_1dd9c_main_render_loop_with_state - Main Render Loop with State Access
// ============================================================================
// Фаза 1: no-op. Захват draw calls теперь происходит автоматически
// в seg003_f9e_proc (перехват). Воспроизведение — в render_callback_v2.
// Фаза 3: здесь будет независимый рендерер (без seg003).
void sub_1dd9c_main_render_loop_with_state(void* _state_ptr) {
    if (v2_fntest_isolated_active) return;  /* (#84) unit-world: scaffolding off */
    (void)_state_ptr;
}
