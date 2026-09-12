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
#include "v2_coop.h"          // UX stage 8: `# coop N` headers, the P2:/P3: key sets
#include "v2_net.h"           // UX stage 8 step 3: the lockstep batches
#include <string>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <mutex>
#include <atomic>

// (#59) file-scope externs: an extern declaration INSIDE the anonymous
// namespace binds to (anonymous namespace)::name — the classic trap.
extern uint16_t input_keys, input_keys_v2;
extern std::atomic<uint8_t> sdl_spec_state[256];
extern std::atomic<uint8_t> sdl_spec_press_latch[256];
extern std::atomic<uint16_t> sdl_input_press_edges;  // render.cpp (#81 tap accumulator)
extern "C" void sdl_int9_note_keydown(int sdl_scancode);  // render.cpp (#62)
extern "C" uint8_t sdl_int9_dos_scan(int sdl_scancode);   // render.cpp (#86): 0 = no [28C] effect

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
    uint8_t  kind;   // 0=KEYDOWN, 1=KEYUP, 2=typematic repeat (KR, #86)
    SDL_Keycode keycode;
    long     seq;    // sub_12352 call number that first saw it (-1 = legacy)
};
std::vector<ReplayEvent> g_replay_queue;
size_t g_replay_pos = 0;
bool g_replay_exhausted_logged = false;
// Seq channel (level2 saga): the frame tag is too coarse — sub_12352 runs
// SEVERAL times per frame (sub_1086f audio loop re-enters it), each call
// re-publishes word_28896/28898, so WHICH call first sees an event decides
// whether the scene script observes it (an edge published to call #k is
// overwritten by call #k+1 before the VM pass reads it). Live loses taps
// that land between the "wrong" calls — replay must lose the SAME ones.
// Fix: tag every recorded event with the global 12352-call number and
// inject it right before the same-numbered call on replay. Legacy files
// (3 columns) keep the old FRAME_BEGIN drain path bit-for-bit.
bool g_replay_has_seq = false;

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
struct PendingRec { const char* action; uint8_t kind; }; // kind 0=KD 1=KU 2=KR (#86)
std::vector<PendingRec> g_pending_record;
std::mutex g_pending_mutex;

// UX stage 8 step 3 — the lockstep. This client's events wait here until the
// next read packs them into the batch of read n + delay (v2_net.h); the
// events applied at a read are the batches of every player for that read.
bool g_net = false;
int  g_net_local = 0;                       // the player this client's keyboard is
std::vector<V2NetEvent> g_pending_net;      // under g_pending_mutex
long g_net_batches = 0, g_net_applied = 0;


void parse_replay_file(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "v2_input_recorder: replay file '%s' open FAILED — disabled\n", path);
        return;
    }
    char line[128];
    int parsed = 0, skipped = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') {
            // `# coop N`: the recording is a 2- or 3-player game (v2_coop.h)
            int np = 0;
            if (sscanf(line, "# coop %d", &np) == 1) v2_coop_set_players(np);
            continue;
        }
        if (line[0] == '\n' || line[0] == '\0') continue;
        int frame;
        char k[4], action[32];
        long seq = -1;
        int n = sscanf(line, "%d %3s %31s %ld", &frame, k, action, &seq);
        if (n < 3) { skipped++; continue; }
        SDL_Keycode kc = action_to_sdl_key(action);
        if (kc == SDLK_UNKNOWN) {
            fprintf(stderr, "v2_input_recorder: unknown action '%s' at f=%d — skipped\n", action, frame);
            skipped++; continue;
        }
        uint8_t kind;
        if      (strcmp(k, "KD") == 0) kind = 0;
        else if (strcmp(k, "KU") == 0) kind = 1;
        else if (strcmp(k, "KR") == 0) kind = 2;  // typematic repeat (#86)
        else {
            fprintf(stderr, "v2_input_recorder: unknown kind '%s' at f=%d — skipped\n", k, frame);
            skipped++; continue;
        }
        ReplayEvent e{};
        e.frame = frame;
        e.kind = kind;
        e.keycode = kc;
        e.seq = (n == 4) ? seq : -1;
        g_replay_queue.push_back(e);
        parsed++;
    }
    fclose(f);
    // seq mode only when EVERY event carries the 4th column — a mixed file
    // would interleave two delivery disciplines, so it falls back to legacy.
    g_replay_has_seq = !g_replay_queue.empty();
    for (const auto& e : g_replay_queue)
        if (e.seq < 0) { g_replay_has_seq = false; break; }
    fprintf(stderr, "v2_input_recorder: replay loaded %d events from '%s' (%d skipped, %s delivery)\n",
            parsed, path, skipped, g_replay_has_seq ? "seq" : "legacy frame");
}

// Stable one-char names for keys outside the action keymap. Letters and
// digits DO affect the game even unmapped — the INT9 letter channel
// ([28C] = LUT[scancode]) feeds the password screen from EVERY keydown —
// so dropping them made recordings of letter-typed passwords unreplayable
// (the level3 recording lost its T/L/P presses entirely). The replay
// parser already resolves single-char names via parse_sdl_key.
static const char* raw_key_name(SDL_Keycode k) {
    static char names[36][2];
    if ((k >= 'a' && k <= 'z')) { char* s = names[k - 'a'];      s[0] = (char)k; s[1] = 0; return s; }
    if ((k >= '0' && k <= '9')) { char* s = names[26 + k - '0']; s[0] = (char)k; s[1] = 0; return s; }
    return nullptr;
}

void log_keyboard_event(const SDL_Event* e) {
    if (!g_record_file) return;
    uint8_t kind;
    if (e->type == SDL_KEYDOWN && e->key.repeat) {
        // #86: a live typematic repeat's ONLY game effect is the INT9 [28C]
        // note (press_edges / spec latches / key bits are all !repeat-gated
        // in both event loops). Record repeats of [28C]-mapped keys as KR so
        // replays feed the channel the same series; repeats of unmapped
        // scancodes are live no-ops and stay unrecorded.
        if (!sdl_int9_dos_scan(SDL_GetScancodeFromKey(e->key.keysym.sym)))
            return;
        kind = 2;
    } else {
        kind = (uint8_t)(e->type == SDL_KEYDOWN ? 0 : 1);
    }
    const char* action = sdl_key_to_action(e->key.keysym.sym);
    if (!action) action = raw_key_name(e->key.keysym.sym);
    if (!action) return;  // truly irrelevant key (no action, no [28C] effect)
    // Don't write here (render thread): only capture the action + edge into the
    // pending queue. The game thread tags it with the read-frame in
    // v2_input_record_drain (called from sub_12352). See g_pending_record note.
    std::lock_guard<std::mutex> lk(g_pending_mutex);
    g_pending_record.push_back({action, kind});
}

// One event of this client, into the pending batch. Live keys of a client
// that is player k > 1 are its own DOS-layout actions renamed `P<k>:<action>`
// (only the game actions with key bits: the letters, the spec keys and the
// numpad sets belong to the host's keyboard); the host keeps everything of
// its player 1 and, with peers, drops its local P2:/P3: sets (those players
// are the peers). A replay's events (the loopback bench: every instance reads
// the same 3-player file) are kept when their action's player is this one;
// the solo pipeline (no peers) keeps every event.
void net_capture_impl(const SDL_Event* e, bool from_replay) {
    uint8_t kind;
    if (e->type == SDL_KEYDOWN && e->key.repeat) {
        if (!sdl_int9_dos_scan(SDL_GetScancodeFromKey(e->key.keysym.sym))) return;   // #86: a live no-op
        kind = 2;
    } else {
        kind = (uint8_t)(e->type == SDL_KEYDOWN ? 0 : 1);
    }
    const char* action = sdl_key_to_action(e->key.keysym.sym);
    if (!action) action = raw_key_name(e->key.keysym.sym);
    if (!action) return;
    uint16_t kv = 0, so = 0; int player = 0;
    v2_keymap_lookup_sdl_player(e->key.keysym.sym, &kv, &so, &player);
    std::string name = action;
    const bool peers = v2_net_peer_count() > 0;
    if (from_replay) {
        if (peers && player != g_net_local) return;
    } else if (g_net_local > 0) {
        if (player != 0 || kv == 0 || so != 0) return;
        name = "P" + std::to_string(g_net_local + 1) + ":" + name;
    } else if (peers && player != 0) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_pending_mutex);
    g_pending_net.push_back(V2NetEvent{v2_dbg_pre_vm_iter, kind, name});
}

// Replay clock: equals the frame counter on the normal path, but keeps
// ADVANCING inside blocking loops (pw screen, dialogs, viking-switch spins)
// where v2_dbg_pre_vm_iter freezes — otherwise events timestamped past the
// loop entry would never become due and no replay could ever type a
// password. FRAME_BEGIN re-syncs it to the real frame; each blocking-tick
// drain advances it by one virtual frame.
long g_replay_clock = 0;

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
    if ((long)e.frame > g_replay_clock)
        return false;  // event gated to a future (virtual) frame — not due yet
    SDL_zerop(out);
    out->type = (e.kind == 1) ? SDL_KEYUP : SDL_KEYDOWN;
    out->key.keysym.sym = e.keycode;
    out->key.keysym.scancode = SDL_GetScancodeFromKey(e.keycode);
    out->key.repeat = (e.kind == 2) ? 1 : 0;   // KR = typematic repeat (#86)
    out->key.state = (e.kind == 1) ? SDL_RELEASED : SDL_PRESSED;
    out->key.timestamp = SDL_GetTicks();
    g_replay_pos++;
    return true;
}

// (#59) Deterministic replay injection. The render-thread poll loop applied
// KD/KU replay events to sdl_spec_state/input_keys ASYNCHRONOUSLY to the
// game thread's frame_begin snapshot — under load an event tagged frame N
// landed in frame N+-1 (PSNAP flakes on [34]/[3C6] scratch words). In
// REPLAY mode the game thread now drains due events itself, right before
// every sdl_spec_snapshot_take(), applying them synchronously; the poll
// wrapper no longer hands KD/KU to the render loops (see below).
void apply_replay_event(const SDL_Event& e, const char* via) {
    uint16_t key_val = 0, spec_off = 0; int player = 0;
    v2_keymap_lookup_sdl_player(e.key.keysym.sym, &key_val, &spec_off, &player);
    if (player > 0) {
        // A co-op key set (P2: / P3:): the action bits go to that player's
        // accumulators, never to the DOS words — but the INT9 letter channel
        // ([28C] = LUT[scancode], #62) notes EVERY keydown as the DOS handler
        // does: player 3's letters are also the letters of the password screen
        // (the canon pw_* replays type them; skipping the note broke them).
        if (e.type == SDL_KEYDOWN) sdl_int9_note_keydown(SDL_GetScancodeFromKey(e.key.keysym.sym));
        if (g_v2_coop_players > 1) v2_coop_key(player, key_val, e.type == SDL_KEYDOWN, e.key.repeat != 0);
        return;
    }
    if (getenv("V2_DRAIN_LOG")) {
        fprintf(stderr, "DRAIN-%s[f%d]: %s sym=%d key=%04X spec=%04X ik=%04X\n",
                via, v2_dbg_pre_vm_iter, e.type == SDL_KEYDOWN ? "KD" : "KU",
                (int)e.key.keysym.sym, key_val, spec_off,
                (uint16_t)input_keys);
    }
    if (e.type == SDL_KEYDOWN) {
        // #62: replays must feed the INT9 letter channel too — the
        // password screen consumes [28C], not only key bits.
        sdl_int9_note_keydown(SDL_GetScancodeFromKey(e.key.keysym.sym));
        // #86: a KR event mirrors the live typematic path exactly — the
        // event loops gate press_edges and the spec latches with !repeat,
        // so a replayed repeat contributes ONLY the [28C] note (and the
        // no-op key-bit OR below, same as live).
        if (e.key.repeat) {
            input_keys |= key_val; input_keys_v2 |= key_val;
            return;
        }
        // Tap accumulator (#81): BOTH live event loops OR every KEYDOWN
        // into sdl_input_press_edges so a KEYDOWN+KEYUP pair shorter than
        // one sub_12352 interval still lands as an edge. The drain skipped
        // it → any same-frame KD+KU pair in a recording (X11 autorepeat
        // emits Release+Press pairs with repeat=0, ~30 Hz — a held arrow
        // key on the password screen produces exactly that) collapsed to
        // state-no-change on replay and the press was LOST, so recorded
        // live runs diverged (level2 password typed a different word).
        // Physical presses reach this point (KR returned above), so no
        // !repeat filter is needed on the OR.
        if (key_val) sdl_input_press_edges.fetch_or(key_val, std::memory_order_relaxed);
        input_keys |= key_val; input_keys_v2 |= key_val;
    } else {
        input_keys &= (uint16_t)~key_val; input_keys_v2 &= (uint16_t)~key_val;
    }
    if (spec_off) {
        if (e.type == SDL_KEYDOWN) {
            sdl_spec_state[spec_off & 0xFF].store(1, std::memory_order_relaxed);
            sdl_spec_press_latch[spec_off & 0xFF].store(1, std::memory_order_relaxed);
        } else {
            sdl_spec_state[spec_off & 0xFF].store(0, std::memory_order_relaxed);
        }
    }
}

int v2_replay_drain_impl(void) {
    if (g_mode != MODE_REPLAY) return 0;
    // seq-delivery files are injected at the sub_12352 call sites
    // (v2_input_tick_12352) — the frame-clock drain must not double-apply.
    if (g_replay_has_seq) return 0;
    // clock: catch up to the real frame counter when it moves; advance one
    // virtual frame per drain call while it is frozen (blocking loops).
    if ((long)v2_dbg_pre_vm_iter > g_replay_clock)
        g_replay_clock = v2_dbg_pre_vm_iter;
    else
        g_replay_clock++;
    int applied = 0;
    SDL_Event e;
    while (dequeue_due_replay(&e)) {
        if (g_net) net_capture_impl(&e, true);   // lockstep: captured, applied at read + delay
        else apply_replay_event(e, "clk");
        applied++;
    }
    return applied;
}

} // namespace

// C-linkage bridge OUTSIDE the anonymous namespace (the anon-ns extern "C"
// trap: language linkage C but internal storage — the symbol never exports).
extern "C" int v2_replay_drain_to_state(void) { return v2_replay_drain_impl(); }

// Global sub_12352 call counter — the seq-delivery coordinate. Incremented by
// v2_input_tick_12352 at the single input-read point of the running world
// (orig sub_12352 head in default/headless, the v2_read_input_12352_iter
// V2_ONLY branch in standalone). Diagnostics (V2_12352_LOG) print it too.
extern "C" { long g_sub12352_seq = 0; }

// One call = one sub_12352 read. Placed at the TOP of the read, BEFORE the
// press_edges exchange, so that:
//  - RECORD: pending events captured so far are exactly the ones THIS call's
//    exchange/input_keys read will see first → tag them with this seq.
//  - REPLAY (seq files): inject every event tagged <= this seq right before
//    the read — each 12352 call then observes bit-for-bit what live saw,
//    including which call an intra-frame tap lands on (the sub_1086f re-entry
//    structure that made frame-tagged delivery too coarse).
extern "C" void v2_input_tick_12352(void) {
    g_sub12352_seq++;
    if (g_mode == MODE_RECORD) {
        v2_input_record_drain();
        if (!g_net) return;         // a networked game may record too: its read still needs the batches below
    }
    if (g_mode == MODE_REPLAY && g_replay_has_seq) {
        while (g_replay_pos < g_replay_queue.size() &&
               g_replay_queue[g_replay_pos].seq <= g_sub12352_seq) {
            const ReplayEvent& re = g_replay_queue[g_replay_pos];
            SDL_Event e; SDL_zerop(&e);
            e.type = (re.kind == 0) ? SDL_KEYDOWN : SDL_KEYUP;
            e.key.keysym.sym = re.keycode;
            e.key.keysym.scancode = SDL_GetScancodeFromKey(re.keycode);
            e.key.repeat = 0;
            e.key.state = (re.kind == 0) ? SDL_PRESSED : SDL_RELEASED;
            e.key.timestamp = SDL_GetTicks();
            if (g_net) net_capture_impl(&e, true);   // lockstep: this read packs it, read + delay applies it everywhere
            else apply_replay_event(e, "seq");
            g_replay_pos++;
        }
        if (g_replay_pos >= g_replay_queue.size() && !g_replay_exhausted_logged &&
            !g_replay_queue.empty()) {
            g_replay_exhausted_logged = true;
            fprintf(stderr, "v2_input_recorder: REPLAY queue exhausted at frame=%d seq=%ld\n",
                    v2_dbg_pre_vm_iter, g_sub12352_seq);
        }
    }
    // UX stage 8 step 3 — the lockstep read: what this client captured since the
    // last read becomes its batch of read seq + delay (sent, and queued for
    // itself); then the batches of every player for THIS read are applied in
    // player order, through the same apply_replay_event a seq replay uses.
    if (g_net) {
        std::vector<V2NetEvent> batch;
        { std::lock_guard<std::mutex> lk(g_pending_mutex); batch.swap(g_pending_net); }
        v2_net_send_batch(g_sub12352_seq + v2_net_delay(), batch);
        g_net_batches++;
        std::vector<V2NetEvent> evs;
        if (!v2_net_wait_batch(g_sub12352_seq, evs)) return;
        for (const V2NetEvent& ne : evs) {
            SDL_Keycode kc = action_to_sdl_key(ne.action.c_str());
            if (kc == SDLK_UNKNOWN) {
                static int warned = 0;
                if (warned++ < 5) fprintf(stderr, "v2_input_recorder: lockstep action '%s' unknown here — dropped (peers need the same key sets)\n", ne.action.c_str());
                continue;
            }
            SDL_Event e; SDL_zerop(&e);
            e.type = (ne.kind == 1) ? SDL_KEYUP : SDL_KEYDOWN;
            e.key.keysym.sym = kc;
            e.key.keysym.scancode = SDL_GetScancodeFromKey(kc);
            e.key.repeat = (ne.kind == 2) ? 1 : 0;
            e.key.state = (ne.kind == 1) ? SDL_RELEASED : SDL_PRESSED;
            e.key.timestamp = SDL_GetTicks();
            apply_replay_event(e, "net");
            g_net_applied++;
        }
    }
}

extern "C" void v2_input_recorder_net(int local_player) {
    g_net = true;
    g_net_local = local_player;
    fprintf(stderr, "v2_input_recorder: LOCKSTEP mode — this client is player %d, input delay %d read(s)\n",
            local_player + 1, v2_net_delay());
}
extern "C" void v2_input_net_capture(const SDL_Event* e) {
    if (!g_net || !e) return;
    if (e->type != SDL_KEYDOWN && e->type != SDL_KEYUP) return;
    net_capture_impl(e, false);
}

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
        fprintf(g_record_file, "# Format: <frame> <KD|KU|KR> <ACTION> <seq>  (frame = v2_dbg_pre_vm_iter;\n"
                               "# seq = global sub_12352 call number: replay injects the event right\n"
                               "# before the same-numbered input read — intra-frame exact delivery.\n"
                               "# KR = typematic repeat (#86): feeds only the INT9 [28C] channel.\n"
                               "# 3-column files from older builds replay via the legacy frame clock.)\n");
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
        // (#59) KD/KU replay events are now applied ON THE GAME THREAD via
        // v2_replay_drain_to_state() (called from sdl_spec_snapshot_take),
        // synchronously with the frame snapshot — never handed to the
        // render-thread loops (the async apply was the PSNAP flake source).
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

// RECORD mode: called from the game thread at the sub_12352 read head (via
// v2_input_tick_12352). Writes every pending key edge captured by the render
// thread, tagged with the frame (v2_dbg_pre_vm_iter, #180 wait-loop gating)
// AND the 12352 call number (seq column — intra-frame delivery coordinate;
// see g_sub12352_seq note). No-op outside RECORD mode.
extern "C" void v2_input_record_drain(void) {
    if (g_mode != MODE_RECORD || !g_record_file) return;
    std::lock_guard<std::mutex> lk(g_pending_mutex);
    if (g_pending_record.empty()) return;
    for (const auto& p : g_pending_record)
        fprintf(g_record_file, "%d %s %s %ld\n", v2_dbg_pre_vm_iter,
                p.kind == 0 ? "KD" : p.kind == 1 ? "KU" : "KR",
                p.action, g_sub12352_seq);
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
        // v2_dbg_pre_vm_iter: file-scope extern (top of file)
        fprintf(g_record_file, "# end-frame %d\n", v2_dbg_pre_vm_iter);
        fclose(g_record_file);
        g_record_file = nullptr;
    }
    g_mode = MODE_DISABLED;
}
