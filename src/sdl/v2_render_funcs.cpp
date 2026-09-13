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
#include "v2_gamestate.h"
#include "v2_ds_layout.h"
#include "v2_coop.h"        // UX stage 8 step 2: the player badges of the HUD portraits
extern int v2_dbg_pre_vm_iter;   // game-frame counter (v2_vm.cpp), C++ linkage — declared once at file scope (clang rejects block externs inside extern "C" functions)

// Task #21 obj-trace ring (defined in v2_vm.cpp).
extern "C" void v2_objtrace(const char* tag, int a, int b, int c, int d);
extern "C" int v2_objtrace_di;

// V2 is fully independent of myDrawInfo / orig drawBuffer.
// v2 decodes chunks itself via v2_draw_viewport_chunk (writes to v2_render_buf
// directly, mirroring orig sub_10cd8 → VGA) and v2_draw_hud_background → v2_hud_buf.

#ifdef V2_RENDER_FROM_SHADOW
bool v2_vm_in_frame = false;
bool v2_compose_at_flip = false;   // render_v2.h: the flip's page composition runs outside a frame too (blocking loops)
#endif

// Helper: get DS base pointer for v2 rendering.
// With V2_RENDER_FROM_SHADOW: reads from v2 VM's shadow DS (independent from original).
// Without: reads from real DS (same data as original VM).
// UX stage 9: presenter overrides (see render_v2.h)
thread_local const uint8_t*  v2_tls_ds = nullptr;
thread_local uint8_t*        v2_tls_out = nullptr;
thread_local const uint8_t*  v2_tls_fs = nullptr;
thread_local const uint32_t* v2_tls_par_acc = nullptr;
thread_local bool            v2_tls_presenter = false;
thread_local const uint16_t* v2_tls_tile_ovr = nullptr;        // render_v2.h: the snapshot's page tile words (presenter)
thread_local bool            v2_tls_ui_cells_from_page = false; // render_v2.h: text cells are page commands
thread_local bool            v2_tls_fg_from_page = false;       // render_v2.h: flagged tiles are page commands
const uint16_t*              v2_tile_override = nullptr;        // render_v2.h: the composed page's tile words (game thread)
static void v2_vga_bg_readout(uint8_t* buf, int fbw, int rows, const uint8_t* bg, uint32_t crtc, uint8_t pan, bool par_on);   // below (the background VGA)
static void v2_render_tile_masked(uint8_t* buf, const uint8_t* tgfx_base, const uint8_t* gs_base, uint16_t tile_entry, int screen_x, int screen_y);   // below (the masked tile engine; used by the display list's V2_CMD_FGTILE)
bool v2_last_frame_tiles = false;   // the last v2_draw_tiles took the tile path (not a chunk screen)

static inline uint8_t* v2_get_ds_base(uint16_t ds_val) {
    if (v2_tls_ds) return (uint8_t*)v2_tls_ds;
#ifdef V2_RENDER_FROM_SHADOW
    uint8_t* shadow = v2_vm_get_shadow_ds();
    if (shadow) return shadow;
#endif
    return v2_m2c_base + ((uint32_t)ds_val << 4);
}

// V2 rendering state — definitions (declared extern in render_v2.h)
uint8_t* v2_m2c_base = nullptr;
std::mutex v2_ds_modify_mutex;
uint8_t  v2_render_buf[V2_FB_MAX_W*240];   // UX stage 9: up to 240 rows (224 on an LVX_TALL224 level); step 4: up to V2_FB_MAX_W columns, v2_fbw live
uint8_t  v2_display_buf[V2_FB_MAX_W*240];
std::mutex v2_display_mutex;
uint8_t  v2_hud_buf[320*64];
// UX stage 0 (full-screen LVX scenes): published with v2_display_buf under
// v2_display_mutex; the presenter shows map rows 176..199 instead of the HUD
// band when set. v2_clip_h is the per-frame sprite/pixel clip height of the
// composition buffer: 176 (orig VGA split) or 200 on a full-screen scene.
int v2_display_fullscreen = 0;
extern "C" int v2_scene_fullscreen(void);   // v2_vm.cpp: LVX_FULLSCREEN of ds:0x25AD
static thread_local int v2_clip_h = 176;   // per thread: the presenter's passes set their own
// UX stage 9, step 4: the frame width this thread renders (= the row stride of
// buf and the horizontal clip). The game thread sets it in v2_draw_tiles — 320
// for a chunk screen, v2_view_w for a tile level; the presenter's passes take
// the snapshot's width (v2_smooth.cpp). v2_display_w travels with the published frame.
thread_local int v2_fbw = 320;
int v2_display_w = 320;
static inline int v2_fb_cols() { return v2_fbw / 8 + 3; }   // tile columns per row: 43 at 320 (the orig sub_16ded count)
static thread_local int v2_tile_rows = 25; // tile rows the passes paint: 25 (200-line page) or 29 (224 view)
extern "C" uint32_t v2_fntest_game_ds_linear(void);

// Static intro/menu chunk pixels backup. Orig keeps static chunk pixels in VGA
// across frames, dirty-rect (sub_1de05) erases sprite trails by redrawing tiles
// (which in intro have empty content, effectively reverting to chunk pixels via
// CRTC page persistence). v2 single-buffer architecture lacks page-flip — so we
// save chunk pixels at load time and restore each frame in v2_draw_tiles for
// intro flag levels (= semantic equivalent of "tile redraw + chunk persistence").
// On op_13/0x11 (menu activation) backup is updated to match cleared+moved state.
uint8_t v2_chunk_bg_backup[320*176];
bool v2_chunk_bg_valid = false;
// ============================================================================
// Shadow VGA (address-anchored page channel, byte parity with the real
// myDrawInfo->drawBuffer). The emu 3-page + anchor model approximates the VGA
// with world-anchored pages (branch-3 memmoves content on scroll); the real
// hardware is address-anchored — pixels stay at their byte address, the CRTC
// window and the band renderers move AROUND them. Path 1 replays every orig
// VGA writer into this flat buffer using the orig's own address streams (the
// LUT rows / di values the v2 mirrors already compute), then extracts via the
// sub_16775 formula. Writers are migrated ONE AT A TIME; v2_vga_cov marks the
// bytes v2 already produces so the parity checker compares only those.
// Layout identical to drawBuffer: linear = vga_byte_addr*4 + plane.
// ============================================================================
uint8_t v2_vga[65536 * 4];
uint8_t v2_vga_cov[65536 * 4];   // 1 = written by a migrated v2 writer
static long g_vgaw_trap = -2;   // V2_VGAW_TRAP: the shadow-VGA byte address whose writers are reported (-1 none, -2 unparsed)
static inline void v2_vga_w(uint32_t addr, uint32_t plane, uint8_t val) {
    // Stage 6.2 gated the shadow-VGA writers out of V2_ONLY (no reader there). Since
    // 2026-09-11 the game build has one: the chunk screens (byte_2AAAF & 0x42 — the logos,
    // the title with its CRTC scroll to the logo and the menu over it, the password screen)
    // are presented from this page model (v2_swap_render_buf), exactly like the test build
    // shows every frame; the tile levels are composed from the page lists.
    uint32_t lin = (addr & 0xFFFFu) * 4u + (plane & 3u);
    // diag (env V2_VGAW_TRAP=vgaaddr): backtrace writers of one VGA byte addr.
    // (V2_VGA_PIXTRAP=<x>,<y>,<frame> re-arms it at the flips of that game frame with the
    // address of that screen pixel of the shown page — see v2_vga_fetch_page.)
    {
        long& _t = g_vgaw_trap;
        if (_t == -2) { const char* e = getenv("V2_VGAW_TRAP"); _t = e ? strtol(e, 0, 0) : -1; }
        if (_t >= 0 && (addr & 0xFFFFu) == (uint32_t)_t) {
            static int _n = 0;
            if (_n < 12) { _n++;
                fprintf(stderr, "VGAW-TRAP f%d addr=%04X pl=%u val=%02X ra=%p %p %p\n",
                        v2_dbg_pre_vm_iter, addr & 0xFFFF, plane & 3, val,
                        __builtin_return_address(0), __builtin_return_address(1), __builtin_return_address(2));
            }
        }
    }
    v2_vga[lin] = val;
    v2_vga_cov[lin] = 1;
    if (v2_vga_bg_writer) v2_vga_bg[lin] = val;   // the background VGA (render_v2.h): tiles and picture chunks only
}
// non-static entry for writers living in other TUs (v2_vm.cpp glyph mirror)
void v2_vga_glyph_px(uint32_t addr, uint32_t plane, uint8_t val) {
    v2_vga_w(addr, plane, val);
}
// ============================================================================
// jpt_1c9b1 engine core, address form (glyph 1E16D + sprite type-1 0648 share
// it). Source: 8 strips × (1 mask + 8 data). Strip pass p covers SOURCE columns
// {p, 4+p} (bit_dx 0/4) and rows {strip*4 + bit_dy}. Screen pixel for source
// column c: t = pan + c (normal) / t = pan + (7 − c) (hflip, jpt_1D18B);
// VGA byte = di + row*0x56 + (t>>2), plane = t&3. pan = low 2 bits of the
// pixel anchor (jpt_1CF34 case), verified against the case-0/case-3 bodies.
// mask_and = low byte of cs:word_1C830 (row-clip mask, 0xFF = full).
// type-2 engine (jpt_1D9E8 pan cases → per-row jpt_1DA02, hflip loc_1DBCD):
// 32-wide row sprite. Per plane pass p: rows × (1 mask + 8 data); mask bit b
// (0x80>>b) selects source byte-column (7−b): pixel col c = (7−b)*4 + p,
// hflip mirrors within 32: c = 31 − ((7−b)*4 + p) = 4b + 3 − p. t = pan + c;
// VGA byte = di + (t>>2), plane = t&3; di += 0x56 per row, src += 9 per row;
// after each plane pass src += skip_tail (= cs:[(top+bot)*2+0x3D], the 9·m
// source-skip of the clipped rows).
// type-4 engine (jpt_1D4FA pan → per-unit jpt_1D514, hflip jpt_1D6E8/1D703):
// 16-wide sprite; unit = 2 rows × 4 byte-cols, 8 units/plane pass, 288 bytes.
// PROVEN case bodies: normal (1D514): 0x80: [si+0]→[di+0]; 0x08: [si+4]→[di+0x56];
// 0x01: [si+7]→[di+0x59]  ⇒ bit b: row=(b≥4)?0:1, col=3−(b&3), val=data[7−b],
// t = pan + (col*4 + q). hflip (1D703): 0x80: [si+0]→[di+3]; 0x01: [si+7]→
// [di+0x56+0] ⇒ t = pan + 15 − (col*4 + q). Unit step: di += 0xAC, src += 9;
// per-pass tail: src += cs:[(top_u+bot_u)*2+0x3D] (9·m).
void v2_vga_sprite16_type4(const uint8_t* g0, uint16_t di0, int pan, int hflip,
                           uint8_t mask_and, int units, uint16_t skip_tail) {
    const uint8_t* g = g0;
    for (int q = 0; q < 4; q++) {
        uint16_t di = di0;
        for (int u = 0; u < units; u++) {
            uint8_t mask = (uint8_t)(g[0] & mask_and);
            const uint8_t* data = g + 1;
            if (mask) {
                for (int b = 0; b < 8; b++) {
                    if (!(mask & (0x80 >> b))) continue;
                    // SOLVED against a full real write-trace (137/137 unique):
                    // bit b → row=(b≥4)?1:0, col=b&3, value=data[b]; t=pan+col*4+q.
                    // (The m2c jumptable case comments belong to an overlapping
                    // table — the exhaustive trace solve is the ground truth.)
                    int row = (b >= 4) ? 1 : 0;
                    int col = b & 3;
                    int c = col * 4 + q;
                    int t = hflip ? (pan + 15 - c) : (pan + c);
                    uint16_t a = (uint16_t)(di + row * 0x56 + (t >> 2));
                    v2_vga_w(a, (uint32_t)(t & 3), data[b]);
                }
            }
            g += 9;
            di = (uint16_t)(di + 0xAC);
        }
        g += skip_tail;
    }
}
void v2_vga_sprite32_type2(const uint8_t* g0, uint16_t di0, int pan, int hflip,
                           uint8_t mask_and, int rows, uint16_t skip_tail) {
    const uint8_t* g = g0;   // points at the first row's mask ([si-1])
    for (int p = 0; p < 4; p++) {
        uint16_t di = di0;
        for (int r = 0; r < rows; r++) {
            uint8_t mask = (uint8_t)(g[0] & mask_and);
            const uint8_t* data = g + 1;
            if (mask) {
                for (int b = 0; b < 8; b++) {
                    if (!(mask & (0x80 >> b))) continue;
                    // PROVEN by jpt case bodies + hflip pass cascade:
                    //   normal jpt_1DA02: mask bit b → data[b] → byte di+b
                    //     (case 0x80: [si+0]→[di+0]; case 0x01: [si+7]→[di+7])
                    //     with pan/pass: t = pan + (4b + q), byte=di+(t>>2), pl=t&3
                    //   hflip  jpt_1DBFE: data[b] → byte di+(7−b)
                    //     (case 0x80: [si+0]→[di+7]; case 0x01: [si+7]→[di+0])
                    //     cascade (case3: planes 2,1,0,3 with INC di ×3) matches
                    //     t = pan + 31 − (4b + q).
                    int c = 4 * b + p;             // source pixel column (q = p)
                    int t = hflip ? (pan + 31 - c) : (pan + c);
                    uint16_t a = (uint16_t)(di + (t >> 2));
                    v2_vga_w(a, (uint32_t)(t & 3), data[b]);
                }
            }
            g += 9;
            di = (uint16_t)(di + 0x56);
        }
        g += skip_tail;
    }
}
void v2_vga_sprite8(const uint8_t* src72, uint16_t di, int pan, int hflip,
                    uint8_t mask_and) {
    static const int bit_dx[8] = {0,4,0,4,0,4,0,4};
    static const int bit_dy[8] = {0,0,1,1,2,2,3,3};
    const uint8_t* g = src72;
    for (int p = 0; p < 4; p++) {
        for (int strip = 0; strip < 2; strip++) {
            uint8_t mask = (uint8_t)(g[0] & mask_and);
            const uint8_t* data = g + 1;
            if (mask) {
                for (int b = 0; b < 8; b++) {
                    if (!(mask & (0x80 >> b))) continue;
                    int c = bit_dx[b] + p;
                    int t = hflip ? (pan + 7 - c) : (pan + c);
                    int row = strip * 4 + bit_dy[b];
                    uint16_t addr = (uint16_t)(di + row * 0x56 + (t >> 2));
                    v2_vga_w(addr, (uint32_t)(t & 3), data[b]);
                }
            }
            g += 9;
        }
    }
}
// VGA→VGA span copy (orig REP MOVSB class: 1DE05 latch, 171DC row copy).
// Coverage travels WITH the data: a copy from a not-yet-migrated source leaves
// the destination unverifiable instead of falsely failing parity.
// Linear layout addr*4+plane keeps the 4 planes of one VGA byte adjacent, so a
// VGA span of n bytes is one contiguous 4n linear range — memcpy/memmove-able
// unless it wraps the 64K address space (then fall back to the per-byte loop).
void v2_vga_copy_span(uint16_t dst, uint16_t src, uint16_t nbytes) {
    // the background VGA (render_v2.h): the same span, the same REP MOVSB semantics as below
    {
        const uint32_t de = (uint32_t)dst + nbytes, se = (uint32_t)src + nbytes;
        const bool fo = dst > src && (uint32_t)dst < se;
        if (!fo && de <= 0x10000u && se <= 0x10000u)
            memmove(v2_vga_bg + (uint32_t)dst * 4u, v2_vga_bg + (uint32_t)src * 4u, (size_t)nbytes * 4u);
        else
            for (uint16_t i = 0; i < nbytes; i++)
                for (uint32_t p = 0; p < 4; p++)
                    v2_vga_bg[(uint32_t)(uint16_t)(dst + i) * 4u + p] = v2_vga_bg[(uint32_t)(uint16_t)(src + i) * 4u + p];
    }
    {
        static long _t = -2;
        if (_t == -2) { const char* e = getenv("V2_VGAW_TRAP"); _t = e ? strtol(e, 0, 0) : -1; }
        if (_t >= 0 && (uint32_t)_t >= dst && (uint32_t)_t < (uint32_t)dst + nbytes) {
            static int _n = 0;
            if (_n < 40) { _n++;
                // v2_dbg_pre_vm_iter: file-scope extern (top of file)
                fprintf(stderr, "SPAN-TRAP f%d dst=%04X src=%04X n=%u srcval@t=%02X cov=%d ra=%p\n",
                        v2_dbg_pre_vm_iter, dst, src, nbytes,
                        v2_vga[(uint32_t)(uint16_t)(src + ((uint32_t)_t - dst)) * 4u],
                        v2_vga_cov[(uint32_t)(uint16_t)(src + ((uint32_t)_t - dst)) * 4u],
                        __builtin_return_address(0));
            }
        }
    }
    uint32_t ds_end = (uint32_t)dst + nbytes, ss_end = (uint32_t)src + nbytes;
    // REP MOVSB (DF=0) copies bytes FORWARD: a forward-overlapping copy
    // (src < dst < src+n) re-reads bytes it has just written, replicating the
    // [src..dst) prefix with period dst-src — memmove would preserve the
    // original source bytes instead (divergence #34, caught by unit 135's
    // overlap case; real pages never overlap, so replays never saw it).
    // Backward/no overlap keeps the memmove fast path (identical result).
    bool fwd_overlap = dst > src && (uint32_t)dst < ss_end;
    if (!fwd_overlap && ds_end <= 0x10000u && ss_end <= 0x10000u) {
        memmove(v2_vga     + (uint32_t)dst * 4u, v2_vga     + (uint32_t)src * 4u, (size_t)nbytes * 4u);
        memmove(v2_vga_cov + (uint32_t)dst * 4u, v2_vga_cov + (uint32_t)src * 4u, (size_t)nbytes * 4u);
        return;
    }
    for (uint16_t i = 0; i < nbytes; i++) {
        for (uint32_t p = 0; p < 4; p++) {
            uint32_t dl = (uint32_t)(uint16_t)(dst + i) * 4u + p;
            uint32_t sl = (uint32_t)(uint16_t)(src + i) * 4u + p;
            v2_vga[dl] = v2_vga[sl];
            v2_vga_cov[dl] = v2_vga_cov[sl];
        }
    }
}
// Zero-fill span (orig REP STOSB class: op13 wipe) — covered.
void v2_vga_fill_span(uint16_t dst, uint32_t nbytes, uint8_t val) {
    for (uint32_t i = 0; i < nbytes; i++) memset(v2_vga_bg + (uint32_t)(uint16_t)(dst + i) * 4u, val, 4);   // the background VGA (render_v2.h): the same fill
    uint32_t end = (uint32_t)dst + nbytes;
    if (end > 0x10000u) { uint32_t n1 = 0x10000u - dst;
        memset(v2_vga + (uint32_t)dst * 4u, val, (size_t)n1 * 4u);
        memset(v2_vga_cov + (uint32_t)dst * 4u, 1, (size_t)n1 * 4u);
        memset(v2_vga, val, (size_t)(nbytes - n1) * 4u);
        memset(v2_vga_cov, 1, (size_t)(nbytes - n1) * 4u);
        return; }
    memset(v2_vga + (uint32_t)dst * 4u, val, (size_t)nbytes * 4u);
    memset(v2_vga_cov + (uint32_t)dst * 4u, 1, (size_t)nbytes * 4u);
}
// v2 CRTC state — published by v2_page_flip_16775 (shadow-side 16775 formula).
uint32_t v2_vga_crtc = 0;
uint8_t  v2_vga_pan  = 0;
// Extract the visible 320×176 window from the shadow VGA exactly like the
// real CRTC unfold (v2_fetch_orig_page): rows from v2_vga_crtc, pitch 0x56,
// linear = (crtc + y*0x56)*4 + pan.
extern uint8_t* v2_vm_get_shadow_ds();   // v2_vm.cpp (C++ linkage; declared here at file scope for the C-linkage reader below)
extern "C" int v2_vga_fetch_page(uint8_t* out, uint32_t count) {
    // debug: V2_VGA_PIXTRAP=<x>,<y>,<frame> — at every flip of that game frame, arm V2_VGAW_TRAP
    // with the shadow-VGA byte of screen pixel (x, y) of the page shown now (the writers of the
    // NEXT frames' passes to that pixel are then reported with their return addresses)
    {
        static int on = -1, px = 0, py = 0, pf = -1, pg = -1;   // <x>,<y>,<frame>[,<page hex>]: only the flips showing that page arm it
        if (on < 0) { const char* e = getenv("V2_VGA_PIXTRAP"); on = 0; if (e && sscanf(e, "%d,%d,%d,%x", &px, &py, &pf, &pg) >= 3) on = 1; }
        const uint8_t* sh = v2_vm_get_shadow_ds();   // file-scope declaration above (this function has C linkage)
        if (on == 1 && v2_dbg_pre_vm_iter == pf && (pg < 0 || (sh && *(const uint16_t*)(sh + 0x92F9) == (uint16_t)pg))) {
            const uint32_t lin = (v2_vga_crtc + (uint32_t)py * 0x56u) * 4u + v2_vga_pan + (uint32_t)px;
            g_vgaw_trap = (long)(lin >> 2);
            fprintf(stderr, "VGAW-PIXTRAP f%d pixel (%d,%d) -> vga addr=%04lX plane=%u (crtc=%04X pan=%u)\n",
                    v2_dbg_pre_vm_iter, px, py, g_vgaw_trap, (unsigned)(lin & 3), v2_vga_crtc, v2_vga_pan);
        }
    }
    uint32_t rows = count / 320;
    if (rows * 320 != count) return 0;
    for (uint32_t y = 0; y < rows; y++) {
        uint32_t base = (v2_vga_crtc + y * 0x56u) * 4u + v2_vga_pan;
        if (base + 320 > sizeof(v2_vga)) return 0;
        memcpy(out + y * 320, v2_vga + base, 320);
    }
    return 1;
}
// Parity check against the real drawBuffer — covered bytes only. Returns diff
// count; logs the first few mismatches (env V2_VGAPARITY, called from verify).
extern "C" { int v2_vga_parity_hud = 0; }   // diffs in the HUD region (addr < 0x1600)
extern "C" int v2_vga_parity_check(const uint8_t* real_drawbuffer, int log_limit) {
    int diffs = 0;
    v2_vga_parity_hud = 0;
    for (uint32_t i = 0; i < sizeof(v2_vga); i++) {
        if (!v2_vga_cov[i]) continue;
        if (v2_vga[i] != real_drawbuffer[i]) {
            if ((i >> 2) < 0x1600u) { v2_vga_parity_hud++; continue; }  // HUD channel — migrated later
            if (diffs < log_limit)
                fprintf(stderr, "VGAPARITY addr=%04X plane=%u v2=%02X real=%02X\n",
                        i >> 2, i & 3, v2_vga[i], real_drawbuffer[i]);
            diffs++;
        }
    }
    // env V2_VGAPARITY_DUMP: once, when diffs exceed the threshold, dump the
    // full diff address list for offline classification.
    static int _dumped = 0;
    const char* dth = getenv("V2_VGAPARITY_DUMP");
    int dump_always = dth && atoi(dth) == 0;   // =0: rewrite every call (final state survives)
    if (dth && (dump_always || (!_dumped && diffs >= atoi(dth)))) {
        _dumped = 1;
        FILE* f = fopen("/tmp/v2_vga_diffs.txt", "w");
        if (f) {
            for (uint32_t i = 0; i < sizeof(v2_vga); i++) {
                if (!v2_vga_cov[i] || v2_vga[i] == real_drawbuffer[i]) continue;
                fprintf(f, "%04X %u %02X %02X\n", i >> 2, i & 3, v2_vga[i], real_drawbuffer[i]);
            }
            fclose(f);
        }
    }
    return diffs;
}

void v2_chunk_bg_update_from_render() {
    v2_fbw = 320;   // UX stage 9 step 4: a chunk screen's frame is the 320-px raster (the backup below is 320-stride)
    memcpy(v2_chunk_bg_backup, v2_render_buf, 320 * 176);
    v2_chunk_bg_valid = true;
}

// v2 shadow DAC (task #22): byte-exact model of the VGA DAC state, updated by
// the v2 mirrors of the orig OUT 3C8/3C9 sites from SHADOW data — the same
// sites whose setPalette mirrors feed drawPalette on the orig side:
//   sub_10fe6 full upload (source HARDCODED ds:0x8202), sub_10ffc positive/
//   negative animation bursts (source ds:[word_303E0 + ...] — can be 0x7F02!),
//   sub_1106f blank at Mode X init (covered by zero init), and the color-3
//   writes (sub_10350 / sub_1041c / sub_103ca / VM cmd_type 6 loc_127e4).
// The previous display model (snapshot of ds:0x8202 + color-3 mirror) was
// wrong whenever a 10ffc burst ran with word_303E0 = 0x7F02 under nonzero
// shade: the DAC held 0x7F02 colors while 0x8202 held shaded ones
// (V2-PAL-DIVERGE idx 0x71). Values are 6-bit VGA (0..0x3F), <<2 at publish.
uint8_t v2_dac_shadow[768] = {};

extern "C" void v2_publish_dac_palette(void);  // defined below (#87)
// the orig side's draw info (render.cpp), by its leading member only — the V2_VGA_DUMP
// forensics below writes its VGA memory next to the shadow VGA (same layout)
struct v2_real_drawinfo_fwd { uint8_t drawBuffer[65536 * 4]; };
extern struct v2_real_drawinfo_fwd* myDrawInfo;
static void v2_dbg_role_bases(const uint8_t* sh, uint16_t rb[3]);   // below v2_effective_camera

void v2_swap_render_buf() {
#ifdef V2_RENDER_FROM_SHADOW
    if (!v2_vm_in_frame) return;
#endif
    // debug: V2_DSHASH=1 — one line per publish in EITHER build: the game frame, a running
    // publish counter and the FNV-1a hash of the whole shadow DS. The two builds' sequences
    // of one replay must be identical line for line (the DS timelines agree).
    {
        static int on = -1; static long n = 0;
        if (on < 0) { const char* e = getenv("V2_DSHASH"); on = e ? ((*e == '2') ? 2 : 1) : 0; }
        if (on >= 1) {
            extern uint8_t* v2_vm_get_shadow_ds(); const uint8_t* sh = v2_vm_get_shadow_ds();
            uint32_t h = 2166136261u;
            if (sh && on == 1) for (uint32_t i = 0; i < 0x10000u; i++) { h ^= sh[i]; h *= 16777619u; }
            // V2_DSHASH=2: the hash leaves out what legitimately differs between the test build
            // (segments and sound handles from the DOS world) and the game build (its own): the
            // verify skip set (v2_vm.cpp v2_ds_skip_ranges: input words, VGA mode byte, AIL driver
            // state 98E4..9950, vsync counter, sound segment pointers) plus the DosMemAlloc segment
            // values wherever they are stored — the registry 2E5D..2E7C, the sound base 992C, the
            // anim chunk far-pointer segments 128D (11 words), the object columns OBJ_SPRITE_SEG
            // 094D..0A4C, OBJ_SUB_SRC_SEG 0B4D..0C4C and OBJ_CODE_SEG 1355..137C — the AIL
            // sequence state table 9950..9C90 (the driver's own records behind the handles), and
            // the whole BIOS checksum word 86D0..86D1 (the skip set names its low byte only; the
            // high byte differs between the two builds' host memories as well).
            if (sh && on == 2) {
                extern bool v2_ds_verify_skip(uint32_t i);
                // V2_DSHASH_MASK=a-b,c-d (hex) adds ranges to leave out without a rebuild.
                static uint8_t* mask = nullptr;
                if (!mask) {
                    mask = (uint8_t*)calloc(0x10000u, 1);
                    static const uint16_t rng[][2] = { {0x094D, 0x0A4C}, {0x0B4D, 0x0C4C}, {0x128D, 0x12A2}, {0x1355, 0x137C},
                                                       {0x2E5D, 0x2E7C}, {0x86D0, 0x86D1}, {0x992C, 0x992D}, {0x9950, 0x9C90} };
                    for (uint32_t i = 0; i < 0x10000u; i++) if (v2_ds_verify_skip(i)) mask[i] = 1;
                    for (const auto& r : rng) for (uint32_t i = r[0]; i <= r[1]; i++) mask[i] = 1;
                    if (const char* ml = getenv("V2_DSHASH_MASK")) {
                        for (const char* p = ml; *p; ) {
                            char* e; long a = strtol(p, &e, 16); if (e == p) break; long b = a;
                            if (*e == '-') { p = e + 1; b = strtol(p, &e, 16); }
                            for (long i = a; i <= b && i < 0x10000; i++) if (i >= 0) mask[i] = 1;
                            p = (*e == ',') ? e + 1 : e;
                        }
                    }
                }
                for (uint32_t i = 0; i < 0x10000u; i++) { if (mask[i]) continue; h ^= sh[i]; h *= 16777619u; }
            }
            fprintf(stderr, "V2-DSHASH f%d n=%ld %08X\n", v2_dbg_pre_vm_iter, n, h);
            // V2_DSHASH_DUMP=<dir>: the whole shadow DS as <dir>/ds_f<frame>_n<publish>.bin at the
            // first publish and at every publish of the frames listed in V2_DSHASH_FRAMES=a,b-c,d
            static const char* dd = getenv("V2_DSHASH_DUMP");
            if (dd && sh) {
                bool want = (n == 0);
                if (const char* fl = getenv("V2_DSHASH_FRAMES")) {
                    for (const char* p = fl; *p; ) {
                        char* e; long f0 = strtol(p, &e, 10); if (e == p) break; long f1 = f0;
                        if (*e == '-') { p = e + 1; f1 = strtol(p, &e, 10); }
                        if (v2_dbg_pre_vm_iter >= f0 && v2_dbg_pre_vm_iter <= f1) want = true;
                        p = (*e == ',') ? e + 1 : e;
                    }
                }
                if (want) {
                    char path[512]; snprintf(path, sizeof path, "%s/ds_f%d_n%ld.bin", dd, v2_dbg_pre_vm_iter, n);
                    if (FILE* f = fopen(path, "wb")) { fwrite(sh, 1, 0x10000u, f); fclose(f); }
                }
            }
            n++;
        }
    }
    // Atomic snapshot of pixels + HUD + palette under same lock. All 3 must be
    // captured together — otherwise level-transition updates one before render
    // thread reads the others → mismatched colors (main artifact: viewport,
    // HUD icons flicker: HUD pixels rendered with palette from different frame).
    std::lock_guard<std::mutex> lock(v2_display_mutex);
    // The display shows the shadow-VGA — the exact byte-parity
    // channel the verifier checks (viewport window via the shadow CRTC, HUD
    // from VGA rows 0..63). The old render_buf/hud_buf lanes remain as the
    // internal composition sources for some mirrors, but what the USER sees
    // is the verified VGA state.
    {
#ifdef V2_ONLY
        // Stage 6.2 gated the shadow-VGA writers out of V2_ONLY ("the
        // Mode-X pixel model dies in the target engine") but this reader
        // kept scanning the now-empty v2_vga — the user window went black
        // while the game ran (2026-08-28 report). The target engine's
        // frame lives in the linear composition buffers: viewport in
        // v2_render_buf, HUD in v2_hud_buf — present those.
        extern uint8_t v2_hud_buf[320 * 64];
        extern uint8_t v2_display_hud_buf[];
        // A chunk screen (byte_2AAAF & 0x42: the logos, the title, the password screen) is
        // presented from the shadow-VGA page model (2026-09-11): the window through the CRTC
        // start the sub_16775 mirror published (the title's scroll from the art to the logo),
        // the HUD band from VGA rows 0..63 (the picture's bottom lives there behind the split),
        // the DAC fade as it is — the frame the DOS build shows, byte for byte. The CJK text
        // overlay (UX6) is painted over it; the tile levels keep the page-list composition.
        {
            extern uint8_t* v2_vm_get_shadow_ds();
            const uint8_t* sh = v2_vm_get_shadow_ds();
            const bool chunk_screen = sh && (sh[DS_LEVEL_FLAGS] & 0x42) != 0;
            if (chunk_screen && v2_vga_fetch_page(v2_display_buf, 320 * 176)) {
                for (int y = 0; y < 64; y++) memcpy(v2_display_hud_buf + y * 320, v2_vga + (size_t)y * 0x56u * 4u, 320);
                memset(v2_display_buf + 320 * 176, 0, 320 * 64);
                v2_display_w = 320;
                v2_display_fullscreen = 0;
                // the CJK overlay (v2_draw_ui with the cells from the page) onto the presented frame
                {
                    uint8_t* save_out = v2_tls_out; const int save_w = v2_fbw; const bool save_f = v2_compose_at_flip, save_c = v2_tls_ui_cells_from_page;
                    v2_tls_out = v2_display_buf; v2_fbw = 320; v2_compose_at_flip = true; v2_tls_ui_cells_from_page = true;
                    v2_draw_ui(0);
                    v2_tls_out = save_out; v2_fbw = save_w; v2_compose_at_flip = save_f; v2_tls_ui_cells_from_page = save_c;
                }
            } else {
                // all 200 rows: rows 176..199 matter only on full-screen LVX scenes
                memcpy(v2_display_buf, v2_render_buf, (size_t)v2_fbw * 240);
                v2_display_w = v2_fbw;   // UX stage 9 step 4: the frame's width travels with it
                { const int r = v2_view_rows(); v2_display_fullscreen = (r > 176) ? r : 0; }   // 0 = HUD layout, else the map rows shown
                memcpy(v2_display_hud_buf, v2_hud_buf, 320 * 64);
            }
        }
        // UX stage 8 step 2 (co-op): which player holds each viking, and where
        // its portrait sits in the HUD art (ds:[vk-0x7A84] -> the VGA offset of
        // v2_draw_hud_portrait) — the presenter paints the P1/P2/P3 badges.
        { extern uint8_t* v2_vm_get_shadow_ds();
          const uint8_t* sh = v2_vm_get_shadow_ds();
          for (int vk = 0; vk < 3; vk++) {
              V2DisplayBadge& b = v2_display_badge[vk];
              b.owner = -1;
              if (!sh || g_v2_coop_players <= 1) continue;
              uint16_t vga_off = *(const uint16_t*)(sh + (uint16_t)(vk * 2 - 0x7A84));
              b.x = (vga_off % 86) * 4 + 3;
              b.y = vga_off / 86;
              b.owner = v2_coop_owner((uint16_t)(vk * 2));
          } }
        v2_smooth_capture();          // UX stage 9: the tick snapshot for the interpolating presenter
#else
        v2_display_fullscreen = 0;   // verification build presents the shadow-VGA window only
        v2_display_w = 320;          // the shadow-VGA window is the 320-px raster
        extern int v2_vga_fetch_page(uint8_t* out, uint32_t count);
        extern uint8_t v2_vga[65536 * 4];
        if (!v2_vga_fetch_page(v2_display_buf, 320 * 176))
            memcpy(v2_display_buf, v2_render_buf, 320 * 240);
        // debug: V2_LINCMP=1 — test mode: the linear composition buffer (what the game build
        // presents) against the shadow-VGA page of the same flip (the orig's screen), map rows
        // 0..175; one line per differing flip, a summary at exit
        {
            static int on = -1; static long flips = 0, bad = 0, badpx = 0; static int lastf = -1;
            if (on < 0) { on = getenv("V2_LINCMP") ? 1 : 0;
                          if (on) atexit([]() { fprintf(stderr, "V2-LINCMP-SUMMARY flips=%ld differing=%ld px=%ld\n", flips, bad, badpx); }); }
            extern uint8_t* v2_vm_get_shadow_ds(); const uint8_t* sh = v2_vm_get_shadow_ds();
            // a chunk screen (byte_2AAAF & 0x42) is presented from this page by the game build too
            // (2026-09-11) — its linear buffer is not what the user sees: not compared
            const bool chunk_screen = sh && (sh[DS_LEVEL_FLAGS] & 0x42) != 0;
            if (on == 1 && v2_fbw == 320 && !chunk_screen) {
                flips++;
                int cnt = 0, x0 = 320, x1 = -1, y0 = 176, y1 = -1;
                // the differing pixels by (page index, composition index) pair — the top pairs
                // name the kind of difference (a tile vs black, a sprite colour vs a tile ...)
                struct { uint8_t a, b; int n; } pairs[6]; int npairs = 0; int other = 0;
                for (int y = 0; y < 176; y++) for (int x = 0; x < 320; x++) {
                    const uint8_t a = v2_display_buf[y * 320 + x], b = v2_render_buf[y * 320 + x];
                    if (a == b) continue;
                    cnt++; if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
                    int k = 0; for (; k < npairs; k++) if (pairs[k].a == a && pairs[k].b == b) { pairs[k].n++; break; }
                    if (k == npairs) { if (npairs < 6) { pairs[npairs].a = a; pairs[npairs].b = b; pairs[npairs].n = 1; npairs++; } else other++; }
                }
                if (cnt) { bad++; badpx += cnt;
                    if (v2_dbg_pre_vm_iter != lastf || bad < 200) { lastf = v2_dbg_pre_vm_iter;
                        fprintf(stderr, "V2-LINCMP f%d diff=%d bbox=%d..%d,%d..%d lvl=%04X flags=%02X pairs(page:lin)", v2_dbg_pre_vm_iter, cnt, x0, x1, y0, y1,
                                sh ? *(const uint16_t*)(sh + DS_LEVEL) : 0, sh ? sh[DS_LEVEL_FLAGS] : 0);
                        for (int k = 0; k < npairs; k++) fprintf(stderr, " %02X:%02X=%d", pairs[k].a, pairs[k].b, pairs[k].n);
                        if (other) fprintf(stderr, " +%d", other);
                        fprintf(stderr, "\n"); } }
            }
        }
        // debug: V2_VGA_DUMP=<dir>:<from>-<to> writes the whole shadow VGA (64K x 4 planes,
        // linear = addr*4+plane) at every flip of the game-frame range, with the page roles
        // and the CRTC start — page-level forensics (which page holds what)
        {
            static int dump = -1; static char dir[480]; static int from = 0, to = -1, n = 0;
            if (dump < 0) {
                const char* e = getenv("V2_VGA_DUMP"); dump = 0;
                if (e && *e) { snprintf(dir, sizeof dir, "%s", e); char* c = strrchr(dir, ':');
                               if (c && sscanf(c + 1, "%d-%d", &from, &to) == 2) { *c = 0; dump = 1; } }
            }
            if (dump == 1 && v2_dbg_pre_vm_iter >= from && v2_dbg_pre_vm_iter <= to && n < 2000) {
                extern uint32_t v2_vga_crtc; extern uint8_t v2_vga_pan;
                extern uint8_t* v2_vm_get_shadow_ds(); const uint8_t* sh = v2_vm_get_shadow_ds();
                char path[560]; snprintf(path, sizeof path, "%s/vga_%04d_f%d.bin", dir, n, v2_dbg_pre_vm_iter);
                FILE* f = fopen(path, "wb");
                if (f) { fwrite(v2_vga, 1, sizeof(v2_vga), f); fclose(f); }
                // the orig side's VGA memory (the m2c drawBuffer, same layout) next to it
                { if (myDrawInfo) {
                      snprintf(path, sizeof path, "%s/real_%04d_f%d.bin", dir, n, v2_dbg_pre_vm_iter);
                      FILE* g = fopen(path, "wb");
                      if (g) { fwrite(myDrawInfo->drawBuffer, 1, sizeof(v2_vga), g); fclose(g); } } }
                if (sh) {
                    // the window base of each page role by the sub_16775 formula (v2_page_flip_16775), and
                    // the FS words of the cells V2_FLIP_CELLS=<col>,<row>,<w>,<h> (tile cells) — the dirty bits
                    uint16_t rb[3]; v2_dbg_role_bases(sh, rb);
                    char cells[640] = "";
                    { static int c0 = -1, r0 = 0, cw = 0, ch = 0; static int init = 0;
                      if (!init) { init = 1; const char* e = getenv("V2_FLIP_CELLS"); if (e) sscanf(e, "%d,%d,%d,%d", &c0, &r0, &cw, &ch); }
                      if (c0 >= 0) { int p = 0;
                          for (int rr = 0; rr < ch && p < 600; rr++) {
                              const uint16_t rowbase = *(const uint16_t*)(sh + (uint16_t)((r0 + rr) * 2 - LUT_ROW_BASE));
                              for (int cc = 0; cc < cw && p < 600; cc++) {
                                  const uint32_t off = (uint32_t)(rowbase + c0 + cc) * 2u;
                                  extern uint8_t v2_vm_shadow_fs[];
                                  const uint16_t w = off + 1 < 0x10000u ? *(const uint16_t*)(v2_vm_shadow_fs + off) : 0xFFFF;
                                  p += snprintf(cells + p, sizeof(cells) - p, " %04X", w);
                              } } } }
                    fprintf(stderr, "V2-VGADUMP n=%d f%d draw=%04X shown=%04X bg=%04X crtc=%04X pan=%u base00=%04X base34=%04X base68=%04X cells%s\n", n, v2_dbg_pre_vm_iter,
                            *(const uint16_t*)(sh + 0x92F7), *(const uint16_t*)(sh + 0x92F9), *(const uint16_t*)(sh + 0x92FB), v2_vga_crtc, v2_vga_pan, rb[0], rb[1], rb[2], cells);
                }
                n++;
            }
        }
        extern uint8_t v2_display_hud_buf[];
        for (int y = 0; y < 64; y++)
            memcpy(v2_display_hud_buf + y * 320, v2_vga + (uint32_t)(y * 0x56) * 4u, 320);
#endif
    }
    extern uint8_t* v2_vm_get_shadow_ds();
    uint8_t* shad = v2_vm_get_shadow_ds();
    if (shad) {
        // Publish the shadow DAC (task #22) — the exact VGA DAC state as
        // maintained by the v2 mirrors of every orig OUT 3C8/3C9 site.
        // (Replaces the old "ds:0x8202 snapshot + color-3 mirror" model,
        // which missed 10ffc bursts sourced from 0x7F02 under shade —
        // V2-PAL-DIVERGE idx 0x71.)
        v2_publish_dac_palette();
    }
    // debug: V2_FLIP_DUMP=<dir>:<from>-<to> writes the frame published by EVERY flip whose
    // game frame lies in [from, to] as <dir>/flip_<n>_f<game frame>.ppm — the display
    // buffer through the palette just published: in the game build the composition
    // buffer, in test mode the shadow-VGA page the judge compares. The two builds' flip
    // sequences of one replay can be laid side by side (chunk screens, level changes).
    {
        static int dump = -1; static char dir[480]; static int from = 0, to = -1, n = 0;
        if (dump < 0) {
            const char* e = getenv("V2_FLIP_DUMP"); dump = 0;
            if (e && *e) { snprintf(dir, sizeof dir, "%s", e); char* c = strrchr(dir, ':');
                           if (c && sscanf(c + 1, "%d-%d", &from, &to) == 2) { *c = 0; dump = 1; } }
        }
        if (dump == 1 && v2_dbg_pre_vm_iter >= from && v2_dbg_pre_vm_iter <= to && n < 6000) {
            // V2_FLIP_OBJ=<slot>[,<slot>...] (up to 4): each object's sprite flags / redraw byte [114D] /
            // erase byte [114E] / sprite position / the CUR ([D4D]) and OLD ([F4D]) rects of the dirty
            // logic at this flip — plus the camera (ds:44/46) and the tile-window origin (ds:257F/2581)
            { static int fo[4] = {-2, -1, -1, -1}; static int nfo = 0;
              if (fo[0] == -2) { fo[0] = -1; const char* e = getenv("V2_FLIP_OBJ");
                  while (e && *e && nfo < 4) { fo[nfo++] = (int)strtol(e, 0, 0); const char* c = strchr(e, ','); e = c ? c + 1 : nullptr; } }
              if (nfo > 0) { extern uint8_t* v2_vm_get_shadow_ds(); const uint8_t* sh = v2_vm_get_shadow_ds();
                  if (sh) {
                      // hash = FNV-1a of the whole shadow DS at this flip (the two builds' timelines compared flip by flip)
                      uint32_t hsh = 2166136261u; for (uint32_t i = 0; i < 0x10000u; i++) { hsh ^= sh[i]; hsh *= 16777619u; }
                      fprintf(stderr, "V2-FLIPCAM n=%d f%d cam=(%d,%d) win=(%d,%d) mode=%04X force=%02X dshash=%08X\n", n, v2_dbg_pre_vm_iter,
                              *(const int16_t*)(sh + 0x44), *(const int16_t*)(sh + 0x46),
                              *(const int16_t*)(sh + 0x257F), *(const int16_t*)(sh + 0x2581),
                              *(const uint16_t*)(sh + DS_GAME_MODE_AC), sh[0x9568], hsh);
                      // V2_FLIP_DSDUMP=<dir>: the whole shadow DS of this flip as <dir>/ds_<n>_f<frame>.bin
                      { static const char* dd = nullptr; static int ddi = -1; if (ddi < 0) { dd = getenv("V2_FLIP_DSDUMP"); ddi = (dd && *dd) ? 1 : 0; }
                        if (ddi == 1) { char p2[560]; snprintf(p2, sizeof p2, "%s/ds_%04d_f%d.bin", dd, n, v2_dbg_pre_vm_iter);
                                        FILE* fd = fopen(p2, "wb"); if (fd) { fwrite(sh, 1, 0x10000, fd); fclose(fd); } } }
                      for (int k = 0; k < nfo; k++) { const int o = fo[k]; if (o < 0) continue;
                          fprintf(stderr, "V2-FLIPOBJ n=%d f%d obj=%02X flags=%04X dirty=%02X erase=%02X xy=(%d,%d) cur=(%d,%d) old=(%d,%d) strips=%u spr=%04X:%04X\n", n, v2_dbg_pre_vm_iter, o,
                                  *(const uint16_t*)(sh + o + OBJ_SPRITE_FLAGS), sh[o + OBJ_DIRTY_MODE], sh[o + 0x114E],
                                  *(const int16_t*)(sh + o + OBJ_SPRITE_X), *(const int16_t*)(sh + o + OBJ_SPRITE_Y),
                                  *(const int16_t*)(sh + o + 0xD4D), *(const int16_t*)(sh + o + 0xE4D),
                                  *(const int16_t*)(sh + o + 0xF4D), *(const int16_t*)(sh + o + 0x104D),
                                  *(const uint16_t*)(sh + o + OBJ_STRIP_COUNT),
                                  *(const uint16_t*)(sh + o + OBJ_SPRITE_SEG), *(const uint16_t*)(sh + o + OBJ_SPRITE_OFF)); } } } }
            char path[560]; snprintf(path, sizeof path, "%s/flip_%04d_f%d.ppm", dir, n, v2_dbg_pre_vm_iter);
            FILE* f = fopen(path, "wb");
            if (f) {
                // the presenter's canvas: the map rows of the display buffer, then the HUD band
                // (rows 176..239) from the published HUD buffer — through the shadow DAC just
                // published (6-bit, << 2 like v2_publish_dac_palette)
                extern uint8_t v2_display_hud_buf[];
                const int W = v2_display_w > 0 ? v2_display_w : 320;
                const int map_rows = v2_display_fullscreen ? v2_display_fullscreen : 176;
                fprintf(f, "P6\n%d 240\n255\n", W);
                for (int y = 0; y < 240; y++) for (int x = 0; x < W; x++) {
                    uint8_t idx = 0;
                    if (y < map_rows) idx = v2_display_buf[y * W + x];
                    else if (!v2_display_fullscreen && x < 320) idx = v2_display_hud_buf[(y - 176) * 320 + x];
                    const uint8_t* c = v2_dac_shadow + idx * 3;
                    fputc(c[0] << 2, f); fputc(c[1] << 2, f); fputc(c[2] << 2, f);
                }
                fclose(f);
            }
#ifndef V2_ONLY
            // test mode only: the linear composition buffer of the same flip next to the
            // shadow-VGA page (<dir>/lin_<n>_f<frame>.ppm) — the two rendering models of
            // one DS timeline side by side (the game build presents the linear buffer)
            { char p2[560]; snprintf(p2, sizeof p2, "%s/lin_%04d_f%d.ppm", dir, n, v2_dbg_pre_vm_iter);
              FILE* f2 = fopen(p2, "wb");
              if (f2) {
                  fprintf(f2, "P6\n320 240\n255\n");
                  extern uint8_t v2_display_hud_buf[];
                  for (int y = 0; y < 240; y++) for (int x = 0; x < 320; x++) {
                      uint8_t idx = 0;
                      if (y < 176) idx = v2_render_buf[y * v2_fbw + x];
                      else idx = v2_display_hud_buf[(y - 176) * 320 + x];
                      const uint8_t* c = v2_dac_shadow + idx * 3;
                      fputc(c[0] << 2, f2); fputc(c[1] << 2, f2); fputc(c[2] << 2, f2);
                  }
                  fclose(f2);
              } }
#endif
            n++;
        }
    }
    v2_flip_notify();   // PACING VRR: the presenter shows this flip at once (render_v2.cpp)
}

// Publish the current shadow DAC to the presenter palette. Called from
// v2_swap_render_buf (frame image snapshot) AND from every palette dispatch
// tick in v2_render_callback (#87): the DOS DAC changes mid-frame (the
// vsync ISR dispatches off_17974[303DE] three times per game frame — e.g.
// the level-2 lift arrows alternate a full-upload red phase and a slot-0
// burst yellow phase inside ONE game frame, by game data). A CRT showed
// those phases within the frame; publishing only at swap time collapsed
// them to one snap per game frame → hard 35 Hz flicker. Per-tick publish
// lets the 60 fps presenter show each phase, like the real screen (and
// like the m2c orig window, whose setPalette writes land immediately).
extern "C" void v2_publish_dac_palette(void) {
    extern SDL_Color v2_display_palette[256];
    extern bool v2_display_palette_valid;
    // V2_LADDER_TRACE: log each publish with the 0x77 window byte — proves
    // the per-tick publish carries BOTH intra-frame phases to the presenter.
    { static int _lp = -1;
      if (_lp < 0) _lp = getenv("V2_LADDER_TRACE") ? 1 : 0;
      if (_lp) { 
        fprintf(stderr, "LADDER[f%d] PUBLISH 77=%02X%02X%02X\n", v2_dbg_pre_vm_iter,
            v2_dac_shadow[0x77*3], v2_dac_shadow[0x77*3+1], v2_dac_shadow[0x77*3+2]); } }
    for (int i = 0; i < 256; i++) {
        v2_display_palette[i].r = v2_dac_shadow[i*3 + 0] << 2;
        v2_display_palette[i].g = v2_dac_shadow[i*3 + 1] << 2;
        v2_display_palette[i].b = v2_dac_shadow[i*3 + 2] << 2;
        v2_display_palette[i].a = 255;
    }
    v2_display_palette_valid = true;
}

// Snapshot buffers — captured atomically at v2_swap_render_buf time.
SDL_Color v2_display_palette[256] = {};
bool v2_display_palette_valid = false;
uint8_t v2_display_hud_buf[320*64] = {};
V2DisplayBadge v2_display_badge[3] = {};   // UX stage 8 step 2 (co-op): the player badge per viking portrait

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
// ============================================================================
// Effective camera: viewport pixel origin with the screen-shake offset folded
// in. Mirrors orig set_display_memory_addr (port of sub_16775, seg000:1880-
// 1912) EXACTLY — all dw (uint16_t) arithmetic:
//   x_offset = x_disp + x_some; if (x_offset > x_level_size) x_offset = x_disp - x_some;
//   y_offset = y_disp + y_some; if (y_offset > y_level_size) y_offset = y_disp - y_some;
// №60: the four former inline copies used int arithmetic with int16_t sign
// extension — a shake word with bit 15 set (e.g. 0xFFFE) picked the OPPOSITE
// branch there (signed sum stays below the limit; the orig unsigned sum wraps
// above it and takes the minus branch). uint16_t end to end, as the port.
// x_some/y_some (DS_SHAKE_X/Y) come from sub_12d2c; the tile-shift pair is
// the v2 linear-buffer derivation (orig splits into CRTC start + pixel pan).
// ============================================================================
struct V2Camera {
    uint16_t x_eff, y_eff;          // effective pixel origin (shake folded in)
    int pix_off_x, pix_off_y;       // sub-tile pixel offset (eff & 7)
    int tile_shift_x, tile_shift_y; // shake tile shift: eff>>3 - disp>>3
};
static V2Camera v2_effective_camera(const uint8_t* ds_base) {
    // phase D pilot: typed V2StateViewC access — same bytes, named fields.
    V2StateViewC st(ds_base);
    uint16_t x_disp = st.viewport_x();
    uint16_t y_disp = st.viewport_y();
    uint16_t x_some = st.shake_x();
    uint16_t y_some = st.shake_y();
    uint16_t x_lvl  = st.scroll_limit_x();
    uint16_t y_lvl  = st.scroll_limit_y();
    V2Camera c;
    c.x_eff = (uint16_t)(x_disp + x_some);
    if (c.x_eff > x_lvl) c.x_eff = (uint16_t)(x_disp - x_some);
    c.y_eff = (uint16_t)(y_disp + y_some);
    if (c.y_eff > y_lvl) c.y_eff = (uint16_t)(y_disp - y_some);
    c.pix_off_x = c.x_eff & 7;
    c.pix_off_y = c.y_eff & 7;
    c.tile_shift_x = (int)(c.x_eff >> 3) - (int)(x_disp >> 3);
    c.tile_shift_y = (int)(c.y_eff >> 3) - (int)(y_disp >> 3);
    return c;
}
// debug (V2_VGA_DUMP): the window base of each page role (0 / 0x34 / 0x68) by the
// sub_16775 formula — LUT[0x89F8 + role + (y>>3)*2] + LUT[0x8E58 + (y&7)*2] + (x>>2) + 8
static void v2_dbg_role_bases(const uint8_t* sh, uint16_t rb[3]) {
    V2Camera cam = v2_effective_camera(sh);
    const int ye = (int)(int16_t)cam.y_eff, xe = (int)(int16_t)cam.x_eff;
    for (int r = 0; r < 3; r++) {
        const uint16_t role = (uint16_t)(r * 0x34);
        rb[r] = (uint16_t)(*(const uint16_t*)(sh + (uint16_t)(0x89F8 + role + ((ye >> 3) * 2))) +
                           *(const uint16_t*)(sh + (uint16_t)(0x8E58 + ((ye & 7) * 2))) + (xe >> 2) + 8);
    }
}

// UX stage 2: the SNES parallax layer under the level tiles (display lane).
// par = (viewport * f) >> 8 per axis (8.8 multiplier from the head), the
// autoscroll axes take the time-true accumulator instead; the map repeats
// with its own period; nibble 0 = transparent (DAC 0 stays = the backdrop),
// the palette row comes from the map cell like the console's tilemap word.
// UX stage 9 (console finale): the cell's priority bit (SNES tilemap bit 13)
// splits the layer in two passes like the PPU's BG2 priorities — prio 0
// under the level tiles (this call from the tile pass), prio 1 over the
// sprites and under the flagged tiles (the call from v2_draw_flagged_tiles:
// mode-1 order BG1.1 > BG2.1 > OBJ2 > BG1.0 > BG2.0; the objects draw at
// priority 2 — the flagged tiles, BG1.1, cover them on the console too).
// The DE finale keeps the dragon head's top and the torches in BG2 priority-1
// quads over the opaque stage-front row of BG1.
static void v2_draw_parallax_pass(const V2StateViewC& st, uint8_t* buf, uint16_t prio) {
    const V2ParallaxLayer& P = v2_parallax;
    if (!P.on || !P.map || !P.tiles) return;
    const int wpx = P.w * 8, hpx = P.h * 8;
    if (!wpx || !hpx) return;
    const uint32_t acc_x = v2_tls_par_acc ? v2_tls_par_acc[0] : P.acc_x;   // UX stage 9: interpolated
    const uint32_t acc_y = v2_tls_par_acc ? v2_tls_par_acc[1] : P.acc_y;
    int px = (P.fx & 0x8000) ? (int)(acc_x / 1792u)
                             : (int)(((uint32_t)st.viewport_x() * (P.fx & 0x7FFF)) >> 8);
    int py = (P.fy & 0x8000) ? (int)(acc_y / 1792u)
                             : (int)(((uint32_t)st.viewport_y() * (P.fy & 0x7FFF)) >> 8);
    px = (px + (int)P.off_x) % wpx; py = (py + (int)P.off_y) % hpx;   // phase offsets (map trailer)
    for (int sy = 0; sy < v2_clip_h; sy++) {
        const int my = (py + sy) % hpx;
        const uint16_t* mrow = P.map + (my >> 3) * P.w;
        const int ty = my & 7;
        uint8_t* out = buf + sy * v2_fbw;
        for (int sx = 0; sx < v2_fbw; sx++) {
            const int mx = (px + sx) % wpx;
            const uint16_t cell = mrow[mx >> 3];
            if ((cell & 0x2000) != prio) continue;           // the other priority pass draws it
            const uint32_t idx = cell & 0x3FF;
            if (idx >= P.ntiles) continue;
            const int tx = (cell & 0x4000) ? (7 - (mx & 7)) : (mx & 7);
            const int tyy = (cell & 0x8000) ? (7 - ty) : ty;
            const uint8_t v = P.tiles[idx * 64 + tyy * 8 + tx];
            if (v) out[sx] = (uint8_t)(((cell >> 10) & 7) * 16 + v);
        }
    }
}
static void v2_draw_parallax(const V2StateViewC& st, uint8_t* buf) { v2_draw_parallax_pass(st, buf, 0); }

void v2_draw_tiles(uint16_t ds_val) {
#ifdef V2_RENDER_FROM_SHADOW
    if (!v2_vm_in_frame && !v2_tls_presenter && !v2_compose_at_flip) return;
#endif
    if (!myDrawInfo_v2 || !v2_m2c_base) return;

    uint8_t* ds_base = v2_get_ds_base(ds_val);
    V2StateViewC st(ds_base);   // phase D: typed field access
    uint8_t* buf = v2_tls_out ? v2_tls_out : v2_render_buf;   // UX stage 9: the presenter's own buffer
    // UX stage 0: clip height for this frame's sprites/UI (200 on a
    // full-screen LVX scene, else the orig 176-row viewport).
    // UX stage 1: on a full-screen scene the console blanks every line from
    // 187 down by a raster split (measured on the DE video and modelled in
    // the scene map: yellow line 187-188, black below) — sprites never show
    // there (the Ship consoles' lowest rows vanish under it on the Genesis),
    // so the sprite/pixel clip stops at 187; the tile pass still paints all
    // 200 rows (the band itself is map data).
    {
        const int rows = v2_view_rows();                 // 176 / 200 (scene) / 224 (LVX_TALL224)
        v2_clip_h = (rows == 200) ? 187 : rows;
        v2_tile_rows = (rows == 224) ? 29 : 25;          // 29 x 8 = 232 px covers 224 + the sub-tile offset
    }

    // Tile map segment (FS)
    uint16_t fs_seg = st.seg_fs();
    // Tile graphics segment
    uint16_t tgfx_seg = st.seg_tilegfx();
    // V2-DRAWT-DBG: log entry state every 60 frames
    {
        static int _dt_dbg = 0; _dt_dbg++;
        if (_dt_dbg <= 5 || _dt_dbg % 200 == 0) {
            fprintf(stderr,
              "V2-DRAWT-DBG[%d]: ds=%04X fs_seg=%04X tgfx_seg=%04X 25CF=%02X 25AD=%04X 25C9=%04X\n",
              _dt_dbg, ds_val, fs_seg, tgfx_seg,
              v2gs(ds_base).level_flags_b(),
              st.level(),
              st.level_load());
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
    uint8_t lvl_flags = v2gs(ds_base).level_flags_b();
    // debug (V2_FLIP_DUMP set): the path this level's frames take, once per level
    { static int tr = -1; static int last_level = -2;
      if (tr < 0) tr = getenv("V2_FLIP_DUMP") ? 1 : 0;
      if (tr && !v2_tls_presenter && (int)st.level() != last_level) {
          last_level = (int)st.level();
          fprintf(stderr, "V2-DRAWTILES: level %d flags %02X path %s chunk_bg_valid %d fbw %d at game frame %d\n", last_level, lvl_flags,
                  (lvl_flags & 0x42) ? "chunk" : "tiles", (int)v2_chunk_bg_valid, v2_fbw, v2_dbg_pre_vm_iter);
      } }
    if (lvl_flags & 0x42) {
        if (v2_tls_presenter) return;      // UX stage 9: the presenter never composes chunk screens
        v2_last_frame_tiles = false;
        v2_fbw = 320;                 // UX stage 9 step 4: chunk screens are the 320-px raster
        if (v2_chunk_bg_valid) {
            memcpy(v2_render_buf, v2_chunk_bg_backup, 320 * 176);
        }
        return;
    }
    if (!v2_tls_presenter) {
        v2_chunk_bg_valid = false; // tile-based level; static backup no longer relevant
        v2_last_frame_tiles = true;   // UX stage 9: this tick's frame is interpolable
        v2_fbw = v2_view_w;           // UX stage 9 step 4: a tile level renders its view width (320 unless WIDE)
    }

    // Normal: clear and draw tiles (all 200 buffer rows: the tile loop below
    // already renders 25 tile rows; rows 176..199 are shown only on
    // full-screen LVX scenes, otherwise the HUD band covers them)
    memset(buf, 0, (size_t)v2_fbw * 240);
    // UX stage 2: the parallax layer goes under the tiles; the tile pass then
    // skips the pixels of colour 0 of each palette row (the console's
    // transparent index — on the DOS palette they are blacked out, sub_112ae).
    v2_draw_parallax(st, buf);
    const bool par_on = v2_parallax.on;

#ifdef V2_RENDER_FROM_SHADOW
    uint8_t* fs_base = v2_resolve_segment(fs_seg);
    uint8_t* tgfx_base = v2_resolve_segment(tgfx_seg);
    if (v2_tls_fs) fs_base = (uint8_t*)v2_tls_fs;   // UX stage 9: the presenter's FS snapshot
#else
    uint8_t* fs_base = v2_m2c_base + ((uint32_t)fs_seg << 4);
    uint8_t* tgfx_base = v2_m2c_base + ((uint32_t)tgfx_seg << 4);
#endif

    uint16_t scroll_x = st.scroll_row();   // tile-row scroll (vp_y>>3)
    uint16_t scroll_y = st.scroll_col();   // tile-column scroll (vp_x>>3)

    // Sub-tile pixel offset from viewport pixel position.
    // Mirrors orig set_display_memory_addr (sub_16775, seg000.cpp:1105):
    //   x_offset = x_disp + x_some; if (x_offset > x_level_size) x_offset = x_disp - x_some;
    //   y_offset = y_disp + y_some; if (y_offset > y_level_size) y_offset = y_disp - y_some;
    // x_some/y_some (ds:0x39E/0x3A0) are screen shake offsets (sub_12d2c, eip 0x2D2C).
    // x_low_bits = x_offset & 0b11 (pixel pan 0..3, OUT to attribute reg 0x33)
    // x_high_bits = x_offset >> 2 (byte position in VGA row, → CRTC start addr)
    // Combined display position = (x_high_bits<<2) + x_low_bits = x_offset (full pixel precision).
    // In v2 linear buffer: total sub-tile pixel offset = x_offset & 7.
    // №60: dw-exact shake fold (see v2_effective_camera).
    V2Camera cam = v2_effective_camera(ds_base);
    int pix_off_x = cam.pix_off_x, pix_off_y = cam.pix_off_y;
    int extra_tile_x = cam.tile_shift_x, extra_tile_y = cam.tile_shift_y;

    for (int row_vis = 0; row_vis < v2_tile_rows; row_vis++) {
        uint16_t row_scrolled = (uint16_t)(row_vis + scroll_x + extra_tile_y);
        // Orig sub_16ded has NO row_scrolled bound — just reads LUT and renders
        // whatever it finds. v2 had `if (row_scrolled >= 64) continue;` hardcoded
        // limit which clipped tiles on large maps when scrolled past row 64.
        // Removed: now mirrors orig (LUT read wraps via uint16_t arithmetic).

        // Row base from lookup table at ds-0x7098
        uint16_t lut_off = (uint16_t)(row_scrolled * 2u - LUT_ROW_BASE);
        uint16_t row_base = *(uint16_t*)(ds_base + lut_off);

        for (int col_vis = 0; col_vis < v2_fb_cols(); col_vis++) {
            uint16_t col_scrolled = (uint16_t)(col_vis + scroll_y + extra_tile_x);

            // Read tile map entry: word at fs:[(row_base + col_scrolled) * 2]
            uint16_t tile_map_off = (uint16_t)((row_base + col_scrolled) * 2u);
            uint16_t tile_entry = *(uint16_t*)(fs_base + tile_map_off);
            // debug: V2_MAP_ROWDUMP=<frame>[,<row_vis>] — one screen row's cells at that frame:
            // the visible column, the render-map cell index, the map word and the page's word
            {
                static int rd = -1, rd_f = 0, rd_row = 11;
                if (rd < 0) { const char* e = getenv("V2_MAP_ROWDUMP"); rd = 0; if (e && *e) { rd = 1; sscanf(e, "%d,%d", &rd_f, &rd_row); } }
                if (rd == 1 && v2_dbg_pre_vm_iter == rd_f && row_vis == rd_row && !v2_tls_presenter) {
                    const uint16_t* ovr = v2_tls_presenter ? v2_tls_tile_ovr : v2_tile_override;
                    fprintf(stderr, "V2-MAPROW f%d row_vis=%d col_vis=%d cell=%04X map=%04X page=%04X fbw=%d\n", v2_dbg_pre_vm_iter, row_vis, col_vis,
                            tile_map_off >> 1, tile_entry, ovr ? ovr[tile_map_off >> 1] : 0xEEEE, v2_fbw);
                }
            }

            // Screen position (with sub-tile pixel offset)
            int screen_x = col_vis * 8 - pix_off_x;
            int screen_y = row_vis * 8 - pix_off_y;

            // The page lists (render_v2.h): the composed page's own word for this cell — the
            // tile the page still shows (an animation frame behind the map), or black (wiped
            // by sub_16880 and not painted since).
            {
                const uint16_t* ovr = v2_tls_presenter ? v2_tls_tile_ovr : v2_tile_override;
                // The page holds 43 columns of cells from the camera's scroll column and 25 rows
                // from its scroll row — 30 on an LVX_TALL224 level (the sub_16DED fill: 0x2B cells
                // per row, 0x19 rows, 0x1E when v2_view_h_cur is 224; the scroll painters keep
                // that window). A cell outside it never lies on any page — the wing of a wide
                // frame beyond 344 px — so the page's word (0xFFFE "wiped, black" included) does
                // not apply there: the map tile does. (2026-09-12: the 16:10 frame showed black
                // from column 344 on.)
                // Of the 43 painted columns the original ever shows 41 (320 px = 40 cells, 41 with
                // a pel pan); columns 41 and 42 are slack the scroll painters do not keep in every
                // path (level 2 in 16:10: cells 41..42 of a row still 0xFFFE from the wipe — a black
                // 16x8 block at the ladder's foot), so the page's word is trusted for columns 0..40.
                if (ovr && col_vis < 0x29 && row_vis < (v2_view_rows() == 224 ? 0x1E : 0x19)) {
                    const uint16_t w = ovr[tile_map_off >> 1];
                    if (w == 0xFFFE) {
                        for (int row = 0; row < 8; row++) {
                            int sy = screen_y + row;
                            if (sy < 0) continue;
                            if (sy >= 240) break;
                            for (int px = 0; px < 8; px++) {
                                int sx = screen_x + px;
                                if (sx < 0) continue;
                                if (sx >= v2_fbw) break;
                                buf[sy * v2_fbw + sx] = 0;
                            }
                        }
                        continue;
                    }
                    if (w != 0xFFFF) tile_entry = w;
                }
            }

            // Tile graphics offset (64-byte aligned) — bits 15:6
            // Bits 5:0 contain flip flags (4=hflip, 5=vflip) and dirty flag (bit 0)
            // draw_tile (sub_1689e) renders ALL tiles including offset 0 — no skip.
            uint16_t tile_gfx_off = tile_entry & 0xFFC0;
            bool hflip = (tile_entry & 0x10) != 0;
            bool vflip = (tile_entry & 0x20) != 0;

            uint8_t* tile = tgfx_base + tile_gfx_off;

            // Decode tile: 4 planes × 8 rows × 2 bytes
            // Normal pixel order per row:
            //   x+0=p0_b0, x+1=p1_b0, x+2=p2_b0, x+3=p3_b0,
            //   x+4=p0_b1, x+5=p1_b1, x+6=p2_b1, x+7=p3_b1
            for (int row = 0; row < 8; row++) {
                int src_row = vflip ? (7 - row) : row;
                int sy = screen_y + row;
                if (sy < 0) continue;
                if (sy >= 240) break;

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
                    if (sx >= v2_fbw) break;
                    if (par_on && (pixels[px] & 0x0F) == 0) continue;   // UX stage 2: row colour 0 = transparent
                    buf[sy * v2_fbw + sx] = pixels[px];
                }
            }
        }
    }
    // The background VGA (render_v2.h): columns 0..319 come from the VGA bytes the CRTC
    // window reads — the page's ring, its fill state, its first-flip gaps, the stale memory
    // beyond the port's half wipe — over the map-based pass above (which stays for the wing
    // of a wide frame beyond the window).
    {
        const uint8_t* bg = v2_tls_presenter ? v2_tls_vga_bg : v2_vga_bg;
        static int no_readout = -1; if (no_readout < 0) no_readout = getenv("V2_NO_BG_READOUT") ? 1 : 0;   // debug: the map pass alone
        if (bg && !no_readout) {
            uint32_t crtc = v2_vga_crtc; uint8_t pan = v2_vga_pan;
            if (v2_tls_presenter) v2_crtc_for_camera(ds_base, &crtc, &pan);   // the interpolated camera's start
            v2_vga_bg_readout(buf, v2_fbw, v2_view_rows(), bg, crtc, pan, par_on);
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
    V2StateViewC st(ds_base);   // phase D: typed field access
    uint16_t fs_seg = st.seg_fs();
    uint16_t tgfx_seg = st.seg_tilegfx();
    if (!fs_seg || !tgfx_seg) return;
#ifdef V2_RENDER_FROM_SHADOW
    uint8_t* fs_base = v2_resolve_segment(fs_seg);
    uint8_t* tgfx_base = v2_resolve_segment(tgfx_seg);
    if (v2_tls_fs) fs_base = (uint8_t*)v2_tls_fs;   // UX stage 9: the presenter's FS snapshot
#else
    uint8_t* fs_base = v2_m2c_base + ((uint32_t)fs_seg << 4);
    uint8_t* tgfx_base = v2_m2c_base + ((uint32_t)tgfx_seg << 4);
#endif
    uint16_t scroll_x = st.scroll_row();
    uint16_t scroll_y = st.scroll_col();
    // Mirrors orig set_display_memory_addr (sub_16775) — apply x_some/y_some shake.
    // №60: dw-exact shake fold (see v2_effective_camera).
    V2Camera cam = v2_effective_camera(ds_base);
    int pix_off_x = cam.pix_off_x, pix_off_y = cam.pix_off_y;
    int extra_tile_x = cam.tile_shift_x, extra_tile_y = cam.tile_shift_y;

    // Convert absolute tilemap coords → visible viewport position (with shake-aware tile shift)
    int visible_row = abs_row - (int)scroll_x - extra_tile_y;
    int visible_col = abs_col - (int)scroll_y - extra_tile_x;
    if (visible_row < 0 || visible_row >= 25) return;
    if (visible_col < 0 || visible_col >= v2_fb_cols()) return;

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
        if (sy >= 240) break;
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
            if (sx >= v2_fbw) break;
            buf[sy * v2_fbw + sx] = pixels[px];
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
    if (sx >= 0 && sx < v2_fbw && sy >= 0 && sy < v2_clip_h)   // 176, or 200 on full-screen scenes; v2_fbw columns
        buf[sy * v2_fbw + sx] = color;
}

static void v2_draw_sprites_impl(uint16_t ds_val, int late_gate, int only_obj = -1);

// ----------------------------------------------------------------------------
// The display list (render_v2.h V2DrawList): every object the sprite layers of the
// sub-frame draw is recorded here in draw order — the early layer resets the list, the
// late layer appends. The presenter composes from this list (v2_draw_list), so what it
// shows is what the passes drew, not a re-derivation of their decisions.
// ----------------------------------------------------------------------------
V2DrawList v2_frame_draws = { 0, {} };
// The late set of the current pass, reported by the sub_1dd9c mirror (v2_late_sprites_1DD9C)
// in its dispatch order: v2_draw_sprites_late draws exactly these slots. Valid from the
// mirror's begin until the late layer consumed it; without a report (test mode's phase
// order) the late layer falls back to its gate re-derivation below.
static uint16_t g_late_slots[128];
static int      g_late_n = 0;
static bool     g_late_valid = false;
void v2_late_list_begin(void) { g_late_n = 0; g_late_valid = true; }
void v2_late_list_add(uint16_t slot) { if (g_late_valid && g_late_n < 128) g_late_slots[g_late_n++] = slot; }

// Rasterise one sprite of the orig's three formats at screen (sx0, sy0) — the body every
// sprite layer and the presenter's list replay share. Column formula: sx0 + N*4 + section
// (normal) or sx0 + (sprite_w-1) - (N*4 + section) (hflip, flags bit 9).
//   type 1 (seg003_648_proc, jpt_1CF4E): 8x8, 2 strips/section, 4 rows/strip, 2 bytes/row
//   type 2 (loc_1d8a8, jpt_1DA02): dynamic, [0xC4D] strips/section, 1 row/strip, 8 bytes/row
//   type 4 (sub_1d3b2, jpt_1d514): 16x16, 8 strips/section, 2 rows/strip, 4 bytes/row
// Sprite data: sprite_off is the 1-based offset of the first data byte, the mask byte sits
// at offset-1; each strip = 1 mask byte + 8 data bytes, 4 plane sections in a row.
// dead / ox / oy (page lists): the command's dead-cell mask (render_v2.h V2DrawCmd::dead) and
// the pixel offset of its top-left corner inside its cell — a pixel whose cell the page has
// restored since the draw is not painted. The mask follows the sprite (cells relative to its
// own top-left), so the presenter's shifted positions keep the same holes.
// mand / clip_top / clip_bot: the handler's window clip at draw time (V2DrawCmd) — the column
// mask ANDed into every strip mask exactly as the engines do (jpt_1DA02 / jpt_1D514), the
// strips (type 2: rows, type 4: 2-row units) skipped at the top and the bottom.
// data (page lists): the record's own copy of the strip bytes (V2DrawCmd::data_off in the
// list's arena) — the pixels as painted, whatever the bank holds now; nullptr = the live bank
static void v2_raster_sprite(uint8_t* buf, int type, uint16_t flags, int sx0, int sy0,
                             uint16_t sprite_seg, uint16_t sprite_off, int strips, int obj, uint16_t cur_lvl,
                             const uint64_t* dead = nullptr, int ox = 0, int oy = 0,
                             uint8_t mand = 0xFF, int clip_top = 0, int clip_bot = 0,
                             const uint8_t* rec_data = nullptr) {
    if (!sprite_seg && !rec_data) return;
    // the pixel writer of this rasterisation: the dead-cell test in front of the pixel put
    auto v2_put_pixel = [&](uint8_t* b, int x, int y, uint8_t v) {
        if (dead) {
            const int cc = (ox + (x - sx0)) >> 3, cr = (oy + (y - sy0)) >> 3;
            if ((unsigned)cc < 8u && (unsigned)cr < 32u && ((dead[cr >> 3] >> (((cr & 7) << 3) | cc)) & 1u)) return;
        }
        ::v2_put_pixel(b, x, y, v);
    };
    int num_strips, rows_per_strip, bytes_per_row;
    if (type == 1) {
        num_strips = 2; rows_per_strip = 4; bytes_per_row = 2;
    } else if (type == 2) {
        num_strips = strips;
        if (num_strips <= 0) return;
        rows_per_strip = 1; bytes_per_row = 8;
    } else if (type == 4) {
        num_strips = 8; rows_per_strip = 2; bytes_per_row = 4;
    } else return;   // types 0,3,5,6,7: cs:0x0000, no renderer

    // Horizontal flip: bit 9 (0x200) of flags. All three type renderers
    // (seg003_648_proc, sub_1d3b2, loc_1d8a8) check this flag and branch to
    // mirrored rendering paths.
    bool hflip = (flags & 0x200) != 0;

    // Coarse bounds check — sprite pixel size. (Display lane only; the
    // byte-exact page channel is the shadow VGA in v2_vm.cpp.)
    int sprite_h = num_strips * rows_per_strip;
    int sprite_w = bytes_per_row * 4;  // 4 planes
    if (sx0 >= v2_fbw || sx0 < -sprite_w || sy0 >= v2_clip_h || sy0 < -sprite_h) return;

    // Sprite data: resolve segment to shadow buffer, add offset.
    // sprite_off = 1-based offset to first data byte; mask at offset-1.
#ifdef V2_RENDER_FROM_SHADOW
    uint8_t* seg_base = rec_data ? nullptr : v2_resolve_segment(sprite_seg);
    if (!seg_base && !rec_data) return;
    uint8_t* sprite = seg_base ? seg_base + sprite_off - 1 : nullptr;
    if (!v2_tls_presenter && !rec_data) {
        static int cmp_mismatch = 0;
        // real-vs-shadow compare only meaningful when orig updates real
        // memory (V2_ONLY: dynamic segments in the snapshot buffer stay 0).
        if (v2_vm_get_real_ds() && cur_lvl < 38 && cmp_mismatch < 10) {
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
    (void)obj; (void)cur_lvl;
    uint32_t sprite_linear = ((uint32_t)sprite_seg << 4) + sprite_off - 1;
    uint8_t* sprite = v2_m2c_base + sprite_linear;
#endif

    // sx(col) computes the screen x for a given data column.
    auto sx = [&](int col) -> int {
        return hflip ? sx0 + sprite_w - 1 - col : sx0 + col;
    };

    const uint8_t* ptr = rec_data ? rec_data : sprite;   // the record's own bytes, else the live bank
    for (int section = 0; section < 4; section++) {
        int plane = section;
        for (int strip = 0; strip < num_strips; strip++) {
            uint8_t mask = (uint8_t)(ptr[0] & mand);                 // the handler's column clip (AND on the strip mask)
            if (type != 1 && (strip < clip_top || strip >= num_strips - clip_bot)) mask = 0;   // the strips the handler skipped
            const uint8_t* data = ptr + 1;
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

// One object of a sprite layer: the trace, the display-list record, the rasterisation at
// world - camera. late = which layer draws it (the record's tag, the trace's label).
static void v2_draw_sprite_obj(uint8_t* buf, uint8_t* ds_base, const V2StateViewC& st,
                               int viewport_x, int viewport_y, int obj, int late) {
    uint16_t flags = *(uint16_t*)(ds_base + obj + OBJ_SPRITE_FLAGS);
    int type = flags & 7;
    // debug: V2_SPR_TRACE=<from>-<to> — every sprite draw of the game-frame range with its
    // pass (early = the sub_1de05 point, late = the sub_1dd9c repaint), in draw order
    { static int tr = -1; static int f0 = 0, f1 = -1;
      if (tr < 0) { const char* e = getenv("V2_SPR_TRACE"); tr = 0; if (e && sscanf(e, "%d-%d", &f0, &f1) == 2) tr = 1; }
      if (tr && !v2_tls_presenter && v2_dbg_pre_vm_iter >= f0 && v2_dbg_pre_vm_iter <= f1)
          fprintf(stderr, "V2-SPRDRAW f%d %s obj=%02X fl=%04X t=%d xy=(%d,%d) seg=%04X off=%04X\n", v2_dbg_pre_vm_iter,
                  late ? "late " : "early", obj, flags, type,
                  *(int16_t*)(ds_base + obj + OBJ_SPRITE_X), *(int16_t*)(ds_base + obj + OBJ_SPRITE_Y),
                  *(uint16_t*)(ds_base + obj + OBJ_SPRITE_SEG), *(uint16_t*)(ds_base + obj + OBJ_SPRITE_OFF)); }

    {
        static int spr_printed = 0;
        uint16_t cur_lvl = st.level();
        if (spr_printed < 15 && cur_lvl < 38) {
            spr_printed++;
            printf("V2-SPR[l%d]: obj=%02x fl=%04x t=%d xy=(%d,%d) seg=%04x off=%04x scr=(%d,%d)\n",
                   cur_lvl, obj, flags, type,
                   *(int16_t*)(ds_base + obj + OBJ_SPRITE_X),
                   *(int16_t*)(ds_base + obj + OBJ_SPRITE_Y),
                   *(uint16_t*)(ds_base + obj + OBJ_SPRITE_SEG),
                   *(uint16_t*)(ds_base + obj + OBJ_SPRITE_OFF),
                   viewport_x, viewport_y);
        }
    }

    // Dispatch table at cs:0x15CB: only types 1, 2, 4 have renderers.
    // Type 1 → cs:0x0648 (seg003_648_proc, 8×8)
    // Type 2 → cs:0x1078 (loc_1d8a8, dynamic size)
    // Type 4 → cs:0x0B82 (sub_1d3b2, 16×16)
    // Types 0,3,5,6,7 → cs:0x0000 (not used)
    if (type != 1 && type != 2 && type != 4) return;

    int16_t world_x = *(int16_t*)(ds_base + obj + OBJ_SPRITE_X);
    int16_t world_y = *(int16_t*)(ds_base + obj + OBJ_SPRITE_Y);
    uint16_t sprite_off = *(uint16_t*)(ds_base + obj + OBJ_SPRITE_OFF);
    uint16_t sprite_seg = *(uint16_t*)(ds_base + obj + OBJ_SPRITE_SEG);
    if (!sprite_seg) return;
    uint16_t strips = *(uint16_t*)(ds_base + obj + OBJ_STRIP_COUNT);
    if (type == 2 && (int)strips <= 0) return;

    // the display list: the game thread's layers record what they draw
    if (!v2_tls_presenter && v2_frame_draws.n < 256) {
        V2DrawCmd& c = v2_frame_draws.cmd[v2_frame_draws.n++];
        c.slot = (uint16_t)obj; c.flags = flags; c.x = world_x; c.y = world_y;
        c.seg = sprite_seg; c.off = sprite_off; c.strips = strips; c.late = (uint8_t)(late ? 1 : 0); c.type = (uint8_t)type;
        c.mand = 0xFF; c.clip_top = 0; c.clip_bot = 0; c.epoch = 0; memset(c.dead, 0, sizeof c.dead);   // the layers' record: no clip, no page history
        memset(c.keep, 0xFF, sizeof c.keep); c.data_len = 0; c.data_off = 0;                              // ... every cell, the live bank
    }
    v2_raster_sprite(buf, type, flags, world_x - viewport_x, world_y - viewport_y, sprite_seg, sprite_off, (int)strips, obj, st.level());
}

// the early layer opens the sub-frame's display list; the presenter never records
void v2_draw_sprites(uint16_t ds_val) { if (!v2_tls_presenter) v2_frame_draws.n = 0; v2_draw_sprites_impl(ds_val, 0); }
void v2_draw_sprites_late(uint16_t ds_val) { v2_draw_sprites_impl(ds_val, 1); }

// The presenter: the list's commands in order, command i at world (pos_x[i], pos_y[i]).
// A glyph cell of the text plane at screen (sx0, sy0): the sub_1E16D engine's pixel mapping
// (the same one v2_draw_ui uses), the dead-cell test as for a sprite.
static void v2_raster_glyph(uint8_t* buf, const uint8_t* ds_base, uint16_t glyph_index, int sx0, int sy0,
                            const uint64_t* dead, int ox, int oy, const uint8_t* rec_data) {
    extern const uint8_t* v2_glyph_bytes(const uint8_t*, uint16_t);
    const uint8_t* glyph = rec_data ? rec_data : v2_glyph_bytes(ds_base, glyph_index);   // the record's own bytes, else the live page
    auto put = [&](int lx, int ly, uint8_t v) {
        const int cc = (ox + lx) >> 3, cr = (oy + ly) >> 3;
        if ((unsigned)cc < 8u && (unsigned)cr < 32u && ((dead[cr >> 3] >> (((cr & 7) << 3) | cc)) & 1u)) return;
        v2_put_pixel(buf, sx0 + lx, sy0 + ly, v);
    };
    for (int plane = 0; plane < 4; plane++) {
        for (int strip = 0; strip < 2; strip++) {
            uint8_t mask = glyph[0];
            const uint8_t* data = glyph + 1;
            int base_y = strip * 4;
            if (mask) {
                if (mask & 0x80) put(0*4 + plane, base_y + 0, data[0]);
                if (mask & 0x40) put(1*4 + plane, base_y + 0, data[1]);
                if (mask & 0x20) put(0*4 + plane, base_y + 1, data[2]);
                if (mask & 0x10) put(1*4 + plane, base_y + 1, data[3]);
                if (mask & 0x08) put(0*4 + plane, base_y + 2, data[4]);
                if (mask & 0x04) put(1*4 + plane, base_y + 2, data[5]);
                if (mask & 0x02) put(0*4 + plane, base_y + 3, data[6]);
                if (mask & 0x01) put(1*4 + plane, base_y + 3, data[7]);
            }
            glyph += 9;
        }
    }
}
void v2_draw_list(const V2DrawList& L, const int16_t* pos_x, const int16_t* pos_y, int which) {
    if (!v2_m2c_base || !myDrawInfo_v2) return;
    uint8_t* ds_base = v2_get_ds_base(0);                     // the presenter's snapshot DS (camera)
    uint8_t* buf = v2_tls_out ? v2_tls_out : v2_render_buf;
    V2Camera cam = v2_effective_camera(ds_base);
    const int viewport_x = (int)(int16_t)cam.x_eff, viewport_y = (int)(int16_t)cam.y_eff;
    // the flagged-tile repaints (V2_CMD_FGTILE) read the level's tile graphics and mask table
    // as v2_draw_flagged_tiles does, resolved once per list
    const uint8_t* fg_tgfx = nullptr; const uint8_t* fg_gs = nullptr; bool fg_resolved = false;
    for (int i = 0; i < L.n; i++) {
        const V2DrawCmd& c = L.cmd[i];
        const bool glyph = (c.type == V2_CMD_GLYPH), fgtile = (c.type == V2_CMD_FGTILE);
        if (which == 0 && (glyph || fgtile)) continue;    // the sprites only
        if (which == 1 && !(glyph || fgtile)) continue;   // the repaints and glyphs only
        const int x = pos_x ? pos_x[i] : c.x, y = pos_y ? pos_y[i] : c.y;
        const uint8_t* rec = c.data_len ? L.arena + c.data_off : nullptr;   // the record's own strip bytes
        if (fgtile) {
            if (c.dead[0] & 1u) continue;   // its one cell restored since the repaint
            if (!fg_resolved) {
                fg_resolved = true;
                V2StateViewC stv(ds_base);
                const uint16_t tgfx_seg = stv.seg_tilegfx(), gs_seg = stv.seg_gs();
                if (tgfx_seg && gs_seg && v2_m2c_base) {   // the guards of v2_draw_flagged_tiles
#ifdef V2_RENDER_FROM_SHADOW
                    fg_tgfx = v2_resolve_segment(tgfx_seg);
                    fg_gs = v2_vm_is_gs_shadow_valid() ? v2_vm_get_shadow_gs() : v2_m2c_base + ((uint32_t)gs_seg << 4);
#else
                    fg_tgfx = v2_m2c_base + ((uint32_t)tgfx_seg << 4);
                    fg_gs = v2_m2c_base + ((uint32_t)gs_seg << 4);
#endif
                }
            }
            if (fg_tgfx && fg_gs) v2_render_tile_masked(buf, fg_tgfx, fg_gs, c.off, x - viewport_x, y - viewport_y);
            continue;
        }
        if (glyph) { v2_raster_glyph(buf, ds_base, c.off, x - viewport_x, y - viewport_y, c.dead, c.x & 7, c.y & 7, rec); continue; }
        v2_raster_sprite(buf, c.type, c.flags, x - viewport_x, y - viewport_y, c.seg, c.off, (int)c.strips, c.slot, 0xFFFF,
                         c.dead, c.x & 7, c.y & 7, c.mand, c.clip_top, c.clip_bot, rec);
    }
}

// ----------------------------------------------------------------------------
// The page lists (render_v2.h): the sprites each of the three VGA pages holds, as the
// pass mirrors put them there and took them away. Page role values 0 / 0x34 / 0x68.
// ----------------------------------------------------------------------------
static V2DrawList g_page_list[3];
static inline int v2_page_idx(uint16_t page) { return page == 0x34 ? 1 : (page == 0x68 ? 2 : 0); }
// records + the used arena only (the struct is ~850 KB)
void v2_drawlist_copy(V2DrawList& dst, const V2DrawList& src) {
    dst.n = src.n;
    dst.arena_used = src.arena_used;
    if (src.n > 0) memcpy(dst.cmd, src.cmd, sizeof(V2DrawCmd) * (size_t)src.n);
    if (src.arena_used > 0) memcpy(dst.arena, src.arena, src.arena_used);
}
// the strip bytes a record's pixels come from: their length by type, and the live bank
// bytes at record time (the rasteriser's own rule: segment + 1-based offset - 1; a glyph's
// 72 bytes from the language page / seg001)
static inline uint16_t v2_cmd_data_len(const V2DrawCmd& c) {
    if (c.type == V2_CMD_GLYPH) return 72;
    if (c.type == 1) return 72;
    if (c.type == 4) return 288;
    if (c.type == 2) return (uint16_t)(36 * (c.strips > 0 ? (c.strips <= 1024 ? c.strips : 1024) : 0));
    return 0;
}
static const uint8_t* v2_cmd_live_data(const V2DrawCmd& c) {
    if (c.type == V2_CMD_GLYPH) {
        extern const uint8_t* v2_glyph_bytes(const uint8_t*, uint16_t);
        return v2_glyph_bytes(v2_get_ds_base(0), c.off);
    }
    if (!c.seg) return nullptr;
#ifdef V2_RENDER_FROM_SHADOW
    uint8_t* seg_base = v2_resolve_segment(c.seg);
    if (!seg_base) return nullptr;
    return seg_base + c.off - 1;
#else
    return v2_m2c_base + ((uint32_t)c.seg << 4) + c.off - 1;
#endif
}
// room in a list's arena for len bytes: compact (the bake drops dead records and rebuilds
// the arena), then fail if the live records alone fill it
static void v2_page_list_bake(uint16_t page, const uint8_t* s);
static bool v2_drawlist_reserve(V2DrawList& L, uint16_t page, uint32_t len) {
    if (L.arena_used + len <= V2_DRAWLIST_ARENA) return true;
    v2_page_list_bake(page, v2_get_ds_base(0));
    return L.arena_used + len <= V2_DRAWLIST_ARENA;
}
// The cells' erase epochs per page, indexed by the render-map word of the cell (fs offset
// LUT_ROW[row] + col * 2, halved — the same offsets the pass mirrors scan). A command is
// dead in a cell whose epoch is newer than the command's own.
static uint32_t g_cell_epoch[3][32768];
static uint32_t g_epoch = 1;
static bool v2_ple_trace_on(void);   // below (V2_PL_ERASE_TRACE)
// The pages' tile words (render_v2.h): per cell the word last painted there; 0xFFFF = the
// map's word (never painted since the fill), 0xFFFE = black (wiped, unpainted).
static uint16_t g_page_tile[3][32768];
static bool g_page_tile_init = false;
static void v2_page_tile_init(void) { if (!g_page_tile_init) { g_page_tile_init = true; memset(g_page_tile, 0xFF, sizeof g_page_tile); } }
// debug: V2_PL_CELL_TRACE=<render-map offset hex> — every tile word written to that cell of
// any page (fills, quadrants, span copies) and the words the composition sees there
static long g_pl_cell_trace = -2;            // the traced render-map word index; -3 = "col,row" given, resolved at the first composition
static int g_pl_cell_col = 0, g_pl_cell_row = 0;
static inline bool v2_pl_cell_on(uint32_t wi) {
    if (g_pl_cell_trace == -2) {
        const char* e = getenv("V2_PL_CELL_TRACE"); g_pl_cell_trace = -1;
        if (e && strchr(e, ',')) { if (sscanf(e, "%d,%d", &g_pl_cell_col, &g_pl_cell_row) == 2) g_pl_cell_trace = -3; }   // a map cell (col,row)
        else if (e) g_pl_cell_trace = strtol(e, nullptr, 16) >> 1;                                                        // a render-map offset (hex)
    }
    return g_pl_cell_trace >= 0 && (long)wi == g_pl_cell_trace;
}
void v2_page_tile_set(uint16_t page, uint16_t fs_off, uint16_t word) {
    v2_page_tile_init();
    g_page_tile[v2_page_idx(page)][(uint16_t)fs_off >> 1] = word;
    if (v2_pl_cell_on((uint16_t)fs_off >> 1)) fprintf(stderr, "V2-PLC f%d tile-set page=%02X fs=%04X word=%04X\n", v2_dbg_pre_vm_iter, page, fs_off, word);
}
void v2_page_tile_set_all(uint16_t fs_off, uint16_t word) {
    v2_page_tile_init();
    for (int p = 0; p < 3; p++) g_page_tile[p][(uint16_t)fs_off >> 1] = word;
    if (v2_pl_cell_on((uint16_t)fs_off >> 1)) fprintf(stderr, "V2-PLC f%d tile-set page=all fs=%04X word=%04X\n", v2_dbg_pre_vm_iter, fs_off, word);
}
// sub_16880: the VGA wiped — every cell black, every list empty
void v2_page_lists_black(void) {
    v2_page_tile_init();
    for (int p = 0; p < 3; p++) { g_page_list[p].n = 0; for (int i = 0; i < 32768; i++) g_page_tile[p][i] = 0xFFFE; }
    if (v2_ple_trace_on()) fprintf(stderr, "V2-PLE f%d 16880 black (all pages)\n", v2_dbg_pre_vm_iter);
}
// the sprite's pixel extent (the rasteriser's rules: type 1 = 8x8, type 2 = 32 x strips, type 4 = 16x16, a glyph cell 8x8)
static inline void v2_cmd_extent(const V2DrawCmd& c, int& w, int& h) {
    if (c.type == 2) { w = 32; h = (int)c.strips; }
    else if (c.type == 1 || c.type == V2_CMD_GLYPH || c.type == V2_CMD_FGTILE) { w = 8; h = 8; }
    else { w = 16; h = 16; }
}
// The background VGA (render_v2.h): the pixel plane of the shadow VGA that only the
// background writers reach — v2_vga_w mirrors a write here while v2_vga_bg_writer is set,
// v2_vga_copy_span / v2_vga_fill_span mirror their spans always.
uint8_t v2_vga_bg[65536 * 4];
bool    v2_vga_bg_writer = false;
thread_local const uint8_t* v2_tls_vga_bg = nullptr;
// sub_16775's CRTC start and pel pan for the camera the DS holds (the shadow-side formula
// of v2_page_flip_16775, repeated here for the presenter's interpolated camera)
void v2_crtc_for_camera(const uint8_t* s, uint32_t* crtc, uint8_t* pan) {
    const uint16_t y_disp = v2gs(s).viewport_y(), y_some = v2gs(s).shake_y();
    const uint16_t y_lvl  = v2gs(s).scroll_limit_y();
    const uint16_t x_disp = v2gs(s).viewport_x(), x_some = v2gs(s).shake_x();
    const uint16_t x_lvl  = v2gs(s).scroll_limit_x();
    const uint16_t page   = v2gs(s).page_shown();
    uint16_t y_off = (uint16_t)(y_disp + y_some);
    if (y_off > y_lvl) y_off = (uint16_t)(y_disp - y_some);
    uint16_t x_off = (uint16_t)(x_disp + x_some);
    if (x_off > x_lvl) x_off = (uint16_t)(x_disp - x_some);
    const uint16_t y_hi = *(const uint16_t*)(s + (uint16_t)(LUT_PAGE_ROW + page + ((y_off >> 3) * 2)));
    const uint16_t y_lo = *(const uint16_t*)(s + (uint16_t)(LUT_SUBROW + (y_off & 7) * 2));
    *crtc = (uint16_t)(y_lo + y_hi + (x_off >> 2) + 8);
    *pan  = (uint8_t)(x_off & 3);
}
// The window read out of the background VGA over the 320 centre columns of the frame: byte
// crtc + y*0x56 + ((pan + x) >> 2), plane (pan + x) & 3 (v2_vga_fetch_page's addressing, with
// the 64K wrap of the address counter). par_on: index 0 stays transparent (the parallax layer
// beneath), as in the tile pass.
static void v2_vga_bg_readout(uint8_t* buf, int fbw, int rows, const uint8_t* bg, uint32_t crtc, uint8_t pan, bool par_on) {
    // The CRTC window starts at the camera's left edge (sub_16775: (x >> 2) + 8 from
    // DS_VIEWPORT_X), and frame column 0 is that edge in every width — the wide frame's
    // camera is the wide window's left edge (v2_view_w), not a 320-px window centred in it.
    // So the readout lands on columns 0..319; the wing beyond comes from the map pass.
    // (2026-09-12: placing it at (fbw - 320) / 2 shifted the whole tile layer of a 384-px
    // frame 32 px right of the sprites — the STRT level in 16:10.)
    const int x0 = 0; (void)fbw;
    for (int y = 0; y < rows; y++) {
        uint8_t* out = buf + (size_t)y * fbw + x0;
        const uint32_t line = crtc + (uint32_t)y * 0x56u;
        for (int x = 0; x < 320; x++) {
            const uint32_t t = (uint32_t)pan + (uint32_t)x;
            const uint8_t px = bg[(uint32_t)(uint16_t)(line + (t >> 2)) * 4u + (t & 3u)];
            if (par_on && (px & 0x0F) == 0) continue;
            out[x] = px;
        }
    }
}
// sub_1E0C7: a glyph cell painted at a map cell of page [92F9]
void v2_page_list_glyph(uint16_t page, int16_t x, int16_t y, uint16_t glyph_index) {
    V2DrawCmd c;
    memset(&c, 0, sizeof c);
    c.slot = 0xFFFF; c.x = x; c.y = y; c.off = glyph_index; c.type = V2_CMD_GLYPH; c.mand = 0xFF; c.late = 1;
    v2_page_list_draw(page, c);
}
// sub_1C8F1: a flagged tile repainted (masked) over a dirty cell of page [92F9]
void v2_page_list_fgtile(uint16_t page, int16_t x, int16_t y, uint16_t tile_word) {
    V2DrawCmd c;
    memset(&c, 0, sizeof c);
    c.slot = 0xFFFE; c.x = x; c.y = y; c.off = tile_word; c.type = V2_CMD_FGTILE; c.mand = 0xFF; c.late = 1;
    v2_page_list_draw(page, c);
}
// A span of cells copied from one page to another (sub_1DE05 pass 2: background → [92F7];
// sub_1DF6A part 2: [92F9] → the rotated-in background at the bit-1 cells): the destination
// cells die, and whatever the source page shows there — its tile words, its commands' pixels
// in those cells — is painted onto the destination: a copy of each source command with pixels
// in the span, its keep mask = the span's cells where the source still showed it.
// the batch of one pass's span copies (render_v2.h): pages, the kill epoch, the spans
static struct { uint16_t dst, src; int di, si; uint32_t epoch; const char* tag; int nspans; bool on;
                struct { uint32_t w0; int n, col, row; } span[1100]; } g_copy;
void v2_page_cells_copy_begin(uint16_t dst, uint16_t src, const char* tag) {
    v2_page_tile_init();
    g_copy.dst = dst; g_copy.src = src; g_copy.di = v2_page_idx(dst); g_copy.si = v2_page_idx(src);
    g_copy.epoch = ++g_epoch; g_copy.tag = tag; g_copy.nspans = 0; g_copy.on = true;
}
void v2_page_cells_copy_span(uint16_t fs_off, int ncells, int map_col, int map_row, const uint8_t* fs) {
    if (!g_copy.on) return;
    const int di = g_copy.di, si = g_copy.si;
    const uint32_t e = g_copy.epoch;
    const uint32_t w0 = (uint32_t)fs_off >> 1;
    for (int i = 0; i < ncells; i++) {
        const uint32_t wi = w0 + (uint32_t)i;
        if (wi >= 32768u) continue;
        g_cell_epoch[di][wi] = e;
        uint16_t tw = g_page_tile[si][wi];
        if (tw == 0xFFFF && fs) tw = *(const uint16_t*)(fs + wi * 2);   // the source shows the map's word as it is now
        g_page_tile[di][wi] = tw;
        if (v2_pl_cell_on(wi)) fprintf(stderr, "V2-PLC f%d %s copy %02X<-%02X fs=%04X word=%04X (src raw %04X)\n", v2_dbg_pre_vm_iter, g_copy.tag, g_copy.dst, g_copy.src, (unsigned)(wi * 2), tw, g_page_tile[si][wi]);
    }
    if (v2_ple_trace_on()) fprintf(stderr, "V2-PLE f%d %s copy %02X<-%02X fs=%04X n=%d cell=(%d,%d) epoch=%u\n", v2_dbg_pre_vm_iter, g_copy.tag, g_copy.dst, g_copy.src, fs_off, ncells, map_col, map_row, e);
    if (g_copy.nspans < 1100) { auto& sp = g_copy.span[g_copy.nspans++]; sp.w0 = w0; sp.n = ncells; sp.col = map_col; sp.row = map_row; }
}
// debug: V2_PL_HIST=<n> — when a list reaches n records, once per page: the records by slot/kind
static void v2_pl_hist(uint16_t page, const V2DrawList& L) {
    static int thr = -2; static bool done[3] = {false, false, false};
    if (thr == -2) { const char* e = getenv("V2_PL_HIST"); thr = e ? atoi(e) : -1; }
    const int pi = v2_page_idx(page);
    if (thr < 0 || L.n < thr || done[pi]) return;
    done[pi] = true;
    int by_slot[0x100 + 1] = {0}, copies = 0, glyphs = 0;
    for (int i = 0; i < L.n; i++) { const V2DrawCmd& c = L.cmd[i]; if (c.late == 2) copies++; if (c.type == V2_CMD_GLYPH) glyphs++; by_slot[c.slot == 0xFFFF ? 0x100 : (c.slot & 0xFF)]++; }
    fprintf(stderr, "V2-PL-HIST f%d page=%02X n=%d arena=%u copies=%d glyphs=%d top:", v2_dbg_pre_vm_iter, page, L.n, L.arena_used, copies, glyphs);
    for (int k = 0; k < 12; k++) { int best = -1, bn = 0; for (int s = 0; s <= 0x100; s++) if (by_slot[s] > bn) { bn = by_slot[s]; best = s; } if (best < 0 || bn == 0) break; fprintf(stderr, " %s%02X:%d", best == 0x100 ? "glyph" : "", best & 0xFF, bn); by_slot[best] = 0; }
    fprintf(stderr, "\n");
}
void v2_page_cells_copy_end(void) {
    if (!g_copy.on) return;
    g_copy.on = false;
    const int di = g_copy.di, si = g_copy.si;
    const V2DrawList& S = g_page_list[si];
    if (S.n == 0 || g_copy.nspans == 0) return;
    V2DrawList& D = g_page_list[di];
    const uint32_t e2 = ++g_epoch;   // the copies are alive from after the kills
    for (int i = 0; i < S.n; i++) {
        const V2DrawCmd& o = S.cmd[i];
        const int cx0 = (int)o.x >> 3, cy0 = (int)o.y >> 3;
        uint64_t keep[4] = {0, 0, 0, 0};
        bool any = false;
        for (int sidx = 0; sidx < g_copy.nspans; sidx++) {
            const auto& sp = g_copy.span[sidx];
            const int r = sp.row - cy0;
            if (r < 0 || r >= 32) continue;
            for (int k = 0; k < sp.n; k++) {
                const int cc = sp.col + k - cx0;
                if (cc < 0 || cc >= 8) continue;
                const uint32_t wi = sp.w0 + (uint32_t)k;
                if (wi >= 32768u) continue;
                const uint64_t bit = (uint64_t)1 << (((r & 7) << 3) | cc);
                if (!(o.keep[r >> 3] & bit)) continue;                 // the source never showed it there
                if (g_cell_epoch[si][wi] > o.epoch) continue;          // the source had restored that cell
                keep[r >> 3] |= bit; any = true;
            }
        }
        if (!any) continue;
        // an earlier copy of the same image on the destination whose cells are all copied again
        // now is redundant (its every cell is re-alive from e2 in the new copy)
        {
            int w = 0;
            for (int j = 0; j < D.n; j++) {
                const V2DrawCmd& p = D.cmd[j];
                bool same = (p.late == 2 && p.slot == o.slot && p.x == o.x && p.y == o.y && p.seg == o.seg && p.off == o.off &&
                             p.flags == o.flags && p.strips == o.strips && p.type == o.type &&
                             p.mand == o.mand && p.clip_top == o.clip_top && p.clip_bot == o.clip_bot &&
                             p.data_len == o.data_len &&
                             (o.data_len == 0 || memcmp(D.arena + p.data_off, S.arena + o.data_off, o.data_len) == 0));
                if (same) { bool sub = true; for (int q = 0; q < 4; q++) if (p.keep[q] & ~keep[q]) { sub = false; break; } if (sub) continue; }
                D.cmd[w++] = p;
            }
            D.n = w;
        }
        V2DrawCmd c = o;
        c.epoch = e2;                // alive on the destination from this copy on
        c.late = 2;                  // a span copy
        memcpy(c.keep, keep, sizeof keep);
        memset(c.dead, 0, sizeof c.dead);
        // the strip bytes travel with the copy (from the source list's arena)
        c.data_len = 0; c.data_off = 0;
        if (o.data_len && v2_drawlist_reserve(D, g_copy.dst, o.data_len)) {
            c.data_off = D.arena_used; c.data_len = o.data_len;
            memcpy(D.arena + D.arena_used, S.arena + o.data_off, o.data_len);
            D.arena_used += o.data_len;
        }
        if (D.n < V2_DRAWLIST_MAX) D.cmd[D.n++] = c;
        else { static int warned = 0; if (warned < 5) { warned++; fprintf(stderr, "V2-PL f%d page=%02X list full, span copy dropped (slot=%02X)\n", v2_dbg_pre_vm_iter, g_copy.dst, o.slot); } }
        v2_pl_hist(g_copy.dst, D);
    }
}
// debug (render_v2.h): V2_PAGELIST_TRACE=<hex slot>
static int g_pl_trace_slot = -2;
bool v2_pl_trace_on(int slot) {
    if (g_pl_trace_slot == -2) { const char* e = getenv("V2_PAGELIST_TRACE"); g_pl_trace_slot = e ? (int)strtol(e, nullptr, 16) : -1; }
    return g_pl_trace_slot >= 0 && slot == g_pl_trace_slot;
}
// Bake the dead masks of a page's commands against the cell epochs (the map's row table
// and clip limits from the DS) and drop every command with no live cell left.
static void v2_page_list_bake(uint16_t page, const uint8_t* s) {
    const int pi = v2_page_idx(page);
    V2DrawList& L = g_page_list[pi];
    const uint32_t* E = g_cell_epoch[pi];
    const int clipx = (int)(int16_t)v2gs(s).clip_limit_x(), clipy = (int)(int16_t)v2gs(s).clip_limit_y();
    // The erased window: sub_1DE05 pass 2 restores 0x2B columns from the camera's scroll column
    // over the page's rows (0x19, 0x1E on an LVX_TALL224 level) from its scroll row and never
    // touches a cell beyond — at 320 px nothing beyond is shown, so a stale sprite image there
    // is the original's own invisible residue. A wide frame shows those cells (the wing past
    // 344 px), where a record would then live for ever: a moving sprite left its earlier
    // frames behind (2026-09-12: coloured fragments beside Erik's head at the page's edge in
    // 16:10, a garbled double viking in 16:9). Outside the window a record's cell stays alive
    // only while the record is its slot's newest — the object as it stands; a static sprite
    // drawn once keeps its cells, a moving one leaves nothing behind.
    const int win_c0 = (int)(int16_t)v2gs(s).scroll_col(), win_r0 = (int)(int16_t)v2gs(s).scroll_row();
    const int win_c1 = win_c0 + 0x2B, win_r1 = win_r0 + (v2_view_rows() == 224 ? 0x1E : 0x19);
    int w = 0;
    for (int i = 0; i < L.n; i++) {
        V2DrawCmd c = L.cmd[i];
        int ew, eh; v2_cmd_extent(c, ew, eh);
        const int cx0 = (int)c.x >> 3, cy0 = (int)c.y >> 3;          // SAR: the orig's cell of a pixel
        int ncols = ((c.x & 7) + ew + 7) >> 3, nrows = ((c.y & 7) + eh + 7) >> 3;
        if (ncols > 8) ncols = 8;
        if (nrows > 32) nrows = 32;
        memset(c.dead, 0, sizeof c.dead);
        int alive = 0;
        uint32_t killer = 0;   // debug: the newest erase epoch among the dead cells
        int newest = -1;       // is this record its slot's newest one? (decided on the first cell outside the erased window)
        for (int r = 0; r < nrows; r++) {
            const int row = cy0 + r;
            if (row < 0 || row >= clipy) { alive += ncols; continue; }   // off the map: the erase clips there, nothing restores such a cell
            const uint16_t rowbase = *(const uint16_t*)(s + (uint16_t)(row * 2 - LUT_ROW_BASE));   // the row's first cell INDEX (words; the mirrors add the column, then shift left)
            for (int cc = 0; cc < ncols; cc++) {
                const int col = cx0 + cc;
                if (col < 0 || col >= clipx) { alive++; continue; }
                if (c.slot <= 0xFE && (col >= win_c1 || row >= win_r1)) {   // beyond the erased window (the wing)
                    if (newest < 0) { newest = 1; for (int j = i + 1; j < L.n; j++) if (L.cmd[j].slot == c.slot) { newest = 0; break; } }
                    if (!newest) { c.dead[r >> 3] |= (uint64_t)1 << (((r & 7) << 3) | cc); continue; }
                }
                const uint32_t wi = (uint16_t)((rowbase + col) << 1) >> 1;
                const uint64_t bit = (uint64_t)1 << (((r & 7) << 3) | cc);
                if (!(c.keep[r >> 3] & bit)) { c.dead[r >> 3] |= bit; continue; }   // outside a span copy's cells
                if (E[wi] > c.epoch) { c.dead[r >> 3] |= bit; if (E[wi] > killer) killer = E[wi]; }
                else alive++;
            }
        }
        if (v2_pl_trace_on(c.slot)) {
            const uint8_t* rd = c.data_len ? L.arena + c.data_off : nullptr;
            const uint8_t* lv = v2_cmd_live_data(c);
            fprintf(stderr, "V2-PL f%d bake page=%02X slot=%02X xy=(%d,%d) seg=%04X off=%04X fl=%04X epoch=%u late=%d clip=%02X/%d/%d cells=%dx%d alive=%d keep=%016llx dead=%016llx killer-epoch=%u%s data[%u]=%02X%02X%02X%02X%02X%02X%02X%02X%02X live=%02X%02X%02X%02X%02X%02X%02X%02X%02X\n",
                    v2_dbg_pre_vm_iter, page, c.slot, c.x, c.y, c.seg, c.off, c.flags, c.epoch, (int)c.late, c.mand, (int)c.clip_top, (int)c.clip_bot, ncols, nrows, alive,
                    (unsigned long long)c.keep[0], (unsigned long long)c.dead[0], killer, alive == 0 ? " dropped" : "", (unsigned)c.data_len,
                    rd ? rd[0] : 0, rd ? rd[1] : 0, rd ? rd[2] : 0, rd ? rd[3] : 0, rd ? rd[4] : 0, rd ? rd[5] : 0, rd ? rd[6] : 0, rd ? rd[7] : 0, rd ? rd[8] : 0,
                    lv ? lv[0] : 0, lv ? lv[1] : 0, lv ? lv[2] : 0, lv ? lv[3] : 0, lv ? lv[4] : 0, lv ? lv[5] : 0, lv ? lv[6] : 0, lv ? lv[7] : 0, lv ? lv[8] : 0);
        }
        // debug: V2_PL_WINGDUMP=<frame> — every record of the shown page that reaches beyond the
        // erased window at that frame, with the slot's current position in the DS
        { static int wd = -1, wd_f = 0; if (wd < 0) { const char* e = getenv("V2_PL_WINGDUMP"); wd = 0; if (e && *e) { wd = 1; wd_f = atoi(e); } }
          if (wd == 1 && v2_dbg_pre_vm_iter == wd_f && (cx0 + ncols > win_c1 || cy0 + nrows > win_r1)) {
              const int sx = c.slot <= 0xFE ? (int)(int16_t)*(const uint16_t*)(s + (uint16_t)(c.slot + OBJ_SPRITE_X)) : -1;
              const int sy = c.slot <= 0xFE ? (int)(int16_t)*(const uint16_t*)(s + (uint16_t)(c.slot + OBJ_SPRITE_Y)) : -1;
              const int sfl = c.slot <= 0xFE ? (int)*(const uint16_t*)(s + (uint16_t)(c.slot + OBJ_SPRITE_FLAGS)) : -1;
              fprintf(stderr, "V2-PLW f%d page=%02X i=%d slot=%02X type=%d xy=(%d,%d) off=%04X strips=%d epoch=%u newest=%d alive=%d ds_xy=(%d,%d) ds_fl=%04X win=[%d..%d)x[%d..%d)\n",
                      v2_dbg_pre_vm_iter, page, i, c.slot, (int)c.type, c.x, c.y, c.off, (int)c.strips, c.epoch, newest, alive, sx, sy, sfl, win_c0, win_c1, win_r0, win_r1);
          } }
        if (alive == 0) continue;
        L.cmd[w++] = c;
    }
    L.n = w;
    // the arena follows the surviving records (dropped ones leave holes; dedupe too)
    {
        static uint8_t scratch[V2_DRAWLIST_ARENA];
        uint32_t used = 0;
        for (int i = 0; i < L.n; i++) {
            V2DrawCmd& c = L.cmd[i];
            if (!c.data_len) continue;
            memcpy(scratch + used, L.arena + c.data_off, c.data_len);
            c.data_off = used;
            used += c.data_len;
        }
        if (used) memcpy(L.arena, scratch, used);
        L.arena_used = used;
    }
}
// sub_1DE05 pass 2: a span of window cells on page [92F7] is restored from the background
// debug: V2_PL_ERASE_TRACE=<from>-<to> — every erase of the game-frame range, with its writer
static bool v2_ple_trace_on(void) {
    static int on = -1, f0 = 0, f1 = -1;
    if (on < 0) { const char* e = getenv("V2_PL_ERASE_TRACE"); on = 0; if (e && sscanf(e, "%d-%d", &f0, &f1) == 2) on = 1; }
    return on == 1 && v2_dbg_pre_vm_iter >= f0 && v2_dbg_pre_vm_iter <= f1;
}
void v2_page_list_erase_cells(uint16_t page, uint16_t fs_off, int ncells, const char* tag) {
    uint32_t* E = g_cell_epoch[v2_page_idx(page)];
    const uint32_t e = ++g_epoch;
    for (int i = 0; i < ncells; i++) {
        const uint32_t wi = ((uint32_t)fs_off >> 1) + (uint32_t)i;
        if (wi < 32768u) E[wi] = e;
    }
    if (v2_ple_trace_on()) fprintf(stderr, "V2-PLE f%d %s page=%02X fs=%04X n=%d epoch=%u\n", v2_dbg_pre_vm_iter, tag, page, fs_off, ncells, e);
}
// tiles painted over all three pages (scroll-in rows/columns, the full fill): the cells die everywhere
void v2_page_lists_erase_cells_all(uint16_t fs_off, int ncells, uint16_t step, const char* tag) {
    const uint32_t e = ++g_epoch;
    for (int i = 0; i < ncells; i++) {
        const uint32_t wi = (uint16_t)(fs_off + (uint16_t)(i * step)) >> 1;
        for (int p = 0; p < 3; p++) g_cell_epoch[p][wi] = e;
    }
    if (v2_ple_trace_on()) fprintf(stderr, "V2-PLE f%d %s page=all fs=%04X n=%d step=%u epoch=%u\n", v2_dbg_pre_vm_iter, tag, fs_off, ncells, step, e);
}
// sub_1DD9C: the object is drawn on this page — appended with the next epoch; the slot's
// earlier commands stay until their cells are restored (their surviving pixels are the
// orig's residue under the new image)
void v2_page_list_draw(uint16_t page, const V2DrawCmd& cmd0) {
    V2DrawList& L = g_page_list[v2_page_idx(page)];
    V2DrawCmd cmd = cmd0;
    cmd.epoch = ++g_epoch;
    memset(cmd.dead, 0, sizeof cmd.dead);
    memset(cmd.keep, 0xFF, sizeof cmd.keep);   // a draw may show every one of its cells
    // the record's strip bytes, copied into the arena (the page keeps what was painted)
    cmd.data_len = 0; cmd.data_off = 0;
    {
        const uint16_t len = v2_cmd_data_len(cmd);
        const uint8_t* src = len ? v2_cmd_live_data(cmd) : nullptr;
        if (src && v2_drawlist_reserve(L, page, len)) {
            cmd.data_off = L.arena_used; cmd.data_len = len;
            memcpy(L.arena + L.arena_used, src, len);
            L.arena_used += len;
        } else if (len) {
            static int warned = 0;
            if (warned < 5) { warned++; fprintf(stderr, "V2-PL f%d page=%02X no arena room / no bank for slot=%02X type=%d len=%u: the record reads the live bank\n", v2_dbg_pre_vm_iter, page, cmd.slot, (int)cmd.type, (unsigned)len); }
        }
    }
    // The same image at the same place again (a static object the scan repaints every pass,
    // a looping animation back on a frame): the new draw covers every pixel the earlier one
    // could still show, so the earlier record is redundant — dropped, pixel for pixel the
    // page is the same. This bounds a page's list by its distinct images. (A span copy of the
    // same image is covered too: its keep is a subset of the full draw's.) "The same image"
    // means the same BYTES: a sprite whose frames are written into one fixed slot of the bank
    // (same segment:offset, new data — a puff of smoke) leaves the earlier frame's pixels
    // under the new one where the new one is transparent, so those records stay.
    {
        int w = 0;
        for (int i = 0; i < L.n; i++) {
            const V2DrawCmd& o = L.cmd[i];
            if (o.slot == cmd.slot && o.x == cmd.x && o.y == cmd.y && o.seg == cmd.seg && o.off == cmd.off &&
                o.flags == cmd.flags && o.strips == cmd.strips && o.type == cmd.type &&
                o.mand == cmd.mand && o.clip_top == cmd.clip_top && o.clip_bot == cmd.clip_bot &&
                o.data_len == cmd.data_len &&
                (cmd.data_len == 0 || memcmp(L.arena + o.data_off, L.arena + cmd.data_off, cmd.data_len) == 0)) continue;
            L.cmd[w++] = o;
        }
        L.n = w;
    }
    if (L.n >= V2_DRAWLIST_MAX) {
        v2_page_list_bake(page, v2_get_ds_base(0));                 // drop what is fully erased
        if (L.n >= V2_DRAWLIST_MAX) {                                // still full: the oldest goes (a divergence — report it)
            static int warned = 0;
            if (warned < 5) { warned++; fprintf(stderr, "V2-PL f%d page=%02X list full, oldest command dropped (slot=%02X)\n", v2_dbg_pre_vm_iter, page, L.cmd[0].slot); }
            memmove(&L.cmd[0], &L.cmd[1], sizeof(V2DrawCmd) * (V2_DRAWLIST_MAX - 1));
            L.n = V2_DRAWLIST_MAX - 1;
        }
    }
    L.cmd[L.n++] = cmd;
    v2_pl_hist(page, L);
    if (v2_pl_trace_on(cmd.slot)) fprintf(stderr, "V2-PL f%d draw page=%02X slot=%02X xy=(%d,%d) off=%04X t=%d epoch=%u n=%d\n", v2_dbg_pre_vm_iter, page, cmd.slot, cmd.x, cmd.y, cmd.off, cmd.type, cmd.epoch, L.n);
}
// sub_1DF6A part 2: the rotated-in background page is cleaned of every cell that ever
// carried a sprite (render-map bit 1) — no sprite survives on it
void v2_page_list_clear(uint16_t page) {
    if (g_pl_trace_slot >= 0) fprintf(stderr, "V2-PL f%d clear page=%02X n=%d\n", v2_dbg_pre_vm_iter, page, g_page_list[v2_page_idx(page)].n);
    g_page_list[v2_page_idx(page)].n = 0;
}
// sub_16880: the level transition clears the VGA
void v2_page_lists_clear_all(void) {
    if (g_pl_trace_slot >= 0) fprintf(stderr, "V2-PL f%d clear-all\n", v2_dbg_pre_vm_iter);
    for (int i = 0; i < 3; i++) g_page_list[i].n = 0;
}

// The frame of a flip: what the shown page holds. Tiles from the map at the camera (the
// chunk picture on a chunk screen), the page's sprite list in its order, the flagged
// tiles, the UI — into v2_render_buf, and the list becomes the flip's display list
// (v2_frame_draws) for the presenter. Called by the sub_16775 mirror before it publishes;
// the layer gates admit it outside a frame (blocking loops flip without a frame).
void v2_compose_page(uint16_t ds_val, uint16_t page) {
    if (!v2_m2c_base || !myDrawInfo_v2) return;
    v2_compose_at_flip = true;
    v2_page_tile_init();
    v2_tile_override = g_page_tile[v2_page_idx(page)];   // the page's own tile words (render_v2.h)
    if (v2_ple_trace_on()) {   // debug V2_PL_ERASE_TRACE: the flip in the writers' order — page, camera, window origin, level
        const uint8_t* s = v2_get_ds_base(ds_val);
        fprintf(stderr, "V2-PLE f%d 16775 flip page=%02X pages=%02X/%02X/%02X cam=(%d,%d) win=(%d,%d) lvl=%04X flags=%02X n=%d\n", v2_dbg_pre_vm_iter, page,
                *(const uint16_t*)(s + 0x92F7), *(const uint16_t*)(s + 0x92F9), *(const uint16_t*)(s + 0x92FB),
                *(const int16_t*)(s + 0x44), *(const int16_t*)(s + 0x46), *(const int16_t*)(s + 0x257F), *(const int16_t*)(s + 0x2581),
                *(const uint16_t*)(s + DS_LEVEL), s[DS_LEVEL_FLAGS], g_page_list[v2_page_idx(page)].n);
    }
    v2_tls_ui_cells_from_page = true;                     // the text cells are the page's glyph commands
    v2_draw_tiles(ds_val);
    v2_page_list_bake(page, v2_get_ds_base(ds_val));   // the dead masks as of this flip; fully erased commands go
    if (g_pl_cell_trace == -3) {   // debug V2_PL_CELL_TRACE=<col>,<row>: resolve the cell through the map's row table
        const uint8_t* s = v2_get_ds_base(ds_val);
        const uint16_t rowbase = *(const uint16_t*)(s + (uint16_t)(g_pl_cell_row * 2 - LUT_ROW_BASE));
        g_pl_cell_trace = (uint16_t)((rowbase + g_pl_cell_col) << 1) >> 1;
        fprintf(stderr, "V2-PLC f%d cell (%d,%d) = render-map offset %04lX\n", v2_dbg_pre_vm_iter, g_pl_cell_col, g_pl_cell_row, g_pl_cell_trace * 2);
    }
    if (g_pl_cell_trace >= 0) {   // debug V2_PL_CELL_TRACE: the composed page's word for the cell and the map's
        const uint8_t* fsb = v2_tls_fs ? v2_tls_fs : nullptr;
        extern uint8_t v2_vm_shadow_fs[];
        if (!fsb) fsb = v2_vm_shadow_fs;
        fprintf(stderr, "V2-PLC f%d compose page=%02X word=%04X map=%04X\n", v2_dbg_pre_vm_iter, page,
                g_page_tile[v2_page_idx(page)][g_pl_cell_trace], *(const uint16_t*)(fsb + g_pl_cell_trace * 2));
    }
    const V2DrawList& L = g_page_list[v2_page_idx(page)];
    if (g_pl_trace_slot >= 0) {
        int at = -1;
        for (int i = 0; i < L.n; i++) if (L.cmd[i].slot == (uint16_t)g_pl_trace_slot) at = i;
        if (at >= 0) fprintf(stderr, "V2-PL f%d compose page=%02X n=%d slot=%02X at=%d xy=(%d,%d) off=%04X\n", v2_dbg_pre_vm_iter, page, L.n, g_pl_trace_slot, at, L.cmd[at].x, L.cmd[at].y, L.cmd[at].off);
        else fprintf(stderr, "V2-PL f%d compose page=%02X n=%d slot=%02X absent\n", v2_dbg_pre_vm_iter, page, L.n, g_pl_trace_slot);
    }
    if (!v2_parallax.on) {
        // the exact page: sprites, sub_1C8F1's flagged-tile repaints and sub_1E0C7's glyph cells
        // in the order the passes painted them (the flagged pass below keeps only its parallax
        // part, none here)
        v2_tls_fg_from_page = true;
        v2_draw_list(L, nullptr, nullptr, -1);
        v2_draw_flagged_tiles(ds_val);
        v2_tls_fg_from_page = false;
    } else {
        // a level with the console parallax layer: its priority pass sits between the sprites
        // and the flagged tiles / glyphs (a console rule with no original to match)
        v2_draw_list(L, nullptr, nullptr, 0);     // the page's sprites, in their order
        v2_draw_flagged_tiles(ds_val);            // the parallax priority pass + the map's flagged tiles
        v2_draw_list(L, nullptr, nullptr, 1);     // sub_1E0C7: the page's glyph cells over those
    }
    v2_draw_ui(ds_val);                       // the CJK overlay only (v2_tls_ui_cells_from_page)
    v2_drawlist_copy(v2_frame_draws, L);      // the flip's list for the presenter (records + arena)
    v2_tls_ui_cells_from_page = false;
    v2_compose_at_flip = false;
}

// late_gate=1: repaint only what orig sub_1dd9c draws in this sub-frame —
// gates evaluated BEFORE v2_late_sprites_1DD9C's DS effects (DEC of [obj+0x114D]) and
// BEFORE v2_dirty_tile_scan_1C8F1 (which clears render-map bit0), matching the orig call
// order 1dd9c -> 1c8f1. sub_1cdef gate: clip the object's tile bbox to the
// viewport (ds:0x9168/0x916A) and scan its cells in the render map (FS) for
// bit0 (seg003 eip 0x634: TEST word fs:[si],1) — set means this sub-frame's
// sub_1c8f1 repaints the cell, so the sprite must be repainted over it.
static void v2_draw_sprites_impl(uint16_t ds_val, int late_gate, int only_obj) {
    // debug (V2_SPR_TRACE range): the layer's entry with its gates — a pass that draws nothing
    // shows here as in_frame=0 (the blocking loops of the game build) or base=0
    { static int tr = -1; static int f0 = 0, f1 = -1;
      if (tr < 0) { const char* e = getenv("V2_SPR_TRACE"); tr = 0; if (e && sscanf(e, "%d-%d", &f0, &f1) == 2) tr = 1; }
      if (tr && !v2_tls_presenter && v2_dbg_pre_vm_iter >= f0 && v2_dbg_pre_vm_iter <= f1)
          fprintf(stderr, "V2-SPRPASS f%d %s in_frame=%d base=%d late_list=%d/%d\n", v2_dbg_pre_vm_iter, late_gate ? "late " : "early",
                  (int)v2_vm_in_frame, (int)(v2_m2c_base != nullptr && myDrawInfo_v2 != nullptr), (int)g_late_valid, g_late_n); }
#ifdef V2_RENDER_FROM_SHADOW
    if (!v2_vm_in_frame && !v2_tls_presenter && !v2_compose_at_flip) return;
#endif
    if (!v2_m2c_base || !myDrawInfo_v2) return;

    uint8_t* ds_base = v2_get_ds_base(ds_val);
    V2StateViewC st(ds_base);   // phase D: typed field access

    uint8_t* buf = v2_tls_out ? v2_tls_out : v2_render_buf;   // UX stage 9

    // Viewport origin — pixel scroll values.
    // Mirrors orig set_display_memory_addr (sub_16775) — apply x_some/y_some shake.
    // Sprites in orig are drawn at world_pos - vp_px in VGA buffer; CRTC then shifts
    // entire display by x_some via start address + pixel pan. v2 single-buffer:
    // bake the shake into the effective camera so all elements stay aligned.
    // №60: dw-exact shake fold (see v2_effective_camera). Sprites subtract
    // the full effective origin (screen = world - camera); int locals keep
    // the downstream signed screen math unchanged.
    V2Camera cam = v2_effective_camera(ds_base);
    int viewport_x = (int)(int16_t)cam.x_eff;
    int viewport_y = (int)(int16_t)cam.y_eff;
    static int spr_dbg = 0; spr_dbg++;
    // The late layer draws the pass's own late set when the sub_1dd9c mirror reported it
    // (v2_late_list_begin/add, the mirror's dispatch order) — the decision the pass made,
    // not a re-derivation from the state after it (a count that reached 0 in the pass, a
    // force flag the pass cleared: the gate below cannot see those any more).
    if (late_gate && g_late_valid && only_obj < 0) {
        g_late_valid = false;
        for (int i = 0; i < g_late_n; i++)
            v2_draw_sprite_obj(buf, ds_base, st, viewport_x, viewport_y, g_late_slots[i], 1);
        return;
    }
    for (int obj = 0xFE; obj >= 0; obj -= 2) {
        if (only_obj >= 0 && obj != only_obj) continue;
        uint16_t flags = *(uint16_t*)(ds_base + obj + OBJ_SPRITE_FLAGS);

        // Must be active (bit 15) with bits 13-14 clear
        if (!(flags & 0x8000) || (flags & 0x6000)) continue;

        // Stage 5.1 tail: sprite-usage trace (offline PNG slicing map).
        {
            static FILE* _st = nullptr; static int _sts = -1;
            if (_sts < 0) { const char* e = getenv("V2_SPRITE_TRACE");
                _sts = (e && *e) ? 1 : 0; if (_sts) _st = fopen(e, "a"); }
            if (_sts && _st) {
                uint16_t t_off = *(uint16_t*)(ds_base + obj + OBJ_SPRITE_OFF);
                int t_type = flags & 7;
                uint16_t t_strips = *(uint16_t*)(ds_base + obj + OBJ_STRIP_COUNT);
                fprintf(_st, "S %04X %d %u\n", t_off, t_type, t_strips);
            }
        }

        // Task #21 ring: v2 layer decisions for the traced object; d = checksum
        // of the FULL sprite data as THIS side reads them (shadow) — same size
        // formula as the orig-side probe in sub_1dd9c.
        if (obj == v2_objtrace_di) {
            uint16_t t_seg = *(uint16_t*)(ds_base + obj + OBJ_SPRITE_SEG);
            uint16_t t_off = *(uint16_t*)(ds_base + obj + OBJ_SPRITE_OFF);
            int t_type = flags & 7;
            int t_sz = (t_type == 1) ? 72 : (t_type == 4) ? 288
                       : 4 * (int)*(uint16_t*)(ds_base + obj + OBJ_STRIP_COUNT) * 9;
            if (t_sz < 1) t_sz = 1; if (t_sz > 4096) t_sz = 4096;
            uint8_t* t_base = v2_resolve_segment(t_seg);
            if (!t_base) t_base = v2_m2c_base + ((uint32_t)t_seg << 4);
            int t_h = 0;
            for (int t_i = 0; t_i < t_sz; t_i++) t_h += t_base[(uint16_t)(t_off - 1 + t_i)];
            int t_slot = 9;   // 9 = display buffer
            v2_objtrace(late_gate ? "v:late" : "v:early",
                        *(int16_t*)(ds_base + obj + OBJ_SPRITE_X),
                        *(int16_t*)(ds_base + obj + OBJ_SPRITE_Y),
                        t_slot * 256 + ds_base[obj + OBJ_DIRTY_MODE], t_h);
        }

        if (late_gate) {
            // orig sub_1dd9c gates (eips 0x157F..0x1590), checked in orig order:
            // force flag / pending-redraw byte / sub_1cdef render-map scan.
            bool draw_it = false;
            int gate_code = 0;   // 1=force 2=rd 3=scan-hit (trace aid)
            if (v2gs(ds_base).sprite_force_b() != 0) { draw_it = true; gate_code = 1; }  // TEST ds:9568h
            else if (ds_base[obj + OBJ_DIRTY_MODE] != 0) { draw_it = true; gate_code = 2; } // TEST byte [di+114Dh]
            else {
                // sub_1cdef: clip object's tile bbox to viewport, scan cells
                // in the render map (FS) for bit0. Exact replica (seg003
                // eips 0x5BF..0x647); the v2_late_sprites_1DD9C bounds mirror carries
                // the same math for its DS effects.
                int16_t cx = (int16_t)*(uint16_t*)(ds_base + obj + OBJ_SPRITE_X);
                int16_t dxv = (int16_t)*(uint16_t*)(ds_base + obj + OBJ_SPRITE_Y);
                int16_t si_h = (int16_t)((*(uint16_t*)(ds_base + obj + OBJ_STRIP_COUNT) >> 3) + 1);
                int16_t bp_w = si_h;
                if (!(cx & 7)) bp_w--;
                if (!(dxv & 7)) si_h--;
                cx >>= 3; dxv >>= 3;                                    // SAR — signed
                int16_t ax_h = si_h;
                int16_t si_off = 0;
                bool visible = true;
                int16_t vw = (int16_t)st.clip_limit_x();
                if (cx < 0) { bp_w += cx; if (bp_w <= 0) visible = false; }
                else {
                    si_off += cx;
                    if (cx >= vw) visible = false;
                    else { int16_t ov = cx + bp_w - vw; if (ov > 0) bp_w -= ov; }
                }
                if (visible) {
                    int16_t vh = (int16_t)st.clip_limit_y();
                    if (dxv < 0) { ax_h += dxv; if (ax_h <= 0) visible = false; }
                    else {
                        if (dxv >= vh) visible = false;
                        else {
                            uint16_t bx = (uint16_t)dxv << 1;
                            si_off += (int16_t)*(uint16_t*)(ds_base + (uint16_t)(bx - LUT_ROW_BASE));
                            int16_t ov = dxv + ax_h - vh; if (ov > 0) ax_h -= ov;
                        }
                    }
                }
                if (visible) {
                    uint16_t fs_seg = st.seg_fs();
#ifdef V2_RENDER_FROM_SHADOW
                    uint8_t* fs_base = v2_resolve_segment(fs_seg);
                    if (!fs_base) fs_base = v2_m2c_base + ((uint32_t)fs_seg << 4);
#else
                    uint8_t* fs_base = v2_m2c_base + ((uint32_t)fs_seg << 4);
#endif
                    // loc_1ce58 (eips 0x628-0x643): dx = (vw - bp) doubled
                    // (ADD dx,dx @0x62E) and si doubled (ADD si,si @0x630) —
                    // si becomes the BYTE offset (row_base+col)*2, cells step
                    // ADD si,2, row tail ADD si,dx. Below si_w stays in WORD
                    // units with off = (si_w+col)*2 and si_w += bp+stride
                    // per row — byte-for-byte the same addresses.
                    uint16_t si_w = (uint16_t)si_off;
                    int16_t stride = vw - bp_w;
                    bool hit = false;
                    for (int16_t row = 0; row < ax_h && !hit; row++) {
                        for (int16_t col = 0; col < bp_w; col++) {
                            uint16_t off = (uint16_t)(si_w + col) * 2u;
                            if (*(uint16_t*)(fs_base + off) & 1) { hit = true; break; }
                        }
                        si_w = (uint16_t)(si_w + bp_w + stride);
                    }
                    draw_it = hit;
                    if (hit) gate_code = 3;
                }
            }
            // Gate-fact trace: slot*4096 + gate*256 + [114D] — logged for the
            // traced object whether it draws or not (v:gate = decision point).
            // d = the FS cell word of the object's first cell at scan time.
            if (obj == v2_objtrace_di) {
                int t_slot = 9;
                int fs_word = -1;
                {
                    uint16_t fs_seg2 = st.seg_fs();
                    uint8_t* fsb2 = v2_resolve_segment(fs_seg2);
                    if (!fsb2) fsb2 = v2_m2c_base + ((uint32_t)fs_seg2 << 4);
                    int16_t oy2 = (int16_t)*(uint16_t*)(ds_base + obj + OBJ_SPRITE_Y);
                    int16_t ox2 = (int16_t)*(uint16_t*)(ds_base + obj + OBJ_SPRITE_X);
                    uint16_t row2 = (uint16_t)((oy2 + 8) >> 3);
                    uint16_t rb2 = *(uint16_t*)(ds_base + (uint16_t)(row2 * 2 - LUT_ROW_BASE));
                    uint16_t moff2 = (uint16_t)((rb2 + (uint16_t)((ox2 + 8) >> 3)) * 2u);
                    fs_word = *(uint16_t*)(fsb2 + moff2);
                }
                v2_objtrace("v:gate",
                            *(int16_t*)(ds_base + obj + OBJ_SPRITE_X),
                            *(int16_t*)(ds_base + obj + OBJ_SPRITE_Y),
                            t_slot * 4096 + gate_code * 256 + ds_base[obj + OBJ_DIRTY_MODE],
                            fs_word);
            }
            if (!draw_it) continue;
        }

        // the format dispatch, the record and the rasterisation: v2_draw_sprite_obj
        v2_draw_sprite_obj(buf, ds_base, st, viewport_x, viewport_y, obj, late_gate);
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

// Render one 8x8 map tile with its per-plane transparency mask to `buf` at
// screen (screen_x, screen_y). v2_put_pixel applies the emu-page blit
// translation (v2_pp_dx/dy/pitch) when a page is the target. Shared by
// v2_draw_flagged_tiles and the dirty channel (v2_masked_tile_1C939) so both
// decode identically (orig sub_1C939 jpt_1c9b1/caa7/cba1/cc9b flip dispatch).
static void v2_render_tile_masked(uint8_t* buf, const uint8_t* tgfx_base,
                                  const uint8_t* gs_base, uint16_t tile_entry,
                                  int screen_x, int screen_y) {
    uint16_t tile_gfx_off = tile_entry & 0xFFC0;
    bool hflip = (tile_entry & 0x10) != 0;
    bool vflip = (tile_entry & 0x20) != 0;
    const uint8_t* tile = tgfx_base + tile_gfx_off;
    const uint8_t* mask_data = gs_base + (tile_gfx_off >> 3);
    for (int ty = 0; ty < 8; ty++) {
        int sy = screen_y + (vflip ? 7 - ty : ty);
        for (int tx = 0; tx < 8; tx++) {
            int sx = screen_x + (hflip ? 7 - tx : tx);
            int plane = tx & 3;       // tx % 4
            int byte_idx = tx >> 2;   // tx / 4 (0 or 1)
            int strip = ty >> 2;      // ty / 4 (0 or 1)
            int row = ty & 3;         // ty % 4
            int mb = plane * 2 + strip;
            int mbit = 7 - (row * 2 + byte_idx);
            if (!(mask_data[mb] & (1 << mbit))) continue;
            uint8_t color = tile[plane * 16 + strip * 8 + row * 2 + byte_idx];
            v2_put_pixel(buf, sx, sy, color);
        }
    }
}

void v2_draw_flagged_tiles(uint16_t ds_val) {
#ifdef V2_RENDER_FROM_SHADOW
    if (!v2_vm_in_frame && !v2_tls_presenter && !v2_compose_at_flip) return;
#endif
    if (!myDrawInfo_v2 || !v2_m2c_base) return;

    uint8_t* ds_base = v2_get_ds_base(ds_val);
    V2StateViewC st(ds_base);   // phase D: typed field access
    uint8_t* buf = v2_tls_out ? v2_tls_out : v2_render_buf;   // UX stage 9

    uint16_t fs_seg = st.seg_fs();
    uint16_t tgfx_seg = st.seg_tilegfx();
    uint16_t gs_seg = st.seg_gs();
    if (!fs_seg || !tgfx_seg || !gs_seg) return;
    // UX stage 9: the parallax layer's priority-1 cells over the sprites,
    // under the flagged tiles (see v2_draw_parallax_pass); only a tile
    // frame has drawn the layer's lower pass this tick
    if (v2_last_frame_tiles || v2_tls_presenter) v2_draw_parallax_pass(st, buf, 0x2000);
    if (v2_tls_fg_from_page) return;   // the flagged tiles are the page's V2_CMD_FGTILE commands (render_v2.h)

#ifdef V2_RENDER_FROM_SHADOW
    uint8_t* fs_base = v2_resolve_segment(fs_seg);
    uint8_t* tgfx_base = v2_resolve_segment(tgfx_seg);
    if (v2_tls_fs) fs_base = (uint8_t*)v2_tls_fs;   // UX stage 9: the presenter's FS snapshot
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

    uint16_t scroll_x = st.scroll_row();
    uint16_t scroll_y = st.scroll_col();

    // Sub-tile pixel offset (same as v2_draw_tiles).
    // Mirrors orig set_display_memory_addr (sub_16775) — apply x_some/y_some shake.
    // №60: dw-exact shake fold (see v2_effective_camera).
    V2Camera cam = v2_effective_camera(ds_base);
    int pix_off_x = cam.pix_off_x, pix_off_y = cam.pix_off_y;
    int extra_tile_x = cam.tile_shift_x, extra_tile_y = cam.tile_shift_y;

    for (int row_vis = 0; row_vis < v2_tile_rows; row_vis++) {
        uint16_t row_scrolled = (uint16_t)(row_vis + scroll_x + extra_tile_y);
        // No `row_scrolled >= 64` clamp — orig sub_1c8f1 (flagged-tile render)
        // doesn't clip rows beyond LUT; v2 hardcode caused tiles missing on
        // large maps when scrolled past row 64.

        uint16_t lut_off = (uint16_t)(row_scrolled * 2u - LUT_ROW_BASE);
        uint16_t row_base = *(uint16_t*)(ds_base + lut_off);

        for (int col_vis = 0; col_vis < v2_fb_cols(); col_vis++) {
            uint16_t col_scrolled = (uint16_t)(col_vis + scroll_y + extra_tile_x);
            uint16_t tile_map_off = (uint16_t)((row_base + col_scrolled) * 2u);
            uint16_t tile_entry = *(uint16_t*)(fs_base + tile_map_off);
            // The page lists (render_v2.h): the composed page's own word for the cell
            {
                const uint16_t* ovr = v2_tls_presenter ? v2_tls_tile_ovr : v2_tile_override;
                // The page holds 43 columns of cells from the camera's scroll column and 25 rows
                // from its scroll row — 30 on an LVX_TALL224 level (the sub_16DED fill: 0x2B cells
                // per row, 0x19 rows, 0x1E when v2_view_h_cur is 224; the scroll painters keep
                // that window). A cell outside it never lies on any page — the wing of a wide
                // frame beyond 344 px — so the page's word (0xFFFE "wiped, black" included) does
                // not apply there: the map tile does. (2026-09-12: the 16:10 frame showed black
                // from column 344 on.)
                // Of the 43 painted columns the original ever shows 41 (320 px = 40 cells, 41 with
                // a pel pan); columns 41 and 42 are slack the scroll painters do not keep in every
                // path (level 2 in 16:10: cells 41..42 of a row still 0xFFFE from the wipe — a black
                // 16x8 block at the ladder's foot), so the page's word is trusted for columns 0..40.
                if (ovr && col_vis < 0x29 && row_vis < (v2_view_rows() == 224 ? 0x1E : 0x19)) {
                    const uint16_t w = ovr[tile_map_off >> 1];
                    if (w == 0xFFFE) continue;          // black cell: nothing painted there yet
                    if (w != 0xFFFF) tile_entry = w;
                }
            }

            // Original sub_1c8f1 ANDs entry with ax=0xFFFE (clears dirty bit 0)
            // then draws if bit 3 (foreground) is set. v2 draws all foreground
            // tiles every frame — only check bit 3.
            if (!(tile_entry & 8)) continue;

            v2_render_tile_masked(buf, tgfx_base, gs_base, tile_entry,
                                  col_vis * 8 - pix_off_x, row_vis * 8 - pix_off_y);
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
    if (!v2_vm_in_frame && !v2_tls_presenter && !v2_compose_at_flip) return;
#endif
    if (!myDrawInfo_v2 || !v2_m2c_base) return;

    uint8_t* ds_base = v2_get_ds_base(ds_val);
    V2StateViewC st(ds_base);   // phase D: typed field access
    uint8_t* buf = v2_tls_out ? v2_tls_out : v2_render_buf;   // UX stage 9

    // Scan UI element list: 40 columns × 22 rows at ds:0x956C
    uint8_t* ui_list = ds_base + DS_GLYPH_BUF;

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
            // v2_dbg_pre_vm_iter: file-scope extern (top of file)
            fprintf(stderr, "V2-UI-COUNT[f%d render=%d lvl=%04X]: nz_cells=%d rows: ",
                v2_dbg_pre_vm_iter, _frame, st.level(), nz);
            for (int r = 0; r < 22; r++)
                if (rows_used[r]) fprintf(stderr, "r%d=%d ", r, rows_used[r]);
            fprintf(stderr, "byte_956B=%02X\n", v2gs(ds_base).glyph_dirty_b());
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

    // the page lists (render_v2.h): on a tile level the cells are the page's glyph commands
    // (drawn by the composition in their place); this pass then paints only the CJK overlay
    for (int pos = 0; pos < 0x370 && !v2_tls_ui_cells_from_page; pos++) {
        uint8_t ch = ui_list[pos];
        if (ch == 0) continue;

        uint16_t glyph_index = (uint16_t)(ch - 0x10);
        int grid_row = pos / 40;
        int grid_col = pos % 40;

        int screen_x = grid_col * 8 + (v2_fbw - 320) / 2;   // UX stage 9 step 4: the 40-cell text plane sits centred on a wide frame
        int screen_y = grid_row * 8;

        // Glyph data at ds:0x687D + glyph_index * 72
        // Original: si = 0x687E + glyph_index*72, reads mask at [si-1]
        extern const uint8_t* v2_glyph_bytes(const uint8_t*, uint16_t);
        const uint8_t* glyph = v2_glyph_bytes(ds_base, glyph_index);   // UX6: the language page for its codes

        for (int plane = 0; plane < 4; plane++) {
            for (int strip = 0; strip < 2; strip++) {
                uint8_t mask = glyph[0];
                const uint8_t* data = glyph + 1;
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
    // UX6 phase 2: the CJK text items — Unifont 16x16 glyphs of the active bank
    // over the space-filled cells, the DOS letter style (body 2, shadow (-1,+1) 1)
    for (auto& it : v2_text_items) {
        if (!it.on) continue;
        if (it.row >= 22 || it.col >= 40 || ui_list[it.row * 40 + it.col] != 0x20) { it.on = 0; continue; }
        int x = it.col * 8 + (v2_fbw - 320) / 2, y = it.row * 8;   // centred like the cells
        for (size_t i = 0; it.utf8[i]; ) {
            uint8_t c = (uint8_t)it.utf8[i];
            if (c == 0x0D) { x = it.col * 8 + (v2_fbw - 320) / 2; y += 16; i++; continue; }
            uint32_t cp; int len = 1;
            if (c < 0x80) cp = c; else if ((c & 0xE0) == 0xC0) { cp = ((c & 0x1F) << 6) | (it.utf8[i+1] & 0x3F); len = 2; }
            else { cp = ((c & 0x0F) << 12) | ((it.utf8[i+1] & 0x3F) << 6) | (it.utf8[i+2] & 0x3F); len = 3; }
            i += len;
            uint8_t w = 8; const uint8_t* g = v2_lang_wide_glyph(cp, &w);
            if (!g) { x += w; continue; }
            for (int pass = 0; pass < 2; pass++)
                for (int yy = 0; yy < 16; yy++) {
                    uint16_t bits = (uint16_t)(g[yy * 2] | (g[yy * 2 + 1] << 8));
                    for (int xx = 0; xx < w; xx++)
                        if (bits & (0x8000 >> xx)) {
                            if (pass == 0) v2_put_pixel(buf, x + xx - 1, y + yy + 1, 1);
                            else v2_put_pixel(buf, x + xx, y + yy, 2);
                        }
                }
            x += w;
        }
    }
    // debug: V2_UI_BOXSHOT=<dir> dumps the indexed frame (with the glyph
    // layer just drawn) as <dir>/uibox_<n>.ppm each time the glyph buffer
    // changes while non-empty — text-box review headless (UX stage 6)
    {
        static const char* dir = nullptr; static int init = 0, shots = 0;
        static uint32_t last_sig = 0;
        if (!init) { dir = getenv("V2_UI_BOXSHOT"); init = 1; }
        if (dir && shots < 16) {
            uint32_t sig = 0; int nz = 0;
            for (int pos = 0; pos < 0x370; pos++) { if (ui_list[pos]) { nz++; sig = sig * 31 + ui_list[pos] + pos; } }
            for (auto& it : v2_text_items) if (it.on) { for (const char* c = it.utf8; *c; c++) sig = sig * 31 + (uint8_t)*c; sig = sig * 31 + it.col + it.row; }   // the CJK items too
            if (nz && sig != last_sig) {
                extern uint8_t v2_dac_shadow[768];
                char fn[512]; snprintf(fn, sizeof fn, "%s/uibox_%d.ppm", dir, shots++);
                if (FILE* f = fopen(fn, "wb")) {
                    fprintf(f, "P6\n%d 200\n255\n", v2_fbw);
                    for (int i = 0; i < v2_fbw * 200; i++) {
                        uint8_t c = buf[i];
                        fputc(v2_dac_shadow[c*3+0] << 2, f); fputc(v2_dac_shadow[c*3+1] << 2, f); fputc(v2_dac_shadow[c*3+2] << 2, f);
                    }
                    fclose(f);
                }
                last_sig = sig;
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
    { static int tr = -1; if (tr < 0) tr = getenv("V2_FLIP_DUMP") ? 1 : 0;   // debug: every picture blit
      if (tr) fprintf(stderr, "V2-VPCHUNK: seg %04X plane_size %u resolved %s\n", chunk_seg, plane_size, chunk ? "yes" : "NO"); }
    v2_fbw = 320;   // UX stage 9 step 4: the planes below are written 320-stride — the frame is the 320-px raster from here on
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
    // #113 interludes: the LVX scenes spawn the viking trio with the HUD
    // machinery off ([25CF] bit0 = 0) — a combination the canon never has
    // (every canonical viking spawn runs with a live HUD). The spawn tail
    // still calls the HUD painters, smearing portraits/icons into the
    // split-screen band. Dead branch on canonical data.
    { extern uint8_t* v2_vm_get_shadow_ds();
      uint8_t* _sh = v2_vm_get_shadow_ds();
      if (_sh && !(_sh[0x25CF] & 1)) return; }

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
    // #113 interludes: the LVX scenes spawn the viking trio with the HUD
    // machinery off ([25CF] bit0 = 0) — a combination the canon never has
    // (every canonical viking spawn runs with a live HUD). The spawn tail
    // still calls the HUD painters, smearing portraits/icons into the
    // split-screen band. Dead branch on canonical data.
    { extern uint8_t* v2_vm_get_shadow_ds();
      uint8_t* _sh = v2_vm_get_shadow_ds();
      if (_sh && !(_sh[0x25CF] & 1)) return; }

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
    uint8_t* item_data = ds_base + DS_HUD_ITEM_GFX + (item_id << 8);

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
    // #113 interludes: same HUD-off gate as the portrait/item/healthbar
    // painters (dead branch on canonical data — see the note there).
    { extern uint8_t* v2_vm_get_shadow_ds();
      uint8_t* _sh = v2_vm_get_shadow_ds();
      if (_sh && !(_sh[0x25CF] & 1)) return; }

#ifdef V2_RENDER_FROM_SHADOW
    if (!v2_vm_in_frame) return;
#endif
    if (!v2_m2c_base) return;

    uint8_t* ds_base = v2_get_ds_base(ds_val);
    uint8_t* sel_data = ds_base + DS_HUD_SEL_GFX;

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
    // #113 interludes: the LVX scenes spawn the viking trio with the HUD
    // machinery off ([25CF] bit0 = 0) — a combination the canon never has
    // (every canonical viking spawn runs with a live HUD). The spawn tail
    // still calls the HUD painters, smearing portraits/icons into the
    // split-screen band. Dead branch on canonical data.
    { extern uint8_t* v2_vm_get_shadow_ds();
      uint8_t* _sh = v2_vm_get_shadow_ds();
      if (_sh && !(_sh[0x25CF] & 1)) return; }

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
