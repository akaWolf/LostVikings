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
#define V2_RENDER_FROM_SHADOW

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

// V2 rendering buffer — single persistent buffer, like drawBuffer in the original.
// Game thread writes, v2_swap_render_buf copies to v2_display_buf for render thread.
// Linear format: [y * 320 + x] = palette index, 320x200
extern uint8_t  v2_render_buf[320*200];

// Display buffer — game copies completed frame here under lock,
// render thread reads it under the same lock. No race.
extern uint8_t  v2_display_buf[320*200];
extern std::mutex v2_display_mutex;

// HUD buffer — rendered independently from game data, 320x64
// Screen rows 176-239 (VGA split screen: always from VGA address 0)
extern uint8_t  v2_hud_buf[320*64];

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
    V2_PHASE_POST_FLIP3,       // sub_108c8..sub_10350 (eip 0x00DB..0x00E7)
    V2_PHASE_FRAME_END,        // verify + cleanup
};
extern void v2_signal_phase(V2Phase phase, uint16_t ds_val);  // signal v2 thread + wait
extern void v2_game_thread_start();   // launch v2 thread (called once at startup)
extern void v2_game_thread_stop();    // stop v2 thread
extern void v2_run_animation_vm(uint16_t ds_val);  // legacy: full frame (used during init)

// V2 VM shadow DS — 64KB copy of DS segment, written by v2 VM opcodes.
// Used by v2 renderer when V2_RENDER_FROM_SHADOW is defined.
extern uint8_t* v2_vm_get_shadow_ds();

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
