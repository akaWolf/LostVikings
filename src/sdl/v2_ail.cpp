// v2_ail.cpp — #61: native AIL music path (interpreted Miles .ADV driver).
//
// The v2 startup mirror (v2_vm.cpp, seg000:6458-6480 replica) already loads
// the whole DOS sound arena into v2_vm_shadow_sound:
//   offset 0                     — the .ADV driver blob   (chunk 0x1C7+[86B6])
//   [DS_SEG_SOUND3]=ds:0x2E6F    — the timbre bank        (chunk 0x20C+[86B8])
//   [DS_SEG_SOUND2]=ds:0x2E6D    — the SFX XMID catalog   (chunk 0x207+[86B8])
//   [DS_SEG_SOUND] =ds:0x2E6B    — music XMID load target (level tracks, 0x215)
// with real paragraph values relative to [DS_SEG_SOUND_BASE]=ds:0x992C.
//
// This layer boots the 8086 interpreter over the blob and replays the EXACT
// original call chains:
//   init  = sub_17561 tail : install([98E6]=0) → fn64([98E8/EA]=desc) →
//           fn65(drv,[86BA..86C0]) → fn66(drv, desc+0xC..0x12) →
//           fn99([9942]) → cache alloc([9934]/[9932]) → fn9A
//   start = sub_176bd tail : fn97(drv,0,bx,ax,[si-66E0],ds,0,0) → handle →
//           [si-66F4]; loop{ fn9B → [9946]; [993E]/[9940]; sub_17512 scan →
//           [992E]/[9930]; fn9C } until 0xFFFF; fnAA
//   stop  = sub_17912 slot body : fnAB(drv,h) → fn98(drv,h) →
//           [si-66F4]=FFFF, [si-66EA]=FFFF
//   fade  = sub_178f1 : fnB1(drv,h,0,0x3E8) (sub_1C7BD volume fade to 0 over 1s)
//   tick  = fn67(drv) at [desc+0x14]+5 Hz (= 125), pumped from the game
//           thread by v2_nopl_pump; OPL writes route into the dual-OPL2
//           nuked pair (v2_native_opl.cpp).
//
// DS side effects are written to the SHADOW DS through the same addresses the
// original wrote — including the driver's own sequence-state block, which
// lives INSIDE DS (music state at [ds:0x9920]=0x9950, SFX states 0x9B58+):
// the interpreter maps the shadow DS as a paragraph range, so the Miles code
// itself updates shadow_ds[0x9950..] exactly like the DOS driver updated the
// real DS.
//
// Activation: V2_NATIVE_AIL=1 and V2_ONLY build — in default (verify) mode
// the orig world plays SDL adlmidi and v2 mirrors muted slots; running the
// native driver there would desync shadow DS from real DS. The default-mode
// symmetric design (a second interpreter instance fed from real memory) is a
// later #61 stage.

#include "v2_midi.h"        // UX stage 11: the MIDI lane on the audible instance
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "v2_ds_layout.h"
#include "v2_gamestate.h"

// ---------------------------------------------------------------------------
// v2_ail_interp.cpp C API
// ---------------------------------------------------------------------------
extern "C" void     v2_ail_interp_load(const uint8_t*, uint32_t, const uint8_t*, uint32_t);
extern "C" void     v2_ail_interp_use(int idx);
extern "C" void*    v2_ail_interp_lock();
extern "C" void     v2_ail_interp_unlock();
extern "C" uint16_t v2_ail_interp_call(uint16_t, const uint16_t*, int);
extern "C" void     v2_ail_interp_set_io(void (*)(uint16_t, uint8_t), uint8_t (*)(uint16_t));
extern "C" void     v2_ail_interp_set_callback(uint16_t (*)());
extern "C" void     v2_ail_interp_map_segment(uint16_t, uint8_t*, uint32_t);
extern "C" void     v2_ail_interp_set_code_para(uint16_t);
extern "C" void     v2_ail_sinki_load(const uint8_t*, uint32_t, const uint8_t*, uint32_t);
extern "C" void     v2_ail_sinki_map(uint16_t, uint8_t*, uint32_t);
extern "C" void     v2_ail_sinki_map_front(uint16_t, uint8_t*, uint32_t);
extern "C" void     v2_ail_sinki_set_io(void (*)(uint16_t, uint8_t), uint8_t (*)(uint16_t));
extern "C" void     v2_ail_sinki_set_callback(uint16_t (*)());
extern "C" void     v2_ail_sinki_set_midi_tap(void (*)(uint16_t, uint16_t, uint16_t));   // UX stage 11
extern "C" void     v2_ail_interp_set_midi_tap(void (*)(uint16_t, uint16_t, uint16_t));
extern "C" uint16_t v2_ail_sinki_call(uint16_t, const uint16_t*, int);
extern "C" uint16_t v2_ail_sinki_fn_lookup(uint16_t);
extern "C" void     v2_ail_sink_publish(uint8_t*, uint16_t, uint16_t, uint16_t);
extern "C" uint16_t v2_ail_fn_lookup(uint16_t);
extern "C" int      v2_ail_interp_current(void);
#ifdef HEADLESS
void headless_dump_divergence(const char*, int, const char*);  // C++ linkage
#endif
extern "C" void     v2_nopl_sink_out(uint16_t, uint8_t);
extern "C" uint8_t  v2_nopl_sink_in(uint16_t);

// stage 6.1 w3 increment 7: the NATIVE sequencer surface (v2_ail_native.cpp).
// V2_AIL_NATIVE=1 in a V2_ONLY build routes every driver call through the
// line-by-line C port instead of the interpreter (same blob image, same io
// hooks, far memory through the same segment map) — the OPL stream must
// stay byte-identical, judged by the opl_ref trace corpus.
extern "C" void     v2_ailnat_load(const uint8_t*, uint32_t);
extern "C" void     v2_ailnat_set_io(void (*)(uint16_t, uint8_t), uint8_t (*)(uint16_t));
extern "C" void     v2_ailnat_map_segment(uint16_t, uint8_t*, uint32_t);
extern "C" void     v2_ailnat_set_self_seg(uint16_t);
extern "C" uint16_t v2_ailnat_last_dx(void);
extern "C" uint16_t v2_ailnat_fn64_desc_27C8(void);
extern "C" uint16_t v2_ailnat_fn65_probe_140E(uint16_t);
extern "C" void     v2_ailnat_fn66_install_35D8(uint16_t);
extern "C" void     v2_ailnat_timer_tick_331E(void);
extern "C" uint16_t v2_ailnat_fn97_register_37F6(uint16_t, uint16_t, uint16_t,
                                                 uint16_t, uint16_t, uint16_t, uint16_t);
extern "C" void     v2_ailnat_release_handle_393C(uint16_t);
extern "C" uint16_t v2_ailnat_fn99_cache_size_16D9(void);
extern "C" void     v2_ailnat_fn9A_set_cache_16F0(uint16_t, uint16_t, uint16_t);
extern "C" uint16_t v2_ailnat_fn9B_timbre_request_1736(uint16_t);
extern "C" void     v2_ailnat_fn9C_load_timbre_18DB(uint16_t, uint16_t, uint16_t, uint16_t);
extern "C" void     v2_ailnat_fnAA_start_3980(uint16_t);
extern "C" void     v2_ailnat_fnAB_stop_3A15(uint16_t);
extern "C" void     v2_ailnat_fnAD_resume_3A5B(uint16_t);
extern "C" uint16_t v2_ailnat_fnAE_status_3A9E(uint16_t);
extern "C" uint16_t v2_ailnat_fnAF_get_volume_3C57(uint16_t);
extern "C" uint16_t v2_ailnat_fnB0_get_tempo_3C31(uint16_t);
extern "C" void     v2_ailnat_fnB1_set_volume_3CF1(uint16_t, uint16_t, uint16_t);
extern "C" void     v2_ailnat_fnB2_set_tempo_3C7D(uint16_t, uint16_t, uint16_t);
extern "C" void     v2_ailnat_fn68_uninstall_3709(uint16_t, uint16_t);
extern "C" uint16_t v2_ailnat_fn96_state_size_3797(void);
extern "C" void     v2_ailnat_fn9D_lock_161C(uint16_t, uint16_t);
extern "C" void     v2_ailnat_fn9E_unlock_1662(uint16_t, uint16_t);
extern "C" uint16_t v2_ailnat_fn9F_cache_off_16A8(uint16_t, uint16_t);
extern "C" uint16_t v2_ailnat_fnB3_get_beat_3AC4(uint16_t);
extern "C" uint16_t v2_ailnat_fnB4_get_measure_3AEA(uint16_t);
extern "C" void     v2_ailnat_fnB5_branch_3B69(uint16_t, uint16_t);
extern "C" uint16_t v2_ailnat_fnB6_get_ctl_3D6E(uint16_t, uint16_t, uint16_t);
extern "C" void     v2_ailnat_fnB7_set_ctl_3DB1(uint16_t, uint16_t, uint16_t, uint16_t);
extern "C" uint16_t v2_ailnat_fnB9_note_count_3DE6(uint16_t, uint16_t);
extern "C" void     v2_ailnat_midi_2629(uint16_t, uint16_t, uint16_t);
extern "C" void     v2_ailnat_fnBB_stub_280A(void);
extern "C" void     v2_ailnat_fnBC_stub_280F(void);
extern "C" void     v2_ailnat_fnBD_set_cb_37AE(uint16_t, uint16_t, uint16_t);
extern "C" void     v2_ailnat_clear_cb_37D4(void);
extern "C" uint16_t v2_ailnat_lock_channel_3E20(void);
extern "C" void     v2_ailnat_release_channel_3EA3(uint16_t);
extern "C" void     v2_ailnat_fnC0_set_map_3B10(uint16_t, uint16_t, uint16_t);
extern "C" uint16_t v2_ailnat_fnC2_get_map_3B3C(uint16_t, uint16_t);
extern "C" uint16_t v2_ail_interp_drv_para(void);

// V2_AIL_NATIVE=1 gate (V2_ONLY only — the verify build keeps the
// interpreter pair as the oracle).
static int v2_ailnat_mode(void) {
#ifdef V2_ONLY
    // Native is the DEFAULT sound engine since the full-corpus OPL A/B
    // (58/58 byte-identical streams). V2_AIL_NATIVE=0 is the emergency
    // fallback to the interpreted driver.
    static int m = -1;
    if (m < 0) { const char* e = getenv("V2_AIL_NATIVE"); m = (e && atoi(e) == 0) ? 0 : 1; }
    return m;
#else
    return 0;
#endif
}

// Dispatch an AIL fn code to the native surface. args[0] is the driver id
// (single-blob model, ignored); the tail mirrors each fn's stack shape.
static uint16_t v2_ailnat_dispatch(uint16_t fn, const uint16_t* a, int argc) {
    (void)argc;
    switch (fn) {
    case 0x64: return v2_ailnat_fn64_desc_27C8();
    case 0x65: return v2_ailnat_fn65_probe_140E(a[1]);
    case 0x66: v2_ailnat_fn66_install_35D8(a[1]); return 0;
    case 0x67: v2_ailnat_timer_tick_331E(); return 0;
    case 0x97: return v2_ailnat_fn97_register_37F6(a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
    case 0x98: v2_ailnat_release_handle_393C(a[1]); return 0;
    case 0x99: return v2_ailnat_fn99_cache_size_16D9();
    case 0x9A: v2_ailnat_fn9A_set_cache_16F0(a[1], a[2], a[3]); return 0;
    case 0x9B: return v2_ailnat_fn9B_timbre_request_1736(a[1]);
    case 0x9C: v2_ailnat_fn9C_load_timbre_18DB(a[1], a[2], a[3], a[4]); return 0;
    case 0xAA: v2_ailnat_fnAA_start_3980(a[1]); return 0;
    case 0xAB: v2_ailnat_fnAB_stop_3A15(a[1]); return 0;
    case 0xAD: v2_ailnat_fnAD_resume_3A5B(a[1]); return 0;
    case 0xAE: return v2_ailnat_fnAE_status_3A9E(a[1]);
    case 0xAF: return v2_ailnat_fnAF_get_volume_3C57(a[1]);
    case 0xB0: return v2_ailnat_fnB0_get_tempo_3C31(a[1]);
    case 0xB1: v2_ailnat_fnB1_set_volume_3CF1(a[1], a[2], a[3]); return 0;
    case 0xB2: v2_ailnat_fnB2_set_tempo_3C7D(a[1], a[2], a[3]); return 0;
    case 0x68: v2_ailnat_fn68_uninstall_3709(a[1], a[2]); return 0;
    case 0x96: return v2_ailnat_fn96_state_size_3797();
    case 0x9D: v2_ailnat_fn9D_lock_161C(a[1], a[2]); return 0;
    case 0x9E: v2_ailnat_fn9E_unlock_1662(a[1], a[2]); return 0;
    case 0x9F: return v2_ailnat_fn9F_cache_off_16A8(a[1], a[2]);
    case 0xB3: return v2_ailnat_fnB3_get_beat_3AC4(a[1]);
    case 0xB4: return v2_ailnat_fnB4_get_measure_3AEA(a[1]);
    case 0xB5: v2_ailnat_fnB5_branch_3B69(a[1], a[2]); return 0;
    case 0xB6: return v2_ailnat_fnB6_get_ctl_3D6E(a[1], a[2], a[3]);
    case 0xB7: v2_ailnat_fnB7_set_ctl_3DB1(a[1], a[2], a[3], a[4]); return 0;
    case 0xB9: return v2_ailnat_fnB9_note_count_3DE6(a[1], a[2]);
    case 0xBA: v2_ailnat_midi_2629(a[1], a[2], a[3]); return 0;
    case 0xBB: v2_ailnat_fnBB_stub_280A(); return 0;
    case 0xBC: v2_ailnat_fnBC_stub_280F(); return 0;
    case 0xBD: v2_ailnat_fnBD_set_cb_37AE(a[1], a[2], v2_ail_interp_drv_para()); return 0;
    case 0xBE: v2_ailnat_clear_cb_37D4(); return 0;
    case 0xBF: return v2_ailnat_lock_channel_3E20();
    case 0xC0: v2_ailnat_fnC0_set_map_3B10(a[1], a[2], a[3]); return 0;
    case 0xC1: v2_ailnat_release_channel_3EA3(a[1]); return 0;
    case 0xC2: return v2_ailnat_fnC2_get_map_3B3C(a[1], a[2]);
    default:
        fprintf(stderr, "V2-AILNAT: fn %02X not natively ported (task #102)\n", fn);
        return 0;
    }
}
extern "C" double   v2_nopl_get_tick_hz(void);
extern "C" uint32_t v2_nopl_get_rate(void);
extern "C" uint16_t v2_ail_call_fn_code(uint16_t, const uint16_t*, int);
extern "C" uint16_t v2_ail_interp_last_dx();
extern "C" uint16_t v2_ail_interp_peek(uint16_t);

extern "C" uint16_t v2_ail_get_real_ds();     // v2_vm.cpp bridge getter
extern "C" uint8_t* v2_m2c_arena(void);       // m2c DOS arena (seg002), valid before v2_set_m2c_base
#ifdef V2_ONLY
// Standalone build links no m2c seg002 (no DOS arena at all) — the whole
// real-world layer below is unreachable (v2_ail_orig_enabled()==0); satisfy
// the link with a null arena.
extern "C" uint8_t* v2_m2c_arena(void) { return nullptr; }
#endif

// v2_native_opl.cpp SBPro port model (dual-OPL2 + mixer)
extern "C" void     v2_nopl_sbpro_out(uint16_t port, uint8_t val);
extern "C" uint8_t  v2_nopl_sbpro_in(uint16_t port);
extern "C" void     v2_nopl_set_tick_hz(double hz);

// DS layout constants used here (mirrors v2_ds_layout.h values; kept literal
// in comments beside each use — this file replicates orig asm addresses).
static constexpr uint16_t DS_86BA_IO   = 0x86BA;  // sound card IO base ([86BA..86C0] = IO/IRQ/DMA/?)
static constexpr uint16_t DS_98E6_DRV  = 0x98E6;  // AIL driver id (install result; 0 in our model)
static constexpr uint16_t DS_98E8_DESC = 0x98E8;  // driver descriptor far ptr (off, seg at +2)
static constexpr uint16_t DS_9932_OFF  = 0x9932;  // timbre cache buffer offset (0)
static constexpr uint16_t DS_9934_SEG  = 0x9934;  // timbre cache buffer segment
static constexpr uint16_t DS_993E_BANK = 0x993E;  // requested timbre: bank
static constexpr uint16_t DS_9940_PATCH= 0x9940;  // requested timbre: patch
static constexpr uint16_t DS_9942_SIZE = 0x9942;  // fn99 cache size result (0xE00)
static constexpr uint16_t DS_9946_REQ  = 0x9946;  // fn9B raw result
static constexpr uint16_t DS_992E_TOFF = 0x992E;  // sub_17512 result: timbre offset
static constexpr uint16_t DS_9930_TSEG = 0x9930;  // sub_17512 result: bank segment
static constexpr uint16_t DS_2E6F_BANK = 0x2E6F;  // timbre bank segment
static constexpr uint16_t DS_992C_SND  = 0x992C;  // sound window base segment
static constexpr uint16_t DS_A378_BUSY = 0xA378;  // async-install busy flag
static constexpr uint16_t DS_A39A_INIT = 0xA39A;  // sound-init done flag

// The timbre cache fn9A hands to the driver. Original: sub_10d96(0, 0xE00) →
// a DOS allocation recorded in [9934]:[9932]. Our cache lives in a private
// arena mapped at a fake paragraph above 640K (no collision with real
// game segments, see v2_ail_interp service paragraphs).
static constexpr uint16_t CACHE_PARA = 0xEC00;
static uint8_t g_cache[0x10000 + 16];
// UX stage 8 tails: the cache is part of the state image (the "AILC" block) —
// without it a loaded state (or a joining lockstep client) runs the sound
// driver on another timbre cache than the world it copies and the driver's DS
// words (slots, timbre offsets) drift apart.
extern "C" uint8_t* v2_ail_cache_data(uint32_t* size) { if (size) *size = (uint32_t)sizeof(g_cache); return g_cache; }

static bool     g_booted = false;
static double   g_tick_hz = 0.0;

// ---------------------------------------------------------------------------
// Silent SBPro port model — for the world that must NOT reach the audible
// chip (the shadow instance in default mode). Same latch+timer-status model
// the smoke rig validated; writes go nowhere but the detect probe still
// reads its own timer bits back.
// ---------------------------------------------------------------------------
struct SilentOpl {
    uint8_t idx[2] = {0, 0};
    uint8_t regs[2][256] = {{0}};
    uint8_t mixer_idx = 0;
    uint8_t mixer[256] = {0};
};
// (#85) per-instance silent state: both worlds run on silent IO now (the
// audible path is the sink) — a SHARED latch would let one world's index
// write leak into the other's data write. Indexed by the interp cursor.
static SilentOpl g_silent[2];
// (#85) verify channels, reset+compared at the FRAME_BEGIN barrier:
//  - OPL stream hash/count per world: FNV over every (port,val) the driver
//    would have sent to the chip. Catches timing-sensitive register traffic
//    whose DS footprint cancels out.
//  - AIL call parity per handler offset (same ADV blob in both worlds →
//    offsets are directly comparable): real increments in the bridge,
//    shadow in sh_call, fn67 ticks on both legs of v2_ail_tick.
static uint32_t g_oplhash[2] = {0x811C9DC5u, 0x811C9DC5u};
static uint32_t g_oplcnt[2] = {0, 0};
#define AILPAR_SLOTS 0x4000
static uint32_t g_ailpar[2][AILPAR_SLOTS];
static inline void v2_ailpar_note(int world, uint16_t off) {
    if (off < AILPAR_SLOTS) g_ailpar[world][off]++;
}
static void silent_out(uint16_t port, uint8_t val) {
    int w = v2_ail_interp_current() ? 1 : 0;
    g_oplhash[w] = (g_oplhash[w] ^ port) * 16777619u;
    g_oplhash[w] = (g_oplhash[w] ^ val) * 16777619u;
    g_oplcnt[w]++;
    SilentOpl& s = g_silent[w];
    if (port >= 0x220 && port <= 0x223) {
        int b = (port - 0x220) >> 1;
        if ((port & 1) == 0) s.idx[b] = val;
        else s.regs[b][s.idx[b]] = val;
        return;
    }
    if (port == 0x224) { s.mixer_idx = val; return; }
    if (port == 0x225) { s.mixer[s.mixer_idx] = val; return; }
}
static uint8_t silent_in(uint16_t port) {
    SilentOpl& s = g_silent[v2_ail_interp_current() ? 1 : 0];
    if (port >= 0x220 && port <= 0x223) {
        uint8_t ctl = s.regs[0][4];
        if (ctl & 0x80) return 0x00;
        if (ctl & 0x01) return 0xC0 | 0x40;
        return 0x00;
    }
    if (port == 0x225) return s.mixer[s.mixer_idx];
    return 0xFF;
}

// All shadow-chain fn calls go through here: CLI-model lock + instance 0.
static uint16_t sh_call(uint16_t fn_code, const uint16_t* args, int argc) {
    v2_ail_interp_lock();
    v2_ail_interp_use(0);
    v2_ailpar_note(0, v2_ail_fn_lookup(fn_code));   // (#85) call parity, shadow leg
    uint16_t ax = v2_ailnat_mode()
        ? v2_ailnat_dispatch(fn_code, args, argc)   // increment 7: native surface
        : v2_ail_call_fn_code(fn_code, args, argc);
    v2_ail_interp_unlock();
    return ax;
}

static uint16_t rdw(const uint8_t* s, uint16_t off) {
    return (uint16_t)(s[off] | (s[(uint16_t)(off + 1)] << 8));
}
static void wrw(uint8_t* s, uint16_t off, uint16_t v) {
    s[off] = (uint8_t)v; s[(uint16_t)(off + 1)] = (uint8_t)(v >> 8);
    v2_gs_evac_mirror_w(s, off, v);  // stage-4 bridge: host-side AIL DS writes
}

// ---------------------------------------------------------------------------
// gate
// ---------------------------------------------------------------------------
extern "C" { int v2_fntest_running = 0; }   // set by v2_fn_test.cpp (absent in V2_ONLY builds)

extern "C" int v2_ail_native_on() {
    // Native AIL is THE sound path (user-approved by ear on both flavours,
    // 2026-08-06). In V2_ONLY the shadow instance is the sole (audible)
    // world. In default/verify the shadow instance runs SILENT while the
    // REAL world drives the same interpreted driver through the sub_1bec2
    // bridge (audible) — same call+tick order in both worlds gives
    // byte-equal driver state, checked by the DS verify.
    // V2_NATIVE_AIL=0 is the emergency fallback while the legacy SDL channel
    // is being dismantled. The unit world stays OFF: synthetic DS holds
    // garbage segment words and must never boot the interpreter.
    // #79 B5: the emergency fallback env gate is gone with the legacy channel —
    // native is unconditional outside the unit world.
    return !v2_fntest_running;
}

// seg002 ret_d4f_a53 callback model: returns cs:word_1BBF2 — the PIT divisor
// snapshot seg002 keeps for the AIL timer (the LIVE m2c install wrapper
// sub_1c0b7 writes it when it programs the PIT with the driver's rate).
// (#82b) default mode returns the REAL cell byte-for-byte: with the real
// instance running the arena blob (code_para fix) the driver actually SEES
// the callback ptr sub_1c537 installed at [2957] and calls here for its
// tempo bookkeeping — the old 0x7FFF constant made music run ~3.4x slow
// (32767 vs the real 9546 divisor). V2_ONLY has no m2c world and no live
// installer: the copy's [2957] cell stays null, the blob never takes this
// path, and the power-on 0x7FFF stands in.
#ifndef V2_ONLY
extern uint16_t& word_1bbf2;   // m2c cs:word_1BBF2 (0d4f:0122)
static uint16_t v2_ail_pit_callback() { return word_1bbf2; }
#else
static uint16_t v2_ail_pit_callback() { return 0x7FFF; }
#endif

// ---------------------------------------------------------------------------
// boot — exact sub_17561 tail over the interpreter
// ---------------------------------------------------------------------------
extern "C" int v2_ail_boot(uint8_t* s, uint8_t* snd, uint32_t snd_size,
                           uint16_t ds_val, uint32_t blob_size, uint32_t bank_size) {
    if (g_booted) return 1;
    if (!blob_size) {
        fprintf(stderr, "V2-AIL: boot without a blob (chunk not loaded?)\n");
        return 0;
    }
    if (!ds_val) {
        // A zero DS paragraph would make every registered state segment look
        // like a FREE slot to the driver (fn67: cmp [slot.seg],0 → skip).
        fprintf(stderr, "V2-AIL: boot refused — ds_val=0\n");
        return 0;
    }
    uint16_t snd_base = rdw(s, DS_992C_SND);
    uint16_t bank_seg = rdw(s, DS_2E6F_BANK);
    uint32_t bank_off = (uint32_t)(uint16_t)(bank_seg - snd_base) << 4;
    if (bank_off >= snd_size) { fprintf(stderr, "V2-AIL: bank segment %04X outside the sound window\n", bank_seg); return 0; }

    // The interpreter works on its own copies of blob+bank (code arena); far
    // pointers arriving from the game resolve into the mapped REAL windows.
    // Instance 0 = shadow world. Audible only in V2_ONLY; in default mode the
    // REAL world (instance 1, fed through the sub_1bec2 bridge) owns the
    // audible chip and the shadow stays on the silent model.
    v2_ail_interp_use(0);
    v2_ail_interp_load(snd, blob_size, snd + bank_off, bank_size);
#ifdef V2_ONLY
    v2_ail_interp_set_io(v2_nopl_sbpro_out, v2_nopl_sbpro_in);
#else
    v2_ail_interp_set_io(silent_out, silent_in);
#endif
    v2_ail_interp_set_callback(v2_ail_pit_callback);
#ifdef V2_ONLY
    // UX stage 11: the interpreted fallback (V2_AIL_NATIVE=0) is the audible
    // driver of this build — the MIDI lane observes its 2629 (the native
    // engine's own 2629 carries the tap otherwise; never both).
    if (!v2_ailnat_mode()) v2_ail_interp_set_midi_tap(v2_midi_event);
#endif
    v2_ail_interp_map_segment(snd_base, snd, snd_size);       // whole sound arena
    v2_ail_interp_map_segment(ds_val, s, 0x10000);            // the game DS (state blocks!)
    v2_ail_interp_map_segment(CACHE_PARA, g_cache, sizeof(g_cache));
    if (v2_ailnat_mode()) {
        // Native surface boots on the same world: same blob image, the same
        // audible io hooks, the identical segment map, and the interp's fake
        // blob paragraph as the self-segment so DS cells stay byte-equal.
        v2_ailnat_load(snd, blob_size);
        v2_ailnat_set_io(v2_nopl_sbpro_out, v2_nopl_sbpro_in);
        v2_ailnat_map_segment(snd_base, snd, snd_size);
        v2_ailnat_map_segment(ds_val, s, 0x10000);
        v2_ailnat_map_segment(CACHE_PARA, g_cache, sizeof(g_cache));
        v2_ailnat_set_self_seg(v2_ail_interp_drv_para());
        fprintf(stderr, "V2-AILNAT: native sequencer surface armed\n");
    }

    // --- sub_17561 tail, DS effects included --------------------------------
    wrw(s, DS_A39A_INIT, 0);                     // eip 0x7561
    wrw(s, DS_A378_BUSY, 0);                     // eip 0x7567
    // sub_1c537 install: returns the driver id; single-blob model → 0.
    wrw(s, DS_98E6_DRV, 0);                      // eip 0x7591
    uint16_t drv = 0;

    // fn64 init(drv, cb_off, cb_seg) → dx:ax = descriptor far ptr.
    // The callback far pointer the stub pushes is seg002:0x0A53; inside the
    // interpreter the call lands in the callback hook, so the stored value
    // only round-trips through cs:0x2957. Pass a recognizable marker pair.
    {
        uint16_t a[3] = { drv, 0x0A53, 0x0D4F };
        uint16_t ax = sh_call(0x64, a, 3);
        uint16_t dx = v2_ailnat_mode() ? v2_ailnat_last_dx()
                                       : v2_ail_interp_last_dx();
        wrw(s, (uint16_t)(DS_98E8_DESC + 2), dx);   // eip 0x75A5 [98EA]=seg
        wrw(s, DS_98E8_DESC, ax);                    // eip 0x75A9 [98E8]=off
    }
    // fn65 detect(drv, [86BA], [86BC], [86BE], [86C0]) — orig pushes the 4
    // config words from the DATA.DAT tail config block (IO base, IRQ, DMA, ?).
    {
        uint16_t a[5] = { drv, rdw(s, DS_86BA_IO), rdw(s, (uint16_t)(DS_86BA_IO + 2)),
                          rdw(s, (uint16_t)(DS_86BA_IO + 4)), rdw(s, (uint16_t)(DS_86BA_IO + 6)) };
        uint16_t ax = sh_call(0x65, a, 5);
        if (ax != 1)
            fprintf(stderr, "V2-AIL: fn65 detect returned %04X (chip model rejected?)\n", ax);
    }
    // fn66 timer-install(drv, desc[0xC], desc[0xE], desc[0x10], desc[0x12]).
    // LES bx,[98E8] in the orig — we read the descriptor through the
    // interpreter arena (desc off is [98E8], inside the blob).
    {
        uint16_t doff = rdw(s, DS_98E8_DESC);
        uint16_t a[5] = { drv,
                          v2_ail_interp_peek((uint16_t)(doff + 0x0C)),
                          v2_ail_interp_peek((uint16_t)(doff + 0x0E)),
                          v2_ail_interp_peek((uint16_t)(doff + 0x10)),
                          v2_ail_interp_peek((uint16_t)(doff + 0x12)) };
        sh_call(0x66, a, 5);
        // Tick rate: fn66 stub reads [desc+0x14] and adds 5 (sub_1c61b eip
        // 0xB71-0xB7A) before registering the seg002 timer slot. SBPFM: 120+5.
        uint16_t hz = v2_ail_interp_peek((uint16_t)(doff + 0x14));
        if (hz != 0xFFFF && hz) {
            g_tick_hz = (double)(hz + 5);
            v2_nopl_set_tick_hz(g_tick_hz);
        }
    }
    // fn99 cache-size → [9942]; [A39A]=1 (eip 0x75FF-0x7602)
    uint16_t cache_size;
    {
        uint16_t a[1] = { drv };
        cache_size = sh_call(0x99, a, 1);
        wrw(s, DS_9942_SIZE, cache_size);
        wrw(s, DS_A39A_INIT, 1);
    }
    // cache alloc (sub_10d96) → [9934]:[9932]; fn9A(drv, off, seg, size)
    if (cache_size != 0) {
        wrw(s, DS_9934_SEG, CACHE_PARA);            // eip 0x7615
        wrw(s, DS_9932_OFF, 0);                     // eip 0x7618
        uint16_t a[4] = { drv, 0, CACHE_PARA, cache_size };
        sh_call(0x9A, a, 4);
    }
    // (orig eip 0x7636: [86B6]==8 GM special case — not our device path; its
    // chunk-0x215 load happens through the normal music-load mirror anyway.)
    g_booted = true;
    // stage 6.1 w3: native-port self-test (unit sweeps vs the interpreter).
    if (getenv("V2_AILNAT_SELFTEST")) {
        extern int v2_ailnat_selftest(void);
        v2_ail_interp_lock();
        v2_ail_interp_use(0);
        int ok = v2_ailnat_selftest();
        v2_ail_interp_unlock();
        fprintf(stderr, "V2-AILNAT: selftest %s\n", ok ? "PASS" : "FAIL");
    }
    fprintf(stderr, "V2-AIL: boot OK — drv=0 desc=%04X:%04X tick=%.0f Hz cache=%04X\n",
            rdw(s, (uint16_t)(DS_98E8_DESC + 2)), rdw(s, DS_98E8_DESC), g_tick_hz, cache_size);
    return 1;
}

// ---------------------------------------------------------------------------
// sub_17512 replica: scan the timbre bank for {patch=[9940], bank=[993E]}.
// Records [992E]=entry off word, [9930]=bank segment. Returns 0 on miss
// (orig aborts through sub_10dba 0x2CE7 — a data-corruption fatal; log loud).
// ---------------------------------------------------------------------------
static int v2_ail_scan_bank_17512(uint8_t* s, const uint8_t* snd, uint32_t snd_size) {
    uint16_t bank_seg = rdw(s, DS_2E6F_BANK);
    uint16_t snd_base = rdw(s, DS_992C_SND);
    uint32_t base = (uint32_t)(uint16_t)(bank_seg - snd_base) << 4;
    uint8_t  al = v2gs(s).ail_req_bank_lob();   // bank   (orig: mov al,[993E])
    uint8_t  ah = v2gs(s).ail_req_patch_lob();  // patch  (orig: mov ah,[9940])
    for (int32_t di = 0; di <= 0x3F82; di += 6) {   // JG loop bound (signed)
        if (base + di + 4 > snd_size) break;
        if (snd[base + di + 1] == al && snd[base + di] == ah) {
            uint16_t off = (uint16_t)(snd[base + di + 2] | (snd[base + di + 3] << 8));
            wrw(s, DS_9930_TSEG, bank_seg);
            wrw(s, DS_992E_TOFF, off);
            return 1;
        }
    }
    fprintf(stderr, "V2-AIL: timbre bank=%02X patch=%02X NOT FOUND (orig fatal 0x2CE7)\n", al, ah);
    return 0;
}

// ---------------------------------------------------------------------------
// music/sequence start — exact sub_176bd tail (eip 0x76BD-0x7748).
// si selects the slot family: 0 = music ([990C] handle, [9920]=0x9950 state),
// 2/4/6/8 = SFX slots. Returns the driver handle (0xFFFF on failure).
// ---------------------------------------------------------------------------
extern "C" uint16_t v2_ail_seq_start(uint8_t* s, uint8_t* snd, uint32_t snd_size,
                                     uint16_t ds_val, uint16_t bx_seg,
                                     uint16_t ax_seq, uint16_t si) {
    if (!g_booted) return 0xFFFF;
    uint16_t drv = rdw(s, DS_98E6_DRV);
    // fn97 register(drv, 0, xmid_seg, seq, state_off=[si-66E0], state_seg=ds, 0, 0)
    uint16_t state_off = rdw(s, (uint16_t)(si - 0x66E0));
    uint16_t handle;
    {
        uint16_t a[8] = { drv, 0, bx_seg, ax_seq, state_off, ds_val, 0, 0 };
        handle = sh_call(0x97, a, 8);
        wrw(s, (uint16_t)(si - 0x66F4), handle);    // eip 0x76DA
    }
    fprintf(stderr, "V2-AIL-START: bx=%04X seq=%u si=%u state=%04X:%04X -> handle=%04X "
            "slot0={%04X:%04X} cnt=%04X\n",
            bx_seg, ax_seq, si, ds_val, state_off, handle,
            v2_ail_interp_peek(0x2931), v2_ail_interp_peek(0x292F),
            v2_ail_interp_peek(0x294F));
    // timbre loop (loc_176DE)
    int n_timbres = 0;
    for (;;) {
        uint16_t a[2] = { drv, handle };
        uint16_t req = sh_call(0x9B, a, 2);
        wrw(s, DS_9946_REQ, req);                   // eip 0x76F0
        if (req == 0xFFFF) break;
        if (rdw(s, DS_A378_BUSY) != 0) continue;    // eip 0x76F8 (async wait)
        wrw(s, DS_993E_BANK,  (uint16_t)(req >> 8));   // eip 0x7700-0x7703
        wrw(s, DS_9940_PATCH, (uint16_t)(req & 0xFF)); // eip 0x7708-0x770E
        if (!v2_ail_scan_bank_17512(s, snd, snd_size)) return 0xFFFF;
        uint16_t c[5] = { drv, rdw(s, DS_993E_BANK), rdw(s, DS_9940_PATCH),
                          rdw(s, DS_992E_TOFF), rdw(s, DS_9930_TSEG) };
        sh_call(0x9C, c, 5);
        n_timbres++;
    }
    fprintf(stderr, "V2-AIL-TIMBRES: %d installed for seq=%u si=%u\n", n_timbres, ax_seq, si);
    // fnAA start(drv, handle) (loc_17735)
    {
        uint16_t a[2] = { drv, rdw(s, (uint16_t)(si - 0x66F4)) };
        sh_call(0xAA, a, 2);
    }
    return handle;
}

// ---------------------------------------------------------------------------
// per-slot stop — sub_17912 body (eip 0x7931-0x7969): fnAB stop, fn98 release,
// clear [si-66F4]/[si-66EA]. The slot-loop policy (music protection via
// [25B9]) stays with the caller — this is the single-slot primitive.
// ---------------------------------------------------------------------------
extern "C" void v2_ail_seq_stop_slot(uint8_t* s, uint16_t si) {
    if (!g_booted) return;
    uint16_t handle = rdw(s, (uint16_t)(si - 0x66F4));
    if (handle == 0xFFFF) return;

    uint16_t drv = rdw(s, DS_98E6_DRV);
    uint16_t a[2] = { drv, handle };
    sh_call(0xAB, a, 2);    // sub_1C79F stop_sequence
    sh_call(0x98, a, 2);    // sub_1C769 release_sequence
    wrw(s, (uint16_t)(si - 0x66F4), 0xFFFF);
    wrw(s, (uint16_t)(si - 0x66EA), 0xFFFF);
}

// ---------------------------------------------------------------------------
// music fade — sub_178f1 (eip 0x78FB-0x790D): fnB1(drv, [990C], 0, 0x3E8) via
// CALLF sub_1C7BD (seg002 0D4F:0CED: mov ax,0B1h). The driver ramps the
// sequence volume from its current level ([state+0x2C] <- 0x64 on call) down
// to 0 over 1000 ms and silences itself.
// ---------------------------------------------------------------------------
extern "C" void v2_ail_music_fade(uint8_t* s) {
    if (!g_booted) return;
    // (#85 call-parity catch) NO handle gate: orig sub_178f1 tests only
    // [302] and always issues fnB1 with [990C] as-is — a stopped-music
    // transition passes handle 0xFFFF and the driver no-ops it itself.
    // The old `handle==FFFF return` here broke per-frame call parity.
    uint16_t handle = rdw(s, (uint16_t)(0 - 0x66F4));   // [990C] (si=0 family)
    uint16_t a[4] = { rdw(s, DS_98E6_DRV), handle, 0, 0x3E8 };
    sh_call(0xB1, a, 4);    // sub_1C7BD
}

// ---------------------------------------------------------------------------
// SFX start — exact sub_177bb tail (eip 0x77BB-0x7829). Slot scan si=8..2:
//   free slot ([si-66F4]==FFFF)  → [si-66EA]=seq; 176bd-start with this si
//   busy slot                    → fnAE status (sub_1C7AB): 1=playing → next
//                                  else fn98 release + reuse the slot
//   all four playing             → drop the request (orig eip 0x7825)
// The handle lands in [si-66F4] inside the start chain (176bd eip 0x76DA).
// Returns the driver handle, 0xFFFF when dropped/muted.
// ---------------------------------------------------------------------------
extern "C" uint16_t v2_ail_sfx_play(uint8_t* s, uint8_t* snd, uint32_t snd_size,
                                    uint16_t ds_val, uint16_t ax_seq) {
    if (!g_booted) return 0xFFFF;
    if (rdw(s, 0x304) != 0) return 0xFFFF;          // eip 0x77BD SFX muted
    uint16_t sfx_seg = rdw(s, 0x2E6D);              // SFX XMID catalog segment
    uint16_t drv = rdw(s, DS_98E6_DRV);
    for (int16_t si = 8; si > 0; si -= 2) {
        uint16_t handle = rdw(s, (uint16_t)(si - 0x66F4));
        if (handle == 0xFFFF) {
            wrw(s, (uint16_t)(si - 0x66EA), ax_seq);            // eip 0x77D2
            return v2_ail_seq_start(s, snd, snd_size, ds_val, sfx_seg, ax_seq, (uint16_t)si);
        }
        uint16_t a[2] = { drv, handle };
        uint16_t status = sh_call(0xAE, a, 2);      // eip 0x77EC status
        if (status == 1) continue;                              // eip 0x77F6 playing
        sh_call(0x98, a, 2);                        // eip 0x7806 release
        wrw(s, (uint16_t)(si - 0x66EA), ax_seq);                // eip 0x7811
        return v2_ail_seq_start(s, snd, snd_size, ds_val, sfx_seg, ax_seq, (uint16_t)si);
    }
    return 0xFFFF;
}

// ---------------------------------------------------------------------------
// SFX stop by sequence — the shared slot-scan body of sub_1782a / sub_1787f
// (eip 0x783E-0x787A / 0x7895-0x78D1): every slot whose SEQ word matches goes
// through fnAB stop + fn98 release and BOTH slot words get cleared to FFFF.
// NOTE: unlike the sub_17912 loop there is NO [si-66F4]==FFFF guard here —
// the orig calls fnAB/fn98 even with a FFFF handle and always clears the seq
// word (divergence found by the headless audio-sym checker: shadow kept
// s=0x57 in a slot the real world had wiped). The handle is re-read from the
// slot before the fn98 push, exactly like orig eip 0x7859/0x78B0. The [304]
// mute gate stays with the caller mirrors (both orig sites test it before
// the scan).
// ---------------------------------------------------------------------------
extern "C" void v2_ail_sfx_stop_seq(uint8_t* s, uint16_t ax_seq) {
    if (!g_booted) return;
    uint16_t drv = rdw(s, DS_98E6_DRV);
    for (int16_t si = 8; si > 0; si -= 2) {
        if (rdw(s, (uint16_t)(si - 0x66EA)) != ax_seq) continue;
        uint16_t a[2] = { drv, rdw(s, (uint16_t)(si - 0x66F4)) };
        sh_call(0xAB, a, 2);                                // eip 0x7850 stop
        a[1] = rdw(s, (uint16_t)(si - 0x66F4));             // re-read (0x7859)
        sh_call(0x98, a, 2);                                // eip 0x7861 release
        wrw(s, (uint16_t)(si - 0x66F4), 0xFFFF);
        wrw(s, (uint16_t)(si - 0x66EA), 0xFFFF);
    }
}

// ---------------------------------------------------------------------------
// music mute-stop — sub_108c8 loc_10959 (eip 0x961-0x97B): fnAB stop + fn98
// release on the music handle [990C], WITHOUT clearing the DS slot words
// (unlike the 17912/1782a bodies — the orig mute path leaves them; the next
// unmute start overwrites [990C] through the fn97 chain).
// ---------------------------------------------------------------------------
extern "C" void v2_ail_music_mute_stop(uint8_t* s) {
    if (!g_booted) return;
    uint16_t a[2] = { rdw(s, DS_98E6_DRV), rdw(s, (uint16_t)(0 - 0x66F4)) };
    sh_call(0xAB, a, 2);
    sh_call(0x98, a, 2);
}

// ---------------------------------------------------------------------------
// REAL-world instance (default/verify mode): fed by the sub_1bec2 bridge in
// seg002 — the ORIGINAL m2c code runs every AIL chain (17561 init, 176bd
// starts, 17912/1782a/1787f stops, 108c8 mutes, 178f1 fades) and only the
// jump INTO the blob is redirected here. One flat mapping (paragraph 0 →
// the whole m2c DOS arena) makes the instance live in the game's actual
// memory: state blocks, XMID data, the bank and the cache buffer resolve
// exactly like on DOS, so the driver writes the real DS at the original
// addresses and the DS verify covers them.
// ---------------------------------------------------------------------------
static bool g_orig_inited = false;

extern "C" int v2_ail_orig_enabled() {
#ifdef V2_ONLY
    return 0;                    // no m2c world in V2_ONLY builds
#else
    return v2_ail_native_on();
#endif
}

static int v2_ail_orig_lazy_init() {
    if (g_orig_inited) return 1;
    extern uint8_t* v2_m2c_base;
    uint8_t* base = v2_m2c_base ? v2_m2c_base : v2_m2c_arena();
    if (!base) { fprintf(stderr, "V2-AIL-ORIG-FAIL: no m2c arena\n"); return 0; }
    uint16_t ds_val = v2_ail_get_real_ds();
    // The first bridged calls (sub_17561 init chain) run BEFORE
    // v2_vm_init_shadow_early publishes the DS value; the ported EXE's data
    // segment paragraph is a build constant: seg004 = m2c::m + 0x19F00
    // (_data.cpp), i.e. paragraph 0x19F0.
    if (!ds_val) ds_val = 0x19F0;
    uint8_t* rds = base + ((uint32_t)ds_val << 4);
    uint16_t blob_seg = rdw(rds, DS_992C_SND);
    if (!blob_seg) { fprintf(stderr, "V2-AIL-ORIG-FAIL: [992C]=0\n"); return 0; }
    uint8_t* blob = base + ((uint32_t)blob_seg << 4);
    uint16_t bank_seg = rdw(rds, DS_2E6F_BANK);
    uint8_t* bank = base + ((uint32_t)bank_seg << 4);
    v2_ail_interp_use(1);
    // Full 64K copies into the code arenas — the exact chunk sizes are
    // irrelevant here: execution only ever reaches valid blob offsets, and
    // BANK_PARA sits above 640K so the copy is a mere placeholder (the real
    // bank is addressed through the flat DOS mapping below).
    v2_ail_interp_load(blob, 0x10000, bank, 0x10000);
    // (#83) The real instance is SILENT again: the audible path is the SINK
    // instance (audio thread, sample-clock ticks). Real+shadow are the
    // deterministic frame-tick verify pair — neither feeds the chip.
    v2_ail_interp_set_io(silent_out, silent_in);
    v2_ail_interp_set_callback(v2_ail_pit_callback);
    v2_ail_interp_map_segment(0, base, 0xA0000);                 // whole DOS arena
    // (#82) run the blob AT its real segment: descriptor/config live in the
    // arena, so the orig init chain (LES bx,[98E8] with dx=cs) reads what
    // fn64/fn65 actually produced. With the fake DRV_PARA the descriptor far
    // ptr came back F000:00C7 → fn66 got zero IO base → silent OPL.
    v2_ail_interp_set_code_para(blob_seg);
    g_orig_inited = true;
    if (g_tick_hz <= 0.0) {
        // [desc+0x14]+5 exactly like the fn66 stub computes (sub_1c61b).
        uint16_t hz = v2_ail_interp_peek((uint16_t)(0xC7 + 0x14));
        if (hz && hz != 0xFFFF) { g_tick_hz = (double)(hz + 5); v2_nopl_set_tick_hz(g_tick_hz); }
    }
    fprintf(stderr, "V2-AIL-ORIG: bridge instance up (blob seg=%04X ds=%04X)\n",
            blob_seg, ds_val);
    // (#83) publish the sink build parameters — the audio thread constructs
    // the sink lazily from these on its first pump.
    v2_ail_sink_publish(base, blob_seg, bank_seg, ds_val);
    return 1;
}

// ---------------------------------------------------------------------------
// (#83) SINK — the audible instance. Design: v2_ail_orig_bridge duplicates
// every bridged call into a SPSC ring (game thread side). The audio callback
// (v2_nopl_mix → v2_ail_sink_pump) builds the instance on first use, drains
// the ring with a per-callback budget and runs fn67 ticks on the SAMPLE
// clock — i.e. the sink lives exactly like the single DOS driver instance
// (real-time INT8, immediate SFX), while the verify pair stays frame-based
// and silent. The sink writes its XMID state into a PRIVATE DS copy and its
// timbre cache into a PRIVATE buffer (map_front windows) — it can never
// touch the verified worlds.
// ---------------------------------------------------------------------------
struct SinkCall { uint16_t off; uint16_t argc; uint16_t args[10]; };
static const size_t SINK_RING = 2048;
static SinkCall g_sink_ring[SINK_RING];
static std::atomic<size_t> g_sink_wr{0}, g_sink_rd{0};

static std::atomic<uint8_t*> g_sink_base{nullptr};
static std::atomic<uint32_t> g_sink_params{0};   // blob_seg<<16 | ds_val
static std::atomic<uint16_t> g_sink_bank_seg{0};

extern "C" void v2_ail_sink_publish(uint8_t* base, uint16_t blob_seg,
                                    uint16_t bank_seg, uint16_t ds_val) {
    g_sink_bank_seg.store(bank_seg, std::memory_order_relaxed);
    g_sink_params.store(((uint32_t)blob_seg << 16) | ds_val, std::memory_order_relaxed);
    g_sink_base.store(base, std::memory_order_release);
}

// Game-thread side: enqueue one bridged call for the sink. Drop-on-full is
// loud — a lost start/stop desyncs what the user HEARS (never the verify).
static void v2_ail_sink_note_call(uint16_t off, const uint16_t* args, int argc) {
    size_t wr = g_sink_wr.load(std::memory_order_relaxed);
    size_t nx = (wr + 1) & (SINK_RING - 1);
    if (nx == g_sink_rd.load(std::memory_order_acquire)) {
        static uint64_t drops = 0;
        if ((++drops & (drops - 1)) == 0)
            fprintf(stderr, "V2-AIL-SINK: call ring FULL — %llu dropped\n",
                    (unsigned long long)drops);
        return;
    }
    SinkCall& c = g_sink_ring[wr];
    c.off = off;
    c.argc = (uint16_t)((argc > 10) ? 10 : argc);
    for (int i = 0; i < c.argc; i++) c.args[i] = args[i];
    g_sink_wr.store(nx, std::memory_order_release);
}

// --- everything below runs on the AUDIO thread only ------------------------
static bool     g_sink_built = false;
static uint8_t  g_sink_ds[0x10000];
static uint8_t  g_sink_cache[sizeof(g_cache)];
static uint16_t g_sink_fn9a_off = 0;     // resolved after build for the cache hook
static uint64_t g_sink_ticks = 0;
static uint64_t g_sink_tick_base = 0;    // sample position of the sink build
extern "C" uint64_t v2_ail_sink_ticks(void) { return g_sink_ticks; }   // UX stage 11: the MIDI lane's clock in the verify build (audio thread, like the tap)

static void v2_ail_sink_build(uint64_t pos) {
    uint8_t* base = g_sink_base.load(std::memory_order_acquire);
    if (!base) return;
    uint32_t bp = g_sink_params.load(std::memory_order_relaxed);
    uint16_t blob_seg = (uint16_t)(bp >> 16), ds_val = (uint16_t)bp;
    uint16_t bank_seg = g_sink_bank_seg.load(std::memory_order_relaxed);
    uint8_t* blob = base + ((uint32_t)blob_seg << 4);
    uint8_t* bank = base + ((uint32_t)bank_seg << 4);
    v2_ail_sinki_load(blob, 0x10000, bank, 0x10000);
    v2_ail_sinki_set_io(v2_nopl_sink_out, v2_nopl_sink_in);
    v2_ail_sinki_set_callback(v2_ail_pit_callback);
    // UX stage 11: the sink is what the user hears in this build — the MIDI
    // lane (SC-55 option / V2_MIDI_DUMP) observes its dispatcher, on this thread
    v2_ail_sinki_set_midi_tap(v2_midi_event);
    v2_midi_set_clock(v2_ail_sink_ticks);
    v2_ail_sinki_map(0, base, 0xA0000);                     // flat arena (reads)
    memcpy(g_sink_ds, base + ((uint32_t)ds_val << 4), 0x10000);
    v2_ail_sinki_map_front(ds_val, g_sink_ds, 0x10000);     // private XMID state
    g_sink_fn9a_off = v2_ail_sinki_fn_lookup(0x9A);
    g_sink_tick_base = pos;
    g_sink_ticks = 0;
    g_sink_built = true;
    fprintf(stderr, "V2-AIL-SINK: up (blob=%04X ds=%04X bank=%04X)\n",
            blob_seg, ds_val, bank_seg);
}

extern "C" void v2_ail_sink_pump(uint64_t pos) {
    if (!g_sink_built) {
        if (!g_sink_base.load(std::memory_order_acquire)) return;
        v2_ail_sink_build(pos);
        if (!g_sink_built) return;
    }
    // 1) drain bridged calls (budget: heavy fns — timbre loads — must not
    //    starve the sample generator; leftovers run next callback, ~6ms away)
    int budget = 8;
    size_t rd = g_sink_rd.load(std::memory_order_relaxed);
    while (budget-- > 0 && rd != g_sink_wr.load(std::memory_order_acquire)) {
        SinkCall& c = g_sink_ring[rd];
        // cache-alloc interception: the duplicated fn9A carries the REAL
        // heap segment; the sink must write its timbre cache privately.
        if (g_sink_fn9a_off && c.off == g_sink_fn9a_off && c.argc >= 3)
            v2_ail_sinki_map_front(c.args[2], g_sink_cache, sizeof(g_sink_cache));
        v2_ail_sinki_call(c.off, c.args, c.argc);
        rd = (rd + 1) & (SINK_RING - 1);
    }
    g_sink_rd.store(rd, std::memory_order_release);
    // 2) fn67 ticks on the sample clock (the DOS INT8): due by wall audio
    //    time since build; same catch-up cap semantics as the V2_ONLY pump.
    double hz = v2_nopl_get_tick_hz();
    if (hz <= 0.0) return;
    double spt = (double)v2_nopl_get_rate() / hz;
    uint64_t rel = (pos > g_sink_tick_base) ? pos - g_sink_tick_base : 0;
    uint64_t due = (uint64_t)((double)rel / spt);
    if (due > g_sink_ticks + 64) {
        uint64_t excess = due - g_sink_ticks - 64;
        g_sink_tick_base += (uint64_t)((double)excess * spt);
        due -= excess;
    }
    uint16_t off67 = v2_ail_sinki_fn_lookup(0x67);
    if (!off67) return;
    int guard = 0;
    uint16_t a[1] = { 0 };
    while (g_sink_ticks < due && guard++ < 96) {
        v2_ail_sinki_call(off67, a, 1);
        g_sink_ticks++;
    }
}

// The sub_1bec2 bridge: the m2c dispatcher resolved the handler offset via
// the ORIGINAL install/lookup code (sub_1c537/sub_1be8a walk real memory);
// we execute that handler on the real instance with the caller's stack
// words ([sp+4]=drv, [sp+6..]=args — same frame the blob would have seen).
// Returns ax; dx via v2_ail_orig_last_dx.
static uint16_t g_orig_dx = 0;
extern "C" uint16_t v2_ail_orig_bridge(uint16_t handler_off,
                                       const uint16_t* args, int argc) {
    if (!v2_ail_orig_enabled()) return 0;
    v2_ail_interp_lock();
    if (!v2_ail_orig_lazy_init()) { v2_ail_interp_unlock(); return 0; }
    v2_ail_interp_use(1);
    v2_ailpar_note(1, handler_off);                 // (#85) call parity, real leg
    uint16_t ax = v2_ail_interp_call(handler_off, args, argc);
    g_orig_dx = v2_ail_interp_last_dx();
    v2_ail_interp_unlock();
    // (#83) mirror the call into the audible sink (audio thread consumes).
    // HEADLESS has no audio device → no consumer: don't queue (the ring
    // would only fill up and spam drop warnings).
#ifndef HEADLESS
    { extern int v2_fntest_running;
      if (!v2_fntest_running) v2_ail_sink_note_call(handler_off, args, argc); }
#endif
    return ax;
}
extern "C" uint16_t v2_ail_orig_last_dx() { return g_orig_dx; }

// ---------------------------------------------------------------------------
// timer tick — the seg002 INT8 slot calls the blob's fn67 handler. Ticks BOTH
// live instances back-to-back under one lock: both worlds see the identical
// tick count between any pair of mirrored chain calls (frame-barrier pacing
// in default mode), which keeps their driver state byte-equal.
// ---------------------------------------------------------------------------
extern "C" void v2_ail_tick() {
    uint16_t a[1] = { 0 };
    // Stage 6.1 w3: window the FULL interpreter trace to one fn67 tick.
    // V2_AIL_TRACE_TICK=N (+V2_AIL_TRACE_TICKF=path) arms the per-step
    // register log for exactly the N-th tick of the shadow leg.
    {
        extern int v2_ail_pctrace_full_on;
        extern FILE* v2_ail_pctrace_full_f;
        static long _tt = -2; static long _cnt = 0;
        if (_tt == -2) {
            const char* e = getenv("V2_AIL_TRACE_TICK");
            _tt = (e && *e) ? strtol(e, 0, 0) : -1;
            if (_tt >= 0) {
                const char* f = getenv("V2_AIL_TRACE_TICKF");
                v2_ail_pctrace_full_f = fopen(f && *f ? f : "/tmp/ail_tick.trace", "w");
            }
        }
        if (_tt >= 0) {
            v2_ail_pctrace_full_on = (_cnt == _tt) ? 1 : 0;
            _cnt++;
        }
    }
    if (g_booted) sh_call(0x67, a, 1);              // parity noted inside
    {
        extern int v2_ail_pctrace_full_on;
        extern FILE* v2_ail_pctrace_full_f;
        if (v2_ail_pctrace_full_on && v2_ail_pctrace_full_f)
            fflush(v2_ail_pctrace_full_f);
        v2_ail_pctrace_full_on = 0;
    }
    if (g_orig_inited) {
        v2_ail_interp_lock();
        v2_ail_interp_use(1);
        v2_ailpar_note(1, v2_ail_fn_lookup(0x67));  // (#85) real tick leg
        v2_ail_call_fn_code(0x67, a, 1);
        v2_ail_interp_unlock();
    }
}

extern "C" int v2_ail_booted() { return (g_booted || g_orig_inited) ? 1 : 0; }

// ---------------------------------------------------------------------------
// (#85) FRAME_BEGIN barrier verify: AIL call parity + OPL stream parity.
// Called from v2_phase_frame_begin (v2 thread; the orig thread is parked on
// the signal, so both worlds' counters are quiescent). Any mismatch is a
// divergence: same fatality path as the DS verify in headless.
// ---------------------------------------------------------------------------
extern "C" void v2_ail_parity_verify(int frame) {
#ifdef V2_ONLY
    (void)frame;    // single world — nothing to compare
#else
    extern int v2_fntest_running;
    if (v2_fntest_running) return;
    static bool boot_frame_skipped = false;
    if (!(g_booted && g_orig_inited)) {   // pre-boot frames: both silent
        // still reset so a lone early call can't linger into the booted era
        memset(g_ailpar, 0, sizeof(g_ailpar));
        g_oplhash[0] = g_oplhash[1] = 0x811C9DC5u;
        g_oplcnt[0] = g_oplcnt[1] = 0;
        return;
    }
    if (!boot_frame_skipped) {
        // The boot frame itself is legitimately asymmetric: the real world
        // boots through the FULL seg002 wrappers (install/describe with
        // their service query at blob off 27C8), while the shadow boot is
        // the exact 17561-TAIL model (fn calls only). Parity is a strict
        // per-frame contract from the first post-boot frame on.
        boot_frame_skipped = true;
        memset(g_ailpar, 0, sizeof(g_ailpar));
        g_oplhash[0] = g_oplhash[1] = 0x811C9DC5u;
        g_oplcnt[0] = g_oplcnt[1] = 0;
        return;
    }
    long bad = 0;
    for (int off = 0; off < AILPAR_SLOTS; off++) {
        if (g_ailpar[0][off] == g_ailpar[1][off]) continue;
        if (bad < 8)
            fprintf(stderr, "V2-AILPAR-DIVERGE[f%d]: off=%04X shadow=%u real=%u\n",
                    frame, off, g_ailpar[0][off], g_ailpar[1][off]);
        bad++;
    }
    bool oplbad = (g_oplhash[0] != g_oplhash[1]) || (g_oplcnt[0] != g_oplcnt[1]);
    if (oplbad)
        fprintf(stderr, "V2-OPLSTREAM-DIVERGE[f%d]: shadow=%08X/%u real=%08X/%u\n",
                frame, g_oplhash[0], g_oplcnt[0], g_oplhash[1], g_oplcnt[1]);
    if (bad || oplbad) {
#ifdef HEADLESS
        char buf[96];
        snprintf(buf, sizeof(buf), "ail parity: %ld call slots, opl %s", bad,
                 oplbad ? "hash/count mismatch" : "ok");
        headless_dump_divergence(bad ? "ail-call-parity" : "opl-stream", frame, buf);
#endif
    }
    memset(g_ailpar, 0, sizeof(g_ailpar));
    g_oplhash[0] = g_oplhash[1] = 0x811C9DC5u;
    g_oplcnt[0] = g_oplcnt[1] = 0;
#endif
}
