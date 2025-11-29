// Lost Vikings keymap editor — interactive SDL2 tool to edit
// vikings_keymap.cfg. Reuses src/sdl/v2_keymap.cpp for the file format so
// behavior matches what the game expects.
//
// Build:   make keymap_editor   (produces ./vikings_keymap_editor)
// Run:     ./vikings_keymap_editor [--keymap=path]   (default ./vikings_keymap.cfg)
//
// Controls:
//   - Click a row → "Press a key to bind" mode. Next key replaces binding.
//   - ESC during bind → cancel.
//   - Save / Reload / Reset Defaults / Quit buttons at the bottom.
//   - Ctrl+S save, Ctrl+Q quit.

#include <SDL2/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "../v2_keymap.h"
#include "font_8x8.h"

namespace {

constexpr int WIN_W = 820;
constexpr int WIN_H = 620;
constexpr int CHAR_W = 8;
constexpr int CHAR_H = 8;
constexpr int ROW_H = 14;
constexpr int LIST_X = 16;
constexpr int LIST_Y = 70;
constexpr int LIST_W = WIN_W - LIST_X * 2;
constexpr int COL_ACTION_X = LIST_X + 8;
constexpr int COL_KEY_X    = LIST_X + 140;
constexpr int COL_FLAGS_X  = LIST_X + 320;
constexpr int BTN_H = 32;
constexpr int BTN_Y = WIN_H - 60;

struct EditableRow {
    std::string action;
    SDL_Keycode sdl_key;
    uint16_t key_val;
    uint16_t spec_off;
    bool modified;
};

std::vector<EditableRow> g_rows;
int g_hover_row = -1;
int g_selected_row = -1;  // >=0 = waiting for next SDL_KEYDOWN
std::string g_status_msg = "Click a row to rebind its key.";
bool g_dirty = false;

struct Button {
    int x;
    int w;
    const char* label;
    void (*action)();
};

void action_save();
void action_reload();
void action_reset();
void action_quit();

const char* g_cfg_path = "vikings_keymap.cfg";
bool g_running = true;

Button g_buttons[] = {
    {LIST_X,              160, "[S] Save",       action_save},
    {LIST_X + 170,        120, "Reload",         action_reload},
    {LIST_X + 300,        180, "Reset Defaults", action_reset},
    {WIN_W - LIST_X - 90,  90, "[Q] Quit",       action_quit},
};
constexpr int N_BUTTONS = (int)(sizeof(g_buttons) / sizeof(g_buttons[0]));

// === Helpers ===

void set_color(SDL_Renderer* r, uint32_t rgb, uint8_t a = 255) {
    SDL_SetRenderDrawColor(r, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, a);
}

void draw_char(SDL_Renderer* r, int x, int y, char c) {
    if (c < 0x20 || c > 0x7E) c = '?';
    const uint8_t* glyph = kFont8x8[c - 0x20];
    for (int row = 0; row < 8; row++) {
        uint8_t b = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (b & (0x80 >> col)) {
                SDL_Rect p{ x + col, y + row, 1, 1 };
                SDL_RenderFillRect(r, &p);
            }
        }
    }
}

void draw_text(SDL_Renderer* r, int x, int y, const std::string& s, uint32_t color) {
    set_color(r, color);
    for (size_t i = 0; i < s.size(); i++) {
        draw_char(r, x + (int)i * CHAR_W, y, s[i]);
    }
}

bool in_rect(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

std::string format_flags(uint16_t key_val, uint16_t spec_off) {
    char buf[64];
    if (key_val && spec_off)      snprintf(buf, sizeof(buf), "key=0x%X spec=0x%X", key_val, spec_off);
    else if (key_val)             snprintf(buf, sizeof(buf), "key=0x%X", key_val);
    else if (spec_off)            snprintf(buf, sizeof(buf), "spec=0x%X", spec_off);
    else                          snprintf(buf, sizeof(buf), "-");
    return buf;
}

void pull_from_keymap() {
    g_rows.clear();
    size_t n = 0;
    const KeyMapEntry* entries = v2_keymap_entries(&n);
    for (size_t i = 0; i < n; i++) {
        EditableRow r;
        r.action   = entries[i].action ? entries[i].action : "?";
        r.sdl_key  = entries[i].sdl_key;
        r.key_val  = entries[i].key_val;
        r.spec_off = entries[i].spec_off;
        r.modified = false;
        g_rows.push_back(std::move(r));
    }
    g_dirty = false;
    g_selected_row = -1;
}

// === Button actions ===

void action_save() {
    std::vector<KeyMapEntry> tmp;
    tmp.reserve(g_rows.size());
    for (const auto& r : g_rows) {
        KeyMapEntry e{};
        e.action   = r.action.c_str();  // valid until tmp goes out of scope below
        e.sdl_key  = r.sdl_key;
        e.key_val  = r.key_val;
        e.spec_off = r.spec_off;
        tmp.push_back(e);
    }
    bool ok = v2_keymap_save(g_cfg_path, tmp.data(), tmp.size());
    if (ok) {
        for (auto& r : g_rows) r.modified = false;
        g_dirty = false;
        char msg[256];
        snprintf(msg, sizeof(msg), "Saved %zu entries to %s", tmp.size(), g_cfg_path);
        g_status_msg = msg;
    } else {
        g_status_msg = "SAVE FAILED — check stderr.";
    }
}

void action_reload() {
    v2_keymap_load(g_cfg_path);
    pull_from_keymap();
    char msg[256];
    snprintf(msg, sizeof(msg), "Reloaded from %s (%zu entries).", g_cfg_path, g_rows.size());
    g_status_msg = msg;
}

void action_reset() {
    v2_keymap_load_defaults();
    pull_from_keymap();
    g_dirty = true;
    for (auto& r : g_rows) r.modified = true;
    g_status_msg = "Reset to built-in defaults (UNSAVED).";
}

void action_quit() {
    g_running = false;
}

// === Render frame ===

void render(SDL_Renderer* r, int mx, int my) {
    set_color(r, 0x141820);
    SDL_RenderClear(r);

    draw_text(r, LIST_X, 14, "Lost Vikings - Keymap Editor", 0xFFFFFF);
    char header[256];
    snprintf(header, sizeof(header), "File: %s%s   (%zu entries)",
             g_cfg_path, g_dirty ? "   *UNSAVED*" : "", g_rows.size());
    draw_text(r, LIST_X, 30, header, g_dirty ? 0xFFCC44 : 0xAAAACC);
    draw_text(r, LIST_X, 46,
              "Click row -> press key. ESC cancels. Ctrl+S save, Ctrl+Q quit.",
              0x99AABB);

    // Column headers
    draw_text(r, COL_ACTION_X, LIST_Y - 14, "ACTION", 0xFFCC44);
    draw_text(r, COL_KEY_X,    LIST_Y - 14, "KEY",    0xFFCC44);
    draw_text(r, COL_FLAGS_X,  LIST_Y - 14, "FLAGS",  0xFFCC44);

    // Rows
    for (size_t i = 0; i < g_rows.size(); i++) {
        int y = LIST_Y + (int)i * ROW_H;
        uint32_t bg;
        if ((int)i == g_selected_row) bg = 0x402030;
        else if ((int)i == g_hover_row) bg = 0x2A3040;
        else bg = (i & 1) ? 0x191D26 : 0x14181F;
        set_color(r, bg);
        SDL_Rect rr{ LIST_X, y, LIST_W, ROW_H };
        SDL_RenderFillRect(r, &rr);

        uint32_t fg;
        if ((int)i == g_selected_row) fg = 0xFFFF66;
        else if (g_rows[i].modified)  fg = 0x88FF88;
        else                          fg = 0xDDDDDD;

        draw_text(r, COL_ACTION_X, y + 3, g_rows[i].action, fg);

        std::string key_text = ((int)i == g_selected_row)
            ? "<press key... ESC=cancel>"
            : v2_keymap_sdl_key_token(g_rows[i].sdl_key);
        draw_text(r, COL_KEY_X, y + 3, key_text, fg);

        draw_text(r, COL_FLAGS_X, y + 3,
                  format_flags(g_rows[i].key_val, g_rows[i].spec_off), 0x8899AA);
    }

    // Buttons
    for (int bi = 0; bi < N_BUTTONS; bi++) {
        bool hot = in_rect(mx, my, g_buttons[bi].x, BTN_Y, g_buttons[bi].w, BTN_H);
        set_color(r, hot ? 0x4878B0 : 0x304878);
        SDL_Rect bb{ g_buttons[bi].x, BTN_Y, g_buttons[bi].w, BTN_H };
        SDL_RenderFillRect(r, &bb);
        set_color(r, 0x6088C0);
        SDL_RenderDrawRect(r, &bb);
        // Center label vertically; left-pad slightly.
        int label_len = (int)strlen(g_buttons[bi].label);
        int tx = g_buttons[bi].x + (g_buttons[bi].w - label_len * CHAR_W) / 2;
        int ty = BTN_Y + (BTN_H - CHAR_H) / 2;
        draw_text(r, tx, ty, g_buttons[bi].label, 0xFFFFFF);
    }

    // Status bar
    set_color(r, 0x202632);
    SDL_Rect sb{ 0, WIN_H - 16, WIN_W, 16 };
    SDL_RenderFillRect(r, &sb);
    draw_text(r, LIST_X, WIN_H - 12, g_status_msg, 0xCCCCAA);

    SDL_RenderPresent(r);
}

// === Event handling ===

void handle_mouse_click(int mx, int my) {
    // Buttons first.
    for (int bi = 0; bi < N_BUTTONS; bi++) {
        if (in_rect(mx, my, g_buttons[bi].x, BTN_Y, g_buttons[bi].w, BTN_H)) {
            g_buttons[bi].action();
            return;
        }
    }
    // Then rows.
    for (size_t i = 0; i < g_rows.size(); i++) {
        int y = LIST_Y + (int)i * ROW_H;
        if (in_rect(mx, my, LIST_X, y, LIST_W, ROW_H)) {
            g_selected_row = (int)i;
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "Press key to bind to '%s' (ESC = cancel).",
                     g_rows[i].action.c_str());
            g_status_msg = buf;
            return;
        }
    }
    // Clicked empty area → exit bind mode.
    if (g_selected_row >= 0) {
        g_selected_row = -1;
        g_status_msg = "Bind cancelled.";
    }
}

void handle_key(const SDL_KeyboardEvent& key) {
    SDL_Keycode k = key.keysym.sym;
    Uint16 mod = key.keysym.mod;

    if (g_selected_row >= 0) {
        if (k == SDLK_ESCAPE) {
            g_selected_row = -1;
            g_status_msg = "Bind cancelled.";
            return;
        }
        // Reject pure modifier presses — usually unintended.
        if (k == SDLK_LCTRL || k == SDLK_RCTRL || k == SDLK_LSHIFT ||
            k == SDLK_RSHIFT || k == SDLK_LALT  || k == SDLK_RALT ||
            k == SDLK_LGUI   || k == SDLK_RGUI) {
            // Allow these — game does use LCTRL/RCTRL/LALT/RALT — but only
            // outside of Ctrl+S / Ctrl+Q shortcuts. Since we're in bind mode
            // they're explicit user choice; accept.
        }
        // Verify it's representable in the cfg format.
        const char* tok = v2_keymap_sdl_key_token(k);
        if (strcmp(tok, "?") == 0) {
            char buf[128];
            snprintf(buf, sizeof(buf), "Key not representable in cfg (SDLK=%d). Press another.", (int)k);
            g_status_msg = buf;
            return;
        }
        g_rows[g_selected_row].sdl_key = k;
        g_rows[g_selected_row].modified = true;
        g_dirty = true;
        char buf[128];
        snprintf(buf, sizeof(buf), "Bound '%s' -> %s.",
                 g_rows[g_selected_row].action.c_str(), tok);
        g_status_msg = buf;
        g_selected_row = -1;
        return;
    }

    // Shortcuts (not in bind mode).
    if ((mod & KMOD_CTRL) && k == SDLK_s) { action_save(); return; }
    if ((mod & KMOD_CTRL) && k == SDLK_q) { action_quit(); return; }
}

} // namespace

int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--keymap=", 9) == 0) {
            g_cfg_path = argv[i] + 9;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [--keymap=path]\n", argv[0]);
            printf("Default keymap path: vikings_keymap.cfg\n");
            return 0;
        }
    }

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* win = SDL_CreateWindow(
        "Lost Vikings — Keymap Editor",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WIN_W, WIN_H, SDL_WINDOW_SHOWN);
    if (!win) { fprintf(stderr, "CreateWindow failed: %s\n", SDL_GetError()); return 1; }

    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) { fprintf(stderr, "CreateRenderer failed: %s\n", SDL_GetError()); return 1; }

    v2_keymap_load(g_cfg_path);
    pull_from_keymap();

    while (g_running) {
        int mx = 0, my = 0;
        SDL_GetMouseState(&mx, &my);

        // Hover-row update each frame from mouse position.
        g_hover_row = -1;
        for (size_t i = 0; i < g_rows.size(); i++) {
            int y = LIST_Y + (int)i * ROW_H;
            if (in_rect(mx, my, LIST_X, y, LIST_W, ROW_H)) {
                g_hover_row = (int)i;
                break;
            }
        }

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
              case SDL_QUIT: g_running = false; break;
              case SDL_MOUSEBUTTONDOWN:
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    handle_mouse_click(ev.button.x, ev.button.y);
                }
                break;
              case SDL_KEYDOWN:
                if (!ev.key.repeat) handle_key(ev.key);
                break;
            }
        }

        render(ren, mx, my);
    }

    if (g_dirty) {
        fprintf(stderr, "[keymap_editor] Quit with unsaved changes.\n");
    }

    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
