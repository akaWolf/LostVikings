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

// V2 rendering state — definitions (declared extern in render_v2.h)
uint8_t* v2_m2c_base = nullptr;
uint8_t  v2_render_buf[2][320*200];
std::atomic<int> v2_render_fill{0};
uint8_t  v2_display_buf[320*200];
std::mutex v2_display_mutex;

void v2_swap_render_buf() {
    // Copy completed frame to display buffer under lock
    {
        std::lock_guard<std::mutex> lock(v2_display_mutex);
        memcpy(v2_display_buf, v2_render_buf[v2_render_fill], 320*200);
    }
    // Flip write target
    int old = v2_render_fill.load(std::memory_order_relaxed);
    v2_render_fill.store(old ^ 1, std::memory_order_release);
}

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
    if (!myDrawInfo_v2 || !v2_m2c_base) return;

    uint8_t* ds_base = v2_m2c_base + ((uint32_t)ds_val << 4);
    uint8_t* buf = v2_render_buf[v2_render_fill];
    memset(buf, 0, 320*200);

    // Tile map segment (FS)
    uint16_t fs_seg = *(uint16_t*)(ds_base + 0x2E69);
    // Tile graphics segment
    uint16_t tgfx_seg = *(uint16_t*)(ds_base + 0x2E5F);
    if (!fs_seg || !tgfx_seg) return;

    uint8_t* fs_base = v2_m2c_base + ((uint32_t)fs_seg << 4);
    uint8_t* tgfx_base = v2_m2c_base + ((uint32_t)tgfx_seg << 4);

    uint16_t scroll_x = *(uint16_t*)(ds_base + 0x2581);  // row scroll
    uint16_t scroll_y = *(uint16_t*)(ds_base + 0x257F);  // column scroll

    for (int row_vis = 0; row_vis < 25; row_vis++) {
        uint16_t row_scrolled = (uint16_t)(row_vis + scroll_x);
        if (row_scrolled >= 64) continue;

        // Row base from lookup table at ds-0x7098
        uint16_t lut_off = (uint16_t)(row_scrolled * 2u - 0x7098u);
        uint16_t row_base = *(uint16_t*)(ds_base + lut_off);

        for (int col_vis = 0; col_vis < 43; col_vis++) {
            uint16_t col_scrolled = (uint16_t)(col_vis + scroll_y);

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

            // Screen position
            int screen_x = col_vis * 8;
            int screen_y = row_vis * 8;

            // Decode tile: 4 planes × 8 rows × 2 bytes
            // Normal pixel order per row:
            //   x+0=p0_b0, x+1=p1_b0, x+2=p2_b0, x+3=p3_b0,
            //   x+4=p0_b1, x+5=p1_b1, x+6=p2_b1, x+7=p3_b1
            for (int row = 0; row < 8; row++) {
                int src_row = vflip ? (7 - row) : row;
                int sy = screen_y + row;
                if (sy >= 200) break;

                // Extract 8 pixels for this row
                uint8_t pixels[8];
                for (int plane = 0; plane < 4; plane++) {
                    int p_in = plane;
                    uint8_t b0 = tile[p_in * 16 + src_row * 2];
                    uint8_t b1 = tile[p_in * 16 + src_row * 2 + 1];

                    if (!hflip) {
                        // Normal: plane→plane, byte0→addr0, byte1→addr1
                        pixels[plane + 0] = b0;  // x + plane
                        pixels[plane + 4] = b1;  // x + plane + 4
                    } else {
                        // Hflip: plane→(3-plane), byte0→addr1, byte1→addr0
                        pixels[(3 - plane) + 4] = b0;  // x + (3-plane) + 4
                        pixels[(3 - plane) + 0] = b1;  // x + (3-plane)
                    }
                }

                // Write to buffer
                int base = sy * 320 + screen_x;
                for (int px = 0; px < 8; px++) {
                    int sx = screen_x + px;
                    if (sx >= 320) break;
                    buf[base + px] = pixels[px];
                }
            }
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
    if (sx >= 0 && sx < 320 && sy >= 0 && sy < 200)
        buf[sy * 320 + sx] = color;
}

void v2_draw_sprites(uint16_t ds_val) {
    if (!myDrawInfo_v2 || !v2_m2c_base) return;

    uint8_t* ds_base = v2_m2c_base + ((uint32_t)ds_val << 4);
    uint8_t* buf = v2_render_buf[v2_render_fill];

    // Viewport origin — pixel scroll values
    int viewport_x = (int)*(int16_t*)(ds_base + 0x44);
    int viewport_y = (int)*(int16_t*)(ds_base + 0x46);

    for (int obj = 0xFE; obj >= 0; obj -= 2) {
        uint16_t flags = *(uint16_t*)(ds_base + obj + 0x44D);

        // Must be active (bit 15) with bits 13-14 clear
        if (!(flags & 0x8000) || (flags & 0x6000)) continue;

        int type = flags & 7;

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

        // Coarse bounds check — sprite pixel size
        int sprite_h = num_strips * rows_per_strip;
        int sprite_w = bytes_per_row * 4;  // 4 planes
        if (sx0 >= 320 || sx0 < -sprite_w || sy0 >= 200 || sy0 < -sprite_h) continue;

        // Sprite data: offset points to first data byte, mask at offset-1
        uint8_t* sprite = v2_m2c_base + ((uint32_t)sprite_seg << 4) + sprite_off - 1;

        // Column formula: sx0 + N*4 + section.
        // In the original, section S writes to VGA plane (start_plane+S)&3.
        // When the plane wraps (3→0), VGA DI increments by 1 (= +4 pixels).
        // The formula base_x + N*4 + p fails at the wrap boundary.
        // Direct formula: pixel column = sx0 + N*4 + section — verified
        // against the original VGA addressing for all start_plane values.

        uint8_t* ptr = sprite;
        for (int section = 0; section < 4; section++) {
            for (int strip = 0; strip < num_strips; strip++) {
                uint8_t mask = ptr[0];
                uint8_t* data = ptr + 1;
                int base_y = sy0 + strip * rows_per_strip;

                if (mask) {
                    if (type == 1) {
                        // Type 1 (jpt_1CF4E): 4 rows × 2 bytes per row
                        // Mask: 76→row0, 54→row1, 32→row2, 10→row3
                        if (mask & 0x80) v2_put_pixel(buf, sx0 + 0*4 + section, base_y + 0, data[0]);
                        if (mask & 0x40) v2_put_pixel(buf, sx0 + 1*4 + section, base_y + 0, data[1]);
                        if (mask & 0x20) v2_put_pixel(buf, sx0 + 0*4 + section, base_y + 1, data[2]);
                        if (mask & 0x10) v2_put_pixel(buf, sx0 + 1*4 + section, base_y + 1, data[3]);
                        if (mask & 0x08) v2_put_pixel(buf, sx0 + 0*4 + section, base_y + 2, data[4]);
                        if (mask & 0x04) v2_put_pixel(buf, sx0 + 1*4 + section, base_y + 2, data[5]);
                        if (mask & 0x02) v2_put_pixel(buf, sx0 + 0*4 + section, base_y + 3, data[6]);
                        if (mask & 0x01) v2_put_pixel(buf, sx0 + 1*4 + section, base_y + 3, data[7]);
                    } else if (type == 2) {
                        // Type 2 (jpt_1DA02): 1 row × 8 bytes
                        // Mask: bit7→data[0], ..., bit0→data[7]
                        if (mask & 0x80) v2_put_pixel(buf, sx0 + 0*4 + section, base_y, data[0]);
                        if (mask & 0x40) v2_put_pixel(buf, sx0 + 1*4 + section, base_y, data[1]);
                        if (mask & 0x20) v2_put_pixel(buf, sx0 + 2*4 + section, base_y, data[2]);
                        if (mask & 0x10) v2_put_pixel(buf, sx0 + 3*4 + section, base_y, data[3]);
                        if (mask & 0x08) v2_put_pixel(buf, sx0 + 4*4 + section, base_y, data[4]);
                        if (mask & 0x04) v2_put_pixel(buf, sx0 + 5*4 + section, base_y, data[5]);
                        if (mask & 0x02) v2_put_pixel(buf, sx0 + 6*4 + section, base_y, data[6]);
                        if (mask & 0x01) v2_put_pixel(buf, sx0 + 7*4 + section, base_y, data[7]);
                    } else { // type == 4
                        // Type 4 (jpt_1d514): 2 rows × 4 bytes per row
                        // Mask: 7654→row0, 3210→row1
                        if (mask & 0x80) v2_put_pixel(buf, sx0 + 0*4 + section, base_y + 0, data[0]);
                        if (mask & 0x40) v2_put_pixel(buf, sx0 + 1*4 + section, base_y + 0, data[1]);
                        if (mask & 0x20) v2_put_pixel(buf, sx0 + 2*4 + section, base_y + 0, data[2]);
                        if (mask & 0x10) v2_put_pixel(buf, sx0 + 3*4 + section, base_y + 0, data[3]);
                        if (mask & 0x08) v2_put_pixel(buf, sx0 + 0*4 + section, base_y + 1, data[4]);
                        if (mask & 0x04) v2_put_pixel(buf, sx0 + 1*4 + section, base_y + 1, data[5]);
                        if (mask & 0x02) v2_put_pixel(buf, sx0 + 2*4 + section, base_y + 1, data[6]);
                        if (mask & 0x01) v2_put_pixel(buf, sx0 + 3*4 + section, base_y + 1, data[7]);
                    }
                }
                ptr += 9;
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
    if (!myDrawInfo_v2 || !v2_m2c_base) return;

    uint8_t* ds_base = v2_m2c_base + ((uint32_t)ds_val << 4);
    uint8_t* buf = v2_render_buf[v2_render_fill];

    // Scan UI element list: 40 columns × 22 rows at ds:0x956C
    uint8_t* ui_list = ds_base + 0x956C;

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
