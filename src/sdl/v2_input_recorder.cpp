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
#include "v2_keymap.h"
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <mutex>

// v2 game frame counter, defined in v2_vm.cpp; bumped in v2_phase_frame_begin.
extern int v2_dbg_pre_vm_iter;

namespace {

enum Mode { MODE_DISABLED, MODE_RECORD, MODE_REPLAY };

Mode g_mode = MODE_DISABLED;
FILE* g_record_file = nullptr;
bool g_strict_replay = false;  // true = ignore real keyboard even after queue exhausted

// Action ↔ SDL_Keycode mapping is sourced from v2_keymap (runtime cfg).
// Recorder writes the action NAME — replay can resurrect the same logical
// action even if the user's physical binding changes between runs.
inline const char* sdl_key_to_action(SDL_Keycode k) { return v2_keymap_sdl_to_action(k); }
inline SDL_Keycode action_to_sdl_key(const char* n) { return v2_keymap_action_to_sdl(n); }

struct ReplayEvent {
    int      frame;  // v2_dbg_pre_vm_iter at record time
    uint8_t  kind;   // 0=KEYDOWN, 1=KEYUP
    SDL_Keycode keycode;
};
std::vector<ReplayEvent> g_replay_queue;
size_t g_replay_pos = 0;
bool g_replay_exhausted_logged = false;

// RECORD mode: the render thread captures SDL key edges into this pending queue
// (action + kind only — NO frame tag). The game thread drains it inside
// sub_12352 (v2_input_record_drain) and writes each event tagged with the frame
// the game ACTUALLY reads input on (v2_dbg_pre_vm_iter at that sub_12352 call).
//
// Why not tag on the render thread: the render thread observes v2_dbg_pre_vm_iter
// asynchronously, so a key pressed inside a blocking wait-loop (which polls input
// but doesn't advance the frame counter) could be logged a frame LATE — after the
// loop already got the input and the main loop bumped the counter. That made the
// recorded frame one ahead of where the game reads it, so on replay the
// frame-gated event never became due while the loop sat on the frozen counter →
// deadlock (#180). Tagging at the game-thread read makes record == replay.
struct PendingRec { const char* action; uint8_t kind; }; // kind 0=KD 1=KU
std::vector<PendingRec> g_pending_record;
std::mutex g_pending_mutex;

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
    // Don't write here (render thread): only capture the action + edge into the
    // pending queue. The game thread tags it with the read-frame in
    // v2_input_record_drain (called from sub_12352). See g_pending_record note.
    std::lock_guard<std::mutex> lk(g_pending_mutex);
    g_pending_record.push_back({action, (uint8_t)(e->type == SDL_KEYDOWN ? 0 : 1)});
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
    if (e.frame > v2_dbg_pre_vm_iter)
        return false;  // event gated to a future frame — not due yet
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

extern "C" void v2_input_recorder_init(const char* record_file, const char* replay_file, int strict_replay) {
    g_strict_replay = (strict_replay != 0);
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
            fprintf(stderr, "v2_input_recorder: REPLAY mode from '%s' (%zu events, %s after exhaustion)\n",
                    replay_file, g_replay_queue.size(),
                    g_strict_replay ? "STRICT: ignore real keyboard" : "live keyboard takes over");
        }
    }
}

extern "C" int v2_input_poll_event(SDL_Event* e) {
    if (!e) return 0;

    if (g_mode == MODE_REPLAY) {
        if (dequeue_due_replay(e)) return 1;
        // Queue not yet exhausted, or strict mode → still ignore real keyboard
        bool keyboard_locked = g_strict_replay ||
                               (g_replay_pos < g_replay_queue.size());
        if (keyboard_locked) {
            while (SDL_PollEvent(e)) {
                if (e->type == SDL_KEYDOWN || e->type == SDL_KEYUP) continue;
                return 1;  // pass through window/quit events
            }
            return 0;
        }
        // Queue exhausted + not strict → live keyboard takeover
        return SDL_PollEvent(e);
    }

    // Record / disabled: pure SDL pass-through; log keyboard in record mode.
    if (!SDL_PollEvent(e)) return 0;
    if (g_mode == MODE_RECORD) log_keyboard_event(e);
    return 1;
}

// RECORD mode: called from the game thread inside sub_12352 (the point the game
// reads input). Writes every pending key edge captured by the render thread,
// tagged with the CURRENT game-loop frame (v2_dbg_pre_vm_iter) — i.e. the frame
// the game actually observes the input on, so the recording replays without the
// wait-loop frame-gating deadlock (#180). No-op outside RECORD mode.
extern "C" void v2_input_record_drain(void) {
    if (g_mode != MODE_RECORD || !g_record_file) return;
    std::lock_guard<std::mutex> lk(g_pending_mutex);
    if (g_pending_record.empty()) return;
    for (const auto& p : g_pending_record)
        fprintf(g_record_file, "%d %s %s\n", v2_dbg_pre_vm_iter,
                p.kind == 0 ? "KD" : "KU", p.action);
    fflush(g_record_file);
    g_pending_record.clear();
}

extern "C" void v2_input_recorder_shutdown(void) {
    if (g_record_file) {
        v2_input_record_drain();  // flush any key edges captured but not yet written
        // Persist the frame the recording session actually ended on, so the
        // replay can run for exactly the same number of frames (run to the last
        // frame reached during recording — not the last input event, not an
        // arbitrary tail). record.sh reads this for the .frames budget.
        extern int v2_dbg_pre_vm_iter;
        fprintf(g_record_file, "# end-frame %d\n", v2_dbg_pre_vm_iter);
        fclose(g_record_file);
        g_record_file = nullptr;
    }
    g_mode = MODE_DISABLED;
}
