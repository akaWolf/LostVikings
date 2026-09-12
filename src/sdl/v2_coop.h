// v2_coop.h — co-op (UX plan stage 8): up to three players in one game, each
// with his own input words and his own active viking.
//
// Model (the SNES DE keeps three active-viking words, [3E2]/[3E4]/[3E6]; the
// DOS build has one, ds:3C2): player 1 IS the DOS game — his words live in
// the DS (3B6 held bits, 3B8 edges, 3BA previous, 3C2 active viking) and every
// engine path that reads them is untouched. Players 2 and 3 live here. While
// the VM executes an object that is the active viking of player k >= 2, the
// reads of [input_keys] / [input_edges] / [active_viking] the object's code
// makes are served from player k's words (v2_coop_view — the hook sits in the
// V2StateView accessors and in V2VM::ds_read), so the unchanged viking
// scripts (`if [active_viking] != [cur_obj] goto stop` and the 119 input
// reads) drive three vikings at once. Outside the VM (camera, HUD, the wait
// loops) every read sees the DS words.
//
// g_v2_coop_players == 1 (the default, and every replay/scenario without a
// `# coop N` header) leaves all of this inert: the game is the byte-exact
// original. Co-op needs V2_ONLY (the default mode mirrors the DOS engine,
// which has no second player).
#pragma once
#include <cstdint>
#include <atomic>

constexpr int V2_COOP_MAX = 3;

struct V2CoopPlayer {
    uint16_t keys = 0, edges = 0, prev = 0;   // this player's 3B6 / 3B8 / 3BA
    uint16_t active = 0xFFFF;                 // this player's active viking (object slot 0/2/4) or none
};
struct V2Coop {
    V2CoopPlayer p[V2_COOP_MAX];              // p[0] mirrors the DS words of player 1 (its `active` = ownership only)
};
extern V2Coop g_coop;
extern int g_v2_coop_players;                 // 1..3
// the object the VM is executing on THIS thread; 0xFFFF outside the VM.
// thread_local: the presenter thread reads the same hooked getters (camera,
// HUD, its own render passes) while the game thread is inside an object —
// its copy stays 0xFFFF, so it always sees the DS words.
extern thread_local uint16_t g_v2_exec_obj;

// SDL side (render thread): per-player held bits and tap edges — the
// counterparts of input_keys / sdl_input_press_edges for players 2 and 3.
extern uint16_t v2_coop_held[V2_COOP_MAX];
extern std::atomic<uint16_t> v2_coop_press[V2_COOP_MAX];

void v2_coop_set_players(int n);
int  v2_coop_players();
// the player whose active viking is object slot `obj`, or -1
int  v2_coop_owner(uint16_t obj);
// a key/button of player `player` (0-based; player 0 is fed through the DOS globals by the caller)
void v2_coop_key(int player, uint16_t bits, bool down, bool repeat);

// Game controllers (SDL_GameController): controller i -> player i (0-based, so
// the first pad doubles player 1's keyboard). Buttons: A action (0x8000), B
// second button (0x4000), X use/talk (0x80), Y item (0x40), LB previous viking
// (0x20), RB next viking (0x10), BACK switch (0x2000), START pause (0x1000);
// the d-pad and the left stick (dead zone 16000) are the four move bits.
struct V2CoopPadEv { int player; uint16_t bits; bool down; };
void v2_coop_pad_added(int device_index);
void v2_coop_pad_removed(int instance_id);
// translate one SDL controller event into 0..4 bit changes; returns the count
int  v2_coop_pad_events(const void* sdl_event, V2CoopPadEv* out, int max);

// RAII: the object the VM executes (set where the DS cur_obj word is set)
struct V2CoopExecGuard {
    uint16_t prev;
    explicit V2CoopExecGuard(uint16_t o) : prev(g_v2_exec_obj) { g_v2_exec_obj = o; }
    ~V2CoopExecGuard() { g_v2_exec_obj = prev; }
};

// game side (v2_vm.cpp)
uint16_t v2_coop_view(uint16_t off, uint16_t real, const uint8_t* ds);
void v2_coop_read_inputs(uint8_t* s);   // after sub_12352: the words of players 2..3 for this read
void v2_coop_cycle(uint8_t* s);         // instead of sub_12e79 in co-op: viking cycling per player, held vikings skipped
void v2_coop_death(uint8_t* s);         // after sub_12e16: reassign the vikings of dead players
