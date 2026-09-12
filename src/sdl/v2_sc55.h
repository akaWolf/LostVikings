// v2_sc55.h — the SOUND < SC55 > option: the game's XMIDI music and effects
// on an emulated Roland SC-55 (Nuked-SC55, src/sdl/third_party/nuked_sc55,
// GPL-2.0-or-later) instead of the OPL3. The AIL driver keeps running as
// before (its DS words, statuses and the canon stay the OPL world's); what
// changes is the sound: every MIDI event the driver dispatches (v2_midi.h)
// goes to the module's UART, the module's output replaces the OPL render in
// the audio mix. The ROM images are the user's (roms/sc55/, or the cfg's
// sc55_roms=<dir>): rom1.bin rom2.bin waverom1.bin waverom2.bin rom_sm.bin
// for the SC-55mk2, the sc55_*.bin set for the mk1, the cm300_* set, ...
// (Nuked-SC55's file names); without them the option reports and stays off.
#pragma once
#include <cstddef>
#include <cstdint>

bool v2_sc55_enabled();                  // the option (v2_options.sound_mode == 2)
bool v2_sc55_start();                    // game thread: load the ROMs once, boot the module, GS reset (idempotent); false = see v2_sc55_status()
void v2_sc55_stop();
bool v2_sc55_running();
const char* v2_sc55_status();            // "SC-55mk2 running" / the last error
void v2_sc55_observe(uint16_t status, uint16_t d1, uint16_t d2);   // always (v2_midi): tracks the channels' bank/program/controllers/pitch for a later (re)start
void v2_sc55_service();                                             // game thread, every tick: the boot gate (channel state replayed once the firmware is up)
void v2_sc55_midi(uint16_t status, uint16_t d1, uint16_t d2);      // (retired lane path — the module is fed by v2_sc55_uart_bytes; kept as a no-op for the lane's call site)
// audio thread: the MT-32 world's MPU-401 bytes — what an SC-55 wired to the game's MT-32 driver received in 1992
// (MT-32 reset, the timbre SysEx it cannot load, MT-32 patch numbers and arrangements). Queued while the module
// boots (its firmware sounds nothing before that), flushed in order once it is up, then passed straight through.
void v2_sc55_uart_bytes(const uint8_t* b, size_t n);
// audio thread: the module's output resampled to `rate` INTO `out` (replaces it); false = not running
bool v2_sc55_mix(int16_t* out, uint32_t frames, uint32_t rate);
