#ifndef STRUCTS_H
#define STRUCTS_H

#include <cstdint>

// Represents a single tile's attributes.
// Bit 0: is_dirty flag (1 = needs redraw, 0 = no redraw)
struct TileAttribute {
    uint16_t value;
};

// Represents the header for DATA.DAT, loaded into dummy7_225b0 (19f0:86b0)
// This structure is based on observed accesses to offsets within the 32-byte block.
struct DataHeader {
    uint16_t unknown_0; // Offset 0
    uint16_t tilemap_segment; // Offset 2 (0x86B2). Segment address of tilemap data.
    uint16_t map_width_tiles; // Offset 4 (0x86B4). Map width in tiles.
    uint16_t map_height_tiles; // Offset 6 (0x86B6). Map height in tiles.
    uint16_t unknown_8; // Offset 8 (0x86B8)
    char remaining_data[22]; // Remaining 22 bytes (32 - 10 = 22)
};

const uint16_t TILE_WIDTH = 16;
const uint16_t TILE_HEIGHT = 16;
};


// Based on analysis of seg003
struct GameObject {
    char unknown_0[0x56];
    uint32_t unknown_56; // at offset 0x56
    uint8_t unknown_5A;
    uint8_t unknown_5B;
    uint16_t unknown_5C;
    char unknown_5E[0xAC - 0x5E];
    uint16_t unknown_AC; // at offset 0xAC
    uint8_t unknown_AD;
    char unknown_AE[0x102 - 0xAE];
    uint16_t unknown_102; // at offset 0x102
    uint8_t unknown_103;
    char unknown_104[0x44D - 0x104];
    uint16_t flags; // at offset 0x44D.
                    // bit 15 (0x8000): active. If 0, object is skipped in main draw loop.
                    // bits 13, 14 (0x6000): if set, object is skipped.
                    // bits 0-2: object type, used as index into drawing function jump table.
    char unknown_44F[0x64D - 0x44F];
    uint16_t x_coord; // at offset 0x64D. Current X coordinate.
    char unknown_64F[0x74D - 0x64F];
    uint16_t y_coord; // at offset 0x74D. Current Y coordinate.
    char unknown_74F[0x84D - 0x74F];
    uint16_t graphics_data_ptr; // at offset 0x84D
    char unknown_84F[0x94D - 0x84F];
    uint16_t data_segment_ptr; // at offset 0x94D. This segment contains tilemap data.
                               // The tilemap data is loaded into offset 0x1000 within this segment.
                               // The row_lookup_table is located at (data_segment_ptr << 4) + 0x1000 - 0x7098.
    char unknown_94F[0xB4D - 0x94F]; // Gap
    uint16_t unknown_0B4D; // at offset 0x0B4D. This field is used to set data_segment_ptr.
                           // It holds the segment address of the tilemap data.
    char unknown_B4F[0xC4D - 0xB4F]; // Gap
    uint16_t size_related; // at offset 0xC4D. Used in culling, might be width or height.
    char unknown_C4F[0xD4D - 0xC4F];
    uint16_t prev_x; // at offset 0xD4D. Previous X coordinate. Updated from x_coord each frame.
    char unknown_D4F[0xE4D - 0xD4F];
    uint16_t prev_y; // at offset 0xE4D. Previous Y coordinate. Updated from y_coord each frame.
    char unknown_E4F[0xF4D - 0xE4F];
    uint16_t prev_x2; // at offset 0xF4D. Previous X coordinate from 2 frames ago.
    char unknown_F4F[0x104D - 0xF4F];
    uint16_t prev_y2; // at offset 0x104D. Previous Y coordinate from 2 frames ago.
    char unknown_104F[0x114D - 0x104F];
    uint8_t draw_counter; // at offset 0x114D. Countdown for drawing, set to 3 when object needs redraw.
    uint8_t redraw_flag; // at offset 0x114E. If non-zero, object has moved and needs redraw.
    char unknown_114F[0x128D - 0x114F]; // Gap
    uint16_t unknown_128D; // at offset 0x128D. This field is read to set unknown_0B4D.
                           // It likely holds an index into the DataHeader, or directly the segment address of the tilemap data.
    // The full size is still unknown, this is just what has been observed so far.
};

#endif // STRUCTS_H