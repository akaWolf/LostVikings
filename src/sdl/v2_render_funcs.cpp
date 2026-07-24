// ============================================================================
// V2 rendering functions — independent reimplementations of seg003 functions
// that draw to v2_render_buf from original game data (tile map, tile graphics).
// Called from seg000 before the original seg003 calls.
// Does NOT read from myDrawInfo/VGA — fully independent.
// ============================================================================

#include <cstdint>
#include <cstring>
#include <cstdio>
#include "render_v2.h"
#include "v2_ds_layout.h"

// Task #21 obj-trace ring (defined in v2_vm.cpp).
extern "C" void v2_objtrace(const char* tag, int a, int b, int c, int d);
extern "C" int v2_objtrace_di;

// V2 is fully independent of myDrawInfo / orig drawBuffer.
// v2 decodes chunks itself via v2_draw_viewport_chunk (writes to v2_render_buf
// directly, mirroring orig sub_10cd8 → VGA) and v2_draw_hud_background → v2_hud_buf.

#ifdef V2_RENDER_FROM_SHADOW
bool v2_vm_in_frame = false;
#endif

// Helper: get DS base pointer for v2 rendering.
// With V2_RENDER_FROM_SHADOW: reads from v2 VM's shadow DS (independent from original).
// Without: reads from real DS (same data as original VM).
static inline uint8_t* v2_get_ds_base(uint16_t ds_val) {
#ifdef V2_RENDER_FROM_SHADOW
    uint8_t* shadow = v2_vm_get_shadow_ds();
    if (shadow) return shadow;
#endif
    return v2_m2c_base + ((uint32_t)ds_val << 4);
}

// V2 rendering state — definitions (declared extern in render_v2.h)
uint8_t* v2_m2c_base = nullptr;
std::mutex v2_ds_modify_mutex;
uint8_t  v2_render_buf[320*200];
uint8_t  v2_display_buf[320*200];
std::mutex v2_display_mutex;
uint8_t  v2_hud_buf[320*64];

// Static intro/menu chunk pixels backup. Orig keeps static chunk pixels in VGA
// across frames, dirty-rect (sub_1de05) erases sprite trails by redrawing tiles
// (which in intro have empty content, effectively reverting to chunk pixels via
// CRTC page persistence). v2 single-buffer architecture lacks page-flip — so we
// save chunk pixels at load time and restore each frame in v2_draw_tiles for
// intro flag levels (= semantic equivalent of "tile redraw + chunk persistence").
// On op_13/0x11 (menu activation) backup is updated to match cleared+moved state.
uint8_t v2_chunk_bg_backup[320*176];
bool v2_chunk_bg_valid = false;
// #39: glyph layer snapshot as of the last orig 1E0C7 painted tick - the
// orig pages keep this layer between passes; v2 repaints it after bg blits.
uint8_t v2_glyph_page_snapshot[0x370];
bool v2_glyph_snap_valid = false;
// #39 f193: previous painted snapshot - cells that leave the buffer (a shorter
// reply) must be erased from the pages with the clean tile background, exactly
// like the orig dirty channel repaints the tiles under the removed text.
uint8_t v2_glyph_prev_snapshot[0x370];

// #39 dirty-tile erosion: orig keeps painted glyphs on the page until a CHANGED
// (dirty) tile redraws over them (sub_1C8F1 dirty channel). v2 re-blits every
// foreground tile each sub-frame, so it needs the glyph snapshot — but the
// snapshot must be eroded wherever the scene content actually changed
// (illustration swap / scroll), else it keeps painting stale dialog text over
// the new picture (the whole intro divergence class: 278/2977/9357/28595 are
// all this one root). Enabled only around the v2_emu_late_end shown-page pass
// (draw_flagged_tiles has many other callers/targets that must not touch it).
bool v2_flagged_erode = false;

void v2_chunk_bg_update_from_render() {
    memcpy(v2_chunk_bg_backup, v2_render_buf, 320 * 176);
    v2_chunk_bg_valid = true;
}

// v2 shadow DAC (task #22): byte-exact model of the VGA DAC state, updated by
// the v2 mirrors of the orig OUT 3C8/3C9 sites from SHADOW data — the same
// sites whose setPalette mirrors feed drawPalette on the orig side:
//   sub_10fe6 full upload (source HARDCODED ds:0x8202), sub_10ffc positive/
//   negative animation bursts (source ds:[word_303E0 + ...] — can be 0x7F02!),
//   sub_1106f blank at Mode X init (covered by zero init), and the color-3
//   writes (sub_10350 / sub_1041c / sub_103ca / VM cmd_type 6 loc_127e4).
// The previous display model (snapshot of ds:0x8202 + color-3 mirror) was
// wrong whenever a 10ffc burst ran with word_303E0 = 0x7F02 under nonzero
// shade: the DAC held 0x7F02 colors while 0x8202 held shaded ones
// (V2-PAL-DIVERGE idx 0x71). Values are 6-bit VGA (0..0x3F), <<2 at publish.
uint8_t v2_dac_shadow[768] = {};

void v2_swap_render_buf() {
#ifdef V2_RENDER_FROM_SHADOW
    if (!v2_vm_in_frame) return;
#endif
    // Atomic snapshot of pixels + HUD + palette under same lock. All 3 must be
    // captured together — otherwise level-transition updates one before render
    // thread reads the others → mismatched colors (main artifact: viewport,
    // HUD icons flicker: HUD pixels rendered with palette from different frame).
    std::lock_guard<std::mutex> lock(v2_display_mutex);
    memcpy(v2_display_buf, v2_render_buf, 320*200);
    extern uint8_t v2_display_hud_buf[];
    memcpy(v2_display_hud_buf, v2_hud_buf, 320*64);
    extern uint8_t* v2_vm_get_shadow_ds();
    uint8_t* shad = v2_vm_get_shadow_ds();
    if (shad) {
        extern SDL_Color v2_display_palette[256];
        extern bool v2_display_palette_valid;
        // Publish the shadow DAC (task #22) — the exact VGA DAC state as
        // maintained by the v2 mirrors of every orig OUT 3C8/3C9 site.
        // (Replaces the old "ds:0x8202 snapshot + color-3 mirror" model,
        // which missed 10ffc bursts sourced from 0x7F02 under shade —
        // V2-PAL-DIVERGE idx 0x71.)
        for (int i = 0; i < 256; i++) {
            v2_display_palette[i].r = v2_dac_shadow[i*3 + 0] << 2;
            v2_display_palette[i].g = v2_dac_shadow[i*3 + 1] << 2;
            v2_display_palette[i].b = v2_dac_shadow[i*3 + 2] << 2;
            v2_display_palette[i].a = 255;
        }
        v2_display_palette_valid = true;
    }
}

// Snapshot buffers — captured atomically at v2_swap_render_buf time.
SDL_Color v2_display_palette[256] = {};
bool v2_display_palette_valid = false;
uint8_t v2_display_hud_buf[320*64] = {};

void v2_set_m2c_base(void* base) {
    if (!v2_m2c_base) v2_m2c_base = (uint8_t*)base;
}

// ============================================================================
// v2_draw_tiles: Renders visible tiles from the tile map and tile graphics.
//
// Data sources (all in game memory, NOT in VGA):
//   Tile map:      FS segment (ds:0x2E69), indexed via row lookup at ds-0x7098
//   Tile graphics: segment at ds:0x2E5F, 64 bytes per tile
//   Tile format:   4 planes × 8 rows × 2 bytes = 64 bytes, offset = entry & 0xFFC0
//   Flip flags:    entry bits 4:5 — bit4=hflip, bit5=vflip
//
// Mirrors sub_1689e (draw_tile) + sub_16dc1 (draw tile row) logic.
// ============================================================================
void v2_draw_tiles(uint16_t ds_val) {
#ifdef V2_RENDER_FROM_SHADOW
    if (!v2_vm_in_frame) return;
#endif
    if (!myDrawInfo_v2 || !v2_m2c_base) return;

    uint8_t* ds_base = v2_get_ds_base(ds_val);
    uint8_t* buf = v2_render_buf;

    // Tile map segment (FS)
    uint16_t fs_seg = *(uint16_t*)(ds_base + DS_SEG_FS);
    // Tile graphics segment
    uint16_t tgfx_seg = *(uint16_t*)(ds_base + DS_SEG_TILEGFX);
    // V2-DRAWT-DBG: log entry state every 60 frames
    {
        static int _dt_dbg = 0; _dt_dbg++;
        if (_dt_dbg <= 5 || _dt_dbg % 200 == 0) {
            fprintf(stderr,
              "V2-DRAWT-DBG[%d]: ds=%04X fs_seg=%04X tgfx_seg=%04X 25CF=%02X 25AD=%04X 25C9=%04X\n",
              _dt_dbg, ds_val, fs_seg, tgfx_seg,
              ds_base[DS_LEVEL_FLAGS],
              *(uint16_t*)(ds_base + DS_LEVEL),
              *(uint16_t*)(ds_base + 0x25C9));
        }
    }

    if (!fs_seg || !tgfx_seg) return;

    // Match orig sub_11439 (eip 0x1439): TEST byte_2AAAF,42h / JNZ loc_11443.
    // On intro flags (0x40=INTRO_TYPE2 OR 0x02=INTRO_TYPE1) orig SKIPS sub_16ded
    // (per-frame tile render) and jumps to set_display_memory_addr — chunk pixels
    // written by sub_10cd8 stay in VGA. HUD-only (0x20) goes through tile render.
    // V2 single-buffer architecture: restore chunk_bg_backup each frame to erase
    // dynamic content (sprites, cursor) and recover static chunk pixels — semantic
    // equivalent of orig page-flip + dirty-rect tile-redraw mechanism.
    uint8_t lvl_flags = ds_base[DS_LEVEL_FLAGS];
    if (lvl_flags & 0x42) {
        if (v2_chunk_bg_valid) {
            memcpy(v2_render_buf, v2_chunk_bg_backup, 320 * 176);
        }
        return;
    }
    v2_chunk_bg_valid = false; // tile-based level; static backup no longer relevant

    // Normal: clear and draw tiles
    memset(buf, 0, 320*176);

#ifdef V2_RENDER_FROM_SHADOW
    uint8_t* fs_base = v2_resolve_segment(fs_seg);
    uint8_t* tgfx_base = v2_resolve_segment(tgfx_seg);
#else
    uint8_t* fs_base = v2_m2c_base + ((uint32_t)fs_seg << 4);
    uint8_t* tgfx_base = v2_m2c_base + ((uint32_t)tgfx_seg << 4);
#endif

    uint16_t scroll_x = *(uint16_t*)(ds_base + 0x2581);  // row scroll
    uint16_t scroll_y = *(uint16_t*)(ds_base + 0x257F);  // column scroll

    // Sub-tile pixel offset from viewport pixel position.
    // Mirrors orig set_display_memory_addr (sub_16775, seg000.cpp:1105):
    //   x_offset = x_disp + x_some; if (x_offset > x_level_size) x_offset = x_disp - x_some;
    //   y_offset = y_disp + y_some; if (y_offset > y_level_size) y_offset = y_disp - y_some;
    // x_some/y_some (ds:0x39E/0x3A0) are screen shake offsets (sub_12d2c, eip 0x2D2C).
    // x_low_bits = x_offset & 0b11 (pixel pan 0..3, OUT to attribute reg 0x33)
    // x_high_bits = x_offset >> 2 (byte position in VGA row, → CRTC start addr)
    // Combined display position = (x_high_bits<<2) + x_low_bits = x_offset (full pixel precision).
    // In v2 linear buffer: total sub-tile pixel offset = x_offset & 7.
    int16_t vp_px = *(int16_t*)(ds_base + DS_VIEWPORT_X);
    int16_t vp_py = *(int16_t*)(ds_base + DS_VIEWPORT_Y);
    int16_t x_some = *(int16_t*)(ds_base + 0x39E);
    int16_t y_some = *(int16_t*)(ds_base + 0x3A0);
    int16_t x_lvl  = *(int16_t*)(ds_base + 0x25A4);
    int16_t y_lvl  = *(int16_t*)(ds_base + 0x25A6);
    int x_eff = (int)vp_px + (int)x_some; if (x_eff > (int)x_lvl) x_eff = (int)vp_px - (int)x_some;
    int y_eff = (int)vp_py + (int)y_some; if (y_eff > (int)y_lvl) y_eff = (int)vp_py - (int)y_some;
    int pix_off_x = x_eff & 7;
    int pix_off_y = y_eff & 7;
    // Tile-aligned shift due to shake (when shake crosses tile boundary).
    // Source tile column index is shifted by (x_eff/8 - vp_px/8); same for rows.
    int extra_tile_x = (x_eff >> 3) - ((int)vp_px >> 3);
    int extra_tile_y = (y_eff >> 3) - ((int)vp_py >> 3);

    for (int row_vis = 0; row_vis < 25; row_vis++) {
        uint16_t row_scrolled = (uint16_t)(row_vis + scroll_x + extra_tile_y);
        // Orig sub_16ded has NO row_scrolled bound — just reads LUT and renders
        // whatever it finds. v2 had `if (row_scrolled >= 64) continue;` hardcoded
        // limit which clipped tiles on large maps when scrolled past row 64.
        // Removed: now mirrors orig (LUT read wraps via uint16_t arithmetic).

        // Row base from lookup table at ds-0x7098
        uint16_t lut_off = (uint16_t)(row_scrolled * 2u - LUT_ROW_BASE);
        uint16_t row_base = *(uint16_t*)(ds_base + lut_off);

        for (int col_vis = 0; col_vis < 43; col_vis++) {
            uint16_t col_scrolled = (uint16_t)(col_vis + scroll_y + extra_tile_x);

            // Read tile map entry: word at fs:[(row_base + col_scrolled) * 2]
            uint16_t tile_map_off = (uint16_t)((row_base + col_scrolled) * 2u);
            uint16_t tile_entry = *(uint16_t*)(fs_base + tile_map_off);

            // Tile graphics offset (64-byte aligned) — bits 15:6
            // Bits 5:0 contain flip flags (4=hflip, 5=vflip) and dirty flag (bit 0)
            // draw_tile (sub_1689e) renders ALL tiles including offset 0 — no skip.
            uint16_t tile_gfx_off = tile_entry & 0xFFC0;
            bool hflip = (tile_entry & 0x10) != 0;
            bool vflip = (tile_entry & 0x20) != 0;

            uint8_t* tile = tgfx_base + tile_gfx_off;

            // Screen position (with sub-tile pixel offset)
            int screen_x = col_vis * 8 - pix_off_x;
            int screen_y = row_vis * 8 - pix_off_y;

            // Decode tile: 4 planes × 8 rows × 2 bytes
            // Normal pixel order per row:
            //   x+0=p0_b0, x+1=p1_b0, x+2=p2_b0, x+3=p3_b0,
            //   x+4=p0_b1, x+5=p1_b1, x+6=p2_b1, x+7=p3_b1
            for (int row = 0; row < 8; row++) {
                int src_row = vflip ? (7 - row) : row;
                int sy = screen_y + row;
                if (sy < 0) continue;
                if (sy >= 200) break;

                // Extract 8 pixels for this row
                uint8_t pixels[8];
                for (int plane = 0; plane < 4; plane++) {
                    int p_in = plane;
                    uint8_t b0 = tile[p_in * 16 + src_row * 2];
                    uint8_t b1 = tile[p_in * 16 + src_row * 2 + 1];

                    if (!hflip) {
                        pixels[plane + 0] = b0;
                        pixels[plane + 4] = b1;
                    } else {
                        pixels[(3 - plane) + 4] = b0;
                        pixels[(3 - plane) + 0] = b1;
                    }
                }

                // Write to buffer (with bounds checking for negative offsets)
                for (int px = 0; px < 8; px++) {
                    int sx = screen_x + px;
                    if (sx < 0) continue;
                    if (sx >= 320) break;
                    buf[sy * 320 + sx] = pixels[px];
                }
            }
        }
    }
}

// ============================================================================
// v2_draw_single_tile: Render ONE tile at (abs_row, abs_col) in v2_render_buf.
//
// Mirrors orig sub_1689e called from sub_1de05 dirty-rect inner loop.
// Used to erase sprite trails on intro levels (where v2_draw_tiles is skipped):
// when sprite moves, sub_1de05 marks tiles under old position dirty, then this
// function redraws each dirty tile's content → erases the sprite pixels.
//
// fs_offset = byte offset in FS shadow where tile entry resides
// abs_row, abs_col = absolute tilemap coordinates (used for screen position)
// ============================================================================
void v2_draw_single_tile(uint16_t ds_val, uint16_t fs_offset, int abs_row, int abs_col) {
#ifdef V2_RENDER_FROM_SHADOW
    if (!v2_vm_in_frame) return;
#endif
    if (!v2_m2c_base) return;
    uint8_t* ds_base = v2_get_ds_base(ds_val);
    uint16_t fs_seg = *(uint16_t*)(ds_base + DS_SEG_FS);
    uint16_t tgfx_seg = *(uint16_t*)(ds_base + DS_SEG_TILEGFX);
    if (!fs_seg || !tgfx_seg) return;
#ifdef V2_RENDER_FROM_SHADOW
    uint8_t* fs_base = v2_resolve_segment(fs_seg);
    uint8_t* tgfx_base = v2_resolve_segment(tgfx_seg);
#else
    uint8_t* fs_base = v2_m2c_base + ((uint32_t)fs_seg << 4);
    uint8_t* tgfx_base = v2_m2c_base + ((uint32_t)tgfx_seg << 4);
#endif
    uint16_t scroll_x = *(uint16_t*)(ds_base + 0x2581);
    uint16_t scroll_y = *(uint16_t*)(ds_base + 0x257F);
    // Mirrors orig set_display_memory_addr (sub_16775) — apply x_some/y_some shake.
    int16_t vp_px = *(int16_t*)(ds_base + DS_VIEWPORT_X);
    int16_t vp_py = *(int16_t*)(ds_base + DS_VIEWPORT_Y);
    int16_t x_some = *(int16_t*)(ds_base + 0x39E);
    int16_t y_some = *(int16_t*)(ds_base + 0x3A0);
    int16_t x_lvl  = *(int16_t*)(ds_base + 0x25A4);
    int16_t y_lvl  = *(int16_t*)(ds_base + 0x25A6);
    int x_eff = (int)vp_px + (int)x_some; if (x_eff > (int)x_lvl) x_eff = (int)vp_px - (int)x_some;
    int y_eff = (int)vp_py + (int)y_some; if (y_eff > (int)y_lvl) y_eff = (int)vp_py - (int)y_some;
    int pix_off_x = x_eff & 7;
    int pix_off_y = y_eff & 7;
    int extra_tile_x = (x_eff >> 3) - ((int)vp_px >> 3);
    int extra_tile_y = (y_eff >> 3) - ((int)vp_py >> 3);

    // Convert absolute tilemap coords → visible viewport position (with shake-aware tile shift)
    int visible_row = abs_row - (int)scroll_x - extra_tile_y;
    int visible_col = abs_col - (int)scroll_y - extra_tile_x;
    if (visible_row < 0 || visible_row >= 25) return;
    if (visible_col < 0 || visible_col >= 43) return;

    if ((uint32_t)fs_offset + 1 >= 0x6000) return;
    uint16_t tile_entry = *(uint16_t*)(fs_base + fs_offset);
    uint16_t tile_gfx_off = tile_entry & 0xFFC0;
    bool hflip = (tile_entry & 0x10) != 0;
    bool vflip = (tile_entry & 0x20) != 0;
    uint8_t* tile = tgfx_base + tile_gfx_off;

    int screen_x = visible_col * 8 - pix_off_x;
    int screen_y = visible_row * 8 - pix_off_y;
    uint8_t* buf = v2_render_buf;

    for (int row = 0; row < 8; row++) {
        int src_row = vflip ? (7 - row) : row;
        int sy = screen_y + row;
        if (sy < 0) continue;
        if (sy >= 200) break;
        uint8_t pixels[8];
        for (int plane = 0; plane < 4; plane++) {
            uint8_t b0 = tile[plane * 16 + src_row * 2];
            uint8_t b1 = tile[plane * 16 + src_row * 2 + 1];
            if (!hflip) {
                pixels[plane + 0] = b0;
                pixels[plane + 4] = b1;
            } else {
                pixels[(3 - plane) + 4] = b0;
                pixels[(3 - plane) + 0] = b1;
            }
        }
        for (int px = 0; px < 8; px++) {
            int sx = screen_x + px;
            if (sx < 0) continue;
            if (sx >= 320) break;
            buf[sy * 320 + sx] = pixels[px];
        }
    }
}

// ============================================================================
// v2_draw_sprites: Draws sprite objects from the object table.
//
// Data sources (all in game memory):
//   Object table:  128 entries at DI=0..0xFE (step 2)
//     Flags:       ds:[di+0x44D]  (word) — bit 15=active, bits 13-14=skip, bits 0-2=type
//     World X:     ds:[di+0x64D]  (signed word)
//     World Y:     ds:[di+0x74D]  (signed word)
//     Sprite data: ds:[di+0x84D]  (word) — offset into sprite segment
//     Sprite seg:  ds:[di+0x94D]  (word) — segment for sprite data
//
// Sprite data format: 4 planes × N strips × (1 mask + 8 data bytes).
// Each strip writes to MULTIPLE screen rows (not one!).
//
// Type 0 (seg003_648_proc, jpt_1CF4E handlers): 8×8 sprite, 72 bytes
//   2 strips/plane, 4 rows/strip spacing. Each strip → 4 rows × 2 bytes/row.
//   data[0,1]→row+0, data[2,3]→row+1, data[4,5]→row+2, data[6,7]→row+3
//   Mask: bits 76→row0, 54→row1, 32→row2, 10→row3
//
// Types 1-7 (0x0B82 renderer, jpt_1d514 handlers): 16×16 sprite, 288 bytes
//   8 strips/plane, 2 rows/strip spacing. Each strip → 2 rows × 4 bytes/row.
//   data[0..3]→row+0, data[4..7]→row+1
//   Mask: bits 7654→row0, 3210→row1
//
// VGA pitch = 86 bytes = 1 row (344 px / 4 planes).
// ============================================================================

// Helper: write one pixel to v2 buffer with bounds checking.
// Writes ALL colors including 0 (matching original VGA behavior where mask
// controls which bytes are written, not the color value).
// Blit translation state: screen-space (sx,sy) + (dx,dy) into a buffer of
// the given pitch/bounds. Display: dx=dy=0, 320x176. Emu page target: the
// window offset (eff − base) with the page pitch — set via v2_blit_to_page().
static int v2_pp_dx = 0, v2_pp_dy = 0, v2_pp_pitch = 320, v2_pp_w = 320, v2_pp_h = 176;
static inline void v2_put_pixel(uint8_t* buf, int sx, int sy, uint8_t color) {
    int x = sx + v2_pp_dx, y = sy + v2_pp_dy;
    if (x >= 0 && x < v2_pp_w && y >= 0 && y < v2_pp_h)
        buf[y * v2_pp_pitch + x] = color;
}

// ============================================================================
// Page emulator (task #21, stage 1): byte-exact model of the orig VGA page
// channel for the A2 sensor. DOS uses one BACKGROUND page (clean tiles, the
// latch-copy source ds:0x92FB in sub_1de05's dirty spans) plus TWO work pages
// flipped per sub-frame (ds:0x92F7). Each sub-frame: dirty cells are latch-
// copied from the background page, then sprites are re-blitted (jpt_1d514) and
// the sub_1dd9c layer runs. v2's display buffer is a clean full-frame render;
// the emu pages reproduce what the orig work page actually contains, so the
// A2 sensor can demand a byte-exact match.
// ============================================================================
uint8_t v2_emu_bg[320 * 176];          // background of THIS sub-frame = clean tile render
// THREE persistent pages, indexed by the orig page slot value/0x34 (the
// ds:0x92F7/92F9/92FB role variables hold 0x00/0x34/0x68 — sub_165aa rotates
// the ROLES over the fixed pages):
//   [92F7] = draw page (the previous sub-frame's shown page; ONLY the 1de05
//            latches land here),
//   [92F9] = shown page (this sub-frame draws sub_1dd9c/1c8f1 layers on it
//            AND the CRTC flips to it — BAND-parity proven),
//   [92FB] = background page (latch source for the 1de05 bit0 spans; on a
//            background-role change sub_1df6a copies bit1 cells 92F9→92FB).
//
// ANCHOR MODEL (f459 shake class): the orig pages hold WORLD content and the
// CRTC window (vp+shake) moves over them WITHOUT touching the pixels. The emu
// pages are anchored at B = eff & ~7 (8-aligned world coordinate of page
// pixel (0,0)) with an 8px right/bottom margin; sub-tile window offsets
// (eff − B ∈ [0,8)) apply only when reading the shown window — a ±1..7 shake
// never moves page content, matching the orig.
uint8_t v2_emu_page[3][V2_EMU_W * V2_EMU_H];
int     v2_emu_cur = 0;                // legacy extern (unused in 3-page model)
bool    v2_emu_valid = false;          // pages initialized
int     v2_emu_base_x = 0;             // world coord of page pixel (0,0), 8-aligned
int     v2_emu_base_y = 0;
static inline int v2_emu_slot(uint8_t* ds_base, uint16_t role_addr) {
    uint16_t v = *(uint16_t*)(ds_base + role_addr);
    int s = v / 0x34;
    return s > 2 ? 2 : s;
}
// Effective viewport (vp + shake with the sub_16775 clamp).
static void v2_emu_eff(uint8_t* ds_base, int* xe, int* ye) {
    int16_t vx = *(int16_t*)(ds_base + DS_VIEWPORT_X), vy = *(int16_t*)(ds_base + DS_VIEWPORT_Y);
    int16_t xs = *(int16_t*)(ds_base + 0x39E), ys = *(int16_t*)(ds_base + 0x3A0);
    int16_t xl = *(int16_t*)(ds_base + 0x25A4), yl = *(int16_t*)(ds_base + 0x25A6);
    *xe = (int)vx + xs; if (*xe > (int)xl) *xe = (int)vx - xs;
    *ye = (int)vy + ys; if (*ye > (int)yl) *ye = (int)vy - ys;
}
// Copy the current screen-space background render into a page at the given
// window offset (off = eff − B). Margin bands outside the 320x176 screen keep
// their previous content (orig pages hold real world there; we lack the data).
static void v2_emu_page_fill_bg(uint8_t* pg, int offx, int offy) {
    for (int y = 0; y < 176; y++)
        memcpy(pg + (y + offy) * V2_EMU_W + offx, v2_emu_bg + y * 320, 320);
}

// Render one map tile directly into a page at page coords (px0,py0) — the
// page-space equivalent of the orig edge channels (sub_170b9/17049 paint map
// rows BEYOND the visible window onto every page; the bg render can't supply
// those). Tile decode identical to v2_draw_tiles.
static void v2_emu_render_tile(uint8_t* pg, uint8_t* ds_base,
                               uint8_t* fs_base, uint8_t* tgfx_base,
                               int map_row, int map_col, int px0, int py0) {
    uint16_t lut = (uint16_t)((uint16_t)map_row * 2u - LUT_ROW_BASE);
    uint16_t row_base = *(uint16_t*)(ds_base + lut);
    uint16_t moff = (uint16_t)(((uint16_t)(row_base + map_col)) * 2u);
    uint16_t entry = *(uint16_t*)(fs_base + moff);
    uint16_t gfx = entry & 0xFFC0;
    bool hflip = (entry & 0x10) != 0, vflip = (entry & 0x20) != 0;
    uint8_t* tile = tgfx_base + gfx;
    for (int row = 0; row < 8; row++) {
        int src_row = vflip ? (7 - row) : row;
        int py = py0 + row;
        if (py < 0 || py >= V2_EMU_H) continue;
        uint8_t pixels[8];
        for (int plane = 0; plane < 4; plane++) {
            uint8_t b0 = tile[plane * 16 + src_row * 2];
            uint8_t b1 = tile[plane * 16 + src_row * 2 + 1];
            if (!hflip) { pixels[plane] = b0; pixels[plane + 4] = b1; }
            else        { pixels[(3 - plane) + 4] = b0; pixels[(3 - plane)] = b1; }
        }
        for (int px = 0; px < 8; px++) {
            int X = px0 + px;
            if (X >= 0 && X < V2_EMU_W)
                pg[py * V2_EMU_W + X] = pixels[px];
        }
    }
}

// Full-page tile render (41x23 tiles from the anchor) — level entry / fresh
// pages, matching orig sub_16ded which paints whole map rows on every page.
static void v2_emu_page_render_full(uint8_t* pg, uint8_t* ds_base) {
    uint16_t fs_seg = *(uint16_t*)(ds_base + DS_SEG_FS);
    uint16_t tg_seg = *(uint16_t*)(ds_base + DS_SEG_TILEGFX);
    if (!fs_seg || !tg_seg) return;
#ifdef V2_RENDER_FROM_SHADOW
    uint8_t* fsb = v2_resolve_segment(fs_seg);
    if (!fsb) fsb = v2_m2c_base + ((uint32_t)fs_seg << 4);
    uint8_t* tgb = v2_resolve_segment(tg_seg);
    if (!tgb) tgb = v2_m2c_base + ((uint32_t)tg_seg << 4);
#else
    uint8_t* fsb = v2_m2c_base + ((uint32_t)fs_seg << 4);
    uint8_t* tgb = v2_m2c_base + ((uint32_t)tg_seg << 4);
#endif
    int col0 = v2_emu_base_x >> 3, row0 = v2_emu_base_y >> 3;
    for (int tr = 0; tr < V2_EMU_H >> 3; tr++)
        for (int tc = 0; tc < V2_EMU_W >> 3; tc++)
            v2_emu_render_tile(pg, ds_base, fsb, tgb,
                               row0 + tr, col0 + tc, tc * 8, tr * 8);
}

// Blit target override for the sprite renderer: null = v2_render_buf.
// When a page is the target, blit coordinates are screen-space and get
// translated by (v2_blit_dx, v2_blit_dy) with the page pitch/bounds.
static uint8_t* v2_blit_target = nullptr;
static void v2_blit_to_page(uint8_t* pg, int offx, int offy) {
    v2_blit_target = pg;
    v2_pp_dx = offx; v2_pp_dy = offy;
    v2_pp_pitch = V2_EMU_W; v2_pp_w = V2_EMU_W; v2_pp_h = V2_EMU_H;
}
static void v2_blit_to_display(void) {
    v2_blit_target = nullptr;
    v2_pp_dx = 0; v2_pp_dy = 0;
    v2_pp_pitch = 320; v2_pp_w = 320; v2_pp_h = 176;
}
static void v2_draw_one_sprite(uint16_t ds_val, int obj); // fwd (kept: debug single-blit)

// Ring trace of emu_early calls (branch decisions) — dumped on A2 divergence.
struct V2EmuTrace { int16_t vpx, vpy; int sdx, sdy; uint8_t branch; uint8_t cur; };
static V2EmuTrace v2_emu_ring[16];
static int v2_emu_ring_n = 0;
static uint8_t v2_emu_branch_pending = 0;   // set by branches below
static void v2_emu_trace(int16_t vpx, int16_t vpy, int sdx, int sdy) {
    V2EmuTrace& t = v2_emu_ring[v2_emu_ring_n++ % 16];
    t.vpx = vpx; t.vpy = vpy; t.sdx = sdx; t.sdy = sdy;
    t.branch = 0; t.cur = (uint8_t)v2_emu_cur;
}
static void v2_emu_trace_branch(uint8_t b) {
    if (v2_emu_ring_n) v2_emu_ring[(v2_emu_ring_n - 1) % 16].branch = b;
}
extern "C" void v2_emu_ring_dump(void) {
    static const char* bn[] = {"?", "chunk", "reinit", "scroll", "dirty"};
    int n = v2_emu_ring_n < 16 ? v2_emu_ring_n : 16;
    for (int i = 0; i < n; i++) {
        const V2EmuTrace& t = v2_emu_ring[(v2_emu_ring_n - n + i) % 16];
        fprintf(stderr, "  EMU-RING[-%d]: eff=(%d,%d) sd=(%d,%d) cur=%d br=%s\n",
            n - i, t.vpx, t.vpy, t.sdx, t.sdy, t.cur, bn[t.branch <= 4 ? t.branch : 0]);
    }
}

// Stage-1 emu sub-frame, early half — mirrors the orig sub-frame structure:
// work-page flip, the sub_1de05 dirty channel (latch spans from the background
// page + sprite re-blit), called right after v2_draw_tiles fills
// v2_render_buf with the clean tile render (= the background page).
void v2_emu_early(uint16_t ds_val) {
#ifdef V2_RENDER_FROM_SHADOW
    if (!v2_vm_in_frame) return;
#endif
    if (!v2_m2c_base || !myDrawInfo_v2) return;
    uint8_t* ds_base = v2_get_ds_base(ds_val);

    // 1. Background page snapshot (clean tiles just rendered).
    memcpy(v2_emu_bg, v2_render_buf, sizeof(v2_emu_bg));

    // 2. Effective viewport (vp + shake, sub_16775 clamp) and the 8-aligned
    //    page anchor. Content NEVER moves for sub-tile window motion (shake,
    //    pixel pan) — only the read window offset changes, like the orig CRTC.
    int xe, ye;
    v2_emu_eff(ds_base, &xe, &ye);
    int nbx = xe & ~7, nby = ye & ~7;
    int offx = xe - nbx, offy = ye - nby;   // 0..7

    // Chunk scenes (intro/menu/password, lvl flags & 0x42): the orig channel
    // paints the raw-chunk picture onto ALL pages at load (sub_10cd8 with the
    // three display offsets) and there is no tile dirty machinery — the work
    // page content is the background picture plus the sprite layers. Model:
    // full background copy + sprite canvas on all three pages.
    if (ds_base[DS_LEVEL_FLAGS] & 0x42) {
        v2_emu_trace((int16_t)xe, (int16_t)ye, 0, 0);
        v2_emu_trace_branch(1);
        v2_emu_base_x = nbx; v2_emu_base_y = nby;
        for (int p = 0; p < 3; p++) {
            memset(v2_emu_page[p], 0, sizeof(v2_emu_page[p]));
            v2_emu_page_fill_bg(v2_emu_page[p], offx, offy);
            v2_blit_to_page(v2_emu_page[p], offx, offy);
            v2_draw_sprites(ds_val);
        }
        v2_blit_to_display();
        v2_emu_valid = true;
        return;
    }

    int d8x = nbx - v2_emu_base_x, d8y = nby - v2_emu_base_y;
    if (!v2_emu_valid || d8x >= 320 || d8x <= -320 || d8y >= 176 || d8y <= -176) {
        // Fresh pages at the new anchor: clean tile background; WORK-role
        // pages additionally get the current sprite canvas (scene-entry spawn
        // tickets painted the work pages before the emu became valid). The
        // BACKGROUND role page stays sprite-free (f342 ghost class).
        v2_emu_base_x = nbx; v2_emu_base_y = nby;
        for (int p = 0; p < 3; p++) {
            v2_emu_page_render_full(v2_emu_page[p], ds_base);
            if (p != v2_emu_slot(ds_base, DS_PAGE_BG)) {
                v2_blit_to_page(v2_emu_page[p], offx, offy);
                v2_draw_sprites(ds_val);
            }
        }
        v2_blit_to_display();
        v2_emu_trace((int16_t)xe, (int16_t)ye, 9999, 9999);
        v2_emu_trace_branch(2);
    } else if (d8x != 0 || d8y != 0) {
        // Anchor moved by whole tiles: shift ALL pages by (−d8x,−d8y) and
        // fill the exposed bands from the current background render (the
        // screen-space equivalent of the orig edge channels painting the new
        // rows/columns on every page cursor at once).
        for (int p = 0; p < 3; p++) {
            uint8_t* pg = v2_emu_page[p];
            if (d8y > 0) {
                memmove(pg, pg + d8y * V2_EMU_W, (size_t)(V2_EMU_H - d8y) * V2_EMU_W);
            } else if (d8y < 0) {
                memmove(pg - d8y * V2_EMU_W, pg, (size_t)(V2_EMU_H + d8y) * V2_EMU_W);
            }
            if (d8x != 0) {
                for (int y = 0; y < V2_EMU_H; y++) {
                    uint8_t* row = pg + y * V2_EMU_W;
                    if (d8x > 0) memmove(row, row + d8x, (size_t)(V2_EMU_W - d8x));
                    else         memmove(row - d8x, row, (size_t)(V2_EMU_W + d8x));
                }
            }
            // Exposed bands: render map tiles directly in page space — the
            // orig edge channels paint whole map rows/columns (including the
            // beyond-window margin the bg render can't supply).
            {
                uint16_t fs_seg2 = *(uint16_t*)(ds_base + DS_SEG_FS);
                uint16_t tg_seg2 = *(uint16_t*)(ds_base + DS_SEG_TILEGFX);
#ifdef V2_RENDER_FROM_SHADOW
                uint8_t* fsb2 = v2_resolve_segment(fs_seg2);
                if (!fsb2) fsb2 = v2_m2c_base + ((uint32_t)fs_seg2 << 4);
                uint8_t* tgb2 = v2_resolve_segment(tg_seg2);
                if (!tgb2) tgb2 = v2_m2c_base + ((uint32_t)tg_seg2 << 4);
#else
                uint8_t* fsb2 = v2_m2c_base + ((uint32_t)fs_seg2 << 4);
                uint8_t* tgb2 = v2_m2c_base + ((uint32_t)tg_seg2 << 4);
#endif
                int col0 = nbx >> 3, row0 = nby >> 3;
                if (d8y != 0) {
                    int ry0 = d8y > 0 ? (V2_EMU_H - d8y) >> 3 : 0;
                    int ry1 = d8y > 0 ? V2_EMU_H >> 3 : (-d8y) >> 3;
                    for (int tr = ry0; tr < ry1; tr++)
                        for (int tc = 0; tc < V2_EMU_W >> 3; tc++)
                            v2_emu_render_tile(pg, ds_base, fsb2, tgb2,
                                               row0 + tr, col0 + tc, tc * 8, tr * 8);
                }
                if (d8x != 0) {
                    int cx0 = d8x > 0 ? (V2_EMU_W - d8x) >> 3 : 0;
                    int cx1 = d8x > 0 ? V2_EMU_W >> 3 : (-d8x) >> 3;
                    for (int tr = 0; tr < V2_EMU_H >> 3; tr++)
                        for (int tc = cx0; tc < cx1; tc++)
                            v2_emu_render_tile(pg, ds_base, fsb2, tgb2,
                                               row0 + tr, col0 + tc, tc * 8, tr * 8);
                }
            }
        }
        v2_emu_base_x = nbx; v2_emu_base_y = nby;
        v2_emu_trace((int16_t)xe, (int16_t)ye, d8x, d8y);
        v2_emu_trace_branch(3);
    } else {
        v2_emu_trace((int16_t)xe, (int16_t)ye, 0, 0);
        v2_emu_trace_branch(4);
    }
    v2_emu_valid = true;
    {
        // 4. sub_1de05 dirty channel: scan the visible 43x25 map window for
        //    render-map bit0 cells and latch-copy each 8x8 cell from the
        //    background page onto the CURRENT work page. Cell->screen mapping
        //    identical to v2_draw_tiles (scroll LUT + sub-tile pixel offset).
        uint16_t fs_seg = *(uint16_t*)(ds_base + DS_SEG_FS);
#ifdef V2_RENDER_FROM_SHADOW
        uint8_t* fs_base = v2_resolve_segment(fs_seg);
        if (!fs_base) fs_base = v2_m2c_base + ((uint32_t)fs_seg << 4);
#else
        uint8_t* fs_base = v2_m2c_base + ((uint32_t)fs_seg << 4);
#endif
        uint16_t scroll_row = *(uint16_t*)(ds_base + 0x2581);
        uint16_t scroll_col = *(uint16_t*)(ds_base + 0x257F);
        // sub_1de05 latch pass: every bit0 cell of the visible 43x25 window is
        // span-copied from the BACKGROUND page role [92FB] onto the DRAW page
        // role [92F7] (erases stale sprite pixels). NO sprite drawing happens
        // in the orig sub_1de05 (verified line-by-line: it only marks cells
        // via sub_1cd7b/7d and REP-MOVSB latches the spans) — redraw of
        // objects over refreshed cells is the sub_1dd9c layer's job (its
        // sub_1cdef scan tests the same fs bit0), mirrored by v2_emu_late.
        // Roles read BEFORE this sub-frame's v2_page_rotate_165aa rotation — same as
        // orig where sub_1de05 (0xBB) runs before sub_165aa (0xC0).
        uint8_t* drw = v2_emu_page[v2_emu_slot(ds_base, DS_PAGE_DRAW)];
        uint8_t* bgr = v2_emu_page[v2_emu_slot(ds_base, DS_PAGE_BG)];
        int trace_cells = 0;   // latch cells inside the traced object's area
        int16_t t_ox = 0, t_oy = 0;
        if (v2_objtrace_di != 0xFFFF) {
            t_ox = *(int16_t*)(ds_base + (uint16_t)(v2_objtrace_di + OBJ_SPRITE_X));
            t_oy = *(int16_t*)(ds_base + (uint16_t)(v2_objtrace_di + OBJ_SPRITE_Y));
            // Shadow FS letter-cell word dynamics (pairs with orig A2WP-FS).
            uint16_t row = (uint16_t)((t_oy + 8) >> 3);
            uint16_t rb = *(uint16_t*)(ds_base + (uint16_t)(row * 2 - LUT_ROW_BASE));
            uint16_t moff = (uint16_t)((rb + (uint16_t)((t_ox + 8) >> 3)) * 2u);
            uint16_t w = *(uint16_t*)(fs_base + moff);
            static uint16_t prev_w = 0xFFFF;
            if (w != prev_w) { v2_objtrace("e:bit", (int16_t)w, (int16_t)prev_w, moff, 0); prev_w = w; }
        }
        for (int rv = 0; rv < 25; rv++) {
            uint16_t rs = (uint16_t)(rv + scroll_row);
            uint16_t lut = (uint16_t)(rs * 2u - LUT_ROW_BASE);
            uint16_t row_base = *(uint16_t*)(ds_base + lut);
            for (int cv = 0; cv < 43; cv++) {
                uint16_t cs2 = (uint16_t)(cv + scroll_col);
                uint16_t moff = (uint16_t)((row_base + cs2) * 2u);
                if (!(*(uint16_t*)(fs_base + moff) & 1)) continue;
                if (v2_objtrace_di != 0xFFFF) {
                    int wx = (int)(cs2 * 8), wy = (int)(rs * 8);
                    if (wx >= t_ox - 8 && wx < t_ox + 40 && wy >= t_oy - 8 && wy < t_oy + 40)
                        trace_cells++;
                }
                // Page coordinates of the cell: world − anchor.
                int px0 = (int)cs2 * 8 - v2_emu_base_x;
                int py0 = (int)rs * 8 - v2_emu_base_y;
                for (int yy = 0; yy < 8; yy++) {
                    int py = py0 + yy;
                    if (py < 0 || py >= V2_EMU_H) continue;
                    int xa = px0 < 0 ? 0 : px0;
                    int xb = px0 + 8 > V2_EMU_W ? V2_EMU_W : px0 + 8;
                    if (xa < xb)
                        memcpy(drw + py * V2_EMU_W + xa, bgr + py * V2_EMU_W + xa, (size_t)(xb - xa));
                }
            }
        }
        if (trace_cells)
            v2_objtrace("e:latch", v2_emu_slot(ds_base, DS_PAGE_DRAW),
                        v2_emu_slot(ds_base, DS_PAGE_BG), trace_cells, 0);
    }
}

// sub_1df6a pixel channel: on a background-role change (sub_165aa tail) every
// bit1 cell of the visible window is span-copied from the NEW shown page
// [92F9] onto the NEW background page [92FB] — keeps the incoming background
// page's world content current before it serves latches. Called from
// v2_page_rotate_165aa right after the role update (same order as orig CALLF).
extern "C" void v2_emu_df6a(uint16_t ds_val) {
#ifdef V2_RENDER_FROM_SHADOW
    if (!v2_vm_in_frame) return;
#endif
    if (!v2_m2c_base || !myDrawInfo_v2 || !v2_emu_valid) return;
    uint8_t* ds_base = v2_get_ds_base(ds_val);
    if (ds_base[DS_LEVEL_FLAGS] & 0x42) return;   // chunk scenes: no tile machinery
    uint16_t fs_seg = *(uint16_t*)(ds_base + DS_SEG_FS);
#ifdef V2_RENDER_FROM_SHADOW
    uint8_t* fs_base = v2_resolve_segment(fs_seg);
    if (!fs_base) fs_base = v2_m2c_base + ((uint32_t)fs_seg << 4);
#else
    uint8_t* fs_base = v2_m2c_base + ((uint32_t)fs_seg << 4);
#endif
    uint16_t scroll_row = *(uint16_t*)(ds_base + 0x2581);
    uint16_t scroll_col = *(uint16_t*)(ds_base + 0x257F);
    uint8_t* shown = v2_emu_page[v2_emu_slot(ds_base, DS_PAGE_SHOWN)];
    uint8_t* bgr   = v2_emu_page[v2_emu_slot(ds_base, DS_PAGE_BG)];
    for (int rv = 0; rv < 25; rv++) {
        uint16_t rs = (uint16_t)(rv + scroll_row);
        uint16_t lut = (uint16_t)(rs * 2u - LUT_ROW_BASE);
        uint16_t row_base = *(uint16_t*)(ds_base + lut);
        for (int cv = 0; cv < 43; cv++) {
            uint16_t cs2 = (uint16_t)(cv + scroll_col);
            uint16_t moff = (uint16_t)((row_base + cs2) * 2u);
            if (!(*(uint16_t*)(fs_base + moff) & 2)) continue;   // bit1 (TEST 2)
            int px0 = (int)cs2 * 8 - v2_emu_base_x;
            int py0 = (int)rs * 8 - v2_emu_base_y;
            for (int yy = 0; yy < 8; yy++) {
                int py = py0 + yy;
                if (py < 0 || py >= V2_EMU_H) continue;
                int xa = px0 < 0 ? 0 : px0;
                int xb = px0 + 8 > V2_EMU_W ? V2_EMU_W : px0 + 8;
                if (xa < xb)
                    memcpy(bgr + py * V2_EMU_W + xa, shown + py * V2_EMU_W + xa, (size_t)(xb - xa));
            }
        }
    }
}

// Init-3-pass pixel steps (task #21): the orig level-init tail (sub_115d2 +
// the 4th block) runs FOUR full render sub-frames — spawn-ticket sprites get
// painted onto ALL rotation pages there (including the future background
// role). The v2 init mirrors carry only the DS side; call this from each
// pass: stage 0 = tiles+early (before the 165aa rotation), stage 1 = late
// (after it, before v2_late_sprites_1DD9C DECs). Forces the render gate open — the
// init mirror runs outside the frame-phase context.
// stage 0 = tiles+early; stage 1 = late_begin (arm page + cascade);
// stage 2 = late_end — call it AFTER the init site's v2_late_sprites_1DD9C so the
// cascade pixels land between them (same split as the render1/2/3 sites).
extern "C" void v2_emu_init_pass(uint16_t ds_val, int stage) {
#ifdef V2_RENDER_FROM_SHADOW
    bool save_in_frame = v2_vm_in_frame;
    v2_vm_in_frame = true;
#endif
    if (stage == 0) {
        v2_draw_tiles(ds_val);
        v2_emu_early(ds_val);
    } else if (stage == 1) {
        v2_emu_late_begin(ds_val);
    } else {
        v2_emu_late_end(ds_val);
    }
#ifdef V2_RENDER_FROM_SHADOW
    v2_vm_in_frame = save_in_frame;
#endif
}

// sub_1406d pixel channel (task #21 flame class): each anim-queue entry
// repaints the 2x2 tile block around (pos−8) onto BOTH the shown [92F9] and
// background [92FB] pages (orig draws two passes, eips 0x40E5.. and 0x413B..),
// bypassing the render-map bits entirely. clip bits (orig dx): 8=UL,4=UR,
// 2=LL,1=LR quadrant suppressed at viewport edges.
extern "C" void v2_emu_anim_tiles(uint16_t ds_val, uint16_t pos_x, uint16_t pos_y, uint16_t clip) {
#ifdef V2_RENDER_FROM_SHADOW
    if (!v2_vm_in_frame) return;
#endif
    if (!v2_m2c_base || !myDrawInfo_v2 || !v2_emu_valid) return;
    uint8_t* ds_base = v2_get_ds_base(ds_val);
    if (ds_base[DS_LEVEL_FLAGS] & 0x42) return;
    uint16_t fs_seg = *(uint16_t*)(ds_base + DS_SEG_FS);
    uint16_t tg_seg = *(uint16_t*)(ds_base + DS_SEG_TILEGFX);
    if (!fs_seg || !tg_seg) return;
#ifdef V2_RENDER_FROM_SHADOW
    uint8_t* fsb = v2_resolve_segment(fs_seg);
    if (!fsb) fsb = v2_m2c_base + ((uint32_t)fs_seg << 4);
    uint8_t* tgb = v2_resolve_segment(tg_seg);
    if (!tgb) tgb = v2_m2c_base + ((uint32_t)tg_seg << 4);
#else
    uint8_t* fsb = v2_m2c_base + ((uint32_t)fs_seg << 4);
    uint8_t* tgb = v2_m2c_base + ((uint32_t)tg_seg << 4);
#endif
    // Block geometry per orig: page rows start at [6E]=pos_y>>2 (slot of row
    // pos_y>>3) and the stored bp map offset covers rows (pos_y>>3)..+1,
    // columns (pos_x>>3)..+1 — the 2x2 block starts AT pos, not around it.
    int row0 = (int)pos_y >> 3, col0 = (int)pos_x >> 3;
    uint8_t* pages[2] = { v2_emu_page[v2_emu_slot(ds_base, DS_PAGE_SHOWN)],
                          v2_emu_page[v2_emu_slot(ds_base, DS_PAGE_BG)] };
    static const uint16_t qbit[4] = { 8, 4, 2, 1 };   // UL UR LL LR
    for (int pi = 0; pi < 2; pi++)
        for (int q = 0; q < 4; q++) {
            if (clip & qbit[q]) continue;
            int r = row0 + (q >> 1), c = col0 + (q & 1);
            v2_emu_render_tile(pages[pi], ds_base, fsb, tgb, r, c,
                               c * 8 - v2_emu_base_x, r * 8 - v2_emu_base_y);
        }
}

// #39 op_13/0x11 (orig loc_14396 "text menu redraw") - PAGE-accurate mirror.
// Orig semantics decoded via inverse LUT_PAGE_ROW (ds_static):
//   MOVSB VGA[0..0x1600) -> 0x2ADC == (role-value 0x00 page, world row 62)
//   MOVSB VGA[0..0x1600) -> 0x70BC == (role-value 0x34 page, world row 62)
//   STOSB #3/#4/#5 jointly zero the VGA head + ALL THREE pages outside the
//   two picture bands (role-value 0x68 page is zeroed entirely).
// Slots are fixed by role VALUE (slot = value/0x34), independent of the
// current rotation - so this is groove-independent, unlike the previous
// screen-space scr_row0 fix which read vp at opcode time (#39 instability).
extern "C" void v2_emu_op13_text_menu(uint16_t ds_val, const uint8_t* hud64) {
    uint8_t* ds_base = v2_get_ds_base(ds_val);
    if (!ds_base) return;
    const int WORLD_ROW = 62;   // inverse LUT: 0x2ADC/0x70BC = row 7*8+6
    for (int p = 0; p < 3; p++) {
        memset(v2_emu_page[p], 0, (size_t)V2_EMU_W * V2_EMU_H);
        if (p == 2) continue;   // role-value 0x68 page: zero only
        int pr0 = WORLD_ROW - v2_emu_base_y;
        for (int r = 0; r < 64; r++) {
            int pr = pr0 + r;
            if (pr < 0 || pr >= V2_EMU_H) continue;
            memcpy(v2_emu_page[p] + (size_t)pr * V2_EMU_W, hud64 + (size_t)r * 320, 320);
        }
    }
    // Screen representation = the shown window over the updated pages
    // (same math as v2_emu_shown), so FRAMESUM/display stay coherent.
    {
        int xe, ye;
        v2_emu_eff(ds_base, &xe, &ye);
        int offx = xe - v2_emu_base_x, offy = ye - v2_emu_base_y;
        if (offx < 0) offx = 0; if (offx > V2_EMU_W - 320) offx = V2_EMU_W - 320;
        if (offy < 0) offy = 0; if (offy > V2_EMU_H - 176) offy = V2_EMU_H - 176;
        uint8_t* pg = v2_emu_page[v2_emu_slot(ds_base, DS_PAGE_SHOWN)];
        for (int y = 0; y < 176; y++)
            memcpy(v2_render_buf + y * 320, pg + (size_t)(y + offy) * V2_EMU_W + offx, 320);
    }
    memcpy(v2_chunk_bg_backup, v2_render_buf, 320 * 176);
    v2_chunk_bg_valid = true;
    v2_emu_valid = true;
    v2_glyph_snap_valid = false;   // orig STOSB wiped the pages incl. the glyph layer
}

// Shown page of the current sub-frame — the A2 sensor compares against this.
// Assembles the 320x176 window (anchor + current effective offset) into a
// static frame buffer, like the orig CRTC unfold reads myOffset's window.
extern "C" const uint8_t* v2_emu_shown(uint16_t ds_val) {
    static uint8_t win[320 * 176];
    uint8_t* ds_base = v2_get_ds_base(ds_val);
    uint8_t* pg = v2_emu_page[v2_emu_slot(ds_base, DS_PAGE_SHOWN)];
    int xe, ye;
    v2_emu_eff(ds_base, &xe, &ye);
    int offx = xe - v2_emu_base_x, offy = ye - v2_emu_base_y;
    if (offx < 0) offx = 0; if (offx > V2_EMU_W - 320) offx = V2_EMU_W - 320;
    if (offy < 0) offy = 0; if (offy > V2_EMU_H - 176) offy = V2_EMU_H - 176;
    for (int y = 0; y < 176; y++)
        memcpy(win + y * 320, pg + (y + offy) * V2_EMU_W + offx, 320);
    return win;
}

// sub_11439/sub_16ded mirror hook (task #21): the orig level-entry init
// renders the visible 25 tile rows onto ALL THREE pages at once (sub_16ded:
// page cursors 9305/9307/9309 from 92F9/92FB/92F7, sub_16dc1 + sub_171dc per
// row) BEFORE the object spawn passes (sub_13ba5/13a0e) — so the pages start
// tile-clean and every sprite arrives later via live [114D] spawn tickets.
// This closes the "blind init window" honestly: no sprite seeding needed.
extern "C" void v2_emu_init_pages(uint16_t ds_val) {
    if (!v2_m2c_base || !myDrawInfo_v2) return;
    uint8_t* ds_base = v2_get_ds_base(ds_val);
    if (ds_base[DS_LEVEL_FLAGS] & 0x42) return;   // intro flags: orig skips sub_16ded
#ifdef V2_RENDER_FROM_SHADOW
    // v2_draw_tiles is gated on v2_vm_in_frame (skips seg000-hook contexts);
    // this runs on the v2 thread inside the level-init mirror — force the
    // gate open for the one init render.
    bool save_in_frame = v2_vm_in_frame;
    v2_vm_in_frame = true;
#endif
    v2_draw_tiles(ds_val);
#ifdef V2_RENDER_FROM_SHADOW
    v2_vm_in_frame = save_in_frame;
#endif
    memcpy(v2_emu_bg, v2_render_buf, sizeof(v2_emu_bg));
    int xe, ye;
    v2_emu_eff(ds_base, &xe, &ye);
    v2_emu_base_x = xe & ~7; v2_emu_base_y = ye & ~7;
    for (int p = 0; p < 3; p++)
        v2_emu_page_render_full(v2_emu_page[p], ds_base);
    v2_emu_valid = true;
}

// Stage-1 emu sub-frame, late half — the sub_1dd9c layer + the flagged-tile
// pass (sub_1c8f1 draws AFTER 1dd9c and clears bit0; call this BEFORE the
// phase runs v2_dirty_tile_scan_1C8F1 so the bits are still live).
static void v2_draw_sprites_impl(uint16_t ds_val, int late_gate, int only_obj);
// Cascade hook: when != 0xFFFF, v2_late_sprites_1DD9C (the DS/FS mirror loop) calls
// v2_draw_one_sprite_late(this, di) at the orig CALL cs:[bp+15CB] point for
// every object it decides to draw. This reproduces the orig SINGLE loop:
// gate → draw pixels → DEC [114D] → sub_1cd7d OR3 cells — so the sub_1cdef
// scan of the NEXT object in the same sub-frame sees the fresh OR3 bits
// (in-sub-frame cascade). A separate pixel pass before the mirror missed
// those bits (scan MISS where orig HIT — flame obj30 class, task #23).
extern "C" uint16_t v2_dd9c_pixel_ds = 0xFFFF;
extern "C" void v2_draw_one_sprite_late(uint16_t ds_val, int obj) {
    // The cascade fires inside v2_late_sprites_1DD9C, which the init mirrors run
    // OUTSIDE the frame-phase context — force the render gate like
    // v2_emu_init_pass does (armed v2_dd9c_pixel_ds IS the render context).
#ifdef V2_RENDER_FROM_SHADOW
    bool save_in_frame = v2_vm_in_frame;
    v2_vm_in_frame = true;
#endif
    v2_draw_sprites_impl(ds_val, 0, obj);
#ifdef V2_RENDER_FROM_SHADOW
    v2_vm_in_frame = save_in_frame;
#endif
}

void v2_emu_late_begin(uint16_t ds_val) {
#ifdef V2_RENDER_FROM_SHADOW
    if (!v2_vm_in_frame) return;
#endif
    if (!v2_m2c_base || !myDrawInfo_v2 || !v2_emu_valid) return;
    // Sprite/flagged layers land on the SHOWN page role [92F9] — the page
    // this sub-frame's 16775 flips to (draw, then show). Measured (BAND
    // parity probe): the orig sub-3 snapshot content always matches the page
    // painted by the SAME sub-frame's late layer, i.e. dd9c renders into
    // [92F9]; [92F7] (previous shown) only receives the 1de05 latches.
    uint8_t* ds_base = v2_get_ds_base(ds_val);
    {
        int xe, ye;
        v2_emu_eff(ds_base, &xe, &ye);
        v2_blit_to_page(v2_emu_page[v2_emu_slot(ds_base, DS_PAGE_SHOWN)],
                        xe - v2_emu_base_x, ye - v2_emu_base_y);
    }
    v2_dd9c_pixel_ds = ds_val;   // enable the per-object cascade in v2_late_sprites_1DD9C
}

static void v2_emu_late_end_tail_marker(uint16_t ds_val);
extern "C" void v2_glyphs_to_shown_page(uint16_t ds_val);
extern uint8_t v2_glyph_page_snapshot[0x370];
extern bool v2_glyph_snap_valid;
void v2_emu_late_end(uint16_t ds_val) {
#ifdef V2_RENDER_FROM_SHADOW
    if (!v2_vm_in_frame) return;
#endif
    if (!v2_m2c_base || !myDrawInfo_v2 || !v2_emu_valid) return;
    v2_dd9c_pixel_ds = 0xFFFF;
    uint8_t* ds_base = v2_get_ds_base(ds_val);
    (void)ds_base;
    // #39: enable dirty-tile erosion of the glyph snapshot for THIS pass only
    // (the shown-page late_end render). Tiles that changed since last sub-frame
    // erode the stale glyph cell before the snapshot is repainted below.
    v2_flagged_erode = true;
    v2_draw_flagged_tiles(ds_val);
    v2_flagged_erode = false;
    // #39: orig 1E0C7 paints the glyph cells onto the CURRENT page at this
    // exact point (after 1DD9C, before the flip) - but only on sub-frames
    // where the flush actually ran a full pass (dirty/throttle gates).
    {
        // #39 snapshot model: orig pages physically KEEP the glyph layer after
        // each 1E0C7 pass (no full bg blits in orig), while the v2 emu re-blits
        // page backgrounds every sub-frame. So: capture the buffer state at
        // each painted tick, and repaint THE SNAPSHOT (not the live buffer -
        // that would run ahead of the orig type-in) after every bg pass.
        extern bool v2_glyph_flush_painted;
        if (v2_glyph_flush_painted) {
            v2_glyph_flush_painted = false;
            uint8_t* ds_base2 = v2_get_ds_base(ds_val);
            // Erase cells the new reply no longer occupies: restore the clean
            // background (v2_emu_bg = this sub-frame tile render) into the shown
            // page at those 8x8 cells - matches orig repainting tiles under the
            // removed line. Applied to SHOWN each sub-frame so both rotation
            // pages get cleaned over two frames.
            {
                const uint8_t* nb = ds_base2 + DS_GLYPH_BUF; (void)nb;
                int xe, ye;
                v2_emu_eff(ds_base2, &xe, &ye);
                int ox = xe - v2_emu_base_x, oy = ye - v2_emu_base_y;
                uint8_t* pg = v2_emu_page[v2_emu_slot(ds_base2, DS_PAGE_SHOWN)];
                for (int pos = 0; pos < 0x370; pos++) {
                    if (!v2_glyph_prev_snapshot[pos] || nb[pos]) continue;  // erase only cells the shorter reply vacated
                    int cx = (pos % 40) * 8, cy = (pos / 40) * 8;
                    for (int k = 0; k < 8; k++) {
                        int py = cy + k + oy, sx = cx + ox;
                        if (py < 0 || py >= V2_EMU_H || sx < 0 || sx + 8 > V2_EMU_W) continue;
                        int sy = cy + k;
                        if (sy >= 176) continue;
                        memcpy(pg + (size_t)py * V2_EMU_W + sx, v2_emu_bg + (size_t)sy * 320 + cx, 8);
                    }
                }
            }
            memcpy(v2_glyph_page_snapshot, ds_base2 + DS_GLYPH_BUF, 0x370);
            memcpy(v2_glyph_prev_snapshot, v2_glyph_page_snapshot, 0x370);
            v2_glyph_snap_valid = false;
            for (int i = 0; i < 0x370; i++)
                if (v2_glyph_page_snapshot[i]) { v2_glyph_snap_valid = true; break; }
        }
        if (v2_glyph_snap_valid)
            v2_glyphs_to_shown_page(ds_val);
    }
    v2_blit_to_display();
    v2_emu_late_end_tail_marker(ds_val);
}

// #39: gated page-side glyph render. Orig sub_1E0C7 paints non-zero glyph
// cells (same 72-byte glyph format as v2_draw_ui) onto the CURRENT page;
// unconditional per-subframe painting made v2 run AHEAD of the intro
// type-in dialog, so this runs only when the flush pass really painted.
extern "C" void v2_glyphs_to_shown_page(uint16_t ds_val) {
    if (!v2_m2c_base || !myDrawInfo_v2 || !v2_emu_valid) return;
    uint8_t* ds_base = v2_get_ds_base(ds_val);
    if (!ds_base) return;
    const uint8_t* gl = v2_glyph_page_snapshot;
    int xe, ye;
    v2_emu_eff(ds_base, &xe, &ye);
    int ox = xe - v2_emu_base_x, oy = ye - v2_emu_base_y;
    uint8_t* pg = v2_emu_page[v2_emu_slot(ds_base, DS_PAGE_SHOWN)];
    for (int pos = 0; pos < 0x370; pos++) {
        uint8_t ch = gl[pos];
        if (ch == 0) continue;
        uint16_t glyph_index = (uint16_t)(ch - 0x10);
        int sx = (pos % 40) * 8 + ox;
        int sy = (pos / 40) * 8 + oy;
        const uint8_t* glyph = ds_base + (uint16_t)(0x687Du + glyph_index * 72u);
        for (int plane = 0; plane < 4; plane++) {
            for (int strip = 0; strip < 2; strip++) {
                uint8_t mask = glyph[0];
                const uint8_t* data = glyph + 1;
                int by = sy + strip * 4;
                if (mask) {
                    static const int bit_dx[8] = {0,4,0,4,0,4,0,4};
                    static const int bit_dy[8] = {0,0,1,1,2,2,3,3};
                    for (int b = 0; b < 8; b++) {
                        if (!(mask & (0x80 >> b))) continue;
                        int px = sx + bit_dx[b] + plane, py = by + bit_dy[b];
                        if (px >= 0 && px < V2_EMU_W && py >= 0 && py < V2_EMU_H)
                            pg[(size_t)py * V2_EMU_W + px] = data[b];
                    }
                }
                glyph += 9;
            }
        }
    }
}

static void v2_emu_late_end_tail_marker(uint16_t ds_val) {
    uint8_t* ds_base = v2_get_ds_base(ds_val);
    // Traced object: checksum its 32x32 page area (world − anchor) on all
    // three pages after the late layer.
    if (v2_objtrace_di != 0xFFFF) {
        int16_t ox = *(int16_t*)(ds_base + (uint16_t)(v2_objtrace_di + OBJ_SPRITE_X));
        int16_t oy = *(int16_t*)(ds_base + (uint16_t)(v2_objtrace_di + OBJ_SPRITE_Y));
        int px = (int)ox - v2_emu_base_x, py = (int)oy - v2_emu_base_y;
        int sums[3] = {0, 0, 0};
        for (int p = 0; p < 3; p++)
            for (int yy = 0; yy < 32; yy++)
                for (int xx = 0; xx < 32; xx++) {
                    int X = px + xx, Y = py + yy;
                    if (X >= 0 && X < V2_EMU_W && Y >= 0 && Y < V2_EMU_H)
                        sums[p] += v2_emu_page[p][Y * V2_EMU_W + X];
                }
        v2_objtrace("e:pg", (int16_t)sums[0], (int16_t)sums[1], (int16_t)sums[2],
                    v2_emu_slot(ds_base, DS_PAGE_DRAW) * 16 + v2_emu_slot(ds_base, DS_PAGE_SHOWN));
        // Display-side checksum of the same area (v2_render_buf holds the
        // full clean frame after the early full sprite pass) — tells whether
        // the display layer carries a DIFFERENT phase than the page layer.
        {
            int xe2, ye2;
            v2_emu_eff(ds_base, &xe2, &ye2);
            int sx = (int)ox - xe2, sy = (int)oy - ye2;
            int ds_sum = 0;
            for (int yy = 0; yy < 32; yy++)
                for (int xx = 0; xx < 32; xx++) {
                    int X = sx + xx, Y = sy + yy;
                    if (X >= 0 && X < 320 && Y >= 0 && Y < 176)
                        ds_sum += v2_render_buf[Y * 320 + X];
                }
            v2_objtrace("e:dsp", (int16_t)ds_sum, (int16_t)sx, (int16_t)sy, 0);
        }
    }
}

static void v2_draw_sprites_impl(uint16_t ds_val, int late_gate, int only_obj = -1);
void v2_draw_sprites(uint16_t ds_val) { v2_draw_sprites_impl(ds_val, 0); }
void v2_draw_sprites_late(uint16_t ds_val) { v2_draw_sprites_impl(ds_val, 1); }
// Single-object blit for the page-emu selective re-blit (sub_1de05 second half).
static void v2_draw_one_sprite(uint16_t ds_val, int obj) { v2_draw_sprites_impl(ds_val, 0, obj); }

// late_gate=1: repaint only what orig sub_1dd9c draws in this sub-frame —
// gates evaluated BEFORE v2_late_sprites_1DD9C's DS effects (DEC of [obj+0x114D]) and
// BEFORE v2_dirty_tile_scan_1C8F1 (which clears render-map bit0), matching the orig call
// order 1dd9c -> 1c8f1. sub_1cdef gate: clip the object's tile bbox to the
// viewport (ds:0x9168/0x916A) and scan its cells in the render map (FS) for
// bit0 (seg003 eip 0x634: TEST word fs:[si],1) — set means this sub-frame's
// sub_1c8f1 repaints the cell, so the sprite must be repainted over it.
static void v2_draw_sprites_impl(uint16_t ds_val, int late_gate, int only_obj) {
#ifdef V2_RENDER_FROM_SHADOW
    if (!v2_vm_in_frame) return;
#endif
    if (!v2_m2c_base || !myDrawInfo_v2) return;

    uint8_t* ds_base = v2_get_ds_base(ds_val);

    uint8_t* buf = v2_blit_target ? v2_blit_target : v2_render_buf;

    // Viewport origin — pixel scroll values.
    // Mirrors orig set_display_memory_addr (sub_16775) — apply x_some/y_some shake.
    // Sprites in orig are drawn at world_pos - vp_px in VGA buffer; CRTC then shifts
    // entire display by x_some via start address + pixel pan. v2 single-buffer:
    // bake the shake into the effective camera so all elements stay aligned.
    int16_t _vp_px = *(int16_t*)(ds_base + DS_VIEWPORT_X);
    int16_t _vp_py = *(int16_t*)(ds_base + DS_VIEWPORT_Y);
    int16_t _xs   = *(int16_t*)(ds_base + 0x39E);
    int16_t _ys   = *(int16_t*)(ds_base + 0x3A0);
    int16_t _xlvl = *(int16_t*)(ds_base + 0x25A4);
    int16_t _ylvl = *(int16_t*)(ds_base + 0x25A6);
    int viewport_x = (int)_vp_px + (int)_xs; if (viewport_x > (int)_xlvl) viewport_x = (int)_vp_px - (int)_xs;
    int viewport_y = (int)_vp_py + (int)_ys; if (viewport_y > (int)_ylvl) viewport_y = (int)_vp_py - (int)_ys;
    static int spr_dbg = 0; spr_dbg++;
    for (int obj = 0xFE; obj >= 0; obj -= 2) {
        if (only_obj >= 0 && obj != only_obj) continue;
        uint16_t flags = *(uint16_t*)(ds_base + obj + OBJ_SPRITE_FLAGS);

        // Must be active (bit 15) with bits 13-14 clear
        if (!(flags & 0x8000) || (flags & 0x6000)) continue;

        // Task #21 ring: v2 layer decisions for the traced object; d = checksum
        // of the FULL sprite data as THIS side reads them (shadow) — same size
        // formula as the orig-side probe in sub_1dd9c.
        if (obj == v2_objtrace_di) {
            uint16_t t_seg = *(uint16_t*)(ds_base + obj + OBJ_SPRITE_SEG);
            uint16_t t_off = *(uint16_t*)(ds_base + obj + OBJ_SPRITE_OFF);
            int t_type = flags & 7;
            int t_sz = (t_type == 1) ? 72 : (t_type == 4) ? 288
                       : 4 * (int)*(uint16_t*)(ds_base + obj + OBJ_STRIP_COUNT) * 9;
            if (t_sz < 1) t_sz = 1; if (t_sz > 4096) t_sz = 4096;
            uint8_t* t_base = v2_resolve_segment(t_seg);
            if (!t_base) t_base = v2_m2c_base + ((uint32_t)t_seg << 4);
            int t_h = 0;
            for (int t_i = 0; t_i < t_sz; t_i++) t_h += t_base[(uint16_t)(t_off - 1 + t_i)];
            int t_slot = 9;   // 9 = display buffer
            for (int t_p = 0; t_p < 3; t_p++)
                if (buf == v2_emu_page[t_p]) t_slot = t_p;
            v2_objtrace(late_gate ? "v:late" : "v:early",
                        *(int16_t*)(ds_base + obj + OBJ_SPRITE_X),
                        *(int16_t*)(ds_base + obj + OBJ_SPRITE_Y),
                        t_slot * 256 + ds_base[obj + OBJ_DIRTY_MODE], t_h);
        }

        if (late_gate) {
            // orig sub_1dd9c gates (eips 0x157F..0x1590), checked in orig order:
            // force flag / pending-redraw byte / sub_1cdef render-map scan.
            bool draw_it = false;
            int gate_code = 0;   // 1=force 2=rd 3=scan-hit (trace aid)
            if (ds_base[DS_SPRITE_FORCE] != 0) { draw_it = true; gate_code = 1; }  // TEST ds:9568h
            else if (ds_base[obj + OBJ_DIRTY_MODE] != 0) { draw_it = true; gate_code = 2; } // TEST byte [di+114Dh]
            else {
                // sub_1cdef: clip object's tile bbox to viewport, scan cells
                // in the render map (FS) for bit0. Exact replica (seg003
                // eips 0x5BF..0x647); the v2_late_sprites_1DD9C bounds mirror carries
                // the same math for its DS effects.
                int16_t cx = (int16_t)*(uint16_t*)(ds_base + obj + OBJ_SPRITE_X);
                int16_t dxv = (int16_t)*(uint16_t*)(ds_base + obj + OBJ_SPRITE_Y);
                int16_t si_h = (int16_t)((*(uint16_t*)(ds_base + obj + OBJ_STRIP_COUNT) >> 3) + 1);
                int16_t bp_w = si_h;
                if (!(cx & 7)) bp_w--;
                if (!(dxv & 7)) si_h--;
                cx >>= 3; dxv >>= 3;                                    // SAR — signed
                int16_t ax_h = si_h;
                int16_t si_off = 0;
                bool visible = true;
                int16_t vw = (int16_t)*(uint16_t*)(ds_base + 0x9168);
                if (cx < 0) { bp_w += cx; if (bp_w <= 0) visible = false; }
                else {
                    si_off += cx;
                    if (cx >= vw) visible = false;
                    else { int16_t ov = cx + bp_w - vw; if (ov > 0) bp_w -= ov; }
                }
                if (visible) {
                    int16_t vh = (int16_t)*(uint16_t*)(ds_base + 0x916A);
                    if (dxv < 0) { ax_h += dxv; if (ax_h <= 0) visible = false; }
                    else {
                        if (dxv >= vh) visible = false;
                        else {
                            uint16_t bx = (uint16_t)dxv << 1;
                            si_off += (int16_t)*(uint16_t*)(ds_base + (uint16_t)(bx - LUT_ROW_BASE));
                            int16_t ov = dxv + ax_h - vh; if (ov > 0) ax_h -= ov;
                        }
                    }
                }
                if (visible) {
                    uint16_t fs_seg = *(uint16_t*)(ds_base + DS_SEG_FS);
#ifdef V2_RENDER_FROM_SHADOW
                    uint8_t* fs_base = v2_resolve_segment(fs_seg);
                    if (!fs_base) fs_base = v2_m2c_base + ((uint32_t)fs_seg << 4);
#else
                    uint8_t* fs_base = v2_m2c_base + ((uint32_t)fs_seg << 4);
#endif
                    // loc_1ce58 (eips 0x628-0x643): dx = (vw - bp) doubled
                    // (ADD dx,dx @0x62E) and si doubled (ADD si,si @0x630) —
                    // si becomes the BYTE offset (row_base+col)*2, cells step
                    // ADD si,2, row tail ADD si,dx. Below si_w stays in WORD
                    // units with off = (si_w+col)*2 and si_w += bp+stride
                    // per row — byte-for-byte the same addresses.
                    uint16_t si_w = (uint16_t)si_off;
                    int16_t stride = vw - bp_w;
                    bool hit = false;
                    for (int16_t row = 0; row < ax_h && !hit; row++) {
                        for (int16_t col = 0; col < bp_w; col++) {
                            uint16_t off = (uint16_t)(si_w + col) * 2u;
                            if (*(uint16_t*)(fs_base + off) & 1) { hit = true; break; }
                        }
                        si_w = (uint16_t)(si_w + bp_w + stride);
                    }
                    draw_it = hit;
                    if (hit) gate_code = 3;
                }
            }
            // Gate-fact trace: slot*4096 + gate*256 + [114D] — logged for the
            // traced object whether it draws or not (v:gate = decision point).
            // d = the FS cell word of the object's first cell at scan time.
            if (obj == v2_objtrace_di) {
                int t_slot = 9;
                for (int t_p = 0; t_p < 3; t_p++)
                    if (buf == v2_emu_page[t_p]) t_slot = t_p;
                int fs_word = -1;
                {
                    uint16_t fs_seg2 = *(uint16_t*)(ds_base + DS_SEG_FS);
                    uint8_t* fsb2 = v2_resolve_segment(fs_seg2);
                    if (!fsb2) fsb2 = v2_m2c_base + ((uint32_t)fs_seg2 << 4);
                    int16_t oy2 = (int16_t)*(uint16_t*)(ds_base + obj + OBJ_SPRITE_Y);
                    int16_t ox2 = (int16_t)*(uint16_t*)(ds_base + obj + OBJ_SPRITE_X);
                    uint16_t row2 = (uint16_t)((oy2 + 8) >> 3);
                    uint16_t rb2 = *(uint16_t*)(ds_base + (uint16_t)(row2 * 2 - LUT_ROW_BASE));
                    uint16_t moff2 = (uint16_t)((rb2 + (uint16_t)((ox2 + 8) >> 3)) * 2u);
                    fs_word = *(uint16_t*)(fsb2 + moff2);
                }
                v2_objtrace("v:gate",
                            *(int16_t*)(ds_base + obj + OBJ_SPRITE_X),
                            *(int16_t*)(ds_base + obj + OBJ_SPRITE_Y),
                            t_slot * 4096 + gate_code * 256 + ds_base[obj + OBJ_DIRTY_MODE],
                            fs_word);
            }
            if (!draw_it) continue;
        }

        int type = flags & 7;

        {
            static int spr_printed = 0;
            uint16_t cur_lvl = *(uint16_t*)(ds_base + DS_LEVEL);
            if (spr_printed < 15 && cur_lvl < 38) {
                spr_printed++;
                printf("V2-SPR[l%d]: obj=%02x fl=%04x t=%d xy=(%d,%d) seg=%04x off=%04x scr=(%d,%d)\n",
                       cur_lvl, obj, flags, type,
                       *(int16_t*)(ds_base + obj + OBJ_SPRITE_X),
                       *(int16_t*)(ds_base + obj + OBJ_SPRITE_Y),
                       *(uint16_t*)(ds_base + obj + OBJ_SPRITE_SEG),
                       *(uint16_t*)(ds_base + obj + OBJ_SPRITE_OFF),
                       viewport_x, viewport_y);
            }
        }

        // Dispatch table at cs:0x15CB: only types 1, 2, 4 have renderers.
        // Type 1 → cs:0x0648 (seg003_648_proc, 8×8)
        // Type 2 → cs:0x1078 (loc_1d8a8, dynamic size)
        // Type 4 → cs:0x0B82 (sub_1d3b2, 16×16)
        // Types 0,3,5,6,7 → cs:0x0000 (not used)
        if (type != 1 && type != 2 && type != 4) continue;

        int16_t world_x = *(int16_t*)(ds_base + obj + OBJ_SPRITE_X);
        int16_t world_y = *(int16_t*)(ds_base + obj + OBJ_SPRITE_Y);

        int sx0 = world_x - viewport_x;
        int sy0 = world_y - viewport_y;

        uint16_t sprite_off = *(uint16_t*)(ds_base + obj + OBJ_SPRITE_OFF);
        uint16_t sprite_seg = *(uint16_t*)(ds_base + obj + OBJ_SPRITE_SEG);
        if (!sprite_seg) continue;

        // Determine format parameters per type:
        //   num_strips:    strips per plane section (= rows / rows_per_strip)
        //   rows_per_strip: VGA rows each strip covers
        //   bytes_per_row:  data bytes per row within a strip
        int num_strips, rows_per_strip, bytes_per_row;
        if (type == 1) {
            // seg003_648_proc: 8×8 sprite, 72 bytes total
            // 2 strips/section, 4 rows/strip (DI += 0x158), 2 bytes/row
            num_strips = 2;
            rows_per_strip = 4;
            bytes_per_row = 2;
        } else if (type == 2) {
            // loc_1d8a8: dynamic size, CX = ds:[obj+0x0C4D]
            // CX strips/section, 1 row/strip (DI += 0x56), 8 bytes/row
            num_strips = (int)*(uint16_t*)(ds_base + obj + OBJ_STRIP_COUNT);
            if (num_strips <= 0) continue;
            rows_per_strip = 1;
            bytes_per_row = 8;
        } else { // type == 4
            // sub_1d3b2: 16×16 sprite, 288 bytes total
            // 8 strips/section, 2 rows/strip (DI += 0x0AC), 4 bytes/row
            num_strips = 8;
            rows_per_strip = 2;
            bytes_per_row = 4;
        }

        // Horizontal flip: bit 9 (0x200) of flags.
        // All three type renderers (seg003_648_proc, sub_1d3b2, loc_1d8a8)
        // check this flag and branch to mirrored rendering paths.
        bool hflip = (flags & 0x200) != 0;

        // Page-lane detection: writing into an emu page (world-anchored 328x184)
        // vs the 320x176 screen buffer. The page lane must reproduce the orig
        // write band exactly (incl. off-window pixels); the screen lane keeps
        // per-pixel window clipping (equivalent inside the window).
        bool to_page = false;
        for (int t_p = 0; t_p < 3; t_p++)
            if (buf == v2_emu_page[t_p]) to_page = true;

        // Coarse bounds check — sprite pixel size.
        // Skipped for the page lane: page writes use the exact orig clips
        // below (raw ds:44/46, no shake); the shaken-window coarse check
        // would drop sprites orig draws (shake delta / off-window band).
        int sprite_h = num_strips * rows_per_strip;
        int sprite_w = bytes_per_row * 4;  // 4 planes
        if (!to_page)
            if (sx0 >= 320 || sx0 < -sprite_w || sy0 >= 176 || sy0 < -sprite_h) continue;

        // Sprite data: resolve segment to shadow buffer, add offset.
        // sprite_off = 1-based offset to first data byte; mask at offset-1.
#ifdef V2_RENDER_FROM_SHADOW
        uint8_t* seg_base = v2_resolve_segment(sprite_seg);
        if (!seg_base) continue;
        uint8_t* sprite = seg_base + sprite_off - 1;
        {
            static int cmp_mismatch = 0;
            uint16_t cur_lvl = *(uint16_t*)(ds_base + DS_LEVEL);
            // real-vs-shadow compare only meaningful when orig updates real
            // memory (V2_ONLY: dynamic segments in the snapshot buffer stay 0).
            if (v2_vm_get_real_ds() && cur_lvl < 38 && cmp_mismatch < 10) {
                uint8_t* real_sprite = v2_m2c_base + ((uint32_t)sprite_seg << 4) + sprite_off - 1;
                if (memcmp(sprite, real_sprite, 32) != 0) {
                    cmp_mismatch++;
                    printf("V2-SPRMIS: obj=%02x seg=%04x off=%04x shadow_ptr=%s\n",
                           obj, sprite_seg, sprite_off,
                           (seg_base != (v2_m2c_base + ((uint32_t)sprite_seg << 4))) ? "SHADOW" : "REAL_FALLBACK");
                    printf("  shd: %02x%02x%02x%02x %02x%02x%02x%02x\n",
                           sprite[0],sprite[1],sprite[2],sprite[3],sprite[4],sprite[5],sprite[6],sprite[7]);
                    printf("  rea: %02x%02x%02x%02x %02x%02x%02x%02x\n",
                           real_sprite[0],real_sprite[1],real_sprite[2],real_sprite[3],
                           real_sprite[4],real_sprite[5],real_sprite[6],real_sprite[7]);
                }
            }
        }
#else
        uint32_t sprite_linear = ((uint32_t)sprite_seg << 4) + sprite_off - 1;
        uint8_t* sprite = v2_m2c_base + sprite_linear;
#endif

        // Column formula: sx0 + N*4 + section (normal) or
        // sx0 + (sprite_w-1) - (N*4 + section) (flipped).
        // sx(col) computes the screen x for a given data column.
        auto sx = [&](int col) -> int {
            return hflip ? sx0 + sprite_w - 1 - col : sx0 + col;
        };

        // Page-lane exact bands for types 1/4 (orig seg003_648_proc /
        // sub_1d3b2): raw ds:44/46 clips (no shake), 4-px column masks and
        // strip-granular top/bot clip for type 4, full 8x8 write for type 1
        // (orig has NO masks / no [114D] cut-in there — only object bounds;
        // up to 7 off-window pixels are written).
        int t14_top_strips = 0, t14_bot_strips = 0;
        uint8_t t14_mask = 0xFF;
        if (to_page && (type == 1 || type == 4)) {
            int16_t vp_raw  = (int16_t)*(uint16_t*)(ds_base + DS_VIEWPORT_X);
            int16_t vpy_raw = (int16_t)*(uint16_t*)(ds_base + DS_VIEWPORT_Y);
            if (type == 1) {
                // seg003_648_proc eips 0x668..0x68E: pure bounds.
                if (world_x >= vp_raw + 0x140) continue;   // JGE 1d154
                if (world_x <= vp_raw - 0x07) continue;    // JLE (ax -= 0x147)
                if (world_y >= vpy_raw + 0x0B0) continue;  // JGE 1d154
                if (world_y <  vpy_raw - 0x07) continue;   // JL (ax -= 0xB7)
            } else {
                // sub_1d3b2 eips 0xBA2..0xC3A.
                static const uint16_t v2_t4_mask_r[4] = {   // cs:[0E92]
                    0xFFFF,0x77EE,0x33CC,0x1188};
                static const uint16_t v2_t4_mask_l[4] = {   // cs:[0E9A]
                    0xFFFF,0xEE77,0xCC33,0x8811};
                uint16_t mask16 = 0xFFFF;
                if (world_x >= vp_raw + 0x140) continue;             // JGE 1d6b1
                if (world_x >= vp_raw + 0x131)                       // JL 1d3fd
                    mask16 = v2_t4_mask_r[(world_x - (vp_raw + 0x131)) >> 2];
                if (world_x <= vp_raw - 0x0F) continue;              // JLE 1d6b1
                if (world_x < vp_raw)                                // JGE 1d425
                    mask16 = v2_t4_mask_l[(vp_raw - world_x) >> 2];
                if (world_y >= vpy_raw + 0x0B0) continue;            // JGE 1d6b1
                if (world_y >= vpy_raw + 0x0A1)                      // JL 1d44c
                    t14_bot_strips = (world_y - (vpy_raw + 0x0A1)) >> 1; // SHR 1
                if (world_y < vpy_raw - 0x0F) continue;              // JL 1d6b1
                if (world_y <= vpy_raw)                              // JG 1d471
                    t14_top_strips = ((vpy_raw - world_y) & 0xFFFE) >> 1; // AND FFFE
                t14_mask = hflip ? (uint8_t)(mask16 >> 8)            // loc_1d6d2 SHR ...,8
                                 : (uint8_t)(mask16 & 0xFF);         // AND ...,0FFh
            }
        }

        if (type == 2 && to_page) {
            // Exact orig write band — loc_1d8a8 (seg003 eip 0x1078..0x1367).
            // Clips are against RAW ds:44/46 (no shake; CRTC pan shifts the
            // window, not the buffer writes). Horizontal clip is a column
            // mask at 4-px granularity (cs:[1379] right / cs:[138B] left,
            // indexed by (edge_delta)>>2; hflip uses the high mask byte via
            // SHR word_1C830,8) — so up to 3 off-window pixels ARE written.
            // Vertical clip is per-row: top = vpy−y (y<=vpy), bot =
            // y−(vpy+0x91) (y>=vpy+0x91); with bot>0 the last drawn row is
            // always vpy+0xB0 — one row below the window, also written.
            // Data: 4 plane blocks of 9*[C4D] bytes, row = [ctrl][d0..d7];
            // ctrl bit b → column k=7−b, color d[k] (jpt_1DA02 writers).
            // Plane rotation by (x+0x20)&3 (jpt_1D9E8 cases + INC DI at the
            // 3→0 wrap) is transparent in world coords: block j lands on
            // world X = x + 4k + j; hflip (jpt_1DBE3/1DBFE: reversed plane
            // order, mirrored write offsets [di+b] ← data [si+7−b]) lands on
            // world X = x + 31 − (4k+j). [114D]=2 side effects and the
            // sub_1cd7d bitmap call live in the v2_late_sprites_1DD9C DS mirror.
            static const uint16_t v2_t2_mask_r[8] = {  // cs:[1379]
                0xFFFF,0x7FFE,0x3FFC,0x1FF8,0x0FF0,0x07E0,0x03C0,0x0180};
            static const uint16_t v2_t2_mask_l[8] = {  // cs:[138B]
                0xFFFF,0xFE7F,0xFC3F,0xF81F,0xF00F,0xE007,0xC003,0x8001};
            int16_t vp_raw  = (int16_t)*(uint16_t*)(ds_base + DS_VIEWPORT_X);
            int16_t vpy_raw = (int16_t)*(uint16_t*)(ds_base + DS_VIEWPORT_Y);
            int rows_total = num_strips;              // ds:[obj+0xC4D]
            uint16_t mask16 = 0xFFFF;
            if (world_x >= vp_raw + 0x140) continue;              // JGE 1db98
            if (world_x >= vp_raw + 0x121)                        // JL 1d8f3
                mask16 = v2_t2_mask_r[(world_x - (vp_raw + 0x121)) >> 2];
            if (world_x <= vp_raw - 0x1F) continue;               // JLE 1db98
            if (world_x < vp_raw)                                 // JGE 1d91b
                mask16 = v2_t2_mask_l[(vp_raw - world_x) >> 2];
            int top = 0, bot = 0;
            if (world_y >= vpy_raw + 0x0B0) continue;             // JGE 1db98
            if (world_y >= vpy_raw + 0x091)                       // JL 1d93d
                bot = world_y - (vpy_raw + 0x091);
            if (world_y < vpy_raw - 0x1F) continue;               // JL 1db98
            if (world_y <= vpy_raw)                               // JG 1d95b
                top = vpy_raw - world_y;
            int cx_rows = rows_total - top - bot;
            if (cx_rows <= 0) continue;  // orig LOOP would wrap; unreachable geometry
            uint8_t mask8 = hflip ? (uint8_t)(mask16 >> 8)        // SHR ...,8
                                  : (uint8_t)(mask16 & 0xFF);     // AND ...,0FFh
            for (int j = 0; j < 4; j++) {
                const uint8_t* blk = sprite + 9 * top + j * 9 * rows_total;
                for (int r = 0; r < cx_rows; r++) {
                    uint8_t ctrl = (uint8_t)(blk[r * 9] & mask8);
                    if (!ctrl) continue;
                    int wy = world_y + top + r;
                    for (int b = 7; b >= 0; b--) {
                        if (!(ctrl & (1 << b))) continue;
                        int k = 7 - b;
                        int s = 4 * k + j;
                        int wx = hflip ? (world_x + 31 - s) : (world_x + s);
                        v2_put_pixel(buf, wx - viewport_x, wy - viewport_y,
                                     blk[r * 9 + 1 + k]);
                    }
                }
            }
            continue;
        }

        uint8_t* ptr = sprite;
        for (int section = 0; section < 4; section++) {
            int plane = section;
            for (int strip = 0; strip < num_strips; strip++) {
                uint8_t mask = ptr[0];
                uint8_t* data = ptr + 1;
                int base_y = sy0 + strip * rows_per_strip;

                // Page lane, type 4: strip-granular top/bot clip (orig skips
                // the clipped strips' data via the cs:[3D] 9m LUT — same as
                // stepping ptr) and the 4-px column mask on the ctrl byte.
                if (to_page && type == 4) {
                    if (strip < t14_top_strips ||
                        strip >= num_strips - t14_bot_strips) { ptr += 9; continue; }
                    mask &= t14_mask;
                }

                if (mask) {
                    if (type == 1) {
                        // Type 1 (jpt_1CF4E): 4 rows × 2 bytes per row
                        // Mask: 76→row0, 54→row1, 32→row2, 10→row3
                        if (mask & 0x80) v2_put_pixel(buf, sx(0*4 + plane), base_y + 0, data[0]);
                        if (mask & 0x40) v2_put_pixel(buf, sx(1*4 + plane), base_y + 0, data[1]);
                        if (mask & 0x20) v2_put_pixel(buf, sx(0*4 + plane), base_y + 1, data[2]);
                        if (mask & 0x10) v2_put_pixel(buf, sx(1*4 + plane), base_y + 1, data[3]);
                        if (mask & 0x08) v2_put_pixel(buf, sx(0*4 + plane), base_y + 2, data[4]);
                        if (mask & 0x04) v2_put_pixel(buf, sx(1*4 + plane), base_y + 2, data[5]);
                        if (mask & 0x02) v2_put_pixel(buf, sx(0*4 + plane), base_y + 3, data[6]);
                        if (mask & 0x01) v2_put_pixel(buf, sx(1*4 + plane), base_y + 3, data[7]);
                    } else if (type == 2) {
                        // Type 2 (jpt_1DA02): 1 row × 8 bytes
                        // Mask: bit7→data[0], ..., bit0→data[7]
                        if (mask & 0x80) v2_put_pixel(buf, sx(0*4 + plane), base_y, data[0]);
                        if (mask & 0x40) v2_put_pixel(buf, sx(1*4 + plane), base_y, data[1]);
                        if (mask & 0x20) v2_put_pixel(buf, sx(2*4 + plane), base_y, data[2]);
                        if (mask & 0x10) v2_put_pixel(buf, sx(3*4 + plane), base_y, data[3]);
                        if (mask & 0x08) v2_put_pixel(buf, sx(4*4 + plane), base_y, data[4]);
                        if (mask & 0x04) v2_put_pixel(buf, sx(5*4 + plane), base_y, data[5]);
                        if (mask & 0x02) v2_put_pixel(buf, sx(6*4 + plane), base_y, data[6]);
                        if (mask & 0x01) v2_put_pixel(buf, sx(7*4 + plane), base_y, data[7]);
                    } else { // type == 4
                        // Type 4 (jpt_1d514): 2 rows × 4 bytes per row
                        // Mask: 7654→row0, 3210→row1
                        if (mask & 0x80) v2_put_pixel(buf, sx(0*4 + plane), base_y + 0, data[0]);
                        if (mask & 0x40) v2_put_pixel(buf, sx(1*4 + plane), base_y + 0, data[1]);
                        if (mask & 0x20) v2_put_pixel(buf, sx(2*4 + plane), base_y + 0, data[2]);
                        if (mask & 0x10) v2_put_pixel(buf, sx(3*4 + plane), base_y + 0, data[3]);
                        if (mask & 0x08) v2_put_pixel(buf, sx(0*4 + plane), base_y + 1, data[4]);
                        if (mask & 0x04) v2_put_pixel(buf, sx(1*4 + plane), base_y + 1, data[5]);
                        if (mask & 0x02) v2_put_pixel(buf, sx(2*4 + plane), base_y + 1, data[6]);
                        if (mask & 0x01) v2_put_pixel(buf, sx(3*4 + plane), base_y + 1, data[7]);
                    }
                }
                ptr += 9;
            }
        }
    }
}

// ============================================================================
// v2_draw_flagged_tiles: Redraws flagged tiles with mask transparency.
//
// Reimplements sub_1c8f1 from seg003. Runs AFTER sprites — draws foreground
// tile pixels over sprites using per-pixel mask from the GS segment.
//
// Scans the same 25×43 visible tile grid as v2_draw_tiles.
// Condition: tile entry has both bit 0 (dirty) AND bit 3 (redraw) set.
//
// Data sources:
//   Tile map:      FS segment (ds:0x2E69)
//   Tile graphics: segment at ds:0x2E5F, 64 bytes per tile
//   Tile mask:     GS segment (ds:0x2E61), 8 bytes per tile
//   Mask offset:   (tile_entry & 0xFFC0) >> 3
//
// Tile data format: same 64 bytes as v2_draw_tiles (4 planes × 2 strips × 8 bytes).
// Mask: 8 bytes, one per (plane, strip) pair. Each mask byte controls 4 rows × 2 bytes:
//   bits 76→row0(b0,b1), 54→row1, 32→row2, 10→row3.
//   Bit=1 → draw pixel, bit=0 → transparent (keep underlying sprite/tile).
//
// Flip flags (bits 4,5) affect screen position, not data/mask indexing.
// Uses jpt_1c9b1 (no flip), jpt_1caa7 (hflip), jpt_1cba1 (vflip), jpt_1cc9b (both).
// ============================================================================

// Render one 8x8 map tile with its per-plane transparency mask to `buf` at
// screen (screen_x, screen_y). v2_put_pixel applies the emu-page blit
// translation (v2_pp_dx/dy/pitch) when a page is the target. Shared by
// v2_draw_flagged_tiles and the dirty channel (v2_masked_tile_1C939) so both
// decode identically (orig sub_1C939 jpt_1c9b1/caa7/cba1/cc9b flip dispatch).
static void v2_render_tile_masked(uint8_t* buf, const uint8_t* tgfx_base,
                                  const uint8_t* gs_base, uint16_t tile_entry,
                                  int screen_x, int screen_y) {
    uint16_t tile_gfx_off = tile_entry & 0xFFC0;
    bool hflip = (tile_entry & 0x10) != 0;
    bool vflip = (tile_entry & 0x20) != 0;
    const uint8_t* tile = tgfx_base + tile_gfx_off;
    const uint8_t* mask_data = gs_base + (tile_gfx_off >> 3);
    for (int ty = 0; ty < 8; ty++) {
        int sy = screen_y + (vflip ? 7 - ty : ty);
        for (int tx = 0; tx < 8; tx++) {
            int sx = screen_x + (hflip ? 7 - tx : tx);
            int plane = tx & 3;       // tx % 4
            int byte_idx = tx >> 2;   // tx / 4 (0 or 1)
            int strip = ty >> 2;      // ty / 4 (0 or 1)
            int row = ty & 3;         // ty % 4
            int mb = plane * 2 + strip;
            int mbit = 7 - (row * 2 + byte_idx);
            if (!(mask_data[mb] & (1 << mbit))) continue;
            uint8_t color = tile[plane * 16 + strip * 8 + row * 2 + byte_idx];
            v2_put_pixel(buf, sx, sy, color);
        }
    }
}

void v2_draw_flagged_tiles(uint16_t ds_val) {
#ifdef V2_RENDER_FROM_SHADOW
    if (!v2_vm_in_frame) return;
#endif
    if (!myDrawInfo_v2 || !v2_m2c_base) return;

    uint8_t* ds_base = v2_get_ds_base(ds_val);
    uint8_t* buf = v2_blit_target ? v2_blit_target : v2_render_buf;

    uint16_t fs_seg = *(uint16_t*)(ds_base + DS_SEG_FS);
    uint16_t tgfx_seg = *(uint16_t*)(ds_base + DS_SEG_TILEGFX);
    uint16_t gs_seg = *(uint16_t*)(ds_base + DS_SEG_GS);
    if (!fs_seg || !tgfx_seg || !gs_seg) return;

#ifdef V2_RENDER_FROM_SHADOW
    uint8_t* fs_base = v2_resolve_segment(fs_seg);
    uint8_t* tgfx_base = v2_resolve_segment(tgfx_seg);
#else
    uint8_t* fs_base = v2_m2c_base + ((uint32_t)fs_seg << 4);
    uint8_t* tgfx_base = v2_m2c_base + ((uint32_t)tgfx_seg << 4);
#endif
#ifdef V2_RENDER_FROM_SHADOW
    uint8_t* gs_base = v2_vm_is_gs_shadow_valid()
        ? v2_vm_get_shadow_gs()
        : v2_m2c_base + ((uint32_t)gs_seg << 4);
#else
    uint8_t* gs_base = v2_m2c_base + ((uint32_t)gs_seg << 4);
#endif

    uint16_t scroll_x = *(uint16_t*)(ds_base + 0x2581);
    uint16_t scroll_y = *(uint16_t*)(ds_base + 0x257F);

    // Sub-tile pixel offset (same as v2_draw_tiles).
    // Mirrors orig set_display_memory_addr (sub_16775) — apply x_some/y_some shake.
    int16_t vp_px = *(int16_t*)(ds_base + DS_VIEWPORT_X);
    int16_t vp_py = *(int16_t*)(ds_base + DS_VIEWPORT_Y);
    int16_t x_some = *(int16_t*)(ds_base + 0x39E);
    int16_t y_some = *(int16_t*)(ds_base + 0x3A0);
    int16_t x_lvl  = *(int16_t*)(ds_base + 0x25A4);
    int16_t y_lvl  = *(int16_t*)(ds_base + 0x25A6);
    int x_eff = (int)vp_px + (int)x_some; if (x_eff > (int)x_lvl) x_eff = (int)vp_px - (int)x_some;
    int y_eff = (int)vp_py + (int)y_some; if (y_eff > (int)y_lvl) y_eff = (int)vp_py - (int)y_some;
    int pix_off_x = x_eff & 7;
    int pix_off_y = y_eff & 7;
    int extra_tile_x = (x_eff >> 3) - ((int)vp_px >> 3);
    int extra_tile_y = (y_eff >> 3) - ((int)vp_py >> 3);

    for (int row_vis = 0; row_vis < 25; row_vis++) {
        uint16_t row_scrolled = (uint16_t)(row_vis + scroll_x + extra_tile_y);
        // No `row_scrolled >= 64` clamp — orig sub_1c8f1 (flagged-tile render)
        // doesn't clip rows beyond LUT; v2 hardcode caused tiles missing on
        // large maps when scrolled past row 64.

        uint16_t lut_off = (uint16_t)(row_scrolled * 2u - LUT_ROW_BASE);
        uint16_t row_base = *(uint16_t*)(ds_base + lut_off);

        for (int col_vis = 0; col_vis < 43; col_vis++) {
            uint16_t col_scrolled = (uint16_t)(col_vis + scroll_y + extra_tile_x);
            uint16_t tile_map_off = (uint16_t)((row_base + col_scrolled) * 2u);
            uint16_t tile_entry = *(uint16_t*)(fs_base + tile_map_off);

            // #39 dirty-tile erosion (only on the late_end shown pass): if the
            // visible tile at this cell changed since the previous sub-frame,
            // the scene content there was redrawn — orig's dirty channel would
            // have overwritten any glyph on the page, so erode the matching
            // glyph-snapshot cell (glyph grid 40x22 aligns with tile cells).
            if (v2_flagged_erode && col_vis < 40 && row_vis < 22) {
                static uint16_t prev_ftile[25 * 43];
                static bool prev_ftile_valid = false;
                int tcell = row_vis * 43 + col_vis;
                if (prev_ftile_valid && tile_entry != prev_ftile[tcell]) {
                    int gpos = row_vis * 40 + col_vis;
                    v2_glyph_page_snapshot[gpos] = 0;
                    v2_glyph_prev_snapshot[gpos] = 0;
                }
                prev_ftile[tcell] = tile_entry;
                // mark valid once the last gated cell (21,39) has been stored,
                // so the next sub-frame's comparisons all have prev data.
                if (row_vis == 21 && col_vis == 39) prev_ftile_valid = true;
            }

            // Original sub_1c8f1 ANDs entry with ax=0xFFFE (clears dirty bit 0)
            // then draws if bit 3 (foreground) is set. v2 draws all foreground
            // tiles every frame — only check bit 3.
            if (!(tile_entry & 8)) continue;

            v2_render_tile_masked(buf, tgfx_base, gs_base, tile_entry,
                                  col_vis * 8 - pix_off_x, row_vis * 8 - pix_off_y);
        }
    }
}

// ============================================================================
// v2_draw_ui: Draws UI glyphs from the UI element list.
//
// Data sources (all in game data segment):
//   UI element list: ds:0x956C (40 cols × 22 rows = 0x370 bytes)
//   Glyph data:      ds:0x687D + glyph_index * 72
//   Glyph format:    4 planes × 2 strips × 9 bytes (1 mask + 8 data) = 72 bytes
//
// Same format as type 0 sprites (jpt_1CF4E handlers):
//   Each strip → 4 rows × 2 bytes/row.
//   data[0,1]→row+0, data[2,3]→row+1, data[4,5]→row+2, data[6,7]→row+3
//   Mask bits: 76→row0, 54→row1, 32→row2, 10→row3
//   2 strips × 4 rows/strip = 8 rows. 2 bytes/row × 4 planes = 8 pixels wide.
//   Strip 0 → rows 0-3, Strip 1 → rows 4-7 (0x158 = 4 VGA rows apart).
// ============================================================================
void v2_draw_ui(uint16_t ds_val) {
#ifdef V2_RENDER_FROM_SHADOW
    if (!v2_vm_in_frame) return;
#endif
    if (!myDrawInfo_v2 || !v2_m2c_base) return;

    uint8_t* ds_base = v2_get_ds_base(ds_val);
    uint8_t* buf = v2_render_buf;

    // Scan UI element list: 40 columns × 22 rows at ds:0x956C
    uint8_t* ui_list = ds_base + DS_GLYPH_BUF;

    // Debug: count non-zero cells and dump row occupancy.
    {
        static int _frame = 0; _frame++;
        static int _last_nz = -1;
        int nz = 0;
        uint8_t rows_used[22] = {0};
        for (int pos = 0; pos < 0x370; pos++) {
            if (ui_list[pos] != 0) {
                nz++;
                int r = pos / 40;
                if (r < 22) rows_used[r]++;
            }
        }
        if (nz != _last_nz) {
            extern int v2_dbg_pre_vm_iter;
            fprintf(stderr, "V2-UI-COUNT[f%d render=%d lvl=%04X]: nz_cells=%d rows: ",
                v2_dbg_pre_vm_iter, _frame, *(uint16_t*)(ds_base + DS_LEVEL), nz);
            for (int r = 0; r < 22; r++)
                if (rows_used[r]) fprintf(stderr, "r%d=%d ", r, rows_used[r]);
            fprintf(stderr, "byte_956B=%02X\n", ds_base[DS_GLYPH_DIRTY]);
            // Dump rows with content as ASCII (glyph 0x10..0x3F = printable chars in font)
            if (nz > 0 && nz < 300) {
                for (int r = 0; r < 22; r++) {
                    if (!rows_used[r]) continue;
                    fprintf(stderr, "  r%02d: '", r);
                    for (int c = 0; c < 40; c++) {
                        uint8_t ch = ui_list[r * 40 + c];
                        if (ch == 0) fputc('.', stderr);
                        else if (ch >= 0x20 && ch < 0x80) fputc(ch, stderr);
                        else fprintf(stderr, "\\x%02X", ch);
                    }
                    fprintf(stderr, "'\n");
                }
            }
            _last_nz = nz;
        }
    }

    for (int pos = 0; pos < 0x370; pos++) {
        uint8_t ch = ui_list[pos];
        if (ch == 0) continue;

        uint16_t glyph_index = (uint16_t)(ch - 0x10);
        int grid_row = pos / 40;
        int grid_col = pos % 40;

        int screen_x = grid_col * 8;
        int screen_y = grid_row * 8;

        // Glyph data at ds:0x687D + glyph_index * 72
        // Original: si = 0x687E + glyph_index*72, reads mask at [si-1]
        uint8_t* glyph = ds_base + (uint16_t)(0x687Du + glyph_index * 72u);

        for (int plane = 0; plane < 4; plane++) {
            for (int strip = 0; strip < 2; strip++) {
                uint8_t mask = glyph[0];
                uint8_t* data = glyph + 1;
                int base_y = screen_y + strip * 4;  // strip 0 → row 0, strip 1 → row 4

                if (mask) {
                    // Type 0 mapping: 4 rows × 2 bytes per row
                    // Mask bits 76→row0, 54→row1, 32→row2, 10→row3
                    if (mask & 0x80) v2_put_pixel(buf, screen_x + 0*4 + plane, base_y + 0, data[0]);
                    if (mask & 0x40) v2_put_pixel(buf, screen_x + 1*4 + plane, base_y + 0, data[1]);
                    if (mask & 0x20) v2_put_pixel(buf, screen_x + 0*4 + plane, base_y + 1, data[2]);
                    if (mask & 0x10) v2_put_pixel(buf, screen_x + 1*4 + plane, base_y + 1, data[3]);
                    if (mask & 0x08) v2_put_pixel(buf, screen_x + 0*4 + plane, base_y + 2, data[4]);
                    if (mask & 0x04) v2_put_pixel(buf, screen_x + 1*4 + plane, base_y + 2, data[5]);
                    if (mask & 0x02) v2_put_pixel(buf, screen_x + 0*4 + plane, base_y + 3, data[6]);
                    if (mask & 0x01) v2_put_pixel(buf, screen_x + 1*4 + plane, base_y + 3, data[7]);
                }

                glyph += 9;
            }
        }
    }
}

// ============================================================================
// HUD rendering — screen rows 176-239 (VGA split screen: always from address 0)
//
// VGA layout: 86 bytes/row × 64 rows, 4 planes interleaved.
// Plane p, VGA offset i → pixel x = (i % 86)*4 + p, y = i / 86
// ============================================================================

static inline void v2_hud_pixel(int x, int y, uint8_t color) {
    if (x >= 0 && x < 320 && y >= 0 && y < 64)
        v2_hud_buf[y * 320 + x] = color;
}

// ============================================================================
// v2_draw_hud_background: Decode raw chunk data (4-plane) into v2_hud_buf.
//
// Called after read_and_display_raw_chunk loads chunk to VGA offset 0.
// Data source: chunk buffer at segment chunk_seg (= ds:0x2E77).
// Format: [plane0: plane_size bytes][plane1][plane2][plane3]
// Each plane byte at offset i → VGA offset i, plane p.
// ============================================================================
void v2_draw_hud_background(uint16_t ds_val, uint16_t chunk_seg, uint16_t plane_size) {
#ifdef V2_RENDER_FROM_SHADOW
    if (!v2_vm_in_frame) return;
#endif
    if (!v2_m2c_base) return;

#ifdef V2_RENDER_FROM_SHADOW
    // V2 VM loads chunk into shadow buffer (v2_read_raw_chunk).
    // v2_resolve_segment maps chunk_seg → shadow chunk buffer.
    uint8_t* chunk = v2_resolve_segment(chunk_seg);
    if (!chunk) chunk = v2_m2c_base + ((uint32_t)chunk_seg << 4);
#else
    uint8_t* chunk = v2_m2c_base + ((uint32_t)chunk_seg << 4);
#endif

    memset(v2_hud_buf, 0, sizeof(v2_hud_buf));

    for (int p = 0; p < 4; p++) {
        uint8_t* plane_data = chunk + plane_size * p;
        for (int i = 0; i < plane_size && i < 86*64; i++) {
            int x = (i % 86) * 4 + p;
            int y = i / 86;
            v2_hud_pixel(x, y, plane_data[i]);
        }
    }
}

// ============================================================================
// v2_draw_viewport_chunk: Decode raw chunk image directly into v2_render_buf.
//
// Mirrors orig sub_10cd8 (read_and_display_raw_chunk) which writes 4 VGA planes
// directly to A000:display_offset via OUT(0x3C4) plane select + REP MOVSB.
// In v2 the equivalent of VGA viewport area is v2_render_buf — write there directly.
// No persistent flag: orig has none. On level transition sub_16880 clears VGA
// (v2_render_buf), and the next per-frame render either re-decodes the chunk
// (intro level) or draws tiles (tile level).
// ============================================================================
void v2_draw_viewport_chunk(uint16_t chunk_seg, uint16_t plane_size) {
    if (!v2_m2c_base) return;

#ifdef V2_RENDER_FROM_SHADOW
    uint8_t* chunk = v2_resolve_segment(chunk_seg);
    if (!chunk) chunk = v2_m2c_base + ((uint32_t)chunk_seg << 4);
#else
    uint8_t* chunk = v2_m2c_base + ((uint32_t)chunk_seg << 4);
#endif

    // Orig: VGA Mode X 4 planes at display_offset; v2: equivalent rectangle in v2_render_buf.
    // Plane interleave x = (i % pitch) * 4 + plane, y = i / pitch. pitch=86 = 320/4 + slack.
    for (int p = 0; p < 4; p++) {
        uint8_t* plane_data = chunk + plane_size * p;
        for (int i = 0; i < plane_size && i < 86 * 176; i++) {
            int x = (i % 86) * 4 + p;
            int y = i / 86;
            if (x < 320 && y < 176) {
                v2_render_buf[y * 320 + x] = plane_data[i];
            }
        }
    }
    // Save backup for per-frame restore in v2_draw_tiles intro path
    memcpy(v2_chunk_bg_backup, v2_render_buf, 320 * 176);
    v2_chunk_bg_valid = true;
}

// ============================================================================
// v2_draw_hud_item: Draw 16×16 inventory item icon in HUD.
//
// Mirrors draw_inventory_item (sub_1183d).
// Data: ds:0x507D + item_id*256 = 4 planes × 16 rows × 4 bytes.
//   Plane order in data: 3, 0, 1, 2 (each 64 bytes).
// Position: ds:[slot - 0x7A9E] = VGA offset in HUD area.
//   VGA offset → hud_x = (off % 86)*4 + 3, hud_y = off / 86.
// Pixel (col, row), col 0..15:
//   plane = {3,0,1,2}[col%4], byte = col/4
//   color = data[plane_off + row*4 + byte]
// ============================================================================
// ============================================================================
// v2_draw_hud_portrait: Draw 32×7 viking portrait in HUD.
//
// Mirrors sub_11aa4 (portrait rendering).
// Data: ds:[si-0x7A7E] + 0x497D = 224 bytes = 4 planes × 7 rows × 8 bytes.
//   Plane order in VGA writes: 3, 0, 1, 2 (POP+INC pattern).
//   Data layout: [plane3:56][plane0:56][plane1:56][plane2:56]
// Position: ds:[di-0x7A84] = VGA offset in HUD area.
//   VGA offset → hud_x = (off % 86)*4 + 3, hud_y = off / 86.
// Pixel (col, row), col 0..31:
//   col%4 → section {0→plane3(off 0), 1→plane0(off 56), 2→plane1(off 112), 3→plane2(off 168)}
//   byte = col/4, color = data[section_off + row*8 + byte]
// ============================================================================
void v2_draw_hud_portrait(uint16_t ds_val, uint16_t viking_di, uint16_t portrait_si) {
#ifdef V2_RENDER_FROM_SHADOW
    if (!v2_vm_in_frame) return;
#endif
    if (!v2_m2c_base) return;

    uint8_t* ds_base = v2_get_ds_base(ds_val);

    // Portrait graphics pointer: ds:[si-0x7A7E] + 0x497D
    // Cast to uint16_t for x86 16-bit address wrapping
    uint16_t portrait_ptr = *(uint16_t*)(ds_base + (uint16_t)(portrait_si - 0x7A7E));
    uint8_t* portrait_data = ds_base + portrait_ptr + 0x497D;

    // HUD VGA offset: ds:[di-0x7A84]
    uint16_t vga_off = *(uint16_t*)(ds_base + (uint16_t)(viking_di - 0x7A84));

    int hud_x = (vga_off % 86) * 4 + 3;
    int hud_y = vga_off / 86;

    // Data: [plane3:56][plane0:56][plane1:56][plane2:56], each 7 rows × 8 bytes
    static const int portrait_plane_off[4] = {0, 56, 112, 168};

    for (int row = 0; row < 7; row++) {
        for (int col = 0; col < 32; col++) {
            int poff = portrait_plane_off[col & 3];
            int byte_idx = col >> 2;
            uint8_t color = portrait_data[poff + row * 8 + byte_idx];
            v2_hud_pixel(hud_x + col, hud_y + row, color);
        }
    }
}

void v2_draw_hud_item(uint16_t ds_val, uint16_t slot_di, uint16_t item_ax) {
#ifdef V2_RENDER_FROM_SHADOW
    if (!v2_vm_in_frame) return;
#endif
    if (!v2_m2c_base) return;

    uint8_t* ds_base = v2_get_ds_base(ds_val);

    // Special case: slot 0x18 with item 0 → use item 0x17 (trash icon).
    // Mirror orig sub_1183d eip 0x183F-0x1849:
    //   CMP di, 0x18; JNZ loc_1184c
    //   CMP ax, 0;   JNZ loc_1184c
    //   MOV ax, 0x17
    // Blink at trash slot intentionally alternates item icon (SET phase) and
    // trash icon (CLEAR phase, ax=0 forced to 0x17). Mirror orig exactly.
    uint16_t item_id = item_ax;
    if (slot_di == 0x18 && item_id == 0)
        item_id = 0x17;

    // Item graphics: 256 bytes at ds:0x507D + item_id * 256
    uint8_t* item_data = ds_base + 0x507D + (item_id << 8);

    // VGA offset from lookup table (uint16_t cast for x86 16-bit wrapping)
    uint16_t vga_off = *(uint16_t*)(ds_base + (uint16_t)(slot_di - 0x7A9E));

    int hud_x = (vga_off % 86) * 4 + 3;
    int hud_y = vga_off / 86;

    // Plane data offsets within item_data: plane order 3,0,1,2
    // col%4 → plane_off: 0→0(plane3), 1→64(plane0), 2→128(plane1), 3→192(plane2)
    static const int plane_off[4] = {0, 64, 128, 192};

    for (int row = 0; row < 16; row++) {
        for (int col = 0; col < 16; col++) {
            int poff = plane_off[col & 3];
            int byte_idx = col >> 2;
            uint8_t color = item_data[poff + row * 4 + byte_idx];
            v2_hud_pixel(hud_x + col, hud_y + row, color);
        }
    }
}

// ============================================================================
// v2_draw_hud_selector: Draw selection cursor frame around inventory slot.
//
// Mirrors display_selector (sub_118ad).
// Data source: ds:0x637D (256 bytes of cursor graphics).
// Position: ds:[di - 0x7A9E] = VGA offset (di already shifted by caller).
//
// The original writes individual bytes/words to specific VGA offsets forming
// a frame border (rows 0-4, 11-15 with gap in middle for item content).
// Uses scatter-write pattern: each entry is (si_offset, vga_offset, plane).
// Word writes cover 2 adjacent VGA bytes in the same plane.
// ============================================================================
void v2_draw_hud_selector(uint16_t ds_val, uint16_t slot_di) {
#ifdef V2_RENDER_FROM_SHADOW
    if (!v2_vm_in_frame) return;
#endif
    if (!v2_m2c_base) return;

    uint8_t* ds_base = v2_get_ds_base(ds_val);
    uint8_t* sel_data = ds_base + 0x637D;

    // VGA offset from lookup table (di already shifted by caller, uint16_t for x86 wrapping)
    uint16_t vga_base = *(uint16_t*)(ds_base + (uint16_t)(slot_di - 0x7A9E));

    // Scatter-write table: {si_offset, vga_offset_relative, plane, is_word}
    // Extracted from sub_118ad drawPixel calls
    static const struct { uint8_t si; uint16_t vga; uint8_t plane; uint8_t word; } t[] = {
        // Plane 3 (mask 0x802)
        {0x00,0x000,3,1},{0x03,0x003,3,0},{0x04,0x056,3,1},{0x07,0x059,3,0},
        {0x08,0x0AC,3,0},{0x0C,0x102,3,0},{0x10,0x158,3,0},
        {0x2C,0x3B2,3,0},{0x30,0x408,3,0},{0x34,0x45E,3,0},
        {0x38,0x4B4,3,1},{0x3B,0x4B7,3,0},{0x3C,0x50A,3,1},{0x3F,0x50D,3,0},
        // Plane 0 (mask 0x102)
        {0x40,0x001,0,0},{0x43,0x004,0,0},{0x44,0x057,0,0},{0x47,0x05A,0,0},
        {0x48,0x0AD,0,0},{0x4C,0x103,0,0},{0x50,0x159,0,0},
        {0x6C,0x3B3,0,0},{0x70,0x409,0,0},{0x74,0x45F,0,0},
        {0x78,0x4B5,0,0},{0x7B,0x4B8,0,0},{0x7C,0x50B,0,0},{0x7F,0x50E,0,0},
        // Plane 1 (mask 0x202)
        {0x80,0x001,1,0},{0x83,0x004,1,0},{0x84,0x057,1,0},{0x87,0x05A,1,0},
        {0x8B,0x0B0,1,0},{0x8F,0x106,1,0},{0x93,0x15C,1,0},
        {0xAF,0x3B6,1,0},{0xB3,0x40C,1,0},{0xB7,0x462,1,0},
        {0xB8,0x4B5,1,0},{0xBB,0x4B8,1,0},{0xBC,0x50B,1,0},{0xBF,0x50E,1,0},
        // Plane 2 (mask 0x402)
        {0xC0,0x001,2,0},{0xC2,0x003,2,1},{0xC4,0x057,2,0},{0xC6,0x059,2,1},
        {0xCB,0x0B0,2,0},{0xCF,0x106,2,0},{0xD3,0x15C,2,0},
        {0xEF,0x3B6,2,0},{0xF3,0x40C,2,0},{0xF7,0x462,2,0},
        {0xF8,0x4B5,2,0},{0xFA,0x4B7,2,1},{0xFC,0x50B,2,0},{0xFE,0x50D,2,1},
    };

    for (int i = 0; i < (int)(sizeof(t)/sizeof(t[0])); i++) {
        int off = vga_base + t[i].vga;
        int x = (off % 86) * 4 + t[i].plane;
        int y = off / 86;
        v2_hud_pixel(x, y, sel_data[t[i].si]);
        if (t[i].word) {
            int off2 = off + 1;
            v2_hud_pixel((off2 % 86) * 4 + t[i].plane, off2 / 86, sel_data[t[i].si + 1]);
        }
    }
}

// ============================================================================
// v2_draw_hud_healthbar: Draw 32×24 active viking status icon in HUD.
//
// Mirrors sub_117d0.
// Data: ds:[bx*2 - 0x7AB6] where bx = viking_index + health*3.
//   768 bytes = 4 planes × 24 rows × 8 bytes.
//   Plane order in VGA writes: 1, 2, 3, 0 (POP+INC for plane 0 only).
//   Data layout: [plane1:192][plane2:192][plane3:192][plane0:192]
// Position: ds:[di*2 - 0x7AA4] = VGA offset in HUD area.
//   VGA offset → hud_x = (off % 86)*4 + 1, hud_y = off / 86.
//   (first plane written is plane 1, not plane 3)
// ============================================================================
void v2_draw_hud_healthbar(uint16_t ds_val, uint16_t health_ax, uint16_t viking_bx, uint16_t pos_di) {
#ifdef V2_RENDER_FROM_SHADOW
    if (!v2_vm_in_frame) return;
#endif
    if (!v2_m2c_base) return;

    uint8_t* ds_base = v2_get_ds_base(ds_val);

    // Data source: bx = viking + health*3, then lookup at ds:[bx*2 - 0x7AB6]
    // uint16_t casts for x86 16-bit address wrapping
    uint16_t idx = viking_bx + health_ax * 3;
    uint16_t data_off = *(uint16_t*)(ds_base + (uint16_t)(idx * 2 - 0x7AB6));
    uint8_t* bar_data = ds_base + data_off;

    // VGA position: ds:[di*2 - 0x7AA4]
    uint16_t vga_off = *(uint16_t*)(ds_base + (uint16_t)(pos_di * 2 - 0x7AA4));

    int hud_x = (vga_off % 86) * 4 + 1;  // plane 1 first
    int hud_y = vga_off / 86;

    // Data: [plane1:192][plane2:192][plane3:192][plane0:192], each 24 rows × 8 bytes
    static const int bar_plane_off[4] = {0, 192, 384, 576};

    for (int row = 0; row < 24; row++) {
        for (int col = 0; col < 32; col++) {
            int poff = bar_plane_off[col & 3];
            int byte_idx = col >> 2;
            uint8_t color = bar_data[poff + row * 8 + byte_idx];
            v2_hud_pixel(hud_x + col, hud_y + row, color);
        }
    }
}
