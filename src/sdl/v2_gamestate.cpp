// v2_gamestate.cpp — Phase D step 1: byte-exact serializer for V2GameState.
//
// Coverage model: every X-macro field marks its DS bytes in a 64K coverage
// map at startup; overlaps abort (a list bug, not a runtime condition).
// serialize writes covered bytes ONLY from fields and uncovered bytes from
// the raw backing — so a green roundtrip proves both the field encodings and
// that no byte has two owners.

#include "v2_gamestate.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

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
    V2_GS_FIELDS_W(V2_GS_C1, V2_GS_CN)
#undef V2_GS_C1
#undef V2_GS_CN
#define V2_GS_CB1(name, off)     cov_mark((off), 1, #name);
#define V2_GS_CBN(name, off, n)  cov_mark((off), (n), #name);
    V2_GS_FIELDS_B(V2_GS_CB1, V2_GS_CBN)
    V2_GS_FIELDS_GAPFILL(V2_GS_CB1, V2_GS_CBN)
#undef V2_GS_CB1
#undef V2_GS_CBN
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
    V2_GS_FIELDS_W(V2_GS_D1, V2_GS_DN)
#undef V2_GS_D1
#undef V2_GS_DN
#define V2_GS_DB1(name, off)     gs->name = ds[(off)];
#define V2_GS_DBN(name, off, n)  memcpy(gs->name, ds + (off), (n));
    V2_GS_FIELDS_B(V2_GS_DB1, V2_GS_DBN)
    V2_GS_FIELDS_GAPFILL(V2_GS_DB1, V2_GS_DBN)
#undef V2_GS_DB1
#undef V2_GS_DBN
    memcpy(gs->raw, ds, 0x10000);   // backing; covered bytes unused on serialize
}

extern "C" void v2_gs_serialize(const V2GameState* gs, uint8_t* ds_out) {
    cov_build();
    // raw pass first: uncovered bytes verbatim; covered bytes are then
    // OVERWRITTEN from fields, so the backing never leaks into typed space.
    memcpy(ds_out, gs->raw, 0x10000);
#define V2_GS_S1(name, off)      wr_w(ds_out, (off), gs->name);
#define V2_GS_SN(name, off, n)   for (uint32_t i = 0; i < (n); i++) wr_w(ds_out, (off) + 2u * i, gs->name[i]);
    V2_GS_FIELDS_W(V2_GS_S1, V2_GS_SN)
#undef V2_GS_S1
#undef V2_GS_SN
#define V2_GS_SB1(name, off)     ds_out[(off)] = gs->name;
#define V2_GS_SBN(name, off, n)  memcpy(ds_out + (off), gs->name, (n));
    V2_GS_FIELDS_B(V2_GS_SB1, V2_GS_SBN)
    V2_GS_FIELDS_GAPFILL(V2_GS_SB1, V2_GS_SBN)
#undef V2_GS_SB1
#undef V2_GS_SBN
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
