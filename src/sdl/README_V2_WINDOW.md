# Второе окно для тестирования рендера (V2)

**Статус**: ✅ Работает  
**Дата**: 2024

---

## Описание

Добавлено второе окно SDL с дублирующимся API (суффикс `_v2`) для тестирования новой реализации рендера из `src/rendering/seg003_implementation.c`.

---

## Архитектура

### Окно 1 (оригинальное)
- **Файл**: `src/sdl/render.cpp`
- **Заголовок**: "FFFF"
- **Функции**: `render_init()`, `render_callback()`, `myDrawInfo`
- **Назначение**: Оригинальный рендер игры

### Окно 2 (тестовое)
- **Файл**: `src/sdl/render_v2.cpp`
- **Заголовок**: "Lost Vikings - Test Renderer V2"
- **Функции**: `render_init_v2()`, `render_callback_v2()`, `myDrawInfo_v2`
- **Назначение**: Тестирование новой реализации seg003

---

## Файлы

```
src/sdl/
├── render.cpp           # Оригинальное окно
├── render_v2.cpp        # Второе окно (копия с _v2)
├── render_v2.h          # Заголовочный файл для v2
└── render_v2_test.cpp   # Callback для тестов (тестовый паттерн)
```

---

## API дублирования

| Оригинал | V2 | Описание |
|----------|----|----|
| `myDrawInfo` | `myDrawInfo_v2` | Структура данных отрисовки |
| `myWindow` | `myWindow_v2` | SDL Window |
| `myRenderer` | `myRenderer_v2` | SDL Renderer |
| `myTexture` | `myTexture_v2` | SDL Texture |
| `input_keys` | `input_keys_v2` | Состояние клавиатуры |
| `need_quit` | `need_quit_v2` | Флаг выхода |
| `render_init()` | `render_init_v2()` | Инициализация |
| `render_callback()` | `render_callback_v2()` | Callback каждого кадра |
| `updateDraw()` | `updateDraw_v2()` | Обновление экрана |

---

## Компиляция

```bash
make
```

Makefile автоматически включает все файлы из `src/sdl/*.cpp`.

---

## Запуск

```bash
./vikings
```

Откроются **два окна**:
1. **"FFFF"** - оригинальный рендер игры
2. **"Lost Vikings - Test Renderer V2"** - тестовый рендер (сейчас показывает тестовый паттерн)

---

## Текущий статус

### ✅ Реализовано

- [x] Второе окно SDL создается
- [x] API полностью дублирует оригинал с суффиксом `_v2`
- [x] Тестовый callback работает (показывает движущиеся полосы)
- [x] Обработка ввода работает
- [x] Окно позиционируется со смещением (чтобы не перекрывать первое)

### ⏳ TODO

- [ ] Интеграция с `seg003_implementation.c`
- [ ] Загрузка спрайтов из DATA.DAT
- [ ] Отрисовка через pixel mask handlers
- [ ] Сравнение с оригинальным рендером

---

## Интеграция с seg003_implementation

### Шаг 1: Подключить заголовочный файл

```cpp
// В render_v2_test.cpp
#include "../rendering/seg003_implementation.h"
```

### Шаг 2: Конвертировать myDrawInfo_v2 в VGA формат

```cpp
void render_callback_v2(void* state)
{
    if (!myDrawInfo_v2) return;
    
    // myDrawInfo_v2->drawBuffer[65536*4] это planar VGA память
    // plane 0: offset * 4 + 0
    // plane 1: offset * 4 + 1
    // plane 2: offset * 4 + 2
    // plane 3: offset * 4 + 3
    
    uint8_t* vga_memory = myDrawInfo_v2->drawBuffer;
    
    // TODO: Вызов seg003_f9e_proc для отрисовки спрайтов
}
```

### Шаг 3: Создать тестовые спрайты

```cpp
// Создать тестовый спрайт
GraphicsData* test_sprite = create_test_sprite(32, 48);

// Отрисовать через seg003_f9e_proc
uint8_t* vga_dest = vga_memory + 50 * 320 + 100; // позиция (100, 50)
seg003_f9e_proc(
    test_sprite->mask_stream,
    test_sprite->pixel_stream,
    vga_dest,
    32,  // width
    48   // height
);
```

---

## Структура данных

### myDrawInfoS_v2

```cpp
struct myDrawInfoS_v2
{
  uint8_t drawBuffer[65536*4];  // VGA planar memory (4 planes)
  SDL_Color drawPalette[256];   // Палитра
  uint32_t myOffset;            // Offset для прокрутки
  uint8_t myPixelOffset;        // Pixel offset
};
```

### VGA Planar Format

```
drawBuffer[offset * 4 + 0] = plane 0 (bit 0 цвета)
drawBuffer[offset * 4 + 1] = plane 1 (bit 1 цвета)
drawBuffer[offset * 4 + 2] = plane 2 (bit 2 цвета)
drawBuffer[offset * 4 + 3] = plane 3 (bit 3 цвета)

offset = y * 320 + x
color = (plane3 << 3) | (plane2 << 2) | (plane1 << 1) | plane0
```

---

## Отладка

### Проверка что окно работает

Запустите игру. Вы должны увидеть:
- Первое окно: игра Lost Vikings
- Второе окно: анимированный градиентный паттерн

### Проверка callback

В консоли должны появляться сообщения каждые 60 кадров:
```
render_v2 frame: 60, keys: 0x0000
render_v2 frame: 120, keys: 0x0000
...
```

### Проверка ввода

Нажмите клавиши в втором окне:
- Стрелки (↑↓←→)
- Space, Enter
- Ctrl, Tab
- E, S, D, F
- Escape

Коды клавиш должны появиться в консоли.

---

## Следующие шаги

### 1. Тест простого спрайта

Создайте простой тестовый спрайт и отрисуйте его через `seg003_f9e_proc`:

```cpp
// render_v2_test.cpp
#include "../rendering/seg003_implementation.h"

void render_callback_v2(void* state)
{
    static GraphicsData* test_sprite = nullptr;
    
    if (!test_sprite) {
        // Создать один раз при первом вызове
        test_sprite = create_simple_test_sprite();
    }
    
    uint8_t* vga_memory = myDrawInfo_v2->drawBuffer;
    uint8_t* vga_dest = vga_memory + 50 * 320 + 100;
    
    seg003_f9e_proc(
        test_sprite->mask_stream,
        test_sprite->pixel_stream,
        vga_dest,
        test_sprite->width,
        test_sprite->height
    );
}
```

### 2. Загрузка из DATA.DAT

Интегрируйте загрузчик DATA.DAT для получения реальных спрайтов игры.

### 3. Анимация

Добавьте переключение кадров анимации для тестирования.

### 4. Сравнение с оригиналом

Сравните вывод двух окон для проверки корректности новой реализации.

---

## Известные проблемы

### SDL_Init вызывается дважды

Обе функции `render_init()` и `render_init_v2()` вызывают `SDL_Init()`. Это безопасно, но выдает предупреждение. Можно игнорировать.

### Закрытие окна

Закрытие любого из окон завершает работу обоих окон.

---

## Пример полной интеграции

```cpp
// render_v2_test.cpp - полная версия
#include <cstdio>
#include <cstdint>
#include "render_v2.h"
#include "../rendering/seg003_implementation.h"

static bool initialized = false;
static GraphicsData* test_sprites[10];
static int frame_counter = 0;

void render_callback_v2(void* state)
{
    if (!myDrawInfo_v2) return;
    
    frame_counter++;
    
    // Инициализация один раз
    if (!initialized) {
        // Создать тестовые спрайты
        for (int i = 0; i < 10; i++) {
            test_sprites[i] = create_test_sprite(32, 32);
        }
        
        // Установить палитру
        for (int i = 0; i < 16; i++) {
            myDrawInfo_v2->drawPalette[i].r = i * 16;
            myDrawInfo_v2->drawPalette[i].g = i * 16;
            myDrawInfo_v2->drawPalette[i].b = i * 16;
            myDrawInfo_v2->drawPalette[i].a = 255;
        }
        
        initialized = true;
        printf("render_v2 initialized\n");
    }
    
    // Очистка буфера
    memset(myDrawInfo_v2->drawBuffer, 0, 320 * 200 * 4);
    
    // Отрисовка тестовых спрайтов
    uint8_t* vga_memory = myDrawInfo_v2->drawBuffer;
    
    for (int i = 0; i < 10; i++) {
        int x = 50 + i * 40;
        int y = 50 + (frame_counter / 10 + i * 20) % 150;
        
        uint8_t* vga_dest = vga_memory + y * 320 + x;
        
        seg003_f9e_proc(
            test_sprites[i]->mask_stream,
            test_sprites[i]->pixel_stream,
            vga_dest,
            test_sprites[i]->width,
            test_sprites[i]->height
        );
    }
    
    // Debug
    if (frame_counter % 60 == 0) {
        printf("render_v2 frame: %d, sprites drawn: 10\n", frame_counter);
    }
}
```

---

**Готово к интеграции!** ✅

Следующий шаг: Добавьте `#include "../rendering/seg003_implementation.h"` в `render_v2_test.cpp` и начните тестирование pixel mask handlers.
