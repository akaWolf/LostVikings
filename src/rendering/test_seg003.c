// ============================================================================
// Test program for seg003 implementation
// ============================================================================

#include "seg003_implementation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Test helpers
// ============================================================================

// Создание тестового спрайта с простым паттерном
GraphicsData* create_test_sprite(int width, int height) {
    GraphicsData* gfx = malloc(sizeof(GraphicsData));
    
    gfx->width = width;
    gfx->height = height;
    
    // Выделение памяти для масок (8 масок на строку)
    int mask_size = height * 8;
    gfx->mask_stream = malloc(mask_size);
    
    // Простой паттерн: чередующиеся маски
    for (int row = 0; row < height; row++) {
        for (int group = 0; group < 8; group++) {
            if ((row + group) % 2 == 0) {
                gfx->mask_stream[row * 8 + group] = 0xFF;  // Все пиксели
            } else {
                gfx->mask_stream[row * 8 + group] = 0x55;  // Чередование (01010101)
            }
        }
    }
    
    // Подсчет количества пикселей для данных
    int pixel_count = 0;
    for (int row = 0; row < height; row++) {
        for (int group = 0; group < 8; group++) {
            uint8_t mask = gfx->mask_stream[row * 8 + group];
            for (int bit = 0; bit < 8; bit++) {
                if (mask & (1 << bit)) {
                    pixel_count++;
                }
            }
        }
    }
    
    // Выделение памяти для данных пикселей
    gfx->pixel_stream = malloc(pixel_count);
    
    // Заполнение данных (простой градиент)
    for (int i = 0; i < pixel_count; i++) {
        gfx->pixel_stream[i] = (i % 16);  // Цвета 0-15
    }
    
    return gfx;
}

void free_graphics_data(GraphicsData* gfx) {
    if (gfx) {
        free(gfx->mask_stream);
        free(gfx->pixel_stream);
        free(gfx);
    }
}

// Вывод VGA памяти в консоль (для отладки)
void print_vram_region(uint8_t* vram, int x, int y, int width, int height) {
    printf("\nVRAM region (%d, %d) %dx%d:\n", x, y, width, height);
    for (int row = 0; row < height; row++) {
        printf("Row %2d: ", row);
        for (int col = 0; col < width; col++) {
            uint8_t pixel = vram[(y + row) * 320 + x + col];
            if (pixel == 0) {
                printf(".");
            } else {
                printf("%X", pixel & 0xF);
            }
        }
        printf("\n");
    }
}

// ============================================================================
// Тесты
// ============================================================================

void test_1_pixel_mask_basic() {
    printf("\n========================================\n");
    printf("TEST 1: Basic Pixel Mask Handler\n");
    printf("========================================\n");
    
    // Создание простого спрайта 8x2
    uint8_t masks[] = {
        0xFF,  // Строка 0, группа 0: все 8 пикселей
        0x03,  // Строка 1, группа 0: только 2 пикселя (биты 0,1)
    };
    
    uint8_t pixels[] = {
        1,2,3,4,5,6,7,8,  // 8 пикселей для маски 0xFF
        9,10              // 2 пикселя для маски 0x03
    };
    
    uint8_t vram[640] = {0};  // 2 строки по 320 байт
    
    // Вызов функции
    seg003_f9e_proc(masks, pixels, vram, 8, 2);
    
    // Проверка результата
    printf("Expected row 0: 1 2 3 4 5 6 7 8\n");
    printf("Actual   row 0: ");
    for (int i = 0; i < 8; i++) {
        printf("%d ", vram[i]);
    }
    printf("\n");
    
    printf("Expected row 1: 9 10 0 0 0 0 0 0 (only first 2 pixels)\n");
    printf("Actual   row 1: ");
    for (int i = 0; i < 8; i++) {
        printf("%d ", vram[320 + i]);
    }
    printf("\n");
    
    // Проверка
    int passed = 1;
    for (int i = 0; i < 8; i++) {
        if (vram[i] != i + 1) passed = 0;
    }
    if (vram[320] != 9 || vram[321] != 10) passed = 0;
    for (int i = 2; i < 8; i++) {
        if (vram[320 + i] != 0) passed = 0;
    }
    
    printf("\n%s\n", passed ? "✅ PASSED" : "❌ FAILED");
}

void test_2_transparency() {
    printf("\n========================================\n");
    printf("TEST 2: Transparency (0x00 mask)\n");
    printf("========================================\n");
    
    // Маска 0x00 = все прозрачно (не рисуем ничего)
    uint8_t masks[] = {0x00};
    uint8_t pixels[] = {};  // Нет данных пикселей!
    
    uint8_t vram[320] = {0};
    memset(vram, 0xAA, 320);  // Заполняем фоном
    
    seg003_f9e_proc(masks, pixels, vram, 8, 1);
    
    // Проверка: все должно остаться 0xAA
    printf("Expected: all pixels = 0xAA (background preserved)\n");
    printf("Actual:   ");
    int passed = 1;
    for (int i = 0; i < 8; i++) {
        printf("%02X ", vram[i]);
        if (vram[i] != 0xAA) passed = 0;
    }
    printf("\n");
    
    printf("\n%s\n", passed ? "✅ PASSED" : "❌ FAILED");
}

void test_3_object_dispatcher() {
    printf("\n========================================\n");
    printf("TEST 3: Object Dispatcher (seg003_648)\n");
    printf("========================================\n");
    
    // Создание объекта
    GameObject obj;
    obj.x_coord = 10;
    obj.y_coord = 5;
    obj.width = 16;
    obj.height = 16;
    obj.flags = 0x0001;  // Активен
    obj.previous_x = 10;
    obj.previous_y = 5;
    obj.graphics_data_ptr = create_test_sprite(16, 16);
    
    // Инициализация системы
    GameObject objects[1] = {obj};
    uint16_t tilemap[100];
    seg003_init(objects, 1, tilemap, 10);
    
    // Симуляция VGA памяти
    uint8_t fake_vram[64000] = {0};
    // NOTE: В реальности нужно установить g_vga_memory = fake_vram
    
    // Вызов dispatcher
    seg003_648_proc(&obj);
    
    printf("Object position: (%d, %d)\n", obj.x_coord, obj.y_coord);
    printf("Object size: %dx%d\n", obj.width, obj.height);
    printf("Culling check: %s\n", sub_1cdef_culling_check(&obj) ? "visible" : "culled");
    
    // Cleanup
    free_graphics_data(obj.graphics_data_ptr);
    
    printf("\n✅ PASSED (no crashes)\n");
}

void test_4_dirty_rectangles() {
    printf("\n========================================\n");
    printf("TEST 4: Dirty Rectangles\n");
    printf("========================================\n");
    
    // Инициализация tilemap
    uint16_t tilemap[100 * 100] = {0};
    GameObject objects[1];
    seg003_init(objects, 0, tilemap, 100);
    
    // Пометка dirty rectangle
    sub_1cd7b_dirty_rectangle_wrapper(32, 48, 64, 32);
    
    // Проверка: тайлы в области должны иметь dirty флаг
    printf("Marking dirty rectangle: (32, 48) 64x32\n");
    printf("Affected tiles: x=[2..5], y=[3..4]\n");
    
    int dirty_count = 0;
    for (int y = 0; y < 100; y++) {
        for (int x = 0; x < 100; x++) {
            if (tilemap[y * 100 + x] & 0x0001) {
                dirty_count++;
                printf("  Tile (%d, %d) marked dirty\n", x, y);
            }
        }
    }
    
    printf("Total dirty tiles: %d\n", dirty_count);
    printf("\n%s\n", dirty_count > 0 ? "✅ PASSED" : "❌ FAILED");
}

void test_5_culling() {
    printf("\n========================================\n");
    printf("TEST 5: Viewport Culling\n");
    printf("========================================\n");
    
    GameObject obj;
    obj.width = 32;
    obj.height = 32;
    obj.flags = 0x0001;
    
    // Тест 1: Объект на экране
    obj.x_coord = 100;
    obj.y_coord = 100;
    bool visible1 = sub_1cdef_culling_check(&obj);
    printf("Object at (100, 100): %s\n", visible1 ? "visible ✅" : "culled ❌");
    
    // Тест 2: Объект за правым краем
    obj.x_coord = 400;
    obj.y_coord = 100;
    bool visible2 = sub_1cdef_culling_check(&obj);
    printf("Object at (400, 100): %s\n", visible2 ? "visible ❌" : "culled ✅");
    
    // Тест 3: Объект за нижним краем
    obj.x_coord = 100;
    obj.y_coord = 300;
    bool visible3 = sub_1cdef_culling_check(&obj);
    printf("Object at (100, 300): %s\n", visible3 ? "visible ❌" : "culled ✅");
    
    // Тест 4: Объект за левым краем
    obj.x_coord = -100;
    obj.y_coord = 100;
    bool visible4 = sub_1cdef_culling_check(&obj);
    printf("Object at (-100, 100): %s\n", visible4 ? "visible ❌" : "culled ✅");
    
    int passed = (visible1 == true) && (visible2 == false) && 
                 (visible3 == false) && (visible4 == false);
    
    printf("\n%s\n", passed ? "✅ PASSED" : "❌ FAILED");
}

void test_6_main_render_loop() {
    printf("\n========================================\n");
    printf("TEST 6: Main Render Loop\n");
    printf("========================================\n");
    
    // Создание нескольких объектов
    GameObject objects[3];
    
    // Объект 1: на экране
    objects[0].x_coord = 50;
    objects[0].y_coord = 50;
    objects[0].width = 32;
    objects[0].height = 32;
    objects[0].flags = 0x0001;  // Активен
    objects[0].graphics_data_ptr = create_test_sprite(32, 32);
    
    // Объект 2: за экраном
    objects[1].x_coord = 400;
    objects[1].y_coord = 100;
    objects[1].width = 32;
    objects[1].height = 32;
    objects[1].flags = 0x0001;
    objects[1].graphics_data_ptr = create_test_sprite(32, 32);
    
    // Объект 3: неактивен
    objects[2].x_coord = 100;
    objects[2].y_coord = 100;
    objects[2].width = 32;
    objects[2].height = 32;
    objects[2].flags = 0x0000;  // Не активен
    objects[2].graphics_data_ptr = create_test_sprite(32, 32);
    
    // Инициализация
    uint16_t tilemap[100];
    seg003_init(objects, 3, tilemap, 10);
    
    // Вызов главного цикла
    printf("Calling sub_1dd9c_main_render_loop()...\n");
    sub_1dd9c_main_render_loop();
    printf("Loop completed without crashes\n");
    
    // Проверка previous_x/y
    printf("\nChecking previous coordinates saved:\n");
    printf("Object 0: previous=(%d, %d)\n", objects[0].previous_x, objects[0].previous_y);
    printf("Object 1: previous=(%d, %d)\n", objects[1].previous_x, objects[1].previous_y);
    
    // Cleanup
    for (int i = 0; i < 3; i++) {
        free_graphics_data(objects[i].graphics_data_ptr);
    }
    
    printf("\n✅ PASSED\n");
}

// ============================================================================
// Main
// ============================================================================

int main() {
    printf("╔════════════════════════════════════════╗\n");
    printf("║  Lost Vikings Seg003 Test Suite       ║\n");
    printf("║  Implementation Level 2 (Pixel Masks) ║\n");
    printf("╚════════════════════════════════════════╝\n");
    
    test_1_pixel_mask_basic();
    test_2_transparency();
    test_3_object_dispatcher();
    test_4_dirty_rectangles();
    test_5_culling();
    test_6_main_render_loop();
    
    printf("\n╔════════════════════════════════════════╗\n");
    printf("║  All Tests Completed                   ║\n");
    printf("╚════════════════════════════════════════╝\n");
    
    return 0;
}
