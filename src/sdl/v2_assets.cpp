// Stage 5.2: the engine reads the OPEN asset tree instead of DATA.DAT.
// assets/.compiled/NNNN.bin = [8B chunk-table header][u16 decomp/plane size]
// [payload] — produced by tools/assets/assetc.py --pack from the open files;
// the byte-exact round-trip judge guarantees payload == the DATA.DAT
// decompression output, so every DS side effect of the original loader is
// reproducible without the archive.
//
// Activation: V2_ASSETS_DIR=<dir> (e.g. assets/.compiled). Designed for the
// V2_ONLY target engine; the default (verify) build keeps DATA.DAT as the
// oracle — the real world reads it via the m2c path anyway.
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

static const char* v2_assets_dir_cached() {
    static const char* d = nullptr;
    static bool init = false;
    if (!init) { d = getenv("V2_ASSETS_DIR"); if (d && !*d) d = nullptr; init = true; }
    return d;
}

extern "C" int v2_assets_on() { return v2_assets_dir_cached() != nullptr; }

// Reads the compiled record for a chunk. Returns payload length, fills the
// 8-byte table header and the u16 size field. payload buffer must hold
// max_size bytes; the record's payload is clamped to it (mirrors the
// original dest window). Returns 0 on any miss (caller falls back).
extern "C" uint32_t v2_assets_read(uint16_t chunk_id, uint8_t* hdr8,
                                   uint16_t* dsize, uint8_t* payload,
                                   uint32_t max_size) {
    const char* dir = v2_assets_dir_cached();
    if (!dir) return 0;
    char path[512];
    snprintf(path, sizeof(path), "%s/%04d.bin", dir, chunk_id);
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "V2-ASSETS: missing %s\n", path);
        return 0;
    }
    uint8_t head[10];
    if (fread(head, 1, 10, f) != 10) { fclose(f); return 0; }
    memcpy(hdr8, head, 8);
    *dsize = (uint16_t)(head[8] | (head[9] << 8));   // the block's lead u16
    // payload = the WHOLE comp block including its lead u16 (record layout
    // [8B header][comp block]); callers index the stream at payload+2.
    payload[0] = head[8]; payload[1] = head[9];
    uint32_t n = 2 + (uint32_t)fread(payload + 2, 1,
                                     max_size > 2 ? max_size - 2 : 0, f);
    fclose(f);
    return n;
}
