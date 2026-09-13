// v2_lvx.h — the flag bits of an LVX trailer record (tools/assets/integrate_snes.py
// build_lvx writes them; v2_vm.cpp's v2_lvx_load reads them). Shared by the engine
// mirror (the scroll limit, the off-screen input gate, the camera pin, the view
// height) and the presenter (v2_smooth.cpp: the presentation camera follows the
// engine's camera exactly on a scene slot).
#pragma once

// UX plan stage 0: LVX trailer flags.
enum { LVX_FULLSCREEN = 0x0001, LVX_CAMLOCK = 0x0002, LVX_TRIO = 0x0004,
       LVX_NOGATE = 0x0008,     // no sub_10813 off-screen input gate (Genesis scene recordings)
       LVX_ALT = 0x0010,        // UX stage 7: an ALTERNATIVE head for a canonical slot (SNES balance)
       LVX_PALTICK3 = 0x0020,   // the palette-animation timers tick once per 3 console frames
                                // (the Genesis scenes; v2_pal_ui_cycle_101be accumulator)
       LVX_CONSOLE = 0x0040,    // UX stage 9: an ALTERNATIVE head gated by the CONSOLE FINALE option
                                // (the SNES finale's BG2 dragon layer + crowd on slot 0x2F)
       LVX_TALL224 = 0x0080 };  // UX stage 9: the level is viewed 224 rows tall without a HUD band,
                                // like the console's finale (map rows 16..239): scroll limit,
                                // camera centring and the object activation edges use 224

// The flags that make a slot a SCENE — a choreographed room where the camera is
// the script's, not the player's (the Genesis interludes carry all four).
enum { LVX_SCENE_FLAGS = LVX_FULLSCREEN | LVX_CAMLOCK | LVX_TRIO | LVX_NOGATE };
