// Stage 6.1 w3: the NATIVE sequencer — a line-by-line port of SBPFM.ADV
// (assets_raw/ail_blob.lst is the source listing; every function carries
// its blob address and mirrors the instructions 1:1). The native state
// lives in this instance's own copy of the blob image (the driver keeps
// its variables inside its code segment), so interp-vs-native unit checks
// can byte-compare the whole data area.
//
// Increment 1 (leaf primitives, proven by the tick-120 window trace):
//   13A8  opl_write_operator(op_index, reg_base, val)
//   13C4  opl_write_voice(voice_index, reg_base, val)
//   13DE  shared OUT tail (port = cs:[0x645] + 2*bank, 6x/8x IN delays)
//   15E9  find_sounding_slot(ah=channel_key, al=note_key) over 0xC0 slots
//
// Self-test: V2_AILNAT_SELFTEST=1 (after the AIL boot) runs randomized
// unit sweeps of each ported function against the interpreter on an
// identical data image; any mismatch is fatal-loud.
#include "v2_midi.h"        // UX stage 11: the MIDI lane (tap at midi_2629)
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

// ---------------------------------------------------------------------------
// Native instance state
// ---------------------------------------------------------------------------
static uint8_t  g_nat[0x10000];     // blob image copy (code+data; data is live)
static uint32_t g_nat_size = 0;

// OUT capture for the unit judge (and later the real OPL feed).
typedef void (*nat_out_fn)(uint16_t port, uint8_t val);
typedef uint8_t (*nat_in_fn)(uint16_t port);
static nat_out_fn g_nat_out = nullptr;
static nat_in_fn  g_nat_in  = nullptr;

extern "C" void v2_ailnat_set_io(nat_out_fn o, nat_in_fn i) { g_nat_out = o; g_nat_in = i; }

extern "C" void v2_ailnat_load(const uint8_t* blob, uint32_t size) {
    if (size > sizeof(g_nat)) size = sizeof(g_nat);
    memcpy(g_nat, blob, size);
    g_nat_size = size;
}
extern "C" uint8_t* v2_ailnat_data() { return g_nat; }
extern "C" uint32_t v2_ailnat_size() { return g_nat_size; }

static inline uint8_t  rd8n(uint16_t off) { return g_nat[off]; }
static inline uint16_t rd16n(uint16_t off) { return (uint16_t)(g_nat[off] | (g_nat[(uint16_t)(off + 1)] << 8)); }
static inline void     wr8n(uint16_t off, uint8_t v) { g_nat[off] = v; }
static inline void     wr16n(uint16_t off, uint16_t v) { g_nat[off] = (uint8_t)v; g_nat[(uint16_t)(off + 1)] = (uint8_t)(v >> 8); }

// ---------------------------------------------------------------------------
// 13DE: shared OPL OUT tail.
//   dx = cs:[0x645]; dl += bh (adc dh,0) twice  -> port = base + 2*bank
//   OUT dx, reg(bl); 6x IN dx (bus settle)
//   OUT dx+1, val(cl); 8x IN dx
// The IN results are discarded by the blob — the native port keeps the IN
// calls (the sink models the status register) but ignores the value too.
// ---------------------------------------------------------------------------
static void nat_out_tail_13DE(uint8_t reg_bl, uint8_t bank_bh, uint8_t val_cl) {
    uint16_t dx = rd16n(0x645);                    // mov dx, cs:[0x645]
    dx = (uint16_t)(dx + bank_bh);                 // add dl,bh; adc dh,0
    dx = (uint16_t)(dx + bank_bh);                 // add dl,bh; adc dh,0
    if (g_nat_out) g_nat_out(dx, reg_bl);          // mov al,bl; out dx,al
    for (int i = 0; i < 6; i++)                    // mov ah,6; in al,dx; dec ah; jne
        if (g_nat_in) (void)g_nat_in(dx);
    if (g_nat_out) g_nat_out((uint16_t)(dx + 1), val_cl); // inc dx; out dx,al(cl); dec dx
    for (int i = 0; i < 8; i++)                    // mov ah,8; in al,dx; dec ah; jne
        if (g_nat_in) (void)g_nat_in(dx);
}

// ---------------------------------------------------------------------------
// 13A8: operator-register write.
//   bl = op_index (byte arg bp+6); bh = 0
//   ah = cs:[bx+0x58F]   (per-operator bank select 0/1)
//   bl = cs:[bx+0x56B] + reg_base (bp+8)   (per-operator register offset)
//   bh = ah; cl = val (bp+0xA)  -> 13DE
// ---------------------------------------------------------------------------
extern "C" void v2_ailnat_opl_op_write_13A8(uint8_t op_index, uint8_t reg_base, uint8_t val) {
    uint16_t bx = op_index;                        // mov bl,[bp+6]; mov bh,0
    uint8_t ah = rd8n((uint16_t)(bx + 0x58F));     // mov ah, cs:[bx+0x58F]
    uint8_t bl = rd8n((uint16_t)(bx + 0x56B));     // mov bl, cs:[bx+0x56B]
    bl = (uint8_t)(bl + reg_base);                 // add bl,[bp+8]
    nat_out_tail_13DE(bl, ah, val);                // mov bh,ah; mov cl,[bp+0xA]; jmp 13DE
}

// ---------------------------------------------------------------------------
// 13C4: voice-register write.
//   bx = voice_index (word arg bp+6)
//   ah = cs:[bx+0x5C5]; bl = cs:[bx+0x5B3] + reg_base; cl = val -> 13DE
// ---------------------------------------------------------------------------
extern "C" void v2_ailnat_opl_voice_write_13C4(uint16_t voice_index, uint8_t reg_base, uint8_t val) {
    uint16_t bx = voice_index;                     // mov bx,[bp+6]
    uint8_t ah = rd8n((uint16_t)(bx + 0x5C5));     // mov ah, cs:[bx+0x5C5]
    uint8_t bl = rd8n((uint16_t)(bx + 0x5B3));     // mov bl, cs:[bx+0x5B3]
    bl = (uint8_t)(bl + reg_base);                 // add bl,[bp+8]
    nat_out_tail_13DE(bl, ah, val);                // mov bh,ah; mov cl,[bp+0xA]; jmp 13DE
}

// ---------------------------------------------------------------------------
// 15E9: find a sounding slot by (ah=channel key, al=note key).
//   for si = 0 .. 0xBF:
//     if (cs:[si+0xC4B] & 0x80) && cs:[si+0xACB]==ah && cs:[si+0xB8B]==al
//         return si
//   return 0xFFFF
// ---------------------------------------------------------------------------
extern "C" uint16_t v2_ailnat_find_slot_15E9(uint16_t ax) {
    uint8_t ah = (uint8_t)(ax >> 8), al = (uint8_t)ax;
    for (uint16_t si = 0; si < 0xC0; si++) {       // mov si,0 / inc si / cmp si,0xC0; jb
        if (!(rd8n((uint16_t)(si + 0xC4B)) & 0x80))    // test cs:[si+0xC4B],0x80; je next
            continue;
        if (rd8n((uint16_t)(si + 0xACB)) != ah)        // cmp cs:[si+0xACB],ah; jne next
            continue;
        if (rd8n((uint16_t)(si + 0xB8B)) == al)        // cmp cs:[si+0xB8B],al; je found
            return si;
    }
    return 0xFFFF;                                  // mov si,0xFFFF
}

// ---------------------------------------------------------------------------
// 1B7C: voice register updater — flush the dirty parameter groups of slot si
// to the chip. Listing: body 1B7C..1E33, group tails 1E44..2152.
//
// Shape (from the listing — NOT a loop): the dirty mask cs:[si+0xE49] is
// tested bit by bit in a one-shot chain 0x80,0x40,0x20,0x10,0x08,0x01
// (1D34..1D73); each serviced tail clears its bit and jumps back to the
// NEXT test. Percussion slots (voice mode cs:[si+0xDA9]==3) run the chain
// twice: pass A on voice+3 with the MODIFIED layer (cs:0x108D..0x1208) —
// its 0x01 group only clears the bit (2011 -> 214C) — then 1D76 restores
// the mask from [bp-0x12] and pass B runs on the voice with the timbre
// BASE layer (cs:0xE99..0x108C). Melodic slots run pass B only (1C83).
// ---------------------------------------------------------------------------
static void nat_upd_pass_1C8C(uint16_t si, int passA, uint8_t vel_scale);

// 2112: shared frequency tail — A0 = fnum lo, B0 = fnum hi | key/sustain
// image cs:[si+0xE71], B0 remembered in cs:[si+0xE5D], dirty bit cleared.
static void nat_freq_tail_2112(uint16_t si, uint16_t voice, uint16_t fw) {
    v2_ailnat_opl_voice_write_13C4(voice, 0xA0, (uint8_t)fw);       // 2124
    uint8_t b0 = (uint8_t)((uint8_t)(fw >> 8) | rd8n((uint16_t)(si + 0xE71))); // 212D
    wr8n((uint16_t)(si + 0xE5D), b0);                               // 2132
    v2_ailnat_opl_voice_write_13C4(voice, 0xB0, b0);                // 2146
    wr8n((uint16_t)(si + 0xE49), (uint8_t)(rd8n((uint16_t)(si + 0xE49)) & 0xFE)); // 214C
}

extern "C" void v2_ailnat_update_voice_1B7C(uint16_t si) {
    if (rd8n((uint16_t)(si + 0xDBD)) == 0xFF)     // 1B88 cmp cs:[si+0xDBD],0xFF -> 1E2D
        return;
    uint8_t vel_scale = 0;                        // [bp-0x8] (dead unless bit 0x40 set)
    if (rd8n((uint16_t)(si + 0xE49)) & 0x40) {    // 1B93
        uint16_t di = (uint16_t)(rd16n((uint16_t)(si + 0xDD1)) & 0xF);
        // (channel volume * expression) rounded, then * note velocity, rounded
        uint16_t ax = (uint16_t)(rd8n((uint16_t)(di + 0x1209)) *
                                 rd8n((uint16_t)(di + 0x1249)));   // 1BA8 mul
        ax = (uint16_t)(ax << 1);                 // shl ax,1
        uint8_t al = (uint8_t)(ax >> 8);          // mov al,ah
        al = (uint8_t)(al + (al >= 1 ? 1 : 0));   // cmp al,1; sbb al,0xFF
        ax = (uint16_t)(al * rd8n((uint16_t)(si + 0xE21)));        // 1BB5 mul
        ax = (uint16_t)(ax << 1);
        al = (uint8_t)(ax >> 8);
        al = (uint8_t)(al + (al >= 1 ? 1 : 0));
        vel_scale = al;                           // 1BC2 mov [bp-0x8],al
    }
    int perc = (rd8n((uint16_t)(si + 0xDA9)) == 3) ? 1 : 0;   // 1BC5..1BD3 [bp-0x10]

    // 1BD6..1C80: the 4-op connection image cs:0xD1C <-> OPL reg 0x104 (bank 1)
    {
        uint16_t di = (uint16_t)(rd16n((uint16_t)(si + 0xDBD)) & 0xFF);
        uint8_t cl = rd8n(0xD1C);                 // 1BDF
        uint8_t dl = rd8n((uint16_t)(di + 0x61F));// per-voice connection bit
        if (perc) {                               // 1BE9 or ax,ax; je 1C07
            cl = (uint8_t)(cl | dl);              // 1BED
            if (cl != rd8n(0xD1C)) {              // 1BEF (equal -> 1C83)
                wr8n(0xD1C, cl);                  // 1BF9
                nat_out_tail_13DE(0x04, 0x01, cl);// 1BFE bx=0x104; call 13DF
            }
        } else {
            cl = (uint8_t)(cl & (uint8_t)~dl);    // 1C07 not dl; and cl,dl
            if (cl != rd8n(0xD1C)) {              // 1C0B (equal -> 1C83)
                wr8n(0xD1C, cl);                  // 1C12
                nat_out_tail_13DE(0x04, 0x01, cl);// 1C17 bx=0x104; call 13DF
                // 1C1E..1C80: choke the released pair voice of this 4-op set
                v2_ailnat_opl_op_write_13A8(rd8n((uint16_t)(di + 0x5FB)), 0x80, 0x0F);
                v2_ailnat_opl_op_write_13A8(rd8n((uint16_t)(di + 0x60D)), 0x80, 0x0F);
                v2_ailnat_opl_voice_write_13C4(rd8n((uint16_t)(di + 0x5E9)), 0xB0, 0x00);
            }
        }
    }
    // 1C83: [bp-0x10]!=0 -> pass A first; the pass itself restores the mask
    if (perc) nat_upd_pass_1C8C(si, 1, vel_scale);
    nat_upd_pass_1C8C(si, 0, vel_scale);          // 1D8C pass B, 1D76 -> exit
}

// One chain pass. passA=1: snapshot 1C8C (voice+3, modified layer, mask saved
// to [bp-0x12] at 1D2C and restored at 1D84). passA=0: snapshot 1D8C.
static void nat_upd_pass_1C8C(uint16_t si, int passA, uint8_t vel_scale) {
    uint16_t si2 = (uint16_t)(si * 2);            // mov bx,si; [bx+si+..] word rows
    uint16_t voice = passA ? (uint16_t)(rd8n((uint16_t)(si + 0xDBD)) + 3)   // 1C90..1C95
                           : (uint16_t)rd8n((uint16_t)(si + 0xDBD));        // 1D90
    uint16_t op1 = rd8n((uint16_t)(voice + 0x547));                 // [bp-0x4]
    uint16_t op2 = rd8n((uint16_t)(voice + 0x559));                 // [bp-0x6]
    uint16_t w16_am  = rd16n((uint16_t)(si2 + (passA ? 0x117D : 0x0F9D)));  // [bp-0x16]
    uint16_t w18_am  = rd16n((uint16_t)(si2 + (passA ? 0x1155 : 0x0F75)));  // [bp-0x18]
    uint8_t  b1A_fl  = rd8n((uint16_t)(si + (passA ? 0x10B5 : 0x0EC1)));    // [bp-0x1A]
    uint8_t  b1C_fl  = rd8n((uint16_t)(si + (passA ? 0x10C9 : 0x0ED5)));    // [bp-0x1C]
    uint16_t w1E_tl  = rd16n((uint16_t)(si2 + (passA ? 0x11CD : 0x103D)));  // [bp-0x1E]
    uint16_t w20_tl  = rd16n((uint16_t)(si2 + (passA ? 0x11A5 : 0x1015)));  // [bp-0x20]
    uint8_t  b22_ksl = rd8n((uint16_t)(si + (passA ? 0x108D : 0x0E99)));    // [bp-0x22]
    uint8_t  b24_ksl = rd8n((uint16_t)(si + (passA ? 0x10A1 : 0x0EAD)));    // [bp-0x24]
    uint8_t  b26_ad  = rd8n((uint16_t)(si + (passA ? 0x10DD : 0x0EE9)));    // [bp-0x26]
    uint8_t  b28_ad  = rd8n((uint16_t)(si + (passA ? 0x10F1 : 0x0EFD)));    // [bp-0x28]
    uint8_t  b2A_sr  = rd8n((uint16_t)(si + (passA ? 0x1105 : 0x0F11)));    // [bp-0x2A]
    uint8_t  b2C_sr  = rd8n((uint16_t)(si + (passA ? 0x1119 : 0x0F25)));    // [bp-0x2C]
    uint16_t w30_fb  = passA ? 0 : rd16n((uint16_t)(si2 + 0x0FC5));         // [bp-0x30]
    uint16_t w2E_ws  = rd16n((uint16_t)(si2 + (passA ? 0x112D : 0x0F4D)));  // [bp-0x2E]
    uint8_t  b32_st  = passA ? (uint8_t)(rd8n((uint16_t)(si + 0xE85)) >> 1) // 1D1A shr al,1
                             : rd8n((uint16_t)(si + 0xE85));                // [bp-0x32]
    uint8_t  b14_vf  = rd8n((uint16_t)(si + (passA ? 0x11F5 : 0x0F39)));    // [bp-0x14]
    uint8_t saved_mask = 0;
    if (passA) saved_mask = rd8n((uint16_t)(si + 0xE49));           // 1D2C [bp-0x12]

    // ---- 1D34: the one-shot dirty chain (each test re-reads the mask) ----
    if (rd8n((uint16_t)(si + 0xE49)) & 0x80) {    // -> 1E44: reg 0x20 pair
        uint16_t di = (uint16_t)(rd16n((uint16_t)(si + 0xDD1)) & 0xF);
        // 1E4C..1E58: tremolo gate — SIGNED compare (jge) of the channel ctl
        uint16_t trem = ((int8_t)rd8n((uint16_t)(di + 0x1259)) >= 0x40) ? 0x40 : 0x00;
        uint8_t v1 = (uint8_t)((uint8_t)((uint16_t)(w16_am >> 4) >> 8) | trem | b1A_fl);
        v2_ailnat_opl_op_write_13A8((uint8_t)op1, 0x20, v1);        // 1E78
        uint8_t v2 = (uint8_t)((uint8_t)((uint16_t)(w18_am >> 4) >> 8) | trem | b1C_fl);
        v2_ailnat_opl_op_write_13A8((uint8_t)op2, 0x20, v2);        // 1E9B
        wr8n((uint16_t)(si + 0xE49), (uint8_t)(rd8n((uint16_t)(si + 0xE49)) & 0x7F));
    }                                             // 1EA7 jmp 1D3F (next test)
    if (rd8n((uint16_t)(si + 0xE49)) & 0x40) {    // -> 1EAA: reg 0x40 pair (TL)
        uint8_t bl = (uint8_t)((uint16_t)(w1E_tl >> 2) >> 8);       // shr bx,2; mov bl,bh
        if (b14_vf & 0x1)                          // 1EB3: velocity-scaled op1
            bl = (uint8_t)((uint16_t)(bl * vel_scale) / 0x7F);      // mul [bp-8]; div 0x7F
        bl = (uint8_t)((uint8_t)~bl & 0x3F);       // 1EC4 not bl; and bl,0x3F
        bl = (uint8_t)(bl | b22_ksl);              // or bl,[bp-0x22]
        v2_ailnat_opl_op_write_13A8((uint8_t)op1, 0x40, bl);        // 1EDB
        uint8_t b2 = (uint8_t)((uint16_t)(w20_tl >> 2) >> 8);
        if (b14_vf & 0x2)
            b2 = (uint8_t)((uint16_t)(b2 * vel_scale) / 0x7F);
        b2 = (uint8_t)((uint8_t)~b2 & 0x3F);
        b2 = (uint8_t)(b2 | b24_ksl);
        v2_ailnat_opl_op_write_13A8((uint8_t)op2, 0x40, b2);        // 1F12
        wr8n((uint16_t)(si + 0xE49), (uint8_t)(rd8n((uint16_t)(si + 0xE49)) & 0xBF));
    }                                             // 1F1E jmp 1D4A
    if (rd8n((uint16_t)(si + 0xE49)) & 0x20) {    // -> 1F21: regs 0x60/0x80 (AD/SR)
        v2_ailnat_opl_op_write_13A8((uint8_t)op1, 0x60, b26_ad);    // 1F33
        v2_ailnat_opl_op_write_13A8((uint8_t)op2, 0x60, b28_ad);    // 1F4B
        v2_ailnat_opl_op_write_13A8((uint8_t)op1, 0x80, b2A_sr);    // 1F63
        v2_ailnat_opl_op_write_13A8((uint8_t)op2, 0x80, b2C_sr);    // 1F7B
        wr8n((uint16_t)(si + 0xE49), (uint8_t)(rd8n((uint16_t)(si + 0xE49)) & 0xDF));
    }                                             // 1F87 jmp 1D55
    if (rd8n((uint16_t)(si + 0xE49)) & 0x10) {    // -> 1F8A: reg 0xE0 pair (waveform)
        v2_ailnat_opl_op_write_13A8((uint8_t)op2, 0xE0, (uint8_t)w2E_ws);        // lo -> op2
        v2_ailnat_opl_op_write_13A8((uint8_t)op1, 0xE0, (uint8_t)(w2E_ws >> 8)); // hi -> op1
        wr8n((uint16_t)(si + 0xE49), (uint8_t)(rd8n((uint16_t)(si + 0xE49)) & 0xEF));
    }                                             // 1FC0 jmp 1D60
    if (rd8n((uint16_t)(si + 0xE49)) & 0x08) {    // -> 1FC3: reg 0xC0 (fb/conn + pan)
        uint8_t al = (uint8_t)((uint8_t)((uint16_t)(w30_fb >> 4) >> 8) & 0x0E);
        al = (uint8_t)(al | (uint8_t)(b32_st & 0x1) | 0x30);        // conn bit + both LR bits
        uint16_t di = (uint16_t)(rd16n((uint16_t)(si + 0xDD1)) & 0xF);
        uint8_t pan = rd8n((uint16_t)(di + 0x1219));                // channel pan ctl
        if (pan <= 0x1B)      al = (uint8_t)(al & 0xEF);            // 1FF1 clear bit 4
        else if (pan >= 0x64) al = (uint8_t)(al & 0xDF);            // 1FED clear bit 5
        v2_ailnat_opl_voice_write_13C4(voice, 0xC0, al);            // 2002
        wr8n((uint16_t)(si + 0xE49), (uint8_t)(rd8n((uint16_t)(si + 0xE49)) & 0xF7));
    }                                             // 200E jmp 1D6B
    if (rd8n((uint16_t)(si + 0xE49)) & 0x01) {    // -> 2011: frequency / key state
        if (passA) {                              // 2011 cmp [bp-0x10],1 -> 214C
            wr8n((uint16_t)(si + 0xE49), (uint8_t)(rd8n((uint16_t)(si + 0xE49)) & 0xFE));
        } else if (rd8n((uint16_t)(si + 0xDA9)) == 2) {   // 201A..2023 -> 2104
            // held-slot form: fnum word straight from the slot row
            nat_freq_tail_2112(si, voice,
                (uint16_t)(rd16n((uint16_t)(si2 + 0x1065)) >> 6));  // 2106..210F
        } else if (!(rd8n((uint16_t)(si + 0xE71)) & 0x20)) {  // 2026: key-off
            uint8_t b0 = (uint8_t)(rd8n((uint16_t)(si + 0xE5D)) & 0xDF);
            v2_ailnat_opl_voice_write_13C4(voice, 0xB0, b0);  // 2044 (0xE5D NOT updated)
            wr8n((uint16_t)(si + 0xE49), (uint8_t)(rd8n((uint16_t)(si + 0xE49)) & 0xFE));
        } else {                                  // 204D..20FF: pitch computation
            uint16_t ch = rd8n((uint16_t)(si + 0xDD1));       // 204D byte form! bh=0
            int16_t ax = (int16_t)(uint16_t)(((uint16_t)rd8n((uint16_t)(ch + 0x1239)) << 7)
                                             | rd8n((uint16_t)(ch + 0x1229)));
            ax = (int16_t)(ax - 0x2000);          // pitch wheel, centre 0x2000
            ax = (int16_t)(ax >> 5);              // sar ax,5
            ax = (int16_t)(ax * 12);              // imul cx(12)
            int16_t note = (int16_t)rd8n((uint16_t)(si + 0xDE5));          // bl; bh=0
            note = (int16_t)(note + (int8_t)rd8n((uint16_t)(si + 0xE0D))); // cbw; add bx,ax
            note = (int16_t)(note - 0x18);        // 2086
            do { note = (int16_t)(note + 0x0C); } while (note < 0);     // 2089 octave-up
            note = (int16_t)(note + 0x0C);        // 2091
            do { note = (int16_t)(note - 0x0C); } while (note > 0x5F);  // 2094 octave-down
            ax = (int16_t)((uint16_t)ax + ((uint16_t)(note & 0xFF) << 8)); // 209C add ah,bl
            ax = (int16_t)(ax + 8);               // rounding
            ax = (int16_t)(ax >> 4);              // sar ax,4
            ax = (int16_t)(ax - 0xC0);            // 20A6
            do { ax = (int16_t)(ax + 0xC0); } while (ax < 0);           // 20A9
            ax = (int16_t)(ax + 0xC0);            // 20B1
            do { ax = (int16_t)(ax - 0xC0); } while (ax > 0x5FF);       // 20B4
            uint16_t din = (uint16_t)((uint16_t)ax >> 4);       // 20BC shr di,4 (logical)
            uint16_t oct = rd8n((uint16_t)(din + 0x2ED));       // octave LUT
            uint16_t fidx = (uint16_t)((uint16_t)(oct << 5)
                                       + (((uint16_t)ax << 1) & 0x1F)); // 20CE..20D8
            uint16_t fn = rd16n((uint16_t)(fidx + 0x10D));      // fnum LUT (signed word)
            uint8_t blk = (uint8_t)(rd8n((uint16_t)(din + 0x28D)) - 1); // block LUT; dec bl
            if ((int16_t)fn < 0) blk = (uint8_t)(blk + 1);      // 20E8 or ax,ax; jge
            if ((int8_t)blk < 0) {                              // 20EE or bl,bl; jge
                blk = (uint8_t)(blk + 1);
                fn = (uint16_t)((int16_t)fn >> 1);              // 20F4 sar ax,1
            }
            blk = (uint8_t)(blk << 2);            // 20F6 shl bl,1 x2
            nat_freq_tail_2112(si, voice,
                (uint16_t)((fn & 0x03FF) | ((uint16_t)blk << 8))); // 20FA and ah,3; or ah,bl
        }
    }                                             // -> 1D76
    if (passA)                                    // 1D7F..1D87: [bp-0x10]=0,
        wr8n((uint16_t)(si + 0xE49), saved_mask); // restore the mask for pass B
}

// ---------------------------------------------------------------------------
// 1B11: voice release — key the slot off, free its voice and bookkeeping.
// ---------------------------------------------------------------------------
extern "C" void v2_ailnat_voice_release_1B11(uint16_t si) {
    if (rd8n((uint16_t)(si + 0xDBD)) == 0xFF)     // 1B1A -> 1B77
        return;
    wr8n((uint16_t)(si + 0xE71), (uint8_t)(rd8n((uint16_t)(si + 0xE71)) & 0xDF)); // key off
    wr8n((uint16_t)(si + 0xE49), (uint8_t)(rd8n((uint16_t)(si + 0xE49)) | 0x01)); // freq group
    v2_ailnat_update_voice_1B7C(si);              // 1B30 -> B0 key-off write
    uint16_t ch = rd8n((uint16_t)(si + 0xDD1));   // 1B36 bh=0; mov bl,..
    wr8n((uint16_t)(ch + 0x1339), (uint8_t)(rd8n((uint16_t)(ch + 0x1339)) - 1));
    uint16_t v = rd8n((uint16_t)(si + 0xDBD));    // 1B42
    if (rd8n((uint16_t)(si + 0xDA9)) == 3)        // percussion: free the pair mark
        wr8n((uint16_t)(v + 0x134C), 0xFF);
    wr8n((uint16_t)(v + 0x1349), 0xFF);           // free the voice
    wr8n((uint16_t)(si + 0xDBD), 0xFF);
    uint8_t m = rd8n((uint16_t)(si + 0xDA9));     // 1B61..1B6F
    if (m == 3 || m == 0)
        wr8n((uint16_t)(si + 0xD95), 0);          // slot goes inactive
}

// ---------------------------------------------------------------------------
// 2155: voice steal — score the active slots, then move voices from the
// lowest-scored voiced slots to the highest-scored voiceless one.
// NOTE (from the listing): the victim locals [bp-0x4]/[bp-0x6]/[bp-0x8] are
// UNINITIALIZED stack words in the original; every judged domain must make
// the selection loop assign them before use (>=1 active voiced slot, and a
// 4-op-capable one when a percussion requester exists).
// ---------------------------------------------------------------------------
extern "C" void v2_ailnat_steal_2155(void) {
    uint16_t count = 0;                           // [bp-0x2]
    // ---- 2163..21A4: effective score per active slot -> cs:[si*2+0x135B]
    for (uint16_t si = 0; si < 0x14; si++) {
        if (rd8n((uint16_t)(si + 0xD95)) == 0) continue;
        count = (uint16_t)(count + 1);
        uint16_t di = (uint16_t)(rd16n((uint16_t)(si + 0xDD1)) & 0xF);
        uint16_t ax = 0xFFFF;
        if ((int8_t)rd8n((uint16_t)(di + 0x1279)) < 0x40)     // signed (jge)
            ax = rd16n((uint16_t)(si * 2 + 0xFED));           // slot priority word
        uint16_t cx = rd8n((uint16_t)(di + 0x1339));          // channel voice count
        ax = (ax >= cx) ? (uint16_t)(ax - cx) : 0;            // sub; jae / ax=0
        wr16n((uint16_t)(si * 2 + 0x135B), ax);
    }
    for (;;) {
        // ---- 21A6..21F6: pick requester (max score, voiceless) and victims
        uint16_t ax = 0, dx = 0xFFFF, cx = 0xFFFF;
        uint16_t req_si = 0, victim_si = 0, victim4_si = 0;   // [bp-6]/[bp-4]/[bp-8]
        for (uint16_t si = 0; si < 0x14; si++) {
            if (rd8n((uint16_t)(si + 0xD95)) == 0) continue;
            uint16_t di = rd16n((uint16_t)(si * 2 + 0x135B));
            uint8_t bl = rd8n((uint16_t)(si + 0xDBD));
            if (bl == 0xFF) {                     // voiceless: track the max
                if (di >= ax) { ax = di; req_si = si; }       // jb skips
                continue;
            }
            if (rd8n((uint16_t)(bl + 0x5D7)) != 0             // 4-op capable voice
                && di <= cx) { cx = di; victim4_si = si; }    // ja skips
            if (di <= dx) { dx = di; victim_si = si; }        // ja skips
        }
        // ---- 21F8..2204: stop conditions
        if (ax < dx) return;                      // cmp ax,dx; jae — else exit
        if (ax == 0) return;
        // ---- 2207..2243: percussion requester steals a 4-op-capable voice
        uint16_t si = victim_si;
        uint16_t bx = req_si;
        if (rd8n((uint16_t)(bx + 0xDA9)) == 3) {
            si = victim4_si;
            if (rd8n((uint16_t)(si + 0xDA9)) != 3) {
                // melodic victim on a 4-op voice: also release the slot that
                // holds the mate voice cs:[si+0x5E9] (slot-indexed, as listed)
                uint8_t al = rd8n((uint16_t)(si + 0x5E9));
                for (uint16_t di = 0; di < 0x14; di++) {
                    if (rd8n((uint16_t)(di + 0xD95)) == 0) continue;
                    if (rd8n((uint16_t)(di + 0xDBD)) != al) continue;
                    v2_ailnat_voice_release_1B11(di);         // 223F
                    break;                        // je 2245 after the call
                }
            }
        }
        // ---- 2245..2287: move the voice to the requester and re-key it
        uint16_t v = rd8n((uint16_t)(si + 0xDBD));            // bh=0
        v2_ailnat_voice_release_1B11(si);         // 224F
        si = req_si;                              // 2256
        wr8n((uint16_t)(si + 0xDBD), (uint8_t)v);
        uint16_t ch = rd8n((uint16_t)(si + 0xDD1));           // mov bl,..
        wr8n((uint16_t)(ch + 0x1339), (uint8_t)(rd8n((uint16_t)(ch + 0x1339)) + 1));
        wr8n((uint16_t)(v + 0x1349), (uint8_t)ch);
        if (rd8n((uint16_t)(si + 0xDA9)) == 3)                // pair mark for perc
            wr8n((uint16_t)(v + 0x134C), (uint8_t)ch);
        wr8n((uint16_t)(si + 0xE49), 0xF9);       // all dirty groups but 0x04|0x02
        v2_ailnat_update_voice_1B7C(si);
        count = (uint16_t)(count - 1);            // 228A dec; je exit; loop 21A6
        if (count == 0) return;
    }
}

// ---------------------------------------------------------------------------
// 1A5A: voice assign (note-on) — round-robin allocation; melodic voices scan
// the 18-voice ring (cursor cs:0xD18), percussion the 6-entry 4-op map
// cs:[0x631] (cursor cs:0xD1A) requiring voice AND pair (+3) free. On a full
// ring the steal path 2155 runs instead.
// ---------------------------------------------------------------------------
extern "C" void v2_ailnat_voice_assign_1A5A(uint16_t si) {
    uint16_t bx;
    if (rd8n((uint16_t)(si + 0xDA9)) != 3) {      // 1A63
        uint16_t dx = 0;
        bx = rd16n(0xD18);                        // melodic ring cursor
        for (;; dx++) {
            if (dx == 0x12) { v2_ailnat_steal_2155(); return; }   // 1AB8
            bx = (uint16_t)(bx + 1);
            if (bx == 0x12) bx = 0;
            wr16n(0xD18, bx);
            if (rd8n((uint16_t)(bx + 0x1349)) == 0xFF) break;     // free voice
        }
    } else {                                      // 1AC1: percussion
        uint16_t dx = 0;
        uint16_t di = rd16n(0xD1A);               // percussion map cursor
        for (;; dx++) {
            if (dx == 0x6) { v2_ailnat_steal_2155(); return; }    // -> 1AB8
            di = (uint16_t)(di + 1);
            if (di == 0x6) di = 0;
            wr16n(0xD1A, di);
            bx = rd8n((uint16_t)(di + 0x631));    // mapped 4-op voice (bh=0)
            if (rd8n((uint16_t)(bx + 0x1349)) != 0xFF) continue;
            if (rd8n((uint16_t)(bx + 0x134C)) != 0xFF) continue;  // pair busy
            break;
        }
        wr8n((uint16_t)(si + 0xDBD), (uint8_t)bx);
        uint16_t ch = rd8n((uint16_t)(si + 0xDD1));
        wr8n((uint16_t)(ch + 0x1339), (uint8_t)(rd8n((uint16_t)(ch + 0x1339)) + 1));
        wr8n((uint16_t)(bx + 0x1349), (uint8_t)ch);
        wr8n((uint16_t)(bx + 0x134C), (uint8_t)ch);               // 1B0A pair mark
        wr8n((uint16_t)(si + 0xE49), 0xF9);       // 1AA5 (shared tail)
        v2_ailnat_update_voice_1B7C(si);
        return;
    }
    wr8n((uint16_t)(si + 0xDBD), (uint8_t)bx);    // 1A8F melodic tail
    uint16_t ch = rd8n((uint16_t)(si + 0xDD1));   // mov bl,..
    wr8n((uint16_t)(ch + 0x1339), (uint8_t)(rd8n((uint16_t)(ch + 0x1339)) + 1));
    wr8n((uint16_t)(bx + 0x1349), (uint8_t)ch);
    wr8n((uint16_t)(si + 0xE49), 0xF9);           // 1AA5
    v2_ailnat_update_voice_1B7C(si);
}

// ---------------------------------------------------------------------------
// Far memory (timbre cache, sequence state) lives OUTSIDE the blob — the
// native port reaches it through a read hook (seg:off -> byte).
// ---------------------------------------------------------------------------
typedef uint8_t (*nat_far_rd_fn)(uint16_t seg, uint16_t off);
static nat_far_rd_fn g_nat_far_rd = nullptr;
extern "C" void v2_ailnat_set_far_read(nat_far_rd_fn f) { g_nat_far_rd = f; }

// Built-in far resolver for the LIVE integration: paragraph ranges into
// host memory, the same shape as the interpreter's segment map. Explicit
// hooks (the unit judge) take precedence when installed.
static struct { uint16_t para; uint8_t* ptr; uint32_t size; } g_nat_map[8];
static int g_nat_map_n = 0;
extern "C" void v2_ailnat_map_segment(uint16_t para, uint8_t* ptr, uint32_t size) {
    if (g_nat_map_n < 8) g_nat_map[g_nat_map_n++] = { para, ptr, size };
}
static uint8_t* nat_map_mem(uint16_t seg, uint16_t off) {
    for (int i = 0; i < g_nat_map_n; i++) {
        uint32_t d = (uint32_t)(uint16_t)(seg - g_nat_map[i].para) << 4;
        if (seg >= g_nat_map[i].para && d + off < g_nat_map[i].size)
            return g_nat_map[i].ptr + d + off;
    }
    return nullptr;
}
static inline uint8_t  fr8(uint16_t seg, uint16_t off) {
    if (g_nat_far_rd) return g_nat_far_rd(seg, off);
    uint8_t* p = nat_map_mem(seg, off);
    return p ? *p : 0;
}
static inline uint16_t fr16(uint16_t seg, uint16_t off) {
    return (uint16_t)(fr8(seg, off) | (fr8(seg, (uint16_t)(off + 1)) << 8));
}

// Far writes (the sequence state struct is game memory).
typedef void (*nat_far_wr_fn)(uint16_t seg, uint16_t off, uint8_t val);
static nat_far_wr_fn g_nat_far_wr = nullptr;
extern "C" void v2_ailnat_set_far_write(nat_far_wr_fn f) { g_nat_far_wr = f; }
void v2_ail_interp_ds_mirror(const uint8_t* p, uint8_t v);   // stage-4 bridge
static inline void fw8(uint16_t seg, uint16_t off, uint8_t v) {
    if (g_nat_far_wr) { g_nat_far_wr(seg, off, v); return; }
    uint8_t* p = nat_map_mem(seg, off);
    if (p) {
        *p = v;
        // stage-4 bridge: far stores landing in the canonical shadow DS
        // must mirror into the evac carrier, exactly like the interpreter's
        // wr8 does (pointer-range test inside; other segments fall through).
        v2_ail_interp_ds_mirror(p, v);
    }
}
static inline void fw16(uint16_t seg, uint16_t off, uint16_t v) {
    fw8(seg, off, (uint8_t)v); fw8(seg, (uint16_t)(off + 1), (uint8_t)(v >> 8));
}

// XMIDI callback (controller 0x77) — the blob far-calls cs:0x2957 with
// ds=cs:0x2955; the native port routes it through a hook.
typedef void (*nat_cb77_fn)(uint16_t seq_handle, uint16_t value);
static nat_cb77_fn g_nat_cb77 = nullptr;
extern "C" void v2_ailnat_set_cb77(nat_cb77_fn f) { g_nat_cb77 = f; }

// ---------------------------------------------------------------------------
// 2477: note-off scan — every active slot holding (channel, note).
// ---------------------------------------------------------------------------
extern "C" void v2_ailnat_note_off_2477(uint16_t ch, uint16_t note) {
    uint8_t al = (uint8_t)note;                   // 2480 mov al,[bp+8]
    uint8_t bl = (uint8_t)ch;                     // 2483 mov bl,[bp+6]
    for (uint16_t si = 0; si < 0x14; si++) {      // 2486 inc si; cmp 0x14
        if (rd8n((uint16_t)(si + 0xD95)) != 1) continue;
        if (rd8n((uint16_t)(si + 0xDF9)) != al) continue;
        if (rd8n((uint16_t)(si + 0xDD1)) != bl) continue;
        // 24A2 bh=0 — sustain pedal check is SIGNED (jge)
        if ((int8_t)rd8n((uint16_t)(bl + 0x1269)) >= 0x40) {
            wr8n((uint16_t)(si + 0xE35), 1);      // 24D7 held by the pedal
        } else if (rd8n((uint16_t)(si + 0xDA9)) == 3 ||
                   rd8n((uint16_t)(si + 0xDA9)) == 0) {   // 24AC/24B4
            v2_ailnat_voice_release_1B11(si);     // 24BE
            wr8n((uint16_t)(si + 0xD95), 0);
        } else {
            wr16n((uint16_t)(si * 2 + 0xD6D), 1); // 24CE release-pending
        }
    }
}

// ---------------------------------------------------------------------------
// 25EE: sustain-pedal release — key off every pedal-held slot of the channel.
// (The listing passes the channel BOTH on the stack and in DI — the 2477 call
// pushes the caller's DI, which every call site loads with the channel.)
// ---------------------------------------------------------------------------
extern "C" void v2_ailnat_sustain_release_25EE(uint16_t ch) {
    for (uint16_t si = 0; si < 0x14; si++) {
        if (rd8n((uint16_t)(si + 0xD95)) == 0) continue;
        if (rd8n((uint16_t)(si + 0xDD1)) != (uint8_t)ch) continue;   // 2602
        if (rd8n((uint16_t)(si + 0xE35)) == 0) continue;             // 2609
        // 2611 push WORD cs:[si+0xDE5]; push di -> 2477(di, note word)
        v2_ailnat_note_off_2477(ch, rd16n((uint16_t)(si + 0xDE5)));
    }
}

// ---------------------------------------------------------------------------
// 2384: melodic timbre install — map the far timbre record into the BASE
// parameter layer (cs:0xE99..0x108C rows) of slot si.
// ---------------------------------------------------------------------------
extern "C" void v2_ailnat_timbre_mel_2384(uint16_t si) {
    uint16_t bx = (uint16_t)(si * 2);             // 238D shl bx,1
    uint16_t di = rd16n((uint16_t)(bx + 0xD1D));  // timbre far ptr (per slot)
    uint16_t sg = rd16n((uint16_t)(bx + 0xD45));
    wr8n((uint16_t)(si + 0xE71), 0x20);           // 239B key/sustain image
    wr8n((uint16_t)(si + 0xDA9), 0x00);           // melodic mode
    wr16n((uint16_t)(bx + 0xD6D), 0xFFFF);        // 23A7 release-pending off
    wr16n((uint16_t)(bx + 0xFED), 0x7FFF);        // 23AE priority word
    uint8_t t8 = fr8(sg, (uint16_t)(di + 0x8));   // 23B7 ah=[di+8]
    wr8n((uint16_t)(si + 0xE85), (uint8_t)(t8 & 0x1));               // conn bit
    wr16n((uint16_t)(bx + 0xFC5), (uint16_t)((uint16_t)(t8 << 8) << 4)); // fb/conn word
    uint8_t k1 = fr8(sg, (uint16_t)(di + 0x4));   // 23CF op1 KSL/TL
    wr8n((uint16_t)(si + 0xE99), (uint8_t)(k1 & 0xC0));
    wr16n((uint16_t)(bx + 0x103D),
          (uint16_t)((uint16_t)((uint8_t)(~k1 & 0x3F) << 8) << 2));  // 23E1 shl x2
    uint8_t k2 = fr8(sg, (uint16_t)(di + 0xA));   // 23EC op2 KSL/TL
    wr8n((uint16_t)(si + 0xEAD), (uint8_t)(k2 & 0xC0));
    wr16n((uint16_t)(bx + 0x1015),
          (uint16_t)((uint16_t)((uint8_t)(~k2 & 0x3F) << 8) << 2));
    uint8_t m1 = fr8(sg, (uint16_t)(di + 0x3));   // 2407 op1 AM/VIB/mult
    wr8n((uint16_t)(si + 0xEC1), (uint8_t)(m1 & 0xF0));
    wr16n((uint16_t)(bx + 0xF9D), (uint16_t)((uint16_t)(m1 << 8) << 4));
    uint8_t m2 = fr8(sg, (uint16_t)(di + 0x9));   // 241E op2
    wr8n((uint16_t)(si + 0xED5), (uint8_t)(m2 & 0xF0));
    wr16n((uint16_t)(bx + 0xF75), (uint16_t)((uint16_t)(m2 << 8) << 4));
    wr8n((uint16_t)(si + 0xEE9), fr8(sg, (uint16_t)(di + 0x5)));  // AD op1
    wr8n((uint16_t)(si + 0xF11), fr8(sg, (uint16_t)(di + 0x6)));  // SR op1
    wr8n((uint16_t)(si + 0xEFD), fr8(sg, (uint16_t)(di + 0xB)));  // AD op2
    wr8n((uint16_t)(si + 0xF25), fr8(sg, (uint16_t)(di + 0xC)));  // SR op2
    wr16n((uint16_t)(bx + 0xF4D),                 // 2455 waveforms: lo=[di+D], hi=[di+7]
          (uint16_t)(fr8(sg, (uint16_t)(di + 0xD)) |
                     (fr8(sg, (uint16_t)(di + 0x7)) << 8)));
    wr8n((uint16_t)(si + 0xF39),                  // 2460 vol-follow flags
         (uint8_t)(rd8n((uint16_t)(si + 0xE85)) | 0x2));
    wr8n((uint16_t)(si + 0xE49), 0xF9);           // 246C all dirty groups
}

// ---------------------------------------------------------------------------
// 2299: percussion timbre install — 2384 first (base layer), then the 4-op
// MODIFIED layer (cs:0x108D..0x1208 rows) from the record's second half.
// ---------------------------------------------------------------------------
extern "C" void v2_ailnat_timbre_perc_2299(uint16_t si) {
    v2_ailnat_timbre_mel_2384(si);                // 22A3
    uint16_t bx = (uint16_t)(si * 2);
    uint16_t di = rd16n((uint16_t)(bx + 0xD1D));
    uint16_t sg = rd16n((uint16_t)(bx + 0xD45));
    wr8n((uint16_t)(si + 0xDA9), 0x3);            // 22BA percussion mode
    uint8_t c8 = fr8(sg, (uint16_t)(di + 0x8));   // 22C0 [di+8]&0x80 >> 6
    wr8n((uint16_t)(si + 0xE85),
         (uint8_t)(rd8n((uint16_t)(si + 0xE85)) | (uint8_t)((c8 & 0x80) >> 6)));
    { // 22CF vol-follow flags via LUT rows cs:0x637/0x63B indexed by 0xE85
        uint16_t e = rd8n((uint16_t)(si + 0xE85));
        wr8n((uint16_t)(si + 0xF39),  rd8n((uint16_t)(e + 0x637)));
        wr8n((uint16_t)(si + 0x11F5), rd8n((uint16_t)(e + 0x63B)));
    }
    uint8_t k1 = fr8(sg, (uint16_t)(di + 0xF));   // 22EE op1 KSL/TL (layer A)
    wr8n((uint16_t)(si + 0x108D), (uint8_t)(k1 & 0xC0));
    wr16n((uint16_t)(bx + 0x11CD),
          (uint16_t)((uint16_t)((uint8_t)(~k1 & 0x3F) << 8) << 2));
    uint8_t k2 = fr8(sg, (uint16_t)(di + 0x15));  // 230B op2 KSL/TL
    wr8n((uint16_t)(si + 0x10A1), (uint8_t)(k2 & 0xC0));
    wr16n((uint16_t)(bx + 0x11A5),
          (uint16_t)((uint16_t)((uint8_t)(~k2 & 0x3F) << 8) << 2));
    uint8_t m1 = fr8(sg, (uint16_t)(di + 0xE));   // 2326 op1 AM/VIB/mult
    wr8n((uint16_t)(si + 0x10B5), (uint8_t)(m1 & 0xF0));
    wr16n((uint16_t)(bx + 0x117D), (uint16_t)((uint16_t)(m1 << 8) << 4));
    uint8_t m2 = fr8(sg, (uint16_t)(di + 0x14));  // 233D op2
    wr8n((uint16_t)(si + 0x10C9), (uint8_t)(m2 & 0xF0));
    wr16n((uint16_t)(bx + 0x1155), (uint16_t)((uint16_t)(m2 << 8) << 4));
    wr8n((uint16_t)(si + 0x10DD), fr8(sg, (uint16_t)(di + 0x10)));  // AD op1
    wr8n((uint16_t)(si + 0x1105), fr8(sg, (uint16_t)(di + 0x11)));  // SR op1
    wr8n((uint16_t)(si + 0x10F1), fr8(sg, (uint16_t)(di + 0x16)));  // AD op2
    wr8n((uint16_t)(si + 0x1119), fr8(sg, (uint16_t)(di + 0x17)));  // SR op2
    wr16n((uint16_t)(bx + 0x112D),                // 2374 waveforms lo=[di+18], hi=[di+12]
          (uint16_t)(fr8(sg, (uint16_t)(di + 0x18)) |
                     (fr8(sg, (uint16_t)(di + 0x12)) << 8)));
}

// ---------------------------------------------------------------------------
// 24E4: note-on — resolve the channel's timbre, claim a free slot, install
// the timbre and assign a voice.
// ---------------------------------------------------------------------------
extern "C" void v2_ailnat_note_on_24E4(uint16_t ch, uint16_t note, uint16_t vel) {
    uint16_t bx = rd8n((uint16_t)(ch + 0x1289));  // 24EF channel patch -> timbre
    if (ch == 9) {                                // 24F4 percussion channel
        bx = rd8n((uint16_t)(note + 0x12B9));     // drum note -> timbre map
        if ((uint8_t)bx == 0xFF) {
            // 2506 search loaded timbres for bank 0x7F, patch = note
            uint16_t r = v2_ailnat_find_slot_15E9((uint16_t)(0x7F00 | (note & 0xFF)));
            bx = r;
            wr8n((uint16_t)(note + 0x12B9), (uint8_t)r);
        }
    }
    if ((uint8_t)bx == 0xFF) return;              // 251A no timbre
    bx = (uint16_t)(bx << 1);                     // 2522
    uint16_t doff = rd16n(0xD0B);                 // 2524 lds di,cs:0xD0B
    uint16_t dseg = rd16n(0xD0D);
    doff = (uint16_t)(doff + rd16n((uint16_t)(bx + 0x94B)));   // 2529
    // 252E..2548: LRU stamp dword cs:0x647 -> per-timbre cs:[bx+0x64B/0x7CB]
    uint32_t lru = (uint32_t)(rd16n(0x647) | ((uint32_t)rd16n(0x649) << 16)) + 1;
    wr16n(0x647, (uint16_t)lru);
    wr16n(0x649, (uint16_t)(lru >> 16));
    wr16n((uint16_t)(bx + 0x64B), (uint16_t)lru);
    wr16n((uint16_t)(bx + 0x7CB), (uint16_t)(lru >> 16));
    uint16_t si = 0;                              // 254D free-slot scan
    while (rd8n((uint16_t)(si + 0xD95)) != 0) {
        si = (uint16_t)(si + 1);
        if (si == 0x14) return;                   // 255E no free slot
    }
    wr8n((uint16_t)(si + 0xDD1), (uint8_t)ch);    // 2564
    wr8n((uint16_t)(si + 0xDF9), (uint8_t)note);  // 256C
    uint8_t al = 0;                               // 2571
    uint8_t cl = fr8(dseg, (uint16_t)(doff + 0x2));   // timbre transpose byte
    if (ch != 9) { al = cl; cl = (uint8_t)note; } // 2579..2580
    wr8n((uint16_t)(si + 0xDE5), cl);             // note (or fixed drum note)
    wr8n((uint16_t)(si + 0xE0D), al);             // transpose (0 for drums)
    // 258C velocity curve: al=[bp+0xA]>>3; xlat cs:[0x537]
    wr8n((uint16_t)(si + 0xE21),
         rd8n((uint16_t)(0x537 + (uint8_t)((uint8_t)vel >> 3))));
    wr16n((uint16_t)(si * 2 + 0xD1D), doff);      // 25A3 per-slot timbre far ptr
    wr16n((uint16_t)(si * 2 + 0xD45), dseg);
    wr8n((uint16_t)(si + 0xD95), 1);              // active
    wr8n((uint16_t)(si + 0xE35), 0);              // not pedal-held
    uint16_t tlen = fr16(dseg, doff);             // 25B9 record type word
    if (tlen == 0x19)      v2_ailnat_timbre_perc_2299(si);
    else if (tlen == 0x0E) v2_ailnat_timbre_mel_2384(si);
    else return;                                  // 25D9 unknown record
    wr8n((uint16_t)(si + 0xDBD), 0xFF);           // 25DB
    v2_ailnat_voice_assign_1A5A(si);              // 25E3
}

extern "C" void v2_ailnat_midi_2629(uint16_t status, uint16_t d1, uint16_t d2);

// ---------------------------------------------------------------------------
// 2CA0: flush every pending note of one sequence (note-offs now).
// ---------------------------------------------------------------------------
extern "C" void v2_ailnat_seq_flush_notes_2CA0(uint16_t sg, uint16_t so) {
    for (uint16_t bx = 0; bx < 0x20; bx++) {      // 2CAA..2CE8
        uint8_t al = fr8(sg, (uint16_t)(so + 0x148 + bx));
        if (al == 0xFF) continue;
        fw8(sg, (uint16_t)(so + 0x148 + bx), 0xFF);
        uint8_t note = fr8(sg, (uint16_t)(so + 0x168 + bx));
        uint16_t mch = fr8(sg, (uint16_t)(so + 0x68 + al));   // channel map
        wr8n((uint16_t)(mch + 0x2A1B), (uint8_t)(rd8n((uint16_t)(mch + 0x2A1B)) - 1));
        v2_ailnat_midi_2629((uint16_t)(mch | 0x80), note, 0); // note off
    }
    fw16(sg, (uint16_t)(so + 0x20), 0);           // 2CEA pending count = 0
}

// ---------------------------------------------------------------------------
// 2C1E: fire the pending note-offs of ONE source channel across ALL
// registered sequences (used when a physical channel gets stolen/locked).
// ---------------------------------------------------------------------------
extern "C" void v2_ailnat_kill_channel_notes_2C1E(uint16_t ch) {
    uint16_t left = rd16n(0x294F);                // [bp-0x4]
    if (left == 0) return;                        // jcxz 2C99
    uint16_t di = 0;                              // [bp-0x2]
    for (;;) {
        uint16_t cur = di; di = (uint16_t)(di + 4);
        if (rd16n((uint16_t)(cur + 0x2931)) == 0) continue;   // inactive handle
        uint16_t so = rd16n((uint16_t)(cur + 0x292F));
        uint16_t sg = rd16n((uint16_t)(cur + 0x2931));
        if (fr16(sg, (uint16_t)(so + 0x20)) != 0) {
            for (uint16_t bx = 0; bx < 0x20; bx++) {          // 2C50..2C92
                uint8_t al = fr8(sg, (uint16_t)(so + 0x148 + bx));
                if (al != (uint8_t)ch) continue;              // cmp [bp+6]
                fw8(sg, (uint16_t)(so + 0x148 + bx), 0xFF);
                uint8_t note = fr8(sg, (uint16_t)(so + 0x168 + bx));
                uint16_t mch = fr8(sg, (uint16_t)(so + 0x68 + al));
                wr8n((uint16_t)(mch + 0x2A1B),
                     (uint8_t)(rd8n((uint16_t)(mch + 0x2A1B)) - 1));
                v2_ailnat_midi_2629((uint16_t)(mch | 0x80), note, 0);
                fw16(sg, (uint16_t)(so + 0x20),
                     (uint16_t)(fr16(sg, (uint16_t)(so + 0x20)) - 1));
            }
        }
        left = (uint16_t)(left - 1);              // 2C94 dec; jne
        if (left == 0) return;
    }
}

// ---------------------------------------------------------------------------
// 3E20: allocate the least-busy unlocked physical channel (8..1), first
// avoiding both lock (0x80) and protect (0x40), then lock-only. The winner
// gets pedal-off, its pending notes killed, count reset, and the lock bit.
// Returns channel+1 (0 = none).
// ---------------------------------------------------------------------------
extern "C" uint16_t v2_ailnat_lock_channel_3E20(void) {
    uint8_t mask = 0xC0;                          // 3E2D
    for (;;) {
        uint8_t cl = 0xFF; uint16_t si = 0xFFFF;  // 3E28
        for (uint16_t di = 8; di >= 1; di--) {    // 3E30..3E4C
            if (rd8n((uint16_t)(di + 0x2A2B)) & mask) continue;
            if (rd8n((uint16_t)(di + 0x2A1B)) >= cl) continue;    // jae
            cl = rd8n((uint16_t)(di + 0x2A1B));
            si = di;
        }
        if (si == 0xFFFF) {                       // 3E4E
            if (mask == 0x80) return 0;           // 3E53 -> 3E94 with si=0xFFFF
            mask = 0x80;                          // second pass allows protected
            continue;
        }
        // 3E5D..3E8E: silence and lock the winner
        v2_ailnat_midi_2629((uint16_t)(si | 0xB0), 0x40, 0);      // pedal off
        v2_ailnat_kill_channel_notes_2C1E((uint16_t)(si & 0xF));
        wr8n((uint16_t)((si & 0xF) + 0x2A1B), 0);
        wr8n((uint16_t)((si & 0xF) + 0x2A2B),
             (uint8_t)(rd8n((uint16_t)((si & 0xF) + 0x2A2B)) | 0x80));
        return (uint16_t)((si & 0xF) + 1);        // 3E94 ax=si+1
    }
}

// ---------------------------------------------------------------------------
// 3EA3: release a locked physical channel and replay its global controller/
// program/pitch images so it returns to the interrupted state.
// ---------------------------------------------------------------------------
extern "C" void v2_ailnat_release_channel_3EA3(uint16_t ch_plus_1) {
    uint16_t si = (uint16_t)(ch_plus_1 - 1);      // 3EAB..3EAE
    if (!(rd8n((uint16_t)(si + 0x2A2B)) & 0x80))  // not locked
        return;
    wr8n((uint16_t)(si + 0x2A2B), (uint8_t)(rd8n((uint16_t)(si + 0x2A2B)) & 0x7F));
    wr8n((uint16_t)(si + 0x2A1B), 0);
    v2_ailnat_midi_2629((uint16_t)(si | 0xB0), 0x40, 0);  // pedal off
    v2_ailnat_midi_2629((uint16_t)(si | 0xB0), 0x7B, 0);  // all notes off
    si = (uint16_t)(si & 0xF);
    uint16_t bx = si;                             // 3F05
    for (uint16_t di = 0; di < 9; di++) {         // controller image replay
        uint8_t dl = rd8n((uint16_t)(bx + 0x295B));
        if (dl != 0xFF)
            v2_ailnat_midi_2629((uint16_t)((bx & 0xF) | 0xB0),
                                rd8n((uint16_t)(di + 0x2814)), dl);
        bx = (uint16_t)(bx + 0x10);               // 3F2B next row
    }
    uint8_t pr = rd8n((uint16_t)(si + 0x29EB));   // 3F37 program image
    if (pr != 0xFF)
        v2_ailnat_midi_2629((uint16_t)(si | 0xC0), pr, 0);
    uint8_t bl_ = rd8n((uint16_t)(si + 0x29FB));  // 3F5A pitch image
    if (bl_ != 0xFF) {
        uint8_t bh_ = rd8n((uint16_t)(si + 0x2A0B));
        if (bh_ != 0xFF)
            v2_ailnat_midi_2629((uint16_t)(si | 0xE0), bl_, bh_);
    }
}

// ---------------------------------------------------------------------------
// 2CF4: sequence cleanup — undo the channel state the sequence changed
// (sustain, channel locks, protect, voice protect) on all 16 channels.
// ---------------------------------------------------------------------------
extern "C" void v2_ailnat_seq_cleanup_2CF4(uint16_t sg, uint16_t so) {
    for (uint16_t di = 0; di < 0x10; di++) {      // 2CFD..2D9D
        if ((int8_t)fr8(sg, (uint16_t)(so + 0xF8 + di)) >= 0x40) {   // sustain row
            wr8n((uint16_t)(di + 0x299B), 0);     // global sustain image
            v2_ailnat_midi_2629((uint16_t)(di | 0xB0), 0x40, 0);
        }
        if ((int8_t)fr8(sg, (uint16_t)(so + 0x118 + di)) >= 0x40) {  // lock row
            v2_ailnat_kill_channel_notes_2C1E(di);
            uint16_t m = fr8(sg, (uint16_t)(so + 0x68 + di));        // 2D44
            v2_ailnat_release_channel_3EA3((uint16_t)(m + 1));
            fw8(sg, (uint16_t)(so + 0x68 + di), (uint8_t)di);        // identity map
        }
        if ((int8_t)fr8(sg, (uint16_t)(so + 0x128 + di)) >= 0x40)    // protect row
            wr8n((uint16_t)(di + 0x2A2B), (uint8_t)(rd8n((uint16_t)(di + 0x2A2B)) & 0xBF));
        if ((int8_t)fr8(sg, (uint16_t)(so + 0x138 + di)) >= 0x40)    // voice protect
            v2_ailnat_midi_2629((uint16_t)(di | 0xB0), 0x70, 0);
    }
}

// ---------------------------------------------------------------------------
// 393C: release a sequence handle (deferred while it still plays).
// ---------------------------------------------------------------------------
extern "C" void v2_ailnat_release_handle_393C(uint16_t handle_off) {
    uint16_t si = handle_off;                     // 3944
    if (si == 0xFFFF) return;
    if (rd16n((uint16_t)(si + 0x2931)) == 0) return;
    uint16_t so = rd16n((uint16_t)(si + 0x292F));
    uint16_t sg = rd16n((uint16_t)(si + 0x2931));
    if (fr16(sg, (uint16_t)(so + 0x1A)) == 1) {   // 3959 still playing
        fw16(sg, (uint16_t)(so + 0x1C), 1);       // defer: flag for the tick
    } else {
        wr16n((uint16_t)(si + 0x2931), 0);        // free the slot
        wr16n(0x294F, (uint16_t)(rd16n(0x294F) - 1));
    }
}

// ---------------------------------------------------------------------------
// 2E8A: cascade the sequence volume to all 16 channel volume controllers.
// ---------------------------------------------------------------------------
extern "C" void v2_ailnat_seq_volume_2E8A(uint16_t sg, uint16_t so) {
    for (uint16_t bx = 0; bx < 0x10; bx++) {      // 2E93..2EE2
        uint8_t al = fr8(sg, (uint16_t)(so + 0xB8 + bx));   // channel vol shadow
        if (al == 0xFF) continue;
        uint16_t ax = (uint16_t)((al * fr16(sg, (uint16_t)(so + 0x24))) / 0x64);
        if (ax >= 0x7F) ax = 0x7F;                // 2EAA clamp
        wr8n((uint16_t)(bx + 0x295B), (uint8_t)ax);
        if (rd8n((uint16_t)(bx + 0x2A2B)) & 0x80) continue;  // muted
        uint16_t st = (uint16_t)(fr8(sg, (uint16_t)(so + 0x68 + bx)) | 0xB0);
        v2_ailnat_midi_2629(st, 0x7, ax);
    }
}

// ---------------------------------------------------------------------------
// 2EE9: XMIDI controller interceptor — remapped shadow rows, then the
// XMIDI-specific controllers; anything else forwards to the dispatcher.
// Returns the event length (always 3).
// ---------------------------------------------------------------------------
extern "C" uint16_t v2_ailnat_xmidi_ctl_2EE9(uint16_t sg, uint16_t so,
                                             uint16_t ch, uint16_t ctl,
                                             uint16_t val) {
    uint16_t bx = (uint16_t)(ch & 0xFF);
    uint8_t dl = (uint8_t)val;
    uint8_t pend = fr8(sg, (uint16_t)(so + 0xA8 + bx));   // 2F00 indirection
    if (pend != 0xFF) {
        fw8(sg, (uint16_t)(so + 0xA8 + bx), 0xFF);
        uint16_t to = fr16(sg, (uint16_t)(so + 0x12));    // 2F0F les [si+0x12]
        uint16_t tg = fr16(sg, (uint16_t)(so + 0x14));
        dl = fr8(tg, (uint16_t)(to + pend));              // table lookup
    }
    uint8_t row = rd8n((uint16_t)((ctl & 0xFF) + 0x282F));    // 2F1A remap
    if (row != 0xFF) {
        uint16_t r = (uint16_t)(row + (uint8_t)ch);       // 2F24 add bl,[bp+0xA]
        wr8n((uint16_t)(r + 0x295B), dl);                 // global image
        fw8(sg, (uint16_t)(so + 0xB8 + r), dl);           // per-seq shadow
    }
    uint8_t al = (uint8_t)ctl;                            // 2F30
    switch (al) {
    case 0x07: {                                  // 2F78 volume, seq-scaled
        uint16_t cxv = fr16(sg, (uint16_t)(so + 0x24));
        if (cxv != 0x64) {
            uint16_t dx16 = (uint16_t)((dl * cxv) / 0x64);
            if (dx16 >= 0x7F) dx16 = 0x7F;        // 2F8E clamp
            dl = (uint8_t)dx16;
            wr8n((uint16_t)(bx + 0x295B), dl);    // 2F96 (row 0 = volume)
        }
        break;                                    // -> forward (2F56)
    }
    case 0x73:                                    // 2FAE indirection arm
        fw8(sg, (uint16_t)(so + 0xA8 + bx), dl);
        return 3;
    case 0x76: {                                  // 2FB4 metronome reset
        fw16(sg, (uint16_t)(so + 0x3E), 0);
        fw16(sg, (uint16_t)(so + 0x40), 0);
        uint32_t inc = (uint32_t)(fr16(sg, (uint16_t)(so + 0x44)) |
                                  ((uint32_t)fr16(sg, (uint16_t)(so + 0x46)) << 16));
        fw16(sg, (uint16_t)(so + 0x48), (uint16_t)inc);
        fw16(sg, (uint16_t)(so + 0x4A), (uint16_t)(inc >> 16));
        return 3;
    }
    case 0x77:                                    // 2FD7 callback trigger
        fw16(sg, (uint16_t)(so + 0x10), dl);
        if ((uint16_t)(rd16n(0x2957) | rd16n(0x2959)) != 0) {
            if (g_nat_cb77)
                g_nat_cb77(fr16(sg, (uint16_t)(so + 0x16)), dl);
        }
        return 3;
    case 0x74: {                                  // 3004 FOR: push loop point
        uint16_t sl = 0;
        for (; sl < 8; sl += 2)
            if (fr16(sg, (uint16_t)(so + 0x60 + sl)) == 0xFFFF) break;
        if (sl == 8) return 3;                    // stack full
        fw16(sg, (uint16_t)(so + 0x60 + sl), dl);
        fw16(sg, (uint16_t)(so + 0x50 + sl * 2), fr16(sg, (uint16_t)(so + 0xC)));
        fw16(sg, (uint16_t)(so + 0x52 + sl * 2), fr16(sg, (uint16_t)(so + 0xE)));
        return 3;
    }
    case 0x75: {                                  // 3029 NEXT: loop back
        if ((int8_t)dl < 0x40) return 3;
        int16_t sl = 6;
        for (; sl >= 0; sl -= 2)
            if (fr16(sg, (uint16_t)(so + 0x60 + sl)) != 0xFFFF) break;
        if (sl < 0) return 3;
        uint16_t cnt = fr16(sg, (uint16_t)(so + 0x60 + sl));
        if (cnt != 0) {                           // 3045 (0 = infinite)
            cnt = (uint16_t)(cnt - 1);
            fw16(sg, (uint16_t)(so + 0x60 + sl), cnt);
            if (cnt == 0) {                       // exhausted: pop, no jump
                fw16(sg, (uint16_t)(so + 0x60 + sl), 0xFFFF);
                return 3;
            }
        }
        fw16(sg, (uint16_t)(so + 0xC), fr16(sg, (uint16_t)(so + 0x50 + sl * 2)));
        fw16(sg, (uint16_t)(so + 0xE), fr16(sg, (uint16_t)(so + 0x52 + sl * 2)));
        return 3;
    }
    case 0x6F:                                    // 3066 protect bit
        wr8n((uint16_t)(bx + 0x2A2B), (uint8_t)(rd8n((uint16_t)(bx + 0x2A2B)) | 0x40));
        if ((int8_t)dl < 0x40)
            wr8n((uint16_t)(bx + 0x2A2B), (uint8_t)(rd8n((uint16_t)(bx + 0x2A2B)) & 0xBF));
        return 3;
    case 0x6E: {                                  // 307D channel remap
        if ((int8_t)dl >= 0x40) {
            uint16_t r = v2_ailnat_lock_channel_3E20();   // 3E20(0)
            r = (uint16_t)(r - 1);                // 3095 dec ax
            if (r == 0xFFFF) r = bx;              // none -> itself
            fw8(sg, (uint16_t)(so + 0x68 + bx), (uint8_t)r);
        } else {
            v2_ailnat_kill_channel_notes_2C1E(bx);        // 30A7
            uint16_t m = fr8(sg, (uint16_t)(so + 0x68 + bx));
            v2_ailnat_release_channel_3EA3((uint16_t)(m + 1));
            fw8(sg, (uint16_t)(so + 0x68 + bx), (uint8_t)bx);  // identity
        }
        return 3;
    }
    default:
        break;                                    // -> forward
    }
    // 2F56: forward unless muted
    if (!(rd8n((uint16_t)(bx + 0x2A2B)) & 0x80)) {
        uint16_t st = (uint16_t)(fr8(sg, (uint16_t)(so + 0x68 + bx)) | 0xB0);
        v2_ailnat_midi_2629(st, al, dl);
    }
    return 3;                                     // 2F6E
}

// ---------------------------------------------------------------------------
// 30CD: XMIDI note-on stepper — read (status/note/velocity + VLQ duration),
// schedule the deferred note-off and fire the note-on. Returns event length.
// ---------------------------------------------------------------------------
extern "C" uint16_t v2_ailnat_xmidi_note_30CD(uint16_t sg, uint16_t so) {
    uint16_t eo = fr16(sg, (uint16_t)(so + 0xC));
    uint16_t eg = fr16(sg, (uint16_t)(so + 0xE));
    uint16_t w0 = fr16(eg, eo);                   // 30DC (note<<8)|status
    uint16_t chn = (uint16_t)((w0 & 0xFF00) | (w0 & 0xF)); // and al,0xF: ah=note, al=ch
    uint8_t vel = fr8(eg, (uint16_t)(eo + 0x2));  // [bp-0x4]
    uint16_t di = (uint16_t)(eo + 3);             // 30ED
    uint32_t dur = 0;                             // bx:dx VLQ
    for (;;) {                                    // 30F6 -> 3101
        uint8_t c = fr8(eg, di); di = (uint16_t)(di + 1);
        dur |= (c & 0x7F);
        if (!(c & 0x80)) break;
        dur <<= 7;                                // 30F8 (7x shl/rcl)
    }
    uint16_t len = (uint16_t)(di - eo);           // [bp-0x6]
    uint16_t ch = (uint16_t)(chn & 0xF);
    if (rd8n((uint16_t)(ch + 0x2A2B)) & 0x80)     // 311B muted channel
        return len;
    // 3127: find a free deferred slot (first 0xFF); none -> slot 0, and the
    // pending count only grows on a real find
    uint16_t idx = 0xFFFF;
    for (uint16_t i = 0; i < 0x20; i++)
        if (fr8(sg, (uint16_t)(so + 0x148 + i)) == 0xFF) { idx = i; break; }
    if (idx == 0xFFFF) idx = 0;                   // 3134 (repnz found nothing)
    else fw16(sg, (uint16_t)(so + 0x20),
              (uint16_t)(fr16(sg, (uint16_t)(so + 0x20)) + 1));   // 3136
    uint32_t d1 = dur - 1;                        // 3143 sub 1 / sbb 0
    fw8(sg, (uint16_t)(so + 0x148 + idx), (uint8_t)chn);          // channel
    fw8(sg, (uint16_t)(so + 0x168 + idx), (uint8_t)(chn >> 8));   // note
    fw16(sg, (uint16_t)(so + 0x188 + idx * 2), (uint16_t)d1);
    fw16(sg, (uint16_t)(so + 0x1C8 + idx * 2), (uint16_t)(d1 >> 16));
    uint16_t mch = fr8(sg, (uint16_t)(so + 0x68 + ch));           // 3162 map
    wr8n((uint16_t)(mch + 0x2A1B), (uint8_t)(rd8n((uint16_t)(mch + 0x2A1B)) + 1));
    v2_ailnat_midi_2629((uint16_t)(mch | 0x90), (uint16_t)(chn >> 8), vel);
    return len;
}

// ---------------------------------------------------------------------------
// 3185: meta event (0xFF type len data) — EOT / time signature / tempo.
// Returns the whole event length.
// ---------------------------------------------------------------------------
extern "C" uint16_t v2_ailnat_xmidi_meta_3185(uint16_t sg, uint16_t so) {
    uint16_t eo = fr16(sg, (uint16_t)(so + 0xC));
    uint16_t eg = fr16(sg, (uint16_t)(so + 0xE));
    uint8_t type = fr8(eg, (uint16_t)(eo + 0x1)); // [bp-0x4]
    uint16_t di = (uint16_t)(eo + 2);
    uint32_t vlen = 0;                            // VLQ length (31B1)
    for (;;) {
        uint8_t c = fr8(eg, di); di = (uint16_t)(di + 1);
        vlen |= (c & 0x7F);
        if (!(c & 0x80)) break;
        vlen <<= 7;
    }
    uint16_t total = (uint16_t)((uint16_t)(di - eo) + (uint16_t)vlen);  // 31C4
    if (type == 0x2F) {                           // 31E5 end of track
        v2_ailnat_seq_cleanup_2CF4(sg, so);
        fw16(sg, (uint16_t)(so + 0x1A), 2);       // status = done
        if (fr16(sg, (uint16_t)(so + 0x1C)) != 0)
            v2_ailnat_release_handle_393C(rd16n(0x2951));  // 31F9
    } else if (type == 0x58) {                    // 3211 time signature
        fw16(sg, (uint16_t)(so + 0x42), fr8(eg, di));      // beats/measure
        int16_t d = (int16_t)(fr8(eg, (uint16_t)(di + 1)) - 2);  // 3219
        uint32_t inc;
        if (d < 0) {                              // 3222 shift 0x2_08D5 right
            inc = 0x208D5u >> (uint16_t)(-d);
        } else {                                  // 3232 (1<<d) * 0x2_08D5
            uint16_t cx = (uint16_t)(1 << d);
            inc = 0;
            while (cx--) inc += 0x208D5u;         // add 0x8D5 / adc 2 loop
        }
        fw16(sg, (uint16_t)(so + 0x44), (uint16_t)inc);
        fw16(sg, (uint16_t)(so + 0x46), (uint16_t)(inc >> 16));
        fw16(sg, (uint16_t)(so + 0x48), (uint16_t)inc);    // 324D beat clock
        fw16(sg, (uint16_t)(so + 0x4A), (uint16_t)(inc >> 16));
    } else if (type == 0x51) {                    // 3267 tempo (24-bit us/qn)
        uint32_t t = ((uint32_t)fr8(eg, di) << 16)
                   | ((uint32_t)fr8(eg, (uint16_t)(di + 1)) << 8)
                   | fr8(eg, (uint16_t)(di + 2));
        t <<= 4;                                  // 3274 shl dx:ax,4
        fw16(sg, (uint16_t)(so + 0x4C), (uint16_t)t);
        fw16(sg, (uint16_t)(so + 0x4E), (uint16_t)(t >> 16));
    }
    return total;                                 // 31DB
}

// ---------------------------------------------------------------------------
// 3286: sysex/escape event — skip [len-VLQ] bytes. Returns event length.
// ---------------------------------------------------------------------------
extern "C" uint16_t v2_ailnat_xmidi_sysex_3286(uint16_t sg, uint16_t so) {
    uint16_t eo = fr16(sg, (uint16_t)(so + 0xC));
    uint16_t eg = fr16(sg, (uint16_t)(so + 0xE));
    uint16_t di = (uint16_t)(eo + 1);             // skip the status byte
    uint32_t vlen = 0;
    for (;;) {
        uint8_t c = fr8(eg, di); di = (uint16_t)(di + 1);
        vlen |= (c & 0x7F);
        if (!(c & 0x80)) break;
        vlen <<= 7;
    }
    return (uint16_t)((uint16_t)(di - eo) + (uint16_t)vlen);   // 32C4
}

// ---------------------------------------------------------------------------
// 331E: the timer tick — walk the sequence handles; per playing sequence
// run the tempo accumulator, the beat clock, the deferred note-offs, the
// event reader and the tempo/volume ramps. Every branch mirrors the listing.
// ---------------------------------------------------------------------------
extern "C" void v2_ailnat_timer_tick_331E(void) {
    if (rd16n(0x2953) != 0) return;               // 3328 reentry gate
    wr16n(0x2953, (uint16_t)(rd16n(0x2953) + 1)); // 334E
    wr16n(0x2951, 0xFFFC);
    uint16_t left = rd16n(0x294F);                // [bp-0x2]
    if (left != 0) {
        for (;;) {                                // 3364 handle loop
            uint16_t di;
            do {
                wr16n(0x2951, (uint16_t)(rd16n(0x2951) + 4));
                di = rd16n(0x2951);
            } while (rd16n((uint16_t)(di + 0x2931)) == 0);
            uint16_t so = rd16n((uint16_t)(di + 0x292F));
            uint16_t sg = rd16n((uint16_t)(di + 0x2931));
            if (fr16(sg, (uint16_t)(so + 0x1A)) == 1) {       // 337C playing
                // 3382: tempo accumulator step
                uint16_t acc = (uint16_t)(fr16(sg, (uint16_t)(so + 0x30))
                                          + fr16(sg, (uint16_t)(so + 0x32)));
                fw16(sg, (uint16_t)(so + 0x30), acc);
                int16_t rem = (int16_t)(acc - 0x64);          // 338B
                while (rem >= 0) {                            // -> 3390
                    fw16(sg, (uint16_t)(so + 0x30), (uint16_t)rem);
                    // 3393: beat clock
                    uint32_t clk = (uint32_t)(fr16(sg, (uint16_t)(so + 0x48))
                                   | ((uint32_t)fr16(sg, (uint16_t)(so + 0x4A)) << 16));
                    clk += (uint32_t)(fr16(sg, (uint16_t)(so + 0x44))
                           | ((uint32_t)fr16(sg, (uint16_t)(so + 0x46)) << 16));
                    uint32_t per = (uint32_t)(fr16(sg, (uint16_t)(so + 0x4C))
                                   | ((uint32_t)fr16(sg, (uint16_t)(so + 0x4E)) << 16));
                    if ((int32_t)clk >= (int32_t)per) {       // 339F..33A9
                        clk -= per;                           // 3333
                        uint16_t beat = (uint16_t)(fr16(sg, (uint16_t)(so + 0x3E)) + 1);
                        fw16(sg, (uint16_t)(so + 0x3E), beat);
                        if (beat >= fr16(sg, (uint16_t)(so + 0x42))) {    // jb
                            fw16(sg, (uint16_t)(so + 0x3E), 0);
                            fw16(sg, (uint16_t)(so + 0x40),
                                 (uint16_t)(fr16(sg, (uint16_t)(so + 0x40)) + 1));
                        }
                    }
                    fw16(sg, (uint16_t)(so + 0x48), (uint16_t)clk);   // 33AB
                    fw16(sg, (uint16_t)(so + 0x4A), (uint16_t)(clk >> 16));
                    // 33B1: deferred note-offs
                    if (fr16(sg, (uint16_t)(so + 0x20)) != 0) {
                        for (uint16_t i = 0; i < 0x20; ) {    // 33EE scan
                            if (fr8(sg, (uint16_t)(so + 0x148 + i)) == 0xFF) {
                                i++; continue;
                            }
                            uint32_t d = (uint32_t)(fr16(sg, (uint16_t)(so + 0x188 + i * 2))
                                       | ((uint32_t)fr16(sg, (uint16_t)(so + 0x1C8 + i * 2)) << 16));
                            d -= 1;                           // 3408 sub/sbb
                            fw16(sg, (uint16_t)(so + 0x188 + i * 2), (uint16_t)d);
                            fw16(sg, (uint16_t)(so + 0x1C8 + i * 2), (uint16_t)(d >> 16));
                            if ((int32_t)d >= 0) { i++; continue; }   // 3412 jge
                            // 3414: expired — fire the note off
                            uint8_t chb = fr8(sg, (uint16_t)(so + 0x148 + i));
                            fw8(sg, (uint16_t)(so + 0x148 + i), 0xFF);
                            uint8_t note = fr8(sg, (uint16_t)(so + 0x168 + i));
                            uint16_t mch = fr8(sg, (uint16_t)(so + 0x68 + chb));
                            wr8n((uint16_t)(mch + 0x2A1B),
                                 (uint8_t)(rd8n((uint16_t)(mch + 0x2A1B)) - 1));
                            v2_ailnat_midi_2629((uint16_t)(mch | 0x80), note, 0);
                            uint16_t pc = (uint16_t)(fr16(sg, (uint16_t)(so + 0x20)) - 1);
                            fw16(sg, (uint16_t)(so + 0x20), pc);      // 3448
                            if (pc == 0) break;               // je -> 33B7
                            i++;
                        }
                    }
                    // 33B7: event countdown
                    int16_t t1E = (int16_t)(fr16(sg, (uint16_t)(so + 0x1E)) - 1);
                    fw16(sg, (uint16_t)(so + 0x1E), (uint16_t)t1E);
                    if (t1E <= 0) {                           // 33BA jle -> 3479
                        // ---- event reader loop ----
                        for (;;) {
                            uint16_t eo = fr16(sg, (uint16_t)(so + 0xC));
                            uint16_t eg = fr16(sg, (uint16_t)(so + 0xE));
                            if (eo >= 0x8000) {               // 347C -> 345A
                                eg = (uint16_t)(eg + (eo >> 4));
                                eo = (uint16_t)(eo & 0xF);
                                fw16(sg, (uint16_t)(so + 0xE), eg);
                                fw16(sg, (uint16_t)(so + 0xC), eo);
                            }
                            uint16_t ev = fr8(eg, eo);        // 3482
                            if (ev < 0x80) {                  // 3487 delta time
                                fw16(sg, (uint16_t)(so + 0xC), (uint16_t)(eo + 1));
                                fw16(sg, (uint16_t)(so + 0x1E), ev);
                                break;                        // 3450 -> 33BC
                            }
                            uint16_t fam = (uint16_t)(ev & 0xF0);
                            uint16_t chx = (uint16_t)(ev & 0xF);
                            uint8_t cl = fr8(eg, (uint16_t)(eo + 1));  // 3494
                            uint8_t dl2 = fr8(eg, (uint16_t)(eo + 2));
                            uint16_t adv;
                            int send = 0; uint16_t sendst = 0;
                            if (fam >= 0xF0) {                // 349C
                                adv = (chx == 0xF)
                                    ? v2_ailnat_xmidi_meta_3185(sg, so)   // 34DF
                                    : v2_ailnat_xmidi_sysex_3286(sg, so); // 34D2
                            } else if (fam >= 0xE0) {         // 34FD pitch
                                fw8(sg, (uint16_t)(so + 0x88 + chx), cl);
                                fw8(sg, (uint16_t)(so + 0x98 + chx), dl2);
                                wr8n((uint16_t)(chx + 0x29FB), cl);
                                wr8n((uint16_t)(chx + 0x2A0B), dl2);
                                adv = 3; send = 1; sendst = fam;   // al = family
                            } else if (fam >= 0xD0) {         // 34A9 -> 351F
                                adv = 2; send = 1; sendst = fam;
                            } else if (fam >= 0xC0) {         // 3514 program
                                fw8(sg, (uint16_t)(so + 0x78 + chx), cl);
                                wr8n((uint16_t)(chx + 0x29EB), cl);
                                adv = 2; send = 1; sendst = fam;
                            } else if (fam >= 0xB0) {         // 34EC controller
                                v2_ailnat_xmidi_ctl_2EE9(sg, so, chx, cl, dl2);
                                adv = 3;
                            } else if (fam >= 0xA0) {         // 34BB -> 351F
                                adv = 3; send = 1; sendst = fam;
                            } else {                          // 0x90 note on
                                adv = v2_ailnat_xmidi_note_30CD(sg, so);  // 34C3
                            }
                            if (send) {                       // 351F common send
                                if (!(rd8n((uint16_t)(chx + 0x2A2B)) & 0x80)) {
                                    uint16_t st = (uint16_t)(sendst |
                                        fr8(sg, (uint16_t)(so + 0x68 + chx)));
                                    v2_ailnat_midi_2629(st, cl, dl2);
                                }
                            }
                            fw16(sg, (uint16_t)(so + 0xC),    // 3534 advance
                                 (uint16_t)(fr16(sg, (uint16_t)(so + 0xC)) + adv));
                            if (fr16(sg, (uint16_t)(so + 0x1A)) != 1)
                                goto next_handle;             // 3540 -> 33D4
                        }
                    }
                    // 33BC: another whole tempo step?
                    rem = (int16_t)(fr16(sg, (uint16_t)(so + 0x30)) - 0x64);
                }
                // 33C4: tempo ramp
                {
                    uint16_t cur = fr16(sg, (uint16_t)(so + 0x32));
                    uint16_t tgt = fr16(sg, (uint16_t)(so + 0x34));
                    if (cur != tgt) {                         // 33CA -> 3543
                        uint32_t a2 = (uint32_t)(fr16(sg, (uint16_t)(so + 0x36))
                                     | ((uint32_t)fr16(sg, (uint16_t)(so + 0x38)) << 16));
                        a2 += 0x53;
                        uint32_t stp = (uint32_t)(fr16(sg, (uint16_t)(so + 0x3A))
                                      | ((uint32_t)fr16(sg, (uint16_t)(so + 0x3C)) << 16));
                        uint16_t cx = 0xFFFF;             // 3550 mov cx,-1
                        for (;;) {                            // 3553 count steps
                            cx = (uint16_t)(cx + 1);          // inc BEFORE the sub:
                            fw16(sg, (uint16_t)(so + 0x36), (uint16_t)a2);   // n subs -> cx = n-1,
                            fw16(sg, (uint16_t)(so + 0x38), (uint16_t)(a2 >> 16)); // so a single
                            a2 -= stp;                        // step applies NOTHING (jcxz)
                            if ((int32_t)a2 < 0) break;       // 3560 jge
                        }
                        // 3562: pushf/IRET pair — the branch below uses the
                        // ORIGINAL cur-vs-tgt compare, not the loop flags
                        if (cx != 0) {                        // 3569 jcxz
                            uint16_t nv;
                            if ((int16_t)cur < (int16_t)tgt) {        // 3571 jl
                                nv = (uint16_t)(cur + cx);            // 357B
                                if ((int16_t)nv > (int16_t)tgt) nv = tgt;
                            } else {
                                nv = (uint16_t)(cur - cx);            // 3573
                                if ((int16_t)nv < (int16_t)tgt) nv = tgt;
                            }
                            fw16(sg, (uint16_t)(so + 0x32), nv);      // 3583
                        }
                    }
                }
                // 33CC: volume ramp
                {
                    uint16_t cur = fr16(sg, (uint16_t)(so + 0x24));
                    uint16_t tgt = fr16(sg, (uint16_t)(so + 0x26));
                    if (cur != tgt) {                         // 33D2 -> 3589
                        uint32_t a2 = (uint32_t)(fr16(sg, (uint16_t)(so + 0x28))
                                     | ((uint32_t)fr16(sg, (uint16_t)(so + 0x2A)) << 16));
                        a2 += 0x53;
                        uint32_t stp = (uint32_t)(fr16(sg, (uint16_t)(so + 0x2C))
                                      | ((uint32_t)fr16(sg, (uint16_t)(so + 0x2E)) << 16));
                        uint16_t cx = 0xFFFF;             // 3596 mov cx,-1
                        for (;;) {                            // 3599
                            cx = (uint16_t)(cx + 1);          // n subs -> cx = n-1
                            fw16(sg, (uint16_t)(so + 0x28), (uint16_t)a2);
                            fw16(sg, (uint16_t)(so + 0x2A), (uint16_t)(a2 >> 16));
                            a2 -= stp;
                            if ((int32_t)a2 < 0) break;
                        }
                        if (cx != 0) {                        // 35AF jcxz 35D5
                            uint16_t nv;
                            if ((int16_t)cur < (int16_t)tgt) {        // 35B7 jl
                                nv = (uint16_t)(cur + cx);
                                if ((int16_t)nv > (int16_t)tgt) nv = tgt;
                            } else {
                                nv = (uint16_t)(cur - cx);
                                if ((int16_t)nv < (int16_t)tgt) nv = tgt;
                            }
                            fw16(sg, (uint16_t)(so + 0x24), nv);      // 35C9
                            v2_ailnat_seq_volume_2E8A(sg, so);
                        }
                    }
                }
            }
        next_handle:                              // 33D4
            left = (uint16_t)(left - 1);
            if (left == 0) break;
        }
    }
    wr16n(0x2953, (uint16_t)(rd16n(0x2953) - 1)); // 33D9
}

// ---------------------------------------------------------------------------
// 272D tail of the dispatcher: mark the dirty bits on every active slot of
// the channel and flush it through the register updater.
// ---------------------------------------------------------------------------
static void nat_mark_channel_272D(uint16_t ch, uint8_t bits) {
    for (uint16_t si = 0; si < 0x14; si++) {
        if (rd8n((uint16_t)(si + 0xD95)) == 0) continue;
        if (rd8n((uint16_t)(si + 0xDD1)) != (uint8_t)ch) continue;
        wr8n((uint16_t)(si + 0xE49), (uint8_t)(rd8n((uint16_t)(si + 0xE49)) | bits));
        v2_ailnat_update_voice_1B7C(si);          // 274A
    }
}

// ---------------------------------------------------------------------------
// 2629: MIDI event dispatcher (status, data1, data2).
// ---------------------------------------------------------------------------
// UX stage 11 (the SC-55 option, v2_midi.h): every dispatched channel message
// is mirrored to the MIDI lane before the OPL driver takes it — the lane is
// presentation only (no DS, no driver state), off unless the option or the
// V2_MIDI_DUMP check asks for it.
extern "C" void v2_ailnat_midi_2629(uint16_t status, uint16_t d1, uint16_t d2) {
    v2_midi_event(status, d1, d2);
    uint16_t si = (uint16_t)(d1 & 0xFF);          // 262F
    uint16_t di = (uint16_t)(status & 0xF);       // channel
    uint16_t ax = (uint16_t)(status & 0xF0);      // family
    uint8_t  cl = (uint8_t)d2;                    // 2643 (ch=0)
    if (ax == 0xB0) {                             // 2699 controllers
        if (si == 0x72) { wr8n((uint16_t)(di + 0x1299), cl); return; }  // bank
        if (si == 0x70) { wr8n((uint16_t)(di + 0x1279), cl); return; }  // protect
        if (si == 0x71) {                         // 26A8 timbre lock bit
            uint16_t t = rd8n((uint16_t)(di + 0x1289));
            if ((uint8_t)t == 0xFF) return;
            uint8_t v = (uint8_t)(rd8n((uint16_t)(t + 0xC4B)) & 0xBF);
            if ((int8_t)cl >= 0x40) v = (uint8_t)(v | 0x40);   // jl = signed
            wr8n((uint16_t)(t + 0xC4B), v);
            return;
        }
        uint8_t bits; uint16_t row;               // 26F2 the marking controllers
        if      (si == 0x01) { bits = 0x80; row = 0x1259; }    // modulation
        else if (si == 0x07) { bits = 0x40; row = 0x1209; }    // volume
        else if (si == 0x0B) { bits = 0x40; row = 0x1249; }    // expression
        else if (si == 0x0A) { bits = 0x08; row = 0x1219; }    // pan
        else if (si == 0x40) {                    // 275B sustain pedal
            wr8n((uint16_t)(di + 0x1269), cl);
            if ((int8_t)cl < 0x40)                // release edge only
                v2_ailnat_sustain_release_25EE(di);
            return;
        }
        else if (si == 0x79) {                    // 279D reset all controllers
            wr8n((uint16_t)(di + 0x1269), 0x00);
            v2_ailnat_sustain_release_25EE(di);
            wr8n((uint16_t)(di + 0x1259), 0x00);
            wr8n((uint16_t)(di + 0x1249), 0x7F);
            wr8n((uint16_t)(di + 0x1229), 0x00);
            wr8n((uint16_t)(di + 0x1239), 0x40);
            nat_mark_channel_272D(di, 0xC1);
            return;
        }
        else if (si == 0x7B) {                    // 2773 all notes off
            for (uint16_t s2 = 0; s2 < 0x14; s2++) {
                if (rd8n((uint16_t)(s2 + 0xD95)) != 1) continue;
                if (rd8n((uint16_t)(s2 + 0xDD1)) != (uint8_t)di) continue;
                v2_ailnat_note_off_2477(di, rd16n((uint16_t)(s2 + 0xDE5)));
            }
            return;
        }
        else return;                              // 2727 unknown controller
        wr8n((uint16_t)(row + di), cl);           // 272A store
        nat_mark_channel_272D(di, bits);
        return;
    }
    if (ax == 0xC0) {                             // 26C9 program change
        wr8n((uint16_t)(di + 0x12A9), (uint8_t)si);
        uint16_t r = v2_ailnat_find_slot_15E9(
            (uint16_t)((rd8n((uint16_t)(di + 0x1299)) << 8) | (uint8_t)si));
        wr8n((uint16_t)(di + 0x1289), (uint8_t)r);
        return;
    }
    if (ax == 0xE0) {                             // 2688 pitch wheel
        wr8n((uint16_t)(di + 0x1229), (uint8_t)si);
        wr8n((uint16_t)(di + 0x1239), cl);
        nat_mark_channel_272D(di, 0x01);          // 2694 al=1 -> 272D
        return;
    }
    if (ax == 0x80) {                             // 2655 note off
        v2_ailnat_note_off_2477(di, si);
        return;
    }
    if (ax == 0x90) {                             // 265A note on (channels 1..9)
        if (di < 1 || di > 9) return;
        if (cl == 0) { v2_ailnat_note_off_2477(di, si); return; }  // 2669 jcxz
        v2_ailnat_note_on_24E4(di, si, (uint16_t)cl);
        return;
    }
    // any other family: 2683 exit
}

// ---------------------------------------------------------------------------
// fn surface — the driver entry points the game calls (plus their helpers).
// ---------------------------------------------------------------------------
// The blob records its own code segment (fn64 stores cs at cs:0xD1 and
// returns the descriptor as cs:0xC7). The native port carries that identity
// as a configurable value so images stay byte-comparable with any executor.
static uint16_t g_nat_self_seg = 0;
static uint16_t g_nat_last_dx = 0;
extern "C" void v2_ailnat_set_self_seg(uint16_t seg) { g_nat_self_seg = seg; }
extern "C" uint16_t v2_ailnat_last_dx(void) { return g_nat_last_dx; }

// 1383: latch the OPL port block from the io base ([bp+6]):
// second pair (bank 1 probe) = io+4/io+5, primary = io/io+1.
extern "C" void v2_ailnat_set_ports_1383(uint16_t io) {
    wr16n(0x63F, (uint16_t)(io + 4));
    wr16n(0x641, (uint16_t)(io + 5));
    wr16n(0x645, io);
    wr16n(0x643, (uint16_t)(io + 1));
}

// 1486: boot-time raw register write (long settle: 6 reads, then 0x2A).
static void nat_boot_write_1486(uint8_t reg, uint8_t val) {
    uint16_t dx = rd16n(0x645);
    if (g_nat_out) g_nat_out(dx, reg);
    for (int i = 0; i < 6; i++) if (g_nat_in) (void)g_nat_in(dx);
    dx = rd16n(0x643);
    if (g_nat_out) g_nat_out(dx, val);
    dx = rd16n(0x645);
    for (int i = 0; i < 0x2A; i++) if (g_nat_in) (void)g_nat_in(dx);
}

// 1405: read the status register.
static uint8_t nat_status_1405(void) {
    return g_nat_in ? g_nat_in(rd16n(0x645)) : 0;
}

// 14AE: classic AdLib detect via timer 1 (status bits 0xE0: 0 -> 0xC0).
static uint16_t nat_detect_14AE(void) {
    nat_boot_write_1486(0x4, 0x60);               // mask timers
    nat_boot_write_1486(0x4, 0x80);               // reset IRQ
    uint16_t di = nat_status_1405();              // 14E7
    nat_boot_write_1486(0x2, 0xFF);               // timer 1 preset
    nat_boot_write_1486(0x4, 0x21);               // start timer 1
    uint16_t si = 0;
    for (uint16_t n = 0xC8; n; n--) si = nat_status_1405();   // 1523 wait loop
    nat_boot_write_1486(0x4, 0x60);
    nat_boot_write_1486(0x4, 0x80);
    si = (uint16_t)(si & 0xE0);                   // 1565
    di = (uint16_t)(di & 0xE0);
    return (di == 0 && si == 0xC0) ? 1 : 0;
}

// fn65 140E(drv, io): non-destructive device probe — detect on the primary
// pair, then the OPL3 status-mirror test on the second pair.
extern "C" uint16_t v2_ailnat_fn65_probe_140E(uint16_t io) {
    uint16_t s63F = rd16n(0x63F), s641 = rd16n(0x641);   // 1416 save cells
    uint16_t s645 = rd16n(0x645), s643 = rd16n(0x643);
    v2_ailnat_set_ports_1383(io);
    uint16_t ax = nat_detect_14AE();              // 1434
    if (ax != 0) {
        uint16_t dx = rd16n(0x63F);               // 143D second pair
        if (g_nat_out) { g_nat_out(dx, 0x0A);     // out dx,ax (word: AL,AH)
                         g_nat_out((uint16_t)(dx + 1), 0x00); }
        dx = rd16n(0x641);
        uint8_t st = g_nat_in ? g_nat_in(dx) : 0; // 144C
        uint8_t ah = st;
        uint8_t al = (uint8_t)(st ^ 0x6);
        if (g_nat_out) g_nat_out(dx, al);         // 1453
        al = g_nat_in ? g_nat_in(dx) : 0;         // 1456
        al = (uint8_t)(al ^ 0x6);
        int equal = (al == ah);                   // 1459 cmp; 145E..1463
        if (g_nat_out) g_nat_out(dx, ah);         // 145D restore
        ax = equal ? 1 : 0;                       // mirror still differs = OPL3
    }
    wr16n(0x63F, s63F); wr16n(0x641, s641);       // 1466 restore cells
    wr16n(0x645, s645); wr16n(0x643, s643);
    return ax;
}

// 1582: OPL3 register init — enable NEW (0x105=1), clear 4-op (0x104=0),
// then the whole register file from the tables at cs:0x34D / cs:0x442.
static void nat_opl3_init_1582(void) {
    nat_out_tail_13DE(0x05, 0x01, 0x01);          // 1587 bx=0x105
    nat_out_tail_13DE(0x04, 0x01, 0x00);          // 1590 bx=0x104
    wr8n(0xD1C, 0);
    for (uint16_t bx = 0x1; bx <= 0xF5; bx++)     // bank 0 from cs:[bx+0x34C]
        nat_out_tail_13DE((uint8_t)bx, 0x00, rd8n((uint16_t)(bx + 0x34C)));
    for (uint16_t bx = 0x101; bx <= 0x1F5; bx++)  // bank 1 from cs:[bx+0x341]
        nat_out_tail_13DE((uint8_t)bx, 0x01, rd8n((uint16_t)(bx + 0x341)));
}

// 19BE: timbre system reset — LRU clock, slot tables, channel maps, rings.
static void nat_timbre_reset_19BE(void) {
    wr16n(0x647, 0); wr16n(0x649, 0);
    for (uint16_t i = 0; i < 0xC0; i++) wr8n((uint16_t)(i + 0xC4B), 0);
    for (uint16_t i = 0; i < 0x10; i++) {
        wr8n((uint16_t)(i + 0x1289), 0xFF);
        wr8n((uint16_t)(i + 0x1339), 0);
        wr8n((uint16_t)(i + 0x12A9), 0xFF);
        wr8n((uint16_t)(i + 0x1299), 0);
    }
    for (uint16_t i = 0; i < 0x14; i++) wr8n((uint16_t)(i + 0xD95), 0);
    for (uint16_t i = 0; i < 0x12; i++) wr8n((uint16_t)(i + 0x1349), 0xFF);
    for (uint16_t i = 0; i < 0x80; i++) wr8n((uint16_t)(i + 0x12B9), 0xFF);
    wr16n(0xD13, 0); wr16n(0xD15, 0);
    wr8n(0xD17, 0);
    wr16n(0xD18, 0xFFFF);                         // 1A41 ring cursors = -1
    wr16n(0xD1A, 0xFFFF);
}

// 37D4: clear the XMIDI trigger callback.
extern "C" void v2_ailnat_clear_cb_37D4(void) {
    wr16n(0x2957, 0);
    wr16n(0x2959, 0);
}

// fn64 27C8: return the descriptor far pointer (cs:0xC7), recording cs.
extern "C" uint16_t v2_ailnat_fn64_desc_27C8(void) {
    wr16n(0xD1, g_nat_self_seg);                  // 27D2
    g_nat_last_dx = g_nat_self_seg;
    return 0xC7;                                  // 27D7
}

// fn66 35D8(drv, io, ...): install — clear sequencer state, build the
// controller remap, set the ports, init the chip, reset timbres and push
// the per-channel controller/program/pitch defaults.
extern "C" void v2_ailnat_fn66_install_35D8(uint16_t io) {
    wr16n(0x2953, 0);                             // 35E1
    wr16n(0x294F, 0);
    for (uint16_t i = 0; i < 0x60 * 2; i++) wr8n((uint16_t)(0x295B + i), 0xFF);
    for (uint16_t i = 0; i < 0x80 * 2; i++) wr8n((uint16_t)(0x282F + i), 0xFF);
    for (uint16_t i = 0; i < 0x10 * 2; i++) wr8n((uint16_t)(0x292F + i), 0x00);
    for (uint16_t i = 0; i < 0x8 * 2; i++)  wr8n((uint16_t)(0x2A2B + i), 0x00);
    for (uint16_t i = 0; i < 0x8 * 2; i++)  wr8n((uint16_t)(0x2A1B + i), 0x00);
    {                                             // 361F controller remap rows
        uint16_t ax = 0;
        for (uint16_t si = 0; si < 9; si++) {
            uint8_t ctl = rd8n((uint16_t)(si + 0x2814));
            wr8n((uint16_t)(ctl + 0x282F), (uint8_t)ax);
            ax = (uint16_t)(ax + 0x10);
        }
    }
    v2_ailnat_set_ports_1383(io);                 // 3647 (extra args unused)
    nat_opl3_init_1582();                         // 364E
    nat_timbre_reset_19BE();                      // 3652
    v2_ailnat_clear_cb_37D4();                    // 3656
    for (uint16_t si = 0; si < 9; si++) {         // 3659 controller defaults
        for (uint16_t di = 1; di <= 9; di++) {
            uint16_t ax = (uint16_t)(di | 0xB0);
            uint16_t bx = rd8n((uint16_t)(si + 0x2814));
            uint8_t cl = rd8n((uint16_t)(si + 0x281D));
            if (cl == 0xFF) break;                // 3670 je 3693 (next row)
            uint16_t dxr = bx;                    // ctl id
            bx = rd8n((uint16_t)(bx + 0x282F));   // 3677 row offset
            bx = (uint16_t)(bx + di);
            wr8n((uint16_t)(bx + 0x295B), cl);    // global image
            v2_ailnat_midi_2629(ax, dxr, cl);     // 3687
        }
    }
    for (uint16_t di = 1; di <= 9; di++) {        // 3699 pitch + program
        wr8n((uint16_t)(di + 0x29FB), 0x00);
        wr8n((uint16_t)(di + 0x2A0B), 0x40);
        v2_ailnat_midi_2629((uint16_t)(di | 0xE0), 0x00, 0x40);   // 36C3
        uint8_t pr = rd8n((uint16_t)(di + 0x2825));               // 36C9
        if (pr != 0xFF) {
            wr8n((uint16_t)(di + 0x29EB), pr);
            v2_ailnat_midi_2629((uint16_t)(di | 0xC0), pr, 0);    // 36EA
        }
    }
    wr16n(0x2A3B, 1);                             // 36F6 installed flag
}

// fn99 16D9: timbre cache size.
extern "C" uint16_t v2_ailnat_fn99_cache_size_16D9(void) { return 0xE00; }

// fn9A 16F0(drv, off, seg, size): assign the timbre cache.
extern "C" void v2_ailnat_fn9A_set_cache_16F0(uint16_t off, uint16_t seg,
                                              uint16_t size) {
    uint16_t no = (uint16_t)(off & 0xF);          // 16FB normalize
    uint16_t ns = (uint16_t)(seg + (off >> 4));
    wr16n(0xD0B, no);
    wr16n(0xD0D, ns);
    wr16n(0xD0F, size);
    wr16n(0xD11, 0);
}

// fn9B 1736(drv, handle): first timbre in the sequence's TIMB chunk that is
// not loaded yet — returns bank<<8|patch, or 0xFFFF when satisfied.
extern "C" uint16_t v2_ailnat_fn9B_timbre_request_1736(uint16_t handle) {
    if (handle == 0xFFFF) return 0xFFFF;
    uint16_t so = rd16n((uint16_t)(handle + 0x292F));
    uint16_t sg = rd16n((uint16_t)(handle + 0x2931));
    if (fr16(sg, (uint16_t)(so + 0x2)) == 0) return 0xFFFF;   // 174B no TIMB
    uint16_t to = fr16(sg, so);                   // 1751 lds si,[si]
    uint16_t tg = fr16(sg, (uint16_t)(so + 0x2));
    if (fr16(tg, to) != 0x4954) return 0xFFFF;    // 'TI'
    if (fr16(tg, (uint16_t)(to + 0x2)) != 0x424D) return 0xFFFF;   // 'MB'
    uint16_t si = (uint16_t)(to + 8);             // 1760
    uint16_t di = fr16(tg, si);                   // count
    for (;;) {
        si = (uint16_t)(si + 2);                  // 1765
        uint16_t key = fr16(tg, si);
        if (v2_ailnat_find_slot_15E9(key) == 0xFFFF)
            return key;                           // 177D not loaded yet
        di = (uint16_t)(di - 1);
        if (di == 0) return 0xFFFF;               // 1779
    }
}

// 1790: evict the least-recently-used unlocked timbre and compact the cache.
static void nat_timbre_evict_1790(void) {
    uint16_t victim = 0xFFFF;                     // [bp-0x2]
    uint16_t lo = 0xFFFF, hi = 0xFFFF;            // ax/dx running minimum
    for (uint16_t si = 0; si < 0xC0; si++) {      // 17A8
        uint8_t st = rd8n((uint16_t)(si + 0xC4B));
        if (!(st & 0x80)) continue;
        if (st & 0x40) continue;                  // locked
        uint16_t h2 = rd16n((uint16_t)(si * 2 + 0x7CB));
        uint16_t l2 = rd16n((uint16_t)(si * 2 + 0x64B));
        if (h2 > hi) continue;                    // 17BB ja
        if (h2 == hi && l2 > lo) continue;        // 17C4 ja
        lo = l2; hi = h2; victim = si;            // 17CB
    }
    if (victim == 0xFFFF) return;                 // 17E2
    uint16_t voff = rd16n((uint16_t)(victim * 2 + 0x94B));    // [bp-0x6]
    uint16_t base_o = rd16n(0xD0B), base_s = rd16n(0xD0D);
    uint16_t vlen = fr16(base_s, (uint16_t)(base_o + voff));  // [bp-0x4]
    {   // 1805..181B compact: move the tail of the cache down over the victim
        uint16_t dst = (uint16_t)(base_o + voff);
        uint16_t src = (uint16_t)(dst + vlen);
        uint16_t cnt = (uint16_t)((uint16_t)(base_o + rd16n(0xD11)) - src);
        for (uint16_t i = 0; i < cnt; i++)
            fw8(base_s, (uint16_t)(dst + i), fr8(base_s, (uint16_t)(src + i)));
    }
    wr8n((uint16_t)(victim + 0xC4B), 0);          // 1820
    wr16n(0xD11, (uint16_t)(rd16n(0xD11) - vlen));
    for (uint16_t di = 0; di < 0x10; di++) {      // 182E unlink patch maps
        uint8_t t = rd8n((uint16_t)(di + 0x1289));
        if (t != 0xFF && t == (uint8_t)victim)
            wr8n((uint16_t)(di + 0x1289), 0xFF);
    }
    for (uint16_t di = 0; di < 0x80; di++)        // 184E drum maps
        if (rd8n((uint16_t)(di + 0x12B9)) == (uint8_t)victim)
            wr8n((uint16_t)(di + 0x12B9), 0xFF);
    for (uint16_t si = 0; si < 0xC0; si++) {      // 1868 shift later offsets
        if (!(rd8n((uint16_t)(si + 0xC4B)) & 0x80)) continue;
        uint16_t o2 = rd16n((uint16_t)(si * 2 + 0x94B));
        if (o2 > voff)
            wr16n((uint16_t)(si * 2 + 0x94B), (uint16_t)(o2 - vlen));
    }
    uint16_t vabs = (uint16_t)(voff + base_o);    // 1892
    for (uint16_t si = 0; si < 0x14; si++) {      // 189C live slot pointers
        if (rd8n((uint16_t)(si + 0xD95)) == 0) continue;
        uint16_t po = rd16n((uint16_t)(si * 2 + 0xD1D));
        if (po < vabs) continue;
        if (po == vabs) {                         // its timbre was evicted
            v2_ailnat_voice_release_1B11(si);     // 18B5
            wr8n((uint16_t)(si + 0xD95), 0);
        } else {
            wr16n((uint16_t)(si * 2 + 0xD1D), (uint16_t)(po - vlen));
        }
    }
}

// fn9C 18DB(drv, bank, patch, off, seg): install a timbre into the cache.
extern "C" void v2_ailnat_fn9C_load_timbre_18DB(uint16_t bank, uint16_t patch,
                                                uint16_t off, uint16_t seg) {
    uint16_t slot = v2_ailnat_find_slot_15E9(
        (uint16_t)(((bank & 0xFF) << 8) | (patch & 0xFF)));   // 18E7
    if (slot == 0xFFFF) {
        if ((off | seg) == 0) return;             // 1900 null source
        uint16_t di;
        for (;;) {
            for (di = 0; di < 0xC0; di++)         // 190B free slot scan
                if (!(rd8n((uint16_t)(di + 0xC4B)) & 0x80)) break;
            if (di == 0xC0) { nat_timbre_evict_1790(); continue; }    // 1930
            uint16_t len = fr16(seg, off);        // 191F word [src]
            uint16_t end = (uint16_t)(len + rd16n(0xD11));    // 1924
            if (end > rd16n(0xD0F)) { nat_timbre_evict_1790(); continue; }
            uint16_t toff = rd16n(0xD11);         // 1936 xchg
            wr16n(0xD11, end);
            slot = di;
            wr8n((uint16_t)(di + 0xB8B), (uint8_t)patch);     // 1944
            wr8n((uint16_t)(di + 0xACB), (uint8_t)bank);
            wr8n((uint16_t)(di + 0xC4B), 0x80);
            uint32_t lru = (uint32_t)(rd16n(0x647) | ((uint32_t)rd16n(0x649) << 16));
            wr16n(0x647, (uint16_t)(lru + 1));    // 195F (stamp = pre-inc)
            wr16n(0x649, (uint16_t)((lru + 1) >> 16));
            wr16n((uint16_t)(di * 2 + 0x64B), (uint16_t)lru);
            wr16n((uint16_t)(di * 2 + 0x7CB), (uint16_t)(lru >> 16));
            wr16n((uint16_t)(di * 2 + 0x94B), toff);
            uint16_t dsto = (uint16_t)(rd16n(0xD0B) + toff);  // 197A
            uint16_t dsts = rd16n(0xD0D);
            for (uint16_t i = 0; i < len; i++)    // 1981 rep movs
                fw8(dsts, (uint16_t)(dsto + i), fr8(seg, (uint16_t)(off + i)));
            break;
        }
    }
    for (uint16_t di = 0; di < 0x10; di++) {      // 198B relink channels
        if (rd8n((uint16_t)(di + 0x12A9)) != (uint8_t)patch) continue;
        if (rd8n((uint16_t)(di + 0x1299)) != (uint8_t)bank) continue;
        wr8n((uint16_t)(di + 0x1289), (uint8_t)slot);
    }
}

// 2A3D(far, count): find the count-th 'FORM XMID' inside a CAT/FORM file.
// Returns the far pointer through *oseg:*ooff (0:0 = not found).
static void nat_find_xmid_2A3D(uint16_t sg, uint16_t so, uint16_t count,
                               uint16_t* oseg, uint16_t* ooff) {
    uint16_t cx = (uint16_t)(count + 1);          // 2A46
    for (;;) {                                    // 2A4D top-level scan
        uint16_t t0 = fr16(sg, so), t1 = fr16(sg, (uint16_t)(so + 2));
        if ((t0 == 0x4143 && t1 == 0x2054) ||     // 'CA','T '
            (t0 == 0x4F46 && t1 == 0x4D52)) {     // 'FO','RM'
            if (fr16(sg, (uint16_t)(so + 8)) == 0x4D58 &&     // 'XM'
                fr16(sg, (uint16_t)(so + 0xA)) == 0x4449) {   // 'ID'
                // 2AC5: walk the members
                uint32_t rest = ((uint32_t)fr8(sg, (uint16_t)(so + 4)) << 24)
                              | ((uint32_t)fr8(sg, (uint16_t)(so + 5)) << 16)
                              | ((uint32_t)fr8(sg, (uint16_t)(so + 6)) << 8)
                              |  fr8(sg, (uint16_t)(so + 7));
                rest -= 5;                        // 2ACF
                if (fr16(sg, so) == 0x4F46 && fr16(sg, (uint16_t)(so + 2)) == 0x4D52) {
                    if (cx == 1) { *oseg = sg; *ooff = so; return; }  // 2AE8
                    *oseg = 0; *ooff = 0; return;                     // 2AED
                }
                so = (uint16_t)(so + 0xC);        // 2AEF skip the CAT header
                for (;;) {
                    if (fr16(sg, (uint16_t)(so + 8)) == 0x4D58 &&
                        fr16(sg, (uint16_t)(so + 0xA)) == 0x4449) {   // 2AF2
                        cx = (uint16_t)(cx - 1);  // 2B52 loop
                        if (cx == 0) { *oseg = sg; *ooff = so; return; }  // 2B54
                    }
                    uint32_t csz = ((uint32_t)fr8(sg, (uint16_t)(so + 4)) << 24)
                                 | ((uint32_t)fr8(sg, (uint16_t)(so + 5)) << 16)
                                 | ((uint32_t)fr8(sg, (uint16_t)(so + 6)) << 8)
                                 |  fr8(sg, (uint16_t)(so + 7));
                    csz += 8;                     // 2B0A
                    if ((int32_t)(rest - csz) < 0) { *oseg = 0; *ooff = 0; return; }
                    rest -= csz;                  // 2B10
                    uint32_t lin = ((uint32_t)sg << 4) + so + csz;    // 2B18
                    so = (uint16_t)(lin & 0xF);
                    sg = (uint16_t)(lin >> 4);
                }
            }
            // fall through: skip this whole chunk (e.g. FORM XDIR)
        } else { *oseg = 0; *ooff = 0; return; }  // 2A60 unknown tag
        uint32_t sz = ((uint32_t)fr8(sg, (uint16_t)(so + 4)) << 24)   // 2A7B
                    | ((uint32_t)fr8(sg, (uint16_t)(so + 5)) << 16)
                    | ((uint32_t)fr8(sg, (uint16_t)(so + 6)) << 8)
                    |  fr8(sg, (uint16_t)(so + 7));
        sz += 8;                                  // 2A85
        uint32_t lin = ((uint32_t)sg << 4) + so + sz;                 // 2A8B
        so = (uint16_t)(lin & 0xF);
        sg = (uint16_t)(lin >> 4);
    }
}

// 2B67(handle): reset the sequence state block to the XMID defaults.
static void nat_seq_defaults_2B67(uint16_t handle) {
    uint16_t so = rd16n((uint16_t)(handle + 0x292F));
    uint16_t sg = rd16n((uint16_t)(handle + 0x2931));
    for (uint16_t l = 0; l < 4; l++)
        fw16(sg, (uint16_t)(so + 0x60 + 2 * l), 0xFFFF);      // loop stack
    for (int bx = 0xF; bx >= 0; bx--) {           // 2B85
        fw8(sg, (uint16_t)(so + 0x68 + bx), (uint8_t)bx);     // identity map
        fw8(sg, (uint16_t)(so + 0x78 + bx), 0xFF);
        fw8(sg, (uint16_t)(so + 0x88 + bx), 0xFF);
        fw8(sg, (uint16_t)(so + 0x98 + bx), 0xFF);
        fw8(sg, (uint16_t)(so + 0xA8 + bx), 0xFF);
    }
    for (int bx = 0x8F; bx >= 0; bx--)            // 2BA1 controller rows
        fw8(sg, (uint16_t)(so + 0xB8 + bx), 0xFF);
    for (int bx = 0x1F; bx >= 0; bx--)            // 2BAC deferred notes
        fw8(sg, (uint16_t)(so + 0x148 + bx), 0xFF);
    fw16(sg, (uint16_t)(so + 0x10), 0xFFFF);      // 2BB7 callback value
    fw16(sg, (uint16_t)(so + 0x1E), 0);
    fw16(sg, (uint16_t)(so + 0x20), 0);
    fw16(sg, (uint16_t)(so + 0x24), 0x64);        // volume 100
    fw16(sg, (uint16_t)(so + 0x26), 0x64);
    fw16(sg, (uint16_t)(so + 0x32), 0x64);        // tempo 100%
    fw16(sg, (uint16_t)(so + 0x34), 0x64);
    fw16(sg, (uint16_t)(so + 0x30), 0);
    fw16(sg, (uint16_t)(so + 0x3E), 0);
    fw16(sg, (uint16_t)(so + 0x40), 0);
    fw16(sg, (uint16_t)(so + 0x48), 0x08D5);      // 2BE9 one beat interval
    fw16(sg, (uint16_t)(so + 0x4A), 0x0002);
    fw16(sg, (uint16_t)(so + 0x42), 4);           // 4/4
    fw16(sg, (uint16_t)(so + 0x44), 0x08D5);
    fw16(sg, (uint16_t)(so + 0x46), 0x0002);
    fw16(sg, (uint16_t)(so + 0x4C), 0x1200);      // 500000us<<4 (120 bpm)
    fw16(sg, (uint16_t)(so + 0x4E), 0x007A);
}

// fn97 37F6(drv, seq_off, seq_seg, index, state_off, state_seg, tbl_off,
// tbl_seg): register a sequence — find the XMID, bind the state block,
// locate TIMB/RBRN/EVNT and initialize. Returns the handle (0xFFFF = none).
extern "C" uint16_t v2_ailnat_fn97_register_37F6(uint16_t seq_off, uint16_t seq_seg,
                                                 uint16_t index,
                                                 uint16_t st_off, uint16_t st_seg,
                                                 uint16_t tbl_off, uint16_t tbl_seg) {
    uint16_t handle = 0xFFFF;                     // 3801 free handle scan
    for (uint16_t bx = 0, cx = 8; cx; bx += 4, cx--)
        if (rd16n((uint16_t)(bx + 0x2931)) == 0) { handle = bx; break; }
    if (handle == 0xFFFF) return 0xFFFF;          // 3814
    uint16_t xg = 0, xo = 0;
    nat_find_xmid_2A3D(seq_seg, seq_off, index, &xg, &xo);    // 3827
    if (xg == 0) return 0xFFFF;                   // 382D
    uint16_t si = st_off, sgm = st_seg;           // 3843 lds [bp+0xE]
    wr16n((uint16_t)(handle + 0x292F), si);
    wr16n((uint16_t)(handle + 0x2931), sgm);
    fw16(sgm, (uint16_t)(si + 0x2), 0);           // 3850
    fw16(sgm, (uint16_t)(si + 0x6), 0);
    fw16(sgm, (uint16_t)(si + 0xA), 0);
    uint32_t pos = 0xC;                           // [bp-0x4] first member
    for (;;) {                                    // 385F chunk walk
        uint32_t lin = ((uint32_t)xg << 4) + xo + pos;
        uint16_t co = (uint16_t)(lin & 0xF);
        uint16_t cg = (uint16_t)(lin >> 4);
        uint16_t t0 = fr16(cg, co), t1 = fr16(cg, (uint16_t)(co + 2));
        uint32_t csz = ((uint32_t)fr8(cg, (uint16_t)(co + 6)) << 8)   // 3899
                     |  fr8(cg, (uint16_t)(co + 7))
                     | ((uint32_t)fr8(cg, (uint16_t)(co + 5)) << 16)
                     | ((uint32_t)fr8(cg, (uint16_t)(co + 4)) << 24);
        pos += csz + 8;                           // 38A5 next member
        if (t0 == 0x4954 && t1 == 0x424D) {       // 'TI','MB'
            fw16(sgm, si, co);                    // 38C0
            fw16(sgm, (uint16_t)(si + 0x2), cg);
        } else if (t0 == 0x4252 && t1 == 0x4E52) {// 'RB','RN'
            fw16(sgm, (uint16_t)(si + 0x4), co);  // 38D6
            fw16(sgm, (uint16_t)(si + 0x6), cg);
        } else if (t0 == 0x5645 && t1 == 0x544E) {// 'EV','NT'
            fw16(sgm, (uint16_t)(si + 0x16), handle);         // 38F3
            fw16(sgm, (uint16_t)(si + 0x8), co);
            fw16(sgm, (uint16_t)(si + 0xA), cg);
            fw16(sgm, (uint16_t)(si + 0x12), tbl_off);        // 38FF
            fw16(sgm, (uint16_t)(si + 0x14), tbl_seg);
            fw16(sgm, (uint16_t)(si + 0x1C), 0);
            fw16(sgm, (uint16_t)(si + 0x18), 0);
            fw16(sgm, (uint16_t)(si + 0x1A), 0);
            wr16n(0x294F, (uint16_t)(rd16n(0x294F) + 1));     // 3917
            nat_seq_defaults_2B67(handle);        // 3920
            return handle;                        // 3926
        }
    }
}

// fn AA 3980(drv, handle): start — stop if playing, defaults, cursor to
// EVNT data (+8, normalized), status = playing.
extern "C" void v2_ailnat_fnAA_start_3980(uint16_t handle);
extern "C" void v2_ailnat_fnAB_stop_3A15(uint16_t handle) {
    if (handle == 0xFFFF) return;
    if (rd16n((uint16_t)(handle + 0x2931)) == 0) return;      // 3A25
    uint16_t so = rd16n((uint16_t)(handle + 0x292F));
    uint16_t sg = rd16n((uint16_t)(handle + 0x2931));
    if (fr16(sg, (uint16_t)(so + 0x1A)) != 1) return;         // 3A32
    v2_ailnat_seq_flush_notes_2CA0(sg, so);       // 3A3B
    v2_ailnat_seq_cleanup_2CF4(sg, so);           // 3A44
    fw16(sg, (uint16_t)(so + 0x1A), 0);           // 3A4A stopped
}
extern "C" void v2_ailnat_fnAA_start_3980(uint16_t handle) {
    if (handle == 0xFFFF) return;
    uint16_t so = rd16n((uint16_t)(handle + 0x292F));
    uint16_t sg = rd16n((uint16_t)(handle + 0x2931));
    if (fr16(sg, (uint16_t)(so + 0x1A)) == 1)     // 3995 playing
        v2_ailnat_fnAB_stop_3A15(handle);         // 39A9
    nat_seq_defaults_2B67(handle);                // 39B3
    uint32_t lin = ((uint32_t)fr16(sg, (uint16_t)(so + 0xA)) << 4)
                 + fr16(sg, (uint16_t)(so + 0x8)) + 8;        // 39B9 EVNT+8
    fw16(sg, (uint16_t)(so + 0xC), (uint16_t)(lin & 0xF));
    fw16(sg, (uint16_t)(so + 0xE), (uint16_t)(lin >> 4));
    fw16(sg, (uint16_t)(so + 0x1A), 1);           // 39FF playing
    fw16(sg, (uint16_t)(so + 0x18), 1);           // was-started
}

// 2DA7(seq): resume support — re-lock remapped channels, replay the
// per-sequence controller shadows and program/pitch images.
static void nat_seq_reassert_2DA7(uint16_t sg, uint16_t so) {
    for (uint16_t di = 0; di < 0x10; di++) {      // 2DB3 channel locks
        uint8_t l = fr8(sg, (uint16_t)(so + 0x118 + di));
        if (l == 0xFF) continue;
        if ((int8_t)l < 0x40) continue;
        uint16_t r = v2_ailnat_lock_channel_3E20();           // 2DCF
        r = (uint16_t)(r - 1);
        if (r == 0xFFFF) r = di;
        fw8(sg, (uint16_t)(so + 0x68 + di), (uint8_t)r);
    }
    for (uint16_t row = 0; row < 9; row++) {      // 2DE8 controller replay
        uint8_t ctl = rd8n((uint16_t)(row + 0x2814));
        if (ctl == 0x6E) continue;                // 2DF5 remap done above
        uint8_t roff = rd8n((uint16_t)(ctl + 0x282F));
        for (uint16_t di = 0; di < 0x10; di++) {
            uint8_t v = fr8(sg, (uint16_t)(so + 0xB8 + roff + di));
            if (v == 0xFF) continue;
            v2_ailnat_xmidi_ctl_2EE9(sg, so, di, ctl, v);     // 2E1D
        }
    }
    for (uint16_t di = 0; di < 0x10; di++) {      // 2E32 pitch + program
        uint8_t lo = fr8(sg, (uint16_t)(so + 0x88 + di));
        if (lo != 0xFF) {
            uint8_t hi = fr8(sg, (uint16_t)(so + 0x98 + di));
            if (hi != 0xFF) {
                uint16_t st = (uint16_t)(fr8(sg, (uint16_t)(so + 0x68 + di)) | 0xE0);
                v2_ailnat_midi_2629(st, lo, hi);  // 2E53
            }
        }
        uint8_t pr = fr8(sg, (uint16_t)(so + 0x78 + di));
        if (pr != 0xFF) {
            uint16_t st = (uint16_t)(fr8(sg, (uint16_t)(so + 0x68 + di)) | 0xC0);
            v2_ailnat_midi_2629(st, pr, 0);       // 2E77
        }
    }
}

// fn AD 3A5B(drv, handle): resume a stopped-but-started sequence.
extern "C" void v2_ailnat_fnAD_resume_3A5B(uint16_t handle) {
    if (handle == 0xFFFF) return;
    if (rd16n((uint16_t)(handle + 0x2931)) == 0) return;
    uint16_t so = rd16n((uint16_t)(handle + 0x292F));
    uint16_t sg = rd16n((uint16_t)(handle + 0x2931));
    if (fr16(sg, (uint16_t)(so + 0x1A)) != 0) return;         // 3A78
    if (fr16(sg, (uint16_t)(so + 0x18)) == 0) return;         // 3A7E
    nat_seq_reassert_2DA7(sg, so);                // 3A87
    fw16(sg, (uint16_t)(so + 0x1A), 1);
}

// fn AE 3A9E(drv, handle): status word.
extern "C" uint16_t v2_ailnat_fnAE_status_3A9E(uint16_t handle) {
    if (handle == 0xFFFF) return 0xFFFF;          // ax unchanged in orig
    uint16_t so = rd16n((uint16_t)(handle + 0x292F));
    uint16_t sg = rd16n((uint16_t)(handle + 0x2931));
    return fr16(sg, (uint16_t)(so + 0x1A));
}

// fn B0 3C31 / fn AF 3C57: tempo percent / volume getters.
extern "C" uint16_t v2_ailnat_fnB0_get_tempo_3C31(uint16_t handle) {
    if (handle == 0xFFFF) return 0xFFFF;
    uint16_t so = rd16n((uint16_t)(handle + 0x292F));
    uint16_t sg = rd16n((uint16_t)(handle + 0x2931));
    return fr16(sg, (uint16_t)(so + 0x32));
}
extern "C" uint16_t v2_ailnat_fnAF_get_volume_3C57(uint16_t handle) {
    if (handle == 0xFFFF) return 0xFFFF;
    uint16_t so = rd16n((uint16_t)(handle + 0x292F));
    uint16_t sg = rd16n((uint16_t)(handle + 0x2931));
    return fr16(sg, (uint16_t)(so + 0x24));
}

// 32D3(lo, hi, div_lo, div_hi): unsigned 32/32 divide (shift-subtract),
// with the 16-bit DIV fast path of the original.
static uint32_t nat_udiv32_32D3(uint32_t dividend, uint32_t divisor) {
    uint16_t ax = (uint16_t)dividend, dx = (uint16_t)(dividend >> 16);
    uint16_t bx = (uint16_t)divisor,  cx = (uint16_t)(divisor >> 16);
    if (cx == 0 && (dx == 0 || bx == 0)) {        // 32E5..32EF
        // div bx (16-bit): quotient must fit — in-domain dx < bx
        uint32_t q = ((uint32_t)dx << 16 | ax) / bx;
        return (uint16_t)q;                       // dx = 0
    }
    uint16_t si = 0, di = 0;                      // remainder
    for (int i = 0; i < 0x20; i++) {              // 32F3
        uint32_t acc = ((uint32_t)dx << 16) | ax;
        uint32_t rem = ((uint32_t)di << 16) | si;
        uint32_t nacc = (acc << 1) & 0xFFFFFFFFu;
        rem = ((rem << 1) | (acc >> 31)) & 0xFFFFFFFFu;
        ax = (uint16_t)nacc; dx = (uint16_t)(nacc >> 16);
        si = (uint16_t)rem;  di = (uint16_t)(rem >> 16);
        if (rem >= divisor) {                     // 3302..330A
            rem -= divisor;
            si = (uint16_t)rem; di = (uint16_t)(rem >> 16);
            ax = (uint16_t)(ax + 1);              // 3310 quotient bit
        }
    }
    return ((uint32_t)dx << 16) | ax;
}

// fn B2 3C7D / fn B1 3CF1: set tempo / volume target with a millisecond
// ramp (step = 10*ms / |diff| through the shared divider).
extern "C" void v2_ailnat_fnB2_set_tempo_3C7D(uint16_t handle, uint16_t target,
                                              uint16_t ms) {
    if (handle == 0xFFFF) return;
    uint16_t so = rd16n((uint16_t)(handle + 0x292F));
    uint16_t sg = rd16n((uint16_t)(handle + 0x2931));
    fw16(sg, (uint16_t)(so + 0x34), target);      // 3C95
    if (ms == 0) { fw16(sg, (uint16_t)(so + 0x32), target); return; }   // 3CE2
    int16_t diff = (int16_t)(target - fr16(sg, (uint16_t)(so + 0x32)));
    if (diff == 0) return;                        // 3CA4
    uint16_t ad = (uint16_t)(diff < 0 ? -diff : diff);        // cwd/xor/sub
    uint32_t q = nat_udiv32_32D3((uint32_t)10 * ms, ad);      // 3CC1
    if (q == 0) q = 1;                            // 3CCB
    fw16(sg, (uint16_t)(so + 0x3A), (uint16_t)q);
    fw16(sg, (uint16_t)(so + 0x3C), (uint16_t)(q >> 16));
    fw16(sg, (uint16_t)(so + 0x36), 0);
    fw16(sg, (uint16_t)(so + 0x38), 0);
}
extern "C" void v2_ailnat_fnB1_set_volume_3CF1(uint16_t handle, uint16_t target,
                                               uint16_t ms) {
    if (handle == 0xFFFF) return;
    uint16_t so = rd16n((uint16_t)(handle + 0x292F));
    uint16_t sg = rd16n((uint16_t)(handle + 0x2931));
    fw16(sg, (uint16_t)(so + 0x26), target);      // 3D09
    if (ms == 0) {                                // 3D56 instant
        fw16(sg, (uint16_t)(so + 0x24), target);
        v2_ailnat_seq_volume_2E8A(sg, so);
        return;
    }
    int16_t diff = (int16_t)(target - fr16(sg, (uint16_t)(so + 0x24)));
    if (diff == 0) return;
    uint16_t ad = (uint16_t)(diff < 0 ? -diff : diff);
    uint32_t q = nat_udiv32_32D3((uint32_t)10 * ms, ad);      // 3D35
    if (q == 0) q = 1;
    fw16(sg, (uint16_t)(so + 0x2C), (uint16_t)q);
    fw16(sg, (uint16_t)(so + 0x2E), (uint16_t)(q >> 16));
    fw16(sg, (uint16_t)(so + 0x28), 0);
    fw16(sg, (uint16_t)(so + 0x2A), 0);
}

// ---------------------------------------------------------------------------
// Non-game fns (task #102) — the rest of the blob's fn table, ported for
// 100% surface coverage.
// ---------------------------------------------------------------------------
// fn96 3797: sequence state block size (the 0x208-byte struct).
extern "C" uint16_t v2_ailnat_fn96_state_size_3797(void) { return 0x208; }

// fnBD 37AE(drv, cb_off, cb_seg): install the XMIDI trigger callback.
// The blob stores the CALLER's DS at cs:0x2955 — in the interpreter world
// that is the fn-frame ds (the blob paragraph), so the judged image carries
// the same convention through caller_ds.
extern "C" void v2_ailnat_fnBD_set_cb_37AE(uint16_t cb_off, uint16_t cb_seg,
                                           uint16_t caller_ds) {
    wr16n(0x2955, caller_ds);                     // 37B6 mov cs:0x2955,ds
    wr16n(0x2957, cb_off);
    wr16n(0x2959, cb_seg);
}

// fnBB 280A / fnBC 280F: empty stubs in the blob (push bp; pop bp; retf).
extern "C" void v2_ailnat_fnBB_stub_280A(void) {}
extern "C" void v2_ailnat_fnBC_stub_280F(void) {}

// 15D0: OPL3 disable (NEW bit off).
static void nat_opl3_off_15D0(void) {
    nat_out_tail_13DE(0x05, 0x01, 0x00);          // 15D5 bx=0x105, cl=0
}

// fn68 3709(drv, x, y): uninstall — stop+release every registered sequence,
// re-init the register file, the (stubbed) BC call, OPL3 off, installed=0.
extern "C" void v2_ailnat_fn68_uninstall_3709(uint16_t x, uint16_t y) {
    (void)x; (void)y;
    if (rd16n(0x2A3B) == 0) return;               // 3714 not installed
    uint16_t left = rd16n(0x294F);                // 3721
    if (left != 0) {
        uint16_t bx = 0;
        for (;;) {
            uint16_t cur = bx; bx = (uint16_t)(bx + 4);
            if (rd16n((uint16_t)(cur + 0x2931)) == 0) continue;
            v2_ailnat_fnAB_stop_3A15(cur);        // 3746
            v2_ailnat_release_handle_393C(cur);   // 3758
            left = (uint16_t)(left - 1);          // 375E
            if (left == 0) break;
        }
    }
    nat_opl3_init_1582();                         // 3763 silence the chip
    v2_ailnat_fnBC_stub_280F();                   // 3778 (no-op in this blob)
    nat_opl3_off_15D0();                          // 377F
    wr16n(0x2A3B, 0);                             // 3782
}

// fn9D 161C / fn9E 1662: lock / unlock a cached timbre (bank,patch);
// (0xFF,0xFF) applies to every slot.
extern "C" void v2_ailnat_fn9D_lock_161C(uint16_t bank, uint16_t patch) {
    uint16_t key = (uint16_t)(((bank & 0xFF) << 8) | (patch & 0xFF));
    if (key == 0xFFFF) {                          // 162A all slots
        for (uint16_t bx = 0; bx < 0xC0; bx++)
            wr8n((uint16_t)(bx + 0xC4B), (uint8_t)(rd8n((uint16_t)(bx + 0xC4B)) | 0x40));
        return;
    }
    uint16_t r = v2_ailnat_find_slot_15E9(key);
    if (r == 0xFFFF) return;
    wr8n((uint16_t)(r + 0xC4B), (uint8_t)(rd8n((uint16_t)(r + 0xC4B)) | 0x40));
}
extern "C" void v2_ailnat_fn9E_unlock_1662(uint16_t bank, uint16_t patch) {
    uint16_t key = (uint16_t)(((bank & 0xFF) << 8) | (patch & 0xFF));
    if (key == 0xFFFF) {
        for (uint16_t bx = 0; bx < 0xC0; bx++)
            wr8n((uint16_t)(bx + 0xC4B), (uint8_t)(rd8n((uint16_t)(bx + 0xC4B)) & 0xBF));
        return;
    }
    uint16_t r = v2_ailnat_find_slot_15E9(key);
    if (r == 0xFFFF) return;
    wr8n((uint16_t)(r + 0xC4B), (uint8_t)(rd8n((uint16_t)(r + 0xC4B)) & 0xBF));
}

// fn9F 16A8: cache offset of a loaded timbre, plus one (0 = not cached).
extern "C" uint16_t v2_ailnat_fn9F_cache_off_16A8(uint16_t bank, uint16_t patch) {
    uint16_t key = (uint16_t)(((bank & 0xFF) << 8) | (patch & 0xFF));
    uint16_t r = v2_ailnat_find_slot_15E9(key);   // 16B8
    uint16_t ax = r;
    if (r != 0xFFFF) ax = rd16n((uint16_t)(r * 2 + 0x94B));   // 16C7
    return (uint16_t)(ax + 1);                    // 16CC inc ax
}

// fnB3 3AC4 / fnB4 3AEA: metronome beat / measure getters.
extern "C" uint16_t v2_ailnat_fnB3_get_beat_3AC4(uint16_t handle) {
    if (handle == 0xFFFF) return 0xFFFF;
    uint16_t so = rd16n((uint16_t)(handle + 0x292F));
    uint16_t sg = rd16n((uint16_t)(handle + 0x2931));
    return fr16(sg, (uint16_t)(so + 0x3E));
}
extern "C" uint16_t v2_ailnat_fnB4_get_measure_3AEA(uint16_t handle) {
    if (handle == 0xFFFF) return 0xFFFF;
    uint16_t so = rd16n((uint16_t)(handle + 0x292F));
    uint16_t sg = rd16n((uint16_t)(handle + 0x2931));
    return fr16(sg, (uint16_t)(so + 0x40));
}

// fnC0 3B10(drv, handle, ch, newch) / fnC2 3B3C(drv, handle, ch): channel
// map setter / getter (1-based channels on the API side).
extern "C" void v2_ailnat_fnC0_set_map_3B10(uint16_t handle, uint16_t ch,
                                            uint16_t newch) {
    if (handle == 0xFFFF) return;
    uint16_t so = rd16n((uint16_t)(handle + 0x292F));
    uint16_t sg = rd16n((uint16_t)(handle + 0x2931));
    fw8(sg, (uint16_t)(so + 0x68 + (uint16_t)(ch - 1)), (uint8_t)(newch - 1));
}
extern "C" uint16_t v2_ailnat_fnC2_get_map_3B3C(uint16_t handle, uint16_t ch) {
    if (handle == 0xFFFF) return 0xFFFF;
    uint16_t so = rd16n((uint16_t)(handle + 0x292F));
    uint16_t sg = rd16n((uint16_t)(handle + 0x2931));
    return (uint16_t)(fr8(sg, (uint16_t)(so + 0x68 + (uint16_t)(ch - 1))) + 1);
}

// fnB5 3B69(drv, handle, branch_id): XMIDI branch — look the id up in the
// RBRN chunk, point the event cursor at the target (offset+8 inside EVNT,
// normalized), reset the delta, flush the pending notes, clear the loops.
extern "C" void v2_ailnat_fnB5_branch_3B69(uint16_t handle, uint16_t id) {
    if (handle == 0xFFFF) return;
    uint16_t so = rd16n((uint16_t)(handle + 0x292F));
    uint16_t sg = rd16n((uint16_t)(handle + 0x2931));
    if (fr16(sg, (uint16_t)(so + 0x6)) == 0) return;          // 3B81 no RBRN
    uint16_t ro = fr16(sg, (uint16_t)(so + 0x4));
    uint16_t rg = fr16(sg, (uint16_t)(so + 0x6));
    if (fr16(rg, ro) != 0x4252) return;           // 'RB'
    if (fr16(rg, (uint16_t)(ro + 0x2)) != 0x4E52) return;     // 'RN'
    uint16_t cx = fr16(rg, (uint16_t)(ro + 0x8)); // entry count
    uint16_t di = (uint16_t)(ro + 0xA);           // 3BA6
    for (;;) {
        if (fr8(rg, di) == (uint8_t)id) break;    // 3BAC
        di = (uint16_t)(di + 6);
        cx = (uint16_t)(cx - 1);
        if (cx == 0) return;                      // 3BB4 loop exhausted
    }
    uint32_t off = (uint32_t)(fr16(rg, (uint16_t)(di + 0x2))
                 | ((uint32_t)fr16(rg, (uint16_t)(di + 0x4)) << 16));
    off += 8;                                     // 3BC0
    uint32_t lin = ((uint32_t)fr16(sg, (uint16_t)(so + 0xA)) << 4)
                 + fr16(sg, (uint16_t)(so + 0x8)) + off;      // 3BC6 EVNT base
    fw16(sg, (uint16_t)(so + 0xC), (uint16_t)(lin & 0xF));    // 3C01
    fw16(sg, (uint16_t)(so + 0xE), (uint16_t)(lin >> 4));
    fw16(sg, (uint16_t)(so + 0x1E), 0);           // 3C07
    v2_ailnat_seq_flush_notes_2CA0(sg, so);       // 3C0F
    for (uint16_t l = 0; l < 4; l++)              // 3C15 loop stack
        fw16(sg, (uint16_t)(so + 0x60 + 2 * l), 0xFFFF);
}

// fnB6 3D6E(drv, handle, ch, ctl): controller shadow getter (sign-extended;
// 0x77 reads the callback value cell).
extern "C" uint16_t v2_ailnat_fnB6_get_ctl_3D6E(uint16_t handle, uint16_t ch,
                                                uint16_t ctl) {
    if (handle == 0xFFFF) return 0xFFFF;
    uint16_t so = rd16n((uint16_t)(handle + 0x292F));
    uint16_t sg = rd16n((uint16_t)(handle + 0x2931));
    if (ctl == 0x77)                              // 3D88
        return fr16(sg, (uint16_t)(so + 0x10));
    uint8_t row = rd8n((uint16_t)((ctl & 0xFF) + 0x282F));    // 3D92
    if (row == 0xFF) return 0xFFFF;               // ax unchanged (0xFFFF)
    uint16_t bx = (uint16_t)((uint16_t)(row + ch) - 1);       // 3D9C..3D9F
    return (uint16_t)(int16_t)(int8_t)fr8(sg, (uint16_t)(so + 0xB8 + bx)); // cbw
}

// fnB7 3DB1(drv, handle, ch, ctl, val): controller setter through the
// XMIDI interceptor (channel is 1-based on the API side).
extern "C" void v2_ailnat_fnB7_set_ctl_3DB1(uint16_t handle, uint16_t ch,
                                            uint16_t ctl, uint16_t val) {
    if (handle == 0xFFFF) return;
    uint16_t so = rd16n((uint16_t)(handle + 0x292F));
    uint16_t sg = rd16n((uint16_t)(handle + 0x2931));
    v2_ailnat_xmidi_ctl_2EE9(sg, so, (uint16_t)(ch - 1), ctl, val);   // 3DD4
}

// fnB9 3DE6(drv, handle, ch): count the deferred notes of a channel.
extern "C" uint16_t v2_ailnat_fnB9_note_count_3DE6(uint16_t handle, uint16_t ch) {
    if (handle == 0xFFFF) return 0xFFFF;
    uint16_t so = rd16n((uint16_t)(handle + 0x292F));
    uint16_t sg = rd16n((uint16_t)(handle + 0x2931));
    uint16_t ax = 0;
    uint8_t cl = (uint8_t)(ch - 1);               // 3E06
    for (uint16_t bx = 0; bx < 0x20; bx++)
        if (fr8(sg, (uint16_t)(so + 0x148 + bx)) == cl) ax = (uint16_t)(ax + 1);
    return ax;
}

// ---------------------------------------------------------------------------
// Self-test vs the interpreter (armed by V2_AILNAT_SELFTEST=1 after boot).
// ---------------------------------------------------------------------------
extern "C" uint16_t v2_ail_interp_call(uint16_t fn_off, const uint16_t* args, int argc);
extern "C" uint8_t* v2_ail_interp_data_raw(uint32_t* size_out);
extern "C" void v2_ail_interp_set_io_hooks(void (*o)(uint16_t, uint8_t), uint8_t (*i)(uint16_t));
extern "C" void v2_ail_interp_get_io_hooks(void (**o)(uint16_t, uint8_t), uint8_t (**i)(uint16_t));

// OUT capture buffers (interp vs native)
static struct { uint16_t port; uint8_t val; } g_cap[2][4096];
static int g_capn[2]; static int g_capw = 0;
static void cap_out(uint16_t port, uint8_t val) {
    if (g_capn[g_capw] < 4096) g_cap[g_capw][g_capn[g_capw]++] = { port, val };
}
static uint8_t cap_in(uint16_t) { return 0; }

// far fixture: the unit sweeps point cs:0xD0B into the blob arena itself,
// so the native far view IS the native image at the same offsets. Sequence
// fixtures live in a separate 64K "game memory" segment range (para
// SEQFIX_PARA..+0xFFF, resolved linearly like the interp's mapped ranges) —
// one copy per executor so the judged runs can't contaminate each other.
static uint16_t g_fix_seg = 0;
static const uint16_t SEQFIX_PARA = 0xE000;
static uint8_t g_seqfix_i[0x10000];   // mapped into the interpreter
static uint8_t g_seqfix_n[0x10000];   // the native far view
static uint8_t nat_far_fixture_rd(uint16_t seg, uint16_t off) {
    if (seg == g_fix_seg) return g_nat[off];
    uint32_t d = (uint32_t)(uint16_t)(seg - SEQFIX_PARA) << 4;
    if (seg >= SEQFIX_PARA && d + off < sizeof(g_seqfix_n))
        return g_seqfix_n[d + off];
    return 0;
}
static void nat_far_fixture_wr(uint16_t seg, uint16_t off, uint8_t val) {
    uint32_t d = (uint32_t)(uint16_t)(seg - SEQFIX_PARA) << 4;
    if (seg >= SEQFIX_PARA && d + off < sizeof(g_seqfix_n))
        g_seqfix_n[d + off] = val;
}

static uint32_t xr = 0x12345678;   // deterministic LCG for the sweeps
static uint32_t xrnd() { xr = xr * 1103515245u + 12345u; return xr >> 8; }

extern "C" int v2_ailnat_selftest(void) {
    uint32_t isz = 0;
    uint8_t* idata = v2_ail_interp_data_raw(&isz);
    if (!idata || !isz) { fprintf(stderr, "AILNAT: no interp data\n"); return 0; }
    v2_ailnat_load(idata, isz);
    // the sweeps randomize live driver state — snapshot the whole image and
    // restore it at the end so the diagnostic run stays non-destructive
    // The shadow blob lives in a full 64K arena (drv_copy) even though the
    // file itself is shorter — snapshot the whole arena so far fixtures
    // planted above the blob are part of the judged image too.
    static uint8_t isave[0x10000];
    uint32_t isave_n = 0x10000;
    memcpy(isave, idata, isave_n);

    void (*old_o)(uint16_t, uint8_t); uint8_t (*old_i)(uint16_t);
    v2_ail_interp_get_io_hooks(&old_o, &old_i);
    int fails = 0, cases = 0;

    // --- 13A8 / 13C4: OUT-stream equality over randomized args -------------
    for (int t = 0; t < 512; t++) {
        uint8_t op = (uint8_t)(xrnd() % 0x24);      // operator/voice index domain
        uint8_t rb = (uint8_t)(xrnd() & 0xFF);
        uint8_t vl = (uint8_t)(xrnd() & 0xFF);
        int use_voice = t & 1;
        g_capn[0] = g_capn[1] = 0;
        g_capw = 0;
        v2_ail_interp_set_io_hooks(cap_out, cap_in);
        {
            uint16_t args[3] = { op, rb, vl };
            v2_ail_interp_call(use_voice ? 0x13C4 : 0x13A8, args, 3);
        }
        v2_ail_interp_set_io_hooks(old_o, old_i);
        g_capw = 1;
        nat_out_fn so = g_nat_out; nat_in_fn si_ = g_nat_in;
        g_nat_out = cap_out; g_nat_in = cap_in;
        if (use_voice) v2_ailnat_opl_voice_write_13C4(op, rb, vl);
        else           v2_ailnat_opl_op_write_13A8(op, rb, vl);
        g_nat_out = so; g_nat_in = si_;
        cases++;
        if (g_capn[0] != g_capn[1] ||
            memcmp(g_cap[0], g_cap[1], sizeof(g_cap[0][0]) * g_capn[0]) != 0) {
            fails++;
            if (fails <= 4)
                fprintf(stderr, "AILNAT-DIFF %s op=%02X rb=%02X vl=%02X: "
                        "i[%d]=%03X/%02X n[%d]=%03X/%02X\n",
                        use_voice ? "13C4" : "13A8", op, rb, vl,
                        g_capn[0], g_cap[0][0].port, g_cap[0][0].val,
                        g_capn[1], g_cap[1][0].port, g_cap[1][0].val);
        }
    }

    // --- 15E9: slot search over randomized tables --------------------------
    for (int t = 0; t < 512; t++) {
        for (int s = 0; s < 0xC0; s++) {
            idata[0xC4B + s] = (uint8_t)(xrnd() & 0xFF);
            idata[0xACB + s] = (uint8_t)(xrnd() & 0x0F);
            idata[0xB8B + s] = (uint8_t)(xrnd() & 0x7F);
        }
        memcpy(g_nat + 0xC4B, idata + 0xC4B, 0xC0);
        memcpy(g_nat + 0xACB, idata + 0xACB, 0xC0);
        memcpy(g_nat + 0xB8B, idata + 0xB8B, 0xC0);
        uint16_t ax = (uint16_t)(((xrnd() & 0x0F) << 8) | (xrnd() & 0x7F));
        uint16_t args[1] = { ax };
        uint16_t ri = v2_ail_interp_call(0x15E9, args, 1);
        uint16_t rn = v2_ailnat_find_slot_15E9(ax);
        cases++;
        if (ri != rn) {
            fails++;
            if (fails <= 4)
                fprintf(stderr, "AILNAT-DIFF 15E9 ax=%04X: i=%04X n=%04X\n", ax, ri, rn);
        }
    }

    // --- 1B7C: register updater over randomized slot/channel/layer state ---
    for (int t = 0; t < 512; t++) {
        uint16_t sl = (uint16_t)(xrnd() % 0x14);
        memcpy(idata, isave, isave_n);   // hermetic case: pristine code+data
        // slot-state region (0xD95..0xE98: mode, voice, channel word, note,
        // transpose, B0 image, key image, dirty mask, controller offsets...)
        for (uint32_t a = 0xD95; a < 0xE99; a++) idata[a] = (uint8_t)(xrnd() & 0xFF);
        // both parameter layers + the per-channel controller rows
        for (uint32_t a = 0xE99; a < 0x1269; a++) idata[a] = (uint8_t)(xrnd() & 0xFF);
        idata[0xD1C] = (uint8_t)(xrnd() & 0xFF);       // 4-op connection image
        // voice byte: mostly the real domain 0..8; 0xFF = early exit; raw
        // bytes cover the out-of-domain reads too (identical on both sides)
        uint32_t vr = xrnd();
        if ((t & 7) == 0)      idata[0xDBD + sl] = 0xFF;
        else if ((t & 7) == 1) idata[0xDBD + sl] = (uint8_t)(vr & 0xFF);
        else                   idata[0xDBD + sl] = (uint8_t)(vr % 9);
        idata[0xDA9 + sl] = (uint8_t)(xrnd() & 0x3);   // voice mode 0..3
        memcpy(g_nat, idata, isave_n);                 // identical full images
        g_capn[0] = g_capn[1] = 0;
        g_capw = 0;
        v2_ail_interp_set_io_hooks(cap_out, cap_in);
        { uint16_t args[1] = { sl }; v2_ail_interp_call(0x1B7C, args, 1); }
        v2_ail_interp_set_io_hooks(old_o, old_i);
        g_capw = 1;
        nat_out_fn so = g_nat_out; nat_in_fn si_ = g_nat_in;
        g_nat_out = cap_out; g_nat_in = cap_in;
        v2_ailnat_update_voice_1B7C(sl);
        g_nat_out = so; g_nat_in = si_;
        cases++;
        int bad = (g_capn[0] != g_capn[1] ||
                   memcmp(g_cap[0], g_cap[1], sizeof(g_cap[0][0]) * g_capn[0]) != 0);
        // whole-image equality (code included) — in-domain writes must match
        if (!bad) bad = memcmp(g_nat, idata, isave_n) != 0;
        if (bad) {
            fails++;
            if (fails <= 4)
                fprintf(stderr, "AILNAT-DIFF 1B7C sl=%u outs i=%d n=%d mask i=%02X n=%02X e5d i=%02X n=%02X\n",
                        sl, g_capn[0], g_capn[1],
                        idata[0xE49 + sl], g_nat[0xE49 + sl],
                        idata[0xE5D + sl], g_nat[0xE5D + sl]);
        }
    }

    // --- 1A5A / 1B11 / 2155: voice management over randomized rings --------
    // Domain = the driver's own invariants (24E4/1A5A only ever produce
    // these): channel byte 0..15, voices 0..0x11 or 0xFF. Out-of-domain
    // bytes would aim cs:[ch+0x1339]/cs:[v+0x1349] writes into blob CODE —
    // the interp then executes the corrupted bytes while the native port
    // doesn't run from the image, so the judged outcomes can't stay equal.
    for (int t = 0; t < 512; t++) {
        memcpy(idata, isave, isave_n);   // hermetic case: pristine code+data
        // both parameter layers + per-channel controller rows
        for (uint32_t a = 0xE99; a < 0x1269; a++) idata[a] = (uint8_t)(xrnd() & 0xFF);
        idata[0xD1C] = (uint8_t)(xrnd() & 0xFF);
        // ring cursors + channel counts + voice occupancy + score rows
        idata[0xD18] = (uint8_t)(xrnd() % 0x12); idata[0xD19] = 0;
        idata[0xD1A] = (uint8_t)(xrnd() % 0x6);  idata[0xD1B] = 0;
        for (uint32_t a = 0x1339; a < 0x1390; a++) idata[a] = (uint8_t)(xrnd() & 0xFF);
        // slot rows: active, voice, mode, channel (low nibble), note,
        // transpose, velocity, sustain, dirty mask, B0/key/stereo images
        for (uint16_t sl2 = 0; sl2 < 0x14; sl2++) {
            idata[0xD95 + sl2] = (uint8_t)(xrnd() & 1);
            uint32_t vv = xrnd();
            idata[0xDBD + sl2] = (vv & 2) ? 0xFF : (uint8_t)(vv % 0x12);
            idata[0xDA9 + sl2] = (uint8_t)(xrnd() & 0x3);
            idata[0xDD1 + sl2] = (uint8_t)(xrnd() & 0x0F);
            idata[0xDE5 + sl2] = (uint8_t)(xrnd() & 0xFF);
            idata[0xE0D + sl2] = (uint8_t)(xrnd() & 0xFF);
            idata[0xE21 + sl2] = (uint8_t)(xrnd() & 0xFF);
            idata[0xE35 + sl2] = (uint8_t)(xrnd() & 1);
            idata[0xE49 + sl2] = (uint8_t)(xrnd() & 0xFF);
            idata[0xE5D + sl2] = (uint8_t)(xrnd() & 0xFF);
            idata[0xE71 + sl2] = (uint8_t)(xrnd() & 0xFF);
            idata[0xE85 + sl2] = (uint8_t)(xrnd() & 0xFF);
        }
        // occupancy ring in-domain: free (0xFF) or a channel key 0..15
        for (uint32_t a = 0x1349; a < 0x135B; a++)
            idata[a] = (xrnd() & 1) ? 0xFF : (uint8_t)(xrnd() & 0x0F);
        // domain guard for the steal selection (see v2_ailnat_steal_2155):
        // one active voiced slot on a 4-op-capable voice keeps the original's
        // uninitialized victim locals always-assigned
        uint8_t cap = 0;
        for (uint8_t v = 0; v < 0x12; v++) if (idata[0x5D7 + v]) { cap = v; break; }
        idata[0xD95 + 0] = 1; idata[0xDBD + 0] = cap; idata[0xDA9 + 0] = 1;
        memcpy(g_nat, idata, isave_n);                 // identical full images
        uint16_t sl = (uint16_t)(xrnd() % 0x14);
        int op = (int)(xrnd() % 3);
        g_capn[0] = g_capn[1] = 0;
        g_capw = 0;
        v2_ail_interp_set_io_hooks(cap_out, cap_in);
        { uint16_t args[1] = { sl };
          v2_ail_interp_call(op == 0 ? 0x1A5A : op == 1 ? 0x1B11 : 0x2155,
                             args, op == 2 ? 0 : 1); }
        v2_ail_interp_set_io_hooks(old_o, old_i);
        g_capw = 1;
        nat_out_fn so = g_nat_out; nat_in_fn si_ = g_nat_in;
        g_nat_out = cap_out; g_nat_in = cap_in;
        if (op == 0)      v2_ailnat_voice_assign_1A5A(sl);
        else if (op == 1) v2_ailnat_voice_release_1B11(sl);
        else              v2_ailnat_steal_2155();
        g_nat_out = so; g_nat_in = si_;
        cases++;
        int bad = (g_capn[0] != g_capn[1] ||
                   memcmp(g_cap[0], g_cap[1], sizeof(g_cap[0][0]) * g_capn[0]) != 0);
        if (!bad) bad = memcmp(g_nat, idata, isave_n) != 0;
        if (bad) {
            fails++;
            if (fails <= 4) {
                uint32_t da = 0;
                for (uint32_t a = 0; a < isave_n; a++)
                    if (g_nat[a] != idata[a]) { da = a; break; }
                fprintf(stderr, "AILNAT-DIFF vmgmt op=%d sl=%u outs i=%d n=%d first-diff @%04X i=%02X n=%02X\n",
                        op, sl, g_capn[0], g_capn[1], da,
                        da ? idata[da] : 0, da ? g_nat[da] : 0);
            }
        }
    }

    // --- MIDI layer: 2629/24E4/2477/25EE/2384/2299 over event streams ------
    // Timbre fixtures are planted INSIDE the 64K blob arena (above the code,
    // at 0x8000+) with cs:0xD0B pointing at them through the arena's own
    // paragraph — the interpreter resolves them natively and the far hook
    // gives the native port the identical view.
    extern uint16_t v2_ail_interp_drv_para(void);
    g_fix_seg = v2_ail_interp_drv_para();
    v2_ailnat_set_far_read(nat_far_fixture_rd);
    for (int t = 0; t < 512; t++) {
        memcpy(idata, isave, isave_n);   // hermetic case
        // slot rows + layers + controllers + rings (driver-invariant domains)
        for (uint32_t a = 0xE99; a < 0x1269; a++) idata[a] = (uint8_t)(xrnd() & 0xFF);
        idata[0xD1C] = (uint8_t)(xrnd() & 0xFF);
        idata[0xD18] = (uint8_t)(xrnd() % 0x12); idata[0xD19] = 0;
        idata[0xD1A] = (uint8_t)(xrnd() % 0x6);  idata[0xD1B] = 0;
        for (uint32_t a = 0x1339; a < 0x1390; a++) idata[a] = (uint8_t)(xrnd() & 0xFF);
        for (uint16_t sl2 = 0; sl2 < 0x14; sl2++) {
            idata[0xD95 + sl2] = (uint8_t)(xrnd() & 1);
            uint32_t vv = xrnd();
            idata[0xDBD + sl2] = (vv & 2) ? 0xFF : (uint8_t)(vv % 0x12);
            idata[0xDA9 + sl2] = (uint8_t)(xrnd() & 0x3);
            idata[0xDD1 + sl2] = (uint8_t)(xrnd() & 0x0F);
            idata[0xDE5 + sl2] = (uint8_t)(xrnd() & 0x7F);
            idata[0xDF9 + sl2] = (uint8_t)(xrnd() & 0x7F);
            idata[0xE0D + sl2] = (uint8_t)(xrnd() & 0xFF);
            idata[0xE21 + sl2] = (uint8_t)(xrnd() & 0xFF);
            idata[0xE35 + sl2] = (uint8_t)(xrnd() & 1);
            idata[0xE49 + sl2] = (uint8_t)(xrnd() & 0xFF);
            idata[0xE5D + sl2] = (uint8_t)(xrnd() & 0xFF);
            idata[0xE71 + sl2] = (uint8_t)(xrnd() & 0xFF);
            idata[0xE85 + sl2] = (uint8_t)(xrnd() & 0xFF);
        }
        for (uint32_t a = 0x1349; a < 0x135B; a++)
            idata[a] = (xrnd() & 1) ? 0xFF : (uint8_t)(xrnd() & 0x0F);
        uint8_t cap = 0;
        for (uint8_t v = 0; v < 0x12; v++) if (idata[0x5D7 + v]) { cap = v; break; }
        idata[0xD95 + 0] = 1; idata[0xDBD + 0] = cap; idata[0xDA9 + 0] = 1;
        // timbre fixture: 8 records at 0x8000+, 0x40 apart; the offset table
        // and the far base both live in the judged image
        idata[0xD0B] = 0x00; idata[0xD0C] = 0x80;          // off = 0x8000
        idata[0xD0D] = (uint8_t)g_fix_seg; idata[0xD0E] = (uint8_t)(g_fix_seg >> 8);
        for (int r = 0; r < 8; r++) {
            uint32_t base = 0x8000u + (uint32_t)r * 0x40;
            idata[0x94B + 2 * r] = (uint8_t)(r * 0x40);
            idata[0x94C + 2 * r] = (uint8_t)((r * 0x40) >> 8);
            uint32_t k = xrnd() % 3;               // record type domain
            uint16_t tl = (k == 0) ? 0x0E : (k == 1) ? 0x19 : (uint16_t)(xrnd() & 0xFF);
            idata[base] = (uint8_t)tl; idata[base + 1] = (uint8_t)(tl >> 8);
            for (uint32_t o = 2; o < 0x20; o++) idata[base + o] = (uint8_t)(xrnd() & 0xFF);
        }
        // channel patch/bank/drum maps: fixture ids or misses
        for (uint16_t c2 = 0; c2 < 0x10; c2++) {
            idata[0x1289 + c2] = (xrnd() & 1) ? 0xFF : (uint8_t)(xrnd() % 8);
            idata[0x1299 + c2] = (uint8_t)(xrnd() & 0x7F);
            idata[0x12A9 + c2] = (uint8_t)(xrnd() & 0x7F);
        }
        for (uint16_t n2 = 0; n2 < 0x80; n2++)
            idata[0x12B9 + n2] = (xrnd() & 1) ? 0xFF : (uint8_t)(xrnd() % 8);
        // loaded-timbre tables for the 15E9 searches (bank 0x7F drums too)
        for (uint16_t s2 = 0; s2 < 0xC0; s2++) {
            idata[0xC4B + s2] = (uint8_t)(xrnd() & 0xFF);
            idata[0xACB + s2] = (xrnd() & 1) ? 0x7F : (uint8_t)(xrnd() & 0x0F);
            idata[0xB8B + s2] = (uint8_t)(xrnd() & 0x7F);
        }
        memcpy(g_nat, idata, isave_n);
        // random MIDI event (data bytes 7-bit, as the wire format guarantees)
        static const uint8_t fam[8] = {0x80,0x90,0x90,0xB0,0xB0,0xC0,0xE0,0xA0};
        static const uint8_t ctl[10] = {0x01,0x07,0x0A,0x0B,0x40,0x70,0x71,0x72,0x79,0x7B};
        uint16_t st = (uint16_t)(fam[xrnd() & 7] | (xrnd() & 0xF));
        uint16_t d1 = (uint16_t)(xrnd() & 0x7F);
        if ((st & 0xF0) == 0xB0 && (xrnd() & 3)) d1 = ctl[xrnd() % 10];
        uint16_t d2 = (uint16_t)(xrnd() & 0x7F);
        if ((xrnd() & 7) == 0) d2 = 0x40;          // pedal / lock edges
        if ((xrnd() & 7) == 1) d2 = 0;             // note-on-as-off edge
        g_capn[0] = g_capn[1] = 0;
        g_capw = 0;
        v2_ail_interp_set_io_hooks(cap_out, cap_in);
        { uint16_t args[3] = { st, d1, d2 }; v2_ail_interp_call(0x2629, args, 3); }
        v2_ail_interp_set_io_hooks(old_o, old_i);
        g_capw = 1;
        nat_out_fn so = g_nat_out; nat_in_fn si_ = g_nat_in;
        g_nat_out = cap_out; g_nat_in = cap_in;
        v2_ailnat_midi_2629(st, d1, d2);
        g_nat_out = so; g_nat_in = si_;
        cases++;
        int bad = (g_capn[0] != g_capn[1] ||
                   memcmp(g_cap[0], g_cap[1], sizeof(g_cap[0][0]) * g_capn[0]) != 0);
        if (!bad) bad = memcmp(g_nat, idata, isave_n) != 0;
        if (bad) {
            fails++;
            if (fails <= 4) {
                uint32_t da = 0;
                for (uint32_t a = 0; a < isave_n; a++)
                    if (g_nat[a] != idata[a]) { da = a; break; }
                fprintf(stderr, "AILNAT-DIFF midi st=%02X d1=%02X d2=%02X outs i=%d n=%d first-diff @%04X i=%02X n=%02X\n",
                        st, d1, d2, g_capn[0], g_capn[1], da,
                        da ? idata[da] : 0, da ? g_nat[da] : 0);
            }
        }
    }
    v2_ailnat_set_far_read(nullptr);

    // --- sequencer: 331E tick over fixture sequences -----------------------
    // Sequence structs and XMID event streams live in a mapped 64K "game
    // memory" range (SEQFIX_PARA) — the interpreter resolves it through its
    // segment map (far-pointer normalization included), the native port
    // through the chained far hooks; each side gets its own copy.
    {
        extern void v2_ail_interp_map_segment(uint16_t, uint8_t*, uint32_t);
        static int mapped = 0;
        if (!mapped) {   // the interp keeps mappings — register once
            v2_ail_interp_map_segment(SEQFIX_PARA, g_seqfix_i, sizeof(g_seqfix_i));
            mapped = 1;
        }
    }
    v2_ailnat_set_far_write(nat_far_fixture_wr);
    for (int t = 0; t < 384; t++) {
        memcpy(idata, isave, isave_n);   // hermetic case
        memset(g_seqfix_i, 0, sizeof(g_seqfix_i));
        // driver channel/slot state + timbre fixture (as the MIDI sweep)
        for (uint32_t a = 0xE99; a < 0x1269; a++) idata[a] = (uint8_t)(xrnd() & 0xFF);
        idata[0xD1C] = (uint8_t)(xrnd() & 0xFF);
        idata[0xD18] = (uint8_t)(xrnd() % 0x12); idata[0xD19] = 0;
        idata[0xD1A] = (uint8_t)(xrnd() % 0x6);  idata[0xD1B] = 0;
        for (uint32_t a = 0x1339; a < 0x1390; a++) idata[a] = (uint8_t)(xrnd() & 0xFF);
        for (uint16_t sl2 = 0; sl2 < 0x14; sl2++) {
            idata[0xD95 + sl2] = (uint8_t)(xrnd() & 1);
            uint32_t vv = xrnd();
            idata[0xDBD + sl2] = (vv & 2) ? 0xFF : (uint8_t)(vv % 0x12);
            idata[0xDA9 + sl2] = (uint8_t)(xrnd() & 0x3);
            idata[0xDD1 + sl2] = (uint8_t)(xrnd() & 0x0F);
            idata[0xDE5 + sl2] = (uint8_t)(xrnd() & 0x7F);
            idata[0xDF9 + sl2] = (uint8_t)(xrnd() & 0x7F);
            idata[0xE0D + sl2] = (uint8_t)(xrnd() & 0xFF);
            idata[0xE21 + sl2] = (uint8_t)(xrnd() & 0xFF);
            idata[0xE35 + sl2] = (uint8_t)(xrnd() & 1);
            idata[0xE49 + sl2] = (uint8_t)(xrnd() & 0xFF);
            idata[0xE5D + sl2] = (uint8_t)(xrnd() & 0xFF);
            idata[0xE71 + sl2] = (uint8_t)(xrnd() & 0xFF);
            idata[0xE85 + sl2] = (uint8_t)(xrnd() & 0xFF);
        }
        for (uint32_t a = 0x1349; a < 0x135B; a++)
            idata[a] = (xrnd() & 1) ? 0xFF : (uint8_t)(xrnd() & 0x0F);
        uint8_t cap = 0;
        for (uint8_t v = 0; v < 0x12; v++) if (idata[0x5D7 + v]) { cap = v; break; }
        idata[0xD95 + 0] = 1; idata[0xDBD + 0] = cap; idata[0xDA9 + 0] = 1;
        idata[0xD0B] = 0x00; idata[0xD0C] = 0x80;
        idata[0xD0D] = (uint8_t)g_fix_seg; idata[0xD0E] = (uint8_t)(g_fix_seg >> 8);
        for (int r = 0; r < 8; r++) {
            uint32_t base = 0x8000u + (uint32_t)r * 0x40;
            idata[0x94B + 2 * r] = (uint8_t)(r * 0x40);
            idata[0x94C + 2 * r] = (uint8_t)((r * 0x40) >> 8);
            uint32_t k = xrnd() % 3;
            uint16_t tl = (k == 0) ? 0x0E : (k == 1) ? 0x19 : (uint16_t)(xrnd() & 0xFF);
            idata[base] = (uint8_t)tl; idata[base + 1] = (uint8_t)(tl >> 8);
            for (uint32_t o = 2; o < 0x20; o++) idata[base + o] = (uint8_t)(xrnd() & 0xFF);
        }
        for (uint16_t c2 = 0; c2 < 0x10; c2++) {
            idata[0x1289 + c2] = (xrnd() & 1) ? 0xFF : (uint8_t)(xrnd() % 8);
            idata[0x1299 + c2] = (uint8_t)(xrnd() & 0x7F);
            idata[0x12A9 + c2] = (uint8_t)(xrnd() & 0x7F);
        }
        for (uint16_t n2 = 0; n2 < 0x80; n2++)
            idata[0x12B9 + n2] = (xrnd() & 1) ? 0xFF : (uint8_t)(xrnd() % 8);
        for (uint16_t s2 = 0; s2 < 0xC0; s2++) {
            idata[0xC4B + s2] = (uint8_t)(xrnd() & 0xFF);
            idata[0xACB + s2] = (xrnd() & 1) ? 0x7F : (uint8_t)(xrnd() & 0x0F);
            idata[0xB8B + s2] = (uint8_t)(xrnd() & 0x7F);
        }
        // no game callback in the unit context (0x77 would far-call it)
        idata[0x2955] = idata[0x2956] = 0;
        idata[0x2957] = idata[0x2958] = 0;
        idata[0x2959] = idata[0x295A] = 0;
        // per-channel global images the tick reads/writes
        for (uint16_t c2 = 0; c2 < 0x10; c2++) {
            idata[0x295B + (xrnd() % 0x90)] = (uint8_t)(xrnd() & 0xFF);
            idata[0x29EB + c2] = (xrnd() & 1) ? 0xFF : (uint8_t)(xrnd() & 0x7F);
            idata[0x29FB + c2] = (xrnd() & 1) ? 0xFF : (uint8_t)(xrnd() & 0x7F);
            idata[0x2A0B + c2] = (xrnd() & 1) ? 0xFF : (uint8_t)(xrnd() & 0x7F);
            idata[0x299B + c2] = (uint8_t)(xrnd() & 0x7F);
        }
        for (uint16_t c2 = 0; c2 < 8; c2++) {
            idata[0x2A1B + c2] = (uint8_t)(xrnd() & 0x3);
            idata[0x2A2B + c2] = (uint8_t)(xrnd() & 0xC0);
        }
        idata[0x2A2B + 8] = (uint8_t)(xrnd() & 0xC0);
        // handles: 1..2 sequences
        int nseq = 1 + (int)(xrnd() & 1);
        idata[0x294F] = (uint8_t)nseq; idata[0x2950] = 0;
        idata[0x2953] = 0; idata[0x2954] = 0;
        for (int k = 0; k < nseq; k++) {
            uint32_t so = 0x100u + (uint32_t)k * 0x400;
            uint32_t ev = ((xrnd() & 7) == 0 ? 0x9000u : 0x1000u) + (uint32_t)k * 0x800;
            idata[0x292F + 4 * k] = (uint8_t)so; idata[0x2930 + 4 * k] = (uint8_t)(so >> 8);
            idata[0x2931 + 4 * k] = (uint8_t)SEQFIX_PARA;
            idata[0x2932 + 4 * k] = (uint8_t)(SEQFIX_PARA >> 8);
            uint8_t* q = g_seqfix_i;
            // ---- the sequence struct ----
            q[so + 0x1A] = ((xrnd() & 7) == 0) ? 2 : 1;       // status
            q[so + 0x1C] = (uint8_t)(xrnd() & 1);             // auto-release
            q[so + 0x1E] = (uint8_t)(xrnd() & 0x3);           // ticks to event
            q[so + 0x20] = 0;
            q[so + 0x24] = (uint8_t)(xrnd() & 0x7F);          // volume
            q[so + 0x26] = (uint8_t)(xrnd() & 0x7F);          // volume target
            q[so + 0x2C] = (uint8_t)(xrnd() | 1);             // ramp steps != 0
            q[so + 0x2D] = (uint8_t)(xrnd() & 0x7F);
            q[so + 0x32] = (uint8_t)(0x40 + (xrnd() & 0x3F)); // tempo percent
            q[so + 0x34] = (uint8_t)(0x40 + (xrnd() & 0x3F)); // tempo target
            q[so + 0x3A] = (uint8_t)(xrnd() | 1);             // tempo ramp step
            q[so + 0x3B] = (uint8_t)(xrnd() & 0x7F);
            q[so + 0x30] = (uint8_t)(xrnd() % 0x64);          // tempo acc
            q[so + 0x42] = (uint8_t)(1 + (xrnd() & 7));       // beats/measure
            uint32_t binc = 0x208D5u >> (xrnd() & 3);         // beat increment
            q[so + 0x44] = (uint8_t)binc; q[so + 0x45] = (uint8_t)(binc >> 8);
            q[so + 0x46] = (uint8_t)(binc >> 16); q[so + 0x47] = (uint8_t)(binc >> 24);
            q[so + 0x48] = (uint8_t)binc; q[so + 0x49] = (uint8_t)(binc >> 8);
            q[so + 0x4A] = (uint8_t)(binc >> 16); q[so + 0x4B] = (uint8_t)(binc >> 24);
            uint32_t per = 0x100000u + (xrnd() & 0xFFFFF);    // quarter period
            q[so + 0x4C] = (uint8_t)per; q[so + 0x4D] = (uint8_t)(per >> 8);
            q[so + 0x4E] = (uint8_t)(per >> 16); q[so + 0x4F] = (uint8_t)(per >> 24);
            for (int l = 0; l < 4; l++) {                     // loop stack empty
                q[so + 0x60 + 2 * l] = 0xFF; q[so + 0x61 + 2 * l] = 0xFF;
            }
            for (int c2 = 0; c2 < 0x10; c2++) {
                q[so + 0x68 + c2] = (xrnd() & 3) ? (uint8_t)c2
                                                 : (uint8_t)(xrnd() & 0xF);
                q[so + 0x78 + c2] = (uint8_t)(xrnd() & 0x7F); // program shadow
                q[so + 0x88 + c2] = (uint8_t)(xrnd() & 0x7F); // bend lo
                q[so + 0x98 + c2] = (uint8_t)(xrnd() & 0x7F); // bend hi
                q[so + 0xA8 + c2] = (xrnd() & 3) ? 0xFF
                                                 : (uint8_t)(xrnd() & 0x1F);
            }
            for (int o = 0xB8; o < 0x148; o++)                // controller rows
                q[so + o] = (xrnd() & 1) ? 0xFF : (uint8_t)(xrnd() & 0x7F);
            // preloaded deferred notes
            int pend = (int)(xrnd() & 3);
            for (int o = 0; o < 0x20; o++) q[so + 0x148 + o] = 0xFF;
            for (int pn = 0; pn < pend; pn++) {
                int slot = (int)(xrnd() % 0x20);
                if (q[so + 0x148 + slot] != 0xFF) continue;
                q[so + 0x148 + slot] = (uint8_t)(xrnd() & 0xF);
                q[so + 0x168 + slot] = (uint8_t)(xrnd() & 0x7F);
                uint16_t dr = (uint16_t)(xrnd() & 0x3);
                q[so + 0x188 + slot * 2] = (uint8_t)dr;
                q[so + 0x189 + slot * 2] = 0;
                q[so + 0x1C8 + slot * 2] = 0; q[so + 0x1C9 + slot * 2] = 0;
                q[so + 0x20] = (uint8_t)(q[so + 0x20] + 1);
            }
            // indirection table for controller 0x73
            q[so + 0x12] = 0x00; q[so + 0x13] = 0x20;         // -> fix:0x2000
            q[so + 0x14] = (uint8_t)SEQFIX_PARA; q[so + 0x15] = (uint8_t)(SEQFIX_PARA >> 8);
            q[so + 0x16] = (uint8_t)k; q[so + 0x17] = 0;      // handle id
            // ---- the event stream ----
            q[so + 0xC] = (uint8_t)ev; q[so + 0xD] = (uint8_t)(ev >> 8);
            q[so + 0xE] = (uint8_t)SEQFIX_PARA; q[so + 0xF] = (uint8_t)(SEQFIX_PARA >> 8);
            uint32_t w = ev;
            int nev = 2 + (int)(xrnd() % 6);
            for (int e2 = 0; e2 < nev; e2++) {
                q[w++] = (uint8_t)(1 + (xrnd() & 0x3F));      // delta > 0
                uint32_t fpick = xrnd() % 10;
                uint8_t ch2 = (uint8_t)(1 + (xrnd() % 9));
                if (fpick < 4) {                              // note on + VLQ
                    q[w++] = (uint8_t)(0x90 | ch2);
                    q[w++] = (uint8_t)(xrnd() & 0x7F);
                    q[w++] = (uint8_t)(xrnd() & 0x7F);
                    uint32_t du = 1 + (xrnd() & 0xFF);
                    if (du > 0x7F) { q[w++] = (uint8_t)(0x80 | (du >> 7)); }
                    q[w++] = (uint8_t)(du & 0x7F);
                } else if (fpick < 7) {                       // controller
                    static const uint8_t cl6[12] = {0x01,0x07,0x0A,0x0B,0x40,0x6E,
                                                    0x6F,0x73,0x74,0x75,0x76,0x77};
                    q[w++] = (uint8_t)(0xB0 | ch2);
                    q[w++] = (xrnd() & 3) ? cl6[xrnd() % 12] : (uint8_t)(xrnd() & 0x7F);
                    q[w++] = (uint8_t)(xrnd() & 0x7F);
                } else if (fpick == 7) {                      // program / pitch
                    if (xrnd() & 1) {
                        q[w++] = (uint8_t)(0xC0 | ch2); q[w++] = (uint8_t)(xrnd() & 0x7F);
                    } else {
                        q[w++] = (uint8_t)(0xE0 | ch2);
                        q[w++] = (uint8_t)(xrnd() & 0x7F); q[w++] = (uint8_t)(xrnd() & 0x7F);
                    }
                } else if (fpick == 8) {                      // meta 51/58
                    if (xrnd() & 1) {
                        q[w++] = 0xFF; q[w++] = 0x51; q[w++] = 3;
                        q[w++] = (uint8_t)(xrnd() & 0x7F);
                        q[w++] = (uint8_t)(xrnd() & 0xFF); q[w++] = (uint8_t)(xrnd() & 0xFF);
                    } else {
                        q[w++] = 0xFF; q[w++] = 0x58; q[w++] = 4;
                        q[w++] = (uint8_t)(1 + (xrnd() & 7)); q[w++] = (uint8_t)(xrnd() & 7);
                        q[w++] = 0x18; q[w++] = 0x08;
                    }
                } else {                                      // sysex skip
                    q[w++] = 0xF0; q[w++] = 3;
                    q[w++] = 0x7E; q[w++] = 0x7F; q[w++] = 0xF7;
                }
            }
            q[w++] = (uint8_t)(1 + (xrnd() & 0x1F));          // final delta
            q[w++] = 0xFF; q[w++] = 0x2F; q[w++] = 0x00;      // end of track
            // 0x73 indirection table bytes
            for (int o = 0; o < 0x20; o++)
                g_seqfix_i[0x2000 + o] = (uint8_t)(xrnd() & 0x7F);
        }
        memcpy(g_nat, idata, isave_n);
        memcpy(g_seqfix_n, g_seqfix_i, sizeof(g_seqfix_i));
        int ticks = 1 + (int)(xrnd() & 3);
        g_capn[0] = g_capn[1] = 0;
        g_capw = 0;
        v2_ail_interp_set_io_hooks(cap_out, cap_in);
        for (int tk = 0; tk < ticks; tk++)
            v2_ail_interp_call(0x331E, (const uint16_t*)0, 0);
        v2_ail_interp_set_io_hooks(old_o, old_i);
        g_capw = 1;
        nat_out_fn so_ = g_nat_out; nat_in_fn si_ = g_nat_in;
        g_nat_out = cap_out; g_nat_in = cap_in;
        for (int tk = 0; tk < ticks; tk++)
            v2_ailnat_timer_tick_331E();
        g_nat_out = so_; g_nat_in = si_;
        cases++;
        int bad = (g_capn[0] != g_capn[1] ||
                   memcmp(g_cap[0], g_cap[1], sizeof(g_cap[0][0]) * g_capn[0]) != 0);
        if (!bad) bad = memcmp(g_nat, idata, isave_n) != 0;
        if (!bad) bad = memcmp(g_seqfix_n, g_seqfix_i, sizeof(g_seqfix_i)) != 0;
        if (bad) {
            fails++;
            if (fails <= 4) {
                uint32_t da = 0xFFFFFFFFu;
                for (uint32_t a = 0; a < isave_n; a++)
                    if (g_nat[a] != idata[a]) { da = a; break; }
                uint32_t sa = 0xFFFFFFFFu;
                for (uint32_t a = 0; a < sizeof(g_seqfix_i); a++)
                    if (g_seqfix_n[a] != g_seqfix_i[a]) { sa = a; break; }
                fprintf(stderr, "AILNAT-DIFF tick t=%d ticks=%d outs i=%d n=%d blob@%04X seq@%04X\n",
                        t, ticks, g_capn[0], g_capn[1],
                        (unsigned)da, (unsigned)sa);
            }
        }
    }
    v2_ailnat_set_far_write(nullptr);

    // --- fn surface: boot -> register -> load timbres -> play -> fade ------
    // A full driver lifecycle on both executors: install (fn66), cache
    // assign (fn9A), sequence registration over a generated XMID file
    // (fn97), the fn9B/fn9C timbre load loop from bank-format records,
    // start (fnAA), ticks, a volume fade (fnB1), more ticks, stop (fnAB),
    // status reads — everything judged as one OUT stream + both images.
    v2_ailnat_set_far_write(nat_far_fixture_wr);
    v2_ailnat_set_self_seg(g_fix_seg);
    for (int t = 0; t < 128; t++) {
        memcpy(idata, isave, isave_n);
        memset(g_seqfix_i, 0, sizeof(g_seqfix_i));
        idata[0x2955] = idata[0x2956] = 0;
        idata[0x2957] = idata[0x2958] = 0;
        idata[0x2959] = idata[0x295A] = 0;
        // ---- XMID file fixture at seqfix:0x1000 ----
        uint8_t* q = g_seqfix_i;
        uint32_t xf = 0x1000;
        int use_cat = (int)(xrnd() & 1);
        uint32_t form = use_cat ? xf + 0xC : xf;
        // TIMB entries
        int ntimb = 1 + (int)(xrnd() % 3);
        uint32_t timb_len = 2 + 2 * (uint32_t)ntimb;
        // EVNT stream (build first to know its length)
        uint8_t ev[256]; uint32_t el = 0;
        int nev = 2 + (int)(xrnd() % 5);
        for (int e2 = 0; e2 < nev; e2++) {
            ev[el++] = (uint8_t)(1 + (xrnd() & 0x1F));
            uint8_t ch2 = (uint8_t)(1 + (xrnd() % 9));
            uint32_t fpick = xrnd() % 8;
            if (fpick < 4) {
                ev[el++] = (uint8_t)(0x90 | ch2);
                ev[el++] = (uint8_t)(xrnd() & 0x7F);
                ev[el++] = (uint8_t)(1 + (xrnd() & 0x7E));
                ev[el++] = (uint8_t)(1 + (xrnd() & 0x3F));   // short duration
            } else if (fpick < 6) {
                static const uint8_t cl7[8] = {0x01,0x07,0x0A,0x0B,0x40,0x74,0x75,0x76};
                ev[el++] = (uint8_t)(0xB0 | ch2);
                ev[el++] = cl7[xrnd() & 7];
                ev[el++] = (uint8_t)(xrnd() & 0x7F);
            } else if (fpick == 6) {
                ev[el++] = (uint8_t)(0xC0 | ch2);
                ev[el++] = (uint8_t)(xrnd() & 0x7F);
            } else {
                ev[el++] = (uint8_t)(0xE0 | ch2);
                ev[el++] = (uint8_t)(xrnd() & 0x7F);
                ev[el++] = (uint8_t)(xrnd() & 0x7F);
            }
        }
        ev[el++] = (uint8_t)(1 + (xrnd() & 0x1F));
        ev[el++] = 0xFF; ev[el++] = 0x2F; ev[el++] = 0x00;
        uint32_t form_len = 4 + (8 + timb_len) + (8 + el);    // XMID + chunks
        if (use_cat) {
            uint32_t cat_len = 4 + 8 + form_len;              // XMID + FORM
            q[xf+0]='C'; q[xf+1]='A'; q[xf+2]='T'; q[xf+3]=' ';
            q[xf+4]=(uint8_t)(cat_len>>24); q[xf+5]=(uint8_t)(cat_len>>16);
            q[xf+6]=(uint8_t)(cat_len>>8);  q[xf+7]=(uint8_t)cat_len;
            q[xf+8]='X'; q[xf+9]='M'; q[xf+10]='I'; q[xf+11]='D';
        }
        q[form+0]='F'; q[form+1]='O'; q[form+2]='R'; q[form+3]='M';
        q[form+4]=(uint8_t)(form_len>>24); q[form+5]=(uint8_t)(form_len>>16);
        q[form+6]=(uint8_t)(form_len>>8);  q[form+7]=(uint8_t)form_len;
        q[form+8]='X'; q[form+9]='M'; q[form+10]='I'; q[form+11]='D';
        uint32_t w = form + 0xC;
        q[w+0]='T'; q[w+1]='I'; q[w+2]='M'; q[w+3]='B';
        q[w+4]=(uint8_t)(timb_len>>24); q[w+5]=(uint8_t)(timb_len>>16);
        q[w+6]=(uint8_t)(timb_len>>8);  q[w+7]=(uint8_t)timb_len;
        q[w+8]=(uint8_t)ntimb; q[w+9]=0;
        uint8_t tp[3], tb[3];
        for (int i = 0; i < ntimb; i++) {
            tp[i] = (uint8_t)(xrnd() & 0x7F);
            tb[i] = (xrnd() & 1) ? 0x7F : (uint8_t)(xrnd() & 0x3);
            q[w + 10 + 2*i] = tp[i]; q[w + 11 + 2*i] = tb[i];
        }
        w += 8 + timb_len;
        q[w+0]='E'; q[w+1]='V'; q[w+2]='N'; q[w+3]='T';
        q[w+4]=(uint8_t)(el>>24); q[w+5]=(uint8_t)(el>>16);
        q[w+6]=(uint8_t)(el>>8);  q[w+7]=(uint8_t)el;
        memcpy(q + w + 8, ev, el);
        // ---- bank-format timbre sources at seqfix:0x5000 ----
        for (int i = 0; i < ntimb; i++) {
            uint32_t b2 = 0x5000u + (uint32_t)i * 0x40;
            uint16_t tl = (tb[i] == 0x7F) ? 0x19 : 0x0E;      // drums = perc
            q[b2] = (uint8_t)tl; q[b2+1] = (uint8_t)(tl >> 8);
            for (uint32_t o = 2; o < tl; o++) q[b2+o] = (uint8_t)(xrnd() & 0x7F);
        }
        memcpy(g_nat, idata, isave_n);
        memcpy(g_seqfix_n, g_seqfix_i, sizeof(g_seqfix_i));
        g_capn[0] = g_capn[1] = 0;
        int ticks1 = 1 + (int)(xrnd() & 3);
        int ticks2 = 1 + (int)(xrnd() & 3);
        uint16_t fade_ms = (uint16_t)(xrnd() & 0x3FF);
        uint16_t iax[4] = {0,0,0,0}, nax[4] = {0,0,0,0};
        // ---- interpreter side ----
        g_capw = 0;
        v2_ail_interp_set_io_hooks(cap_out, cap_in);
        { uint16_t a[2] = { 0 };
          iax[0] = v2_ail_interp_call(0x27C8, a, 1); }        // fn64
        { uint16_t a[2] = { 0, 0x388 };
          iax[1] = v2_ail_interp_call(0x140E, a, 2); }        // fn65 probe
        { uint16_t a[5] = { 0, 0x388, 0, 0, 0 };
          v2_ail_interp_call(0x35D8, a, 5); }                 // fn66 install
        { uint16_t a[4] = { 0, 0x4000, SEQFIX_PARA, 0xE00 };
          v2_ail_interp_call(0x16F0, a, 4); }                 // fn9A cache
        uint16_t ih;
        { uint16_t a[8] = { 0, (uint16_t)0x1000, SEQFIX_PARA, 0,
                            0x100, SEQFIX_PARA, 0x2000, SEQFIX_PARA };
          ih = v2_ail_interp_call(0x37F6, a, 8); }            // fn97 register
        for (int gu = 0; gu < 8 && ih != 0xFFFF; gu++) {      // fn9B/9C loop
            uint16_t a[2] = { 0, ih };
            uint16_t req = v2_ail_interp_call(0x1736, a, 2);
            if (req == 0xFFFF) break;
            int idx = 0;
            for (int i = 0; i < ntimb; i++)
                if (tp[i] == (uint8_t)req && tb[i] == (uint8_t)(req >> 8)) idx = i;
            uint16_t a2[5] = { 0, (uint16_t)(req >> 8), (uint16_t)(req & 0xFF),
                               (uint16_t)(0x5000 + idx * 0x40), SEQFIX_PARA };
            v2_ail_interp_call(0x18DB, a2, 5);                // fn9C load
        }
        if (ih != 0xFFFF) {
            uint16_t a[2] = { 0, ih };
            v2_ail_interp_call(0x3980, a, 2);                 // fnAA start
            for (int tk = 0; tk < ticks1; tk++)
                v2_ail_interp_call(0x331E, (const uint16_t*)0, 0);
            { uint16_t a3[4] = { 0, ih, 0, fade_ms };
              v2_ail_interp_call(0x3CF1, a3, 4); }            // fnB1 fade
            for (int tk = 0; tk < ticks2; tk++)
                v2_ail_interp_call(0x331E, (const uint16_t*)0, 0);
            iax[2] = v2_ail_interp_call(0x3A9E, a, 2);        // fnAE status
            v2_ail_interp_call(0x3A15, a, 2);                 // fnAB stop
            iax[3] = v2_ail_interp_call(0x3A9E, a, 2);
        }
        v2_ail_interp_set_io_hooks(old_o, old_i);
        // ---- native side ----
        g_capw = 1;
        nat_out_fn so_ = g_nat_out; nat_in_fn si_ = g_nat_in;
        g_nat_out = cap_out; g_nat_in = cap_in;
        nax[0] = v2_ailnat_fn64_desc_27C8();
        nax[1] = v2_ailnat_fn65_probe_140E(0x388);
        v2_ailnat_fn66_install_35D8(0x388);
        v2_ailnat_fn9A_set_cache_16F0(0x4000, SEQFIX_PARA, 0xE00);
        uint16_t nh = v2_ailnat_fn97_register_37F6(0x1000, SEQFIX_PARA, 0,
                                                   0x100, SEQFIX_PARA,
                                                   0x2000, SEQFIX_PARA);
        for (int gu = 0; gu < 8 && nh != 0xFFFF; gu++) {
            uint16_t req = v2_ailnat_fn9B_timbre_request_1736(nh);
            if (req == 0xFFFF) break;
            int idx = 0;
            for (int i = 0; i < ntimb; i++)
                if (tp[i] == (uint8_t)req && tb[i] == (uint8_t)(req >> 8)) idx = i;
            v2_ailnat_fn9C_load_timbre_18DB((uint16_t)(req >> 8),
                                            (uint16_t)(req & 0xFF),
                                            (uint16_t)(0x5000 + idx * 0x40),
                                            SEQFIX_PARA);
        }
        if (nh != 0xFFFF) {
            v2_ailnat_fnAA_start_3980(nh);
            for (int tk = 0; tk < ticks1; tk++) v2_ailnat_timer_tick_331E();
            v2_ailnat_fnB1_set_volume_3CF1(nh, 0, fade_ms);
            for (int tk = 0; tk < ticks2; tk++) v2_ailnat_timer_tick_331E();
            nax[2] = v2_ailnat_fnAE_status_3A9E(nh);
            v2_ailnat_fnAB_stop_3A15(nh);
            nax[3] = v2_ailnat_fnAE_status_3A9E(nh);
        }
        g_nat_out = so_; g_nat_in = si_;
        cases++;
        int bad = (ih != nh || memcmp(iax, nax, sizeof(iax)) != 0);
        if (!bad) bad = (g_capn[0] != g_capn[1] ||
                   memcmp(g_cap[0], g_cap[1], sizeof(g_cap[0][0]) * g_capn[0]) != 0);
        if (!bad) bad = memcmp(g_nat, idata, isave_n) != 0;
        if (!bad) bad = memcmp(g_seqfix_n, g_seqfix_i, sizeof(g_seqfix_i)) != 0;
        if (bad) {
            fails++;
            if (fails <= 4) {
                uint32_t da = 0xFFFFFFFFu, sa = 0xFFFFFFFFu;
                for (uint32_t a = 0; a < isave_n; a++)
                    if (g_nat[a] != idata[a]) { da = a; break; }
                for (uint32_t a = 0; a < sizeof(g_seqfix_i); a++)
                    if (g_seqfix_n[a] != g_seqfix_i[a]) { sa = a; break; }
                fprintf(stderr, "AILNAT-DIFF fnsurf t=%d h i=%04X n=%04X ax %04X/%04X %04X/%04X %04X/%04X %04X/%04X outs i=%d n=%d blob@%04X seq@%04X\n",
                        t, ih, nh, iax[0],nax[0], iax[1],nax[1],
                        iax[2],nax[2], iax[3],nax[3],
                        g_capn[0], g_capn[1], (unsigned)da, (unsigned)sa);
            }
        }
    }
    v2_ailnat_set_far_write(nullptr);

    // --- task #102 fns: the non-game surface over a full lifecycle ---------
    // Same lifecycle as the fn-surface sweep, with an RBRN chunk in the XMID
    // and a deterministic batch of the remaining fns judged by return value,
    // OUT stream and both images.
    v2_ailnat_set_far_write(nat_far_fixture_wr);
    v2_ailnat_set_self_seg(g_fix_seg);
    for (int t = 0; t < 96; t++) {
        memcpy(idata, isave, isave_n);
        memset(g_seqfix_i, 0, sizeof(g_seqfix_i));
        idata[0x2955] = idata[0x2956] = 0;
        idata[0x2957] = idata[0x2958] = 0;
        idata[0x2959] = idata[0x295A] = 0;
        uint8_t* q = g_seqfix_i;
        uint32_t form = 0x1000;
        int ntimb = 1 + (int)(xrnd() % 3);
        uint32_t timb_len = 2 + 2 * (uint32_t)ntimb;
        int nbr = 1 + (int)(xrnd() & 3);           // RBRN entries
        uint32_t rbrn_len = 2 + 6 * (uint32_t)nbr;
        uint8_t ev[256]; uint32_t el = 0;
        uint32_t ev_starts[8]; int nstarts = 0;
        int nev = 2 + (int)(xrnd() % 4);
        for (int e2 = 0; e2 < nev; e2++) {
            ev_starts[nstarts++] = el;             // branch targets must be
            ev[el++] = (uint8_t)(1 + (xrnd() & 0x1F));   // event-aligned
            uint8_t ch2 = (uint8_t)(1 + (xrnd() % 9));
            uint32_t fpick = xrnd() % 6;
            if (fpick < 3) {
                ev[el++] = (uint8_t)(0x90 | ch2);
                ev[el++] = (uint8_t)(xrnd() & 0x7F);
                ev[el++] = (uint8_t)(1 + (xrnd() & 0x7E));
                ev[el++] = (uint8_t)(1 + (xrnd() & 0x3F));
            } else if (fpick < 5) {
                static const uint8_t cl8[6] = {0x01,0x07,0x0A,0x0B,0x40,0x74};
                ev[el++] = (uint8_t)(0xB0 | ch2);
                ev[el++] = cl8[xrnd() % 6];
                ev[el++] = (uint8_t)(xrnd() & 0x7F);
            } else {
                ev[el++] = (uint8_t)(0xC0 | ch2);
                ev[el++] = (uint8_t)(xrnd() & 0x7F);
            }
        }
        ev[el++] = 0x40;                           // long tail delta
        ev[el++] = 0xFF; ev[el++] = 0x2F; ev[el++] = 0x00;
        uint32_t form_len = 4 + (8 + timb_len) + (8 + rbrn_len) + (8 + el);
        q[form+0]='F'; q[form+1]='O'; q[form+2]='R'; q[form+3]='M';
        q[form+4]=(uint8_t)(form_len>>24); q[form+5]=(uint8_t)(form_len>>16);
        q[form+6]=(uint8_t)(form_len>>8);  q[form+7]=(uint8_t)form_len;
        q[form+8]='X'; q[form+9]='M'; q[form+10]='I'; q[form+11]='D';
        uint32_t w = form + 0xC;
        q[w+0]='T'; q[w+1]='I'; q[w+2]='M'; q[w+3]='B';
        q[w+4]=(uint8_t)(timb_len>>24); q[w+5]=(uint8_t)(timb_len>>16);
        q[w+6]=(uint8_t)(timb_len>>8);  q[w+7]=(uint8_t)timb_len;
        q[w+8]=(uint8_t)ntimb; q[w+9]=0;
        uint8_t tp[3], tb[3];
        for (int i = 0; i < ntimb; i++) {
            tp[i] = (uint8_t)(xrnd() & 0x7F);
            tb[i] = (xrnd() & 1) ? 0x7F : (uint8_t)(xrnd() & 0x3);
            q[w + 10 + 2*i] = tp[i]; q[w + 11 + 2*i] = tb[i];
        }
        w += 8 + timb_len;
        q[w+0]='R'; q[w+1]='B'; q[w+2]='R'; q[w+3]='N';
        q[w+4]=(uint8_t)(rbrn_len>>24); q[w+5]=(uint8_t)(rbrn_len>>16);
        q[w+6]=(uint8_t)(rbrn_len>>8);  q[w+7]=(uint8_t)rbrn_len;
        q[w+8]=(uint8_t)nbr; q[w+9]=0;
        for (int i = 0; i < nbr; i++) {            // {id, pad, off32 into EVNT}
            q[w + 10 + 6*i] = (uint8_t)i;
            q[w + 11 + 6*i] = 0;
            uint32_t bo = ev_starts[xrnd() % (uint32_t)nstarts];
            q[w + 12 + 6*i] = (uint8_t)bo; q[w + 13 + 6*i] = (uint8_t)(bo >> 8);
            q[w + 14 + 6*i] = 0; q[w + 15 + 6*i] = 0;
        }
        w += 8 + rbrn_len;
        q[w+0]='E'; q[w+1]='V'; q[w+2]='N'; q[w+3]='T';
        q[w+4]=(uint8_t)(el>>24); q[w+5]=(uint8_t)(el>>16);
        q[w+6]=(uint8_t)(el>>8);  q[w+7]=(uint8_t)el;
        memcpy(q + w + 8, ev, el);
        for (int i = 0; i < ntimb; i++) {
            uint32_t b2 = 0x5000u + (uint32_t)i * 0x40;
            uint16_t tl = (tb[i] == 0x7F) ? 0x19 : 0x0E;
            q[b2] = (uint8_t)tl; q[b2+1] = (uint8_t)(tl >> 8);
            for (uint32_t o = 2; o < tl; o++) q[b2+o] = (uint8_t)(xrnd() & 0x7F);
        }
        memcpy(g_nat, idata, isave_n);
        memcpy(g_seqfix_n, g_seqfix_i, sizeof(g_seqfix_i));
        g_capn[0] = g_capn[1] = 0;
        // the deterministic fn batch parameters
        uint16_t p_bank = (xrnd() & 7) == 0 ? 0xFF : (uint16_t)(tb[0]);
        uint16_t p_patch = (p_bank == 0xFF) ? 0xFF : (uint16_t)(tp[0]);
        uint16_t p_ch = (uint16_t)(1 + (xrnd() % 9));
        uint16_t p_ctl = (uint16_t)((xrnd() & 1) ? 0x07 : 0x77);
        uint16_t p_val = (uint16_t)(xrnd() & 0x7F);
        uint16_t p_bid = (uint16_t)(xrnd() % (nbr + 1));   // sometimes missing
        uint16_t p_nch = (uint16_t)(1 + (xrnd() % 9));
        uint16_t p_st = (uint16_t)((xrnd() & 1 ? 0x90 : 0x80) | (1 + (xrnd() % 9)));
        uint16_t p_d1 = (uint16_t)(xrnd() & 0x7F);
        uint16_t p_d2 = (uint16_t)(xrnd() & 0x7F);
        int ticks1 = 1 + (int)(xrnd() & 3);
        uint16_t iax[12], nax[12];
        memset(iax, 0, sizeof(iax)); memset(nax, 0, sizeof(nax));
        // ---- interpreter side ----
        g_capw = 0;
        v2_ail_interp_set_io_hooks(cap_out, cap_in);
        { uint16_t a[5] = { 0, 0x388, 0, 0, 0 };
          v2_ail_interp_call(0x35D8, a, 5); }
        { uint16_t a[4] = { 0, 0x4000, SEQFIX_PARA, 0xE00 };
          v2_ail_interp_call(0x16F0, a, 4); }
        uint16_t ih;
        { uint16_t a[8] = { 0, (uint16_t)0x1000, SEQFIX_PARA, 0,
                            0x100, SEQFIX_PARA, 0x2000, SEQFIX_PARA };
          ih = v2_ail_interp_call(0x37F6, a, 8); }
        for (int gu = 0; gu < 8 && ih != 0xFFFF; gu++) {
            uint16_t a[2] = { 0, ih };
            uint16_t req = v2_ail_interp_call(0x1736, a, 2);
            if (req == 0xFFFF) break;
            int idx = 0;
            for (int i = 0; i < ntimb; i++)
                if (tp[i] == (uint8_t)req && tb[i] == (uint8_t)(req >> 8)) idx = i;
            uint16_t a2[5] = { 0, (uint16_t)(req >> 8), (uint16_t)(req & 0xFF),
                               (uint16_t)(0x5000 + idx * 0x40), SEQFIX_PARA };
            v2_ail_interp_call(0x18DB, a2, 5);
        }
        if (ih != 0xFFFF) {
            uint16_t ah2[2] = { 0, ih };
            v2_ail_interp_call(0x3980, ah2, 2);
            for (int tk = 0; tk < ticks1; tk++)
                v2_ail_interp_call(0x331E, (const uint16_t*)0, 0);
            { uint16_t a[1] = { 0 };
              iax[0] = v2_ail_interp_call(0x3797, a, 1); }             // 96
            { uint16_t a[3] = { 0, p_bank, p_patch };
              v2_ail_interp_call(0x161C, a, 3);                        // 9D
              iax[1] = v2_ail_interp_call(0x16A8, a, 3);               // 9F
              v2_ail_interp_call(0x1662, a, 3); }                      // 9E
            iax[2] = v2_ail_interp_call(0x3AC4, ah2, 2);               // B3
            iax[3] = v2_ail_interp_call(0x3AEA, ah2, 2);               // B4
            { uint16_t a[3] = { 0, ih, p_ch };
              iax[4] = v2_ail_interp_call(0x3DE6, a, 3); }             // B9
            { uint16_t a[4] = { 0, ih, p_ch, p_ctl };
              iax[5] = v2_ail_interp_call(0x3D6E, a, 4); }             // B6
            { uint16_t a[5] = { 0, ih, p_ch, 0x07, p_val };
              v2_ail_interp_call(0x3DB1, a, 5); }                      // B7
            { uint16_t a[4] = { 0, ih, p_ch, p_nch };
              v2_ail_interp_call(0x3B10, a, 4); }                      // C0
            { uint16_t a[3] = { 0, ih, p_ch };
              iax[6] = v2_ail_interp_call(0x3B3C, a, 3); }             // C2
            { uint16_t a[3] = { 0, ih, p_bid };
              v2_ail_interp_call(0x3B69, a, 3); }                      // B5
            { uint16_t a[3] = { 0, 0x1234, SEQFIX_PARA };
              v2_ail_interp_call(0x37AE, a, 3); }                      // BD
            v2_ail_interp_call(0x37D4, (const uint16_t*)0, 0);         // BE
            { uint16_t a[1] = { 0 };
              iax[7] = v2_ail_interp_call(0x3E20, a, 1);               // BF
              uint16_t a2[2] = { 0, iax[7] };
              v2_ail_interp_call(0x3EA3, a2, 2); }                     // C1
            { uint16_t a[4] = { 0, p_st, p_d1, p_d2 };
              v2_ail_interp_call(0x27E6, a, 4); }                      // BA
            v2_ail_interp_call(0x280A, (const uint16_t*)0, 0);         // BB
            for (int tk = 0; tk < ticks1; tk++)
                v2_ail_interp_call(0x331E, (const uint16_t*)0, 0);
            { uint16_t a[3] = { 0, 0, 0 };
              v2_ail_interp_call(0x3709, a, 3); }                      // 68
        }
        v2_ail_interp_set_io_hooks(old_o, old_i);
        // ---- native side ----
        g_capw = 1;
        nat_out_fn so_ = g_nat_out; nat_in_fn si_ = g_nat_in;
        g_nat_out = cap_out; g_nat_in = cap_in;
        v2_ailnat_fn66_install_35D8(0x388);
        v2_ailnat_fn9A_set_cache_16F0(0x4000, SEQFIX_PARA, 0xE00);
        uint16_t nh = v2_ailnat_fn97_register_37F6(0x1000, SEQFIX_PARA, 0,
                                                   0x100, SEQFIX_PARA,
                                                   0x2000, SEQFIX_PARA);
        for (int gu = 0; gu < 8 && nh != 0xFFFF; gu++) {
            uint16_t req = v2_ailnat_fn9B_timbre_request_1736(nh);
            if (req == 0xFFFF) break;
            int idx = 0;
            for (int i = 0; i < ntimb; i++)
                if (tp[i] == (uint8_t)req && tb[i] == (uint8_t)(req >> 8)) idx = i;
            v2_ailnat_fn9C_load_timbre_18DB((uint16_t)(req >> 8),
                                            (uint16_t)(req & 0xFF),
                                            (uint16_t)(0x5000 + idx * 0x40),
                                            SEQFIX_PARA);
        }
        if (nh != 0xFFFF) {
            v2_ailnat_fnAA_start_3980(nh);
            for (int tk = 0; tk < ticks1; tk++) v2_ailnat_timer_tick_331E();
            nax[0] = v2_ailnat_fn96_state_size_3797();
            v2_ailnat_fn9D_lock_161C(p_bank, p_patch);
            nax[1] = v2_ailnat_fn9F_cache_off_16A8(p_bank, p_patch);
            v2_ailnat_fn9E_unlock_1662(p_bank, p_patch);
            nax[2] = v2_ailnat_fnB3_get_beat_3AC4(nh);
            nax[3] = v2_ailnat_fnB4_get_measure_3AEA(nh);
            nax[4] = v2_ailnat_fnB9_note_count_3DE6(nh, p_ch);
            nax[5] = v2_ailnat_fnB6_get_ctl_3D6E(nh, p_ch, p_ctl);
            v2_ailnat_fnB7_set_ctl_3DB1(nh, p_ch, 0x07, p_val);
            v2_ailnat_fnC0_set_map_3B10(nh, p_ch, p_nch);
            nax[6] = v2_ailnat_fnC2_get_map_3B3C(nh, p_ch);
            v2_ailnat_fnB5_branch_3B69(nh, p_bid);
            v2_ailnat_fnBD_set_cb_37AE(0x1234, SEQFIX_PARA, g_fix_seg);
            v2_ailnat_clear_cb_37D4();
            nax[7] = v2_ailnat_lock_channel_3E20();
            v2_ailnat_release_channel_3EA3(nax[7]);
            v2_ailnat_midi_2629(p_st, p_d1, p_d2);
            v2_ailnat_fnBB_stub_280A();
            for (int tk = 0; tk < ticks1; tk++) v2_ailnat_timer_tick_331E();
            v2_ailnat_fn68_uninstall_3709(0, 0);
        }
        g_nat_out = so_; g_nat_in = si_;
        cases++;
        int bad = (ih != nh || memcmp(iax, nax, sizeof(iax)) != 0);
        if (!bad) bad = (g_capn[0] != g_capn[1] ||
                   memcmp(g_cap[0], g_cap[1], sizeof(g_cap[0][0]) * g_capn[0]) != 0);
        if (!bad) bad = memcmp(g_nat, idata, isave_n) != 0;
        if (!bad) bad = memcmp(g_seqfix_n, g_seqfix_i, sizeof(g_seqfix_i)) != 0;
        if (bad) {
            fails++;
            if (fails <= 4) {
                uint32_t da = 0xFFFFFFFFu, sa = 0xFFFFFFFFu;
                for (uint32_t a = 0; a < isave_n; a++)
                    if (g_nat[a] != idata[a]) { da = a; break; }
                for (uint32_t a = 0; a < sizeof(g_seqfix_i); a++)
                    if (g_seqfix_n[a] != g_seqfix_i[a]) { sa = a; break; }
                int ax_d = -1;
                for (int i = 0; i < 12; i++) if (iax[i] != nax[i]) { ax_d = i; break; }
                fprintf(stderr, "AILNAT-DIFF nongame t=%d h i=%04X n=%04X ax#%d %04X/%04X outs i=%d n=%d blob@%04X seq@%04X\n",
                        t, ih, nh, ax_d,
                        ax_d >= 0 ? iax[ax_d] : 0, ax_d >= 0 ? nax[ax_d] : 0,
                        g_capn[0], g_capn[1], (unsigned)da, (unsigned)sa);
            }
        }
    }
    v2_ailnat_set_far_write(nullptr);

    memcpy(idata, isave, isave_n);   // restore the live driver image
    fprintf(stderr, "AILNAT-SELFTEST: cases=%d fails=%d\n", cases, fails);
    return fails == 0;
}
