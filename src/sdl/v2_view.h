// Stage 6.2: viewport parameterization SEED (the widescreen groundwork).
//
// The composer (v2_draw_tiles / v2_draw_sprites / v2_draw_ui) still uses
// the native 320x200 (176 play-field + 24 HUD) constants directly; this
// header only NAMES the geometry so the post-stage-6 widescreen work has
// ONE authoritative place to widen. Changing these values today does NOT
// widen anything — the composer migration to V2View is that future work's
// first step, kept out of stage 6 on purpose (no behavior change).
#pragma once
#include <cstdint>

struct V2View {
    int fb_w, fb_h;      // composer framebuffer (320 x 200)
    int play_h;          // play-field height before the HUD split (176)
    int hud_h;           // HUD band height (24)
};

static inline V2View v2_view_native(void) {
    return V2View{320, 200, 176, 24};
}
