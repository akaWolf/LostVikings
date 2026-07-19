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

// ---------------------------------------------------------------------------
// Object table columns (ds:[obj + COL]; obj = slot*2)
// ---------------------------------------------------------------------------
constexpr uint16_t OBJ_SPRITE_FLAGS  = 0x044D; // bit15 active, 13-14 prio, 9 hflip, 0-2 type
constexpr uint16_t OBJ_SPRITE_X      = 0x064D; // sprite world X (sub-sprite slots)
constexpr uint16_t OBJ_SPRITE_Y      = 0x074D; // sprite world Y
constexpr uint16_t OBJ_SPRITE_OFF    = 0x084D; // sprite data offset (1-based)
constexpr uint16_t OBJ_SPRITE_SEG    = 0x094D; // sprite data segment
constexpr uint16_t OBJ_STRIP_COUNT   = 0x0C4D; // type-2 dynamic strip count
constexpr uint16_t OBJ_DIRTY_MODE    = 0x114D; // redraw mode/counter (2 show, 0x200 hide, 0x400 clear)
constexpr uint16_t OBJ_CODE_SEG      = 0x1355; // VM bytecode segment (0 = slot free)
constexpr uint16_t OBJ_PC            = 0x132D; // VM program counter (resume point)
constexpr uint16_t OBJ_ALT_PC        = 0x137D; // saved call-jump return PC
constexpr uint16_t OBJ_COLL_BITS     = 0x13F5; // collision result bits (per bit-index)
constexpr uint16_t OBJ_Y_PREV        = 0x13CD; // previous-frame world Y
constexpr uint16_t OBJ_ANIM_TABLE    = 0x141D; // anim state table selector
constexpr uint16_t OBJ_WIDTH         = 0x1445; // sprite width (anim header byte 9)
constexpr uint16_t OBJ_HEIGHT        = 0x146D; // sprite height (anim header byte 0xA)
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
constexpr uint16_t OBJ_ANIM_IDX      = 0x16ED; // animation index (bit15 = none)
constexpr uint16_t OBJ_ANIM_SUB      = 0x16C5; // anim sub-state (op 0D/0E bit ops; 0xFFFF init)
constexpr uint16_t OBJ_TIMER         = 0x1715; // per-object timer
constexpr uint16_t OBJ_WORLD_X       = 0x173D; // world X (pixels)
constexpr uint16_t OBJ_WORLD_Y       = 0x1765; // world Y (pixels)
constexpr uint16_t OBJ_TYPE_ID       = 0x17DD; // scan type id (filter-table compares)
constexpr uint16_t OBJ_CHILD         = 0x182D; // spawned child slot (op 14 tail)
constexpr uint16_t OBJ_SPRITE_BASE   = 0x1855; // sprite resource base offset
constexpr uint16_t OBJ_SUB_SLOT      = 0x1A85; // first sub-sprite slot
constexpr uint16_t OBJ_SUB_END       = 0x1AAD; // sub-sprite slot end (exclusive)
constexpr uint16_t OBJ_SUB_COUNT     = 0x1AD5; // sub-sprite count (0 = none; flip-loop gate)
constexpr uint16_t OBJ_PARTNER       = 0x1995; // partner/link slot (indexed_1995 addressing base)
constexpr uint16_t OBJ_VEL_X         = 0x1945; // X velocity (sub_15517 clears)
constexpr uint16_t OBJ_VEL_Y         = 0x196D; // Y velocity
constexpr uint16_t OBJ_COLL_TABLE    = 0x1B25; // collision partner table ((obj<<4)+bit_idx base)

// Viking-indexed rows (vk = viking slot 0/2/4)
constexpr uint16_t VIK_PORTRAIT      = 0x15AD; // word_29A8D+vk: current portrait idx (11B0B) — aliases OBJ_RES_HANDLE column semantics on viking rows
constexpr uint16_t VIK_HEALTH        = 0x16ED; // word_29BCD+vk row inside 120FF health calc (aliases OBJ_ANIM_IDX col)

#endif // V2_DS_LAYOUT_H
