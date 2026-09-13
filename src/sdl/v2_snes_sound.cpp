// UX stage 10: the SNES DE sound engine — see v2_snes_sound.h and
// docs2/SNES_SOUND_ENGINE.md (every routine below names the ROM code it
// transcribes: bank 5 $05:xxxx = the engine, bank 0 $00:xxxx = the glue).
#include "v2_snes_sound.h"
#include "v2_ui.h"
#include "third_party/snes_spc/Snes_Spc.h"
#include <atomic>
#include <thread>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <chrono>

// ---------------------------------------------------------------- assets --
// The block store is split into parts of 0xC000 bytes (integrate_snes.py SND_PART: a
// chunk's compressed block must fit the archive loader's FS window of 0xF000 bytes);
// the parts occupy 0x312.. and are read until the first missing chunk (at most SND_NDATA).
enum { SND_DRIVER = 0x310, SND_DIR = 0x311, SND_DATA0 = 0x312, SND_NDATA = 4, SND_LEVELS = 0x316, SND_SFXMAP = 0x317 };
static const uint16_t DRIVER_SIZE = 0x15A2;          // $05:8D35 LDX #$15A2
static const uint16_t DRIVER_ENTRY = 0x1000;         // $05:8D22 LDA #$1000
// $00:885F: the music volume per set — eight bytes, but the glue indexes it
// by the set number without a bound: sets 8 and 9 (the finale) read the code
// bytes that follow the table ($8867 AD, $8868 60) — kept as the console does
static const uint8_t MUSIC_VOL[10] = { 0x50, 0x20, 0x20, 0x30, 0x30, 0x30, 0x50, 0x30, 0xAD, 0x60 };

int v2_snes_sfx_vol_hint = -1;
int v2_snes_sys_id_hint = -1;
// V2_SNESSND_TRACE=1: every API function the manager issues (play / param / 07 / stop all)
// and the menu glue, with the engine's tick — the console-side view of a run.
static int snd_trace_on() { static int on = -1; if (on < 0) { const char* e = getenv("V2_SNESSND_TRACE"); on = (e && e[0] == '1') ? 1 : 0; } return on; }

struct Block { uint8_t type, id; uint16_t len; uint32_t off; };
static std::vector<uint8_t> g_driver, g_dir, g_data;
static std::vector<Block>   g_blocks;
static uint8_t g_levels[256];                         // [set][entry mode][exit mode][0] per slot
static bool g_assets_ok = false;

// ------------------------------------------------------- game -> sound --
enum CmdKind : uint8_t { CMD_LEVEL_START = 1, CMD_LEVEL_EXIT, CMD_SFX, CMD_STOP, CMD_PARAM, CMD_MUSIC, CMD_FADE, CMD_MUSIC_ON, CMD_SFX_ON, CMD_MUSIC_VOL, CMD_MENU_OPEN, CMD_MENU_CLOSE };
struct Cmd { uint8_t kind; uint8_t a; uint16_t b; };
enum { CMD_RING = 256 };
static Cmd g_cmd[CMD_RING];
static std::atomic<uint32_t> g_cmd_wr{0}, g_cmd_rd{0};
static void push(uint8_t kind, uint8_t a = 0, uint16_t b = 0) {
    uint32_t wr = g_cmd_wr.load(std::memory_order_relaxed);
    if (wr - g_cmd_rd.load(std::memory_order_acquire) >= CMD_RING) return;   // full: drop (never blocks the game)
    g_cmd[wr % CMD_RING] = { kind, a, b };
    g_cmd_wr.store(wr + 1, std::memory_order_release);
}

// ------------------------------------------------------ sound -> audio --
enum { OUT_RING = 1 << 16 };                          // stereo frames at 32 kHz (2 s)
static int16_t g_out[OUT_RING * 2];
static std::atomic<uint32_t> g_out_wr{0}, g_out_rd{0};
static std::atomic<bool> g_thread_on{false}, g_quit{false};
static std::thread g_thread;

// --------------------------------------------------------------- state --
// The 65816 side ($7F:C000..): four 0x40-byte track blocks (byte-addressed
// like the ROM code), the note-off table, the frame packet.
struct Engine {
    Snes_Spc spc;
    uint8_t  trk[4][0x40];
    uint16_t noteoff[16][2];                          // $7FC104: [timer][voice]
    uint8_t  pkt[256]; uint16_t pkt_len;              // $7FC144 / $7FC1A4
    uint16_t budget;                                  // $7FC102
    bool     magic;                                   // $7FC100 == 0x1234
    // glue state
    int      cur_set;                                 // $19D3 (-1 = none loaded)
    bool     music_on, sfx_on, sound_on;              // $0302 / $0304 / $0306
    uint16_t last_music_id;                           // $0360
    uint8_t  ambient_mask;                            // $19D5: the looping effects $890A found playing (bits $897B), for $8943
    int      menu_depth;                              // the PC nests its screens (pause -> quit prompt); the effects stop at the first open and restart at the last close
    // pacing
    double   tick_acc;
    // debug
    FILE*    port_log; uint32_t frame_no;
    bool     dead;                                    // a protocol timeout — stop driving the SPC
};
static Engine* E = nullptr;

static inline uint16_t r16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline void w16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline uint16_t tr16(int t, int off) { return r16(&E->trk[t][off]); }
static inline void trw16(int t, int off, uint16_t v) { w16(&E->trk[t][off], v); }
static inline uint8_t rd8(uint32_t off) { return off < g_data.size() ? g_data[off] : 0; }   // $8CD8 on the store

// --------------------------------------------------------- SPC access --
// One SPC frame = a run of samples; ports are read/written at the start of
// the next frame (time 0). Handshake polls advance the SPC one stereo
// sample (32 clocks) per step — the console's tight CPU loops poll about
// that often.
static bool audio_live() {                            // any sequence playing -> the output must be paced in real time
    for (int t = 0; t < 4; t++) if (!(r16(E->trk[t]) & 0x8000)) return true;
    return false;
}
static void run_samples(int n) {
    static int16_t buf[64 * 2];
    while (n > 0) {
        int k = n > 64 ? 64 : n;
        E->spc.play(k * 2, buf);
        if (!audio_live()) { n -= k; continue; }       // silence during a set load: no throttle (the console's loading screens)
        uint32_t wr = g_out_wr.load(std::memory_order_relaxed);
        for (int i = 0; i < k; i++) {
            // block while the ring is full (the audio device paces us)
            while (wr - g_out_rd.load(std::memory_order_acquire) >= OUT_RING - 1) {
                if (g_quit.load()) return;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            g_out[(wr % OUT_RING) * 2] = buf[i * 2];
            g_out[(wr % OUT_RING) * 2 + 1] = buf[i * 2 + 1];
            wr++;
            g_out_wr.store(wr, std::memory_order_release);
        }
        n -= k;
    }
}
// A few clocks of SPC time (the 65816's own instruction time between port
// accesses): end_frame at clock granularity; whatever samples complete go
// to the ring like run_samples' do.
static void run_clocks(int clocks) {
    static int16_t buf[32];
    E->spc.set_output(buf, 32);
    E->spc.end_frame(clocks);
    int n = E->spc.sample_count() / 2;
    if (n <= 0 || !audio_live()) return;
    uint32_t wr = g_out_wr.load(std::memory_order_relaxed);
    for (int i = 0; i < n; i++) {
        while (wr - g_out_rd.load(std::memory_order_acquire) >= OUT_RING - 1) {
            if (g_quit.load()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        g_out[(wr % OUT_RING) * 2] = buf[i * 2];
        g_out[(wr % OUT_RING) * 2 + 1] = buf[i * 2 + 1];
        wr++;
        g_out_wr.store(wr, std::memory_order_release);
    }
}
static const int GAP_CLOCKS = 12;                     // ~ the console's inter-access time (STA/CMP loops of the 65816)
static inline int port_rd(int p) { return E->spc.read_port(0, p); }
// Every CPU-side port write is followed by one SPC sample (32 clocks): on
// the console the 65816's own instructions between two port writes give the
// driver's polling loops time to observe each value (the echo wait of $12E6
// needs to see the zero before the next command lands).
static inline void port_wr(int p, int v) {
    E->spc.write_port(0, p, v);
    if (E->port_log) fprintf(E->port_log, "W f%u %04X %02X\n", E->frame_no, 0x2140 + p, v & 0xFF);
    run_clocks(GAP_CLOCKS);
}
static const int POLL_LIMIT = 1000000;                // 12-clock steps (~12 s) before giving up
static bool wait_port_eq(int p, int v) {
    for (int i = 0; i < POLL_LIMIT; i++) { if (port_rd(p) == v) return true; run_clocks(GAP_CLOCKS); if (g_quit.load()) return false; }
    fprintf(stderr, "V2-SNESSND: port %d never became %02X — driver dead\n", p, v); E->dead = true; return false;
}
// $05:8D99 — byte command with echo
static void send_byte(uint8_t cmd) {
    if (E->dead) return;
    port_wr(0, cmd); if (!wait_port_eq(0, cmd)) return; port_wr(0, 0);
}
// $05:8DAE — word: lo/1, hi/2, returns the port-1 value that ended the wait
static uint16_t send_word(uint16_t w) {
    if (E->dead) return 0;
    port_wr(0, w & 0xFF); port_wr(1, 1);
    if (!wait_port_eq(1, 1)) return 0;
    port_wr(0, w >> 8); port_wr(1, 2);
    int a = 0;
    for (int i = 0; i < POLL_LIMIT; i++) {            // LDA $2141; CMP #1; BEQ; CMP $2141; BNE
        a = port_rd(1);
        if (a != 1 && port_rd(1) == a) break;
        run_clocks(GAP_CLOCKS); if (g_quit.load()) return 0;
        if (i == POLL_LIMIT - 1) { fprintf(stderr, "V2-SNESSND: word ack timeout\n"); E->dead = true; return 0; }
    }
    port_wr(0, 0); port_wr(1, 0);
    return (uint16_t)(a & 0xFF);
}
// $05:8DED — receive a word
static uint16_t recv_word() {
    if (E->dead) return 0;
    if (!wait_port_eq(1, 1)) return 0;
    int lo = port_rd(0); port_wr(1, 1);
    if (!wait_port_eq(1, 2)) return 0;
    int hi = port_rd(0); port_wr(1, 2);
    int a = 2;
    for (int i = 0; i < POLL_LIMIT; i++) { a = port_rd(1); if (a != 2) break; run_clocks(GAP_CLOCKS); if (g_quit.load()) return 0; }
    port_wr(1, a);
    return (uint16_t)((hi << 8) | lo);
}

// ------------------------------------------------------------- store --
// $05:8C2B — find (type, id) in the directory; data offset by summing the
// lengths before it (the blocks lie in directory order after the directory)
static bool find_block(uint8_t type, uint8_t id, uint32_t* off, uint16_t* len) {
    for (const Block& b : g_blocks) if (b.type == type && b.id == id) { *off = b.off; *len = b.len; return true; }
    return false;
}
// $05:8E27 (proto 1, cmd 03) / $8EA5 (2, 05) / $8F21 (3, 0B): 0F query, then the upload
static bool upload_block(uint8_t proto, uint8_t cmd, uint8_t id, uint32_t off, uint16_t len) {
    send_byte(0x0F);
    send_word((uint16_t)((id << 8) | proto));
    uint16_t reply = recv_word();
    if (E->dead) return false;
    if (reply != 0) return true;                      // already resident in the SPC's cache
    send_byte(cmd);
    send_word(id);
    if (send_word(len) != 2) return false;
    int16_t left = (int16_t)len;
    do {                                              // $8E65: data words, $08 -= 2 until 0 / negative
        uint16_t w = (uint16_t)(rd8(off) | (rd8(off + 1) << 8)); off += 2;
        send_word(w);
        left -= 2;
    } while (left > 0 && !E->dead);
    return !E->dead;
}

// ------------------------------------------------- engine functions --
// function 0 ($8386): the driver upload + a clean state
static void fn_init() {
    // the console pushes the driver through the IPL protocol; the emulated
    // S-SMP starts with the same image (RAM $1000.. = driver, PC = $1000)
    static uint8_t img[0x10200];
    memset(img, 0, sizeof img);
    memcpy(img, "SNES-SPC700 Sound File Data v0.30", 33);
    img[0x21] = 26; img[0x22] = 26; img[0x23] = 27; img[0x24] = 30;
    img[0x25] = DRIVER_ENTRY & 0xFF; img[0x26] = DRIVER_ENTRY >> 8; img[0x2B] = 0xEF;
    memcpy(img + 0x100 + DRIVER_ENTRY, g_driver.data(), g_driver.size());
    img[0x100 + 0xF1] = 0x80;                         // control: IPL state (ROM on, timers off)
    img[0x10100 + 0x6C] = 0xE0;                       // DSP FLG at reset: mute, echo off
    E->spc.load_spc(img, sizeof img);
    E->spc.clear_echo();
    // $8D72..$8D93: the CPU zeroes its ports and waits for the driver's zeros
    port_wr(0, 0); port_wr(1, 0); port_wr(2, 0); port_wr(3, 0);
    for (int p = 0; p < 4; p++) wait_port_eq(p, 0);
    for (int t = 0; t < 4; t++) { memset(E->trk[t], 0, 0x40); trw16(t, 0x00, 0xFFFF); trw16(t, 0x0E, 0xFFFF); trw16(t, 0x0C, 0); trw16(t, 0x10, 0xFFFF); }
    for (int i = 0; i < 16; i++) { E->noteoff[i][0] = 0xFFFF; E->noteoff[i][1] = 0xFFFF; }
    E->magic = true;
}
// function 1 ($8444): reset — tracks off, note table off, FE
static void fn_reset() {
    E->magic = false;
    for (int t = 0; t < 4; t++) { trw16(t, 0x00, 0xFFFF); trw16(t, 0x10, 0xFFFF); }
    for (int i = 0; i < 16; i++) { E->noteoff[i][0] = 0xFFFF; E->noteoff[i][1] = 0xFFFF; }
    send_byte(0xFE);
    E->magic = true;
}
// function 2 ($84C1): load the song set (type 4) — every [type][id] pair with type < 3
static int fn_load_set(uint8_t set) {
    uint32_t off; uint16_t len;
    if (!find_block(4, set, &off, &len)) return 1;
    for (uint32_t p = off; p + 1 < off + len;) {
        uint8_t ty = rd8(p), id = rd8(p + 1); p += 2;
        if (ty == 0xFF) break;
        uint32_t boff; uint16_t blen;
        if (!find_block(ty, id, &boff, &blen)) return 2;
        if (ty >= 3) continue;                        // $8528: CMP #3 BCS — sequences stay on the 65816
        bool ok = (ty == 0) ? upload_block(1, 0x03, id, boff, blen)
                : (ty == 1) ? upload_block(2, 0x05, id, boff, blen)
                            : upload_block(3, 0x0B, id, boff, blen);
        if (!ok) return 3;
    }
    return 0;
}
// $8AD8: release the voices of track t — note-off entries of the track fire next tick
static void release_track_voices(int t) {
    for (int i = 0; i < 16; i++)
        if (!(E->noteoff[i][0] & 0x8000) && (E->noteoff[i][1] & 0xF0) == (t << 4)) E->noteoff[i][0] = 1;
}
// function 3 ($858A): play sequence `id` (>= 0x80: restart if already playing)
static int fn_play(uint8_t id, uint16_t flag, uint16_t vol) {
    if (snd_trace_on()) fprintf(stderr, "V2-SNESSND-TRACE: t%u play %02X vol %02X\n", E->frame_no, id, vol);
    int t = -1;
    if (id >= 0x80) {
        for (int i = 0; i < 4; i++) if (tr16(i, 0) == id) { t = i; break; }
        if (t >= 0) { trw16(t, 0, 0xFFFF); release_track_voices(t); }
    }
    if (t < 0) { for (int i = 0; i < 4; i++) if (tr16(i, 0) == 0xFFFF) { t = i; break; } }
    if (t < 0) return 4;
    uint32_t off; uint16_t len;
    if (!find_block(3, id, &off, &len)) return 2;
    trw16(t, 0x02, (uint16_t)off); trw16(t, 0x04, (uint16_t)(off >> 16));
    uint16_t n = (uint16_t)(rd8(off) | (rd8(off + 1) << 8));
    uint32_t pos = off + 2 + n * 2;
    trw16(t, 0x06, (uint16_t)pos); trw16(t, 0x08, (uint16_t)(pos >> 16));
    trw16(t, 0x0A, 0); trw16(t, 0x10, 0);
    if (flag & 0x8000) { trw16(t, 0x14, (uint16_t)((vol - 1) << 8)); trw16(t, 0x16, (uint16_t)(vol << 8)); trw16(t, 0x12, 0x0100); }
    else               { trw16(t, 0x12, flag); trw16(t, 0x14, 0); trw16(t, 0x16, (uint16_t)(vol << 8)); }
    trw16(t, 0x18, 0xFFFF);
    trw16(t, 0x00, id);
    return 0;
}
// function 4 ($889F): FFFF = stop the sequence, else fade-out step v
static int fn_param(uint8_t id, uint16_t v) {
    if (snd_trace_on()) fprintf(stderr, "V2-SNESSND-TRACE: t%u param %02X %04X\n", E->frame_no, id, v);
    for (int t = 0; t < 4; t++) if (tr16(t, 0) == id) {
        if (v == 0xFFFF) { trw16(t, 0, 0xFFFF); release_track_voices(t); }
        else { trw16(t, 0x12, v); trw16(t, 0x10, 1); }
        return 0;
    }
    return 5;
}
// function 7 ($879B) / 9 ($8852): 07 + word + word
static void fn_07(uint16_t sel, uint16_t v) { E->magic = false; send_byte(0x07); send_word(sel); send_word(v); E->magic = true; }
// function 8 ($87E8): stop all — tracks off, note table off, 0E
static void fn_stop_all() {
    E->magic = false;
    for (int t = 0; t < 4; t++) trw16(t, 0x00, 0xFFFF);
    for (int i = 0; i < 16; i++) { E->noteoff[i][0] = 0xFFFF; E->noteoff[i][1] = 0xFFFF; }
    send_byte(0x0E);
    E->magic = true;
}

// --------------------------------------------------------- the tick --
static inline void pkt_push(uint8_t b) { if (E->pkt_len < sizeof E->pkt) E->pkt[E->pkt_len++] = b; }
// $05:891C — 8-bit value, or 15-bit when bit 7 of the low byte is set
static inline uint16_t val891C(uint8_t lo, uint8_t hi) { return (lo & 0x80) ? (uint16_t)(((lo & 0x7F) << 8) | hi) : lo; }
// $05:8932 — track position := entry `idx` of the sequence's offset table
static void goto_entry(int t, uint16_t idx) {
    uint32_t seq = tr16(t, 0x02) | ((uint32_t)tr16(t, 0x04) << 16);
    uint16_t n = (uint16_t)(rd8(seq) | (rd8(seq + 1) << 8));
    uint32_t ent = seq + idx * 2 + 2;
    uint16_t o = (uint16_t)(rd8(ent) | (rd8(ent + 1) << 8));
    uint32_t pos = seq + n * 2 + 2 + o;
    trw16(t, 0x06, (uint16_t)pos); trw16(t, 0x08, (uint16_t)(pos >> 16));
}
// $05:8A74 — a note-off entry (timer = dur, voice); false = table full
static bool alloc_noteoff(uint16_t dur, uint16_t voice) {
    for (int i = 0; i < 16; i++) if (E->noteoff[i][0] == 0xFFFF) { E->noteoff[i][0] = dur; E->noteoff[i][1] = voice; return true; }
    return false;
}
// $05:89B4 — note-offs due this frame; true = the event budget ran out
static bool tick_noteoffs() {
    for (int i = 0; i < 16; i++) {
        uint16_t tm = E->noteoff[i][0];
        if (tm & 0x8000) continue;
        tm--; E->noteoff[i][0] = tm;
        if (tm != 0) continue;
        uint16_t voice = E->noteoff[i][1];
        int t = (voice >> 4) & 0x0F, ch = voice & 0x0F;
        uint8_t instr = E->trk[t & 3][0x1E + ch];
        pkt_push(0x02); pkt_push((uint8_t)(voice >> 8)); pkt_push(instr); pkt_push(0x00); pkt_push((uint8_t)voice);
        E->noteoff[i][0] = 0xFFFF;
        if (--E->budget == 0) return true;
    }
    return false;
}
// $05:8B2A — track volume fades; true = budget ran out
static bool tick_fades() {
    for (int t = 0; t < 4; t++) {
        if (tr16(t, 0) & 0x8000) continue;
        uint16_t st = tr16(t, 0x10);
        if (st & 0x8000) continue;
        uint16_t cur;
        if (st == 0) { cur = (uint16_t)(tr16(t, 0x12) + tr16(t, 0x14)); trw16(t, 0x14, cur); }
        else         { cur = (uint16_t)(tr16(t, 0x14) - tr16(t, 0x12)); trw16(t, 0x14, cur); }
        if (st != 0) {
            if (cur & 0x8000) { release_track_voices(t); trw16(t, 0x10, 0xFFFF); trw16(t, 0x00, 0xFFFF); trw16(t, 0x14, 0); cur = 0; }
        } else if (cur >= tr16(t, 0x16)) trw16(t, 0x10, 0xFFFF);
        pkt_push(0x07); pkt_push((uint8_t)t); pkt_push(0x20); pkt_push((uint8_t)(cur >> 8)); pkt_push((uint8_t)(cur >> 8));
        if (--E->budget == 0) return true;
    }
    return false;
}
// $05:8049 — the frame
static void tick() {
    if (!E->magic || E->dead) return;
    E->budget = 12; E->pkt_len = 0;
    bool flush = tick_noteoffs();
    if (!flush) flush = tick_fades();
    for (int t = 0; t < 4 && !flush; t++) {
        uint8_t* T = E->trk[t];
        if (r16(T) & 0x8000) continue;
        if (r16(T + 0x0A)) { w16(T + 0x0A, (uint16_t)(r16(T + 0x0A) - 1)); continue; }
        for (;;) {                                    // $808D: events of this track until a delay
            uint32_t pos = r16(T + 0x06) | ((uint32_t)r16(T + 0x08) << 16);
            uint8_t b;
            do { b = rd8(pos++); } while (b >= 0x80);  // $80C3: skip the delta bytes (>= 0x80), consume the last
            uint8_t cmd = rd8(pos++);
            uint8_t voice = (uint8_t)((t << 4) | (cmd & 0x0F));
            bool end_track = false;
            if (cmd < 0xA0) {                         // note
                uint8_t note = rd8(pos++), vel = rd8(pos++), d1 = rd8(pos++), d2 = 0;
                if (d1 >= 0x80) d2 = rd8(pos++);
                uint8_t instr = T[0x1E + (cmd & 0x0F)];
                uint16_t dur = val891C(d1, d2);
                if (alloc_noteoff(dur, voice)) { pkt_push(0x01); pkt_push(note); pkt_push(instr); pkt_push(vel); pkt_push(voice); }
            } else if (cmd < 0xC0) {                  // A0..BF: p, v
                uint8_t p = rd8(pos++), v = rd8(pos++);
                w16(T + 0x06, (uint16_t)pos); w16(T + 0x08, (uint16_t)(pos >> 16));
                if (p == 0x74) { }
                else if (p == 0x75) { goto_entry(t, v & 0x7F); continue; }
                else if (p == 0x76) {
                    uint16_t cnt = r16(T + 0x0E);
                    if (cnt == 0xFFFF) { }
                    else if ((cnt & 0xFF) == v || v == 0x7F) { w16(T + 0x0E, 0xFFFF); goto_entry(t, v & 0x7F); continue; }
                }
                else if (p == 0x77) w16(T + 0x18, v & 0x7F);
                else if (p == 0x78) { w16(T + 0x1A, r16(T + 0x06)); w16(T + 0x1C, r16(T + 0x08)); goto_entry(t, v & 0x7F); continue; }
                else if (p == 0x79) { w16(T + 0x06, r16(T + 0x1A)); w16(T + 0x08, r16(T + 0x1C)); pos = r16(T + 0x06) | ((uint32_t)r16(T + 0x08) << 16); }
                else { pkt_push(p < 0x0C ? 0x08 : 0x07); pkt_push(voice); pkt_push(p); pkt_push(v); pkt_push(v); }
            } else if (cmd < 0xD0) {                  // C0..CF: channel instrument
                T[0x1E + (cmd & 0x0F)] = rd8(pos++);
            } else if (cmd < 0xF0) {                  // D0..EF: 08 voice E0 v v
                uint8_t v = rd8(pos++);
                pkt_push(0x08); pkt_push(voice); pkt_push(0xE0); pkt_push(v); pkt_push(v);
            } else {                                  // F0+: end of track
                release_track_voices(t); w16(T + 0x00, 0xFFFF); end_track = true;
            }
            if (end_track) break;
            // $82E6: peek the next delta (not consumed), spend it against the remainder
            uint8_t b1 = rd8(pos), b2 = rd8(pos + 1);
            uint16_t delta = val891C(b1, b2);
            uint16_t rem = r16(T + 0x0C);
            int32_t d = (int32_t)(int16_t)delta - (int32_t)(int16_t)rem;
            if (d < 0) { w16(T + 0x0C, (uint16_t)(rem - delta)); delta = 0; }
            else { delta = (uint16_t)d; w16(T + 0x0C, 0); }
            w16(T + 0x0A, delta);
            w16(T + 0x06, (uint16_t)pos); w16(T + 0x08, (uint16_t)(pos >> 16));
            if (--E->budget == 0) { flush = true; break; }
            if (delta) break;                         // $8340: a delay set — next track
        }
    }
    // $834C: the packet
    if (E->pkt_len) {
        send_byte(0xFD);
        send_word(E->pkt_len);
        for (int i = 0; i < E->pkt_len; i += 2) send_word((uint16_t)(E->pkt[i] | (E->pkt[i + 1] << 8)));
    }
}

// ---------------------------------------------------------- the glue --
// $00:880E: load the set if it changed (reset + load)
static void glue_load_set(uint8_t set) {
    if (E->cur_set == set) return;
    fn_reset();
    E->cur_set = set;
    int err = fn_load_set(set);
    if (err) fprintf(stderr, "V2-SNESSND: load set %02X failed (%d)\n", set, err);
}
// $00:8832: music for the set; $00:88B1: the silent song when music is off
// MUSIC VOL / SFX VOL (2026-09-11): the volumes the console's glue passes to the engine
// (function 3's volume byte: the music table $885F, the effects' own values, the menu's
// 0xE7 at 0x60, the ambient restarts of $898B) scaled by the option; at 100 % untouched.
// An effect never goes below 1 (the engine starts the track at vol - 1: 0 would wrap).
static uint16_t vol_scaled(uint16_t vol, int pct) {
    if (pct >= 100) return vol;
    if (pct < 0) pct = 0;
    int v = (int)((vol * pct + 50) / 100);
    if (v < 1 && vol > 0) v = 1;
    return (uint16_t)v;
}
static int g_music_vol_base = 0;   // the unscaled start volume of the music track playing (for a live MUSIC VOL change)
static void glue_play_music_for_set(uint8_t set) {
    glue_load_set(set);
    if (E->music_on) {
        uint8_t id = (set == 7) ? 1 : set;
        E->last_music_id = id;
        g_music_vol_base = MUSIC_VOL[id < 10 ? id : 9];
        int err = fn_play(id, 0xFFFF, vol_scaled((uint16_t)g_music_vol_base, v2_options.music_volume.load()));
        if (err) fprintf(stderr, "V2-SNESSND: play music %02X failed (%d)\n", id, err);
    }
}
// MUSIC VOL changed while a track plays: the track gets its volume the way function 3 gives a
// fresh track its volume — a one-step fade up from v - 1 to v (+0x14, +0x16, step +0x12 = 1.0,
// state +0x10 = 0): the next tick ($8B2A, tick_fades) sends `07 t 20 v v` to the SPC and idles
// the fade. A track fading out (function 4, state 1) is on its way out and is left alone.
static void snes_music_volume_live(int pct) {
    if (!E->music_on || g_music_vol_base <= 0) return;
    const uint16_t v = vol_scaled((uint16_t)g_music_vol_base, pct);
    for (int t = 0; t < 4; t++) if (tr16(t, 0) == (uint16_t)E->last_music_id) {
        if (tr16(t, 0x10) == 1) continue;
        trw16(t, 0x14, (uint16_t)((v - 1) << 8)); trw16(t, 0x16, (uint16_t)(v << 8)); trw16(t, 0x12, 0x0100); trw16(t, 0x10, 0);
        if (snd_trace_on()) fprintf(stderr, "V2-SNESSND-TRACE: t%u music volume %d%% -> %02X\n", E->frame_no, pct, v);
    }
}
// $00:87E1 (+ dispatch $8804 by mode) — mode 0 play, 1 nothing, 2 fade (function 4 of the last music, 0x80), 3 load only
static void glue_dispatch(uint8_t set, uint8_t mode) {
    switch (mode) {
    case 0: glue_play_music_for_set(set); break;
    case 2: fn_param((uint8_t)E->last_music_id, 0x0080); break;
    case 3: glue_load_set(set); break;
    default: break;
    }
}
// $00:890A / $00:8943: the eight looping effects (started by the scripts) are
// stopped when the pause menu ($8435, START) or the inventory ($EF1A, SELECT)
// opens — each one function 4 (id, FFFF) actually stopped (carry clear) is
// remembered as its bit of $897B in $19D5 — and restarted with its own volume
// ($898B) when the menu closes on the continue path ($84AC / $EF75; the quit
// exit skips it). Both menus then play 0xE7 through $88D4 (volume 0x60).
static const uint8_t AMBIENT_IDS[8] = { 0x85, 0x8D, 0x91, 0x98, 0x99, 0xAC, 0xB5, 0xE0 };   // $00:8983
static const uint8_t AMBIENT_VOL[8] = { 0x7F, 0x40, 0x61, 0x4F, 0x4F, 0x7F, 0x40, 0x7F };   // $00:898B
static void glue_stop_ambient() {                     // $890A
    if (!E->sfx_on) return;                           // $0304
    E->ambient_mask = 0;                              // STZ $19D5
    for (int i = 0; i < 8; i++)                       // $891F: function 4 (id, FFFF); BCC -> $19D5 |= $897B[X]
        if (fn_param(AMBIENT_IDS[i], 0xFFFF) == 0) E->ambient_mask |= (uint8_t)(1 << i);
}
static void glue_restart_ambient() {                  // $8943
    if (!E->sfx_on) return;                           // $0304
    for (int i = 0; i < 8; i++)                       // $8955: bit set -> function 3 (id, FFFF, $898B[X])
        if (E->ambient_mask & (1 << i)) fn_play(AMBIENT_IDS[i], 0xFFFF, vol_scaled(AMBIENT_VOL[i], v2_options.sfx_volume.load()));
}
static void glue_menu_open() {                        // $843A-$8440 / $EF26-$EF44
    if (snd_trace_on()) fprintf(stderr, "V2-SNESSND-TRACE: t%u menu open (depth %d)\n", E->frame_no, E->menu_depth);
    if (E->menu_depth++ == 0) glue_stop_ambient();
    if (E->sfx_on) fn_play(0xE7, 0xFFFF, vol_scaled(0x60, v2_options.sfx_volume.load()));      // $88D4 -> $88E5 (function 3 when $0304)
}
static void glue_menu_close() {                       // $84AC / $EF75
    if (snd_trace_on()) fprintf(stderr, "V2-SNESSND-TRACE: t%u menu close (depth %d, mask %02X)\n", E->frame_no, E->menu_depth, E->ambient_mask);
    if (E->menu_depth > 0 && --E->menu_depth == 0) glue_restart_ambient();
}
// $00:9BCC — the level entry: $87E1 (function 7 by $0306, then the dispatch by
// the head's entry mode) and $88B1 (the silent song when music is off). No
// effect plays here: 0xE7 belongs to the menus above.
static void glue_level_start(uint16_t level) {
    E->menu_depth = 0;
    fn_07(0, E->sound_on ? 0 : 0xFFFF);              // $87E1: function 7 by $0306
    const uint8_t* L = &g_levels[(level & 63) * 4];
    if (L[0] == 0xFF) return;                         // no SNES twin — keep whatever plays
    glue_dispatch(L[0], L[1]);                        // by head+4
    if (!E->music_on) fn_play(0x30, 0xFFFF, 0x20);   // $88B1
}
static void glue_level_exit(uint16_t level) {
    const uint8_t* L = &g_levels[(level & 63) * 4];
    if (L[0] == 0xFF) return;
    glue_dispatch(L[0], L[2]);                        // $87F9: by head+6
}
static void handle(const Cmd& c) {
    switch (c.kind) {
    case CMD_LEVEL_START: glue_level_start(c.b); break;
    case CMD_LEVEL_EXIT:  glue_level_exit(c.b); break;
    case CMD_SFX:         if (E->sfx_on) fn_play(c.a, 0xFFFF, vol_scaled(c.b, v2_options.sfx_volume.load())); break;      // $88E5 (SFX VOL)
    case CMD_STOP:        if (E->sfx_on) fn_param(c.a, 0xFFFF); break;          // $88F9
    case CMD_PARAM:       if (E->sfx_on) fn_param(c.a, c.b); break;
    case CMD_MUSIC:       if (E->music_on) { E->last_music_id = c.a; g_music_vol_base = 0x30; fn_play(c.a, 0xFFFF, vol_scaled(0x30, v2_options.music_volume.load())); } break;   // $C314 (MUSIC VOL)
    case CMD_MUSIC_VOL:   snes_music_volume_live((int)c.a); break;                // MUSIC VOL changed on the game thread
    case CMD_FADE:        if (E->music_on) fn_param(c.a, 0x0080); break;        // $C330
    case CMD_MUSIC_ON:    E->music_on = c.a != 0; break;
    case CMD_SFX_ON:      E->sfx_on = c.a != 0; break;
    case CMD_MENU_OPEN:   glue_menu_open(); break;
    case CMD_MENU_CLOSE:  glue_menu_close(); break;
    }
}

// -------------------------------------------------------------- thread --
static void thread_main() {
    E = new Engine();
    memset(E->trk, 0, sizeof E->trk); E->pkt_len = 0; E->budget = 12; E->magic = false;
    E->cur_set = -1; E->music_on = true; E->sfx_on = true; E->sound_on = true; E->last_music_id = 0;
    E->tick_acc = 0; E->port_log = nullptr; E->frame_no = 0; E->dead = false;
    if (const char* e = getenv("V2_SPC_PORT_LOG")) if (e[0]) E->port_log = fopen(e, "w");
    E->spc.init();
    fn_init();                                        // function 0
    const double SAMPLES_PER_TICK = 32000.0 / 60.0988;
    // pacing: the output ring throttles the loop while a sequence plays;
    // in silence (nothing pushed) the wall clock keeps the console's frame
    // rate, so ticks and the port-log frame numbers stay meaningful
    auto next_tick = std::chrono::steady_clock::now();
    const auto TICK = std::chrono::nanoseconds((long long)(1e9 / 60.0988));
    while (!g_quit.load()) {
        if (!audio_live()) {
            std::this_thread::sleep_until(next_tick);
            next_tick += TICK;
            if (std::chrono::steady_clock::now() > next_tick + TICK * 4) next_tick = std::chrono::steady_clock::now();
        } else next_tick = std::chrono::steady_clock::now();
        // the game's calls of this frame
        for (;;) {
            uint32_t rd = g_cmd_rd.load(std::memory_order_relaxed);
            if (rd == g_cmd_wr.load(std::memory_order_acquire)) break;
            Cmd c = g_cmd[rd % CMD_RING];
            g_cmd_rd.store(rd + 1, std::memory_order_release);
            handle(c);
        }
        tick();                                       // JSL $85:8000
        E->frame_no++;
        E->tick_acc += SAMPLES_PER_TICK;
        int n = (int)E->tick_acc; E->tick_acc -= n;
        run_samples(n);
    }
    if (E->port_log) fclose(E->port_log);
    delete E; E = nullptr;
}

// ---------------------------------------------------------------- API --
bool v2_snes_sound_enabled() { return v2_options.sound_mode.load() == 1; }

// PC effect number -> the console's sequence id (0 = the PC sound has no console twin).
// The scripts are the same programs on both machines but op 2 / anim command 2 carry
// each machine's own numbering (PC: the XMIDI sequence 0x03..0x5D, SNES: the driver
// sequences 0x80..0xEA); tools/assets/snes_sfx_map.py derives the table from the
// paired class and animation code, integrate_snes.py packs it as chunk 0x317.
static uint8_t g_sfx_map[256];
static bool load_assets() {
    if (g_assets_ok) return true;
    static uint8_t buf[0x10000];
    uint32_t n = v2_snd_read_chunk(SND_DRIVER, buf, sizeof buf);
    if (n != DRIVER_SIZE) { fprintf(stderr, "V2-SNESSND: driver chunk %04X: %u bytes (want %u) — SNES sound off\n", SND_DRIVER, n, DRIVER_SIZE); return false; }
    g_driver.assign(buf, buf + n);
    n = v2_snd_read_chunk(SND_DIR, buf, sizeof buf);
    if (n < 4) { fprintf(stderr, "V2-SNESSND: no directory chunk\n"); return false; }
    g_dir.assign(buf, buf + n);
    g_data.clear();
    for (int k = 0; k < SND_NDATA; k++) {
        n = v2_snd_read_chunk((uint16_t)(SND_DATA0 + k), buf, sizeof buf);
        if (!n) break;
        g_data.insert(g_data.end(), buf, buf + n);
    }
    n = v2_snd_read_chunk(SND_LEVELS, buf, sizeof buf);
    memset(g_levels, 0xFF, sizeof g_levels);
    if (n) memcpy(g_levels, buf, n < sizeof g_levels ? n : sizeof g_levels);
    n = v2_snd_read_chunk(SND_SFXMAP, buf, sizeof buf);
    memset(g_sfx_map, 0, sizeof g_sfx_map);
    if (n) memcpy(g_sfx_map, buf, n < sizeof g_sfx_map ? n : sizeof g_sfx_map);
    else fprintf(stderr, "V2-SNESSND: no effect map chunk %04X — the scripts' effects stay silent\n", SND_SFXMAP);
    // the directory: [u24 size][entries][FF]; data offsets cumulative
    g_blocks.clear();
    uint32_t off = 0;
    for (size_t p = 3; p + 3 < g_dir.size() && g_dir[p] != 0xFF; p += 4) {
        Block b{ g_dir[p], g_dir[p + 1], (uint16_t)(g_dir[p + 2] | (g_dir[p + 3] << 8)), off };
        off += b.len; g_blocks.push_back(b);
    }
    if (off != g_data.size()) fprintf(stderr, "V2-SNESSND: store size %u vs directory %u\n", (unsigned)g_data.size(), off);
    fprintf(stderr, "V2-SNESSND: driver %u B, %u blocks, %u B of data\n", (unsigned)g_driver.size(), (unsigned)g_blocks.size(), (unsigned)g_data.size());
    g_assets_ok = true;
    return true;
}
void v2_snes_sound_start() {
    if (g_thread_on.load()) return;
    if (!load_assets()) return;
    g_quit = false;
    g_thread = std::thread(thread_main);
    g_thread_on = true;
}
void v2_snes_sound_shutdown() {
    if (!g_thread_on.load()) return;
    g_quit = true;
    if (g_thread.joinable()) g_thread.join();
    g_thread_on = false;
}
static inline bool live() { return g_thread_on.load(std::memory_order_acquire); }
void v2_snes_snd_level_start(uint16_t level)       { if (live()) push(CMD_LEVEL_START, 0, level); }
void v2_snes_snd_level_exit(uint16_t level)        { if (live()) push(CMD_LEVEL_EXIT, 0, level); }
void v2_snes_snd_play_sfx(uint8_t id, uint8_t vol) { if (live()) push(CMD_SFX, id, vol); }
void v2_snes_snd_set_music_volume(uint8_t pct) { if (live()) push(CMD_MUSIC_VOL, pct, 0); }
void v2_snes_snd_stop_sfx(uint8_t id)              { if (live()) push(CMD_STOP, id, 0xFFFF); }
void v2_snes_snd_sfx_param(uint8_t id, uint16_t v) { if (live()) push(CMD_PARAM, id, v); }
void v2_snes_snd_play_music(uint8_t id)            { if (live()) push(CMD_MUSIC, id, 0); }
void v2_snes_snd_fade_music(uint8_t id)            { if (live()) push(CMD_FADE, id, 0); }
void v2_snes_snd_set_music_on(bool on)             { if (live()) push(CMD_MUSIC_ON, on ? 1 : 0, 0); }
void v2_snes_snd_set_sfx_on(bool on)               { if (live()) push(CMD_SFX_ON, on ? 1 : 0, 0); }
void v2_snes_snd_menu_open()                       { if (live()) push(CMD_MENU_OPEN, 0, 0); }
int  v2_snes_sfx_map(int pc_id)                    { return (pc_id >= 0 && pc_id < 256 && g_assets_ok) ? g_sfx_map[pc_id] : 0; }
void v2_snes_snd_menu_close()                      { if (live()) push(CMD_MENU_CLOSE, 0, 0); }

// audio thread: linear resampling 32000 -> rate from the output ring
bool v2_snes_sound_mix(int16_t* out, uint32_t frames, uint32_t rate) {
    if (!live() || !rate) return false;
    static double pos = 0.0;                          // fractional read position in ring frames
    static int16_t last[2] = {0, 0};
    const double step = 32000.0 / (double)rate;
    uint32_t rd = g_out_rd.load(std::memory_order_relaxed);
    uint32_t avail = g_out_wr.load(std::memory_order_acquire) - rd;
    for (uint32_t i = 0; i < frames; i++) {
        if (avail >= 2) {
            uint32_t i0 = rd % OUT_RING, i1 = (rd + 1) % OUT_RING;
            double f = pos;
            for (int c = 0; c < 2; c++) {
                double s = g_out[i0 * 2 + c] + (g_out[i1 * 2 + c] - g_out[i0 * 2 + c]) * f;
                last[c] = (int16_t)s;
            }
            pos += step;
            while (pos >= 1.0 && avail >= 2) { pos -= 1.0; rd++; avail--; }
        }
        out[i * 2] = last[0]; out[i * 2 + 1] = last[1];
    }
    g_out_rd.store(rd, std::memory_order_release);
    return true;
}
