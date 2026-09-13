// UX stage 9 (2026-09-05), reworked 2026-09-11: presenter-side interpolation
// between the two newest FLIPS — the sub-frames of the DOS game.
//
// The DOS engine renders three sub-frames per game frame (render1 / render2 /
// render3), one per 60 Hz refresh of Mode X, and moves the camera and the
// sprites on every one of them (the object's world position steps once per
// game frame; the sub-sprites follow it in thirds, the viewport scrolls per
// refresh — a DOSBox capture and the flip log agree: 1..3 px per refresh). So
// the original's motion is already 60 Hz smooth, and with the game's vsync
// locked to the display's (render_v2.cpp) every sub-frame is shown once, as on
// the VGA. Nothing is left to interpolate on a 60 or 120 Hz display.
//
// The first version of this file interpolated between game FRAMES: it took the
// positions of consecutive ticks and lerped them over the ~50 ms — throwing the
// original's sub-frame positions away, a game frame of latency, and a ±1 px
// shimmer where the camera and a sprite were rounded separately. Gone.
//
// What remains useful is the display whose refresh is no multiple of 60 (75,
// 90, 144, 165 Hz): the 60 Hz schedule of the game's vsyncs is uneven there
// (a sub-frame held for two refreshes, the next for three), and interpolating
// the camera and the sprite positions between the two newest flips by the
// wall-clock fraction of the sub-frame period gives even motion at the
// display's rate — one sub-frame (16.7 ms) of latency. SMOOTH < NONE | AUTO |
// ON >: AUTO = exactly that case, ON = always (a 60 Hz display then shows
// positions between flips too, at the same latency), NONE = never.
//
// Model (2026-09-11, the display list): the presenter composes from a snapshot of
// the newest flip — the DS (UI, palette, camera), the FS map, and the flip's
// DISPLAY LIST (render_v2.h V2DrawList): the sprite commands the game's layers
// drew, in their order — the early layer (every active object at the sub_1de05
// point) and the late layer exactly as the sub_1dd9c mirror dispatched it. The
// presenter never decides what to draw or in which order: a sprite layer's set
// is a function of the pass (a redraw count that reached 0 in the pass, the force
// flag the pass cleared, a scan hit), not of the state after it, and re-deriving
// it from the snapshot put a hint button over the viking standing on it (level 2,
// the button's count still 1 at the flip, the viking's already 0). Each command
// is moved back towards the previous flip by (1 - t) of its slot's step, rounded
// ONCE relative to the exact interpolated camera (x = cam_i + round(x_f - cam_f)):
// the sprite keeps its distance to the background, no shimmer. A slot absent from
// the previous flip's list or moved more than V2_SMOOTH_MAX_STEP px (spawn,
// teleport, wrap) keeps its newest position; two flips closer than 4 ms are one
// sub-frame (the game flips again in the post-flip phases) — the later one wins.
// At t = 1 every command keeps its own position and the frame IS the flip, byte
// for byte (the stand "presenter == flip" held 0 differences on every canon replay
// before the game thread stopped composing, 2026-09-15). The tile layer and the
// parallax come from the map at the interpolated camera, the foreground tiles
// and the UI from the snapshot; all paint into the presenter's buffer through the
// thread-local overrides of v2_render_funcs.cpp; the game state is never touched,
// the canon (headless, no presenter) is unaffected.
//
// The presenter as the ONLY renderer of the game build (2026-09-15): the game
// thread paints no pixels any more — at every flip it publishes a snapshot into
// the pool below (the DS, the render map, the page's display list and tile words,
// the background VGA of a tile frame or the WHOLE shadow VGA of a chunk screen,
// the HUD art the HUD mirrors painted, the CRTC start and pan of the flip, the
// co-op badges) and goes on; every frame the presenter shows is composed here
// from the newest snapshot (or the two newest, under SMOOTH): a tile frame by
// the passes above, a chunk screen (byte_2AAAF & 0x42: the logos, the title with
// its CRTC scroll, the password screen — the page IS the picture) by the CRTC
// readout of the snapshot's shadow VGA with the CJK overlay, the HUD band from
// the snapshot's art or, on a chunk screen, from the VGA rows 0..63 (the picture's
// bottom lives there behind the split). The test build is untouched: its game
// thread still composes the flip's frame (the LINCMP oracle against the page)
// and its presenter shows the published page.
#include "render_v2.h"
#include "v2_ds_layout.h"
#include "v2_lvx.h"         // the LVX trailer flags: a scene slot's camera is the script's (CAMERA SMOOTH)
#include "v2_ui.h"
#include "v2_coop.h"        // UX stage 8 step 2: the local player's camera; the badges' owners
#include "v2_stats.h"       // the STATS overlay's sub-frame counters
#include "v2_timing.h"      // v2_tick_wait_ticks: the game thread's vsync waits (STATS: work = frame - waits)
#include <SDL2/SDL.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

extern uint8_t* v2_vm_get_shadow_ds();
extern uint8_t v2_vm_shadow_fs[];
extern bool v2_last_frame_tiles;     // v2_render_funcs.cpp: the last frame-kind decision took the tile path
extern thread_local int v2_fbw;      // v2_render_funcs.cpp: the frame width this thread renders
extern uint8_t v2_vga[65536 * 4];    // v2_render_funcs.cpp: the shadow VGA (a chunk screen's page lives there)
extern uint32_t v2_vga_crtc;         // v2_render_funcs.cpp: the CRTC start of the last flip (sub_16775 mirror)
extern uint8_t  v2_vga_pan;          //                       and its pel pan
extern uint8_t v2_hud_buf[320 * 64]; // v2_render_funcs.cpp: the HUD art the HUD mirrors paint (game thread)
extern uint8_t v2_dac_shadow[768];
extern "C" int v2_scene_fullscreen(void);
extern "C" int v2_view_rows(void);
extern "C" uint16_t v2_lvx_flags(uint16_t level);
extern bool v2_vsync_auto_smooth(void);   // render_v2.cpp: the display's refresh is no multiple of 60 (the lock on)
void v2_draw_tiles(uint16_t ds_val);
void v2_draw_sprites(uint16_t ds_val);
void v2_draw_sprites_late(uint16_t ds_val);
void v2_draw_flagged_tiles(uint16_t ds_val);
void v2_draw_ui(uint16_t ds_val);

namespace {
constexpr uint32_t DS_SIZE = 0x10000;
constexpr uint32_t FS_SIZE = 0x10000;
constexpr int V2_SMOOTH_MAX_STEP = 48;       // px per sub-frame beyond which a slot is not interpolated
constexpr double SAME_SUBFRAME_MS = 4.0;     // flips closer than this are one sub-frame (the later wins)

struct Snap {
    uint8_t ds[DS_SIZE];
    uint8_t fs[FS_SIZE];
    V2DrawList draws;           // the sub-frame's display list (render_v2.h): what the sprite layers drew, in order
    uint16_t tile_ovr[32768];   // the shown page's tile words (render_v2.h page lists; index = render-map word)
    bool tile_ovr_valid;
    // one shadow-VGA plane: the BACKGROUND VGA on a tile frame (render_v2.h: the tile layer's
    // pixels), the WHOLE shadow VGA otherwise (vga_full: a chunk screen, or a flip before the
    // map segments are up — the page is presented as the test build presents every frame)
    uint8_t vga[65536 * 4];
    bool vga_full;
    uint32_t crtc; uint8_t pan;  // the CRTC start and pel pan the flip set (sub_16775 mirror)
    uint8_t hud[320 * 64];      // the HUD art as the HUD mirrors left it at this flip
    int rows;                   // v2_view_rows at the flip: 176 (HUD band below), 200 (LVX scene), 224 (LVX_TALL224)
    uint32_t par_acc_x, par_acc_y;
    int subframe;               // the flip's sub-frame: 1..3 in render1..3, 0 elsewhere (render_v2.h MOTION EXACT)
    int frame;                  // the game frame (v2_dbg_pre_vm_iter) at capture — the presentation camera's history
    uint16_t coop_active[V2_COOP_MAX];   // each player's active viking (0xFFFF none) — the presentation camera's target in co-op
    uint64_t t;                 // SDL_GetPerformanceCounter at capture
    uint32_t seq;               // the fill's ordinal (the presenter's composition cache)
    bool valid, tile_frame, fullscreen;
    int w;                      // the frame width of the flip (v2_fbw; UX stage 9 step 4)
    // UX stage 8 step 2: the logical cameras of players 2..3 at this flip
    // (game state, captured with the DS on the game thread) and the badges
    int players;
    uint16_t cam_x[V2_COOP_MAX], cam_y[V2_COOP_MAX];
    bool cam_valid[V2_COOP_MAX];
    V2DisplayBadge badge[3];
};
// A pool of snapshots, handed over by index: the game thread fills a free slot and publishes
// it as the newest (g_cur_i; the previous newest becomes g_prev_i when the flip is a new
// sub-frame), the presenter pins the two it renders from (g_pin_c/g_pin_p) and reads them
// in place. Nothing is copied on either side after the fill: the former g_cur/g_prev pair
// cost a whole-Snap memcpy per flip and two per present (~1.3 MB each), and the presenter's
// copies ran after the vsync latch, inside the ~3 ms it has before the refresh.
constexpr int POOL_N = 6;   // 2 published + 2 pinned + 1 being filled, with one to spare
Snap g_pool[POOL_N];
int g_cur_i = -1, g_prev_i = -1;    // published (under g_mx)
int g_pin_c = -1, g_pin_p = -1;     // pinned by the presenter (under g_mx)
std::mutex g_mx;
uint32_t g_fill_seq = 0;            // under g_mx
static int free_slot() {   // under g_mx: a slot neither published nor pinned
    for (int i = 0; i < POOL_N; i++)
        if (i != g_cur_i && i != g_prev_i && i != g_pin_c && i != g_pin_p) return i;
    return -1;   // unreachable with POOL_N >= 5
}
std::atomic<bool> g_effective{false};   // the last render interpolated
std::atomic<uint32_t> g_subframe_seq{0};      // distinct sub-frames captured (STATS, the VRR presenter)
std::atomic<uint64_t> g_last_flip_ticks{0};   // the newest flip's time

inline int16_t rd16(const uint8_t* b, uint32_t o) { return (int16_t)(b[o] | (b[o + 1] << 8)); }
inline void wr16(uint8_t* b, uint32_t o, int16_t v) { b[o] = (uint8_t)v; b[o + 1] = (uint8_t)((uint16_t)v >> 8); }
inline int16_t lerp16(int16_t a, int16_t b, double t) {
    return (int16_t)(a + (int)((double)(b - a) * t + ((b >= a) ? 0.5 : -0.5)));
}
}  // namespace

float v2_smooth_last_t = -1.0f;    // debug: fraction of the last interpolated frame
int v2_smooth_last_w = 320;         // the width of the last composed frame (its row stride)
int v2_smooth_last_reason = 0;     // debug: 0 = a frame was composed, 2 = no snapshot yet

extern int v2_dbg_pre_vm_iter;   // the game-frame counter (pre-VM barrier)

static void fill(Snap& S, const uint8_t* s) {
    memcpy(S.ds, s, DS_SIZE);
    memcpy(S.fs, v2_vm_shadow_fs, FS_SIZE);
    v2_drawlist_copy(S.draws, v2_frame_draws);   // the flip's display list (records + the used arena)
    S.tile_ovr_valid = (v2_tile_override != nullptr);   // the page lists: the composed page's tile words
    if (S.tile_ovr_valid) memcpy(S.tile_ovr, v2_tile_override, sizeof S.tile_ovr);
    // A tile frame: the frame-kind decision of this flip took the tile path (v2_compose_page:
    // the map segments are up and the level is no chunk screen) AND this flip's level is not a
    // chunk screen (byte_2AAAF & 0x42 — sub_11439 skips the tile render there, the chunk pixels
    // stay in VGA). The flag alone went stale across a level change: the fade-in flips of the
    // title (a chunk screen) after the S&S logo (a tile level) still carried it, the presenter
    // composed them itself and its buffer showed whatever it had composed last — the S&S logo
    // under the title's palette (2026-09-11 report).
    S.tile_frame = v2_last_frame_tiles && !(s[DS_LEVEL_FLAGS] & 0x42);
    S.vga_full = !S.tile_frame;
    memcpy(S.vga, S.vga_full ? v2_vga : v2_vga_bg, sizeof S.vga);   // the page's source: the whole shadow VGA, or the tile layer's plane
    S.crtc = v2_vga_crtc; S.pan = v2_vga_pan;
    memcpy(S.hud, v2_hud_buf, sizeof S.hud);
    S.rows = v2_view_rows();
    S.par_acc_x = v2_parallax.acc_x;
    S.par_acc_y = v2_parallax.acc_y;
    S.subframe = v2_flip_subframe();
    S.frame = v2_dbg_pre_vm_iter;
    for (int k = 0; k < V2_COOP_MAX; k++) S.coop_active[k] = g_coop.p[k].active;
    S.t = SDL_GetPerformanceCounter();
    S.fullscreen = v2_scene_fullscreen() != 0;
    S.w = v2_fbw;
    S.players = g_v2_coop_players;
    for (int k = 0; k < V2_COOP_MAX; k++) {
        S.cam_x[k] = g_coop.p[k].cam_x;
        S.cam_y[k] = g_coop.p[k].cam_y;
        S.cam_valid[k] = (k > 0) && g_coop.p[k].cam_valid;
    }
    // UX stage 8 step 2 (co-op): which player holds each viking, and where its portrait sits in
    // the HUD art (ds:[vk-0x7A84] -> the VGA offset of v2_draw_hud_portrait) — the presenter
    // paints the P1/P2/P3 badges
    for (int vk = 0; vk < 3; vk++) {
        V2DisplayBadge& b = S.badge[vk];
        b.owner = -1; b.x = 0; b.y = 0;
        if (g_v2_coop_players <= 1) continue;
        const uint16_t vga_off = (uint16_t)rd16(s, (uint16_t)(vk * 2 - 0x7A84));
        b.x = (vga_off % 86) * 4 + 3;
        b.y = vga_off / 86;
        b.owner = v2_coop_owner((uint16_t)(vk * 2));
    }
    S.seq = ++g_fill_seq;
    S.valid = true;
}

// Game thread, at every page flip (v2_swap_render_buf): the flip becomes the
// newest sub-frame; a flip within SAME_SUBFRAME_MS of the newest replaces it
// (the post-flip phases flip again without a vsync wait), any other shifts it
// into the previous slot.
void v2_smooth_capture(void) {
    uint8_t* s = v2_vm_get_shadow_ds();
    if (!s) return;
    std::lock_guard<std::mutex> lock(g_mx);
    // debug: V2_SMOOTH_DUMP — every page flip as the game thread made it: the
    // viewport and the active viking's object / first sub-sprite positions, so the
    // motion WITHIN a game frame (the sub-frames) can be read
    { static FILE* ff = nullptr; static int init = 0; static int nflip = 0;
      if (!init) { init = 1; const char* dd = getenv("V2_SMOOTH_DUMP");
          if (dd && *dd) { char path[512]; snprintf(path, sizeof path, "%s/flip_log.txt", dd); ff = fopen(path, "w"); } }
      if (ff) { const uint16_t vk = (uint16_t)rd16(s, DS_ACTIVE_VIKING); const uint16_t sub = (uint16_t)rd16(s, vk + OBJ_SUB_SLOT);
          fprintf(ff, "FLIP #%d f%d t=%.1fms vp %d,%d | vik %02X obj %d,%d | sub %02X %d,%d\n", nflip++, v2_dbg_pre_vm_iter,
                  (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency() * 1000.0, rd16(s, DS_VIEWPORT_X), rd16(s, DS_VIEWPORT_Y),
                  vk, rd16(s, vk + OBJ_WORLD_X), rd16(s, vk + OBJ_WORLD_Y), sub,
                  sub <= 0xFE ? rd16(s, sub + OBJ_SPRITE_X) : 0, sub <= 0xFE ? rd16(s, sub + OBJ_SPRITE_Y) : 0); } }
    const uint64_t now = SDL_GetPerformanceCounter();
    const Snap* cur = (g_cur_i >= 0 && g_pool[g_cur_i].valid) ? &g_pool[g_cur_i] : nullptr;
    const double since_cur_ms = cur ? (double)(now - cur->t) / (double)SDL_GetPerformanceFrequency() * 1000.0 : 1e9;
    const bool new_subframe = !cur || since_cur_ms >= SAME_SUBFRAME_MS;
    const int k = free_slot();
    if (k < 0) return;   // cannot happen with POOL_N slots; never overwrite what the presenter reads
    fill(g_pool[k], s);
    if (cur && new_subframe) g_prev_i = g_cur_i;   // a new sub-frame: the newest so far becomes the previous one
    g_cur_i = k;                                    // a flip within SAME_SUBFRAME_MS replaces the newest instead
    g_last_flip_ticks.store(g_pool[k].t, std::memory_order_relaxed);
    if (new_subframe) {
        g_subframe_seq.fetch_add(1, std::memory_order_relaxed);
        v2_stats.subframes.fetch_add(1, std::memory_order_relaxed);
        // sub-frames per game frame: the count of the frame just finished (3 in play); its wall time
        // (first flip to first flip) and its work = that time minus what the game thread spent in
        // its vsync waits (v2_tick_wait_ticks, v2_timing.h); slow = the work alone exceeded a refresh
        static int last_iter = -1, in_frame = 0; static uint64_t frame_t0 = 0, wait_t0 = 0;
        if (v2_dbg_pre_vm_iter != last_iter) {
            const uint64_t waited = v2_tick_wait_ticks.load(std::memory_order_relaxed);
            if (last_iter >= 0 && frame_t0) {
                const double freq = (double)SDL_GetPerformanceFrequency();
                const double frame_ms = (double)(g_pool[g_cur_i].t - frame_t0) / freq * 1000.0;
                const double work_ms = frame_ms - (double)(waited - wait_t0) / freq * 1000.0;
                v2_stats.subframes_per_frame.store(in_frame, std::memory_order_relaxed);
                v2_stats.frame_ms_x100.store((int)(frame_ms * 100.0 + 0.5), std::memory_order_relaxed);
                v2_stats.work_ms_x100.store((int)((work_ms > 0.0 ? work_ms : 0.0) * 100.0 + 0.5), std::memory_order_relaxed);
                if (work_ms > 16.7) v2_stats.slow_frames.fetch_add(1, std::memory_order_relaxed);
            }
            last_iter = v2_dbg_pre_vm_iter; in_frame = 0; frame_t0 = g_pool[g_cur_i].t; wait_t0 = waited;
        }
        in_frame++;
    }
}
uint32_t v2_smooth_subframe_seq(void) { return g_subframe_seq.load(std::memory_order_relaxed); }
uint64_t v2_smooth_last_flip_ticks(void) { return g_last_flip_ticks.load(std::memory_order_relaxed); }

// the SMOOTH option: 0 NONE, 1 AUTO (the display's refresh is no multiple of 60), 2 ON
static bool smooth_wanted(void) {
    switch (v2_options.smooth.load()) {
        case 2: return true;
        case 1: return v2_vsync_auto_smooth();
        default: return false;
    }
}
bool v2_smooth_effective(void) { return g_effective.load(std::memory_order_relaxed); }

// The HUD band as presented (render_v2.h): the 320-px HUD art sits centred on a wide frame; the
// wings beside it continue the stone wall (2026-09-06: the sides were black). The HUD is a wall
// of 32-px blocks with the portrait slots in the middle; the outermost block column on each
// side (art columns 0..31 / 288..319) is reflected outward, ping-pong, so the wall runs on
// without a seam: the first mirror axis is the picture edge (the block cut there just doubles),
// the next ones fall inside the rough stone texture. Only the wall is used — a plain mirror of
// 53 columns (16:9) reached into the first portrait slot. Then (UX stage 8 step 2, co-op) the
// player's number in the corner of the portrait of the viking he holds — the DOS letter look
// (body colour 2, the (+1,+1) shadow 1: black/white on 1/2 in every level palette).
void v2_layout_hud_band(uint8_t* dst, int stride, int FW, const uint8_t* hud320, const V2DisplayBadge* badges) {
    const int x0 = (FW - 320) / 2;
    const int WALL = 32;
    for (int y = 0; y < 64; y++) {
        uint8_t* row = dst + (size_t)y * stride;
        const uint8_t* art = hud320 + y * 320;
        memset(row, 0, (size_t)stride);
        memcpy(row + x0, art, 320);
        for (int d = 1; d <= x0; d++) {                              // d = distance from the picture edge
            const int k = (d - 1) % (2 * WALL);
            const int off = (k < WALL) ? k : 2 * WALL - 1 - k;       // 0..31, reflected every 32 px
            row[x0 - d] = art[off];                                  // left wing
            if (x0 + 320 + d - 1 < FW) row[x0 + 320 + d - 1] = art[319 - off];   // right wing
        }
    }
    if (!badges) return;
    static const char* const DIGIT[3][5] = {
        { ".#.", "##.", ".#.", ".#.", "###" },
        { "###", "..#", "###", "#..", "###" },
        { "###", "..#", "###", "..#", "###" },
    };
    for (int vk = 0; vk < 3; vk++) {
        const V2DisplayBadge& b = badges[vk];
        if (b.owner < 0 || b.owner > 2) continue;
        const int bx = x0 + b.x + 1, by = b.y + 1;
        for (int r = 0; r < 5; r++)
            for (int c = 0; c < 3; c++) {
                if (DIGIT[b.owner][r][c] != '#') continue;
                const int px = bx + c, py = by + r;
                if (py + 1 < 64 && px + 1 < FW) dst[(size_t)(py + 1) * stride + px + 1] = 1;   // shadow
                if (py < 64 && px < FW)         dst[(size_t)py * stride + px] = 2;             // body
            }
    }
}

// the passes of a tile frame, in the gameplay frame's order, on the caller's buffers: `work` is
// the snapshot's DS with the camera and the moved positions already written, pos_x/pos_y the
// commands' positions (nullptr = each command's own), acc the parallax accumulators
static void compose_map(const Snap& C, const int16_t* pos_x, const int16_t* pos_y, const uint32_t* acc, uint8_t* work, uint8_t* out) {
    const int savew = v2_fbw;
    v2_tls_ds = work;
    v2_tls_out = out;
    v2_fbw = C.w;                   // this thread renders at the snapshot's width
    v2_tls_fs = C.fs;
    v2_tls_par_acc = acc;
    v2_tls_presenter = true;
    v2_tls_tile_ovr = C.tile_ovr_valid ? C.tile_ovr : nullptr; v2_tls_ui_cells_from_page = C.tile_ovr_valid; v2_tls_vga_bg = C.vga;
    v2_draw_tiles(0);
    if (!v2_parallax.on) {   // the exact page: every command in the passes' order (render_v2.h V2_CMD_FGTILE)
        v2_tls_fg_from_page = true;
        v2_draw_list(C.draws, pos_x, pos_y, -1);   // the flip's own commands, in its order, at the given positions
        v2_draw_flagged_tiles(0);
        v2_tls_fg_from_page = false;
    } else {                 // the console parallax layer's priority pass between the sprites and the rest
        v2_draw_list(C.draws, pos_x, pos_y, 0);
        v2_draw_flagged_tiles(0);
        v2_draw_list(C.draws, pos_x, pos_y, 1);
    }
    v2_draw_ui(0);
    v2_tls_tile_ovr = nullptr; v2_tls_ui_cells_from_page = false; v2_tls_vga_bg = nullptr;
    v2_tls_presenter = false;
    v2_tls_par_acc = nullptr;
    v2_tls_fs = nullptr;
    v2_tls_out = nullptr;
    v2_tls_ds = nullptr;
    v2_fbw = savew;
}

// the page of a chunk screen (or of a flip before the map segments are up): the CRTC readout
// of the snapshot's whole shadow VGA — the window through the CRTC start the sub_16775 mirror
// set (the title's scroll from the art to the logo), 320 x 176 — the HUD band from the VGA rows
// 0..63 (the picture's bottom lives there behind the split), the CJK text overlay (UX6) over it
static void compose_page(const Snap& C, uint8_t* work, uint8_t* out, uint8_t* hud) {
    memset(out, 0, (size_t)320 * 240);
    v2_vga_readout(out, 320, 176, C.vga, C.crtc, C.pan, false);
    for (int y = 0; y < 64; y++) memcpy(hud + y * 320, C.vga + (size_t)y * 0x56u * 4u, 320);
    memcpy(work, C.ds, DS_SIZE);
    const int savew = v2_fbw;
    v2_tls_ds = work; v2_tls_out = out; v2_fbw = 320; v2_tls_presenter = true; v2_tls_ui_cells_from_page = true;
    v2_draw_ui(0);   // the CJK overlay only (the cells are the page's glyph commands, already in the picture)
    v2_tls_ui_cells_from_page = false; v2_tls_presenter = false; v2_tls_out = nullptr; v2_tls_ds = nullptr;
    v2_fbw = savew;
}

// The sub-pixel layers (render_v2.h V2PresentLayers): the presenter's own buffers. A world
// layer carries one tile of margin beyond the frame (it is shown shifted left/up by the camera's
// fraction); the commands' bitmaps live in one arena — the page lists bound their pixels well
// below it (a type-2 record's strips are its data, 36 bytes each in a 768 KB list arena).
namespace {
// the world layers around the logical camera carry CAM_MARGIN px on every side (the presentation
// camera's reach: the leash, the sub-frame interpolation, the shake); the parallax layers follow
// the presentation camera itself and carry the old tile of margin right / below
constexpr int CAM_MARGIN = 32;
constexpr int LAYER_W = V2_FB_MAX_W + 2 * CAM_MARGIN, LAYER_H = 240 + 2 * CAM_MARGIN;
constexpr int PAR_W = V2_FB_MAX_W + 8, PAR_H = 248;
uint8_t s_bg[LAYER_W * LAYER_H], s_bg_cov[LAYER_W * LAYER_H];
uint8_t s_prio[LAYER_W * LAYER_H], s_prio_cov[LAYER_W * LAYER_H];
uint8_t s_par0[PAR_W * PAR_H], s_par1[PAR_W * PAR_H], s_par1_cov[PAR_W * PAR_H];
uint8_t s_ui[V2_FB_MAX_W * 240], s_ui_cov[V2_FB_MAX_W * 240];
constexpr size_t CMD_ARENA = 4u << 20;
uint8_t s_cmd_px[CMD_ARENA], s_cmd_cov[CMD_ARENA];
V2PresentLayer s_cmd[V2_DRAWLIST_MAX];
// any pixel covered? (a layer nothing was painted into is dropped: no conversion, no copy)
bool cov_any(const uint8_t* cov, size_t n) {
    const uint64_t* q = (const uint64_t*)cov; const size_t n8 = n / 8;
    for (size_t i = 0; i < n8; i++) if (q[i]) return true;
    for (size_t i = n8 * 8; i < n; i++) if (cov[i]) return true;
    return false;
}
}  // namespace

// The layers of a tile frame (render_v2.h): compose_map's passes split by what moves separately
// at k x. dev_x/dev_y = each command's device position (its 1/k position relative to the
// presentation camera, computed by the caller); pcam = the presentation camera (game px, exact);
// Vr = the flip's logical camera, which `work` (the snapshot's DS) holds — the world layers are
// composed around it with CAM_MARGIN px on every side and placed by their distance to pcam.
static void compose_layers(const Snap& C, const int* dev_x, const int* dev_y, const uint32_t* acc, int k, const double pcam[2], const int Vr[2],
                           uint8_t* work, uint8_t* hud, V2DisplayBadge* badges, V2PresentLayers* L) {
    const int W = C.w, rows = C.rows;
    const int clip_h = (rows == 200) ? 187 : rows;   // v2_draw_tiles' sprite / text clip of this frame
    const int M = CAM_MARGIN, LW = W + 2 * M;
    const long ck[2] = { lround(pcam[0] * (double)k), lround(pcam[1] * (double)k) };   // the presentation camera in device pixels
    const int savew = v2_fbw;
    v2_tls_ds = work;
    v2_tls_fs = C.fs;
    v2_tls_par_acc = acc;
    v2_tls_presenter = true;
    v2_tls_tile_ovr = C.tile_ovr_valid ? C.tile_ovr : nullptr; v2_tls_ui_cells_from_page = C.tile_ovr_valid; v2_tls_vga_bg = C.vga;
    const bool par = v2_parallax.on;
    L->par0 = V2PresentLayer{ nullptr, nullptr, 0, 0, 0, 0, 0 };
    L->par1 = L->par0;
    // 0. a parallax level: the layer under the tiles follows the presentation camera — composed
    //    for its whole part (v2_tls_par_view) with a tile of margin right / below and shifted by the
    //    remainder of the layer's own offset (floor(c f / 256) per axis in the pass; an autoscroll
    //    axis ignores the camera), so the parallax moves at its factor at 1/k too
    int pv[2] = { (int)floor(pcam[0]), (int)floor(pcam[1]) };
    if (pv[0] < 0) pv[0] = 0; if (pv[1] < 0) pv[1] = 0;
    int par_dx = 0, par_dy = 0;
    if (par) {
        auto par_shift = [&](int axis) -> int {
            const uint16_t f = axis ? v2_parallax.fy : v2_parallax.fx;
            if (f & 0x8000) return 0;
            const double exact = pcam[axis] * (double)(f & 0x7FFF) / 256.0;
            const double whole = (double)(((uint32_t)(uint16_t)pv[axis] * (uint32_t)(f & 0x7FFF)) >> 8);
            return -(int)lround((exact - whole) * (double)k);
        };
        par_dx = par_shift(0); par_dy = par_shift(1);
        v2_tls_par_view = pv;
        v2_tls_out = s_par0; v2_tls_cov = nullptr; v2_fbw = W + 8; v2_clip_h = clip_h + 8; v2_tls_kx_lead = 0; v2_tls_kx_margin = 0; v2_tls_rows_max = PAR_H;
        memset(s_par0, 0, (size_t)(W + 8) * (size_t)(clip_h + 8));
        v2_draw_parallax_layer(0, 0);
        L->par0 = V2PresentLayer{ s_par0, nullptr, W + 8, clip_h + 8, W + 8, par_dx, par_dy };
        v2_tls_par_view = nullptr;
    }
    // 1. the tile layer around the logical camera: the tile pass and the background-VGA readout
    //    with CAM_MARGIN px of margin on every side (v2_tls_kx_lead / v2_tls_kx_margin) — opaque, or
    //    with coverage over the parallax layer (index 0 uncovered, v2_tls_par_separate)
    v2_tls_out = s_bg; v2_tls_cov = par ? s_bg_cov : nullptr; v2_fbw = LW; v2_tls_kx_lead = M; v2_tls_kx_margin = M; v2_tls_rows_max = LAYER_H; v2_tls_par_separate = par;
    if (par) memset(s_bg_cov, 0, (size_t)LW * (size_t)(rows + 2 * M));
    v2_draw_tiles(0);
    const int bg_dx = (int)((long)(Vr[0] - M) * k - ck[0]), bg_dy = (int)((long)(Vr[1] - M) * k - ck[1]);   // the layer's (0, 0) is the world point (Vr - M)
    L->bg = V2PresentLayer{ s_bg, par ? s_bg_cov : nullptr, LW, rows + 2 * M, LW, bg_dx, bg_dy };
    // 2. the commands: each alone in a bitmap of its extent, at its own device position, in the
    //    list's order — on a parallax level the sprites first, then the priority layer, then the
    //    glyphs and repaints (compose_map's two passes); else every command in order
    size_t used = 0; int n = 0;
    auto add = [&](int i) {
        const V2DrawCmd& c = C.draws.cmd[i];
        if (c.type == V2_CMD_FGTILE && (c.dead[0] & 1u)) return;   // its one cell restored since the repaint (v2_draw_list skips it)
        int cw = 0, ch = 0; v2_cmd_extent_of(c, &cw, &ch);
        if (cw <= 0 || ch <= 0) return;
        const size_t need = (size_t)cw * (size_t)ch;
        if (used + need > CMD_ARENA) {
            static bool said = false;
            if (!said) { said = true; fprintf(stderr, "V2-KX: the command arena is full after %d commands — the rest of this frame's commands are not shown\n", n); }
            return;
        }
        uint8_t* px = s_cmd_px + used; uint8_t* cov = s_cmd_cov + used; used += need;
        memset(px, 0, need); memset(cov, 0, need);
        v2_raster_cmd_bitmap(C.draws, i, px, cov, cw, ch);
        s_cmd[n] = V2PresentLayer{ px, cov, cw, ch, cw, dev_x[i], dev_y[i] };
        n++;
    };
    for (int i = 0; i < C.draws.n; i++) {
        const V2DrawCmd& c = C.draws.cmd[i];
        if (par && (c.type == V2_CMD_GLYPH || c.type == V2_CMD_FGTILE)) continue;
        add(i);
    }
    L->prio_after = n;
    L->prio = V2PresentLayer{ nullptr, nullptr, 0, 0, 0, 0, 0 };
    if (par) {
        // 3a. the parallax's priority-1 cells over the sprites, for the presentation camera (coverage)
        v2_tls_par_view = pv;
        v2_tls_out = s_par1; v2_tls_cov = s_par1_cov; v2_fbw = W + 8; v2_clip_h = clip_h + 8; v2_tls_kx_lead = 0; v2_tls_kx_margin = 0; v2_tls_rows_max = PAR_H;
        memset(s_par1, 0, (size_t)(W + 8) * (size_t)(clip_h + 8)); memset(s_par1_cov, 0, (size_t)(W + 8) * (size_t)(clip_h + 8));
        v2_draw_parallax_layer(0, 1);
        L->par1 = V2PresentLayer{ s_par1, s_par1_cov, W + 8, clip_h + 8, W + 8, par_dx, par_dy };
        if (!cov_any(s_par1_cov, (size_t)(W + 8) * (size_t)(clip_h + 8))) L->par1.px = nullptr;
        v2_tls_par_view = nullptr;
        // 3b. the map's flagged tiles over the sprites (v2_draw_flagged_tiles without its parallax
        //     pass), around the logical camera with the tile layer's margins, coverage
        v2_tls_out = s_prio; v2_tls_cov = s_prio_cov; v2_fbw = LW; v2_tls_kx_lead = M; v2_tls_kx_margin = M; v2_tls_rows_max = LAYER_H; v2_clip_h = clip_h + 2 * M;
        memset(s_prio, 0, (size_t)LW * (size_t)(rows + 2 * M)); memset(s_prio_cov, 0, (size_t)LW * (size_t)(rows + 2 * M));
        v2_draw_flagged_tiles(0);
        L->prio = V2PresentLayer{ s_prio, s_prio_cov, LW, rows + 2 * M, LW, bg_dx, bg_dy };
        if (!cov_any(s_prio_cov, (size_t)LW * (size_t)(rows + 2 * M))) L->prio.px = nullptr;   // nothing painted: no layer
        for (int i = 0; i < C.draws.n; i++) {
            const V2DrawCmd& c = C.draws.cmd[i];
            if (c.type == V2_CMD_GLYPH || c.type == V2_CMD_FGTILE) add(i);
        }
    }
    v2_tls_kx_lead = 0; v2_tls_kx_margin = 0; v2_tls_rows_max = 240; v2_tls_par_separate = false;
    // 4. the text plane (its cells when they are not page commands) and the CJK overlay: screen-
    //    anchored, no shift, the frame's own width and clip
    memset(s_ui, 0, (size_t)W * 240); memset(s_ui_cov, 0, (size_t)W * 240);
    v2_tls_out = s_ui; v2_tls_cov = s_ui_cov; v2_fbw = W; v2_clip_h = clip_h;
    v2_draw_ui(0);
    L->ui = V2PresentLayer{ s_ui, s_ui_cov, W, clip_h, W, 0, 0 };
    if (!cov_any(s_ui_cov, (size_t)W * (size_t)clip_h)) L->ui.px = nullptr;   // no text this frame: no layer
    v2_tls_cov = nullptr;
    v2_tls_tile_ovr = nullptr; v2_tls_ui_cells_from_page = false; v2_tls_vga_bg = nullptr;
    v2_tls_presenter = false;
    v2_tls_par_acc = nullptr;
    v2_tls_fs = nullptr;
    v2_tls_out = nullptr;
    v2_tls_ds = nullptr;
    v2_fbw = savew;
    memcpy(hud, C.hud, 320 * 64);
    L->k = k; L->w = W; L->rows = rows > 176 ? rows : 0; L->map_h = rows; L->clip_h = clip_h;
    L->n_cmd = n; L->cmd = s_cmd; L->hud = hud; L->badges = badges;
}

// One frame from a snapshot (P = the previous one when interpolating, t its fraction; local =
// the player whose own camera replaces the DS camera, -1 for the DS camera): the map rows into
// `out` (w x 240), the HUD art into `hud`, the badges, the width and the full-screen row count
// (0 = the HUD layout). The caller owns every buffer (the presenter thread its own, the flip
// dump on the game thread its own). L with k >= 1 (the sub-pixel presentation): a tile frame is
// composed as LAYERS into L — `out` gets the flat frame beside them only when out_too asks (the
// stand V2_KX_SELFTEST compares the two at t = 1; the flat frame then stands on the layers'
// whole camera, which is the flip's own when nothing is interpolated); a chunk screen always
// lands in `out` (L->k stays 0).
// MOTION EXACT (render_v2.h): the presenter's per-object history — the whole position, the
// previous whole position and the fraction of the frame last seen, and the fraction of the frame
// before it (F_prev), per axis; stamped by the composition that updated it
namespace {
struct MotionHist { uint32_t stamp; bool valid; int16_t W[2], XP[2]; int F[2], Fprev[2]; };
thread_local MotionHist g_mhist[0x80];   // per object slot (di / 2)
thread_local uint32_t g_mhist_comp = 0;  // the composition counter
// the sub-sprite catch-up steps applied by the renders up to sub-frame r (0 = none)
inline int catchup_sum(int r, int d) { int s = 0; for (int i = 0; i < r && i < 3; i++) s += v2_subsprite_delta_fn(i, (int16_t)d); return s; }
// the correction of a sprite's whole position at sub-frame r to the exact trajectory (render_v2.h):
// F_prev / 256 + r v / 3 - the steps applied, v = d + (F - F_prev) / 256; r = 0 (a flip outside the
// renders: the sprites stand at the last render's step) is taken as the frame's end, where the
// correction is F / 256 whatever F_prev
inline double exact_delta(int r, int d, int F, int Fprev) {
    if (r <= 0 || r > 3) r = 3;
    const double v = (double)d + (double)(F - Fprev) / 256.0;
    return (double)Fprev / 256.0 + (double)r * v / 3.0 - (double)catchup_sum(r, d);
}
// the object whose sub-sprite range holds `slot` (the catch-up's gates: a live object with
// sub-sprites), -1 for none
int owner_of(const uint8_t* ds, uint16_t slot) {
    const uint16_t n = (uint16_t)rd16(ds, DS_OBJ_COUNT);
    for (uint16_t di = 0; di < n && di < 0x100; di += 2) {
        if (rd16(ds, (uint16_t)(di + OBJ_CODE_SEG)) == 0) continue;
        if (rd16(ds, (uint16_t)(di + OBJ_SUB_COUNT)) == 0) continue;
        const uint16_t s0 = (uint16_t)rd16(ds, (uint16_t)(di + OBJ_SUB_SLOT)), s1 = (uint16_t)rd16(ds, (uint16_t)(di + OBJ_SUB_END));
        if (slot >= s0 && slot < s1) return (int)di;
    }
    return -1;
}
// the history entry of object `own`, brought up to the snapshot's frame (once per composition:
// the stamp): a new frame — the whole position, the previous whole position or the fraction
// changed (an object slower than a pixel per frame moves its fraction alone) — shifts F to F_prev
// when the chain holds (the whole position last seen is this frame's X_PREV), else F_prev := F
MotionHist* mhist_touch(int own, const uint8_t* ds) {
    MotionHist* h = &g_mhist[(own >> 1) & 0x7F];
    if (h->stamp == g_mhist_comp) return h;
    h->stamp = g_mhist_comp;
    const int16_t W[2] = { rd16(ds, (uint16_t)(own + OBJ_WORLD_X)), rd16(ds, (uint16_t)(own + OBJ_WORLD_Y)) };
    const int16_t XP[2] = { rd16(ds, (uint16_t)(own + OBJ_X_PREV)), rd16(ds, (uint16_t)(own + OBJ_Y_PREV)) };
    const int F[2] = { ds[(uint16_t)(own + OBJ_FRAC_X)], ds[(uint16_t)(own + OBJ_FRAC_Y)] };   // the byte the integrator adds to
    if (!h->valid || h->W[0] != W[0] || h->W[1] != W[1] || h->XP[0] != XP[0] || h->XP[1] != XP[1] || h->F[0] != F[0] || h->F[1] != F[1]) {
        const bool chain = h->valid && h->W[0] == XP[0] && h->W[1] == XP[1];
        for (int a = 0; a < 2; a++) { h->Fprev[a] = chain ? h->F[a] : F[a]; h->W[a] = W[a]; h->XP[a] = XP[a]; h->F[a] = F[a]; }
        h->valid = true;
    }
    return h;
}
// the object's exact position at sub-frame r (render_v2.h MOTION EXACT): P(n-1) + r v / 3
double obj_exact(const MotionHist* h, int axis, int r) {
    const int rr = (r <= 0 || r > 3) ? 3 : r;
    const int d = (int)(int16_t)(h->W[axis] - h->XP[axis]);
    const double v = (double)d + (double)(h->F[axis] - h->Fprev[axis]) / 256.0;
    return (double)h->XP[axis] + (double)h->Fprev[axis] / 256.0 + (double)rr * v / 3.0;
}

// The presentation camera (render_v2.h CAMERA SMOOTH): its state, and the exact logical camera
// it is leashed to.
constexpr double CAM_TAU = 0.10;                      // s: the exponential approach to the target
// The leash around the exact logical camera. y: the dead zone's half-width (sub_1064b: 0x50..0x60) —
// the target is the logical camera itself there. x: the engine holds a running viking at the far
// edge of its dead zone plus its speed (the follow moves by the overshoot: an overshoot of o
// scrolls o px, so the viking sits at W/2 + 16 + v), while the centred presentation camera stands
// 16 + v ahead of it — 24 px at the top speed of 8 px per frame; with a leash of 16 the camera
// caught the leash at every walk start (the engine's camera waits for the overshoot) and stood
// two sub-frames still: a hitch. 32 keeps it slack at any speed and binds only during the
// engine's own pans, where P then follows L exactly.
constexpr int CAM_LEASH_X = 32, CAM_LEASH_Y = 8;
constexpr int CAM_SNAP = 64;                          // px: a jump of the logical camera beyond this resets the presentation camera
struct CamFrame { int frame; int end[2]; };
thread_local CamFrame g_cam_ring[4] = { { -1, { 0, 0 } }, { -1, { 0, 0 } }, { -1, { 0, 0 } }, { -1, { 0, 0 } } };   // the ends of the frames seen
thread_local bool g_cam_valid = false;
thread_local double g_cam_p[2] = { 0.0, 0.0 };
thread_local uint64_t g_cam_last = 0;
// the frame's pending scroll per axis from a snapshot's DS: the amount the follow set this frame
// (left/up wins over right/down, as sub_10704 reads them) and its sign; 0 when none or locked
int cam_pending_amt(const uint8_t* ds, int axis, int* sign) {
    *sign = 0;
    if (rd16(ds, axis ? DS_SCROLL_LOCK_Y : DS_SCROLL_LOCK_X)) return 0;
    const int neg = rd16(ds, axis ? DS_SCROLL_AMT_UP : DS_SCROLL_AMT_LEFT), pos = rd16(ds, axis ? DS_SCROLL_AMT_DOWN : DS_SCROLL_AMT_RIGHT);
    int amt = 0;
    if (neg) { amt = neg; *sign = -1; } else if (pos) { amt = pos; *sign = 1; }
    if (amt < 0 || amt > 16) { *sign = 0; return 0; }   // the follow clamps the amount to 0x10
    return amt;
}
// the clamp of the movers: right / down stop at the limit, left / up at 0
int cam_clamp(const uint8_t* ds, int axis, int v) {
    const int limit = (uint16_t)rd16(ds, axis ? DS_SCROLL_LIMIT_Y : DS_SCROLL_LIMIT_X);
    if (v >= limit) v = limit;
    if (v < 0) v = 0;
    return v;
}
// the camera the engine holds at the end of the frame of a snapshot, per axis: the flip's
// viewport V plus the steps the frame's amount still has to apply (step 1 before render2's flip,
// step 2 before render3's — sub_10704 / sub_10753 through the tables), clamped as the movers
// clamp; own = a co-op player's own camera (set once per frame: nothing pending)
int cam_frame_end(const uint8_t* ds, int axis, int r, int V, bool own) {
    if (own || r <= 0 || r >= 3) return V;
    int sign = 0; const int amt = cam_pending_amt(ds, axis, &sign);
    if (!amt) return V;
    int rest = 0;
    if (r < 2) rest += rd16(ds, (uint16_t)(DS_SCROLL_STEP1_TBL + amt * 2));
    rest += rd16(ds, (uint16_t)(DS_SCROLL_STEP2_TBL + amt * 2));
    return cam_clamp(ds, axis, V + sign * rest);
}
// ... and the whole movement of that frame (the three parts), for a frame whose predecessor was not seen
int cam_frame_total(const uint8_t* ds, int axis, bool own) {
    if (own) return 0;
    int sign = 0; const int amt = cam_pending_amt(ds, axis, &sign);
    if (!amt) return 0;
    return sign * (rd16(ds, (uint16_t)(DS_SCROLL_AMT_TBL + amt * 2)) + rd16(ds, (uint16_t)(DS_SCROLL_STEP1_TBL + amt * 2)) + rd16(ds, (uint16_t)(DS_SCROLL_STEP2_TBL + amt * 2)));
}
// the exact logical camera of a snapshot (render_v2.h): the line from the previous frame's end to
// this frame's end, sampled at the flip's sub-frame; the ring keeps the ends of the frames seen
// (the newest reconstruction of a frame wins)
void cam_exact(const Snap& S, bool own, int local, double out[2]) {
    const int V[2] = { own ? (int)(int16_t)S.cam_x[local] : (int)rd16(S.ds, DS_VIEWPORT_X), own ? (int)(int16_t)S.cam_y[local] : (int)rd16(S.ds, DS_VIEWPORT_Y) };
    int end[2], prev[2];
    for (int a = 0; a < 2; a++) end[a] = cam_frame_end(S.ds, a, S.subframe, V[a], own);
    CamFrame& e = g_cam_ring[S.frame & 3]; e.frame = S.frame; e.end[0] = end[0]; e.end[1] = end[1];
    const CamFrame& p = g_cam_ring[(S.frame - 1) & 3];
    if (p.frame == S.frame - 1) { prev[0] = p.end[0]; prev[1] = p.end[1]; }
    else for (int a = 0; a < 2; a++) prev[a] = cam_clamp(S.ds, a, end[a] - cam_frame_total(S.ds, a, own));   // not seen: the frame's own movement backwards
    const int r = (S.subframe <= 0 || S.subframe > 3) ? 3 : S.subframe;
    for (int a = 0; a < 2; a++) out[a] = (double)prev[a] + (double)r * (double)(end[a] - prev[a]) / 3.0;
}
}  // namespace

// One frame from a snapshot: see the note above. exact = MOTION EXACT (render_v2.h): the sprite
// commands at their exact positions; cam_smooth = CAMERA SMOOTH: the layers placed by the
// presentation camera (both the presenter only; the flip dump keeps the engine's). Returns
// whether the presentation camera is still on its way (the caller composes again next present).
static bool compose_snapshot(const Snap& C, const Snap* P, double t, bool interp, int local,
                             uint8_t* work, uint8_t* out, uint8_t* hud, V2DisplayBadge* badges, int* w, int* rows,
                             V2PresentLayers* L, int k, bool out_too, bool exact, bool cam_smooth) {
    memcpy(badges, C.badge, sizeof C.badge);
    if (L) L->k = 0;
    if (!C.tile_frame) {
        compose_page(C, work, out, hud);
        *w = 320; *rows = 0;
        return false;
    }
    if (!P) P = &C;
    memcpy(work, C.ds, DS_SIZE);      // the newest flip: sprite frames, UI, everything not interpolated
    // camera: viewport + the tile-scroll state derived from it (vp >> 3) —
    // the DS viewport, or (UX stage 8 step 2) the local player's own camera
    const bool own_cam = local > 0 && local < C.players && C.cam_valid[local];
    const bool own_valid = own_cam && P->cam_valid[local];
    int16_t cx = own_valid ? (int16_t)C.cam_x[local] : rd16(C.ds, DS_VIEWPORT_X);
    int16_t px = own_valid ? (int16_t)P->cam_x[local] : rd16(P->ds, DS_VIEWPORT_X);
    int16_t cy = own_valid ? (int16_t)C.cam_y[local] : rd16(C.ds, DS_VIEWPORT_Y);
    int16_t py = own_valid ? (int16_t)P->cam_y[local] : rd16(P->ds, DS_VIEWPORT_Y);
    if (own_cam && !own_valid) { cx = px = rd16(C.ds, DS_VIEWPORT_X); cy = py = rd16(C.ds, DS_VIEWPORT_Y); }
    // the exact interpolated camera (the sprites are rounded relative to it) and
    // the integer one written to the frame (the tiles scroll by it)
    double camx_f = cx, camy_f = cy;
    int16_t vx = cx, vy = cy;
    bool write = own_cam;
    if (interp && (cx != px || cy != py) &&
        cx - px <= V2_SMOOTH_MAX_STEP && px - cx <= V2_SMOOTH_MAX_STEP &&
        cy - py <= V2_SMOOTH_MAX_STEP && py - cy <= V2_SMOOTH_MAX_STEP) {
        camx_f = px + (double)(cx - px) * t; camy_f = py + (double)(cy - py) * t;
        vx = lerp16(px, cx, t); vy = lerp16(py, cy, t);
        write = true;
    }
    if (vx < 0) { vx = 0; camx_f = 0.0; }
    if (vy < 0) { vy = 0; camy_f = 0.0; }
    // the flat frame's camera (compose_map): the interpolated whole camera; the layers below
    // compose around the flip's own logical camera and place themselves by the exact one
    if (write) {
        wr16(work, DS_VIEWPORT_X, vx);
        wr16(work, DS_VIEWPORT_Y, vy);
        wr16(work, DS_SCROLL_COL, (int16_t)((uint16_t)vx >> 3));
        wr16(work, DS_SCROLL_ROW, (int16_t)((uint16_t)vy >> 3));
    }
    // The display list's commands, each moved back towards the previous flip by (1 - t):
    // the command's slot is looked up in the previous flip's list (its last record);
    // a slot present in both lists and within V2_SMOOTH_MAX_STEP is interpolated, any
    // other command (spawn, teleport, wrap, one flip only) keeps the newest position.
    // The world position moved back is rounded once relative to the exact camera so the
    // distance to the background is the rounded exact one (a camera-locked viking stays
    // still on the screen, as in the original; no ±1 px shimmer). At t = 1 every command
    // keeps its own position: the frame is the flip.
    static thread_local int16_t pos_x[V2_DRAWLIST_MAX], pos_y[V2_DRAWLIST_MAX];
    static thread_local int dev_x[V2_DRAWLIST_MAX], dev_y[V2_DRAWLIST_MAX];
    static thread_local double xfa[V2_DRAWLIST_MAX], yfa[V2_DRAWLIST_MAX];   // each command's exact world position for the layers
    // MOTION EXACT / CAMERA SMOOTH (render_v2.h): this composition's stamp for the object history, the trace
    if (exact || cam_smooth) g_mhist_comp++;
    static int mtrace = -1; if (mtrace < 0) mtrace = getenv("V2_MOTION_TRACE") ? 1 : 0;   // debug: the active viking's first sub-sprite per composition
    const uint16_t trace_slot = mtrace ? (uint16_t)rd16(C.ds, (uint16_t)(rd16(C.ds, DS_ACTIVE_VIKING) + OBJ_SUB_SLOT)) : 0xFFFF;
    for (int i = 0; i < C.draws.n; i++) {
        const V2DrawCmd& c = C.draws.cmd[i];
        pos_x[i] = c.x; pos_y[i] = c.y;
        double xf = c.x, yf = c.y;   // the command's exact world position: its own, or moved back below
        bool moved = false;
        // glyph cells and priority-tile repaints are map cells, not objects: they stay in the
        // world (moved only by the camera), their pseudo slots are no object record of the DS
        const bool sprite = c.type != V2_CMD_GLYPH && c.type != V2_CMD_FGTILE;
        // the page lists keep a slot's earlier commands while their residue lives: only the
        // slot's LAST command is the object as it stands, the earlier ones stay where they are
        bool last = sprite && (interp || exact);
        if (last) for (int j = i + 1; j < C.draws.n; j++) if (C.draws.cmd[j].slot == c.slot) { last = false; break; }
        // MOTION EXACT: the correction of this command's own position (the newest flip) — the
        // owner object's whole step, fraction and previous fraction (the history), the flip's sub-frame
        double dxC = 0.0, dyC = 0.0; int own = -1; MotionHist* h = nullptr;
        if (exact && last) {
            own = owner_of(C.ds, c.slot);
            if (own >= 0) {
                h = mhist_touch(own, C.ds);   // the object's frame in the history (once per composition)
                dxC = exact_delta(C.subframe, (int16_t)(h->W[0] - h->XP[0]), h->F[0], h->Fprev[0]);
                dyC = exact_delta(C.subframe, (int16_t)(h->W[1] - h->XP[1]), h->F[1], h->Fprev[1]);
                xf = c.x + dxC; yf = c.y + dyC;
                moved = dxC != 0.0 || dyC != 0.0;
            }
        }
        if (interp && last) {
            const V2DrawCmd* p = nullptr;
            for (int j = P->draws.n - 1; j >= 0; j--) if (P->draws.cmd[j].slot == c.slot) { p = &P->draws.cmd[j]; break; }
            if (p) {
                const int16_t ax = c.x, bx = p->x, ay = c.y, by = p->y;
                // MOTION EXACT: the previous flip's own correction — in the same frame of the object
                // (its whole and previous whole positions equal) the same F_prev; in an earlier frame
                // its own fraction stands in (at its frame's end the correction is F / 256 anyway)
                double dxP = 0.0, dyP = 0.0;
                if (own >= 0) {
                    const int16_t WP[2] = { rd16(P->ds, (uint16_t)(own + OBJ_WORLD_X)), rd16(P->ds, (uint16_t)(own + OBJ_WORLD_Y)) };
                    const int16_t XPP[2] = { rd16(P->ds, (uint16_t)(own + OBJ_X_PREV)), rd16(P->ds, (uint16_t)(own + OBJ_Y_PREV)) };
                    const int FP[2] = { P->ds[(uint16_t)(own + OBJ_FRAC_X)], P->ds[(uint16_t)(own + OBJ_FRAC_Y)] };
                    const bool same = WP[0] == h->W[0] && WP[1] == h->W[1] && XPP[0] == h->XP[0] && XPP[1] == h->XP[1];
                    dxP = exact_delta(P->subframe, (int16_t)(WP[0] - XPP[0]), FP[0], same ? h->Fprev[0] : FP[0]);
                    dyP = exact_delta(P->subframe, (int16_t)(WP[1] - XPP[1]), FP[1], same ? h->Fprev[1] : FP[1]);
                }
                const double axf = c.x + dxC, bxf = p->x + dxP, ayf = c.y + dyC, byf = p->y + dyP;
                if ((axf != bxf || ayf != byf) &&
                    !(ax - bx > V2_SMOOTH_MAX_STEP || bx - ax > V2_SMOOTH_MAX_STEP ||
                      ay - by > V2_SMOOTH_MAX_STEP || by - ay > V2_SMOOTH_MAX_STEP)) {   // else spawn / teleport / wrap
                    xf = bxf + (axf - bxf) * t; yf = byf + (ayf - byf) * t;
                    moved = true;
                }
            }
        }
        if (mtrace && c.slot == trace_slot && last) {
            if (own >= 0 && h)
                fprintf(stderr, "V2-MOTION f%d r%d t=%.2f slot=%02X own=%02X eng=%d,%d exact=%.3f,%.3f d=%d,%d F=%d,%d Fprev=%d,%d\n", v2_dbg_pre_vm_iter, C.subframe, interp ? t : 1.0,
                        c.slot, own, c.x, c.y, xf, yf, (int)(int16_t)(h->W[0] - h->XP[0]), (int)(int16_t)(h->W[1] - h->XP[1]), h->F[0], h->F[1], h->Fprev[0], h->Fprev[1]);
            else
                fprintf(stderr, "V2-MOTION f%d r%d t=%.2f slot=%02X eng=%d,%d exact=%.3f,%.3f (no owner)\n", v2_dbg_pre_vm_iter, C.subframe, interp ? t : 1.0, c.slot, c.x, c.y, xf, yf);
        }
        if (moved) {
            pos_x[i] = (int16_t)(vx + lround(xf - camx_f));
            pos_y[i] = (int16_t)(vy + lround(yf - camy_f));
            // the DS copy carries the same position (the V2_SMOOTH_DUMP lines read it)
            wr16(work, c.slot + OBJ_SPRITE_X, pos_x[i]);
            wr16(work, c.slot + OBJ_SPRITE_Y, pos_y[i]);
        }
        xfa[i] = xf; yfa[i] = yf;   // the layers place the command by this (below, once the presentation camera is known)
    }
    // parallax autoscroll accumulators (units of 1/1792 px): lerp unless wrapped
    uint32_t acc[2] = { C.par_acc_x, C.par_acc_y };
    if (interp) {
        if (C.par_acc_x >= P->par_acc_x && C.par_acc_x - P->par_acc_x < 1792u * 64u)
            acc[0] = P->par_acc_x + (uint32_t)((double)(C.par_acc_x - P->par_acc_x) * t);
        if (C.par_acc_y >= P->par_acc_y && C.par_acc_y - P->par_acc_y < 1792u * 64u)
            acc[1] = P->par_acc_y + (uint32_t)((double)(C.par_acc_y - P->par_acc_y) * t);
    }
    // debug: V2_SMOOTH_DUMP=<dir> — one line per interpolated frame: game frame,
    // fraction, the sub-frame period, the wall-clock since the newest flip, camera
    // prev/cur/lerp, the active viking's object X/Y and its first sub-sprite's X/Y
    // prev/cur/lerp
    if (interp) {
        static FILE* lf = nullptr; static int init = 0;
        if (!init) { init = 1; const char* dd = getenv("V2_SMOOTH_DUMP");
            if (dd && *dd) { char path[512]; snprintf(path, sizeof path, "%s/smooth_log.txt", dd); lf = fopen(path, "w"); } }
        if (lf) {
            const double freq = (double)SDL_GetPerformanceFrequency();
            const double period = (double)(C.t - P->t) / freq;
            const uint16_t vk = (uint16_t)rd16(C.ds, DS_ACTIVE_VIKING);
            const uint16_t sub = (uint16_t)rd16(C.ds, vk + OBJ_SUB_SLOT);
            fprintf(lf, "f%d t=%.3f period=%.1fms since=%.1fms vp %d,%d -> %d,%d = %d,%d | vik %02X obj %d,%d -> %d,%d | sub %02X x %d -> %d = %d y %d -> %d = %d\n",
                    v2_dbg_pre_vm_iter, t, period * 1000.0, (double)(SDL_GetPerformanceCounter() - C.t) / freq * 1000.0,
                    px, py, cx, cy, rd16(work, DS_VIEWPORT_X), rd16(work, DS_VIEWPORT_Y),
                    vk, rd16(P->ds, vk + OBJ_WORLD_X), rd16(P->ds, vk + OBJ_WORLD_Y), rd16(C.ds, vk + OBJ_WORLD_X), rd16(C.ds, vk + OBJ_WORLD_Y),
                    sub,
                    sub <= 0xFE ? rd16(P->ds, sub + OBJ_SPRITE_X) : 0, sub <= 0xFE ? rd16(C.ds, sub + OBJ_SPRITE_X) : 0, sub <= 0xFE ? rd16(work, sub + OBJ_SPRITE_X) : 0,
                    sub <= 0xFE ? rd16(P->ds, sub + OBJ_SPRITE_Y) : 0, sub <= 0xFE ? rd16(C.ds, sub + OBJ_SPRITE_Y) : 0, sub <= 0xFE ? rd16(work, sub + OBJ_SPRITE_Y) : 0);
        }
    }
    if (!L || out_too) {
        compose_map(C, pos_x, pos_y, acc, work, out);
        memcpy(hud, C.hud, sizeof C.hud);
    }
    bool cam_moving = false;
    if (L) {
        // the layers compose around the flip's own logical camera (a co-op player's own for
        // players 2..3): back into the working DS after the flat frame's interpolated one
        const int Vr[2] = { own_cam ? (int)(int16_t)C.cam_x[local] : (int)rd16(C.ds, DS_VIEWPORT_X), own_cam ? (int)(int16_t)C.cam_y[local] : (int)rd16(C.ds, DS_VIEWPORT_Y) };
        wr16(work, DS_VIEWPORT_X, (int16_t)Vr[0]); wr16(work, DS_VIEWPORT_Y, (int16_t)Vr[1]);
        wr16(work, DS_SCROLL_COL, (int16_t)((uint16_t)Vr[0] >> 3)); wr16(work, DS_SCROLL_ROW, (int16_t)((uint16_t)Vr[1] >> 3));
        // the shake fold of that camera: the sprite raster subtracts x_eff = viewport + shake
        // (v2_draw_list), so the device positions below are relative to the camera plus it
        int sdx = 0, sdy = 0;
        v2_camera_shake(work, &sdx, &sdy);
        // the presentation camera: ORIGINAL — the exact interpolated camera the flat frame rounds;
        // SMOOTH — the presenter's own (render_v2.h CAMERA)
        double pcam[2] = { camx_f, camy_f };
        if (cam_smooth) {
            double LC[2], Lt[2];
            cam_exact(C, own_cam, local, LC);
            Lt[0] = LC[0]; Lt[1] = LC[1];
            if (interp) { double LP[2]; cam_exact(*P, own_cam, local, LP); Lt[0] = LP[0] + (LC[0] - LP[0]) * t; Lt[1] = LP[1] + (LC[1] - LP[1]) * t; }
            // the target: x — the active viking's exact position centred, led by its velocity over the
            // approach time (a steady walk keeps it centred); y — the exact logical camera; the
            // logical camera itself when it is locked or there is no viking; inside the level's limits
            const uint16_t vk = own_cam ? C.coop_active[local] : (uint16_t)rd16(C.ds, DS_ACTIVE_VIKING);
            double T[2] = { Lt[0], Lt[1] };
            const bool has_vk = vk <= 0xFE && (vk & 1) == 0 && rd16(C.ds, (uint16_t)(vk + OBJ_CODE_SEG)) != 0;
            // Interactive play only: the camera the presenter leads and leashes is the player's.
            // A scene — the attract / intro / transition mode word 0x8000 (the vikings walk on a
            // demo stream), an LVX scene slot (the Genesis interludes: pinned camera, full screen,
            // the trio choreography), the PC's own scene slots 37..47 (logos, title, the intro
            // ship, the finale) — is the script's: its camera pans are part of the choreography,
            // so the presentation camera IS the exact logical camera there (2026-09-18: with the
            // leash the intro's vikings walked up to 32 px off their scripted screen positions).
            const uint16_t level = (uint16_t)rd16(C.ds, DS_LEVEL);
            const bool interactive = rd16(C.ds, DS_GAME_MODE_AC) != 0x8000 &&
                                     !(v2_lvx_flags(level) & LVX_SCENE_FLAGS) &&
                                     !(level >= 37 && level < 48);
            if (has_vk && interactive && !rd16(C.ds, DS_SCROLL_LOCK_X)) {
                const MotionHist* hv = mhist_touch((int)vk, C.ds);
                const double ox = obj_exact(hv, 0, C.subframe);
                const double vpf = (double)(int16_t)(hv->W[0] - hv->XP[0]) + (double)(hv->F[0] - hv->Fprev[0]) / 256.0;   // px per game frame
                T[0] = ox - (double)C.w / 2.0 + vpf * 20.0 * CAM_TAU;
            }
            for (int a = 0; a < 2; a++) { const double lim = (double)(uint16_t)rd16(C.ds, a ? DS_SCROLL_LIMIT_Y : DS_SCROLL_LIMIT_X); if (T[a] < 0.0) T[a] = 0.0; if (T[a] > lim) T[a] = lim; }
            // the step on the presenter's clock, the reset on a jump of the logical camera
            const uint64_t now = SDL_GetPerformanceCounter();
            double dt = g_cam_valid ? (double)(now - g_cam_last) / (double)SDL_GetPerformanceFrequency() : 0.0;
            if (dt < 0.0) dt = 0.0; if (dt > 0.05) dt = 0.05;
            g_cam_last = now;
            if (!interactive) { g_cam_p[0] = Lt[0]; g_cam_p[1] = Lt[1]; g_cam_valid = true; }   // a scene: the logical camera, no approach, no leash
            else if (!g_cam_valid || fabs(Lt[0] - g_cam_p[0]) > CAM_SNAP || fabs(Lt[1] - g_cam_p[1]) > CAM_SNAP) { g_cam_p[0] = T[0]; g_cam_p[1] = T[1]; g_cam_valid = true; }
            else { const double a = 1.0 - exp(-dt / CAM_TAU); g_cam_p[0] += (T[0] - g_cam_p[0]) * a; g_cam_p[1] += (T[1] - g_cam_p[1]) * a; }
            // the leash to the exact logical camera (the dead zone's half-widths), the level's limits
            int leash = 0;
            if (g_cam_p[0] < Lt[0] - CAM_LEASH_X) { g_cam_p[0] = Lt[0] - CAM_LEASH_X; leash |= 1; }
            if (g_cam_p[0] > Lt[0] + CAM_LEASH_X) { g_cam_p[0] = Lt[0] + CAM_LEASH_X; leash |= 1; }
            if (g_cam_p[1] < Lt[1] - CAM_LEASH_Y) { g_cam_p[1] = Lt[1] - CAM_LEASH_Y; leash |= 2; }
            if (g_cam_p[1] > Lt[1] + CAM_LEASH_Y) { g_cam_p[1] = Lt[1] + CAM_LEASH_Y; leash |= 2; }
            for (int a = 0; a < 2; a++) { const double lim = (double)(uint16_t)rd16(C.ds, a ? DS_SCROLL_LIMIT_Y : DS_SCROLL_LIMIT_X); if (g_cam_p[a] < 0.0) g_cam_p[a] = 0.0; if (g_cam_p[a] > lim) g_cam_p[a] = lim; }
            pcam[0] = g_cam_p[0]; pcam[1] = g_cam_p[1];
            cam_moving = fabs(T[0] - pcam[0]) > 0.25 / (double)k || fabs(T[1] - pcam[1]) > 0.25 / (double)k;
            // debug: V2_CAMERA_TRACE=1 — one line per composition: the flip's viewport, the exact
            // logical camera, the target, the presentation camera, the leash, the step's dt
            static int ctrace = -1; if (ctrace < 0) ctrace = getenv("V2_CAMERA_TRACE") ? 1 : 0;
            if (ctrace) fprintf(stderr, "V2-CAMERA f%d r%d t=%.2f V=%d,%d L=%.3f,%.3f T=%.3f,%.3f P=%.3f,%.3f leash=%d dt=%.1fms\n", C.frame, C.subframe, interp ? t : 1.0,
                                Vr[0], Vr[1], Lt[0], Lt[1], T[0], T[1], pcam[0], pcam[1], leash, dt * 1000.0);
        }
        // the device positions: the distance to the presentation camera rounded once at k x (a
        // camera-locked sprite stays still, as on the flat frame); a command with a whole world
        // position lands exactly where the shifted background puts that point
        for (int i = 0; i < C.draws.n; i++) {
            dev_x[i] = (int)lround((xfa[i] - pcam[0]) * (double)k) - sdx * k;
            dev_y[i] = (int)lround((yfa[i] - pcam[1]) * (double)k) - sdy * k;
        }
        compose_layers(C, dev_x, dev_y, acc, k, pcam, Vr, work, hud, badges, L);
    }
    *w = C.w;
    *rows = C.rows > 176 ? C.rows : 0;
    return cam_moving;
}

// Presenter thread (render_v2.h): the frame to show now — the newest snapshot, or under SMOOTH
// an interpolation between the two newest by the wall-clock fraction of their period. The two
// snapshots are pinned while they are read (the game thread never fills a pinned slot) and
// released before returning; the frame lives in this function's own buffers. A frame that is
// not interpolated and whose snapshot has not changed since the last call is not composed
// again (the menu, a wait: the same flip presented refresh after refresh).
bool v2_present_compose(V2PresentFrame* out) {
    v2_options_ensure_loaded();
    static uint8_t s_work[DS_SIZE], s_out[V2_FB_MAX_W * 240], s_hud[320 * 64];
    static V2DisplayBadge s_badges[3];
    static V2PresentLayers s_L;
    static int s_w = 320, s_rows = 0;
    static uint32_t s_seq = 0; static bool s_interp = true, s_have = false; static int s_k = -1;
    // the stand V2_KX_SELFTEST: the flat frame composed beside the layers (render_v2.cpp compares the two)
    static int kx_test = -1; if (kx_test < 0) kx_test = getenv("V2_KX_SELFTEST") ? 1 : 0;
    const int local = g_v2_local_player;   // UX stage 8 step 2: a client's own camera (players 2..3)
    const bool smooth_on = smooth_wanted();
    int ci = -1, pi = -1;
    {
        std::lock_guard<std::mutex> lock(g_mx);
        const bool cur_ok = g_cur_i >= 0 && g_pool[g_cur_i].valid;
        const bool prev_ok = g_prev_i >= 0 && g_pool[g_prev_i].valid;
        if (!cur_ok) { g_effective = false; v2_smooth_last_reason = 2; return false; }
        ci = g_cur_i; pi = prev_ok ? g_prev_i : g_cur_i;
        g_pin_c = ci; g_pin_p = pi;
    }
    struct Unpin { ~Unpin() { std::lock_guard<std::mutex> lock(g_mx); g_pin_c = -1; g_pin_p = -1; } } unpin;
    const Snap& C = g_pool[ci];
    const Snap& P = g_pool[pi];
    double t = 1.0;
    bool interp = false;
    if (smooth_on && pi != ci) {
        // a sub-frame pair: the same level and width, both tile frames, 6..40 ms apart
        // (the DOS game flips once per game frame on some screens: 50 ms, not a sub-frame)
        const double freq = (double)SDL_GetPerformanceFrequency();
        const double period = (double)(C.t - P.t) / freq;               // s between the two newest flips
        const bool pair = C.tile_frame && P.tile_frame && C.fullscreen == P.fullscreen && C.w == P.w &&
                          rd16(C.ds, DS_LEVEL) == rd16(P.ds, DS_LEVEL) && period >= 0.006 && period <= 0.040;
        if (pair) {
            t = (double)(SDL_GetPerformanceCounter() - C.t) / freq / period;
            if (t < 0.0) t = 0.0;
            if (t > 1.0) t = 1.0;
            interp = true;
        }
    }
    // the sub-pixel presentation (render_v2.h V2PresentLayers): the window's integer scale for
    // this frame's width — 0 = the flat frame (the option off, a chunk screen, no renderer)
    const int k = (v2_options.subpixel.load() && C.tile_frame) ? v2_present_scale_k(C.w) : 0;
    const bool exact = v2_options.motion.load() == 1;   // MOTION EXACT (render_v2.h)
    const int cam_mode = v2_options.camera.load();      // CAMERA SMOOTH (render_v2.h): the layers only
    const bool cam_smooth = cam_mode == 1 && k > 0;
    static bool s_exact = false, s_cam_moving = false; static int s_cam = -1;
    // (a presentation camera still on its way composes again although nothing else changed)
    if (!(s_have && !interp && !s_interp && C.seq == s_seq && k == s_k && exact == s_exact && cam_mode == s_cam && !s_cam_moving)) {
        s_cam_moving = compose_snapshot(C, interp ? &P : nullptr, t, interp, local, s_work, s_out, s_hud, s_badges, &s_w, &s_rows,
                                        k > 0 ? &s_L : nullptr, k, kx_test != 0, exact, cam_smooth);
        s_seq = C.seq; s_interp = interp; s_have = true; s_k = k; s_exact = exact; s_cam = cam_mode;
    }
    const bool layers = k > 0 && s_L.k > 0;
    g_effective = interp;
    v2_smooth_last_t = (float)t;
    v2_smooth_last_w = s_w;
    v2_smooth_last_reason = 0;
    v2_stats.kx.store(layers ? k : 0, std::memory_order_relaxed);
    out->map = (layers && !kx_test) ? nullptr : s_out; out->w = s_w; out->rows = s_rows; out->hud = s_hud; out->badges = s_badges; out->smooth = interp;
    out->layers = layers ? &s_L : nullptr;
    return true;
}

// Game thread, right after v2_smooth_capture (the V2_FLIP_DUMP of v2_swap_render_buf): the
// newest snapshot composed at t = 1 with the DS camera into the caller's buffers — the frame
// the presenter shows for this flip, before any interpolation
bool v2_flip_frame_for_dump(uint8_t* map, uint8_t* hud, V2DisplayBadge* badges, int* w, int* rows) {
    static uint8_t work[DS_SIZE];
    int ci;
    { std::lock_guard<std::mutex> lock(g_mx); ci = g_cur_i; }
    if (ci < 0 || !g_pool[ci].valid) return false;
    compose_snapshot(g_pool[ci], nullptr, 1.0, false, -1, work, map, hud, badges, w, rows, nullptr, 0, false, false, false);   // the engine's positions and camera: the oracle against the test build's page
    return true;
}
