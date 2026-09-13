// ============================================================================
// Callback для второго окна (render_v2)
// Рендер-поток: копирует v2_render_buf в stableBuffer с палитрой
// ============================================================================

#include <cstdio>
#include <cstdint>
#include <cstring>
#include "render_v2.h"

// Оригинальная структура VGA памяти (для палитры)
#include <SDL2/SDL.h>
extern int v2_dbg_pre_vm_iter;   // game-frame counter (v2_vm.cpp), C++ linkage — declared once at file scope (clang rejects block externs inside extern "C" functions)
struct myDrawInfoS_orig {
    uint8_t drawBuffer[65536*4];
    SDL_Color drawPalette[256];
    uint32_t myOffset;
    uint8_t myPixelOffset;
};
extern struct myDrawInfoS_orig* myDrawInfo;

// Функция callback вызывается каждый кадр рендер-потоком
int v2_present_w = 320;   // UX stage 9 step 4: the width of the frame in stableBuffer (read by v2_present_frame)
void render_callback_v2(void* state)
{
    static bool palette_copied = false;

    if (!myDrawInfo_v2) return;

    // Read palette from v2_display_palette SNAPSHOT (captured from SHADOW DS
    // atomically with v2_display_buf at v2_swap_render_buf time). v2 stays
    // independent from orig per CLAUDE.md (no real→shadow copy), and pixels
    // + palette stay matched across level transitions.
    {
        extern SDL_Color v2_display_palette[256];
        extern bool v2_display_palette_valid;
        if (v2_display_palette_valid) {
            std::lock_guard<std::mutex> lock(v2_display_mutex);
            memcpy(myDrawInfo_v2->drawPalette, v2_display_palette, 256 * sizeof(SDL_Color));
            palette_copied = true;
        }
        // Pre-game (no swap yet) → palette stays zero (window black) until first swap
    }

    (void)state;

    uint8_t* sbuf = myDrawInfo_v2->stableBuffer;

    // UX stage 9: an interpolated frame between the two newest ticks (own
    // passes on a snapshot, v2_smooth.cpp) — else the tick frame as before.
    static uint8_t v2_smooth_frame[V2_FB_MAX_W * 240];
    const bool smooth = v2_smooth_render(v2_smooth_frame);
    extern int v2_smooth_last_w;
    // UX stage 9 step 4: the frame's width (its row stride) — the interpolated
    // frame's own, else the published tick frame's
    const int FW = smooth ? v2_smooth_last_w : v2_display_w;
    v2_present_w = FW;
    // debug: V2_PRESENT_DUMP=<dir>:<from>-<to> writes EVERY presented frame whose
    // game frame lies in [from, to] as <dir>/pf_<n>_f<game frame>_<ms>.ppm (the
    // 320x240 canvas the presenter shows: the interpolated frame or the published
    // flip, through the palette in effect) — what the user saw, present by present.
    {
        static int dump = -1; static char dir[480]; static int from = 0, to = -1, n = 0;
        if (dump < 0) {
            const char* e = getenv("V2_PRESENT_DUMP"); dump = 0;
            if (e && *e) { snprintf(dir, sizeof dir, "%s", e); char* c = strrchr(dir, ':');
                           if (c && sscanf(c + 1, "%d-%d", &from, &to) == 2) { *c = 0; dump = 1; } }
        }
        if (dump == 1 && v2_dbg_pre_vm_iter >= from && v2_dbg_pre_vm_iter <= to && n < 4000) {
            extern int v2_smooth_last_reason;   // the file name tells whether the presenter composed the frame itself (sm) or showed the flip (tick<reason>)
            char path[560]; snprintf(path, sizeof path, "%s/pf_%04d_f%d_%u_%s%d.ppm", dir, n, v2_dbg_pre_vm_iter, (unsigned)SDL_GetTicks(),
                                     smooth ? "sm" : "tick", v2_smooth_last_reason);
            FILE* f = fopen(path, "wb");
            if (f) {
                const uint8_t* src = smooth ? v2_smooth_frame : v2_display_buf;
                fprintf(f, "P6\n%d 240\n255\n", FW);
                for (int i = 0; i < FW * 240; i++) { const SDL_Color& c = myDrawInfo_v2->drawPalette[src[i]]; fputc(c.r, f); fputc(c.g, f); fputc(c.b, f); }
                fclose(f);
            }
            n++;
        }
    }
    // debug: V2_SMOOTH_DUMP=<dir> writes the first 48 presented frames after
    // game frame 100 as <dir>/pf_<n>_f<game frame>_t<fraction>.ppm (+ the
    // tick frame it interpolates towards) — proves the sub-tick positions.
    {
        static int dump = -1; static const char* dd = nullptr; static int n = 0; static int from = 100;
        if (dump < 0) { dd = getenv("V2_SMOOTH_DUMP"); dump = (dd && *dd) ? 1 : 0;
                        const char* f0 = getenv("V2_SMOOTH_DUMP_FROM"); if (f0 && *f0) from = atoi(f0); }
        // v2_dbg_pre_vm_iter: file-scope extern (top of file)
        if (dump == 1 && v2_dbg_pre_vm_iter >= from && n < 48) {
            extern int v2_smooth_last_reason;
            char path[512]; snprintf(path, sizeof path, "%s/pf_%02d_f%d_t%.2f_%s%d_%u.ppm", dd, n, v2_dbg_pre_vm_iter,
                                     smooth ? v2_smooth_last_t : -1.0f, smooth ? "sm" : "tick", v2_smooth_last_reason,
                                     (unsigned)SDL_GetTicks());
            FILE* f = fopen(path, "wb");
            if (f) {
                const uint8_t* src = smooth ? v2_smooth_frame : v2_display_buf;
                fprintf(f, "P6\n%d 200\n255\n", FW);
                for (int i = 0; i < FW * 200; i++) { const SDL_Color& c = myDrawInfo_v2->drawPalette[src[i]]; fputc(c.r, f); fputc(c.g, f); fputc(c.b, f); }
                fclose(f);
            }
            n++;
        }
    }
    // Copy viewport (rows 0-175) from the chosen frame under lock
    {
        std::lock_guard<std::mutex> lock(v2_display_mutex);
        const uint8_t* src = smooth ? v2_smooth_frame : v2_display_buf;
        for (int y = 0; y < 176; y++) {
            memcpy(sbuf + y * V2_FB_MAX_W, src + y * FW, FW);
            memset(sbuf + y * V2_FB_MAX_W + FW, 0, V2_FB_MAX_W - FW); // padding
        }
        // F12 PGM dump (v2 path). Clear flag AFTER both orig+v2 saved.
        extern std::atomic<bool> g_dump_pgm_request;
        if (g_dump_pgm_request.load(std::memory_order_acquire)) {
            FILE* f = fopen("/tmp/v2_ladder.ppm", "wb");
            if (f) {
                const int H = v2_display_fullscreen ? v2_display_fullscreen : 176;   // UX stage 0/9: map rows shown
                fprintf(f, "P6\n%d %d\n255\n", v2_display_w, H);
                for (int y = 0; y < H; y++) {
                    for (int x = 0; x < v2_display_w; x++) {
                        uint8_t c = v2_display_buf[y * v2_display_w + x];
                        fputc(myDrawInfo_v2->drawPalette[c].r, f);
                        fputc(myDrawInfo_v2->drawPalette[c].g, f);
                        fputc(myDrawInfo_v2->drawPalette[c].b, f);
                    }
                }
                fclose(f);
                fprintf(stderr, "PGM-DUMP: saved /tmp/v2_ladder.ppm\n");
            }
            // Clear flag — orig render side might've already saved before this point
            g_dump_pgm_request.store(false, std::memory_order_release);
        }
    }
    // Copy HUD (rows 176-239) from v2_display_hud_buf SNAPSHOT (captured atomically
    // with v2_display_buf + v2_display_palette at v2_swap_render_buf time). Live
    // v2_hud_buf without snapshot caused HUD icon flicker on level transitions —
    // HUD pixels updated mid-frame while palette snapshot was from earlier swap.
    extern uint8_t v2_display_hud_buf[];
    {
        std::lock_guard<std::mutex> lock(v2_display_mutex);
        if (v2_display_fullscreen) {
            const uint8_t* src2 = smooth ? v2_smooth_frame : v2_display_buf;   // UX stage 9
            // UX stage 0/9: LVX full-screen scene (200 rows) or an LVX_TALL224
            // level (224 rows) — rows 176.. come from the map render, the rest
            // stay black. The HUD band is never painted on these slots.
            const int shown = v2_display_fullscreen - 176;
            for (int y = 0; y < 64; y++) {
                if (y < shown) memcpy(sbuf + (176 + y) * V2_FB_MAX_W, src2 + (176 + y) * FW, FW);
                else        memset(sbuf + (176 + y) * V2_FB_MAX_W, 0, FW);
                memset(sbuf + (176 + y) * V2_FB_MAX_W + FW, 0, V2_FB_MAX_W - FW); // padding
            }
        } else {
            // the 320-px HUD art sits centred on a wide frame; the wings beside it
            // continue the stone wall (2026-09-06: the sides were black). The HUD
            // is a wall of 32-px blocks with the portrait slots in the middle; the
            // outermost block column on each side (art columns 0..31 / 288..319)
            // is reflected outward, ping-pong, so the wall runs on without a seam:
            // the first mirror axis is the picture edge (the block cut there just
            // doubles), the next ones fall inside the rough stone texture. Only the
            // wall is used — a plain mirror of 53 columns (16:9) reached into the
            // first portrait slot.
            const int x0 = (FW - 320) / 2;
            const int WALL = 32;
            for (int y = 0; y < 64; y++) {
                uint8_t* row = sbuf + (176 + y) * V2_FB_MAX_W;
                const uint8_t* art = v2_display_hud_buf + y * 320;
                memset(row, 0, V2_FB_MAX_W);
                memcpy(row + x0, art, 320);
                for (int d = 1; d <= x0; d++) {                              // d = distance from the picture edge
                    const int k = (d - 1) % (2 * WALL);
                    const int off = (k < WALL) ? k : 2 * WALL - 1 - k;       // 0..31, reflected every 32 px
                    row[x0 - d] = art[off];                                  // left wing
                    if (x0 + 320 + d - 1 < FW) row[x0 + 320 + d - 1] = art[319 - off];   // right wing
                }
            }
            // UX stage 8 step 2 (co-op): the player's number in the corner of
            // the portrait of the viking he holds — the DOS letter look (body
            // colour 2, the (+1,+1) shadow 1: black/white on 1/2 in every
            // level palette).
            static const char* const DIGIT[3][5] = {
                { ".#.", "##.", ".#.", ".#.", "###" },
                { "###", "..#", "###", "#..", "###" },
                { "###", "..#", "###", "..#", "###" },
            };
            for (int vk = 0; vk < 3; vk++) {
                const V2DisplayBadge& b = v2_display_badge[vk];
                if (b.owner < 0 || b.owner > 2) continue;
                const int bx = x0 + b.x + 1, by = b.y + 1;
                for (int r = 0; r < 5; r++)
                    for (int c = 0; c < 3; c++) {
                        if (DIGIT[b.owner][r][c] != '#') continue;
                        const int px = bx + c, py = by + r;
                        if (py + 1 < 64 && px + 1 < FW) sbuf[(176 + py + 1) * V2_FB_MAX_W + px + 1] = 1;   // shadow
                        if (py < 64 && px < FW)         sbuf[(176 + py) * V2_FB_MAX_W + px] = 2;           // body
                    }
            }
        }
    }
}
