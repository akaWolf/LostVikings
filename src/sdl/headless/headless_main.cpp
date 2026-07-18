// HEADLESS main initialization — CLI parsing, env setup, hook installation.
//
// Called from default mode main() (asm.cpp) when -DHEADLESS is set.
// Strategy: keep SDL linked but use dummy video/audio drivers so no real
// display/audio device needed. CI machines without X server / sound card
// can run this binary.
//
// Headless-specific CLI:
//   --replay-input=<file>   REQUIRED. Recorder file driving input.
//   --dump-dir=<path>       Output dir for divergence dumps (default /tmp/headless_<pid>/)
//   --max-frames=<N>        Exit cleanly after N idle frames past replay exhaustion
//   --seed=<N>              PRNG seed (default 0)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#ifndef _WIN32
#include <unistd.h>   // _exit
#endif
#include "headless_dump.h"
#include "../v2_input_recorder.h"
#include "../v2_keymap.h"

extern int v2_dbg_pre_vm_iter;
extern "C" void v2_fntest_report(void);  // FN-TEST summary (FNTEST env); _exit() skips atexit

// Globals controlling headless behavior (referenced from verify hooks etc.)
const char* g_headless_replay_input = nullptr;
const char* g_headless_dump_dir = nullptr;
const char* g_headless_keymap = nullptr;
int g_headless_max_frames = 10000;
int g_headless_seed = 0;
std::atomic<int> g_headless_replay_exhausted_frame{-1};  // frame when queue ran out

void headless_parse_cli(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--replay-input=", 15) == 0) {
            g_headless_replay_input = argv[i] + 15;
        } else if (strncmp(argv[i], "--dump-dir=", 11) == 0) {
            g_headless_dump_dir = argv[i] + 11;
        } else if (strncmp(argv[i], "--max-frames=", 13) == 0) {
            g_headless_max_frames = atoi(argv[i] + 13);
        } else if (strncmp(argv[i], "--seed=", 7) == 0) {
            g_headless_seed = atoi(argv[i] + 7);
        } else if (strncmp(argv[i], "--keymap=", 9) == 0) {
            g_headless_keymap = argv[i] + 9;
        }
    }
}

void headless_init(int argc, char* argv[]) {
    headless_parse_cli(argc, argv);

    if (!g_headless_replay_input) {
        fprintf(stderr, "HEADLESS: --replay-input=<file> is REQUIRED\n");
        fprintf(stderr, "Usage: vikings_headless --replay-input=<file> [--dump-dir=<path>] [--max-frames=N] [--seed=N]\n");
        exit(2);
    }

    // Force SDL to use dummy drivers — no display/audio needed for headless.
    // Done BEFORE any SDL_Init call (env vars consulted at init time).
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("SDL_AUDIODRIVER", "dummy", 1);

    // Init dump system + signal handler
    headless_dump_init(g_headless_dump_dir);
    headless_install_sigsegv_handler();

    // Keymap before recorder (recorder consults v2_keymap for action lookup).
    v2_keymap_load(g_headless_keymap);

    // Init input recorder — strict mode (CI: no human input ever)
    v2_input_recorder_init(nullptr, g_headless_replay_input, /*strict=*/1);

    fprintf(stderr, "HEADLESS: initialized. replay=%s max_frames=%d seed=%d\n",
            g_headless_replay_input, g_headless_max_frames, g_headless_seed);
}

// Called from game loop to enforce max-frames timeout.
// Returns true if exit condition met.
int headless_check_exit(void) {
    if (v2_dbg_pre_vm_iter >= g_headless_max_frames) {
        fprintf(stderr, "HEADLESS: max-frames=%d reached, exiting cleanly\n",
                g_headless_max_frames);
        // Final reports: SFX audit + opcode coverage + PSNAP summary +
        // render-diff summary (non-critical render divergences accumulated).
        extern void v2_audit_dump_final();
        extern void v2_dump_opcode_coverage();
        extern void v2_dump_psnap_summary();
        extern void headless_dump_render_diff_summary();
        v2_audit_dump_final();
        v2_dump_opcode_coverage();
        v2_dump_psnap_summary();
        headless_dump_render_diff_summary();
#ifndef V2_ONLY
        v2_fntest_report();   // fn-test infra is not part of the V2_ONLY build
#endif
        fflush(stdout); fflush(stderr);
        _exit(0);
    }
    return 0;
}
