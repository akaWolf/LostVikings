// v2_gamestate.cpp — Phase D step 1: byte-exact serializer for V2GameState.
//
// Coverage model: every X-macro field marks its DS bytes in a 64K coverage
// map at startup; overlaps abort (a list bug, not a runtime condition).
// serialize writes covered bytes ONLY from fields and uncovered bytes from
// the raw backing — so a green roundtrip proves both the field encodings and
// that no byte has two owners.

#include "v2_gamestate.h"
#include <sys/syscall.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>   // _exit — a barrier-parked peer thread deadlocks exit()

static uint8_t g_cov[0x10000];      // 0 = raw, 1 = field-owned
static bool    g_cov_built = false;

static void cov_mark(uint32_t off, uint32_t len, const char* name) {
    if (off + len > 0x10000) {
        fprintf(stderr, "V2-GS: field %s [%04X..%04X) exceeds DS\n", name, off, off + len);
        abort();
    }
    for (uint32_t i = 0; i < len; i++) {
        if (g_cov[off + i]) {
            fprintf(stderr, "V2-GS: field %s overlaps at DS %04X\n", name, off + i);
            abort();
        }
        g_cov[off + i] = 1;
    }
}

static void cov_build(void) {
    if (g_cov_built) return;
    memset(g_cov, 0, sizeof(g_cov));
#define V2_GS_C1(name, off)      cov_mark((off), 2, #name);
#define V2_GS_CN(name, off, n)   cov_mark((off), 2u * (n), #name);
#define V2_GS_CR(name, off, n, rec, fld) V2_GS_CN(name, off, n)
    V2_GS_FIELDS_W(V2_GS_C1, V2_GS_CN, V2_GS_CR)
#undef V2_GS_C1
#undef V2_GS_CN
#undef V2_GS_CR
#define V2_GS_CB1(name, off)     cov_mark((off), 1, #name);
#define V2_GS_CBN(name, off, n)  cov_mark((off), (n), #name);
#define V2_GS_CBR(name, off, n, rec, fld) V2_GS_CBN(name, off, n)
    V2_GS_FIELDS_B(V2_GS_CB1, V2_GS_CBN)
    V2_GS_FIELDS_GAPFILL(V2_GS_CB1, V2_GS_CBN)
#undef V2_GS_CB1
#undef V2_GS_CBN
#undef V2_GS_CBR
    g_cov_built = true;
    uint32_t covered = 0;
    for (uint32_t i = 0; i < 0x10000; i++) covered += g_cov[i];
    fprintf(stderr, "V2-GS: coverage %u/65536 bytes typed (%.1f%%)\n",
            covered, covered * 100.0 / 65536.0);
}

static inline uint16_t rd_w(const uint8_t* ds, uint32_t off) {
    return (uint16_t)(ds[off] | (ds[(uint16_t)(off + 1)] << 8));
}
static inline void wr_w(uint8_t* ds, uint32_t off, uint16_t v) {
    ds[off] = (uint8_t)v;
    ds[(uint16_t)(off + 1)] = (uint8_t)(v >> 8);
}

extern "C" void v2_gs_deserialize(V2GameState* gs, const uint8_t* ds) {
    cov_build();
#define V2_GS_D1(name, off)      gs->name = rd_w(ds, (off));
#define V2_GS_DN(name, off, n)   for (uint32_t i = 0; i < (n); i++) gs->name[i] = rd_w(ds, (off) + 2u * i);
#define V2_GS_DR(name, off, n, rec, fld) V2_GS_DN(name, off, n)
    V2_GS_FIELDS_W(V2_GS_D1, V2_GS_DN, V2_GS_DR)
#undef V2_GS_D1
#undef V2_GS_DN
#undef V2_GS_DR
#define V2_GS_DB1(name, off)     gs->name = ds[(off)];
#define V2_GS_DBN(name, off, n)  memcpy(gs->name, ds + (off), (n));
#define V2_GS_DBR(name, off, n, rec, fld) V2_GS_DBN(name, off, n)
    V2_GS_FIELDS_B(V2_GS_DB1, V2_GS_DBN)
    V2_GS_FIELDS_GAPFILL(V2_GS_DB1, V2_GS_DBN)
#undef V2_GS_DB1
#undef V2_GS_DBN
#undef V2_GS_DBR
    memcpy(gs->raw, ds, 0x10000);   // backing; covered bytes unused on serialize
}

extern "C" void v2_gs_serialize(const V2GameState* gs, uint8_t* ds_out) {
    cov_build();
    // raw pass first: uncovered bytes verbatim; covered bytes are then
    // OVERWRITTEN from fields, so the backing never leaks into typed space.
    memcpy(ds_out, gs->raw, 0x10000);
#define V2_GS_S1(name, off)      wr_w(ds_out, (off), gs->name);
#define V2_GS_SN(name, off, n)   for (uint32_t i = 0; i < (n); i++) wr_w(ds_out, (off) + 2u * i, gs->name[i]);
#define V2_GS_SR(name, off, n, rec, fld) V2_GS_SN(name, off, n)
    V2_GS_FIELDS_W(V2_GS_S1, V2_GS_SN, V2_GS_SR)
#undef V2_GS_S1
#undef V2_GS_SN
#undef V2_GS_SR
#define V2_GS_SB1(name, off)     ds_out[(off)] = gs->name;
#define V2_GS_SBN(name, off, n)  memcpy(ds_out + (off), gs->name, (n));
#define V2_GS_SBR(name, off, n, rec, fld) V2_GS_SBN(name, off, n)
    V2_GS_FIELDS_B(V2_GS_SB1, V2_GS_SBN)
    V2_GS_FIELDS_GAPFILL(V2_GS_SB1, V2_GS_SBN)
#undef V2_GS_SB1
#undef V2_GS_SBN
#undef V2_GS_SBR
}

// Named-field text dump — the phase-D state inspector. Every field the
// serializer knows prints by name; big byte zones print as hex rows. Gated
// by V2_GS_DUMP_TEXT=<path> at the same frame_begin hook as the roundtrip.
extern "C" void v2_gs_dump_text(const uint8_t* ds, const char* path) {
    static V2GameState gs;
    v2_gs_deserialize(&gs, ds);
    FILE* f = fopen(path, "w");
    if (!f) return;
#define V2_GS_P1(name, off)  fprintf(f, "%-22s @%04X = %04X\n", #name, (unsigned)(off), gs.name);
#define V2_GS_PN(name, off, n) { \
    fprintf(f, "%-22s @%04X [%u]:", #name, (unsigned)(off), (unsigned)(n)); \
    for (uint32_t i = 0; i < (n); i++) fprintf(f, " %04X", gs.name[i]); \
    fprintf(f, "\n"); }
#define V2_GS_PR(name, off, n, rec, fld) V2_GS_PN(name, off, n)
    V2_GS_FIELDS_W(V2_GS_P1, V2_GS_PN, V2_GS_PR)
#undef V2_GS_P1
#undef V2_GS_PN
#undef V2_GS_PR
#define V2_GS_PB1(name, off) fprintf(f, "%-22s @%04X = %02X\n", #name, (unsigned)(off), gs.name);
#define V2_GS_PBN(name, off, n) { \
    fprintf(f, "%-22s @%04X [%u]:", #name, (unsigned)(off), (unsigned)(n)); \
    for (uint32_t i = 0; i < (n); i++) { \
        if ((i & 31) == 0) fprintf(f, "\n  %04X:", (unsigned)((off) + i)); \
        fprintf(f, " %02X", gs.name[i]); } \
    fprintf(f, "\n"); }
#define V2_GS_PBR(name, off, n, rec, fld) V2_GS_PBN(name, off, n)
    V2_GS_FIELDS_B(V2_GS_PB1, V2_GS_PBN)
    V2_GS_FIELDS_GAPFILL(V2_GS_PB1, V2_GS_PBN)
#undef V2_GS_PB1
#undef V2_GS_PBN
#undef V2_GS_PBR
    fclose(f);
}

// Self-check hardening: the raw-backing serialize pass masks a field whose
// serializer writes the wrong bytes only if it writes the SAME wrong bytes
// the backing already held — impossible for a wrong offset (coverage would
// have flagged the overlap) and for a dropped write (backing holds the live
// value, so identity still proves the byte stream). The one hole would be a
// field DEserialized from the wrong offset; kill it by scrambling covered
// backing bytes before serialize in the check below.
extern "C" int v2_gs_roundtrip_check(const uint8_t* ds, const char* tag) {
    static V2GameState gs;          // 192K+: keep off the stack
    static uint8_t out[0x10000];
    v2_gs_deserialize(&gs, ds);
    for (uint32_t i = 0; i < 0x10000; i++)
        if (g_cov[i]) gs.raw[i] ^= 0xA5;    // covered backing must be dead
    v2_gs_serialize(&gs, out);
    int diffs = 0;
    for (uint32_t i = 0; i < 0x10000; i++) {
        if (out[i] != ds[i]) {
            if (diffs < 16)
                fprintf(stderr, "V2-GS-DIFF[%s]: DS %04X ds=%02X roundtrip=%02X (%s)\n",
                        tag ? tag : "?", i, ds[i], out[i],
                        g_cov[i] ? "typed" : "raw");
            diffs++;
        }
    }
    if (diffs)
        fprintf(stderr, "V2-GS-ROUNDTRIP[%s]: %d byte diffs\n", tag ? tag : "?", diffs);
    return diffs;
}

// ============================================================================
// Golden end-state channel — common to ALL builds (headless verify, V2_ONLY,
// windowed). Historically lived in headless_main.cpp (hence the name, kept
// so every existing call site and doc stays valid); moved here so V2_ONLY
// gencode/soak binaries emit the same catalog snapshots at clean exits.
// Idempotent: first call wins (a quit dump is not overwritten by a later
// max-frames dump).
// ============================================================================
extern uint8_t* v2_vm_get_shadow_ds();       // C++ linkage (v2_vm.cpp)
extern "C" int v2_state_save(const char*);
extern "C" void headless_golden_dump(void) {
    static int done = 0;
    if (done) return;
    const char* gp = getenv("V2_GOLDEN_DUMP");
    uint8_t* shd = v2_vm_get_shadow_ds();
    // stage 4 II.c: the dump reads through the view (members for evacuated
    // fields) — validate the mirror right before snapshotting.
    if (shd) {
        extern long v2_gs_evac_check_calls;
        fprintf(stderr, "V2-GS-EVAC: %ld frame checks before dump\n",
                v2_gs_evac_check_calls);
        v2_gs_evac_check(shd);
    }
    if (gp && shd) { v2_gs_dump_text(shd, gp); done = 1; }
    const char* sp = getenv("V2_SAVE_STATE");
    if (sp && shd) { v2_state_save(sp); done = 1; }
}

// ============================================================================
// Stage 4 II.b: bounds sanitizer sink. Dedup by (base, off) pair; every new
// pair prints immediately (headless exits via _exit, atexit never runs).
// The map of survivors = the wrap classes the carrier swap must preserve.
// ============================================================================
#ifdef V2_GS_BOUNDS
extern "C" void v2_gs_bounds_note(uint32_t base, uint32_t len, uint32_t off) {
    static uint64_t seen[256];
    static int n_seen = 0;
    static uint64_t total = 0;
    total++;
    uint64_t key = ((uint64_t)base << 32) | off;
    for (int i = 0; i < n_seen; i++)
        if (seen[i] == key) return;
    if (n_seen < 256) seen[n_seen++] = key;
    fprintf(stderr, "V2-GS-BOUNDS[#%d]: field@%04X len=%u off=%u (addr=%04X) total=%llu\n",
            n_seen, base, len, off, (uint16_t)(base + off),
            (unsigned long long)total);
}
#endif

// ============================================================================
// Stage 4 II.c: evacuated-field storage + integrity check.
// ============================================================================
V2GsEvac g_gs_evac;

// Mirror one word written to the flat image into the evacuated member (used
// by the OPERAND write path — interpreter bodies/helpers write through
// V2VM::ds_write with computed addresses; this keeps members in sync so the
// per-frame check stays meaningful and the read flip stays possible).
// Generated from the same EVAC list: field spans only, cheap range tests.
// stage-4 wave 10: the per-byte router replaces the field-list chains.
uint8_t* v2_gs_route[0x10000];

static void v2_gs_route_build(uint8_t* ds) {
    for (uint32_t a = 0; a < 0x10000; a++) v2_gs_route[a] = ds + a;
#define V2_GS_RT1(name, off) \
    { uint8_t* m = (uint8_t*)&g_gs_evac.name; \
      v2_gs_route[(off)] = m; v2_gs_route[(uint16_t)((off) + 1)] = m + 1; }
#define V2_GS_RTN(name, off, n) \
    for (uint32_t i = 0; i < (n); i++) { \
        uint8_t* m = (uint8_t*)&g_gs_evac.name[i]; \
        v2_gs_route[(uint16_t)((off) + 2u * i)] = m; \
        v2_gs_route[(uint16_t)((off) + 2u * i + 1)] = m + 1; }
#define V2_GS_RTR(name, off, n, rec, fld) \
    for (uint32_t i = 0; i < (n); i++) { \
        uint8_t* m = (uint8_t*)&g_gs_evac.rec[i].fld; \
        v2_gs_route[(uint16_t)((off) + 2u * i)] = m; \
        v2_gs_route[(uint16_t)((off) + 2u * i + 1)] = m + 1; }
    V2_GS_FIELDS_EVAC(V2_GS_RT1, V2_GS_RTN, V2_GS_RTR)
    V2_GS_FIELDS_EVAC_BRIDGE(V2_GS_RT1, V2_GS_RTN, V2_GS_RTR)
#undef V2_GS_RT1
#undef V2_GS_RTN
#undef V2_GS_RTR
#define V2_GS_RTB1(name, off) v2_gs_route[(off)] = &g_gs_evac.name;
#define V2_GS_RTBN(name, off, n) \
    for (uint32_t i = 0; i < (n); i++) \
        v2_gs_route[(uint16_t)((off) + i)] = &g_gs_evac.name[i];
#define V2_GS_RTBR(name, off, n, rec, fld) \
    for (uint32_t i = 0; i < (n); i++) { \
        uint8_t* m = (uint8_t*)&g_gs_evac.rec[i].fld; \
        v2_gs_route[(uint16_t)((off) + 2u * i)] = m; \
        v2_gs_route[(uint16_t)((off) + 2u * i + 1)] = m + 1; }
    V2_GS_FIELDS_EVACB(V2_GS_RTB1, V2_GS_RTBN)
#undef V2_GS_RTB1
#undef V2_GS_RTBN
#undef V2_GS_RTBR
}

extern "C" void v2_gs_evac_mirror_b(const uint8_t* ds, uint16_t addr, uint8_t val) {
    if (!v2_gs_evac_on(ds)) return;
    uint8_t* t = v2_gs_route[addr];
    if (t != ds + addr) *t = val;   // evacuated byte: land in the member
}

extern "C" void v2_gs_evac_mirror_w(const uint8_t* ds, uint16_t addr, uint16_t val) {
    if (!v2_gs_evac_on(ds)) return;
    uint16_t a1 = (uint16_t)(addr + 1);
    uint8_t* lo = v2_gs_route[addr];
    uint8_t* hi = v2_gs_route[a1];
    if (lo != ds + addr) *lo = (uint8_t)val;
    if (hi != ds + a1) *hi = (uint8_t)(val >> 8);
}

extern "C" void v2_gs_evac_mirror_span(const uint8_t* ds, uint32_t addr, uint32_t len) {
    if (!v2_gs_evac_on(ds) || len == 0) return;
    uint32_t end = addr + len; if (end > 0x10000) end = 0x10000;
    for (uint32_t a = addr; a < end; a++) {
        uint8_t* t = v2_gs_route[a];
        if (t != ds + a) *t = ds[a];
    }
}


extern "C" { const uint8_t* v2_gs_evac_canonical = nullptr; }

extern "C" void v2_gs_evac_refresh(const uint8_t* ds) {
#define V2_GS_EV1(name, off) \
    g_gs_evac.name = *(const uint16_t*)(ds + (off));
#define V2_GS_EVN(name, off, n) \
    for (uint32_t i = 0; i < (n); i++) \
        g_gs_evac.name[i] = *(const uint16_t*)(ds + (off) + 2u * i);
#define V2_GS_EVR(name, off, n, rec, fld) \
    for (uint32_t i = 0; i < (n); i++) \
        g_gs_evac.rec[i].fld = *(const uint16_t*)(ds + (off) + 2u * i);
    V2_GS_FIELDS_EVAC(V2_GS_EV1, V2_GS_EVN, V2_GS_EVR)
    V2_GS_FIELDS_EVAC_BRIDGE(V2_GS_EV1, V2_GS_EVN, V2_GS_EVR)
#undef V2_GS_EV1
#undef V2_GS_EVN
#undef V2_GS_EVR
#define V2_GS_EVB1(name, off) \
    g_gs_evac.name = ds[(off)];
#define V2_GS_EVBN(name, off, n) \
    for (uint32_t i = 0; i < (n); i++) g_gs_evac.name[i] = ds[(off) + i];
#define V2_GS_EVBR(name, off, n, rec, fld) V2_GS_EVBN(name, off, n)
    V2_GS_FIELDS_EVACB(V2_GS_EVB1, V2_GS_EVBN)
#undef V2_GS_EVB1
#undef V2_GS_EVBN
#undef V2_GS_EVBR
}

// stage-4 bridge helper for the AIL blob interpreter: mirror a byte store
// that resolved into the canonical shadow DS image (pointer-range test —
// the sink/real instances resolve elsewhere and fall through untouched).
void v2_ail_interp_ds_mirror(const uint8_t* p, uint8_t v) {
    const uint8_t* base = v2_gs_evac_canonical;
    if (base && p >= base && p < base + 0x10000)
        v2_gs_evac_mirror_b(base, (uint16_t)(p - base), v);
}

// stage-4 wave 13: rebuild the flat image FROM the carrier members. The
// router knows which bytes are carrier-owned; everything else stays as-is.
extern "C" void v2_gs_image_render(uint8_t* ds) {
    if (!v2_gs_evac_on(ds)) return;
    for (uint32_t a = 0; a < 0x10000; a++) {
        const uint8_t* t = v2_gs_route[a];
        if (t != ds + a) ds[a] = *t;
    }
}

extern "C" void v2_gs_evac_set_canonical(const uint8_t* ds) {
    v2_gs_evac_canonical = ds;
    if (ds) {
        v2_gs_evac_refresh(ds);
        // wave 10: rebuild the per-byte router for the new canonical image.
        // The shadow is a writable static; const enters this API for the
        // readers' sake only.
        v2_gs_route_build(const_cast<uint8_t*>(ds));
    }
}

// Members vs image bytes. A diff means something wrote the image behind the
// accessors (a bulk writer not yet routed through the view) — hard bug.
extern "C" { long v2_gs_evac_check_calls = 0; }
extern "C" int v2_gs_evac_check(const uint8_t* ds) {
    if (!v2_gs_evac_on(ds)) return 0;
    v2_gs_evac_check_calls++;
    int diffs = 0;
#define V2_GS_EV_CHK(name, off, idx_expr, img_expr) \
    do { uint16_t _img = (img_expr); \
         if ((idx_expr) != _img) { \
             if (diffs < 8) \
                 fprintf(stderr, "V2-GS-EVAC-DIFF: %s @%04X member=%04X image=%04X tid=%ld\n", \
                         #name, (unsigned)(off), (idx_expr), _img, (long)syscall(SYS_gettid)); \
             diffs++; } } while (0)
#define V2_GS_EV1(name, off) \
    V2_GS_EV_CHK(name, (off), g_gs_evac.name, *(const uint16_t*)(ds + (off)));
#define V2_GS_EVN(name, off, n) \
    for (uint32_t i = 0; i < (n); i++) \
        V2_GS_EV_CHK(name, (off) + 2u * i, g_gs_evac.name[i], \
                     *(const uint16_t*)(ds + (off) + 2u * i));
#define V2_GS_EVR(name, off, n, rec, fld) \
    for (uint32_t i = 0; i < (n); i++) \
        V2_GS_EV_CHK(name, (off) + 2u * i, g_gs_evac.rec[i].fld, \
                     *(const uint16_t*)(ds + (off) + 2u * i));
    V2_GS_FIELDS_EVAC(V2_GS_EV1, V2_GS_EVN, V2_GS_EVR)
    V2_GS_FIELDS_EVAC_BRIDGE(V2_GS_EV1, V2_GS_EVN, V2_GS_EVR)
#undef V2_GS_EV1
#undef V2_GS_EVN
#undef V2_GS_EVR
#define V2_GS_EVB1(name, off) \
    V2_GS_EV_CHK(name, (off), g_gs_evac.name, ds[(off)]);
#define V2_GS_EVBN(name, off, n) \
    for (uint32_t i = 0; i < (n); i++) \
        V2_GS_EV_CHK(name, (off) + i, g_gs_evac.name[i], ds[(off) + i]);
#define V2_GS_EVBR(name, off, n, rec, fld) \
    for (uint32_t i = 0; i < (n); i++) \
        V2_GS_EV_CHK(name, (off) + 2u * i, g_gs_evac.rec[i].fld, \
                     *(const uint16_t*)(ds + (off) + 2u * i));
    V2_GS_FIELDS_EVACB(V2_GS_EVB1, V2_GS_EVBN)
#undef V2_GS_EVB1
#undef V2_GS_EVBN
#undef V2_GS_EVBR
#undef V2_GS_EV_CHK
    if (diffs) {
        // Bridge stage: reads still come from the image, so a desync is a
        // MAP entry (who bypasses the view), not a behavior change. After
        // the bypass writers are routed, V2_GS_EVAC_STRICT=1 turns this
        // into the hard gate and the read flip closes stage 4.
        // wave 12 (stage-4 II.c final): STRICT is the default — the carrier
        // and the image must never diverge. V2_GS_EVAC_STRICT=0 downgrades
        // to report-only for debugging hunts.
        static int strict = -1;
        if (strict < 0) {
            const char* e = getenv("V2_GS_EVAC_STRICT");
            strict = (e && e[0] == '0') ? 0 : 1;
        }
        if (strict) {
            fprintf(stderr, "FATAL: stage-4 evac desync — %d words changed "
                    "behind the view accessors\n", diffs);
            fflush(stderr);
            _exit(1);
        }
        // report-only: resync so each bypass site logs once per change
        v2_gs_evac_refresh(ds);
    }
    return diffs;
}

// stage-4 diag: first desyncs seen AT READ TIME, with backtrace.
#include <execinfo.h>
extern "C" void v2_gs_evac_read_desync(const char* fld, uint32_t off, uint16_t mem, uint16_t img) {
    static int n = 0;
    if (n >= 4) return;
    n++;
    fprintf(stderr, "V2-EVAC-READ-DESYNC[%d]: %s @%04X member=%04X image=%04X\n",
            n, fld, off, mem, img);
    void* bt[8]; int bn = backtrace(bt, 8);
    backtrace_symbols_fd(bt, bn, 2);
}
