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

// Task #21 obj-trace ring (defined in v2_vm.cpp).
extern "C" void v2_objtrace(const char* tag, int a, int b, int c, int d);
extern "C" int v2_objtrace_di;

// V2 is fully independent of myDrawInfo / orig drawBuffer.
// v2 decodes chunks itself via v2_draw_viewport_chunk (writes to v2_render_buf
// directly, mirroring orig sub_10cd8 → VGA) and v2_draw_hud_background → v2_hud_buf.

#ifdef V2_RENDER_FROM_SHADOW
bool v2_vm_in_frame = false;
#endif

// Helper: get DS base pointer for v2 rendering.
// With V2_RENDER_FROM_SHADOW: reads from v2 VM's shadow DS (independent from original).
// Without: reads from real DS (same data as original VM).
static inline uint8_t* v2_get_ds_base(uint16_t ds_val) {
#ifdef V2_RENDER_FROM_SHADOW
    uint8_t* shadow = v2_vm_get_shadow_ds();
    if (shadow) return shadow;
#endif
    return v2_m2c_base + ((uint32_t)ds_val << 4);
}

// V2 rendering state — definitions (declared extern in render_v2.h)
uint8_t* v2_m2c_base = nullptr;
std::mutex v2_ds_modify_mutex;
uint8_t  v2_render_buf[320*200];
uint8_t  v2_display_buf[320*200];
std::mutex v2_display_mutex;
uint8_t  v2_hud_buf[320*64];
// UX stage 0 (full-screen LVX scenes): published with v2_display_buf under
// v2_display_mutex; the presenter shows map rows 176..199 instead of the HUD
// band when set. v2_clip_h is the per-frame sprite/pixel clip height of the
// composition buffer: 176 (orig VGA split) or 200 on a full-screen scene.
int v2_display_fullscreen = 0;
extern "C" int v2_scene_fullscreen(void);   // v2_vm.cpp: LVX_FULLSCREEN of ds:0x25AD
static int v2_clip_h = 176;
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
static inline void v2_vga_w(uint32_t addr, uint32_t plane, uint8_t val) {
#ifdef V2_ONLY
    // Stage 6.2: the shadow-VGA page emulation is the DEFAULT-build verify
    // oracle (A2 pixel parity vs the real CRTC scan-out). The target engine
    // composes frames directly (v2_draw_* into the linear buffer) — no
    // reader exists here, so the Mode-X pixel model dies in V2_ONLY.
    (void)addr; (void)plane; (void)val;
    return;
#endif
    uint32_t lin = (addr & 0xFFFFu) * 4u + (plane & 3u);
    // diag (env V2_VGAW_TRAP=vgaaddr): backtrace writers of one VGA byte addr.
    {
        static long _t = -2;
        if (_t == -2) { const char* e = getenv("V2_VGAW_TRAP"); _t = e ? strtol(e, 0, 0) : -1; }
        if (_t >= 0 && (addr & 0xFFFFu) == (uint32_t)_t) {
            static int _n = 0;
            if (_n < 12) { _n++;
                fprintf(stderr, "VGAW-TRAP addr=%04X pl=%u val=%02X ra=%p %p\n",
                        addr & 0xFFFF, plane & 3, val,
                        __builtin_return_address(0), __builtin_return_address(1));
            }
        }
    }
    v2_vga[lin] = val;
    v2_vga_cov[lin] = 1;
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
    {
        static long _t = -2;
        if (_t == -2) { const char* e = getenv("V2_VGAW_TRAP"); _t = e ? strtol(e, 0, 0) : -1; }
        if (_t >= 0 && (uint32_t)_t >= dst && (uint32_t)_t < (uint32_t)dst + nbytes) {
            static int _n = 0;
            if (_n < 40) { _n++;
                extern int v2_dbg_pre_vm_iter;
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
extern "C" int v2_vga_fetch_page(uint8_t* out, uint32_t count) {
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

void v2_swap_render_buf() {
#ifdef V2_RENDER_FROM_SHADOW
    if (!v2_vm_in_frame) return;
#endif
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
        // all 200 rows: rows 176..199 matter only on full-screen LVX scenes
        memcpy(v2_display_buf, v2_render_buf, 320 * 200);
        v2_display_fullscreen = v2_scene_fullscreen();
        extern uint8_t v2_display_hud_buf[];
        memcpy(v2_display_hud_buf, v2_hud_buf, 320 * 64);
#else
        v2_display_fullscreen = 0;   // verification build presents the shadow-VGA window only
        extern int v2_vga_fetch_page(uint8_t* out, uint32_t count);
        extern uint8_t v2_vga[65536 * 4];
        if (!v2_vga_fetch_page(v2_display_buf, 320 * 176))
            memcpy(v2_display_buf, v2_render_buf, 320 * 200);
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
      if (_lp) { extern int v2_dbg_pre_vm_iter;
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

// UX stage 2: the SNES parallax layer under the level tiles (display lane).
// par = (viewport * f) >> 8 per axis (8.8 multiplier from the head), the
// autoscroll axes take the time-true accumulator instead; the map repeats
// with its own period; nibble 0 = transparent (DAC 0 stays = the backdrop),
// the palette row comes from the map cell like the console's tilemap word.
static void v2_draw_parallax(const V2StateViewC& st, uint8_t* buf) {
    const V2ParallaxLayer& P = v2_parallax;
    if (!P.on || !P.map || !P.tiles) return;
    const int wpx = P.w * 8, hpx = P.h * 8;
    if (!wpx || !hpx) return;
    int px = (P.fx & 0x8000) ? (int)(P.acc_x / 1792u)
                             : (int)(((uint32_t)st.viewport_x() * (P.fx & 0x7FFF)) >> 8);
    int py = (P.fy & 0x8000) ? (int)(P.acc_y / 1792u)
                             : (int)(((uint32_t)st.viewport_y() * (P.fy & 0x7FFF)) >> 8);
    px %= wpx; py %= hpx;
    for (int sy = 0; sy < v2_clip_h; sy++) {
        const int my = (py + sy) % hpx;
        const uint16_t* mrow = P.map + (my >> 3) * P.w;
        const int ty = my & 7;
        uint8_t* out = buf + sy * 320;
        for (int sx = 0; sx < 320; sx++) {
            const int mx = (px + sx) % wpx;
            const uint16_t cell = mrow[mx >> 3];
            const uint32_t idx = cell & 0x3FF;
            if (idx >= P.ntiles) continue;
            const int tx = (cell & 0x4000) ? (7 - (mx & 7)) : (mx & 7);
            const int tyy = (cell & 0x8000) ? (7 - ty) : ty;
            const uint8_t v = P.tiles[idx * 64 + tyy * 8 + tx];
            if (v) out[sx] = (uint8_t)(((cell >> 10) & 7) * 16 + v);
        }
    }
}

void v2_draw_tiles(uint16_t ds_val) {
#ifdef V2_RENDER_FROM_SHADOW
    if (!v2_vm_in_frame) return;
#endif
    if (!myDrawInfo_v2 || !v2_m2c_base) return;

    uint8_t* ds_base = v2_get_ds_base(ds_val);
    V2StateViewC st(ds_base);   // phase D: typed field access
    uint8_t* buf = v2_render_buf;
    // UX stage 0: clip height for this frame's sprites/UI (200 on a
    // full-screen LVX scene, else the orig 176-row viewport).
    // UX stage 1: on a full-screen scene the console blanks every line from
    // 187 down by a raster split (measured on the DE video and modelled in
    // the scene map: yellow line 187-188, black below) — sprites never show
    // there (the Ship consoles' lowest rows vanish under it on the Genesis),
    // so the sprite/pixel clip stops at 187; the tile pass still paints all
    // 200 rows (the band itself is map data).
    v2_clip_h = v2_scene_fullscreen() ? 187 : 176;

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
    if (lvl_flags & 0x42) {
        if (v2_chunk_bg_valid) {
            memcpy(v2_render_buf, v2_chunk_bg_backup, 320 * 176);
        }
        return;
    }
    v2_chunk_bg_valid = false; // tile-based level; static backup no longer relevant

    // Normal: clear and draw tiles (all 200 buffer rows: the tile loop below
    // already renders 25 tile rows; rows 176..199 are shown only on
    // full-screen LVX scenes, otherwise the HUD band covers them)
    memset(buf, 0, 320*200);
    // UX stage 2: the parallax layer goes under the tiles; the tile pass then
    // skips the pixels of colour 0 of each palette row (the console's
    // transparent index — on the DOS palette they are blacked out, sub_112ae).
    v2_draw_parallax(st, buf);
    const bool par_on = v2_parallax.on;

#ifdef V2_RENDER_FROM_SHADOW
    uint8_t* fs_base = v2_resolve_segment(fs_seg);
    uint8_t* tgfx_base = v2_resolve_segment(tgfx_seg);
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

    for (int row_vis = 0; row_vis < 25; row_vis++) {
        uint16_t row_scrolled = (uint16_t)(row_vis + scroll_x + extra_tile_y);
        // Orig sub_16ded has NO row_scrolled bound — just reads LUT and renders
        // whatever it finds. v2 had `if (row_scrolled >= 64) continue;` hardcoded
        // limit which clipped tiles on large maps when scrolled past row 64.
        // Removed: now mirrors orig (LUT read wraps via uint16_t arithmetic).

        // Row base from lookup table at ds-0x7098
        uint16_t lut_off = (uint16_t)(row_scrolled * 2u - LUT_ROW_BASE);
        uint16_t row_base = *(uint16_t*)(ds_base + lut_off);

        for (int col_vis = 0; col_vis < 43; col_vis++) {
            uint16_t col_scrolled = (uint16_t)(col_vis + scroll_y + extra_tile_x);

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

            // Screen position (with sub-tile pixel offset)
            int screen_x = col_vis * 8 - pix_off_x;
            int screen_y = row_vis * 8 - pix_off_y;

            // Decode tile: 4 planes × 8 rows × 2 bytes
            // Normal pixel order per row:
            //   x+0=p0_b0, x+1=p1_b0, x+2=p2_b0, x+3=p3_b0,
            //   x+4=p0_b1, x+5=p1_b1, x+6=p2_b1, x+7=p3_b1
            for (int row = 0; row < 8; row++) {
                int src_row = vflip ? (7 - row) : row;
                int sy = screen_y + row;
                if (sy < 0) continue;
                if (sy >= 200) break;

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
                    if (sx >= 320) break;
                    if (par_on && (pixels[px] & 0x0F) == 0) continue;   // UX stage 2: row colour 0 = transparent
                    buf[sy * 320 + sx] = pixels[px];
                }
            }
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
    if (visible_col < 0 || visible_col >= 43) return;

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
        if (sy >= 200) break;
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
            if (sx >= 320) break;
            buf[sy * 320 + sx] = pixels[px];
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
    if (sx >= 0 && sx < 320 && sy >= 0 && sy < v2_clip_h)   // 176, or 200 on full-screen scenes
        buf[sy * 320 + sx] = color;
}

static void v2_draw_sprites_impl(uint16_t ds_val, int late_gate, int only_obj = -1);
void v2_draw_sprites(uint16_t ds_val) { v2_draw_sprites_impl(ds_val, 0); }
void v2_draw_sprites_late(uint16_t ds_val) { v2_draw_sprites_impl(ds_val, 1); }

// late_gate=1: repaint only what orig sub_1dd9c draws in this sub-frame —
// gates evaluated BEFORE v2_late_sprites_1DD9C's DS effects (DEC of [obj+0x114D]) and
// BEFORE v2_dirty_tile_scan_1C8F1 (which clears render-map bit0), matching the orig call
// order 1dd9c -> 1c8f1. sub_1cdef gate: clip the object's tile bbox to the
// viewport (ds:0x9168/0x916A) and scan its cells in the render map (FS) for
// bit0 (seg003 eip 0x634: TEST word fs:[si],1) — set means this sub-frame's
// sub_1c8f1 repaints the cell, so the sprite must be repainted over it.
static void v2_draw_sprites_impl(uint16_t ds_val, int late_gate, int only_obj) {
#ifdef V2_RENDER_FROM_SHADOW
    if (!v2_vm_in_frame) return;
#endif
    if (!v2_m2c_base || !myDrawInfo_v2) return;

    uint8_t* ds_base = v2_get_ds_base(ds_val);
    V2StateViewC st(ds_base);   // phase D: typed field access

    uint8_t* buf = v2_render_buf;

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

        int type = flags & 7;

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
        if (type != 1 && type != 2 && type != 4) continue;

        int16_t world_x = *(int16_t*)(ds_base + obj + OBJ_SPRITE_X);
        int16_t world_y = *(int16_t*)(ds_base + obj + OBJ_SPRITE_Y);

        int sx0 = world_x - viewport_x;
        int sy0 = world_y - viewport_y;

        uint16_t sprite_off = *(uint16_t*)(ds_base + obj + OBJ_SPRITE_OFF);
        uint16_t sprite_seg = *(uint16_t*)(ds_base + obj + OBJ_SPRITE_SEG);
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
            num_strips = (int)*(uint16_t*)(ds_base + obj + OBJ_STRIP_COUNT);
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

        // Horizontal flip: bit 9 (0x200) of flags.
        // All three type renderers (seg003_648_proc, sub_1d3b2, loc_1d8a8)
        // check this flag and branch to mirrored rendering paths.
        bool hflip = (flags & 0x200) != 0;

        // Coarse bounds check — sprite pixel size. (Display lane only; the
        // byte-exact page channel is the shadow VGA in v2_vm.cpp.)
        int sprite_h = num_strips * rows_per_strip;
        int sprite_w = bytes_per_row * 4;  // 4 planes
        if (sx0 >= 320 || sx0 < -sprite_w || sy0 >= v2_clip_h || sy0 < -sprite_h) continue;

        // Sprite data: resolve segment to shadow buffer, add offset.
        // sprite_off = 1-based offset to first data byte; mask at offset-1.
#ifdef V2_RENDER_FROM_SHADOW
        uint8_t* seg_base = v2_resolve_segment(sprite_seg);
        if (!seg_base) continue;
        uint8_t* sprite = seg_base + sprite_off - 1;
        {
            static int cmp_mismatch = 0;
            uint16_t cur_lvl = st.level();
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
        uint32_t sprite_linear = ((uint32_t)sprite_seg << 4) + sprite_off - 1;
        uint8_t* sprite = v2_m2c_base + sprite_linear;
#endif

        // Column formula: sx0 + N*4 + section (normal) or
        // sx0 + (sprite_w-1) - (N*4 + section) (flipped).
        // sx(col) computes the screen x for a given data column.
        auto sx = [&](int col) -> int {
            return hflip ? sx0 + sprite_w - 1 - col : sx0 + col;
        };

        uint8_t* ptr = sprite;
        for (int section = 0; section < 4; section++) {
            int plane = section;
            for (int strip = 0; strip < num_strips; strip++) {
                uint8_t mask = ptr[0];
                uint8_t* data = ptr + 1;
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
    if (!v2_vm_in_frame) return;
#endif
    if (!myDrawInfo_v2 || !v2_m2c_base) return;

    uint8_t* ds_base = v2_get_ds_base(ds_val);
    V2StateViewC st(ds_base);   // phase D: typed field access
    uint8_t* buf = v2_render_buf;

    uint16_t fs_seg = st.seg_fs();
    uint16_t tgfx_seg = st.seg_tilegfx();
    uint16_t gs_seg = st.seg_gs();
    if (!fs_seg || !tgfx_seg || !gs_seg) return;

#ifdef V2_RENDER_FROM_SHADOW
    uint8_t* fs_base = v2_resolve_segment(fs_seg);
    uint8_t* tgfx_base = v2_resolve_segment(tgfx_seg);
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

    for (int row_vis = 0; row_vis < 25; row_vis++) {
        uint16_t row_scrolled = (uint16_t)(row_vis + scroll_x + extra_tile_y);
        // No `row_scrolled >= 64` clamp — orig sub_1c8f1 (flagged-tile render)
        // doesn't clip rows beyond LUT; v2 hardcode caused tiles missing on
        // large maps when scrolled past row 64.

        uint16_t lut_off = (uint16_t)(row_scrolled * 2u - LUT_ROW_BASE);
        uint16_t row_base = *(uint16_t*)(ds_base + lut_off);

        for (int col_vis = 0; col_vis < 43; col_vis++) {
            uint16_t col_scrolled = (uint16_t)(col_vis + scroll_y + extra_tile_x);
            uint16_t tile_map_off = (uint16_t)((row_base + col_scrolled) * 2u);
            uint16_t tile_entry = *(uint16_t*)(fs_base + tile_map_off);

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
    if (!v2_vm_in_frame) return;
#endif
    if (!myDrawInfo_v2 || !v2_m2c_base) return;

    uint8_t* ds_base = v2_get_ds_base(ds_val);
    V2StateViewC st(ds_base);   // phase D: typed field access
    uint8_t* buf = v2_render_buf;

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
            extern int v2_dbg_pre_vm_iter;
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
