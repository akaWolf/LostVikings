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

void v2_chunk_bg_update_from_render() {
    memcpy(v2_chunk_bg_backup, v2_render_buf, 320 * 176);
    v2_chunk_bg_valid = true;
}

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
        const uint8_t* pal = shad + 0x8202;
        for (int i = 0; i < 256; i++) {
            v2_display_palette[i].r = pal[i*3 + 0] << 2;
            v2_display_palette[i].g = pal[i*3 + 1] << 2;
            v2_display_palette[i].b = pal[i*3 + 2] << 2;
            v2_display_palette[i].a = 255;
        }
        // Color index 3 mirror from cmd_type=6 (dialog text bg) at shad[0x7F0B..0x7F0D]
        v2_display_palette[3].r = shad[0x7F0B] << 2;
        v2_display_palette[3].g = shad[0x7F0C] << 2;
        v2_display_palette[3].b = shad[0x7F0D] << 2;
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
    uint16_t fs_seg = *(uint16_t*)(ds_base + 0x2E69);
    // Tile graphics segment
    uint16_t tgfx_seg = *(uint16_t*)(ds_base + 0x2E5F);
    // V2-DRAWT-DBG: log entry state every 60 frames
    {
        static int _dt_dbg = 0; _dt_dbg++;
        if (_dt_dbg <= 5 || _dt_dbg % 200 == 0) {
            fprintf(stderr,
              "V2-DRAWT-DBG[%d]: ds=%04X fs_seg=%04X tgfx_seg=%04X 25CF=%02X 25AD=%04X 25C9=%04X\n",
              _dt_dbg, ds_val, fs_seg, tgfx_seg,
              ds_base[0x25CF],
              *(uint16_t*)(ds_base + 0x25AD),
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
    uint8_t lvl_flags = ds_base[0x25CF];
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
    int16_t vp_px = *(int16_t*)(ds_base + 0x44);
    int16_t vp_py = *(int16_t*)(ds_base + 0x46);
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
        uint16_t lut_off = (uint16_t)(row_scrolled * 2u - 0x7098u);
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
    uint16_t fs_seg = *(uint16_t*)(ds_base + 0x2E69);
    uint16_t tgfx_seg = *(uint16_t*)(ds_base + 0x2E5F);
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
    int16_t vp_px = *(int16_t*)(ds_base + 0x44);
    int16_t vp_py = *(int16_t*)(ds_base + 0x46);
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
static inline void v2_put_pixel(uint8_t* buf, int sx, int sy, uint8_t color) {
    if (sx >= 0 && sx < 320 && sy >= 0 && sy < 176)
        buf[sy * 320 + sx] = color;
}

void v2_draw_sprites(uint16_t ds_val) {
#ifdef V2_RENDER_FROM_SHADOW
    if (!v2_vm_in_frame) return;
#endif
    if (!v2_m2c_base || !myDrawInfo_v2) return;

    uint8_t* ds_base = v2_get_ds_base(ds_val);

    uint8_t* buf = v2_render_buf;

    // Viewport origin — pixel scroll values.
    // Mirrors orig set_display_memory_addr (sub_16775) — apply x_some/y_some shake.
    // Sprites in orig are drawn at world_pos - vp_px in VGA buffer; CRTC then shifts
    // entire display by x_some via start address + pixel pan. v2 single-buffer:
    // bake the shake into the effective camera so all elements stay aligned.
    int16_t _vp_px = *(int16_t*)(ds_base + 0x44);
    int16_t _vp_py = *(int16_t*)(ds_base + 0x46);
    int16_t _xs   = *(int16_t*)(ds_base + 0x39E);
    int16_t _ys   = *(int16_t*)(ds_base + 0x3A0);
    int16_t _xlvl = *(int16_t*)(ds_base + 0x25A4);
    int16_t _ylvl = *(int16_t*)(ds_base + 0x25A6);
    int viewport_x = (int)_vp_px + (int)_xs; if (viewport_x > (int)_xlvl) viewport_x = (int)_vp_px - (int)_xs;
    int viewport_y = (int)_vp_py + (int)_ys; if (viewport_y > (int)_ylvl) viewport_y = (int)_vp_py - (int)_ys;
    static int spr_dbg = 0; spr_dbg++;
    for (int obj = 0xFE; obj >= 0; obj -= 2) {
        uint16_t flags = *(uint16_t*)(ds_base + obj + 0x44D);

        // Must be active (bit 15) with bits 13-14 clear
        if (!(flags & 0x8000) || (flags & 0x6000)) continue;

        int type = flags & 7;

        {
            static int spr_printed = 0;
            uint16_t cur_lvl = *(uint16_t*)(ds_base + 0x25AD);
            if (spr_printed < 15 && cur_lvl < 38) {
                spr_printed++;
                printf("V2-SPR[l%d]: obj=%02x fl=%04x t=%d xy=(%d,%d) seg=%04x off=%04x scr=(%d,%d)\n",
                       cur_lvl, obj, flags, type,
                       *(int16_t*)(ds_base + obj + 0x64D),
                       *(int16_t*)(ds_base + obj + 0x74D),
                       *(uint16_t*)(ds_base + obj + 0x94D),
                       *(uint16_t*)(ds_base + obj + 0x84D),
                       viewport_x, viewport_y);
            }
        }

        // Dispatch table at cs:0x15CB: only types 1, 2, 4 have renderers.
        // Type 1 → cs:0x0648 (seg003_648_proc, 8×8)
        // Type 2 → cs:0x1078 (loc_1d8a8, dynamic size)
        // Type 4 → cs:0x0B82 (sub_1d3b2, 16×16)
        // Types 0,3,5,6,7 → cs:0x0000 (not used)
        if (type != 1 && type != 2 && type != 4) continue;

        int16_t world_x = *(int16_t*)(ds_base + obj + 0x64D);
        int16_t world_y = *(int16_t*)(ds_base + obj + 0x74D);

        int sx0 = world_x - viewport_x;
        int sy0 = world_y - viewport_y;

        uint16_t sprite_off = *(uint16_t*)(ds_base + obj + 0x84D);
        uint16_t sprite_seg = *(uint16_t*)(ds_base + obj + 0x94D);
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
            num_strips = (int)*(uint16_t*)(ds_base + obj + 0x0C4D);
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

        // Coarse bounds check — sprite pixel size
        int sprite_h = num_strips * rows_per_strip;
        int sprite_w = bytes_per_row * 4;  // 4 planes
        if (sx0 >= 320 || sx0 < -sprite_w || sy0 >= 176 || sy0 < -sprite_h) continue;

        // Sprite data: resolve segment to shadow buffer, add offset.
        // sprite_off = 1-based offset to first data byte; mask at offset-1.
#ifdef V2_RENDER_FROM_SHADOW
        uint8_t* seg_base = v2_resolve_segment(sprite_seg);
        if (!seg_base) continue;
        uint8_t* sprite = seg_base + sprite_off - 1;
        {
            static int cmp_mismatch = 0;
            uint16_t cur_lvl = *(uint16_t*)(ds_base + 0x25AD);
            if (cur_lvl < 38 && cmp_mismatch < 10) {
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

        uint8_t* ptr = sprite;
        for (int section = 0; section < 4; section++) {
            int plane = section;
            for (int strip = 0; strip < num_strips; strip++) {
                uint8_t mask = ptr[0];
                uint8_t* data = ptr + 1;
                int base_y = sy0 + strip * rows_per_strip;

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
void v2_draw_flagged_tiles(uint16_t ds_val) {
#ifdef V2_RENDER_FROM_SHADOW
    if (!v2_vm_in_frame) return;
#endif
    if (!myDrawInfo_v2 || !v2_m2c_base) return;

    uint8_t* ds_base = v2_get_ds_base(ds_val);
    uint8_t* buf = v2_render_buf;

    uint16_t fs_seg = *(uint16_t*)(ds_base + 0x2E69);
    uint16_t tgfx_seg = *(uint16_t*)(ds_base + 0x2E5F);
    uint16_t gs_seg = *(uint16_t*)(ds_base + 0x2E61);
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
    int16_t vp_px = *(int16_t*)(ds_base + 0x44);
    int16_t vp_py = *(int16_t*)(ds_base + 0x46);
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

        uint16_t lut_off = (uint16_t)(row_scrolled * 2u - 0x7098u);
        uint16_t row_base = *(uint16_t*)(ds_base + lut_off);

        for (int col_vis = 0; col_vis < 43; col_vis++) {
            uint16_t col_scrolled = (uint16_t)(col_vis + scroll_y + extra_tile_x);
            uint16_t tile_map_off = (uint16_t)((row_base + col_scrolled) * 2u);
            uint16_t tile_entry = *(uint16_t*)(fs_base + tile_map_off);

            // Original sub_1c8f1 ANDs entry with ax=0xFFFE (clears dirty bit 0)
            // then draws if bit 3 (foreground) is set. v2 draws all foreground
            // tiles every frame — only check bit 3.
            if (!(tile_entry & 8)) continue;

            uint16_t tile_gfx_off = tile_entry & 0xFFC0;
            bool hflip = (tile_entry & 0x10) != 0;
            bool vflip = (tile_entry & 0x20) != 0;

            uint8_t* tile = tgfx_base + tile_gfx_off;

            uint16_t mask_off = tile_gfx_off >> 3;
            uint8_t* mask_data = gs_base + mask_off;

            int screen_x = col_vis * 8 - pix_off_x;
            int screen_y = row_vis * 8 - pix_off_y;

            // For each pixel in the 8×8 tile, check mask and draw if set
            for (int ty = 0; ty < 8; ty++) {
                int sy = screen_y + (vflip ? 7 - ty : ty);
                if (sy < 0 || sy >= 176) continue;

                for (int tx = 0; tx < 8; tx++) {
                    int sx = screen_x + (hflip ? 7 - tx : tx);
                    if (sx < 0 || sx >= 320) continue;

                    // Tile data layout: plane*16 + strip*8 + row*2 + byte
                    int plane = tx & 3;       // tx % 4
                    int byte_idx = tx >> 2;   // tx / 4 (0 or 1)
                    int strip = ty >> 2;      // ty / 4 (0 or 1)
                    int row = ty & 3;         // ty % 4

                    // Mask: byte index = plane*2 + strip
                    //        bit = 7 - (row*2 + byte_idx)
                    int mb = plane * 2 + strip;
                    int mbit = 7 - (row * 2 + byte_idx);
                    if (!(mask_data[mb] & (1 << mbit))) continue;

                    // Pixel color from tile data
                    uint8_t color = tile[plane * 16 + strip * 8 + row * 2 + byte_idx];
                    buf[sy * 320 + sx] = color;
                }
            }
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
    uint8_t* ui_list = ds_base + 0x956C;

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
                v2_dbg_pre_vm_iter, _frame, *(uint16_t*)(ds_base + 0x25AD), nz);
            for (int r = 0; r < 22; r++)
                if (rows_used[r]) fprintf(stderr, "r%d=%d ", r, rows_used[r]);
            fprintf(stderr, "byte_956B=%02X\n", ds_base[0x956B]);
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
