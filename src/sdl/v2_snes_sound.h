// UX stage 10: the SNES DE sound option — the console's own music and sound
// effects, produced the way the console produces them: the SPC700 driver of
// the ROM runs in an emulated S-SMP/S-DSP (snes_spc, third_party/snes_spc),
// fed by a transcription of the 65816 side of the engine (bank 5 of the ROM:
// block uploads, the per-frame sequencer, the API of ten functions) and of
// the game's glue (bank 0: level music by the level head, sound effects by
// id). See docs2/SNES_SOUND_ENGINE.md for the reverse.
//
// Threads: the game thread queues events (an SPSC ring); the sound thread
// owns the SPC and the 65816-side state, runs the sequencer at the console's
// frame rate and streams 32 kHz stereo into an output ring; the SDL audio
// callback (play.cpp) resamples that into the device stream instead of the
// OPL mix while the option is on. Nothing here touches the DS.
#pragma once
#include <cstdint>

bool v2_snes_sound_enabled();                        // the F1 option (v2_options.sound_mode == 1)
void v2_snes_sound_start();                          // game thread: load the assets, start the thread (idempotent)
void v2_snes_sound_shutdown();

// game-thread events = the console's glue
void v2_snes_snd_level_start(uint16_t level);        // $9BCC: $87E1 (function 7 + dispatch by head+4) + $88B1
void v2_snes_snd_level_exit(uint16_t level);         // $87F9: dispatch by head+6 (death / level end)
void v2_snes_snd_play_sfx(uint8_t id, uint8_t vol);  // $88E5: function 3 (id, FFFF, vol)
void v2_snes_snd_stop_sfx(uint8_t id);               // $88F9 with FFFF: function 4 (id, FFFF)
void v2_snes_snd_sfx_param(uint8_t id, uint16_t v);  // $88F9: function 4 (id, v)
void v2_snes_snd_play_music(uint8_t id);             // op 213 ($C314): function 3 (id, FFFF, 0x30) when music is on
void v2_snes_snd_fade_music(uint8_t id);             // op 214 ($C330): function 4 (id, 0x80) when music is on
void v2_snes_snd_set_music_on(bool on);              // the console's $0302 (music enabled)
void v2_snes_snd_set_sfx_on(bool on);                // the console's $0304 (effects enabled)
void v2_snes_snd_menu_open();                        // $8435 / $EF1A: the pause menu / inventory opens — $890A stops the looping effects (remembering them in $19D5), then SFX 0xE7
void v2_snes_snd_menu_close();
int  v2_snes_sfx_map(int pc_id);                     // the PC effect number's console sequence id (chunk 0x317), 0 = none                       // $84AC / $EF75: the menu closes — $8943 restarts the remembered effects (not on the quit exit)

// audio thread: fill `frames` stereo frames at `rate` from the SNES output;
// false = the option is off / nothing produced (caller keeps its own mix)
bool v2_snes_sound_mix(int16_t* out, uint32_t frames, uint32_t rate);

// The SNES play-SFX op carries a volume byte the PC op ignores; the PC's
// SFX entry point reads this hint (set by the op handlers) — -1 = none,
// the glue's default (0x60, 0x50 for the viking-switch sound 0x83) applies.
extern int v2_snes_sfx_vol_hint;
// The PC's own system clicks (sub_177bb ids 0..4: pause, cursor, item placed, trash) are
// translated to the console's ids by the hook; a site that knows better sets this to the
// console id it wants (0x83 for the inventory's viking switch, 0x100 = no sound) before
// the call. Consumed by the next play.
extern int v2_snes_sys_id_hint;

// v2_vm.cpp: read a chunk without DS side effects (scratch ds_ctx)
extern "C" uint32_t v2_snd_read_chunk(uint16_t cid, uint8_t* dest, uint32_t max);
