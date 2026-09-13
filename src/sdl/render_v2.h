#ifndef RENDER_V2_H
#define RENDER_V2_H

#include <cstdint>
#include <atomic>
#include <mutex>
#include <SDL2/SDL.h>

// When defined, v2 renderer reads DS data (viewport, objects, flags, scroll)
// from v2 VM's shadow DS instead of real DS. This makes rendering independent
// from the original VM — v2 VM's writes to shadow are what gets rendered.
// Tile map and tile/sprite graphics segments remain shared (read-only level data).
#ifndef V2_RENDER_FROM_SHADOW
#define V2_RENDER_FROM_SHADOW
#endif

// ============================================================================
// Заголовочный файл для второго окна (render_v2)
// ============================================================================

// Структура данных отрисовки (дубликат для v2)
struct myDrawInfoS_v2
{
  uint8_t drawBuffer[65536*4];   // не используется напрямую
  uint8_t stableBuffer[65536*4]; // рендер-поток: линейный буфер 344x240
  SDL_Color drawPalette[256];
  uint32_t myOffset;
  uint8_t myPixelOffset;
};

// Pointer to emulated memory base (m2c::m), set once at startup
extern uint8_t* v2_m2c_base;

// Size of the static EXE image (m2c::m[0..0x29F00]) captured in exe_static.bin.
// In V2_ONLY, addresses below this hold valid snapshot data (seg001 text,
// CS tables); addresses above are dynamic segments and stay zero.
constexpr uint32_t V2_EXE_STATIC_SIZE = 0x29F00;

// V2 rendering buffer — single persistent buffer, like drawBuffer in the original.
// Game thread writes, v2_swap_render_buf copies to v2_display_buf for render thread.
// Linear format: [y * 320 + x] = palette index, 320x200
// UX stage 9, step 4 (wide screen): the frame buffers hold up to V2_FB_MAX_W
// columns; the live width is v2_fbw (per thread: the game thread's frame,
// the presenter's interpolated frame) and v2_display_w is the width the last
// published frame was rendered at. 320 on every canonical level and on every
// chunk screen (title, menus, intro); a wide level renders v2_view_w columns.
#define V2_FB_MAX_W 512
extern uint8_t  v2_render_buf[V2_FB_MAX_W*240];   // UX stage 9: 240 rows (224 shown on an LVX_TALL224 level)

// Display buffer — game copies completed frame here under lock,
// render thread reads it under the same lock. No race.
extern uint8_t  v2_display_buf[V2_FB_MAX_W*240];
extern thread_local int v2_fbw;   // the width (= stride) of the frame this thread is rendering
extern int v2_display_w;          // width of the published v2_display_buf frame (under v2_display_mutex)
extern int v2_view_w;             // v2_vm.cpp: the loaded level's view width (0x140, or the WIDE option's, clamped to the map)
// UX stage 0: published with v2_display_buf under v2_display_mutex — 1 when the
// running slot is an LVX full-screen scene: the presenter shows display rows
// 176..199 (map) instead of the HUD band.
extern int      v2_display_fullscreen;
extern std::mutex v2_display_mutex;

// HUD buffer — rendered independently from game data, 320x64
// Screen rows 176-239 (VGA split screen: always from VGA address 0)
extern uint8_t  v2_hud_buf[320*64];
// UX stage 8 step 2 (co-op): per viking portrait, the player holding that
// viking (0-based, -1 = nobody / one-player game) and the portrait's HUD
// position — published at the swap under v2_display_mutex.
struct V2DisplayBadge { int x, y, owner; };
extern V2DisplayBadge v2_display_badge[3];

// UX stage 2: the SNES parallax layer (display lane only — never touches DS,
// the shadow VGA or the canon replays). Loaded per level from the head copy
// in DS (0x25EC map chunk, 0x25EE tile chunk, 0x25F2/0x25F4 = fx/fy), see
// tools/assets/parallax_snes.py for the chunk formats and the measured model:
//   par = (cam * f) >> 8 per axis (8.8), bit 15 = autoscroll at f/256 px per
//   console frame — kept time-true at the 70 Hz tick: acc += f*6 per tick,
//   px = acc / 1792 (= f/256 * 60/70).
struct V2ParallaxLayer {
    bool     on;        // level has a layer and V2_PARALLAX != 0
    uint16_t w, h;      // map size in 8x8 tiles
    uint16_t fx, fy;    // head +0x3F / +0x41
    uint32_t acc_x, acc_y;   // autoscroll accumulators, units of 1/1792 px
    uint32_t off_x, off_y;   // phase offsets in px: optional map trailer [off_x u16][off_y u16]
                             // after the cells (the Genesis scenes' self-driven plane B starts at
                             // the console's own phase; 0 when the trailer is absent)
    uint32_t ntiles;
    const uint8_t*  tiles;   // ntiles x 64 pixels (nibbles, 0 = transparent)
    const uint16_t* map;     // w*h cells: idx | pal<<10 | prio<<13 | hf<<14 | vf<<15
};
extern V2ParallaxLayer v2_parallax;

// UX stage 9: presenter-side interpolation (v2_smooth.cpp). The render passes
// read the DS/FS images and paint the buffer named by these thread-locals;
// on the game thread they stay null (= the shadow DS / v2_render_buf), the
// presenter thread points them at its interpolated snapshot and its own
// composition buffer, so both threads can run the same passes at once.
extern thread_local const uint8_t*  v2_tls_ds;
extern thread_local uint8_t*        v2_tls_out;
extern thread_local const uint8_t*  v2_tls_fs;
extern thread_local const uint32_t* v2_tls_par_acc;   // {acc_x, acc_y} of the parallax autoscroll
extern thread_local bool            v2_tls_presenter; // passes run outside the VM frame gate
extern "C" int v2_view_rows(void);           // v2_vm.cpp: 176 / 200 (LVX scene) / 224 (LVX_TALL224 level)
void v2_smooth_capture(void);                 // game thread, at the page flip
bool v2_smooth_render(uint8_t* out);          // presenter: true = out (320x200) holds an interpolated frame
bool v2_smooth_effective(void);               // the presenter is interpolating between the sub-frames right now (menu)
uint32_t v2_smooth_subframe_seq(void);        // distinct sub-frames flipped so far (STATS)
uint64_t v2_smooth_last_flip_ticks(void);     // SDL_GetPerformanceCounter at the newest flip
void v2_flip_notify(void);                    // game thread, after the flip: the VRR presenter waits on it
// the vsync lock (render_v2.cpp, 2026-09-11): the game's vsync waits follow the display's refreshes
bool   v2_vsync_wait_game(void);              // game thread: block until the next game vsync; false = no lock, pace yourself
bool   v2_vsync_locked(void);
double v2_vsync_display_hz(void);             // the refresh the schedule runs on (0 = unknown / no lock)
bool   v2_vsync_auto_smooth(void);            // SMOOTH AUTO's decision: the refresh is no multiple of 60
extern float v2_smooth_last_t;                // debug: fraction of the last interpolated frame

// UX stage 6 phase 2: a text item of a CJK language bank — a box's UTF-8 text
// (or a raw YES/NO word) that v2_draw_ui paints with the bank's Unifont 16x16
// glyphs over the cells loc_124c5 filled with spaces. Lives while its first
// cell still holds that space (the box close / sub_12816 zero the cells).
struct V2TextItem { uint8_t on, raw; uint16_t col, row; char utf8[400]; };
extern V2TextItem v2_text_items[8];
const uint8_t* v2_lang_wide_glyph(uint32_t cp, uint8_t* w);   // 16 rows u16 LE, bit 15 = left; null = none


// Глобальные переменные (extern)
extern struct myDrawInfoS_v2* myDrawInfo_v2;
extern uint16_t input_keys_v2;
extern bool need_quit_v2;

// Функции API
extern void render_init_v2(void* state);
extern void render_callback_v2(void* state);

// V2 rendering functions — called from seg000 and v2 VM.
// With V2_RENDER_FROM_SHADOW: seg000 calls are skipped (v2 VM handles rendering).
extern void v2_draw_tiles(uint16_t ds_val);
extern void v2_draw_sprites(uint16_t ds_val);
extern void v2_draw_flagged_tiles(uint16_t ds_val);
extern void v2_draw_ui(uint16_t ds_val);
extern void v2_swap_render_buf();
extern void v2_set_m2c_base(void* base);

#ifdef V2_RENDER_FROM_SHADOW
// When true, v2 VM is executing its frame — rendering calls proceed.
// When false (seg000 hooks), rendering calls are skipped.
extern bool v2_vm_in_frame;
#endif

// V2 game loop — runs in separate thread, synchronized with original via barriers.
// Each phase runs in parallel with the original's corresponding phase.
// seg000 calls v2_signal_phase() at each eip to trigger the v2 phase + wait for completion.
enum V2Phase {
    V2_PHASE_FRAME_BEGIN = 0,  // level check + state init (eip 0x001E)
    V2_PHASE_PRE_VM,           // sub_12352..sub_1673c (eip 0x001E..0x0036)
    V2_PHASE_VM,               // sub_14207 (eip 0x0039)
    V2_PHASE_POST_VM,          // sub_1386b..sub_1064b (eip 0x003C..0x004E)
    V2_PHASE_RENDER1,          // render + rotation1 + pageflip1 (eip 0x0051..0x0071)
    V2_PHASE_POST_FLIP1,       // sub_12e16..sub_12d2c (eip 0x0074..0x0083)
    V2_PHASE_RENDER2,          // rotation2 + pageflip2 (eip 0x0086..0x00A6)
    V2_PHASE_POST_FLIP2,       // sub_10753..sub_101be (eip 0x00A9..0x00B8)
    V2_PHASE_RENDER3,          // rotation3 + pageflip3 (eip 0x00BB..0x00D8)
    V2_PHASE_AUDIO_TICK,       // word_30C14=0 + sub_108c8 (eip 0x00DB..0x00E1) —
                               // №59: signaled BEFORE sub_1086f so the 108c8
                               // mirror runs in orig order (it used to ride
                               // POST_FLIP3, i.e. after the 1086f flip+vsync)
    V2_PHASE_POST_FLIP3,       // sub_10350 + sub_1086f epilogue (eip 0x00E4..0x00E7)
    V2_PHASE_FRAME_END,        // verify + cleanup

    // === Blocking phases (signaled around orig blocking loops) ===
    // Each maps 1:1 to specific v2 mirror function. orig signals BEFORE entering
    // its blocking loop, v2 dispatcher invokes corresponding mirror which has
    // the FULL blocking loop logic. Both threads spin reading input in parallel,
    // both exit when input matches. v2_render_buf shows v2's render (with palette
    // anim, dialog text, etc). In V2_ONLY same mirror functions called inline.
    V2_PHASE_VIKING_SWITCH_LOOP,  // sub_10138 loc_10169 — viking switch screen
    V2_PHASE_PAUSE_ENTRY,         // sub_11ba5 BEFORE loc_11c1f (eip 0x1bbd..0x1c1c) —
                                  // sets word_28925=0x11, word_28927=1 (mode=carrying),
                                  // sprite mode 2, cursor pos, sub_11f47 proximity.
                                  // Without it, v2 starts pause loop in mode 0 while
                                  // orig is in mode 1 → diverging item handler paths.
    V2_PHASE_PAUSE_LOOP,          // sub_11ba5 loc_11c1f — TAB pause
    V2_PHASE_TRANSITION_TEXT,     // sub_104A1 loc_104C3 — quit-prompt iter body
    V2_PHASE_PASSWORD_PROMPT,     // sub_1041c — password input
    V2_PHASE_PRE_SUB_1086F,       // BEFORE orig sub_1086f at eip 0xE7 — v2 mirror
                                  // processes shadow cmd buffer first so orig's
                                  // m2c v2_draw_ui inside sub_1086f reads fresh
                                  // shadow glyph buffer (dialog text).
    V2_PHASE_PW_ENTRY,            // orig signals at sub_104A1 entry (line 2706)
                                  // BEFORE pre-loop setup (sub_1450b/sub_1047c
                                  // prelude, palette OUTs). v2 runs equivalent
                                  // shadow setup so loop iters start aligned.
    V2_PHASE_PW_EXIT,             // orig signals at sub_104A1 loc_104FF (line 2748)
                                  // AFTER loop exit + post-loop sub_12352 + word_28814
                                  // toggle. v2 runs equivalent shadow cleanup
                                  // (sub_165aa/sub_1DD9C/sub_1C8F1/sub_1E0C7/sub_16775
                                  // pair + sub_12816 glyph clear).
    V2_PHASE_INPUT_UPDATE,        // orig signals from INSIDE sub_12352 (after ax is
                                  // computed and v2_input_snapshot written, before
                                  // word_28896/28898/2889A get set). v2 handler runs
                                  // v2_read_input_12352_iter ONCE per orig sub_12352 call,
                                  // so shadow input state tracks orig 1:1 across all
                                  // call sites (main loop + sub_1086f recursion +
                                  // VIKING_SWITCH/TRANSITION_TEXT/PAUSE_LOOP iters).
                                  // Without this signal, orig calls sub_12352 multiple
                                  // times per frame (sub_1086f recursion etc.) but v2
                                  // only mirrors at PRE_VM → shadow_28896/2889A lag,
                                  // PSNAP-DIVERGE at 0x03B6/0x03B8/0x03BA.
};
extern void v2_signal_phase(V2Phase phase, uint16_t ds_val);  // signal v2 thread + wait
extern void v2_game_thread_start();   // launch v2 thread (called once at startup)
extern void v2_game_thread_stop();    // stop v2 thread
extern void v2_run_animation_vm(uint16_t ds_val);  // legacy: full frame (used during init)

// V2 VM shadow DS — 64KB copy of DS segment, written by v2 VM opcodes.
// Used by v2 renderer when V2_RENDER_FROM_SHADOW is defined.
extern uint8_t* v2_vm_get_shadow_ds();

// Real DS pointer (nullptr in V2_ONLY / before first frame) — gate for
// real-vs-shadow diagnostic compares.
extern uint8_t* v2_vm_get_real_ds();

// Sync shadow DS from real DS before rendering. Call right before v2_draw_tiles.
extern void v2_vm_sync_for_render();

// Mutex to synchronize render callback DS writes with replay verify DS snapshots.
// Render callback (sub_1797b) DECs word_3287C in DS from render thread.
// Replay verify snapshots DS before/after each opcode in game thread.
// Without lock: race condition causes spurious verify diffs.
extern std::mutex v2_ds_modify_mutex;


// V2 VM shadow tile map — 64KB copy of tile map segment, written by v2 VM.
extern uint8_t* v2_vm_get_shadow_tilemap();
extern bool v2_vm_is_tilemap_shadow_valid();

// V2 VM shadow tile graphics — 64KB copy of tile graphics segment.
extern uint8_t* v2_vm_get_shadow_tilegfx();
extern bool v2_vm_is_tilegfx_shadow_valid();

// V2 VM shadow animation data (ds:0x2E67)
extern uint8_t* v2_vm_get_shadow_animdata();
extern bool v2_vm_is_animdata_shadow_valid();
// V2 VM shadow GS segment (ds:0x2E61, tile masks)
extern uint8_t* v2_vm_get_shadow_gs();
extern bool v2_vm_is_gs_shadow_valid();
// V2 VM shadow sound data (ds:0x2E6B)
extern uint8_t* v2_vm_get_shadow_sound();
extern bool v2_vm_is_sound_shadow_valid();
// V2 VM shadow chunk buffer (ds:0x2E77)
extern uint8_t* v2_vm_get_shadow_chunk();
extern bool v2_vm_is_chunk_shadow_valid();
// V2 VM shadow sprite data — 256KB buffer for decompressed sprites.
// Returns pointer to shadow sprite data if the address falls in shadow range, else nullptr.
extern uint8_t* v2_vm_get_shadow_sprite(uint32_t linear_addr);

extern void v2_draw_hud_background(uint16_t ds_val, uint16_t chunk_seg, uint16_t plane_size);
extern void v2_draw_viewport_chunk(uint16_t chunk_seg, uint16_t plane_size);
// Second (late) sprite layer at the orig sub_1dd9c point: repaints ONLY the
// objects orig's sub_1dd9c would draw — force flag ds:0x9568, pending redraw
// counter byte [obj+0x114D], or the sub_1cdef gate (any of the object's tile
// cells carries render-map bit0, i.e. will be repainted by this sub-frame's
// sub_1c8f1). Everything else keeps its early-layer (sub_1de05-point) pixels
// — that is exactly what the orig page shows (task #21: the lift kept the
// pre-165aa position on scroll frames while flames took the post-update one).
extern void v2_draw_sprites_late(uint16_t ds_val);

// The display list of a sub-frame (2026-09-11): one record per object the sprite layers
// drew, in draw order — the early layer (every active object at the sub_1de05 point) and
// the late layer (exactly the objects the sub_1dd9c mirror dispatched, in its order; the
// mirror reports them through v2_late_list_begin/add and v2_draw_sprites_late consumes
// that list instead of re-deriving the pass's decisions from the state after the pass).
// The presenter composes its frames from this list — the same commands, the same order,
// only the positions shifted for the interpolation — never from a re-derivation of what
// the passes decided (the late set is a function of the pass, not of the DS after it).
struct V2DrawCmd {
    uint16_t slot;      // object slot (di)
    uint16_t flags;     // [slot+0x44D] at draw time (type bits 0-2, hflip bit 9)
    int16_t  x, y;      // world position drawn ([slot+0x64D]/[0x74D])
    uint16_t seg, off;  // sprite data segment / 1-based offset
    uint16_t strips;    // [slot+0xC4D] (type 2: strips per plane)
    uint8_t  late;      // 0 = early layer, 1 = late layer
    uint8_t  type;      // flags & 7
    uint8_t  mand;      // the handler's column-clip mask ANDed into every strip mask (0xFF = no clip;
                        // the 1379/138B (type 2) and 0E92/0E9A (type 4) table byte of the draw's side)
    uint8_t  clip_top;  // strips (type 2: rows, type 4: 2-row units) the handler skipped at the top
    uint8_t  clip_bot;  // ... and at the bottom — the sprite's window clip at draw time
    uint32_t epoch;     // page lists: the draw's place in the page's erase order (v2_page_list_draw)
    uint32_t data_off;  // page lists: the record's strip data in the list's arena (data_len 0 = read the live bank)
    uint16_t data_len;
    uint64_t keep[4];   // page lists: the cells this record may show at all — all of them for a draw,
                        // the copied span's live cells for a span copy (v2_page_cells_copy); same
                        // layout as dead
    uint64_t dead[4];   // page lists: the sprite's cells the page has since restored from the background,
                        // relative to the sprite's top-left cell (8 columns x 32 rows: word row >> 3,
                        // bit (row & 7) * 8 + column) — baked by v2_compose_page for the composed list
                        // (a cell outside keep is dead too)
};
// type codes beyond the sprite types 1/2/4: a glyph cell of the text plane (sub_1E0C7 painted
// it at a map cell: x/y = the cell's world position, off = the glyph index, slot = 0xFFFF)
#define V2_CMD_GLYPH 0x80
// ... and a flagged (priority) tile sub_1C8F1 repainted over the sprites of a dirty cell
// (x/y = the cell's world position, off = the tile word, slot = 0xFFFE): the original
// repaints such a tile only where a pass dirtied the cell, so the frame carries exactly
// those repaints, in their order among the sprites and glyphs
#define V2_CMD_FGTILE 0x81
// a page can hold a whole text box (up to 40 x 22 glyph cells) on top of its sprites
#define V2_DRAWLIST_MAX 1024
// The pixels of a command are the sprite bytes AS THEY WERE at draw time: the page keeps
// what was painted, the sprite banks may be reloaded underneath (a level or scene change
// re-fills the segments while the old pages are still shown — the vikings would change
// pose, or turn to garbage). Every record carries a copy of its strip data in the list's
// arena (type 1: 72 bytes, type 4: 288, type 2: 36 x strips, a glyph 72); the raster reads
// the copy. Lists are copied with v2_drawlist_copy (records + the used arena only).
#define V2_DRAWLIST_ARENA (768 * 1024)
struct V2DrawList { int n; uint32_t arena_used; V2DrawCmd cmd[V2_DRAWLIST_MAX]; uint8_t arena[V2_DRAWLIST_ARENA]; };
extern void v2_drawlist_copy(V2DrawList& dst, const V2DrawList& src);
extern V2DrawList v2_frame_draws;                  // the game thread's list of the sub-frame being composed
extern void v2_late_list_begin(void);              // sub_1dd9c mirror: a pass starts (the late set is being decided)
extern void v2_late_list_add(uint16_t slot);       // sub_1dd9c mirror: this object's type handler ran
// Draw the list's commands in order, command i at world position (pos_x[i], pos_y[i])
// (the caller's interpolated positions; the command's own x/y when nothing moves).
// which: 0 = the sprite commands only, 1 = the glyph commands only, -1 = every command
extern void v2_draw_list(const V2DrawList& L, const int16_t* pos_x, const int16_t* pos_y, int which = -1);

// The PAGE lists (2026-09-11): what each of the orig's three VGA pages (roles 0 / 0x34 /
// 0x68, ds:92F7/92F9/92FB) currently holds in sprites — every draw command painted on it,
// in draw order, each minus the CELLS the page has restored from the background since.
// The orig erases by cell, never by object: sub_1DE05 pass 1 marks the OLD rect of an object
// with [114E] != 0 (fs OR 3), the sprite handlers mark their cells at draw time, sub_1E16D
// marks the glyph cells, and pass 2 of the next sub_1DE05 copies the background page over
// EVERY bit-0 cell of the window on page [92F7] (sub_1C8F1 clears bit 0 at the end of each
// pass). So a frame change whose OLD latch lags leaves the previous frame's pixels under the
// new one, an object drawn without an erase count stays where it was, a box erases the
// sprites under it and the scan repaints them — all of it per cell. Maintained by the pass
// mirrors alone:
//   sub_1DE05 pass 2 (v2_bg_latch_1DE05): each restored span on page [92F7] →
//     v2_page_list_erase_cells(page, fs offset of the first cell, count): the cells' erase
//     epoch advances; a command drawn before it loses those cells (its dead mask);
//   sub_1DD9C (v2_late_sprites_1DD9C): every object it dispatches is drawn on page [92F9]
//     → v2_page_list_draw(page, cmd): appended with the next epoch, the slot's earlier
//     commands stay (their surviving pixels are the orig's residue) until their cells die;
//   sub_1DF6A part 2 (page rotation): the new background page [92FB] gets the clean
//     background copied into every cell with render-map bit 1 — every sprite handler ORs 3
//     into its cells at draw time and only pass 2 clears bit 1, so the page loses all its
//     sprites → v2_page_list_clear(page);
//   sub_16880 (level transition, clears the VGA) → v2_page_lists_clear_all().
// The frame of a flip is the composition of the shown page: tiles from the map at the
// camera, the page's commands in their order with the dead cells skipped, the flagged
// tiles, the UI (v2_compose_page, called by the sub_16775 mirror right before it publishes;
// it bakes the dead masks and drops the commands with no live cell left) — the same list
// the presenter gets.
// tag: the writer's name for the debug trace (V2_PL_ERASE_TRACE=<from>-<to>: every erase of the
// game-frame range as "V2-PLE f<frame> <tag> page=.. fs=.. n=.. epoch=..")
extern void v2_page_list_erase_cells(uint16_t page, uint16_t fs_off, int ncells, const char* tag = "");
// tiles painted over every page at once (the scroll-in row sub_16DC1 + its copies sub_171DC,
// the scroll-in column sub_16DD9 + sub_1712B, the full fill of sub_11439): the cells at
// fs_off, fs_off + step, ... die on all three pages
extern void v2_page_lists_erase_cells_all(uint16_t fs_off, int ncells, uint16_t step, const char* tag = "");
// The page's TILES are page state too: each page holds, per cell, the tile word last painted
// there (g_page_tile: 0xFFFF = never painted since the level's fill → the map's word at
// composition, 0xFFFE = black, the VGA wiped by sub_16880 and not painted since). Painters:
//   sub_16DC1 / sub_16DD9 (a row / column of tiles onto the draw page, copied onto the other
//     two by sub_171DC / sub_1712B) → v2_page_tile_set_all(fs_off, word) per cell;
//   sub_1406d (an animated-tile quadrant onto [92F9] and [92FB]) → v2_page_tile_set(page, ..);
//   the span copies — sub_1DE05 pass 2 (background page → [92F7]) and sub_1DF6A part 2 (the
//     page rotation, [92F9] → [92FB] at the render-map's bit-1 cells) → v2_page_cells_copy:
//     the destination cells die, the SOURCE page's content moves in — its tile words and its
//     commands' pixels in those cells (copies restricted to the span; a sprite the background
//     page had baked in travels on, as on the VGA);
//   sub_16880 → v2_page_lists_black(): every cell black, every list empty.
// Glyphs are page content as well: sub_1E0C7 paints each text cell at a MAP cell of [92F9]
// (the tile window's column + [257F], row + [2581]) — v2_page_list_glyph records a
// V2_CMD_GLYPH command there; the composition draws them after the flagged tiles, as the
// pass does (1E0C7 runs after 1C8F1), and v2_draw_ui then paints only the CJK overlay.
// A pass's span copies are one batch (nothing else touches the pages between the spans of
// sub_1DE05 pass 2 or of sub_1DF6A part 2): begin names the pages, every span kills its
// destination cells and moves the tile words, end appends ONE copy per source record with
// pixels in any of the spans (keep = those cells) — instead of a copy per span per record.
// fs_off = the render-map offset of the span's first cell, (map_col, map_row) its map cell, fs = the render map
extern void v2_page_cells_copy_begin(uint16_t dst, uint16_t src, const char* tag = "");
extern void v2_page_cells_copy_span(uint16_t fs_off, int ncells, int map_col, int map_row, const uint8_t* fs);
extern void v2_page_cells_copy_end(void);
extern void v2_page_tile_set(uint16_t page, uint16_t fs_off, uint16_t word);
extern void v2_page_tile_set_all(uint16_t fs_off, uint16_t word);
extern void v2_page_lists_black(void);
// The BACKGROUND VGA (2026-09-12): the tile layer of a frame is what the VGA memory holds,
// read out the way the CRTC does. The pages are not rows of map cells: each page's rows sit
// at a 42-byte phase inside the 0x56 pitch, a tile row is painted at LUT_PAGE_ROW[row] plus
// a column term and the window is the linear span of bytes from the CRTC start (sub_16775:
// LUT_SUBROW + LUT_PAGE_ROW + (x >> 2) + 8), 80 bytes per line across pitch boundaries and
// through the ring — so the first flip of a level shows the fill's rows where the fill put
// them and, below them, whatever the memory held: the 0x00 of the clear, or a previous
// scene's picture and a previous level's tiles where sub_16880 (the port's half wipe) did not
// reach — exactly like the original. A second pixel plane of the shadow VGA, v2_vga_bg,
// receives only the BACKGROUND writers: the tile painter (the sub_1689E mirror), the picture
// chunks of sub_10cd8 (v2_vga_bg_writer set around them, checked in v2_vga_w), the span copies
// and the fills — never the sprite engines, the glyphs or the flagged-tile repaint, which stay
// commands on top. The composition reads the window out of it — byte crtc + y * 0x56 +
// ((pan + x) >> 2), plane (pan + x) & 3 — over columns 0..319: the window starts at the
// camera's left edge, which is frame column 0 in every width (the map-based pass
// stays for the wings of a wide frame; index 0 stays transparent over the parallax layer).
// The presenter carries a copy in its snapshot and derives its interpolated camera's CRTC
// start with the same formula (v2_crtc_for_camera).
extern uint8_t v2_vga_bg[65536 * 4];
extern bool    v2_vga_bg_writer;                          // true while a background writer paints
extern thread_local const uint8_t* v2_tls_vga_bg;         // the presenter's copy
// the CRTC start + pel pan sub_16775 computes for the DS's camera (shadow-side formula)
extern void v2_crtc_for_camera(const uint8_t* ds, uint32_t* crtc, uint8_t* pan);
extern void v2_page_list_glyph(uint16_t page, int16_t x, int16_t y, uint16_t glyph_index);
extern void v2_page_list_fgtile(uint16_t page, int16_t x, int16_t y, uint16_t tile_word);   // sub_1C8F1's repaint of a dirty flagged cell
// the flagged tiles come from the page's V2_CMD_FGTILE commands: v2_draw_flagged_tiles paints only
// the parallax layer's priority pass (a console rule) and skips the map's bit-3 tiles
extern thread_local bool v2_tls_fg_from_page;
extern void v2_page_list_draw(uint16_t page, const V2DrawCmd& cmd);
// the composition's tile words: the composed page's per-cell array (index = render-map word)
// on the game thread, the snapshot's copy on the presenter (thread-local); nullptr = the map
extern const uint16_t* v2_tile_override;
extern thread_local const uint16_t* v2_tls_tile_ovr;
// the text cells come from the page's glyph commands: v2_draw_ui paints only the CJK overlay
extern thread_local bool v2_tls_ui_cells_from_page;
extern void v2_page_list_clear(uint16_t page);
extern void v2_page_lists_clear_all(void);
extern void v2_compose_page(uint16_t ds_val, uint16_t page);
extern bool v2_compose_at_flip;   // true while v2_compose_page runs: the layer gates admit it outside a frame (blocking loops)
// debug: V2_PAGELIST_TRACE=<hex slot> — every page-list event of that slot (erase / draw /
// clear) and its record at each flip's composition, plus the DS fields the pass mirrors
// saw ([114D]/[114E], CUR/OLD, sprite offset, the page words) — lines "V2-PL f<frame> ..."
extern bool v2_pl_trace_on(int slot);

// Single-tile redraw for dirty-rect (sub_1de05 inner loop) — exact orig sub_1689e equivalent
extern void v2_draw_single_tile(uint16_t ds_val, uint16_t fs_offset, int abs_row, int abs_col);
// Refresh static intro/menu chunk_bg backup from current v2_render_buf (call after
// op_13/0x11 menu activation to capture cleared+moved state as new static background).
extern void v2_chunk_bg_update_from_render();

// v2 shadow DAC (task #22): exact VGA DAC state maintained by the v2 mirrors
// of the orig OUT 3C8/3C9 sites (6-bit values; published <<2 by v2_swap_render_buf).
extern uint8_t v2_dac_shadow[768];
extern void v2_draw_hud_item(uint16_t ds_val, uint16_t slot_di, uint16_t item_ax);
extern void v2_draw_hud_portrait(uint16_t ds_val, uint16_t viking_di, uint16_t portrait_si);
extern void v2_draw_hud_selector(uint16_t ds_val, uint16_t slot_di);
extern void v2_draw_hud_healthbar(uint16_t ds_val, uint16_t health_ax, uint16_t viking_bx, uint16_t pos_di);

// Init shadow DS from real DS — call BEFORE sub_11080 to capture pre-init state
extern void v2_vm_init_shadow_early(uint16_t ds_val);
// Run v2 init chain (sub_11080 equivalent) on shadow DS
extern void v2_vm_run_init(uint16_t ds_val);
// Post-init verification: compare all segments after init, before game loop
extern void v2_vm_verify_after_init(uint16_t ds_val);
// Post-game-loop verification
extern void v2_vm_verify_game_loop(uint16_t ds_val);
extern void v2_vm_verify_collision(uint16_t ds_val);
extern void v2_vm_verify_tilemap(uint16_t ds_val);
extern void v2_vm_verify_all_segments(uint16_t ds_val);

// Segment resolver: maps segment value to shadow buffer pointer.
// For standalone v2: fake segments → shadow buffers.
// For verify mode: real segments → real memory via v2_m2c_base.
extern uint8_t* v2_resolve_segment(uint16_t seg, uint8_t* shadow_ds = nullptr);

#endif // RENDER_V2_H
