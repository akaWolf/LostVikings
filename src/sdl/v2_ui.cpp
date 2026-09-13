// UX stage 3: options menu + debug tools overlay. See v2_ui.h.
#include "v2_ui.h"
#include "v2_net.h"        // UX stage 8 tails: the lobby items and the status line
#include <cstdio>
#include <cstring>
#include <string>
#include <cstdlib>

V2Options v2_options;
std::atomic<bool> v2_ui_menu_open{false};
std::atomic<int>  v2_ui_req_save{-1};
std::atomic<int>  v2_ui_req_load{-1};
std::atomic<int>  v2_ui_req_level{-1};
std::atomic<int>  v2_ui_req_net_host{-1};
std::atomic<bool> v2_ui_req_net_join{false};
char              v2_ui_net_addr[64] = "127.0.0.1:7420";
std::atomic<int>  v2_ui_net_players{3};
std::atomic<int>  v2_ui_net_delay{2};
std::atomic<bool> v2_ui_rewind_hold{false};
V2UiLevel v2_ui_levels[64];
std::atomic<int> v2_ui_nlevels{0};
extern bool g_debug_mode;

// ---------------------------------------------------------------- options --
static const char* OPT_PATH = "v2_options.cfg";
char v2_options_lang_code[8] = "en";
char v2_options_sc55_roms[256] = "";
char v2_options_mt32_roms[256] = "";
void v2_options_ensure_loaded() {
    static std::atomic<bool> done{false};
    if (done.exchange(true)) return;
    FILE* f = fopen(OPT_PATH, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        char key[64]; int val = 0; char sval[16];
        if (sscanf(line, " language = %15[A-Za-z-]", sval) == 1) { strncpy(v2_options_lang_code, sval, 7); v2_options_lang_code[7] = 0; continue; }
        { char pv[256]; if (sscanf(line, " sc55_roms = %255s", pv) == 1) { snprintf(v2_options_sc55_roms, sizeof v2_options_sc55_roms, "%s", pv); continue; } }
        { char pv[256]; if (sscanf(line, " mt32_roms = %255s", pv) == 1) { snprintf(v2_options_mt32_roms, sizeof v2_options_mt32_roms, "%s", pv); continue; } }
        if (sscanf(line, " %63[a-z_] = %d", key, &val) == 2) {
            if (!strcmp(key, "parallax")) v2_options.parallax = val != 0;
            else if (!strcmp(key, "scenes")) v2_options.scenes = val != 0;
            else if (!strcmp(key, "snes_balance")) v2_options.snes_balance = val != 0;
            else if (!strcmp(key, "smooth")) v2_options.smooth = (val >= 0 && val <= 2) ? val : 1;   // 0 NONE, 1 AUTO (an old cfg's ON = the new default), 2 ON
            else if (!strcmp(key, "console_finale")) v2_options.console_finale = val != 0;
            else if (!strcmp(key, "filter")) v2_options.filter = (val >= 0 && val <= 5) ? val : 1;   // out of range: the default, SHARP
            else if (!strcmp(key, "integer_scale")) v2_options.integer_scale = val != 0;
            // (square_pixels — the step-3 ASPECT toggle — is gone, 2026-09-06: the
            // canvas is always the 320x240 raster; an old cfg's key is ignored here)
            else if (!strcmp(key, "border")) v2_options.border = (val >= 0 && val <= 1) ? val : 0;
            else if (!strcmp(key, "snes_sound")) { if (val != 0) v2_options.sound_mode = 1; }   // the pre-UX11 key
            else if (!strcmp(key, "sound")) v2_options.sound_mode = (val >= 0 && val <= 3) ? val : 0;
            else if (!strcmp(key, "wide")) v2_options.wide = (val >= 0 && val <= 2) ? val : 0;
        }
    }
    fclose(f);
}
// UX9 step 4: the WIDE option's view width. The DOS raster is 320x240 Mode X —
// 4:3 with SQUARE pixels — so a picture of R:1 keeping the 240-row height and
// square pixels is W = 240 * R columns: 16:10 -> 384, 16:9 -> 426.
int v2_wide_view_width() {
    switch (v2_options.wide.load()) {
        case 1: return 384;   // 16:10 = 240 * 16/10
        case 2: return 426;   // 16:9  = 240 * 16/9
        default: return 320;
    }
}
void v2_options_save() {
    FILE* f = fopen(OPT_PATH, "w");
    if (!f) return;
    fprintf(f, "parallax=%d\nscenes=%d\nsnes_balance=%d\nlanguage=%s\nsmooth=%d\nconsole_finale=%d\n", (int)v2_options.parallax.load(), (int)v2_options.scenes.load(), (int)v2_options.snes_balance.load(), v2_locale_code_at(v2_options.language.load()), (int)v2_options.smooth.load(), (int)v2_options.console_finale.load());
    fprintf(f, "filter=%d\ninteger_scale=%d\nborder=%d\nsound=%d\nwide=%d\n", v2_options.filter.load(), (int)v2_options.integer_scale.load(), v2_options.border.load(), v2_options.sound_mode.load(), v2_options.wide.load());
    if (v2_options_sc55_roms[0]) fprintf(f, "sc55_roms=%s\n", v2_options_sc55_roms);
    if (v2_options_mt32_roms[0]) fprintf(f, "mt32_roms=%s\n", v2_options_mt32_roms);
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
enum Item { IT_PARALLAX, IT_SCENES, IT_BALANCE, IT_LANG, IT_SMOOTH, IT_FINALE, IT_FILTER, IT_INTEGER, IT_BORDER, IT_WIDE, IT_SOUND,
            IT_NET_PLAYERS, IT_NET_DELAY, IT_NET_HOST, IT_NET_JOIN,      // UX stage 8 tails: the co-op lobby
            IT_LEVEL, IT_SAVE, IT_LOAD, IT_COUNT };
// cfg codes: 0 NEAREST, 1 SHARP, 2 LINEAR (stage 9), 3 XBRZ, 4 HQX, 5 NONE (stage 11 tail); the menu walks NONE..HQX
static const char* const FILTER_NAMES[6] = { "NEAREST", "SHARP  ", "LINEAR ", "XBRZ   ", "HQX    ", "NONE   " };
static const int FILTER_ORDER[6] = { 5, 0, 1, 2, 3, 4 };
static int filter_step(int code, int d) {
    int i = 0; for (int k = 0; k < 6; k++) if (FILTER_ORDER[k] == code) i = k;
    return FILTER_ORDER[(i + d + 6) % 6];
}
static const char* const BORDER_NAMES[2] = { "BLACK", "GLOW " };
static int cursor = 0, sel_slot = 1, sel_level_idx = 0, net_port = 7420;
static bool editing_addr = false;           // the JOIN line takes the address from the keyboard
static std::string toast_text; static uint32_t toast_until = 0;
void v2_ui_toast(const char* text) { toast_text = text; toast_until = SDL_GetTicks() + 1500; }

static int n_items() { return g_debug_mode ? IT_COUNT : IT_LEVEL; }
// UX stage 8 tails: in a network game the options that shape the simulation
// are the host's for everybody (they came with HELLO); changing one here would
// split the worlds at the next level
static bool net_locked() {
    if (!v2_net_active() || v2_net_peer_count() == 0) return false;
    v2_ui_toast("NOT IN A NETWORK GAME");
    return true;
}
static void activate() {
    switch (cursor) {
    case IT_PARALLAX: v2_options.parallax = !v2_options.parallax.load(); v2_options_save(); break;
    case IT_SCENES:   if (net_locked()) break; v2_options.scenes = !v2_options.scenes.load(); v2_options_save(); break;
    case IT_BALANCE:  if (net_locked()) break; v2_options.snes_balance = !v2_options.snes_balance.load(); v2_options_save(); v2_ui_toast("SNES BALANCE: NEXT LEVEL"); break;
    case IT_LANG:     { if (net_locked()) break; int n = v2_locale_count(); if (n > 1) { v2_options.language = (v2_options.language.load() + 1) % n; v2_options_save(); } break; }
    case IT_SMOOTH:   v2_options.smooth = (v2_options.smooth.load() + 1) % 3; v2_options_save(); break;
    case IT_FINALE:   if (net_locked()) break; v2_options.console_finale = !v2_options.console_finale.load(); v2_options_save(); v2_ui_toast("FINALE: NEXT LOAD"); break;
    case IT_FILTER:   v2_options.filter = filter_step(v2_options.filter.load(), 1); v2_options_save(); break;
    case IT_INTEGER:  v2_options.integer_scale = !v2_options.integer_scale.load(); v2_options_save(); break;
    case IT_BORDER:   v2_options.border = (v2_options.border.load() + 1) % 2; v2_options_save(); break;
    case IT_WIDE:     if (net_locked()) break; v2_options.wide = (v2_options.wide.load() + 1) % 3; v2_options_save(); v2_ui_toast("WIDE: NEXT LEVEL"); break;
    case IT_SOUND:    v2_options.sound_mode = (v2_options.sound_mode.load() + 1) % 4; v2_options_save(); { const int m = v2_options.sound_mode.load(); v2_ui_toast(m == 1 ? "SOUND: SNES (NEXT LEVEL)" : m == 2 ? "SOUND: SC-55" : m == 3 ? "SOUND: MT-32" : "SOUND: PC"); } break;
    case IT_NET_PLAYERS: v2_ui_net_players = (v2_ui_net_players.load() == 3) ? 2 : 3; break;
    case IT_NET_DELAY:   v2_ui_net_delay = v2_ui_net_delay.load() % 8 + 1; break;
    case IT_NET_HOST:    if (v2_net_active()) { v2_ui_toast("ALREADY IN A NETWORK GAME"); break; }
                         v2_ui_req_net_host = net_port; v2_ui_menu_open = false; break;
    case IT_NET_JOIN:    if (v2_net_active()) { v2_ui_toast("ALREADY IN A NETWORK GAME"); break; }
                         editing_addr = true; break;
    case IT_LEVEL:    if (v2_ui_nlevels.load() > 0) { v2_ui_req_level = v2_ui_levels[sel_level_idx].slot; v2_ui_menu_open = false; } break;
    case IT_SAVE:     v2_ui_req_save = sel_slot; v2_ui_menu_open = false; break;
    case IT_LOAD:     v2_ui_req_load = sel_slot; v2_ui_menu_open = false; break;
    }
}
static void adjust(int d) {
    switch (cursor) {
    case IT_PARALLAX: case IT_SCENES: case IT_BALANCE: case IT_FINALE: case IT_INTEGER: activate(); break;
    case IT_SMOOTH:   v2_options.smooth = (v2_options.smooth.load() + d + 3) % 3; v2_options_save(); break;
    case IT_SOUND:    v2_options.sound_mode = (v2_options.sound_mode.load() + d + 4) % 4; v2_options_save(); break;
    case IT_FILTER:   v2_options.filter = filter_step(v2_options.filter.load(), d); v2_options_save(); break;
    case IT_BORDER:   v2_options.border = (v2_options.border.load() + d + 2) % 2; v2_options_save(); break;
    case IT_WIDE:     if (net_locked()) break; v2_options.wide = (v2_options.wide.load() + d + 3) % 3; v2_options_save(); v2_ui_toast("WIDE: NEXT LEVEL"); break;
    case IT_LANG: { if (net_locked()) break; int n = v2_locale_count(); if (n > 1) { v2_options.language = (v2_options.language.load() + d + n) % n; v2_options_save(); } break; }
    case IT_NET_PLAYERS: activate(); break;
    case IT_NET_DELAY:   v2_ui_net_delay = (v2_ui_net_delay.load() - 1 + d + 8) % 8 + 1; break;
    case IT_NET_HOST:    net_port = (net_port + d < 1024) ? 1024 : (net_port + d > 65535) ? 65535 : net_port + d; break;
    case IT_LEVEL: { int n = v2_ui_nlevels.load(); if (n > 0) sel_level_idx = (sel_level_idx + d + n) % n; break; }
    case IT_SAVE: case IT_LOAD: sel_slot = (sel_slot - 1 + d + 9) % 9 + 1; break;
    }
}
// the JOIN line's address editor: letters, digits, '.', ':', '-', BACKSPACE; RETURN connects, ESC cancels
static bool edit_addr_key(SDL_Keycode k) {
    std::string a = v2_ui_net_addr;
    if (k == SDLK_ESCAPE) { editing_addr = false; return true; }
    if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
        editing_addr = false;
        if (!a.empty()) { v2_ui_req_net_join = true; v2_ui_menu_open = false; }
        return true;
    }
    if (k == SDLK_BACKSPACE) { if (!a.empty()) a.pop_back(); }
    else if ((k >= 'a' && k <= 'z') || (k >= '0' && k <= '9') || k == '.' || k == ':' || k == '-') { if (a.size() < 60) a.push_back((char)k); }
    else if (k >= SDLK_KP_1 && k <= SDLK_KP_9) { if (a.size() < 60) a.push_back((char)('1' + (k - SDLK_KP_1))); }
    else if (k == SDLK_KP_0) { if (a.size() < 60) a.push_back('0'); }
    else if (k == SDLK_KP_PERIOD) { if (a.size() < 60) a.push_back('.'); }
    else return true;
    snprintf(v2_ui_net_addr, sizeof v2_ui_net_addr, "%s", a.c_str());
    return true;
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
    if (editing_addr) return edit_addr_key(k);
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
        char lines[24][48]; int n = 0;          // title + up to 18 items (debug mode) + the network status
        snprintf(lines[n++], 40, "OPTIONS  (F1/ESC CLOSE)");
        snprintf(lines[n++], 40, "PARALLAX  [%s]", v2_options.parallax.load() ? "ON " : "OFF");
        snprintf(lines[n++], 40, "SCENES    [%s]", v2_options.scenes.load() ? "ON " : "OFF");
        snprintf(lines[n++], 40, "SNES BAL. [%s]", v2_options.snes_balance.load() ? "ON " : "OFF");
        { char lc[8]; snprintf(lc, sizeof lc, "%s", v2_locale_code_at(v2_options.language.load()));
          for (char* c = lc; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
          snprintf(lines[n++], 40, "LANGUAGE  < %s >", lc); }
        { static const char* const SMOOTH_NAMES[3] = { "NONE", "AUTO", "ON  " };
          extern bool v2_smooth_effective(void); extern double v2_vsync_display_hz(void); extern bool v2_vsync_locked(void);
          const int sm = v2_options.smooth.load() % 3;
          // AUTO shows what it decided for this display; the refresh the game's vsync is locked to follows
          if (v2_vsync_locked()) snprintf(lines[n++], 44, "SMOOTH    < %s >%s %.0f HZ", SMOOTH_NAMES[sm], sm == 1 ? (v2_smooth_effective() ? " ON " : " OFF") : "", v2_vsync_display_hz());
          else snprintf(lines[n++], 44, "SMOOTH    < %s >%s NO VSYNC", SMOOTH_NAMES[sm], sm == 1 ? " OFF" : ""); }
        snprintf(lines[n++], 40, "FINALE    [%s]", v2_options.console_finale.load() ? "SNES" : "PC ");
        snprintf(lines[n++], 40, "FILTER    < %s >", FILTER_NAMES[v2_options.filter.load() % 6]);
        snprintf(lines[n++], 40, "INT.SCALE [%s]", v2_options.integer_scale.load() ? "ON " : "OFF");
        snprintf(lines[n++], 40, "BORDER    < %s >", BORDER_NAMES[v2_options.border.load() % 2]);
        { static const char* const WIDE_NAMES[3] = { "OFF  ", "16:10", "16:9 " };
          snprintf(lines[n++], 40, "WIDE      < %s >", WIDE_NAMES[v2_options.wide.load() % 3]); }
        { static const char* const SOUND_NAMES[4] = { "PC  ", "SNES", "SC55", "MT32" };
          snprintf(lines[n++], 40, "SOUND     < %s >", SOUND_NAMES[v2_options.sound_mode.load() % 4]); }
        // UX stage 8 tails: the co-op lobby
        snprintf(lines[n++], 40, "CO-OP     < %d PLAYERS >", v2_ui_net_players.load());
        snprintf(lines[n++], 40, "DELAY     < %d READS >", v2_ui_net_delay.load());
        snprintf(lines[n++], 40, "HOST GAME < PORT %d >  ENTER", net_port);
        if (editing_addr) snprintf(lines[n++], 40, "JOIN      %s_", v2_ui_net_addr);
        else              snprintf(lines[n++], 40, "JOIN      %s  ENTER", v2_ui_net_addr);
        if (g_debug_mode) {
            int nl = v2_ui_nlevels.load();
            if (nl > 0 && sel_level_idx < nl)
                snprintf(lines[n++], 40, "LEVEL     < %02d %s >  ENTER=GO", v2_ui_levels[sel_level_idx].slot, v2_ui_levels[sel_level_idx].pw);
            else snprintf(lines[n++], 40, "LEVEL     (NOT READY)");
            snprintf(lines[n++], 40, "SAVE SLOT < %d >  ENTER (F2)", sel_slot);
            snprintf(lines[n++], 40, "LOAD SLOT < %d >  ENTER (F3)", sel_slot);
            snprintf(lines[n++], 40, "REWIND: HOLD F8");
        }
        // the network status under the items (not selectable)
        snprintf(lines[n++], 40, "%.39s", v2_net_status());
        int maxlen = 0; for (int i = 0; i < n; i++) maxlen = maxlen > (int)strlen(lines[i]) ? maxlen : (int)strlen(lines[i]);
        const int x0 = 8, y0 = 8, bw = maxlen * 6 + 14, bh = n * 9 + 8;
        darken_box(rgba, w, h, fmt, x0, y0, bw, bh);
        for (int i = 0; i < n; i++) {
            bool cur = (i >= 1 && i - 1 == cursor && i - 1 < n_items());
            if (cur) put_text(rgba, w, h, fmt, x0 + 3, y0 + 4 + i * 9, ">", YELLOW);
            put_text(rgba, w, h, fmt, x0 + 10, y0 + 4 + i * 9, lines[i], (i == 0 || i == n - 1) ? GREY : (cur ? YELLOW : WHITE));
        }
    }
    if (!toast_text.empty() && SDL_GetTicks() < toast_until) {
        int len = (int)toast_text.size();
        darken_box(rgba, w, h, fmt, 8, h - 20, len * 6 + 8, 13);
        put_text(rgba, w, h, fmt, 12, h - 17, toast_text.c_str(), YELLOW);
    }
}
