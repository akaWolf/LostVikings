// v2_midi.h — the MIDI lane: every channel message the AIL driver dispatches
// (v2_ailnat_midi_2629 — note on/off incl. the deferred XMIDI note-offs,
// controllers after the driver's own volume scaling, program changes, pitch
// wheel; the AIL-only controllers 0x70..0x72 are not MIDI and stay behind)
// mirrored to the SC-55 option (v2_sc55.h) and, with V2_MIDI_DUMP=<file.mid>,
// to a Standard MIDI File of the run (one tick of the driver = one MIDI tick,
// tempo 1 s per `tick_hz` ticks) — the check against assets/music/*.derived.mid.
#pragma once
#include <cstdint>

extern "C" {   // C linkage: the drivers (extern "C" surfaces) call these
void v2_midi_event(uint16_t status, uint16_t d1, uint16_t d2);   // the audible driver's thread (game thread in V2_ONLY, the sink's audio thread in the verify build)
void v2_midi_shutdown(void);                                     // writes the dump, if any
void v2_midi_set_clock(uint64_t (*ticks)(void));                 // the audible instance's tick counter (default: the native pump's)
}
