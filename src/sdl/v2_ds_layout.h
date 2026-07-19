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
constexpr uint16_t DS_PRIO_QUEUE     = 0x0378; // byte queue of slots (drained on DI register, #28)
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
constexpr uint16_t DS_CMD_WRITE      = 0x218F; // word_2A66F: command-buffer write offset
constexpr uint16_t DS_CMD_READ       = 0x2B64; // word_2B044: command-buffer read offset
constexpr uint16_t DS_CMD_BUF        = 0x1DA7; // command entry base (type/si/di/param/text fields)

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
constexpr uint16_t OBJ_SPRITE_SEG    = 0x094D; // sprite data segment
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
constexpr uint16_t OBJ_ANIM_PC       = 0x1A0D; // last anim frame bytecode offset (0xFFFF none; saved anim_bx)
constexpr uint16_t OBJ_ANIM_TIMER    = 0x1A35; // per-object save of DS_ANIM_TIMER across frames
constexpr uint16_t OBJ_ANIM_CONT     = 0x1A5D; // per-object save of DS_ANIM_CONT across frames
constexpr uint16_t OBJ_SUB_SLOT      = 0x1A85; // first sub-sprite slot
constexpr uint16_t OBJ_SUB_END       = 0x1AAD; // sub-sprite slot end (exclusive)
constexpr uint16_t OBJ_SUB_COUNT     = 0x1AD5; // sub-sprite count (0 = none; flip-loop gate)
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
constexpr uint16_t LUT_SCAN_FILTER   = 0x6B34; // byte table of object types, sorted; JB/JZ scans (op 2C/2D/35, 15fb1/15fbe, 15fd0) walk ds:[i-0x6B34]
constexpr uint16_t LUT_FIELD_OFF     = 0x6CBA; // word table: bytecode idx -> object-table column offset ([b-0x6CBA], channels/setters/field ops)
constexpr uint16_t LUT_BIT_MASK      = 0x6C34; // word bit-mask table (collision bit idx, op 9D/9E/A1.. masks: [idx-0x6C34])
constexpr uint16_t LUT_BIT_CLEAR     = 0x6C14; // word clear-mask table (op 9D family: AND mask [idx-0x6C14])
constexpr uint16_t LUT_BYTE_AND      = 0x6C3C; // byte AND-mask table (op 0D: [bit-0x6C3C])
constexpr uint16_t LUT_BYTE_OR       = 0x6C44; // byte OR-mask table (op 0E: [bit-0x6C44])
constexpr uint16_t LUT_ROW_BASE      = 0x7098; // tile-map row base words: ds:[row*2 - 0x7098] (141ba/13fc2/render)

// ---------------------------------------------------------------------------
// HUD / portrait tracking
// ---------------------------------------------------------------------------
constexpr uint16_t DS_HUD_SEL        = 0x0414; // word_288F4[3]: selector value per viking (0x414/416/418)
constexpr uint16_t DS_HUD_SEL_PREV   = 0x041A; // tracked previous selector (0x41A/41C/41E)
constexpr uint16_t DS_HUD_ITEMS      = 0x03E4; // item table current (12 words, sub_12199 source)
constexpr uint16_t DS_HUD_ITEMS_PREV = 0x03FC; // item table tracked copy
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
constexpr uint16_t DS_FS_PAGE_STRIDE = 0x8F6C; // FS page stride word (added to FS cursor per page)

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
constexpr uint16_t LUT_PAGE_ROW      = 0x89F8; // 3 pages x 0x1A rows x 2B: VGA offset of tile row y on page p (idx: +pgs[p]+(y>>3)*2); [0] = wrap target in page-copy

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

#endif // V2_DS_LAYOUT_H
