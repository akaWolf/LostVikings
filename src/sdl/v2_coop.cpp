// v2_coop.cpp — the shared co-op state, the SDL-side input accumulators and
// the game controllers (see v2_coop.h). The game-side logic — the VM view,
// the per-player input words, cycling and death — lives in v2_vm.cpp next to
// the DOS mirrors it extends.
#include "v2_coop.h"
#include <SDL2/SDL.h>
#include <cstdio>

V2Coop g_coop;
int g_v2_coop_players = 1;
thread_local uint16_t g_v2_exec_obj = 0xFFFF;
uint16_t v2_coop_held[V2_COOP_MAX] = {0, 0, 0};
std::atomic<uint16_t> v2_coop_press[V2_COOP_MAX];

void v2_coop_set_players(int n) {
    if (n < 1) n = 1;
    if (n > V2_COOP_MAX) n = V2_COOP_MAX;
    g_v2_coop_players = n;
    for (auto& p : g_coop.p) p = V2CoopPlayer{};
    for (int k = 0; k < V2_COOP_MAX; k++) { v2_coop_held[k] = 0; v2_coop_press[k].store(0, std::memory_order_relaxed); }
}

int v2_coop_players() { return g_v2_coop_players; }

int v2_coop_owner(uint16_t obj) {
    if (obj >= 6) return -1;
    for (int k = 0; k < g_v2_coop_players; k++)
        if (g_coop.p[k].active == obj) return k;
    return -1;
}

void v2_coop_key(int player, uint16_t bits, bool down, bool repeat) {
    if (player <= 0 || player >= V2_COOP_MAX || !bits) return;
    if (down) {
        // the tap accumulator (#81): a press shorter than one sub_12352 read still lands as an edge
        if (!repeat) v2_coop_press[player].fetch_or(bits, std::memory_order_relaxed);
        v2_coop_held[player] |= bits;
    } else {
        v2_coop_held[player] &= (uint16_t)~bits;
    }
}

// ---------------------------------------------------------- game controllers
namespace {
struct Pad {
    SDL_GameController* gc = nullptr;
    SDL_JoystickID id = -1;
    uint16_t dir = 0;      // the move bits the d-pad + left stick currently hold
    int16_t ax = 0, ay = 0;
    bool dpad[4] = {false, false, false, false};   // up, down, left, right
};
Pad g_pads[V2_COOP_MAX];

uint16_t button_bits(Uint8 b) {
    switch (b) {
    case SDL_CONTROLLER_BUTTON_A:             return 0x8000;   // action / jump
    case SDL_CONTROLLER_BUTTON_B:             return 0x4000;   // second button (D)
    case SDL_CONTROLLER_BUTTON_X:             return 0x80;     // use / talk (S)
    case SDL_CONTROLLER_BUTTON_Y:             return 0x40;     // item (E)
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return 0x20;     // previous viking (CTRL)
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return 0x10;     // next viking
    case SDL_CONTROLLER_BUTTON_BACK:          return 0x2000;   // switch (TAB)
    case SDL_CONTROLLER_BUTTON_START:         return 0x1000;   // pause / menu (ESC)
    default: return 0;
    }
}
uint16_t dir_bits(const Pad& p) {
    const int dz = 16000; uint16_t d = 0;
    if (p.dpad[0] || p.ay < -dz) d |= 0x800;
    if (p.dpad[1] || p.ay >  dz) d |= 0x400;
    if (p.dpad[2] || p.ax < -dz) d |= 0x200;
    if (p.dpad[3] || p.ax >  dz) d |= 0x100;
    return d;
}
int pad_index(SDL_JoystickID id) {
    for (int i = 0; i < V2_COOP_MAX; i++) if (g_pads[i].gc && g_pads[i].id == id) return i;
    return -1;
}
} // namespace

void v2_coop_pad_added(int device_index) {
    if (!SDL_IsGameController(device_index)) return;
    for (int i = 0; i < V2_COOP_MAX; i++) {
        if (g_pads[i].gc) continue;
        SDL_GameController* gc = SDL_GameControllerOpen(device_index);
        if (!gc) { fprintf(stderr, "v2_coop: cannot open controller %d: %s\n", device_index, SDL_GetError()); return; }
        g_pads[i] = Pad{}; g_pads[i].gc = gc;
        g_pads[i].id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc));
        fprintf(stderr, "v2_coop: controller '%s' -> player %d\n", SDL_GameControllerName(gc), i + 1);
        return;
    }
    fprintf(stderr, "v2_coop: controller %d ignored (three players already have pads)\n", device_index);
}

void v2_coop_pad_removed(int instance_id) {
    int i = pad_index((SDL_JoystickID)instance_id);
    if (i < 0) return;
    SDL_GameControllerClose(g_pads[i].gc);
    fprintf(stderr, "v2_coop: player %d's controller removed\n", i + 1);
    g_pads[i] = Pad{};
}

int v2_coop_pad_events(const void* sdl_event, V2CoopPadEv* out, int max) {
    const SDL_Event& e = *(const SDL_Event*)sdl_event; int n = 0;
    auto emit = [&](int player, uint16_t bits, bool down) { if (bits && n < max) out[n++] = V2CoopPadEv{player, bits, down}; };
    if (e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_CONTROLLERBUTTONUP) {
        int i = pad_index(e.cbutton.which); if (i < 0) return 0;
        bool down = e.type == SDL_CONTROLLERBUTTONDOWN; Uint8 b = e.cbutton.button;
        int dp = b == SDL_CONTROLLER_BUTTON_DPAD_UP ? 0 : b == SDL_CONTROLLER_BUTTON_DPAD_DOWN ? 1 :
                 b == SDL_CONTROLLER_BUTTON_DPAD_LEFT ? 2 : b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT ? 3 : -1;
        if (dp < 0) { emit(i, button_bits(b), down); return n; }
        g_pads[i].dpad[dp] = down;
    } else if (e.type == SDL_CONTROLLERAXISMOTION) {
        int i = pad_index(e.caxis.which); if (i < 0) return 0;
        if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) g_pads[i].ax = e.caxis.value;
        else if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) g_pads[i].ay = e.caxis.value;
        else return 0;
    } else return 0;
    // the move bits: report every changed bit as a press / release
    int i = pad_index(e.type == SDL_CONTROLLERAXISMOTION ? e.caxis.which : e.cbutton.which);
    uint16_t nd = dir_bits(g_pads[i]), od = g_pads[i].dir; g_pads[i].dir = nd;
    static const uint16_t kDirs[4] = {0x800, 0x400, 0x200, 0x100};
    for (uint16_t bit : kDirs) {
        if ((nd & bit) && !(od & bit)) emit(i, bit, true);
        else if (!(nd & bit) && (od & bit)) emit(i, bit, false);
    }
    return n;
}
