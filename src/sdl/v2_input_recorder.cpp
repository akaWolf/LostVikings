// V2_ONLY input record/replay. Drop-in wrapper for SDL_PollEvent.
//
// FRAME-BASED for deterministic replay: events tagged with v2_dbg_pre_vm_iter
// (incremented in v2_phase_frame_begin). Replay injects events when game
// frame counter reaches the recorded frame — independent of system clock /
// FPS → identical game state on every replay → reliable bug reproduction.
//
// File format SDL-INDEPENDENT (action names) — future SDL-less replayers
// can parse same files.

#include "v2_input_recorder.h"
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

// v2 game frame counter, defined in v2_vm.cpp; bumped in v2_phase_frame_begin.
extern int v2_dbg_pre_vm_iter;

namespace {

enum Mode { MODE_DISABLED, MODE_RECORD, MODE_REPLAY };

Mode g_mode = MODE_DISABLED;
FILE* g_record_file = nullptr;

// SDL-independent action codes ↔ SDL_Keycode mapping. ONE table for both
// directions. ANY future input source can replay by mapping action → its event.
struct ActionMap { const char* name; SDL_Keycode sdl_key; };
const ActionMap g_actions[] = {
    {"LEFT",    SDLK_LEFT},   {"RIGHT",  SDLK_RIGHT},  {"UP",     SDLK_UP},
    {"DOWN",    SDLK_DOWN},   {"SPACE",  SDLK_SPACE},  {"RETURN", SDLK_RETURN},
    {"LCTRL",   SDLK_LCTRL},  {"RCTRL",  SDLK_RCTRL},  {"TAB",    SDLK_TAB},
    {"E",       SDLK_e},      {"S",      SDLK_s},      {"D",      SDLK_d},
    {"F",       SDLK_f},      {"ESC",    SDLK_ESCAPE}, {"M",      SDLK_m},
    {"X",       SDLK_x},      {"LALT",   SDLK_LALT},   {"RALT",   SDLK_RALT},
    {"F10",     SDLK_F10},    {"DEL",    SDLK_DELETE}, {"Q",      SDLK_q},
    {"R",       SDLK_r},      {"Y",      SDLK_y},      {"A",      SDLK_a},
    {"N",       SDLK_n},      {"F4",     SDLK_F4},     {"F5",     SDLK_F5},
    {"F6",      SDLK_F6},     {"1",      SDLK_1},      {"2",      SDLK_2},
    {"3",       SDLK_3},      {"F12",    SDLK_F12},
};

const char* sdl_key_to_action(SDL_Keycode k) {
    for (const auto& a : g_actions) if (a.sdl_key == k) return a.name;
    return nullptr;
}
SDL_Keycode action_to_sdl_key(const char* name) {
    for (const auto& a : g_actions) if (strcmp(a.name, name) == 0) return a.sdl_key;
    return SDLK_UNKNOWN;
}

struct ReplayEvent {
    int      frame;  // v2_dbg_pre_vm_iter at record time
    uint8_t  kind;   // 0=KEYDOWN, 1=KEYUP
    SDL_Keycode keycode;
};
std::vector<ReplayEvent> g_replay_queue;
size_t g_replay_pos = 0;
bool g_replay_exhausted_logged = false;

void parse_replay_file(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "v2_input_recorder: replay file '%s' open FAILED — disabled\n", path);
        return;
    }
    char line[128];
    int parsed = 0, skipped = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\0') continue;
        int frame;
        char k[4], action[32];
        if (sscanf(line, "%d %3s %31s", &frame, k, action) != 3) { skipped++; continue; }
        SDL_Keycode kc = action_to_sdl_key(action);
        if (kc == SDLK_UNKNOWN) {
            fprintf(stderr, "v2_input_recorder: unknown action '%s' at f=%d — skipped\n", action, frame);
            skipped++; continue;
        }
        ReplayEvent e{};
        e.frame = frame;
        e.kind = (strcmp(k, "KD") == 0) ? 0 : 1;
        e.keycode = kc;
        g_replay_queue.push_back(e);
        parsed++;
    }
    fclose(f);
    fprintf(stderr, "v2_input_recorder: replay loaded %d events from '%s' (%d skipped)\n",
            parsed, path, skipped);
}

void log_keyboard_event(const SDL_Event* e) {
    if (!g_record_file) return;
    if (e->type == SDL_KEYDOWN && e->key.repeat) return;  // skip typematic
    const char* action = sdl_key_to_action(e->key.keysym.sym);
    if (!action) return;  // unmapped key — don't record (won't affect game)
    int frame = v2_dbg_pre_vm_iter;
    const char* k = (e->type == SDL_KEYDOWN) ? "KD" : "KU";
    fprintf(g_record_file, "%d %s %s\n", frame, k, action);
    fflush(g_record_file);
}

bool dequeue_due_replay(SDL_Event* out) {
    if (g_replay_pos >= g_replay_queue.size()) {
        if (!g_replay_exhausted_logged && !g_replay_queue.empty()) {
            g_replay_exhausted_logged = true;
            fprintf(stderr, "v2_input_recorder: REPLAY queue exhausted at frame=%d\n",
                    v2_dbg_pre_vm_iter);
        }
        return false;
    }
    const ReplayEvent& e = g_replay_queue[g_replay_pos];
    if (e.frame > v2_dbg_pre_vm_iter) return false;  // not yet — wait
    SDL_zerop(out);
    out->type = (e.kind == 0) ? SDL_KEYDOWN : SDL_KEYUP;
    out->key.keysym.sym = e.keycode;
    out->key.keysym.scancode = SDL_GetScancodeFromKey(e.keycode);
    out->key.repeat = 0;
    out->key.state = (e.kind == 0) ? SDL_PRESSED : SDL_RELEASED;
    out->key.timestamp = SDL_GetTicks();
    g_replay_pos++;
    return true;
}

} // namespace

extern "C" void v2_input_recorder_init(const char* record_file, const char* replay_file) {
    bool have_rec = (record_file && record_file[0]);
    bool have_rep = (replay_file && replay_file[0]);
    if (have_rec && have_rep) {
        fprintf(stderr, "v2_input_recorder: --record-input and --replay-input are mutually exclusive — disabled\n");
        return;
    }
    if (have_rec) {
        g_record_file = fopen(record_file, "w");
        if (!g_record_file) {
            fprintf(stderr, "v2_input_recorder: record file '%s' open FAILED — disabled\n", record_file);
            return;
        }
        fprintf(g_record_file, "# Lost Vikings v2 input recording — frame-based, action names SDL-independent\n");
        fprintf(g_record_file, "# Format: <frame> <KD|KU> <ACTION>  (frame = v2_dbg_pre_vm_iter)\n");
        fflush(g_record_file);
        g_mode = MODE_RECORD;
        fprintf(stderr, "v2_input_recorder: RECORD mode → '%s'\n", record_file);
        return;
    }
    if (have_rep) {
        parse_replay_file(replay_file);
        if (!g_replay_queue.empty()) {
            g_mode = MODE_REPLAY;
            fprintf(stderr, "v2_input_recorder: REPLAY mode from '%s' (%zu events)\n",
                    replay_file, g_replay_queue.size());
        }
    }
}

extern "C" int v2_input_poll_event(SDL_Event* e) {
    if (!e) return 0;

    if (g_mode == MODE_REPLAY) {
        // Replay drives keyboard fully — real SDL keyboard ignored.
        if (dequeue_due_replay(e)) return 1;
        while (SDL_PollEvent(e)) {
            if (e->type == SDL_KEYDOWN || e->type == SDL_KEYUP) continue;
            return 1;  // pass through window/quit events
        }
        return 0;
    }

    // Record / disabled: pure SDL pass-through; log keyboard in record mode.
    if (!SDL_PollEvent(e)) return 0;
    if (g_mode == MODE_RECORD) log_keyboard_event(e);
    return 1;
}

extern "C" void v2_input_recorder_shutdown(void) {
    if (g_record_file) {
        fclose(g_record_file);
        g_record_file = nullptr;
    }
    g_mode = MODE_DISABLED;
}
