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
// Self-test vs the interpreter (armed by V2_AILNAT_SELFTEST=1 after boot).
// ---------------------------------------------------------------------------
extern "C" uint16_t v2_ail_interp_call(uint16_t fn_off, const uint16_t* args, int argc);
extern "C" uint8_t* v2_ail_interp_data_raw(uint32_t* size_out);
extern "C" void v2_ail_interp_set_io_hooks(void (*o)(uint16_t, uint8_t), uint8_t (*i)(uint16_t));
extern "C" void v2_ail_interp_get_io_hooks(void (**o)(uint16_t, uint8_t), uint8_t (**i)(uint16_t));

// OUT capture buffers (interp vs native)
static struct { uint16_t port; uint8_t val; } g_cap[2][64];
static int g_capn[2]; static int g_capw = 0;
static void cap_out(uint16_t port, uint8_t val) {
    if (g_capn[g_capw] < 64) g_cap[g_capw][g_capn[g_capw]++] = { port, val };
}
static uint8_t cap_in(uint16_t) { return 0; }

static uint32_t xr = 0x12345678;   // deterministic LCG for the sweeps
static uint32_t xrnd() { xr = xr * 1103515245u + 12345u; return xr >> 8; }

extern "C" int v2_ailnat_selftest(void) {
    uint32_t isz = 0;
    uint8_t* idata = v2_ail_interp_data_raw(&isz);
    if (!idata || !isz) { fprintf(stderr, "AILNAT: no interp data\n"); return 0; }
    v2_ailnat_load(idata, isz);

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

    fprintf(stderr, "AILNAT-SELFTEST: cases=%d fails=%d\n", cases, fails);
    return fails == 0;
}
