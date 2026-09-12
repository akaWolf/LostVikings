// v2_mt32.h — SOUND < MT32 >: the game as an owner of a Roland MT-32 heard it
// in 1992. Not a MIDI re-routing: the game's OWN MT-32 driver (DATA.DAT chunk
// 0x1CF, Miles MT32MPU.ADV) runs as a second driver instance in the AIL
// interpreter (v2_ail_interp.cpp, the sink pattern of #83) on the audio
// thread, fed with the very call stream the audible driver receives, and its
// MPU-401 port writes go to an emulated MT-32 (Munt / libmt32emu,
// src/sdl/third_party/mt32emu, LGPL-2.1-or-later; the module's ROMs are the
// user's — roms/mt32/, or the cfg's mt32_roms=<dir>).
//
// What the MT-32 configuration of the game changes (sub_17561 / the loader,
// seg000:2B11-2B39, 7636-7672, 7782), all of it replicated for the instance:
//   card 8 = driver chunk 0x1C7+8 = 0x1CF; music set 3 = timbre bank
//   0x20C+3 = 0x20F (64 custom MT-32 timbres, bank 1, patches 0-63), SFX set
//   0x207+3 = 0x20A (the same 98 sequences arranged for the MT-32), every
//   track's MT-32 variant = table[track]+3; at init the game plays chunk
//   0x215 (a sequence whose TIMB list is all 64 custom timbres) with the
//   timbre-request servicing of sub_176bd switched on ([A378]=0) — the driver
//   asks for each (bank,patch), the game finds it in the bank (sub_17512) and
//   installs it (fn9C, a SysEx upload into the module) — then [A378]=1: from
//   then on the requests are never serviced (every custom timbre is resident).
// The instance sees the FM world's calls: the same AIL functions with the same
// arguments, EXCEPT what the game would have taken from the MT-32 driver
// itself — the fn66 timer parameters and the tick rate (its descriptor), the
// fn99/fn9A cache size (its own), the fn9C timbre installs (the FM world's
// servicing, dropped: the MT-32 world does the 0x215 preload instead) — and
// the data behind the pointers: the bank, SFX set and track paragraphs are
// mapped to the MT-32 chunks in front of the game's memory. Sequence handles
// are the driver's own slot numbers; the preload takes one, so the instance's
// handles are mapped from the FM world's per registration.
#pragma once
#include <cstdint>

bool v2_mt32_enabled();                 // the option (v2_options.sound_mode == 3)
const char* v2_mt32_status();           // "MT-32: ..." for the menu / toasts

// game thread — the producers
void v2_mt32_note_call(uint16_t fn_code, const uint16_t* args, int argc, uint16_t ret);   // every driver call the audible driver got (+ what it returned)
void v2_mt32_note_track(uint16_t chunk_rel);       // sub_1775d loaded a track: chunk_rel = table[track] (the FM variant is +[86B8]; the MT-32 one is +3)
void v2_mt32_publish(const uint8_t* game_ds, uint16_t ds_para,
                     uint8_t* arena, uint16_t arena_para, uint32_t arena_size,
                     uint16_t bank_para, uint16_t sfx_para, uint16_t track_para);   // at the audible driver's boot: the paragraphs the game's pointers use
void v2_mt32_service();                 // every tick: the option on/off, status toasts

// audio thread
void v2_mt32_pump(uint64_t sample_pos, uint32_t offset_frames, uint32_t rate);   // build lazily, drain the calls, fn67 on the sample clock (offset: frames into the current callback, for Munt's timestamps)
bool v2_mt32_mix(int16_t* out, uint32_t frames, uint32_t rate);                // Munt's output REPLACES the buffer; false = not running (no ROMs / off)

void v2_mt32_shutdown();                // V2_MT32_DUMP=<file.mid>: the module's MIDI stream (incl. the SysEx uploads) as an SMF
