// UX stage 3: the options menu (every mode) and the debug tools (--debug only:
// state save/load slots, rewind, level jump). Render-thread side = input +
// overlay drawing; game-thread side = v2_ui_service in v2_vm.cpp, driven by the
// atomics below. Nothing here touches the DS except through the game thread.
#pragma once
#include <cstdint>
#include <atomic>
#include <SDL2/SDL.h>

struct V2Options {
    std::atomic<bool> parallax{true};   // the SNES parallax layer (mod levels)
    std::atomic<bool> scenes{true};     // play the Genesis interludes (slots 53-57)
    std::atomic<bool> snes_balance{false};   // the SNES 1993 level variants (13 levels), takes effect at the next level load
    std::atomic<int> language{0};            // UX6: index into the language banks (0 = the English original)
    std::atomic<bool> smooth{true};          // UX9: the presenter interpolates camera/sprites between ticks
    std::atomic<bool> console_finale{true};  // UX9: the SNES finale's BG2 dragon layer + crowd on the concert (next load)
    // UX9 step 3 — the presenter's picture (render thread only, no game state):
    std::atomic<int>  filter{0};             // 0 NEAREST, 1 SHARP (integer pre-scale + linear), 2 LINEAR
    std::atomic<bool> integer_scale{false};  // whole multiples of the 320x240 (or 320xH) canvas only
    std::atomic<bool> aspect43{true};        // 4:3 like the DOS monitor (320x200 raster on 320x240) / square pixels
    std::atomic<int>  border{0};             // 0 BLACK, 1 GLOW (the frame blurred and dimmed behind the picture)
    std::atomic<int>  wide{0};               // UX9 step 4: 0 OFF (the 320-px raster), 1 16:10, 2 16:9 — the view width of the next level (v2_wide_view_width)
    std::atomic<bool> snes_sound{false};     // UX10: the SNES DE music/effects (SPC700 emulation), takes effect at the next level load
};
extern V2Options v2_options;
void v2_options_ensure_loaded();        // cwd/v2_options.cfg, once
void v2_options_save();
extern char v2_options_lang_code[8];   // the cfg's language code until the banks are scanned
int v2_locale_count();                 // v2_vm.cpp: 1 + the banks found
const char* v2_locale_code_at(int i);

// render thread
bool v2_ui_handle_event(const SDL_Event* e);            // true = consumed
void v2_ui_draw(uint32_t* rgba, int w, int h, SDL_PixelFormat* fmt);
void v2_ui_toast(const char* text);                     // ~1.5 s message

// game thread <- render thread
extern std::atomic<bool> v2_ui_menu_open;               // game thread waits at the tick boundary
// UX9 step 4: the view width the WIDE option asks for, in game pixels: 320 when
// off; for a 16:10 / 16:9 picture 400 / 426 with the 4:3 pixel (the 320x200
// raster shown as 4:3, pixel aspect 1.2) and 320 / 356 with square pixels.
// Read by sub_113b0 at level init (clamped to the map there).
int v2_wide_view_width();
extern std::atomic<int>  v2_ui_req_save;                // slot or -1
extern std::atomic<int>  v2_ui_req_load;
extern std::atomic<int>  v2_ui_req_level;               // level slot or -1
extern std::atomic<bool> v2_ui_rewind_hold;

// level list for the menu (filled by the game thread)
struct V2UiLevel { int slot; char pw[5]; };
extern V2UiLevel v2_ui_levels[64];
extern std::atomic<int> v2_ui_nlevels;
