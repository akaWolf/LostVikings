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

#include "../v2_midi.h"   // UX stage 11: V2_MIDI_DUMP is written before the _exit paths
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#ifndef _WIN32
#include <unistd.h>   // _exit
#ifdef FT_COV_BUILD
extern "C" void __gcov_dump(void);   // libgcov: flush counters before _exit
#endif
#endif
#include "headless_dump.h"
#include "../v2_input_recorder.h"
#include "../v2_net.h"   // UX stage 8 step 3: close the lockstep sockets before the _exit below
#include "../v2_keymap.h"

extern int v2_dbg_pre_vm_iter;
extern "C" void v2_fntest_report(void);  // FN-TEST summary (FNTEST env); _exit() skips atexit
extern uint8_t* v2_vm_get_shadow_ds(void);            // v2_vm.cpp (C++ linkage)
extern "C" void v2_gs_dump_text(const uint8_t*, const char*);  // v2_gamestate.cpp
extern "C" int v2_state_save(const char*);            // v2_vm.cpp (direction V step 2)

// (direction V) golden end-state snapshot: the named-field text dump of the
// final shadow DS. Called from EVERY clean exit path — the max-frames exit
// below and the v2 quit-path _exit(0) sites in v2_vm.cpp (F10/ALT+X quit
// scenarios end there, not here). Idempotent: first call wins, so a quit
// dump is not overwritten by a later max-frames dump. The per-scenario
// exit point is deterministic, so this IS the checkpoint the
// tests/golden_states/ catalog compares against — the phase-D oracle that
// survives the verify-scaffolding teardown.
// headless_golden_dump moved to v2_gamestate.cpp — common to all builds
// (V2_ONLY gencode/soak binaries need the same clean-exit snapshots).
extern "C" void headless_golden_dump(void);

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
        { extern void v2_flipring_dump(void); v2_flipring_dump(); }  // #32 probe
        headless_golden_dump();
#ifndef V2_ONLY
        v2_fntest_report();   // fn-test infra is not part of the V2_ONLY build
#endif
        v2_net_shutdown();    // UX stage 8 step 3: the peers see the close, the summary line lands (no-op without a game)
        v2_midi_shutdown();   // UX stage 11: V2_MIDI_DUMP
        fflush(stdout); fflush(stderr);
        // COV_SEG000 builds: flush gcov counters before the atexit-skipping
        // _exit — replays feed the merged coverage profile (#47 step 1).
        // (a block-scope weak declaration silently dropped the attribute and
        // the call never flushed — hence the honest ifdef)
#ifdef FT_COV_BUILD
        __gcov_dump();
#endif
        _exit(0);
    }
    return 0;
}
