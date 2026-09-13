// v2_mt32.cpp — see v2_mt32.h.
#include "v2_mt32.h"
#include "v2_ui.h"
#include "v2_sc55.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <cmath>
#include <cstdarg>
#include <dirent.h>
#include <sys/stat.h>

#include "Synth.h"
#include "ROMInfo.h"
#include "FileStream.h"
#include "MidiStreamParser.h"
#include "SampleRateConverter.h"

// the third interpreter instance (v2_ail_interp.cpp)
extern "C" {
void     v2_ail_mt32i_load(const uint8_t* blob, uint32_t bs, const uint8_t* bank, uint32_t ks);
void     v2_ail_mt32i_map(uint16_t para, uint8_t* ptr, uint32_t size);
void     v2_ail_mt32i_map_front(uint16_t para, uint8_t* ptr, uint32_t size);
void     v2_ail_mt32i_set_io(void (*out_fn)(uint16_t, uint8_t), uint8_t (*in_fn)(uint16_t));
void     v2_ail_mt32i_set_callback(uint16_t (*cb)());
uint16_t v2_ail_mt32i_call(uint16_t fn_off, const uint16_t* args, int argc);
uint16_t v2_ail_mt32i_last_dx(void);
uint16_t v2_ail_mt32i_peek(uint16_t off);
uint16_t v2_ail_mt32i_fn_lookup(uint16_t fn_code);
uint16_t v2_ail_pit_cb_value(void);                 // v2_ail.cpp: what the AIL timer callback returns (the PIT divisor snapshot)
uint32_t v2_chunk_read_private(uint16_t cid, uint8_t* dest, uint32_t max_size);   // v2_vm.cpp: a DATA.DAT chunk without the loader's DS effects
}

static bool munt_mode() { return v2_options.sound_mode.load() == 3; }   // 3 = Munt renders; 2 = the stream goes to the SC-55

namespace {

// ---------------------------------------------------------------------------
// the call ring (game thread -> audio thread), the init transcript, the tracks
// ---------------------------------------------------------------------------
enum { K_CALL = 1, K_TRACK = 2, K_STOPALL = 3 };
struct Ent { uint8_t kind; uint8_t argc; uint16_t code; uint16_t ret; uint16_t args[10]; uint16_t slot; };
const size_t RING = 4096;
Ent g_ring[RING];
std::atomic<size_t> g_wr{0}, g_rd{0};
bool ring_push(const Ent& e) {
    const size_t wr = g_wr.load(std::memory_order_relaxed), nx = (wr + 1) & (RING - 1);
    if (nx == g_rd.load(std::memory_order_acquire)) return false;
    g_ring[wr] = e;
    g_wr.store(nx, std::memory_order_release);
    return true;
}
// The music memo (game thread only): the registration of the sequence playing
// now — its track chunk, the fn97 that registered it and the fnAA that started
// it — kept whether or not the option is on. A world that starts later (the
// option switched on in the title or in a level) is built from the boot
// transcript alone; the music's calls went by before it listened, so once the
// module took over the mix nothing played until the next track. At arming
// (v2_mt32_service) the track is staged again and the two calls re-queued
// ahead of everything else: the module plays the sequence from its top.
struct MusicMemo { bool have = false, live = false; uint16_t rel = 0xFFFF; Ent reg{}, start{}; };
MusicMemo g_memo;
std::atomic<bool> g_armed{false};       // the option is on: the producers queue
Ent  g_init[24]; int g_init_n = 0; bool g_init_done = false;   // the boot chain (fn64 .. fn9A), captured always
uint64_t g_drops = 0;

const int TRACK_SLOTS = 4;
uint8_t  g_trk_stage[TRACK_SLOTS][0x10000]; uint32_t g_trk_stage_len[TRACK_SLOTS]; int g_trk_next = 0;

// published at the audible driver's boot
struct Pub { const uint8_t* ds; uint16_t ds_para; uint8_t* arena; uint16_t arena_para; uint32_t arena_size; uint16_t bank_para, sfx_para, track_para; };
Pub g_pub; std::atomic<bool> g_published{false};

// the MT-32 data lives at paragraphs of its own (the FM world's slots are sized for the FM
// chunks: its bank slot is 6 KB, the MT-32 bank 16 KB); the game's pointers to the bank, the
// SFX set and the track buffer are rewritten to these in the calls (fn97 args[2], fn9C args[4])
const uint16_t P_BANK = 0xB000, P_SFX = 0xC000, P_TRACK = 0xD000;
// the MT-32 world's data (read once, game thread)
uint8_t  g_blob[0x10000];  uint32_t g_blob_len = 0;    // 0x1CF MT32MPU.ADV
uint8_t  g_bank[0x10000];  uint32_t g_bank_len = 0;    // 0x20F
uint8_t  g_sfx[0x10000];   uint32_t g_sfx_len = 0;     // 0x20A
uint8_t  g_pre[0x1000];    uint32_t g_pre_len = 0;     // 0x215
bool g_data_ok = false, g_data_tried = false;

// ---------------------------------------------------------------------------
// audio-thread state: the instance
// ---------------------------------------------------------------------------
bool g_built = false;
std::atomic<int> g_toast{0};     // audio thread -> game thread: 1 = Munt is up, 2 = no ROMs (the OPL stays audible)
uint8_t  g_ds[0x10000];          // private copy of the game DS (the state blocks fn97 writes)
uint8_t  g_track[0x10000];       // the track buffer: 0x215 for the preload, then every track's MT-32 variant
uint8_t  g_cache[0x10000];       // the timbre cache, sized by the MT-32 driver's own fn99
uint16_t g_drv = 0;              // the driver id the game uses in every call (args[0])
uint16_t g_desc_off = 0;         // the MT-32 driver's descriptor (fn64) inside its blob
uint16_t g_cache_size = 0;       // its fn99 answer
double   g_tick_hz = 0.0;        // [desc+0x14] + 5 (the fn66 stub's sum, sub_1c61b)
uint64_t g_ticks = 0, g_tick_base = 0;
bool     g_preloaded = false;
int16_t  g_slot0 = -1;           // the world's [990C]: the music slot's handle — the preload's after init, the music's after its fn97
int32_t  g_fm_slot0 = 0x10000;   // the FM world's [990C] as last seen (sub_17912's hook, fn97 of slot 0); 0x10000 = unknown. The game's
                                 // slot-0 calls carry THIS value (a stale 0 from the DS image before any music!) — they mean g_slot0 in the world
int16_t  g_hmap[256];            // FM-world handle -> instance handle (-1 = unknown)
uint16_t g_off[0x100];           // fn code -> handler offset in the MT-32 blob
uint64_t g_calls = 0, g_skipped = 0;
int g_trace = -1;
bool trace_on() { if (g_trace < 0) { const char* e = getenv("V2_MT32_TRACE"); g_trace = (e && *e) ? 1 : 0; } return g_trace == 1; }
char g_status[160] = "MT-32: off";

uint16_t rdw(const uint8_t* m, uint16_t off) { return (uint16_t)(m[off] | (m[(uint16_t)(off + 1)] << 8)); }

// ---------------------------------------------------------------------------
// the MPU-401 (UART mode) the driver talks to — from the driver's own code:
//   fn65 (0x45A) stores the data port (cs:43F = base) and the status/command
//   port (cs:441 = base+1) from its first argument; 0x4C9 sends a command:
//   waits for DRR (status bit 6) clear, OUTs the command to base+1, then polls
//   status until DSR (bit 7) is clear and reads the data port expecting the
//   ACK 0xFE; 0x499 sends a data byte: DRR clear -> OUT to base, draining any
//   input first. The model: one command port (the first port the driver
//   polls), ACK on every command, DRR always ready, data-port writes = MIDI.
// ---------------------------------------------------------------------------
uint8_t  g_mpu_in[16]; int g_mpu_in_n = 0;
// the driver keeps its two ports in its own cells (fn65, 0x443-0x459): cs:43F = data, cs:441 = status/command
uint16_t mpu_data_port()   { return v2_ail_mt32i_peek(0x43F); }
uint16_t mpu_status_port() { return v2_ail_mt32i_peek(0x441); }
bool     g_munt_mode = true;     // build-time: 3 = Munt renders, 2 = the bytes go to the SC-55 (v2_sc55_mix renders)
uint8_t  g_bda[256];             // BIOS data area at 0040:0000 — the driver reads the CRT base at 0040:0063 and paces on its status port bit 3 (0x79E)
uint8_t  g_odd_toggle = 0;
struct UnknownPort { uint16_t port; int reads, writes; };
UnknownPort g_unk[8]; int g_unk_n = 0;
void note_unknown_port(uint16_t port, bool write) {
    for (int i = 0; i < g_unk_n; i++) if (g_unk[i].port == port) { (write ? g_unk[i].writes : g_unk[i].reads)++; return; }
    if (g_unk_n < 8) { g_unk[g_unk_n++] = { port, write ? 0 : 1, write ? 1 : 0 }; fprintf(stderr, "V2-MT32: driver touches port %04X (%s) — not the MPU pair\n", port, write ? "out" : "in"); }
}

// the MIDI stream out of the driver -> Munt + the dump
struct Dump { std::vector<uint8_t> trk; uint64_t last = 0; size_t events = 0; bool on = false; const char* path = nullptr; };
Dump g_dump;
void put_vlq(std::vector<uint8_t>& o, uint32_t v) { uint8_t b[5]; int n = 0; do { b[n++] = (uint8_t)(v & 0x7F); v >>= 7; } while (v); while (n--) o.push_back((uint8_t)(b[n] | (n ? 0x80 : 0))); }
void dump_event(const uint8_t* bytes, size_t n) {
    if (!g_dump.on) return;
    put_vlq(g_dump.trk, (uint32_t)(g_ticks - g_dump.last)); g_dump.last = g_ticks;
    if (bytes[0] == 0xF0) { g_dump.trk.push_back(0xF0); put_vlq(g_dump.trk, (uint32_t)(n - 1)); g_dump.trk.insert(g_dump.trk.end(), bytes + 1, bytes + n); }
    else g_dump.trk.insert(g_dump.trk.end(), bytes, bytes + n);
    g_dump.events++;
}

// what the module reports (V2_MT32_TRACE): the LCD, queue overflows, ROM errors
class Reporter : public MT32Emu::ReportHandler {
    void printDebug(const char* fmt, va_list list) override { if (trace_on()) { fprintf(stderr, "V2-MT32 munt: "); vfprintf(stderr, fmt, list); fprintf(stderr, "\n"); } }
    void showLCDMessage(const char* m) override { fprintf(stderr, "V2-MT32 LCD: %s\n", m); }
    bool onMIDIQueueOverflow() override { static uint64_t n = 0; n++; if ((n & (n - 1)) == 0) fprintf(stderr, "V2-MT32 munt: MIDI queue overflow (%llu)\n", (unsigned long long)n); return false; }
    void onErrorControlROM() override { fprintf(stderr, "V2-MT32 munt: control ROM error\n"); }
    void onErrorPCMROM() override { fprintf(stderr, "V2-MT32 munt: PCM ROM error\n"); }
    void onProgramChanged(MT32Emu::Bit8u part, const char* group, const char* patch) override { if (trace_on()) fprintf(stderr, "V2-MT32 munt: part %u -> %s %s\n", part, group, patch); }
};
Reporter g_reporter;
uint64_t g_play_fail = 0, g_notes_sent = 0;
MT32Emu::Synth* g_synth = nullptr;
MT32Emu::SampleRateConverter* g_src = nullptr;
const MT32Emu::ROMImage* g_rom_ctrl = nullptr; const MT32Emu::ROMImage* g_rom_pcm = nullptr;
MT32Emu::FileStream* g_rom_files[4] = {nullptr, nullptr, nullptr, nullptr}; int g_rom_files_n = 0;
bool g_synth_open = false;
uint32_t g_offset_frames = 0;    // frames into the current callback (Munt timestamps)
uint64_t g_bytes_out = 0, g_sysex_out = 0, g_short_out = 0;

class Parser : public MT32Emu::MidiStreamParser {
    void handleShortMessage(const MT32Emu::Bit32u m) override {
        uint8_t b[3] = { (uint8_t)m, (uint8_t)(m >> 8), (uint8_t)(m >> 16) };
        const uint8_t fam = b[0] & 0xF0; size_t n = (fam == 0xC0 || fam == 0xD0) ? 2 : 3;
        if (fam < 0x80) return;
        g_short_out++;
        dump_event(b, n);
        if (g_synth_open) { if (fam == 0x90 && b[2]) g_notes_sent++; if (!g_synth->playMsg(m, munt_timestamp())) g_play_fail++; }
    }
    void handleSysex(const MT32Emu::Bit8u s[], const MT32Emu::Bit32u len) override {
        g_sysex_out++;
        dump_event(s, len);
        if (g_synth_open && !g_synth->playSysex(s, len, munt_timestamp())) g_play_fail++;
    }
    void handleSystemRealtimeMessage(const MT32Emu::Bit8u) override {}
    void printDebug(const char* msg) override { if (trace_on()) fprintf(stderr, "V2-MT32 parser: %s\n", msg); }
    static MT32Emu::Bit32u munt_timestamp() {
        double ts = g_src ? g_src->convertOutputToSynthTimestamp((double)g_offset_frames) : 0.0;
        return (MT32Emu::Bit32u)(g_synth->getInternalRenderedSampleCount() + (MT32Emu::Bit32u)ts);
    }
};
Parser g_parser;

void mpu_out(uint16_t port, uint8_t val) {
    const uint16_t st = mpu_status_port(), da = mpu_data_port();
    if (st && port == st) {                                          // command: reset (FF), UART (3F), ... -> ACK
        if (g_mpu_in_n < 16) g_mpu_in[g_mpu_in_n++] = 0xFE;
        if (trace_on()) fprintf(stderr, "V2-MT32 mpu: command %02X\n", val);
        return;
    }
    if (da && port == da) {                                          // data: a MIDI byte
        g_bytes_out++;
        g_parser.parseStream(&val, 1);                                 // Munt (mode 3), the dump, the trace
        if (!g_munt_mode) v2_sc55_uart_bytes(&val, 1);                  // mode 2: the SC-55's UART, byte for byte
        return;
    }
    if (port == 0x20 || port == 0x21 || port == 0xA0 || port == 0xA1) return;   // the PICs (IRQ mask / EOI): nothing to model
    note_unknown_port(port, true);
}
uint8_t mpu_in(uint16_t port) {
    const uint16_t st = mpu_status_port(), da = mpu_data_port();
    if (st && port == st) return g_mpu_in_n ? 0x00 : 0x80;          // bit7 DSR: 0 = input waiting; bit6 DRR: 0 = ready
    if (da && port == da) {
        if (!g_mpu_in_n) return 0xFF;
        uint8_t b = g_mpu_in[0]; memmove(g_mpu_in, g_mpu_in + 1, --g_mpu_in_n); return b;
    }
    if (port == 0x21 || port == 0xA1) return 0xFF;                    // PIC masks: everything masked
    if (port == 0x3DA || port == 0x3BA) { g_odd_toggle ^= 0x08; return g_odd_toggle; }   // CRT status: the retrace bit toggles, the driver's delay loop (0x79E) ends at once
    note_unknown_port(port, false);
    g_odd_toggle ^= 0x08;
    return g_odd_toggle;
}
uint16_t pit_cb() { return v2_ail_pit_cb_value(); }

// ---------------------------------------------------------------------------
// Munt
// ---------------------------------------------------------------------------
void munt_close() {
    if (g_src) { delete g_src; g_src = nullptr; }
    if (g_synth) { if (g_synth_open) g_synth->close(); delete g_synth; g_synth = nullptr; }
    g_synth_open = false;
    if (g_rom_ctrl) { MT32Emu::ROMImage::freeROMImage(g_rom_ctrl); g_rom_ctrl = nullptr; }
    if (g_rom_pcm)  { MT32Emu::ROMImage::freeROMImage(g_rom_pcm);  g_rom_pcm = nullptr; }
    for (int i = 0; i < g_rom_files_n; i++) { delete g_rom_files[i]; g_rom_files[i] = nullptr; }
    g_rom_files_n = 0;
}
bool munt_open(uint32_t rate) {
    const char* dir = v2_options_mt32_roms[0] ? v2_options_mt32_roms : "roms/mt32";
    DIR* d = opendir(dir);
    if (!d) { snprintf(g_status, sizeof g_status, "MT-32: no ROM dir %s (MT32_CONTROL.ROM + MT32_PCM.ROM)", dir); return false; }
    std::vector<std::string> names; struct dirent* e;
    while ((e = readdir(d)) != nullptr) { if (e->d_name[0] != '.') names.push_back(std::string(dir) + "/" + e->d_name); }
    closedir(d);
    // every file the library recognises: the first control ROM and the first PCM ROM win (MT-32 before CM-32L)
    std::string ctrl_name, pcm_name; int ctrl_rank = 9, pcm_rank = 9;
    for (const std::string& n : names) {
        struct stat st; if (stat(n.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        MT32Emu::FileStream* fs = new MT32Emu::FileStream();
        if (!fs->open(n.c_str())) { delete fs; continue; }
        const MT32Emu::ROMImage* img = MT32Emu::ROMImage::makeROMImage(fs);
        const MT32Emu::ROMInfo* info = img ? img->getROMInfo() : nullptr;
        if (!info || info->pairType != MT32Emu::ROMInfo::Full) { if (img) MT32Emu::ROMImage::freeROMImage(img); delete fs; continue; }
        const int rank = (strstr(info->shortName, "cm32l") || strstr(info->shortName, "CM32L")) ? 1 : 0;
        if (info->type == MT32Emu::ROMInfo::Control && rank < ctrl_rank) {
            if (g_rom_ctrl) MT32Emu::ROMImage::freeROMImage(g_rom_ctrl);
            g_rom_ctrl = img; ctrl_rank = rank; ctrl_name = info->description; if (g_rom_files_n < 4) g_rom_files[g_rom_files_n++] = fs; else delete fs;
        } else if (info->type == MT32Emu::ROMInfo::PCM && rank < pcm_rank) {
            if (g_rom_pcm) MT32Emu::ROMImage::freeROMImage(g_rom_pcm);
            g_rom_pcm = img; pcm_rank = rank; pcm_name = info->description; if (g_rom_files_n < 4) g_rom_files[g_rom_files_n++] = fs; else delete fs;
        } else { MT32Emu::ROMImage::freeROMImage(img); delete fs; }
    }
    if (!g_rom_ctrl || !g_rom_pcm) {
        snprintf(g_status, sizeof g_status, "MT-32: %s missing in %s", !g_rom_ctrl ? "control ROM" : "PCM ROM", dir);
        munt_close(); return false;
    }
    g_synth = new MT32Emu::Synth(&g_reporter);
    if (!g_synth->open(*g_rom_ctrl, *g_rom_pcm)) { snprintf(g_status, sizeof g_status, "MT-32: synth open failed"); munt_close(); return false; }
    g_synth_open = true;
    g_src = new MT32Emu::SampleRateConverter(*g_synth, (double)rate, MT32Emu::SamplerateConversionQuality_GOOD);
    fprintf(stderr, "V2-MT32: Munt up — %s / %s, %u -> %u Hz\n", ctrl_name.c_str(), pcm_name.c_str(), g_synth->getStereoOutputSampleRate(), rate);
    snprintf(g_status, sizeof g_status, "MT-32: %s", ctrl_name.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// the instance
// ---------------------------------------------------------------------------
uint16_t icall(uint16_t code, const uint16_t* args, int argc) {
    uint16_t off = g_off[code & 0xFF];
    if (!off) { fprintf(stderr, "V2-MT32: fn %02X absent in the MT-32 driver\n", code); return 0; }
    g_calls++;
    return v2_ail_mt32i_call(off, args, argc);
}

// sub_17512 over the MT-32 bank: {patch, bank} -> entry offset word (6-byte index entries from 0, bound 0x3F82)
bool scan_bank(uint8_t bank, uint8_t patch, uint16_t* off) {
    for (int32_t di = 0; di <= 0x3F82; di += 6) {
        if ((uint32_t)di + 4 > g_bank_len) break;
        if (g_bank[di + 1] == bank && g_bank[di] == patch) { *off = rdw(g_bank, (uint16_t)(di + 2)); return true; }
    }
    return false;
}

// the game's MT-32 init tail (seg000:764A-7672): chunk 0x215 into the track
// buffer, sub_176bd with [A378]=0 — fn97, then every timbre request served
// from the bank (sub_17512 + fn9C), then fnAA start (sub_1C799)
void preload() {
    memcpy(g_track, g_pre, g_pre_len);
    const uint16_t state_off = rdw(g_ds, 0x9920);          // [si-0x66E0], si = 0: the music state block
    uint16_t a[8] = { g_drv, 0, P_TRACK, 0, state_off, g_pub.ds_para, 0, 0 };
    const uint16_t h = icall(0x97, a, 8);
    int installed = 0, missing = 0;
    for (int guard = 0; guard < 4096; guard++) {
        uint16_t q[2] = { g_drv, h };
        const uint16_t req = icall(0x9B, q, 2);
        if (req == 0xFFFF) break;
        const uint8_t bank = (uint8_t)(req >> 8), patch = (uint8_t)(req & 0xFF);
        uint16_t off = 0;
        if (!scan_bank(bank, patch, &off)) { missing++; fprintf(stderr, "V2-MT32: preload: timbre bank=%02X patch=%02X not in chunk 0x20F (orig fatal 0x2CE7)\n", bank, patch); break; }
        uint16_t c[6] = { g_drv, bank, patch, off, P_BANK, 0 };
        icall(0x9C, c, 6);
        installed++;
    }
    uint16_t s2[2] = { g_drv, h };
    icall(0xAA, s2, 2);
    g_slot0 = (h == 0xFFFF) ? -1 : (int16_t)h;   // the game's [990C] now holds this handle (sub_176bd eip 0x76DA)
    // The sequence itself (a channel lock, program changes 0..63 over 8.4 s, unlock) ran in the
    // background in DOS while the 6 s of timbre SysEx crawled down the MIDI wire and the logos
    // followed — long done by the first music. Here the wire is instant and the loads are too, so
    // it is driven to its end now: fn67 until the state block leaves "playing" (the driver's time
    // is nothing but these ticks; no other sequence exists yet). Same driver state as after the
    // 8.4 s; the module receives the 64 program changes in one go (no notes among them).
    int ff = 0;
    { uint16_t t[1] = { 0 }; while (ff < 1400 && rdw(g_ds, (uint16_t)(state_off + 0x1A)) == 1) { v2_ail_mt32i_call(g_off[0x67], t, 1); ff++; } }
    g_preloaded = true;
    fprintf(stderr, "V2-MT32: 0x215 preload — handle %04X, %d timbres installed%s, sequence run through in %d ticks (state %u) (%llu MIDI bytes, %llu sysex so far)\n",
            h, installed, missing ? ", MISSING" : "", ff, rdw(g_ds, (uint16_t)(state_off + 0x1A)), (unsigned long long)g_bytes_out, (unsigned long long)g_sysex_out);
}

bool is_handle_fn(uint16_t code) {
    switch (code) {
    case 0x98: case 0x9B: case 0xAA: case 0xAB: case 0xAD: case 0xAE: case 0xAF: case 0xB0: case 0xB1: case 0xB2:
    case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7: case 0xB9: case 0xC0: case 0xC2: return true;
    default: return false;
    }
}

// V2_MT32_TRACE: the driver's per-channel tables — cs:40F slot (FF = no patch: note-ons dropped, 0x8FE),
// cs:41F bank (CC 114), cs:42F patch — after every sequence-level call
void trace_channels(const char* why) {
    if (!trace_on()) return;
    char line[256]; int n = snprintf(line, sizeof line, "V2-MT32: %s slot/bank/patch:", why);
    for (int ch = 1; ch <= 9; ch++) n += snprintf(line + n, sizeof line - n, " %02X/%02X/%02X", v2_ail_mt32i_peek((uint16_t)(0x40F + ch)) & 0xFF, v2_ail_mt32i_peek((uint16_t)(0x41F + ch)) & 0xFF, v2_ail_mt32i_peek((uint16_t)(0x42F + ch)) & 0xFF);
    fprintf(stderr, "%s (tick %llu)\n", line, (unsigned long long)g_ticks);
}

uint64_t g_ring_fn67 = 0, g_ring_calls = 0;
void run_call(const Ent& e) {
    uint16_t a[10]; memcpy(a, e.args, sizeof a); int argc = e.argc;
    g_ring_calls++; if (e.code == 0x67) g_ring_fn67++;
    switch (e.code) {
    case 0x64: {   // init: the MT-32 driver's own descriptor
        g_drv = a[0];
        const uint16_t ax = icall(0x64, a, argc); const uint16_t dx = v2_ail_mt32i_last_dx();
        g_desc_off = ax;
        const uint16_t hz = v2_ail_mt32i_peek((uint16_t)(g_desc_off + 0x14));
        if (hz && hz != 0xFFFF) g_tick_hz = (double)(hz + 5);
        if (trace_on()) fprintf(stderr, "V2-MT32: fn64 -> desc %04X:%04X, tick %.0f Hz\n", dx, ax, g_tick_hz);
        return;
    }
    case 0x66: {   // timer install: LES bx,[98E8] of the game reads ITS driver's descriptor — here the MT-32 one
        uint16_t b[5] = { a[0], v2_ail_mt32i_peek((uint16_t)(g_desc_off + 0x0C)), v2_ail_mt32i_peek((uint16_t)(g_desc_off + 0x0E)),
                          v2_ail_mt32i_peek((uint16_t)(g_desc_off + 0x10)), v2_ail_mt32i_peek((uint16_t)(g_desc_off + 0x12)) };
        icall(0x66, b, 5);
        return;
    }
    case 0x99: {   // the cache size the game would have allocated for THIS driver
        g_cache_size = icall(0x99, a, argc);
        if (trace_on()) fprintf(stderr, "V2-MT32: fn99 cache size %04X (FM world: %04X)\n", g_cache_size, e.ret);
        // the game's init tail follows at once: fn9A only when the size is not 0 (sub_17561 eip 0x7604),
        // then the card-8 branch (eip 0x7636) — the 0x215 preload. The MT-32 driver answers 0, so its
        // world has no fn9A; the preload comes right here (in the FM stream fn9A follows fn99).
        if (g_cache_size == 0 && !g_preloaded) preload();
        return;
    }
    case 0x9A: {   // set cache: the game's segment, the MT-32 driver's size, our private buffer behind it
        if (g_cache_size != 0) {
            v2_ail_mt32i_map_front(a[2], g_cache, sizeof g_cache);
            uint16_t b[4] = { a[0], a[1], a[2], g_cache_size };
            icall(0x9A, b, 4);
        }
        if (!g_preloaded) preload();                        // the init tail's preload, after the cache
        return;
    }
    case 0x9C:     // the FM world's timbre servicing — the MT-32 world never services after its preload ([A378]=1)
        g_skipped++;
        return;
    case 0x97: {   // register: the same call (the preload ran in the init tail; here only if the boot chain was never seen)
        if (!g_preloaded) preload();
        if (a[2] == g_pub.sfx_para) a[2] = P_SFX; else if (a[2] == g_pub.track_para) a[2] = P_TRACK;   // the XMIDI data: the MT-32 set
        const uint16_t mt = icall(0x97, a, argc);
        if (e.ret < 256) g_hmap[e.ret] = (mt == 0xFFFF) ? -1 : (int16_t)mt;
        if (a[4] == rdw(g_ds, 0x9920)) { g_slot0 = (mt == 0xFFFF) ? -1 : (int16_t)mt; g_fm_slot0 = e.ret; }   // the music slot (si = 0): the game overwrites [990C] in both worlds
        if (trace_on() || mt == 0xFFFF) fprintf(stderr, "V2-MT32: fn97 seq=%u xmid=%04X -> handle %04X (FM %04X)\n", a[3], a[2], mt, e.ret);
        return;
    }
    default:
        if (is_handle_fn(e.code) && argc >= 2) {
            const uint16_t fm = a[1];
            bool via_slot0 = false;
            if (fm != 0xFFFF) {
                if (g_fm_slot0 <= 0xFFFF && fm == (uint16_t)g_fm_slot0) {   // the game read [990C]: the world's own slot-0 handle
                    if (g_slot0 < 0) { g_skipped++; return; }
                    a[1] = (uint16_t)g_slot0; via_slot0 = true;
                } else {
                    if (fm >= 256 || g_hmap[fm] < 0) { g_skipped++; return; }   // registered before this world started
                    a[1] = (uint16_t)g_hmap[fm];
                }
            }
            const uint16_t r = icall(e.code, a, argc);
            if (e.code == 0x98) {
                if (via_slot0) { if (trace_on()) fprintf(stderr, "V2-MT32: slot 0 released (world handle %04X, FM %04X)\n", g_slot0, fm); g_slot0 = -1; g_fm_slot0 = 0xFFFF; }
                else if (fm < 256) { if (g_hmap[fm] == g_slot0) g_slot0 = -1; g_hmap[fm] = -1; }
            }
            if (e.code == 0xAA || e.code == 0xAB || e.code == 0x98) { char why[48]; snprintf(why, sizeof why, "after fn%02X h=%04X", e.code, fm); trace_channels(why); }
            if (e.code == 0x9B && trace_on() && r != e.ret) fprintf(stderr, "V2-MT32: fn9B handle %04X -> %04X (FM world %04X): a timbre request the MT-32 world leaves pending (bank %02X patch %02X)\n", fm, r, e.ret, r >> 8, r & 0xFF);
            return;
        }
        icall(e.code, a, argc);
        return;
    }
}

void build(uint32_t rate) {
    for (int i = 0; i < 256; i++) g_hmap[i] = -1;
    memcpy(g_ds, g_pub.ds, 0x10000);
    v2_ail_mt32i_load(g_blob, g_blob_len, g_bank, g_bank_len);
    for (int c = 0; c < 0x100; c++) g_off[c] = v2_ail_mt32i_fn_lookup((uint16_t)c);
    v2_ail_mt32i_set_io(mpu_out, mpu_in);
    v2_ail_mt32i_set_callback(pit_cb);
    v2_ail_mt32i_map(g_pub.arena_para, g_pub.arena, g_pub.arena_size);           // the game's memory (reads), behind the overrides
    memset(g_bda, 0, sizeof g_bda); g_bda[0x63] = 0xD4; g_bda[0x64] = 0x03;         // BIOS data: a colour CRT at 0x3D4 (the driver's retrace delay)
    v2_ail_mt32i_map_front(0x0040, g_bda, sizeof g_bda);
    v2_ail_mt32i_map_front(P_BANK, g_bank, sizeof g_bank);
    v2_ail_mt32i_map_front(P_SFX, g_sfx, sizeof g_sfx);
    v2_ail_mt32i_map_front(P_TRACK, g_track, sizeof g_track);
    v2_ail_mt32i_map_front(g_pub.ds_para, g_ds, sizeof g_ds);
    g_mpu_in_n = 0; g_unk_n = 0;
    g_ticks = 0; g_tick_base = 0; g_preloaded = false; g_slot0 = -1; g_fm_slot0 = 0x10000; g_calls = 0; g_skipped = 0; g_desc_off = 0; g_cache_size = 0; g_tick_hz = 0.0;
    g_bytes_out = 0; g_sysex_out = 0; g_short_out = 0;
    if (!g_dump.on) { const char* p = getenv("V2_MT32_DUMP"); if (p && *p) { g_dump.on = true; g_dump.path = p; } }
    g_munt_mode = munt_mode();
    if (g_munt_mode && !g_synth_open) g_toast.store(munt_open(rate) ? 1 : 2, std::memory_order_release);   // without ROMs the driver still runs (the dump); the mix falls through to the OPL. Already open: v2_mt32_prepare did it before the game booted
    else { snprintf(g_status, sizeof g_status, "MT-32 world -> SC-55"); g_toast.store(0, std::memory_order_release); }
    // the boot chain: in the ring when the option was on at the game's start, otherwise the transcript
    size_t rd = g_rd.load(std::memory_order_relaxed);
    const bool ring_has_init = (rd != g_wr.load(std::memory_order_acquire)) && g_ring[rd].kind == K_CALL && g_ring[rd].code == 0x64;
    if (!ring_has_init) {
        if (!g_init_done) { fprintf(stderr, "V2-MT32: the driver boot chain was not seen yet — waiting\n"); return; }
        for (int i = 0; i < g_init_n; i++) run_call(g_init[i]);
    }
    g_built = true;
    fprintf(stderr, "V2-MT32: instance up (blob %u, bank %u, sfx %u, preload %u bytes; bank@%04X sfx@%04X track@%04X ds@%04X; init %s)\n",
            g_blob_len, g_bank_len, g_sfx_len, g_pre_len, g_pub.bank_para, g_pub.sfx_para, g_pub.track_para, g_pub.ds_para,
            ring_has_init ? "from the ring" : "replayed");
}

void teardown() {
    munt_close();
    g_built = false;
    g_rd.store(g_wr.load(std::memory_order_acquire), std::memory_order_release);   // drop what was queued
    snprintf(g_status, sizeof g_status, "MT-32: off");
    fprintf(stderr, "V2-MT32: instance down (%llu calls, %llu skipped, %llu MIDI bytes, %llu sysex)\n",
            (unsigned long long)g_calls, (unsigned long long)g_skipped, (unsigned long long)g_bytes_out, (unsigned long long)g_sysex_out);
}

bool load_data() {
    if (g_data_tried) return g_data_ok;
    g_data_tried = true;
    g_blob_len = v2_chunk_read_private(0x1CF, g_blob, sizeof g_blob);
    g_bank_len = v2_chunk_read_private(0x20F, g_bank, sizeof g_bank);
    g_sfx_len  = v2_chunk_read_private(0x20A, g_sfx, sizeof g_sfx);
    g_pre_len  = v2_chunk_read_private(0x215, g_pre, sizeof g_pre);
    g_data_ok = g_blob_len && g_bank_len && g_sfx_len && g_pre_len;
    if (!g_data_ok) { snprintf(g_status, sizeof g_status, "MT-32: chunks unavailable (1CF:%u 20F:%u 20A:%u 215:%u)", g_blob_len, g_bank_len, g_sfx_len, g_pre_len); fprintf(stderr, "V2-MT32: %s\n", g_status); }
    else if (trace_on()) fprintf(stderr, "V2-MT32: data 1CF:%u 20F:%u 20A:%u 215:%u\n", g_blob_len, g_bank_len, g_sfx_len, g_pre_len);
    return g_data_ok;
}

} // namespace

// the world runs for MT32 (Munt) and for SC55 (the same MPU stream into the SC-55 — the 1992 SC-55 owner's setup)
bool v2_mt32_enabled() { const int m = v2_options.sound_mode.load(); return m == 3 || m == 2; }
const char* v2_mt32_status() { return g_status; }

// ---------------------------------------------------------------------------
// producers (game thread)
// ---------------------------------------------------------------------------
void v2_mt32_note_call(uint16_t fn_code, const uint16_t* args, int argc, uint16_t ret) {
    if (fn_code == 0x67) return;             // the audible driver's timer ticks: the MT-32 world ticks on its own sample clock (v2_mt32_pump)
    Ent e; e.kind = K_CALL; e.code = fn_code; e.ret = ret; e.argc = (uint8_t)(argc > 10 ? 10 : argc); e.slot = 0;
    memset(e.args, 0, sizeof e.args); for (int i = 0; i < e.argc; i++) e.args[i] = args[i];
    if (!g_init_done) {                      // the boot chain, kept for a world that starts later
        if (g_init_n < 24) g_init[g_init_n++] = e;
        if (fn_code == 0x9A || (fn_code == 0x99 && ret == 0)) g_init_done = true;
    }
    // the music memo: a registration with the track paragraph (sub_176bd), its start, its stop
    if (g_published.load(std::memory_order_acquire)) {
        if (fn_code == 0x97 && argc >= 3 && args[2] == g_pub.track_para) {
            g_memo.have = (ret != 0xFFFF); g_memo.live = false; g_memo.reg = e;
        } else if (g_memo.have && argc >= 2 && args[1] == g_memo.reg.ret) {
            if (fn_code == 0xAA) { g_memo.live = true; g_memo.start = e; }
            else if (fn_code == 0xAB || fn_code == 0x98) g_memo.live = false;
        }
    }
    if (!g_armed.load(std::memory_order_acquire)) return;
    const size_t wr = g_wr.load(std::memory_order_relaxed), nx = (wr + 1) & (RING - 1);
    if (nx == g_rd.load(std::memory_order_acquire)) { g_drops++; if ((g_drops & (g_drops - 1)) == 0) fprintf(stderr, "V2-MT32: call ring FULL — %llu dropped\n", (unsigned long long)g_drops); return; }
    g_ring[wr] = e;
    g_wr.store(nx, std::memory_order_release);
}

void v2_mt32_note_track(uint16_t chunk_rel) {
    g_memo.rel = chunk_rel;                  // the music memo: the track the registration that follows will play
    if (!g_armed.load(std::memory_order_acquire)) return;
    const int slot = g_trk_next; g_trk_next = (g_trk_next + 1) % TRACK_SLOTS;
    const uint16_t cid = (uint16_t)(chunk_rel + 3);       // the music set of card 8
    g_trk_stage_len[slot] = v2_chunk_read_private(cid, g_trk_stage[slot], sizeof g_trk_stage[slot]);
    if (!g_trk_stage_len[slot]) { fprintf(stderr, "V2-MT32: track chunk %04X unavailable\n", cid); return; }
    Ent e; memset(&e, 0, sizeof e); e.kind = K_TRACK; e.slot = (uint16_t)slot; e.code = cid;
    const size_t wr = g_wr.load(std::memory_order_relaxed), nx = (wr + 1) & (RING - 1);
    if (nx == g_rd.load(std::memory_order_acquire)) { fprintf(stderr, "V2-MT32: call ring FULL — track %04X dropped\n", cid); return; }
    g_ring[wr] = e;
    g_wr.store(nx, std::memory_order_release);
}

void v2_mt32_note_stop_all(uint16_t si_start, uint16_t fm_slot0_handle) {
    if (si_start == 0) g_memo.live = false;  // the music memo: sub_17912 from slot 0 stops the music
    if (!g_armed.load(std::memory_order_acquire)) return;
    Ent e; memset(&e, 0, sizeof e); e.kind = K_STOPALL; e.args[0] = si_start; e.args[1] = fm_slot0_handle;
    const size_t wr = g_wr.load(std::memory_order_relaxed), nx = (wr + 1) & (RING - 1);
    if (nx == g_rd.load(std::memory_order_acquire)) return;
    g_ring[wr] = e;
    g_wr.store(nx, std::memory_order_release);
}

void v2_mt32_publish(const uint8_t* game_ds, uint16_t ds_para, uint8_t* arena, uint16_t arena_para, uint32_t arena_size,
                     uint16_t bank_para, uint16_t sfx_para, uint16_t track_para) {
    g_pub = { game_ds, ds_para, arena, arena_para, arena_size, bank_para, sfx_para, track_para };
    g_published.store(true, std::memory_order_release);
    if (v2_mt32_enabled()) load_data();
}

// v2_main, before the game thread, with MT32 chosen at start: the chunks and
// Munt (the ROM images, a fraction of a second) come up now. Otherwise the world
// was built on the audio thread at its first pump after the driver boot — the
// OPL rendering was heard until then, the module joined with the title already
// playing and started the sequence from its top: a switch in the music at the
// "MT-32: ..." toast. Open ahead, the world catches the boot chain within a pump
// of the driver's start and the mix is the module's from the first note; the OPL
// is never heard. Without ROMs the status says so and build() reports it (the OPL stays).
static uint32_t g_mix_rate_hint = 0;   // the device rate play.cpp got from SDL (0 until sound_init; HEADLESS has no audio)
void v2_mt32_set_mix_rate(uint32_t rate) { g_mix_rate_hint = rate; }
void v2_mt32_prepare() {
    v2_options_ensure_loaded();
    if (v2_options.sound_mode.load() != 3) return;
    if (!load_data()) { fprintf(stderr, "V2-MT32: %s\n", g_status); return; }
    g_munt_mode = true;
    if (munt_open(g_mix_rate_hint ? g_mix_rate_hint : 44100)) g_toast.store(1, std::memory_order_release);
    else fprintf(stderr, "V2-MT32: %s\n", g_status);
}

void v2_mt32_service() {
    static int last = -1;
    const int on = v2_mt32_enabled() ? 1 : 0;
    { const int t = g_toast.exchange(0); if (t) v2_ui_toast(g_status); }   // what the audio thread found when it built the world
    if (on == last) return;
    last = on;
    if (on) {
        v2_options_ensure_loaded();
        if (!load_data()) { v2_ui_toast(g_status); v2_options.sound_mode = 0; last = 0; return; }
        snprintf(g_status, sizeof g_status, munt_mode() ? "MT-32: starting" : "MT-32 world -> SC-55: starting");
        g_armed.store(true, std::memory_order_release);
        // the music memo: the sequence playing now was registered and started before the
        // world listened — stage its track and queue the two calls ahead of everything else
        if (g_memo.have && g_memo.live && g_memo.rel != 0xFFFF) {
            // first the stop-all of slot 0 the world never saw (sub_17912 at the level start,
            // FM handle FFFF = nothing registered there in the FM world): the boot transcript
            // the world replays leaves the 0x215 preload registered in slot 0, and a music
            // slot beside a live preload slot steps twice per tick (the shared state block) —
            // the tempo doubling of the first ROM runs again (SOUND_SYSTEM.md, the MT-32 tempo)
            Ent st; memset(&st, 0, sizeof st); st.kind = K_STOPALL; st.args[0] = 0; st.args[1] = 0xFFFF;
            ring_push(st);
            v2_mt32_note_track(g_memo.rel);
            if (ring_push(g_memo.reg) && ring_push(g_memo.start))
                fprintf(stderr, "V2-MT32: resuming the music playing now (track %04X, FM handle %04X) from its top\n",
                        (unsigned)(g_memo.rel + 3), g_memo.reg.ret);
        }
    } else {
        g_armed.store(false, std::memory_order_release);       // the audio thread tears the instance down
    }
}

// ---------------------------------------------------------------------------
// audio thread
// ---------------------------------------------------------------------------
void v2_mt32_pump(uint64_t pos, uint32_t offset_frames, uint32_t rate) {
    const bool armed = g_armed.load(std::memory_order_acquire);
    if (!armed) { if (g_built) teardown(); return; }
    if (g_built && g_munt_mode != munt_mode()) { teardown(); g_armed.store(true, std::memory_order_release); }   // SC55 <-> MT32: a fresh world for the other module
    if (!g_built) {
        if (!g_published.load(std::memory_order_acquire) || !g_data_ok) return;
        build(rate);
        if (!g_built) return;
        g_tick_base = pos;
    }
    g_offset_frames = offset_frames;
    // 1) the calls (budget: a preload runs 64 timbre uploads inside one fn97)
    int budget = 32;
    size_t rd = g_rd.load(std::memory_order_relaxed);
    while (budget-- > 0 && rd != g_wr.load(std::memory_order_acquire)) {
        const Ent& e = g_ring[rd];
        if (e.kind == K_TRACK) {
            memcpy(g_track, g_trk_stage[e.slot], g_trk_stage_len[e.slot]);
            if (trace_on()) fprintf(stderr, "V2-MT32: track chunk %04X (%u bytes) in the buffer\n", e.code, g_trk_stage_len[e.slot]);
        } else if (e.kind == K_STOPALL) {
            // sub_17912 in the MT-32 world: slot 0 holds the preload's handle where the FM world holds
            // none (the FM stream carries no call for it) — stop + release it as the game's loop does
            if (trace_on()) fprintf(stderr, "V2-MT32: sub_17912 stop-all si_start=%u FM slot0=%04X world slot0=%d (tick %llu)\n", e.args[0], e.args[1], g_slot0, (unsigned long long)g_ticks);
            if (e.args[0] == 0) g_fm_slot0 = e.args[1];      // the calls that follow for slot 0 carry this value
            if (e.args[0] == 0 && e.args[1] == 0xFFFF && g_slot0 >= 0) {
                uint16_t q[2] = { g_drv, (uint16_t)g_slot0 };
                icall(0xAB, q, 2); icall(0x98, q, 2);
                if (trace_on()) fprintf(stderr, "V2-MT32: stop-all released the preload handle %04X\n", g_slot0);
                g_slot0 = -1;
            }
        } else run_call(e);
        rd = (rd + 1) & (RING - 1);
    }
    g_rd.store(rd, std::memory_order_release);
    // 2) fn67 ticks on the sample clock — the DOS INT8 of this driver, at ITS rate
    if (g_tick_hz <= 0.0 || !g_off[0x67]) return;
    const double spt = (double)rate / g_tick_hz;
    const uint64_t rel = (pos > g_tick_base) ? pos - g_tick_base : 0;
    uint64_t due = (uint64_t)((double)rel / spt);
    if (due > g_ticks + 64) { const uint64_t excess = due - g_ticks - 64; g_tick_base += (uint64_t)((double)excess * spt); due -= excess; }
    uint16_t a[1] = { 0 }; int guard = 0;
    while (g_ticks < due && guard++ < 96) { v2_ail_mt32i_call(g_off[0x67], a, 1); g_ticks++; if ((g_ticks % 250) == 0) trace_channels("tick"); }
}

bool v2_mt32_mix(int16_t* out, uint32_t frames, uint32_t rate) {
    if (!g_built || !g_synth_open || !g_src) return false;
    g_src->getOutputSamples(out, frames);
    if (trace_on()) {   // one line per second of output: what went to the module and whether it sounds
        static uint64_t acc = 0, sec = 0; acc += frames;
        if (acc >= rate) {
            acc -= rate; sec++;
            double e = 0; for (uint32_t i = 0; i < frames * 2; i++) e += (double)out[i] * out[i];
            const uint16_t st = rdw(g_ds, 0x9920);   // the music state block: tempo inc/target, interval countdown, status
            fprintf(stderr, "V2-MT32 sec %llu: notes %llu, sysex %llu, bytes %llu, play failures %llu, active %d, rms %.0f, tick %llu (ring calls %llu, fn67 via ring %llu); music state +32=%u +34=%u +1E=%u +1A=%u\n",
                    (unsigned long long)sec, (unsigned long long)g_notes_sent, (unsigned long long)g_sysex_out, (unsigned long long)g_bytes_out,
                    (unsigned long long)g_play_fail, g_synth->isActive() ? 1 : 0, sqrt(e / (frames * 2)), (unsigned long long)g_ticks,
                    (unsigned long long)g_ring_calls, (unsigned long long)g_ring_fn67, rdw(g_ds, (uint16_t)(st + 0x32)), rdw(g_ds, (uint16_t)(st + 0x34)), rdw(g_ds, (uint16_t)(st + 0x1E)), rdw(g_ds, (uint16_t)(st + 0x1A)));
        }
    }
    return true;
}

void v2_mt32_shutdown() {
    if (!g_dump.on || !g_dump.path) return;
    g_dump.on = false;
    double hz = g_tick_hz > 0.0 ? g_tick_hz : 125.0;
    const uint16_t division = (uint16_t)(hz + 0.5);
    std::vector<uint8_t> trk; put_vlq(trk, 0); trk.insert(trk.end(), {0xFF, 0x51, 0x03, 0x0F, 0x42, 0x40});
    trk.insert(trk.end(), g_dump.trk.begin(), g_dump.trk.end());
    put_vlq(trk, 0); trk.insert(trk.end(), {0xFF, 0x2F, 0x00});
    FILE* f = fopen(g_dump.path, "wb");
    if (!f) { fprintf(stderr, "V2-MT32: cannot write %s\n", g_dump.path); return; }
    const uint8_t hdr[14] = { 'M','T','h','d', 0,0,0,6, 0,0, 0,1, (uint8_t)(division >> 8), (uint8_t)division };
    fwrite(hdr, 1, 14, f);
    const uint32_t len = (uint32_t)trk.size();
    const uint8_t th[8] = { 'M','T','r','k', (uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len };
    fwrite(th, 1, 8, f); fwrite(trk.data(), 1, trk.size(), f); fclose(f);
    fprintf(stderr, "V2-MT32: dump %zu events (%llu sysex) over %llu ticks at %.1f Hz -> %s\n",
            g_dump.events, (unsigned long long)g_sysex_out, (unsigned long long)g_dump.last, hz, g_dump.path);
}
