#pragma once
#include <SDL2/SDL.h>

// V2_ONLY input record/replay. Drop-in wrapper for SDL_PollEvent.
//
// - Record mode (--record-input=<file>): keyboard events from SDL_PollEvent
//   are also logged to file with relative timestamps.
// - Replay mode (--replay-input=<file>): real keyboard events from SDL are
//   IGNORED entirely. Keyboard events come from the recorded file at the
//   recorded timestamps. Non-keyboard events (SDL_QUIT, SDL_WINDOWEVENT)
//   still flow through SDL_PollEvent so window can be closed normally.
// - Disabled mode (neither flag): pure SDL_PollEvent pass-through.
//
// File format (one event per line, frame-based for deterministic replay):
//   <frame_number> KD <ACTION>
//   <frame_number> KU <ACTION>
// frame_number = v2_dbg_pre_vm_iter at event time. Replay injects event when
// current frame counter reaches that value → bit-by-bit reproducibility
// regardless of system clock / FPS variance.

#ifdef __cplusplus
extern "C" {
#endif

// Initialize: pass record_file != NULL for record mode, replay_file != NULL
// for replay mode. Both NULL = pass-through. Mutually exclusive.
// strict_replay: in replay mode, after queue exhausted, behavior:
//   false (default): real keyboard takes over → user can continue interactively
//   true (--replay-strict): keep ignoring real keyboard forever (for headless/CI)
void v2_input_recorder_init(const char* record_file, const char* replay_file, int strict_replay);

// Drop-in replacement for SDL_PollEvent(&e). Returns 1 if event filled, 0 if
// no event pending. Behaviour depends on mode (see header comment).
int v2_input_poll_event(SDL_Event* e);

// UX stage 8 step 3 — the lockstep (v2_net.h). After the lobby: this client's
// events are captured instead of applied (the render loop hands its key events
// to v2_input_net_capture; a replay's events are captured when they fall due)
// and every read applies the batches of all players for that read.
void v2_input_recorder_net(int local_player);
void v2_input_net_capture(const SDL_Event* e);

// RECORD mode only: flush pending key edges (captured by the render thread) to
// the file, tagged with the CURRENT game frame. Call from the game thread at the
// point input is read (sub_12352) so the recorded frame == the frame the game
// observes the input on — required for deadlock-free replay through blocking
// wait-loops that poll input without advancing the frame counter (#180).
void v2_input_record_drain(void);

// Cleanup.
void v2_input_recorder_shutdown(void);

#ifdef __cplusplus
}
#endif
