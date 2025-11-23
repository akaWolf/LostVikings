#pragma once
// Runtime-configurable key bindings.
//
// Loaded once at startup from a .cfg file (or built-in defaults). All input
// code consults this table — no hardcoded switch/case on SDLK_*.
//
// Two consumers:
//   render_v2.cpp        SDLK → (key_val, spec_off)   game-state plumbing
//   v2_input_recorder    SDLK ↔ action name           replay portability

#include <SDL2/SDL.h>
#include <cstdint>

struct KeyMapEntry {
    const char* action;     // stable name written/read in .inp replays
    SDL_Keycode sdl_key;    // physical key
    uint16_t    key_val;    // OR-mask into input_keys; 0 = none
    uint16_t    spec_off;   // DS offset for sdl_spec_state[]; 0 = none
};

// Initialize keymap. If path == NULL or file missing, built-in defaults used.
// Logs to stderr on parse errors (skips bad lines).
void v2_keymap_load(const char* path);

// Look up by physical key. Returns true if mapped; out args set to bitmask /
// spec offset (either may be 0). Multiple bindings for same SDLK use first.
bool v2_keymap_lookup_sdl(SDL_Keycode key, uint16_t* out_key_val, uint16_t* out_spec_off);

// SDLK → action name. NULL if unmapped (recorder will skip such events).
const char* v2_keymap_sdl_to_action(SDL_Keycode key);

// Action name → canonical SDLK (first binding with that name).
// SDLK_UNKNOWN if action unknown. Used by replay to synthesize events.
SDL_Keycode v2_keymap_action_to_sdl(const char* action);
