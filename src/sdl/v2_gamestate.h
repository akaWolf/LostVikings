// v2_gamestate.h — Phase D step 1: typed GameState + byte-exact DS serializer.
//
// The struct is the FUTURE high-level game state; DS compatibility lives in
// the SERIALIZER only (per-field writes into the flat 64K image), so the
// struct shape is free to evolve while the byte image stays canonical.
//
// Incremental-carving model:
//   - every field below is declared ONCE in an X-macro list; the same entry
//     generates the struct member, deserialize, serialize and the coverage
//     map — a field cannot be "forgotten" in one direction;
//   - bytes not covered by any field live in a raw backing copy and are
//     serialized verbatim (identity by construction);
//   - a startup pass asserts fields never overlap; roundtrip
//     DS -> struct -> DS' must be memcmp-identical on every checked frame
//     (v2_gs_roundtrip_check), which proves both the field encodings and the
//     coverage completeness on live data;
//   - widths/counts follow v2_ds_layout.h knowledge; where the exact width is
//     still uncertain the tail bytes simply stay raw — identity is unaffected,
//     so typing can proceed field by field without ever going red.
//
// Alias rule: overlapping names from v2_ds_layout.h (VIK_* rows over OBJ_*
// columns, DS_PAL_SRC_C192/C224 inside DS_PAL_SRC, DS_HUD_SEL_2/3 inside the
// DS_HUD_SEL triple, ...) are represented by exactly ONE covering field.

#pragma once
#include <cstdint>
#include "v2_ds_layout.h"

// ---------------------------------------------------------------------------
// Word fields: F1(member, ds_off) scalar; FN(member, ds_off, count) array of
// `count` contiguous uint16_t words starting at ds_off.
// ---------------------------------------------------------------------------

// Core globals + input + scroll + collision scratch
#define V2_GS_FIELDS_CORE(F1, FN) \
  F1(cur_obj,            DS_CUR_OBJ)            \
  F1(viewport_x,         DS_VIEWPORT_X)         \
  F1(viewport_y,         DS_VIEWPORT_Y)         \
  F1(text_idx,           DS_TEXT_IDX)           \
  F1(mode_word,          DS_MODE_WORD)          \
  F1(scratch_34,         DS_SCRATCH_34)         \
  F1(scratch_36,         DS_SCRATCH_36)         \
  F1(scratch_38,         DS_SCRATCH_38)         \
  F1(scratch_3a,         DS_SCRATCH_3A)         \
  F1(aim_sign_x,         DS_AIM_SIGN_X)         \
  F1(aim_sign_y,         DS_AIM_SIGN_Y)         \
  F1(text_col,           DS_TEXT_COL)           \
  F1(text_row,           DS_TEXT_ROW)           \
  F1(anim_timer,         DS_ANIM_TIMER)         \
  F1(anim_cont,          DS_ANIM_CONT)          \
  F1(anim_slot,          DS_ANIM_SLOT)          \
  F1(anim_slot_end,      DS_ANIM_SLOT_END)      \
  F1(accumulator,        DS_ACCUMULATOR)        \
  F1(flag_202,           DS_FLAG_202)           \
  F1(last_char,          DS_LAST_CHAR)          \
  F1(music_mute,         DS_MUSIC_MUTE)         \
  F1(sfx_mute,           DS_SFX_MUTE)           \
  FN(pw_chars,           DS_PW_CHAR0, 4)        \
  F1(transition,         DS_TRANSITION)         \
  F1(scratch_331,        DS_SCRATCH_331)        \
  F1(frame_flags,        DS_FRAME_FLAGS)        \
  F1(scratch_336,        DS_SCRATCH_336)        \
  F1(obj_scan_start,     DS_OBJ_SCAN_START)     \
  F1(hud_disp_mode,      DS_HUD_DISP_MODE)      \
  F1(scratch_348,        DS_SCRATCH_348)        \
  F1(scratch_34a,        DS_SCRATCH_34A)        \
  F1(scratch_34c,        DS_SCRATCH_34C)        \
  F1(scroll_delta_x,     DS_SCROLL_DELTA_X)     \
  F1(scroll_delta_y,     DS_SCROLL_DELTA_Y)     \
  F1(rng_timer,          DS_RNG_TIMER)          \
  F1(rng_seed_lo,        DS_RNG_SEED)           \
  F1(obj_count,          DS_OBJ_COUNT)          \
  F1(spawn_pool_sel,     DS_SPAWN_POOL_SEL)     \
  F1(prio_count,         DS_PRIO_COUNT)         \
  F1(anim_sub_mask,      DS_ANIM_SUB_MASK)      \
  F1(coll_bit_idx,       DS_COLL_BIT_IDX)       \
  F1(coll_phase,         DS_COLL_PHASE)         \
  F1(coll_state_392,     DS_COLL_STATE_392)     \
  F1(scroll_lock_x,      DS_SCROLL_LOCK_X)      \
  F1(scroll_lock_y,      DS_SCROLL_LOCK_Y)      \
  F1(coll_state_398,     DS_COLL_STATE_398)     \
  F1(shake_src_x,        DS_SHAKE_SRC_X)        \
  F1(shake_src_y,        DS_SHAKE_SRC_Y)        \
  F1(shake_x,            DS_SHAKE_X)            \
  F1(shake_y,            DS_SHAKE_Y)            \
  F1(shake_gate_x,       DS_SHAKE_GATE_X)       \
  F1(shake_gate_y,       DS_SHAKE_GATE_Y)       \
  F1(pan_x,              DS_PAN_X)              \
  F1(pan_y,              DS_PAN_Y)              \
  F1(search_filter,      DS_SEARCH_FILTER)      \
  F1(search_jump,        DS_SEARCH_JUMP)        \
  F1(search_y,           DS_SEARCH_Y)           \
  F1(search_si,          DS_SEARCH_SI)          \
  F1(search_res_type,    DS_SEARCH_RES_TYPE)    \
  F1(search_res_slot,    DS_SEARCH_RES_SLOT)    \
  F1(input_keys,         DS_INPUT_KEYS)         \
  F1(input_edges,        DS_INPUT_EDGES)        \
  F1(input_prev,         DS_INPUT_PREV)         \
  F1(active_viking,      DS_ACTIVE_VIKING)      \
  F1(prev_viking,        DS_PREV_VIKING)        \
  F1(blink_counter,      DS_BLINK_COUNTER)      \
  F1(search_best,        DS_SEARCH_BEST)        \
  F1(game_mode_ac,       DS_GAME_MODE_AC)       \
  F1(scratch_3ce,        DS_SCRATCH_3CE)        \
  F1(scratch_3d0,        DS_SCRATCH_3D0)        \
  F1(hud_sel_si,         DS_HUD_SEL_SI)         \
  F1(scratch_3d6,        DS_SCRATCH_3D6)        \
  F1(scroll_amt_left,    DS_SCROLL_AMT_LEFT)    \
  F1(scroll_amt_right,   DS_SCROLL_AMT_RIGHT)   \
  F1(scroll_amt_down,    DS_SCROLL_AMT_DOWN)    \
  F1(scroll_amt_up,      DS_SCROLL_AMT_UP)      \
  F1(spawn_tbl_lo,       DS_SPAWN_TBL_LO)       \
  F1(spawn_tbl_hi,       DS_SPAWN_TBL_HI)

// HUD / vikings / quit prompt
#define V2_GS_FIELDS_HUD(F1, FN) \
  FN(hud_items,          DS_HUD_ITEMS, 12)      \
  FN(hud_items_prev,     DS_HUD_ITEMS_PREV, 12) \
  FN(hud_sel,            DS_HUD_SEL, 3)         \
  FN(hud_sel_prev,       DS_HUD_SEL_PREV, 3)    \
  F1(hud_draw_di,        DS_HUD_DRAW_DI)        \
  FN(portrait_prev,      DS_PORTRAIT_PREV, 3)   \
  FN(portrait_snd,       DS_PORTRAIT_SND, 3)    \
  FN(portrait_snd_prev,  DS_PORTRAIT_SND_PREV, 3) \
  FN(hud_health,         DS_HUD_HEALTH, 3)      \
  FN(hud_health_prev,    DS_HUD_HEALTH_PREV, 3) \
  F1(hud_blink_field,    DS_HUD_BLINK_FIELD)    \
  F1(quit_active,        DS_QUIT_ACTIVE)        \
  F1(quit_blink,         DS_QUIT_BLINK)         \
  F1(quit_mode,          DS_QUIT_MODE)          \
  F1(hud_field_449,      DS_HUD_FIELD_449)

// Level / scroll state / palette-anim / sound-track / spawn descriptors
#define V2_GS_FIELDS_LEVEL(F1, FN) \
  F1(saved_vp_x,         DS_SAVED_VP_X)         \
  F1(saved_vp_y,         DS_SAVED_VP_Y)         \
  F1(scroll_col,         DS_SCROLL_COL)         \
  F1(scroll_row,         DS_SCROLL_ROW)         \
  F1(scroll_limit_x,     DS_SCROLL_LIMIT_X)     \
  F1(scroll_limit_y,     DS_SCROLL_LIMIT_Y)     \
  F1(level_prev,         DS_LEVEL_PREV)         \
  F1(level,              DS_LEVEL)              \
  F1(music_track,        DS_MUSIC_TRACK)        \
  F1(anim_scroll_dx,     DS_ANIM_SCROLL_DX)     \
  F1(anim_scroll_dy,     DS_ANIM_SCROLL_DY)     \
  F1(spawn_x,            DS_SPAWN_X)            \
  F1(spawn_y,            DS_SPAWN_Y)            \
  F1(spawn_code,         DS_SPAWN_CODE)         \
  F1(spawn_anim,         DS_SPAWN_ANIM)         \
  F1(spawn_pool0,        DS_SPAWN_POOL0)        \
  F1(level_load,         DS_LEVEL_LOAD)         \
  F1(level_load_aux,     DS_LEVEL_LOAD_AUX)     \
  F1(map_bp,             DS_MAP_BP)             \
  F1(map_height,         DS_MAP_HEIGHT)         \
  F1(chunk_cur,          DS_CHUNK_CUR)          \
  F1(chunk_tile,         DS_CHUNK_TILE)         \
  F1(chunk_bg,           DS_CHUNK_BG)           \
  F1(cmd_write,          DS_CMD_WRITE)          \
  F1(obj_queue_head,     DS_OBJ_QUEUE_HEAD)     \
  F1(cmd_read,           DS_CMD_READ)           \
  F1(decomp_size,        DS_DECOMP_SIZE)

// Segment registry + loader cursors + sound/config words
#define V2_GS_FIELDS_SEG(F1, FN) \
  F1(seg_tiledata,       DS_SEG_TILEDATA)       \
  F1(seg_tilegfx,        DS_SEG_TILEGFX)        \
  F1(seg_gs,             DS_SEG_GS)             \
  F1(seg_tilemap,        DS_SEG_TILEMAP)        \
  F1(decomp_di_end,      DS_DECOMP_DI_END)      \
  F1(seg_anim,           DS_SEG_ANIM)           \
  F1(seg_fs,             DS_SEG_FS)             \
  F1(seg_sound,          DS_SEG_SOUND)          \
  F1(seg_sound2,         DS_SEG_SOUND2)         \
  F1(seg_sound3,         DS_SEG_SOUND3)         \
  F1(template_chunk,     DS_TEMPLATE_CHUNK)     \
  F1(seg_sprite,         DS_SEG_SPRITE)         \
  F1(anim_init_off,      DS_ANIM_INIT_OFF)      \
  F1(seg_chunk,          DS_SEG_CHUNK)          \
  F1(anim_ptr_lo,        DS_ANIM_PTR_LO)        \
  F1(anim_ptr_hi,        DS_ANIM_PTR_HI)        \
  FN(anim_chunk_ids,     DS_ANIM_CHUNK_IDS, 16) \
  FN(anim_chunk_off,     DS_ANIM_CHUNK_OFF, 16) \
  FN(anim_chunk_seg,     DS_ANIM_CHUNK_SEG, 16) \
  FN(sprite_res_id,      DS_SPRITE_RES_ID, 32)  \
  FN(sprite_res_base,    DS_SPRITE_RES_BASE, 32)

// Page emulator / palette pipeline words / VGA-page state
#define V2_GS_FIELDS_PAGES(F1, FN) \
  F1(pal_req,            DS_PAL_REQ)            \
  F1(pal_src_ptr,        DS_PAL_SRC_PTR)        \
  F1(page_draw,          DS_PAGE_DRAW)          \
  F1(page_shown,         DS_PAGE_SHOWN)         \
  F1(page_bg,            DS_PAGE_BG)            \
  F1(page_rowcur_2,      DS_PAGE_ROWCUR_2)      \
  F1(page_rowcur_3,      DS_PAGE_ROWCUR_3)      \
  F1(page_rowcur_1,      DS_PAGE_ROWCUR_1)      \
  F1(page_vga_2,         DS_PAGE_VGA_2)         \
  F1(page_vga_3,         DS_PAGE_VGA_3)         \
  F1(page_vga_1,         DS_PAGE_VGA_1)         \
  F1(page_copy_src1,     DS_PAGE_COPY_SRC1)     \
  F1(page_copy_dst1,     DS_PAGE_COPY_DST1)     \
  F1(page_copy_src2,     DS_PAGE_COPY_SRC2)     \
  F1(page_copy_dst2,     DS_PAGE_COPY_DST2)     \
  F1(page_copy_src3,     DS_PAGE_COPY_SRC3)     \
  F1(page_scratch_346,   DS_PAGE_SCRATCH_346)   \
  FN(lut_page_row,       LUT_PAGE_ROW, 78)      \
  FN(lut_subrow,         LUT_SUBROW, 8)         \
  F1(fs_page_stride,     DS_FS_PAGE_STRIDE)     \
  F1(text_fullscreen,    DS_TEXT_FULLSCREEN)    \
  FN(glyph_buf,          DS_GLYPH_BUF, 0x1B8)   \
  F1(ui_throttle,        DS_UI_THROTTLE)        \
  F1(clip_limit_x,       DS_CLIP_LIMIT_X)       \
  F1(clip_limit_y,       DS_CLIP_LIMIT_Y)

// Sound / AIL / startup config words
#define V2_GS_FIELDS_SOUND(F1, FN) \
  F1(startup_cx,         DS_STARTUP_CX)         \
  F1(int24_vector,       DS_INT24_VECTOR)       \
  F1(int24_vector_hi,    DS_INT24_VECTOR_HI)    \
  F1(music_mute_src,     DS_MUSIC_MUTE_SRC)     \
  F1(sfx_mute_src,       DS_SFX_MUTE_SRC)       \
  F1(sound_card,         DS_SOUND_CARD)         \
  F1(music_card,         DS_MUSIC_CARD)         \
  F1(datadat_magic,      DS_DATADAT_MAGIC)      \
  F1(bios_checksum,      DS_BIOS_CHECKSUM)      \
  F1(joystick_present,   DS_JOYSTICK_PRESENT)   \
  F1(input_joy,          DS_INPUT_JOY)          \
  F1(input_accum,        DS_INPUT_ACCUM)        \
  F1(counter_8734,       DS_COUNTER_8734)       \
  FN(cmd_handler_tbl,    DS_CMD_HANDLER_TBL, 27) \
  FN(vm_subdispatch_tbl, DS_VM_DISPATCH_TBL, 10) \
  FN(vm_setter_tbl,      DS_VM_SETTER_TBL, 5)   \
  FN(vm_optable,         DS_VM_OPTABLE, 216)    \
  F1(sound_field_8ea,    DS_SOUND_FIELD_8EA)    \
  F1(music_id,           DS_MUSIC_ID)           \
  F1(sound_init_92a,     DS_SOUND_INIT_92A)     \
  F1(seg_sound_base,     DS_SEG_SOUND_BASE)     \
  F1(sound_init_932,     DS_SOUND_INIT_932)     \
  F1(xmi_buf_ptr,        DS_XMI_BUF_PTR)        \
  F1(sound_field_942,    DS_SOUND_FIELD_942)    \
  F1(ail_music_state,    DS_AIL_MUSIC_STATE)    \
  F1(ail_init_done,      DS_AIL_INIT_DONE)      \
  F1(vsync_count,        DS_VSYNC_COUNT)        \
  F1(vsync_calib,        DS_VSYNC_CALIB)        \
  F1(pit_latch,          DS_PIT_LATCH)

// Spec-key init bit-mask words (INT9 cluster)
#define V2_GS_FIELDS_SPEC(F1, FN) \
  F1(spec_mask_1ee, DS_SPEC_MASK_1EE) F1(spec_mask_20a, DS_SPEC_MASK_20A) \
  F1(spec_mask_210, DS_SPEC_MASK_210) F1(spec_mask_21e, DS_SPEC_MASK_21E) \
  F1(spec_mask_224, DS_SPEC_MASK_224) F1(spec_mask_226, DS_SPEC_MASK_226) \
  F1(spec_mask_22a, DS_SPEC_MASK_22A) F1(spec_mask_22c, DS_SPEC_MASK_22C) \
  F1(spec_mask_22e, DS_SPEC_MASK_22E) F1(spec_mask_25e, DS_SPEC_MASK_25E) \
  F1(spec_mask_260, DS_SPEC_MASK_260) F1(spec_mask_27a, DS_SPEC_MASK_27A) \
  F1(spec_mask_27c, DS_SPEC_MASK_27C) F1(spec_mask_27e, DS_SPEC_MASK_27E) \
  F1(spec_mask_282, DS_SPEC_MASK_282) F1(spec_mask_284, DS_SPEC_MASK_284) \
  F1(spec_mask_286, DS_SPEC_MASK_286) F1(spec_mask_288, DS_SPEC_MASK_288) \
  F1(spec_mask_28c, DS_SPEC_MASK_28C) F1(spec_mask_290, DS_SPEC_MASK_290)

// Sprite table columns — 128 contiguous words each (slot addressing obj=slot*2)
#define V2_GS_FIELDS_SPRITE(F1, FN) \
  FN(spr_flags,     OBJ_SPRITE_FLAGS, 128) \
  FN(spr_class,     OBJ_SUB_CLASS,    128) \
  FN(spr_x,         OBJ_SPRITE_X,     128) \
  FN(spr_y,         OBJ_SPRITE_Y,     128) \
  FN(spr_off,       OBJ_SPRITE_OFF,   128) \
  FN(spr_seg,       OBJ_SPRITE_SEG,   128) \
  FN(spr_src_base,  OBJ_SUB_SRC_BASE, 128) \
  FN(spr_src_seg,   OBJ_SUB_SRC_SEG,  128) \
  FN(spr_strips,    OBJ_STRIP_COUNT,  128) \
  FN(spr_cur_x,     OBJ_SPRITE_CUR_X, 128) \
  FN(spr_cur_y,     OBJ_SPRITE_CUR_Y, 128) \
  FN(spr_old_x,     OBJ_SPRITE_OLD_X, 128) \
  FN(spr_old_y,     OBJ_SPRITE_OLD_Y, 128) \
  FN(spr_dirty,     OBJ_DIRTY_MODE,   128)

// VM object table columns — 20 contiguous words each (stride 0x28 between
// column bases; the region is a dense run of 20-word columns)
#define V2_GS_FIELDS_OBJ(F1, FN) \
  FN(obj_pc,          OBJ_PC,          20) \
  FN(obj_code_seg,    OBJ_CODE_SEG,    20) \
  FN(obj_alt_pc,      OBJ_ALT_PC,      20) \
  FN(obj_x_prev,      OBJ_X_PREV,      20) \
  FN(obj_y_prev,      OBJ_Y_PREV,      20) \
  FN(obj_coll_bits,   OBJ_COLL_BITS,   20) \
  FN(obj_anim_table,  OBJ_ANIM_TABLE,  20) \
  FN(obj_width,       OBJ_WIDTH,       20) \
  FN(obj_height,      OBJ_HEIGHT,      20) \
  FN(obj_half_h,      OBJ_HALF_H,      20) \
  FN(obj_half_w,      OBJ_HALF_W,      20) \
  FN(obj_bbox_y0,     OBJ_BBOX_Y0,     20) \
  FN(obj_bbox_y1,     OBJ_BBOX_Y1,     20) \
  FN(obj_bbox_x0,     OBJ_BBOX_X0,     20) \
  FN(obj_bbox_x1,     OBJ_BBOX_X1,     20) \
  FN(obj_flags,       OBJ_FLAGS,       20) \
  FN(obj_res_handle,  OBJ_RES_HANDLE,  20) \
  FN(obj_res_cost,    OBJ_RES_COST,    20) \
  FN(obj_state_idx,   OBJ_STATE_IDX,   20) \
  FN(obj_class_bits,  OBJ_CLASS_BITS,  20) \
  FN(obj_anim_dx,     OBJ_ANIM_DX,     20) \
  FN(obj_anim_dy,     OBJ_ANIM_DY,     20) \
  FN(obj_spawn_pool,  OBJ_SPAWN_POOL,  20) \
  FN(obj_anim_sub,    OBJ_ANIM_SUB,    20) \
  FN(obj_anim_idx,    OBJ_ANIM_IDX,    20) \
  FN(obj_timer,       OBJ_TIMER,       20) \
  FN(obj_world_x,     OBJ_WORLD_X,     20) \
  FN(obj_world_y,     OBJ_WORLD_Y,     20) \
  FN(obj_vel_x_max,   OBJ_VEL_X_MAX,   20) \
  FN(obj_vel_y_max,   OBJ_VEL_Y_MAX,   20) \
  FN(obj_type_id,     OBJ_TYPE_ID,     20) \
  FN(obj_parent,      OBJ_PARENT,      20) \
  FN(obj_child,       OBJ_CHILD,       20) \
  FN(obj_sprite_base, OBJ_SPRITE_BASE, 20) \
  FN(obj_state_187d,  OBJ_STATE_187D,  20) \
  FN(obj_state_18a5,  OBJ_STATE_18A5,  20) \
  FN(obj_state_18cd,  OBJ_STATE_18CD,  20) \
  FN(obj_state_18f5,  OBJ_STATE_18F5,  20) \
  FN(obj_cur_sprite,  OBJ_CUR_SPRITE_IDX, 20) \
  FN(obj_vel_x,       OBJ_VEL_X,       20) \
  FN(obj_vel_y,       OBJ_VEL_Y,       20) \
  FN(obj_partner,     OBJ_PARTNER,     20) \
  FN(obj_frac_x,      OBJ_FRAC_X,      20) \
  FN(obj_frac_y,      OBJ_FRAC_Y,      20) \
  FN(obj_anim_pc,     OBJ_ANIM_PC,     20) \
  FN(obj_anim_timer,  OBJ_ANIM_TIMER,  20) \
  FN(obj_anim_cont,   OBJ_ANIM_CONT,   20) \
  FN(obj_sub_slot,    OBJ_SUB_SLOT,    20) \
  FN(obj_sub_end,     OBJ_SUB_END,     20) \
  FN(obj_sub_count,   OBJ_SUB_COUNT,   20) \
  FN(obj_sub_anim_ptr,OBJ_SUB_ANIM_PTR,20)

#define V2_GS_FIELDS_W(F1, FN) \
  V2_GS_FIELDS_CORE(F1, FN)  \
  V2_GS_FIELDS_HUD(F1, FN)   \
  V2_GS_FIELDS_LEVEL(F1, FN) \
  V2_GS_FIELDS_SEG(F1, FN)   \
  V2_GS_FIELDS_PAGES(F1, FN) \
  V2_GS_FIELDS_SOUND(F1, FN) \
  V2_GS_FIELDS_SPEC(F1, FN)  \
  V2_GS_FIELDS_SPRITE(F1, FN)\
  V2_GS_FIELDS_OBJ(F1, FN)

// ---------------------------------------------------------------------------
// Byte fields: B1(member, ds_off) scalar byte; BN(member, ds_off, count).
// ---------------------------------------------------------------------------
#define V2_GS_FIELDS_B(B1, BN) \
  B1(scratch_28,       DS_SCRATCH_28)          \
  B1(cmd_active,       DS_CMD_ACTIVE)          \
  B1(scratch_32e,      DS_SCRATCH_32E)         \
  BN(pal_shade,        DS_PAL_SHADE_R, 6)      \
  B1(level_flags,      DS_LEVEL_FLAGS)         \
  B1(pal_anim_en,      DS_PAL_ANIM_EN)         \
  BN(pal_anim_reload,  DS_PAL_ANIM_RELOAD, 8)  \
  BN(pal_anim_timer,   DS_PAL_ANIM_TIMER, 8)   \
  BN(pal_anim_start,   DS_PAL_ANIM_START, 8)   \
  BN(pal_anim_end,     DS_PAL_ANIM_END, 8)     \
  B1(snd_type,         DS_SND_TYPE)            \
  B1(snd_track,        DS_SND_TRACK)           \
  B1(snd_flag,         DS_SND_FLAG)            \
  B1(active_vk_sel,    DS_ACTIVE_VK_SEL)       \
  B1(hud_force,        DS_HUD_FORCE)           \
  B1(pal_flags,        DS_PAL_FLAGS)           \
  B1(dac_r_save,       DS_DAC_R_SAVE)          \
  B1(dac_b_save,       DS_DAC_B_SAVE)          \
  BN(pal_src,          DS_PAL_SRC, 768)        \
  BN(pal_out,          DS_PAL_OUT, 768)        \
  BN(byte_save,        DS_BYTE_SAVE_0, 3)      \
  B1(pixel_pan,        DS_PIXEL_PAN)           \
  B1(pan_gate,         DS_PAN_GATE)            \
  B1(vga_page_flag,    DS_VGA_PAGE_FLAG)       \
  B1(vga_mode_byte,    DS_VGA_MODE_BYTE)       \
  BN(page_split,       DS_PAGE_SPLIT_1A, 4)    \
  B1(key_q,            DS_KEY_Q)               \
  B1(spec_key_y,       DS_SPEC_KEY_Y)          \
  B1(spec_key_s,       DS_SPEC_KEY_S)          \
  B1(key_x,            DS_KEY_X)               \
  B1(spec_key_n,       DS_SPEC_KEY_N)          \
  B1(spec_key_m,       DS_SPEC_KEY_M)          \
  B1(key_alt,          DS_KEY_ALT)             \
  B1(spec_key_f5,      DS_SPEC_KEY_F5)         \
  B1(spec_key_f6,      DS_SPEC_KEY_F6)         \
  B1(key_f10,          DS_KEY_F10)             \
  B1(sprite_force,     DS_SPRITE_FORCE)        \
  B1(glyph_dirty,      DS_GLYPH_DIRTY)         \
  BN(hud_sel_gfx,      DS_HUD_SEL_GFX, 256)    \
  BN(hud_item_gfx,     DS_HUD_ITEM_GFX, 4864)  \
  BN(chunk_hdr,        DS_CHUNK_HDR, 8)

// ---------------------------------------------------------------------------
// The typed state. Serializer contract: v2_gs_deserialize fills every field
// from a 64K DS image (and snapshots the raw backing); v2_gs_serialize emits
// a 64K image where covered bytes come ONLY from fields and uncovered bytes
// come from the backing. v2_gs_roundtrip_check does both + memcmp.
// ---------------------------------------------------------------------------
struct V2GameState {
#define V2_GS_M1(name, off)      uint16_t name;
#define V2_GS_MN(name, off, n)   uint16_t name[n];
  V2_GS_FIELDS_W(V2_GS_M1, V2_GS_MN)
#undef V2_GS_M1
#undef V2_GS_MN
#define V2_GS_MB1(name, off)     uint8_t name;
#define V2_GS_MBN(name, off, n)  uint8_t name[n];
  V2_GS_FIELDS_B(V2_GS_MB1, V2_GS_MBN)
#undef V2_GS_MB1
#undef V2_GS_MBN
  // uncovered DS bytes, verbatim (shrinks as fields get carved out)
  uint8_t raw[0x10000];
};

extern "C" {
void v2_gs_deserialize(V2GameState* gs, const uint8_t* ds);
void v2_gs_serialize(const V2GameState* gs, uint8_t* ds_out);
// Returns diff count (0 = byte-identical roundtrip); logs first diffs.
int  v2_gs_roundtrip_check(const uint8_t* ds, const char* tag);
}

// ---------------------------------------------------------------------------
// V2StateView — typed accessors over the LIVE flat DS image. Same field
// names as V2GameState (generated from the same X-macro lists), but the
// carrier stays the byte image: view.viewport_x() reads/writes the exact DS
// bytes every other subsystem sees, so it can be adopted call-site by
// call-site with zero behavior change. Once all v2 code goes through the
// view, swapping the carrier to V2GameState (+ serializer at the verify
// barriers) is a single-point change — that is the phase-D endgame.
//
// Word access uses the project-wide unaligned *(uint16_t*) idiom (same as
// every existing v2 site); array fields return a pointer to the first
// element's DS bytes (words: unaligned-safe on x86 like the rest of v2).
// ---------------------------------------------------------------------------
// Alias accessors — phase-A layout names that overlap covering fields
// (documented in the phase-D memory). They are VIEW-only channels into the
// same bytes; the serializer/coverage lists do not include them.
#define V2_GS_ALIASES(A1) \
  A1(scroll_disp_x,      DS_SCROLL_DISP_X)      \
  A1(scroll_disp_y,      DS_SCROLL_DISP_Y)      \
  A1(scroll_disp_x2,     DS_SCROLL_DISP_X2)     \
  A1(scroll_disp_y2,     DS_SCROLL_DISP_Y2)     \
  A1(portrait_snd_2,     DS_PORTRAIT_SND_2)     \
  A1(portrait_snd_3,     DS_PORTRAIT_SND_3)     \
  A1(vk_state_1,         DS_VK_STATE_1)         \
  A1(vk_state_2,         DS_VK_STATE_2)         \
  A1(vk_state_3,         DS_VK_STATE_3)         \
  A1(vk_state_4,         DS_VK_STATE_4)         \
  A1(vk_portrait_snd_2,  DS_VK_PORTRAIT_SND_2)  \
  A1(vk_portrait_snd_3,  DS_VK_PORTRAIT_SND_3)  \
  A1(hud_scratch_42b,    DS_HUD_SCRATCH_42B)    \
  A1(hud_scratch_42d,    DS_HUD_SCRATCH_42D)    \
  A1(hud_sel_2,          DS_HUD_SEL_2)          \
  A1(hud_sel_3,          DS_HUD_SEL_3)          \
  A1(hud_track_1,        DS_HUD_TRACK_1)        \
  A1(hud_track_2,        DS_HUD_TRACK_2)        \
  A1(vk_health_2,        DS_VK_HEALTH_2)        \
  A1(vk_health_3,        DS_VK_HEALTH_3)        \
  A1(render_117d,        DS_RENDER_117D)        \
  A1(render_117f,        DS_RENDER_117F)        \
  A1(snd_track_w,        DS_SND_TRACK)          \
  A1(dac_r_save_w,       DS_DAC_R_SAVE)

struct V2StateView {
    uint8_t* ds;
    explicit V2StateView(uint8_t* ds_) : ds(ds_) {}

#define V2_GS_A1(name, off) \
    uint16_t name() const              { return *(const uint16_t*)(ds + (off)); } \
    void     name(uint16_t v)          { *(uint16_t*)(ds + (off)) = v; } \
    uint16_t& name##_ref()             { return *(uint16_t*)(ds + (off)); }
#define V2_GS_AN(name, off, n) \
    uint16_t name(uint32_t i) const    { return *(const uint16_t*)(ds + (off) + 2u * i); } \
    void     name(uint32_t i, uint16_t v) { *(uint16_t*)(ds + (off) + 2u * i) = v; }
    V2_GS_FIELDS_W(V2_GS_A1, V2_GS_AN)
#undef V2_GS_A1
#undef V2_GS_AN
#define V2_GS_AB1(name, off) \
    uint8_t  name##_b() const          { return ds[(off)]; } \
    void     name##_b(uint8_t v)       { ds[(off)] = v; }
#define V2_GS_ABN(name, off, n) \
    uint8_t* name##_bytes()            { return ds + (off); } \
    const uint8_t* name##_bytes() const { return ds + (off); }
    V2_GS_FIELDS_B(V2_GS_AB1, V2_GS_ABN)
#undef V2_GS_AB1
#undef V2_GS_ABN
#define V2_GS_A1(name, off) \
    uint16_t name() const              { return *(const uint16_t*)(ds + (off)); } \
    void     name(uint16_t v)          { *(uint16_t*)(ds + (off)) = v; } \
    uint16_t& name##_ref()             { return *(uint16_t*)(ds + (off)); }
    V2_GS_ALIASES(V2_GS_A1)
#undef V2_GS_A1
};

// Read-only view over a const DS image (same names, getters only) — for
// renderer-side code that receives `const uint8_t*`.
struct V2StateViewC {
    const uint8_t* ds;
    explicit V2StateViewC(const uint8_t* ds_) : ds(ds_) {}
#define V2_GS_A1(name, off) \
    uint16_t name() const              { return *(const uint16_t*)(ds + (off)); }
#define V2_GS_AN(name, off, n) \
    uint16_t name(uint32_t i) const    { return *(const uint16_t*)(ds + (off) + 2u * i); }
    V2_GS_FIELDS_W(V2_GS_A1, V2_GS_AN)
#undef V2_GS_A1
#undef V2_GS_AN
#define V2_GS_AB1(name, off) \
    uint8_t  name##_b() const          { return ds[(off)]; }
#define V2_GS_ABN(name, off, n) \
    const uint8_t* name##_bytes() const { return ds + (off); }
    V2_GS_FIELDS_B(V2_GS_AB1, V2_GS_ABN)
#undef V2_GS_AB1
#undef V2_GS_ABN
#define V2_GS_A1(name, off) \
    uint16_t name() const              { return *(const uint16_t*)(ds + (off)); }
    V2_GS_ALIASES(V2_GS_A1)
#undef V2_GS_A1
};

// Inline factories: typed access at any call site without a local view.
static inline V2StateView  v2gs(uint8_t* ds)        { return V2StateView(ds); }
static inline V2StateViewC v2gs(const uint8_t* ds)  { return V2StateViewC(ds); }
