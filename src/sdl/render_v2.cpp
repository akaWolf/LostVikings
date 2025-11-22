#include <SDL2/SDL.h>
#include <thread>
#include <atomic>
#include <cassert>
#include <cstdio>
#include "v2_input_recorder.h"
#include <unistd.h>  // _exit

// ============================================================================
// Второе окно для тестирования новой реализации рендера
// API дублирует render.cpp с суффиксом _v2
// ============================================================================

const int SCREEN_SCALE_V2 = 2;
const int SCREEN_WIDTH_V2 = 320;
const int SCREEN_HEIGHT_V2 = 240;
const int RENDER_WIDTH_V2 = 344;
const int RENDER_HEIGHT_V2 = 240;
uint32_t tempDrawBuffer_v2[RENDER_WIDTH_V2*RENDER_HEIGHT_V2];

#include "render_v2.h"

struct myDrawInfoS_v2* myDrawInfo_v2 = nullptr;
SDL_Window* myWindow_v2 = NULL;
SDL_Renderer* myRenderer_v2 = NULL;
SDL_Texture* myTexture_v2 = NULL;
SDL_PixelFormat *myFormat_v2 = NULL;

extern void render_callback_v2(void *);
extern bool need_quit;  // Используем флаг первого окна
uint16_t input_keys_v2 = 0;
bool need_quit_v2 = false;  // Не используется, но оставим для совместимости

unsigned int plane4_to_linear_v2(unsigned int plane, unsigned int offset)
{
  return offset * 4 + plane;
}

uint32_t planar_to_linear_v2(uint32_t x, uint32_t y)
{
  return (y * RENDER_WIDTH_V2 + x);
}

void updateDraw_v2()
{
  static int call_count = 0;
  call_count++;

  // stableBuffer is now linear (y*344+x), no page offset needed.
  uint8_t* buf = myDrawInfo_v2->stableBuffer;

  for (int i = 0; i < RENDER_HEIGHT_V2 * RENDER_WIDTH_V2; i++)
  {
    auto color = buf[i];
    auto sdl_color = myDrawInfo_v2->drawPalette[color];
    tempDrawBuffer_v2[i] = SDL_MapRGBA(myFormat_v2, sdl_color.r, sdl_color.g, sdl_color.b, sdl_color.a);
  }
  
  SDL_UpdateTexture(myTexture_v2, NULL, tempDrawBuffer_v2, RENDER_WIDTH_V2*sizeof(uint32_t));
  SDL_RenderClear(myRenderer_v2);
  SDL_Rect srcRect = {0, 0, SCREEN_WIDTH_V2, SCREEN_HEIGHT_V2};
  SDL_RenderCopy(myRenderer_v2, myTexture_v2, &srcRect, NULL);
  SDL_RenderPresent(myRenderer_v2);
}

std::thread render_thread_v2;

void render_thread_proc_v2(void* _state)
{
  myDrawInfo_v2 = (myDrawInfoS_v2 *)calloc(1, sizeof(myDrawInfoS_v2));
  assert(myDrawInfo_v2);

  // Задержка чтобы первое окно успело инициализироваться
  printf("render_v2: Starting initialization (after 200ms delay)...\n");
  SDL_Delay(200);
  
  // SDL уже инициализирован первым окном, но это безопасно
  if( SDL_Init( SDL_INIT_VIDEO ) < 0 )
  {
    printf( "SDL v2 could not initialize! SDL_Error: %s\n", SDL_GetError() );
  }
  
  printf("render_v2: Creating window...\n");
  
  // Получаем размеры экрана для позиционирования в правый нижний угол
  SDL_DisplayMode display_mode;
  int window_width = SCREEN_WIDTH_V2 * SCREEN_SCALE_V2;
  int window_height = SCREEN_HEIGHT_V2 * SCREEN_SCALE_V2;
  int pos_x, pos_y;
  
  if (SDL_GetCurrentDisplayMode(0, &display_mode) == 0) {
    // Успешно получили размеры экрана
    int screen_width = display_mode.w;
    int screen_height = display_mode.h;
    
    // Вычисляем позицию для правого нижнего угла
    pos_x = screen_width - window_width - 10;   // 10px отступ от края
    pos_y = screen_height - window_height - 50; // 50px отступ снизу (для панели задач)
    
    printf("render_v2: Screen size: %dx%d, positioning at (%d, %d)\n", 
           screen_width, screen_height, pos_x, pos_y);
  } else {
    // Fallback: если не удалось получить размеры экрана
    printf("render_v2: Could not get display mode, using fallback position\n");
    pos_x = 1270;  // Примерная позиция для 1920x1080
    pos_y = 670;
  }
  
  // Создаем второе окно в правом нижнем углу
  myWindow_v2 = SDL_CreateWindow( 
    "Lost Vikings - Test Renderer V2", 
    pos_x, pos_y,
    window_width, window_height,
    SDL_WINDOW_SHOWN 
  );
  
  if( myWindow_v2 == NULL )
  {
    printf( "Window v2 could not be created! SDL_Error: %s\n", SDL_GetError() );
  }
  else
  {
    printf("render_v2: Window created successfully!\n");
    printf("render_v2: Creating renderer...\n");
    myRenderer_v2 = SDL_CreateRenderer(myWindow_v2, -1, SDL_RENDERER_ACCELERATED);
    myTexture_v2 = SDL_CreateTexture(myRenderer_v2, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STREAMING, RENDER_WIDTH_V2, RENDER_HEIGHT_V2);
    myFormat_v2 = SDL_AllocFormat(SDL_PIXELFORMAT_RGBA8888);

    printf("render_v2: Entering main loop...\n");

    int loop_counter = 0;
    while (!need_quit)  // Используем флаг первого окна
    {
#ifdef V2_ONLY
      // V2_ONLY: orig window hidden, no event handler there → handle events here.
      // v2_input_poll_event = drop-in SDL_PollEvent wrapper for record/replay.
      extern uint16_t input_keys, input_keys_v2;
      SDL_Event event;
      while (v2_input_poll_event(&event) > 0) {
          switch (event.type) {
          case SDL_QUIT:
              need_quit = true;
              fflush(stdout);
              _exit(0);
              break;
          case SDL_KEYDOWN:
          case SDL_KEYUP: {
              uint16_t key_val = 0;
              uint16_t spec_off = 0;
              switch (event.key.keysym.sym) {
                case SDLK_LEFT:   key_val = 0x200; break;
                case SDLK_RIGHT:  key_val = 0x100; break;
                case SDLK_UP:     key_val = 0x800; break;
                case SDLK_DOWN:   key_val = 0x400; break;
                case SDLK_SPACE:
                case SDLK_RETURN: key_val = 0x8000; break;
                case SDLK_LCTRL:
                case SDLK_RCTRL:  key_val = 0x20;   spec_off = 0x9189; break;  // CTRL → byte_31669
                case SDLK_TAB:    key_val = 0x2000; break;
                case SDLK_e:      key_val = 0x40; break;
                case SDLK_s:      key_val = 0x80;   spec_off = 0x918B; break;  // mute SFX
                case SDLK_d:      key_val = 0x4000; break;
                case SDLK_f:      key_val = 0x8000; break;
                case SDLK_ESCAPE: key_val = 0x1000; break;
                case SDLK_m:      spec_off = 0x919E; break;  // mute music
                case SDLK_x:      spec_off = 0x9199; break;
                case SDLK_LALT:
                case SDLK_RALT:   spec_off = 0x91A4; break;
                case SDLK_F10:    spec_off = 0x91B0; break;
                case SDLK_DELETE: spec_off = 0x91BF; break;
                case SDLK_q:      spec_off = 0x917C; break;  // sc 0x10 — ALT+Q reset
                case SDLK_r:      spec_off = 0x917F; break;  // sc 0x13
                case SDLK_y:      spec_off = 0x9181; break;  // sc 0x15 yes
                case SDLK_a:      spec_off = 0x918A; break;  // sc 0x1E
                case SDLK_n:      spec_off = 0x919D; break;  // sc 0x31 no
                case SDLK_F4:     spec_off = 0x91AA; break;  // sc 0x3E INT 3 debug
                case SDLK_F5:     spec_off = 0x91AB; break;  // sc 0x3F prev level
                case SDLK_F6:     spec_off = 0x91AC; break;  // sc 0x40 next level
                case SDLK_1:      spec_off = 0x916E; break;  // sc 0x02 viking 1
                case SDLK_2:      spec_off = 0x916F; break;  // sc 0x03 viking 2
                case SDLK_3:      spec_off = 0x9170; break;  // sc 0x04 viking 3
              }
              if (event.type == SDL_KEYDOWN) {
                  input_keys |= key_val; input_keys_v2 |= key_val;
              } else {
                  input_keys &= ~key_val; input_keys_v2 &= ~key_val;
              }
              if (spec_off) {
                  extern std::atomic<uint8_t> sdl_spec_state[256];
                  if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
                      sdl_spec_state[spec_off & 0xFF].store(1, std::memory_order_relaxed);
                  } else if (event.type == SDL_KEYUP && (spec_off == 0x91A4 || spec_off == 0x9189)) {
                      // Modifiers (ALT, CTRL): clear on release.
                      sdl_spec_state[spec_off & 0xFF].store(0, std::memory_order_relaxed);
                  }
              }
              break;
          }
          }
      }
#endif
      // НЕ вызываем SDL_PollEvent (default mode) - события обрабатываются только в первом окне
      // Это избегает конфликтов с обработкой событий

      render_callback_v2(_state);  // snapshot drawBuffer→stableBuffer + sprite replay
      updateDraw_v2();             // читает только stableBuffer
      SDL_Delay(15);

      loop_counter++;
    }
  }
}

void render_init_v2(void* state)
{
  render_thread_v2 = std::thread(render_thread_proc_v2, state);
  render_thread_v2.detach();
}
