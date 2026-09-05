// UX stage 3: options menu + debug tools overlay. See v2_ui.h.
#include "v2_ui.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <cstdlib>

V2Options v2_options;
std::atomic<bool> v2_ui_menu_open{false};
std::atomic<int>  v2_ui_req_save{-1};
std::atomic<int>  v2_ui_req_load{-1};
std::atomic<int>  v2_ui_req_level{-1};
std::atomic<bool> v2_ui_rewind_hold{false};
V2UiLevel v2_ui_levels[64];
std::atomic<int> v2_ui_nlevels{0};
extern bool g_debug_mode;

// ---------------------------------------------------------------- options --
static const char* OPT_PATH = "v2_options.cfg";
char v2_options_lang_code[8] = "en";
void v2_options_ensure_loaded() {
    static std::atomic<bool> done{false};
    if (done.exchange(true)) return;
    FILE* f = fopen(OPT_PATH, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        char key[64]; int val = 0; char sval[16];
        if (sscanf(line, " language = %15[A-Za-z-]", sval) == 1) { strncpy(v2_options_lang_code, sval, 7); v2_options_lang_code[7] = 0; continue; }
        if (sscanf(line, " %63[a-z_] = %d", key, &val) == 2) {
            if (!strcmp(key, "parallax")) v2_options.parallax = val != 0;
            else if (!strcmp(key, "scenes")) v2_options.scenes = val != 0;
            else if (!strcmp(key, "snes_balance")) v2_options.snes_balance = val != 0;
            else if (!strcmp(key, "smooth")) v2_options.smooth = val != 0;
        }
    }
    fclose(f);
}
void v2_options_save() {
    FILE* f = fopen(OPT_PATH, "w");
    if (!f) return;
    fprintf(f, "parallax=%d\nscenes=%d\nsnes_balance=%d\nlanguage=%s\nsmooth=%d\n", (int)v2_options.parallax.load(), (int)v2_options.scenes.load(), (int)v2_options.snes_balance.load(), v2_locale_code_at(v2_options.language.load()), (int)v2_options.smooth.load());
    fclose(f);
}

// ------------------------------------------------------------------- font --
// 5x7 glyphs, one string per row, '#' = pixel.
struct Glyph { char ch; const char* rows[7]; };
static const Glyph FONT[] = {
    {'A', {" ### ", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"}},
    {'B', {"#### ", "#   #", "#   #", "#### ", "#   #", "#   #", "#### "}},
    {'C', {" ####", "#    ", "#    ", "#    ", "#    ", "#    ", " ####"}},
    {'D', {"#### ", "#   #", "#   #", "#   #", "#   #", "#   #", "#### "}},
    {'E', {"#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#####"}},
    {'F', {"#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#    "}},
    {'G', {" ####", "#    ", "#    ", "# ###", "#   #", "#   #", " ####"}},
    {'H', {"#   #", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"}},
    {'I', {"#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "#####"}},
    {'J', {"    #", "    #", "    #", "    #", "    #", "#   #", " ### "}},
    {'K', {"#   #", "#  # ", "# #  ", "##   ", "# #  ", "#  # ", "#   #"}},
    {'L', {"#    ", "#    ", "#    ", "#    ", "#    ", "#    ", "#####"}},
    {'M', {"#   #", "## ##", "# # #", "# # #", "#   #", "#   #", "#   #"}},
    {'N', {"#   #", "##  #", "# # #", "#  ##", "#   #", "#   #", "#   #"}},
    {'O', {" ### ", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "}},
    {'P', {"#### ", "#   #", "#   #", "#### ", "#    ", "#    ", "#    "}},
    {'Q', {" ### ", "#   #", "#   #", "#   #", "# # #", "#  # ", " ## #"}},
    {'R', {"#### ", "#   #", "#   #", "#### ", "# #  ", "#  # ", "#   #"}},
    {'S', {" ####", "#    ", "#    ", " ### ", "    #", "    #", "#### "}},
    {'T', {"#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  "}},
    {'U', {"#   #", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "}},
    {'V', {"#   #", "#   #", "#   #", "#   #", "#   #", " # # ", "  #  "}},
    {'W', {"#   #", "#   #", "#   #", "# # #", "# # #", "## ##", "#   #"}},
    {'X', {"#   #", "#   #", " # # ", "  #  ", " # # ", "#   #", "#   #"}},
    {'Y', {"#   #", "#   #", " # # ", "  #  ", "  #  ", "  #  ", "  #  "}},
    {'Z', {"#####", "    #", "   # ", "  #  ", " #   ", "#    ", "#####"}},
    {'0', {" ### ", "#   #", "#  ##", "# # #", "##  #", "#   #", " ### "}},
    {'1', {"  #  ", " ##  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### "}},
    {'2', {" ### ", "#   #", "    #", "   # ", "  #  ", " #   ", "#####"}},
    {'3', {"#####", "   # ", "  #  ", "   # ", "    #", "#   #", " ### "}},
    {'4', {"   # ", "  ## ", " # # ", "#  # ", "#####", "   # ", "   # "}},
    {'5', {"#####", "#    ", "#### ", "    #", "    #", "#   #", " ### "}},
    {'6', {"  ## ", " #   ", "#    ", "#### ", "#   #", "#   #", " ### "}},
    {'7', {"#####", "    #", "   # ", "  #  ", " #   ", " #   ", " #   "}},
    {'8', {" ### ", "#   #", "#   #", " ### ", "#   #", "#   #", " ### "}},
    {'9', {" ### ", "#   #", "#   #", " ####", "    #", "   # ", " ##  "}},
    {' ', {"     ", "     ", "     ", "     ", "     ", "     ", "     "}},
    {':', {"     ", "  #  ", "  #  ", "     ", "  #  ", "  #  ", "     "}},
    {'-', {"     ", "     ", "     ", "#####", "     ", "     ", "     "}},
    {'[', {" ### ", " #   ", " #   ", " #   ", " #   ", " #   ", " ### "}},
    {']', {" ### ", "   # ", "   # ", "   # ", "   # ", "   # ", " ### "}},
    {'<', {"   # ", "  #  ", " #   ", "#    ", " #   ", "  #  ", "   # "}},
    {'>', {" #   ", "  #  ", "   # ", "    #", "   # ", "  #  ", " #   "}},
    {'=', {"     ", "     ", "#####", "     ", "#####", "     ", "     "}},
    {'(', {"   # ", "  #  ", " #   ", " #   ", " #   ", "  #  ", "   # "}},
    {')', {" #   ", "  #  ", "   # ", "   # ", "   # ", "  #  ", " #   "}},
    {'.', {"     ", "     ", "     ", "     ", "     ", " ##  ", " ##  "}},
    {'/', {"    #", "    #", "   # ", "  #  ", " #   ", "#    ", "#    "}},
};
static const Glyph* glyph(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    for (const Glyph& g : FONT) if (g.ch == c) return &g;
    return nullptr;
}
static void put_text(uint32_t* rgba, int w, int h, SDL_PixelFormat* fmt, int x, int y, const char* s, uint32_t color) {
    for (; *s; s++, x += 6) {
        const Glyph* g = glyph(*s);
        if (!g) continue;
        for (int r = 0; r < 7; r++)
            for (int c = 0; c < 5; c++)
                if (g->rows[r][c] == '#') {
                    int px = x + c, py = y + r;
                    if (px >= 0 && px < w && py >= 0 && py < h) rgba[py * w + px] = color;
                }
    }
}
static void darken_box(uint32_t* rgba, int w, int h, SDL_PixelFormat* fmt, int x0, int y0, int bw, int bh) {
    for (int y = y0; y < y0 + bh && y < h; y++)
        for (int x = x0; x < x0 + bw && x < w; x++) {
            if (x < 0 || y < 0) continue;
            Uint8 r, g, b, a;
            SDL_GetRGBA(rgba[y * w + x], fmt, &r, &g, &b, &a);
            rgba[y * w + x] = SDL_MapRGBA(fmt, r / 4, g / 4, b / 4, 255);
        }
}

// ------------------------------------------------------------------- menu --
enum Item { IT_PARALLAX, IT_SCENES, IT_BALANCE, IT_LANG, IT_SMOOTH, IT_LEVEL, IT_SAVE, IT_LOAD, IT_COUNT };
static int cursor = 0, sel_slot = 1, sel_level_idx = 0;
static std::string toast_text; static uint32_t toast_until = 0;
void v2_ui_toast(const char* text) { toast_text = text; toast_until = SDL_GetTicks() + 1500; }

static int n_items() { return g_debug_mode ? IT_COUNT : 5; }
static void activate() {
    switch (cursor) {
    case IT_PARALLAX: v2_options.parallax = !v2_options.parallax.load(); v2_options_save(); break;
    case IT_SCENES:   v2_options.scenes = !v2_options.scenes.load(); v2_options_save(); break;
    case IT_BALANCE:  v2_options.snes_balance = !v2_options.snes_balance.load(); v2_options_save(); v2_ui_toast("SNES BALANCE: NEXT LEVEL"); break;
    case IT_LANG:     { int n = v2_locale_count(); if (n > 1) { v2_options.language = (v2_options.language.load() + 1) % n; v2_options_save(); } break; }
    case IT_SMOOTH:   v2_options.smooth = !v2_options.smooth.load(); v2_options_save(); break;
    case IT_LEVEL:    if (v2_ui_nlevels.load() > 0) { v2_ui_req_level = v2_ui_levels[sel_level_idx].slot; v2_ui_menu_open = false; } break;
    case IT_SAVE:     v2_ui_req_save = sel_slot; v2_ui_menu_open = false; break;
    case IT_LOAD:     v2_ui_req_load = sel_slot; v2_ui_menu_open = false; break;
    }
}
static void adjust(int d) {
    switch (cursor) {
    case IT_PARALLAX: case IT_SCENES: case IT_BALANCE: case IT_SMOOTH: activate(); break;
    case IT_LANG: { int n = v2_locale_count(); if (n > 1) { v2_options.language = (v2_options.language.load() + d + n) % n; v2_options_save(); } break; }
    case IT_LEVEL: { int n = v2_ui_nlevels.load(); if (n > 0) sel_level_idx = (sel_level_idx + d + n) % n; break; }
    case IT_SAVE: case IT_LOAD: sel_slot = (sel_slot - 1 + d + 9) % 9 + 1; break;
    }
}

bool v2_ui_handle_event(const SDL_Event* e) {
    if (e->type != SDL_KEYDOWN && e->type != SDL_KEYUP) return false;
    v2_options_ensure_loaded();
    const SDL_Keycode k = e->key.keysym.sym;
    const bool down = e->type == SDL_KEYDOWN, rep = e->key.repeat != 0;
    if (k == SDLK_F1) {                          // every mode: the options menu
        if (down && !rep) { v2_ui_menu_open = !v2_ui_menu_open.load(); if (cursor >= n_items()) cursor = 0; }
        return true;
    }
    if (g_debug_mode) {                          // debug tools outside the menu
        if (k == SDLK_F8) { v2_ui_rewind_hold = down; return true; }
        if (k == SDLK_F2) { if (down && !rep) v2_ui_req_save = sel_slot; return true; }
        if (k == SDLK_F3) { if (down && !rep) v2_ui_req_load = sel_slot; return true; }
    }
    if (!v2_ui_menu_open.load()) return false;
    if (!down) return true;                      // swallow releases while open
    switch (k) {
    case SDLK_ESCAPE: v2_ui_menu_open = false; break;
    case SDLK_UP:     cursor = (cursor - 1 + n_items()) % n_items(); break;
    case SDLK_DOWN:   cursor = (cursor + 1) % n_items(); break;
    case SDLK_LEFT:   adjust(-1); break;
    case SDLK_RIGHT:  adjust(+1); break;
    case SDLK_RETURN: case SDLK_SPACE: case SDLK_KP_ENTER: activate(); break;
    default: break;
    }
    return true;
}

void v2_ui_draw(uint32_t* rgba, int w, int h, SDL_PixelFormat* fmt) {
    // debug: V2_UI_TEST=<frame> opens the menu by itself after that many
    // presented frames (headless/dummy-video review of the overlay)
    { static int test = -2, frames = 0; frames++;
      if (test == -2) { const char* e = getenv("V2_UI_TEST"); test = (e && *e) ? atoi(e) : -1; }
      if (test > 0 && frames == test) v2_ui_menu_open = true; }
    const uint32_t WHITE = SDL_MapRGBA(fmt, 255, 255, 255, 255);
    const uint32_t YELLOW = SDL_MapRGBA(fmt, 255, 230, 80, 255);
    const uint32_t GREY = SDL_MapRGBA(fmt, 160, 160, 160, 255);
    if (v2_ui_menu_open.load()) {
        char lines[8][40]; int n = 0;
        snprintf(lines[n++], 40, "OPTIONS  (F1/ESC CLOSE)");
        snprintf(lines[n++], 40, "PARALLAX  [%s]", v2_options.parallax.load() ? "ON " : "OFF");
        snprintf(lines[n++], 40, "SCENES    [%s]", v2_options.scenes.load() ? "ON " : "OFF");
        snprintf(lines[n++], 40, "SNES BAL. [%s]", v2_options.snes_balance.load() ? "ON " : "OFF");
        { char lc[8]; snprintf(lc, sizeof lc, "%s", v2_locale_code_at(v2_options.language.load()));
          for (char* c = lc; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
          snprintf(lines[n++], 40, "LANGUAGE  < %s >", lc); }
        snprintf(lines[n++], 40, "SMOOTH    [%s]", v2_options.smooth.load() ? "ON " : "OFF");
        if (g_debug_mode) {
            int nl = v2_ui_nlevels.load();
            if (nl > 0 && sel_level_idx < nl)
                snprintf(lines[n++], 40, "LEVEL     < %02d %s >  ENTER=GO", v2_ui_levels[sel_level_idx].slot, v2_ui_levels[sel_level_idx].pw);
            else snprintf(lines[n++], 40, "LEVEL     (NOT READY)");
            snprintf(lines[n++], 40, "SAVE SLOT < %d >  ENTER (F2)", sel_slot);
            snprintf(lines[n++], 40, "LOAD SLOT < %d >  ENTER (F3)", sel_slot);
            snprintf(lines[n++], 40, "REWIND: HOLD F8");
        }
        int maxlen = 0; for (int i = 0; i < n; i++) maxlen = maxlen > (int)strlen(lines[i]) ? maxlen : (int)strlen(lines[i]);
        const int x0 = 8, y0 = 8, bw = maxlen * 6 + 14, bh = n * 9 + 8;
        darken_box(rgba, w, h, fmt, x0, y0, bw, bh);
        for (int i = 0; i < n; i++) {
            bool cur = (i >= 1 && i - 1 == cursor && i - 1 < n_items());
            if (cur) put_text(rgba, w, h, fmt, x0 + 3, y0 + 4 + i * 9, ">", YELLOW);
            put_text(rgba, w, h, fmt, x0 + 10, y0 + 4 + i * 9, lines[i], i == 0 ? GREY : (cur ? YELLOW : WHITE));
        }
    }
    if (!toast_text.empty() && SDL_GetTicks() < toast_until) {
        int len = (int)toast_text.size();
        darken_box(rgba, w, h, fmt, 8, h - 20, len * 6 + 8, 13);
        put_text(rgba, w, h, fmt, 12, h - 17, toast_text.c_str(), YELLOW);
    }
}
