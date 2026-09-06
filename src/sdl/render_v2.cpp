#include <SDL2/SDL.h>
#include "v2_timing.h"
extern "C" void sdl_int9_note_keydown(int sdl_scancode);  // render.cpp (#62)
#include <thread>
#include <vector>
#include <cstring>
#include <atomic>
#include <cassert>
#include <cstdio>
#include "v2_input_recorder.h"
#include "v2_keymap.h"
#include <unistd.h>  // _exit

// ============================================================================
// Второе окно для тестирования новой реализации рендера
// API дублирует render.cpp с суффиксом _v2
// ============================================================================

const int SCREEN_SCALE_V2 = 2;
bool v2_present_vsync = false;   // UX stage 9: SDL_RenderPresent blocks on the display refresh
const int SCREEN_WIDTH_V2 = 320;
const int SCREEN_HEIGHT_V2 = 240;
const int RENDER_WIDTH_V2 = 344;
const int RENDER_HEIGHT_V2 = 240;
uint32_t tempDrawBuffer_v2[RENDER_WIDTH_V2*RENDER_HEIGHT_V2];

#include "render_v2.h"
#include "v2_ui.h"
extern int v2_dbg_pre_vm_iter;   // game-frame counter (v2_vm.cpp), C++ linkage — declared once at file scope (clang rejects block externs inside extern "C" functions)

struct myDrawInfoS_v2* myDrawInfo_v2 = nullptr;
SDL_Window* myWindow_v2 = NULL;
SDL_Renderer* myRenderer_v2 = NULL;
SDL_Texture* myTexture_v2 = NULL;
SDL_PixelFormat *myFormat_v2 = NULL;

extern void render_callback_v2(void *);
extern int v2_display_fullscreen;   // v2_render_funcs.cpp: 0 = HUD layout, else the map rows shown (200/224)

// UX stage 9, step 3 — the presenter's own layout (no SDL logical size):
//   ASPECT  4:3 = the raster on a 320x240 canvas like the DOS monitor (200 rows
//           stretched by 1.2, the 224-row finale by 15/14), 1:1 = square pixels;
//   INT.SCALE = whole multiples of that canvas only (letterboxed);
//   FILTER  NEAREST = crisp, LINEAR = bilinear on the source, SHARP = nearest
//           pre-scale to the next integer multiple, then linear to the window
//           (crisp pixels, no shimmer at fractional scales);
//   BORDER  BLACK, or GLOW = the frame decimated to 40x30, drawn linear over
//           the whole output at 28 % brightness behind the picture.
// V2_PRESENT_SHOT=<path.ppm>[:<call>] dumps the composed output once.
static SDL_Texture* g_sharp_tex = nullptr; static int g_sharp_k = 0, g_sharp_h = 0;
static SDL_Texture* g_glow_tex = nullptr;
static int g_tex_filter = -1;   // the sampling the source texture was created with (0 nearest, 1 linear)
static SDL_Texture* v2_make_texture(int access, int w, int h, int linear) {
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, linear ? "1" : "0");   // sampled at creation time
    SDL_Texture* tx = SDL_CreateTexture(myRenderer_v2, SDL_PIXELFORMAT_RGBA8888, access, w, h);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    return tx;
}
static void v2_present_frame(int H) {
    const int filter = v2_options.filter.load();
    const bool integer = v2_options.integer_scale.load();
    const bool a43 = v2_options.aspect43.load();
    const int border = v2_options.border.load();
    const int want = (filter == 2) ? 1 : 0;
    if (g_tex_filter != want || !myTexture_v2) {
        if (myTexture_v2) SDL_DestroyTexture(myTexture_v2);
        myTexture_v2 = v2_make_texture(SDL_TEXTUREACCESS_STREAMING, RENDER_WIDTH_V2, RENDER_HEIGHT_V2, want);
        g_tex_filter = want;
    }
    if (!myTexture_v2) return;
    SDL_UpdateTexture(myTexture_v2, NULL, tempDrawBuffer_v2, RENDER_WIDTH_V2 * sizeof(uint32_t));
    int W = 0, Hout = 0;
    SDL_GetRendererOutputSize(myRenderer_v2, &W, &Hout);
    const int cw = SCREEN_WIDTH_V2, ch = a43 ? SCREEN_HEIGHT_V2 : H;
    double s = (W > 0 && Hout > 0) ? ((double)W / cw < (double)Hout / ch ? (double)W / cw : (double)Hout / ch) : 1.0;
    if (integer) { s = (double)(int)s; if (s < 1.0) s = 1.0; }
    const int dw = (int)(cw * s + 0.5), dh = (int)(ch * s + 0.5);
    SDL_Rect dst = { (W - dw) / 2, (Hout - dh) / 2, dw, dh };
    SDL_Rect src = { 0, 0, SCREEN_WIDTH_V2, H };
    SDL_SetRenderDrawColor(myRenderer_v2, 0, 0, 0, 255);
    SDL_RenderClear(myRenderer_v2);
    if (border == 1) {
        if (!g_glow_tex) {
            g_glow_tex = v2_make_texture(SDL_TEXTUREACCESS_TARGET, 40, 30, 1);
            if (g_glow_tex) SDL_SetTextureColorMod(g_glow_tex, 72, 72, 72);
        }
        if (g_glow_tex) {
            SDL_SetRenderTarget(myRenderer_v2, g_glow_tex);
            SDL_RenderCopy(myRenderer_v2, myTexture_v2, &src, NULL);
            SDL_SetRenderTarget(myRenderer_v2, NULL);
            SDL_RenderCopy(myRenderer_v2, g_glow_tex, NULL, NULL);
        }
    }
    bool drawn = false;
    if (filter == 1) {
        int k = (int)s; if (k < s) k++; if (k < 1) k = 1; if (k > 8) k = 8;
        if (!g_sharp_tex || g_sharp_k != k || g_sharp_h != H) {
            if (g_sharp_tex) SDL_DestroyTexture(g_sharp_tex);
            g_sharp_tex = v2_make_texture(SDL_TEXTUREACCESS_TARGET, SCREEN_WIDTH_V2 * k, H * k, 1);
            g_sharp_k = k; g_sharp_h = H;
        }
        if (g_sharp_tex) {
            SDL_SetRenderTarget(myRenderer_v2, g_sharp_tex);
            SDL_RenderCopy(myRenderer_v2, myTexture_v2, &src, NULL);   // nearest, exact integer k
            SDL_SetRenderTarget(myRenderer_v2, NULL);
            SDL_RenderCopy(myRenderer_v2, g_sharp_tex, NULL, &dst);   // linear to the window
            drawn = true;
        }
    }
    if (!drawn) SDL_RenderCopy(myRenderer_v2, myTexture_v2, &src, &dst);
    {
        static int shot = -1, at = 1, calls = 0; static const char* path = nullptr; static char pbuf[512];
        if (shot < 0) {
            const char* e = getenv("V2_PRESENT_SHOT");
            shot = (e && *e) ? 0 : 2;
            if (shot == 0) { snprintf(pbuf, sizeof pbuf, "%s", e); char* c = strrchr(pbuf, ':'); if (c && c[1]) { at = atoi(c + 1); *c = 0; } path = pbuf; }
        }
        calls++;
        if (shot == 0 && calls >= at) {
            std::vector<uint8_t> px((size_t)W * Hout * 3);
            if (SDL_RenderReadPixels(myRenderer_v2, NULL, SDL_PIXELFORMAT_RGB24, px.data(), W * 3) == 0) {
                FILE* f = fopen(path, "wb");
                if (f) { fprintf(f, "P6\n%d %d\n255\n", W, Hout); fwrite(px.data(), 1, px.size(), f); fclose(f); }
                fprintf(stderr, "V2-PRESENT-SHOT: %s %dx%d picture %dx%d at (%d,%d) filter=%d int=%d a43=%d border=%d H=%d\n",
                        path, W, Hout, dst.w, dst.h, dst.x, dst.y, filter, (int)integer, (int)a43, border, H);
            }
            shot = 1;
        }
    }
    SDL_RenderPresent(myRenderer_v2);
}
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

  // Black-screen forensics (2026-08-28 user report: sound+gameplay alive,
  // window black): once a second sum the presented pixel bytes and the
  // palette — tells WHICH stage is dark (pixels vs palette vs blit).
  {
    static int diag = -1;
    if (diag < 0) diag = getenv("V2_PRESENT_DIAG") ? 1 : 0;
    static uint32_t last_ms = 0;
    uint32_t now = SDL_GetTicks();
    if (diag && now - last_ms >= 1000) {
      last_ms = now;
      uint32_t psum = 0, palsum = 0;
      for (int i = 0; i < RENDER_HEIGHT_V2 * RENDER_WIDTH_V2; i += 7) psum += buf[i];
      for (int i = 0; i < 256; i++) {
        auto& c = myDrawInfo_v2->drawPalette[i];
        palsum += c.r + c.g + c.b;
      }
      extern SDL_Color v2_display_palette[256];
      extern bool v2_display_palette_valid;
      extern uint8_t v2_vga[65536 * 4];
      extern uint8_t v2_render_buf[320 * 240];
      extern uint8_t v2_display_buf[];
      extern uint16_t v2_vga_crtc, v2_vga_pan;
      uint32_t vsum = 0, rsum = 0, dsum = 0;
      for (int i = 0; i < 65536 * 4; i += 97) vsum += v2_vga[i];
      for (int i = 0; i < 320 * 240; i += 7) rsum += v2_render_buf[i];
      for (int i = 0; i < 320 * 176; i += 7) dsum += v2_display_buf[i];
      fprintf(stderr, "V2-PRESENT: calls=%d stable_sum=%u pal_sum=%u pubvalid=%d "
              "vga_sum=%u rbuf_sum=%u dbuf_sum=%u crtc=%04X pan=%u\n",
              call_count, psum, palsum, (int)v2_display_palette_valid,
              vsum, rsum, dsum, v2_vga_crtc, v2_vga_pan);
    }
  }

  for (int i = 0; i < RENDER_HEIGHT_V2 * RENDER_WIDTH_V2; i++)
  {
    auto color = buf[i];
    auto sdl_color = myDrawInfo_v2->drawPalette[color];
    tempDrawBuffer_v2[i] = SDL_MapRGBA(myFormat_v2, sdl_color.r, sdl_color.g, sdl_color.b, sdl_color.a);
  }
  
  // UX stage 9 step 3: the picture is the content rows only — 200 (176 + the
  // 24-row HUD band of the 320x200 DOS raster, or a full-screen scene) or 224
  // (LVX_TALL224); the overlay's toast sits above that bottom edge
  const int content_h = v2_display_fullscreen ? v2_display_fullscreen : 200;
  v2_ui_draw(tempDrawBuffer_v2, RENDER_WIDTH_V2, content_h, myFormat_v2);   // UX stage 3 overlay
  // debug: V2_UI_SHOT=<path.ppm> dumps the presented frame once while the
  // options menu is open (the overlay lives only in this 32-bit buffer)
  {
      static int shot = -1; static const char* sp = nullptr;
      if (shot < 0) { sp = getenv("V2_UI_SHOT"); shot = (sp && *sp) ? 0 : 2; }
      if (shot == 0 && v2_ui_menu_open.load()) {
          FILE* f = fopen(sp, "wb");
          if (f) {
              fprintf(f, "P6\n%d %d\n255\n", RENDER_WIDTH_V2, RENDER_HEIGHT_V2);
              for (int i = 0; i < RENDER_WIDTH_V2 * RENDER_HEIGHT_V2; i++) {
                  Uint8 r, g, b, a; SDL_GetRGBA(tempDrawBuffer_v2[i], myFormat_v2, &r, &g, &b, &a);
                  fputc(r, f); fputc(g, f); fputc(b, f);
              }
              fclose(f); shot = 1;
          }
      }
  }
  v2_present_frame(content_h);
}

std::thread render_thread_v2;

void render_thread_proc_v2(void* _state)
{
  // myDrawInfo_v2 is allocated in render_init_v2() on the game thread BEFORE this
  // detached thread starts (#179). It doubles as the "v2 mirror enabled" gate
  // (`if (myDrawInfo_v2)` throughout seg000); calloc'ing it here raced both the
  // game thread's first drawPixel (NULL deref @ f0) AND v2 activation timing
  // (under load v2 could fail to start → false-pass with no verification).
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
  if (const char* e = getenv("V2_WINDOW_SIZE")) {          // UX stage 9: WxH (presenter layout tests)
      int w = 0, h = 0;
      if (sscanf(e, "%dx%d", &w, &h) == 2 && w >= 320 && h >= 200) { window_width = w; window_height = h; }
  }
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
  
  // Nearest-neighbor scaling so pixel art stays crisp at any window size
  // (also the SDL default, but pin it explicitly for portability).
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

  // Создаем второе окно в правом нижнем углу (resizable, fullscreen via F11).
  myWindow_v2 = SDL_CreateWindow(
    "Lost Vikings - Test Renderer V2",
    pos_x, pos_y,
    window_width, window_height,
    SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
  );

  if( myWindow_v2 == NULL )
  {
    printf( "Window v2 could not be created! SDL_Error: %s\n", SDL_GetError() );
  }
  else
  {
    printf("render_v2: Window created successfully!\n");
    printf("render_v2: Creating renderer...\n");
    // UX stage 9: present at the display's refresh (vsync) — the presenter
    // paces itself on SDL_RenderPresent then (v2_present_sleep skips its
    // sleep); the game tick keeps its own pacing (v2_main.cpp) and the
    // vsync-wait loop its own (v2_tick_sleep), so game speed is unchanged.
    // V2_NO_PRESENT_VSYNC=1 restores the free-running 15 ms presenter.
    {
        const char* e = getenv("V2_NO_PRESENT_VSYNC");
        Uint32 rf = SDL_RENDERER_ACCELERATED | ((e && *e == '1') ? 0 : SDL_RENDERER_PRESENTVSYNC);
        myRenderer_v2 = SDL_CreateRenderer(myWindow_v2, -1, rf);
        SDL_RendererInfo ri;
        v2_present_vsync = myRenderer_v2 && SDL_GetRendererInfo(myRenderer_v2, &ri) == 0 &&
                           (ri.flags & SDL_RENDERER_PRESENTVSYNC);
        printf("render_v2: presenter %s\n", v2_present_vsync ? "vsync" : "15 ms sleep");
    }
    // UX stage 9 step 3: no SDL logical size — v2_present_frame lays the
    // picture out itself (aspect / integer scale / filter / border options)
    // and creates the source texture with the sampling the FILTER asks for.
    myTexture_v2 = nullptr;
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
          // UX stage 3: F1 options menu (every mode) + --debug tools; a
          // consumed key never reaches the game input, and an open menu
          // drops the held game keys so nothing sticks under it.
          if (v2_ui_handle_event(&event)) {
              if (v2_ui_menu_open.load()) { input_keys = 0; input_keys_v2 = 0; }
              continue;
          }
          switch (event.type) {
          case SDL_QUIT:
              need_quit = true;
              fflush(stdout);
              _exit(0);
              break;
          case SDL_KEYDOWN:
          case SDL_KEYUP: {
              // F11 = toggle desktop-fullscreen (handled before keymap so it
              // never reaches game-input mapping).
              if (event.type == SDL_KEYDOWN && !event.key.repeat &&
                  event.key.keysym.sym == SDLK_F11) {
                  Uint32 wf = SDL_GetWindowFlags(myWindow_v2);
                  SDL_SetWindowFullscreen(myWindow_v2,
                      (wf & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                  break;
              }
              uint16_t key_val = 0;
              uint16_t spec_off = 0;
              v2_keymap_lookup_sdl(event.key.keysym.sym, &key_val, &spec_off);
              if (event.type == SDL_KEYDOWN) {
                  // #62: the INT9 letter channel ([28C] = LUT[scancode]) was
                  // fed ONLY by the default-window handler — in V2_ONLY the
                  // password screen never received letters/Enter. Same shared
                  // writer as render.cpp (typematic repeats included).
                  sdl_int9_note_keydown(event.key.keysym.scancode);
                  // (#81) tap accumulator: without this V2_ONLY lost any
                  // KEYDOWN+KEYUP shorter than one 12352 interval (the
                  // default-window handler feeds it in render.cpp:794).
                  if (key_val && !event.key.repeat) {
                      extern std::atomic<uint16_t> sdl_input_press_edges;
                      sdl_input_press_edges.fetch_or(key_val, std::memory_order_relaxed);
                  }
                  input_keys |= key_val; input_keys_v2 |= key_val;
              } else {
                  input_keys &= ~key_val; input_keys_v2 &= ~key_val;
              }
              if (spec_off) {
                  extern std::atomic<uint8_t> sdl_spec_state[256];
                  extern std::atomic<uint8_t> sdl_spec_press_latch[256];
                  // EXACT orig INT9 replication: KEYDOWN scancode → byte_316XX=1,
                  // KEYUP (release scancode) → byte_316XX=0, for ALL spec keys.
                  // Matches render.cpp behavior. Without clear on KEYUP, F10/X/Q
                  // etc. stay 1 forever → triggers re-fire every iter (e.g.,
                  // F10 menu reopens immediately after N dismissal).
                  if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
                      sdl_spec_state[spec_off & 0xFF].store(1, std::memory_order_relaxed);
                      sdl_spec_press_latch[spec_off & 0xFF].store(1, std::memory_order_relaxed);
                  } else if (event.type == SDL_KEYUP) {
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
#ifndef HEADLESS
      updateDraw_v2();             // читает только stableBuffer
#else
      // task #36: headless dummy video — pure presenter blit skipped (the
      // stableBuffer snapshot above still runs: it feeds the A2 extraction).
#endif
      // Frame counter in the window title (updated every ~10 frames) — handy
      // when recording replays: the number matches the .inp frame column.
      {
          // v2_dbg_pre_vm_iter: file-scope extern (top of file)
          static int last_shown = -1;
          int f = v2_dbg_pre_vm_iter;
          if (f - last_shown >= 10 || f < last_shown) {
              last_shown = f;
              char t[64];
              snprintf(t, sizeof(t), "Lost Vikings v2 - frame %d", f);
              if (myWindow_v2) SDL_SetWindowTitle(myWindow_v2, t);
          }
      }
      v2_present_sleep();          // stage 6.3: single pacing source (v2_timing.h)

      loop_counter++;
    }
  }
}

void render_init_v2(void* state)
{
  // Allocate myDrawInfo_v2 synchronously on the game thread BEFORE spawning the
  // detached v2 render thread — it gates the entire v2 mirror and is written via
  // drawPixel from frame 0. Async calloc in the thread raced startup under heavy
  // parallel load → NULL deref @ f0 / nondeterministic v2 activation (#179).
  if (!myDrawInfo_v2) {
    myDrawInfo_v2 = (myDrawInfoS_v2 *)calloc(1, sizeof(myDrawInfoS_v2));
    assert(myDrawInfo_v2);
  }
  render_thread_v2 = std::thread(render_thread_proc_v2, state);
  render_thread_v2.detach();
}
