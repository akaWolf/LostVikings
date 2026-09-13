#include "v2_midi.h"   // UX stage 11: V2_MIDI_DUMP is written before the _exit paths
#include "third_party/xbrz/xbrz.h"   // UX stage 11 tail: FILTER < XBRZ >
extern "C" { void hqxInit(void); void hq2x_32(const uint32_t*, uint32_t*, int, int); void hq3x_32(const uint32_t*, uint32_t*, int, int); void hq4x_32(const uint32_t*, uint32_t*, int, int); }   // third_party/hqx: FILTER < HQX >
#include <thread>
#include <vector>
#include <SDL2/SDL.h>
#include "v2_timing.h"
extern "C" void sdl_int9_note_keydown(int sdl_scancode);  // render.cpp (#62)
#include <thread>
#include <vector>
#include <cstring>
#include <atomic>
#include <cassert>
#include <cstdio>
#include "v2_input_recorder.h"
#include "v2_keymap.h"
#include "v2_coop.h"          // UX stage 8: co-op key sets and game controllers
#include "v2_net.h"           // UX stage 8 step 3: the lockstep captures the keys instead
#include <unistd.h>  // _exit

// ============================================================================
// Второе окно для тестирования новой реализации рендера
// API дублирует render.cpp с суффиксом _v2
// ============================================================================

const int SCREEN_SCALE_V2 = 2;
bool v2_present_vsync = false;   // UX stage 9: SDL_RenderPresent blocks on the display refresh

// ============================================================================
// UX stage 9 rework (2026-09-11): the game's vsync IS the display's vsync.
// The DOS game paces itself on the VGA vertical retrace (sub_10130 polls it,
// three waits per game frame at the 60 Hz of Mode X) and moves the camera and
// the sprites on EVERY retrace: the three sub-frames of a game frame (render1
// / render2 / render3) hold different positions — a DOSBox capture and the
// flip log agree, 1..3 px per 60 Hz frame. Pacing those waits with a 16 ms
// timer while the presenter ran on the display's own refresh (16.67 ms) made
// the two clocks beat: every ~0.9 s a sub-frame was shown twice and another
// dropped. The tick-to-tick interpolation of stage 9 hid that hitch but threw
// the original's sub-frame positions away (a game frame of latency and a
// rounding shimmer on top). Now the presenter hands the game a "vsync" after
// each present, on the 60 Hz schedule of the display's refreshes (60 Hz: every
// one, 120: every other, 144: two-three-two-...), the game's wait loop blocks
// on it (v2_tick_sleep -> v2_vsync_wait_game), and the presenter latches the
// game's newest flip as late as it can before its next present — one refresh
// of latency, like the VGA page flip. Without a real vsync (V2_NO_PRESENT_VSYNC,
// a compositor that does not block, SDL's dummy driver) the game keeps a 60.0
// Hz timer of its own and the presenter its 15 ms sleep, as before.
// ============================================================================
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cmath>
#include "v2_stats.h"
#include "v2_ui.h"                     // the PACING option (v2_options)
V2Stats v2_stats;                      // the STATS overlay's figures (v2_stats.h)
std::atomic<uint64_t> v2_tick_wait_ticks{0};   // v2_timing.h: the game thread's time inside its vsync waits
extern bool need_quit;
namespace {
std::mutex g_vs_mx; std::condition_variable g_vs_cv;
uint64_t g_vs_game_seq = 0;            // game vsyncs fired (under g_vs_mx)
double   g_vs_acc = 0.0;               // the 60 Hz schedule's phase, in refreshes
double   g_display_hz = 0.0;           // the mode of the window's display (0 = unknown)
double   g_present_ema_ms = 0.0;       // the measured interval between presents
uint64_t g_present_last = 0;           // SDL_GetPerformanceCounter at the last present
unsigned g_present_n = 0;              // presents measured so far (the lock waits for the interval to settle)
const unsigned VS_WARMUP = 32;         // presents before the lock may engage (~0.5 s at 60 Hz)
std::atomic<bool> g_vs_locked{false};  // present blocks on a plausible refresh: the game waits on it
std::atomic<int>  g_vs_hz_x100{0};     // the effective refresh x 100 (menu, SMOOTH AUTO)
}
static void v2_vsync_query_display(SDL_Window* w) {
    SDL_DisplayMode m; const int di = w ? SDL_GetWindowDisplayIndex(w) : -1;
    g_display_hz = (di >= 0 && SDL_GetCurrentDisplayMode(di, &m) == 0 && m.refresh_rate > 0) ? (double)m.refresh_rate : 0.0;
}
// presenter thread, right after SDL_RenderPresent returned (= the display's vsync when it blocks)
static void v2_vsync_on_present(void) {
    const uint64_t now = SDL_GetPerformanceCounter(); const double freq = (double)SDL_GetPerformanceFrequency();
    if (g_present_last) {
        const double dt = (double)(now - g_present_last) / freq * 1000.0;
        // a stall (the level load, a texture rebuild: several refreshes long) is not an interval
        // sample — with the mode known it is left out, so the average settles on the refresh at once
        const bool stall = g_display_hz > 0.0 && dt > 2.5 * 1000.0 / g_display_hz;
        if (dt > 0.5 && dt < 200.0 && !stall) { g_present_ema_ms = g_present_ema_ms > 0.0 ? g_present_ema_ms * 0.9 + dt * 0.1 : dt; g_present_n++; }
        // STATS: a present that took more than 1.5 refreshes missed one (the lock on)
        if (g_vs_locked.load(std::memory_order_relaxed) && g_present_ema_ms > 0.0 && dt > 1.5 * g_present_ema_ms) v2_stats.present_late.fetch_add(1, std::memory_order_relaxed);
    }
    g_present_last = now;
    v2_stats.present_ms_x100.store((int)(g_present_ema_ms * 100.0 + 0.5), std::memory_order_relaxed);
    // the effective refresh: the mode's rate when the measured interval agrees with it, else the
    // measurement itself — once it has settled (the first presents of a window run long: the
    // level load, the texture creation; an early lock read 20 ms and SMOOTH AUTO took 60 Hz for 48)
    double hz = 0.0;
    if (g_present_n >= VS_WARMUP && g_present_ema_ms > 4.0 && g_present_ema_ms < 40.0) {
        // the mode's rate wins while the measurement is anywhere near it (a loaded machine stretches
        // the intervals; the mode does not change) — the measurement alone when the mode is unknown
        // or plainly different (a compositor presenting at its own rate)
        const double meas = 1000.0 / g_present_ema_ms;
        hz = (g_display_hz > 0.0 && fabs(1000.0 / g_display_hz - g_present_ema_ms) < 0.35 * g_present_ema_ms) ? g_display_hz : meas;
    }
    const bool locked = v2_present_vsync && hz > 0.0;
    if (locked != g_vs_locked.load(std::memory_order_relaxed)) {
        g_vs_locked.store(locked, std::memory_order_release);
        fprintf(stderr, "render_v2: vsync lock %s (display mode %.0f Hz, present interval %.2f ms)\n", locked ? "ON" : "OFF", g_display_hz, g_present_ema_ms);
    }
    g_vs_hz_x100.store((int)(hz * 100.0 + 0.5), std::memory_order_relaxed);
    v2_stats.vsync_locked.store(locked ? 1 : 0, std::memory_order_relaxed);
    v2_stats.display_hz_x100.store((int)(hz * 100.0 + 0.5), std::memory_order_relaxed);
    if (!locked) return;
    // the 60 Hz schedule on the refreshes: an exact divider for the multiples of 60 (0.5 Hz tolerance), the measured ratio otherwise
    double step = 60.0 / hz;
    { const int k = (int)(hz / 60.0 + 0.5); if (k >= 1 && fabs(hz - 60.0 * k) < 0.5) step = 1.0 / k; }
    bool fire = false;
    { std::lock_guard<std::mutex> lk(g_vs_mx);
      g_vs_acc += step;
      if (g_vs_acc >= 1.0 - 1e-9) { g_vs_acc -= 1.0; if (g_vs_acc > 1.0 || g_vs_acc < 0.0) g_vs_acc = 0.0; g_vs_game_seq++; fire = true; } }
    if (fire) g_vs_cv.notify_all();
}
// game thread (v2_tick_sleep): block until the presenter's next game vsync.
// false = no lock, or the presenter stalled (a hidden window): the caller paces
// itself. A vsync that fired while the game was busy counts (the backlog is
// dropped): the DOS loop sees the retrace flag and goes on, it never catches up.
bool v2_vsync_wait_game(void) {
    if (!g_vs_locked.load(std::memory_order_acquire)) return false;
    static uint64_t seen = 0;
    std::unique_lock<std::mutex> lk(g_vs_mx);
    if (g_vs_game_seq > seen) { seen = g_vs_game_seq; return true; }
    const bool ok = g_vs_cv.wait_for(lk, std::chrono::milliseconds(60), [] { return g_vs_game_seq > seen || need_quit; });
    seen = g_vs_game_seq;
    return ok && !need_quit;
}
// presenter thread: sleep until shortly before the next refresh, so the snapshot
// that follows holds the flip the game made for it (the compose and the texture
// upload take about a millisecond)
static void v2_vsync_latch_sleep(void) {
    if (!g_vs_locked.load(std::memory_order_acquire) || g_present_last == 0 || g_present_ema_ms <= 0.0) return;
    const double freq = (double)SDL_GetPerformanceFrequency();
    const double margin = g_present_ema_ms * 0.4 < 3.0 ? g_present_ema_ms * 0.4 : 3.0;
    const double target_ms = g_present_ema_ms - margin;
    for (;;) {
        const double since = (double)(SDL_GetPerformanceCounter() - g_present_last) / freq * 1000.0;
        if (since >= target_ms || need_quit) break;
        const double left = target_ms - since;
        if (left > 1.5) SDL_Delay((Uint32)(left - 1.0)); else SDL_Delay(0);
    }
}
bool   v2_vsync_locked(void) { return g_vs_locked.load(std::memory_order_acquire); }
double v2_vsync_display_hz(void) { return g_vs_hz_x100.load(std::memory_order_relaxed) / 100.0; }
// SMOOTH AUTO: interpolate only where the 60 Hz schedule is uneven on this display
bool v2_vsync_auto_smooth(void) {
    if (!g_vs_locked.load(std::memory_order_acquire)) return false;
    const double hz = v2_vsync_display_hz(); const int k = (int)(hz / 60.0 + 0.5);
    return !(k >= 1 && fabs(hz - 60.0 * k) < 0.5);
}

// ---------------------------------------------------------------------------
// PACING < VSYNC | VRR > (2026-09-11). VRR = a G-Sync / FreeSync display: the
// present does not wait for a refresh (SDL_RenderSetVSync 0), the game keeps
// its own 60.0 Hz timer (no lock), and the presenter shows every flip the
// moment the game made it — one present per sub-frame, the display follows the
// game's 60 Hz. The game's flip rings a bell (v2_flip_notify, from
// v2_swap_render_buf) the presenter waits on instead of sleeping.
// ---------------------------------------------------------------------------
namespace {
std::mutex g_flip_mx; std::condition_variable g_flip_cv; uint64_t g_flip_seq = 0;
int g_pacing_applied = -1;
}
void v2_flip_notify(void) {
    { std::lock_guard<std::mutex> lk(g_flip_mx); g_flip_seq++; }
    g_flip_cv.notify_all();
}
static bool v2_pacing_vrr(void) { return v2_options.pacing.load(std::memory_order_relaxed) == 1; }
// presenter: wait for the game's next flip (VRR pacing), at most `ms` (menus and
// waits without flips still get their presents)
static void v2_presenter_wait_flip(uint32_t ms) {
    static uint64_t seen = 0;
    std::unique_lock<std::mutex> lk(g_flip_mx);
    g_flip_cv.wait_for(lk, std::chrono::milliseconds(ms), [] { return g_flip_seq > seen || need_quit; });
    seen = g_flip_seq;
}
// presenter, once per loop: the option changed -> the renderer's vsync follows, the lock starts over
static void v2_pacing_apply(SDL_Renderer* r) {
    const int want = v2_options.pacing.load(std::memory_order_relaxed);
    if (want == g_pacing_applied || !r) return;
    g_pacing_applied = want;
#if SDL_VERSION_ATLEAST(2, 0, 18)
    { const char* e = getenv("V2_NO_PRESENT_VSYNC");
      const int vs = (want == 1 || (e && *e == '1')) ? 0 : 1;
      if (SDL_RenderSetVSync(r, vs) == 0) {
          SDL_RendererInfo ri; v2_present_vsync = SDL_GetRendererInfo(r, &ri) == 0 && (ri.flags & SDL_RENDERER_PRESENTVSYNC);
      } else fprintf(stderr, "render_v2: SDL_RenderSetVSync(%d) failed: %s\n", vs, SDL_GetError()); }
#endif
    // the lock measures again from scratch under the new pacing
    { std::lock_guard<std::mutex> lk(g_vs_mx); g_vs_acc = 0.0; }
    g_present_n = 0; g_present_ema_ms = 0.0; g_present_last = 0;
    if (g_vs_locked.exchange(false)) fprintf(stderr, "render_v2: vsync lock OFF (pacing changed)\n");
    v2_stats.vsync_locked.store(0, std::memory_order_relaxed);
    fprintf(stderr, "render_v2: pacing %s (present %s)\n", want == 1 ? "VRR" : "VSYNC", v2_present_vsync ? "waits for the refresh" : "returns at once");
}
const int SCREEN_WIDTH_V2 = 320;
const int SCREEN_HEIGHT_V2 = 240;
const int RENDER_WIDTH_V2 = 512;   // the linear stride of stableBuffer / tempDrawBuffer = V2_FB_MAX_W (render_v2.h, included below; static_assert there) — was the 344-px VGA pitch, a wide frame needs more
const int RENDER_HEIGHT_V2 = 240;
uint32_t tempDrawBuffer_v2[RENDER_WIDTH_V2*RENDER_HEIGHT_V2];

#include "render_v2.h"
static_assert(RENDER_WIDTH_V2 == V2_FB_MAX_W, "the presenter's row stride must hold the widest frame");
#include "v2_ui.h"
extern int v2_dbg_pre_vm_iter;   // game-frame counter (v2_vm.cpp), C++ linkage — declared once at file scope (clang rejects block externs inside extern "C" functions)

struct myDrawInfoS_v2* myDrawInfo_v2 = nullptr;
SDL_Window* myWindow_v2 = NULL;
SDL_Renderer* myRenderer_v2 = NULL;
SDL_Texture* myTexture_v2 = NULL;
SDL_PixelFormat *myFormat_v2 = NULL;

extern void render_callback_v2(void *);
extern int v2_display_fullscreen;   // v2_render_funcs.cpp: 0 = HUD layout, else the map rows shown (200/224)

// UX stage 9, step 3 — the presenter's own layout (no SDL logical size):
//   the canvas is always the 320x240 raster (square pixels; a 224-row scene sits
//           at the top with black below — 2026-09-06, the ASPECT toggle is gone);
//   INT.SCALE = whole multiples of that canvas only (letterboxed);
//   FILTER  NEAREST = crisp, LINEAR = bilinear on the source, SHARP = nearest
//           pre-scale to the next integer multiple, then linear to the window
//           (crisp pixels, no shimmer at fractional scales);
//   BORDER  BLACK, or GLOW = the frame decimated to 40x30, drawn linear over
//           the whole output at 28 % brightness behind the picture.
// V2_PRESENT_SHOT=<path.ppm>[:<call>] dumps the composed output once.
static SDL_Texture* g_sharp_tex = nullptr; static int g_sharp_k = 0, g_sharp_h = 0, g_sharp_w = 0;
static SDL_Texture* g_glow_tex = nullptr;
static int g_tex_filter = -1;   // the sampling the source texture was created with (0 nearest, 1 linear)
static SDL_Texture* v2_make_texture(int access, int w, int h, int linear) {
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, linear ? "1" : "0");   // sampled at creation time
    SDL_Texture* tx = SDL_CreateTexture(myRenderer_v2, SDL_PIXELFORMAT_RGBA8888, access, w, h);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    return tx;
}
extern int v2_present_w;   // render_v2_test.cpp: the width of the frame in stableBuffer

// UX stage 11 tail: FILTER < XBRZ | HQX > — the frame after the palette scaled by an integer k on the
// CPU (xBRZ 1.8: 2-6x, row slices on several threads; hqx: 2-4x, LUT), uploaded as ARGB8888 and
// stretched linearly for the remainder; FILTER < NONE > — the raster 1:1 in the middle of the window.
static SDL_Texture* g_hq_tex = nullptr; static int g_hq_w = 0, g_hq_h = 0;
static std::vector<uint32_t> g_hq_src, g_hq_dst;
static bool v2_present_hq(int filter, int PW, int H, double s, int* k_out) {
    const int kmax = (filter == 3) ? 6 : 4;
    int k = (int)s; if (k < 2) k = 2; if (k > kmax) k = kmax;
    const size_t n = (size_t)PW * H;
    g_hq_src.resize(n); g_hq_dst.resize(n * (size_t)k * k);
    // the frame is the left PW columns of the RENDER_WIDTH_V2-wide buffer;
    // SDL_PIXELFORMAT_RGBA8888 is 0xRRGGBBAA as a uint32, the scalers take 0xAARRGGBB
    for (int y = 0; y < H; y++) {
        const uint32_t* row = tempDrawBuffer_v2 + (size_t)y * RENDER_WIDTH_V2; uint32_t* out = g_hq_src.data() + (size_t)y * PW;
        for (int x = 0; x < PW; x++) out[x] = 0xFF000000u | (row[x] >> 8);
    }
    if (filter == 3) {
        static const xbrz::ScalerCfg cfg;
        unsigned nt = std::thread::hardware_concurrency(); if (nt < 1) nt = 1; if (nt > 8) nt = 8; if ((int)nt > H / 16) nt = (unsigned)(H / 16);
        std::vector<std::thread> th; int y0 = 0;
        const uint32_t* src = g_hq_src.data(); uint32_t* dst = g_hq_dst.data();
        for (unsigned t = 0; t < nt; t++) {
            const int y1 = (t + 1 == nt) ? H : (int)((long long)H * (t + 1) / nt);
            th.emplace_back([=] { xbrz::scale((size_t)k, src, dst, PW, H, xbrz::ColorFormat::ARGB, cfg, y0, y1); });
            y0 = y1;
        }
        for (std::thread& t : th) t.join();
    } else {
        static bool inited = false; if (!inited) { hqxInit(); inited = true; }
        (k == 2 ? hq2x_32 : k == 3 ? hq3x_32 : hq4x_32)(g_hq_src.data(), g_hq_dst.data(), PW, H);
    }
    if (!g_hq_tex || g_hq_w != PW * k || g_hq_h != H * k) {
        if (g_hq_tex) SDL_DestroyTexture(g_hq_tex);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
        g_hq_tex = SDL_CreateTexture(myRenderer_v2, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, PW * k, H * k);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
        g_hq_w = PW * k; g_hq_h = H * k;
    }
    if (!g_hq_tex) return false;
    SDL_UpdateTexture(g_hq_tex, NULL, g_hq_dst.data(), PW * k * (int)sizeof(uint32_t));
    *k_out = k;
    return true;
}

static void v2_present_frame(int H) {
    const int PW = v2_present_w;
    const int filter = v2_options.filter.load();
    const bool integer = v2_options.integer_scale.load();
    const int border = v2_options.border.load();
    const int want = (filter == 2) ? 1 : 0;
    if (g_tex_filter != want || !myTexture_v2) {
        if (myTexture_v2) SDL_DestroyTexture(myTexture_v2);
        myTexture_v2 = v2_make_texture(SDL_TEXTUREACCESS_STREAMING, RENDER_WIDTH_V2, RENDER_HEIGHT_V2, want);
        g_tex_filter = want;
    }
    if (!myTexture_v2) return;
    SDL_UpdateTexture(myTexture_v2, NULL, tempDrawBuffer_v2, RENDER_WIDTH_V2 * sizeof(uint32_t));
    int W = 0, Hout = 0;
    SDL_GetRendererOutputSize(myRenderer_v2, &W, &Hout);
    const int cw = PW, ch = H;   // the canvas: the frame's width x the 240-row raster (H is always SCREEN_HEIGHT_V2)
    double s = (W > 0 && Hout > 0) ? ((double)W / cw < (double)Hout / ch ? (double)W / cw : (double)Hout / ch) : 1.0;
    if (integer) { s = (double)(int)s; if (s < 1.0) s = 1.0; }
    const int dw = (int)(cw * s + 0.5), dh = (int)(ch * s + 0.5);
    SDL_Rect dst = { (W - dw) / 2, (Hout - dh) / 2, dw, dh };
    if (filter == 5) dst = SDL_Rect{ (W - cw) / 2, (Hout - ch) / 2, cw, ch };   // NONE: the raster itself, 1:1
    SDL_Rect src = { 0, 0, PW, H };
    SDL_SetRenderDrawColor(myRenderer_v2, 0, 0, 0, 255);
    SDL_RenderClear(myRenderer_v2);
    if (border == 1) {
        if (!g_glow_tex) {
            g_glow_tex = v2_make_texture(SDL_TEXTUREACCESS_TARGET, 40, 30, 1);
            if (g_glow_tex) SDL_SetTextureColorMod(g_glow_tex, 72, 72, 72);
        }
        if (g_glow_tex) {
            SDL_SetRenderTarget(myRenderer_v2, g_glow_tex);
            SDL_RenderCopy(myRenderer_v2, myTexture_v2, &src, NULL);
            SDL_SetRenderTarget(myRenderer_v2, NULL);
            SDL_RenderCopy(myRenderer_v2, g_glow_tex, NULL, NULL);
        }
    }
    bool drawn = false;
    if (filter == 1) {
        int k = (int)s; if (k < s) k++; if (k < 1) k = 1; if (k > 8) k = 8;
        if (!g_sharp_tex || g_sharp_k != k || g_sharp_h != H || g_sharp_w != PW) {
            if (g_sharp_tex) SDL_DestroyTexture(g_sharp_tex);
            g_sharp_tex = v2_make_texture(SDL_TEXTUREACCESS_TARGET, PW * k, H * k, 1);
            g_sharp_k = k; g_sharp_h = H; g_sharp_w = PW;
        }
        if (g_sharp_tex) {
            SDL_SetRenderTarget(myRenderer_v2, g_sharp_tex);
            SDL_RenderCopy(myRenderer_v2, myTexture_v2, &src, NULL);   // nearest, exact integer k
            SDL_SetRenderTarget(myRenderer_v2, NULL);
            SDL_RenderCopy(myRenderer_v2, g_sharp_tex, NULL, &dst);   // linear to the window
            drawn = true;
        }
    }
    if (filter == 3 || filter == 4) {
        int k = 0;
        if (v2_present_hq(filter, PW, H, s, &k)) { SDL_RenderCopy(myRenderer_v2, g_hq_tex, NULL, &dst); drawn = true; }
    }
    if (!drawn) SDL_RenderCopy(myRenderer_v2, myTexture_v2, &src, &dst);
    {
        static int shot = -1, at = 1, at_frame = -1, calls = 0; static const char* path = nullptr; static char pbuf[512];
        if (shot < 0) {
            const char* e = getenv("V2_PRESENT_SHOT");
            shot = (e && *e) ? 0 : 2;
            // <path>[:<presenter call>] or <path>:f<game frame> (UX stage 8: a shot at a known point of a replay)
            if (shot == 0) { snprintf(pbuf, sizeof pbuf, "%s", e); char* c = strrchr(pbuf, ':');
                             if (c && c[1]) { if (c[1] == 'f') at_frame = atoi(c + 2); else at = atoi(c + 1); *c = 0; } path = pbuf; }
        }
        calls++;
        if (shot == 0 && (at_frame >= 0 ? v2_dbg_pre_vm_iter >= at_frame : calls >= at)) {
            std::vector<uint8_t> px((size_t)W * Hout * 3);
            if (SDL_RenderReadPixels(myRenderer_v2, NULL, SDL_PIXELFORMAT_RGB24, px.data(), W * 3) == 0) {
                FILE* f = fopen(path, "wb");
                if (f) { fprintf(f, "P6\n%d %d\n255\n", W, Hout); fwrite(px.data(), 1, px.size(), f); fclose(f); }
                fprintf(stderr, "V2-PRESENT-SHOT: %s %dx%d picture %dx%d at (%d,%d) filter=%d int=%d border=%d H=%d\n",
                        path, W, Hout, dst.w, dst.h, dst.x, dst.y, filter, (int)integer, border, H);
            }
            shot = 1;
        }
    }
    SDL_RenderPresent(myRenderer_v2);
    v2_vsync_on_present();   // the display's vsync -> the game's (see the module above)
}
uint16_t input_keys_v2 = 0;
bool need_quit_v2 = false;  // Не используется, но оставим для совместимости

unsigned int plane4_to_linear_v2(unsigned int plane, unsigned int offset)
{
  return offset * 4 + plane;
}

uint32_t planar_to_linear_v2(uint32_t x, uint32_t y)
{
  return (y * RENDER_WIDTH_V2 + x);
}

void updateDraw_v2()
{
  static int call_count = 0;
  call_count++;

  // stableBuffer is now linear (y*344+x), no page offset needed.
  uint8_t* buf = myDrawInfo_v2->stableBuffer;

  // Black-screen forensics (2026-08-28 user report: sound+gameplay alive,
  // window black): once a second sum the presented pixel bytes and the
  // palette — tells WHICH stage is dark (pixels vs palette vs blit).
  {
    static int diag = -1;
    if (diag < 0) diag = getenv("V2_PRESENT_DIAG") ? 1 : 0;
    static uint32_t last_ms = 0;
    uint32_t now = SDL_GetTicks();
    if (diag && now - last_ms >= 1000) {
      last_ms = now;
      uint32_t psum = 0, palsum = 0;
      for (int i = 0; i < RENDER_HEIGHT_V2 * RENDER_WIDTH_V2; i += 7) psum += buf[i];
      for (int i = 0; i < 256; i++) {
        auto& c = myDrawInfo_v2->drawPalette[i];
        palsum += c.r + c.g + c.b;
      }
      extern SDL_Color v2_display_palette[256];
      extern bool v2_display_palette_valid;
      extern uint8_t v2_vga[65536 * 4];
      extern uint8_t v2_render_buf[V2_FB_MAX_W*240];
      extern uint8_t v2_display_buf[];
      extern uint16_t v2_vga_crtc, v2_vga_pan;
      uint32_t vsum = 0, rsum = 0, dsum = 0;
      for (int i = 0; i < 65536 * 4; i += 97) vsum += v2_vga[i];
      for (int i = 0; i < 320 * 240; i += 7) rsum += v2_render_buf[i];
      for (int i = 0; i < 320 * 176; i += 7) dsum += v2_display_buf[i];
      fprintf(stderr, "V2-PRESENT: calls=%d stable_sum=%u pal_sum=%u pubvalid=%d "
              "vga_sum=%u rbuf_sum=%u dbuf_sum=%u crtc=%04X pan=%u\n",
              call_count, psum, palsum, (int)v2_display_palette_valid,
              vsum, rsum, dsum, v2_vga_crtc, v2_vga_pan);
    }
  }

  for (int i = 0; i < RENDER_HEIGHT_V2 * RENDER_WIDTH_V2; i++)
  {
    auto color = buf[i];
    auto sdl_color = myDrawInfo_v2->drawPalette[color];
    tempDrawBuffer_v2[i] = SDL_MapRGBA(myFormat_v2, sdl_color.r, sdl_color.g, sdl_color.b, sdl_color.a);
  }
  
  // The DOS raster is 320x240 Mode X (square pixels): 176 viewport + the 64-row
  // HUD band (reference_vga_mode_x: split at line 176, HUD rows 176..239). So a
  // normal level's picture is the full 240 rows — step 3 wrongly cut it to 200
  // (a 24-row HUD) and lost the bottom 40 HUD rows (2026-09-06 report). A
  // full-screen LVX scene / an LVX_TALL224 level shows its map rows (224) with
  // no HUD band and black below; content_h only places the overlay's toast
  // above that bottom edge — the presented canvas is always the 240-row raster.
  const int content_h = v2_display_fullscreen ? v2_display_fullscreen : 240;
  v2_ui_draw(tempDrawBuffer_v2, RENDER_WIDTH_V2, content_h, myFormat_v2);   // UX stage 3 overlay
  // debug: V2_UI_SHOT=<path.ppm> dumps the presented frame once while the
  // options menu is open (the overlay lives only in this 32-bit buffer)
  {
      static int shot = -1; static const char* sp = nullptr;
      if (shot < 0) { sp = getenv("V2_UI_SHOT"); shot = (sp && *sp) ? 0 : 2; }
      if (shot == 0 && v2_ui_menu_open.load()) {
          FILE* f = fopen(sp, "wb");
          if (f) {
              fprintf(f, "P6\n%d %d\n255\n", RENDER_WIDTH_V2, RENDER_HEIGHT_V2);
              for (int i = 0; i < RENDER_WIDTH_V2 * RENDER_HEIGHT_V2; i++) {
                  Uint8 r, g, b, a; SDL_GetRGBA(tempDrawBuffer_v2[i], myFormat_v2, &r, &g, &b, &a);
                  fputc(r, f); fputc(g, f); fputc(b, f);
              }
              fclose(f); shot = 1;
          }
      }
  }
  v2_present_frame(SCREEN_HEIGHT_V2);   // TRIAL (not committed): constant 240-row canvas, content unstretched
}

std::thread render_thread_v2;

void render_thread_proc_v2(void* _state)
{
  // myDrawInfo_v2 is allocated in render_init_v2() on the game thread BEFORE this
  // detached thread starts (#179). It doubles as the "v2 mirror enabled" gate
  // (`if (myDrawInfo_v2)` throughout seg000); calloc'ing it here raced both the
  // game thread's first drawPixel (NULL deref @ f0) AND v2 activation timing
  // (under load v2 could fail to start → false-pass with no verification).
  assert(myDrawInfo_v2);

  // Задержка чтобы первое окно успело инициализироваться
  printf("render_v2: Starting initialization (after 200ms delay)...\n");
  SDL_Delay(200);
  
  // SDL уже инициализирован первым окном, но это безопасно
  if( SDL_Init( SDL_INIT_VIDEO ) < 0 )
  {
    printf( "SDL v2 could not initialize! SDL_Error: %s\n", SDL_GetError() );
  }
  
  printf("render_v2: Creating window...\n");
  
  // Получаем размеры экрана для позиционирования в правый нижний угол
  SDL_DisplayMode display_mode;
  int window_width = SCREEN_WIDTH_V2 * SCREEN_SCALE_V2;
  int window_height = SCREEN_HEIGHT_V2 * SCREEN_SCALE_V2;
  if (const char* e = getenv("V2_WINDOW_SIZE")) {          // UX stage 9: WxH (presenter layout tests)
      int w = 0, h = 0;
      if (sscanf(e, "%dx%d", &w, &h) == 2 && w >= 320 && h >= 200) { window_width = w; window_height = h; }
  }
  int pos_x, pos_y;
  
  if (SDL_GetCurrentDisplayMode(0, &display_mode) == 0) {
    // Успешно получили размеры экрана
    int screen_width = display_mode.w;
    int screen_height = display_mode.h;
    
    // Вычисляем позицию для правого нижнего угла
    pos_x = screen_width - window_width - 10;   // 10px отступ от края
    pos_y = screen_height - window_height - 50; // 50px отступ снизу (для панели задач)
    
    printf("render_v2: Screen size: %dx%d, positioning at (%d, %d)\n", 
           screen_width, screen_height, pos_x, pos_y);
  } else {
    // Fallback: если не удалось получить размеры экрана
    printf("render_v2: Could not get display mode, using fallback position\n");
    pos_x = 1270;  // Примерная позиция для 1920x1080
    pos_y = 670;
  }
  
  // Nearest-neighbor scaling so pixel art stays crisp at any window size
  // (also the SDL default, but pin it explicitly for portability).
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

  // Создаем второе окно в правом нижнем углу (resizable, fullscreen via F11).
  myWindow_v2 = SDL_CreateWindow(
    "Lost Vikings - Test Renderer V2",
    pos_x, pos_y,
    window_width, window_height,
    SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
  );

  if( myWindow_v2 == NULL )
  {
    printf( "Window v2 could not be created! SDL_Error: %s\n", SDL_GetError() );
  }
  else
  {
    printf("render_v2: Window created successfully!\n");
    printf("render_v2: Creating renderer...\n");
    // UX stage 9: present at the display's refresh (vsync) — the presenter
    // paces itself on SDL_RenderPresent then (v2_present_sleep skips its
    // sleep); the game tick keeps its own pacing (v2_main.cpp) and the
    // vsync-wait loop its own (v2_tick_sleep), so game speed is unchanged.
    // V2_NO_PRESENT_VSYNC=1 restores the free-running 15 ms presenter.
    {
        const char* e = getenv("V2_NO_PRESENT_VSYNC");
        Uint32 rf = SDL_RENDERER_ACCELERATED | ((e && *e == '1') ? 0 : SDL_RENDERER_PRESENTVSYNC);
        myRenderer_v2 = SDL_CreateRenderer(myWindow_v2, -1, rf);
        SDL_RendererInfo ri;
        v2_present_vsync = myRenderer_v2 && SDL_GetRendererInfo(myRenderer_v2, &ri) == 0 &&
                           (ri.flags & SDL_RENDERER_PRESENTVSYNC);
        v2_vsync_query_display(myWindow_v2);
        printf("render_v2: presenter %s, display mode %.0f Hz (the game's vsync follows the display once the present interval is measured)\n",
               v2_present_vsync ? "vsync" : "15 ms sleep", g_display_hz);
    }
    // UX stage 9 step 3: no SDL logical size — v2_present_frame lays the
    // picture out itself (aspect / integer scale / filter / border options)
    // and creates the source texture with the sampling the FILTER asks for.
    myTexture_v2 = nullptr;
    myFormat_v2 = SDL_AllocFormat(SDL_PIXELFORMAT_RGBA8888);

#ifdef V2_ONLY
    // UX stage 8: game controllers (a failure only means no pads — the
    // headless/CI boxes have none). Already-plugged pads arrive as
    // SDL_CONTROLLERDEVICEADDED events on the first polls.
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0)
        printf("render_v2: no game controller support (%s)\n", SDL_GetError());
#endif
    printf("render_v2: Entering main loop...\n");

    int loop_counter = 0;
    while (!need_quit)  // Используем флаг первого окна
    {
#ifdef V2_ONLY
      // V2_ONLY: orig window hidden, no event handler there → handle events here.
      // v2_input_poll_event = drop-in SDL_PollEvent wrapper for record/replay.
      extern uint16_t input_keys, input_keys_v2;
      SDL_Event event;
      while (v2_input_poll_event(&event) > 0) {
          // UX stage 3: F1 options menu (every mode) + --debug tools; a
          // consumed key never reaches the game input, and an open menu
          // drops the held game keys so nothing sticks under it.
          if (v2_ui_handle_event(&event)) {
              if (v2_ui_menu_open.load()) { input_keys = 0; input_keys_v2 = 0; }
              continue;
          }
          switch (event.type) {
          case SDL_QUIT:
              need_quit = true;
              v2_net_shutdown();          // UX stage 8 step 3: tell the peers before the hard exit
              fflush(stdout);
              v2_midi_shutdown();   // UX stage 11: V2_MIDI_DUMP (_exit skips atexit)
              _exit(0);
              break;
          case SDL_KEYDOWN:
          case SDL_KEYUP: {
              // F11 = toggle desktop-fullscreen (handled before keymap so it
              // never reaches game-input mapping).
              if (event.type == SDL_KEYDOWN && !event.key.repeat &&
                  event.key.keysym.sym == SDLK_F11) {
                  Uint32 wf = SDL_GetWindowFlags(myWindow_v2);
                  SDL_SetWindowFullscreen(myWindow_v2,
                      (wf & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                  break;
              }
              // UX stage 8 step 3: in a lockstep game the key is this client's
              // event for a later read, applied then on every client — never
              // straight into the words (the replay path applies it).
              if (v2_net_active()) { v2_input_net_capture(&event); break; }
              uint16_t key_val = 0;
              uint16_t spec_off = 0;
              v2_keymap_lookup_sdl(event.key.keysym.sym, &key_val, &spec_off);
              if (event.type == SDL_KEYDOWN) {
                  // #62: the INT9 letter channel ([28C] = LUT[scancode]) was
                  // fed ONLY by the default-window handler — in V2_ONLY the
                  // password screen never received letters/Enter. Same shared
                  // writer as render.cpp (typematic repeats included).
                  sdl_int9_note_keydown(event.key.keysym.scancode);
                  // (#81) tap accumulator: without this V2_ONLY lost any
                  // KEYDOWN+KEYUP shorter than one 12352 interval (the
                  // default-window handler feeds it in render.cpp:794).
                  if (key_val && !event.key.repeat) {
                      extern std::atomic<uint16_t> sdl_input_press_edges;
                      sdl_input_press_edges.fetch_or(key_val, std::memory_order_relaxed);
                  }
                  input_keys |= key_val; input_keys_v2 |= key_val;
              } else {
                  input_keys &= ~key_val; input_keys_v2 &= ~key_val;
              }
              if (spec_off) {
                  extern std::atomic<uint8_t> sdl_spec_state[256];
                  extern std::atomic<uint8_t> sdl_spec_press_latch[256];
                  // EXACT orig INT9 replication: KEYDOWN scancode → byte_316XX=1,
                  // KEYUP (release scancode) → byte_316XX=0, for ALL spec keys.
                  // Matches render.cpp behavior. Without clear on KEYUP, F10/X/Q
                  // etc. stay 1 forever → triggers re-fire every iter (e.g.,
                  // F10 menu reopens immediately after N dismissal).
                  if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
                      sdl_spec_state[spec_off & 0xFF].store(1, std::memory_order_relaxed);
                      sdl_spec_press_latch[spec_off & 0xFF].store(1, std::memory_order_relaxed);
                  } else if (event.type == SDL_KEYUP) {
                      sdl_spec_state[spec_off & 0xFF].store(0, std::memory_order_relaxed);
                  }
              }
              break;
          }
          // UX stage 8: game controllers — controller i is player i+1 (the first
          // one doubles the keyboard of player 1); buttons/sticks map to the
          // DOS action bits (v2_coop_pad_events). Player 1's bits take the
          // keyboard path above (tap accumulator + held words), the others go
          // to their player's accumulators. A recording keeps them as actions
          // (`ACTION@k` for the pads of players 2..3); in a lockstep game the
          // first pad is this client's player and goes through the batches.
          case SDL_CONTROLLERDEVICEADDED:   v2_coop_pad_added(event.cdevice.which); break;
          case SDL_CONTROLLERDEVICEREMOVED: v2_coop_pad_removed(event.cdevice.which); break;
          case SDL_CONTROLLERBUTTONDOWN:
          case SDL_CONTROLLERBUTTONUP:
          case SDL_CONTROLLERAXISMOTION: {
              V2CoopPadEv ev[4]; int n = v2_coop_pad_events(&event, ev, 4);
              for (int i = 0; i < n; i++) {
                  v2_input_record_bits(ev[i].player, ev[i].bits, ev[i].down ? 1 : 0);
                  if (v2_net_active()) {
                      if (ev[i].player == 0) v2_input_net_capture_bits(ev[i].bits, ev[i].down ? 1 : 0);
                      continue;
                  }
                  if (ev[i].player > 0) { v2_coop_key(ev[i].player, ev[i].bits, ev[i].down, false); continue; }
                  if (ev[i].down) {
                      extern std::atomic<uint16_t> sdl_input_press_edges;
                      sdl_input_press_edges.fetch_or(ev[i].bits, std::memory_order_relaxed);
                      input_keys |= ev[i].bits; input_keys_v2 |= ev[i].bits;
                  } else {
                      input_keys &= (uint16_t)~ev[i].bits; input_keys_v2 &= (uint16_t)~ev[i].bits;
                  }
              }
              break;
          }
          }
      }
#endif
      // НЕ вызываем SDL_PollEvent (test mode) - события обрабатываются только в первом окне
      // Это избегает конфликтов с обработкой событий

      if ((loop_counter & 127) == 0) v2_vsync_query_display(myWindow_v2);   // the window may have moved to another display
      v2_pacing_apply(myRenderer_v2);   // PACING < VSYNC | VRR >
      if (v2_pacing_vrr()) v2_presenter_wait_flip(20);   // VRR: one present per game flip, the display follows the game
      else v2_vsync_latch_sleep();      // with the vsync lock: snapshot just before the refresh, after the game's flip for it
      render_callback_v2(_state);  // snapshot drawBuffer→stableBuffer + sprite replay
      // STATS: sub-frame delivery — how many of the game's distinct flips this present skipped
      // (drops) or repeated (doubles, counted only when the game is flipping and nothing is
      // interpolated: one present per sub-frame is the contract of the 60 Hz lock)
      { static uint32_t seen = 0; static bool init = false;
        extern uint32_t v2_smooth_subframe_seq(void); extern uint64_t v2_smooth_last_flip_ticks(void);
        const uint32_t seq = v2_smooth_subframe_seq();
        if (init) {
            const uint32_t d = seq - seen;
            const double since_flip_ms = (double)(SDL_GetPerformanceCounter() - v2_smooth_last_flip_ticks()) / (double)SDL_GetPerformanceFrequency() * 1000.0;
            const bool flowing = since_flip_ms < 3.0 * 16.7;
            if (d >= 2) v2_stats.flip_drops.fetch_add(d - 1, std::memory_order_relaxed);
            if (d == 0 && flowing && g_vs_locked.load(std::memory_order_relaxed) && !v2_smooth_effective() && !v2_pacing_vrr())
                v2_stats.flip_doubles.fetch_add(1, std::memory_order_relaxed);
        }
        seen = seq; init = true; }
#ifndef HEADLESS
      updateDraw_v2();             // читает только stableBuffer
#else
      // task #36: headless dummy video — pure presenter blit skipped (the
      // stableBuffer snapshot above still runs: it feeds the A2 extraction).
#endif
      // Frame counter in the window title (updated every ~10 frames) — handy
      // when recording replays: the number matches the .inp frame column.
      {
          // v2_dbg_pre_vm_iter: file-scope extern (top of file)
          static int last_shown = -1;
          int f = v2_dbg_pre_vm_iter;
          if (f - last_shown >= 10 || f < last_shown) {
              last_shown = f;
              char t[64];
              snprintf(t, sizeof(t), "Lost Vikings v2 - frame %d", f);
              if (myWindow_v2) SDL_SetWindowTitle(myWindow_v2, t);
          }
      }
      if (!v2_pacing_vrr()) v2_present_sleep();   // stage 6.3: single pacing source (v2_timing.h); VRR paces on the flips

      loop_counter++;
    }
  }
}

void render_init_v2(void* state)
{
  // Allocate myDrawInfo_v2 synchronously on the game thread BEFORE spawning the
  // detached v2 render thread — it gates the entire v2 mirror and is written via
  // drawPixel from frame 0. Async calloc in the thread raced startup under heavy
  // parallel load → NULL deref @ f0 / nondeterministic v2 activation (#179).
  if (!myDrawInfo_v2) {
    myDrawInfo_v2 = (myDrawInfoS_v2 *)calloc(1, sizeof(myDrawInfoS_v2));
    assert(myDrawInfo_v2);
  }
  render_thread_v2 = std::thread(render_thread_proc_v2, state);
  render_thread_v2.detach();
}
