#pragma once
#include <cstdint>
#include <SDL2/SDL.h>

// HEADLESS divergence dump infrastructure.
// On first divergence detected by any verify mechanism (PSNAP, trace_compare,
// A2 render, audio audit, etc.), dumps both render buffers as PPM images +
// DS state binaries + diff text, then exits with code 1.
//
// Output directory: configurable via --dump-dir=<path>, default /tmp/headless_<pid>/

// All functions use C++ linkage (no extern "C") to match call sites.

// Initialize dump system. Creates output directory. Call at startup.
void headless_dump_init(const char* dump_dir);

// Primary divergence hook. Called from verify mechanisms.
// source: short tag like "PSNAP", "trace", "A2", "audit", "gameloop"
// frame: v2_dbg_pre_vm_iter at detection
// detail: human-readable extra (e.g., "0x14E5 orig=1234 v2=5678")
// Triggers: PPM dumps + DS binary dumps + context.txt + exit(1).
// Idempotent — only first call does work, subsequent calls return immediately.
void headless_dump_divergence(const char* source, int frame, const char* detail);

// Manual PPM writer (also used for diagnostics).
// pixels: 1-byte-per-pixel linear, w*h bytes.
// palette: 256 SDL_Color entries (r/g/b/a). a ignored, written as RGB only.
void headless_write_ppm(const char* path, const uint8_t* pixels,
                        int w, int h, const SDL_Color* palette);

// Diff PPM: pixels where a != b → RED, else copy from a.
void headless_write_diff_ppm(const char* path,
                             const uint8_t* a, const uint8_t* b,
                             int w, int h, const SDL_Color* palette);

// SIGSEGV handler — best-effort dump current state, exit(4).
void headless_install_sigsegv_handler(void);

// Render-diff logger — NON-critical: appends frame info to render_diffs.log
// inside dump dir. Does NOT exit. Use for divergences expected/known to be
// render-layer (where orig/v2 pixels differ but DS state matches). Summary
// printed at clean exit via headless_dump_render_diff_summary().
void headless_log_render_diff(int frame, int viewport_diff, int x, int y,
                              uint32_t orig_hash, uint32_t v2_hash);

// Print + write final summary of accumulated render diffs.
// Called at normal exit (max-frames) and on critical divergence.
void headless_dump_render_diff_summary(void);
