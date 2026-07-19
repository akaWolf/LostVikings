#include <SDL2/SDL.h>
#include "v2_ds_layout.h"
#include <thread>
#include <vector>
#include <cassert>
#include <cstdio>
#include "v2_input_recorder.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <unistd.h>  // _exit

const int SCREEN_SCALE = 4;
const int SCREEN_WIDTH = 320;
const int SCREEN_HEIGHT = 240;
const int RENDER_WIDTH = 344;
const int RENDER_HEIGHT = 240;
uint32_t tempDrawBuffer[RENDER_WIDTH*RENDER_HEIGHT];
struct myDrawInfoS
{
  uint8_t drawBuffer[65536*4];
  SDL_Color drawPalette[256];
  uint32_t myOffset;
  uint8_t myPixelOffset;
};
struct myDrawInfoS* myDrawInfo = nullptr;
// fn-test (unit 32+): the isolated oracle calls drawPixel (raw-chunk display);
// the selftest process never starts the game/render threads, so give it the
// same buffer the game thread would calloc (#179 path). Contents are legacy
// (myDrawInfo is outside the v2 contract) — only the NULL deref matters.
extern "C" void v2_fntest_alloc_drawinfo(void) {
    if (!myDrawInfo) myDrawInfo = (myDrawInfoS *)calloc(1, sizeof(myDrawInfoS));
}
// Class-C pixel probe: snapshot the orig VISIBLE page (drawBuffer at the
// current flip offset — same mapping headless_dump uses for its PPMs).
// Returns 0 if the buffer isn't available or the page window would overrun.
//
// CRTC unfold (task #19): drawBuffer models VGA Mode X memory expanded ×4
// (byte_addr*4 + plane), and the real CRT scans each screen row from
// myOffset + y*0x56 — CRTC offset reg 0x13 = 0x2B words = 86 bytes/row
// (init table ds:0x89DC, sub_16807). One screen row therefore spans 344
// drawBuffer bytes, of which 320 are visible; myPixelOffset is the
// attribute-controller pixel pan (0..3 px, value/2 — seg000 sub_1797b).
// The old linear-320 read sheared every frame by 24 px/row: all "class 2/3"
// A2 diffs (stars, password screen, scroll) were artifacts of that shear.
// A2 page snapshot (task #21): the sensor runs on the v2 game-loop thread
// AFTER the orig thread signals V2_PHASE_RENDER3, but orig keeps going and
// executes the NEXT frame's sub-frame-1 sub_16775 (myOffset advances to the
// next page of the 3-page rotation) before v2 finishes comparing. On frames
// where adjacent pages differ (flame animation phases, transitions, scroll)
// the sensor then unfolded the WRONG page — every residual R1/R2 diff class
// matched the previous rotation page exactly (V2-ALT-PAGE proof). Snapshot
// the whole unfolded page in the ORIG thread right after the 3rd-sub-frame
// sub_16775 (eip 0xD8), where myOffset is exactly this frame's page.
static uint8_t v2_a2_page_snap[320 * 176];
static int     v2_a2_page_snap_valid = 0;
// crtc_offset/pixel_pan come from the sub_16775 body locals (the freshly
// computed sub-frame-3 CRTC start + x pan). myDrawInfo->myOffset must NOT be
// used here: it is the retrace-latch published by sub_1797b (render callback)
// and stably carries the SUB-2 value — the measured +1-row panorama skew.
extern "C" int v2_objtrace_di;
extern "C" void v2_objtrace(const char* tag, int a, int b, int c, int d);
void v2_hw_wp_arm(uint8_t* ptr, const char* label);   // C++ linkage (v2_vm.cpp)
uint8_t* v2_a2_softwp_ptr = nullptr;   // soft watch byte (task #21 writer hunt)
uint8_t* v2_a2_softwp_fs_ptr = nullptr; // soft watch on the REAL FS cell word
uint16_t v2_a2_softwp_fs_moff = 0;      // its map offset (shadow drift compare)
uint16_t v2_a2_snap_pg = 0xFFFF;   // ds:0x92F9 at the snapshot moment (sub-3 body)
extern "C" void v2_a2_snapshot_page(const uint8_t* dsb, uint32_t crtc_offset, uint32_t pixel_pan) {
    if (!myDrawInfo) return;
    if (dsb) v2_a2_snap_pg = *(const uint16_t*)(dsb + 0x92F9);
    // Traced-object band probe (task #21): checksum the object's 32-row band
    // on ALL THREE REAL pages via their y_high subtables — tells whether the
    // orig background-role page carries the sprite pixels (the letter classes).
    {
        int tdi = v2_objtrace_di;
        if (dsb && tdi != 0xFFFF) {
            {
                int16_t oy = *(const int16_t*)(dsb + (uint16_t)(tdi + OBJ_SPRITE_Y));
                int sums[3] = {0, 0, 0};
                static const uint16_t pgs[3] = {0, 0x34, 0x68};
                for (int p = 0; p < 3; p++) {
                    for (int y = oy; y < oy + 32; y++) {
                        uint16_t yh = *(const uint16_t*)(dsb + (uint16_t)(LUT_PAGE_ROW + pgs[p] + (uint16_t)((y >> 3) * 2)));
                        uint32_t rowaddr = (uint32_t)yh + (uint32_t)(y & 7) * 0x56u + 8u;
                        uint32_t base = rowaddr * 4u;
                        if (base + 320 > sizeof(myDrawInfo->drawBuffer)) continue;
                        for (int x = 0; x < 320; x += 4) sums[p] += myDrawInfo->drawBuffer[base + x];
                    }
                }
                v2_objtrace("o:pg", (int16_t)sums[0], (int16_t)sums[1], (int16_t)sums[2], oy);
                // Writer hunt: soft watch on one letter pixel byte of the pg34
                // page — v2_record_orig_phase_snap polls it at every phase
                // point and reports the phase whose window changed the byte
                // (env V2_A2_WP=1; perf HW breakpoints are not permitted here).
                static int wp_armed = -1;
                if (wp_armed == -1) wp_armed = getenv("V2_A2_WP") ? 0 : 2;
                // Direct map-word watch (env V2_A2_WP_MOFF): rearm every
                // snapshot so scene/segment changes keep the pointer fresh.
                if (wp_armed != 2) {
                    if (const char* e2 = getenv("V2_A2_WP_MOFF")) {
                        extern uint8_t* v2_a2_softwp_fs_ptr;
                        extern uint8_t* v2_m2c_base;
                        uint16_t fsseg2 = *(const uint16_t*)(dsb + DS_SEG_FS);
                        uint16_t moff2 = (uint16_t)strtol(e2, 0, 0);
                        if (fsseg2 && v2_m2c_base) {
                            v2_a2_softwp_fs_ptr = v2_m2c_base + (uint32_t)fsseg2 * 16 + moff2;
                            v2_a2_softwp_fs_moff = moff2;
                        }
                    }
                }
                uint16_t ofl = *(const uint16_t*)(dsb + (uint16_t)(tdi + OBJ_SPRITE_FLAGS));
                if (wp_armed == 0 && (ofl & 0x8000) && oy > 0) {
                    int16_t ox = *(const int16_t*)(dsb + (uint16_t)(tdi + OBJ_SPRITE_X));
                    int y = oy + 8, x = ox + 8;
                    uint16_t yh = *(const uint16_t*)(dsb + (uint16_t)(LUT_PAGE_ROW + 0x34 + (uint16_t)((y >> 3) * 2)));
                    uint32_t base = ((uint32_t)yh + (uint32_t)(y & 7) * 0x56u + 8u) * 4u + (uint32_t)x;
                    if (base < sizeof(myDrawInfo->drawBuffer)) {
                        extern uint8_t* v2_a2_softwp_ptr;
                        v2_a2_softwp_ptr = myDrawInfo->drawBuffer + base;
                        fprintf(stderr, "A2WP-ARM(soft): base=0x%X x=%d y=%d\n", base, x, y);
                        wp_armed = 1;
                    }
                    // Also arm a soft watch on the REAL FS render-map word of
                    // the letter cell — real-vs-shadow bit dynamics comparison.
                    {
                        extern uint8_t* v2_a2_softwp_fs_ptr;
                        extern uint8_t* v2_m2c_base;
                        uint16_t fsseg = *(const uint16_t*)(dsb + DS_SEG_FS);
                        uint16_t row = (uint16_t)(y >> 3);
                        uint16_t rb = *(const uint16_t*)(dsb + (uint16_t)(row * 2 - 0x7098));
                        uint16_t moff = (uint16_t)((rb + (uint16_t)(x >> 3)) * 2u);
                        // Direct override: env V2_A2_WP_MOFF=0xNNNN watches
                        // that exact map word instead of the derived cell.
                        if (const char* e = getenv("V2_A2_WP_MOFF"))
                            moff = (uint16_t)strtol(e, 0, 0);
                        if (fsseg && v2_m2c_base) {
                            v2_a2_softwp_fs_ptr = v2_m2c_base + (uint32_t)fsseg * 16 + moff;
                            v2_a2_softwp_fs_moff = moff;
                            fprintf(stderr, "A2WP-ARM(fs): moff=%04X\n", moff);
                        }
                    }
                }
            }
        }
    }
    for (uint32_t y = 0; y < 176; y++) {
        uint32_t base = (crtc_offset + y * 0x56u) * 4u + pixel_pan;
        if (base + 320 > sizeof(myDrawInfo->drawBuffer)) { v2_a2_page_snap_valid = 0; return; }
        memcpy(v2_a2_page_snap + y * 320, myDrawInfo->drawBuffer + base, 320);
    }
    v2_a2_page_snap_valid = 1;
}
extern "C" int v2_fetch_orig_page(uint8_t* out, uint32_t count) {
    if (!myDrawInfo) return 0;
    uint32_t rows = count / 320;
    if (rows * 320 != count) return 0;
    // Prefer the phase-synchronized snapshot; fall back to a live unfold
    // (crash dumps / callers outside the frame loop).
    if (v2_a2_page_snap_valid && count <= sizeof(v2_a2_page_snap)) {
        memcpy(out, v2_a2_page_snap, count);
        return 1;
    }
    for (uint32_t y = 0; y < rows; y++) {
        uint32_t base = (myDrawInfo->myOffset + y * 0x56u) * 4u + myDrawInfo->myPixelOffset;
        if (base + 320 > sizeof(myDrawInfo->drawBuffer)) return 0;
        memcpy(out + y * 320, myDrawInfo->drawBuffer + base, 320);
    }
    return 1;
}
// Class-C palette probe: snapshot the orig-DAC shadow (drawPalette, kept in
// sync by the setPalette mirrors at every OUT 3C8/3C9 site) as flat RGB.
extern "C" void v2_fetch_orig_dac(uint8_t* rgb768) {
    if (!myDrawInfo) { memset(rgb768, 0, 768); return; }
    for (int i = 0; i < 256; i++) {
        rgb768[i*3 + 0] = myDrawInfo->drawPalette[i].r;
        rgb768[i*3 + 1] = myDrawInfo->drawPalette[i].g;
        rgb768[i*3 + 2] = myDrawInfo->drawPalette[i].b;
    }
}
SDL_Window* myWindow = NULL;
SDL_Renderer* myRenderer = NULL;
SDL_Texture* myTexture = NULL;
SDL_PixelFormat *myFormat = NULL;

extern void render_callback(void *);
extern uint16_t input_keys_v2;
uint16_t input_keys = 0;
bool need_quit = false;

// Phase 1: mirror orig DOS INT 9 ISR semantics — render thread updates
// word_30bbe (ds:0x86DE) DIRECTLY on KEYDOWN/KEYUP via atomic OR/AND.
// orig sub_12352 line `OR ax, word_30bbe` reads it natively — same memory
// path as DOS ISR-updated word_30bbe.
//
// Shadow DS also updated atomically. Both stay in lockstep.
// 0x86DE is excluded from v2 verify (input layer, not VM-managed state;
// orig snapshot vs live shadow can diverge by render thread timing — this
// is hardware-async input, conceptually analogous to BIOS keyboard buffer).
extern "C" void v2_shadow_input_or(uint16_t bit);
extern "C" void v2_mirror_int9_char(uint8_t dos_scan);  // task #37
extern "C" void v2_shadow_input_and_not(uint16_t bit);
extern "C" uint16_t v2_shadow_word_288ac();
#ifndef V2_ONLY
extern uint16_t& word_30bbe;
static inline void m2c_input_or(uint16_t bit) {
    if (!bit) return;
    __atomic_or_fetch(&word_30bbe, bit, __ATOMIC_RELAXED);
    v2_shadow_input_or(bit);
}
static inline void m2c_input_and_not(uint16_t bit) {
    if (!bit) return;
    __atomic_and_fetch(&word_30bbe, (uint16_t)~bit, __ATOMIC_RELAXED);
    v2_shadow_input_and_not(bit);
}
#else
static inline void m2c_input_or(uint16_t bit) { v2_shadow_input_or(bit); }
static inline void m2c_input_and_not(uint16_t bit) { v2_shadow_input_and_not(bit); }
#endif
std::atomic<bool> g_dump_pgm_request{false};  // set by F12 → both render threads dump

// Save 320x176 viewport as PGM file. Includes palette as PAM if available.
static void save_pgm(const char* path, const uint8_t* buf_320x176_or_344_stride,
                     int stride, const SDL_Color* palette /*nullable*/) {
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "PGM-DUMP: cannot open %s\n", path); return; }
    if (palette) {
        // PPM (binary RGB) so colors are visible
        fprintf(f, "P6\n320 176\n255\n");
        for (int y = 0; y < 176; y++) {
            for (int x = 0; x < 320; x++) {
                uint8_t c = buf_320x176_or_344_stride[y * stride + x];
                fputc(palette[c].r, f);
                fputc(palette[c].g, f);
                fputc(palette[c].b, f);
            }
        }
    } else {
        // PGM grayscale = raw color indices
        fprintf(f, "P5\n320 176\n255\n");
        for (int y = 0; y < 176; y++)
            fwrite(buf_320x176_or_344_stride + y * stride, 1, 320, f);
    }
    fclose(f);
    fprintf(stderr, "PGM-DUMP: saved %s (%s)\n", path, palette ? "PPM with palette" : "PGM raw");
}

// Cherry-pick 3f2114a: gate updateDraw() on word_3287c (VSYNC counter).
// Game thread DECs word_3287c in render_callback (via sub_1797b). When > 0,
// game has finished a frame and is waiting in sub_10130 for next render tick.
// Reading drawBuffer ONLY then prevents race vs game thread writing
// (graphics artefacts on door/dialog).
//
// V2_ONLY: word_3287c is in seg000/m2c which isn't linked. orig window is
// hidden anyway (V2 renders to its own window via render_v2.cpp), so the
// gate is moot — always update.
#ifndef V2_ONLY
typedef uint16_t dw;
#ifndef V2_ONLY
extern dw& word_3287c;
#endif
#endif

// SDL spec-key state (replaces orig int 9 ISR's writes to byte_31669 etc).
// Game logic ORs sdl_spec_get(off) at byte_316xx CMP/TEST sites.
//
// Two key roles need different semantics to mirror orig DOS keyboard ISR:
//
// MODIFIERS (ALT, CTRL): sticky — KEYDOWN sets atomic=1, KEYUP sets =0.
// Snapshot just copies. Stays 1 the whole time the key is held, so a trigger
// pressed later in the same hold combines correctly (e.g. ALT held, then S
// pressed → both register simultaneously in same frame).
//
// TRIGGERS (S, M, X, F10, DEL): edge — KEYDOWN sets atomic=1; snapshot
// consumes via exchange(0). Each press produces exactly one game-side
// toggle, even if user holds the key (no auto-repeat-driven retoggle).
//
// Two-stage atomic→snap to avoid orig/v2 desync within a frame: render
// thread updates atomic; v2_phase_frame_begin snapshots once per frame.
std::atomic<uint8_t> sdl_spec_state[256] = {};
static uint8_t sdl_spec_snap[256] = {};

// Latch: catches brief KEYDOWN+KEYUP between frame_begins. KEYDOWN sets latch=1.
// snap_take captures (state OR latch) then clears latch. snap stays = 1 for at
// least one full frame, allowing orig+v2 (race-free at single snap) to detect
// brief tap that state alone would miss.
std::atomic<uint8_t> sdl_spec_press_latch[256] = {};

// Edge accumulator: catches brief KEYDOWN+KEYUP within one render iter (race
// where input_keys cleared before game polls). KEYDOWN OR's bit, snap_take
// exchanges to snap. snap_get returns snap (used by v2_input_intro_mask for
// V2_ONLY paths and as backup for transient presses).
std::atomic<uint16_t> sdl_input_press_edges{0};
// Task #37b: INT9 letter channel per-frame snap. KEYDOWN stores the DOS scan
// here (bit8 = valid, last press wins = DOS typematic ISR overwrites
// word_2876C); the game thread drains it at frame begin so BOTH DS copies
// (real + shadow) receive the char at the same logical point - the immediate
// render-thread write raced the split VM order (v2 runs BEFORE orig: a char
// landing between them leaked into ds:0x34 on the password screen, #37 crash
// report). With this snap the {0x28C} verify skip is REMOVED (stricter).
std::atomic<uint16_t> sdl_int9_char_pending{0};
// Non-static so sub_12352 (seg000.cpp) and v2_read_input_12352_iter (v2_vm.cpp) can
// OR into it directly during per-call drain — see LAYER 1 comments in both.
uint16_t sdl_input_press_snap = 0;
uint16_t sdl_input_press_snap_get() { return sdl_input_press_snap; }

// SDL INT-9 ISR mirror: tracks which press_snap bits have already had their
// "force edge" applied this frame. Reset at frame_begin (sdl_spec_snapshot_take).
// Used by sub_12352 to clear matching word_2889a bits on the first call of
// the frame so that brief KEYDOWN+KEYUP (which lands between main sub_12352
// calls due to 9 FPS frame duration > 80ms typical hold) still produces an
// edge in word_28898. Without this, sub_1086f recursion's sub_12352 sets
// word_2889a sticky → next frame's main sub_12352 computes edge=0 (false-negative).
//
// Two separate flags: orig runs in game thread and consumes via seg000 sub_12352.
// v2 mirror runs in v2 thread (default mode) or game thread (V2_ONLY) and
// consumes via v2_read_input_12352_iter. Each side must clear ITS OWN word_2889a copy.
// Sharing one flag would cause v2 mirror to skip its shadow clear (orig already
// consumed) → DS-DIFF at 0x03B8/9.
uint16_t g_press_snap_consumed_this_frame = 0;        // orig side (seg000 sub_12352)
uint16_t g_press_snap_consumed_shadow_this_frame = 0; // v2 side (v2_read_input_12352_iter)

// is-first-sub12352 flag: only the FIRST sub_12352 of a frame (main sub_12352
// at eip 0x001E) should consume snap bits via LAYER 2 (and mark them for snap
// clear at next frame_begin). Recursion sub_12352 (inside sub_1086f cmd
// dispatch, sub_104a1 quit-prompt loop, sub_10138 viking-switch loop) still
// drains edges via LAYER 1 (for word_2889a clear) but skips LAYER 2 so the
// snap bit persists into NEXT frame's main sub_12352 — that's where VM iter
// will pick it up. Without this, recursion's LAYER 2 fire would consume snap
// and next frame's main would get edge=0 → VM iter misses press.
// Reset to true at frame_begin (sdl_spec_snapshot_take), set to false at end
// of each sub_12352 call.
bool g_is_first_sub12352_orig = true;
bool g_is_first_sub12352_shadow = true;

// Per-sub_12352-call edges drain → captures KEYDOWN events that arrived since
// last sub_12352 call. Needed for blocking loops (sub_104a1 quit-prompt,
// sub_10138 viking-switch wait, sub_104A1 transition-text) where many
// sub_12352 calls fire within a single main game frame without intervening
// FRAME_BEGIN to refresh press_snap. orig drains in seg000 sub_12352, stashes
// the value here, and v2_read_input_12352_iter reads it (default mode) or drains
// itself (V2_ONLY). Atomically updated by game thread; v2 thread reads after
// INPUT_UPDATE signal-handler barrier.
uint16_t g_last_sub12352_new_keydowns = 0;

// ENTER missed-press investigation: arm a trace on every KEYDOWN sym=13 so
// sub_12352 logs its inputs and computed edge for the next N calls. The trace
// captures whether word_30bbe sees the 0x8000 bit and whether word_28898 ever
// has 0x8000 set after the press. Compare succeeded (#6) vs missed (#9) to
// determine if it's input race (bit never reached sub_12352) or VM consume
// race (edge computed but not read by VM at PC=86D0).
static auto _enter_trace_t0 = std::chrono::steady_clock::now();
static uint32_t enter_trace_ms_now() {
    using namespace std::chrono;
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now() - _enter_trace_t0).count();
}
std::atomic<int>      g_enter_trace_arm{0};   // sub_12352 calls remaining to trace
std::atomic<int>      g_enter_seq{0};         // total Enter KEYDOWNs seen
std::atomic<uint32_t> g_enter_keydown_ms{0};  // ms of last KEYDOWN Enter

#ifndef V2_ONLY
extern uint16_t& word_30bbe;
extern uint16_t& word_28896;
extern uint16_t& word_28898;
extern uint16_t& word_2889a;
extern uint16_t& word_30bba;
extern uint16_t& word_30bbc;
extern uint16_t& word_288ac;
extern uint16_t& word_2a66f;
extern uint16_t& word_2b044;
extern uint16_t& word_287e2;
#endif
extern int       v2_dbg_pre_vm_iter;

// Called from sub_12352 right after word_28898 / word_2889a are set.
extern "C" void enter_trace_sub12352() {
#ifndef V2_ONLY
    int armed = g_enter_trace_arm.load(std::memory_order_relaxed);
    bool any_enter_bit = ((word_30bbe | word_28896 | word_28898 | word_2889a | input_keys | sdl_input_press_snap) & 0x8000) != 0;
    if (armed <= 0 && !any_enter_bit) return;
    int seq = g_enter_seq.load(std::memory_order_relaxed);
    uint32_t ms = enter_trace_ms_now();
    uint32_t dkd = ms - g_enter_keydown_ms.load(std::memory_order_relaxed);
    fprintf(stderr,
            "ENTER-TRACE-SUB12352 seq=%d arm=%d t=%ums dkd=%ums "
            "w30bbe=%04X w28896=%04X w28898=%04X w2889a=%04X input_keys=%04X "
            "press_snap=%04X w30bba=%04X w30bbc=%04X w288ac=%04X "
            "queue: rd=%04X wr=%04X | f=%d w287e2=%04X\n",
            seq, armed, ms, dkd,
            word_30bbe, word_28896, word_28898, word_2889a, input_keys,
            sdl_input_press_snap, word_30bba, word_30bbc, word_288ac,
            word_2b044, word_2a66f, v2_dbg_pre_vm_iter, word_287e2);
    if (armed > 0) g_enter_trace_arm.fetch_sub(1, std::memory_order_relaxed);
#endif
}

static bool sdl_spec_is_modifier(uint16_t off) {
    return off == DS_KEY_ALT /* ALT */ || off == 0x9189 /* CTRL */;
}

uint8_t sdl_spec_get(uint16_t off) {
    // Read frame-frozen snap so orig+v2 mirror see SAME value (race-free).
    // snap_take fires at frame_begin AND on first sub_12352 of each pause/dialog
    // iter (see seg000 sub_12352 entry — added for press timing).
    return sdl_spec_snap[off & 0xFF];
}

// Direct held-state read (mirrors orig INT9 ISR's byte_316XX semantics:
// byte=1 between press and release scancodes). Use INSTEAD of sdl_spec_get
// when caller needs press-during-blocking-loop preservation — sdl_spec_get
// reads `snap` which is exchange-to-0 at each frame_begin (consumed even
// when caller doesn't read), so presses occurring during dialog/pause loops
// are lost. State persists until KEYUP clears it.
uint8_t sdl_spec_state_get(uint16_t off) {
    return sdl_spec_state[off & 0xFF].load(std::memory_order_relaxed);
}

// F5/F6 level cheat — handled by orig's existing eip 0x106 check (which reads
// sdl_spec_snap via the same path as F4/F10/etc.) and v2's mirror in
// v2_phase_frame_end. No extra wiring needed — both already work like orig DOS.

// Refresh ONLY sdl_spec_snap (per-scancode held state). DEPRECATED — caused
// orig+v2 race when state changes between orig main thread read and v2 thread
// read of same snap. Kept as no-op to avoid breaking callers.
void sdl_spec_snapshot_refresh_only() {
    // No-op: snap_take at frame_begin only (race-free).
}

void sdl_spec_snapshot_take() {
    for (int i = 0; i < 256; i++) {
        // snap = state OR press_latch. Latch catches brief KEYDOWN+KEYUP
        // between frame_begins (sub-frame taps that state alone misses).
        // Latch consumed (cleared) after capture so press is visible exactly
        // one frame, not multiple — matches orig DOS INT9 single-press semantic.
        uint8_t latch = sdl_spec_press_latch[i].exchange(0, std::memory_order_relaxed);
        sdl_spec_snap[i] = (uint8_t)(sdl_spec_state[i].load(std::memory_order_relaxed) | latch);
    }
    // Snap update with immediate consume clear:
    //   1. Clear bits that fired as edge in main sub_12352 of the just-finished
    //      frame (consumed_this_frame is set ONLY by main sub_12352, not by
    //      recursion — see g_is_first_sub12352_* gating in LAYER 2).
    //   2. Reset consumed trackers + is-first flags for the new frame.
    //   3. OR-accumulate any new KEYDOWNs since last drain.
    sdl_input_press_snap &= (uint16_t)~(g_press_snap_consumed_this_frame | g_press_snap_consumed_shadow_this_frame);
    g_press_snap_consumed_this_frame        = 0;
    g_press_snap_consumed_shadow_this_frame = 0;
    g_is_first_sub12352_orig                = true;
    g_is_first_sub12352_shadow              = true;
    sdl_input_press_snap |= sdl_input_press_edges.exchange(0, std::memory_order_relaxed);
    // #37b: drain the INT9 letter channel - one char per frame boundary,
    // written to BOTH real and shadow DS by v2_mirror_int9_char.
    {
        uint16_t pend = sdl_int9_char_pending.exchange(0, std::memory_order_relaxed);
        if (pend & 0x100) v2_mirror_int9_char((uint8_t)(pend & 0xFF));
    }
}




unsigned int plane4_to_linear(unsigned int plane, unsigned int offset)
{
  return offset * 4 + plane;
}
uint32_t planar_to_linear(uint32_t x, uint32_t y)
{
  return (y * RENDER_WIDTH + x);
}
/*void drawPixel(uint32_t offset, uint8_t color)
{
  myDrawInfo->drawBuffer[offset] = color;
}
void drawPixel(uint32_t x, uint32_t y, uint8_t color)
{
  int offset = planar_to_linear(x, y);

  myDrawInfo->drawBuffer[offset] = color;
}
void setPalette(uint8_t color, uint8_t r, uint8_t g, uint8_t b)
{
  myDrawInfo->drawPalette[color] = {r, g, b, 255};
}*/
void updateDraw()
 {
   auto offset = myDrawInfo->myOffset * 4 + myDrawInfo->myPixelOffset;
   // Cherry-pick 3f2114a: VGA memory wraparound — drawBuffer is 65536*4 bytes,
   // offset+i can exceed it during page-flip transitions. Bounds-safe access
   // prevents reading past buffer (graphics artefacts).
   constexpr int VGA_MEM_SIZE = 65536 * 4;
  // Periodic dump of original viewport content
  {
    static int frame_counter = 0;
    frame_counter++;
    if (frame_counter % 66 == 0 && frame_counter <= 66*30) {
      char fname[64];
      snprintf(fname, sizeof(fname), "/tmp/orig_viewport_f%d.pgm", frame_counter);
      FILE* f = fopen(fname, "wb");
      if (f) {
        fprintf(f, "P5\n320 176\n255\n");
        // Extract 320x176 from planar drawBuffer — CRTC unfold, pitch 0x56
        // bytes/row (see v2_fetch_orig_page comment, task #19).
        for (int y = 0; y < 176; y++) {
          uint32_t row = ((myDrawInfo->myOffset + y * 0x56u) * 4u + myDrawInfo->myPixelOffset);
          for (int x = 0; x < 320; x++) {
            uint8_t c = myDrawInfo->drawBuffer[(row + x) % VGA_MEM_SIZE];
            fputc(c, f);
          }
        }
        fclose(f);
        printf("V2-DBG-ORIG: Saved viewport dump to %s (offset=%x)\n", fname, offset);
      }
    }
  }
  // Viewport rows 0..175: CRT scans from myOffset with CRTC offset reg 0x13 =
  // 0x2B words = 86 bytes/row; drawBuffer is VGA memory ×4 (addr*4+plane), so
  // one screen row = 344 buffer bytes (320 visible + 24 overscan slack).
  // Old linear read (offset + i) sheared 86-pitch content 24 px/row (task #19).
  for (int y = 0; y < 176; y++)
  {
	//myDrawInfo->myOffset=0x5be8;
	//myDrawInfo->myOffset=0xa1c8;
	//myDrawInfo->myOffset=0x66a8;
	//myDrawInfo->myOffset=0;
	uint32_t row = (myDrawInfo->myOffset + y * 0x56u) * 4u + myDrawInfo->myPixelOffset;
	for (int x = 0; x < RENDER_WIDTH; x++)
	{
	  auto color = myDrawInfo->drawBuffer[(row + x) % VGA_MEM_SIZE];
	  auto sdl_color = myDrawInfo->drawPalette[color];
	  tempDrawBuffer[y * RENDER_WIDTH + x] = SDL_MapRGBA(myFormat, sdl_color.r, sdl_color.g, sdl_color.b, sdl_color.a);
	}
  }
  // HUD rows 176..199: VGA split screen (CRTC line compare 0x15F → scan row
  // 176) restarts scanning at address 0, same 86-byte row pitch, no pixel pan.
  for (int y = 0; y < RENDER_HEIGHT - 176; y++)
  {
	uint32_t row = (0u + y * 0x56u) * 4u;
	for (int x = 0; x < RENDER_WIDTH; x++)
	{
	  auto color = myDrawInfo->drawBuffer[(row + x) % VGA_MEM_SIZE];
	  auto sdl_color = myDrawInfo->drawPalette[color];
	  tempDrawBuffer[(176 + y) * RENDER_WIDTH + x] = SDL_MapRGBA(myFormat, sdl_color.r, sdl_color.g, sdl_color.b, sdl_color.a);
	}
  }
  SDL_UpdateTexture(myTexture, NULL, tempDrawBuffer, RENDER_WIDTH*sizeof(uint32_t));
  SDL_RenderClear(myRenderer);
  SDL_Rect srcRect = {0, 0, SCREEN_WIDTH, SCREEN_HEIGHT};
  SDL_RenderCopy(myRenderer, myTexture, &srcRect, NULL);
  SDL_RenderPresent(myRenderer);
}

  std::thread render_thread;
  void render_thread_proc(void* _state)
  {
	// myDrawInfo is allocated in render_init() on the game thread BEFORE this
	// detached thread starts (#179) — it was calloc'd here, which raced the game
	// thread's first drawPixel under heavy parallel startup → NULL deref @ f0.
	assert(myDrawInfo);

	printf("render: Starting initialization...\n");
	if( SDL_Init( SDL_INIT_VIDEO ) < 0 )
    {
	  printf( "SDL could not initialize! SDL_Error: %s\n", SDL_GetError() );
    }
	printf("render: Creating window...\n");
#ifdef V2_ONLY
	// V2_ONLY: hide orig window — orig m2c renders to myDrawInfo->drawBuffer for v2's
	// drawBuffer mirror but orig window itself is unused.
	uint32_t _window_flags = SDL_WINDOW_HIDDEN;
#else
	uint32_t _window_flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
#endif
	// Nearest-neighbor scaling so pixel art stays crisp at any non-integer
	// scale. SDL's default is already "0" (nearest); pin it explicitly.
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
	// Позиционируем в левый верхний угол
	myWindow = SDL_CreateWindow( "FFFF", 0, 0, SCREEN_WIDTH * SCREEN_SCALE, SCREEN_HEIGHT * SCREEN_SCALE, _window_flags );
		if( myWindow == NULL )
		{
			printf( "Window could not be created! SDL_Error: %s\n", SDL_GetError() );
		}
		else
		{
		  printf("render: Window created successfully!\n");
		  printf("render: Creating renderer...\n");
		  //struct m2c::_STATE state;
		  //struct m2c::_STATE *_state = &state;
			  //    X86_REGREF

			myRenderer = SDL_CreateRenderer(myWindow, -1, SDL_RENDERER_ACCELERATED);
			// Keep 4:3 aspect at any window size — SDL letterboxes when needed.
			SDL_RenderSetLogicalSize(myRenderer, SCREEN_WIDTH, SCREEN_HEIGHT);

			myTexture = SDL_CreateTexture(myRenderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STREAMING, RENDER_WIDTH, RENDER_HEIGHT);

			myFormat = SDL_AllocFormat(SDL_PIXELFORMAT_RGBA8888);

		   printf("render: Entering main loop...\n");
		   while (!need_quit)
			{
			   SDL_Event event;
			   while (v2_input_poll_event(&event) > 0) {
				 // Per-event scope: declared INSIDE while so they reset on each
				 // event. Previously key_val leaked from previous keydown, so
				 // pressing a spec-only key (Q/R/Y/A/N/M/X/ALT/F10/DEL/F4-F6/1-3)
				 // after a movement key would re-OR the leftover bit into input_keys.
				 uint16_t key_val = 0;
				 uint16_t spec_off = 0;
				 switch (event.type) {
				 case SDL_KEYDOWN:
				 case SDL_KEYUP:
				   // F11 = toggle desktop fullscreen on this window (not a
				   // game input, handled before keymap switch).
				   if (event.type == SDL_KEYDOWN && !event.key.repeat &&
				       event.key.keysym.sym == SDLK_F11) {
				       Uint32 wf = SDL_GetWindowFlags(myWindow);
				       SDL_SetWindowFullscreen(myWindow,
				           (wf & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
				       break;
				   }
				   switch( event.key.keysym.sym ){
					 case SDLK_LEFT:
					   key_val = 0x200;
					   break;
					 case SDLK_RIGHT:
					   key_val = 0x100;
					   break;
					 case SDLK_UP:
					   key_val = 0x800;
					   break;
					 case SDLK_DOWN:
					   key_val = 0x400;
					   break;
					 case SDLK_SPACE:
					 case SDLK_RETURN:
					   key_val = 0x8000;
					   break;
				     case SDLK_LCTRL:
					 case SDLK_RCTRL:
					   key_val = 0x20;
					   spec_off = 0x9189;  // sc 0x1D byte_31669
					   break;
					 case SDLK_TAB:
					   key_val = 0x2000;
					   break;
					 case SDLK_e:
					   key_val = 0x40;
					   break;
					 case SDLK_s:
					   key_val = 0x80;
					   spec_off = 0x918B;  // sc 0x1F byte_3166b — mute SFX
					   break;
					 case SDLK_d:
					   key_val = 0x4000;
					   break;
					 case SDLK_f:
					   key_val = 0x8000;
					   break;
					 case SDLK_ESCAPE:
					   key_val = 0x1000;
					   break;
					 case SDLK_m:
					   spec_off = 0x919E;  // sc 0x32 byte_3167e — mute music
					   break;
					 case SDLK_x:
					   spec_off = DS_KEY_X;  // sc 0x2D byte_31679
					   break;
					 case SDLK_LALT:
					 case SDLK_RALT:
					   spec_off = DS_KEY_ALT;  // sc 0x38 byte_31684
					   break;
					 case SDLK_F10:
					   spec_off = DS_KEY_F10;  // sc 0x44 byte_31690 — restart level
					   break;
					 case SDLK_DELETE:
					   spec_off = 0x91BF;  // sc 0x53 byte_3169f — CTRL+ALT+DEL combo
					   break;
					 case SDLK_q:
					   spec_off = DS_KEY_Q;  // sc 0x10 byte_3165C — ALT+Q restart level
					   break;
					 case SDLK_r:
					   spec_off = 0x917F;  // sc 0x13 byte_3165F — eip 0x2901 read
					   break;
					 case SDLK_y:
					   spec_off = 0x9181;  // sc 0x15 byte_31661 — yes/no prompt YES
					   break;
					 case SDLK_a:
					   spec_off = 0x918A;  // sc 0x1E byte_3166A — eip 0x2905 read
					   break;
					 case SDLK_n:
					   spec_off = 0x919D;  // sc 0x31 byte_3167D — yes/no prompt NO
					   break;
					 case SDLK_F4:
					   spec_off = 0x91AA;  // sc 0x3E byte_3168A — INT 3 debug trap
					   break;
					 case SDLK_F5:
					   spec_off = 0x91AB;  // sc 0x3F byte_3168B — prev level cheat
					   break;
					 case SDLK_F6:
					   spec_off = 0x91AC;  // sc 0x40 byte_3168C — next level cheat
					   break;
					 case SDLK_1:
					   spec_off = 0x916E;  // sc 0x02 — viking 1 select (orig has no reader; infra ready)
					   break;
					 case SDLK_2:
					   spec_off = 0x916F;  // sc 0x03 — viking 2 select
					   break;
					 case SDLK_3:
					   spec_off = 0x9170;  // sc 0x04 — viking 3 select
					   break;
					 case SDLK_F12:
					   if (event.type == SDL_KEYDOWN && event.key.repeat == 0)
						 g_dump_pgm_request.store(true, std::memory_order_release);
					   break;
				     default:
					   key_val = 0;
					   break;
				   }
				   if (event.type == SDL_KEYDOWN) {
					 // Task #37: mirror orig INT9's word_2876C = LUT[scancode] for EVERY
					 // press (typematic repeats included — DOS resent make codes). The
					 // SDL→set-1 map below is the PC keyboard spec (the same codes the
					 // game's own LUT at ds:[-0x7198] is indexed by); the CHARACTER
					 // values come from that original LUT, not from here.
					 {
					   static const struct { SDL_Scancode s; uint8_t dos; } k2dos[] = {
					     {SDL_SCANCODE_ESCAPE,0x01},{SDL_SCANCODE_1,0x02},{SDL_SCANCODE_2,0x03},
					     {SDL_SCANCODE_3,0x04},{SDL_SCANCODE_4,0x05},{SDL_SCANCODE_5,0x06},
					     {SDL_SCANCODE_6,0x07},{SDL_SCANCODE_7,0x08},{SDL_SCANCODE_8,0x09},
					     {SDL_SCANCODE_9,0x0A},{SDL_SCANCODE_0,0x0B},{SDL_SCANCODE_MINUS,0x0C},
					     {SDL_SCANCODE_EQUALS,0x0D},{SDL_SCANCODE_BACKSPACE,0x0E},
					     {SDL_SCANCODE_TAB,0x0F},{SDL_SCANCODE_Q,0x10},{SDL_SCANCODE_W,0x11},
					     {SDL_SCANCODE_E,0x12},{SDL_SCANCODE_R,0x13},{SDL_SCANCODE_T,0x14},
					     {SDL_SCANCODE_Y,0x15},{SDL_SCANCODE_U,0x16},{SDL_SCANCODE_I,0x17},
					     {SDL_SCANCODE_O,0x18},{SDL_SCANCODE_P,0x19},{SDL_SCANCODE_LEFTBRACKET,0x1A},
					     {SDL_SCANCODE_RIGHTBRACKET,0x1B},{SDL_SCANCODE_RETURN,0x1C},
					     {SDL_SCANCODE_A,0x1E},{SDL_SCANCODE_S,0x1F},{SDL_SCANCODE_D,0x20},
					     {SDL_SCANCODE_F,0x21},{SDL_SCANCODE_G,0x22},{SDL_SCANCODE_H,0x23},
					     {SDL_SCANCODE_J,0x24},{SDL_SCANCODE_K,0x25},{SDL_SCANCODE_L,0x26},
					     {SDL_SCANCODE_SEMICOLON,0x27},{SDL_SCANCODE_APOSTROPHE,0x28},
					     {SDL_SCANCODE_GRAVE,0x29},{SDL_SCANCODE_BACKSLASH,0x2B},
					     {SDL_SCANCODE_Z,0x2C},{SDL_SCANCODE_X,0x2D},{SDL_SCANCODE_C,0x2E},
					     {SDL_SCANCODE_V,0x2F},{SDL_SCANCODE_B,0x30},{SDL_SCANCODE_N,0x31},
					     {SDL_SCANCODE_M,0x32},{SDL_SCANCODE_COMMA,0x33},
					     {SDL_SCANCODE_PERIOD,0x34},{SDL_SCANCODE_SLASH,0x35},
					     {SDL_SCANCODE_SPACE,0x39},
					   };
					   for (const auto& m : k2dos)
					     if (m.s == event.key.keysym.scancode) {
					       sdl_int9_char_pending.store((uint16_t)(0x100 | m.dos), std::memory_order_relaxed); // #37b: drained at frame begin
					       break;
					     }
					 }
					 input_keys |= key_val;
					 input_keys_v2 |= key_val;
					 // Edge accumulator: catches brief KEYDOWN+KEYUP-same-iter race.
					 // Filter !repeat: only initial press fires the force-edge clear in
					 // sub_12352. Without this, SDL auto-repeat fires KEYDOWN every ~50ms
					 // with repeat=1 → edges OR'd every repeat → press_snap=bit every frame
					 // → sub_12352 clears word_2889a every frame → held key retriggers
					 // edge every frame (scroll explosion). Held keys are tracked via
					 // word_30bbe/input_keys (stays set until KEYUP) — edge fires once on
					 // first press, word_2889a tracks across frames, no re-trigger.
					 // Mirror orig INT9 ISR (seg000_6440_proc eip 0x6458): if word_288AC
					 // == 0x8000 (intro mode), KEYDOWN dispatches via loc_164CA. Non-special
					 // keys hit default → MOV word_30bbe, 0xFFFF (intro skip signal).
					 // Special keys (CTRL/ALT/DEL/F10/X/S/M) set their state byte only,
					 // NO word_30bbe write. Normal mode: OR key_val into word_30bbe.
					 uint16_t cur_288ac;
#ifndef V2_ONLY
					 extern uint16_t& word_288ac;
					 cur_288ac = word_288ac;
#else
					 cur_288ac = v2_shadow_word_288ac();
#endif
					 bool intro_mode = (cur_288ac == 0x8000);
					 if (intro_mode) {
						 if (!spec_off && !event.key.repeat) {
							 // Non-special key in intro → orig writes word_30bbe = 0xFFFF.
							 // shadow update mirrors. V2_ONLY: v2_input_or now reads shadow
							 // directly (see v2_vm.cpp), no input_keys overwrite needed.
#ifndef V2_ONLY
							 __atomic_store_n(&word_30bbe, (uint16_t)0xFFFF, __ATOMIC_RELAXED);
#endif
							 v2_shadow_input_or(0xFFFF);
							 sdl_input_press_edges.fetch_or(0xFFFF, std::memory_order_relaxed);
						 }
						 // Special keys: state byte set below in spec_off block. No word_30bbe OR.
					 } else {
						 if (key_val && !event.key.repeat) sdl_input_press_edges.fetch_or(key_val, std::memory_order_relaxed);
						 // Phase 1 (orig ISR mirror): direct atomic write to word_30bbe
						 // (real DS) + shadow DS. sub_12352 reads natively.
						 m2c_input_or(key_val);
					 }
					 // ENTER trace: arm sub_12352 logger for next ~50 calls.
					 if (event.key.keysym.sym == SDLK_RETURN && !event.key.repeat) {
						 int seq = g_enter_seq.fetch_add(1, std::memory_order_relaxed) + 1;
						 uint32_t ms = enter_trace_ms_now();
						 g_enter_keydown_ms.store(ms, std::memory_order_relaxed);
						 g_enter_trace_arm.store(50, std::memory_order_relaxed);
#ifndef V2_ONLY
						 fprintf(stderr,
							 "ENTER-TRACE-KEYDOWN seq=%d t=%ums word_30bbe=%04X (post-OR) "
							 "input_keys=%04X edges=%04X f=%d\n",
							 seq, ms, word_30bbe, input_keys,
							 sdl_input_press_edges.load(std::memory_order_relaxed),
							 v2_dbg_pre_vm_iter);
#endif
					 }
				   } else {
					 input_keys &= ~key_val;
					 input_keys_v2 &= ~key_val;
					 m2c_input_and_not(key_val);
					 if (event.key.keysym.sym == SDLK_RETURN) {
						 uint32_t ms = enter_trace_ms_now();
						 uint32_t dkd = ms - g_enter_keydown_ms.load(std::memory_order_relaxed);
						 int seq = g_enter_seq.load(std::memory_order_relaxed);
#ifndef V2_ONLY
						 fprintf(stderr,
							 "ENTER-TRACE-KEYUP seq=%d t=%ums dkd=%ums word_30bbe=%04X "
							 "(post-AND) input_keys=%04X f=%d\n",
							 seq, ms, dkd, word_30bbe, input_keys, v2_dbg_pre_vm_iter);
#endif
					 }
				   }
				   // Spec key SDL state. Modifiers: KEYDOWN sets, KEYUP clears.
				   // Triggers: KEYDOWN sets only (snapshot consumes via exchange).
				   if (event.type == SDL_KEYDOWN) {
					 fprintf(stderr, "KEY-DBG sym=%d spec_off=0x%04X repeat=%d\n",
					         (int)event.key.keysym.sym, spec_off, (int)event.key.repeat);
				   }
				   if (spec_off) {
					 // EXACT orig INT9 replication: KEYDOWN scancode → byte_316XX=1,
					 // KEYUP (release scancode) → byte_316XX=0, for ALL spec keys
					 // (modifiers + triggers). orig line 14681/14698. Game code clears
					 // byte=0 after consuming action (e.g. sub_108c8 ALT+M toggle);
					 // typematic KEYDOWN repeat will re-set byte=1 → consume cycle
					 // → multi-toggle on hold, matching orig.
					 if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
					   sdl_spec_state[spec_off & 0xFF].store(1, std::memory_order_relaxed);
					   // Latch press so brief tap caught by next snap_take.
					   sdl_spec_press_latch[spec_off & 0xFF].store(1, std::memory_order_relaxed);
					 } else if (event.type == SDL_KEYUP) {
					   sdl_spec_state[spec_off & 0xFF].store(0, std::memory_order_relaxed);
					 }
				   }
				   break;

				 case SDL_QUIT:
				   fprintf(stderr, "SDL_QUIT received — _exit\n");
				   fflush(stderr);
				   _exit(0);
				 case SDL_WINDOWEVENT:
				   if (event.window.event == SDL_WINDOWEVENT_CLOSE) {
					 fprintf(stderr, "SDL_WINDOWEVENT_CLOSE (windowID=%u) — _exit\n", event.window.windowID);
					 fflush(stderr);
					 _exit(0);
				   }
				   break;
			      }
			   }
			   //printf("VGA pan: %x %x\n", myDrawInfo->myOffset, myDrawInfo->myPixelOffset);
			   // F12 PGM dump (orig drawBuffer at current VGA offset)
			   if (g_dump_pgm_request.load(std::memory_order_acquire)) {
				 auto cur_offset = myDrawInfo->myOffset * 4 + myDrawInfo->myPixelOffset;
				 // Note: g_dump_pgm_request cleared by v2 side AFTER its dump (so both dump same frame)
				 // CRTC row pitch = 0x56 VGA bytes = 344 drawBuffer bytes (task #19).
				 save_pgm("/tmp/orig_ladder.ppm",
				          myDrawInfo->drawBuffer + cur_offset,
				          0x56 * 4,
				          myDrawInfo->drawPalette);
			   }
			   // Cherry-pick 3f2114a: only update screen when game has finished a
			   // frame (waits in sub_10130). Prevents race with game thread writes
			   // (door/dialog artifacts). V2_ONLY: gate disabled (no m2c).
#ifndef V2_ONLY
			   if (word_3287c > 0)
#endif
#ifdef HEADLESS
			   { /* task #36: dummy video driver — the ARGB blit+scale burned
			        ~40% CPU presenting to nowhere; skip in headless. */ }
#else
				   updateDraw();
#endif
			   // RESTORED from orig: render_callback (sub_1797b) DECs word_3287C from
			   // render thread at ~60Hz. Without this, game thread sub_10130 sleeps
			   // 16ms each call (3+ per frame) → severe slowdown.
			   { extern std::atomic<int64_t> v2_dbg_render_callback_calls; v2_dbg_render_callback_calls++; }
			   // ============================================================
			   // Render thread no longer modifies DS. Previously called orig
			   // render_callback (sub_1797b m2c) and v2_render_callback to
			   // DEC word_3287c + dispatch palette at ~60Hz, imitating the
			   // original VGA vsync interrupt. But that asynchronously mutated
			   // shadow_ds[0xA39C] and shadow_ds[0x7EFE] outside v2 mirror's
			   // own write paths, racing with game thread snapshots in
			   // trace_compare → forced these bytes into v2_ds_hash_skip.
			   //
			   // New design (see seg000.cpp sub_10130 comment): each game
			   // thread (orig m2c + v2 mirror) does its own DEC + palette
			   // dispatch synchronously inside sub_10130, so per-thread writes
			   // and DECs are 1:1 and snapshots match byte-for-byte. Render
			   // thread is purely a presenter — it reads SDL framebuffer and
			   // calls updateDraw, nothing else.
			   // ============================================================
			   // Increment render-tick counter + notify any waiter (v2 game thread
			   // waits in v2_phase_post_vm until tick advances, ensuring
			   // shadow[0x7EFE] palette flag is cleared before trace_compare).
			   {
			     extern std::atomic<uint64_t> v2_render_tick;
			     extern std::condition_variable v2_render_tick_cv;
			     v2_render_tick.fetch_add(1, std::memory_order_release);
			     v2_render_tick_cv.notify_all();
			   }
#ifdef HEADLESS
			   SDL_Delay(0);   // headless: no display → no vsync pacing, run flat out
#else
			   SDL_Delay(15);
#endif
		   }
		}

  }

void render_init(void* state)
{
    // Allocate myDrawInfo synchronously on the game thread BEFORE spawning the
    // detached render thread. The game thread writes myDrawInfo->drawBuffer via
    // drawPixel from frame 0; previously the render thread calloc'd it async, so
    // under heavy parallel startup (CI fuzz oversubscribing cores) the first
    // drawPixel could run before the thread was scheduled → NULL deref @ f0 (#179).
    if (!myDrawInfo) {
        myDrawInfo = (myDrawInfoS *)calloc(1, sizeof(myDrawInfoS));
        assert(myDrawInfo);
    }
    render_thread = std::thread(render_thread_proc, state);
	render_thread.detach();
}
