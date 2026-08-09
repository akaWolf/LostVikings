// v2_ds_layout.h — named DS-segment layout for the v2 reimplementation.
//
// Task #38 (humanization phase A). PURE NAMING LAYER: every constant below is
// a verified DS offset from the original game (sources: unit oracles,
// docs2/ref, reference_ds_data_map, line-by-line asm reads). The shadow DS
// stays a flat 64K byte array — these names change only how the code READS,
// never how memory is laid out. m2c-generated files must keep using raw
// word_XXXXX globals (they ARE the original).
//
// Naming: DS_* for absolute globals, OBJ_* for per-object table columns
// (indexed by the object slot: even values 0..0x26 for the 20-slot table,
// sub-sprite slots continue upward; orig addresses columns as ds:[obj+COL]).

#ifndef V2_DS_LAYOUT_H
#define V2_DS_LAYOUT_H

#include <cstdint>

// ---------------------------------------------------------------------------
// Core globals
// ---------------------------------------------------------------------------
constexpr uint16_t DS_CUR_OBJ        = 0x0042; // word_28522: current VM object slot (0xFFFF = none)
constexpr uint16_t DS_VIEWPORT_X     = 0x0044; // word_28524: viewport pixel X (world coords)
constexpr uint16_t DS_VIEWPORT_Y     = 0x0046; // word_28526: viewport pixel Y
constexpr uint16_t DS_TEXT_IDX       = 0x002A; // word_2850A: seg001 text offset (sub_12515)
constexpr uint16_t DS_MODE_WORD      = 0x0032; // word_28512: text-cluster mode word (sub_1250b)
constexpr uint16_t DS_SCRATCH_34     = 0x0034; // word_28514: text width / generic scratch
constexpr uint16_t DS_SCRATCH_36     = 0x0036; // word_28516: text height / generic scratch
constexpr uint16_t DS_SCRATCH_38     = 0x0038; // word_28518: text align X / scan scratch
constexpr uint16_t DS_SCRATCH_3A     = 0x003A; // word_2851A: text align Y / scan partner
constexpr uint16_t DS_TEXT_COL       = 0x006C; // word_2854C: text column / X result
constexpr uint16_t DS_TEXT_ROW       = 0x006E; // word_2854E: text row / Y result
constexpr uint16_t DS_ACCUMULATOR    = 0x008A; // VM accumulator (acc_base macro target)
constexpr uint16_t DS_LAST_CHAR      = 0x028C; // word_2876C: INT9 last-key char (ASCII / 0x81 confirm; task #37)
constexpr uint16_t DS_MUSIC_MUTE     = 0x0302; // music mute flag
constexpr uint16_t DS_SFX_MUTE       = 0x0304; // SFX mute flag (gates sub_177bb)
constexpr uint16_t DS_PW_CHAR0       = 0x0310; // word_287F0..F6: password chars (4 words)
constexpr uint16_t DS_TRANSITION     = 0x032F; // word_2880F: level-transition counter
constexpr uint16_t DS_FRAME_FLAGS    = 0x0334; // word_28814: frame/transition bit flags
constexpr uint16_t DS_OBJ_COUNT      = 0x0372; // word_28852: active object table end (slot*2 bound)
constexpr uint16_t DS_PRIO_COUNT     = 0x0376; // word_28856: priority drain queue count
constexpr uint16_t DS_PRIO_QUEUE     = 0x0378; // byte queue of slots (drained on DI register, #28); append at [DS_PRIO_COUNT], word each
constexpr uint16_t DS_COLL_PHASE     = 0x0390; // word_28870: collision state machine (-1/0/+1 paths)
constexpr uint16_t DS_COLL_BIT_IDX   = 0x038E; // collision bit index (per-pass, +=2 per op)
constexpr uint16_t DS_INPUT_KEYS     = 0x03B6; // word_28896: held action bits
constexpr uint16_t DS_INPUT_EDGES    = 0x03B8; // word_28898: edge action bits
constexpr uint16_t DS_INPUT_PREV     = 0x03BA; // word_2889A: previous press state (edge XOR base)
constexpr uint16_t DS_ACTIVE_VIKING  = 0x03C2; // word_288A2: active viking slot
constexpr uint16_t DS_PREV_VIKING    = 0x03C4; // word_288A4: previous viking slot
constexpr uint16_t DS_BLINK_COUNTER  = 0x03C6; // word_288A6: viking-switch blink countdown (0x15 start)
constexpr uint16_t DS_GAME_MODE_AC   = 0x03CC; // word_288AC: mode (0x8000 intro, 0x8001/2 transitions)
constexpr uint16_t DS_LEVEL          = 0x25AD; // word_2AA8D: current level id
constexpr uint16_t DS_LEVEL_FLAGS    = 0x25CF; // byte_2AAAF: level flags (bit0 HUD, 0x42 chunk scenes)
constexpr uint16_t DS_SPAWN_TABLE    = 0x25F6; // level object spawn/descriptor table: 14-byte (0x0E) entries, 0xFFFF-terminated (chunk_id/type/x/...); VM ops C7-CA index by OBJ_ANIM_SUB
constexpr uint16_t DS_CMD_WRITE      = 0x218F; // word_2A66F: command-buffer write offset
constexpr uint16_t DS_CMD_READ       = 0x2B64; // word_2B044: command-buffer read offset
constexpr uint16_t DS_TRANSITION_LEVEL_TBL = 0x2B66; // level-transition table: 7 words (6 level ids + 0xFFFF terminator, ds_static: 02 09 08 0F 17 19 FFFF); walker si+=2 with wrap-to-0 on FFFF (loc_1031F)
constexpr uint16_t DS_TRANSITION_CHUNK_TBL = 0x2B74; // level-transition table: 6 chunk ids (ds_static: 1A2 1A3 1A4 1A6 1A5 1C0), parallel to the level table, same si index
constexpr uint16_t DS_CMD_BUF        = 0x1DA7; // command entry base (type/si/di/param/text fields)
constexpr uint16_t DS_CMD_ENTRY_SI   = 0x1DA9; // command entry +2: si/value1 field (base DS_CMD_BUF+2)
constexpr uint16_t DS_CMD_ENTRY_DI   = 0x1DAB; // command entry +4: di/value2 field
constexpr uint16_t DS_CMD_ENTRY_PARAM= 0x1DAD; // command entry +6: param/value3 field

// Segment registry (DosMemAlloc results; wraps documented in unit 31)
constexpr uint16_t DS_SEG_TILEGFX    = 0x2E5F; // tile graphics segment
constexpr uint16_t DS_SEG_GS         = 0x2E61; // GS segment (flagged-tile masks)
constexpr uint16_t DS_SEG_TILEMAP    = 0x2E63; // tile map segment
constexpr uint16_t DS_SEG_ANIM       = 0x2E67; // animation/bytecode segment
constexpr uint16_t DS_SEG_FS         = 0x2E69; // FS render tile map segment
constexpr uint16_t DS_SEG_SPRITE     = 0x2E73; // word_2B353: sprite data segment (loader 1167a)
constexpr uint16_t DS_SEG_CHUNK      = 0x2E77; // chunk buffer segment
constexpr uint16_t DS_ANIM_PTR_LO    = 0x2E79; // dword_2B359 lo: anim load cursor offset (loader 116ae)
constexpr uint16_t DS_ANIM_PTR_HI    = 0x2E7B; // dword_2B359 hi: anim load cursor segment

constexpr uint16_t DS_VSYNC_COUNT    = 0xA39C; // word_3287C: vsync wait counter (sub_10130/16775)
constexpr uint16_t DS_VSYNC_CALIB    = 0xA39E; // word_3287E: vsync calibration accumulator (init 0x0010)
constexpr uint16_t DS_PIT_LATCH      = 0xA3A0; // word_32880: PIT latch snapshot (init 0xE3FF)

// Anim interpreter working registers + per-level chunk/resource tables
constexpr uint16_t DS_ANIM_TIMER     = 0x0078; // anim frame delay countdown (interpreter working reg; saved per object)
constexpr uint16_t DS_ANIM_CONT      = 0x007A; // anim continuation bytecode offset (working reg; saved per object)
constexpr uint16_t DS_ANIM_CHUNK_IDS = 0x124D; // per-level anim chunk id table (116ae, step 2)
constexpr uint16_t DS_ANIM_CHUNK_OFF = 0x126D; // anim chunk buffer far-ptr offsets (116ae)
constexpr uint16_t DS_ANIM_CHUNK_SEG = 0x128D; // anim chunk buffer far-ptr segments (116ae)
constexpr uint16_t DS_SPRITE_RES_ID  = 0x12AD; // sprite resource table: chunk ids (0x40B, 1167a fill / 13809 lookup)
constexpr uint16_t DS_SPRITE_RES_BASE= 0x12ED; // sprite resource table: sprite data base offsets

// ---------------------------------------------------------------------------
// Object table columns (ds:[obj + COL]; obj = slot*2)
// ---------------------------------------------------------------------------
constexpr uint16_t OBJ_SPRITE_FLAGS  = 0x044D; // bit15 active, 13-14 prio, 9 hflip, 0-2 type
constexpr uint16_t OBJ_SPRITE_X      = 0x064D; // sprite world X (sub-sprite slots)
constexpr uint16_t OBJ_SPRITE_Y      = 0x074D; // sprite world Y
constexpr uint16_t OBJ_SPRITE_OFF    = 0x084D; // sprite data offset (1-based)
constexpr uint16_t OBJ_SUB_SRC_BASE  = 0x0A4D; // sub-sprite: sprite source data base (356E/32DF/134dc)
constexpr uint16_t OBJ_SUB_SRC_SEG   = 0x0B4D; // sub-sprite: sprite source data segment
constexpr uint16_t OBJ_SPRITE_SEG    = 0x094D; // sprite data segment
constexpr uint16_t OBJ_SUB_CLASS     = 0x054D; // sub-sprite class/layer mask (orig TEST [si+54D],di; anim cmd gate + 0x341F setter)
constexpr uint16_t OBJ_STRIP_COUNT   = 0x0C4D; // type-2 dynamic strip count
constexpr uint16_t OBJ_SPRITE_CUR_X  = 0x0D4D; // sprite current draw X (copied to OLD after erase)
constexpr uint16_t OBJ_SPRITE_CUR_Y  = 0x0E4D; // sprite current draw Y
constexpr uint16_t OBJ_SPRITE_OLD_X  = 0x0F4D; // sprite last-rendered X (dirty-rect erase source)
constexpr uint16_t OBJ_SPRITE_OLD_Y  = 0x104D; // sprite last-rendered Y
constexpr uint16_t OBJ_DIRTY_MODE    = 0x114D; // redraw mode/counter (2 show, 0x200 hide, 0x400 clear)
constexpr uint16_t OBJ_DIRTY_CNT     = 0x114E; // = OBJ_DIRTY_MODE+1 high byte: pending redraw counter (DEC by 1DE05/1DF6A)
constexpr uint16_t OBJ_CODE_SEG      = 0x1355; // VM bytecode segment (0 = slot free)
constexpr uint16_t OBJ_PC            = 0x132D; // VM program counter (resume point)
constexpr uint16_t OBJ_ALT_PC        = 0x137D; // saved call-jump return PC
constexpr uint16_t OBJ_COLL_BITS     = 0x13F5; // collision result bits (per bit-index)
constexpr uint16_t OBJ_X_PREV        = 0x13A5; // previous-frame world X (backup before velocity apply)
constexpr uint16_t OBJ_Y_PREV        = 0x13CD; // previous-frame world Y
constexpr uint16_t OBJ_ANIM_TABLE    = 0x141D; // anim state table selector
constexpr uint16_t OBJ_WIDTH         = 0x1445; // sprite width (anim header byte 9)
constexpr uint16_t OBJ_HEIGHT        = 0x146D; // sprite height (anim header byte 0xA)
constexpr uint16_t OBJ_HALF_H        = 0x1495; // half-height (despawn bounds check y±hh)
constexpr uint16_t OBJ_HALF_W        = 0x14BD; // half-width (despawn bounds check x±hw)
constexpr uint16_t OBJ_BBOX_Y0       = 0x14E5; // bbox top (also generic indexed field base +0x14E5)
constexpr uint16_t OBJ_FIELD_BASE    = 0x14E5; // generic indexed-field bias: field addr = slot_col
                                               // ([b-0x6CBA] (+[obj+0x1995])) + this; numerically the
                                               // BBOX_Y0 column — channels/setters/ops address through it
constexpr uint16_t OBJ_BBOX_Y1       = 0x150D; // bbox bottom
constexpr uint16_t OBJ_BBOX_X0       = 0x1535; // bbox left
constexpr uint16_t OBJ_BBOX_X1       = 0x155D; // bbox right
constexpr uint16_t OBJ_FLAGS         = 0x1585; // status flags (0x40 hflip, 0x80 vflip, 0x200 anim-path, 0x8000 alive)
constexpr uint16_t OBJ_STATE_IDX     = 0x15FD; // object type/state id (scan filters)
constexpr uint16_t OBJ_RES_HANDLE    = 0x15AD; // sprite resource refcount/handle (15505 release)
constexpr uint16_t OBJ_RES_COST      = 0x15D5; // resource cost (15505 subtrahend)
constexpr uint16_t OBJ_CLASS_BITS    = 0x1625; // object class bit mask (anim header +0xF; TEST filters in area scans)
constexpr uint16_t OBJ_ANIM_DX       = 0x164D; // last anim-scripted move delta X (walk mechanics result; flip-negated)
constexpr uint16_t OBJ_ANIM_DY       = 0x1675; // last anim-scripted move delta Y (landing check reads sign)
constexpr uint16_t OBJ_SPAWN_POOL    = 0x169D; // ds:0x374 snapshot at spawn (slot pool selector)
constexpr uint16_t OBJ_ANIM_IDX      = 0x16ED; // animation index (bit15 = none)
constexpr uint16_t OBJ_ANIM_SUB      = 0x16C5; // anim sub-state (op 0D/0E bit ops; 0xFFFF init)
constexpr uint16_t OBJ_TIMER         = 0x1715; // per-object timer
constexpr uint16_t OBJ_WORLD_X       = 0x173D; // world X (pixels)
constexpr uint16_t OBJ_WORLD_Y       = 0x1765; // world Y (pixels)
constexpr uint16_t OBJ_VEL_X_MAX     = 0x178D; // X velocity clamp (anim header +0x11)
constexpr uint16_t OBJ_VEL_Y_MAX     = 0x17B5; // Y velocity clamp (anim header +0x13)
constexpr uint16_t OBJ_TYPE_ID       = 0x17DD; // scan type id (filter-table compares)
constexpr uint16_t OBJ_PARENT        = 0x1805; // spawner slot / linked-list prev (0xFFFF none; unlink pair of OBJ_CHILD)
constexpr uint16_t OBJ_CHILD         = 0x182D; // spawned child slot (op 14 tail)
constexpr uint16_t OBJ_SPRITE_BASE   = 0x1855; // sprite resource base offset
constexpr uint16_t OBJ_STATE_187D    = 0x187D; // per-object state (spawn-cleared; orig template-copies via 13809 MOVSW block, no direct reader found)
constexpr uint16_t OBJ_STATE_18A5    = 0x18A5; // per-object state (spawn-cleared; same template block)
constexpr uint16_t OBJ_STATE_18CD    = 0x18CD; // per-object state (spawn-cleared; same template block)
constexpr uint16_t OBJ_STATE_18F5    = 0x18F5; // per-object state (spawn-cleared; same template block)
constexpr uint16_t OBJ_CUR_SPRITE_IDX= 0x191D; // last decompressed sprite index (loc_134dc skip-if-same; 0xFFFF init)
constexpr uint16_t OBJ_ANIM_PC       = 0x1A0D; // last anim frame bytecode offset (0xFFFF none; saved anim_bx)
constexpr uint16_t OBJ_ANIM_TIMER    = 0x1A35; // per-object save of DS_ANIM_TIMER across frames
constexpr uint16_t OBJ_ANIM_CONT     = 0x1A5D; // per-object save of DS_ANIM_CONT across frames
constexpr uint16_t OBJ_SUB_SLOT      = 0x1A85; // first sub-sprite slot
constexpr uint16_t OBJ_SUB_END       = 0x1AAD; // sub-sprite slot end (exclusive)
constexpr uint16_t OBJ_SUB_COUNT     = 0x1AD5; // sub-sprite count (0 = none; flip-loop gate)
constexpr uint16_t OBJ_SUB_ANIM_PTR  = 0x1AFD; // sub-sprite anim pointer (verify-watch; no direct orig reader)
constexpr uint16_t OBJ_PARTNER       = 0x1995; // partner/link slot (indexed_1995 addressing base)
constexpr uint16_t OBJ_VEL_X         = 0x1945; // X velocity (sub_15517 clears)
constexpr uint16_t OBJ_VEL_Y         = 0x196D; // Y velocity
constexpr uint16_t OBJ_FRAC_X        = 0x19BD; // X sub-pixel velocity accumulator (byte add; snaps clear the word)
constexpr uint16_t OBJ_FRAC_Y        = 0x19E5; // Y sub-pixel velocity accumulator (byte add; snaps clear the word)
constexpr uint16_t OBJ_COLL_TABLE    = 0x1B25; // collision partner table ((obj<<4)+bit_idx base)

// Viking-indexed rows (vk = viking slot 0/2/4)
constexpr uint16_t VIK_PORTRAIT      = 0x15AD; // word_29A8D+vk: current portrait idx (11B0B) — aliases OBJ_RES_HANDLE column semantics on viking rows
constexpr uint16_t VIK_HEALTH        = 0x16ED; // word_29BCD+vk row inside 120FF health calc (aliases OBJ_ANIM_IDX col)

// ---------------------------------------------------------------------------
// Static LUT bases (bytecode-index subtractive addressing: ds:[idx - BASE])
// ---------------------------------------------------------------------------
constexpr uint16_t LUT_SCAN_FILTER   = 0x6B34; // subtractive base: DATA at ds:0x94CC (wrap of 0-0x6B34); FF-terminated type-byte lists; JB/JZ scans (op 2C/2D/35) walk ds:[i-0x6B34]
constexpr uint16_t LUT_FIELD_OFF     = 0x6CBA; // subtractive base: DATA at ds:0x9348 (33 words, values are +OBJ_FIELD_BASE column offsets, e.g. b=2 -> 0x28 -> BBOX_Y1); [b-0x6CBA]
constexpr uint16_t LUT_BIT_MASK      = 0x6C34; // subtractive base: DATA at ds:0x93CC (16 words 1<<i); [idx-0x6C34]
constexpr uint16_t LUT_BIT_CLEAR     = 0x6C14; // subtractive base: DATA at ds:0x93EC (16 words ~(1<<i)); [idx-0x6C14]
constexpr uint16_t LUT_BYTE_AND      = 0x6C3C; // subtractive base: DATA at ds:0x93C4 (8 bytes FE FD FB F7 EF DF BF 7F); [bit-0x6C3C]
constexpr uint16_t LUT_BYTE_OR       = 0x6C44; // subtractive base: DATA at ds:0x93BC (8 bytes 01..80); [bit-0x6C44]
constexpr uint16_t LUT_KBD_ASCII     = 0x8E68; // INT9 scancode->ASCII word LUT, 128 entries: read as ds:[scan*2 - 0x7198] (uint16 wrap lands here; #37/#62); '1'..'9','0' at scan 2..11, ENTER->0x81, vowels->0
constexpr uint16_t DS_AUDIO_CMD_TBL  = 0x2BA6; // off_2B086: sub_1086f command dispatch table, 6 CS handler addrs + null (call off_2B086[si] @eip 0x890)
constexpr uint16_t LUT_ROW_BASE      = 0x7098; // subtractive base: DATA IS fs_row_off_tbl at ds:0x8F68 (wrap of 0-0x7098); ds:[row*2 - 0x7098]

// ---------------------------------------------------------------------------
// HUD / portrait tracking
// ---------------------------------------------------------------------------
constexpr uint16_t DS_HUD_SEL        = 0x0414; // word_288F4[3]: selector value per viking (0x414/416/418)
constexpr uint16_t DS_HUD_SEL_PREV   = 0x041A; // tracked previous selector (0x41A/41C/41E)
constexpr uint16_t DS_HUD_ITEMS      = 0x03E4; // item table current (12 words, sub_12199 source)
constexpr uint16_t DS_HUD_ITEMS_PREV = 0x03FC; // item table tracked copy
constexpr uint16_t DS_HUD_ITEM_GFX   = 0x507D; // HUD item graphics: item_id*256, 4 planes x 16 rows x 4 bytes (sub_1183d source)
constexpr uint16_t DS_HUD_SEL_GFX    = 0x637D; // HUD selector cursor graphics: 256 bytes (sub_118ad source)
constexpr uint16_t DS_HUD_HEALTH     = 0x0435; // health state per viking (0x435/437/439)
constexpr uint16_t DS_HUD_HEALTH_PREV= 0x043B; // tracked previous health (0x43B/43D/43F)
constexpr uint16_t DS_PORTRAIT_PREV  = 0x0423; // word_28903[3]: tracked portrait idx (11B0B prev)
constexpr uint16_t DS_PORTRAIT_SND   = 0x0429; // word_28909[3]: portrait sound state
constexpr uint16_t DS_PORTRAIT_SND_PREV = 0x042F; // word_2890F[3]: tracked sound state

// ---------------------------------------------------------------------------
// VGA page roles / panning (page emulator anchors)
// ---------------------------------------------------------------------------
constexpr uint16_t DS_PAGE_DRAW      = 0x92F7; // page role: draw target this sub-frame
constexpr uint16_t DS_PAGE_SHOWN     = 0x92F9; // page role: shown page
constexpr uint16_t DS_PAGE_BG        = 0x92FB; // page role: clean background (1de05 latch source)
constexpr uint16_t DS_PIXEL_PAN      = 0x92EE; // byte_317CE: CRTC pixel pan value (myPixelOffset = /2)
constexpr uint16_t DS_PAN_GATE       = 0x92F2; // byte_317DF: pan/palette dispatch enable gate

// ---------------------------------------------------------------------------
// DATA.DAT chunk loading / FS
// ---------------------------------------------------------------------------
constexpr uint16_t DS_CHUNK_HDR      = 0x2BB4; // chunk header read buffer: fread 8B, dword@+0 = data offset
constexpr uint16_t DS_FS_ROW_OFF_TBL = 0x8F68; // FS row->byte-offset LUT (0x100 entries, built as i*stride where stride=DS_MAP_BP*2)
constexpr uint16_t DS_FS_PAGE_STRIDE = 0x8F6C; // FS page stride word = ELEMENT 2 of DS_FS_ROW_OFF_TBL (2 tile rows * stride; builder loc_16595 fills i*stride for i=0..255)

// ---------------------------------------------------------------------------
// RNG pair (op 0x0F random branch)
// ---------------------------------------------------------------------------
constexpr uint16_t DS_RNG_TIMER      = 0x0352; // word_28832: timer-path RNG word — xchg ah,al; store; rcl ax,3 (CF=0 entry); xor into stored
constexpr uint16_t DS_RNG_SEED       = 0x8639; // dword_30B19: 32-bit LCG state — seed = seed*0x15A4E35 + 1; result = high word

// ---------------------------------------------------------------------------
// Palette pipeline (source -> shaded output -> DAC)
// ---------------------------------------------------------------------------
constexpr uint16_t DS_PAL_FLAGS      = 0x7EFD; // palette state flag byte (op_3E: AND 0xFE; ==0 gates PAL_SRC_PTR reset)
constexpr uint16_t DS_PAL_REQ        = 0x7EFE; // word_303DE: palette write request (0=done, 2=rotate update, 4=full write)
constexpr uint16_t DS_PAL_SRC_PTR    = 0x7F00; // which palette DAC write uses next: DS_PAL_SRC or DS_PAL_OUT
constexpr uint16_t DS_PAL_SRC        = 0x7F02; // source palette, 768B (256*RGB)
constexpr uint16_t DS_PAL_SRC_C192   = 0x8142; // = DS_PAL_SRC + 192*3: op_13/D9 48B block target (colors 192-207)
constexpr uint16_t DS_PAL_SRC_C224   = 0x81A2; // = DS_PAL_SRC + 224*3: same 48B block duplicated (colors 224-239)
constexpr uint16_t DS_PAL_OUT        = 0x8202; // = DS_PAL_SRC + 0x300: shaded/output palette, 768B (10f03/10e99 dest, DAC source)

// ---------------------------------------------------------------------------
// Per-page tile-row VGA offset LUT
// ---------------------------------------------------------------------------
constexpr uint16_t LUT_PAGE_ROW      = 0x89F8; // repeating VGA row-address table: 0x1600+i*0x2B0 period of 78 words repeated 7x + 14-word tail = 560 words to 8E58 (page bases index into the repeats; overflow-safe by repetition)
constexpr uint16_t LUT_SUBROW        = 0x8E58; // 8 words: VGA byte offset of sub-tile row (y&7)*86 (set_display_memory_addr y_low)

// ---------------------------------------------------------------------------
// Held-special-key state bytes (INT9 cluster 0x916C+; full map = v2_keymap.cpp)
// ---------------------------------------------------------------------------
constexpr uint16_t DS_KEY_Q          = 0x917C; // Q held (quit combo path)
constexpr uint16_t DS_KEY_X          = 0x9199; // X held (ALT+X quit)
constexpr uint16_t DS_KEY_ALT        = 0x91A4; // ALT held (heavily traced via V2_WRITE_91A4_SHAD)
constexpr uint16_t DS_KEY_F10        = 0x91B0; // F10 held (password/palette-transform path)

// ---------------------------------------------------------------------------
// Sprite/UI redraw gates + glyph buffer
// ---------------------------------------------------------------------------
constexpr uint16_t DS_SPRITE_FORCE   = 0x9568; // byte: force-render all sprites this pass (set on page rotate, cleared by 1DD9C path)
constexpr uint16_t DS_TEXT_FULLSCREEN= 0x9569; // word_31A49: fullscreen text mode flag (1E0C7 layout switch)
constexpr uint16_t DS_GLYPH_DIRTY    = 0x956B; // byte: glyph buffer dirty (gates 1E0C7 flush)
constexpr uint16_t DS_GLYPH_BUF      = 0x956C; // glyph buffer, 0x1B8 words (12816 clear, 1241e put, 1E0C7 flush)
constexpr uint16_t DS_UI_THROTTLE    = 0x98DC; // word_31DBC: UI redraw throttle counter (JG 3 -> skip, else INC)


// ---------------------------------------------------------------------------
// DS globals batch 1 (scroll / palette-shade / anim-cursor / search scratch)
// ---------------------------------------------------------------------------
constexpr uint16_t DS_ANIM_SLOT         = 0x007C; // word_2855C: anim sub-sprite loop cursor start (exec_anim_cmd si=ds:7C)
constexpr uint16_t DS_ANIM_SLOT_END     = 0x0080; // word_28560: anim sub-sprite loop cursor end
constexpr uint16_t DS_PAL_SHADE_R       = 0x0342; // word_28822: palette shade accumulator R (sub_10e99, ORed with 0x345)
constexpr uint16_t DS_PAL_SHADE_G       = 0x0343; // byte: palette shade accumulator G (ORed with 0x346)
constexpr uint16_t DS_PAL_SHADE_B       = 0x0344; // byte: palette shade accumulator B (ORed with 0x347)
constexpr uint16_t DS_PAL_SHADE_R2      = 0x0345; // byte: palette shade accumulator R second term
constexpr uint16_t DS_PAL_SHADE_G2      = 0x0346; // byte: palette shade accumulator G second term
constexpr uint16_t DS_PAL_SHADE_B2      = 0x0347; // byte: palette shade accumulator B second term
constexpr uint16_t DS_SCROLL_DELTA_X    = 0x034E; // word_2882E: per-frame viewport scroll delta X (movers write, CRTC pan)
constexpr uint16_t DS_SCROLL_DELTA_Y    = 0x0350; // word_28830: per-frame viewport scroll delta Y
constexpr uint16_t DS_SPAWN_POOL_SEL    = 0x0374; // word_28854: spawn slot-pool selector (13d68 pool by count; op14/spawn)
constexpr uint16_t DS_ANIM_SUB_MASK     = 0x038C; // word_2886C: anim sub-sprite mask (exec_anim_cmd gate ds:0x38C)
constexpr uint16_t DS_SCROLL_LOCK_X     = 0x0394; // word_28874: horizontal scroll lock (movers 17496/1746c early-return)
constexpr uint16_t DS_SCROLL_LOCK_Y     = 0x0396; // word_28876: vertical scroll lock (movers 174e9/174bf early-return)
constexpr uint16_t DS_SHAKE_X           = 0x039E; // word_2887E: screen-shake X offset (emu_eff vp+shake, 16775 pan)
constexpr uint16_t DS_SHAKE_Y           = 0x03A0; // word_28880: screen-shake Y offset
constexpr uint16_t DS_SEARCH_RES_TYPE   = 0x03B2; // word_28892: tile/obj search result type id
constexpr uint16_t DS_SEARCH_RES_SLOT   = 0x03B4; // word_28894: tile/obj search result slot (0xFFFF=none)
constexpr uint16_t DS_SCROLL_AMT_LEFT   = 0x03D8; // word_288B8: pending scroll amount, left (camera_follow/scroll_apply)
constexpr uint16_t DS_SCROLL_AMT_RIGHT  = 0x03DA; // word_288BA: pending scroll amount, right
constexpr uint16_t DS_SCROLL_AMT_DOWN   = 0x03DC; // word_288BC: pending scroll amount, down
constexpr uint16_t DS_SCROLL_AMT_UP     = 0x03DE; // word_288BE: pending scroll amount, up
constexpr uint16_t DS_SCROLL_STEP2_TBL  = 0x2B80; // scroll pixel LUT base (one rising table 0,0,0,1,1,1..6 = 19 words to 0x2BA6; STEP1/AMT are +2/+4 shifted bases into the SAME table)
constexpr uint16_t DS_SCROLL_STEP1_TBL  = 0x2B82; // scroll step-1 pixel LUT (v2_scroll_step1_10704 + scroll_lr/ud, indexed by amount*2)
constexpr uint16_t DS_SCROLL_AMT_TBL    = 0x2B84; // scroll amount->pixel LUT (camera_follow_1064b + scroll movers, indexed by amt*2)


// ---------------------------------------------------------------------------
// DS globals batch 2 (level / scroll-state / palette-anim / sound / spawn-table)
// ---------------------------------------------------------------------------
constexpr uint16_t DS_SAVED_VP_X          = 0x257B; // word_2AA5B: viewport X saved at page flip (16775)
constexpr uint16_t DS_SAVED_VP_Y          = 0x257D; // word_2AA5D: viewport Y saved at page flip
constexpr uint16_t DS_SCROLL_COL          = 0x257F; // word_2AA5F: tile-column scroll state (vp_x>>3)
constexpr uint16_t DS_SCROLL_ROW          = 0x2581; // word_2AA61: tile-row scroll state (vp_y>>3)
constexpr uint16_t DS_PAL_ANIM_EN         = 0x2583; // byte_2AA63: palette-animation enable bits (pal_anim 10ffc)
constexpr uint16_t DS_PAL_ANIM_RELOAD     = 0x2584; // byte[8] @2AA64: palette-anim per-slot timer reload values
constexpr uint16_t DS_PAL_ANIM_TIMER      = 0x258C; // byte[8] @2AA6C: palette-anim per-slot countdown timers
constexpr uint16_t DS_PAL_ANIM_START      = 0x2594; // byte[8] @2AA74: palette-anim per-slot start color
constexpr uint16_t DS_PAL_ANIM_END        = 0x259C; // byte[8] @2AA7C: palette-anim per-slot end color
constexpr uint16_t DS_SCROLL_LIMIT_X      = 0x25A4; // word_2AA84: viewport scroll X limit (level width*16-0x140)
constexpr uint16_t DS_SCROLL_LIMIT_Y      = 0x25A6; // word_2AA86: viewport scroll Y limit (level height*16-0xB0)
constexpr uint16_t DS_LEVEL_PREV          = 0x25AB; // word_2AA8B: previous level id (saved before transition)
constexpr uint16_t DS_MUSIC_TRACK         = 0x25AF; // word_2AA8F: current music track (0xFFFF = none)
constexpr uint16_t DS_ANIM_SCROLL_DX      = 0x25B3; // word_2AA93: anim scroll delta X (135cf adds to OBJ_ANIM_DX)
constexpr uint16_t DS_ANIM_SCROLL_DY      = 0x25B5; // word_2AA95: anim scroll delta Y (135cf adds to OBJ_ANIM_DY)
constexpr uint16_t DS_SND_TYPE            = 0x25B7; // byte_2AA97: level-enter sound dispatch type (music_dispatch)
constexpr uint16_t DS_SND_TRACK           = 0x25B8; // word_2AA98: sound track index
constexpr uint16_t DS_SND_FLAG            = 0x25B9; // byte_2AA99: sound state flag (1 = skip music slot)
constexpr uint16_t DS_ACTIVE_VK_SEL       = 0x25BA; // byte_2AA9A: active-viking selector (nonzero -> use word_288A2)
constexpr uint16_t DS_SPAWN_X             = 0x25BB; // word_2AA9B: level-spawn X (copied to ds:0x6C)
constexpr uint16_t DS_SPAWN_Y             = 0x25BD; // word_2AA9D: level-spawn Y (copied to ds:0x6E)
constexpr uint16_t DS_SPAWN_CODE          = 0x25BF; // word_2AA9F: level-spawn code-seg idx (13809 arg)
constexpr uint16_t DS_SPAWN_ANIM          = 0x25C1; // word_2AAA1: level-spawn anim/flags (13809 arg)
constexpr uint16_t DS_SPAWN_POOL0         = 0x25C3; // word_2AAA3: level-spawn initial pool value -> ds:0x374
constexpr uint16_t DS_LEVEL_LOAD          = 0x25C9; // word_2AAA9: level index being loaded (load_level; spawn-table writes)
constexpr uint16_t DS_LEVEL_LOAD_AUX      = 0x25CB; // word_2AAAB: aux level-load field (di_val in load_level)
constexpr uint16_t DS_MAP_BP              = 0x25DC; // word_2AABC: tile-map bp cursor (1C8F1 flagged-tile scan)
constexpr uint16_t DS_MAP_HEIGHT          = 0x25DE; // word_2AABE: level map height in tiles


// ---------------------------------------------------------------------------
// DS globals batch 3 (search/collision scratch, shake, pan, pw-chars, spawn-table)
// ---------------------------------------------------------------------------
constexpr uint16_t DS_CMD_ACTIVE          = 0x0308; // byte_287E8: command-loop active flag (set 1 at 2867 / 0 at 2874)
constexpr uint16_t DS_PW_CHAR1            = 0x0312; // word_287F2: password char slot 1 (continuation of DS_PW_CHAR0 area)
constexpr uint16_t DS_PW_CHAR2            = 0x0314; // word_287F4: password char slot 2
constexpr uint16_t DS_PW_CHAR3            = 0x0316; // word_287F6: password char slot 3
constexpr uint16_t DS_SCRATCH_32E         = 0x032E; // byte_2880E: init-cleared scratch (no distinct orig reader)
constexpr uint16_t DS_SCRATCH_331         = 0x0331; // word_28811: init-cleared scratch
constexpr uint16_t DS_SCRATCH_336         = 0x0336; // word_28816: init-cleared scratch
constexpr uint16_t DS_OBJ_SCAN_START      = 0x033C; // word_2881C: object-loop start index (game_mode_init sets 0/2)
constexpr uint16_t DS_HUD_DISP_MODE       = 0x0340; // word_28820: HUD/portrait display mode (sub_117ad sets 2)
constexpr uint16_t DS_SCRATCH_348         = 0x0348; // word_28828: init-cleared scratch
constexpr uint16_t DS_SCRATCH_34A         = 0x034A; // word_2882A: init-cleared scratch
constexpr uint16_t DS_SCRATCH_34C         = 0x034C; // word_2882C: init-cleared scratch
constexpr uint16_t DS_COLL_STATE_392      = 0x0392; // word_28872: collision state field (init-cleared)
constexpr uint16_t DS_COLL_STATE_398      = 0x0398; // word_28878: collision state field (set 1)
constexpr uint16_t DS_SHAKE_SRC_X         = 0x039A; // word_2887A: shake source X (XORed into DS_SHAKE_X)
constexpr uint16_t DS_SHAKE_SRC_Y         = 0x039C; // word_2887C: shake source Y (XORed into DS_SHAKE_Y)
constexpr uint16_t DS_SHAKE_GATE_X        = 0x03A2; // word_28882: shake enable gate X
constexpr uint16_t DS_SHAKE_GATE_Y        = 0x03A4; // word_28884: shake enable gate Y
constexpr uint16_t DS_PAN_X               = 0x03A6; // word_28886: horizontal pan (from LUT [di-0x7AC2])
constexpr uint16_t DS_PAN_Y               = 0x03A8; // word_28888: vertical pan (from LUT [di-0x7ABC])
constexpr uint16_t DS_SEARCH_FILTER       = 0x03AA; // word_2888A: object-search filter index scratch
constexpr uint16_t DS_SEARCH_JUMP         = 0x03AC; // word_2888C: object-search jump-target scratch
constexpr uint16_t DS_SEARCH_Y            = 0x03AE; // word_2888E: object-search Y scratch (bbox_y0-1)
constexpr uint16_t DS_SEARCH_SI           = 0x03B0; // word_28890: object-search current slot scratch
constexpr uint16_t DS_SEARCH_BEST         = 0x03CA; // word_288AA: object-search best-match slot
constexpr uint16_t DS_SCRATCH_3CE         = 0x03CE; // word_288AE: init-cleared scratch
constexpr uint16_t DS_SCRATCH_3D0         = 0x03D0; // word_288B0: init-cleared scratch
constexpr uint16_t DS_HUD_SEL_SI          = 0x03D4; // word_288B4: HUD selector slot scratch
constexpr uint16_t DS_SCRATCH_3D6         = 0x03D6; // word_288B6: init-cleared scratch
constexpr uint16_t DS_SPAWN_TBL_LO        = 0x03E0; // word_288C0: level spawn-table field (di+0x25FA)
constexpr uint16_t DS_SPAWN_TBL_HI        = 0x03E2; // word_288C2: level spawn-table field (di+0x25FC)


// ---------------------------------------------------------------------------
// DS globals batch 4 (page-flip/scroll display state + spec-key init bit-masks)
// ---------------------------------------------------------------------------
constexpr uint16_t DS_SPEC_MASK_1EE       = 0x91EE; // word_313CE: spec-key state bit-mask (init 0x1000)
constexpr uint16_t DS_SPEC_MASK_20A       = 0x920A; // word_313EA: spec-key state bit-mask (init 0x2000)
constexpr uint16_t DS_SPEC_MASK_210       = 0x9210; // word_313F0: spec-key state bit-mask (init 0x40)
constexpr uint16_t DS_SPEC_MASK_21E       = 0x921E; // word_313FE: spec-key state bit-mask (init 0x1000)
constexpr uint16_t DS_SPEC_MASK_224       = 0x9224; // word_31404: spec-key state bit-mask (init 0x8000)
constexpr uint16_t DS_SPEC_MASK_226       = 0x9226; // word_31406: spec-key state bit-mask (init 0x20)
constexpr uint16_t DS_SPEC_MASK_22A       = 0x922A; // word_3140A: spec-key state bit-mask (init 0x80)
constexpr uint16_t DS_SPEC_MASK_22C       = 0x922C; // word_3140C: spec-key state bit-mask (init 0x4000)
constexpr uint16_t DS_SPEC_MASK_22E       = 0x922E; // word_3140E: spec-key state bit-mask (init 0x8000)
constexpr uint16_t DS_SPEC_MASK_25E       = 0x925E; // word_3143E: spec-key state bit-mask (init 0x8000)
constexpr uint16_t DS_SPEC_MASK_260       = 0x9260; // word_31440: spec-key state bit-mask (init 0x2000)
constexpr uint16_t DS_SPEC_MASK_27A       = 0x927A; // word_3145A: spec-key state bit-mask (init 0x20)
constexpr uint16_t DS_SPEC_MASK_27C       = 0x927C; // word_3145C: spec-key state bit-mask (init 0x800)
constexpr uint16_t DS_SPEC_MASK_27E       = 0x927E; // word_3145E: spec-key state bit-mask (init 0x10)
constexpr uint16_t DS_SPEC_MASK_282       = 0x9282; // word_31462: spec-key state bit-mask (init 0x200)
constexpr uint16_t DS_SPEC_MASK_284       = 0x9284; // word_31464: spec-key state bit-mask (init 0x400)
constexpr uint16_t DS_SPEC_MASK_286       = 0x9286; // word_31466: spec-key state bit-mask (init 0x100)
constexpr uint16_t DS_SPEC_MASK_288       = 0x9288; // word_31468: spec-key state bit-mask (init 0x8000)
constexpr uint16_t DS_SPEC_MASK_28C       = 0x928C; // word_3146C: spec-key state bit-mask (init 0x400)
constexpr uint16_t DS_SPEC_MASK_290       = 0x9290; // word_31470: spec-key state bit-mask (init 0x10)
constexpr uint16_t DS_SCROLL_DISP_X       = 0x92EF; // word_317CF: scroll display X (pixel pan low)
constexpr uint16_t DS_SCROLL_DISP_Y       = 0x92F1; // word_317D1: scroll display Y
constexpr uint16_t DS_SCROLL_DISP_X2      = 0x92F3; // word_317D3: scroll display X halved (>>1)
constexpr uint16_t DS_SCROLL_DISP_Y2      = 0x92F5; // word_317D5: scroll display Y halved
constexpr uint16_t DS_VGA_PAGE_FLAG       = 0x92FF; // byte_317DF: VGA page/mode restore flag (1686f)
constexpr uint16_t DS_VGA_MODE_BYTE       = 0x9300; // byte_317E0: VGA mode byte (0xFF gate in mode-restore)
constexpr uint16_t DS_PAGE_ROWCUR_2       = 0x9305; // word_317E5: page-2 row cursor (shown role + row)
constexpr uint16_t DS_PAGE_ROWCUR_3       = 0x9307; // word_317E7: page-3 row cursor (bg role + row)
constexpr uint16_t DS_PAGE_ROWCUR_1       = 0x9309; // word_317E9: page-1 row cursor (draw role + row)
constexpr uint16_t DS_PAGE_VGA_2          = 0x930B; // word_317EB: page-2 VGA row address
constexpr uint16_t DS_PAGE_VGA_3          = 0x930D; // word_317ED: page-3 VGA row address
constexpr uint16_t DS_PAGE_VGA_1          = 0x930F; // word_317EF: page-1 VGA row address
constexpr uint16_t DS_PAGE_SPLIT_1A       = 0x9311; // byte_317F1: page split-screen counter 1a (0x9C modulo)
constexpr uint16_t DS_PAGE_SPLIT_1B       = 0x9312; // byte_317F2: page split counter 1b (0x32 - 1a)
constexpr uint16_t DS_PAGE_SPLIT_2A       = 0x9313; // byte_317F3: page split counter 2a
constexpr uint16_t DS_PAGE_SPLIT_2B       = 0x9314; // byte_317F4: page split counter 2b
constexpr uint16_t DS_PAGE_COPY_SRC1      = 0x9315; // word_317F5: page-copy source 1 (1712b)
constexpr uint16_t DS_PAGE_COPY_DST1      = 0x9317; // word_317F7: page-copy dest 1
constexpr uint16_t DS_PAGE_COPY_SRC2      = 0x9319; // word_317F9: page-copy source 2
constexpr uint16_t DS_PAGE_COPY_DST2      = 0x931B; // word_317FB: page-copy dest 2
constexpr uint16_t DS_PAGE_COPY_SRC3      = 0x931D; // word_317FD: page-copy source 3
constexpr uint16_t DS_PAGE_SCRATCH_346    = 0x9346; // word_31826: page-related scratch


// ---------------------------------------------------------------------------
// DS globals batch 5 (HUD selector[2..3] / viking state / quit-prompt)
// ---------------------------------------------------------------------------
constexpr uint16_t DS_HUD_SEL_2           = 0x0416; // word_288F6: HUD selector value, viking slot 2 (DS_HUD_SEL[1])
constexpr uint16_t DS_HUD_SEL_3           = 0x0418; // word_288F8: HUD selector value, viking slot 3 (DS_HUD_SEL[2])
constexpr uint16_t DS_HUD_SEL_PREV_2      = 0x041C; // word_288FC: tracked previous selector, viking 2
constexpr uint16_t DS_HUD_SEL_PREV_3      = 0x041E; // word_288FE: tracked previous selector, viking 3
constexpr uint16_t DS_PORTRAIT_SND_2      = 0x0425; // word_28905: portrait sound state viking 2 (-> viking row 0x15AF)
constexpr uint16_t DS_PORTRAIT_SND_3      = 0x0427; // word_28907: portrait sound state viking 3 (-> viking row 0x15B1)
constexpr uint16_t DS_HUD_SCRATCH_42B     = 0x042B; // word_2890B: HUD scratch (cleared)
constexpr uint16_t DS_HUD_SCRATCH_42D     = 0x042D; // word_2890D: HUD scratch (cleared)
constexpr uint16_t DS_VK_STATE_1          = 0x0431; // word_28911: per-viking state field 1 (init 0xFFFF)
constexpr uint16_t DS_VK_STATE_2          = 0x0433; // word_28913: per-viking state field 2 (init 0xFFFF)
constexpr uint16_t DS_VK_STATE_3          = 0x0437; // word_28917: per-viking state field 3 (init 0xFFFF)
constexpr uint16_t DS_VK_STATE_4          = 0x0439; // word_28919: per-viking state field 4 (init 0xFFFF)
constexpr uint16_t DS_HUD_TRACK_1         = 0x043D; // word_2891D: HUD tracked-prev field 1
constexpr uint16_t DS_HUD_TRACK_2         = 0x043F; // word_2891F: HUD tracked-prev field 2
constexpr uint16_t DS_HUD_BLINK_FIELD     = 0x0441; // word_28921: HUD blink-gated field (cnt&0x10)
constexpr uint16_t DS_QUIT_ACTIVE         = 0x0443; // word_28923: quit-prompt active flag (set 1)
constexpr uint16_t DS_QUIT_BLINK          = 0x0445; // word_28925: quit-prompt blink counter (init 0x11, DEC in 10555)
constexpr uint16_t DS_QUIT_MODE           = 0x0447; // word_28927: quit-prompt mode
constexpr uint16_t DS_HUD_FORCE           = 0x044B; // byte_2892B: HUD force-redraw byte (set 0xFF)
constexpr uint16_t DS_HUD_FIELD_483       = 0x0483; // word_28963: HUD field (verify-cross-ref)


// ---------------------------------------------------------------------------
// DS globals batch 6 (truly-new addrs; already-named addrs use existing phase-A names)
// ---------------------------------------------------------------------------
constexpr uint16_t DS_AIM_SIGN_X            = 0x003E; // word_2851E: aim/trajectory X-direction sign (negate xr; XOR OBJ_FLAGS&0x40)
constexpr uint16_t DS_AIM_SIGN_Y            = 0x0040; // word_28520: aim/trajectory Y-direction sign (negate yr; XOR OBJ_FLAGS&0x80)
constexpr uint16_t DS_FLAG_202              = 0x0202; // word_286E2: state flag (set 1; 0x202 also dirty-mode marker value)
constexpr uint16_t DS_HUD_DRAW_DI           = 0x0421; // word_28901: cached di draw-offset for HUD portrait/selector
constexpr uint16_t DS_HUD_FIELD_449         = 0x0449; // word_28929: HUD field (init 0xFFFF)
constexpr uint16_t DS_PROBE_77C             = 0x077C; // word_28C5C: divergence-probe address (no orig symbol reader; debug watch[])
constexpr uint16_t DS_RENDER_117D           = 0x117D; // word_2965D: dd9c-render field, block 0x117D-0x1181 (verify-probed)
constexpr uint16_t DS_RENDER_117E           = 0x117E; // word_2965E: dd9c-render field (block 0x117D-0x1181)
constexpr uint16_t DS_RENDER_117F           = 0x117F; // word_2965F: dd9c-render field (block 0x117D-0x1181)
constexpr uint16_t DS_VK_PORTRAIT_SND_2     = 0x15AF; // word_29A8F: viking-2 HUD row portrait-sound (from DS_PORTRAIT_SND_2)
constexpr uint16_t DS_VK_PORTRAIT_SND_3     = 0x15B1; // word_29A91: viking-3 HUD row portrait-sound (from DS_PORTRAIT_SND_3)
constexpr uint16_t DS_VK_HEALTH_2           = 0x16EF; // word_29BCF: viking-2 HUD row health value
constexpr uint16_t DS_VK_HEALTH_3           = 0x16F1; // word_29BD1: viking-3 HUD row health value
constexpr uint16_t DS_OBJ_QUEUE_HEAD        = 0x2191; // word_2A671: head index of 0x800-word queue at ds:0x2191 (init 2; ADD 2 push)
constexpr uint16_t DS_TRANSITION_CHUNK_BUF  = 0x2193; // transition-chunk load buffer (v2_read_chunk target, 0x10000-0x2193 bytes; = queue body region)
constexpr uint16_t DS_PAL_ANIM_TIMER_1      = 0x258D; // byte_2AA6D: palette-anim timer slot 1
constexpr uint16_t DS_PAL_ANIM_TIMER_2      = 0x258E; // byte_2AA6E: palette-anim timer slot 2
constexpr uint16_t DS_PAL_ANIM_TIMER_3      = 0x258F; // byte_2AA6F: palette-anim timer slot 3
constexpr uint16_t DS_PAL_ANIM_TIMER_4      = 0x2590; // byte_2AA70: palette-anim timer slot 4
constexpr uint16_t DS_PAL_ANIM_TIMER_5      = 0x2591; // byte_2AA71: palette-anim timer slot 5
constexpr uint16_t DS_PAL_ANIM_TIMER_6      = 0x2592; // byte_2AA72: palette-anim timer slot 6
constexpr uint16_t DS_PAL_ANIM_TIMER_7      = 0x2593; // byte_2AA73: palette-anim timer slot 7
constexpr uint16_t DS_DAC_R_SAVE            = 0x25A8; // word_2AA88: saved DAC R (from DS_DAC_R at page flip)
constexpr uint16_t DS_DAC_B_SAVE            = 0x25AA; // word_2AA8A: saved DAC B (from DS_DAC_B)
constexpr uint16_t DS_CHUNK_CUR             = 0x25E1; // word_2AAC1: current level chunk id
constexpr uint16_t DS_CHUNK_TILE            = 0x25E3; // word_2AAC3: tile-layer chunk id
constexpr uint16_t DS_CHUNK_BG              = 0x25E5; // word_2AAC5: background-layer chunk id
constexpr uint16_t DS_DECOMP_SIZE           = 0x2BBC; // word_2B09C: last decompressed chunk size (LZSS output length)
constexpr uint16_t DS_SEG_TILEDATA          = 0x2E5D; // word_2B33D: tile-data resource segment (-> v2_vm_shadow_gs_tiledata)
constexpr uint16_t DS_DECOMP_DI_END         = 0x2E65; // word_2B345: di offset after decompression (end cursor)
constexpr uint16_t DS_SEG_SOUND             = 0x2E6B; // word_2B34B: sound resource segment (-> v2_vm_shadow_sound)
constexpr uint16_t DS_SEG_SOUND2            = 0x2E6D; // word_2B34D: sound buffer segment 2 (sound_buf_seg + off>>4)
constexpr uint16_t DS_SEG_SOUND3            = 0x2E6F; // word_2B34F: sound buffer segment 3 (sound_buf_seg + off>>4)
constexpr uint16_t DS_TEMPLATE_CHUNK        = 0x2E71; // word_2B351: current template chunk id (init 0x1C6)
constexpr uint16_t DS_ANIM_INIT_OFF         = 0x2E75; // word_2B355: initial anim dword offset (-> DS_ANIM_PTR_LO)
constexpr uint16_t DS_PROBE_414D            = 0x414D; // word_2C62D: divergence-probe field (debug print)
constexpr uint16_t DS_DAC_R                 = 0x7F0B; // byte_303EB: VGA DAC write R component
constexpr uint16_t DS_DAC_G                 = 0x7F0C; // byte_303EC: VGA DAC write G component
constexpr uint16_t DS_DAC_B                 = 0x7F0D; // byte_303ED: VGA DAC write B component
constexpr uint16_t DS_STARTUP_CX            = 0x863B; // word_30B1B: startup probe field (set 0x1234)
constexpr uint16_t DS_INT24_VECTOR          = 0x86AC; // word_30B8C: saved DOS INT 24h critical-error vector
constexpr uint16_t DS_INT24_VECTOR_HI       = 0x86AE; // word_30B8E: saved DOS INT 24h vector (segment word)
constexpr uint16_t DS_MUSIC_MUTE_SRC        = 0x86B2; // word_30B92: config music-mute source (-> DS_MUSIC_MUTE)
constexpr uint16_t DS_SFX_MUTE_SRC          = 0x86B4; // word_30B94: config sfx-mute source (-> DS_SFX_MUTE)
constexpr uint16_t DS_SOUND_CARD            = 0x86B6; // word_30B96: sound-card/driver select (==8 gate)
constexpr uint16_t DS_MUSIC_CARD            = 0x86B8; // word_30B98: music-card/driver select (==3 gate)
constexpr uint16_t DS_DATADAT_MAGIC         = 0x86C4; // word_30BA4: DATA.DAT magic word (expected 0x6969)
constexpr uint16_t DS_BIOS_CHECKSUM         = 0x86D0; // word_30BB0: BIOS checksum probe (#95)
constexpr uint16_t DS_JOYSTICK_PRESENT      = 0x86DA; // word_30BBA: joystick present flag (0 = no joystick)
constexpr uint16_t DS_CMD_HANDLER_TBL       = 0x86E6; // word_30BC6: command handler jump table base (handler=[+cmd*2])
constexpr uint16_t DS_VM_DISPATCH_TBL       = 0x87AE; // word_30C8E: VM runtime sub-dispatch table (off_30C8E, 10 words to off_30CA2; sub-bases off_30C92/94/98 alias inside)
constexpr uint16_t DS_VM_SETTER_TBL         = 0x87C2; // off_30CA2: VM setter dispatch table (5 words to off_30CAC; #41 setter family)
constexpr uint16_t DS_VM_OPTABLE            = 0x87CC; // off_30CAC: THE unified VM opcode table, 216 words (0x00-0xD7); bounds proven by the EXE image dw-run ending exactly at 0x897C (db[32] blocks follow)
constexpr uint16_t DS_ROW38_8FB4            = 0x8FB4; // word_31494: verify-probed field (row38; no orig symbol reader)
constexpr uint16_t DS_CLIP_LIMIT_X          = 0x9168; // word_31648: sprite clip limit X (CMP cx; JGE skip)
constexpr uint16_t DS_CLIP_LIMIT_Y          = 0x916A; // word_3164A: sprite clip limit Y (CMP dx; JGE skip)
constexpr uint16_t DS_SPEC_KEY_Y            = 0x9181; // byte_31661: spec key Y state (SDL INT9 mirror)
constexpr uint16_t DS_SPEC_KEY_S            = 0x918B; // byte_3166B: spec key S state (SDL INT9; ==1 gate)
constexpr uint16_t DS_SPEC_KEY_N            = 0x919D; // byte_3167D: spec key N state (SDL INT9; live atomic)
constexpr uint16_t DS_SPEC_KEY_M            = 0x919E; // byte_3167E: spec key M state (SDL INT9; ==1 gate)
constexpr uint16_t DS_SPEC_KEY_F5           = 0x91AB; // byte_3168B: spec key F5 state (prev level; ==1)
constexpr uint16_t DS_SPEC_KEY_F6           = 0x91AC; // byte_3168C: spec key F6 state (next level; ==1)
constexpr uint16_t DS_SOUND_FIELD_8EA       = 0x98EA; // word_31DCA: sound-init field (set 0xFFFF)
constexpr uint16_t DS_MUSIC_ID              = 0x990C; // word_31DEC: slot 0 of the 5-word sequence HANDLE table [si-0x66F4] (si=0 music, 2..8 SFX -> 990C..9914); the parallel SEQ table [si-0x66EA] is 9916..991E
constexpr uint16_t DS_SOUND_DISPATCH_TBL    = 0xA37A; // off_3285A: music dispatch table, 5 CS handler addrs (7791/77B1/78F1/775D/77B1) - sub_17749 reads via type byte
constexpr uint16_t DS_SND_DESC_OFF_TBL      = 0xA384; // music track -> chunk-id table (11 words, 1D0..202 step 5): sub_1775d reads [track*2-0x5C7C] (wrap lands here), ADDs ds:86B8 and loads that chunk into seg_sound via sub_10982
constexpr uint16_t DS_SOUND_INIT_92A        = 0x992A; // word_31E0A: sound-init field (cleared 0)
constexpr uint16_t DS_SEG_SOUND_BASE        = 0x992C; // word_31E0C: sound resource base segment (snd_base)
constexpr uint16_t DS_SOUND_INIT_932        = 0x9932; // word_31E12: sound-init field (cleared 0)
constexpr uint16_t DS_XMI_BUF_PTR           = 0x9934; // word_31E14: XMI music buffer pointer (#99)
constexpr uint16_t DS_SOUND_FIELD_942       = 0x9942; // word_31E22: sound-init field (set 0x0E00)
constexpr uint16_t DS_AIL_MUSIC_STATE       = 0xA378; // AIL music/card-8 driver state flag (v2_ail_init_17561: gated on DS_SOUND_CARD==8 / DS_MUSIC_CARD==3, toggled around play_music)
constexpr uint16_t DS_AIL_INIT_DONE         = 0xA39A; // AIL sound-init complete flag (v2_ail_init_17561: 0 at entry, 1 when ready or sound disabled)


// ---------------------------------------------------------------------------
// DS globals batch final (input-layer INT9 / byte-save temp / counter)
// ---------------------------------------------------------------------------
constexpr uint16_t DS_BYTE_SAVE_0           = 0x8504; // word_309E4: 3-byte save/restore temp, byte 0 ([di] backup)
constexpr uint16_t DS_BYTE_SAVE_1           = 0x8505; // word_309E5: 3-byte save/restore temp, byte 1 ([di+1] backup)
constexpr uint16_t DS_BYTE_SAVE_2           = 0x8506; // word_309E6: 3-byte save/restore temp, byte 2 ([di+2] backup)
constexpr uint16_t DS_INPUT_JOY             = 0x86DC; // word_30BBC: joystick/input value (INT9 async; read gated by DS_JOYSTICK_PRESENT)
constexpr uint16_t DS_INPUT_ACCUM           = 0x86DE; // word_30BBE: INT9 async input accumulator (atomic OR; input layer, 13 sites)
constexpr uint16_t DS_COUNTER_8734          = 0x8734; // word_30C14: anim-quad queue cursor (WORD index into the
                                                      // 0x8736 record table, step 3 words/record; writer sub_13fc2
                                                      // +3 per on-screen 2x2 animated tile quad, consumer sub_1406d
                                                      // walks it tail-first; cleared 0)
constexpr uint16_t DS_SCRATCH_28            = 0x0028; // byte_28508: game-state init scratch byte (cleared in sub_12ca3; no orig symbol reader)

#endif // V2_DS_LAYOUT_H
