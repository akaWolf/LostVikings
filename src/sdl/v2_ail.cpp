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

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

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
static SilentOpl g_silent;
static void silent_out(uint16_t port, uint8_t val) {
    if (port >= 0x220 && port <= 0x223) {
        int b = (port - 0x220) >> 1;
        if ((port & 1) == 0) g_silent.idx[b] = val;
        else g_silent.regs[b][g_silent.idx[b]] = val;
        return;
    }
    if (port == 0x224) { g_silent.mixer_idx = val; return; }
    if (port == 0x225) { g_silent.mixer[g_silent.mixer_idx] = val; return; }
}
static uint8_t silent_in(uint16_t port) {
    if (port >= 0x220 && port <= 0x223) {
        uint8_t ctl = g_silent.regs[0][4];
        if (ctl & 0x80) return 0x00;
        if (ctl & 0x01) return 0xC0 | 0x40;
        return 0x00;
    }
    if (port == 0x225) return g_silent.mixer[g_silent.mixer_idx];
    return 0xFF;
}

// All shadow-chain fn calls go through here: CLI-model lock + instance 0.
static uint16_t sh_call(uint16_t fn_code, const uint16_t* args, int argc) {
    v2_ail_interp_lock();
    v2_ail_interp_use(0);
    uint16_t ax = v2_ail_call_fn_code(fn_code, args, argc);
    v2_ail_interp_unlock();
    return ax;
}

static uint16_t rdw(const uint8_t* s, uint16_t off) {
    return (uint16_t)(s[off] | (s[(uint16_t)(off + 1)] << 8));
}
static void wrw(uint8_t* s, uint16_t off, uint16_t v) {
    s[off] = (uint8_t)v; s[(uint16_t)(off + 1)] = (uint8_t)(v >> 8);
}

// ---------------------------------------------------------------------------
// gate
// ---------------------------------------------------------------------------
extern "C" int v2_fntest_running = 0;   // set by v2_fn_test.cpp (absent in V2_ONLY builds)

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
// snapshot seg002 keeps for the AIL timer. Our timer model paces fn67 by the
// audio clock instead of the PIT; the divisor value only feeds the driver's
// internal rate bookkeeping. 0x7FFF mirrors the seg002 power-on default
// (sub_1bedc reprograms it later on real DOS — not modeled, no PIT here).
static uint16_t v2_ail_pit_callback() { return 0x7FFF; }

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
    v2_ail_interp_map_segment(snd_base, snd, snd_size);       // whole sound arena
    v2_ail_interp_map_segment(ds_val, s, 0x10000);            // the game DS (state blocks!)
    v2_ail_interp_map_segment(CACHE_PARA, g_cache, sizeof(g_cache));

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
        uint16_t dx = v2_ail_interp_last_dx();
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
    uint8_t  al = s[DS_993E_BANK];      // bank   (orig: mov al,[993E])
    uint8_t  ah = s[DS_9940_PATCH];     // patch  (orig: mov ah,[9940])
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
    uint16_t handle = rdw(s, (uint16_t)(0 - 0x66F4));   // [990C] (si=0 family)
    if (handle == 0xFFFF) return;
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
    v2_ail_interp_set_io(v2_nopl_sbpro_out, v2_nopl_sbpro_in);   // audible world
    v2_ail_interp_set_callback(v2_ail_pit_callback);
    v2_ail_interp_map_segment(0, base, 0xA0000);                 // whole DOS arena
    g_orig_inited = true;
    if (g_tick_hz <= 0.0) {
        // [desc+0x14]+5 exactly like the fn66 stub computes (sub_1c61b).
        uint16_t hz = v2_ail_interp_peek((uint16_t)(0xC7 + 0x14));
        if (hz && hz != 0xFFFF) { g_tick_hz = (double)(hz + 5); v2_nopl_set_tick_hz(g_tick_hz); }
    }
    fprintf(stderr, "V2-AIL-ORIG: bridge instance up (blob seg=%04X ds=%04X)\n",
            blob_seg, ds_val);
    return 1;
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
    uint16_t ax = v2_ail_interp_call(handler_off, args, argc);
    g_orig_dx = v2_ail_interp_last_dx();
    v2_ail_interp_unlock();
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
    if (g_booted) sh_call(0x67, a, 1);
    if (g_orig_inited) {
        v2_ail_interp_lock();
        v2_ail_interp_use(1);
        v2_ail_call_fn_code(0x67, a, 1);
        v2_ail_interp_unlock();
    }
}

extern "C" int v2_ail_booted() { return (g_booted || g_orig_inited) ? 1 : 0; }
