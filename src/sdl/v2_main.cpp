// V2_ONLY entry point — replaces orig m2c main entry.
// No m2c-decompiled code is linked. v2 phase functions handle game on shadow DS.

// (VI.4) Windows: SDL2 renames main to SDL_main and expects its own WinMain
// bootstrap; the test-mode entry (asm.cpp) never includes SDL.h before
// main so it dodges this — V2_ONLY must opt out explicitly.
#define SDL_MAIN_HANDLED
#include "v2_midi.h"   // UX stage 11: V2_MIDI_DUMP is written before the _exit paths
#include "v2_sc55.h"   // UX stage 11: the SC-55 module boots before the game thread starts
#include "v2_mt32.h"   // UX stage 11: the MT-32 world (chunks, Munt) comes up before the game thread starts
#include "v2_ui.h"     // v2_options
#include <SDL2/SDL.h>
#include "v2_timing.h"
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "v2_input_recorder.h"
#include "v2_keymap.h"
#include "v2_coop.h"          // UX stage 8: --coop=N
#include "v2_net.h"           // UX stage 8 step 3: --host / --join / --delay
#include "v2_gamestate.h"   // stage 4 II.c: evac refresh after teleport load
#include "render_v2.h"   // V2_EXE_STATIC_SIZE
#include <csignal>
extern int v2_dbg_pre_vm_iter;   // game-frame counter (v2_vm.cpp), C++ linkage — declared once at file scope (clang rejects block externs inside extern "C" functions)

extern bool need_quit;

static void sigint_handler(int) {
    need_quit = true;
}
extern void render_init_v2(void* state);
extern void sound_init();

// v2 phase dispatcher: V2Phase enum + v2_signal_phase come from render_v2.h.
extern void v2_set_m2c_base(void* base);
extern void v2_vm_init_shadow_early(uint16_t ds_val);
extern void v2_run_animation_vm(uint16_t ds_val);
extern void v2_vm_verify_after_init(uint16_t ds_val);
extern void v2_game_thread_start();
extern void v2_game_thread_stop();

// Stubs for missing m2c globals (referenced by v2_vm.cpp / render.cpp etc).
namespace m2c {
    struct _STATE {};
    static _STATE _the_state;
    _STATE* k_state = &_the_state;
}

// Static EXE data: exe_static.bin is dumped from test-mode m2c::m[0..0x29F00]
// after the C++ Initializer in vikings.exe.cpp populates it. Without this data,
// seg001 text/menu region (+0x9480) is zero, sub_12529 reads width=0, sub_12388
// LOOPs cx=0xFFFE = 65k iterations → hang.
// V2_EXE_STATIC_SIZE comes from render_v2.h (shared with v2_resolve_segment's
// V2_ONLY fallback classifier).
static uint8_t v2_m2c_buf[0x100000] = {0};

// #104: LVX1 trailer parser lives in v2_vm.cpp (file-scope extern "C" —
// a block-scope extern would mangle as C++ and fail to link).
extern "C" void v2_lvx_load(const uint8_t* img, uint32_t size);

// The console content pack (the SNES / Genesis material of the UX plan —
// tools/assets/build_content.py, `make content`): a content/ directory with
// .compiled/ and exe_static.bin, beside the executable or in the working
// directory, is the asset store when neither V2_ASSETS_DIR nor V2_EXE_STATIC
// is set; either variable keeps precedence and disables the lookup for both
// (the store and the image must come from one tree). V2_CONTENT=0 turns the
// lookup off — the A/B benches of tests/ run the canon content from the repo
// root, where content/ may sit.
extern "C" void v2_assets_set_dir(const char* dir);   // v2_assets.cpp
static std::string g_content_dir;

static bool v2_content_dir_ok(const std::string& dir) {
    FILE* a = fopen((dir + "/.compiled/0000.bin").c_str(), "rb");
    FILE* b = fopen((dir + "/exe_static.bin").c_str(), "rb");
    if (a) fclose(a);
    if (b) fclose(b);
    return a && b;
}

static void v2_find_content() {
    const char* a = getenv("V2_ASSETS_DIR");
    const char* e = getenv("V2_EXE_STATIC");
    const char* c = getenv("V2_CONTENT");
    if ((a && *a) || (e && *e)) return;
    if (c && *c == '0') return;
    std::vector<std::string> cands;
    cands.push_back("content");
    char* base = SDL_GetBasePath();
    if (base) { cands.push_back(std::string(base) + "content"); SDL_free(base); }
    for (const std::string& d : cands) {
        if (v2_content_dir_ok(d)) { g_content_dir = d; break; }
    }
    if (g_content_dir.empty()) return;
    v2_assets_set_dir((g_content_dir + "/.compiled").c_str());
    printf("V2_ONLY: console content pack %s (V2_CONTENT=0 plays the archive as is)\n",
           g_content_dir.c_str());
}

static void v2_load_static_data() {
    // V2_EXE_STATIC: alternate image path (task #108 — the dialog texts
    // live in seg001 of this image; the editor plays patched copies);
    // else the content pack's image, else exe_static.bin of the working directory.
    const char* p = getenv("V2_EXE_STATIC");
    const std::string path = (p && *p) ? std::string(p)
                           : !g_content_dir.empty() ? g_content_dir + "/exe_static.bin"
                           : std::string("exe_static.bin");
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        fprintf(stderr,
            "V2_ONLY: exe_static.bin not found — text/menu rendering will hang.\n"
            "  Run default build once: `make -j$(nproc) && ./vikings` to generate it,\n"
            "  then rebuild with `V2_ONLY=1 make`.\n");
        return;
    }
    size_t n = fread(v2_m2c_buf, 1, V2_EXE_STATIC_SIZE, f);
    // #104: LVX1 trailer (extra level slots + their passwords) — it sits
    // PAST the 0x29F00 image, so it must be read separately (the fread
    // above stops exactly at the image size). Absent -> tn == 0 -> no-op.
    uint8_t lvx_tail[4096];
    size_t tn = fread(lvx_tail, 1, sizeof(lvx_tail), f);
    fclose(f);
    if (tn) v2_lvx_load(lvx_tail, (uint32_t)tn);
    else    v2_lvx_load(v2_m2c_buf, (uint32_t)n);
    printf("V2_ONLY: loaded %zu bytes from %s (seg001 sample @0x9480: "
           "%02X %02X %02X %02X)\n",
           n, path.c_str(), v2_m2c_buf[0x9480], v2_m2c_buf[0x9481], v2_m2c_buf[0x9482], v2_m2c_buf[0x9483]);
}

// --debug CLI flag: enable orig debug-build cheats (F4 INT 3, F5/F6 level cheats).
// Mirrors orig DOS conditional gated by word_286E2 (ds:0x202) being non-zero.
bool g_debug_mode = false;

// №59: --max-frames limit for plain V2_ONLY builds (0 = unlimited). The
// HEADLESS hook keeps its own enforcement; this one stops the main loop.
int g_v2only_max_frames = 0;   // read by the v2_nopl_pump quit choke too

extern "C" int v2_state_save(const char*);   // v2_vm.cpp (direction V step 2)
extern "C" void headless_golden_dump(void);  // v2_gamestate.cpp (all builds)
extern "C" int v2_state_load(const char*);

int main(int argc, char* argv[]) {
    printf("V2_ONLY: starting standalone v2 build (no m2c)\n");
    v2_find_content();   // before any chunk read: the store is settled at its first use

    const char* record_input = nullptr;
    const char* replay_input = nullptr;
    const char* keymap_path  = nullptr;
    bool strict_replay = false;
    int net_host_port = 0;            // UX stage 8 step 3: --host=PORT (with --coop=N)
    const char* net_join = nullptr;   //                    --join=HOST[:PORT]
    int net_delay = -1;               //                    --delay=N reads (default 2; alone = the solo lockstep)
    int net_lobby = -1;               //                    --lobby=K: the host starts once K clients are in (default all; the rest join the running game)
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0) {
            g_debug_mode = true;
            fprintf(stderr, "[v2_main] --debug enabled: F4/F5/F6 cheats active\n");
        } else if (strncmp(argv[i], "--record-input=", 15) == 0) {
            record_input = argv[i] + 15;
        } else if (strncmp(argv[i], "--replay-input=", 15) == 0) {
            replay_input = argv[i] + 15;
        } else if (strcmp(argv[i], "--replay-strict") == 0) {
            strict_replay = true;
        } else if (strncmp(argv[i], "--keymap=", 9) == 0) {
            keymap_path = argv[i] + 9;
        } else if (strncmp(argv[i], "--coop=", 7) == 0) {
            // UX stage 8: 2 or 3 players in one game (v2_coop.h); 1 = the original
            v2_coop_set_players(atoi(argv[i] + 7));
            fprintf(stderr, "[v2_main] --coop: %d players\n", atoi(argv[i] + 7));
        } else if (strncmp(argv[i], "--host=", 7) == 0) {
            net_host_port = atoi(argv[i] + 7);
        } else if (strncmp(argv[i], "--join=", 7) == 0) {
            net_join = argv[i] + 7;
        } else if (strncmp(argv[i], "--delay=", 8) == 0) {
            net_delay = atoi(argv[i] + 8);
        } else if (strncmp(argv[i], "--lobby=", 8) == 0) {
            net_lobby = atoi(argv[i] + 8);
        } else if (strncmp(argv[i], "--player=", 9) == 0) {
            // UX stage 8 step 2: the player this client presents (1..3; the
            // camera and the badge); step 3's lobby sets it from the host
            int pl = atoi(argv[i] + 9);
            g_v2_local_player = (pl < 1) ? 0 : (pl > V2_COOP_MAX) ? V2_COOP_MAX - 1 : pl - 1;
            fprintf(stderr, "[v2_main] --player: this client is player %d\n", g_v2_local_player + 1);
        }
        else if (strncmp(argv[i], "--max-frames=", 13) == 0) {
            // №59: parsed in ALL V2_ONLY builds (used to be HEADLESS-only —
            // a plain V2_ONLY replay silently ignored the limit and ran on).
            g_v2only_max_frames = atoi(argv[i] + 13);
#ifdef HEADLESS
            extern int g_headless_max_frames;
            g_headless_max_frames = g_v2only_max_frames;
#endif
        }
#ifdef HEADLESS
        else if (strncmp(argv[i], "--dump-dir=", 11) == 0) {
            extern const char* g_headless_dump_dir;
            g_headless_dump_dir = argv[i] + 11;
        }
#endif
    }

    SDL_SetMainReady();   // pairs with SDL_MAIN_HANDLED above

    // Load keymap before recorder init (recorder consults v2_keymap).
    v2_keymap_load(keymap_path);

    // SIGINT (Ctrl-C) → graceful shutdown.
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    // Init SDL window + sound (only v2 window — orig render not needed since m2c disabled)
    render_init_v2(nullptr);
    sound_init();
    // UX stage 11: the SC-55 option boots its module now, ahead of the game's
    // first music (the firmware needs 0.2 s on the SC-55mk2, 2 s on the SC-55
    // v1.21); until it is up the OPL render stays audible (the boot gate in
    // v2_sc55). A failure is reported by v2_ui_service's own attempt (toast, back to PC).
    v2_options_ensure_loaded();
    if (v2_options.sound_mode.load() == 2) v2_sc55_start();
    // the MT-32 world likewise: the chunks and Munt (the ROMs) before the driver
    // boots, so the module carries the music from its first note (v2_mt32_prepare)
    if (v2_options.sound_mode.load() == 3) v2_mt32_prepare();

    // Input record/replay (V2_ONLY only). File format is SDL-independent.
    v2_input_recorder_init(record_input, replay_input, strict_replay ? 1 : 0);

    // UX stage 8 step 3: the lockstep lobby — the host waits for its players,
    // a client takes its player number and the host's world options; either
    // way the recorder then captures this client's keys for a later read.
    {
        const int delay = (net_delay < 0) ? 2 : (net_delay < 1 ? 1 : (net_delay > 8 ? 8 : net_delay));
        if (net_host_port > 0) {
            if (!v2_net_host(net_host_port, v2_coop_players(), delay, net_lobby)) return 1;
        } else if (net_join) {
            if (!v2_net_join(net_join)) return 1;
        } else if (net_delay >= 0) {
            v2_net_solo(delay);
        }
        // a client that joined a game already running gets the host's image first (synced = 0)
        if (v2_net_active()) v2_input_recorder_net(g_v2_local_player, (net_join && v2_net_joined_late()) ? 0 : 1);
    }

    // Load baked static EXE data, then point v2 base at it.
    v2_load_static_data();
    v2_set_m2c_base(v2_m2c_buf);
    v2_vm_init_shadow_early(0);
    v2_run_animation_vm(0);
    // --debug shadow_ds[0x202] write happens inside v2_startup() (called from
    // v2_run_animation_vm above). g_debug_mode parsed from argv at top of main.

    v2_game_thread_start();

    // (direction V step 2) teleport: load a full state snapshot before the
    // first frame (game thread is parked until the first phase signal, so the
    // fill is single-threaded). With V2_SAVE_STATE also set, save back
    // immediately — the file pair is a byte-roundtrip channel for tests.
    { const char* lp = getenv("V2_LOAD_STATE");
      if (lp) {
          if (v2_state_load(lp)) { fprintf(stderr, "V2: teleport load FAILED\n"); return 1; }
          // Stage 4 II.c: the load rewrote the image wholesale — refresh
          // the evacuated members before the first frame runs.
          { extern uint8_t* v2_vm_get_shadow_ds();
            v2_gs_evac_refresh(v2_vm_get_shadow_ds()); }
          const char* sp = getenv("V2_SAVE_STATE");
          if (sp) v2_state_save(sp);
      } }

    // Main loop: signal v2 phases sequentially. Each phase blocks until done.
    //
    // Orig timing (measured on Linux ARM64 by adding tick counter to render.cpp):
    //   render thread tick ≈ 57 Hz (SDL_Delay(15) → ~17.5 ms on Linux scheduler)
    //   word_3287c DEC per render tick → ~17.5 ms per sub_10130 wait
    //   Per orig game frame: 3× sub_16775 + 3× sub_10130 = ~53 ms = ~18.9 FPS
    //   (NOT 22 FPS as the spec ~15 ms × 3 = ~45 ms would imply — Linux jitter
    //    rounds SDL_Delay up to scheduler tick boundary).
    uint16_t ds = 0;
    uint32_t frame_target_ms = SDL_GetTicks();
    const uint32_t FRAME_PERIOD_MS = V2_FRAME_BUDGET_MS;  // stage 6.3: v2_timing.h
    // FPS instrumentation: track work time per frame (excluding sleep). Print
    // running stats every N frames + warn when individual frame exceeds budget.
    uint32_t fps_window_start_ms = SDL_GetTicks();
    int      fps_frames_in_window = 0;
    uint32_t fps_work_total_us = 0;  // sum of work time (no sleep) per window
    int      fps_slow_frames = 0;    // frames where work > FRAME_PERIOD_MS
    while (!need_quit) {
        uint32_t frame_start_ms = SDL_GetTicks();
        uint32_t phase_ms[11] = {0};
        uint32_t t = SDL_GetTicks();
        v2_signal_phase(V2_PHASE_FRAME_BEGIN, ds);    phase_ms[0] = SDL_GetTicks() - t;
        t = SDL_GetTicks();
        v2_signal_phase(V2_PHASE_PRE_VM, ds);         phase_ms[1] = SDL_GetTicks() - t;
        t = SDL_GetTicks();
        v2_signal_phase(V2_PHASE_VM, ds);             phase_ms[2] = SDL_GetTicks() - t;
        t = SDL_GetTicks();
        v2_signal_phase(V2_PHASE_POST_VM, ds);        phase_ms[3] = SDL_GetTicks() - t;
        t = SDL_GetTicks();
        v2_signal_phase(V2_PHASE_RENDER1, ds);        phase_ms[4] = SDL_GetTicks() - t;
        t = SDL_GetTicks();
        v2_signal_phase(V2_PHASE_POST_FLIP1, ds);     phase_ms[5] = SDL_GetTicks() - t;
        t = SDL_GetTicks();
        v2_signal_phase(V2_PHASE_RENDER2, ds);        phase_ms[6] = SDL_GetTicks() - t;
        t = SDL_GetTicks();
        v2_signal_phase(V2_PHASE_POST_FLIP2, ds);     phase_ms[7] = SDL_GetTicks() - t;
        t = SDL_GetTicks();
        v2_signal_phase(V2_PHASE_RENDER3, ds);        phase_ms[8] = SDL_GetTicks() - t;
        // №59: orig 0xDB/0xE1 (word_30C14=0 + sub_108c8) run BEFORE sub_1086f
        v2_signal_phase(V2_PHASE_AUDIO_TICK, ds);
        t = SDL_GetTicks();
        v2_signal_phase(V2_PHASE_POST_FLIP3, ds);     phase_ms[9] = SDL_GetTicks() - t;
        t = SDL_GetTicks();
        v2_signal_phase(V2_PHASE_FRAME_END, ds);      phase_ms[10] = SDL_GetTicks() - t;
        // Log per-phase breakdown when frame is slow (>30ms)
        if (frame_start_ms != 0 && SDL_GetTicks() - frame_start_ms > 30) {
            fprintf(stderr, "FPS-PHASE: FB=%u PV=%u VM=%u PoV=%u R1=%u PF1=%u R2=%u PF2=%u R3=%u PF3=%u FE=%u total=%u\n",
                phase_ms[0], phase_ms[1], phase_ms[2], phase_ms[3], phase_ms[4],
                phase_ms[5], phase_ms[6], phase_ms[7], phase_ms[8], phase_ms[9], phase_ms[10],
                SDL_GetTicks() - frame_start_ms);
        }
        uint32_t work_end_ms = SDL_GetTicks();
        uint32_t work_ms = work_end_ms - frame_start_ms;
        fps_work_total_us += work_ms * 1000;
        fps_frames_in_window++;
        // Per-frame slow-frame warning (any frame whose work alone exceeds budget).
        if (work_ms > FRAME_PERIOD_MS) {
            fps_slow_frames++;
            // v2_dbg_pre_vm_iter: file-scope extern (top of file)
            fprintf(stderr, "FPS-SLOW: frame_iter=%d work=%ums > budget=%ums\n",
                    v2_dbg_pre_vm_iter, work_ms, FRAME_PERIOD_MS);
        }
        // Window stats every ~2s (fps target × 2s).
        if (work_end_ms - fps_window_start_ms >= 2000) {
            float avg_work_ms = (float)fps_work_total_us / (float)fps_frames_in_window / 1000.0f;
            float effective_fps = 1000.0f * fps_frames_in_window / (float)(work_end_ms - fps_window_start_ms);
            fprintf(stderr, "FPS-STATS: window=%ums frames=%d avg_work=%.1fms slow=%d "
                            "effective_fps=%.1f (target=%.1f)\n",
                    work_end_ms - fps_window_start_ms, fps_frames_in_window,
                    avg_work_ms, fps_slow_frames, effective_fps,
                    1000.0f / FRAME_PERIOD_MS);
            fps_window_start_ms = work_end_ms;
            fps_frames_in_window = 0;
            fps_work_total_us = 0;
            fps_slow_frames = 0;
        }

        // Frame rate limiter: sleep to target 60 FPS.
#ifdef HEADLESS
        // V2_ONLY+HEADLESS: the barrier-phase hook that normally enforces
        // --max-frames never runs standalone — enforce it from the main loop.
        { extern int headless_check_exit(void); headless_check_exit(); }
#endif
        // №59: plain V2_ONLY --max-frames enforcement (frame counter is the
        // FRAME_BEGIN barrier increment — same counter the traces use).
        if (g_v2only_max_frames > 0) {
            // v2_dbg_pre_vm_iter: file-scope extern (top of file)
            if (v2_dbg_pre_vm_iter >= g_v2only_max_frames) {
                fprintf(stderr, "V2_ONLY: --max-frames=%d reached, exiting\n",
                        g_v2only_max_frames);
                need_quit = true;
            }
        }
        // V2_NOVSYNC=1 (diagnostics): drop the real-time frame pacing so a
        // replay runs at CPU speed — the benchmarking channel for the
        // gencode-vs-interpreter comparison. Default behavior unchanged.
        static int novsync = -1;
        if (novsync < 0) { const char* e = getenv("V2_NOVSYNC"); novsync = (e && *e == '1') ? 1 : 0; }
        frame_target_ms += FRAME_PERIOD_MS;
        uint32_t now = SDL_GetTicks();
        if (!novsync && (int32_t)(frame_target_ms - now) > 0) {
            SDL_Delay(frame_target_ms - now);
        } else if ((int32_t)(frame_target_ms - now) <= 0) {
            // Fell behind — reset target to avoid catch-up burst.
            frame_target_ms = now;
        }
    }

    printf("V2_ONLY: quitting\n");
    v2_game_thread_stop();
    v2_net_shutdown();          // UX stage 8 step 3: tell the peers, close the sockets
    v2_midi_shutdown();   // UX stage 11: V2_MIDI_DUMP
    // Golden end-state channel at the max-frames/window-close exit — same
    // idempotent dump the quit sites call (V2_GOLDEN_DUMP / V2_SAVE_STATE).
    headless_golden_dump();
    return 0;
}
