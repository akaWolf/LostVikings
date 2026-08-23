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

// Stage 4 II.c: EVACUATED fields — their CARRIER is the typed member in
// g_gs_evac (for the canonical shadow world); the flat image is kept
// write-through so serialization/verify/dumps see identical bytes, and a
// per-frame check (v2_gs_evac_check) FATALs if anything writes the image
// bytes behind the accessors' back. Moving a field here = evacuating it.
// The list participates in the struct/serializer/coverage exactly like
// CORE (it is appended in V2_GS_FIELDS_W below).
// Byte-typed evacuated fields (uint8_t members). Same machinery as the
// word list: write-through, mirrors, span refresh, per-frame check.
#define V2_GS_FIELDS_EVACB(B1, BN) \
  V2_GS_FIELDS_EVACB_B(B1, BN) \
  V2_GS_FIELDS_GAPFILL(B1, BN)

// Stage 4 II.c BRIDGE stage for freshly evacuated fields: writes go
// member+image (write-through via setters/mirrors), READS STAY FLAT until
// the frame check is clean on the full corpus — only then the group moves
// into V2_GS_FIELDS_EVAC (read flip). Lesson of wave 7: flipping reads in
// the same commit as the evacuation turns every missed write channel into
// a behavior change instead of a check report.
#define V2_GS_FIELDS_EVAC_BRIDGE(F1, FN, FR)

#define V2_GS_FIELDS_EVAC(F1, FN, FR) \
  FR(obj_frac_x,      OBJ_FRAC_X,      20, obj_rec, frac_x) \
  FR(obj_frac_y,      OBJ_FRAC_Y,      20, obj_rec, frac_y) \
  FR(obj_anim_pc,     OBJ_ANIM_PC,     20, obj_rec, anim_pc) \
  FR(obj_anim_timer,  OBJ_ANIM_TIMER,  20, obj_rec, anim_timer) \
  FR(obj_anim_cont,   OBJ_ANIM_CONT,   20, obj_rec, anim_cont) \
  FR(obj_sub_slot,    OBJ_SUB_SLOT,    20, obj_rec, sub_slot) \
  FR(obj_sub_end,     OBJ_SUB_END,     20, obj_rec, sub_end) \
  FR(obj_sub_count,   OBJ_SUB_COUNT,   20, obj_rec, sub_count) \
  FR(obj_sub_anim_ptr,OBJ_SUB_ANIM_PTR,20, obj_rec, sub_anim_ptr) \
  FN(anim_chunk_ids,     DS_ANIM_CHUNK_IDS, 16) \
  FN(anim_chunk_off,     DS_ANIM_CHUNK_OFF, 16) \
  FN(anim_chunk_seg,     DS_ANIM_CHUNK_SEG, 16) \
  FN(sprite_res_id,      DS_SPRITE_RES_ID, 32) \
  FN(sprite_res_base,    DS_SPRITE_RES_BASE, 32) \
  FN(seq_handle_slots,   DS_MUSIC_ID, 5)        \
  FN(seq_seq_slots,      0x9916, 5) \
  F1(accumulator,        DS_ACCUMULATOR) \
  F1(music_mute,         DS_MUSIC_MUTE) \
  F1(sfx_mute,           DS_SFX_MUTE) \
  F1(frame_flags,        DS_FRAME_FLAGS) \
  F1(rng_seed_lo,        DS_RNG_SEED) \
  FN(pal_chunk_addr_tbl, 0x854A, 11) \
  F1(obj_count,          DS_OBJ_COUNT) \
  F1(input_keys,         DS_INPUT_KEYS) \
  F1(input_edges,        DS_INPUT_EDGES) \
  F1(input_prev,         DS_INPUT_PREV) \
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
  F1(sound_init_92a,     DS_SOUND_INIT_92A)     \
  F1(seg_sound_base,     DS_SEG_SOUND_BASE)     \
  F1(sound_init_932,     DS_SOUND_INIT_932)     \
  F1(xmi_buf_ptr,        DS_XMI_BUF_PTR)        \
  F1(ail_timbre_toff,    0x992E)                \
  F1(ail_timbre_tseg,    0x9930)                \
  F1(ail_req_bank,       0x993E)                \
  F1(ail_req_patch,      0x9940)                \
  F1(ail_req_raw,        0x9946)                \
  FN(ail_state_ptrs,     0x9920, 5)             \
  F1(sound_field_942,    DS_SOUND_FIELD_942)    \
  F1(ail_music_state,    DS_AIL_MUSIC_STATE)    \
  F1(ail_init_done,      DS_AIL_INIT_DONE)      \
  FN(sound_dispatch_tbl, DS_SOUND_DISPATCH_TBL, 5) \
  FN(music_track_chunk_tbl, DS_SND_DESC_OFF_TBL, 11) \
  F1(vsync_count,        DS_VSYNC_COUNT)        \
  F1(vsync_calib,        DS_VSYNC_CALIB)        \
  F1(pit_latch,          DS_PIT_LATCH) \
  FN(script_vars,        0x0204, 68) \
  F1(script_var_28e,     0x028E) \
  F1(scratch_34,         DS_SCRATCH_34) \
  F1(scratch_36,         DS_SCRATCH_36) \
  F1(scratch_38,         DS_SCRATCH_38) \
  F1(scratch_3a,         DS_SCRATCH_3A) \
  F1(mode_word,          DS_MODE_WORD) \
  F1(text_col,           DS_TEXT_COL) \
  F1(text_row,           DS_TEXT_ROW) \
  F1(text_idx,           DS_TEXT_IDX) \
  F1(aim_sign_x,         DS_AIM_SIGN_X) \
  F1(aim_sign_y,         DS_AIM_SIGN_Y) \
  F1(search_filter,      DS_SEARCH_FILTER) \
  F1(search_jump,        DS_SEARCH_JUMP) \
  F1(search_y,           DS_SEARCH_Y) \
  F1(search_si,          DS_SEARCH_SI) \
  F1(search_res_type,    DS_SEARCH_RES_TYPE) \
  F1(search_res_slot,    DS_SEARCH_RES_SLOT) \
  F1(search_best,        DS_SEARCH_BEST) \
  F1(coll_bit_idx,       DS_COLL_BIT_IDX) \
  F1(coll_phase,         DS_COLL_PHASE) \
  F1(coll_state_392,     DS_COLL_STATE_392) \
  F1(coll_state_398,     DS_COLL_STATE_398) \
  F1(prio_count,         DS_PRIO_COUNT) \
  F1(anim_timer,         DS_ANIM_TIMER) \
  F1(anim_cont,          DS_ANIM_CONT) \
  F1(anim_slot,          DS_ANIM_SLOT) \
  F1(anim_slot_end,      DS_ANIM_SLOT_END) \
  F1(anim_sub_mask,      DS_ANIM_SUB_MASK) \
  F1(spawn_pool_sel,     DS_SPAWN_POOL_SEL) \
  F1(spawn_tbl_lo,       DS_SPAWN_TBL_LO) \
  F1(cur_obj,            DS_CUR_OBJ) \
  F1(viewport_x,         DS_VIEWPORT_X) \
  F1(viewport_y,         DS_VIEWPORT_Y) \
  F1(flag_202,           DS_FLAG_202) \
  F1(word_200,           0x0200) \
  F1(last_char,          DS_LAST_CHAR) \
  FN(pw_chars,           DS_PW_CHAR0, 4) \
  F1(transition,         DS_TRANSITION) \
  F1(scratch_331,        DS_SCRATCH_331) \
  F1(scratch_336,        DS_SCRATCH_336) \
  F1(obj_scan_start,     DS_OBJ_SCAN_START) \
  F1(hud_disp_mode,      DS_HUD_DISP_MODE) \
  F1(scratch_348,        DS_SCRATCH_348) \
  F1(scratch_34a,        DS_SCRATCH_34A) \
  F1(scratch_34c,        DS_SCRATCH_34C) \
  F1(scroll_delta_x,     DS_SCROLL_DELTA_X) \
  F1(scroll_delta_y,     DS_SCROLL_DELTA_Y) \
  F1(rng_timer,          DS_RNG_TIMER) \
  F1(scroll_lock_x,      DS_SCROLL_LOCK_X) \
  F1(scroll_lock_y,      DS_SCROLL_LOCK_Y) \
  F1(shake_src_x,        DS_SHAKE_SRC_X) \
  F1(shake_src_y,        DS_SHAKE_SRC_Y) \
  F1(shake_x,            DS_SHAKE_X) \
  F1(shake_y,            DS_SHAKE_Y) \
  F1(shake_gate_x,       DS_SHAKE_GATE_X) \
  F1(shake_gate_y,       DS_SHAKE_GATE_Y) \
  F1(pan_x,              DS_PAN_X) \
  F1(pan_y,              DS_PAN_Y) \
  F1(active_viking,      DS_ACTIVE_VIKING) \
  F1(prev_viking,        DS_PREV_VIKING) \
  F1(blink_counter,      DS_BLINK_COUNTER) \
  F1(game_mode_ac,       DS_GAME_MODE_AC) \
  F1(scratch_3ce,        DS_SCRATCH_3CE) \
  F1(scratch_3d0,        DS_SCRATCH_3D0) \
  F1(hud_sel_si,         DS_HUD_SEL_SI) \
  F1(scratch_3d6,        DS_SCRATCH_3D6) \
  F1(scroll_amt_left,    DS_SCROLL_AMT_LEFT) \
  F1(scroll_amt_right,   DS_SCROLL_AMT_RIGHT) \
  F1(scroll_amt_down,    DS_SCROLL_AMT_DOWN) \
  F1(scroll_amt_up,      DS_SCROLL_AMT_UP) \
  F1(spawn_tbl_hi,       DS_SPAWN_TBL_HI) \
  FN(hud_items,          DS_HUD_ITEMS, 12) \
  FN(hud_items_prev,     DS_HUD_ITEMS_PREV, 12) \
  FN(hud_sel,            DS_HUD_SEL, 3) \
  FN(hud_sel_prev,       DS_HUD_SEL_PREV, 3) \
  F1(hud_draw_di,        DS_HUD_DRAW_DI) \
  FN(portrait_prev,      DS_PORTRAIT_PREV, 3) \
  FN(portrait_snd,       DS_PORTRAIT_SND, 3) \
  FN(portrait_snd_prev,  DS_PORTRAIT_SND_PREV, 3) \
  FN(hud_health,         DS_HUD_HEALTH, 3) \
  FN(hud_health_prev,    DS_HUD_HEALTH_PREV, 3) \
  F1(hud_blink_field,    DS_HUD_BLINK_FIELD) \
  F1(quit_active,        DS_QUIT_ACTIVE) \
  F1(quit_blink,         DS_QUIT_BLINK) \
  F1(quit_mode,          DS_QUIT_MODE) \
  F1(hud_field_449,      DS_HUD_FIELD_449) \
  F1(saved_vp_x,         DS_SAVED_VP_X) \
  F1(saved_vp_y,         DS_SAVED_VP_Y) \
  F1(scroll_col,         DS_SCROLL_COL) \
  F1(scroll_row,         DS_SCROLL_ROW) \
  F1(scroll_limit_x,     DS_SCROLL_LIMIT_X) \
  F1(scroll_limit_y,     DS_SCROLL_LIMIT_Y) \
  F1(level_prev,         DS_LEVEL_PREV) \
  F1(level,              DS_LEVEL) \
  F1(music_track,        DS_MUSIC_TRACK) \
  F1(seg_tiledata,       DS_SEG_TILEDATA) \
  F1(seg_tilegfx,        DS_SEG_TILEGFX) \
  F1(seg_gs,             DS_SEG_GS) \
  F1(seg_tilemap,        DS_SEG_TILEMAP) \
  F1(decomp_di_end,      DS_DECOMP_DI_END) \
  F1(seg_anim,           DS_SEG_ANIM) \
  F1(seg_fs,             DS_SEG_FS) \
  F1(seg_sound,          DS_SEG_SOUND) \
  F1(seg_sound2,         DS_SEG_SOUND2) \
  F1(seg_sound3,         DS_SEG_SOUND3) \
  F1(template_chunk,     DS_TEMPLATE_CHUNK) \
  F1(seg_sprite,         DS_SEG_SPRITE) \
  F1(anim_init_off,      DS_ANIM_INIT_OFF) \
  F1(seg_chunk,          DS_SEG_CHUNK) \
  F1(anim_ptr_lo,        DS_ANIM_PTR_LO) \
  F1(anim_ptr_hi,        DS_ANIM_PTR_HI) \
  F1(pal_req,            DS_PAL_REQ) \
  F1(pal_src_ptr,        DS_PAL_SRC_PTR) \
  F1(page_draw,          DS_PAGE_DRAW) \
  F1(page_shown,         DS_PAGE_SHOWN) \
  F1(page_bg,            DS_PAGE_BG) \
  F1(page_rowcur_2,      DS_PAGE_ROWCUR_2) \
  F1(page_rowcur_3,      DS_PAGE_ROWCUR_3) \
  F1(page_rowcur_1,      DS_PAGE_ROWCUR_1) \
  F1(page_vga_2,         DS_PAGE_VGA_2) \
  F1(page_vga_3,         DS_PAGE_VGA_3) \
  F1(page_vga_1,         DS_PAGE_VGA_1) \
  F1(page_copy_src1,     DS_PAGE_COPY_SRC1) \
  F1(page_copy_dst1,     DS_PAGE_COPY_DST1) \
  F1(page_copy_src2,     DS_PAGE_COPY_SRC2) \
  F1(page_copy_dst2,     DS_PAGE_COPY_DST2) \
  F1(page_copy_src3,     DS_PAGE_COPY_SRC3) \
  F1(page_scratch_346,   DS_PAGE_SCRATCH_346) \
  FN(page_row_lut,       LUT_PAGE_ROW, 560) \
  FN(lut_subrow,         LUT_SUBROW, 8) \
  FN(fs_row_off_tbl,     DS_FS_ROW_OFF_TBL, 256) \
  FN(kbd_ascii_lut,      LUT_KBD_ASCII, 128) \
  FN(audio_cmd_tbl,      DS_AUDIO_CMD_TBL, 7) \
  FN(field_off_lut,      0x9348, 33) \
  FN(row43_lut,          0x938A, 25) \
  FN(bit_mask_lut,       0x93CC, 16) \
  FN(bit_clear_lut,      0x93EC, 16) \
  FN(level_chunk_tbl,    0x940C, 48) \
  FN(level_template_tbl, 0x946C, 48) \
  F1(text_fullscreen,    DS_TEXT_FULLSCREEN) \
  FN(glyph_buf,          DS_GLYPH_BUF, 0x1B8) \
  F1(ui_throttle,        DS_UI_THROTTLE) \
  F1(clip_limit_x,       DS_CLIP_LIMIT_X) \
  F1(clip_limit_y,       DS_CLIP_LIMIT_Y) \
  F1(spec_mask_1ee, DS_SPEC_MASK_1EE) F1(spec_mask_20a, DS_SPEC_MASK_20A) \
  F1(spec_mask_210, DS_SPEC_MASK_210) F1(spec_mask_21e, DS_SPEC_MASK_21E) \
  F1(spec_mask_224, DS_SPEC_MASK_224) F1(spec_mask_226, DS_SPEC_MASK_226) \
  F1(spec_mask_22a, DS_SPEC_MASK_22A) F1(spec_mask_22c, DS_SPEC_MASK_22C) \
  F1(spec_mask_22e, DS_SPEC_MASK_22E) F1(spec_mask_25e, DS_SPEC_MASK_25E) \
  F1(spec_mask_260, DS_SPEC_MASK_260) F1(spec_mask_27a, DS_SPEC_MASK_27A) \
  F1(spec_mask_27c, DS_SPEC_MASK_27C) F1(spec_mask_27e, DS_SPEC_MASK_27E) \
  F1(spec_mask_282, DS_SPEC_MASK_282) F1(spec_mask_284, DS_SPEC_MASK_284) \
  F1(spec_mask_286, DS_SPEC_MASK_286) F1(spec_mask_288, DS_SPEC_MASK_288) \
  F1(spec_mask_28c, DS_SPEC_MASK_28C) F1(spec_mask_290, DS_SPEC_MASK_290) \
  F1(anim_scroll_dx,     DS_ANIM_SCROLL_DX) \
  F1(anim_scroll_dy,     DS_ANIM_SCROLL_DY) \
  F1(spawn_x,            DS_SPAWN_X) \
  F1(spawn_y,            DS_SPAWN_Y) \
  F1(spawn_code,         DS_SPAWN_CODE) \
  F1(spawn_anim,         DS_SPAWN_ANIM) \
  F1(level_load,         DS_LEVEL_LOAD) \
  F1(level_load_aux,     DS_LEVEL_LOAD_AUX) \
  F1(map_bp,             DS_MAP_BP) \
  F1(map_height,         DS_MAP_HEIGHT) \
  F1(level_var_25c5,     0x25C5) \
  F1(level_var_25c7,     0x25C7) \
  FR(spr_flags,     OBJ_SPRITE_FLAGS, 128, spr_rec, flags) \
  FR(spr_class,     OBJ_SUB_CLASS,    128, spr_rec, cls) \
  FR(spr_x,         OBJ_SPRITE_X,     128, spr_rec, x) \
  FR(spr_y,         OBJ_SPRITE_Y,     128, spr_rec, y) \
  FR(spr_off,       OBJ_SPRITE_OFF,   128, spr_rec, off) \
  FR(spr_seg,       OBJ_SPRITE_SEG,   128, spr_rec, seg) \
  FR(spr_src_base,  OBJ_SUB_SRC_BASE, 128, spr_rec, src_base) \
  FR(spr_src_seg,   OBJ_SUB_SRC_SEG,  128, spr_rec, src_seg) \
  FR(spr_strips,    OBJ_STRIP_COUNT,  128, spr_rec, strips) \
  FR(spr_cur_x,     OBJ_SPRITE_CUR_X, 128, spr_rec, cur_x) \
  FR(spr_cur_y,     OBJ_SPRITE_CUR_Y, 128, spr_rec, cur_y) \
  FR(spr_old_x,     OBJ_SPRITE_OLD_X, 128, spr_rec, old_x) \
  FR(spr_old_y,     OBJ_SPRITE_OLD_Y, 128, spr_rec, old_y) \
  FR(spr_dirty,     OBJ_DIRTY_MODE,   128, spr_rec, dirty) \
  FR(obj_pc,          OBJ_PC,          20, obj_rec, pc) \
  FR(obj_code_seg,    OBJ_CODE_SEG,    20, obj_rec, code_seg) \
  FR(obj_alt_pc,      OBJ_ALT_PC,      20, obj_rec, alt_pc) \
  FR(obj_x_prev,      OBJ_X_PREV,      20, obj_rec, x_prev) \
  FR(obj_y_prev,      OBJ_Y_PREV,      20, obj_rec, y_prev) \
  FR(obj_coll_bits,   OBJ_COLL_BITS,   20, obj_rec, coll_bits) \
  FR(obj_anim_table,  OBJ_ANIM_TABLE,  20, obj_rec, anim_table) \
  FR(obj_width,       OBJ_WIDTH,       20, obj_rec, width) \
  FR(obj_height,      OBJ_HEIGHT,      20, obj_rec, height) \
  FR(obj_half_h,      OBJ_HALF_H,      20, obj_rec, half_h) \
  FR(obj_half_w,      OBJ_HALF_W,      20, obj_rec, half_w) \
  FR(obj_bbox_y0,     OBJ_BBOX_Y0,     20, obj_rec, bbox_y0) \
  FR(obj_bbox_y1,     OBJ_BBOX_Y1,     20, obj_rec, bbox_y1) \
  FR(obj_bbox_x0,     OBJ_BBOX_X0,     20, obj_rec, bbox_x0) \
  FR(obj_bbox_x1,     OBJ_BBOX_X1,     20, obj_rec, bbox_x1) \
  FR(obj_flags,       OBJ_FLAGS,       20, obj_rec, flags) \
  FR(obj_res_handle,  OBJ_RES_HANDLE,  20, obj_rec, res_handle) \
  FR(obj_res_cost,    OBJ_RES_COST,    20, obj_rec, res_cost) \
  FR(obj_state_idx,   OBJ_STATE_IDX,   20, obj_rec, state_idx) \
  FR(obj_class_bits,  OBJ_CLASS_BITS,  20, obj_rec, class_bits) \
  FR(obj_anim_dx,     OBJ_ANIM_DX,     20, obj_rec, anim_dx) \
  FR(obj_anim_dy,     OBJ_ANIM_DY,     20, obj_rec, anim_dy) \
  FR(obj_spawn_pool,  OBJ_SPAWN_POOL,  20, obj_rec, spawn_pool) \
  FR(obj_anim_sub,    OBJ_ANIM_SUB,    20, obj_rec, anim_sub) \
  FR(obj_anim_idx,    OBJ_ANIM_IDX,    20, obj_rec, anim_idx) \
  FR(obj_timer,       OBJ_TIMER,       20, obj_rec, timer) \
  FR(obj_world_x,     OBJ_WORLD_X,     20, obj_rec, world_x) \
  FR(obj_world_y,     OBJ_WORLD_Y,     20, obj_rec, world_y) \
  FR(obj_vel_x_max,   OBJ_VEL_X_MAX,   20, obj_rec, vel_x_max) \
  FR(obj_vel_y_max,   OBJ_VEL_Y_MAX,   20, obj_rec, vel_y_max) \
  FR(obj_type_id,     OBJ_TYPE_ID,     20, obj_rec, type_id) \
  FR(obj_parent,      OBJ_PARENT,      20, obj_rec, parent) \
  FR(obj_child,       OBJ_CHILD,       20, obj_rec, child) \
  FR(obj_sprite_base, OBJ_SPRITE_BASE, 20, obj_rec, sprite_base) \
  FR(obj_state_187d,  OBJ_STATE_187D,  20, obj_rec, state_187d) \
  FR(obj_state_18a5,  OBJ_STATE_18A5,  20, obj_rec, state_18a5) \
  FR(obj_state_18cd,  OBJ_STATE_18CD,  20, obj_rec, state_18cd) \
  FR(obj_state_18f5,  OBJ_STATE_18F5,  20, obj_rec, state_18f5) \
  FR(obj_cur_sprite,  OBJ_CUR_SPRITE_IDX, 20, obj_rec, cur_sprite) \
  FR(obj_vel_x,       OBJ_VEL_X,       20, obj_rec, vel_x) \
  FR(obj_vel_y,       OBJ_VEL_Y,       20, obj_rec, vel_y) \
  FR(obj_partner,     OBJ_PARTNER,     20, obj_rec, partner) \
// bulk-copies on level switch — the evac check caught the desync on the
// level-3→4 transition. They evacuate only after that copy goes through
// the view (deserialize-based level load); until then they stay flat.

// Core globals + input + scroll + collision scratch
#define V2_GS_FIELDS_CORE(F1, FN, FR)

// HUD / vikings / quit prompt
#define V2_GS_FIELDS_HUD(F1, FN, FR) 

// Level / scroll state / palette-anim / sound-track / spawn descriptors
#define V2_GS_FIELDS_LEVEL(F1, FN, FR) \
  F1(spawn_pool0,        DS_SPAWN_POOL0) \
  F1(chunk_cur,          DS_CHUNK_CUR) \
  F1(chunk_tile,         DS_CHUNK_TILE) \
  F1(chunk_bg,           DS_CHUNK_BG) \
  F1(cmd_write,          DS_CMD_WRITE) \
  F1(obj_queue_head,     DS_OBJ_QUEUE_HEAD) \
  F1(cmd_read,           DS_CMD_READ) \
  FN(transition_level_tbl, DS_TRANSITION_LEVEL_TBL, 7) \
  FN(transition_chunk_tbl, DS_TRANSITION_CHUNK_TBL, 6) \
  FN(scroll_px_lut,      DS_SCROLL_STEP2_TBL, 19) \
  F1(decomp_size,        DS_DECOMP_SIZE)

// Segment registry + loader cursors + sound/config words
#define V2_GS_FIELDS_SEG(F1, FN, FR)

// Sound / AIL / startup config words
#define V2_GS_FIELDS_SOUND(F1, FN, FR)

// Page emulator / palette pipeline words / VGA-page state
#define V2_GS_FIELDS_PAGES(F1, FN, FR)

// Spec-key init bit-mask words (INT9 cluster)
#define V2_GS_FIELDS_SPEC(F1, FN, FR)
// Sprite table columns — 128 contiguous words each (slot addressing obj=slot*2)
#define V2_GS_FIELDS_SPRITE(F1, FN, FR)

// VM object table columns — 20 contiguous words each (stride 0x28 between
// column bases; the region is a dense run of 20-word columns)
#define V2_GS_FIELDS_OBJ(F1, FN, FR)

// Word aggregate WITHOUT the evacuated fields — the views generate flat
// accessors from this and EVAC-backed accessors from V2_GS_FIELDS_EVAC.
#define V2_GS_FIELDS_W_NOEVAC(F1, FN, FR) \
  V2_GS_FIELDS_CORE(F1, FN, FR)  \
  V2_GS_FIELDS_HUD(F1, FN, FR)   \
  V2_GS_FIELDS_LEVEL(F1, FN, FR) \
  V2_GS_FIELDS_SEG(F1, FN, FR)   \
  V2_GS_FIELDS_PAGES(F1, FN, FR) \
  V2_GS_FIELDS_SOUND(F1, FN, FR) \
  V2_GS_FIELDS_SPEC(F1, FN, FR)  \
  V2_GS_FIELDS_SPRITE(F1, FN, FR)\
  V2_GS_FIELDS_OBJ(F1, FN, FR)

#define V2_GS_FIELDS_W(F1, FN, FR) \
  V2_GS_FIELDS_EVAC(F1, FN, FR)  \
  V2_GS_FIELDS_EVAC_BRIDGE(F1, FN, FR) \
  V2_GS_FIELDS_W_NOEVAC(F1, FN, FR)

// ---------------------------------------------------------------------------
// Byte fields: B1(member, ds_off) scalar byte; BN(member, ds_off, count).
// ---------------------------------------------------------------------------
#define V2_GS_FIELDS_EVACB_B(B1, BN) \
  BN(ail_seq_states,     0x9950, 2600)      \
  B1(scratch_28,       DS_SCRATCH_28)          \
  B1(cmd_active,       DS_CMD_ACTIVE)          \
  B1(scratch_32e,      DS_SCRATCH_32E)         \
  B1(pal_shade_r,      DS_PAL_SHADE_R)         \
  B1(pal_shade_g,      DS_PAL_SHADE_G)         \
  B1(pal_shade_b,      DS_PAL_SHADE_B)         \
  B1(pal_shade_r2,     DS_PAL_SHADE_R2)        \
  B1(pal_shade_g2,     DS_PAL_SHADE_G2)        \
  B1(pal_shade_b2,     DS_PAL_SHADE_B2)        \
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
  B1(byte_save_0,      DS_BYTE_SAVE_0)         \
  B1(byte_save_1,      DS_BYTE_SAVE_1)         \
  B1(byte_save_2,      DS_BYTE_SAVE_2)         \
  B1(pixel_pan,        DS_PIXEL_PAN)           \
  B1(pan_gate,         DS_PAN_GATE)            \
  B1(vga_page_flag,    DS_VGA_PAGE_FLAG)       \
  B1(vga_mode_byte,    DS_VGA_MODE_BYTE)       \
  B1(page_split_1a,    DS_PAGE_SPLIT_1A)       \
  B1(page_split_1b,    DS_PAGE_SPLIT_1B)       \
  B1(page_split_2a,    DS_PAGE_SPLIT_2A)       \
  B1(page_split_2b,    DS_PAGE_SPLIT_2B)       \
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
  BN(chunk_hdr,        DS_CHUNK_HDR, 8)        \
  BN(level_pal_chunks, 0x2E7D, 6912)           \
  BN(chunk_d_tbl,      0x497D, 1568)           \
  BN(hud_gfx_tail,     0x647D, 1024)           \
  BN(pw_level_tbl,     0x687D, 5760)          \
  BN(spawn_area,       DS_SPAWN_TABLE, 1390)  \
  BN(dead_tail_a3a2,      0xA3A2, 23646)         \
  BN(unused_gap_4f9d,  0x4F9D, 224)            \
  BN(unused_8c,        0x008C, 372)            \
  BN(coll_partner_tbl, 0x1B25, 624)            \
  BN(coll_area_pad,    0x1D95, 18)             \
  BN(cmd_ring,         0x1DA7, 1000)           \
  BN(transition_buf,   0x2193, 1000)           \
  BN(error_msg_2bbe,   0x2BBE, 671)            \
  B1(pad_8507,         0x8507)                 \
  BN(viking_spawn_triplets, 0x8508, 54)        \
  BN(pan_luts,         0x853E, 12)             \
  BN(dead_data_8560,   0x8560, 69)             \
  BN(level_passwords,  0x85A5, 148)            \
  BN(scan_filter_lists,0x94CC, 156)

// AIL sequencer state block: joined the carrier in wave 11 (the router
// mirrors every write channel; values stay excluded from ORIG-vs-V2 verify
// as documented wall-clock state).
#define V2_GS_FIELDS_B_NOEVAC(B1, BN)

#define V2_GS_FIELDS_B(B1, BN) \
  V2_GS_FIELDS_EVACB_B(B1, BN) \
  V2_GS_FIELDS_B_NOEVAC(B1, BN)

// Chunk-loaded DS data zones (bounds = decompressed sizes in DATA.DAT,
// verified against the loader ladder in v2_vm.cpp:6294):
//   level_pal_chunks: chunks 4..0xC, 9 x 0x300 at 2E7D..497D (level tables)
//   chunk_d_tbl:      chunk 0xD, 0x620 at 497D (raw gap E0 to 507D follows)
//   hud_item_gfx + hud_sel_gfx + hud_gfx_tail: chunk 0xE, 0x1800 at 507D
//   pw_level_tbl:     chunk 2, 0x1680 at 687D..7EFD (password/level select)
//   ail_state_ptrs[5] @9920 -> {9950,9B58,9D60,9F68,A170}: five XMID
//                     sequence state blocks of 0x208 bytes each =
//                     ail_seq_states @9950..A378 (ends exactly at
//                     DS_AIL_MUSIC_STATE; pointer table read from the live
//                     DS dump, stride 0x208 uniform)
//   unused_tail @A3A2..10000: DGROUP BSS reserve. Proof it is dead: all
//                     zeros in the EXE image, all zeros in live DS dumps at
//                     the END of level1 and empty replays, and the full-DS
//                     hash verify never flagged the range in the project's
//                     entire history (neither world writes it).
//   unused_gap_4f9d / unused_8c: dead like the tail (zero in image AND in
//                     live dumps; full-DS hash verify never flagged them)
//   word_200 @200:    C-RTL startup time low word; the m2c INT21/2C model
//                     returns fixed cx=0x1234 dx=0x5678 (asm.cpp:513) and
//                     the Borland RTL stores them here (0x204 = 0x5678 in
//                     the rt_0204 zone = the high half + RTL scratch)
//   coll_partner_tbl @1B25: rows of 16 bytes addressed (obj<<4)+bit_idx;
//                     obj is EVEN (slot*2) so live rows interleave with
//                     unused ones; bit_idx resets to 0 per frame and grows
//                     +2 per collision opcode — the 16-entry mask LUTs cap
//                     the meaningful range at one 16-byte row. 0x270 bytes
//                     to obj 0x26 + an 18-byte pad to the command ring.
//   cmd_ring @1DA7:   VARIABLE-length command stream, not fixed records:
//                     a type word (4 / 8 / 0xA ...) followed by type-specific
//                     args; writers advance DS_CMD_WRITE by 2/8/... bytes,
//                     the reader drains at DS_CMD_READ
//   cmd_ring @1DA7..218F: command ring buffer body (write/read cursors are
//                     the DS_CMD_WRITE/DS_CMD_READ fields)
//   transition_buf @2193..257B: transition-chunk load / queue body area
//   level_chunk_tbl/level_template_tbl @940C/946C: parallel 48-entry
//                     per-level tables read by sub_111b1 as [level*2-0x6BF4]
//                     / [level*2-0x6B94] (chunk id / template id, FFFF=none)
//   subsprite_off_tbl @871C: sub-sprite data pointers read as
//                     [(si-0x30)-0x78E4] in the 1358C loop — values are the
//                     1-based OBJ_SPRITE_OFF offsets (12 slots si=30..46)
//   pan_luts @853E: PAN X ([di-0x7AC2] wrap) and PAN Y ([di-0x7ABC] wrap,
//                     base +6) byte-pair tables read at eip 0x139B/0x13A5
//   pal_chunk_addr_tbl[11] @854A: the ORIG source of the level-table loader
//                     ladder (2E7D 317D .. 507D — the v2_vm:6297 list mirrors it)
//   level_passwords @85A5: 37 levels x 4 high-bit-ASCII letters, ends
//                     exactly at rng_seed (first entry = "STRT")
//   viking_spawn_triplets @8508: 3 records x 18B, each = three {x,y,anim}
//                     viking spawn points fed to sub_13809 via sub_11569
//                     (static sites di=8508/851A/852C); data_853e tail holds
//                     high-bit-ASCII letters (password-style), readers TBD
//   row43_lut @938A:  25 x i*0x2B — tile-row to VGA WORD offset (43 = 86/2
//                     word pitch, 25 = 200/8 screen tile rows)
//   error_msg_2bbe:   ASCII error text (immutable image data)
//   anim_quad_queue (@8736, 120B = 20 records x 3 words): the animated-tile
//                     redraw queue. Writer sub_13fc2 (VM "place 2x2 animated
//                     tile quad" helper, eip 0x4035-0x4043): each on-screen
//                     quad appends {fs_map_offset, world_x(ds:6C),
//                     world_y(ds:6E)} and bumps the cursor ds:8734 by 3
//                     (words). Consumer sub_1406d (the anim queue pass,
//                     CC_1406D) walks it tail-first with visibility
//                     hysteresis. (The old "sound far record {seg,~,len}"
//                     guess was a value coincidence — l1's fs offset 5D24
//                     happened to land inside the sound window. VI.1 verdict
//                     via GDB watchpoint on real_ds[0x8736]: 4/4 hits at
//                     seg000 eip 0x4035, caller chain 15473->141e0->13fc2.)
//   VI.2 role verdicts (2026-08-12), readers verified line-by-line:
//   slope_ramps_897c: byte LUT [slope_type<<4 | x&0xF] -> height 0..F,
//                     reader sub_16390 ("slope height diff", dispatch case
//                     103, eip 0x639B [si-7684h]); static content IS the
//                     ramp set (asc 0..F, desc F..0, half-slopes...).
//                     124B = 7 full ramps + 12B stub before LUT_PAGE_ROW
//                     @89F8 -> legal slope types 0..7. The old "shade_ramps"
//                     name was a guess - nothing shading-related.
//   hexdigit_cells@931F: word LUT, reader eip 0xDEF..0xE2x prints AX as hex
//                     after the DOS INT21/9 banner: per nibble bx=nib*2,
//                     word [bx-6CE1h] low byte -> byte_2b337..2b339 char
//                     cells (high byte 0x07 = text attr). CONFIRMED.
//   Boot chunk map (from the loader log, sizes hex) closes the whole
//   497D..687D region:
//     chunk_d_tbl@497D      = DATA.DAT chunk #D payload (0x620 exactly)
//     unused_gap_4f9d (224) = the hole between chunks #D and #E, zero in
//                             static and never referenced (alignment gap)
//     chunk #E payload      = 507D..687D (0x1800) — ALREADY fully typed:
//                             hud_item_gfx@507D (4864B, item_id*256 glyphs,
//                             the sub_1183d hudvga-unit source) +
//                             hud_sel_gfx@637D (256B) + hud_gfx_tail@647D
//                             (1024B). So chunk #E is the HUD graphics
//                             bank. (A naive one-BN merge overlapped at
//                             507D — the phase-D coverage bitmap caught it
//                             and failed the run: the guard works.)
//     pw_level_tbl@687D     = chunk #2 payload (0x1680 exactly)
//   ail_seq_states@9950 (2600B): XMIDI per-sequence state records, live
//                     content in every golden; addressed si-relative (the
//                     [si-66F4h] handle / [si-66E0h] family — the field map
//                     lives in src/sdl/v2_ail.cpp), si dispatched via
//                     off_3285a by snd type [25B7]. Mirrored + hashed (the
//                     98E4..9950 skip window ENDS here). Record stride =
//                     next-wave GDB read-watchpoint.
//   Wave-4 bulk verdicts (2026-08-12, batch classifier: static zeros +
//   live-golden bytes + exact raddr-form refs over seg000/002/003):
//   MECHANISM-VERIFIED (role carried by an already-verified channel):
//     pal_anim_reload/start/end (@2584/2594/259C, 8B each) — the 10ffc
//       palette-anim mirror fields (DS_PAL_ANIM_* constants);
//     chunk_hdr@2BB4 — the 8B fread header (class-D chunk channel);
//     byte_or_lut@93BC / byte_and_lut@93C4 — the subtractive-base bit
//       LUTs (LUT_BYTE_OR@6C44 family, unit-exercised);
//     subsprite_off_tbl@871C — the 12fc6/12fcb sub-sprite offset pairs;
//     viking_spawn_triplets@8508 / image_86e0 — static data blocks
//       (content present in ds_static, byte-identical in goldens).
//   DEAD-PADDING (all-zero in static AND in live goldens, zero exact
//   references — the earlier grep "refs" on small offsets were literal
//   noise; raddr-form counts are 0): unused_8c(372), zero_028e(116),
//     zero_9292/91b1/9230/91f0/9262/9212/918c/916c/9182(9xxx input pad),
//     zero_98ec/98de/9936/9948/9944 (AIL pad), zero_0048/0070/0082/002c/
//     0309/03bc/0318/0338/033e/03c8/03d2/0306/007e/0029/003c/0333/0420/
//     044c (boot-era scratch, never revisited), zero_25a9/25b1/25c5,
//     zero_8502, zero_86c6/86d2, zero_9220/9228/9280/928a/928e/92fd/9301/
//     920c/91a5/919f/919a/91ad/917d, coll_area_pad@1D95, pad_a37a_none.
//   spawn_area@25F6: SOLVED (wave 5, GDB watch over level1): the level
//     loader (eip 0x11D5: MOV di,25B3h; es=ds; JMP read_chunk) streams the
//     LEVEL chunk straight into ds:25B3.. — so 25B3..2B64 is the per-level
//     payload (map params, snd type @25B7, map stride @25DC, spawn records
//     consumed by the 13809 spawn machinery; both ends v2-mirrored).
//   Wave-6 watchpoint verdicts (2026-08-12, attract/level1 GDB watches):
//     rt_0204 / rt_0354: written by the VM op_57 store (MOV [si],ax at
//       eip 0x46AC) — LEVEL-SCRIPT VARIABLE SLOTS; exact contents are
//       script data, the mechanism is unit-verified (vmops class B).
//     rt_0378: the VM priority-object queue ([si+378h]=di, cursor ds:376
//       INC — the live [372]/[376] re-read pass of v2_vm_pass_14207).
//     rt_92ef / rt_92f3 (word_317CF/317D3): CRTC display-address fields
//       written by set_display_memory_addr (sub_16775 family, 95 hits/
//       attract) — the page-flip mirror owns them.
//     pan_luts@853E: static LUT bytes (present in ds_static, never
//       written at runtime — no writer is correct, like image_86e0).
//     rt_86b0/86ba, rt_25cd/25d0/25e0/25e7 + the live=1 single-byte
//       tails: not exercised in the current replay windows — micro-queue
//       for future real-gameplay replays (a few dozen bytes total).
//   Wave-3 quick verdicts (2026-08-12):
//     error_msg_2bbe: CONFIRMED by content — DOS $-terminated error text
//       ("*** An Error has occured while running PC Vikings ***", the 564K
//       memory message...) for the INT21/9 startup failures.
//     scan_filter_lists@94CC: ALREADY VERIFIED via LUT_SCAN_FILTER — the
//       FF-terminated type lists walked by VM ops 2C/2D/35 (unit-covered).
//     coll_partner_tbl@1B25: CONFIRMED by body — sub_16235 (the 15530
//       collision family) stores/loads the per-object partner index at
//       [di+1B25h].
//     level_passwords@85A5: name UNVERIFIED and likely wrong — the zone is
//       all-zero in the static image AND in every live golden; no reader
//       found yet. Do not trust the name until a reader confirms it.
//     pw_level_tbl@687D reader status: NOT exercised by the replay corpus
//       (read-watchpoints over pw_00_STRT, synth_pw_enter AND the real
//       level2 walkthrough saw only the PSNAP memcpy); rt_86b0/86ba and
//       rt_25cd..25e7 also stayed silent through level2 — the micro-queue
//       keeps its "awaiting a triggering scenario" status honestly.
//   cmd_ring@1DA7 (1000B): ALREADY VERIFIED as the sub_1086f command ring
//                     (entries at [bx+1DA7], write cursor DS_CMD_WRITE@218F,
//                     read cursor DS_CMD_READ@2B64) — the 1086f unit and the
//                     battle handler mirror exercise it end-to-end.
//   transition_buf@2193 (1000B): ALREADY VERIFIED as the transition-chunk
//                     load region (DS_TRANSITION_CHUNK_BUF, v2_read_chunk
//                     target — class-D chunk channel ran the whole DATA.DAT
//                     through it byte-for-byte twice).
//   pw_level_tbl@687D (5760B): loader-CONFIRMED as the DATA.DAT chunk #2
//                     payload (boot loader eip 0x2C0C, right after chunk
//                     #14 -> 0x507D). No direct/computed readers and no
//                     static pointer holds 0x687D — access goes through a
//                     computed register base; reader hunt = next phase-D
//                     wave (GDB read-watchpoint over a pw scenario).
//   level_pal_chunks@2E7D (6912B = 9 slots x 0x300): CONFIRMED palette
//                     chunk slots — the boot loader (seg000 eip 0x2B7D..)
//                     reads DATA.DAT chunks #4..#12 into consecutive 768B
//                     slots 2E7D/317D/347D/.../467D; pal_chunk_addr_tbl
//                     @854A holds exactly these 9 addresses (+2 tail ptrs).
//                     Live golden content is 6-bit DAC triplets.
//   dead_tail_a3a2 (23646B, 36% of the raw backing): CONFIRMED DEAD by
//                     three channels (phase-D wave 1, 2026-08-12): all-zero
//                     in ds_static.bin, all-zero in the LIVE golden end
//                     states (attract gameplay, level1, pw scenarios), zero
//                     direct references in seg000 — and the per-opcode
//                     full-DS hash has compared this zone orig-vs-v2 for
//                     months without a single diff, so the orig keeps it
//                     zero too. (The stack is NOT here: ss is a separate
//                     arena segment.)
//   dead_data_8560 (69B between pal_chunk_addr_tbl and level_passwords):
//                     ZERO references in the whole port (direct 856xh
//                     literals + computed [-0x7AA0] form, seg000/002/003 and
//                     dispatchers). Leftover EXE data - stays raw BN.
//   spawn_area:       25F6..2B64 (bounds = neighboring proven fields).
//                     Content is LEVEL-VARIABLE, two overlapping views:
//                     (a) 14-byte entries indexed OBJ_ANIM_SUB*14 — word
//                     x @+0 (FFFF = terminator), flags @+10 (bit 0x800 =
//                     permanent spawn, 13bbd); (b) the 1133a HUD/pal-anim
//                     prefix: en-byte, then 3-byte (reload,start,end)
//                     records each followed by an FFFF-terminated word
//                     sub-list. A fixed field carve is impossible by design.

// Gap-fill zones (generated; classes proven by the three-dump protocol:
// ds_static image + live empty-replay dump + live level1-end dump, plus
// the project-wide full-DS hash verify history):
//   zero_*  = dead reserve;  image_* = immutable data;  rt_* = runtime
#define V2_GS_FIELDS_GAPFILL(B1, BN) \
  BN(zero_0000,          0x0000, 40)           \
  BN(zero_0029,          0x0029, 1)           \
  BN(zero_002c,          0x002c, 6)           \
  BN(zero_003c,          0x003c, 2)           \
  BN(zero_0048,          0x0048, 36)           \
  BN(zero_0070,          0x0070, 8)           \
  BN(zero_007e,          0x007e, 2)           \
  BN(zero_0082,          0x0082, 8)           \
  BN(zero_0290,          0x0290, 114)           \
  BN(zero_0306,          0x0306, 2)           \
  BN(zero_0309,          0x0309, 7)           \
  BN(zero_0318,          0x0318, 22)           \
  BN(zero_0333,          0x0333, 1)           \
  BN(zero_0338,          0x0338, 4)           \
  BN(zero_033e,          0x033e, 2)           \
  BN(rt_0354,            0x0354, 30)           \
  BN(rt_0378,            0x0378, 20)           \
  BN(zero_03bc,          0x03bc, 6)           \
  BN(zero_03c8,          0x03c8, 2)           \
  BN(zero_03d2,          0x03d2, 2)           \
  BN(zero_0420,          0x0420, 1)           \
  BN(zero_044c,          0x044c, 1)           \
  BN(zero_25a9,          0x25a9, 1)           \
  BN(zero_25b1,          0x25b1, 2)           \
  BN(rt_25cd,            0x25cd, 2)           \
  BN(rt_25d0,            0x25d0, 12)           \
  BN(rt_25e0,            0x25e0, 1)           \
  BN(rt_25e7,            0x25e7, 15)           \
  BN(zero_8502,          0x8502, 2)           \
  BN(rtl_io_error_msg,   0x863d, 111)          \
  BN(rt_86b0,            0x86b0, 2)           \
  BN(rt_86ba,            0x86ba, 10)           \
  BN(zero_86c6,          0x86c6, 10)           \
  BN(zero_86d2,          0x86d2, 8)           \
  BN(image_86e0,         0x86e0, 6)           \
  BN(subsprite_off_tbl,  0x871c, 24)           \
  BN(anim_quad_queue,    0x8736, 120)           \
  BN(slope_ramps_897c,   0x897c, 124)          \
  BN(zero_916c,          0x916c, 16)           \
  BN(zero_917d,          0x917d, 4)           \
  BN(zero_9182,          0x9182, 9)           \
  BN(zero_918c,          0x918c, 13)           \
  BN(zero_919a,          0x919a, 3)           \
  BN(zero_919f,          0x919f, 5)           \
  BN(zero_91a5,          0x91a5, 6)           \
  BN(zero_91ad,          0x91ad, 3)           \
  BN(zero_91b1,          0x91b1, 61)           \
  BN(zero_91f0,          0x91f0, 26)           \
  BN(zero_920c,          0x920c, 4)           \
  BN(zero_9212,          0x9212, 12)           \
  BN(zero_9220,          0x9220, 4)           \
  BN(zero_9228,          0x9228, 2)           \
  BN(zero_9230,          0x9230, 46)           \
  BN(zero_9262,          0x9262, 24)           \
  BN(zero_9280,          0x9280, 2)           \
  BN(zero_928a,          0x928a, 2)           \
  BN(zero_928e,          0x928e, 2)           \
  BN(zero_9292,          0x9292, 92)           \
  BN(rt_92ef,            0x92ef, 3)           \
  BN(rt_92f3,            0x92f3, 4)           \
  BN(zero_92fd,          0x92fd, 2)           \
  BN(zero_9301,          0x9301, 4)           \
  BN(hexdigit_cells,     0x931f, 39)           \
  BN(byte_or_lut,        0x93BC, 8)             \
  BN(byte_and_lut,       0x93C4, 8)             \
  BN(zero_98de,          0x98de, 12)           \
  BN(zero_98ec,          0x98ec, 32)           \
  BN(zero_9936,          0x9936, 8)           \
  BN(zero_9944,          0x9944, 2)           \
  BN(zero_9948,          0x9948, 8)           \
  BN(pad_a37a_none,      0xa39a, 0)

// ---------------------------------------------------------------------------
// The typed state. Serializer contract: v2_gs_deserialize fills every field
// from a 64K DS image (and snapshots the raw backing); v2_gs_serialize emits
// a 64K image where covered bytes come ONLY from fields and uncovered bytes
// come from the backing. v2_gs_roundtrip_check does both + memcmp.
// ---------------------------------------------------------------------------
struct V2GameState {
#define V2_GS_M1(name, off)      uint16_t name;
#define V2_GS_MN(name, off, n)   uint16_t name[n];
#define V2_GS_MR(name, off, n, rec, fld) V2_GS_MN(name, off, n)
  V2_GS_FIELDS_W(V2_GS_M1, V2_GS_MN, V2_GS_MR)
#undef V2_GS_M1
#undef V2_GS_MN
#undef V2_GS_MR
#define V2_GS_MB1(name, off)     uint8_t name;
#define V2_GS_MBN(name, off, n)  uint8_t name[n];
#define V2_GS_MBR(name, off, n, rec, fld) V2_GS_MBN(name, off, n)
  V2_GS_FIELDS_B(V2_GS_MB1, V2_GS_MBN)
  V2_GS_FIELDS_GAPFILL(V2_GS_MB1, V2_GS_MBN)
#undef V2_GS_MB1
#undef V2_GS_MBN
#undef V2_GS_MBR
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
  A1(fs_page_stride,     DS_FS_PAGE_STRIDE)     \
  A1(music_id,           DS_MUSIC_ID)           \
  A1(dac_r_save_w,       DS_DAC_R_SAVE)

// ---------------------------------------------------------------------------
// Stage 4 II.c: evacuated-field storage. For the CANONICAL world (the live
// shadow DS) the members below are the carrier; accessors write through to
// the flat image so serialization/verify/golden see identical bytes, and
// v2_gs_evac_check FATALs on any image byte changed behind the accessors.
// All other worlds (oracle copies, unit scratch, save buffers) stay flat.
// ---------------------------------------------------------------------------
// wave 14: object/sprite columns live as record arrays — the natural
// shape (the DS kept them as parallel 20/128-word columns; the router
// maps every byte back to the flat offsets).
struct V2ObjRec {
    uint16_t frac_x;
    uint16_t frac_y;
    uint16_t anim_pc;
    uint16_t anim_timer;
    uint16_t anim_cont;
    uint16_t sub_slot;
    uint16_t sub_end;
    uint16_t sub_count;
    uint16_t sub_anim_ptr;
    uint16_t pc;
    uint16_t code_seg;
    uint16_t alt_pc;
    uint16_t x_prev;
    uint16_t y_prev;
    uint16_t coll_bits;
    uint16_t anim_table;
    uint16_t width;
    uint16_t height;
    uint16_t half_h;
    uint16_t half_w;
    uint16_t bbox_y0;
    uint16_t bbox_y1;
    uint16_t bbox_x0;
    uint16_t bbox_x1;
    uint16_t flags;
    uint16_t res_handle;
    uint16_t res_cost;
    uint16_t state_idx;
    uint16_t class_bits;
    uint16_t anim_dx;
    uint16_t anim_dy;
    uint16_t spawn_pool;
    uint16_t anim_sub;
    uint16_t anim_idx;
    uint16_t timer;
    uint16_t world_x;
    uint16_t world_y;
    uint16_t vel_x_max;
    uint16_t vel_y_max;
    uint16_t type_id;
    uint16_t parent;
    uint16_t child;
    uint16_t sprite_base;
    uint16_t state_187d;
    uint16_t state_18a5;
    uint16_t state_18cd;
    uint16_t state_18f5;
    uint16_t cur_sprite;
    uint16_t vel_x;
    uint16_t vel_y;
    uint16_t partner;
};
struct V2SprRec {
    uint16_t flags;
    uint16_t cls;
    uint16_t x;
    uint16_t y;
    uint16_t off;
    uint16_t seg;
    uint16_t src_base;
    uint16_t src_seg;
    uint16_t strips;
    uint16_t cur_x;
    uint16_t cur_y;
    uint16_t old_x;
    uint16_t old_y;
    uint16_t dirty;
};
struct V2GsEvac {
    V2ObjRec obj_rec[20];
    V2SprRec spr_rec[128];
#define V2_GS_EV1(name, off)     uint16_t name;
#define V2_GS_EVN(name, off, n)  uint16_t name[n];
#define V2_GS_EVR(name, off, n, rec, fld)
  V2_GS_FIELDS_EVAC(V2_GS_EV1, V2_GS_EVN, V2_GS_EVR)
  V2_GS_FIELDS_EVAC_BRIDGE(V2_GS_EV1, V2_GS_EVN, V2_GS_EVR)
#undef V2_GS_EV1
#undef V2_GS_EVN
#undef V2_GS_EVR
#define V2_GS_EVB1(name, off)    uint8_t name;
#define V2_GS_EVBN(name, off, n) uint8_t name[n];
#define V2_GS_EVBR(name, off, n, rec, fld) V2_GS_EVBN(name, off, n)
  V2_GS_FIELDS_EVACB(V2_GS_EVB1, V2_GS_EVBN)
#undef V2_GS_EVB1
#undef V2_GS_EVBN
#undef V2_GS_EVBR
};
extern V2GsEvac g_gs_evac;
extern "C" {
extern const uint8_t* v2_gs_evac_canonical;   // the live shadow DS (or null)
void v2_gs_evac_set_canonical(const uint8_t* ds);   // also refreshes members
void v2_gs_evac_refresh(const uint8_t* ds);   // members <- image bytes
int  v2_gs_evac_check(const uint8_t* ds);     // 0 ok; diffs logged + FATAL
void v2_gs_evac_read_desync(const char* fld, uint32_t off, uint16_t mem, uint16_t img);
}
static inline bool v2_gs_evac_on(const uint8_t* ds) {
    return ds == v2_gs_evac_canonical && ds != nullptr;
}

// Mirror functions (stage-4 wave 10): O(1) LUT-router wrappers — bodies in
// v2_gamestate.cpp. v2_gs_route[a] points at the byte OWNING DS offset a
// (a carrier member byte for evacuated fields, the flat image byte
// otherwise); rebuilt on every canonical change.
extern uint8_t* v2_gs_route[0x10000];
// wave 13: rebuild the image from the members (router-guided). The members
// are the authoritative carrier; the flat image is a derived read cache —
// V2_GS_IMAGE_REBUILD=1 proves it by rebuilding the image every frame.
extern "C" void v2_gs_image_render(uint8_t* ds);
extern "C" void v2_gs_evac_mirror_w(const uint8_t* ds, uint16_t addr, uint16_t val);
extern "C" void v2_gs_evac_mirror_span(const uint8_t* ds, uint32_t addr, uint32_t len);
extern "C" void v2_gs_evac_mirror_b(const uint8_t* ds, uint16_t addr, uint8_t val);

// Stage 4 II.b: bounds sanitizer. Reports (dedup) every runtime-indexed
// access that leaves its field's span — building the wrap map that decides
// which fields may leave the flat layout in the carrier swap.
#ifdef V2_GS_BOUNDS
extern "C" void v2_gs_bounds_note(uint32_t base, uint32_t len, uint32_t off);
#define V2_GS_BCHK(off, len, o) \
    do { if ((uint32_t)(o) + 2u > (uint32_t)(len)) \
             v2_gs_bounds_note((uint32_t)(off), (uint32_t)(len), (uint32_t)(o)); } while (0);
#else
#define V2_GS_BCHK(off, len, o)
#endif

struct V2StateView {
    uint8_t* ds;
    explicit V2StateView(uint8_t* ds_) : ds(ds_) {}

#define V2_GS_A1(name, off) \
    uint16_t name() const              { return *(const uint16_t*)(ds + (off)); } \
    void     name(uint16_t v)          { *(uint16_t*)(ds + (off)) = v; } \
    uint16_t& name##_ref()             { return *(uint16_t*)(ds + (off)); }
#define V2_GS_AN(name, off, n) \
    uint16_t name(uint32_t i) const    { V2_GS_BCHK(off, 2u*(n), 2u*i) return *(const uint16_t*)(ds + (off) + 2u * i); } \
    void     name(uint32_t i, uint16_t v) { V2_GS_BCHK(off, 2u*(n), 2u*i) *(uint16_t*)(ds + (off) + 2u * i) = v; }
#define V2_GS_AR(name, off, n, rec, fld) V2_GS_AN(name, off, n)
    V2_GS_FIELDS_W_NOEVAC(V2_GS_A1, V2_GS_AN, V2_GS_AR)
    // Stage 4 II.c: evacuated fields — typed member carrier + write-through.
// FLIPPED stage (bridge held: writes still keep the image in sync for the
// serializer/oracles and the operand read path): READS for the canonical
// world come from the typed members — the member IS the carrier. The
// bypass map stayed empty across the full corpus before this flip; the
// per-frame check still guards the mirror.
#define V2_GS_AE1(name, off) \
    uint16_t name() const              { if (v2_gs_evac_on(ds)) { \
        uint16_t _img = *(const uint16_t*)(ds + (off)); \
        if (g_gs_evac.name != _img) v2_gs_evac_read_desync(#name, (off), g_gs_evac.name, _img); \
        return g_gs_evac.name; } return *(const uint16_t*)(ds + (off)); } \
    void     name(uint16_t v)          { if (v2_gs_evac_on(ds)) g_gs_evac.name = v; *(uint16_t*)(ds + (off)) = v; }
#define V2_GS_AEN(name, off, n) \
    uint16_t name(uint32_t i) const    { V2_GS_BCHK(off, 2u*(n), 2u*i) if (v2_gs_evac_on(ds)) return g_gs_evac.name[i]; return *(const uint16_t*)(ds + (off) + 2u * i); } \
    void     name(uint32_t i, uint16_t v) { V2_GS_BCHK(off, 2u*(n), 2u*i) if (v2_gs_evac_on(ds)) g_gs_evac.name[i] = v; *(uint16_t*)(ds + (off) + 2u * i) = v; }
#define V2_GS_AER(name, off, n, rec, fld) \
    uint16_t name(uint32_t i) const    { V2_GS_BCHK(off, 2u*(n), 2u*i) if (v2_gs_evac_on(ds)) return g_gs_evac.rec[i].fld; return *(const uint16_t*)(ds + (off) + 2u * i); } \
    void     name(uint32_t i, uint16_t v) { V2_GS_BCHK(off, 2u*(n), 2u*i) if (v2_gs_evac_on(ds)) g_gs_evac.rec[i].fld = v; *(uint16_t*)(ds + (off) + 2u * i) = v; }
    V2_GS_FIELDS_EVAC(V2_GS_AE1, V2_GS_AEN, V2_GS_AER)
#undef V2_GS_AE1
#undef V2_GS_AEN
#undef V2_GS_AER
    // (wave 9) the bridge group moved into V2_GS_FIELDS_EVAC above — this
    // expansion is now empty and kept only as the landing pad for the next
    // freshly evacuated group.
#define V2_GS_AE1(name, off) \
    uint16_t name() const              { return *(const uint16_t*)(ds + (off)); } \
    void     name(uint16_t v)          { if (v2_gs_evac_on(ds)) g_gs_evac.name = v; *(uint16_t*)(ds + (off)) = v; }
#define V2_GS_AEN(name, off, n) \
    uint16_t name(uint32_t i) const    { V2_GS_BCHK(off, 2u*(n), 2u*i) return *(const uint16_t*)(ds + (off) + 2u * i); } \
    void     name(uint32_t i, uint16_t v) { V2_GS_BCHK(off, 2u*(n), 2u*i) if (v2_gs_evac_on(ds)) g_gs_evac.name[i] = v; *(uint16_t*)(ds + (off) + 2u * i) = v; }
#define V2_GS_AER(name, off, n, rec, fld) V2_GS_AEN(name, off, n)
    V2_GS_FIELDS_EVAC_BRIDGE(V2_GS_AE1, V2_GS_AEN, V2_GS_AER)
#undef V2_GS_AE1
#undef V2_GS_AEN
#undef V2_GS_AER
#undef V2_GS_A1
#undef V2_GS_AN
#undef V2_GS_AR
#define V2_GS_AB1(name, off) \
    uint8_t  name##_b() const          { return ds[(off)]; } \
    void     name##_b(uint8_t v)       { v2_gs_evac_mirror_b(ds, (off), v); ds[(off)] = v; } \
    uint8_t& name##_bref()             { return ds[(off)]; }
#define V2_GS_ABN(name, off, n) \
    uint8_t* name##_bytes()            { return ds + (off); } \
    const uint8_t* name##_bytes() const { return ds + (off); }
#define V2_GS_ABR(name, off, n, rec, fld) V2_GS_ABN(name, off, n)
    V2_GS_FIELDS_B_NOEVAC(V2_GS_AB1, V2_GS_ABN)
    // Stage 4 II.c EVACB: byte fields on the typed carrier. Same flip
    // discipline as the word fields: reads come from the member, writes go
    // member+image (write-through). No _bref for evacuated fields — a
    // byte reference would bypass the carrier; the compiler finds clients.
    // _bytes() stays image-backed (write-through keeps it live for readers);
    // WRITERS through _bytes() must use _bset()/spans instead.
// FLIPPED stage (wave 9): byte reads come from the typed member — the
// member IS the carrier. Write-through still keeps the image live for the
// serializer/oracles and the remaining flat readers (_bytes()).
#define V2_GS_ABE1(name, off) \
    uint8_t  name##_b() const          { if (v2_gs_evac_on(ds)) { \
        uint8_t _img = ds[(off)]; \
        if (g_gs_evac.name != _img) v2_gs_evac_read_desync(#name, (off), g_gs_evac.name, _img); \
        return g_gs_evac.name; } return ds[(off)]; } \
    void     name##_b(uint8_t v)       { if (v2_gs_evac_on(ds)) g_gs_evac.name = v; ds[(off)] = v; }
#define V2_GS_ABEN(name, off, n) \
    uint8_t* name##_bytes()            { return ds + (off); } \
    const uint8_t* name##_bytes() const { return ds + (off); } \
    uint8_t  name##_bat(uint32_t i) const { V2_GS_BCHK(off, (uint32_t)(n), i) \
        if (v2_gs_evac_on(ds)) return g_gs_evac.name[i]; return ds[(off) + i]; } \
    void     name##_bset(uint32_t i, uint8_t v) { V2_GS_BCHK(off, (uint32_t)(n), i) \
        if (v2_gs_evac_on(ds)) g_gs_evac.name[i] = v; ds[(off) + i] = v; }
#define V2_GS_ABER(name, off, n, rec, fld) \
    uint16_t name(uint32_t i) const    { V2_GS_BCHK(off, 2u*(n), 2u*i) if (v2_gs_evac_on(ds)) return g_gs_evac.rec[i].fld; return *(const uint16_t*)(ds + (off) + 2u * i); } \
    void     name(uint32_t i, uint16_t v) { V2_GS_BCHK(off, 2u*(n), 2u*i) if (v2_gs_evac_on(ds)) g_gs_evac.rec[i].fld = v; *(uint16_t*)(ds + (off) + 2u * i) = v; }
    V2_GS_FIELDS_EVACB(V2_GS_ABE1, V2_GS_ABEN)
#undef V2_GS_ABE1
#undef V2_GS_ABEN
#undef V2_GS_ABER
    // Stage 4 II.a: word access at a runtime byte offset from a field base.
    // EXACT flat semantics incl. 8086 wrap: addr = (uint16_t)((off) + o).
    // II.b: with -DV2_GS_BOUNDS every access outside [off, off+len) is
    // reported (dedup) — the wrap map that gates the carrier's layout
    // freedom (partner=0xFFFF-class reads are LEGAL flat behavior).
#define V2_GS_ATW_IMPL(name, off, len) \
    uint16_t name##_at(uint16_t o) const { V2_GS_BCHK(off, len, o) return *(const uint16_t*)(ds + (uint16_t)((off) + o)); } \
    void     name##_at(uint16_t o, uint16_t v) { V2_GS_BCHK(off, len, o) \
        v2_gs_evac_mirror_w(ds, (uint16_t)((off) + o), v); /* stage-4: computed writes mirror too */ \
        *(uint16_t*)(ds + (uint16_t)((off) + o)) = v; }
#define V2_GS_ATW1(name, off)    V2_GS_ATW_IMPL(name, off, 2u)
#define V2_GS_ATWN(name, off, n) V2_GS_ATW_IMPL(name, off, 2u*(n))
#define V2_GS_ATWR(name, off, n, rec, fld) V2_GS_ATWN(name, off, n)
    V2_GS_FIELDS_W(V2_GS_ATW1, V2_GS_ATWN, V2_GS_ATWR)
#undef V2_GS_ATW1
#undef V2_GS_ATWN
#undef V2_GS_ATWR
#define V2_GS_ATW1(name, off)    V2_GS_ATW_IMPL(name, off, 1u)
#define V2_GS_ATWN(name, off, n) V2_GS_ATW_IMPL(name, off, (uint32_t)(n))
#define V2_GS_ATWR(name, off, n, rec, fld) V2_GS_ATWN(name, off, n)
    V2_GS_FIELDS_B(V2_GS_ATW1, V2_GS_ATWN)
    V2_GS_FIELDS_GAPFILL(V2_GS_ATW1, V2_GS_ATWN)
#undef V2_GS_ATW1
#undef V2_GS_ATWN
#undef V2_GS_ATWR
#undef V2_GS_ATW_IMPL

#undef V2_GS_AB1
#undef V2_GS_ABN
#undef V2_GS_ABR
#define V2_GS_A1(name, off) \
    uint16_t name() const              { return *(const uint16_t*)(ds + (off)); } \
    void     name(uint16_t v)          { v2_gs_evac_mirror_w(ds, (off), v); *(uint16_t*)(ds + (off)) = v; } \
    uint16_t& name##_ref()             { return *(uint16_t*)(ds + (off)); }
    V2_GS_ALIASES(V2_GS_A1)
#undef V2_GS_A1
    // Lo-byte aliases for word fields the orig touches with byte MOVs
    // (phase-9 view translation; the byte IS the word's low half).
#define V2_GS_LOB(name, off) \
    uint8_t  name##_lob() const        { return ds[(off)]; } \
    void     name##_lob(uint8_t v)     { v2_gs_evac_mirror_b(ds, (off), v); ds[(off)] = v; } \
    uint8_t& name##_lobref()           { return ds[(off)]; }
    V2_GS_LOB(sfx_mute,    DS_SFX_MUTE)
    V2_GS_LOB(music_mute,  DS_MUSIC_MUTE)
    V2_GS_LOB(input_accum, DS_INPUT_ACCUM)
    V2_GS_LOB(input_prev,  DS_INPUT_PREV)
    V2_GS_LOB(vsync_count, DS_VSYNC_COUNT)
    V2_GS_LOB(pal_req,     DS_PAL_REQ)
    V2_GS_LOB(render_117d, DS_RENDER_117D)   // word alias exists; byte MOVs hit its low half
    V2_GS_LOB(ail_req_bank,  DS_AIL_REQ_BANK)  // mov al,[993E]-style byte reads
    V2_GS_LOB(ail_req_patch, DS_AIL_REQ_PATCH)
#undef V2_GS_LOB
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
#define V2_GS_AR(name, off, n, rec, fld) V2_GS_AN(name, off, n)
    V2_GS_FIELDS_W_NOEVAC(V2_GS_A1, V2_GS_AN, V2_GS_AR)
#define V2_GS_AE1(name, off) \
    uint16_t name() const              { return *(const uint16_t*)(ds + (off)); }
#define V2_GS_AEN(name, off, n) \
    uint16_t name(uint32_t i) const    { return *(const uint16_t*)(ds + (off) + 2u * i); }
#define V2_GS_AER(name, off, n, rec, fld) V2_GS_AEN(name, off, n)
    V2_GS_FIELDS_EVAC(V2_GS_AE1, V2_GS_AEN, V2_GS_AER)
    V2_GS_FIELDS_EVAC_BRIDGE(V2_GS_AE1, V2_GS_AEN, V2_GS_AER)
#undef V2_GS_AE1
#undef V2_GS_AEN
#undef V2_GS_AER
#undef V2_GS_A1
#undef V2_GS_AN
#undef V2_GS_AR
#define V2_GS_AB1(name, off) \
    uint8_t  name##_b() const          { return ds[(off)]; }
#define V2_GS_ABN(name, off, n) \
    const uint8_t* name##_bytes() const { return ds + (off); }
#define V2_GS_ABR(name, off, n, rec, fld) V2_GS_ABN(name, off, n)
    V2_GS_FIELDS_B(V2_GS_AB1, V2_GS_ABN)
    V2_GS_FIELDS_GAPFILL(V2_GS_AB1, V2_GS_ABN)
    // Stage 4 II.a (const view): read-only _at counterpart.
#define V2_GS_ATW1(name, off) \
    uint16_t name##_at(uint16_t o) const { return *(const uint16_t*)(ds + (uint16_t)((off) + o)); }
#define V2_GS_ATWN(name, off, n) V2_GS_ATW1(name, off)
#define V2_GS_ATWR(name, off, n, rec, fld) V2_GS_ATWN(name, off, n)
    V2_GS_FIELDS_W(V2_GS_ATW1, V2_GS_ATWN, V2_GS_ATWR)
    V2_GS_FIELDS_B(V2_GS_ATW1, V2_GS_ATWN)
    V2_GS_FIELDS_GAPFILL(V2_GS_ATW1, V2_GS_ATWN)
#undef V2_GS_ATW1
#undef V2_GS_ATWN
#undef V2_GS_ATWR

#undef V2_GS_AB1
#undef V2_GS_ABN
#undef V2_GS_ABR
#define V2_GS_A1(name, off) \
    uint16_t name() const              { return *(const uint16_t*)(ds + (off)); }
    V2_GS_ALIASES(V2_GS_A1)
#undef V2_GS_A1
};

// Inline factories: typed access at any call site without a local view.
static inline V2StateView  v2gs(uint8_t* ds)        { return V2StateView(ds); }
static inline V2StateViewC v2gs(const uint8_t* ds)  { return V2StateViewC(ds); }
