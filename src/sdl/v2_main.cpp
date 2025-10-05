// V2_ONLY entry point — replaces orig m2c main entry.
// No m2c-decompiled code is linked. v2 phase functions handle game on shadow DS.

#include <SDL2/SDL.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <csignal>

extern bool need_quit;

static void sigint_handler(int) {
    need_quit = true;
}
extern void render_init_v2(void* state);
extern void sound_init();

// v2 phase dispatcher (defined in v2_vm.cpp)
enum V2Phase {
    V2_PHASE_FRAME_BEGIN = 0,
    V2_PHASE_PRE_VM = 1,
    V2_PHASE_VM = 2,
    V2_PHASE_POST_VM = 3,
    V2_PHASE_RENDER1 = 4,
    V2_PHASE_POST_FLIP1 = 5,
    V2_PHASE_RENDER2 = 6,
    V2_PHASE_POST_FLIP2 = 7,
    V2_PHASE_RENDER3 = 8,
    V2_PHASE_POST_FLIP3 = 9,
    V2_PHASE_FRAME_END = 10
};

extern void v2_signal_phase(V2Phase phase, uint16_t ds_val);
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

// Static EXE data: exe_static.bin is dumped from default-mode m2c::m[0..0x29F00]
// after the C++ Initializer in vikings.exe.cpp populates it. Without this data,
// seg001 text/menu region (+0x9480) is zero, sub_12529 reads width=0, sub_12388
// LOOPs cx=0xFFFE = 65k iterations → hang.
static constexpr size_t V2_EXE_STATIC_SIZE = 0x29F00;
static uint8_t v2_m2c_buf[0x100000] = {0};

static void v2_load_static_data() {
    FILE* f = fopen("exe_static.bin", "rb");
    if (!f) {
        fprintf(stderr,
            "V2_ONLY: exe_static.bin not found — text/menu rendering will hang.\n"
            "  Run default build once: `make -j$(nproc) && ./vikings` to generate it,\n"
            "  then rebuild with `V2_ONLY=1 make`.\n");
        return;
    }
    size_t n = fread(v2_m2c_buf, 1, V2_EXE_STATIC_SIZE, f);
    fclose(f);
    printf("V2_ONLY: loaded %zu bytes from exe_static.bin (seg001 sample @0x9480: "
           "%02X %02X %02X %02X)\n",
           n, v2_m2c_buf[0x9480], v2_m2c_buf[0x9481], v2_m2c_buf[0x9482], v2_m2c_buf[0x9483]);
}

// --debug CLI flag: enable orig debug-build cheats (F4 INT 3, F5/F6 level cheats).
// Mirrors orig DOS conditional gated by word_286E2 (ds:0x202) being non-zero.
bool g_debug_mode = false;

int main(int argc, char* argv[]) {
    printf("V2_ONLY: starting standalone v2 build (no m2c)\n");

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0) {
            g_debug_mode = true;
            fprintf(stderr, "[v2_main] --debug enabled: F4/F5/F6 cheats active\n");
        }
    }

    // SIGINT (Ctrl-C) → graceful shutdown.
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    // Init SDL window + sound (only v2 window — orig render not needed since m2c disabled)
    render_init_v2(nullptr);
    sound_init();

    // Load baked static EXE data, then point v2 base at it.
    v2_load_static_data();
    v2_set_m2c_base(v2_m2c_buf);
    v2_vm_init_shadow_early(0);
    v2_run_animation_vm(0);
    // --debug shadow_ds[0x202] write happens inside v2_startup() (called from
    // v2_run_animation_vm above). g_debug_mode parsed from argv at top of main.

    v2_game_thread_start();

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
    const uint32_t FRAME_PERIOD_MS = 53;  // ~18.9 FPS — measured orig on Linux
    while (!need_quit) {
        v2_signal_phase(V2_PHASE_FRAME_BEGIN, ds);
        v2_signal_phase(V2_PHASE_PRE_VM, ds);
        v2_signal_phase(V2_PHASE_VM, ds);
        v2_signal_phase(V2_PHASE_POST_VM, ds);
        v2_signal_phase(V2_PHASE_RENDER1, ds);
        v2_signal_phase(V2_PHASE_POST_FLIP1, ds);
        v2_signal_phase(V2_PHASE_RENDER2, ds);
        v2_signal_phase(V2_PHASE_POST_FLIP2, ds);
        v2_signal_phase(V2_PHASE_RENDER3, ds);
        v2_signal_phase(V2_PHASE_POST_FLIP3, ds);
        v2_signal_phase(V2_PHASE_FRAME_END, ds);

        // Frame rate limiter: sleep to target 60 FPS.
        frame_target_ms += FRAME_PERIOD_MS;
        uint32_t now = SDL_GetTicks();
        if ((int32_t)(frame_target_ms - now) > 0) {
            SDL_Delay(frame_target_ms - now);
        } else {
            // Fell behind — reset target to avoid catch-up burst.
            frame_target_ms = now;
        }
    }

    printf("V2_ONLY: quitting\n");
    v2_game_thread_stop();
    return 0;
}
