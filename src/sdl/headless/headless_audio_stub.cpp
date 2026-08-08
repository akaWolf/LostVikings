// HEADLESS audio stub — replaces play.cpp under HEADLESS build.
//
// #79: the AudioPool machinery is gone project-wide; sound is the native
// interpreted AIL driver (silent in headless: nobody opens a device and
// v2_nopl_mix is never pumped by a callback). Only the sound_init entry
// remains for the shared init path in aux/asm.cpp.

#include <cstdio>

void sound_init() {
    fprintf(stderr, "[headless] sound_init: no audio device (native AIL silent)\n");
}
