// play.h — SDL audio device layer (#79: the AudioPool/adlmidi channel is
// REMOVED; sound is the native interpreted AIL driver in v2_ail/v2_ail_interp,
// rendered by the Nuked OPL3 core in v2_native_opl and mixed here).
//
// This layer only owns the SDL audio device: sound_init() opens it and hands
// the obtained rate to v2_nopl_set_mix_rate(); my_audio_callback() mixes the
// native OPL output (v2_nopl_mix) into the device stream, with the underrun/
// clipping diagnostics and the V2_AUDIO_DUMP tap.
#pragma once

void sound_init();
void my_audio_callback(void* argument, unsigned char* stream, int len);
