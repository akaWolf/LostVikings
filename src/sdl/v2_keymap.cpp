#include "v2_keymap.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace {

std::vector<KeyMapEntry> g_map;
// std::deque: references to existing elements remain valid after push_back,
// unlike std::vector. Critical because g_map[i].action is a const char* into
// these strings — vector reallocation would dangle every prior action ptr.
std::deque<std::string> g_strings;

const char* intern(const std::string& s) {
    for (const auto& existing : g_strings) {
        if (existing == s) return existing.c_str();
    }
    g_strings.push_back(s);
    return g_strings.back().c_str();
}

// Built-in defaults: identical to entries in vikings_keymap.cfg. Used when
// the file is missing so the binary always runs out-of-the-box.
const KeyMapEntry kBuiltinDefaults[] = {
    {"LEFT",   SDLK_LEFT,   0x200,  0},
    {"RIGHT",  SDLK_RIGHT,  0x100,  0},
    {"UP",     SDLK_UP,     0x800,  0},
    {"DOWN",   SDLK_DOWN,   0x400,  0},
    {"TAB",    SDLK_TAB,    0x2000, 0},
    {"SPACE",  SDLK_SPACE,  0x8000, 0},
    {"RETURN", SDLK_RETURN, 0x8000, 0},
    {"E",      SDLK_e,      0x40,   0},
    {"S",      SDLK_s,      0x80,   0x918B},
    {"D",      SDLK_d,      0x4000, 0},
    {"F",      SDLK_f,      0x8000, 0},
    {"LCTRL",  SDLK_LCTRL,  0x20,   0x9189},
    {"RCTRL",  SDLK_RCTRL,  0x20,   0x9189},
    {"LALT",   SDLK_LALT,   0,      0x91A4},
    {"RALT",   SDLK_RALT,   0,      0x91A4},
    {"ESC",    SDLK_ESCAPE, 0x1000, 0},
    {"M",      SDLK_m,      0,      0x919E},
    {"X",      SDLK_x,      0,      0x9199},
    {"F10",    SDLK_F10,    0,      0x91B0},
    {"DEL",    SDLK_DELETE, 0,      0x91BF},
    {"Q",      SDLK_q,      0,      0x917C},
    {"R",      SDLK_r,      0,      0x917F},
    {"Y",      SDLK_y,      0,      0x9181},
    {"A",      SDLK_a,      0,      0x918A},
    {"N",      SDLK_n,      0,      0x919D},
    {"F4",     SDLK_F4,     0,      0x91AA},
    {"F5",     SDLK_F5,     0,      0x91AB},
    {"F6",     SDLK_F6,     0,      0x91AC},
    {"1",      SDLK_1,      0,      0x916E},
    {"2",      SDLK_2,      0,      0x916F},
    {"3",      SDLK_3,      0,      0x9170},
    {"F12",    SDLK_F12,    0,      0},
};

void load_defaults() {
    g_map.clear();
    g_strings.clear();
    for (const auto& e : kBuiltinDefaults) {
        KeyMapEntry copy = e;
        copy.action = intern(e.action);  // pin to owned storage
        g_map.push_back(copy);
    }
}

// SDLK name parser. Mirrors SDL_GetKeyFromName (which exists) — but using
// own lookup keeps the .cfg format stable across SDL versions and lets us
// support tokens like "RETURN" vs SDL's "Return".
struct NamedKey { const char* name; SDL_Keycode key; };
const NamedKey kNamedKeys[] = {
    {"LEFT",   SDLK_LEFT},   {"RIGHT", SDLK_RIGHT}, {"UP",   SDLK_UP},   {"DOWN", SDLK_DOWN},
    {"SPACE",  SDLK_SPACE},  {"RETURN",SDLK_RETURN},{"TAB",  SDLK_TAB},  {"ESCAPE",SDLK_ESCAPE},
    {"ESC",    SDLK_ESCAPE}, {"DELETE",SDLK_DELETE},{"DEL",  SDLK_DELETE},
    {"LCTRL",  SDLK_LCTRL},  {"RCTRL", SDLK_RCTRL}, {"LALT", SDLK_LALT}, {"RALT",  SDLK_RALT},
    {"LSHIFT", SDLK_LSHIFT}, {"RSHIFT",SDLK_RSHIFT},{"BACKSPACE",SDLK_BACKSPACE},
    {"F1",     SDLK_F1},     {"F2",    SDLK_F2},   {"F3",   SDLK_F3},   {"F4",   SDLK_F4},
    {"F5",     SDLK_F5},     {"F6",    SDLK_F6},   {"F7",   SDLK_F7},   {"F8",   SDLK_F8},
    {"F9",     SDLK_F9},     {"F10",   SDLK_F10},  {"F11",  SDLK_F11},  {"F12",  SDLK_F12},
};

SDL_Keycode parse_sdl_key(const char* tok) {
    if (!tok || !*tok) return SDLK_UNKNOWN;
    // Named keys (case-insensitive).
    for (const auto& nk : kNamedKeys) {
        if (strcasecmp(nk.name, tok) == 0) return nk.key;
    }
    // Single character (a-z, 0-9).
    if (tok[1] == '\0') {
        char c = tok[0];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            return (SDL_Keycode)c;
        }
    }
    return SDLK_UNKNOWN;
}

bool parse_hex(const char* tok, uint16_t* out) {
    if (!tok) return false;
    char* end = nullptr;
    unsigned long v = strtoul(tok, &end, 0);
    if (end == tok || v > 0xFFFFul) return false;
    *out = (uint16_t)v;
    return true;
}

bool parse_line(const char* line_in, KeyMapEntry* out, std::string* action_str) {
    // Strip trailing CR/comment.
    std::string line(line_in);
    size_t hash = line.find('#');
    if (hash != std::string::npos) line.erase(hash);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
        line.pop_back();
    if (line.empty()) return false;

    // Tokenize on whitespace.
    std::vector<std::string> toks;
    std::string cur;
    for (char c : line) {
        if (c == ' ' || c == '\t') {
            if (!cur.empty()) { toks.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) toks.push_back(cur);

    if (toks.size() < 2) return false;

    *action_str = toks[0];
    out->action  = nullptr;  // caller interns
    out->sdl_key = parse_sdl_key(toks[1].c_str());
    out->key_val = 0;
    out->spec_off = 0;

    if (out->sdl_key == SDLK_UNKNOWN) {
        fprintf(stderr, "v2_keymap: unknown SDL key '%s' for action '%s' — skipped\n",
                toks[1].c_str(), toks[0].c_str());
        return false;
    }

    for (size_t i = 2; i < toks.size(); i++) {
        const std::string& t = toks[i];
        if (t.compare(0, 4, "key=") == 0) {
            uint16_t v = 0;
            if (!parse_hex(t.c_str() + 4, &v)) {
                fprintf(stderr, "v2_keymap: bad key= value '%s'\n", t.c_str());
                return false;
            }
            out->key_val = v;
        } else if (t.compare(0, 5, "spec=") == 0) {
            uint16_t v = 0;
            if (!parse_hex(t.c_str() + 5, &v)) {
                fprintf(stderr, "v2_keymap: bad spec= value '%s'\n", t.c_str());
                return false;
            }
            out->spec_off = v;
        } else {
            fprintf(stderr, "v2_keymap: unknown token '%s'\n", t.c_str());
            return false;
        }
    }
    return true;
}

} // namespace

void v2_keymap_load_defaults(void) {
    load_defaults();
    fprintf(stderr, "v2_keymap: loaded %zu built-in default entries\n", g_map.size());
}

void v2_keymap_load(const char* path) {
    // NULL/empty path → try standard location ./vikings_keymap.cfg, then fall
    // back to built-in defaults. Explicit path → require it to exist.
    const char* effective = (path && *path) ? path : "vikings_keymap.cfg";
    FILE* f = fopen(effective, "r");
    if (!f) {
        load_defaults();
        if (path && *path) {
            fprintf(stderr, "v2_keymap: '%s' not found, using built-in defaults (%zu entries)\n",
                    path, g_map.size());
        } else {
            fprintf(stderr, "v2_keymap: no '%s' present, using built-in defaults (%zu entries)\n",
                    effective, g_map.size());
        }
        return;
    }
    g_map.clear();
    g_strings.clear();
    char line[256];
    int line_no = 0, loaded = 0, skipped = 0;
    while (fgets(line, sizeof(line), f)) {
        line_no++;
        KeyMapEntry e{};
        std::string action;
        if (!parse_line(line, &e, &action)) {
            // Blank/comment line → silent; parse errors already printed inside.
            std::string trimmed(line);
            size_t hash = trimmed.find('#');
            if (hash != std::string::npos) trimmed.erase(hash);
            bool nonblank = false;
            for (char c : trimmed) if (c != ' ' && c != '\t' && c != '\n' && c != '\r') { nonblank = true; break; }
            if (nonblank) skipped++;
            continue;
        }
        e.action = intern(action);
        g_map.push_back(e);
        loaded++;
    }
    fclose(f);
    if (g_map.empty()) {
        load_defaults();
        fprintf(stderr, "v2_keymap: '%s' produced 0 entries, falling back to defaults\n", effective);
        return;
    }
    fprintf(stderr, "v2_keymap: loaded %d entries from '%s' (%d skipped)\n",
            loaded, effective, skipped);
}

bool v2_keymap_lookup_sdl(SDL_Keycode key, uint16_t* out_key_val, uint16_t* out_spec_off) {
    if (g_map.empty()) load_defaults();
    bool found = false;
    uint16_t kv = 0, so = 0;
    // Aggregate across all entries for this physical key — supports the rare
    // case where one key contributes to both key_val and spec_off (CTRL/S do
    // this in defaults, but also lets a single .cfg row be split).
    for (const auto& e : g_map) {
        if (e.sdl_key == key) {
            if (!found) { kv = e.key_val; so = e.spec_off; found = true; }
            else {
                if (e.key_val)  kv = e.key_val;
                if (e.spec_off) so = e.spec_off;
            }
        }
    }
    if (out_key_val)  *out_key_val  = kv;
    if (out_spec_off) *out_spec_off = so;
    return found;
}

const char* v2_keymap_sdl_to_action(SDL_Keycode key) {
    if (g_map.empty()) load_defaults();
    for (const auto& e : g_map) if (e.sdl_key == key) return e.action;
    return nullptr;
}

SDL_Keycode v2_keymap_action_to_sdl(const char* action) {
    if (!action) return SDLK_UNKNOWN;
    if (g_map.empty()) load_defaults();
    for (const auto& e : g_map) if (strcmp(e.action, action) == 0) return e.sdl_key;
    return SDLK_UNKNOWN;
}

const KeyMapEntry* v2_keymap_entries(size_t* out_count) {
    if (g_map.empty()) load_defaults();
    if (out_count) *out_count = g_map.size();
    return g_map.empty() ? nullptr : g_map.data();
}

const char* v2_keymap_sdl_key_token(SDL_Keycode key) {
    // Reverse of parse_sdl_key. Single chars (a-z, 0-9) → return the char as
    // a 2-byte buffer; named keys → return name from kNamedKeys.
    static thread_local char buf[2] = {0, 0};
    if ((key >= SDLK_a && key <= SDLK_z) || (key >= SDLK_0 && key <= SDLK_9)) {
        buf[0] = (char)key;
        buf[1] = 0;
        return buf;
    }
    for (const auto& nk : kNamedKeys) {
        if (nk.key == key) return nk.name;
    }
    return "?";
}

bool v2_keymap_save(const char* path, const KeyMapEntry* entries, size_t count) {
    if (!path || !*path || !entries) return false;
    FILE* f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "v2_keymap_save: cannot open '%s' for writing\n", path);
        return false;
    }
    fprintf(f, "# Lost Vikings — runtime keymap config.\n");
    fprintf(f, "# Generated by vikings_keymap_editor. Hand-editing OK; comments and\n");
    fprintf(f, "# blank lines you add will be lost the next time the editor saves.\n");
    fprintf(f, "#\n");
    fprintf(f, "# Format: <ACTION>  <SDL_KEY>  [key=0xVAL]  [spec=0xOFF]\n");
    fprintf(f, "\n");
    for (size_t i = 0; i < count; i++) {
        const KeyMapEntry& e = entries[i];
        const char* keytok = v2_keymap_sdl_key_token(e.sdl_key);
        // Pad columns for readability (matches hand-written cfg style).
        fprintf(f, "%-10s %-10s", e.action ? e.action : "?", keytok);
        if (e.key_val)  fprintf(f, " key=0x%X",  e.key_val);
        if (e.spec_off) fprintf(f, " spec=0x%X", e.spec_off);
        fprintf(f, "\n");
    }
    fclose(f);
    return true;
}
