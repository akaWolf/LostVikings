// HEADLESS divergence dump implementation.
// See header for protocol.

#include "headless_dump.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#include <csignal>
#include <atomic>

#ifdef __linux__
#include <execinfo.h>
#endif

// Externs: defined elsewhere in v2 codebase.
extern uint8_t* v2_vm_get_shadow_ds();
extern uint8_t* v2_vm_get_real_ds();
extern int      v2_dbg_pre_vm_iter;
extern bool     need_quit;

#define v2_vm_real_ds_ptr  v2_vm_get_real_ds()
#define v2_vm_shadow_ds    v2_vm_get_shadow_ds()

// Externs: render buffers.
extern uint8_t v2_render_buf[320*200];
extern uint8_t v2_hud_buf[320*64];

// Orig drawBuffer access via myDrawInfo (defined in render.cpp).
struct myDrawInfoS_dump {
    uint8_t drawBuffer[65536 * 4];
    SDL_Color drawPalette[256];
    uint32_t myOffset;
    uint8_t  myPixelOffset;
};
extern struct myDrawInfoS_dump* myDrawInfo;

namespace {

std::atomic<bool> g_already_dumped{false};
char g_dump_dir[512] = "/tmp/headless_unset";

void mkdir_p(const char* path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

void write_binary(const char* path, const void* data, size_t size) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fwrite(data, 1, size, f);
    fclose(f);
}

// CRTC unfold of the visible orig page (task #19): drawBuffer models VGA
// Mode X memory ×4 (byte_addr*4 + plane); the CRT scans each screen row from
// myOffset + y*0x56 (CRTC offset reg 0x13 = 0x2B words = 86 bytes/row), plus
// pixel pan myPixelOffset (0..3). A flat 320-wide read shears 24 px/row.
uint8_t g_orig_unfold[320 * 176];
const uint8_t* orig_page_unfold() {
    if (!myDrawInfo) return nullptr;
    for (uint32_t y = 0; y < 176; y++) {
        uint32_t base = (myDrawInfo->myOffset + y * 0x56u) * 4u + myDrawInfo->myPixelOffset;
        if (base + 320 > sizeof(myDrawInfo->drawBuffer)) return nullptr;
        memcpy(g_orig_unfold + y * 320, myDrawInfo->drawBuffer + base, 320);
    }
    return g_orig_unfold;
}

} // namespace

void headless_dump_init(const char* dump_dir) {
    if (dump_dir && dump_dir[0]) {
        snprintf(g_dump_dir, sizeof(g_dump_dir), "%s", dump_dir);
    } else {
        snprintf(g_dump_dir, sizeof(g_dump_dir), "/tmp/headless_%d", (int)getpid());
    }
    mkdir_p(g_dump_dir);
    fprintf(stderr, "headless_dump: dump directory = %s\n", g_dump_dir);
}

void headless_write_ppm(const char* path, const uint8_t* pixels,
                                    int w, int h, const SDL_Color* palette) {
    if (!path || !pixels) return;
    FILE* f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "headless_dump: PPM open FAILED: %s\n", path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    if (palette) {
        for (int i = 0; i < w * h; i++) {
            uint8_t c = pixels[i];
            uint8_t rgb[3] = {palette[c].r, palette[c].g, palette[c].b};
            fwrite(rgb, 1, 3, f);
        }
    } else {
        // No palette → grayscale
        for (int i = 0; i < w * h; i++) {
            uint8_t c = pixels[i];
            uint8_t rgb[3] = {c, c, c};
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

void headless_write_diff_ppm(const char* path,
                                         const uint8_t* a, const uint8_t* b,
                                         int w, int h, const SDL_Color* palette) {
    if (!path || !a || !b) return;
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        uint8_t rgb[3];
        if (a[i] != b[i]) {
            // Differ → RED
            rgb[0] = 255; rgb[1] = 0; rgb[2] = 0;
        } else if (palette) {
            rgb[0] = palette[a[i]].r;
            rgb[1] = palette[a[i]].g;
            rgb[2] = palette[a[i]].b;
        } else {
            rgb[0] = rgb[1] = rgb[2] = a[i];
        }
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

void headless_dump_divergence(const char* source, int frame, const char* detail) {
    // Idempotent — first call wins.
    bool expected = false;
    if (!g_already_dumped.compare_exchange_strong(expected, true)) {
        return;
    }

    char subdir[640];
    snprintf(subdir, sizeof(subdir), "%s/diverge_f%d_%s", g_dump_dir, frame, source);
    mkdir_p(subdir);

    fprintf(stderr, "\n=== HEADLESS DIVERGENCE [%s @ f%d] ===\n", source, frame);
    fprintf(stderr, "  detail: %s\n", detail ? detail : "(none)");
    fprintf(stderr, "  dump dir: %s\n", subdir);

    // 1. context.txt — human-readable summary
    {
        char p[768];
        snprintf(p, sizeof(p), "%s/context.txt", subdir);
        FILE* f = fopen(p, "w");
        if (f) {
            fprintf(f, "source: %s\nframe: %d\ndetail: %s\n",
                    source, frame, detail ? detail : "");
            if (v2_vm_shadow_ds) {
                fprintf(f, "level: 0x%04X\n", *(uint16_t*)(v2_vm_shadow_ds + 0x25AD));
                fprintf(f, "obj_idx: 0x%04X\n", *(uint16_t*)(v2_vm_shadow_ds + 0x42));
            }
            fclose(f);
        }
    }

    // 2. DS binary dumps
    if (v2_vm_real_ds_ptr) {
        char p[768];
        snprintf(p, sizeof(p), "%s/ds_orig.bin", subdir);
        write_binary(p, v2_vm_real_ds_ptr, 0x10000);
    }
    if (v2_vm_shadow_ds) {
        char p[768];
        snprintf(p, sizeof(p), "%s/ds_v2.bin", subdir);
        write_binary(p, v2_vm_shadow_ds, 0x10000);
    }

    // 3. DS diff text — first 100 byte differences
    if (v2_vm_real_ds_ptr && v2_vm_shadow_ds) {
        char p[768];
        snprintf(p, sizeof(p), "%s/ds_diff.txt", subdir);
        FILE* f = fopen(p, "w");
        if (f) {
            int diffs = 0;
            for (uint32_t i = 0; i < 0x10000 && diffs < 100; i++) {
                uint8_t r = v2_vm_real_ds_ptr[i];
                uint8_t s = v2_vm_shadow_ds[i];
                if (r != s) {
                    fprintf(f, "  0x%04X: orig=0x%02X v2=0x%02X (diff=%+d)\n",
                            (uint16_t)i, r, s, (int)s - (int)r);
                    diffs++;
                }
            }
            fprintf(f, "(total scanned 64KB, first 100 diffs shown)\n");
            fclose(f);
        }
    }

    // 4. PPM dumps — both render buffers (viewport region 320x176).
    // Orig side CRTC-unfolded (pitch 0x56 bytes/row — task #19).
    if (const uint8_t* orig_px = orig_page_unfold()) {
        char p[768];
        snprintf(p, sizeof(p), "%s/orig_buffer.ppm", subdir);
        headless_write_ppm(p, orig_px, 320, 176, myDrawInfo->drawPalette);
    }
    {
        char p[768];
        snprintf(p, sizeof(p), "%s/v2_buffer.ppm", subdir);
        // Use orig palette if available, else null (grayscale)
        const SDL_Color* pal = myDrawInfo ? myDrawInfo->drawPalette : nullptr;
        headless_write_ppm(p, v2_render_buf, 320, 176, pal);
    }
    if (const uint8_t* orig_px = orig_page_unfold()) {
        char p[768];
        snprintf(p, sizeof(p), "%s/diff.ppm", subdir);
        headless_write_diff_ppm(p, orig_px, v2_render_buf,
                                320, 176, myDrawInfo->drawPalette);
    }

    // Dump final atexit reports (audit, opcode coverage, PSNAP summary, render
    // diff summary) before _exit bypasses atexit handlers.
    extern void v2_audit_dump_final();
    extern void v2_dump_opcode_coverage();
    extern void v2_dump_psnap_summary();
    v2_audit_dump_final();
    v2_dump_opcode_coverage();
    v2_dump_psnap_summary();
    headless_dump_render_diff_summary();

    fprintf(stderr, "=== Dump complete. Exit code 1. ===\n");
    fflush(stdout); fflush(stderr);
    need_quit = true;
    _exit(1);
}

static void headless_sigsegv_handler(int sig) {
    fprintf(stderr, "\n=== HEADLESS SIGSEGV (sig=%d) @ f%d ===\n", sig, v2_dbg_pre_vm_iter);
#ifdef __linux__
    void* bt[40];
    int n = backtrace(bt, 40);
    backtrace_symbols_fd(bt, n, fileno(stderr));
#endif
    // Try to dump current state — may fail if memory corrupt.
    headless_dump_divergence("SEGFAULT", v2_dbg_pre_vm_iter, "process crashed");
    _exit(4);
}

void headless_install_sigsegv_handler(void) {
    signal(SIGSEGV, headless_sigsegv_handler);
    signal(SIGBUS,  headless_sigsegv_handler);
    signal(SIGABRT, headless_sigsegv_handler);
}

// ============================================================================
// Render-diff logger (non-critical) — accumulates frames with render divergence
// without exiting. Render divergences are common (rendering differences are
// often visual cosmetic / known issues vs. game-state divergence which is
// critical). Summary dumped at clean exit.
// ============================================================================
namespace {
struct RenderDiffEntry {
    int frame;
    int viewport_diff;
    int first_x, first_y;
    uint32_t orig_hash, v2_hash;
};
constexpr int RENDER_DIFF_MAX = 10000;
RenderDiffEntry g_render_diffs[RENDER_DIFF_MAX];
std::atomic<int> g_render_diff_count{0};
FILE* g_render_diff_log = nullptr;
}

void headless_log_render_diff(int frame, int viewport_diff, int x, int y,
                               uint32_t orig_hash, uint32_t v2_hash) {
    int idx = g_render_diff_count.fetch_add(1, std::memory_order_relaxed);
    if (idx < RENDER_DIFF_MAX) {
        g_render_diffs[idx] = {frame, viewport_diff, x, y, orig_hash, v2_hash};
    }
    // Lazy-open the log file inside dump dir on first call.
    if (!g_render_diff_log) {
        char p[768];
        snprintf(p, sizeof(p), "%s/render_diffs.log", g_dump_dir);
        g_render_diff_log = fopen(p, "w");
        if (g_render_diff_log) {
            fprintf(g_render_diff_log,
                "# render-diff frames (non-critical, render-layer divergence)\n"
                "# format: <frame> viewport_diff=<bytes> first@(<x>,<y>) "
                "orig_hash=<h> v2_hash=<h>\n");
        }
    }
    if (g_render_diff_log) {
        fprintf(g_render_diff_log,
            "%d viewport_diff=%d first@(%d,%d) orig_hash=%08X v2_hash=%08X\n",
            frame, viewport_diff, x, y, orig_hash, v2_hash);
        fflush(g_render_diff_log);
    }
}

void headless_dump_render_diff_summary(void) {
    // Dirty-lag stats (task #20/#21): frames whose only pixel diffs matched
    // another page of the 3-page rotation (orig dirty channels repaint a
    // changed object on [obj+0x114D]-counted pages only — the 0x202
    // sub-sprite channel covers 2 of 3, so one page lags a phase on real
    // DOS hardware too). These are NOT divergences; kept as a visible count.
    {
        extern uint64_t v2_render_lag_frames, v2_render_lag_px;
        if (v2_render_lag_frames)
            fprintf(stderr, "RENDER-LAG (legit dirty-page lag): frames=%llu px=%llu\n",
                    (unsigned long long)v2_render_lag_frames,
                    (unsigned long long)v2_render_lag_px);
    }
    int n = g_render_diff_count.load(std::memory_order_relaxed);
    if (n == 0) return;
    int captured = n < RENDER_DIFF_MAX ? n : RENDER_DIFF_MAX;
    int first = g_render_diffs[0].frame;
    int last  = g_render_diffs[captured - 1].frame;
    fprintf(stderr, "\n========== RENDER DIFF SUMMARY ==========\n");
    fprintf(stderr, "Total render-diff frames: %d (captured first %d)\n",
            n, captured);
    fprintf(stderr, "Frame range: f%d..f%d\n", first, last);
    if (g_dump_dir[0]) {
        fprintf(stderr, "Full per-frame log: %s/render_diffs.log\n", g_dump_dir);
    }
    fprintf(stderr, "Note: render diffs are NON-CRITICAL (pixel-level only,\n");
    fprintf(stderr, "      DS state unaffected). Exit not triggered by these.\n");
    fprintf(stderr, "==========================================\n");
    if (g_render_diff_log) {
        fclose(g_render_diff_log);
        g_render_diff_log = nullptr;
    }
}
