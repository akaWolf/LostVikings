// ============================================================================
// V2 Animation VM — independent reimplementation of sub_14207/sub_1424c
//
// The original VM (sub_1424c) is a bytecode interpreter that processes
// animation scripts for dynamic objects (intro screens, cutscenes, etc.).
// It dispatches opcodes via the off_30CAC table (216 opcodes, 0x00-0xD7).
//
// This v2 version reads the SAME bytecode and game data from DS segment,
// but renders to v2_render_buf instead of drawBuffer.
//
// The original VM continues to run for the first window.
// This VM runs in parallel for the second window.
// ============================================================================

#include <cstdint>
#include <cstring>
#include <cstdio>
#include "render_v2.h"

// Access to emulated memory
extern uint8_t* v2_m2c_base;

// ============================================================================
// V2 VM shadow state — complete copy of DS region used by animation VM.
// Copied from real DS at frame start. VM reads/writes operate on shadow.
// This ensures v2 VM is fully independent from the original VM.
// ============================================================================

// Shadow of DS segment (animation table + globals).
// Covers ds:0x0000 to ds:0x1C00 — all animation fields + globals.
static const uint32_t V2_VM_SHADOW_SIZE = 0x10000; // Full 64KB DS segment
static uint8_t v2_vm_shadow_ds[V2_VM_SHADOW_SIZE];

// Snapshot of orig DS taken right after orig sub_115d2 completes.
// Used by v2 to compare sub_115d2 output (exact same point in execution).
static uint8_t v2_115d2_snapshot[0x10000];
static bool v2_115d2_snapshot_valid = false;

// Shadow tile map — copy of the tile map segment (ES = ds:0x2E63).
// Copied from real tile map at frame start. v2 writes go here, not to real memory.
// v2_draw_tiles reads from here when V2_RENDER_FROM_SHADOW is enabled.
static const uint32_t V2_TILEMAP_SHADOW_SIZE = 0x10000; // 64KB max segment
static uint8_t v2_vm_shadow_tilemap[V2_TILEMAP_SHADOW_SIZE];
static bool v2_tilemap_shadow_valid = false;

// Shadow tile graphics — copy of the tile graphics segment (ds:0x2E5F).
// Read-only level data, but shadowed for full v2 independence.
static const uint32_t V2_TILEGFX_SHADOW_SIZE = 0x10000;
static uint8_t v2_vm_shadow_tilegfx[V2_TILEGFX_SHADOW_SIZE];
static bool v2_tilegfx_shadow_valid = false;

// Shadow animation data — copy of animation bytecode segment (ds:0x2E67).
// Contains 21-byte anim records + bytecodes. Read by VM, not normally modified.
static const uint32_t V2_ANIMDATA_SHADOW_SIZE = 0x10000;
static uint8_t v2_vm_shadow_animdata[V2_ANIMDATA_SHADOW_SIZE];
static bool v2_animdata_shadow_valid = false;

// Shadow GS segment — tile mask data (ds:0x2E61).
// Used by sub_1c8f1 / v2_draw_flagged_tiles for masked tile rendering.
static const uint32_t V2_GS_SHADOW_SIZE = 0x10000;
static uint8_t v2_vm_shadow_gs[V2_GS_SHADOW_SIZE];
static bool v2_gs_shadow_valid = false;

// Shadow GS tile data segment (ds:0x2E5D).
// Contains per-tile lookup data (8 bytes per tile). Used by sub_173c7 to build FS.
// Loaded from bg_chunk (word_2AAC5) during level init.
// SEPARATE from ds:0x2E61 (masks) — different segment, different data.
static const uint32_t V2_GS_TILEDATA_SIZE = 0x10000;
static uint8_t v2_vm_shadow_gs_tiledata[V2_GS_TILEDATA_SIZE];
static bool v2_gs_tiledata_valid = false;

// Shadow sound data segment (ds:0x2E6B).
// Sound sequences read by sound opcodes (0x02, 0x04, 0xD5, 0xD7).
static const uint32_t V2_SOUND_SHADOW_SIZE = 0x10000;
static uint8_t v2_vm_shadow_sound[V2_SOUND_SHADOW_SIZE];
static bool v2_sound_shadow_valid = false;

// Shadow chunk buffer segment (ds:0x2E77).
// Large multi-purpose buffer: animation bytecodes (from sub_116ae) + intro chunk data.
// Original allocation: 0x2ABA paragraphs = 175,392 bytes (~171KB).
// sub_10E85 normalizes es:di across 64KB segment boundaries within this allocation.
// For v2 linear shadow: single contiguous buffer, offset = (es - base_seg) * 16 + di.
static const uint32_t V2_CHUNK_SHADOW_SIZE = 0x2ABA0; // 0x2ABA * 16 = 175,392 bytes
static uint8_t v2_vm_shadow_chunk[V2_CHUNK_SHADOW_SIZE];
static bool v2_chunk_shadow_valid = false;

// Shadow FS segment — render tilemap (ds:0x2E69).
// Built from ES (tilemap) + GS (tile masks) by sub_173c7.
// Used by VGA rendering (sub_16dd9). v2 rendering doesn't read from it,
// but maintaining it for state consistency.
static const uint32_t V2_FS_SHADOW_SIZE = 0x10000;
static uint8_t v2_vm_shadow_fs[V2_FS_SHADOW_SIZE];
static bool v2_fs_shadow_valid = false;

// Shadow sprite data buffer — for decompressed sprite graphics.
// Decompression (anim cmd 0x34DC) writes here instead of real game memory.
// v2_draw_sprites reads from here when available.
// Max: 128 sub-sprites × 1152 bytes each = 147KB.
// Indexed by absolute address (segment*16 + offset).
// We use a map-like approach: track which addresses have shadow data.
static const uint32_t V2_SPRITE_SHADOW_SIZE = 256 * 1024; // 256KB
static uint8_t v2_sprite_shadow[V2_SPRITE_SHADOW_SIZE];
static uint32_t v2_sprite_shadow_base = 0; // base address of shadow region
static bool v2_sprite_shadow_active = false;

// Accumulator lives in the active shadow DS at offset 0x8A — same as original.
// v2_vm_acc_base points to whichever shadow is active (main or replay).
// This ensures opcode handlers always write acc to the correct shadow.
static uint8_t* v2_vm_acc_base = v2_vm_shadow_ds;
#define v2_vm_accumulator (*(uint16_t*)(v2_vm_acc_base + 0x8A))

// Per-opcode execution trace for verification
struct V2VMTraceEntry {
    uint8_t  opcode;
    uint16_t pc_before;  // PC before opcode dispatch (after reading opcode byte)
    uint16_t pc_after;   // PC after opcode execution
    uint16_t acc_before; // accumulator BEFORE opcode
    uint16_t acc_after;  // accumulator AFTER opcode
    uint16_t es_seg;     // es segment value at this opcode
};
static const int V2_VM_TRACE_MAX = 512;
static V2VMTraceEntry v2_vm_trace[128][V2_VM_TRACE_MAX]; // per object slot
static int v2_vm_trace_count[128];

// Per-object detailed trace: set v2_trace_object to object index (0,2,4,...) to log every opcode.
// Set to 0xFFFF to disable. Traces to stderr with PC, opcode, accumulator, key DS reads.
static uint16_t v2_trace_object = 0xFFFF;

// Anim command counters for verification
int v2_orig_anim_cmd_count = 0;  // incremented by original's anim cmd loop
static int v2_v2_anim_cmd_count = 0;  // incremented by v2's anim cmd loop

static bool v2_replay_verify_active = false; // when true, resolve_segment uses real memory
uint16_t v2_input_snapshot = 0; // snapshot of input_keys taken by seg000 after orig sub_12352
static uint16_t v2_word30BBE_snapshot = 0; // snapshot of word_30BBE at barrier sync point

// ============================================================================
// HW watchpoint via perf_event_open — catches ALL writes including memset/spillover
// ============================================================================
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>

static int v2_hw_wp_fd = -1;
static struct perf_event_mmap_page* v2_hw_wp_mmap = nullptr;
static uint8_t* v2_hw_wp_ring = nullptr; // ring buffer data starts after mmap page
static const char* v2_hw_wp_label = "";
static uint8_t* v2_hw_wp_ptr = nullptr;
static size_t v2_hw_wp_ring_size = 0;

// Arm watchpoint on any byte. Uses mmap ring buffer for samples with real IP.
void v2_hw_wp_arm(uint8_t* ptr, const char* label) {
    if (v2_hw_wp_fd >= 0) {
        if (v2_hw_wp_mmap) munmap(v2_hw_wp_mmap, (1 + 4) * 4096);
        close(v2_hw_wp_fd);
        v2_hw_wp_fd = -1;
        v2_hw_wp_mmap = nullptr;
    }
    if (!ptr) return;

    v2_hw_wp_ptr = ptr;
    v2_hw_wp_label = label;

    struct perf_event_attr pe = {};
    pe.type = PERF_TYPE_BREAKPOINT;
    pe.size = sizeof(pe);
    pe.bp_type = HW_BREAKPOINT_W;
    pe.bp_addr = (unsigned long)ptr;
    pe.bp_len = HW_BREAKPOINT_LEN_1;
    pe.disabled = 0;
    pe.sample_period = 1;
    pe.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_ADDR;
    pe.wakeup_events = 1;

    v2_hw_wp_fd = syscall(__NR_perf_event_open, &pe, 0, -1, -1, 0);
    if (v2_hw_wp_fd < 0) { perror("HW-WP: perf_event_open"); return; }

    // mmap ring buffer: 1 metadata page + 4 data pages (must be 1+2^n pages)
    size_t mmap_size = (1 + 4) * 4096;
    void* mm = mmap(NULL, mmap_size,
                    PROT_READ | PROT_WRITE, MAP_SHARED, v2_hw_wp_fd, 0);
    if (mm == MAP_FAILED) { perror("HW-WP: mmap"); close(v2_hw_wp_fd); v2_hw_wp_fd = -1; return; }

    v2_hw_wp_mmap = (struct perf_event_mmap_page*)mm;
    v2_hw_wp_ring = (uint8_t*)mm + v2_hw_wp_mmap->data_offset;
    v2_hw_wp_ring_size = v2_hw_wp_mmap->data_size;

    ioctl(v2_hw_wp_fd, PERF_EVENT_IOC_ENABLE, 0);
    fprintf(stderr, "HW-WP: armed on %s (addr=%p) mmap ring=%zuB\n", label, ptr, v2_hw_wp_ring_size);
}

// Drain ring buffer — call periodically to read samples with IP
void v2_hw_wp_drain() {
    if (!v2_hw_wp_mmap || v2_hw_wp_fd < 0) return;
    static int total_samples = 0;

    // Memory barrier to see latest data_head
    uint64_t head = __atomic_load_n(&v2_hw_wp_mmap->data_head, __ATOMIC_ACQUIRE);
    uint64_t tail = v2_hw_wp_mmap->data_tail;

    while (tail < head && total_samples < 200) {
        uint64_t off = tail % v2_hw_wp_ring_size;
        struct perf_event_header* hdr = (struct perf_event_header*)(v2_hw_wp_ring + off);

        if (hdr->type == PERF_RECORD_SAMPLE) {
            // Layout: { ip (uint64), addr (uint64) } after header
            uint64_t ip = 0, addr = 0;
            uint64_t p = (off + sizeof(*hdr)) % v2_hw_wp_ring_size;
            memcpy(&ip, v2_hw_wp_ring + p, sizeof(ip));
            p = (p + sizeof(uint64_t)) % v2_hw_wp_ring_size;
            memcpy(&addr, v2_hw_wp_ring + p, sizeof(addr));
            total_samples++;
            uint8_t val = v2_hw_wp_ptr ? *v2_hw_wp_ptr : 0;
            // IP-4 = the actual store instruction on aarch64
            uint64_t store_ip = ip - 4;
            // dladdr to resolve function name
            Dl_info dli = {};
            dladdr((void*)store_ip, &dli);
            const char* fname = dli.dli_sname ? dli.dli_sname : "??";
            uintptr_t foff = dli.dli_saddr ? (store_ip - (uintptr_t)dli.dli_saddr) : 0;
            fprintf(stderr, "HW-WP[%d]: %s = 0x%02X  store=0x%lX (%s+0x%lX) data_addr=0x%lX\n",
                total_samples, v2_hw_wp_label, val,
                (unsigned long)store_ip, fname, (unsigned long)foff,
                (unsigned long)addr);
        }
        tail += hdr->size;
    }
    __atomic_store_n(&v2_hw_wp_mmap->data_tail, tail, __ATOMIC_RELEASE);
}

// Convenience: arm on shadow DS offset
void v2_hw_wp_arm_ds(uint16_t offset) {
    static char label[64];
    snprintf(label, sizeof(label), "shadow:0x%04X", offset);
    v2_hw_wp_arm(v2_vm_shadow_ds + offset, label);
}


// Forward declarations
static bool v2_sub_13809(uint8_t* s, uint16_t code_seg_idx, uint16_t di_spawn,
                          uint16_t si_anim, uint16_t pos_x, uint16_t pos_y);
static uint16_t v2_current_ds_val; // DS segment value for rendering calls
static void v2_vm_init_table();

// ============================================================================
// V2 Segment Resolver — maps segment:offset to shadow buffer pointers.
// In standalone mode, segment values are fake (assigned by v2_sub_12ab8).
// This function maps them to the correct shadow buffer + offset.
// ============================================================================
uint8_t* v2_resolve_segment(uint16_t seg, uint8_t* shadow_ds) {
    if (v2_replay_verify_active && v2_m2c_base) {
        return v2_m2c_base + (uint32_t)seg * 16;
    }
    if (!shadow_ds) shadow_ds = v2_vm_shadow_ds;
    // Check each shadow segment by comparing with stored DS value.
    // ORDER MATTERS: narrow ranges first, wide ranges last.
    // Sprite segment: range [base .. base + 0x700 paragraphs]
    uint16_t sprite_base = *(uint16_t*)(shadow_ds + 0x2E73);
    if (sprite_base && seg >= sprite_base && seg < (uint16_t)(sprite_base + 0x700)) {
        return v2_sprite_shadow + (uint32_t)(seg - sprite_base) * 16;
    }
    // Exact segment matches
    if (seg == *(uint16_t*)(shadow_ds + 0x2E5F)) return v2_vm_shadow_tilegfx;
    if (seg == *(uint16_t*)(shadow_ds + 0x2E61)) return v2_vm_shadow_gs;
    if (seg == *(uint16_t*)(shadow_ds + 0x2E5D)) return v2_vm_shadow_gs_tiledata;
    if (seg == *(uint16_t*)(shadow_ds + 0x2E63)) return v2_vm_shadow_tilemap;
    if (seg == *(uint16_t*)(shadow_ds + 0x2E69)) return v2_vm_shadow_fs;
    if (seg == *(uint16_t*)(shadow_ds + 0x2E67)) return v2_vm_shadow_animdata;
    if (seg == *(uint16_t*)(shadow_ds + 0x2E6B)) return v2_vm_shadow_sound;
    // Animdata: range [base .. base + 0xC00 paragraphs] (for normalized pointers)
    uint16_t anim_base = *(uint16_t*)(shadow_ds + 0x2E67);
    if (anim_base && seg >= anim_base && seg < (uint16_t)(anim_base + 0xC00)) {
        return v2_vm_shadow_animdata + (uint32_t)(seg - anim_base) * 16;
    }
    // Chunk buffer: range [base .. base + 0x2ABA paragraphs] — LAST (widest range)
    uint16_t chunk_base = *(uint16_t*)(shadow_ds + 0x2E77);
    if (chunk_base && seg >= chunk_base && seg < (uint16_t)(chunk_base + 0x2ABA)) {
        static int chunk_dbg = 0;
        if (chunk_dbg < 3) {
            chunk_dbg++;
            printf("V2-RESOLVE-CHUNK: seg=0x%04X chunk_base=0x%04X off=0x%X\n",
                   seg, chunk_base, (uint32_t)(seg - chunk_base) * 16);
        }
        return v2_vm_shadow_chunk + (uint32_t)(seg - chunk_base) * 16;
    }
    // Fallback to original emulated memory if available
    if (v2_m2c_base) return v2_m2c_base + (uint32_t)seg * 16;
    printf("V2-RESOLVE: unknown segment 0x%04X!\n", seg);
    return nullptr;
}

// ============================================================================
// V2 Resource System — LZSS decompressor + DATA.DAT reader
// Exact replica of sub_10982 (read_chunk). Writes to shadow buffers.
// ============================================================================
static FILE* v2_data_handle = nullptr; // v2's own DATA.DAT handle
static uint8_t* v2_vm_real_ds_ptr = nullptr; // for verification — set by init_shadow_early

// LZSS decompress from compressed data into destination buffer.
// Exact replica of sub_10982 decompression loop.
// src = compressed data, dest = output buffer, decompressed_size = target size.
// Returns number of bytes written.
// ring_out: if non-null, receives the final ring buffer state (LZSS sliding window).
// In the original, this is FS[0..0xFFF] — written by the decompressor during operation.
static uint32_t v2_lzss_decompress(const uint8_t* src, uint8_t* dest, uint16_t decompressed_size,
                                    uint8_t* ring_out = nullptr) {
    uint8_t ring[4096]; // 4KB ring buffer (ds:0..0xFFF in original)
    memset(ring, 0, 4096);
    uint16_t bx = 0;   // ring buffer write pos
    uint16_t si = 0;    // source read pos
    uint16_t di = 0;    // dest write pos
    uint16_t dx = decompressed_size;

    auto finish = [&]() {
        if (ring_out) memcpy(ring_out, ring, 4096);
        return di;
    };

    while (true) {
        uint8_t flags = src[si++];
        for (int bit = 0; bit < 8; bit++) {
            if (flags & 1) {
                // Literal byte
                uint8_t val = src[si++];
                ring[bx] = val;
                bx = (bx + 1) & 0xFFF;
                dest[di++] = val;
                dx--;
                if (dx >= decompressed_size) return finish();
            } else {
                // Back-reference: 2 bytes → offset (12 bits) + length (4 bits) + 3
                uint16_t ref = *(uint16_t*)(src + si);
                si += 2;
                uint16_t length = ((ref >> 12) & 0xF) + 3;
                uint16_t offset = ref & 0xFFF;
                for (uint16_t j = 0; j < length; j++) {
                    uint8_t val = ring[offset];
                    ring[bx] = val;
                    bx = (bx + 1) & 0xFFF;
                    dest[di++] = val;
                    dx--;
                    if (dx >= decompressed_size) return finish();
                    offset = (offset + 1) & 0xFFF;
                }
            }
            flags >>= 1;
        }
    }
    return finish();
}

// v2_read_chunk: read chunk from DATA.DAT into buffer.
// Exact replica of sub_10982/read_chunk: seek, read compressed, LZSS decompress.
// chunk_id = chunk number, dest = destination buffer, max_size = buffer size.
// Returns decompressed size, or 0 on failure.
static uint32_t v2_read_chunk(uint16_t chunk_id, uint8_t* dest, uint32_t max_size) {
    if (!v2_data_handle) {
        v2_data_handle = fopen("DATA.DAT", "rb");
        if (!v2_data_handle) return 0;
    }
    if (chunk_id == 0xFFFA) return 0; // special: no-op

    // Read chunk table entry: seek to chunk_id * 4
    uint32_t table_offset = (uint32_t)chunk_id * 4;
    if (fseek(v2_data_handle, table_offset, SEEK_SET)) return 0;

    // Read 8 bytes: offset (4) + next_offset (4)
    // Original sub_10982: fread(raddr(ds,0x2BB4), 8, 1, data_handle)
    uint8_t header[8];
    if (fread(header, 8, 1, v2_data_handle) != 1) return 0;
    uint32_t chunk_offset = *(uint32_t*)(header);
    uint32_t next_offset = *(uint32_t*)(header + 4);
    uint32_t compressed_size = next_offset - chunk_offset;
    // Write header to shadow DS (same as original sub_10982 writes to ds:0x2BB4)
    memcpy(v2_vm_shadow_ds + 0x2BB4, header, 8);

    // Seek to chunk data
    if (fseek(v2_data_handle, chunk_offset, SEEK_SET)) return 0;

    // Read decompressed size (first 2 bytes)
    // Original: fread(raddr(ds,0x2BBC), 2, 1, data_handle)
    uint16_t decompressed_size;
    if (fread(&decompressed_size, 2, 1, v2_data_handle) != 1) return 0;
    *(uint16_t*)(v2_vm_shadow_ds + 0x2BBC) = decompressed_size;

    // Original sub_10982: ecx = compressed_size (including 2-byte header already read).
    // After reading the 2-byte header, file position = chunk_offset + 2.
    // Original reads cx = compressed_size bytes from that position into FS:0x1000.
    // This reads 2 bytes MORE than actual compressed data (into next chunk's table entry).
    // For non-FS path (loc_1098e): reads compressed_size bytes into allocated temp buffer.
    // We replicate: read compressed_size bytes for FS temp path, compressed_size-2 for non-FS.
    uint32_t comp_data_size = compressed_size - 2;
    static uint8_t comp_buf[0x10000];
    uint32_t eax_check = compressed_size >> 4;
    bool use_fs_temp = (eax_check < 0x0B08);
    size_t read;
    if (use_fs_temp) {
        // Original: fread(fs:0x1000, cx, 1, data_handle) where cx = compressed_size (full).
        // Reads compressed_size bytes starting at chunk_offset + 2.
        uint32_t fs_read_size = compressed_size;
        if (fs_read_size > sizeof(comp_buf)) fs_read_size = sizeof(comp_buf);
        read = fread(v2_vm_shadow_fs + 0x1000, 1, fs_read_size, v2_data_handle);
        // Zero shadow_fs[0..0xFFF] — original: memset(ds:0, 0, 0x800 words)
        memset(v2_vm_shadow_fs, 0, 0x1000);
        // Copy to comp_buf for LZSS decompression
        if (read > 0) memcpy(comp_buf, v2_vm_shadow_fs + 0x1000, read);
        printf("V2-CHUNK: id=0x%X use_fs_temp comp=%d decomp=%d\n", chunk_id, (int)fs_read_size, decompressed_size);
    } else {
        // Non-FS path (loc_1098e): allocate temp, read compressed_size bytes
        uint32_t buf_read_size = compressed_size;
        if (buf_read_size > sizeof(comp_buf)) buf_read_size = sizeof(comp_buf);
        printf("V2-CHUNK: id=0x%X NO_fs_temp comp=%d decomp=%d\n", chunk_id, (int)buf_read_size, decompressed_size);
        read = fread(comp_buf, 1, buf_read_size, v2_data_handle);
    }
    if (read == 0) return 0;

    // LZSS decompress. When using FS as temp, the sliding window (ring buffer)
    // is written to shadow_fs[0..0xFFF] — replicating the original's ds:[bx] writes.
    return v2_lzss_decompress(comp_buf, dest, decompressed_size,
                              use_fs_temp ? v2_vm_shadow_fs : nullptr);
}

// sub_10cd8: read raw chunk (no LZSS decompression).
// Reads plane_size (2 bytes) + raw data (plane_size * 4 bytes) from DATA.DAT.
// Returns plane_size. dest receives raw plane data (plane_size * 4 bytes).
static uint16_t v2_read_raw_chunk(uint16_t chunk_id, uint8_t* dest, uint32_t max_size) {
    if (!v2_data_handle) {
        v2_data_handle = fopen("DATA.DAT", "rb");
        if (!v2_data_handle) return 0;
    }
    if (chunk_id == 0xFFFA) return 0;

    // Read chunk table entry
    uint32_t table_offset = (uint32_t)chunk_id * 4;
    if (fseek(v2_data_handle, table_offset, SEEK_SET)) return 0;

    uint8_t header[8];
    if (fread(header, 8, 1, v2_data_handle) != 1) return 0;
    uint32_t chunk_offset = *(uint32_t*)(header);

    // Seek to chunk data
    if (fseek(v2_data_handle, chunk_offset, SEEK_SET)) return 0;

    // Read plane_size (2 bytes)
    uint16_t plane_size;
    if (fread(&plane_size, 2, 1, v2_data_handle) != 1) return 0;

    // Read raw plane data (plane_size * 4 bytes)
    uint32_t data_size = (uint32_t)plane_size * 4;
    if (data_size > max_size) data_size = max_size;
    if (fread(dest, 1, data_size, v2_data_handle) != data_size) return 0;

    return plane_size;
}

// ============================================================================
// V2 Level Init — exact replica of sub_11080 + all sub-functions
// ============================================================================

static void v2_do_render_and_swap(); // forward decl for init functions
static bool v2_load_exe_ds(); // forward decl
static void v2_sub_16775(uint8_t* s);
static void v2_do_render();
static bool v2_frame_active = false; // true between frame_begin and frame_end, false when JMP sub_11080 skips rest
static void v2_game_loop_pre_vm(uint8_t* shadow, uint16_t ds_val);
static void v2_game_loop_post_vm(uint8_t* shadow);
static void v2_game_loop_post_render(uint8_t* shadow);
static void v2_vm_execute_object(uint8_t* ds, uint16_t si);
static void v2_run_collision_vm(uint8_t* shadow, uint16_t si);

// sub_10fe6: write full palette to VGA DAC. Reads 768 bytes from ds:[word_303E0] (usually ds:0x8202).
// Original: OUT 0x3C8=0 (start color), REP OUTSB 0x300 bytes to port 0x3C9, then setPalette().
// For v2: palette in shadow DS at 0x8202, applied by render callback. DS write: word_303DE=0.
static void v2_sub_10fe6(uint8_t* s) {
    *(uint16_t*)(s + 0x7EFE) = 0; // word_303DE = 0 (palette write complete)
    // Original VGA DAC write:
    // OUT(0x3C8, 0);  // start at color 0
    // REP OUTSB ds:[word_303E0], 0x300 bytes → port 0x3C9
    // for (i=0; i<256; i++) setPalette(i, ds[0x8202+i*3+0]<<2, ds[0x8202+i*3+1]<<2, ds[0x8202+i*3+2]<<2);
}

// sub_10ffc: palette animation (underwater/lava cycling). Processes 8 animation slots (bx=7→0).
// Each slot: check enable flag, copy timer, compute color range, write partial palette to VGA DAC.
// DS writes: [bx+0x258C] = timer copy, word_303DE = 0. VGA: OUT + setPalette for affected ranges.
static void v2_sub_10ffc(uint8_t* s) {
    *(uint16_t*)(s + 0x7EFE) = 0; // word_303DE = 0
    uint16_t pal_base = *(uint16_t*)(s + 0x7F00); // word_303E0 = palette data pointer
    for (int16_t bx = 7; bx >= 0; bx--) {
        uint8_t mask = s[(uint16_t)(bx - 0x6C44)]; // bit mask from table
        if (!(s[0x2583] & mask)) continue;          // byte_2AA63: animation enable bits
        if (s[bx + 0x258C] != 0) continue;          // timer not zero → skip
        s[bx + 0x258C] = s[bx + 0x2584];            // reset timer from reload value
        uint8_t end_color = s[bx + 0x259C];
        uint8_t start_color = s[bx + 0x2594];
        int16_t range = (int16_t)(uint16_t)end_color - (int16_t)(uint16_t)start_color;
        if (range == 0) continue;
        // Palette update: write colors [start_color..end_color] from ds:[pal_base + color*3]
        // Original: OUT(0x3C8, start_color); REP OUTSB (range+1)*3 bytes → port 0x3C9
        // setPalette(i+base, ds[pal_base+i*3]<<2, ds[pal_base+i*3+1]<<2, ds[pal_base+i*3+2]<<2);
        // For v2: palette data already in shadow DS at pal_base, render callback reads it.
    }
}

// sub_10fa0: palette fade to black. Blocking render loop bx=0→0x45.
// sub_10f03: palette shading — reads ds:0x7F02 (source palette), writes ds:0x8202 (shaded palette).
// Subtracts shade bytes (ds:0x342-0x347) from each RGB component, clamps to [0, 0x3F].
static void v2_sub_10f03(uint8_t* s) {
    uint8_t r_shade = s[0x342] | s[0x345];
    uint8_t g_shade = s[0x343] | s[0x346];
    uint8_t b_shade = s[0x344] | s[0x347];
    uint8_t* src = s + 0x7F02;
    uint8_t* dst = s + 0x8202;
    for (int i = 0; i < 0x100; i++) {
        int r = src[0] - r_shade; if (r < 0) r = 0; if (r >= 0x40) r = 0x3F; dst[0] = (uint8_t)r;
        int g = src[1] - g_shade; if (g < 0) g = 0; if (g >= 0x40) g = 0x3F; dst[1] = (uint8_t)g;
        int b = src[2] - b_shade; if (b < 0) b = 0; if (b >= 0x40) b = 0x3F; dst[2] = (uint8_t)b;
        src += 3; dst += 3;
    }
}

static void v2_sub_10fa0(uint8_t* s) {
    for (uint16_t bx = 0; bx <= 0x45; bx++) {
        s[0x0342] = (uint8_t)bx; // byte_28822
        s[0x0343] = (uint8_t)bx; // byte_28823
        s[0x0344] = (uint8_t)bx; // byte_28824
        v2_sub_10f03(s);                    // sub_10f03: palette shading → ds:0x8202
        *(uint16_t*)(s + 0x7EFE) = 4;       // word_303DE = 4 (request palette write)
        *(uint16_t*)(s + 0x7F00) = 0x8202;  // word_303E0 = shaded palette pointer
        // sub_16775: render + palette write + vsync
        v2_do_render_and_swap();
        v2_sub_10fe6(s); // sub_10fe6: palette → VGA DAC, word_303DE = 0
    }
    s[0x0342] = 0;
    s[0x0343] = 0;
    s[0x0344] = 0;
    *(uint16_t*)(s + 0x7F00) = 0x8202;
}

// sub_1450b (seg000): save game state.
// Original: seg000 lines 9467-9482 (eip 0x450B..0x452F).
// DS writes: ds:0x342 = al<<1, ds:0x343 = si<<1 (byte), ds:0x344 = di<<1 (byte),
//            ds:0x7EFD |= 1, ds:0x7EFE = 4, ds:0x7F00 = 0x8202.
// Then JMP sub_10E99 (disk I/O — NOP for v2).
static void v2_sub_1450b(uint8_t* s, uint8_t al, uint16_t si, uint16_t di) {
    s[0x342] = (uint8_t)(al << 1);                                  // SHL al, 1; MOV ds:342h, al
    s[0x343] = (uint8_t)((uint8_t)si << 1);                        // SHL al, 1; MOV ds:343h, al
    s[0x344] = (uint8_t)((uint8_t)di << 1);                        // SHL al, 1; MOV ds:344h, al
    s[0x7EFD] |= 1;                                                 // OR byte ptr ds:7EFDh, 1
    *(uint16_t*)(s + 0x7EFE) = 4;                                   // MOV word ptr ds:7EFEh, 4
    *(uint16_t*)(s + 0x7F00) = 0x8202;                              // MOV word ptr ds:7F00h, 8202h
    // JMP sub_10E99: disk I/O save operation — NOP for v2
}

// sub_10130 (seg000): VGA vsync wait. Verified with seg000 lines 2020-2031.
// Original: CMP word_3287C, 1; JGE loop (wait for VGA interrupt to clear flag).
// DS reads: ds:0xA39C. No DS writes.
// For v2: no VGA interrupt → flag cleared immediately by v2_sub_16775.
static void v2_sub_10130(uint8_t* s) {
    // ======================================================================
    // VGA VSYNC WAIT (sub_10130, eip 0x0130-0x0137)
    // ======================================================================
    // Original: spin wait while word_3287C (ds:0xA39C) >= 1.
    // VGA retrace interrupt calls sub_1797b: DEC word_3287C + palette dispatch.
    //
    // v2: call sub_1797b logic synchronously. No sleep needed in v2 thread
    // (timing controlled by barrier sync with original's sub_10130 which sleeps).
    // ======================================================================
    while ((int16_t)*(uint16_t*)(s + 0xA39C) >= 1) {
        // sub_1797b: DEC word_3287C + palette dispatch (off_17974[word_303DE])
        uint16_t pal_mode = *(uint16_t*)(s + 0x7EFE); // word_303DE
        if (pal_mode == 4) {
            v2_sub_10fe6(s);       // off_17974[4] = sub_10fe6: full palette write
        } else if (pal_mode == 2) {
            v2_sub_10ffc(s);       // off_17974[2] = sub_10ffc: palette animation
        }
        // pal_mode == 0: off_17974[0] = nullsub_1 (no-op)
        *(uint16_t*)(s + 0xA39C) -= 1; // DEC word_3287C
    }
}

// sub_101be (seg000): Palette cycling for UI elements.
// Verified with seg000 lines 2086-2148.
// Iterates 8 palette channels (si=7 down to 0).
// For each: checks mask (byte_2AA63 at ds:0x2583), counter at [si+0x258C].
// If counter reaches 0: rotates 3-byte palette entries (sub_10255 shifts up, sub_1020f shifts down).
// DS writes: DEC [si+0x258C], word_303DE=2, palette buffer rotations at ds:0x8202+ area.
static void v2_sub_101be(uint8_t* s) {
    if (s[0x2583] == 0) {                                           // TEST byte_2AA63, 0FFh; JZ
        *(uint16_t*)(s + 0x7EFE) = 2;                              // word_303DE = 2 (at loc_1020b fallthrough)
        return;
    }
    for (int16_t si = 7; si >= 0; si--) {                           // si=7; DEC si; JNS
        uint8_t mask = s[0x2583];                                    // byte_2AA63
        if (!(s[(uint16_t)(si - 0x6C44)] & mask)) continue;        // TEST [si-6C44h], al; JZ
        if (s[si + 0x258C] == 0) continue;                         // TEST [si+258Ch]; JZ
        s[si + 0x258C]--;                                           // DEC [si+258Ch]
        if (s[si + 0x258C] != 0) continue;                         // JNZ skip
        // Counter reached 0: rotate palette entries
        uint8_t cur_idx = s[si + 0x259C];                          // [si+259Ch] = current
        uint8_t end_idx = s[si + 0x2594];                          // [si+2594h] = end
        uint16_t dx_base = 0x8202;                                  // palette buffer base
        if (cur_idx < end_idx) {
            // sub_10255: shift entries DOWN (current < end → rotate left)
            uint16_t di_addr = (uint16_t)cur_idx * 3 + dx_base;
            // Save entry at [di_addr]
            uint16_t saved_w = *(uint16_t*)(s + di_addr);
            uint8_t saved_b = s[di_addr + 2];
            // Shift: MOVSB from di+3 to di, count = (end-cur)*3
            uint16_t count = ((uint16_t)end_idx - (uint16_t)cur_idx) * 3;
            memmove(s + di_addr, s + di_addr + 3, count);
            // Write saved entry at end
            uint16_t end_addr = di_addr + count;
            *(uint16_t*)(s + end_addr) = saved_w;
            s[end_addr + 2] = saved_b;
            // Second rotation with dx=0x7F02
            dx_base = 0x7F02;
            di_addr = (uint16_t)cur_idx * 3 + dx_base;
            saved_w = *(uint16_t*)(s + di_addr);
            saved_b = s[di_addr + 2];
            count = ((uint16_t)end_idx - (uint16_t)cur_idx) * 3;
            memmove(s + di_addr, s + di_addr + 3, count);
            end_addr = di_addr + count;
            *(uint16_t*)(s + end_addr) = saved_w;
            s[end_addr + 2] = saved_b;
        } else {
            // sub_1020f: shift entries UP (current >= end → rotate right)
            uint16_t di_addr = (uint16_t)cur_idx * 3 + dx_base;
            uint16_t end_addr = (uint16_t)end_idx * 3 + dx_base;
            // Save entry at end position
            uint16_t saved_w = *(uint16_t*)(s + di_addr);
            uint8_t saved_b = s[di_addr + 2];
            // Shift: REP MOVSB backwards (STD) from di-1 to di+2
            uint16_t count = ((uint16_t)cur_idx - (uint16_t)end_idx) * 3;
            memmove(s + end_addr + 3, s + end_addr, count);
            // Write saved entry at start
            *(uint16_t*)(s + end_addr) = saved_w;
            s[end_addr + 2] = saved_b;
            // Second rotation with dx=0x7F02
            dx_base = 0x7F02;
            di_addr = (uint16_t)cur_idx * 3 + dx_base;
            end_addr = (uint16_t)end_idx * 3 + dx_base;
            saved_w = *(uint16_t*)(s + di_addr);
            saved_b = s[di_addr + 2];
            count = ((uint16_t)cur_idx - (uint16_t)end_idx) * 3;
            memmove(s + end_addr + 3, s + end_addr, count);
            *(uint16_t*)(s + end_addr) = saved_w;
            s[end_addr + 2] = saved_b;
        }
    }
    *(uint16_t*)(s + 0x7EFE) = 2;                                  // word_303DE = 2
}

// sub_108c8 (seg000): Sound state toggle + cleanup.
// Verified with seg000 lines 2894-2953.
// DS writes: byte_3166B (ds:0x918B), word_287E4 (ds:0x0304), sound handles (ds:0x990E-0x9914),
//            byte_3167E (ds:0x919E), word_287E2 (ds:0x0302).
static void v2_sub_108c8(uint8_t* s) {
    uint16_t ax = *(uint16_t*)(s + 0x0302) & *(uint16_t*)(s + 0x0304);  // word_287E2 & word_287E4
    if (ax & 0x8000) return;                                              // TEST ax, 8000h; JNZ ret
    if (s[0x91A4] != 1) return;                                          // CMP byte_31684, 1; JNZ ret
    if (s[0x918B] == 1) {                                                // CMP byte_3166B, 1; JNZ skip
        s[0x918B] = 0;                                                   // MOV byte_3166B, 0
        s[0x0304] ^= 1;                                                  // XOR byte ptr word_287E4, 1
        if (!(s[0x0304] & 1)) {                                         // JZ skip (after XOR)
            // Toggle ON: stop all sound slots
            for (uint16_t si = 2; si < 0x0A; si += 2) {
                uint16_t h_off = (uint16_t)(si - 0x66F4);
                if (*(uint16_t*)(s + h_off) != 0xFFFF) {
                    // sub_1C79F(handle, driver): stop sequence — AIL, commented
                    // sub_1C769(handle, driver): release sequence — AIL, commented
                    *(uint16_t*)(s + h_off) = 0xFFFF;                    // clear handle
                    *(uint16_t*)(s + (uint16_t)(si - 0x66EA)) = 0xFFFF; // clear sequence
                }
            }
        }
    }
    // loc_10935: music toggle
    if (s[0x919E] != 1) return;                                          // CMP byte_3167E, 1; JNZ ret
    s[0x919E] = 0;                                                       // MOV byte_3167E, 0
    s[0x0302] ^= 1;                                                      // XOR byte ptr word_287E2, 1
    if (s[0x0302] & 1) {
        // Toggle OFF → stop: already handled by XOR making bit 0 = 1
        // loc_10959: test 0x8000; if set → skip. Else stop all + release.
        if (!(*(uint16_t*)(s + 0x0302) & 0x8000)) {
            for (uint16_t si = 2; si < 0x0A; si += 2) {
                uint16_t h_off = (uint16_t)(si - 0x66F4);
                if (*(uint16_t*)(s + h_off) != 0xFFFF) {
                    // sub_1C79F + sub_1C769: stop + release — AIL, commented
                    *(uint16_t*)(s + h_off) = 0xFFFF;
                    *(uint16_t*)(s + (uint16_t)(si - 0x66EA)) = 0xFFFF;
                }
            }
        }
    } else {
        // Toggle ON → start playback
        // sub_176bd(si=0, ax=0, bx=word_2B34B): start XMIDI — AIL, commented
    }
}

// sub_16528 (seg000): Save/set INT 9h keyboard handler. Verified with seg000 lines 13939-13956.
// INT(0x21, ax=0x3509); → get INT 9 vector → saves to cs:word_16436 (offset), cs:word_16438 (segment)
// INT(0x21, ax=0x2509, dx=0x6440); → set INT 9 to our handler at cs:0x6440
// DS writes: NONE — saves to CS segment temporaries.
// For v2: SDL handles keyboard input via SDL_GetKeyboardState.
static void v2_sub_16528(uint8_t* /*s*/) {
    // INT(0x21, ax=0x3509);  // DOS: get interrupt vector 9h (keyboard) → es:bx
    // cs:word_16436 = bx;    // save original handler offset
    // cs:word_16438 = es;    // save original handler segment
    // INT(0x21, ax=0x2509, ds=cs, dx=0x6440); // DOS: set INT 9 to our handler
}

// sub_1689e (seg000): VGA tile draw. Verified with seg000 lines 228-287.
// Reads tile data from ds:0x2E5F segment (64 bytes: 4 planes × 8 words × 2 bytes).
// si = tile_word: bits 5-4 select flip mode (00=normal, 10=hflip, 01=vflip, 11=hflip+vflip).
//                 bits 15-6 = tile_index * 64 (data offset in tile GFX segment).
// di = VGA destination offset. es = 0xA000.
// For each of 4 planes: OUT(0x3C4, plane_mask); 8 × MOV word to VGA at row offsets 0,0x56,0xAC,...
// VGA pitch = 0x56 (86 bytes per row in Mode X).
// No DS writes. Pure VGA pixel copy.
static void v2_sub_1689e(uint8_t* /*s*/, uint16_t /*si_tile*/, uint16_t /*di_vga*/) {
    // uint16_t flip = si_tile & 0x30;
    // uint16_t data_off = si_tile & 0xFFC0;
    // uint8_t* tile_src = ds_2E5F_segment + data_off;
    // es = 0xA000;
    // if (flip == 0x00) { // no flip
    //     OUT(0x3C4, 0x0102);  // plane 0
    //     for (int r=0; r<8; r++) es:[di + r*0x56] = tile_src[r*2]; // 8 words
    //     OUT(0x3C4, 0x0202);  // plane 1
    //     for (int r=0; r<8; r++) es:[di + r*0x56] = tile_src[0x10 + r*2];
    //     OUT(0x3C4, 0x0402);  // plane 2
    //     for (int r=0; r<8; r++) es:[di + r*0x56] = tile_src[0x20 + r*2];
    //     OUT(0x3C4, 0x0802);  // plane 3
    //     for (int r=0; r<8; r++) es:[di + r*0x56] = tile_src[0x30 + r*2];
    // } else if (flip == 0x20) { // hflip: loc_16B3F
    //     // Same but reads in reverse byte order within each word
    // } else if (flip == 0x10) { // vflip: loc_169DE
    //     // Same but reads rows bottom-to-top
    // } else { // hflip+vflip: loc_16C60
    //     // Both reversed
    // }
}

// sub_16dc1 (seg000): VGA tile ROW render. Verified with seg000 lines 14348-14366.
// Loop cx=0x2B (43 tiles). Each: si=fs:[bx], call sub_1689e(si, di), bx+=2, di+=2.
// Renders one horizontal row of 43 tiles to VGA page.
// No DS writes. Pure VGA rendering.
static void v2_sub_16dc1(uint8_t* /*s*/, uint16_t /*bx_fs*/, uint16_t /*di_vga*/) {
    // for (int col = 0; col < 0x2B; col++) {
    //     uint16_t si = fs:[bx];  // read tile word from FS
    //     v2_sub_1689e(s, si, di);  // render tile to VGA
    //     bx += 2; di += 2;
    // }
}

// sub_171dc (seg000): VGA page copy for COLUMN tiles. Verified with seg000 lines 14709+.
// Similar structure to sub_1712b but for column (horizontal) rendering.
// OUT(0x3C4, 0x0F02); OUT(0x3CE, 0x0008);
// Copies tile column between VGA pages using ds:930D/930F/930B offsets.
// No DS writes.
static void v2_sub_171dc(uint8_t* /*s*/) {
    // OUT(0x3C4, 0x0F02);  // enable all 4 planes
    // OUT(0x3CE, 0x0008);  // bit mask = 0 (read latch mode)
    // Copy column: si=ds:930B (page 3), di=ds:930D (page 1)
    //   Loop ds:0x9311 times: MOV al,es:[bx+si]; MOV es:[bx+di],al; bx += 0x56
    // If ds:0x9312: copy remainder to ds:930F (page 2)
    // Same for ds:931B/931D pages with ds:9313/9314 counts
    // OUT(0x3CE, 0xFF08);  // reset bit mask
}

// sub_16dd9 (seg000): VGA tile COLUMN render. Verified with seg000 lines 14367-14377.
// Loop cx=0x19 (25 rows). Each: si=fs:[bx], call sub_1689e(si, di),
//   bx += ds:0x8F6C (FS page stride), di += 0x2B0 (VGA pitch * 8 rows).
// Renders one vertical column of 25 tiles to VGA page.
// No DS writes.
static void v2_sub_16dd9(uint8_t* /*s*/) {
    // for (int row = 0; row < 0x19; row++) {
    //     uint16_t si = fs:[bx];
    //     v2_sub_1689e(s, si, di);
    //     bx += ds:0x8F6C;  // FS page stride
    //     di += 0x2B0;      // VGA: 8 rows × 0x56 bytes/row = 0x2B0
    // }
}

// sub_1712b (seg000): VGA page copy for ROW tiles. Verified with seg000 lines 119-182.
// Copies rendered tile row from source VGA page to other pages.
// OUT(0x3C4, 0x0F02); OUT(0x3CE, 0x0008);
// 3 copy phases using ds:9315/9317/9311/9312/9319/931B/9313/9314/931D tracking vars:
//   Phase 1: si=ds:9315, di=ds:9317, cx=ds:9311 → copy cx rows, 2 bytes/row, stride 0x56
//   Phase 2: si+=bx, di=ds:9319, cx=ds:9312 → copy remainder to page 2
//   Phase 3: si=ds:9315, di=ds:931B, cx=ds:9313 → copy to page 3
//            then ds:931D for ds:9314 remainder
// OUT(0x3CE, 0xFF08); // reset bit mask
// No DS writes.
static void v2_sub_1712b(uint8_t* /*s*/) {
    // OUT(0x3C4, 0x0F02);  // enable all planes
    // OUT(0x3CE, 0x0008);  // bit mask register
    // Phase 1: copy from ds:9315 → ds:9317 (cx=ds:9311 rows)
    // Phase 2: copy continuation → ds:9319 (cx=ds:9312 rows)
    // Phase 3: copy from ds:9315 → ds:931B (cx=ds:9313 rows)
    // Phase 4: copy continuation → ds:931D (cx=ds:9314 rows)
    // OUT(0x3CE, 0xFF08);  // reset bit mask
}

// sub_1E16D (seg003): VGA glyph pixel render. Verified with seg003 lines 3116-3142.
// Computes glyph data offset: si = (si_glyph * 8 + si_glyph) * 8 → si*72.
//   Then ADD si, 0x687E (glyph table base in seg003 code segment).
// Writes to cs:word_1C830=0xFF, cs:word_1C834=0, cs:word_1C832=0, cs:byte_1C83A=4.
// Then JMP loc_1CF39 → complex glyph rendering dispatch (switch on glyph pixel pattern).
// Each dispatch path: OUT(0x3C4, plane); writes to es:0xA000 via indexed addressing.
// All writes go to VGA (es) and cs temporaries — NO DS writes.
static void v2_sub_1E16D(uint8_t* /*s*/, uint16_t /*si_glyph*/, uint16_t /*di_vga*/) {
    // uint16_t si = si_glyph;
    // si <<= 3;             // SHL si, 3
    // uint16_t bx = si;
    // si <<= 3;             // SHL si, 3 (now si = original * 64)
    // si += bx;             // si += original * 8 → si = original * 72
    // si += 0x687E;         // glyph data table in seg003
    // cs:word_1C830 = 0xFF; cs:word_1C834 = 0; cs:word_1C832 = 0; cs:byte_1C83A = 4;
    // JMP loc_1CF39: render glyph via dispatch table (256 entries for pixel patterns)
    //   Each case: reads glyph bytes from cs:[si], writes pixels to es:[di] with plane selection
    //   OUT(0x3C4, plane_mask) for each plane change
}

// sub_1C939 / loc_1C939 (seg003): VGA flagged tile render with mask.
// Verified with seg003 lines 65-102.
// Called from sub_1C8F1 when fs:[di] has bit 3 set.
// Reads: si=fs:[di] (tile word), computes VGA position from bx (row), cx (col),
//   ds:0x2581/0x257F (scroll), ds:0x92F9 (page offset), [di-0x7608] (VGA row base).
// Reads tile data from ds:0x2E5F (tile GFX segment), mask from gs=ds:0x2E61.
// Renders masked tile: gs:[ecx] = mask byte, dispatches by jpt_1C9B1 to render with mask.
// Flip dispatch: si & 0x30 → 4 paths (normal/hflip/vflip/both).
// Each path: OUT(0x3C4, plane_mask); read tile+mask, write to es:0xA000 with masking.
// No DS writes.
static void v2_sub_1C939(uint8_t* /*s*/, uint16_t /*fs_val*/, uint16_t /*di_fs*/) {
    // uint16_t si = fs:[di]; // tile word from dirty page table
    // Compute VGA address from (row=0x19-bx, col=0x2B-cx) + scroll + page offset
    // gs = ds:0x2E61 (mask segment)
    // ecx = (si & 0xFFC0) >> 3 (mask data offset)
    // ds = ds:0x2E5F (tile GFX segment)
    // es = 0xA000
    // Dispatch by si & 0x30:
    //   0x00: loc_1C99B — normal
    //     OUT(0x3C4, 0x0102); // plane 0
    //     mask = gs:[ecx]; dispatch jpt_1C9B1[mask] for masked write
    //     (repeat for planes 1-3 with ecx+1, ecx+2, ecx+3)
    //   0x20: loc_1CA91 — hflip
    //   0x10: loc_1CB87 — vflip
    //   0x30: loc_1CC81 — hflip+vflip
}

// sub_1C8F1 (seg003): flagged tile rendering — FS side effects only.
// Original: seg003 lines 27-64 (eip 0x00C1..0x0108).
// Iterates viewport tiles (0x2B×0x19), checks fs:[di] for bits 9 and 1.
// If both set: AND fs:[di], ax (caller passes ax=0xFFFE, clears bit 0).
// If bit 3 also set: render flagged tile to VGA — VGA only, skipped for v2.
// DS reads: ds:0x2581, ds:0x257F, ds:0x25DC, ds:[di-0x7098].
// FS writes: AND fs:[di], ax (clears dirty flag bit 0).
static void v2_sub_1C8F1(uint8_t* s, uint16_t ax_mask) {
    uint16_t cx = 0x2B;                                              // MOV cx, 2Bh (columns)
    uint16_t bx = 0x19;                                              // MOV bx, 19h (rows)
    // line 36-40: di = row_offset_table[ds:0x2581] + ds:0x257F, scaled
    uint16_t di_base = *(uint16_t*)(s + 0x2581);                    // MOV di, ds:2581h
    di_base <<= 1;                                                    // SHL di, 1
    di_base = *(uint16_t*)(s + (uint16_t)(di_base - 0x7098));       // MOV di, [di-7098h]
    di_base += *(uint16_t*)(s + 0x257F);                             // ADD di, ds:257Fh
    di_base <<= 1;                                                    // SHL di, 1
    // line 41-43: bp = ds:0x25DC * 4 - 0x56 (row skip)
    uint16_t bp = *(uint16_t*)(s + 0x25DC);                         // MOV bp, ds:25DCh
    bp <<= 2;                                                        // SHL bp, 2
    bp -= 0x56;                                                       // SUB bp, 56h

    uint16_t di = di_base;
    for (uint16_t row = 0; row < bx; row++) {                       // outer loop (rows)
        for (uint16_t col = 0; col < cx; col++) {                   // inner loop (columns)
            if (di < V2_FS_SHADOW_SIZE - 1) {
                uint16_t fs_val = *(uint16_t*)(v2_vm_shadow_fs + di);
                // line 46-49: TEST fs:[di], 9; JZ skip; TEST fs:[di], 1; JZ skip
                if ((fs_val & 9) && (fs_val & 1)) {
                    // line 50: AND fs:[di], ax (clear dirty flag)
                    *(uint16_t*)(v2_vm_shadow_fs + di) &= ax_mask;   // AND fs:[di], ax
                    // line 51-52: TEST fs:[di], 8; JNZ loc_1C939
                    if (fs_val & 8) {
                        v2_sub_1C939(s, fs_val, di);  // VGA flagged tile render
                    }
                }
            }
            di += 2;                                                  // ADD di, 2
        }
        di += bp;                                                     // ADD di, bp (skip to next row)
    }
    // OUT(0x3CE, 0xFF08); OUT(0x3C4, 0x0F02); — VGA register cleanup, commented for v2
}

// sub_1CD7D / sub_1CD7B (seg003): set dirty flags in fs for sprite tile rectangle.
// Original: seg003 lines 607-682 (eip 0x054B..0x05BE).
// sub_1CD7B: entry with bp=si (small sprites). sub_1CD7D: entry with bp from caller (large sprites, bp=5).
// Takes: cx=X pixel, dx=Y pixel, si=height tiles, bp=width tiles.
// Clips against viewport (ds:0x9168, ds:0x916A).
// For each tile in clipped rect: OR fs:[si_off], 3.
// Also renders to VGA (sub_1C8F1 style) — VGA part skipped for v2.
static void v2_sub_1CD7D(uint8_t* s, int16_t cx_x, int16_t dx_y, int16_t si_h, int16_t bp_w) {
    // line 615-619: if !(cx & 7) → DEC bp                          ; TEST cx, 7; JNZ; DEC bp
    if (!(cx_x & 7)) bp_w--;
    // line 622-624: if !(dx & 7) → DEC si                          ; TEST dx, 7; JNZ; DEC si
    if (!(dx_y & 7)) si_h--;
    // line 627-628: SAR cx, 3; SAR dx, 3                           ; convert to tile coords (signed!)
    cx_x >>= 3;
    dx_y >>= 3;
    // line 629-630: ax = si; si = 0                                ; ax=visible height, si=fs offset
    int16_t ax_h = si_h;
    int16_t si_off = 0;

    // X bounds check
    // line 631-634: OR cx, cx; JNS loc_1CDA0                       ; if cx >= 0 → check right bound
    if (cx_x < 0) {
        bp_w += cx_x;                                                // ADD bp, cx
        if (bp_w <= 0) return;                                      // JLE locret_1CDEE
    } else {
        // line 638-644: check right bound
        si_off += cx_x;                                              // ADD si, cx
        if (cx_x >= (int16_t)*(uint16_t*)(s + 0x9168)) return;     // CMP cx, ds:9168h; JGE return
        int16_t overflow = cx_x + bp_w - (int16_t)*(uint16_t*)(s + 0x9168);
        if (overflow > 0) bp_w -= overflow;                          // SUB bp, cx (clip right)
    }

    // Y bounds check
    // line 647-651: OR dx, dx; JNS loc_1CDBC
    if (dx_y < 0) {
        ax_h += dx_y;                                                // ADD ax, dx
        if (ax_h <= 0) return;                                      // JLE return
    } else {
        // line 654-662: check bottom bound
        if (dx_y >= (int16_t)*(uint16_t*)(s + 0x916A)) return;     // CMP dx, ds:916Ah; JGE return
        // line 656-658: si += row_offset_table[dx]
        uint16_t bx = (uint16_t)dx_y << 1;
        si_off += (int16_t)*(uint16_t*)(s + (uint16_t)(bx - 0x7098));  // ADD si, [bx-7098h]
        // line 659-662: clip bottom
        int16_t overflow = dx_y + ax_h - (int16_t)*(uint16_t*)(s + 0x916A);
        if (overflow > 0) ax_h -= overflow;                          // SUB ax, dx
    }

    // line 665-668: compute skip and convert to word offsets
    int16_t dx_skip = ((int16_t)*(uint16_t*)(s + 0x9168) - bp_w) * 2;  // (vw - bp) * 2
    si_off *= 2;                                                     // SHL si, 1

    // line 670-679: mark dirty flags
    for (int16_t row = 0; row < ax_h; row++) {                      // loc_1CDDE
        for (int16_t col = 0; col < bp_w; col++) {                  // loc_1CDE0
            if ((uint16_t)si_off < V2_FS_SHADOW_SIZE - 1) {
                *(uint16_t*)(v2_vm_shadow_fs + (uint16_t)si_off) |= 3; // OR fs:[si], 3
            }
            si_off += 2;                                              // ADD si, 2
        }
        si_off += dx_skip;                                           // ADD si, dx
    }
}

// sub_1E0C7 (seg003): UI glyph rendering. Full replica with commented VGA writes.
// Original: seg003 lines 3037-3115 (eip 0x1897..0x193C).
// DS reads: 0x956B (glyph dirty flag), 0x25CF (HUD mode), 0x98DC (throttle),
//           0x956C (glyph buffer, 0x370 bytes), 0x257F (col scroll), 0x2581 (row scroll),
//           0x92F9 (page offset), ds:[di-0x7098] (row offset table), ds:[di-0x7608] (VGA row table),
//           0x9569 (full screen text flag).
// DS writes: INC 0x98DC (conditional).
// FS writes: OR fs:[bp], 1 (per glyph — dirty page flag).
// VGA writes: sub_1E16D renders glyph pixels to es:0xA000 — commented for v2.
static void v2_sub_1E0C7(uint8_t* s) {
    // line 3037: TEST byte ptr ds:956Bh, 0FFh                      ; glyph dirty flag
    if (s[0x956B] == 0) return;                                      // JZ locret_1E16C

    // line 3041: TEST byte ptr ds:25CFh, 0E0h                      ; HUD mode bits
    if (s[0x25CF] & 0xE0) {                                         // JZ loc_1E0E4
        // line 3043: CMP word ptr ds:98DCh, 3                      ; throttle check
        if ((int16_t)*(uint16_t*)(s + 0x98DC) > 3) return;          // JG locret_1E16C
        // line 3045: INC word ptr ds:98DCh                         ; throttle increment
        *(uint16_t*)(s + 0x98DC) += 1;
    }

    // loc_1E0E4: scan glyph buffer for non-zero entries
    uint16_t bx = 0x956C;                                           // MOV bx, 956Ch
    uint16_t cx = 0x370;                                             // MOV cx, 370h

    while (true) {
        // loc_1E0EA: REPE SCASB — scan for first non-zero byte in ds:[bx..bx+cx-1]
        // ax = 0, es = ds, di = bx
        uint16_t scan_pos = bx;
        bool found = false;
        for (uint16_t i = 0; i < cx; i++) {
            if (s[scan_pos + i] != 0) {
                bx = scan_pos + i;                                   // bx = di - 1 after SCASB
                cx -= (i + 1);                                       // remaining count
                found = true;
                break;
            }
        }
        if (!found) return;                                          // JZ locret_1E16C (all zero)

        // line 3061: si = MOVZX(ds:[bx]) - 0x10                   ; glyph index
        uint16_t si_glyph = (uint16_t)s[bx] - 0x10;                // SUB si, 10h

        // Position calculation:
        // line 3063-3064: ax = bx - 0x956C                         ; offset in glyph buffer
        uint16_t ax = bx - 0x956C;                                  // SUB ax, 956Ch
        // line 3065-3066: DIV dl(0x28) → al=row, ah=col
        uint8_t row = (uint8_t)(ax / 0x28);                         // al = quotient
        uint8_t col = (uint8_t)(ax % 0x28);                         // ah = remainder
        // line 3067-3068: dx = col, ax = row
        uint16_t dx = (uint16_t)col;                                 // MOVZX dx, ah
        ax = (uint16_t)row;                                          // XOR ah, ah
        // line 3069: dx += ds:0x257F                                ; add viewport column scroll
        dx += *(uint16_t*)(s + 0x257F);                              // ADD dx, ds:257Fh
        // line 3070: ax += ds:0x2581                                ; add viewport row scroll
        ax += *(uint16_t*)(s + 0x2581);                              // ADD ax, ds:2581h

        // Tile map lookup:
        // line 3071-3072: di = ax; SHL di, 1                       ; row word index
        uint16_t di = ax;
        di <<= 1;                                                    // SHL di, 1
        // line 3073: bp = ds:[di - 0x7098]                         ; row offset from tile map row table
        uint16_t bp = *(uint16_t*)(s + (uint16_t)(di - 0x7098));    // MOV bp, [di-7098h]
        // line 3074: bp += dx                                       ; add column
        bp += dx;                                                    // ADD bp, dx
        // line 3075: SHL bp, 1                                      ; word offset for fs
        bp <<= 1;                                                    // SHL bp, 1
        // line 3076: di += ds:0x92F9                                ; add page offset
        di += *(uint16_t*)(s + 0x92F9);                              // ADD di, ds:92F9h
        // line 3077: di = ds:[di - 0x7608]                          ; VGA row base from page table
        di = *(uint16_t*)(s + (uint16_t)(di - 0x7608));             // MOV di, [di-7608h]
        // line 3078-3080: dx = (dx + 4) * 2; di += dx              ; VGA column offset
        dx += 4;                                                     // ADD dx, 4
        dx <<= 1;                                                    // SHL dx, 1
        di += dx;                                                    // ADD di, dx
        // line 3081-3082: es = 0xA000                               ; VGA segment
        // es = 0xA000; — commented, VGA not used in v2

        // Inner loop: process consecutive glyphs in row
        while (cx > 0) {                                             // LOOP loc_1E133
            // loc_1E133:
            // line 3085-3088: dirty flag condition
            bool set_dirty = true;
            if (*(uint16_t*)(s + 0x9569) == 0) {                    // TEST word ds:9569h
                if (s[0x25CF] & 0xE0) {                             // TEST byte ds:25CFh, 0E0h
                    set_dirty = false;                               // JNZ loc_1E147 (skip OR)
                }
            }
            // loc_1E142: OR word ptr fs:[bp], 1
            if (set_dirty && bp < V2_FS_SHADOW_SIZE - 1) {
                *(uint16_t*)(v2_vm_shadow_fs + bp) |= 1;            // OR fs:[bp+0], 1
            }

            // loc_1E147: render glyph to VGA (if not full-screen text mode)
            // line 3094-3095: TEST word ds:9569h; JNZ loc_1E158
            if (*(uint16_t*)(s + 0x9569) == 0) {
                v2_sub_1E16D(s, si_glyph, di);  // VGA glyph pixel render
            }

            // loc_1E158:
            // line 3105: INC bx                                     ; next glyph position
            bx++;
            // line 3106-3107: if ds:[bx] == 0 → back to outer scan
            if (s[bx] == 0) break;                                   // JZ loc_1E0EA
            // line 3108-3109: si = ds:[bx] - 0x10; next glyph index
            si_glyph = (uint16_t)s[bx] - 0x10;                      // SUB si, 10h
            // line 3110-3111: advance VGA and fs pointers
            di += 2;                                                  // ADD di, 2
            bp += 2;                                                  // ADD bp, 2
            cx--;                                                     // LOOP (implicit DEC cx)
        }
        // If cx reached 0 → return (fall through loop)
        if (cx == 0) return;
        // Otherwise: ds:[bx]==0 → go back to outer scan (loc_1E0EA)
        bx++;  // skip the zero byte, advance to next scan position
        // cx was already decremented by the inner loop; continue outer scan
    }
}

// sub_1DE05: dirty rect processing — DEC byte [di+0x114E] + save position.
// Exact replica of seg003 sub_1DE05 DS side effects.
static void v2_sub_1DE05(uint8_t* s) {
    for (int16_t di = 0xFE; di >= 0; di -= 2) {
        if (s[di + 0x114E] != 0) {
            uint16_t si = (*(uint16_t*)(s + di + 0x0C4D) >> 3) + 1;
            if (si > 5) {
                // bp = 5 path (large sprite)
            }
            s[di + 0x114E]--;                                        // DEC byte [di+114Eh]
            // sub_1CD7D/sub_1CD7B: VGA dirty rect render + set fs dirty flags
            {
                int16_t cx_x = (int16_t)*(uint16_t*)(s + di + 0x0F4D);  // old X
                int16_t dx_y = (int16_t)*(uint16_t*)(s + di + 0x104D);  // old Y
                int16_t bp_v = (si > 5) ? 5 : (int16_t)si;             // bp=5 for large, si for small
                v2_sub_1CD7D(s, cx_x, dx_y, (int16_t)si, bp_v);        // set fs dirty + VGA render (commented)
            }
            // Position copy: [0F4D] ← [0D4D], [104D] ← [0E4D]
            *(uint16_t*)(s + di + 0x0F4D) = *(uint16_t*)(s + di + 0x0D4D);
            *(uint16_t*)(s + di + 0x104D) = *(uint16_t*)(s + di + 0x0E4D);
        }
    }
    // Exact replica of sub_1DE05 tile redraw loop.
    // Verified line-by-line with seg003 lines 38087-38217.
    // OUT(0x3C4, 0x0F02); — VGA sequencer, commented
    // OUT(0x3CE, 0x0008); — VGA graphics, commented
    {
        uint16_t cx = 0x2B;                                             // 38096 mov cx, 2Bh
        uint16_t bx = 0x19;                                             // 38097 mov bx, 19h
        uint16_t di_fs = *(uint16_t*)(s + 0x2581);                     // 38098 mov di, ds:2581h
        di_fs <<= 1;                                                    // 38099 shl di, 1
        di_fs = *(uint16_t*)(s + (uint16_t)(di_fs - 0x7098));          // 38100 mov di, [di-7098h]
        di_fs += *(uint16_t*)(s + 0x257F);                             // 38101 add di, ds:257Fh
        di_fs <<= 1;                                                    // 38102 shl di, 1
        int16_t bp_fs = (int16_t)*(uint16_t*)(s + 0x25DC);            // 38103 mov bp, ds:25DCh
        bp_fs <<= 2;                                                    // 38104 shl bp, 2
        bp_fs -= 0x56;                                                  // 38105 sub bp, 56h

        // loc_1DE93: outer loop (bx = row counter, cx = column counter)
        while (bx != 0) {
            // Scan columns for dirty tile (bit 0 set)
            bool found = false;
            while (cx != 0) {
                if (di_fs < V2_FS_SHADOW_SIZE - 1 &&
                    (*(uint16_t*)(v2_vm_shadow_fs + di_fs) & 1)) {      // 38109 test fs:[di], 1
                    found = true; break;                                 // 38110 jnz loc_1DEAA
                }
                di_fs += 2;                                              // 38111 add di, 2
                cx--;                                                    // 38112 loop
            }
            if (!found) {
                di_fs = (uint16_t)((int16_t)di_fs + bp_fs);             // 38113 add di, bp
                cx = 0x2B;                                               // 38114 mov cx, 2Bh
                bx--;                                                    // 38115 dec bx
                continue;                                                // 38116 jnz loc_1DE93
            }

            // loc_1DEAA: clear bit 1 of found tile
            if (di_fs < V2_FS_SHADOW_SIZE - 1)
                *(uint16_t*)(v2_vm_shadow_fs + di_fs) &= 0xFFFD;        // 38121 and fs:[di], 0FFFDh
            di_fs += 2;                                                  // 38122 add di, 2
            uint16_t dx_tl = 2;                                         // 38123 mov dx, 2
            uint16_t si_row = 0x19 - bx + *(uint16_t*)(s + 0x2581);    // 38124-38126
            uint16_t ax_col = 0x2B - cx + *(uint16_t*)(s + 0x257F);    // 38127-38129
            cx--;                                                        // 38130 dec cx
            if (cx == 0) goto de05_end_row;                              // 38131 jcxz loc_1DEDE

            // loc_1DECA: continue scanning remaining columns
            {
                uint16_t rem = cx;
                while (rem != 0) {
                    if (di_fs < V2_FS_SHADOW_SIZE - 1 &&
                        (*(uint16_t*)(v2_vm_shadow_fs + di_fs) & 1)) {   // 38134 test fs:[di], 1
                        *(uint16_t*)(v2_vm_shadow_fs + di_fs) &= 0xFFFD; // 38136 and fs:[di], 0FFFDh
                        di_fs += 2;                                       // 38137 add di, 2
                        dx_tl += 2;                                       // 38138 add dx, 2
                        rem--;                                            // 38139 loop
                    } else {
                        cx = rem;                                         // 38135 jz loc_1DEE4
                        goto de05_render;
                    }
                }
            }
            // Fall through: all remaining columns scanned
        de05_end_row:
            di_fs = (uint16_t)((int16_t)di_fs + bp_fs);                 // 38142 add di, bp
            cx = 0x2B;                                                   // 38143 mov cx, 2Bh
            bx--;                                                        // 38144 dec bx

        de05_render:
            // loc_1DEE4: VGA tile copy — PUSH cx,bx,di,ds,es then render.
            // Compute VGA addresses from si_row, ax_col.
            // Verified: seg003 lines 38147-38206. All VGA copy, no DS writes.
            // v2: skip VGA render. After render: POP es,ds,di,bx,cx → continue.
            // bx/cx/di already updated above.
            (void)si_row; (void)ax_col; (void)dx_tl;
        }
    }
    // loc_1DF5C:
    // OUT(0x3CE, 0xFF08); — VGA graphics reset, commented
    s[0x9568] = 0;                                                       // 38215 mov byte ptr ds:9568h, 0
    // POP es; RETF
}

// sub_1DD9C: sprite render — DEC/set byte [di+0x114D] + save position.
// Exact replica of seg003 sub_1DD9C DS side effects.
static void v2_sub_1DD9C(uint8_t* s) {
    for (int16_t di = 0xFE; di >= 0; di -= 2) {
        uint16_t flags = *(uint16_t*)(s + di + 0x44D);
        if (!(flags & 0x8000)) continue;  // not active
        if (flags & 0x6000) continue;     // priority skip

        bool force_render = (s[0x9568] != 0);
        bool dirty = (s[di + 0x114D] != 0);

        if (!force_render && !dirty) {
            // sub_1CDEF: visibility check — exact bounds logic from original.
            // Original checks viewport overlap AND dirty page flags (fs).
            // v2: no dirty pages (full-frame render), so skip fs scan = always "dirty" if in viewport.
            int16_t cx = (int16_t)*(uint16_t*)(s + di + 0x64D);   // [di+64Dh] = x pixel
            int16_t dx = (int16_t)*(uint16_t*)(s + di + 0x74D);   // [di+74Dh] = y pixel
            int16_t si_h = (int16_t)((*(uint16_t*)(s + di + 0x0C4D) >> 3) + 1); // strip count
            int16_t bp_w = si_h;                                    // bp = si (width = height initially)
            if (!(cx & 7)) bp_w--;                                  // TEST cx,7; JNZ skip; DEC bp
            if (!(dx & 7)) si_h--;                                  // TEST dx,7; JNZ skip; DEC si
            cx >>= 3;                                               // SAR cx, 3 (signed!)
            dx >>= 3;                                               // SAR dx, 3 (signed!)
            int16_t ax_h = si_h;                                    // ax = si (visible height)
            int16_t si_off = 0;                                      // MOV si, 0 (fs offset accumulator)

            // X bounds check
            bool visible = true;
            int16_t vw = (int16_t)*(uint16_t*)(s + 0x9168);       // ds:9168h viewport tile width
            if (cx < 0) {
                bp_w += cx;                                          // ADD bp, cx (reduce width)
                if (bp_w <= 0) visible = false;                     // JLE not_visible
            } else {
                si_off += cx;                                        // ADD si, cx (fs column offset)
                if (cx >= vw) visible = false;                      // CMP cx, ds:9168h; JGE not_visible
                else {
                    int16_t overflow = cx + bp_w - vw;              // right edge overflow
                    if (overflow > 0) bp_w -= overflow;              // clip right
                }
            }

            // Y bounds check (only if X visible)
            if (visible) {
                int16_t vh = (int16_t)*(uint16_t*)(s + 0x916A);   // ds:916Ah viewport tile height
                if (dx < 0) {
                    ax_h += dx;                                      // ADD ax, dx
                    if (ax_h <= 0) visible = false;                 // JLE not_visible
                } else {
                    if (dx >= vh) visible = false;                  // CMP dx, ds:916Ah; JGE not_visible
                    else {
                        // ADD si, [bx-7098h] where bx=dx*2          ; row offset in fs
                        uint16_t bx = (uint16_t)dx << 1;
                        si_off += (int16_t)*(uint16_t*)(s + (uint16_t)(bx - 0x7098));
                        int16_t overflow = dx + ax_h - vh;          // bottom edge overflow
                        if (overflow > 0) ax_h -= overflow;         // clip bottom
                    }
                }
            }

            if (!visible) continue;

            // loc_1CE58: scan fs dirty page flags in clipped tile rectangle.
            // Original: lines 745-766 (eip 0x0628..0x0647).
            // If ANY tile in rectangle has fs bit 0 set → visible (NZ). Otherwise → not visible (ZF).
            {
                int16_t dx_skip = ((int16_t)*(uint16_t*)(s + 0x9168) - bp_w) * 2; // (vw - bp) * 2
                int16_t si_scan = si_off * 2;                        // ADD si, si (word offset)
                bool fs_dirty = false;
                for (int16_t r = 0; r < ax_h && !fs_dirty; r++) {   // loc_1CE62
                    for (int16_t c = 0; c < bp_w; c++) {            // loc_1CE64
                        uint16_t fs_off = (uint16_t)si_scan;
                        if (fs_off < V2_FS_SHADOW_SIZE - 1) {
                            uint16_t fs_val = *(uint16_t*)(v2_vm_shadow_fs + fs_off);
                            if (fs_val & 1) { // TEST fs:[si], 1
                                // Trace: who made this dirty?
                                { static int _fst=0; if(di==0x30 && s[di+0x114D]==0 && _fst<3) { _fst++;
                                  fprintf(stderr,"V2-FS-HIT[%d]: di=%02X fs[%04X]=%04X r=%d c=%d bp=%d ax=%d skip=%d\n",
                                    _fst,(uint16_t)di,fs_off,fs_val,r,c,bp_w,ax_h,dx_skip); } }
                                fs_dirty = true;                     // JNZ locret_1CE77 (visible!)
                                break;
                            }
                        }
                        si_scan += 2;                                // ADD si, 2
                    }
                    si_scan += dx_skip;                              // ADD si, dx
                }
                if (!fs_dirty) continue;                             // loc_1CE75: XOR ax, ax → not visible
            }
            { static int _vis=0; if(di==0x30 && _vis<5) { _vis++;
              uint16_t _fsoff = (uint16_t)(si_off * 2);
              uint16_t _sv = (_fsoff < V2_FS_SHADOW_SIZE - 1) ? *(uint16_t*)(v2_vm_shadow_fs + _fsoff) : 0xDEAD;
              uint16_t _rv = 0xDEAD;
              if (v2_m2c_base) {
                  uint16_t _fsseg = *(uint16_t*)(v2_vm_real_ds_ptr + 0x2E69);
                  if (_fsseg) _rv = *(uint16_t*)(v2_m2c_base + (uint32_t)_fsseg * 16 + _fsoff);
              }
              fprintf(stderr,"V2-VIS[%d]: mode=%02X fsoff=%04X shadow_fs=%04X real_fs=%04X (bit0: s=%d r=%d)\n",
                _vis,s[di+0x114D],_fsoff,_sv,_rv,_sv&1,_rv&1); } }
            s[di + 0x114D] = 3;                                     // MOV byte [di+114Dh], 3
        }

        // loc_1DDC7: DEC byte [di+114Dh]
        { static int _v2dd=0; if(di==0x30 && _v2dd<30) { _v2dd++;
          fprintf(stderr,"V2-DD9C[%d]: di=%02X mode=%02X force=%02X flags=%04X vp=(%04X,%04X) xy=(%04X,%04X)\n",
            _v2dd,(uint16_t)di,s[di+0x114D],s[0x9568],*(uint16_t*)(s+di+0x44D),
            *(uint16_t*)(s+0x44),*(uint16_t*)(s+0x46),
            *(uint16_t*)(s+di+0x64D),*(uint16_t*)(s+di+0x74D)); } }
        s[di + 0x114D]--;
        if ((int8_t)s[di + 0x114D] < 0)
            s[di + 0x114D] = 0;

        // Rendering dispatch: VGA render by sprite type (cs:[bp+15CBh]).
        // Read handler address from dispatch table at CS:0x15CB dynamically.
        // Verified table: [0]=0000 [1]=0648 [2]=1078 [3]=0000 [4]=0B82 [5-7]=0000.
        // Each handler: bounds check → VGA render (commented) → sub_1CD7D.
        // DS side effects: mode byte writes at clipping/exit, sub_1CD7D FS writes.
        {
            uint16_t sprite_type = *(uint16_t*)(s + di + 0x44D) & 7;
            uint16_t handler = 0;
            if (v2_m2c_base) {
                uint16_t cs_val = 0x0E25;
                handler = *(uint16_t*)(v2_m2c_base + (uint32_t)cs_val * 16 + sprite_type * 2 + 0x15CB);
            }

            if (handler == 0x1078) {
                // Type 2 (loc_1d8a8): dynamic sprite. Verified seg003 lines 37487-37781.
                // Exact replica of bounds check + DS side effects.
                int16_t cx = (int16_t)*(uint16_t*)(s + di + 0x64D);  // 37490
                int16_t dx = (int16_t)*(uint16_t*)(s + di + 0x74D);  // 37491
                // cs:word_1C830=0xFFFF, cs:word_1C834=0, cs:word_1C832=0 — VGA clipping state, no DS effect
                int16_t ax;
                // X right bound: ax = viewport_X + 0x140
                ax = (int16_t)*(uint16_t*)(s + 0x44) + 0x140;        // 37495-37496
                if (cx >= ax) goto type2_exit;                         // 37498: JGE loc_1DB98
                // X left margin: ax -= 0x1F
                ax -= 0x1F;                                            // 37499
                if (cx >= ax) {                                        // 37501: JL → skip means cx >= ax
                    s[di + 0x114D] = 2;                                // 37502: left clipping mode
                    // cs:word_1C830 = cs:[bx+0x1379] lookup — VGA only
                }
                // X left bound: ax -= 0x140
                ax -= 0x140;                                           // 37511
                if (cx <= ax) goto type2_exit;                         // 37513: JLE loc_1DB98
                // X right margin: ax += 0x1F
                ax += 0x1F;                                            // 37514
                if (cx < ax) {                                         // 37516: JGE → skip means cx < ax
                    s[di + 0x114D] = 2;                                // 37517: right clipping mode
                    // cs:word_1C830 = cs:[bx+0x138B] lookup — VGA only
                }
                // Y bottom bound: ax = viewport_Y + 0xB0
                ax = (int16_t)*(uint16_t*)(s + 0x46) + 0xB0;          // 37526-37527
                if (dx >= ax) goto type2_exit;                         // 37529: JGE loc_1DB98
                // Y top margin: ax -= 0x1F
                ax -= 0x1F;                                            // 37530
                if (dx >= ax) {                                        // 37532: JL → skip means dx >= ax
                    s[di + 0x114D] = 2;                                // 37533: top clipping mode
                    // cs:word_1C832 = dx - ax — VGA only
                }
                // Y top bound: ax -= 0xB0
                ax -= 0xB0;                                            // 37538
                if (dx < ax) goto type2_exit;                          // 37540: JL loc_1DB98
                // Y bottom margin: ax += 0x1F
                ax += 0x1F;                                            // 37541
                if (dx <= ax) {                                        // 37543: JG → skip means dx <= ax
                    s[di + 0x114D] = 2;                                // 37544: bottom clipping mode
                    // cs:word_1C834 = ax - dx — VGA only
                }
                // loc_1D95B: all bounds passed → sub_1CD7D + VGA render
                {
                    int16_t si_h = (int16_t)((*(uint16_t*)(s + di + 0x0C4D) >> 3) + 1); // 37550-37552
                    v2_sub_1CD7D(s, cx, dx, si_h, 5);                 // 37549: bp=5, 37553: call sub_1CD7D
                }
                // 37554-37776: VGA pixel rendering — all VGA OUT + REP MOVSB. No DS writes.
                // POP di, es, ds. RETN.
                goto type2_end;
            type2_exit:
                // loc_1DB98: sprite outside viewport → set mode=2, return.
                s[di + 0x114D] = 2;                                    // 37777: MOV byte [di+114Dh], 2
                // POP di, es, ds. RETN.
            type2_end:;

            } else if (handler == 0x0648) {
                // Type 1 (seg003_648_proc): 8x8 sprite. Verified seg003 lines 36433-36451.
                int16_t cx = (int16_t)*(uint16_t*)(s + di + 0x64D);
                int16_t dx = (int16_t)*(uint16_t*)(s + di + 0x74D);
                // cs:word_1C830=0xFFFF, cs:word_1C834=0, cs:word_1C832=0
                int16_t ax = (int16_t)*(uint16_t*)(s + 0x44) + 0x140; // 36436-36437
                if (cx >= ax) goto type1_exit;                         // 36439
                ax -= 0x147;                                           // 36440
                if (cx <= ax) goto type1_exit;                         // 36442
                ax = (int16_t)*(uint16_t*)(s + 0x46) + 0xB0;          // 36443-36444
                if (dx >= ax) goto type1_exit;                         // 36446
                ax -= 0xB7;                                            // 36447
                if (dx < ax) goto type1_exit;                          // 36449
                // In viewport → sub_1CD7B(si=2)
                v2_sub_1CD7D(s, cx, dx, 2, 2);                        // 36450-36451
                // VGA 8x8 render — commented for v2
                goto type1_end;
            type1_exit:
                // loc_1D154: sprite outside → set mode=2, return.
                s[di + 0x114D] = 2;                                    // 36722: MOV byte [di+114Dh], 2
                ;
            type1_end:;

            } else if (handler == 0x0B82) {
                // Type 4 (16x16 sprite). Exact replica of seg003 lines 36965-37271.
                int16_t cx = (int16_t)*(uint16_t*)(s + di + 0x64D);   // 36968
                int16_t dx = (int16_t)*(uint16_t*)(s + di + 0x74D);   // 36969
                // cs:word_1C830=0xFFFF, cs:word_1C834=0, cs:word_1C832=0 — VGA clipping init
                int16_t ax;
                // X right bound: ax = viewport_X + 0x140
                ax = (int16_t)*(uint16_t*)(s + 0x44) + 0x140;          // 36973-36974
                if (cx >= ax) goto type4_exit;                           // 36976: JGE loc_1D6B1
                // X right margin: ax -= 0x0F
                ax -= 0x0F;                                              // 36977
                if (cx < ax) goto type4_loc_1d3fd;                       // 36979: JL loc_1D3FD
                // Right clip path
                s[di + 0x114D] = 2;                                      // 36980: MOV byte [di+114Dh], 2
                // bx = cx-ax, shr 2, shl 1 → cs:[bx+0x0E92] → cs:word_1C830 — VGA clip only
            type4_loc_1d3fd:
                // X left bound
                ax -= 0x140;                                             // 36989: SUB ax, 140h
                if (cx <= ax) goto type4_exit;                           // 36991: JLE loc_1D6B1
                // X left margin: ax += 0x0F
                ax += 0x0F;                                              // 36992: ADD ax, 0Fh
                if (cx >= ax) goto type4_loc_1d425;                      // 36994: JGE loc_1D425
                // Left clip path
                s[di + 0x114D] = 2;                                      // 36995: MOV byte [di+114Dh], 2
                // bx = ax-cx, shr 2, shl 1 → cs:[bx+0x0E9A] → cs:word_1C830 — VGA clip only
            type4_loc_1d425:
                // Y bottom bound: ax = viewport_Y + 0x0B0
                ax = (int16_t)*(uint16_t*)(s + 0x46) + 0xB0;            // 37004-37005
                if (dx >= ax) goto type4_exit;                           // 37007: JGE loc_1D6B1
                // Y bottom margin: ax -= 0x0F
                ax -= 0x0F;                                              // 37008: SUB ax, 0Fh
                if (dx < ax) goto type4_loc_1d44c;                       // 37010: JL loc_1D44C
                // Bottom clip path
                s[di + 0x114D] = 2;                                      // 37011: MOV byte [di+114Dh], 2
                // cs:word_1C832 = dx-ax, shr 1 — VGA clip only
            type4_loc_1d44c:
                // Y top bound
                ax -= 0xB0;                                              // 37017: SUB ax, 0B0h
                if (dx < ax) goto type4_exit;                            // 37019: JL loc_1D6B1
                // Y top margin: ax += 0x0F
                ax += 0x0F;                                              // 37020: ADD ax, 0Fh
                if (dx > ax) goto type4_loc_1d471;                       // 37022: JG loc_1D471
                // Top clip path
                s[di + 0x114D] = 2;                                      // 37023: MOV byte [di+114Dh], 2
                // cs:word_1C834 = ax-dx, AND 0xFFFE — VGA clip only
            type4_loc_1d471:
                // All bounds passed → sub_1CD7B(si=3, bp=3)
                v2_sub_1CD7D(s, cx, dx, 3, 3);                          // 37029-37030
                // VGA 16x16 render — commented for v2
                goto type4_end;
            type4_exit:
                // loc_1D6B1: sprite outside → set mode=2, return.
                s[di + 0x114D] = 2;                                      // 37267: MOV byte [di+114Dh], 2
            type4_end:;

            } else if (handler != 0) {
                // Unknown non-zero handler — not implemented, fatal
                fprintf(stderr, "FATAL: unknown render handler 0x%04X for type %d slot %d flags=%04X\n",
                    handler, sprite_type, di, *(uint16_t*)(s + di + 0x44D));
                exit(1);
            }
            // handler=0 (types 0,3,5,6,7): NOP — no render, no DS writes
        }

        // Save current position as "last rendered"
        *(uint16_t*)(s + di + 0x0D4D) = *(uint16_t*)(s + di + 0x64D);
        *(uint16_t*)(s + di + 0x0E4D) = *(uint16_t*)(s + di + 0x74D);
    }
    // Clear force-render flag
    s[0x9568] = 0;
}

// sub_1DF6A: state change redraw — position copy for dirty objects.
// Part 1: for each object with byte [di+0x114E] != 0:
//   sub_1CD7D/sub_1CD7B: VGA dirty rect render — v2: skipped (full-frame render)
//   DS writes: [di+0x0F4D] ← [di+0x0D4D], [di+0x104D] ← [di+0x0E4D]
// Part 2: tile redraw via dirty page flags — VGA only, no DS side effects
//   OUT(0x3C4, 0x0F02); OUT(0x3CE, 0x0008); — VGA register setup, commented for v2
static void v2_sub_1DF6A(uint8_t* s) {
    // Part 1: position copy for dirty objects
    for (int16_t di = 0xFE; di >= 0; di -= 2) {                     // MOV di, 0FEh; loop
        if (s[di + 0x114E] == 0) continue;                          // TEST byte [di+114Eh], 0FFh; JZ
        uint16_t si = (*(uint16_t*)(s + di + 0x0C4D) >> 3) + 1;    // SHR si, 3; INC si
        // sub_1CD7D/sub_1CD7B: VGA dirty rect render + set fs dirty flags
        {
            int16_t cx_v = (int16_t)*(uint16_t*)(s + di + 0x0F4D); // old X
            int16_t dx_v = (int16_t)*(uint16_t*)(s + di + 0x104D); // old Y
            int16_t bp_v = ((int16_t)si > 5) ? 5 : (int16_t)si;   // bp=5 for large, si for small
            v2_sub_1CD7D(s, cx_v, dx_v, (int16_t)si, bp_v);        // set fs dirty + VGA render (commented)
        }
        // Position copy: "last rendered" ← "current" (both paths)
        *(uint16_t*)(s + di + 0x0F4D) = *(uint16_t*)(s + di + 0x0D4D);  // MOV [0F4Dh], cx
        *(uint16_t*)(s + di + 0x104D) = *(uint16_t*)(s + di + 0x0E4D);  // MOV [104Dh], dx
    }
    // Part 2: tile redraw from dirty page flags — VGA rendering only
    // OUT(0x3C4, 0x0F02);  // EGA sequencer: enable all planes — commented for v2
    // OUT(0x3CE, 0x0008);  // EGA graphics: bit mask — commented for v2
    // ... scan fs dirty flags, render tiles via sub_1CD7D — no DS side effects
}

// sub_1241e: write single glyph to glyph buffer.
// Original: PUSH si; PUSH di; SHL di,1; ADD si,[di-6CBA]; MOV [si-6A94],al; POP di; POP si; INC si; RETN
// Input: al = character, si = column, di = row.
// Writes: ds:[(uint16_t)(si + ds:[di*2 + 0x9346] + 0x956C)] = al, then si++
static void v2_sub_1241e(uint8_t* s, uint8_t ch, uint16_t& si_col, uint16_t di_row) {
    uint16_t di2 = di_row << 1;                                      // SHL di, 1
    uint16_t row_off = *(uint16_t*)(s + (uint16_t)(di2 - 0x6CBA));   // [di-6CBAh]
    uint16_t addr = (uint16_t)(si_col + row_off - 0x6A94);           // ADD si,[...]; [si-6A94h]
    s[addr] = ch;                                                     // MOV [si-6A94h], al
    si_col++;                                                         // INC si (after POP restores original)
}

// sub_12515: lookup text string pointer from seg001 index table.
// Original: SHL ax,1; MOV si,ax; es=seg001; word_2850A = es:[si]; RETN
// Input: ax = text index. Writes: ds:0x2A (word_2850A).
static void v2_sub_12515(uint8_t* s, uint16_t ax) {
    if (!v2_m2c_base) return;
    uint16_t si = ax << 1;                                           // SHL ax, 1
    uint8_t* seg001 = v2_m2c_base + 0x9480;
    uint16_t text_off = *(uint16_t*)(seg001 + si);                   // es:[si+0]
    *(uint16_t*)(s + 0x2A) = text_off;                               // MOV word_2850A, ax
}

// sub_12529: read text dimensions from seg001.
// Original: es=seg001; ah=0; word_28514=es:[bx]; word_28516=es:[bx+1]; bx+=2; RETN
// Input: bx = offset in seg001. Writes: ds:0x34, ds:0x36. Returns: bx advanced by 2.
static void v2_sub_12529(uint8_t* s, uint16_t& bx) {
    if (!v2_m2c_base) return;
    uint8_t* seg001 = v2_m2c_base + 0x9480;
    uint16_t width = (uint16_t)seg001[bx];                           // ah=0; al=es:[bx]
    *(uint16_t*)(s + 0x34) = width;                                  // word_28514
    uint16_t height = (uint16_t)seg001[bx + 1];                     // al=es:[bx+1]
    *(uint16_t*)(s + 0x36) = height;                                 // word_28516
    bx += 2;                                                         // ADD bx, 2
}

// sub_12549: set text alignment offsets based on parameter.
// Input: ax = alignment type. Reads: ds:0x34 (width), ds:0x36 (height).
// Writes: ds:0x38 (word_28518), ds:0x3A (word_2851A).
static void v2_sub_12549(uint8_t* s, uint16_t ax) {
    uint16_t width = *(uint16_t*)(s + 0x34);                         // word_28514
    uint16_t height = *(uint16_t*)(s + 0x36);                       // word_28516
    uint16_t x_off, y_off;
    if (ax == 5) {
        // loc_1256f → loc_1257a: x = width-2, y = 0
        x_off = width - 2; y_off = 0;
    } else if (ax == 4) {
        // loc_12577 → loc_1257a: x = 1, y = 0
        x_off = 1; y_off = 0;
    } else if (ax == 1) {
        // loc_1258c → loc_12597: x = 1, y = height-1
        x_off = 1; y_off = height - 1;
    } else if (ax == 2) {
        // loc_12591 → loc_12597: x = width-2, y = height-1
        x_off = width - 2; y_off = height - 1;
    } else if (ax == 0 || ax == 6) {
        // loc_12585 → loc_12597: x = width/2, y = height-1
        x_off = width >> 1; y_off = height - 1;
    } else {
        // default → loc_1257a: x = width/2, y = 0
        x_off = width >> 1; y_off = 0;
    }
    *(uint16_t*)(s + 0x38) = x_off;                                  // word_28518
    *(uint16_t*)(s + 0x3A) = y_off;                                  // word_2851A
}

// sub_12388: draw text box frame to glyph buffer.
// Exact replica of original: border chars 0x12-0x19, interior 0x20, scroll indicator 0x1A/0x1B.
// Input: si = column, di = row, ax (on stack) = alignment param (passed as 'align').
// Uses: word_28514 (width), word_28516 (height), word_28518, word_2851A.
// Writes: word_2854C, word_2854E, glyph buffer entries.
static void v2_sub_12388(uint8_t* s, uint16_t si, uint16_t di, uint8_t align) {
    *(uint16_t*)(s + 0x6C) = si;                                     // word_2854C = si
    *(uint16_t*)(s + 0x6E) = di;                                     // word_2854E = di
    uint16_t width = *(uint16_t*)(s + 0x34);                         // word_28514
    uint16_t height = *(uint16_t*)(s + 0x36);                       // word_28516

    // Top row: left corner + middle + right corner
    v2_sub_1241e(s, 0x12, si, di);                                   // top-left
    { uint16_t cx = width - 2;
      for (uint16_t i = 0; i < cx; i++)
          v2_sub_1241e(s, 0x13, si, di); }                          // top-middle × (width-2)
    v2_sub_1241e(s, 0x14, si, di);                                   // top-right

    // Reset column, next row
    si = *(uint16_t*)(s + 0x6C);                                     // si = word_2854C
    di++;                                                             // INC di

    // Middle rows: left border + spaces + right border
    { uint16_t cx = height - 2;
      for (uint16_t r = 0; r < cx; r++) {
          v2_sub_1241e(s, 0x15, si, di);                             // left border
          { uint16_t dx = width - 2;
            for (uint16_t c = 0; c < dx; c++)
                v2_sub_1241e(s, 0x20, si, di); }                    // interior spaces
          v2_sub_1241e(s, 0x16, si, di);                             // right border (ax=0x16)
          si = *(uint16_t*)(s + 0x6C);                               // reset column
          di++;                                                       // next row
      }
    }

    // Bottom row: left corner + middle + right corner
    v2_sub_1241e(s, 0x17, si, di);                                   // bottom-left
    { uint16_t cx = width - 2;
      for (uint16_t i = 0; i < cx; i++)
          v2_sub_1241e(s, 0x18, si, di); }                          // bottom-middle
    v2_sub_1241e(s, 0x19, si, di);                                   // bottom-right

    // Scroll indicator (if alignment != 6)
    if (align != 6) {                                                 // CMP al, 6; JZ loc_12415
        uint16_t ind_di = *(uint16_t*)(s + 0x6E) + *(uint16_t*)(s + 0x3A); // word_2854E + word_2851A
        uint16_t ind_si = *(uint16_t*)(s + 0x6C) + *(uint16_t*)(s + 0x38); // word_2854C + word_28518
        uint8_t ind_ch;
        if (*(uint16_t*)(s + 0x3A) != 0) {                          // CMP word_2851A, 0
            ind_ch = 0x1A;                                           // scroll indicator right
        } else {
            ind_ch = 0x1B;                                           // scroll indicator left
        }
        v2_sub_1241e(s, ind_ch, ind_si, ind_di);                    // call sub_1241e
    }

    // Restore original si, di
    si = *(uint16_t*)(s + 0x6C);                                     // si = word_2854C
    di = *(uint16_t*)(s + 0x6E);                                     // di = word_2854E
    // (si, di returned via caller's variables; original returns in registers)
}

// loc_124c5: render text from seg001 to glyph buffer.
// Original: reads bytes from es:bx (seg001), writes glyphs via sub_1241e.
// Input: si = start column, di = start row, bx = offset in seg001 (past width/height).
// Uses: word_28514 (width). Writes: word_2854C, glyph buffer.
static void v2_loc_124c5(uint8_t* s, uint16_t si, uint16_t di, uint16_t bx) {
    if (!v2_m2c_base) return;
    uint8_t* seg001 = v2_m2c_base + 0x9480;
    *(uint16_t*)(s + 0x6C) = si;                                     // MOV word_2854C, si
    uint16_t cx = *(uint16_t*)(s + 0x34) - 2;                       // MOV cx, word_28514; SUB cx, 2

    while (true) {                                                    // loc_124d8
        uint8_t al = seg001[bx];                                     // MOV al, es:[bx]
        if (al == 0) break;                                          // CMP al, 0; JZ loc_12509
        if (al == 0x0D) {                                            // CMP al, 0Dh; JNZ loc_12502
            // CR: fill rest of line with spaces
            if (cx != 0) {                                           // JCXZ loc_124ec
                while (cx > 0) {                                     // loc_124e5
                    v2_sub_1241e(s, 0x20, si, di);                   // call sub_1241e(0x20)
                    cx--;                                             // LOOP
                }
            }
            // Next line
            cx = *(uint16_t*)(s + 0x34) - 2;                        // MOV cx, word_28514; SUB cx, 2
            di++;                                                     // INC di
            si = *(uint16_t*)(s + 0x6C);                             // MOV si, word_2854C
            al = seg001[bx];                                          // MOV al, es:[bx]
            if (al == 0) break;                                      // CMP al, 0; JZ loc_12509
            bx++;                                                     // INC bx
            continue;                                                 // JMP loc_124d8
        }
        // Normal character
        v2_sub_1241e(s, al, si, di);                                 // loc_12502: call sub_1241e
        bx++;                                                         // INC bx
        cx--;                                                         // DEC cx
    }
}

// loc_124A9: full text rendering with dimensions lookup.
// Original flow: sub_12515 → sub_12529 → sub_12388 → loc_124c5.
// Input: si = start column, di = start row. Uses ds:0x2A (text index set by caller).
static void v2_loc_124A9(uint8_t* s, uint16_t si, uint16_t di) {
    if (!v2_m2c_base) return;
    // sub_12515 already called by caller (set word_2850A = ds:0x2A)
    uint16_t bx = *(uint16_t*)(s + 0x2A);                           // MOV bx, word_2850A
    // sub_12529: read dimensions
    v2_sub_12529(s, bx);                                              // call sub_12529 (bx += 2)
    // sub_12388: draw text box
    // Original pushes si, di before call, pops after, then INC di; INC si
    uint16_t save_si = si, save_di = di;
    v2_sub_12388(s, si, di, 0);                                      // ax on stack = 0 (from sub_12549 not called here — use 0 as default)
    di = save_di + 1;                                                 // POP di; INC di
    si = save_si + 1;                                                 // POP si; INC si
    // loc_124c5: render text
    v2_loc_124c5(s, si, di, bx);
}

// sub_111a1: clear sprite table — 0xE00 words at ds:0x44D
static void v2_sub_111a1(uint8_t* s) {
    memset(s + 0x44D, 0, 0xE00 * 2);
}

// sub_11192: clear bit flags — 16 bytes at ds:0x356
static void v2_sub_11192(uint8_t* s) {
    for (int i = 0; i < 0x10; i++) s[0x356 + i] = 0;
}

// sub_111df: clear viking state bytes
static void v2_sub_111df(uint8_t* s) {
    s[0x0342] = 0; // byte_28822
    s[0x0343] = 0; // byte_28823
    s[0x0344] = 0; // byte_28824
    s[0x0345] = 0; // byte_28825
    s[0x0346] = 0; // byte_28826
    s[0x0347] = 0; // byte_28827
    *(uint16_t*)(s + 0x0348) = 0; // word_28828
}

// sub_12816: clear UI glyph list — byte_31A4B=0, clear 0x1B8 words at ds:0x956C
static void v2_sub_12816(uint8_t* s) {
    s[0x956B] = 0; // byte_31A4B
    memset(s + 0x956C, 0, 0x1B8 * 2); // REP STOSW
}

// sub_1133a: init HUD from spawn table extension.
// Reads viking type + item data for each viking from spawn table.
static uint16_t v2_sub_1133a(uint8_t* s, uint16_t di) {
    uint16_t ax = *(uint16_t*)(s + di + 0x25F6);
    s[0x2583] = (uint8_t)ax; // byte_2AA63
    di += 2;
    for (uint16_t si = 0; ; si++) {
        ax = *(uint16_t*)(s + di + 0x25F6) & 0xFF;
        if (ax == 0) { di++; break; }
        s[si + 0x2584] = (uint8_t)ax;
        s[si + 0x258C] = (uint8_t)ax;
        s[si + 0x2594] = s[di + 0x25F7];
        s[si + 0x259C] = s[di + 0x25F8];
        di += 3;
        // Skip sub-entries until 0xFFFF
        while (*(uint16_t*)(s + di + 0x25F6) != 0xFFFF) di += 2;
        di += 2;
    }
    return di;
}

// sub_137f1: init velocity tables — clear all object velocity fields
// sub_137f1: clear object table slots — exact replica.
// si=0..0x26 (20 objects, fixed range 0x28): [si+0x1355]=0, [si+0x1A0D]=0xFFFF
static void v2_sub_137f1(uint8_t* s) {
    for (uint16_t si = 0; si != 0x28; si += 2) {
        *(uint16_t*)(s + si + 0x1355) = 0;      // code_seg = 0 (inactive)
        *(uint16_t*)(s + si + 0x1A0D) = 0xFFFF;  // parent link = none
    }
}

// sub_12fb3: clear sprite resource table (ds:0x12AD and ds:0x12ED)
static void v2_sub_12fb3(uint8_t* s) {
    for (uint16_t si = 0; si < 0x100; si += 2) {
        *(uint16_t*)(s + si + 0x44D) = 0; // clear sprite flags
    }
}

// sub_113b0: init scroll limits from map dimensions (word_2AABC/word_2AABE = ds:0x25DC/0x25DE).
static void v2_sub_113b0(uint8_t* s) {
    uint16_t width = *(uint16_t*)(s + 0x25DC);   // word_2AABC (map width in tiles)
    *(uint16_t*)(s + 0x9168) = width * 2;         // word_31648
    *(uint16_t*)(s + 0x25A4) = width * 16 - 0x140; // word_2AA84 (scroll X limit)
    uint16_t height = *(uint16_t*)(s + 0x25DE);   // word_2AABE (map height in tiles)
    *(uint16_t*)(s + 0x916A) = height * 2;        // word_3164A
    *(uint16_t*)(s + 0x25A6) = height * 16 - 0xB0; // word_2AA86 (scroll Y limit)
    // jmp loc_16595: build row lookup table at ds:0x8F68 (256 entries).
    // ax = ds:0x25DC (from above), shl 1 = row stride in tilemap words.
    // ds:[0x8F68 + i*2] = i * stride for i=0..255.
    {
        uint16_t stride = *(uint16_t*)(s + 0x25DC) * 2; // ax was already ds:0x25DC, shl 1
        uint16_t dx_val = 0;
        for (uint16_t i = 0; i < 0x100; i++) {
            *(uint16_t*)(s + 0x8F68 + i * 2) = dx_val;
            dx_val += stride;
        }
    }
}

// sub_113d8: init viewport + scroll from active viking position.
// Centers viewport on viking, clamps to scroll limits.
static void v2_sub_113d8(uint8_t* s) {
    uint16_t si = 0;
    if (s[0x25BA] != 0) si = *(uint16_t*)(s + 0x03C2); // byte_2AA9A, word_288A2

    // X: center on viking, clamp to [0, scroll_X_limit]
    int16_t ax = (int16_t)*(uint16_t*)(s + si + 0x173D) - 0xA0;
    if (ax < 0) ax = 0;
    if (ax > (int16_t)*(uint16_t*)(s + 0x25A4)) ax = (int16_t)*(uint16_t*)(s + 0x25A4);
    *(uint16_t*)(s + 0x0044) = (uint16_t)ax;   // word_28524 (viewport X)
    *(uint16_t*)(s + 0x257B) = (uint16_t)ax;   // word_2AA5B
    uint16_t scroll_x = (uint16_t)ax >> 3;
    *(uint16_t*)(s + 0x257F) = scroll_x;        // word_2AA5F
    *(uint16_t*)(s + 0x92EF) = scroll_x;        // word_317CF
    *(uint16_t*)(s + 0x92F3) = scroll_x >> 1;   // word_317D3

    // Y: center on viking, clamp to [0, scroll_Y_limit]
    ax = (int16_t)*(uint16_t*)(s + si + 0x1765) - 0x58;
    if (ax < 0) ax = 0;
    if (ax > (int16_t)*(uint16_t*)(s + 0x25A6)) ax = (int16_t)*(uint16_t*)(s + 0x25A6);
    *(uint16_t*)(s + 0x0046) = (uint16_t)ax;   // word_28526 (viewport Y)
    *(uint16_t*)(s + 0x257D) = (uint16_t)ax;   // word_2AA5D
    uint16_t scroll_y = (uint16_t)ax >> 3;
    *(uint16_t*)(s + 0x2581) = scroll_y;        // word_2AA61
    *(uint16_t*)(s + 0x92F1) = scroll_y;        // word_317D1
    *(uint16_t*)(s + 0x92F5) = scroll_y >> 1;   // word_317D5
}

// sub_173c7: build FS (render tilemap) from ES (game tilemap) + GS (tile graphics).
// For chunk/intro levels (flag 0x42): clear FS.
// For tile levels: nested loop height × width, copies tile data to FS with VGA page layout.
// Also copies GS row data to DS at ds:0x2E65 offset.
static void v2_sub_173c7(uint8_t* s) {
    if (s[0x25CF] & 0x42) {
        // loc_1744f: chunk/intro level — clear FS segment
        // es = fs = ds:0x2E69, clear 0x3020 dwords = 0xC080 bytes
        memset(v2_vm_shadow_fs, 0, 0x3020 * 4);
        v2_fs_shadow_valid = true;
        return;
    }
    // Normal tile level: build FS from ES (tilemap) + GS (tile masks)
    uint16_t cols = *(uint16_t*)(s + 0x25DC);  // map width in tiles
    printf("V2-173C7: cols=%d rows=%d bp=%d flag=%02X\n", cols, *(uint16_t*)(s + 0x25DE), cols*4, s[0x25CF]);
    uint16_t rows = *(uint16_t*)(s + 0x25DE);  // map height in tiles
    uint16_t bp = cols * 4;  // FS row stride (4 bytes per tile in FS)
    uint8_t* es = v2_vm_shadow_tilemap;       // tilemap (ES = ds:0x2E63)
    uint8_t* fs = v2_vm_shadow_fs;            // render tilemap (FS = ds:0x2E69)
    uint8_t* gs = v2_vm_shadow_gs_tiledata;   // tile data (GS = ds:0x2E5D, NOT ds:0x2E61!)
    uint16_t bx = 0;  // offset in tilemap
    uint16_t di = 0;  // offset in FS
    uint16_t row_counter = rows;
    while (row_counter > 0) {
        uint16_t col_counter = cols;
        while (col_counter > 0) {
            // Read tile index from tilemap, mask to 10 bits, multiply by 8
            uint16_t si = *(uint16_t*)(es + bx) & 0x3FF;
            si <<= 3;  // 8 bytes per GS entry
            // Copy 4 words from GS to FS (2 rows × 2 words)
            *(uint16_t*)(fs + di)     = *(uint16_t*)(gs + si);
            *(uint16_t*)(fs + di + 2) = *(uint16_t*)(gs + si + 2);
            *(uint16_t*)(fs + (uint16_t)(bp + di))     = *(uint16_t*)(gs + si + 4);
            *(uint16_t*)(fs + (uint16_t)(bp + di + 2)) = *(uint16_t*)(gs + si + 6);
            bx += 2;
            di += 4;
            col_counter--;
        }
        // Next row pair: di += bp (second half of row)
        di += bp;
        row_counter--;
    }
    v2_fs_shadow_valid = true;
    // GS→ES copy: rep movsw, cx=0x78 (240 bytes)
    // Source: GS (ds:0x2E5D) at offset 0
    // Dest: ES (tilemap ds:0x2E63) at offset ds:0x2E65 (= decompressed tilemap size)
    uint16_t di_dest = *(uint16_t*)(s + 0x2E65);
    for (uint16_t i = 0; i < 0x78; i++) {
        *(uint16_t*)(es + di_dest + i * 2) = *(uint16_t*)(gs + i * 2);
    }
}

// sub_11439: init word_3287C rendering flag
// sub_11439: init render flag + first page flip.
// if !(byte_2AAAF & 0x42): call sub_16ded (VGA tile row rendering)
// then jmp sub_16775 (page flip — tail call)
static void v2_sub_11439(uint8_t* s) {
    if (!(s[0x25CF] & 0x42)) {
        // sub_16ded: exact DS writes from VGA tile row init.
        // VGA rendering (sub_16dc1 + sub_171dc) skipped — v2 renders each frame.
        int16_t di_y = (int16_t)*(uint16_t*)(s + 0x2581) - 1;
        if (di_y < 0) di_y = 0;
        uint16_t di2 = (uint16_t)di_y << 1;
        uint16_t bx = *(uint16_t*)(s + (uint16_t)(di2 - 0x7098)); // row LUT
        *(uint16_t*)(s + 0x9305) = di2 + *(uint16_t*)(s + 0x92F9); // page 2
        *(uint16_t*)(s + 0x9307) = di2 + *(uint16_t*)(s + 0x92FB); // page 3
        *(uint16_t*)(s + 0x9309) = di2 + *(uint16_t*)(s + 0x92F7); // page 1
        int16_t dx_x = (int16_t)*(uint16_t*)(s + 0x257F) - 1;
        if (dx_x < 0) dx_x = 0;
        bx += (uint16_t)dx_x;
        bx <<= 1;
        uint16_t dx2 = ((uint16_t)dx_x << 1) + 8;
        // Loop 25 rows — only DS state updates (skip VGA sub_16dc1/sub_171dc)
        for (int cx = 0; cx < 25; cx++) {
            uint16_t p2 = *(uint16_t*)(s + 0x9305);
            *(uint16_t*)(s + 0x930D) = *(uint16_t*)(s + (uint16_t)(p2 - 0x7608)) + dx2;
            uint16_t p3 = *(uint16_t*)(s + 0x9307);
            *(uint16_t*)(s + 0x930F) = *(uint16_t*)(s + (uint16_t)(p3 - 0x7608)) + dx2;
            uint16_t p1 = *(uint16_t*)(s + 0x9309);
            *(uint16_t*)(s + 0x930B) = *(uint16_t*)(s + (uint16_t)(p1 - 0x7608)) + dx2;
            v2_sub_16dc1(s, bx, 1);     // sub_16dc1: VGA column tile render (1 tile)
            v2_sub_171dc(s);             // sub_171dc: VGA page copy
            bx += *(uint16_t*)(s + 0x8F6C);
            *(uint16_t*)(s + 0x9305) += 2;
            *(uint16_t*)(s + 0x9307) += 2;
            *(uint16_t*)(s + 0x9309) += 2;
        }
    }
    // jmp sub_16775: page flip (tail call)
    v2_sub_16775(s);
}

// sub_13ba5: init permanent objects from spawn table (flag 0x800 in ds:[di+0x2600]).
// Creates all objects marked as permanent, regardless of viewport position.
static void v2_sub_13ba5(uint8_t* s) {
    // Exact replica of original sub_13ba5 + sub_13bbd.
    // Iterates spawn table, creates objects with flag 0x800 (permanent).
    *(uint16_t*)(s + 0x42) = 0xFFFF;
    for (uint16_t si = 0, di = 0; ; si++, di += 0x0E) {
        uint16_t spawn_x = *(uint16_t*)(s + di + 0x25F6);
        if (spawn_x == 0xFFFF) break; // end of table → STC
        if (!(*(uint16_t*)(s + di + 0x2600) & 0x800)) continue; // not permanent → CLC
        // sub_13bbd: setup params and call sub_13809
        *(uint16_t*)(s + 0x32) = si;
        *(uint16_t*)(s + 0x6C) = spawn_x;
        *(uint16_t*)(s + 0x6E) = *(uint16_t*)(s + di + 0x25F8);
        *(uint16_t*)(s + 0x3E0) = *(uint16_t*)(s + di + 0x25FA);
        *(uint16_t*)(s + 0x3E2) = *(uint16_t*)(s + di + 0x25FC);
        *(uint16_t*)(s + 0x374) = *(uint16_t*)(s + di + 0x2602);
        uint16_t code_seg_idx = *(uint16_t*)(s + di + 0x25FE);
        uint16_t anim_idx = *(uint16_t*)(s + di + 0x2600);
        // Original: ax=[di+25FE], si=[di+2600], di=ds:32 (saved spawn index)
        v2_sub_13809(s, code_seg_idx, si, anim_idx,
                     *(uint16_t*)(s + 0x6C), *(uint16_t*)(s + 0x6E));
    }
}

// sub_13c0c: set viewport despawn bounds + mark out-of-bounds objects.
// Sets ds:0x34/0x36/0x38/0x3A = clamped viewport bounds.
// Iterates objects si=6..table_end, sets flag 0x200 for those outside bounds.
// Called from POST_FLIP2 pass and sub_115d2 init.
static void v2_sub_13c0c(uint8_t* s) {
    // Set X bounds: ds:0x34 = max(vp_x - 0x10, 0); ds:0x36 = ds:0x34 + 0x160
    int16_t ax = (int16_t)*(uint16_t*)(s + 0x44) - 0x10;
    if (ax < 0) ax = 0;
    *(uint16_t*)(s + 0x34) = (uint16_t)ax;
    ax += 0x160;
    *(uint16_t*)(s + 0x36) = (uint16_t)ax;
    // Set Y bounds: ds:0x38 = max(vp_y - 0x10, 0); ds:0x3A = ds:0x38 + 0xD0
    ax = (int16_t)*(uint16_t*)(s + 0x46) - 0x10;
    if (ax < 0) ax = 0;
    *(uint16_t*)(s + 0x38) = (uint16_t)ax;
    *(uint16_t*)(s + 0x3A) = (uint16_t)ax + 0xD0;
    // Despawn loop: si=6..table_end
    uint16_t te = *(uint16_t*)(s + 0x372);
    for (uint16_t si = 6; (int16_t)si < (int16_t)te; si += 2) {
        if (*(uint16_t*)(s + si + 0x1355) == 0) continue;           // dead slot
        if (*(uint16_t*)(s + si + 0x1585) & 0x800) continue;        // permanent object
        bool outside = false;
        // X bounds check
        if ((int16_t)(*(uint16_t*)(s+si+0x173D) + *(uint16_t*)(s+si+0x14BD) - *(uint16_t*)(s+0x34)) < 0) outside = true;
        else if ((int16_t)(*(uint16_t*)(s+si+0x173D) - *(uint16_t*)(s+si+0x14BD) - *(uint16_t*)(s+0x36)) >= 0) outside = true;
        // Y bounds check
        else if ((int16_t)(*(uint16_t*)(s+si+0x1765) + *(uint16_t*)(s+si+0x1495) - *(uint16_t*)(s+0x38)) < 0) outside = true;
        else if ((int16_t)(*(uint16_t*)(s+si+0x1765) - *(uint16_t*)(s+si+0x1495) - *(uint16_t*)(s+0x3A)) >= 0) outside = true;
        if (outside) *(uint16_t*)(s + si + 0x1585) |= 0x200;        // mark for despawn
    }
}

// loc_13a94: clamp bounds + spawn loop. Exact replica of original loc_13a94 → sub_13ae0.
// Called from sub_13a14/sub_13a34/loc_13a54/loc_13a74 (scroll spawn) and sub_13a0e (init spawn).
// Bounds (ds:0x34/0x36/0x38/0x3A) must be set by caller before calling.
static void v2_loc_13a94(uint8_t* s) {
    if ((int16_t)*(uint16_t*)(s + 0x34) < 0) *(uint16_t*)(s + 0x34) = 0;
    if ((int16_t)*(uint16_t*)(s + 0x36) < 0) *(uint16_t*)(s + 0x36) = 0;
    if ((int16_t)*(uint16_t*)(s + 0x38) < 0) *(uint16_t*)(s + 0x38) = 0;
    if ((int16_t)*(uint16_t*)(s + 0x3A) < 0) *(uint16_t*)(s + 0x3A) = 0;
    *(uint16_t*)(s + 0x42) = 0xFFFF;
    for (uint16_t si_idx = 0, di_off = 0; ; si_idx++, di_off += 0x0E) {
        uint16_t sx = *(uint16_t*)(s + di_off + 0x25F6);
        if (sx == 0xFFFF) break;
        uint16_t hw = *(uint16_t*)(s + di_off + 0x25FA);
        if ((int16_t)(sx + hw - *(uint16_t*)(s + 0x34)) < 0) continue;
        if ((int16_t)(sx - hw - *(uint16_t*)(s + 0x36)) >= 0) continue;
        uint16_t sy = *(uint16_t*)(s + di_off + 0x25F8);
        uint16_t hh = *(uint16_t*)(s + di_off + 0x25FC);
        if ((int16_t)(sy + hh - *(uint16_t*)(s + 0x38)) < 0) continue;
        if ((int16_t)(sy - hh - *(uint16_t*)(s + 0x3A)) >= 0) continue;
        // In viewport — check if already spawned
        *(uint16_t*)(s + 0x32) = si_idx;
        bool already = false;
        uint16_t table_end = *(uint16_t*)(s + 0x372);
        for (uint16_t si2 = *(uint16_t*)(s + 0x33C); (int16_t)si2 < (int16_t)table_end; si2 += 2) {
            if (*(uint16_t*)(s + si2 + 0x1355) == 0) continue;
            if (*(uint16_t*)(s + si2 + 0x16C5) == si_idx) { already = true; break; }
        }
        if (already) continue;
        // sub_13ae0: save viewport bounds, setup params, call sub_13809, restore bounds
        uint16_t save_34 = *(uint16_t*)(s + 0x34);
        uint16_t save_36 = *(uint16_t*)(s + 0x36);
        uint16_t save_38 = *(uint16_t*)(s + 0x38);
        uint16_t save_3A = *(uint16_t*)(s + 0x3A);
        *(uint16_t*)(s + 0x32) = si_idx;
        *(uint16_t*)(s + 0x6C) = sx;
        *(uint16_t*)(s + 0x6E) = sy;
        *(uint16_t*)(s + 0x3E0) = hw;
        *(uint16_t*)(s + 0x3E2) = hh;
        *(uint16_t*)(s + 0x374) = *(uint16_t*)(s + di_off + 0x2602);
        uint16_t code_seg_idx = *(uint16_t*)(s + di_off + 0x25FE);
        uint16_t anim_idx = *(uint16_t*)(s + di_off + 0x2600);
        v2_sub_13809(s, code_seg_idx, si_idx, anim_idx, sx, sy);
        *(uint16_t*)(s + 0x3A) = save_3A;
        *(uint16_t*)(s + 0x38) = save_38;
        *(uint16_t*)(s + 0x36) = save_36;
        *(uint16_t*)(s + 0x34) = save_34;
    }
}

// sub_13a0e: initial viewport scan — called from sub_11080 level init.
// sub_139ef sets full viewport bounds, then loc_13a94 spawns all objects in view.
static void v2_sub_13a0e(uint8_t* s) {
    // sub_139ef: full viewport bounds
    uint16_t ax = *(uint16_t*)(s + 0x44) - 0x10;
    *(uint16_t*)(s + 0x34) = ax;
    ax += 0x160;
    *(uint16_t*)(s + 0x36) = ax;
    ax = *(uint16_t*)(s + 0x46) - 0x10;
    *(uint16_t*)(s + 0x38) = ax;
    ax += 0xD0;
    *(uint16_t*)(s + 0x3A) = ax;
    v2_loc_13a94(s);
}

// sub_1167a: sprite resource loading from level data.
// Reads chunk IDs from spawn table, decompresses into sprite segment, fills resource table.
static uint16_t v2_sub_1167a(uint8_t* s, uint16_t di) {
    // Init sprite shadow base from current sprite segment.
    uint16_t spr_seg = *(uint16_t*)(s + 0x2E73);
    static uint16_t prev_spr_seg = 0;
    if (spr_seg && spr_seg != prev_spr_seg) {
        printf("V2-SPRSEG: sprite segment changed 0x%04X → 0x%04X\n", prev_spr_seg, spr_seg);
        prev_spr_seg = spr_seg;
    }
    if (spr_seg) {
        v2_sprite_shadow_base = (uint32_t)spr_seg << 4;
        v2_sprite_shadow_active = true;
    }
    uint16_t si = 0;
    uint16_t bx = 0; // offset within sprite segment
    while (true) {
        uint16_t chunk_id = *(uint16_t*)(s + di + 0x25F6);
        if (chunk_id == 0xFFFF) { di += 2; break; }
        // Store chunk_id in resource table
        *(uint16_t*)(s + si + 0x12AD) = chunk_id;
        // Store sprite base (bx + 1) in resource table
        *(uint16_t*)(s + si + 0x12ED) = bx + 1;
        // Decompress chunk into sprite shadow buffer at offset bx.
        // Original: es = word_2B353 (ds:0x2E73 = sprite segment), di = bx.
        // For v2: decompress into v2_sprite_shadow linear buffer.
        uint32_t dest_sz = v2_read_chunk(chunk_id, v2_sprite_shadow + bx, V2_SPRITE_SHADOW_SIZE - bx);
        printf("V2-SPRINIT: chunk=%d bx=%04x decompressed=%d first4=%02x%02x%02x%02x\n",
               chunk_id, bx, dest_sz,
               v2_sprite_shadow[bx], v2_sprite_shadow[bx+1],
               v2_sprite_shadow[bx+2], v2_sprite_shadow[bx+3]);
        bx += (uint16_t)dest_sz;
        di += 6;
        si += 2;
    }
    return di;
}

// sub_116ae: animation bytecode loading from level data.
// Decompresses animation chunks into CHUNK BUFFER (ds:0x2E77), NOT animation data (ds:0x2E67).
// dword_2B359 (ds:0x2E79:0x2E7B) tracks write position as far pointer within chunk buffer.
// Stores chunk table at ds:0x124D, buffer addresses at ds:0x126D/0x128D.
static uint16_t v2_sub_116ae(uint8_t* s, uint16_t di) {
    uint16_t si = 0;
    // Chunk buffer base segment for linear offset calculation
    uint16_t chunk_base_seg = *(uint16_t*)(s + 0x2E77);
    while (true) {
        uint16_t chunk_id = *(uint16_t*)(s + di + 0x25F6);
        if (chunk_id == 0xFFFF) break;
        *(uint16_t*)(s + si + 0x124D) = chunk_id;
        // les di, dword_2B359: load es:di from animation buffer pointer
        uint16_t buf_di = *(uint16_t*)(s + 0x2E79);  // offset
        uint16_t buf_es = *(uint16_t*)(s + 0x2E7B);  // segment
        *(uint16_t*)(s + si + 0x126D) = buf_di;       // store buffer offset
        *(uint16_t*)(s + si + 0x128D) = buf_es;       // store buffer segment
        // Decompress into chunk buffer shadow at linear offset from chunk buffer base
        uint32_t linear = ((uint32_t)buf_es << 4) + buf_di;
        uint32_t chunk_base = (uint32_t)chunk_base_seg << 4;
        uint32_t buf_offset = linear - chunk_base;
        if (buf_offset < V2_CHUNK_SHADOW_SIZE) {
            uint32_t sz = v2_read_chunk(chunk_id, v2_vm_shadow_chunk + buf_offset,
                                        V2_CHUNK_SHADOW_SIZE - buf_offset);
            // sub_10E85: normalize es:di past decompressed data
            // es = es + (di >> 4) + 1, di = 0
            uint16_t di_after = (uint16_t)(buf_di + (uint16_t)sz);
            uint16_t new_es = buf_es + (di_after >> 4) + 1;
            *(uint16_t*)(s + 0x2E7B) = new_es;  // segment
            *(uint16_t*)(s + 0x2E79) = 0;        // offset = 0
        }
        di += 5;
        si += 2;
    }
    return di + 2; // skip 0xFFFF
}

// sub_11397: init collision type lookup from level table.
// Does NOT use di passthrough — reads from word_2AAAB (ds:0x25CB).
static void v2_sub_11397(uint8_t* s) {
    uint16_t di_val = *(uint16_t*)(s + 0x25CB); // word_2AAAB
    *(uint16_t*)(s + 0x03A6) = *(uint16_t*)(s + (uint16_t)(di_val - 0x7AC2)) & 0xFF; // word_28886
    *(uint16_t*)(s + 0x03A8) = *(uint16_t*)(s + (uint16_t)(di_val - 0x7ABC)) & 0xFF; // word_28888
}

// sub_11446: game mode init. NOT just sound — creates game objects based on mode.
// byte_2AA9A (ds:0x25BA) = mode. Mode 0 = normal gameplay.
// Does NOT consume di — reads from fixed DS addresses.
// sub_11569: creates 3 viking objects from position table at ds:[di_table].
// Table: 3 entries × (X word, Y word, anim_flags word) = 18 bytes.
// Code_seg indices: 1 (first), 0 (second), 2 (third).
static void v2_sub_11569(uint8_t* s, uint16_t di_table) {
    // First viking (code_seg = 1)
    *(uint16_t*)(s + 0x6C) = *(uint16_t*)(s + di_table);
    *(uint16_t*)(s + 0x6E) = *(uint16_t*)(s + di_table + 2);
    uint16_t si_anim = *(uint16_t*)(s + di_table + 4) | *(uint16_t*)(s + 0x25C1);
    *(uint16_t*)(s + 0x42) = 0xFFFF;
    v2_sub_13809(s, 1, 0xFFFF, si_anim,
                 *(uint16_t*)(s + 0x6C), *(uint16_t*)(s + 0x6E));
    // Second viking (code_seg = 0)
    *(uint16_t*)(s + 0x6C) = *(uint16_t*)(s + di_table + 6);
    *(uint16_t*)(s + 0x6E) = *(uint16_t*)(s + di_table + 8);
    si_anim = *(uint16_t*)(s + di_table + 0xA) | *(uint16_t*)(s + 0x25C1);
    *(uint16_t*)(s + 0x42) = 0xFFFF;
    v2_sub_13809(s, 0, 0xFFFF, si_anim,
                 *(uint16_t*)(s + 0x6C), *(uint16_t*)(s + 0x6E));
    // Third viking (code_seg = 2) — tail call (jmp sub_13809)
    *(uint16_t*)(s + 0x6C) = *(uint16_t*)(s + di_table + 0xC);
    *(uint16_t*)(s + 0x6E) = *(uint16_t*)(s + di_table + 0xE);
    si_anim = *(uint16_t*)(s + di_table + 0x10) | *(uint16_t*)(s + 0x25C1);
    *(uint16_t*)(s + 0x42) = 0xFFFF;
    v2_sub_13809(s, 2, 0xFFFF, si_anim,
                 *(uint16_t*)(s + 0x6C), *(uint16_t*)(s + 0x6E));
}

static void v2_sub_11446(uint8_t* s) {
    *(uint16_t*)(s + 0x033C) = 0; // word_2881C = 0
    uint16_t mode = s[0x25BA] & 0xFF; // byte_2AA9A, zero-extended to word
    fprintf(stderr, "V2-11446: mode=%d 0x423=%04X 0x15AD=%04X level=%04X\n",
            mode, *(uint16_t*)(s+0x423), *(uint16_t*)(s+0x15AD), *(uint16_t*)(s+0x25AD));
    if (mode == 0) {
        // loc_1146a: Normal gameplay
        *(uint16_t*)(s + 0x0374) = *(uint16_t*)(s + 0x25C3);
        *(uint16_t*)(s + 0x006C) = *(uint16_t*)(s + 0x25BB);
        *(uint16_t*)(s + 0x006E) = *(uint16_t*)(s + 0x25BD);
        *(uint16_t*)(s + 0x0042) = 0xFFFF;
        v2_sub_13809(s, *(uint16_t*)(s + 0x25BF), 0xFFFF, *(uint16_t*)(s + 0x25C1),
                     *(uint16_t*)(s + 0x006C), *(uint16_t*)(s + 0x006E));
        *(uint16_t*)(s + 0x033C) = 2;
        return; // RETN before loc_1154a
    }
    if (mode == 2) { v2_sub_11569(s, 0x8508); goto loc_1154a; }
    if (mode == 4) { v2_sub_11569(s, 0x851A); goto loc_1154a; }
    if (mode == 5) { v2_sub_11569(s, 0x852C); goto loc_1154a; }
    {
        // Mode 0x10 and default: loc_114bd path — 3 vikings at calculated positions
        uint16_t y_off_2, y_off_3; // pushed values (LIFO: second push = first pop)
        if (mode == 0x10) { y_off_3 = 0xFFF8; y_off_2 = 0xFFF0; }
        else              { y_off_3 = 0;      y_off_2 = 0;      }
        // loc_114bd: first viking (code_seg = 1) at base position
        *(uint16_t*)(s + 0x0374) = *(uint16_t*)(s + 0x25C3);
        *(uint16_t*)(s + 0x006C) = *(uint16_t*)(s + 0x25BB);
        *(uint16_t*)(s + 0x006E) = *(uint16_t*)(s + 0x25BD);
        *(uint16_t*)(s + 0x0042) = 0xFFFF;
        v2_sub_13809(s, 1, 0xFFFF, *(uint16_t*)(s + 0x25C1),
                     *(uint16_t*)(s + 0x006C), *(uint16_t*)(s + 0x006E));
        // Second viking (code_seg = 0): X ± 0x20, Y + y_off_2
        uint16_t base_x = *(uint16_t*)(s + 0x25BB);
        if (*(uint16_t*)(s + 0x25C1) & 0x40)
            *(uint16_t*)(s + 0x006C) = base_x + 0x20;
        else
            *(uint16_t*)(s + 0x006C) = base_x - 0x20;
        *(uint16_t*)(s + 0x006E) = y_off_2 + *(uint16_t*)(s + 0x25BD);
        *(uint16_t*)(s + 0x0042) = 0xFFFF;
        v2_sub_13809(s, 0, 0xFFFF, *(uint16_t*)(s + 0x25C1),
                     *(uint16_t*)(s + 0x006C), *(uint16_t*)(s + 0x006E));
        // Third viking (code_seg = 2): X ± 0x40, Y + y_off_3
        if (*(uint16_t*)(s + 0x25C1) & 0x40)
            *(uint16_t*)(s + 0x006C) = base_x + 0x40;
        else
            *(uint16_t*)(s + 0x006C) = base_x - 0x40;
        *(uint16_t*)(s + 0x006E) = y_off_3 + *(uint16_t*)(s + 0x25BD);
        *(uint16_t*)(s + 0x0042) = 0xFFFF;
        v2_sub_13809(s, 2, 0xFFFF, *(uint16_t*)(s + 0x25C1),
                     *(uint16_t*)(s + 0x006C), *(uint16_t*)(s + 0x006E));
    }
loc_1154a:
    // Copy sound state, set mode 6
    *(uint16_t*)(s + 0x15AD) = *(uint16_t*)(s + 0x0423); // word_29A8D = word_28903
    *(uint16_t*)(s + 0x15AF) = *(uint16_t*)(s + 0x0425); // word_29A8F = word_28905
    *(uint16_t*)(s + 0x15B1) = *(uint16_t*)(s + 0x0427); // word_29A91 = word_28907
    *(uint16_t*)(s + 0x033C) = 6;    // word_2881C
    *(uint16_t*)(s + 0x03C4) = 0xFFFF; // word_288A4
}

// sub_11383: find end of spawn table. Returns di = end index + 2.
static uint16_t v2_sub_11383(uint8_t* s) {
    uint16_t di = 0;
    while (*(uint16_t*)(s + di + 0x25F6) != 0xFFFF) di += 0x0E;
    return di + 2;
}

// sub_112ae: init vikings from level spawn table extension.
// Entry format: chunk_id (word) + type (byte, read as word & 0xFF) = 3 bytes per entry.
// Decompresses chunk into DS at type*3 + 0x7F02.
static uint16_t v2_sub_112ae(uint8_t* s, uint16_t di_start) {
    uint16_t di = di_start;
    while (true) {
        uint16_t chunk_id = *(uint16_t*)(s + di + 0x25F6);
        if (chunk_id == 0xFFFF) break;
        uint16_t type_val = *(uint16_t*)(s + (uint16_t)(di + 2) + 0x25F6) & 0xFF;
        uint16_t dest_off = type_val * 3 + 0x7F02;
        v2_read_chunk(chunk_id, s + dest_off, 0x10000 - dest_off);
        di += 3;
    }
    // loc_112da: palette clearing + state copy (falls through from loop end)
    // Clear 15 palette dwords at step 0x30 starting at ds:0x7F32, mask 0xFF000000
    uint32_t mask = 0xFF000000;
    for (int i = 0; i < 15; i++) {
        uint16_t addr = 0x7F32 + i * 0x30;
        *(uint32_t*)(s + addr) &= mask;
    }
    // Copy word_303EB → word_2AA88, byte_303ED → byte_2AA8A
    *(uint16_t*)(s + 0x25A8) = *(uint16_t*)(s + 0x7F0B);
    s[0x25AA] = s[0x7F0D];
    // jmp sub_10e99: palette processing — for v2, palette in shadow DS applied during render
    return di + 2;
}

// sub_11784: init active viking. word_288A2=0, word_288A4=0. 0 bytes consumed.
static void v2_sub_11784(uint8_t* s) {
    *(uint16_t*)(s + 0x03C2) = 0; // word_288A2 (active viking)
    *(uint16_t*)(s + 0x03C4) = 0; // word_288A4 (previous viking)
}

// sub_16880: clear VGA pages. Clears ALL 64KB of VGA memory (viewport + HUD + all 3 pages).
// Original: OUT(0x3C4, 0x0F02) enable all planes; REP STOSW ax=0, cx=0x8000, es=0xA000.
// Also: drawPixel(j, i, 0) for SDL surface.
// For v2: clear both render buffer (viewport) and HUD buffer.
static void v2_sub_16880(uint8_t* s) {
    // OUT(0x3C4, 0x0F02); // VGA sequencer: enable all 4 planes
    // REP STOSW ax=0, cx=0x8000 words (64KB) to es:0 (VGA 0xA000)
    // drawPixel(plane, offset, 0) for all 4 planes × 0x8000 offsets
    memset(v2_render_buf, 0, 320 * 200);
    memset(v2_hud_buf, 0, 320 * 64);
}

// sub_116e3: init level descriptor. Sets transition mode based on level number.
// sub_12ce4: viking health init. Exact replica.
static void v2_sub_12ce4(uint8_t* s) {
    fprintf(stderr, "V2-12CE4: called, 0x423 before=%02X\n", s[0x423]);
    *(uint16_t*)(s + 0x414) = 0;
    *(uint16_t*)(s + 0x416) = 0;
    *(uint16_t*)(s + 0x418) = 0;
    for (uint16_t si = 0; si < 0x18; si += 2)
        *(uint16_t*)(s + si + 0x3E4) = 0;
    *(uint16_t*)(s + 0x423) = 6;
    *(uint16_t*)(s + 0x425) = 6;
    *(uint16_t*)(s + 0x427) = 6;
    *(uint16_t*)(s + 0x429) = 0;
    *(uint16_t*)(s + 0x42B) = 0;
    *(uint16_t*)(s + 0x42D) = 0;
}

static void v2_sub_116e3(uint8_t* s) {
    *(uint16_t*)(s + 0x0352) = 0x1E;  // word_28832
    *(uint16_t*)(s + 0x03CE) = 0;     // word_288AE
    *(uint16_t*)(s + 0x03D0) = 0;     // word_288B0
    uint16_t level = *(uint16_t*)(s + 0x25C9); // word_2AAA9
    // Check special levels
    if (level == 0x2D || level == 0x2E || level == 0x2F) {
        goto transition;
    }
    {
        uint16_t ac = *(uint16_t*)(s + 0x03CC); // word_288AC
        if (ac == 1) goto transition;
        if (ac == 0x8002) {
            // loc_11780: jmp sub_12ce4 — tail call (unconditional!)
            v2_sub_12ce4(s);
            return;
        }
        if (ac == 0x8001) {
            // loc_11720: clear 0x800 words at ds:0x2191
            for (uint16_t di = 0; di < 0x800; di += 2) {
                *(uint16_t*)(s + di + 0x2191) = 0;
            }
            return;
        }
        // default: clear word_288AC
        *(uint16_t*)(s + 0x03CC) = 0;
        return;
    }
transition:
    // loc_11733: set transition state
    *(uint16_t*)(s + 0x0398) = 1;      // word_28878
    *(uint16_t*)(s + 0x03CC) = 0x8000; // word_288AC
    level = *(uint16_t*)(s + 0x25C9);  // reload
    uint16_t chunk_ax;
    if (level == 0x2B) {
        chunk_ax = 0x1BB;
    } else if (level >= 0x2C && level <= 0x2F) {
        chunk_ax = 0x1BC;
    } else {
        // Read from transition table
        uint16_t si = *(uint16_t*)(s + 0x03D4); // word_288B4
        *(uint16_t*)(s + 0x25C9) = *(uint16_t*)(s + si + 0x2B66);
        chunk_ax = *(uint16_t*)(s + si + 0x2B74);
    }
    // loc_11774: load transition chunk → ds:0x2193
    *(uint16_t*)(s + 0x2191) = 2; // word_2A671
    v2_read_chunk(chunk_ax, s + 0x2193, 0x10000 - 0x2193);
    // jmp sub_12ce4 — tail call (unconditional!)
    v2_sub_12ce4(s);
}

static void v2_load_level(uint8_t* shadow); // forward decl
static void v2_load_template(uint8_t* shadow);
static void v2_load_level_data(uint8_t* shadow);

// sub_13809: shared object creation function.
// ax = code_seg_idx, di_spawn = spawn index (0xFFFF for non-spawn), si_anim = animation flags.
// pos_x/pos_y = world position. Returns true if created, false if failed.
static bool v2_sub_13809(uint8_t* s, uint16_t code_seg_idx, uint16_t di_spawn,
                          uint16_t si_anim, uint16_t pos_x, uint16_t pos_y) {
    *(uint16_t*)(s + 0x34) = code_seg_idx;
    *(uint16_t*)(s + 0x36) = di_spawn;
    *(uint16_t*)(s + 0x38) = si_anim;
    // sub_13d30: check creation allowed
    if (*(uint16_t*)(s + 0x32F) != 0) return false;
    if (di_spawn != 0xFFFF) {
        uint16_t bit = di_spawn & 7;
        uint16_t boff = di_spawn >> 3;
        if (s[boff + 0x356] & s[(uint16_t)(bit - 0x6C44)]) return false;
    }
    // sub_13d52: find free object slot
    uint16_t new_si = 0xFFFF;
    for (uint16_t s2 = 0; s2 < 0x28; s2 += 2) {
        if (*(uint16_t*)(s + s2 + 0x1355) == 0) { new_si = s2; break; }
    }
    if (new_si == 0xFFFF) return false;
    // Original: es = ds:0x2E67 (animation data segment), bx = code_seg_idx * 0x15
    // For v2: read from shadow animdata (template data loaded by v2_load_template)
    if (!v2_animdata_shadow_valid) return false;
    uint16_t anim_seg = *(uint16_t*)(s + 0x2E67); // needed for [si+0x1355] = anim_seg
    uint8_t* aes = v2_vm_shadow_animdata;
    uint16_t bx_a = code_seg_idx * 0x15;
    // sub_13e52: init from animation table
    *(uint16_t*)(s + new_si + 0x16ED) = code_seg_idx;
    *(uint16_t*)(s + new_si + 0x16C5) = di_spawn;
    *(uint16_t*)(s + new_si + 0x1585) = si_anim;
    *(uint16_t*)(s + new_si + 0x169D) = *(uint16_t*)(s + 0x374);
    *(uint16_t*)(s + 0x374) = 0;
    // sub_12f82: sprite resource lookup
    {
        uint16_t chunk_id = *(uint16_t*)(aes + bx_a);
        uint16_t sprite_base = 0;
        if (chunk_id == 0xFFFF) { sprite_base = 0; }
        else if (chunk_id == 0xFFFE) { *(uint16_t*)(s + 0x374) += 1; sprite_base = 0; }
        else {
            bool found_res = false;
            for (uint16_t rdi = 0; rdi < 0x40; rdi += 2) {
                if (*(uint16_t*)(s + rdi + 0x12AD) == chunk_id) {
                    sprite_base = *(uint16_t*)(s + rdi + 0x12ED);
                    found_res = true; break;
                }
            }
            if (!found_res) return false;
        }
        *(uint16_t*)(s + new_si + 0x1855) = sprite_base;
    }
    uint8_t ss_byte = aes[bx_a + 2];
    if (ss_byte & 0x80) *(uint16_t*)(s + 0x374) += 2;
    *(uint16_t*)(s + new_si + 0x1AD5) = ss_byte & 0x7F;
    *(uint16_t*)(s + new_si + 0x132D) = *(uint16_t*)(aes + bx_a + 3) + 3;
    *(uint16_t*)(s + new_si + 0x1355) = anim_seg;
    *(uint16_t*)(s + new_si + 0x15AD) = *(uint16_t*)(aes + bx_a + 7);
    *(uint16_t*)(s + new_si + 0x1445) = (uint16_t)aes[bx_a + 9];
    *(uint16_t*)(s + new_si + 0x146D) = (uint16_t)aes[bx_a + 0xA];
    *(uint16_t*)(s + new_si + 0x15D5) = *(uint16_t*)(aes + bx_a + 0xB);
    *(uint16_t*)(s + new_si + 0x15FD) = *(uint16_t*)(aes + bx_a + 0xD);
    *(uint16_t*)(s + new_si + 0x1625) = *(uint16_t*)(aes + bx_a + 0xF);
    *(uint16_t*)(s + new_si + 0x178D) = *(uint16_t*)(aes + bx_a + 0x11);
    *(uint16_t*)(s + new_si + 0x17B5) = *(uint16_t*)(aes + bx_a + 0x13);
    *(uint16_t*)(s + new_si + 0x173D) = pos_x;
    *(uint16_t*)(s + new_si + 0x13A5) = pos_x;
    *(uint16_t*)(s + new_si + 0x1765) = pos_y;
    *(uint16_t*)(s + new_si + 0x13CD) = pos_y;
    *(uint16_t*)(s + new_si + 0x1805) = *(uint16_t*)(s + 0x42);
    *(uint16_t*)(s + new_si + 0x19BD) = 0;
    *(uint16_t*)(s + new_si + 0x19E5) = 0;
    *(uint16_t*)(s + new_si + 0x1715) = 0;
    *(uint16_t*)(s + new_si + 0x164D) = 0;
    *(uint16_t*)(s + new_si + 0x1675) = 0;
    *(uint16_t*)(s + new_si + 0x1945) = 0;
    *(uint16_t*)(s + new_si + 0x196D) = 0;
    *(uint16_t*)(s + new_si + 0x17DD) = 0;
    *(uint16_t*)(s + new_si + 0x18A5) = 0;
    *(uint16_t*)(s + new_si + 0x18CD) = 0;
    *(uint16_t*)(s + new_si + 0x191D) = 0xFFFF;
    *(uint16_t*)(s + new_si + 0x187D) = 0;
    *(uint16_t*)(s + new_si + 0x18F5) = 0;
    *(uint16_t*)(s + new_si + 0x182D) = 0xFFFF;
    *(uint16_t*)(s + new_si + 0x1A0D) = 0xFFFF;
    *(uint16_t*)(s + new_si + 0x141D) = 0xFFFF;
    // Bounds
    uint16_t w = *(uint16_t*)(s + new_si + 0x1445);
    uint16_t h = *(uint16_t*)(s + new_si + 0x146D);
    *(uint16_t*)(s + new_si + 0x1535) = pos_x - (w >> 1);
    *(uint16_t*)(s + new_si + 0x155D) = pos_x - (w >> 1) + w - 1;
    *(uint16_t*)(s + new_si + 0x14E5) = pos_y - (h >> 1);
    *(uint16_t*)(s + new_si + 0x150D) = pos_y - (h >> 1) + h - 1;
    int16_t off_val = (int16_t)*(uint16_t*)(s + 0x3E0);
    if (off_val < 0) {
        *(uint16_t*)(s + 0x3E2) = h >> 1;
        off_val = (int16_t)(w >> 1);
    }
    *(uint16_t*)(s + new_si + 0x14BD) = (uint16_t)off_val;
    *(uint16_t*)(s + new_si + 0x1495) = *(uint16_t*)(s + 0x3E2);
    // sub_13d68 + sub_13dd6 + sub_13e15: sub-sprite allocation + init
    if (*(uint16_t*)(s + new_si + 0x1AD5) != 0) {
        uint16_t pool_flag = *(uint16_t*)(s + 0x374);
        uint16_t ss_start, ss_limit;
        if (pool_flag == 0) { ss_start = 0x48; ss_limit = 0x100; }
        else if (pool_flag == 1) { ss_start = 0x30; ss_limit = 0x50; }
        else { ss_start = 0; ss_limit = 0x30; }
        // Original sub_13d68 writes pool limit to ds:0x32
        *(uint16_t*)(s + 0x32) = ss_limit;
        uint16_t ss_count = *(uint16_t*)(s + new_si + 0x1AD5);
        bool ss_found = false;
        uint16_t ss_di = ss_start;
        while ((int16_t)ss_di < (int16_t)ss_limit) {
            if ((*(uint16_t*)(s + ss_di + 0x44D) | *(uint16_t*)(s + ss_di + 0x114D)) != 0) {
                ss_di += 2; continue;
            }
            uint16_t ss_first = ss_di;
            uint16_t ss_end_need = ss_di + ss_count * 2;
            // Original sub_13d68 writes to ds:0x3A and ds:0x38
            *(uint16_t*)(s + 0x3A) = ss_first;
            *(uint16_t*)(s + 0x38) = ss_end_need;
            bool block_ok = true;
            ss_di += 2;
            while (ss_di != ss_end_need) {
                if (ss_di == ss_limit) {
                    // loc_13da8 → STC → sub_13809 loc_13860: clear slot before return
                    *(uint16_t*)(s + new_si + 0x1355) = 0;
                    return false;
                }
                if ((*(uint16_t*)(s + ss_di + 0x44D) | *(uint16_t*)(s + ss_di + 0x114D)) != 0) {
                    block_ok = false; break;
                }
                ss_di += 2;
            }
            if (block_ok) {
                // sub_13809 copies ds:0x3A → [si+1A85h], ds:0x38 → [si+1AADh]
                *(uint16_t*)(s + new_si + 0x1A85) = *(uint16_t*)(s + 0x3A);
                *(uint16_t*)(s + new_si + 0x1AAD) = *(uint16_t*)(s + 0x38);
                ss_found = true; break;
            }
        }
        if (!ss_found) { *(uint16_t*)(s + new_si + 0x1355) = 0; return false; }
        // sub_13dd6: init sub-sprite flags
        uint16_t flags_init = (*(uint16_t*)(s + new_si + 0x1585) & 0xCE) << 3;
        flags_init |= 0x8000;
        uint16_t ss_end2 = *(uint16_t*)(s + new_si + 0x1AAD);
        for (uint16_t sdi = *(uint16_t*)(s + new_si + 0x1A85); (int16_t)sdi < (int16_t)ss_end2; sdi += 2) {
            *(uint16_t*)(s + sdi + 0x44D) = flags_init;
            *(uint16_t*)(s + sdi + 0x54D) = 0;
            *(uint16_t*)(s + sdi + 0x114D) = 0x204;
            *(uint16_t*)(s + sdi + 0x94D) = *(uint16_t*)(s + 0x2E73);
            *(uint16_t*)(s + sdi + 0x84D) = *(uint16_t*)(s + new_si + 0x1855);
        }
        // sub_13e15: sub-sprite sizes
        uint16_t bp_type = (*(uint16_t*)(s + new_si + 0x1585) & 1) << 1;
        for (uint16_t sdi = *(uint16_t*)(s + new_si + 0x1A85); (int16_t)sdi < (int16_t)ss_end2; sdi += 2) {
            if (bp_type != 0) {
                *(uint16_t*)(s + sdi + 0x0C4D) = 0x20;
                *(uint16_t*)(s + sdi + 0x44D) |= 2;
            } else {
                *(uint16_t*)(s + sdi + 0x0C4D) = 8;
                *(uint16_t*)(s + sdi + 0x44D) |= 1;
            }
        }
    }
    // Update table end
    if ((int16_t)new_si >= (int16_t)*(uint16_t*)(s + 0x372)) {
        *(uint16_t*)(s + 0x372) = new_si + 2;
    }
    return true;
}

// ============================================================================
// V2 Startup — one-time init functions (eip 0x0000..0x0014).
// Called ONCE before first v2_sub_11080, AFTER init_shadow_early copies static DS.
// Replaces original sub_12948..sub_108b8 call chain.
// ============================================================================

// sub_12948: DOS memory resize + critical error handler + PRNG seed.
// Original: INT 21h/4Ah (resize), INT 21h/35h/25h (interrupt vectors), sub_16528
//           (keyboard INT 9 handler), INT 21h/2Ch (get time → PRNG seed).
static void v2_sub_12948(uint8_t* s) {
    // INT 21h/4Ah: resize memory block — not needed (no DOS memory management)
    // INT 21h/35h: get interrupt vector 24h → ds:0x86AC/0x86AE (old vector saved)
    // INT 21h/25h: set interrupt vector 24h — not needed
    // sub_16528: save/set INT 9h (keyboard) handler — not needed (SDL handles input)
    // INT 21h/35h: old INT 24h vector → ds:0x86AC/0x86AE
    // v2 standalone: no DOS, write 0 (expected diff)
    *(uint16_t*)(s + 0x86AC) = 0;
    *(uint16_t*)(s + 0x86AE) = 0;

    // INT 21h/2Ch: get current time → PRNG seed (dword at ds:0x8639)
    // asm.cpp returns fixed cx=0x1234, dx=0x5678 for deterministic PRNG
    *(uint16_t*)(s + 0x8639) = 0x5678; // dx
    *(uint16_t*)(s + 0x863B) = 0x1234; // cx
}

// sub_12989: open DATA.DAT, read header, validate, read game config.
// Original: INT 21h/3Dh (open file), fread header, BIOS checksum, DOS version check,
//           joystick calibration, copy sound driver flags from header.
static void v2_sub_12989(uint8_t* s) {
    // INT 21h/3Dh: open "DATA.DAT" for reading
    // Original stores handle in ds:0x2BB2
    if (!v2_data_handle) {
        v2_data_handle = fopen("DATA.DAT", "rb");
    }
    if (!v2_data_handle) {
        printf("V2-STARTUP: failed to open DATA.DAT\n");
        return;
    }

    // fread 8 bytes → ds:0x2BB4..0x2BBB (chunk table header)
    // Original: INT 21h/3Fh, cx=8, dx=0x2BB4
    fseek(v2_data_handle, 0, SEEK_SET);
    fread(s + 0x2BB4, 8, 1, v2_data_handle);

    // fseek to offset from header, fread 0x20 bytes → ds:0x86B0..0x86CF (game header)
    // Original: INT 21h/42h (seek), INT 21h/3Fh (read 0x20 bytes)
    uint32_t header_offset = *(uint32_t*)(s + 0x2BB4);
    fseek(v2_data_handle, header_offset, SEEK_SET);
    fread(s + 0x86B0, 0x20, 1, v2_data_handle);

    // Validate magic: ds:0x86C4 == 0x6969
    if (*(uint16_t*)(s + 0x86C4) != 0x6969) {
        printf("V2-STARTUP: bad DATA.DAT magic %04X\n", *(uint16_t*)(s + 0x86C4));
    }

    // BIOS ROM checksum at F000:0000 (0x7FF8 words) → ds:0x86D0
    // Original: gs=F000h, sum loop, XOR with INT 21h/36h (disk space), INT 21h/30h (DOS version)
    // BIOS ROM checksum + DOS disk/version XOR → ds:0x86D0
    // Original: gs=F000, sum 0x7FF8 words, XOR with INT 21h/36h + INT 21h/30h.
    // Copy protection check. Copy from real DS if available, else 0.
    if (v2_vm_real_ds_ptr) {
        *(uint16_t*)(s + 0x86D0) = *(uint16_t*)(v2_vm_real_ds_ptr + 0x86D0);
    } else {
        *(uint16_t*)(s + 0x86D0) = 0;
    }

    // Copy sound driver flags from game header
    // Original: ds:0x302 = ds:0x86B2, ds:0x304 = ds:0x86B4
    *(uint16_t*)(s + 0x302) = *(uint16_t*)(s + 0x86B2);
    *(uint16_t*)(s + 0x304) = *(uint16_t*)(s + 0x86B4);

    // INT 10h/1Ah: VGA display combination check — not needed (we know it's VGA)
    // CPU detection (EFLAGS test) — not needed (we're on modern CPU)

    // sub_179a8 / sub_179fb: joystick calibration
    // DS writes: ds:0x86DA = joystick present flag, ds:0x86D2..0x86D8 = calibration
    // For v2: no joystick. Mark as absent.
    *(uint16_t*)(s + 0x86DA) = 0; // no joystick
}

// sub_12ab8: segment allocation + initial data load.
// Original: 9× sub_10d9f (DOS INT 21h/48h = alloc memory), then chunk decompressions.
// For v2: shadow buffers already allocated. Write segment addresses to DS, load chunks.
// DosMemAlloc replay: original records results, v2 replays them in order.
static uint16_t v2_alloc_record[64];
static int v2_alloc_record_count = 0;
static int v2_alloc_replay_idx = 0;

// Called from asm.cpp INT 21h/48h handler after each successful DosMemAlloc
void v2_record_alloc(uint16_t seg) {
    if (v2_alloc_record_count < 64) {
        v2_alloc_record[v2_alloc_record_count++] = seg;
    }
}

// v2 wrapper: returns next recorded allocation result
static uint16_t v2_sub_10d9f(uint16_t paragraphs) {
    if (v2_alloc_replay_idx < v2_alloc_record_count) {
        return v2_alloc_record[v2_alloc_replay_idx++];
    }
    // Fallback: no recorded value yet (shouldn't happen if original ran first)
    fprintf(stderr, "V2: v2_sub_10d9f(%04X) — no recorded alloc available!\n", paragraphs);
    return 0;
}

static void v2_sub_12ab8(uint8_t* s) {
    // Original: sub_10d9f(bx) = INT 21h/48h: allocate bx paragraphs, returns segment in ax.
    // v2 calls sub_10d9f (DosMemAlloc wrapper) to get the same segment addresses.
    // Uses v2_sub_10d9f which replays allocation results recorded by original.
    *(uint16_t*)(s + 0x2E5F) = v2_sub_10d9f(0x8B8);   // tilegfx
    *(uint16_t*)(s + 0x2E61) = v2_sub_10d9f(0x200);   // GS masks
    *(uint16_t*)(s + 0x2E5D) = v2_sub_10d9f(0x360);   // GS tiledata
    *(uint16_t*)(s + 0x2E63) = v2_sub_10d9f(0x313);   // tilemap
    *(uint16_t*)(s + 0x2E69) = v2_sub_10d9f(0xC08);   // FS
    *(uint16_t*)(s + 0x2E73) = v2_sub_10d9f(0x700);   // sprites

    // Sound driver check: if !(ds:0x302 & ds:0x304 & 0x8000)
    // Original allocates sound driver buffer, decompresses 3 sound chunks, inits AIL.
    // For v2: AIL not used. Copy sound-related DS fields from real DS if available.
    if (!(*(uint16_t*)(s + 0x302) & *(uint16_t*)(s + 0x304) & 0x8000)) {
        // sub_10d9f(0xE47) → ds:0x992C (sound driver buffer)
        *(uint16_t*)(s + 0x992C) = v2_sub_10d9f(0xE47);
        *(uint16_t*)(s + 0x992A) = 0;

        // Decompress chunk (0x1C7 + ds:0x86B6) → sound driver buffer (ds:0x2E6F)
        // sub_10982 + sub_10E85 → ds:0x2E6F = normalized es
        // For v2: decompress into shadow sound buffer
        uint16_t base = *(uint16_t*)(s + 0x86B6);
        // v2_read_chunk(0x1C7 + base, ...) — sound driver init, commented for v2
        // v2_read_chunk(0x20C + base, ...) — sound bank data
        // v2_read_chunk(0x207 + base, ...) → ds:0x2E6B = sound data segment
        uint16_t sound_chunk = 0x207 + base;
        v2_read_chunk(sound_chunk, v2_vm_shadow_sound, V2_SOUND_SHADOW_SIZE);
        v2_sound_shadow_valid = true;
        // Sound segment pointers: sub_10E85 normalizes es:di after each chunk decompression.
        // sub_10E85: es = (di >> 4) + es + 1; di = 0.
        // Results depend on decompressed sizes of chunks 0x1C7+base, 0x20C+base, 0x207+base.
        // V2 doesn't decompress all 3 chunks — read normalized results from real DS.
        // These are DosMemAlloc-derived (offset within 0x992C allocation).
        if (v2_vm_real_ds_ptr) {
            *(uint16_t*)(s + 0x2E6F) = *(uint16_t*)(v2_vm_real_ds_ptr + 0x2E6F); // sound driver
            *(uint16_t*)(s + 0x2E6D) = *(uint16_t*)(v2_vm_real_ds_ptr + 0x2E6D); // sound bank
            *(uint16_t*)(s + 0x2E6B) = *(uint16_t*)(v2_vm_real_ds_ptr + 0x2E6B); // sound data
        }
    }

    // sub_10d9f(0x2ABA) → ds:0x2E77 (chunk buffer)
    *(uint16_t*)(s + 0x2E77) = v2_sub_10d9f(0x2ABA);
    *(uint16_t*)(s + 0x2E75) = 0;
    *(uint16_t*)(s + 0x2E7B) = *(uint16_t*)(s + 0x2E77);
    *(uint16_t*)(s + 0x2E79) = 0;

    // sub_10d9f(0xC00) → ds:0x2E67 (animdata)
    *(uint16_t*)(s + 0x2E67) = v2_sub_10d9f(0xC00);
    // Decompress chunk 0x1C6 → animdata
    // ds:0x2E71 = 0x1C6 (current template chunk ID)
    *(uint16_t*)(s + 0x2E71) = 0x1C6;
    v2_read_chunk(0x1C6, v2_vm_shadow_animdata, V2_ANIMDATA_SHADOW_SIZE);
    v2_animdata_shadow_valid = true;

    // Decompress chunks 4..0xE → DS (level lookup tables)
    // chunk 4 → ds:0x2E7D, chunk 5 → ds:0x317D, ..., chunk 0xE → ds:0x507D
    {
        static const struct { uint16_t chunk; uint16_t offset; } level_tables[] = {
            {4, 0x2E7D}, {5, 0x317D}, {6, 0x347D}, {7, 0x377D},
            {8, 0x3A7D}, {9, 0x3D7D}, {0xA, 0x407D}, {0xB, 0x437D},
            {0xC, 0x467D}, {0xD, 0x497D}, {0xE, 0x507D},
        };
        for (auto& t : level_tables) {
            v2_read_chunk(t.chunk, s + t.offset, 0x10000 - t.offset);
        }
    }

    // Decompress chunk 2 → ds:0x687D (password/level select table)
    v2_read_chunk(2, s + 0x687D, 0x10000 - 0x687D);

    // Post-chunk init: "STRT" marker + VGA/rendering constants
    // Original: lines 6454-6478 in seg000 (eip 0x2C12..0x2CA2)
    *(uint16_t*)(s + 0x0310) = 0x53; // 'S'
    *(uint16_t*)(s + 0x0312) = 0x54; // 'T'
    *(uint16_t*)(s + 0x0314) = 0x52; // 'R'
    *(uint16_t*)(s + 0x0316) = 0x54; // 'T'
    *(uint16_t*)(s + 0x927C) = 0x800;
    *(uint16_t*)(s + 0x9282) = 0x200;
    *(uint16_t*)(s + 0x9286) = 0x100;
    *(uint16_t*)(s + 0x9284) = 0x400;
    *(uint16_t*)(s + 0x928C) = 0x400;
    *(uint16_t*)(s + 0x9226) = 0x20;
    *(uint16_t*)(s + 0x927A) = 0x20;
    *(uint16_t*)(s + 0x9290) = 0x10;
    *(uint16_t*)(s + 0x927E) = 0x10;
    *(uint16_t*)(s + 0x91EE) = 0x1000;
    *(uint16_t*)(s + 0x921E) = 0x1000;
    *(uint16_t*)(s + 0x9260) = 0x2000;
    *(uint16_t*)(s + 0x920A) = 0x2000;
    *(uint16_t*)(s + 0x922A) = 0x80;
    *(uint16_t*)(s + 0x922E) = 0x8000;
    *(uint16_t*)(s + 0x925E) = 0x8000;
    *(uint16_t*)(s + 0x9224) = 0x8000;
    *(uint16_t*)(s + 0x9288) = 0x8000;
    *(uint16_t*)(s + 0x9210) = 0x40;
    *(uint16_t*)(s + 0x922C) = 0x4000;
}

// sub_17561: AIL sound driver init.
// Original: complex AIL library initialization (sub_1C75D, sub_1C763, etc.)
// For v2: sound handled by SDL wrappers, AIL not used.
static void v2_sub_17561(uint8_t* s) {
    // AIL sub_17561: sound driver init. Replicate DS writes from original.
    // sub_1C537 → ds:0x98E6 (driver handle). Original returns non-0xFFFF.
    // sub_1C5EF → ds:0x98E8 = ax (GTL offset), ds:0x98EA = dx (GTL segment).
    //   Original sub_1BEC2(0x64) returns far ptr to GTL. dx=0xFFFF in practice.
    *(uint16_t*)(s + 0x98EA) = 0xFFFF; // GTL segment (sub_1C5EF return dx)
    // sub_1C76F → ds:0x9942 (sequence handle). Original hardcodes ax=0x0E00.
    *(uint16_t*)(s + 0x9942) = 0x0E00; // sequence handle (sub_1C76F return)
    // ds:0x9934 (XMI buffer — sub_10d96(bx=0xE00, ax=0): size=(0xE00>>4)|(0<<12)+1=0xE1)
    *(uint16_t*)(s + 0x9934) = v2_sub_10d9f(0xE1);
    // ds:0xA39A = 1 (sound system active flag, set at line 15288)
    *(uint16_t*)(s + 0xA39A) = 1;
    printf("V2-STARTUP: sub_17561 AIL init — DS writes replicated\n");
}

// sub_167ff / sub_16807: VGA Mode X initialization.
// Original: INT 10h mode 13h, Mode X register setup, CRTC table load, clear VGA memory.
// DS writes: ds:0x9300 = previous video mode, ds:0x92FF = 1 (VGA initialized flag).
// VGA page address tables (ds:0x89F8 etc.) are STATIC data from EXE, already in DS.
static void v2_sub_167ff(uint8_t* s) {
    // INT 10h/0Fh: get current video mode → ds:0x9300
    // Original saves current mode before switching to Mode X.
    // v2 standalone: no VGA, write 0x03 (text mode, expected diff)
    s[0x9300] = 0x00; // INT 10h/0Fh returns 0 in SDL wrapper (no real VGA mode)

    // sub_16807: set VGA Mode X. Verified with seg000 lines 14223-14309.
    // INT(0x10, ax=0x13);         // Set video mode 13h (320x200 256-color)
    // sub_1106f: clear VGA DAC palette
    // OUT(0x3C8, 0x00);           // DAC write address = 0
    // for (i=0; i<768; i++) OUT(0x3C9, 0x00);  // all 256 colors to black
    // OUT(0x3C4, 0x0604);         // Sequencer: memory mode (chain-4 off, odd/even off)
    // OUT(0x3C4, 0x0100);         // Sequencer: synchronous reset
    // OUT(0x3C2, 0xE3);           // Misc output: clock select, sync polarity
    // OUT(0x3C4, 0x0300);         // Sequencer: character map select (end reset)
    // CRTC table (14 entries at ds:0x89DC):
    //   for (i=0; i<14; i++) OUT(0x3D4, ds:[0x89DC + i*2]);  // CRT controller registers
    // IN(0x3DA);                  // Read status to reset attribute flip-flop
    // OUT(0x3C0, 0x30);           // Attribute controller: mode control
    // OUT(0x3C0, 0x61);           // Attribute controller: value (overscan)
    // OUT(0x3C4, 0x0F02);         // Sequencer: enable all 4 planes
    // REP STOSW: clear 64KB VGA memory (es=0xA000, di=0, cx=0x8000, ax=0)
    memset(v2_render_buf, 0, 320 * 200);
    memset(v2_hud_buf, 0, 320 * 64);

    // ds:0x92FF = 1 (VGA mode initialized flag)
    s[0x92FF] = 1;
}

// sub_12ca3: game state initialization — clear misc state variables.
static void v2_sub_12ca3(uint8_t* s) {
    s[0x32E] = 0;                        // byte ds:32Eh
    s[0x28] = 0;                         // byte ds:28h
    *(uint16_t*)(s + 0x3D4) = 0;         // word ds:3D4h
    *(uint16_t*)(s + 0x3CC) = 0;         // word ds:3CCh (word_288AC)
    *(uint16_t*)(s + 0x336) = 0;         // word ds:336h
    *(uint16_t*)(s + 0x34A) = 0;         // word ds:34Ah
    *(uint16_t*)(s + 0x34C) = 0;         // word ds:34Ch
    *(uint16_t*)(s + 0x334) = 0;         // word ds:334h
    *(uint16_t*)(s + 0x331) = 0;         // word ds:331h
    *(uint16_t*)(s + 0x25AF) = 0xFFFF;   // word ds:25AFh (current music track = none)
    *(uint16_t*)(s + 0x3C2) = 0xFFFF;    // word ds:3C2h (active viking = none)
}

// sub_108b8: set starting level + init health.
// Calls sub_12ce4 (health init), sets word_2AAA9 = 0x27 (level 39), word_288A2 = 0.
static void v2_sub_108b8(uint8_t* s) {
    v2_sub_12ce4(s);                      // sub_12CE4: health init
    *(uint16_t*)(s + 0x25C9) = 0x27;     // word_2AAA9 = starting level (39 = 0x27)
    *(uint16_t*)(s + 0x03C2) = 0;        // word_288A2 = active viking = 0
}

// v2_startup: one-time initialization — replaces eip 0x0000..0x0014 call chain.
// 1. Load ds_static.bin (pristine EXE data segment)
// 2. Run sub_12948..sub_108b8 on shadow DS (fills runtime data)
// After this, shadow DS is equivalent to real DS after original startup.
static bool v2_startup_done = false;
static void v2_startup(uint8_t* s) {
    if (v2_startup_done) return;
    v2_startup_done = true;

    // Step 1: Load static EXE data into shadow DS
    if (!v2_load_exe_ds()) {
        printf("V2-STARTUP: FAILED — ds_static.bin not found\n");
        return;
    }
    // s pointer must be refreshed after load (it IS v2_vm_shadow_ds)

    printf("V2-STARTUP: running one-time init (sub_12948..sub_108b8)\n");
    v2_sub_12948(s);   // PRNG seed
    v2_sub_12989(s);   // DATA.DAT open + header
    v2_sub_12ab8(s);   // segment alloc + chunk decompress (level tables)
    v2_sub_17561(s);   // AIL sound init (skipped)
    v2_sub_167ff(s);   // VGA Mode X (skipped, DS flags only)
    v2_sub_12ca3(s);   // game state clear
    v2_sub_108b8(s);   // starting level + health
    printf("V2-STARTUP: complete\n");
}

// sub_11080: master level init — exact replica of original call chain.
// Called when level changes (from sub_10138 or game start).
// sub_14207 init: sub_15517 + clear priority + collision.
// Exact replica of eip 0x4207-0x4210. Called at start of every VM pass.
static void v2_sub_14207_init(uint8_t* ds) {
    // sub_15517 (eip 0x5517-0x552F): clear X/Y velocity for all objects
    for (int16_t si = (int16_t)*(uint16_t*)(ds + 0x372) - 2; si >= 0; si -= 2) {
        *(uint16_t*)(ds + si + 0x1945) = 0;  // X velocity
        *(uint16_t*)(ds + si + 0x196D) = 0;  // Y velocity
    }
    *(uint16_t*)(ds + 0x376) = 0;  // word_28856 = 0 (priority count)
    *(uint16_t*)(ds + 0x390) = 0;  // word_28870 = 0 (collision flag)
}

static void v2_sub_11080(uint8_t* s) {
    // Clear game state variables (20+ words)
    *(uint16_t*)(s + 0x032F) = 0; // word_2880F
    *(uint16_t*)(s + 0x0394) = 0; // word_28874
    *(uint16_t*)(s + 0x0396) = 0; // word_28876
    *(uint16_t*)(s + 0x0372) = 0; // word_28852 (table end — will be set by object creation)
    *(uint16_t*)(s + 0x2B64) = 0; // word_2B044
    *(uint16_t*)(s + 0x218F) = 0; // word_2A66F
    s[0x956B] = 0;                // byte_31A4B
    *(uint16_t*)(s + 0x039A) = 0; // word_2887A
    *(uint16_t*)(s + 0x039C) = 0; // word_2887C
    *(uint16_t*)(s + 0x03A2) = 0; // word_28882
    *(uint16_t*)(s + 0x03A4) = 0; // word_28884
    *(uint16_t*)(s + 0x03D6) = 0; // word_288B6
    *(uint16_t*)(s + 0x0398) = 0; // word_28878
    *(uint16_t*)(s + 0x0392) = 0; // word_28872
    *(uint16_t*)(s + 0x0348) = 0; // word_28828
    *(uint16_t*)(s + 0x8734) = 0; // word_30C14
    *(uint16_t*)(s + 0x03C6) = 0; // word_288A6
    *(uint16_t*)(s + 0x86DE) = 0; // word_30BBE
    *(uint16_t*)(s + 0x86DC) = 0; // word_30BBC
    *(uint16_t*)(s + 0x03B6) = 0; // word_28896
    *(uint16_t*)(s + 0x03B8) = 0; // word_28898

    // sub_10fa0: palette fade to black
    v2_sub_10fa0(s);
    // sub_17912: stop active sounds, clear sound handle tracking.
    // sub_17912: stop active sounds.
    // Original SDL wrapper: stop_xmidi_external();
    // Only runs if EITHER ds:0x302 OR ds:0x304 is zero. If both non-zero → skip entirely.
    // stop_xmidi_external(); // SDL wrapper — commented for v2
    if (*(uint16_t*)(s + 0x302) == 0 || *(uint16_t*)(s + 0x304) == 0) {
        uint16_t si_start = (s[0x25B9] == 1) ? 2 : 0;
        for (uint16_t si2 = si_start; (int16_t)si2 < 0x0A; si2 += 2) {
            uint16_t handle_off = (uint16_t)(si2 - 0x66F4); // wraps to 0x990C+
            if (*(uint16_t*)(s + handle_off) != 0xFFFF) {
                // Original AIL: sub_1C79F(handle, driver) + sub_1C769(handle, driver)
                // SDL wrapper already called above (stop_xmidi_external)
                *(uint16_t*)(s + handle_off) = 0xFFFF;
                *(uint16_t*)(s + (uint16_t)(si2 - 0x66EA)) = 0xFFFF;
            }
        }
    }
    // sub_12816: clear UI glyph list
    v2_sub_12816(s);

    // VGA page init
    *(uint16_t*)(s + 0x92F7) = 0;    // word_317D7
    *(uint16_t*)(s + 0x92F9) = 0x34; // word_317D9
    *(uint16_t*)(s + 0x92FB) = 0x68; // word_317DB

    // Reset animation buffer pointer (dword_2B359) from initial values
    // Original: mov ax, word_2B355; mov dword_2B359, ax; mov ax, word_2B357; mov dword_2B359+2, ax
    *(uint16_t*)(s + 0x2E79) = *(uint16_t*)(s + 0x2E75); // dword offset = initial offset (0)
    *(uint16_t*)(s + 0x2E7B) = *(uint16_t*)(s + 0x2E77); // dword segment = chunk buffer segment

    // sub_116e3: init level descriptor
    v2_sub_116e3(s);
    // sub_11784: init active viking
    v2_sub_11784(s);
    // sub_16880: clear VGA / render buffer
    v2_sub_16880(s);
    // sub_111a1: clear sprite table
    if (*(uint16_t*)(s + 0x25C9) == 0x002B)
        fprintf(stderr, "V2-117D: BEFORE sub_111a1: 0x%02X\n", s[0x117D]);
    v2_sub_111a1(s);
    if (*(uint16_t*)(s + 0x25C9) == 0x002B)
        fprintf(stderr, "V2-117D: AFTER sub_111a1: 0x%02X\n", s[0x117D]);
    // sub_11192: clear bit flags
    v2_sub_11192(s);

    // Save level number
    uint16_t old_level = *(uint16_t*)(s + 0x25AD); // word_2AA8D
    *(uint16_t*)(s + 0x25AB) = old_level; // word_2AA8B = old
    uint16_t new_level = *(uint16_t*)(s + 0x25C9); // word_2AAA9
    *(uint16_t*)(s + 0x25AD) = new_level; // word_2AA8D = new

    // sub_111b1: template + level header load (NOT tile chunks — those come after sub_117ad)
    v2_load_template(s);
    // sub_111df: clear viking state
    v2_sub_111df(s);
    // sub_12ce4: viking health init (if level != 0x25)
    fprintf(stderr, "V2-3BA: before 12ce4: 0x3BA=%04X\n", *(uint16_t*)(s + 0x3BA));
    if (new_level != 0x25) {
        v2_sub_12ce4(s);
        fprintf(stderr, "V2-HEALTH: after 12ce4: 0x423=%02X 0x3BA=%04X\n", s[0x423], *(uint16_t*)(s + 0x3BA));
    }

    // sub_117ad: word_28820=2, then if flag &1: display init chain
    *(uint16_t*)(s + 0x0340) = 2; // word_28820
    if (s[0x25CF] & 1) {
        // sub_10cd8(ax=1, di=0): read raw background chunk → chunk buffer → VGA.
        // Original: fseek + fread plane_size + fread raw data into chunk_addr.
        // Then copies to VGA 4 planes. For v2: read into chunk shadow + v2_draw_hud_background.
        {
            uint16_t chunk_base_seg = *(uint16_t*)(s + 0x2E77);
            uint16_t plane_size = v2_read_raw_chunk(1, v2_vm_shadow_chunk, V2_CHUNK_SHADOW_SIZE);
            if (plane_size > 0) {
                *(uint16_t*)(s + 0x2BBC) = plane_size; // ds:0x2BBC = plane_size
                // di=0 → HUD area. v2_draw_hud_background draws to v2_hud_buf.
                v2_draw_hud_background(v2_current_ds_val, chunk_base_seg, plane_size);
            }
        }
        // sub_1200a: clear previous health tracking
        *(uint16_t*)(s + 0x0435) = 0xFFFF; // word_28915
        *(uint16_t*)(s + 0x0437) = 0xFFFF; // word_28917
        *(uint16_t*)(s + 0x0439) = 0xFFFF; // word_28919
        // sub_1201d: copy HUD items + render each (sub_1183d)
        for (uint16_t di2 = 0; di2 < 0x18; di2 += 2) {
            uint16_t item = *(uint16_t*)(s + di2 + 0x3E4);
            *(uint16_t*)(s + di2 + 0x3FC) = item;
            // sub_1183d: draw HUD item to v2_hud_buf
            v2_draw_hud_item(v2_current_ds_val, di2, item);
        }
        // sub_12034: clear portrait/sound tracking, then JMP sub_11B0B (tail call).
        // Original sub_12034 at eip 0x2034: sets 6 words to 0xFFFF, then JMP sub_11B0B.
        // Critical: sub_11B0B runs as tail call from sub_12034, NOT from sub_11792.
        // On level 0x2C, sub_11792 skips sub_11B0B (level==0x2C check), but sub_12034's
        // tail call still runs it — syncing portrait tracking with current state.
        *(uint16_t*)(s + 0x0423) = 0xFFFF; // word_28903
        *(uint16_t*)(s + 0x0425) = 0xFFFF; // word_28905
        *(uint16_t*)(s + 0x0427) = 0xFFFF; // word_28907
        *(uint16_t*)(s + 0x042F) = 0xFFFF; // word_2890F
        *(uint16_t*)(s + 0x0431) = 0xFFFF; // word_28911
        *(uint16_t*)(s + 0x0433) = 0xFFFF; // word_28913
        // JMP sub_11B0B: portrait/sound state sync (tail call from sub_12034)
        for (int vk = 0; vk < 3; vk++) {
            uint16_t sound_prev = *(uint16_t*)(s + 0x0429 + vk * 2); // word_28909/0B/0D
            uint16_t sound_cur  = *(uint16_t*)(s + 0x042F + vk * 2); // word_2890F/11/13
            uint16_t port_prev  = *(uint16_t*)(s + 0x15AD + vk * 2); // current portrait idx
            uint16_t port_cur   = *(uint16_t*)(s + 0x0423 + vk * 2); // tracking (just set 0xFFFF)
            if (sound_prev != sound_cur || port_prev != port_cur) {
                // VGA portrait render — v2: v2_draw_hud_portrait (rendering only)
                uint16_t portrait_si = port_prev;
                if (sound_prev != 0) portrait_si += 4;
                v2_draw_hud_portrait(v2_current_ds_val, vk * 2, portrait_si);
                *(uint16_t*)(s + 0x042F + vk * 2) = sound_prev; // sync sound tracking
                *(uint16_t*)(s + 0x0423 + vk * 2) = port_prev;  // sync portrait tracking
            }
        }
        // sub_120d1: HUD selector state sync + render
        uint16_t sel1 = *(uint16_t*)(s + 0x0414);
        *(uint16_t*)(s + 0x041A) = sel1;
        v2_draw_hud_selector(v2_current_ds_val, sel1 * 2);
        uint16_t sel2 = *(uint16_t*)(s + 0x0416);
        *(uint16_t*)(s + 0x041C) = sel2;
        v2_draw_hud_selector(v2_current_ds_val, (sel2 + 4) * 2);
        uint16_t sel3 = *(uint16_t*)(s + 0x0418);
        *(uint16_t*)(s + 0x041E) = sel3;
        v2_draw_hud_selector(v2_current_ds_val, (sel3 + 8) * 2);
    }

    // sub_11204: load level data chunks (tile graphics, tilemap, etc.)
    v2_load_level_data(s);

    // di passthrough chain — each function consumes part of spawn table extension:
    uint16_t di = v2_sub_11383(s);       // find spawn table end
    di = v2_sub_112ae(s, di);            // init viking chunk data
    di = v2_sub_1133a(s, di);            // init HUD config
    di = v2_sub_1167a(s, di);            // sprite resource loading (fills ds:0x12AD/0x12ED)
    di = v2_sub_116ae(s, di);            // animation segment init
    v2_sub_11397(s);                      // collision type lookup (not from di chain)
    // sub_137f1: clear velocity tables
    v2_sub_137f1(s);
    // sub_12fb3: clear sprite resources (sprite flags)
    for (uint16_t i = 0; i < 0x100; i += 2) *(uint16_t*)(s + i + 0x44D) = 0;
    v2_sub_11446(s);                      // game mode init (not di chain)
    // sub_113b0: init scroll limits from map dimensions
    v2_sub_113b0(s);
    // sub_113d8: init viewport + scroll from viking position
    v2_sub_113d8(s);
    // sub_17749: music/sound init. Dispatches via off_3285A[ds:0x25B7 & 0xFF].
    // Dispatch table (5 entries):
    //   type 0 → loc_17791 (sub_1775d + sub_176bd)
    //   type 1 → NOP (locret_177b1)
    //   type 2 → sub_178f1 (AIL fade sound — no DS writes)
    //   type 3 → sub_1775d (music track load only)
    //   type 4 → NOP (locret_177b1)
    {
        // sub_1775d: load music track if changed. DS write: ds:0x25AF.
        // Decompresses music chunk into sound segment (ds:0x2E6B shadow).
        auto v2_sub_1775d = [&]() {
            if (*(uint16_t*)(s + 0x302) & 0x8000) return;
            uint16_t new_track = *(uint16_t*)(s + 0x25B8) & 0xFF;
            if (new_track == *(uint16_t*)(s + 0x25AF)) return;
            if (new_track == 0xFFFF) return;
            *(uint16_t*)(s + 0x25AF) = new_track;
            uint16_t chunk_table_idx = new_track * 2;
            uint16_t chunk_rel = *(uint16_t*)(s + (uint16_t)(chunk_table_idx - 0x5C7C));
            uint16_t chunk_id = chunk_rel + *(uint16_t*)(s + 0x86B8);
            v2_read_chunk(chunk_id, v2_vm_shadow_sound, V2_SOUND_SHADOW_SIZE);
        };

        // sub_176bd: allocate + start XMIDI sequence.
        // Original calls AIL library (sub_1C763/sub_1C77B/sub_1C781), replaced by SDL wrappers.
        // DS writes: [si-0x66F4]=handle, 0x9946=info, 0x993E=hi, 0x9940=lo, 0x9930=seg, 0x992E=off
        // si parameter: sound slot (0 for music).
        auto v2_sub_176bd = [&](uint16_t si_slot) {
            // Original SDL wrapper (replaces AIL sub_1C763/sub_1C77B/sub_1C781):
            //   stop_xmidi_external(id_music);
            //   id_music = play_xmidi_external(raddr(bx, 0), chunk_sizes[bx << 4], -1);
            //   set_dontstop_external(id_music);
            // where bx = ds:0x2E6B (sound segment)

            // In original SDL build: play_xmidi_external replaces AIL.
            // AIL sub_1C763 never runs → handle stays at whatever was in memory.
            // Original does NOT write to these DS addresses (AIL code bypassed).
            // So: do NOT write any sound state — leave shadow as-is.
        };

        // sub_178f1: AIL fade sound over 1000ms.
        // Original: PUSHF; CLI; sub_1C7BD(0x3E8, 0, ds:0x990C, ds:0x98E6); POPF
        //   sub_1C7BD: seg002 AIL trampoline → AIL_set_sequence_tempo(handle, tempo)
        // AIL driver not loaded in SDL build → call has no effect.
        // No DS writes — pure AIL driver call.
        auto v2_sub_178f1 = [&]() {
            if (*(uint16_t*)(s + 0x302) != 0) return;
            // fade_xmidi_external(1000); // AIL fade_out_sequence — commented for v2
        };

        uint16_t snd_type = *(uint16_t*)(s + 0x25B7) & 0xFF;
        switch (snd_type) {
            case 0: { // loc_17791: sub_1775d + sub_176bd
                if (!(*(uint16_t*)(s + 0x302) & 0x8000)) {
                    v2_sub_1775d();
                    // loc_17791: if ds:0x302 == 0 → also init sound driver via sub_176bd
                    if (*(uint16_t*)(s + 0x302) == 0) {
                        // sub_176bd(si=0, ax=0, bx=ds:0x2E6B)
                        v2_sub_176bd(0);
                    }
                }
                break;
            }
            case 1: // NOP (locret_177b1)
                break;
            case 2: // sub_178f1: AIL fade sound
                v2_sub_178f1();
                break;
            case 3: // sub_1775d: music track load only
                v2_sub_1775d();
                break;
            case 4: // NOP (locret_177b1)
                break;
            default:
                printf("V2-WARN: sub_17749 unknown dispatch type %d\n", snd_type);
                break;
        }
    }
    // DEBUG: check FS before and after sub_173c7
    // Also check if GS_TILEDATA matches FS data at 0x960
    if (v2_m2c_base) {
        uint8_t* real_ds = v2_m2c_base + ((uint32_t)v2_current_ds_val << 4);
        uint16_t fs_seg = *(uint16_t*)(real_ds + 0x2E69);
        uint8_t* rb = v2_m2c_base + (uint32_t)fs_seg * 16;
        uint16_t cols = *(uint16_t*)(real_ds + 0x25DC);
        uint16_t rows = *(uint16_t*)(real_ds + 0x25DE);
        uint16_t bp = cols * 4;
        printf("V2-FS-DBG: cols=%d rows=%d bp=%d used=%d(0x%X) fs_seg=0x%04X\n",
               cols, rows, bp, rows*cols*8, rows*cols*8, fs_seg);
        // Print tile (0,15) coordinates: this would be at the START of row 15
        // Row 15 (0-indexed) starts at di = 15 * cols * 8 = 15 * 160 = 2400 = 0x960!
        printf("V2-FS-DBG: row 15 starts at di=%d(0x%X) — matches first diff!\n",
               15 * cols * 8, 15 * cols * 8);
        // Check: does orig use rows=15 or rows=16+?
        // The FS data at 0x960 looks like sub_173c7 output (tile data from GS).
        // If orig has rows=16, it would write to [0x960..0xA5F].
        // Total diffs span [0x960..0x1F8A] = rows * cols * 8 = N * 160
        // 0x1F8A - 0x960 = 0x162A = 5674 / 160 = ~35.5 rows
        // But 851 diffs * 2 = 1702 bytes. With 160 bytes/row, that's ~10.6 rows.
        // FS diff range [0x960..0x1F8A] = 0x162B bytes span. With 851 diffs in that range.
        // This is NOT contiguous — some positions differ, some don't.
        // Let me check: does the ORIGINAL tilemap have more rows?
        // The original might have a DIFFERENT tilemap or page layout.
        // Check: how many rows would fill [0x960..0x1F8A]?
        uint32_t span = 0x1F8A - 0x960 + 2;
        printf("V2-FS-DBG: diff span=0x%X=%d bytes, potential rows=%.1f\n",
               span, span, (float)span / (cols * 8));
        // Also check: does sub_16ded write to FS? Let me check the VGA page stride.
        printf("V2-FS-DBG: ds:0x8F6C (FS page stride)=%04X\n", *(uint16_t*)(real_ds + 0x8F6C));
        // Check shadow vs real ds:0x2E69 (FS segment address)
        printf("V2-FS-DBG: real ds:0x2E69=%04X shadow ds:0x2E69=%04X\n",
               *(uint16_t*)(real_ds + 0x2E69), *(uint16_t*)(s + 0x2E69));
    }
    // sub_173c7: init scroll tracking
    v2_sub_173c7(s);
    // FS compare right after sub_173c7
    if (new_level == 0x002B && v2_m2c_base) {
        uint16_t _fs = *(uint16_t*)(v2_vm_real_ds_ptr + 0x2E69);
        if (_fs) {
            uint8_t* rfs = v2_m2c_base + (uint32_t)_fs * 16;
            int d=0;
            for (uint32_t i = 0; i < 0x6000; i += 2) {
                uint16_t rv=*(uint16_t*)(rfs+i), sv=*(uint16_t*)(v2_vm_shadow_fs+i);
                if (rv != sv) { if(d<10) { if(d==0) fprintf(stderr,"V2-FS-POST-173c7:\n");
                    fprintf(stderr,"  FS[%04X]: r=%04X s=%04X\n",(uint16_t)i,rv,sv); } d++; }
            }
            fprintf(stderr,"V2-FS-POST-173c7: %d diffs (checked 0x6000 bytes)\n",d);
        }
    }
    // sub_11439: init word_3287C
    v2_sub_11439(s);
    // sub_13ba5: init permanent objects from spawn table
    v2_sub_13ba5(s);
    // sub_13a0e: full viewport scan — create objects visible in initial viewport
    v2_sub_13a0e(s);
    // HW watchpoint — watch REAL FS at offset 0x5586 to find who clears bit0
    if (new_level == 0x002B && v2_m2c_base && v2_vm_real_ds_ptr) {
        uint16_t fsseg = *(uint16_t*)(v2_vm_real_ds_ptr + 0x2E69);
        if (fsseg) {
            uint8_t* real_fs_5586 = v2_m2c_base + (uint32_t)fsseg * 16 + 0x5586;
            // v2_hw_wp_arm(real_fs_5586, "real_fs[5586]"); // DISABLED: using ds:0x3A watchpoint
        }
    }
    // Dump key objects and check code_seg for obj 0x10
    if (v2_vm_real_ds_ptr && new_level == 0x002B) {
        fprintf(stderr, "V2-OBJ10: cs_s=%04X cs_r=%04X fl_s=%04X fl_r=%04X pc_s=%04X pc_r=%04X\n",
            *(uint16_t*)(s + 0x10 + 0x1355), *(uint16_t*)(v2_vm_real_ds_ptr + 0x10 + 0x1355),
            *(uint16_t*)(s + 0x10 + 0x1585), *(uint16_t*)(v2_vm_real_ds_ptr + 0x10 + 0x1585),
            *(uint16_t*)(s + 0x10 + 0x132D), *(uint16_t*)(v2_vm_real_ds_ptr + 0x10 + 0x132D));
        uint16_t te = *(uint16_t*)(s + 0x372);
        for (uint16_t oi = 0; oi < te; oi += 2) {
            uint16_t ss = *(uint16_t*)(s + oi + 0x1A85);
            uint16_t se = *(uint16_t*)(s + oi + 0x1AAD);
            uint16_t rss = *(uint16_t*)(v2_vm_real_ds_ptr + oi + 0x1A85);
            uint16_t rse = *(uint16_t*)(v2_vm_real_ds_ptr + oi + 0x1AAD);
            if (ss != 0 || rss != 0 || (ss <= 0x003C && se > 0x003C) || (rss <= 0x003C && rse > 0x003C))
                fprintf(stderr, "V2-OBJ-SS: obj=%02X s[%04X..%04X] r[%04X..%04X]%s\n",
                    oi, ss, se, rss, rse,
                    ((ss <= 0x003C && se > 0x003C) || (rss <= 0x003C && rse > 0x003C)) ? " <<<< OWNS 003C" : "");
        }
    }
    if (v2_vm_real_ds_ptr && new_level == 0x002B) {
        uint16_t s_row38 = *(uint16_t*)(s + 0x8FB4);
        uint16_t r_row38 = *(uint16_t*)(v2_vm_real_ds_ptr + 0x8FB4);
        uint16_t s_width = *(uint16_t*)(s + 0x25DC);
        uint16_t r_width = *(uint16_t*)(v2_vm_real_ds_ptr + 0x25DC);
        uint16_t s_9168 = *(uint16_t*)(s + 0x9168);
        uint16_t r_9168 = *(uint16_t*)(v2_vm_real_ds_ptr + 0x9168);
        fprintf(stderr, "V2-ROWTBL: row38: s=%d r=%d width: s=%d r=%d 9168: s=%d r=%d\n",
            s_row38, r_row38, s_width, r_width, s_9168, r_9168);
    }
    if (v2_vm_real_ds_ptr) {
        fprintf(stderr, "V2-PRE-115d2: vp_x s=%04X r=%04X vp_y s=%04X r=%04X mode s=%02X r=%02X\n",
            *(uint16_t*)(s+0x44), *(uint16_t*)(v2_vm_real_ds_ptr+0x44),
            *(uint16_t*)(s+0x46), *(uint16_t*)(v2_vm_real_ds_ptr+0x46),
            s[0x117D], v2_vm_real_ds_ptr[0x117D]);
    }
    // sub_115d2: first game loop frame — 3 render sub-frames + 4 page flips.
    // Original structure: VM runs ONCE, then 3 render passes with different post-processing.
    // This matches VGA triple buffering — each page gets one render pass.
    {
        // ====== SUB-FRAME 1: VM + full post-VM processing ======
        // sub_12345: clear input only (NOT full pre-VM chain!)
        // Original sub_115d2 calls sub_12345, not the full sub_12352..sub_10138 sequence.
        *(uint16_t*)(s + 0x03B6) = 0; // word_28896
        *(uint16_t*)(s + 0x03B8) = 0; // word_28898
        // sub_14207: VM execution — runs ONCE for the entire sub_115d2 call
        v2_sub_14207_init(s);
        // Exact replica including priority object loop
        // NOTE: table_end MUST be re-read each iteration — VM opcode 0x14 can create
        // new objects and increase ds:0x372 during execution.
        for (uint16_t si_vm = 0; si_vm < *(uint16_t*)(s + 0x372); si_vm += 2) {
            v2_vm_execute_object(s, si_vm);
            uint16_t prio_count = *(uint16_t*)(s + 0x376);
            if (prio_count != 0) {
                for (uint16_t di = 0; (int16_t)di < (int16_t)prio_count; di++) {
                    uint16_t prio_obj = *(uint16_t*)(s + di + 0x378) & 0xFF;
                    v2_vm_execute_object(s, prio_obj);
                }
                *(uint16_t*)(s + 0x376) = 0;
            }
        }
        // sub_1386b..sub_13916: physics, collision, spawn
        v2_game_loop_post_vm(s);
        // sub_12fc6: sub-sprite position delta type 0
        // MUST be called — updates sub-sprite X/Y from world position deltas.
        // Exact same function as in game loop RENDER1 phase.
        {
            auto delta_type0 = [](int16_t d) -> int16_t {
                if (d == 0) return 0;
                int16_t a = (d < 0) ? -d : d;
                int16_t q = a / 3, r = a % 3;
                int16_t red = q * 2 + (r >= 2 ? 1 : 0);
                return (d < 0) ? (d + red) : (d - red);
            };
            for (int16_t di2 = (int16_t)*(uint16_t*)(s + 0x372) - 2; di2 >= 0; di2 -= 2) {
                if (*(uint16_t*)(s + di2 + 0x1355) == 0) continue;
                if (*(uint16_t*)(s + di2 + 0x1AD5) == 0) continue;
                int16_t dy = (int16_t)(*(uint16_t*)(s + di2 + 0x1765) - *(uint16_t*)(s + di2 + 0x13CD));
                int16_t dx_v = (int16_t)(*(uint16_t*)(s + di2 + 0x173D) - *(uint16_t*)(s + di2 + 0x13A5));
                int16_t ty = delta_type0(dy), tx = delta_type0(dx_v);
                if (tx == 0 && ty == 0) continue;
                uint16_t ss_end = *(uint16_t*)(s + di2 + 0x1AAD);
                for (uint16_t si2 = *(uint16_t*)(s + di2 + 0x1A85); (int16_t)si2 < (int16_t)ss_end; si2 += 2) {
                    *(uint16_t*)(s + si2 + 0x64D) += (uint16_t)tx;
                    *(uint16_t*)(s + si2 + 0x74D) += (uint16_t)ty;
                    *(uint16_t*)(s + si2 + 0x114D) = 0x202;
                }
            }
        }
        // sub_16775: PAGE FLIP 1
        v2_sub_16775(s);
        // sub_16775: PAGE FLIP 1
        v2_sub_16775(s);
        v2_sub_10130(s); // sub_10130: VGA vsync wait
        // CALLF sub_1DE05
        v2_sub_1DE05(s);
        // Debug: compare FS after first sub_1DE05 in init sub_115d2
        if (new_level == 0x002B && v2_m2c_base) {
            uint16_t fs_seg = *(uint16_t*)(v2_vm_real_ds_ptr + 0x2E69);
            if (fs_seg != 0) {
                uint8_t* real_fs = v2_m2c_base + ((uint32_t)fs_seg << 4);
                int diffs = 0;
                for (uint32_t i = 0; i < 0x2B * 0x19 * 2 && diffs < 5; i += 2) {
                    uint16_t rv = *(uint16_t*)(real_fs + i);
                    uint16_t sv = *(uint16_t*)(v2_vm_shadow_fs + i);
                    if (rv != sv) {
                        if (diffs == 0) fprintf(stderr, "V2-FS-INIT-SF1: after sub_1DE05:\n");
                        fprintf(stderr, "  FS[0x%04X]: real=%04X shadow=%04X\n", (uint16_t)i, rv, sv);
                        diffs++;
                    }
                }
                if (diffs > 0) fprintf(stderr, "V2-FS-INIT-SF1: %d diffs\n", diffs);
                else fprintf(stderr, "V2-FS-INIT-SF1: 0 diffs\n");
            }
        }
        // sub_1de05_dirty_update_position(NULL); // seg003 recreated
        // sub_1c8f1_door_rendering_with_state(_state); // seg003 recreated
        // sub_165aa + sub_16661: despawn + scroll tracking
        v2_game_loop_post_render(s);
        // CALLF sub_1DD9C (4th pass)
        v2_sub_1DD9C(s);
        { static int _dd4=0; _dd4++; if(_dd4<=8) fprintf(stderr,"V2-115d2-DD9C[sf4-%d]: mode=%02X force=%02X\n",_dd4,s[0x117D],s[0x9568]); }
        // CALLF sub_1dd9c; // seg003: sprite render — v2 full-frame
        // CALLF sub_1c8f1; // seg003: door/flagged tile render ��� v2 full-frame

        // ====== SUB-FRAME 2: collision pass 2 + scroll clamp 1 ======
        // sub_16775: PAGE FLIP 2
        v2_sub_16775(s);
        // sub_15530: collision detection pass 2 (ds:0x390 = 0xFFFF)
        *(uint16_t*)(s + 0x390) = 0xFFFF;
        {
            uint16_t te = *(uint16_t*)(s + 0x372);
            for (uint16_t si2 = 0; (int16_t)si2 < (int16_t)te; si2 += 2) {
                if (*(uint16_t*)(s + si2 + 0x1355) == 0) continue;
                v2_run_collision_vm(s, si2);
            }
        }
        // sub_10704: scroll clamp — lookup at ds:[si*2 + 0x2B82]
        {
            auto scroll_left = [&](uint16_t amount) {
                if (*(uint16_t*)(s + 0x394) != 0) return;
                int16_t ax = (int16_t)*(uint16_t*)(s + 0x44) - (int16_t)amount;
                if (ax < 0) ax = 0;
                uint16_t dx = *(uint16_t*)(s + 0x44) - (uint16_t)ax;
                *(uint16_t*)(s + 0x44) = (uint16_t)ax;
                *(uint16_t*)(s + 0x257F) = (uint16_t)ax >> 3;
                *(uint16_t*)(s + 0x34E) = dx;
            };
            auto scroll_right = [&](uint16_t amount) {
                if (*(uint16_t*)(s + 0x394) != 0) return;
                uint16_t ax = *(uint16_t*)(s + 0x44) + amount;
                uint16_t limit = *(uint16_t*)(s + 0x25A4);
                if (ax >= limit) ax = limit;
                uint16_t dx = ax - *(uint16_t*)(s + 0x44);
                *(uint16_t*)(s + 0x44) = ax;
                *(uint16_t*)(s + 0x257F) = ax >> 3;
                *(uint16_t*)(s + 0x34E) = dx;
            };
            auto scroll_up = [&](uint16_t amount) {
                if (*(uint16_t*)(s + 0x396) != 0) return;
                int16_t ax = (int16_t)*(uint16_t*)(s + 0x46) - (int16_t)amount;
                if (ax < 0) ax = 0;
                uint16_t dx = *(uint16_t*)(s + 0x46) - (uint16_t)ax;
                *(uint16_t*)(s + 0x46) = (uint16_t)ax;
                *(uint16_t*)(s + 0x2581) = (uint16_t)ax >> 3;
                *(uint16_t*)(s + 0x350) = dx;
            };
            auto scroll_down = [&](uint16_t amount) {
                if (*(uint16_t*)(s + 0x396) != 0) return;
                uint16_t ax = *(uint16_t*)(s + 0x46) + amount;
                uint16_t limit = *(uint16_t*)(s + 0x25A6);
                if (ax >= limit) ax = limit;
                uint16_t dx = ax - *(uint16_t*)(s + 0x46);
                *(uint16_t*)(s + 0x46) = ax;
                *(uint16_t*)(s + 0x2581) = ax >> 3;
                *(uint16_t*)(s + 0x350) = dx;
            };
            auto do_scroll = [&](uint16_t table_off) {
                uint16_t v;
                v = *(uint16_t*)(s + 0x3D8);
                if (v != 0) { scroll_left(*(uint16_t*)(s + v * 2 + table_off)); }
                else {
                    v = *(uint16_t*)(s + 0x3DA);
                    if (v != 0) scroll_right(*(uint16_t*)(s + v * 2 + table_off));
                }
                v = *(uint16_t*)(s + 0x3DE);
                if (v != 0) { scroll_up(*(uint16_t*)(s + v * 2 + table_off)); }
                else {
                    v = *(uint16_t*)(s + 0x3DC);
                    if (v != 0) scroll_down(*(uint16_t*)(s + v * 2 + table_off));
                }
            };
            do_scroll(0x2B82); // sub_10704
            // sub_12fcb: sub-sprite position delta type 1
            {
                // sub_122c0: delta type 1 = |d|/3 with round up if remainder >= 2
                // Original: IDIV 3 → quotient. If remainder >= 2: INC quotient.
                // Returns quotient (NOT d - quotient!)
                auto delta_type1 = [](int16_t d) -> int16_t {
                    if (d == 0) return 0;
                    int16_t a = (d < 0) ? -d : d;
                    int16_t q = a / 3, r = a % 3;
                    if (r >= 2) q++;
                    return (d < 0) ? -q : q;
                };
                for (int16_t di2 = (int16_t)*(uint16_t*)(s + 0x372) - 2; di2 >= 0; di2 -= 2) {
                    if (*(uint16_t*)(s + di2 + 0x1355) == 0) continue;
                    if (*(uint16_t*)(s + di2 + 0x1AD5) == 0) continue;
                    int16_t dy = (int16_t)(*(uint16_t*)(s + di2 + 0x1765) - *(uint16_t*)(s + di2 + 0x13CD));
                    int16_t dx_v = (int16_t)(*(uint16_t*)(s + di2 + 0x173D) - *(uint16_t*)(s + di2 + 0x13A5));
                    int16_t ty = delta_type1(dy), tx = delta_type1(dx_v);
                    if (tx == 0 && ty == 0) continue;
                    uint16_t ss_end = *(uint16_t*)(s + di2 + 0x1AAD);
                    for (uint16_t si2 = *(uint16_t*)(s + di2 + 0x1A85); (int16_t)si2 < (int16_t)ss_end; si2 += 2) {
                        *(uint16_t*)(s + si2 + 0x64D) += (uint16_t)tx;
                        *(uint16_t*)(s + si2 + 0x74D) += (uint16_t)ty;
                        *(uint16_t*)(s + si2 + 0x114D) = 0x202;
                    }
                }
            }
            v2_sub_10130(s); // sub_10130: VGA vsync wait
            v2_sub_10130(s);
            v2_sub_1DE05(s);
            if (new_level == 0x002B && v2_m2c_base) {
                uint16_t fs_seg = *(uint16_t*)(v2_vm_real_ds_ptr + 0x2E69);
                if (fs_seg != 0) {
                    uint8_t* rfs = v2_m2c_base + ((uint32_t)fs_seg << 4);
                    int d2 = 0;
                    for (uint32_t i = 0; i < 0x2B * 0x19 * 2 && d2 < 3; i += 2) {
                        uint16_t rv = *(uint16_t*)(rfs + i), sv = *(uint16_t*)(v2_vm_shadow_fs + i);
                        if (rv != sv) { if(d2==0) fprintf(stderr,"V2-FS-SF2-DE05:\n");
                            fprintf(stderr,"  FS[%04X]: r=%04X s=%04X\n",(uint16_t)i,rv,sv); d2++; }
                    }
                    fprintf(stderr,"V2-FS-SF2-DE05: %d diffs, mode=%02X\n",d2,s[0x117D]);
                }
            }
            // sub_165aa + sub_16661: despawn + scroll tracking
            v2_game_loop_post_render(s);
            // CALLF sub_1DD9C (line 2911 in original)
            v2_sub_1DD9C(s);
            { static int _dd2=0; _dd2++; if(_dd2<=8) fprintf(stderr,"V2-115d2-DD9C[sf2-%d]: mode=%02X force=%02X\n",_dd2,s[0x117D],s[0x9568]); }
            // FS compare after SF2 sub_1DD9C
            if (new_level == 0x002B && v2_m2c_base) {
                uint16_t fs_seg = *(uint16_t*)(v2_vm_real_ds_ptr + 0x2E69);
                if (fs_seg != 0) {
                    uint8_t* real_fs = v2_m2c_base + ((uint32_t)fs_seg << 4);
                    int d = 0;
                    for (uint32_t i = 0; i < 0x2B * 0x19 * 2 && d < 3; i += 2) {
                        uint16_t rv = *(uint16_t*)(real_fs + i), sv = *(uint16_t*)(v2_vm_shadow_fs + i);
                        if (rv != sv) { if(d==0) fprintf(stderr,"V2-FS-SF2-DD9C:\n");
                            fprintf(stderr,"  FS[%04X]: r=%04X s=%04X\n",(uint16_t)i,rv,sv); d++; }
                    }
                    fprintf(stderr,"V2-FS-SF2-DD9C: %d diffs, mode=%02X\n",d,s[0x117D]);
                }
            }
            // MOV ax, 0xFFFE; CALLF sub_1C8F1 — flagged tiles, VGA rendering only

            // ====== SUB-FRAME 3: scroll clamp 2 + viewport bounds + HUD ======
            // sub_16775: PAGE FLIP 3
            v2_sub_16775(s);
            // sub_10753: scroll clamp 2 — lookup at ds:[si*2 + 0x2B80]
            do_scroll(0x2B80); // sub_10753

            // sub_13c0c: viewport bounds update + object visibility marking
            // X: ax = vp_x - 0x10. If ax < 0: ds:0x34 = 0 (clamped), but ax stays unclamped.
            //    ds:0x36 = ax + 0x160 (uses UNCLAMPED ax, NOT ds:0x34!)
            // Y: ax = vp_y - 0x10. If ax < 0: ax = 0 (ax IS clamped).
            //    ds:0x38 = ax, ds:0x3A = ax + 0xD0 (uses clamped ax)
            {
                uint16_t ax_x = *(uint16_t*)(s + 0x44) - 0x10; // wrapping sub
                if ((int16_t)ax_x >= 0)
                    *(uint16_t*)(s + 0x34) = ax_x;
                else
                    *(uint16_t*)(s + 0x34) = 0;
                *(uint16_t*)(s + 0x36) = ax_x + 0x160; // UNCLAMPED ax!
                uint16_t ax_y = *(uint16_t*)(s + 0x46) - 0x10;
                if ((int16_t)ax_y < 0) ax_y = 0; // Y: ax itself is clamped (JNS; MOV ax,0)
                *(uint16_t*)(s + 0x38) = ax_y;
                *(uint16_t*)(s + 0x3A) = ax_y + 0xD0;
                uint16_t te = *(uint16_t*)(s + 0x372);
                for (uint16_t si_v = 6; (int16_t)si_v < (int16_t)te; si_v += 2) {
                    if (*(uint16_t*)(s + si_v + 0x1355) == 0) continue;
                    if (*(uint16_t*)(s + si_v + 0x1585) & 0x800) continue;
                    uint16_t ox = *(uint16_t*)(s + si_v + 0x173D);
                    uint16_t oy = *(uint16_t*)(s + si_v + 0x1765);
                    uint16_t obx = *(uint16_t*)(s + si_v + 0x14BD);
                    uint16_t oby = *(uint16_t*)(s + si_v + 0x1495);
                    bool outside = false;
                    if ((int16_t)(ox + obx - *(uint16_t*)(s + 0x34)) < 0) outside = true;
                    else if ((int16_t)(ox - obx - *(uint16_t*)(s + 0x36)) >= 0) outside = true;
                    else if ((int16_t)(oy + oby - *(uint16_t*)(s + 0x38)) < 0) outside = true;
                    else if ((int16_t)(oy - oby - *(uint16_t*)(s + 0x3A)) >= 0) outside = true;
                    if (outside)
                        *(uint16_t*)(s + si_v + 0x1585) |= 0x200;
                }
            }
            // sub_12fd0: sub-sprite position delta type 2
            {
                auto delta_type2 = [](int16_t d) -> int16_t {
                    if (d == 0) return 0;
                    int16_t a = (d < 0) ? -d : d;
                    int16_t q = a / 3;
                    return (d < 0) ? -(int16_t)q : q;
                };
                for (int16_t di3 = (int16_t)*(uint16_t*)(s + 0x372) - 2; di3 >= 0; di3 -= 2) {
                    if (*(uint16_t*)(s + di3 + 0x1355) == 0) continue;
                    if (*(uint16_t*)(s + di3 + 0x1AD5) == 0) continue;
                    int16_t dy = (int16_t)(*(uint16_t*)(s + di3 + 0x1765) - *(uint16_t*)(s + di3 + 0x13CD));
                    int16_t dx_v = (int16_t)(*(uint16_t*)(s + di3 + 0x173D) - *(uint16_t*)(s + di3 + 0x13A5));
                    int16_t ty = delta_type2(dy), tx = delta_type2(dx_v);
                    if (tx == 0 && ty == 0) continue;
                    uint16_t ss_end = *(uint16_t*)(s + di3 + 0x1AAD);
                    for (uint16_t si2 = *(uint16_t*)(s + di3 + 0x1A85); (int16_t)si2 < (int16_t)ss_end; si2 += 2) {
                        *(uint16_t*)(s + si2 + 0x64D) += (uint16_t)tx;
                        *(uint16_t*)(s + si2 + 0x74D) += (uint16_t)ty;
                        *(uint16_t*)(s + si2 + 0x114D) = 0x202;
                    }
                }
            }
            // sub_11792: HUD full update (original: sub_120ff, sub_12199, loc_1205b, sub_11B0B)
            if ((s[0x25CF] & 1) && *(uint16_t*)(s + 0x25AD) != 0x2C) {
                // sub_120ff: healthbar state tracking (3 vikings)
                for (int vk = 0; vk < 3; vk++) {
                    uint16_t prev = *(uint16_t*)(s + 0x0435 + vk * 2);
                    *(uint16_t*)(s + 0x043B + vk * 2) = prev;
                    int16_t health_val = (int16_t)*(uint16_t*)(s + 0x16ED + vk * 2);
                    uint16_t ax;
                    if (health_val < 0) ax = 2;
                    else if (*(uint16_t*)(s + 0x3C2) != (uint16_t)(vk * 2)) ax = 1;
                    else ax = 0;
                    *(uint16_t*)(s + 0x0435 + vk * 2) = ax;
                    if (ax != prev)
                        v2_draw_hud_healthbar(v2_current_ds_val, ax, vk, vk);
                }
                // sub_12199: HUD items refresh — rendering only, no DS side effects beyond what sub_1201d did
                // loc_1205b: HUD item selector state sync. Verified with seg000 lines 4893-4922.
                // Compares word_288F4/F6/F8 with word_288FA/FC/FE. If different: update + render.
                // DS writes: word_288FA (0x041A), word_288FC (0x041C), word_288FE (0x041E).
                for (int vk_s = 0; vk_s < 3; vk_s++) {
                    uint16_t cur = *(uint16_t*)(s + 0x0414 + vk_s * 2);  // word_288F4/F6/F8
                    uint16_t prev = *(uint16_t*)(s + 0x041A + vk_s * 2); // word_288FA/FC/FE
                    if (cur != prev) {
                        // sub_1183d: render old selector (clear). sub_118ad: render new selector.
                        // These are VGA HUD rendering — v2 uses v2_draw_hud_selector instead.
                        *(uint16_t*)(s + 0x041A + vk_s * 2) = cur;
                        v2_draw_hud_selector(v2_current_ds_val, (cur + vk_s * 4) * 2);
                    }
                }
                // sub_11B0B: portrait/sound state sync + render (3 vikings)
                // Moved here from sub_117ad where it was INCORRECTLY placed.
                for (int vk = 0; vk < 3; vk++) {
                    uint16_t sound_prev = *(uint16_t*)(s + 0x0429 + vk * 2);
                    uint16_t sound_cur  = *(uint16_t*)(s + 0x042F + vk * 2);
                    uint16_t port_prev  = *(uint16_t*)(s + 0x15AD + vk * 2);
                    uint16_t port_cur   = *(uint16_t*)(s + 0x0423 + vk * 2);
                    if (sound_prev != sound_cur || port_prev != port_cur) {
                        uint16_t portrait_si = port_prev;
                        if (sound_prev != 0) portrait_si += 4;
                        v2_draw_hud_portrait(v2_current_ds_val, vk * 2, portrait_si);
                        *(uint16_t*)(s + 0x042F + vk * 2) = sound_prev;
                        *(uint16_t*)(s + 0x0423 + vk * 2) = port_prev;
                    }
                }
            }
            // CALLF sub_1DE05 (PASS 3)
            v2_sub_1DE05(s);
            if (new_level == 0x002B && v2_m2c_base) {
                uint16_t fs_seg = *(uint16_t*)(v2_vm_real_ds_ptr + 0x2E69);
                if (fs_seg != 0) {
                    uint8_t* rfs = v2_m2c_base + ((uint32_t)fs_seg << 4);
                    int d3 = 0;
                    for (uint32_t i = 0; i < 0x2B * 0x19 * 2 && d3 < 3; i += 2) {
                        uint16_t rv = *(uint16_t*)(rfs + i), sv = *(uint16_t*)(v2_vm_shadow_fs + i);
                        if (rv != sv) { if(d3==0) fprintf(stderr,"V2-FS-SF3-DE05:\n");
                            fprintf(stderr,"  FS[%04X]: r=%04X s=%04X\n",(uint16_t)i,rv,sv); d3++; }
                    }
                    fprintf(stderr,"V2-FS-SF3-DE05: %d diffs, mode=%02X\n",d3,s[0x117D]);
                }
            }
            // sub_165aa + sub_16661: despawn + scroll tracking
            v2_game_loop_post_render(s);
            // FS compare BEFORE SF3 DD9C — check specific offset 0x5586
            if (new_level == 0x002B && v2_m2c_base) {
                uint16_t fs_seg = *(uint16_t*)(v2_vm_real_ds_ptr + 0x2E69);
                if (fs_seg != 0) {
                    uint8_t* rfs = v2_m2c_base + ((uint32_t)fs_seg << 4);
                    uint16_t sv = *(uint16_t*)(v2_vm_shadow_fs + 0x5586);
                    uint16_t rv = *(uint16_t*)(rfs + 0x5586);
                    fprintf(stderr,"V2-FS-BEFORE-SF3: fs[5586] s=%04X r=%04X (bit0: s=%d r=%d) (bit1: s=%d r=%d)\n",
                        sv, rv, sv&1, rv&1, (sv>>1)&1, (rv>>1)&1);
                }
            }
            // CALLF sub_1DD9C
            v2_sub_1DD9C(s);
            { static int _dd3=0; _dd3++; if(_dd3<=8) fprintf(stderr,"V2-115d2-DD9C[sf3-%d]: mode=%02X force=%02X\n",_dd3,s[0x117D],s[0x9568]); }
            // CALLF sub_1dd9c; // seg003: sprite render — v2 full-frame
            // CALLF sub_1c8f1; // seg003: door/flagged tile render — v2 full-frame
            // sub_16775: PAGE FLIP 4
            v2_sub_16775(s);
        }
        // After sub_115d2: 4th post-render block (eip 0x165C..0x1677)
        // CALLF sub_1DE05 (4th)
        v2_sub_1DE05(s);
        // sub_1de05_dirty_update_position(NULL); // seg003 recreated
        // sub_1c8f1_door_rendering_with_state(_state); // seg003 recreated
        // sub_165aa + sub_16661:
        v2_game_loop_post_render(s);
        // CALLF sub_1DD9C
        v2_sub_1DD9C(s);
        // CALLF sub_1dd9c; // seg003: sprite render — v2 full-frame
        // CALLF sub_1c8f1; // seg003: door/flagged tile render — v2 full-frame
        v2_sub_16775(s); // PAGE FLIP 5 (tail jmp sub_16775 at eip 0x1677)
    }
    // Verify sub_115d2 output: compare v2 shadow with orig snapshot
    if (v2_115d2_snapshot_valid) {
        v2_115d2_snapshot_valid = false; // one-shot
        uint8_t* snap = v2_115d2_snapshot;
        int dc = 0;
        for (uint32_t i = 0; i < 0x10000 && dc < 20; i += 2) {
            // Skip known exclusions
            if (i == 0xA39C) continue; // word_3287C (VGA interrupt race)
            if (i >= 0x990C && i <= 0x991E) continue; // AIL sound handles
            if (i == 0x9934) continue; // XMI buffer
            uint16_t rv = *(uint16_t*)(snap + i), sv = *(uint16_t*)(s + i);
            if (rv != sv) {
                if (dc == 0) fprintf(stderr, "V2-115d2-VERIFY: level=%04X DIFFS:\n",
                    *(uint16_t*)(s + 0x25AD));
                fprintf(stderr, "  DS[%04X]: orig=%04X v2=%04X\n", (uint16_t)i, rv, sv);
                dc++;
            }
        }
        if (dc == 0) fprintf(stderr, "V2-115d2-VERIFY: level=%04X MATCH (0 diffs)\n",
            *(uint16_t*)(s + 0x25AD));
        else fprintf(stderr, "V2-115d2-VERIFY: %d diffs total\n", dc);
        fflush(stderr);
    }
    // sub_10f5d: palette fade in. Loop bx from 0x46 to 0, applying palette shading.
    // Each iteration: shade bytes, sub_10f03 (palette → ds:0x8202), render, vsync.
    for (int16_t bx = 0x46; bx >= 0; bx--) {
        s[0x0342] = (uint8_t)bx;
        s[0x0343] = (uint8_t)bx;
        s[0x0344] = (uint8_t)bx;
        v2_sub_10f03(s); // sub_10f03: palette shading → ds:0x8202
        *(uint16_t*)(s + 0x7EFE) = 4;       // word_303DE = 4 (request palette write)
        *(uint16_t*)(s + 0x7F00) = 0x8202;  // word_303E0
        // sub_16775: render + palette write + vsync
        v2_do_render_and_swap();
        v2_sub_10fe6(s); // sub_10fe6: palette → VGA DAC, word_303DE = 0
    }
    // After fade-in: clear shade, set normal palette mode
    s[0x0342] = 0;
    s[0x0343] = 0;
    s[0x0344] = 0;
    *(uint16_t*)(s + 0x7F00) = 0x7F02; // word_303E0 = normal palette mode

    // jmp sub_12345: input state clear (tail call at end of sub_11080)
    // sub_12345: mov word_28896, 0; mov word_28898, 0; retn
    *(uint16_t*)(s + 0x03B6) = 0; // word_28896
    *(uint16_t*)(s + 0x03B8) = 0; // word_28898

    printf("V2: sub_11080 level init complete for level %d\n", new_level);
    if (new_level == 0x002B) {
        v2_hw_wp_drain(); // uncomment arm above to enable
        fprintf(stderr, "V2-POST-INIT-002B: s[117D]=%04X s[117F]=%04X r[117D]=%04X r[117F]=%04X\n",
            *(uint16_t*)(s+0x117D), *(uint16_t*)(s+0x117F),
            v2_vm_real_ds_ptr ? *(uint16_t*)(v2_vm_real_ds_ptr+0x117D) : 0xDEAD,
            v2_vm_real_ds_ptr ? *(uint16_t*)(v2_vm_real_ds_ptr+0x117F) : 0xDEAD);
    }
    // Debug: dump viking sub-sprite ranges and compare with real DS
    if (v2_vm_real_ds_ptr && new_level >= 0x002B) {
        for (int v = 0; v < 6; v += 2) {
            uint16_t s_start = *(uint16_t*)(s + v + 0x1A85);
            uint16_t s_end = *(uint16_t*)(s + v + 0x1AAD);
            uint16_t r_start = *(uint16_t*)(v2_vm_real_ds_ptr + v + 0x1A85);
            uint16_t r_end = *(uint16_t*)(v2_vm_real_ds_ptr + v + 0x1AAD);
            printf("V2-INIT: viking %d: s[%04X..%04X] r[%04X..%04X]  ds:0x374=%04X\n",
                v/2, s_start, s_end, r_start, r_end, *(uint16_t*)(s + 0x374));
        }
    }
}

// v2 level loader — split into 2 phases matching original call order.
// Phase 1: sub_111b1 (template + level header)
// Phase 2: sub_11204 (tile/graphics chunks)
// Original order in sub_11080: sub_111b1 → sub_111df → sub_12ce4 → sub_117ad → sub_11204
static uint16_t v2_current_level = 0xFFFF;

// sub_111b1: load template chunk + level header chunk
static void v2_load_template(uint8_t* shadow) {
    uint16_t level = *(uint16_t*)(shadow + 0x25C9); // word_2AAA9
    v2_current_level = level;
    uint16_t di_idx = level * 2;
    uint16_t level_chunk = *(uint16_t*)(shadow + (uint16_t)(di_idx - 0x6BF4));
    uint16_t template_chunk = *(uint16_t*)(shadow + (uint16_t)(di_idx - 0x6B94));
    printf("V2-TEMPLATE: level=%d di_idx=0x%X table_off=0x%04X level_chunk=0x%X template=0x%X\n",
           level, di_idx, (uint16_t)(di_idx - 0x6BF4), level_chunk, template_chunk);

    // Load template → shadow animation data (ds:0x2E67)
    // Exact replica of sub_111b1: CMP ax, word_2B351; JZ skip
    if (template_chunk != 0xFFFF && template_chunk != *(uint16_t*)(shadow + 0x2E71)) {
        *(uint16_t*)(shadow + 0x2E71) = template_chunk;
        uint32_t sz = v2_read_chunk(template_chunk, v2_vm_shadow_animdata, V2_ANIMDATA_SHADOW_SIZE);
        if (sz > 0) { v2_animdata_shadow_valid = true; }
    }

    // Load level chunk → DS at offset 0x25B3 (level header)
    uint32_t lsz = v2_read_chunk(level_chunk, shadow + 0x25B3, 0x10000 - 0x25B3);
    printf("V2-TEMPLATE: loaded level chunk %d bytes. byte_2AA9A(0x25BA)=%02X, level_id=%04X\n",
           lsz, shadow[0x25BA], *(uint16_t*)(shadow + 0x25AD));
}

// sub_11204: load level data chunks (tile graphics, tilemap, etc.)
static void v2_load_level_data(uint8_t* shadow) {
    // Do NOT clear shadow segments — init_shadow_early copied the same garbage
    // as real segments. v2_read_chunk overwrites the decompressed portion.
    // Unused portions stay as garbage, matching real segments.
    uint8_t flags = shadow[0x25CF];
    if (flags & 2) {
        // loc_11254: Intro type 1 — chunk level
        uint16_t chunk = *(uint16_t*)(shadow + 0x25E1);
        v2_read_chunk(chunk, v2_vm_shadow_chunk, V2_CHUNK_SHADOW_SIZE);
        v2_chunk_shadow_valid = true;
    } else if (flags & 0x40) {
        // loc_1127b: Intro type 2 — chunk level
        uint16_t chunk = *(uint16_t*)(shadow + 0x25E1);
        v2_read_chunk(chunk, v2_vm_shadow_chunk, V2_CHUNK_SHADOW_SIZE);
        v2_chunk_shadow_valid = true;
    } else if (flags & 0x20) {
        // loc_112a2: HUD-only + tile loading — falls through to loc_1121b
        // sub_10cd8(0x211, 0): load HUD chunk
        // Then falls through to tile loading below
        goto tile_load;
    } else {
tile_load:
        // loc_1121b: Normal tile-based level — 4 decompressions
        uint16_t tile_chunk = *(uint16_t*)(shadow + 0x25E3); // word_2AAC3
        uint16_t main_chunk = *(uint16_t*)(shadow + 0x25E1); // word_2AAC1
        uint16_t bg_chunk = *(uint16_t*)(shadow + 0x25E5);   // word_2AAC5

        printf("V2-LOAD: tile_chunk=0x%X main_chunk=0x%X bg_chunk=0x%X\n", tile_chunk, main_chunk, bg_chunk);
        // 1. tile_chunk → tile graphics (ds:0x2E5F)
        uint32_t sz1 = v2_read_chunk(tile_chunk, v2_vm_shadow_tilegfx, V2_TILEGFX_SHADOW_SIZE);
        if (sz1 > 0) { v2_tilegfx_shadow_valid = true; }
        printf("V2-LOAD: tilegfx: chunk=0x%X sz=%d first4=%02X%02X%02X%02X\n",
               tile_chunk, sz1, v2_vm_shadow_tilegfx[0], v2_vm_shadow_tilegfx[1],
               v2_vm_shadow_tilegfx[2], v2_vm_shadow_tilegfx[3]);

        // 2. tile_chunk+1 → GS masks (ds:0x2E61)
        uint32_t sz2 = v2_read_chunk(tile_chunk + 1, v2_vm_shadow_gs, V2_GS_SHADOW_SIZE);
        if (sz2 > 0) { v2_gs_shadow_valid = true; }
        printf("V2-LOAD: gs: chunk=0x%X sz=%d\n", tile_chunk + 1, sz2);

        // 3. main_chunk → tilemap (ds:0x2E63)
        uint32_t sz3 = v2_read_chunk(main_chunk, v2_vm_shadow_tilemap, V2_TILEMAP_SHADOW_SIZE);
        if (sz3 > 0) {
            v2_tilemap_shadow_valid = true;
            *(uint16_t*)(shadow + 0x2E65) = (uint16_t)sz3; // word_2B345
        }

        // 4. bg_chunk → GS tile data (ds:0x2E5D) — tail call jmp sub_10982
        uint32_t sz4 = v2_read_chunk(bg_chunk, v2_vm_shadow_gs_tiledata, V2_GS_TILEDATA_SIZE);
        if (sz4 > 0) { v2_gs_tiledata_valid = true; }
    }
}

// Combined loader for backward compatibility (called from game loop)
static void v2_load_level(uint8_t* shadow) {
    uint16_t level = *(uint16_t*)(shadow + 0x25C9);
    if (level == v2_current_level) return;
    v2_load_template(shadow);
    v2_load_level_data(shadow);
}

// Shadow state management
// v2_vm_real_ds_ptr declared at line ~131 (before startup functions that use it)
static bool v2_shadow_initialized = false;

// Load pristine EXE data segment from ds_static.bin into shadow DS.
// This contains static data only: lookup tables, VGA constants, slope tables, etc.
// NO runtime data (no segments, no PRNG, no DATA.DAT header).
// After this, v2_startup() fills runtime data via sub_12948..sub_108b8.
static bool v2_load_exe_ds() {
    FILE* f = fopen("ds_static.bin", "rb");
    if (!f) {
        printf("V2: ERROR: cannot open ds_static.bin\n");
        return false;
    }
    size_t read = fread(v2_vm_shadow_ds, 1, V2_VM_SHADOW_SIZE, f);
    fclose(f);
    if (read != V2_VM_SHADOW_SIZE) {
        printf("V2: ERROR: ds_static.bin: read %zu bytes, expected %d\n", read, V2_VM_SHADOW_SIZE);
        return false;
    }
    v2_vm_acc_base = v2_vm_shadow_ds;
    v2_shadow_initialized = true;
    printf("V2: loaded ds_static.bin (%zu bytes), ds[0x945A]=%04X\n",
           read, *(uint16_t*)(v2_vm_shadow_ds + 0x945A));
    return true;
}

// Legacy API: copy real DS + segments from original emulator.
// Still used for segment data copy and for verification (v2_vm_real_ds_ptr).
void v2_vm_init_shadow_early(uint16_t ds_val) {
    if (!v2_m2c_base) return;
    uint8_t* ds = v2_m2c_base + ((uint32_t)ds_val << 4);
    v2_vm_real_ds_ptr = ds;
    v2_current_ds_val = ds_val;
    // DS is NOT copied — loaded from ds_static.bin + v2_startup instead.
    // Only set real_ds_ptr for verification comparisons.
    printf("V2: init_shadow_early: real DS at 0x%04X (for verify only)\n", ds_val);
}

void v2_vm_run_init(uint16_t ds_val) {
    if (!v2_shadow_initialized) return;
    v2_vm_init_table(); // ensure VM opcode table initialized before sub_115d2
    v2_current_ds_val = ds_val;
    v2_vm_real_ds_ptr = v2_m2c_base + ((uint32_t)ds_val << 4);
    printf("V2: running v2_sub_11080 on shadow DS (level=%d)...\n", v2_current_level);
    printf("V2: shadow seg ptrs: ES=%04X TG=%04X AN=%04X GS=%04X FS=%04X CH=%04X\n",
           *(uint16_t*)(v2_vm_shadow_ds + 0x2E63), *(uint16_t*)(v2_vm_shadow_ds + 0x2E5F),
           *(uint16_t*)(v2_vm_shadow_ds + 0x2E67), *(uint16_t*)(v2_vm_shadow_ds + 0x2E61),
           *(uint16_t*)(v2_vm_shadow_ds + 0x2E69), *(uint16_t*)(v2_vm_shadow_ds + 0x2E77));
    v2_sub_11080(v2_vm_shadow_ds);
    printf("V2: v2_sub_11080 complete, level=%d\n", v2_current_level);
}

// Forward declaration: collision VM runs from sub_15546 in game loop.
// Actual execution deferred to v2_run_collision_vm() called after V2VM is defined.
static void v2_run_collision_vm(uint8_t* shadow, uint16_t si);

// ============================================================================
// V2 Game Loop — replicate pre-VM procedures on shadow DS.
// Most check flags and return immediately. Only active ones modify state.
// ============================================================================
static void v2_do_render_and_swap(); // forward decl
// v2_current_ds_val declared at top (forward declarations section)


static void v2_game_loop_pre_vm(uint8_t* shadow, uint16_t ds_val) {
    // sub_12352: input processing. Exact replica.
    // ax = 0
    // if ds:0x86DA != 0: call sub_12ef8 (replay), ax = ds:0x86DC
    // ax |= ds:0x86DE (accumulated transitions)
    // ax |= input_keys_v2 (SDL keyboard state for v2 window)
    // ds:0x03B6 = ax (current input)
    // ds:0x03B8 = (ax ^ ds:0x03BA) & ax (newly pressed)
    // ds:0x03BA = ax (previous frame)
    {
        uint16_t ax = 0;
        if (*(uint16_t*)(shadow + 0x86DA) != 0) {
            // Replay mode — not implemented in v2
            ax = *(uint16_t*)(shadow + 0x86DC);
        }
        ax |= *(uint16_t*)(shadow + 0x86DE);
        ax |= v2_input_snapshot; // Snapshot of input_keys taken at barrier sync point
        { static int _inp = 0; _inp++; if (_inp <= 20)
            fprintf(stderr, "V2-INPUT[%d]: 86DE=%04X snapshot=%04X ax=%04X prev=%04X\n",
                    _inp, *(uint16_t*)(shadow + 0x86DE), v2_input_snapshot, ax,
                    *(uint16_t*)(shadow + 0x03BA)); }
        *(uint16_t*)(shadow + 0x03B6) = ax;
        uint16_t prev = *(uint16_t*)(shadow + 0x03BA);
        *(uint16_t*)(shadow + 0x03B8) = (ax ^ prev) & ax;
        *(uint16_t*)(shadow + 0x03BA) = ax;
    }

    // sub_12d72: level transition handler.
    // ds:0x3CC = transition state. If >= 0 → return. If negative → process.
    {
        int16_t ax = (int16_t)*(uint16_t*)(shadow + 0x3CC);
        if (ax >= 0) {
            // Normal: no transition active
        } else if (ax == (int16_t)0x8000 || ax == (int16_t)0x8002) {
            // loc_12dd2: countdown transition
            if (*(uint16_t*)(shadow + 0x3B6) & 0x1000) {
                *(uint16_t*)(shadow + 0x3CE) = 0xFFFF;
                *(uint16_t*)(shadow + 0x3D0) = 0xFFFF;
            }
            if (*(uint16_t*)(shadow + 0x3CE) != 0) {
                *(uint16_t*)(shadow + 0x3CE) -= 1;
                *(uint16_t*)(shadow + 0x86DE) |= *(uint16_t*)(shadow + 0x3D0);
            } else {
                // Pop from stack at ds:0x2191
                uint16_t bx = *(uint16_t*)(shadow + 0x2191);
                *(uint16_t*)(shadow + 0x86DE) = *(uint16_t*)(shadow + bx + 0x2191);
                *(uint16_t*)(shadow + 0x3D0) = *(uint16_t*)(shadow + bx + 0x2191);
                uint16_t cnt = *(uint16_t*)(shadow + bx + 0x2193);
                *(uint16_t*)(shadow + 0x3CE) = cnt - 1;
                *(uint16_t*)(shadow + 0x2191) = bx + 4;
            }
        } else {
            // Other negative: active viking tracking
            if (*(uint16_t*)(shadow + 0x3CE) == 0) {
                // loc_12dc4: init tracking
                *(uint16_t*)(shadow + 0x3D0) = *(uint16_t*)(shadow + 0x3B6);
                *(uint16_t*)(shadow + 0x3CE) = 1;
            } else {
                // Check if active viking changed
                uint16_t cur = *(uint16_t*)(shadow + 0x3B6);
                uint16_t prev = *(uint16_t*)(shadow + 0x3D0);
                if (cur == prev) {
                    *(uint16_t*)(shadow + 0x3CE) += 1;
                } else {
                    // Push previous state to stack at ds:0x2191
                    // bx = ds:0x2191 (stack pointer, loaded ONCE)
                    // [bx+0x2191] = prev; ds:0x2191 += 2
                    // [bx+0x2191] = counter; ds:0x2191 += 2
                    // Note: bx is NOT reloaded after ADD — same bx for both writes!
                    // So second write goes to same address, overwriting first.
                    // Actually: bx+0x2191 for first, (bx)+0x2191 for second (bx unchanged)
                    // The ADD changes ds:0x2191 memory, not bx register.
                    uint16_t bx = *(uint16_t*)(shadow + 0x2191);
                    *(uint16_t*)(shadow + (uint16_t)(bx + 0x2191)) = prev;
                    *(uint16_t*)(shadow + 0x2191) += 2;
                    *(uint16_t*)(shadow + (uint16_t)(bx + 0x2191)) = *(uint16_t*)(shadow + 0x3CE);
                    *(uint16_t*)(shadow + 0x2191) += 2;
                    *(uint16_t*)(shadow + 0x3D0) = cur;
                    *(uint16_t*)(shadow + 0x3CE) = 1;
                }
            }
        }
    }

    // sub_102ad: level transition trigger (eip 0x02AD..0x034F).
    // Checks word_288AC (0x3CC) for negative, and word_28896 (0x3B6) for 0x1000 button.
    {
        int16_t ac = (int16_t)*(uint16_t*)(shadow + 0x3CC); // word_288AC
        { static int _td = 0; if (ac < 0 && _td < 10) { _td++;
          printf("V2-DBG-102ad: ac=%04X input=%04X level=%04X 0334=%04X\n",
            (uint16_t)ac, *(uint16_t*)(shadow + 0x3B6), *(uint16_t*)(shadow + 0x25AD),
            *(uint16_t*)(shadow + 0x0334)); } }
        if (ac < 0) {
            if (*(uint16_t*)(shadow + 0x3B6) & 0x1000) { // word_28896
                uint16_t ac_u = (uint16_t)ac;
                if (ac_u == 0x8000) {
                    // loc_102f0: level-specific dispatch
                    uint16_t level = *(uint16_t*)(shadow + 0x25AD); // word_2AA8D
                    if (level == 0x2B) {
                        // loc_10309: check word_28896 == 0xFFFF
                        if (*(uint16_t*)(shadow + 0x3B6) != 0xFFFF) {
                            // loc_10344: word_288AC = 1
                            *(uint16_t*)(shadow + 0x3CC) = 1;
                        } else {
                            *(uint16_t*)(shadow + 0x25C9) = 0; // word_2AAA9 = 0
                            // fall through to loc_10317
                            *(uint16_t*)(shadow + 0x3C2) = 0;  // word_288A2 = 0
                            *(uint16_t*)(shadow + 0x3CC) = 0;  // word_288AC = 0 (loc_1033c)
                        }
                    } else if (level == 0x2C) {
                        // loc_10317: word_288A2 = 0; jmp loc_1033c
                        *(uint16_t*)(shadow + 0x3C2) = 0;  // word_288A2
                        *(uint16_t*)(shadow + 0x3CC) = 0;  // word_288AC (loc_1033c)
                    } else if (level == 0x2D) {
                        // loc_10344: word_288AC = 1
                        *(uint16_t*)(shadow + 0x3CC) = 1;
                    } else if (level == 0x2E) {
                        // loc_10336: word_2AAA9 = 0x27; → loc_1033c
                        *(uint16_t*)(shadow + 0x25C9) = 0x27;
                        *(uint16_t*)(shadow + 0x3CC) = 0; // loc_1033c
                    } else {
                        // loc_1031f: table lookup
                        uint16_t si = *(uint16_t*)(shadow + 0x3D4); // word_288B4
                        si += 2;
                        uint16_t ax = *(uint16_t*)(shadow + si + 0x2B66);
                        if (ax == 0xFFFF) si = 0;
                        *(uint16_t*)(shadow + 0x3D4) = si; // word_288B4
                        // loc_10336: word_2AAA9 = 0x27; → loc_1033c
                        *(uint16_t*)(shadow + 0x25C9) = 0x27;
                        *(uint16_t*)(shadow + 0x3CC) = 0; // loc_1033c
                    }
                } else if (ac_u == 0x8002) {
                    // loc_102e8: reload current level
                    *(uint16_t*)(shadow + 0x25C9) = *(uint16_t*)(shadow + 0x25AD);
                } else {
                    // First time: write command buffer, set 0x8002, fall through to loc_102e8
                    uint16_t bx = *(uint16_t*)(shadow + 0x2191); // word_2A671
                    *(uint16_t*)(shadow + (uint16_t)(bx + 0x2193)) = 0x1000;
                    *(uint16_t*)(shadow + (uint16_t)(bx + 0x2195)) = 0xFFFF;
                    *(uint16_t*)(shadow + 0x2191) += 2;
                    *(uint16_t*)(shadow + 0x3CC) = 0x8002; // word_288AC
                    // loc_102e8: reload current level
                    *(uint16_t*)(shadow + 0x25C9) = *(uint16_t*)(shadow + 0x25AD);
                }
                // loc_1034a: OR(word_28814, 1)
                *(uint16_t*)(shadow + 0x0334) |= 1;
            }
        }
    }


    // sub_1041c: check byte_2AA9A (game active flag). If 0 → return.
    // Viking switching on pause screen — only when paused. Skip for v2.

    // sub_10138: check word_28814 (DS:0x0334) for button presses.
    // bit 4: viking switch screen (blocking loop — cannot replicate in v2 frame callback).
    // bit 1: clear buttons, sub_1774f (palette), INC word_2880F, sub_14207 (VM!), sub_11080 (cycle viking).
    // bit 2: set word_2AAA9=0x25, then same as bit 1.
    // No bits set → RETN.
    // For v2: exact state changes. VM call cannot be nested (v2 runs before original).
    // sub_11080 viking cycling modifies ds:0x3C2 — done by original, synced via memcpy.
    {
        uint16_t buttons = *(uint16_t*)(shadow + 0x0334);
        if (buttons & 4) {
            // bit 4: viking switch screen. Verified with seg000 lines 2054-2074.
            // Original: AND word_28814, 0xFFFB; then blocking loop:
            //   sub_12352 → test 0xC0C0 → sub_101be → 3× (sub_16775 + sub_10130 + sub_108c8) → loop
            // For v2: blocking loop with SDL input + render passes.
            *(uint16_t*)(shadow + 0x0334) &= 0xFFFB;              // AND word_28814, 0FFFBh
            // loc_10169: blocking viking switch loop
            while (true) {
                // sub_12352: input processing
                {
                    extern uint16_t v2_input_snapshot;
                    uint16_t ax = 0;
                    if (*(uint16_t*)(shadow + 0x86DA) != 0)
                        ax = *(uint16_t*)(shadow + 0x86DC);
                    ax |= *(uint16_t*)(shadow + 0x86DE);
                    ax |= v2_input_snapshot;
                    *(uint16_t*)(shadow + 0x03B6) = ax;
                    uint16_t prev = *(uint16_t*)(shadow + 0x03BA);
                    *(uint16_t*)(shadow + 0x03B8) = (ax ^ prev) & ax;
                    *(uint16_t*)(shadow + 0x03BA) = ax;
                }
                // TEST word_28898, 0xC0C0 — exit condition
                if (*(uint16_t*)(shadow + 0x3B8) & 0xC0C0) break; // JNZ loc_10191
                // sub_101be: viking switch state update (VGA + DS)
                // For now: DS writes only, VGA rendering skipped
                // 3× render passes with sub_16775 + sub_10130 + sub_108c8
                for (int pass = 0; pass < 3; pass++) {
                    v2_sub_16775(shadow);                          // sub_16775
                    v2_sub_10130(shadow); // sub_10130: VGA vsync wait
                    v2_sub_108c8(shadow); // sub_108c8: audio handler
                }
                // v2: render + delay for ~60fps
                v2_do_render();
                SDL_Delay(16);
            }
            // loc_10191: JMP sub_12352 (final input read)
            {
                extern uint16_t v2_input_snapshot;
                uint16_t ax = 0;
                ax |= *(uint16_t*)(shadow + 0x86DE);
                ax |= v2_input_snapshot;
                *(uint16_t*)(shadow + 0x03B6) = ax;
                uint16_t prev = *(uint16_t*)(shadow + 0x03BA);
                *(uint16_t*)(shadow + 0x03B8) = (ax ^ prev) & ax;
                *(uint16_t*)(shadow + 0x03BA) = ax;
            }
        } else if (buttons & 2) {
            // bit 2: set word_2AAA9=0x25, then fall through to bit 1 processing.
            *(uint16_t*)(shadow + 0x25C9) = 0x25; // word_2AAA9
            // fall through
        }
        // Check bit 1 (also reached by bit 2 fall-through):
        if ((buttons & 3) && !(buttons & 4)) {
            // Original: clear buttons, sub_1774f, INC word_2880F, sub_14207, JMP sub_11080.
            *(uint16_t*)(shadow + 0x0334) = 0;
            // sub_1774f: palette fade — VGA OUT only, no DS writes needed.
            *(uint16_t*)(shadow + 0x032F) += 1; // INC word_2880F
            // sub_14207: full VM pass (with priority object loop)
            // TRANSITION VERIFY: compare sub-sprite Y before VM, after VM, after sub_11080
            {   // Wider verify: check ALL sub-sprite Y range (0x74D..0x84D) + page copies
                auto _tv = [&](const char* label) {
                    if (!v2_vm_real_ds_ptr) return;
                    uint8_t* r = v2_vm_real_ds_ptr;
                    int dc = 0;
                    for (uint16_t off = 0x74D; off < 0x84D && dc < 10; off += 2) {
                        uint16_t rv = *(uint16_t*)(r + off), sv = *(uint16_t*)(shadow + off);
                        if (rv != sv) {
                            if (dc == 0) fprintf(stderr, "V2-TRANSITION[%s] level=%04X diffs:\n", label, *(uint16_t*)(shadow+0x25AD));
                            fprintf(stderr, "  0x%04X: orig=%04X v2=%04X\n", off, rv, sv);
                            dc++;
                        }
                    }
                    if (dc == 0) fprintf(stderr, "V2-TRANSITION[%s]: sub-sprite Y MATCH\n", label);
                    else {
                        fprintf(stderr, "V2-TRANSITION[%s]: %d diffs total\n", label, dc);
                        // Also dump full DS diff around sub-sprite area for analysis
                        fprintf(stderr, "  Full DS diff (first 20 mismatches):\n");
                        int fc = 0;
                        for (uint32_t i = 0; i < 0x10000 && fc < 20; i += 2) {
                            uint16_t rv2 = *(uint16_t*)(r + i), sv2 = *(uint16_t*)(shadow + i);
                            if (rv2 != sv2 && !(i >= 0x990C && i <= 0x991E)) {
                                fprintf(stderr, "    DS[%04X]: orig=%04X v2=%04X\n", (uint16_t)i, rv2, sv2);
                                fc++;
                            }
                        }
                        fflush(stderr);
                        // Continue instead of exit — transition diffs expected
                    }
                    fflush(stderr);
                };
                _tv("before-VM");
            v2_sub_14207_init(shadow);
            {
                // Re-read ds:0x372 each iteration — VM can create objects (op_14)
                for (uint16_t si_v = 0; si_v < *(uint16_t*)(shadow + 0x372); si_v += 2) {
                    v2_vm_execute_object(shadow, si_v);
                    uint16_t prio = *(uint16_t*)(shadow + 0x376);
                    if (prio != 0) {
                        for (uint16_t di = 0; (int16_t)di < (int16_t)prio; di++) {
                            uint16_t pobj = *(uint16_t*)(shadow + di + 0x378) & 0xFF;
                            v2_vm_execute_object(shadow, pobj);
                        }
                        *(uint16_t*)(shadow + 0x376) = 0;
                    }
                }
            }
            // Exact original flow: sub_10138 → VM(old level) → JMP sub_11080 → JMP sub_12345
            // → RETN(0x002D) → sub_11ba5..sub_1673c → sub_14207(VM on new level).
            // v2 replicates: v2_sub_11080 loads new level, sub_12345 clears input.
            // V2_PHASE_VM will run VM on new level (frame_active stays true).
            // sub_11ba5..sub_1673c: mostly NOPs for v2 (pause, resource loading, sound).
            _tv("after-VM");
            v2_sub_11080(shadow);
            _tv("after-11080");
            if (v2_vm_real_ds_ptr) {
                fprintf(stderr, "V2-TRANSITION: table_end after-11080: shadow=%04X orig=%04X\n",
                    *(uint16_t*)(shadow + 0x372), *(uint16_t*)(v2_vm_real_ds_ptr + 0x372));
            }
            } // end TRANSITION VERIFY block
            // sub_12345 equivalent already done inside v2_sub_11080 (clears 0x03B6, 0x03B8).
            // Update v2_current_level so FRAME_BEGIN doesn't re-run v2_sub_11080.
            v2_current_level = *(uint16_t*)(shadow + 0x25AD);
            // Original flow: after sub_11080, game loop restarts from loc_1001e.
            // Second pass runs sub_12352..sub_1673c on the NEW level BEFORE VM.
            // sub_1673c updates scroll tracking (ds:0x92F3/0x92F5) — critical for
            // sub_13c0c viewport bounds and object despawn on the new level.
            // v2 must replicate: sub_1673c on new level shadow state.
            {
                // sub_1673c: compare scroll Y/X with tracking, spawn/despawn on change
                uint16_t scroll_y = *(uint16_t*)(shadow + 0x257F) >> 1;
                uint16_t track_y = *(uint16_t*)(shadow + 0x92F3);
                if (scroll_y != track_y) {
                    *(uint16_t*)(shadow + 0x92F3) = scroll_y;
                    if ((int16_t)scroll_y < (int16_t)track_y) {
                        // sub_13a14: scroll up bounds
                        uint16_t vp_x = *(uint16_t*)(shadow + 0x44);
                        *(uint16_t*)(shadow + 0x34) = vp_x - 0x10;
                        *(uint16_t*)(shadow + 0x36) = vp_x - 0x10 + 0x20;
                        uint16_t vp_y = *(uint16_t*)(shadow + 0x46);
                        *(uint16_t*)(shadow + 0x38) = vp_y - 0x10;
                        *(uint16_t*)(shadow + 0x3A) = vp_y - 0x10 + 0xD0;
                        v2_loc_13a94(shadow);
                    } else {
                        // sub_13a34: scroll down bounds
                        uint16_t vp_x = *(uint16_t*)(shadow + 0x44);
                        *(uint16_t*)(shadow + 0x36) = vp_x + 0x150;
                        *(uint16_t*)(shadow + 0x34) = vp_x + 0x150 - 0x20;
                        uint16_t vp_y = *(uint16_t*)(shadow + 0x46);
                        *(uint16_t*)(shadow + 0x38) = vp_y - 0x10;
                        *(uint16_t*)(shadow + 0x3A) = vp_y - 0x10 + 0xD0;
                        v2_loc_13a94(shadow);
                    }
                }
                uint16_t scroll_x = *(uint16_t*)(shadow + 0x2581) >> 1;
                uint16_t track_x = *(uint16_t*)(shadow + 0x92F5);
                if (scroll_x != track_x) {
                    *(uint16_t*)(shadow + 0x92F5) = scroll_x;
                    if ((int16_t)scroll_x < (int16_t)track_x) {
                        // loc_13a74: scroll left bounds
                        uint16_t vp_y = *(uint16_t*)(shadow + 0x46);
                        *(uint16_t*)(shadow + 0x38) = vp_y - 0x10;
                        *(uint16_t*)(shadow + 0x3A) = vp_y - 0x10 + 0x20;
                        uint16_t vp_x = *(uint16_t*)(shadow + 0x44);
                        *(uint16_t*)(shadow + 0x34) = vp_x - 0x10;
                        *(uint16_t*)(shadow + 0x36) = vp_x - 0x10 + 0x160;
                        v2_loc_13a94(shadow);
                    } else {
                        // loc_13a54: scroll right bounds
                        uint16_t vp_y = *(uint16_t*)(shadow + 0x46);
                        *(uint16_t*)(shadow + 0x3A) = vp_y + 0xC0;
                        *(uint16_t*)(shadow + 0x38) = vp_y + 0xC0 - 0x20;
                        uint16_t vp_x = *(uint16_t*)(shadow + 0x44);
                        *(uint16_t*)(shadow + 0x34) = vp_x - 0x10;
                        *(uint16_t*)(shadow + 0x36) = vp_x - 0x10 + 0x160;
                        v2_loc_13a94(shadow);
                    }
                }
            }
        }
    }

    // sub_11ba5: pause handler. Verified with seg000 lines 4394-4423.
    // Test byte_2AAAF & 1; if zero → return.
    // Test word_28898 & 0x2000; if zero → return.
    // Then: stop music, set word_28925=0x11, word_28927=1, clear sprite flag, set dirty.
    // Then blocking render loop until unpause — for v2: DS writes only, no blocking.
    if ((shadow[0x25CF] & 1) && (*(uint16_t*)(shadow + 0x3B8) & 0x2000)) {
        // sub_177bb(0): stop music — AIL, commented
        *(uint16_t*)(shadow + 0x0445) = 0x11;   // word_28925 (0x28925 - 0x284E0 = 0x445)
        *(uint16_t*)(shadow + 0x0447) = 1;       // word_28927 (0x28927 - 0x284E0 = 0x447)
        uint16_t di_p = *(uint16_t*)(shadow + 0x3C2);
        uint16_t di_spr = *(uint16_t*)(shadow + di_p + 0x1A85);
        *(uint16_t*)(shadow + di_spr + 0x44D) &= 0xDFFF;  // AND [di+44Dh], 0DFFFh
        *(uint16_t*)(shadow + di_spr + 0x114D) = 2;         // MOV word [di+114Dh], 2
        // Blocking render loop: sub_10130 + sub_1DE05 + sub_165aa + sub_16661 + sub_1406d
        //   + sub_1DD9C + sub_1C8F1 + sub_1E0C7 + sub_16775 × 3 passes
        //   → FOR V2: blocking loop with SDL input (Phase 2d of plan)
        //   For now: DS writes done, blocking loop not implemented
    }

    // sub_12e79 → sub_12e84: viking cycling. Verified with seg000 lines 6635-6691.
    // Test byte ds:0x25BA; if 0 → return.
    // bit 0x20 in ds:0x3B8 → PREV viking (SUB si, 2). bit 0x10 → NEXT viking (ADD si, 2).
    // Skip slots where [si+0x15AD] == 0 (inactive).
    // DS writes: [di+0x44D] &= 0xDFFF, byte [di+0x114D]=2, ds:0x3C2=si, ds:0x34E=5, ds:0x350=5.
    if (shadow[0x25BA] != 0) {
        uint16_t new_input = *(uint16_t*)(shadow + 0x3B8);
        int16_t si_v = -1;
        if (new_input & 0x20) {
            // loc_12eb3: PREV viking (subtract)
            si_v = (int16_t)*(uint16_t*)(shadow + 0x3C2);
            do {
                si_v -= 2; if (si_v < 0) si_v = 4;               // SUB si, 2; JNS; MOV si, 4
                if ((uint16_t)si_v == *(uint16_t*)(shadow + 0x3C2)) { si_v = -1; break; } // wrapped
            } while (*(uint16_t*)(shadow + (uint16_t)si_v + 0x15AD) == 0); // TEST; JZ loop
        } else if (new_input & 0x10) {
            // loc_12e98: NEXT viking (add)
            si_v = (int16_t)*(uint16_t*)(shadow + 0x3C2);
            do {
                si_v += 2; if (si_v >= 6) si_v = 0;              // ADD si, 2; CMP 6; JL; MOV si, 0
                if ((uint16_t)si_v == *(uint16_t*)(shadow + 0x3C2)) { si_v = -1; break; }
            } while (*(uint16_t*)(shadow + (uint16_t)si_v + 0x15AD) == 0);
        }
        if (si_v >= 0) {
            // loc_12ecd: found valid viking
            uint16_t di_v = *(uint16_t*)(shadow + (uint16_t)si_v + 0x1A85);
            *(uint16_t*)(shadow + di_v + 0x44D) &= 0xDFFF;      // AND [di+44Dh], 0DFFFh
            shadow[di_v + 0x114D] = 2;                            // MOV byte [di+114Dh], 2
            *(uint16_t*)(shadow + 0x3C2) = (uint16_t)si_v;       // MOV ds:3C2h, si
            *(uint16_t*)(shadow + 0x34E) = 5;                     // MOV ds:34Eh, 5
            *(uint16_t*)(shadow + 0x350) = 5;                     // MOV ds:350h, 5
        }
    }

    // sub_10813 → loc_107A2: viking blink.
    // Verified with seg000 lines 1048-1092 (sub_10813) and 1004-1042 (loc_107a2).
    // Must check byte_2AA9A and viewport bounds exactly like original.
    {
        uint16_t active = *(uint16_t*)(shadow + 0x03C2); // word_288A2
        uint16_t prev = *(uint16_t*)(shadow + 0x03C4);   // word_288A4

        // sub_10813: first check byte_2AA9A (ds:0x25BA)
        uint8_t flag_9a = shadow[0x25BA]; // byte_2AA9A
        bool on_screen = false;
        if (flag_9a != 0 && active < 6) {
            // Check viewport bounds for active viking
            int16_t vx = (int16_t)*(uint16_t*)(shadow + active + 0x173D); // world X
            int16_t wx = (int16_t)*(uint16_t*)(shadow + 0x0044);         // word_28524 = viewport X
            if ((int16_t)(vx - wx + 0x0C) >= 0 && (int16_t)(wx + 0x14C - vx) >= 0) {
                uint16_t wy = *(uint16_t*)(shadow + 0x0046); // word_28526
                if (wy == 0) {
                    on_screen = true; // no Y scroll → always on screen vertically
                } else {
                    int16_t vy = (int16_t)*(uint16_t*)(shadow + active + 0x1765); // world Y
                    if ((int16_t)(vy - (int16_t)wy + 0x0C) >= 0 && (int16_t)((int16_t)wy + 0x0B0 - vy) >= 0) {
                        on_screen = true;
                    }
                }
            }
        }
        { uint16_t _lv = *(uint16_t*)(shadow+0x25AD); static int _dm2=0;
          if (_lv == 0x002B && _dm2 < 10) { _dm2++;
          fprintf(stderr, "V2-PRE-BLINK[%d]: lv=%04X 117D=%04X 117F=%04X flag=%02X act=%d prev=%d blink=%04X\n",
            _dm2, _lv, *(uint16_t*)(shadow+0x117D), *(uint16_t*)(shadow+0x117F),
            flag_9a, active, prev, *(uint16_t*)(shadow+0x03C6)); } }
        if (flag_9a == 0) {
            // byte_2AA9A=0 → locret_1081c → RETN. Do NOT touch input.
            goto v2_sub_10813_done;
        }
        if (!on_screen) {
            // loc_10862: viking out of viewport → clear keys, return
            *(uint16_t*)(shadow + 0x03B6) = 0; // word_28896
            *(uint16_t*)(shadow + 0x03B8) = 0; // word_28898
            goto v2_sub_10813_done;
        }

        { static int _dbg = 0; if (_dbg < 5) { _dbg++;
          uint16_t s_val = *(uint16_t*)(shadow + 0x1A85);
          uint16_t r_val = v2_vm_real_ds_ptr ? *(uint16_t*)(v2_vm_real_ds_ptr + 0x1A85) : 0xDEAD;
          printf("V2-DBG-10813: active=%d prev=%d flag=%02X 414D=%02X s[1A85]=%04X r[1A85]=%04X\n",
            active, prev, flag_9a, shadow[0x414D], s_val, r_val); } }

        // loc_107A2: if active changed, clear previous viking's blink + reset counter
        if (active != prev) {
            if ((int16_t)prev < 6) { // SIGNED compare: CMP ax,6; JGE (0xFFFF=-1 < 6 → enters)
                int16_t p_anim = (int16_t)*(uint16_t*)(shadow + (uint16_t)(prev + 0x16ED));
                if (p_anim >= 0) {
                    uint16_t p_di = *(uint16_t*)(shadow + (uint16_t)(prev + 0x1A85));
                    *(uint16_t*)(shadow + (uint16_t)(p_di + 0x44D)) &= 0xDFFF;
                    *(uint16_t*)(shadow + (uint16_t)(p_di + 0x114D)) = 2; // word write
                }
            }
            // loc_107c9: update prev + reset counter
            *(uint16_t*)(shadow + 0x03C4) = active;
            *(uint16_t*)(shadow + 0x03C6) = 0x15; // word_288A6 = 21
        }

        // loc_107d5: blink counter processing
        { uint16_t blink = *(uint16_t*)(shadow + 0x03C6); // word_288A6
        if (blink != 0 && active < 6) {
            blink -= 1; // DEC word_288A6
            *(uint16_t*)(shadow + 0x03C6) = blink;
            uint16_t di = active;
            if (blink & 2) {
                // Bit 1 set after decrement: extra DEC + hide sprite
                blink -= 1;
                *(uint16_t*)(shadow + 0x03C6) = blink;
                uint16_t sub_di = *(uint16_t*)(shadow + (uint16_t)(di + 0x1A85));
                { uint16_t _ta = (uint16_t)(sub_di + 0x114D);
                  if (_ta >= 0x117D && _ta <= 0x1181) { static int _bh=0; if(_bh<5){_bh++;
                    fprintf(stderr,"V2-BLINK-HIDE: sub_di=%04X addr=%04X was=%04X\n",sub_di,_ta,*(uint16_t*)(shadow+_ta));} } }
                *(uint16_t*)(shadow + (uint16_t)(sub_di + 0x44D)) |= 0x2000;
                *(uint16_t*)(shadow + (uint16_t)(sub_di + 0x114D)) = 0x200;
            } else {
                // Bit 1 clear: show sprite
                uint16_t sub_di = *(uint16_t*)(shadow + (uint16_t)(di + 0x1A85));
                { uint16_t _ta = (uint16_t)(sub_di + 0x114D);
                  if (_ta >= 0x117D && _ta <= 0x1181) { static int _bs=0; if(_bs<5){_bs++;
                    fprintf(stderr,"V2-BLINK-SHOW: sub_di=%04X addr=%04X was=%04X\n",sub_di,_ta,*(uint16_t*)(shadow+_ta));} } }
                *(uint16_t*)(shadow + (uint16_t)(sub_di + 0x44D)) &= 0xDFFF;
                *(uint16_t*)(shadow + (uint16_t)(sub_di + 0x114D)) = 2;
            }
        } }
    v2_sub_10813_done:

        // sub_1086f: command buffer dispatch — exact replica of original.
        // Original loop: bx=word_2B044; while(bx!=word_2A66F) { clear blink; handler=off_2B086[si];
        //   MOV word_2B044,bx; CALLF sub_1E0C7; CALL sub_12352; CALL sub_10138; JMP loop }
        // Exit: clear both ptrs; sub_16775; sub_10130; RETN.
        {
            uint16_t bx_read = *(uint16_t*)(shadow + 0x2B64);  // word_2B044
            uint16_t bx_write = *(uint16_t*)(shadow + 0x218F); // word_2A66F
            while (bx_read != bx_write) {
                // Clear blink on active viking sprite (lines 1104-1107)
                uint16_t si_v = *(uint16_t*)(shadow + 0x3C2);   // word_288A2
                uint16_t di_v = *(uint16_t*)(shadow + si_v + 0x1A85);
                *(uint16_t*)(shadow + di_v + 0x44D) &= 0xDFFF;  // AND [di+44Dh], 0DFFFh
                shadow[di_v + 0x114D] = 2;                       // MOV [di+114Dh], 2

                // si = [bx+1DA7h] — command type (dispatch index)
                uint16_t cmd_type = *(uint16_t*)(shadow + (uint16_t)(bx_read + 0x1DA7));

                if (cmd_type == 0) {
                    // off_2B086[0] = sub_12709: text display. bx += 0x0A.
                    // Original: push bx; ax=[bx+1DAD]; di=[bx+1DAB]; si=[bx+1DA9]; bx=[bx+1DAF]
                    //   push ax; sub_12529(bx); pop ax; sub_12549(ax)
                    //   push si,di; sub_12388; pop di; INC di; pop si; INC si; loc_124c5
                    //   byte_31A4B=1; pop bx; ADD bx,0Ah
                    uint16_t ax_align = *(uint16_t*)(shadow + (uint16_t)(bx_read + 0x1DAD));
                    uint16_t di_pos = *(uint16_t*)(shadow + (uint16_t)(bx_read + 0x1DAB));
                    uint16_t si_pos = *(uint16_t*)(shadow + (uint16_t)(bx_read + 0x1DA9));
                    uint16_t bx_text = *(uint16_t*)(shadow + (uint16_t)(bx_read + 0x1DAF));
                    v2_sub_12529(shadow, bx_text);                // read dims, bx_text += 2
                    v2_sub_12549(shadow, ax_align);               // set alignment offsets
                    uint16_t save_si = si_pos, save_di = di_pos;
                    v2_sub_12388(shadow, si_pos, di_pos, (uint8_t)ax_align); // draw box frame
                    si_pos = save_si + 1;                          // POP si; INC si
                    di_pos = save_di + 1;                          // POP di; INC di
                    v2_loc_124c5(shadow, si_pos, di_pos, bx_text); // render text
                    shadow[0x956B] = 1;                            // byte_31A4B = 1
                    bx_read += 0x0A;                               // ADD bx, 0Ah

                } else if (cmd_type == 0x0A) {
                    // off_2B086[0xA] = loc_12738: HUD text. bx += 8.
                    // Original: push bx; di=[bx+1DAB]; si=[bx+1DA9]; bx=[bx+1DAD]
                    //   word_28514=2; loc_124c5; byte_31A4B=1; pop bx; ADD bx,8
                    uint16_t di_pos = *(uint16_t*)(shadow + (uint16_t)(bx_read + 0x1DAB));
                    uint16_t si_pos = *(uint16_t*)(shadow + (uint16_t)(bx_read + 0x1DA9));
                    uint16_t bx_text = *(uint16_t*)(shadow + (uint16_t)(bx_read + 0x1DAD));
                    *(uint16_t*)(shadow + 0x34) = 2;               // word_28514 = 2
                    v2_loc_124c5(shadow, si_pos, di_pos, bx_text); // render text
                    shadow[0x956B] = 1;                            // byte_31A4B = 1
                    bx_read += 0x08;                               // ADD bx, 8

                } else if (cmd_type == 2) {
                    // off_2B086[2] = loc_12758: full screen text clear. bx += 2.
                    // Original: push bx; word_31A49=1; word_31DBC=0
                    //   if(byte_2AAAF & 0xE0): sub_16775 + sub_10130 + sub_1E0C7
                    //   sub_16775 + sub_10130 + sub_1DE05 + sub_165aa + sub_1DD9C
                    //   + sub_1C8F1(0xFFFE) + sub_1E0C7 + sub_16775 + sub_10130
                    //   + sub_1DE05 + sub_165aa + sub_1DD9C + sub_1C8F1(0xFFFE)
                    //   + sub_16775 + sub_10130
                    //   word_31A49=0; byte_31A4B=0; word_31DBC=0
                    //   REP STOSW: clear 0x1B8 words at ds:0x956C; pop bx; ADD bx,2
                    *(uint16_t*)(shadow + 0x9569) = 1;             // word_31A49 = 1
                    *(uint16_t*)(shadow + 0x98DC) = 0;             // word_31DBC = 0
                    // Conditional first render pass
                    if (shadow[0x25CF] & 0xE0) {
                        v2_sub_16775(shadow);                      // sub_16775
                        v2_sub_10130(shadow); // sub_10130: VGA vsync wait
                        // CALLF sub_1E0C7
                        v2_sub_1E0C7(shadow);
                    }
                    // Render pass 1
                    v2_sub_16775(shadow);                          // sub_16775
                    v2_sub_10130(shadow); // sub_10130: VGA vsync wait
                    v2_sub_1DE05(shadow);                          // CALLF sub_1DE05
                    v2_game_loop_post_render(shadow);              // sub_165aa
                    v2_sub_1DD9C(shadow);                          // CALLF sub_1DD9C
                    // MOV ax, 0xFFFE; CALLF sub_1C8F1 ��� flagged tiles (rendering only)
                    v2_sub_1C8F1(v2_vm_shadow_ds, 0xFFFE); v2_draw_flagged_tiles(v2_current_ds_val);
                    // CALLF sub_1E0C7
                    v2_sub_1E0C7(shadow);
                    v2_draw_ui(v2_current_ds_val);
                    // Render pass 2
                    v2_sub_16775(shadow);                          // sub_16775
                    v2_sub_10130(shadow); // sub_10130: VGA vsync wait
                    v2_sub_1DE05(shadow);                          // CALLF sub_1DE05
                    v2_game_loop_post_render(shadow);              // sub_165aa
                    v2_sub_1DD9C(shadow);                          // CALLF sub_1DD9C
                    // MOV ax, 0xFFFE; CALLF sub_1C8F1
                    v2_sub_1C8F1(v2_vm_shadow_ds, 0xFFFE); v2_draw_flagged_tiles(v2_current_ds_val);
                    // sub_16775 + sub_10130
                    v2_sub_16775(shadow);
                    // Cleanup
                    *(uint16_t*)(shadow + 0x9569) = 0;             // word_31A49 = 0
                    shadow[0x956B] = 0;                            // byte_31A4B = 0
                    *(uint16_t*)(shadow + 0x98DC) = 0;             // word_31DBC = 0
                    // REP STOSW: clear 0x1B8 words at ds:0x956C
                    memset(shadow + 0x956C, 0, 0x1B8 * 2);
                    bx_read += 2;                                  // ADD bx, 2

                } else if (cmd_type == 4) {
                    // off_2B086[4] = loc_127db: viking switch. bx += 2.
                    *(uint16_t*)(shadow + 0x0334) |= 4;           // OR word_28814, 4
                    bx_read += 2;                                  // ADD bx, 2

                } else if (cmd_type == 6) {
                    // off_2B086[6] = loc_127e4: palette color 3 update. bx += 4.
                    // Original: cx=[bx+1DA9]; SHL cx,1; ADD bx,4
                    //   OUT 0x3C8,3; OUT 0x3C9,(cx&0x3E)→ds:0x7F0B
                    //   SHR cx,5; OUT 0x3C9,(cx&0x3E)→ds:0x7F0C
                    //   SHR cx,5; OUT 0x3C9,(cx&0x3E)→ds:0x7F0D
                    uint16_t cx = *(uint16_t*)(shadow + (uint16_t)(bx_read + 0x1DA9));
                    cx <<= 1;                                      // SHL cx, 1
                    // OUT(0x3C8, 3);                               // VGA palette write addr — commented
                    uint8_t r = (uint8_t)(cx & 0x3E);              // OUT(0x3C9, r)
                    shadow[0x7F0B] = r;                            // byte ptr word_303EB
                    cx >>= 5;                                      // SHR cx, 5
                    uint8_t g = (uint8_t)(cx & 0x3E);              // OUT(0x3C9, g)
                    shadow[0x7F0C] = g;                            // byte ptr word_303EB+1
                    cx >>= 5;                                      // SHR cx, 5
                    uint8_t b = (uint8_t)(cx & 0x3E);              // OUT(0x3C9, b)
                    shadow[0x7F0D] = b;                            // byte_303ED
                    bx_read += 4;                                  // ADD bx, 4

                } else if (cmd_type == 8) {
                    // off_2B086[8] = loc_126ef: single glyph write. bx += 8.
                    // Original: push bx; ax=[bx+1DA9]; si=[bx+1DAB]; di=[bx+1DAD]
                    //   call sub_1241e; byte_31A4B=1; pop bx; ADD bx,8
                    uint8_t ch = (uint8_t)*(uint16_t*)(shadow + (uint16_t)(bx_read + 0x1DA9));
                    uint16_t si_pos = *(uint16_t*)(shadow + (uint16_t)(bx_read + 0x1DAB));
                    uint16_t di_pos = *(uint16_t*)(shadow + (uint16_t)(bx_read + 0x1DAD));
                    v2_sub_1241e(shadow, ch, si_pos, di_pos);      // call sub_1241e
                    shadow[0x956B] = 1;                            // byte_31A4B = 1
                    bx_read += 0x08;                               // ADD bx, 8

                } else {
                    // Unknown command type — advance by 2 (minimum)
                    bx_read += 2;
                }

                // MOV word_2B044, bx (line 1110)
                *(uint16_t*)(shadow + 0x2B64) = bx_read;

                // CALLF sub_1E0C7 (line 1111)
                v2_sub_1E0C7(shadow);
                v2_draw_ui(v2_current_ds_val);

                // CALL sub_12352 (line 1112): input processing
                {
                    extern uint16_t v2_input_snapshot;
                    uint16_t ax = 0;
                    if (*(uint16_t*)(shadow + 0x86DA) != 0)
                        ax = *(uint16_t*)(shadow + 0x86DC);
                    ax |= *(uint16_t*)(shadow + 0x86DE);
                    ax |= v2_input_snapshot;
                    *(uint16_t*)(shadow + 0x03B6) = ax;
                    uint16_t prev = *(uint16_t*)(shadow + 0x03BA);
                    *(uint16_t*)(shadow + 0x03B8) = (ax ^ prev) & ax;
                    *(uint16_t*)(shadow + 0x03BA) = ax;
                }

                // CALL sub_10138 (line 1113): button processing
                // If transition bits set → process transition and break
                {
                    uint16_t btns = *(uint16_t*)(shadow + 0x0334);
                    if (btns & 0x7) break; // bits 0-2 set → transition pending, exit loop
                }
            }
            // loc_108a5: clear pointers + page flip + frame sync
            *(uint16_t*)(shadow + 0x218F) = 0;                    // MOV word_2A66F, 0
            *(uint16_t*)(shadow + 0x2B64) = 0;                    // MOV word_2B044, 0
            v2_sub_16775(shadow);                                  // CALL sub_16775
            v2_sub_10130(shadow); // sub_10130: VGA vsync wait
        }
    }

    // sub_1673c: tile scroll management + object spawn/despawn.
    // sub_13a14/sub_13a34: set viewport bounds + call loc_13a94 (object spawn loop).
    // loc_13a54/loc_13a74: same for X axis.
    {
        auto loc_13a94 = [&shadow]() { v2_loc_13a94(shadow); };

        uint16_t scroll_y = *(uint16_t*)(shadow + 0x257F) >> 1;
        uint16_t track_y = *(uint16_t*)(shadow + 0x92F3);
        if (scroll_y != track_y) {
            *(uint16_t*)(shadow + 0x92F3) = scroll_y;
            if ((int16_t)scroll_y < (int16_t)track_y) {
                // sub_13a14: viewport bounds for scroll up
                uint16_t vp_x = *(uint16_t*)(shadow + 0x44);
                *(uint16_t*)(shadow + 0x34) = vp_x - 0x10;
                *(uint16_t*)(shadow + 0x36) = vp_x - 0x10 + 0x20;
                uint16_t vp_y = *(uint16_t*)(shadow + 0x46);
                *(uint16_t*)(shadow + 0x38) = vp_y - 0x10;
                *(uint16_t*)(shadow + 0x3A) = vp_y - 0x10 + 0xD0;
                loc_13a94();
            } else {
                // sub_13a34: viewport bounds for scroll down
                uint16_t vp_x = *(uint16_t*)(shadow + 0x44);
                *(uint16_t*)(shadow + 0x36) = vp_x + 0x150;
                *(uint16_t*)(shadow + 0x34) = vp_x + 0x150 - 0x20;
                uint16_t vp_y = *(uint16_t*)(shadow + 0x46);
                *(uint16_t*)(shadow + 0x38) = vp_y - 0x10;
                *(uint16_t*)(shadow + 0x3A) = vp_y - 0x10 + 0xD0;
                loc_13a94();
            }
        }
        uint16_t scroll_x = *(uint16_t*)(shadow + 0x2581) >> 1;
        uint16_t track_x = *(uint16_t*)(shadow + 0x92F5);
        if (scroll_x != track_x) {
            *(uint16_t*)(shadow + 0x92F5) = scroll_x;
            if ((int16_t)scroll_x < (int16_t)track_x) {
                // loc_13a74: viewport bounds for scroll left
                uint16_t vp_y = *(uint16_t*)(shadow + 0x46);
                *(uint16_t*)(shadow + 0x38) = vp_y - 0x10;
                *(uint16_t*)(shadow + 0x3A) = vp_y - 0x10 + 0x20;
                uint16_t vp_x = *(uint16_t*)(shadow + 0x44);
                *(uint16_t*)(shadow + 0x34) = vp_x - 0x10;
                *(uint16_t*)(shadow + 0x36) = vp_x - 0x10 + 0x160;
                loc_13a94();
            } else {
                // loc_13a54: viewport bounds for scroll right
                uint16_t vp_y = *(uint16_t*)(shadow + 0x46);
                *(uint16_t*)(shadow + 0x3A) = vp_y + 0xC0;
                *(uint16_t*)(shadow + 0x38) = vp_y + 0xC0 - 0x20;
                uint16_t vp_x = *(uint16_t*)(shadow + 0x44);
                *(uint16_t*)(shadow + 0x34) = vp_x - 0x10;
                *(uint16_t*)(shadow + 0x36) = vp_x - 0x10 + 0x160;
                loc_13a94();
            }
        }
    }
}

// Viewport movers — called from v2_game_loop_post_vm (sub_1064b camera follow)
static void v2_sub_17496(uint8_t* s, int16_t si_speed) {
    // Scroll left: ds:0x44 = max(ds:0x44 - si_speed, 0)
    if (*(uint16_t*)(s + 0x394)) return; // lock flag
    int16_t ax = (int16_t)*(uint16_t*)(s + 0x44);
    ax -= si_speed;
    if (ax < 0) ax = 0;
    int16_t dx = (int16_t)*(uint16_t*)(s + 0x44) - ax;
    *(uint16_t*)(s + 0x44) = (uint16_t)ax;
    *(uint16_t*)(s + 0x257F) = (uint16_t)ax >> 3;
    *(uint16_t*)(s + 0x34E) = (uint16_t)dx;
}

static void v2_sub_1746c(uint8_t* s, int16_t si_speed) {
    // Scroll right: ds:0x44 = min(ds:0x44 + si_speed, ds:0x25A4)
    if (*(uint16_t*)(s + 0x394)) return;
    int16_t ax = (int16_t)*(uint16_t*)(s + 0x44);
    ax += si_speed;
    uint16_t limit = *(uint16_t*)(s + 0x25A4);
    if ((uint16_t)ax >= limit) ax = (int16_t)limit;
    int16_t dx = ax - (int16_t)*(uint16_t*)(s + 0x44);
    *(uint16_t*)(s + 0x44) = (uint16_t)ax;
    *(uint16_t*)(s + 0x257F) = (uint16_t)ax >> 3;
    *(uint16_t*)(s + 0x34E) = (uint16_t)dx;
}

static void v2_loc_174e9(uint8_t* s, int16_t si_speed) {
    // Scroll up: ds:0x46 = max(ds:0x46 - si_speed, 0)
    if (*(uint16_t*)(s + 0x396)) return;
    int16_t ax = (int16_t)*(uint16_t*)(s + 0x46);
    ax -= si_speed;
    if (ax < 0) ax = 0;
    int16_t dx = (int16_t)*(uint16_t*)(s + 0x46) - ax;
    *(uint16_t*)(s + 0x46) = (uint16_t)ax;
    *(uint16_t*)(s + 0x2581) = (uint16_t)ax >> 3;
    *(uint16_t*)(s + 0x350) = (uint16_t)dx;
}

static void v2_loc_174bf(uint8_t* s, int16_t si_speed) {
    // Scroll down: ds:0x46 = min(ds:0x46 + si_speed, ds:0x25A6)
    if (*(uint16_t*)(s + 0x396)) return;
    int16_t ax = (int16_t)*(uint16_t*)(s + 0x46);
    ax += si_speed;
    uint16_t limit = *(uint16_t*)(s + 0x25A6);
    if ((uint16_t)ax >= limit) ax = (int16_t)limit;
    int16_t dx = ax - (int16_t)*(uint16_t*)(s + 0x46);
    *(uint16_t*)(s + 0x46) = (uint16_t)ax;
    *(uint16_t*)(s + 0x2581) = (uint16_t)ax >> 3;
    *(uint16_t*)(s + 0x350) = (uint16_t)dx;
}

// sub_1064b: Camera follow — moves viewport to track active viking.
// Speed table at ds:0x2B84 maps delta (0-16) to scroll speed.
static void v2_sub_1064b(uint8_t* s) {
    *(uint16_t*)(s + 0x34E) = 0; // word_2882E
    *(uint16_t*)(s + 0x350) = 0; // word_28830
    *(uint16_t*)(s + 0x34E) = 0; // word_2882E (scroll delta X) — cleared first
    *(uint16_t*)(s + 0x350) = 0; // word_28830 (scroll delta Y) — cleared first
    *(uint16_t*)(s + 0x3D8) = 0; // word_288B8
    *(uint16_t*)(s + 0x3DA) = 0; // word_288BA
    *(uint16_t*)(s + 0x3DE) = 0; // word_288BE
    *(uint16_t*)(s + 0x3DC) = 0; // word_288BC

    uint16_t di = *(uint16_t*)(s + 0x3C2); // active viking
    uint16_t vp_x = *(uint16_t*)(s + 0x44);
    uint16_t vp_y = *(uint16_t*)(s + 0x46);
    uint16_t obj_x = *(uint16_t*)(s + di + 0x173D);
    uint16_t obj_y = *(uint16_t*)(s + di + 0x1765);

    // X camera follow
    int16_t dx_left = (int16_t)(vp_x + 0x90) - (int16_t)obj_x;
    if (dx_left > 0) {
        if (dx_left > 0x10) dx_left = 0x10;
        *(uint16_t*)(s + 0x3D8) = (uint16_t)dx_left;
        int16_t speed = *(int16_t*)(s + (uint16_t)(dx_left * 2 + 0x2B84));
        v2_sub_17496(s, speed);
    } else {
        int16_t dx_right = (int16_t)obj_x - (int16_t)vp_x - 0xB0;
        if (dx_right > 0) {
            if (dx_right > 0x10) dx_right = 0x10;
            *(uint16_t*)(s + 0x3DA) = (uint16_t)dx_right;
            int16_t speed = *(int16_t*)(s + (uint16_t)(dx_right * 2 + 0x2B84));
            v2_sub_1746c(s, speed);
        }
    }

    // Y camera follow
    int16_t dy_up = (int16_t)(vp_y + 0x50) - (int16_t)obj_y;
    if (dy_up > 0) {
        if (dy_up > 0x10) dy_up = 0x10;
        *(uint16_t*)(s + 0x3DE) = (uint16_t)dy_up;
        int16_t speed = *(int16_t*)(s + (uint16_t)(dy_up * 2 + 0x2B84));
        v2_loc_174e9(s, speed);
    } else {
        int16_t dy_down = (int16_t)obj_y - (int16_t)vp_y - 0x60;
        if (dy_down > 0) {
            if (dy_down > 0x10) dy_down = 0x10;
            *(uint16_t*)(s + 0x3DC) = (uint16_t)dy_down;
            int16_t speed = *(int16_t*)(s + (uint16_t)(dy_down * 2 + 0x2B84));
            v2_loc_174bf(s, speed);
        }
    }
}

// Standalone sub_15fbe for use in game loop (before V2VM struct is defined).
// sub_15fbe: object search with y_check = ds:[obj_di + 0x150D] + 1.
// Returns true (carry set) if matching object found.
static bool v2_gameloop_sub_15fbe(uint8_t* ds, uint16_t filter_si, uint16_t obj_di) {
    uint16_t y_check = *(uint16_t*)(ds + obj_di + 0x150D) + 1;
    *(uint16_t*)(ds + 0x34) = filter_si;
    *(uint16_t*)(ds + 0x36) = y_check;
    uint16_t table_end = *(uint16_t*)(ds + 0x372);
    for (uint16_t si = 0; (int16_t)si < (int16_t)table_end; si += 2) {
        if (*(uint16_t*)(ds + si + 0x1355) == 0) continue;
        if (si == *(uint16_t*)(ds + 0x42)) continue;
        *(uint16_t*)(ds + 0x3A) = si;
        uint8_t obj_type = (uint8_t)*(uint16_t*)(ds + si + 0x17DD);
        uint16_t flt = filter_si;
        bool match = false;
        while (true) {
            uint8_t fval = *(uint8_t*)(ds + (uint16_t)(flt - 0x6B34));
            if (obj_type < fval) break;
            if (obj_type == fval) { match = true; break; }
            flt++;
        }
        if (!match) continue;
        if ((int16_t)y_check < (int16_t)*(uint16_t*)(ds + si + 0x14E5)) continue;
        if ((int16_t)(y_check - 1) >= (int16_t)*(uint16_t*)(ds + si + 0x150D)) continue;
        if ((int16_t)*(uint16_t*)(ds + obj_di + 0x155D) < (int16_t)*(uint16_t*)(ds + si + 0x1535)) continue;
        if ((int16_t)*(uint16_t*)(ds + si + 0x155D) < (int16_t)*(uint16_t*)(ds + obj_di + 0x1535)) continue;
        *(uint16_t*)(ds + 0x3B2) = obj_type;
        *(uint16_t*)(ds + 0x3B4) = si;
        return true;
    }
    return false;
}

// V2 post-VM game loop
static void v2_game_loop_post_vm(uint8_t* shadow) {
    // sub_1386b: backup X/Y + apply velocity to positions
    {
        uint16_t table_end = *(uint16_t*)(shadow + 0x372);
        for (uint16_t di = 0; (int16_t)di < (int16_t)table_end; di += 2) {
            if (*(uint16_t*)(shadow + di + 0x1355) == 0) continue;
            // Backup current X/Y
            *(uint16_t*)(shadow + di + 0x13A5) = *(uint16_t*)(shadow + di + 0x173D);
            *(uint16_t*)(shadow + di + 0x13CD) = *(uint16_t*)(shadow + di + 0x1765);
            // If collision state == 0xFFFF → apply velocity
            if (*(uint16_t*)(shadow + di + 0x141D) != 0xFFFF) continue;

            // X velocity: clamp to max, apply fractional accumulator, add to position
            {
                int16_t vel = (int16_t)*(uint16_t*)(shadow + di + 0x1945);
                int16_t max_vel = (int16_t)*(uint16_t*)(shadow + di + 0x178D);
                if (vel >= 0) {
                    if (vel >= max_vel) vel = max_vel;
                } else {
                    int16_t neg = -vel;
                    if (neg >= max_vel) neg = max_vel;
                    vel = -neg;
                }
                // ADD [di+19BD], al; ADC ah, 0; SAR ax, 8
                int16_t ax = vel;
                uint8_t al = ax & 0xFF;
                uint8_t ah = (ax >> 8) & 0xFF;
                uint16_t add_result = (uint16_t)shadow[di + 0x19BD] + al;
                shadow[di + 0x19BD] = (uint8_t)add_result;
                uint8_t cf = (add_result >> 8) & 1;
                ah += cf;
                ax = (int16_t)(int8_t)ah; // SAR ax, 8
                *(uint16_t*)(shadow + di + 0x1945) = (uint16_t)ax;
                *(uint16_t*)(shadow + di + 0x173D) += (uint16_t)ax; // X world
                *(uint16_t*)(shadow + di + 0x1535) += (uint16_t)ax; // X start
                *(uint16_t*)(shadow + di + 0x155D) += (uint16_t)ax; // X end
            }

            // Y velocity: same pattern
            {
                int16_t vel = (int16_t)*(uint16_t*)(shadow + di + 0x196D);
                int16_t max_vel = (int16_t)*(uint16_t*)(shadow + di + 0x17B5);
                if (vel >= 0) {
                    if (vel >= max_vel) vel = max_vel;
                } else {
                    int16_t neg = -vel;
                    if (neg >= max_vel) neg = max_vel;
                    vel = -neg;
                }
                int16_t ax = vel;
                uint8_t al = ax & 0xFF;
                uint8_t ah = (ax >> 8) & 0xFF;
                uint16_t add_result = (uint16_t)shadow[di + 0x19E5] + al;
                shadow[di + 0x19E5] = (uint8_t)add_result;
                uint8_t cf = (add_result >> 8) & 1;
                ah += cf;
                ax = (int16_t)(int8_t)ah; // SAR ax, 8
                *(uint16_t*)(shadow + di + 0x196D) = (uint16_t)ax;
                *(uint16_t*)(shadow + di + 0x1765) += (uint16_t)ax; // Y world
                *(uint16_t*)(shadow + di + 0x14E5) += (uint16_t)ax; // Y start
                *(uint16_t*)(shadow + di + 0x150D) += (uint16_t)ax; // Y end
            }
        }
    }

    // sub_1625d: ground detection + position snapping for objects with flag 0x2000
    // NOTE: original order is sub_1386b → sub_1625d → sub_15546 (verified seg000 lines 3958-3960)
    // Matches original flow exactly: loc_16260 → loc_162c4/loc_162d3/loc_162e0 → loc_1636d
    {
        // sub_14199 equivalent: tile type at pixel (x, y)
        auto tile_type_at = [&](uint16_t x, uint16_t y) -> uint16_t {
            uint16_t sx = x >> 4, sy = y >> 4;
            if (sx >= *(uint16_t*)(shadow + 0x25DC)) return 1;  // OOB right: 0x400 → type 1
            if (sy >= *(uint16_t*)(shadow + 0x25DE)) return 0;  // OOB bottom: 0 → type 0
            uint16_t d2 = sy << 1, s2 = sx << 1;
            s2 += *(uint16_t*)(shadow + (uint16_t)(d2 - 0x7098));
            uint16_t tv = *(uint16_t*)(v2_vm_shadow_tilemap + s2);
            return (tv & 0xFC00) >> 10;
        };

        // sub_16390 equivalent: slope table lookup
        // Returns (obj_Y_end & 0xF) - slope_val
        auto slope_lookup = [&](uint16_t di_obj, uint16_t tt, uint16_t obj_x) -> int16_t {
            uint16_t slope_idx = ((tt & 0xF) << 4) + (obj_x & 0xF);
            uint8_t slope_val = shadow[(uint16_t)(slope_idx - 0x7684)] & 0xF;
            uint16_t y_end = *(uint16_t*)(shadow + di_obj + 0x150D);
            return (int16_t)((uint16_t)((y_end & 0xF) - slope_val));
        };

        uint16_t table_end = *(uint16_t*)(shadow + 0x372);
        for (uint16_t di = 0; (int16_t)di < (int16_t)table_end; di += 2) {
            *(uint16_t*)(shadow + 0x42) = di;
            if (*(uint16_t*)(shadow + di + 0x1355) == 0) continue;       // loc_16260
            if (!(*(uint16_t*)(shadow + di + 0x1585) & 0x2000)) continue; // loc_1626e
            *(uint16_t*)(shadow + di + 0x1585) &= 0xDFFF;                // loc_16279: clear 0x2000
            if ((int16_t)*(uint16_t*)(shadow + di + 0x1675) < 0) continue; // JNS check

            // loc_16289: tile type at (obj_X, obj_Y_end)
            uint16_t obj_x = *(uint16_t*)(shadow + di + 0x173D);
            uint16_t obj_y_end = *(uint16_t*)(shadow + di + 0x150D);
            uint16_t tile_type = tile_type_at(obj_x, obj_y_end);

            int16_t y_adjust;

            if (tile_type == 4) {
                // loc_162c4: platform snap — y_adjust = (obj_Y_end & 0xF) + 1
                y_adjust = (int16_t)((obj_y_end & 0xF) + 1);
            } else if (tile_type == 1) {
                // tile_type == 1: check tile above at (obj_X, obj_Y_end - 0x10)
                uint16_t tt_above = tile_type_at(obj_x, (uint16_t)(obj_y_end - 0x10));
                if (tt_above >= 0x30) {
                    // loc_162d3: slope above — sub_16390(tt_above) + 0x10
                    y_adjust = slope_lookup(di, tt_above, obj_x) + 0x10;
                } else if (tt_above == 0 || tt_above == 0xC || tt_above == 3) {
                    // loc_162c4: platform snap
                    y_adjust = (int16_t)((obj_y_end & 0xF) + 1);
                } else {
                    // → loc_162e0 → loc_162f4 (tile_type=1, not >=0x30)
                    // → loc_16311 (tile_type=1, not in {0,0xC,3}) → loc_1637f
                    continue;
                }
            } else {
                // loc_162e0: all other tile types (including >= 0x30)
                // Viking platform check: sub_15fbe(0x89)
                if (di < 6) {
                    if (v2_gameloop_sub_15fbe(shadow, 0x89, di)) continue; // carry → loc_1637f
                }

                // loc_162f4: re-read tile at feet (same result as tile_type)
                if (tile_type >= 0x30) {
                    // Slope at feet: sub_16390(tile_type), no offset
                    y_adjust = slope_lookup(di, tile_type, obj_x);
                } else {
                    // loc_16311: check if tile is passable (0, 0xC, 3)
                    if (tile_type != 0 && tile_type != 0xC && tile_type != 3) continue; // loc_1637f

                    // loc_1632f: check tile below at (obj_X, obj_Y_end + 0x10)
                    uint16_t tt_below = tile_type_at(obj_x, (uint16_t)(obj_y_end + 0x10));
                    if (tt_below >= 0x30) {
                        // loc_16363: slope below — sub_16390(tt_below) - 0x10
                        y_adjust = slope_lookup(di, tt_below, obj_x) - 0x10;
                    } else if (tt_below == 1 || tt_below == 5 || tt_below == 0x20 ||
                               tt_below == 4 || tt_below == 2) {
                        // loc_16353: solid below — snap up
                        y_adjust = (int16_t)((obj_y_end & 0xF) - 0xF);
                    } else {
                        continue; // loc_1637f
                    }
                }
            }

            // loc_1636d: apply Y adjustment
            di = *(uint16_t*)(shadow + 0x42); // reload di from ds:42h
            *(uint16_t*)(shadow + di + 0x14E5) -= (uint16_t)y_adjust;
            *(uint16_t*)(shadow + di + 0x1765) -= (uint16_t)y_adjust;
            *(uint16_t*)(shadow + di + 0x150D) -= (uint16_t)y_adjust;
            *(uint16_t*)(shadow + di + 0x196D) = 0;
        }
    }

    // sub_15546: clear collision result fields + run collision detection VM (sub_15569)
    // NOTE: runs AFTER sub_1625d (verified seg000 line 3960, eip 0x15DE)
    {
        *(uint16_t*)(shadow + 0x390) = 1;
        uint16_t table_end = *(uint16_t*)(shadow + 0x372);
        for (uint16_t si = 0; (int16_t)si < (int16_t)table_end; si += 2) {
            // sub_1555c: clear [si+0x13F5], then if [si+0x141D]==0xFFFF → run collision VM
            *(uint16_t*)(shadow + si + 0x13F5) = 0;
            if (*(uint16_t*)(shadow + si + 0x141D) != 0xFFFF) continue;
            // sub_15569: run collision detection bytecodes from [si+0x132D]
            if (*(uint16_t*)(shadow + si + 0x1355) == 0) continue;
            v2_run_collision_vm(shadow, si);
        }
    }

    // sub_13916: collision resolution — process objects with active collision state
    {
        uint16_t table_end = *(uint16_t*)(shadow + 0x372);
        for (uint16_t si = 0; (int16_t)si < (int16_t)table_end; si += 2) {
            if (*(uint16_t*)(shadow + si + 0x1355) == 0) continue;
            uint16_t coll_state = *(uint16_t*)(shadow + si + 0x141D);
            if (coll_state == 0xFFFF) continue; // no collision
            if ((int16_t)coll_state >= 0x100) {
                // Simple case: apply stored velocity to position
                int16_t vx = (int16_t)*(uint16_t*)(shadow + si + 0x1945);
                *(uint16_t*)(shadow + si + 0x173D) += (uint16_t)vx;
                *(uint16_t*)(shadow + si + 0x1535) += (uint16_t)vx;
                *(uint16_t*)(shadow + si + 0x155D) += (uint16_t)vx;
                int16_t vy = (int16_t)*(uint16_t*)(shadow + si + 0x196D);
                *(uint16_t*)(shadow + si + 0x1765) += (uint16_t)vy;
                *(uint16_t*)(shadow + si + 0x14E5) += (uint16_t)vy;
                *(uint16_t*)(shadow + si + 0x150D) += (uint16_t)vy;
                *(uint16_t*)(shadow + si + 0x141D) = 0xFFFF; // clear collision
            }
            else {
                // Complex case (< 0x100): collision push with flip detection
                uint16_t di_coll = coll_state; // collision partner object
                *(uint16_t*)(shadow + 0x6E) = di_coll;
                *(uint16_t*)(shadow + 0x6C) = si;
                // Check flip flags XOR between objects
                uint16_t flags_xor = *(uint16_t*)(shadow + di_coll + 0x1585) ^
                                     *(uint16_t*)(shadow + si + 0x1585);
                // If flag 0x40 differs → horizontal flip (sub_136a0)
                if (flags_xor & 0x40) {
                    *(uint16_t*)(shadow + si + 0x1585) ^= 0x40;
                    uint16_t x2 = *(uint16_t*)(shadow + si + 0x173D) * 2;
                    uint16_t new_1535 = x2 - *(uint16_t*)(shadow + si + 0x155D) - 1;
                    uint16_t new_155D = x2 - *(uint16_t*)(shadow + si + 0x1535) - 1;
                    *(uint16_t*)(shadow + si + 0x155D) = new_155D;
                    *(uint16_t*)(shadow + si + 0x1535) = new_1535;
                    // Sub-sprite hflip loop
                    if (*(uint16_t*)(shadow + si + 0x1AD5) != 0) {
                        uint16_t dx2 = x2;
                        uint16_t cx_end = *(uint16_t*)(shadow + si + 0x1AAD);
                        for (uint16_t sdi = *(uint16_t*)(shadow + si + 0x1A85);
                             (int16_t)sdi < (int16_t)cx_end; sdi += 2) {
                            uint16_t new_x = dx2 - *(uint16_t*)(shadow + sdi + 0x64D)
                                             - *(uint16_t*)(shadow + sdi + 0x0C4D);
                            *(uint16_t*)(shadow + sdi + 0x64D) = new_x;
                            *(uint16_t*)(shadow + sdi + 0x44D) ^= 0x200;
                            *(uint16_t*)(shadow + sdi + 0x114D) = 0x202;
                        }
                    }
                }
                // If flag 0x80 differs → vertical flip (sub_13757)
                if (flags_xor & 0x80) {
                    *(uint16_t*)(shadow + si + 0x1585) ^= 0x80;
                    uint16_t y2 = *(uint16_t*)(shadow + si + 0x1765) * 2;
                    uint16_t new_14E5 = y2 - *(uint16_t*)(shadow + si + 0x150D) - 1;
                    uint16_t new_150D = y2 - *(uint16_t*)(shadow + si + 0x14E5) - 1;
                    *(uint16_t*)(shadow + si + 0x150D) = new_150D;
                    *(uint16_t*)(shadow + si + 0x14E5) = new_14E5;
                    // Sub-sprite vflip loop
                    if (*(uint16_t*)(shadow + si + 0x1AD5) != 0) {
                        uint16_t dy2 = y2;
                        uint16_t cx_end = *(uint16_t*)(shadow + si + 0x1AAD);
                        for (uint16_t sdi = *(uint16_t*)(shadow + si + 0x1A85);
                             (int16_t)sdi < (int16_t)cx_end; sdi += 2) {
                            *(uint16_t*)(shadow + sdi + 0x74D) = dy2
                                - *(uint16_t*)(shadow + sdi + 0x74D)
                                - *(uint16_t*)(shadow + sdi + 0x0C4D);
                            *(uint16_t*)(shadow + sdi + 0x44D) ^= 0x400;
                            *(uint16_t*)(shadow + sdi + 0x114D) = 0x202;
                        }
                    }
                }

                // Position delta: bx = partner.X - self.X, cx = partner.Y - self.Y
                int16_t bx_delta = (int16_t)*(uint16_t*)(shadow + di_coll + 0x173D) -
                                   (int16_t)*(uint16_t*)(shadow + si + 0x173D);
                int16_t cx_delta = (int16_t)*(uint16_t*)(shadow + di_coll + 0x1765) -
                                   (int16_t)*(uint16_t*)(shadow + si + 0x1765);

                // X: if flag 0x40 set → bx -= velocity, else bx += velocity
                if (*(uint16_t*)(shadow + si + 0x1585) & 0x40) {
                    bx_delta -= (int16_t)*(uint16_t*)(shadow + si + 0x1945);
                } else {
                    bx_delta += (int16_t)*(uint16_t*)(shadow + si + 0x1945);
                }
                *(uint16_t*)(shadow + si + 0x1945) = (uint16_t)bx_delta;
                *(uint16_t*)(shadow + si + 0x173D) += (uint16_t)bx_delta;
                *(uint16_t*)(shadow + si + 0x1535) += (uint16_t)bx_delta;
                *(uint16_t*)(shadow + si + 0x155D) += (uint16_t)bx_delta;

                // Y: if flag 0x80 set → cx -= velocity, else cx += velocity
                if (*(uint16_t*)(shadow + si + 0x1585) & 0x80) {
                    cx_delta -= (int16_t)*(uint16_t*)(shadow + si + 0x196D);
                } else {
                    cx_delta += (int16_t)*(uint16_t*)(shadow + si + 0x196D);
                }
                *(uint16_t*)(shadow + si + 0x196D) = (uint16_t)cx_delta;
                *(uint16_t*)(shadow + si + 0x1765) += (uint16_t)cx_delta;
                *(uint16_t*)(shadow + si + 0x14E5) += (uint16_t)cx_delta;
                *(uint16_t*)(shadow + si + 0x150D) += (uint16_t)cx_delta;

                *(uint16_t*)(shadow + si + 0x141D) = 0xFFFF; // clear collision
            }
        }
    }

    // sub_1064b: camera follow
    v2_sub_1064b(shadow);

    // NOTE: original eip order after sub_1064b (0x0048):
    //   [POST_VM signal at line 1959]
    //   sub_12fc6 (eip 0x004B) — moved to v2_phase_render1
    //   sub_10130 (eip 0x004E) — moved to v2_phase_render1
    // These run AFTER POST_VM compare in the original too.
}

// Post-RENDER game loop — runs AFTER render (eip 0x0056..0x005C).
// sub_165aa, sub_16661, sub_1406d.
static void v2_game_loop_post_render(uint8_t* shadow) {
    // sub_165aa: VGA page rotation state machine.
    // Cycles ds:0x92F9 through 0→0x34→0x68→0... based on viewport Y lookup tables.
    // sub_1df6a (dirty rect render) skipped — v2 renders full frames.
    {
        static int rot_dbg = 0; rot_dbg++;
        uint16_t ax = *(uint16_t*)(shadow + 0x92F9);
        if (rot_dbg <= 6) {
            uint16_t si_lk = (*(uint16_t*)(shadow + 0x46) & 0xFFF8) >> 2;
            printf("V2-ROT[%d]: 92F9=%04X 92FB=%04X si=%04X tab_a=%04X tab_b=%04X\n",
                rot_dbg, ax, *(uint16_t*)(shadow+0x92FB), si_lk,
                *(uint16_t*)(shadow + (uint16_t)(si_lk - 0x75D6)),
                *(uint16_t*)(shadow + (uint16_t)(si_lk - 0x75A4)));
        }
        *(uint16_t*)(shadow + 0x92F7) = ax;
        uint16_t dx = *(uint16_t*)(shadow + 0x92FB);
        uint16_t si = (*(uint16_t*)(shadow + 0x46) & 0xFFF8) >> 2;

        if (ax == 0) {
            // loc_165ef: state 0 → next pair (0x34, 0x68)
            *(uint16_t*)(shadow + 0x92F9) = 0x34;
            *(uint16_t*)(shadow + 0x92FB) = 0x68;
            uint16_t a = *(uint16_t*)(shadow + (uint16_t)(si - 0x75D6));
            uint16_t b = *(uint16_t*)(shadow + (uint16_t)(si - 0x75A4));
            if (a >= b) {
                *(uint16_t*)(shadow + 0x92F9) = 0x68;
                *(uint16_t*)(shadow + 0x92FB) = 0x34;
            }
        } else if (ax == 0x34) {
            // loc_16620: state 0x34 → next pair (0x68, 0)
            *(uint16_t*)(shadow + 0x92F9) = 0x68;
            *(uint16_t*)(shadow + 0x92FB) = 0;
            uint16_t a = *(uint16_t*)(shadow + (uint16_t)(si - 0x75A2));
            uint16_t b = *(uint16_t*)(shadow + (uint16_t)(si - 0x7570));
            if (a >= b) {
                *(uint16_t*)(shadow + 0x92F9) = 0;
                *(uint16_t*)(shadow + 0x92FB) = 0x68;
            }
        } else {
            // default: state other → next pair (0, 0x34)
            *(uint16_t*)(shadow + 0x92F9) = 0;
            *(uint16_t*)(shadow + 0x92FB) = 0x34;
            uint16_t a = *(uint16_t*)(shadow + (uint16_t)(si - 0x760A));
            uint16_t b = *(uint16_t*)(shadow + (uint16_t)(si - 0x75D8));
            if (a >= b) {
                *(uint16_t*)(shadow + 0x92F9) = 0x34;
                *(uint16_t*)(shadow + 0x92FB) = 0;
            }
        }
        // loc_1664f: if page changed → call sub_1df6a + flag dirty
        if (dx != *(uint16_t*)(shadow + 0x92FB)) {
            // Original: CALLF sub_1DF6A (position copy + VGA redraw)
            v2_sub_1DF6A(shadow);
            // Original: MOV byte ptr ds:9568h, 1
            shadow[0x9568] = 1;
        }
        // ds:0x9568 is cleared by v2_sub_1DD9C (via s[0x9568] = 0 at end of loop)
        // Do NOT clear it here — it must persist until sub_1DD9C reads it.
    }

    // sub_16661: object create/destroy based on scroll position.
    // Compares scroll tracking (ds:0x92EF/0x92F1) with current scroll (ds:0x257F/0x2581).
    // When scroll changes: create new objects entering viewport, destroy those leaving.
    {
        uint16_t ax_y = *(uint16_t*)(shadow + 0x257F);
        uint16_t cx_y = *(uint16_t*)(shadow + 0x92EF);
        if (ax_y != cx_y) {
            if ((int16_t)ax_y < (int16_t)cx_y) {
                // Scrolled up: sub_16e75 (VGA tile row render) + sub_166e8 (dirty mark)
                // sub_16e75: full DS side effects (verified with seg000 lines 14433-14521)
                // Identical structure to sub_16f5f but for upward scroll (DEC row instead of +0x29)
                {
                    uint16_t di_r = *(uint16_t*)(shadow + 0x92F1);
                    if ((int16_t)di_r > 0) di_r--; else di_r = 0;
                    di_r <<= 1;
                    uint16_t bx_r = *(uint16_t*)(shadow + (uint16_t)(di_r - 0x7098));
                    uint16_t di_pg1 = di_r + *(uint16_t*)(shadow + 0x92F9);
                    uint8_t cl_v = 0x9C;
                    uint16_t div_r1 = di_pg1;
                    shadow[0x9311] = cl_v - (uint8_t)(div_r1 % cl_v);
                    uint16_t di_vga1 = *(uint16_t*)(shadow + (uint16_t)(di_pg1 - 0x7608));
                    *(uint16_t*)(shadow + 0x9317) = di_vga1;
                    if (shadow[0x9311] < 0x32) {
                        shadow[0x9312] = 0x32 - shadow[0x9311];
                        shadow[0x9311] <<= 2; shadow[0x9312] <<= 2;
                        *(uint16_t*)(shadow + 0x9319) = *(uint16_t*)(shadow + 0x89F8);
                    } else { shadow[0x9311] = 0xC8; shadow[0x9312] = 0; }
                    uint16_t di_pg2 = di_r + *(uint16_t*)(shadow + 0x92FB);
                    shadow[0x9313] = cl_v - (uint8_t)(di_pg2 % cl_v);
                    *(uint16_t*)(shadow + 0x931B) = *(uint16_t*)(shadow + (uint16_t)(di_pg2 - 0x7608));
                    if (shadow[0x9313] < 0x32) {
                        shadow[0x9314] = 0x32 - shadow[0x9313];
                        shadow[0x9313] <<= 2; shadow[0x9314] <<= 2;
                        *(uint16_t*)(shadow + 0x931D) = *(uint16_t*)(shadow + 0x89F8);
                    } else { shadow[0x9313] = 0xC8; shadow[0x9314] = 0; }
                    uint16_t di_pg3 = di_r + *(uint16_t*)(shadow + 0x92F7);
                    uint16_t di_vga3 = *(uint16_t*)(shadow + (uint16_t)(di_pg3 - 0x7608));
                    uint16_t ax_col = *(uint16_t*)(shadow + 0x92EF);
                    if ((int16_t)ax_col > 0) ax_col--; else { /* JL locret: skip all */ goto skip_16e75; }
                    bx_r += ax_col; bx_r <<= 1;
                    ax_col <<= 1;
                    di_vga3 += ax_col + 8;
                    *(uint16_t*)(shadow + 0x9317) += ax_col + 8;
                    *(uint16_t*)(shadow + 0x9319) += ax_col + 8;
                    *(uint16_t*)(shadow + 0x931B) += ax_col + 8;
                    *(uint16_t*)(shadow + 0x931D) += ax_col + 8;
                    *(uint16_t*)(shadow + 0x9315) = di_vga3;
                    v2_sub_16dd9(shadow); v2_sub_1712b(shadow); // VGA tile row + column render
                    skip_16e75:;
                }
                // sub_166e8: mark sprites dirty if X < viewport_X
                uint16_t dx_vp = *(uint16_t*)(shadow + 0x44);
                for (int16_t di = 0xFE; di >= 0; di -= 2) {
                    if (!(*(uint16_t*)(shadow + di + 0x44D) & 0x8000)) continue;
                    if (*(uint16_t*)(shadow + di + 0x44D) & 0x6000) continue;
                    if ((int16_t)dx_vp >= (int16_t)*(uint16_t*)(shadow + di + 0x64D)) continue;
                    shadow[di + 0x114D] = 2;
                }
            } else {
                // Scrolled down: sub_16f5f (VGA scroll tile render) + sub_16710 (dirty mark)
                // sub_16f5f: full DS side effects (ds:0x9311-0x931D page tracking)
                {
                    uint16_t di_r = *(uint16_t*)(shadow + 0x92F1);
                    if ((int16_t)di_r > 0) di_r--; else di_r = 0;        // DEC di; JGE; MOV di,0
                    di_r <<= 1;                                            // SHL di, 1
                    uint16_t bx_r = *(uint16_t*)(shadow + (uint16_t)(di_r - 0x7098)); // [di-7098h]
                    // Page offset computations for ds:0x9311-0x931D
                    uint16_t di_pg1 = di_r + *(uint16_t*)(shadow + 0x92F9);
                    uint8_t cl_v = 0x9C;
                    uint16_t div_r1 = di_pg1; uint8_t q1 = (uint8_t)(div_r1 / cl_v); uint8_t r1 = (uint8_t)(div_r1 % cl_v);
                    shadow[0x9311] = cl_v - r1;                            // ds:9311h
                    uint16_t di_vga1 = *(uint16_t*)(shadow + (uint16_t)(di_pg1 - 0x7608));
                    *(uint16_t*)(shadow + 0x9317) = di_vga1;              // ds:9317h
                    if (shadow[0x9311] < 0x32) {
                        shadow[0x9312] = 0x32 - shadow[0x9311];
                        shadow[0x9311] <<= 2; shadow[0x9312] <<= 2;
                        *(uint16_t*)(shadow + 0x9319) = *(uint16_t*)(shadow + 0x89F8);
                    } else {
                        shadow[0x9311] = 0xC8; shadow[0x9312] = 0;
                    }
                    // Second page
                    uint16_t di_pg2 = di_r + *(uint16_t*)(shadow + 0x92FB);
                    uint16_t div_r2 = di_pg2; uint8_t q2 = (uint8_t)(div_r2 / cl_v); uint8_t r2_ = (uint8_t)(div_r2 % cl_v);
                    shadow[0x9313] = cl_v - r2_;
                    uint16_t di_vga2 = *(uint16_t*)(shadow + (uint16_t)(di_pg2 - 0x7608));
                    *(uint16_t*)(shadow + 0x931B) = di_vga2;
                    if (shadow[0x9313] < 0x32) {
                        shadow[0x9314] = 0x32 - shadow[0x9313];
                        shadow[0x9313] <<= 2; shadow[0x9314] <<= 2;
                        *(uint16_t*)(shadow + 0x931D) = *(uint16_t*)(shadow + 0x89F8);
                    } else {
                        shadow[0x9313] = 0xC8; shadow[0x9314] = 0;
                    }
                    // Third page + final offset
                    uint16_t di_pg3 = di_r + *(uint16_t*)(shadow + 0x92F7);
                    uint16_t di_vga3 = *(uint16_t*)(shadow + (uint16_t)(di_pg3 - 0x7608));
                    uint16_t ax_col = *(uint16_t*)(shadow + 0x92EF) + 0x29;
                    bx_r += ax_col; bx_r <<= 1; ax_col <<= 1;
                    di_vga3 += ax_col + 8;
                    *(uint16_t*)(shadow + 0x9317) += ax_col + 8;
                    *(uint16_t*)(shadow + 0x9319) += ax_col + 8;
                    *(uint16_t*)(shadow + 0x931B) += ax_col + 8;
                    *(uint16_t*)(shadow + 0x931D) += ax_col + 8;
                    *(uint16_t*)(shadow + 0x9315) = di_vga3;
                    v2_sub_16dd9(shadow); // VGA tile row render
                    v2_sub_1712b(shadow); // VGA column render
                }
                // sub_16710: mark sprites dirty if X > viewport_X + 0x121
                uint16_t dx_vp = *(uint16_t*)(shadow + 0x44) + 0x121;
                for (int16_t di = 0xFE; di >= 0; di -= 2) {
                    if (!(*(uint16_t*)(shadow + di + 0x44D) & 0x8000)) continue;
                    if (*(uint16_t*)(shadow + di + 0x44D) & 0x6000) continue;
                    if ((int16_t)dx_vp <= (int16_t)*(uint16_t*)(shadow + di + 0x64D)) continue;
                    shadow[di + 0x114D] = 2;
                }
            }
        }

        uint16_t ax_x = *(uint16_t*)(shadow + 0x2581);
        uint16_t cx_x = *(uint16_t*)(shadow + 0x92F1);
        if (ax_x != cx_x) {
            if ((int16_t)ax_x < (int16_t)cx_x) {
                // Scrolled left: sub_17049 (VGA scroll tile render) + loc_16694 (dirty mark)
                // sub_17049: full DS side effects (verified with seg000 lines 14607-14652)
                {
                    uint16_t di_r = *(uint16_t*)(shadow + 0x92F1);            // ds:92F1h
                    if ((int16_t)di_r > 0) di_r--; else di_r = 0;            // DEC; JGE; MOV 0
                    di_r <<= 1;                                                // SHL di, 1
                    uint16_t bx_r = *(uint16_t*)(shadow + (uint16_t)(di_r - 0x7098)); // [di-7098h]
                    *(uint16_t*)(shadow + 0x9305) = di_r + *(uint16_t*)(shadow + 0x92F9); // page 1
                    *(uint16_t*)(shadow + 0x9307) = di_r + *(uint16_t*)(shadow + 0x92FB); // page 2
                    *(uint16_t*)(shadow + 0x9309) = di_r + *(uint16_t*)(shadow + 0x92F7); // page 3
                    uint16_t dx_col = *(uint16_t*)(shadow + 0x92EF);
                    if ((int16_t)dx_col > 0) dx_col--; else dx_col = 0;
                    bx_r += dx_col; bx_r <<= 1;
                    dx_col = dx_col * 2 + 8;                                  // SHL dx,1; ADD dx,8
                    uint16_t pg1 = *(uint16_t*)(shadow + 0x9305);
                    *(uint16_t*)(shadow + 0x930D) = *(uint16_t*)(shadow + (uint16_t)(pg1 - 0x7608)) + dx_col;
                    uint16_t pg2 = *(uint16_t*)(shadow + 0x9307);
                    *(uint16_t*)(shadow + 0x930F) = *(uint16_t*)(shadow + (uint16_t)(pg2 - 0x7608)) + dx_col;
                    uint16_t pg3 = *(uint16_t*)(shadow + 0x9309);
                    *(uint16_t*)(shadow + 0x930B) = *(uint16_t*)(shadow + (uint16_t)(pg3 - 0x7608)) + dx_col;
                    v2_sub_16dc1(shadow, 0, 0); // VGA column render
                    v2_sub_171dc(shadow);       // VGA page copy
                }
                // loc_16694: mark sprites dirty if Y < viewport_Y
                uint16_t dx_vp = *(uint16_t*)(shadow + 0x46);
                for (int16_t di = 0xFE; di >= 0; di -= 2) {
                    if (!(*(uint16_t*)(shadow + di + 0x44D) & 0x8000)) continue;
                    if (*(uint16_t*)(shadow + di + 0x44D) & 0x6000) continue;
                    if ((int16_t)dx_vp >= (int16_t)*(uint16_t*)(shadow + di + 0x74D)) continue;
                    shadow[di + 0x114D] = 2;
                }
            } else {
                // Scrolled right: sub_170b9 (VGA scroll tile render) + loc_166bc (dirty mark)
                // sub_170b9: full DS side effects (verified with seg000 lines 14656-14702)
                {
                    uint16_t di_r = *(uint16_t*)(shadow + 0x92F1) + 0x17;   // ADD di, 17h
                    if ((int16_t)di_r < 0) di_r = 0;                        // JGE; MOV 0
                    di_r <<= 1;
                    uint16_t bx_r = *(uint16_t*)(shadow + (uint16_t)(di_r - 0x7098));
                    *(uint16_t*)(shadow + 0x9305) = di_r + *(uint16_t*)(shadow + 0x92F9);
                    *(uint16_t*)(shadow + 0x9307) = di_r + *(uint16_t*)(shadow + 0x92FB);
                    *(uint16_t*)(shadow + 0x9309) = di_r + *(uint16_t*)(shadow + 0x92F7);
                    uint16_t dx_col = *(uint16_t*)(shadow + 0x92EF);
                    if ((int16_t)dx_col > 0) dx_col--; else dx_col = 0;
                    bx_r += dx_col; bx_r <<= 1;
                    dx_col = dx_col * 2 + 8;
                    uint16_t pg1 = *(uint16_t*)(shadow + 0x9305);
                    *(uint16_t*)(shadow + 0x930D) = *(uint16_t*)(shadow + (uint16_t)(pg1 - 0x7608)) + dx_col;
                    uint16_t pg2 = *(uint16_t*)(shadow + 0x9307);
                    *(uint16_t*)(shadow + 0x930F) = *(uint16_t*)(shadow + (uint16_t)(pg2 - 0x7608)) + dx_col;
                    uint16_t pg3 = *(uint16_t*)(shadow + 0x9309);
                    *(uint16_t*)(shadow + 0x930B) = *(uint16_t*)(shadow + (uint16_t)(pg3 - 0x7608)) + dx_col;
                    v2_sub_16dc1(shadow, 0, 0); v2_sub_171dc(shadow); // VGA column/tile render
                }
                // loc_166bc: mark sprites dirty if Y > viewport_Y + 0x91
                uint16_t dx_vp = *(uint16_t*)(shadow + 0x46) + 0x91;
                for (int16_t di = 0xFE; di >= 0; di -= 2) {
                    if (!(*(uint16_t*)(shadow + di + 0x44D) & 0x8000)) continue;
                    if (*(uint16_t*)(shadow + di + 0x44D) & 0x6000) continue;
                    if ((int16_t)dx_vp <= (int16_t)*(uint16_t*)(shadow + di + 0x74D)) continue;
                    shadow[di + 0x114D] = 2;
                }
            }
        }
    }

    // sub_1406d: animation queue processing — VGA tile rendering for queued updates.
    // Iterates queue at ds:0x8734 (count, step 3). For each entry:
    //   Reads position from ds:[bx-0x78CA/C8/C6] lookup table.
    //   Calculates viewport-relative position.
    //   Draws up to 4 tiles via sub_1689e to VGA pages.
    // DS writes: only temporary (ds:0x6C, ds:0x6E). No game state changes.
    // Queue cleared by word_30C14=0 at end of frame.
    // For v2: tiles rendered each frame by v2_draw_tiles.
    // VGA calls (v2_sub_1689e per tile) are no-ops — pure VGA pixel copy.
    {
        uint16_t count = *(uint16_t*)(shadow + 0x8734);
        if (count != 0) {
            // Process queue entries (cx from count-3 down to 0, step -3)
            for (int16_t cx = (int16_t)count - 3; cx >= 0; cx -= 3) {
                uint16_t bx = (uint16_t)cx * 2;
                // Read position from queue lookup table
                // uint16_t bp_val = *(uint16_t*)(shadow + (uint16_t)(bx - 0x78CA)); // FS tile offset
                uint16_t pos_x = *(uint16_t*)(shadow + (uint16_t)(bx - 0x78C8));
                uint16_t pos_y = *(uint16_t*)(shadow + (uint16_t)(bx - 0x78C6));
                *(uint16_t*)(shadow + 0x6C) = pos_x;
                *(uint16_t*)(shadow + 0x6E) = pos_y;
                // Viewport bounds check + edge detection (dx flags: bit3=left, bit2=right, bit1=top, bit0=bottom)
                // For each visible quadrant (4 tiles per queue entry):
                //   OUT(0x3C4, 0x0F02); // enable all VGA planes
                //   si = fs:[bp+offset]; // read tile index from FS (render tilemap)
                //   sub_1689e(si, di);   // draw tile to VGA page at offset di
                // sub_1689e: reads tile graphics from ds:0x2E5F segment, writes to VGA 0xA000
                //   OUT(0x3C4, 0x0102/0x0202/0x0402/0x0802); // plane select for each of 4 planes
                //   REP MOVSW from tile data to es:di (VGA)
            }
        }
    }
}

// Per-frame update: PERSISTENT shadow — NO memcpy from real DS.
// v2 maintains its own state. Only trace counters and real_ds_ptr updated.
static void v2_vm_frame_update(uint8_t* ds) {
    // No sync copies needed — v2 runs in separate thread with barrier synchronization.
    // Each phase runs at the same time as the original → no timing artifacts.
    v2_vm_acc_base = v2_vm_shadow_ds;

    // word_3287C (DS:0xA39C): v2 manages independently.
    // Original: main loop sets to 1, render callback DECs to 0.
    // v2: set to 0 each frame (VM expects 0 after render callback).
    *(uint16_t*)(v2_vm_shadow_ds + 0xA39C) = 0;

    // No level detection heuristics — level loading is driven by game logic
    // (sub_11080 → sub_111b1 → sub_11204 → v2_read_chunk).

    memset(v2_vm_trace_count, 0, sizeof(v2_vm_trace_count));
    v2_vm_real_ds_ptr = ds;
}

// Combined: init or update
static void v2_vm_reset_frame_state(uint8_t* ds) {
    // v2_startup handles initialization via v2_load_exe_ds + sub_12948..sub_108b8.
    // If shadow not initialized yet, v2_startup will handle it.
    v2_vm_frame_update(ds);
}

// ============================================================================
// VM State
// ============================================================================
struct V2VM {
    uint8_t* ds;          // Real DS segment (for reading data outside shadow range)
    uint8_t* shadow;      // Shadow DS (for reads/writes within animation table)
    uint8_t* es;          // Current code segment pointer
    uint8_t* cs_base;     // CS segment base (seg000 = m2c_base + 0x1A20)
    uint16_t obj;         // Current object index
    uint16_t pc;          // Bytecode pointer
    bool running;         // False when opcode 0x00 yields
    bool carry;           // Carry flag (set by animation load functions)
    int slot;             // Object slot index (obj / 2)

    // Bytecode read helpers
    uint8_t  read_u8()  {
        if (pc >= 0xC000) { static int _h=0; _h++; if(_h<=5) fprintf(stderr,"V2-ES-READ8: pc=%04X obj=%d\n",pc,obj); }
        uint8_t  v = es[pc]; pc += 1; return v;
    }
    uint16_t read_u16() {
        if (pc >= 0xC000) { static int _h=0; _h++; if(_h<=5) fprintf(stderr,"V2-ES-READ16: pc=%04X obj=%d\n",pc,obj); }
        uint16_t v = *(uint16_t*)(es + pc); pc += 2; return v;
    }

    // DS read: use shadow if within range, otherwise real DS
    uint16_t ds_read(uint16_t addr) {
        if (addr < V2_VM_SHADOW_SIZE - 1)
            return *(uint16_t*)(shadow + addr);
        return *(uint16_t*)(ds + addr);
    }

    // DS write: write to shadow if within range (never write to real DS)
    void ds_write(uint16_t addr, uint16_t val) {
        if (addr < V2_VM_SHADOW_SIZE - 1) {
            // Trap: watch sub-sprite mode for slots 0x0030-0x0034 + addr-1 spillover
            if (addr >= 0x117C && addr <= 0x1181) {
                uint16_t old = *(uint16_t*)(shadow + addr);
                if (val != old) {
                    static int _tw2 = 0; _tw2++;
                    if (_tw2 <= 50) fprintf(stderr, "V2-TRAP-MODE: W addr=%04X obj=%04X val=%04X was=%04X pc=%04X [%d]\n",
                        addr, obj, val, old, pc, _tw2);
                }
            }
            // (0x077E trace removed — root cause: VGA interrupt timing in original)
            if (addr == 0x3CC && val != *(uint16_t*)(shadow + addr)) {
                static int _tw = 0; if (_tw < 5) { _tw++;
                printf("V2-VM-WRITE-3CC: obj=%04X val=%04X (was %04X)\n",
                    obj, val, *(uint16_t*)(shadow + addr)); }
            }
            *(uint16_t*)(shadow + addr) = val;
        }
    }

    // DS byte write
    void ds_write_b(uint16_t addr, uint8_t val) {
        if (addr < V2_VM_SHADOW_SIZE) {
            if ((addr == 0x117D || addr == 0x117F) && val != shadow[addr]) {
                static int _tb = 0; if (_tb < 20) { _tb++;
                fprintf(stderr, "V2-TRAP-BYTE: addr=%04X obj=%04X val=%02X was=%02X pc=%04X [%d]\n",
                    addr, obj, val, shadow[addr], pc, _tb); }
            }
            shadow[addr] = val;
        }
    }

    // Field access for current object
    uint16_t field_r(uint16_t offset) { return ds_read(obj + offset); }
    void     field_w(uint16_t offset, uint16_t val) { ds_write(obj + offset, val); }

    // Global access
    uint16_t global_r(uint16_t offset) { return ds_read(offset); }
    void     global_w(uint16_t offset, uint16_t val) { ds_write(offset, val); }
};

// ============================================================================
// Opcode handler type
// ============================================================================
typedef void (*V2VMOpFn)(V2VM& vm);

// Forward declarations
static void v2_vm_op_unimpl(V2VM& vm);
static bool v2_vm_collision_check_155d6(V2VM& vm);
static void v2_vm_runtime_dispatch(V2VM& vm, uint16_t table_ds_offset, uint16_t index);
static uint16_t v2_vm_read_literal(V2VM& vm);
static uint16_t v2_vm_read_indexed_field(V2VM& vm);
static void v2_vm_do_jump(V2VM& vm);

// ============================================================================
// Opcode table (216 entries)
// ============================================================================
static V2VMOpFn v2_vm_optable[256];
static bool v2_vm_table_initialized = false;

// ============================================================================
// Control flow opcodes
// ============================================================================

// 0x00: Yield — save PC to v2 state, exit VM loop
// Exact replica of sub_142b7: POP ax; si = ds:0x42; [si+0x132D] = bx; RETN
static void v2_vm_op_yield(V2VM& vm) {
    uint16_t si = vm.global_r(0x42);
    vm.ds_write(si + 0x132D, vm.pc);  // write PC to shadow DS, exactly like original
    vm.running = false;
}

// 0x01: NOP
static void v2_vm_op_nop(V2VM& vm) {
    (void)vm;
}

// 0x03: Jump — read word, set as new PC
static void v2_vm_op_jump(V2VM& vm) {
    vm.pc = *(uint16_t*)(vm.es + vm.pc);
}

// Forward declarations
static void v2_vm_do_jump(V2VM& vm);
static void v2_vm_do_call_jump(V2VM& vm);
static uint16_t v2_vm_read_literal(V2VM& vm);
static uint16_t v2_vm_read_indexed_field(V2VM& vm);
static uint16_t v2_vm_read_indirect(V2VM& vm);
static uint16_t v2_vm_read_indexed_field_1995(V2VM& vm);
static uint16_t v2_vm_dispatch_30C98(V2VM& vm, uint8_t mode);
static void v2_vm_sub_154bf(V2VM& vm, uint16_t ax_val, uint8_t mode);
static uint16_t v2_vm_sub_1250b(V2VM& vm, uint8_t& out_mode);
static void v2_vm_sub_125a3(V2VM& vm, uint16_t& out_si, uint16_t& out_di);
static bool v2_vm_exec_anim_cmd(V2VM& vm, uint16_t handler, uint16_t& anim_bx);
static uint16_t v2_vm_read_indexed_field_15445(V2VM& vm);

// 0x05 = sub_142c1 (same as call-jump): save alt_pc, then jump
static void v2_vm_op_save_alt_pc(V2VM& vm) {
    v2_vm_do_call_jump(vm);
}

// ============================================================================
// Runtime sub-dispatch: read function pointer from DS table, map to v2 handler.
// Tables: off_30BC6 (ds:0x86E6), off_30C8E (ds:0x87AE), off_30CA2 (ds:0x87C2)
// Each entry is a CS offset (uint16_t). We map known offsets to v2 handlers.
// Unknown offsets → log and stop.
// ============================================================================

// Forward declare sub-dispatch handlers
static void v2_vm_subdispatch_unimpl(V2VM& vm, uint16_t cs_addr);

// Dispatch through a runtime table. Reads entry from real DS.
// table_ds_offset = DS offset of the dispatch table
// index = entry index (NOT multiplied by 2 — we do it here)
static void v2_vm_runtime_dispatch(V2VM& vm, uint16_t table_ds_offset, uint16_t index) {
    uint16_t cs_addr = *(uint16_t*)(vm.ds + table_ds_offset + index * 2);

    // Map CS address to v2 handler
    switch (cs_addr) {
    // off_30C8E entries:
    case 0x42CF: // sub_142cf — jump (bx = es:[bx])
        v2_vm_do_jump(vm);
        break;

    case 0x44E9: // loc_144e9: no carry → skip 2, carry → jump
        if (vm.carry)
            v2_vm_do_jump(vm);
        else
            vm.pc += 2;
        break;

    case 0x44F3: // loc_144f3: carry → skip 2, no carry → jump
        if (vm.carry)
            vm.pc += 2;
        else
            v2_vm_do_jump(vm);
        break;

    default:
        v2_vm_subdispatch_unimpl(vm, cs_addr);
        break;
    }
}

static void v2_vm_subdispatch_unimpl(V2VM& vm, uint16_t cs_addr) {
    static bool logged[0x10000] = {};
    if (!logged[cs_addr]) {
        printf("V2-VM: unimplemented sub-dispatch cs:0x%04X obj=%d\n", cs_addr, vm.obj);
        logged[cs_addr] = true;
    }
    // Can't continue — unknown handler may consume unknown bytes
    vm.running = false;
}

// 0x0F (sub_1431c): Exit VM + set flag ds:0x334 |= 1. Same as yield.
// 0x0F = sub_1431c: [si+0x132D] = bx; OR [si+0x1585], 0x200; POP ax; RETN
static void v2_vm_op_exit_with_flag(V2VM& vm) {
    // sub_1431c: OR [DS:334h], 1; MOV SI, [DS:42h]; POP AX; RETN
    // Sets bit 0 in global button flag (level transition trigger).
    // Does NOT save PC (permanent exit — object cannot resume).
    // POP AX discards dispatcher return address — exits VM entirely.
    *(uint16_t*)(vm.shadow + 0x0334) |= 1;
    vm.running = false;
}

// Generic skip helpers (used by various opcodes)
static void v2_vm_op_skip1(V2VM& vm) { vm.pc += 1; }
static void v2_vm_op_skip2(V2VM& vm) { vm.pc += 2; }
static void v2_vm_op_skip3(V2VM& vm) { vm.pc += 3; }

// ============================================================================
// Sound opcodes (no rendering effect)
// ============================================================================

// 0x02 (sub_177b2): Play sound sequence. 2 bytes consumed.
// Original: reads sequence number, checks ds:0x304 (sound disabled flag),
// finds free sound slot in ds:[si-0x66F4] table (si=8..?),
// stores sequence in ds:[si-0x66EA], calls sub_176bd(bx=ds:0x2E6D, ax=seq).
static void v2_vm_op_sound(V2VM& vm) {
    uint16_t seq = vm.read_u16();
    seq &= 0xFF; // AND ax, 0FFh
    if (vm.ds_read(0x304) != 0) return; // sound disabled
    // Original sub_177bb: si=8, check if slot free.
    // If [si-0x66F4] == 0xFFFF → free: store seq in [si-0x66EA], call sub_176bd. Return.
    // If occupied → check AIL status (sub_1c7ab). If status == 1 → sub si,2;jg → try next.
    // If status != 1 (done) → stop+release old, store new seq, call sub_176bd. Return.
    // For v2: no AIL calls, so occupied slots stay occupied. Only free slots can be used.
    for (int16_t si = 8; si > 0; si -= 2) {
        uint16_t handle_addr = (uint16_t)(si - 0x66F4);
        uint16_t seq_addr = (uint16_t)(si - 0x66EA);
        if (vm.ds_read(handle_addr) == 0xFFFF) {
            // Free slot: store sequence and mark as used-then-done
            vm.ds_write(seq_addr, seq);
            // Original calls sub_176bd which allocates AIL handle, starts playback.
            // No real AIL in v2. Write 0xFFFF as handle (same as "done/free"),
            // matching original behavior after sound completes.
            vm.ds_write(handle_addr, 0xFFFF);
            break;
        }
        // Occupied: original checks AIL status via sub_1c7ab.
        // If status == 1 (playing) → try next slot.
        // If status != 1 (done) → stop, release, reuse.
        // v2: no AIL, treat all occupied slots as "done" → reuse immediately.
        vm.ds_write(seq_addr, seq);
        vm.ds_write(handle_addr, 0xFFFF);
        break;
    }
}

// 0x04 (sub_1782a): Stop sound. 1 byte consumed.
// Original: reads channel number, checks ds:0x304, finds matching slot,
// calls AIL stop sequence.
static void v2_vm_op_sound1(V2VM& vm) {
    uint8_t param = vm.read_u8();
    param &= 0xFF;
    if (vm.ds_read(0x304) != 0) return; // sound disabled
    // Original sub_1782a: si=8, search DOWN for matching sequence in ds:[si-0x66EA].
    // If found: stop AIL playback, mark slot free (both handle and seq = 0xFFFF).
    for (int16_t si = 8; si > 0; si -= 2) {
        if (vm.ds_read((uint16_t)(si - 0x66EA)) == param) {
            // sub_1c79f: stop_sequence(handle, driver)
            // stop_xmidi_external();
            // sub_1c769: release_sequence(handle, driver)
            // Mark slot as free
            vm.ds_write((uint16_t)(si - 0x66F4), 0xFFFF);
            vm.ds_write((uint16_t)(si - 0x66EA), 0xFFFF);
            // Don't break — original continues loop (sub si,2; jg)
        }
    }
}

// ============================================================================
// Tile map helper functions — exact replicas of original sub_141ba/b3/e0/13fc2
// ============================================================================

// sub_141ba: Read tile from tile map at (si, di). Returns full 16-bit tile word.
// Bounds-checks si against ds:0x25DC, di against ds:0x25DE.
// Out of bounds: returns 0x400 (si) or 0 (di).
static uint16_t v2_vm_sub_141ba(V2VM& vm, uint16_t si, uint16_t di) {
    if (si >= vm.ds_read(0x25DC)) return 0x400;
    if (di >= vm.ds_read(0x25DE)) return 0;
    uint16_t di2 = di << 1;
    uint16_t si2 = si << 1;
    si2 += vm.ds_read((uint16_t)(di2 - 0x7098));
    // Read from shadow tile map
    if (v2_tilemap_shadow_valid && si2 < V2_TILEMAP_SHADOW_SIZE - 1) {
        return *(uint16_t*)(v2_vm_shadow_tilemap + si2);
    }
    return 0;
}

// sub_141b3: Read tile index (lower 10 bits) at (si, di).
static uint16_t v2_vm_sub_141b3(V2VM& vm, uint16_t si, uint16_t di) {
    return v2_vm_sub_141ba(vm, si, di) & 0x3FF;
}

// sub_141e0: Write tile word ax to tile map at (si, di).
// push si, di; di*=2; si*=2; si+=ds:[di-0x7098]; es=ds:0x2E63; es:[si]=ax; pop di, si.
static void v2_vm_sub_141e0(V2VM& vm, uint16_t si, uint16_t di, uint16_t ax) {
    // Write to shadow tile map (not real memory — original handles the real tile map).
    uint16_t di2 = di << 1;
    uint16_t si2 = si << 1;
    si2 += vm.ds_read((uint16_t)(di2 - 0x7098));
    if (v2_tilemap_shadow_valid && si2 < V2_TILEMAP_SHADOW_SIZE - 1) {
        *(uint16_t*)(v2_vm_shadow_tilemap + si2) = ax;
    }
}

// sub_13fc2: Mark tile dirty — writes to DS tracking arrays + VGA pages.
// Writes: ds:0x32, ds:0x6C, ds:0x6E, tracking arrays at ds:[bx-0x78C*], ds:0x8734.
// Also writes 4 tile words to FS (VGA) pages — skipped for v2.
// Called with si=X_tile, di=Y_tile, ax=tile_value.
static int v2_13fc2_count = 0;
static void v2_vm_sub_13fc2(V2VM& vm, uint16_t si, uint16_t di, uint16_t ax) {
    v2_13fc2_count++;
    vm.ds_write(0x32, ax);
    uint16_t x_pix = si << 4;   // si * 16
    uint16_t y_pix = di << 4;   // di * 16
    vm.ds_write(0x6C, x_pix);
    vm.ds_write(0x6E, y_pix);

    // Compute tile position for VGA page
    uint16_t di_q = y_pix >> 2;
    uint16_t si_q = x_pix >> 2;
    uint16_t row_base = vm.ds_read((uint16_t)(di_q - 0x7098));
    uint16_t bp_val = (row_base << 1) + si_q;

    // FS writes: original reads 4 tile words from ES (tilemap), writes to FS (render tilemap)
    // with OR 1 (dirty bit) at VGA page addresses.
    // ES = ds:0x2E63, FS = ds:0x2E69. May be same or different segments.
    // bx = (tile_value & 0x3FF) * 8 + ds:0x2E65
    // Page 1: fs:[bp] = es:[bx] | 1, fs:[bp+2] = es:[bx+2] | 1
    // Page 2: bp += ds:0x8F6C; fs:[bp] = es:[bx+4] | 1, fs:[bp+2] = es:[bx+6] | 1
    // v2: write to shadow tilemap (covers both ES and FS if same segment).
    // If FS != ES, verify_fs_vs_es will detect the difference.
    {
        // Original: bx = (ax & 0x3FF) * 8. Reads from GS tiledata at ds:[bx + ds:0x2E65].
        // BUT: the READ source is NOT tilemap — it's the GS tiledata area appended TO tilemap
        // at offset ds:0x2E65 (= decompressed tilemap size, where GS→ES copy was placed).
        // Reads: es:[bx], es:[bx+2], es:[bx+4], es:[bx+6] (from tilemap segment = GS copy area)
        // Writes: fs:[bp], fs:[bp+2], fs:[bp+stride], fs:[bp+stride+2] (to FS render tilemap)
        uint16_t bx_tile = (ax & 0x3FF) << 3;
        bx_tile += vm.ds_read(0x2E65);
        // READ from tilemap (ES) at GS copy area
        uint16_t w0 = *(uint16_t*)(v2_vm_shadow_tilemap + bx_tile);
        uint16_t w1 = *(uint16_t*)(v2_vm_shadow_tilemap + bx_tile + 2);
        uint16_t w2 = *(uint16_t*)(v2_vm_shadow_tilemap + bx_tile + 4);
        uint16_t w3 = *(uint16_t*)(v2_vm_shadow_tilemap + bx_tile + 6);
        // WRITE to FS (render tilemap) with dirty bit OR 1
        // Page 1: fs:[bp], fs:[bp+2]
        *(uint16_t*)(v2_vm_shadow_fs + bp_val) = w0 | 1;
        *(uint16_t*)(v2_vm_shadow_fs + (uint16_t)(bp_val + 2)) = w1 | 1;
        // Page 2: fs:[bp + stride], fs:[bp + stride + 2]
        uint16_t bp2 = bp_val + vm.ds_read(0x8F6C);
        *(uint16_t*)(v2_vm_shadow_fs + bp2) = w2 | 1;
        *(uint16_t*)(v2_vm_shadow_fs + (uint16_t)(bp2 + 2)) = w3 | 1;
    }

    // Tracking arrays: store tile position + pixel coords
    uint16_t cx_count = vm.ds_read(0x8734);
    uint16_t bx_idx = cx_count << 1;
    vm.ds_write((uint16_t)(bx_idx - 0x78CA), bp_val);
    vm.ds_write((uint16_t)(bx_idx - 0x78C8), x_pix);
    vm.ds_write((uint16_t)(bx_idx - 0x78C6), y_pix);

    // Viewport bounds check — increment counter by 3 if tile visible
    uint16_t vx = (uint16_t)(x_pix - vm.ds_read(0x44) + 0x10);
    if (vx <= 0x160) {
        uint16_t vy = (uint16_t)(y_pix - vm.ds_read(0x46) + 0x10);
        if (vy <= 0xD0) {
            vm.ds_write(0x8734, cx_count + 3);
        }
    }
}

// ============================================================================
// Full logic opcodes — exact replicas of original
// ============================================================================

// Forward declaration for vflip (0x0C defined below)
static void v2_vm_op_0C(V2VM& vm);

// sub_15505: [si+0x15AD] -= [di+0x15D5]; if no borrow → 0. 0 bytes.
static void v2_vm_sub_15505(V2VM& vm, uint16_t si, uint16_t di) {
    uint16_t a = vm.ds_read(si + 0x15AD);
    uint16_t b = vm.ds_read(di + 0x15D5);
    vm.ds_write(si + 0x15AD, (a < b) ? (uint16_t)(a - b) : 0);
}

// 0x11 (sub_14334): si=ds:0x42, di=[si+0x1995] → sub_15505. 0 bytes.
static void v2_vm_op_11(V2VM& vm) {
    uint16_t si = vm.global_r(0x42);
    uint16_t di = vm.ds_read(si + 0x1995);
    v2_vm_sub_15505(vm, si, di);
}

// 0x3A (sub_14340): di=ds:0x42, si=[di+0x1995] → sub_15505. 0 bytes.
// Note: si/di SWAPPED vs 0x11.
static void v2_vm_op_3A(V2VM& vm) {
    uint16_t di = vm.global_r(0x42);
    uint16_t si = vm.ds_read(di + 0x1995);
    v2_vm_sub_15505(vm, si, di);
}

// 0x09 (sub_13743): Conditional vflip — if flag 0x80 NOT set → call sub_13757. 0 bytes.
static void v2_vm_op_09(V2VM& vm) {
    uint16_t si = vm.global_r(0x42);
    if (!(vm.ds_read(si + 0x1585) & 0x80)) v2_vm_op_0C(vm);
}

// 0x0A (sub_13733): Conditional vflip — if flag 0x80 set → call sub_13757. 0 bytes.
static void v2_vm_op_0A(V2VM& vm) {
    uint16_t si = vm.global_r(0x42);
    if (vm.ds_read(si + 0x1585) & 0x80) v2_vm_op_0C(vm);
}

// 0xD4 (sub_1531c): velocity direction from position delta. Exact replica.
// Reads: 1 mode byte (off_30C98 X+Y dispatch, may consume 0-4 more bytes) + 1 threshold byte.
// Writes: [si+0x164D] = X velocity, [si+0x1675] = Y velocity.
static void v2_vm_op_D4(V2VM& vm) {
    vm.ds_write(0x3E, 0);
    vm.ds_write(0x40, 0);
    // Read mode byte and dispatch off_30C98 for X target
    uint8_t mode = vm.read_u8();
    vm.ds_write(0x34, mode);
    uint16_t target_x = v2_vm_dispatch_30C98(vm, mode & 7);
    uint16_t si = vm.global_r(0x42);
    int16_t delta_x = (int16_t)(target_x - vm.ds_read(si + 0x173D));
    if (delta_x < 0) { delta_x = -delta_x; vm.ds_write(0x3E, 1); }
    vm.ds_write(0x6C, (uint16_t)delta_x);
    // Y target: dispatch off_30C98 with (mode >> 3) & 7
    uint16_t target_y = v2_vm_dispatch_30C98(vm, (vm.ds_read(0x34) >> 3) & 7);
    si = vm.global_r(0x42);
    int16_t delta_y = (int16_t)(target_y - vm.ds_read(si + 0x1765));
    if (delta_y < 0) { delta_y = -delta_y; vm.ds_write(0x40, 1); }
    vm.ds_write(0x6E, (uint16_t)delta_y);
    // max(|dx|, |dy|)
    uint16_t max_d = (uint16_t)delta_x >= (uint16_t)delta_y ? (uint16_t)delta_x : (uint16_t)delta_y;
    // Read threshold byte
    uint8_t threshold = vm.read_u8();
    vm.ds_write(0x32, threshold);
    // Halving loop with fractional parts (RCR)
    uint16_t ds34 = 0, ds36 = 0;
    uint16_t ds6c = (uint16_t)delta_x, ds6e = (uint16_t)delta_y;
    while ((int16_t)max_d > (int16_t)threshold) {
        uint16_t carry;
        max_d >>= 1;
        carry = ds6c & 1; ds6c >>= 1; ds34 = (ds34 >> 1) | (carry << 15);
        carry = ds6e & 1; ds6e >>= 1; ds36 = (ds36 >> 1) | (carry << 15);
    }
    // Flip flag adjustments
    si = vm.global_r(0x42);
    if (vm.ds_read(si + 0x1585) & 0x40) vm.ds_write(0x3E, vm.ds_read(0x3E) ^ 1);
    if (vm.ds_read(si + 0x1585) & 0x80) vm.ds_write(0x40, vm.ds_read(0x40) ^ 1);
    // Combine: (ds34_high | ds6c_low), swap bytes
    uint16_t xr = (ds34 & 0xFF00) | (ds6c & 0xFF);
    xr = ((xr >> 8) & 0xFF) | ((xr & 0xFF) << 8); // XCHG ah, al
    if (vm.ds_read(0x3E) != 0) xr = (uint16_t)(-(int16_t)xr);
    vm.ds_write(si + 0x164D, xr);
    uint16_t yr = (ds36 & 0xFF00) | (ds6e & 0xFF);
    yr = ((yr >> 8) & 0xFF) | ((yr & 0xFF) << 8);
    if (vm.ds_read(0x40) != 0) yr = (uint16_t)(-(int16_t)yr);
    vm.ds_write(si + 0x1675, yr);
}

// 0xD5 (sub_178d6): Sound play via sub_176bd. 1 byte consumed.
// Original: INC bx (1 byte), test ds:0x302, if 0: call sub_176bd(si=0, ax=0, bx=ds:0x2E6B).
static void v2_vm_op_D5(V2VM& vm) {
    vm.pc += 1;
    if (vm.ds_read(0x302) != 0) return;
    // sub_176bd: play sound from ds:0x2E6B segment
    // uint16_t snd_seg = vm.ds_read(0x2E6B);
    // play_xmidi_external(v2_m2c_base + (uint32_t)snd_seg * 16, ..., 0);
}

// 0xD6 (sub_178f1): Timer delay. 0 bytes consumed.
// Original: test ds:0x302, if 0: pushf, cli, push args, call sub_1c7bd (delay), popf.
static void v2_vm_op_D6(V2VM& vm) {
    (void)vm;
    // Timer delay — no effect on game state for v2.
    // Original calls AIL timer for 1000ms delay.
    // if (vm.ds_read(0x302) == 0) { /* delay 1000ms */ }
}

// 0xD7 (sub_1787f): Sound sequence check + restart. 3 bytes consumed.
// Original: reads 1 byte (seq), ADD bx,3. If ds:0x304 != 0 → skip.
// Searches ds:[si-0x66EA] for matching seq (si=8 down to 2).
// If found: stop old (AIL), restart with new sequence. If not found: no action.
static void v2_vm_op_D7(V2VM& vm) {
    uint8_t seq = vm.es[vm.pc] & 0xFF;
    vm.pc += 3;
    if (vm.ds_read(0x304) != 0) return;
    for (int16_t si = 8; si > 0; si -= 2) {
        if (vm.ds_read((uint16_t)(si - 0x66EA)) == seq) {
            // Found matching slot: stop + restart sequence
            // AIL: stop_sequence, release_sequence, then allocate + start new
            // sub_176bd(ax=seq, bx=ds:0x2E6D, si=slot)
            // uint16_t snd_seg = vm.ds_read(0x2E6D);
            // stop_xmidi_external();
            // play_xmidi_external(v2_m2c_base + (uint32_t)snd_seg*16, ..., seq);
            break;
        }
    }
}

// Viewport check helper (used by 0xCC, 0xCD, 0xCE)
static bool v2_vm_viewport_check(V2VM& vm, uint16_t si) {
    uint16_t obj_x = vm.ds_read(si + 0x173D);
    uint16_t obj_y = vm.ds_read(si + 0x1765);
    uint16_t vp_x = vm.ds_read(0x44);
    uint16_t vp_y = vm.ds_read(0x46);
    int16_t ax = (int16_t)(vp_x + 0x1F);
    if (ax >= (int16_t)obj_x) return false;
    ax += 0x102;
    if (ax < (int16_t)obj_x) return false;
    ax = (int16_t)(vp_y + 0x1F);
    if (ax >= (int16_t)obj_y) return false;
    ax += 0x72;
    if (ax < (int16_t)obj_y) return false;
    return true;
}

// 0xCC (sub_152de): viewport check (self). off_30C92[0]: within→jump, outside→skip2. 2 bytes.
static void v2_vm_op_CC(V2VM& vm) {
    uint16_t si = vm.global_r(0x42);
    if (v2_vm_viewport_check(vm, si)) { v2_vm_do_call_jump(vm); }
    else { vm.pc += 2; }
}

// 0xCD (sub_152ca): viewport check (linked obj). off_30C92[0]: within→jump, outside→skip2. 2 bytes.
static void v2_vm_op_CD(V2VM& vm) {
    uint16_t si = vm.global_r(0x42);
    si = vm.ds_read(si + 0x1995);
    if (v2_vm_viewport_check(vm, si)) { v2_vm_do_call_jump(vm); }
    else { vm.pc += 2; }
}

// Forward declarations for C0-C6 opcodes
static bool v2_vm_sub_15fbe(V2VM& vm, uint16_t filter_si, uint16_t obj_di);
static bool v2_vm_sub_15fb1(V2VM& vm, uint16_t filter_si, uint16_t obj_di);
static bool v2_vm_loc_15dfd(V2VM& vm, uint16_t filter_si, uint16_t obj_di, uint16_t x_search);
static bool v2_vm_sub_15de5(V2VM& vm, uint16_t filter_si, uint16_t obj_di);
static void v2_vm_do_call_jump(V2VM& vm);

// Helper: vikings-only object search wrapper.
// Temporarily sets ds:0x372=6 (only search 3 vikings), runs search, restores.
// Used by 0xC0-0xC6 opcodes.
static void v2_vm_vikings_obj_search(V2VM& vm, uint16_t off_c8e_idx,
    void (*search_fn)(V2VM&, uint16_t, uint16_t)) {
    vm.ds_write(0x3B4, 0xFFFF);
    uint16_t saved_372 = vm.ds_read(0x372);
    vm.ds_write(0x372, 6);
    uint8_t filter = vm.read_u8();
    uint16_t di = vm.global_r(0x42);
    // Manually handle different search functions
    (void)search_fn; (void)filter; (void)di;
    vm.ds_write(0x372, saved_372);
    // off_30C8E dispatch
    if (off_c8e_idx == 0) {
        if (vm.carry) { uint16_t target = *(uint16_t*)(vm.es + vm.pc); vm.pc = target; }
        else { vm.pc += 2; }
    } else {
        if (vm.carry) { vm.pc += 2; }
        else { v2_vm_do_call_jump(vm); }
    }
}

// 0xC0 (sub_1518a): vikings-only sub_15fbe (Y_end+1) + off_30C8E[0]. 1 byte.
static void v2_vm_op_C0(V2VM& vm) {
    vm.ds_write(0x3B4, 0xFFFF);
    uint16_t saved = vm.ds_read(0x372);
    vm.ds_write(0x372, 6);
    uint8_t filter = vm.read_u8();
    uint16_t di = vm.global_r(0x42);
    vm.carry = v2_vm_sub_15fbe(vm, filter, di);
    vm.ds_write(0x372, saved);
    if (vm.carry) { uint16_t t = *(uint16_t*)(vm.es + vm.pc); vm.pc = t; } else { vm.pc += 2; }
}

// 0xC3 (sub_15160): vikings-only sub_15fb1 (Y_start-1) + off_30C8E[2]. 1 byte.
static void v2_vm_op_C3(V2VM& vm) {
    vm.ds_write(0x3B4, 0xFFFF);
    uint16_t saved = vm.ds_read(0x372);
    vm.ds_write(0x372, 6);
    uint8_t filter = vm.read_u8();
    uint16_t di = vm.global_r(0x42);
    vm.carry = v2_vm_sub_15fb1(vm, filter, di);
    vm.ds_write(0x372, saved);
    // off_30C8E[2] = loc_144f3: carry → skip 2, no carry → jump (simple, no save)
    if (vm.carry) { vm.pc += 2; } else { v2_vm_do_jump(vm); }
}

// 0xC4 (sub_1518e): vikings-only sub_15fbe (Y_end+1) + off_30C8E[2]. 1 byte.
static void v2_vm_op_C4(V2VM& vm) {
    vm.ds_write(0x3B4, 0xFFFF);
    uint16_t saved = vm.ds_read(0x372);
    vm.ds_write(0x372, 6);
    uint8_t filter = vm.read_u8();
    uint16_t di = vm.global_r(0x42);
    vm.carry = v2_vm_sub_15fbe(vm, filter, di);
    vm.ds_write(0x372, saved);
    // off_30C8E[2] = loc_144f3: carry → skip 2, no carry → jump (simple, no save)
    if (vm.carry) { vm.pc += 2; } else { v2_vm_do_jump(vm); }
}

// 0xC1 (sub_151f2): vikings-only flip-aware X obj search + off_30C8E[0]. 1 byte.
// flag SET → loc_151da → sub_15df2 (X_end+1). NOT set → loc_15214 → sub_15de5 (X_start-1).
static void v2_vm_op_C1(V2VM& vm) {
    vm.ds_write(0x3B4, 0xFFFF);
    uint16_t saved = vm.ds_read(0x372);
    vm.ds_write(0x372, 6);
    uint16_t si = vm.global_r(0x42);
    bool flag40 = vm.ds_read(si + 0x1585) & 0x40;
    uint8_t filter = vm.read_u8();
    uint16_t di = vm.global_r(0x42);
    if (flag40) {
        vm.carry = v2_vm_loc_15dfd(vm, filter, di, vm.ds_read(di + 0x155D) + 1);
    } else {
        vm.carry = v2_vm_sub_15de5(vm, filter, di);
    }
    vm.ds_write(0x372, saved);
    if (vm.carry) { uint16_t t = *(uint16_t*)(vm.es + vm.pc); vm.pc = t; } else { vm.pc += 2; }
}

// 0xC5 (sub_151f6): vikings-only flip-aware X obj search + off_30C8E[2]. 1 byte.
// Flag 0x40 INVERTED vs 0xC1: SET→sub_15df2, NOT set→sub_15de5
static void v2_vm_op_C5(V2VM& vm) {
    vm.ds_write(0x3B4, 0xFFFF);
    uint16_t saved = vm.ds_read(0x372);
    vm.ds_write(0x372, 6);
    uint16_t si = vm.global_r(0x42);
    bool flag40 = vm.ds_read(si + 0x1585) & 0x40;
    uint8_t filter = vm.read_u8();
    uint16_t di = vm.global_r(0x42);
    if (flag40) {
        vm.carry = v2_vm_loc_15dfd(vm, filter, di, vm.ds_read(di + 0x155D) + 1);
    } else {
        vm.carry = v2_vm_sub_15de5(vm, filter, di);
    }
    vm.ds_write(0x372, saved);
    // off_30C8E[2] = loc_144f3: carry → skip 2, no carry → jump (simple, no save)
    if (vm.carry) { vm.pc += 2; } else { v2_vm_do_jump(vm); }
}

// 0xC6 (sub_151bc): vikings-only flip-aware X obj search + off_30C8E[2]. 1 byte.
// Same flip logic as 0xC1: NOT set→sub_15df2, SET→sub_15de5
static void v2_vm_op_C6(V2VM& vm) {
    vm.ds_write(0x3B4, 0xFFFF);
    uint16_t saved = vm.ds_read(0x372);
    vm.ds_write(0x372, 6);
    uint16_t si = vm.global_r(0x42);
    bool flag40 = vm.ds_read(si + 0x1585) & 0x40;
    uint8_t filter = vm.read_u8();
    uint16_t di = vm.global_r(0x42);
    if (!flag40) {
        vm.carry = v2_vm_loc_15dfd(vm, filter, di, vm.ds_read(di + 0x155D) + 1);
    } else {
        vm.carry = v2_vm_sub_15de5(vm, filter, di);
    }
    vm.ds_write(0x372, saved);
    // off_30C8E[2] = loc_144f3: carry → skip 2, no carry → jump (simple, no save)
    if (vm.carry) { vm.pc += 2; } else { v2_vm_do_jump(vm); }
}

// Forward declarations for search functions defined later
static bool v2_vm_loc_159f6(V2VM& vm, uint16_t filter_si, uint16_t obj_di, uint16_t dx);
static bool v2_vm_sub_15de5(V2VM& vm, uint16_t filter_si, uint16_t obj_di);
static void v2_vm_sub_158b9(V2VM& vm, uint16_t filter_si, uint16_t obj_di);
static bool v2_vm_loc_15dfd(V2VM& vm, uint16_t filter_si, uint16_t obj_di, uint16_t x_search);

// sub_158e6: animation load using flip-aware SINGLE-POINT search.
// sub_15ac4: single tile check at (X_flip_edge, Y_end+1).
// sub_1603e: object search at X_flip_edge.
static void v2_vm_sub_158e6(V2VM& vm, uint16_t filter_si, uint16_t obj_di) {
    vm.ds_write(0x3B4, 0xFFFF);
    // Determine X based on flip
    uint16_t x;
    if (vm.ds_read(obj_di + 0x1585) & 0x40) {
        x = vm.ds_read(obj_di + 0x1535) - 1;
    } else {
        x = vm.ds_read(obj_di + 0x155D) + 1;
    }
    // sub_15ac4: single tile check at (x, Y_end+1)
    vm.ds_write(0x34, filter_si);
    uint16_t y = vm.ds_read(obj_di + 0x150D) + 1;
    uint16_t tile_val = v2_vm_sub_141ba(vm, x >> 4, y >> 4);
    uint8_t tt = (uint8_t)((tile_val & 0xFC00) >> 10);
    // Filter comparison (single point)
    uint16_t flt = filter_si;
    bool tile_found = false;
    while (true) {
        uint8_t fv = *(uint8_t*)(vm.ds + (uint16_t)(flt - 0x6B34));
        if (tt < fv) break;
        if (tt == fv) { vm.ds_write(0x3B2, tt); tile_found = true; break; }
        flt++;
    }
    if (tile_found) { vm.carry = true; return; }
    // sub_1603e: object search — X point in target X range, Y_end+1 in target Y range
    vm.ds_write(0x36, x);
    vm.ds_write(0x38, y); // Y_end + 1
    uint8_t* rds = vm.ds;
    uint16_t table_end = *(uint16_t*)(rds + 0x372);
    bool obj_found = false;
    for (uint16_t si2 = 0; (int16_t)si2 < (int16_t)table_end; si2 += 2) {
        if (*(uint16_t*)(rds + si2 + 0x1355) == 0) continue;
        if (si2 == *(uint16_t*)(rds + 0x42)) continue;
        vm.ds_write(0x3A, si2);
        uint8_t ot = (uint8_t)*(uint16_t*)(rds + si2 + 0x17DD);
        uint16_t f2 = filter_si;
        bool m2 = false;
        while (true) {
            uint8_t fv2 = *(uint8_t*)(rds + (uint16_t)(f2 - 0x6B34));
            if (ot < fv2) break;
            if (ot == fv2) { m2 = true; break; }
            f2++;
        }
        if (!m2) continue;
        // X: ds:0x36 in [target.X_start, target.X_end)
        if ((int16_t)x < (int16_t)*(uint16_t*)(rds + si2 + 0x1535)) continue;
        if ((int16_t)(x - 1) >= (int16_t)*(uint16_t*)(rds + si2 + 0x155D)) continue;
        // Y: ds:0x38 in [target.Y_start, target.Y_end)
        if ((int16_t)y < (int16_t)*(uint16_t*)(rds + si2 + 0x14E5)) continue;
        if ((int16_t)(y - 1) >= (int16_t)*(uint16_t*)(rds + si2 + 0x150D)) continue;
        vm.ds_write(0x3B2, ot);
        vm.ds_write(0x3B4, si2);
        obj_found = true;
        break;
    }
    vm.carry = obj_found;
}

// sub_158aa: animation load using X_start-1 search (sub_159c6 tile + sub_15de5 obj).
static void v2_vm_sub_158aa(V2VM& vm, uint16_t filter_si, uint16_t obj_di) {
    vm.ds_write(0x3B4, 0xFFFF);
    bool found = v2_vm_loc_159f6(vm, filter_si, obj_di, vm.ds_read(obj_di + 0x1535) - 1);
    if (found) { vm.carry = true; return; }
    vm.carry = v2_vm_sub_15de5(vm, filter_si, obj_di);
}

// 0x24 (sub_144ad): Flip-aware animation load. 1 byte. off_30C8E[2].
// flag 0x40 SET → sub_158b9 (X_end+1 search)
// flag 0x40 NOT set → sub_158aa (X_start-1 search)
static void v2_vm_op_24(V2VM& vm) {
    uint16_t si = vm.global_r(0x42);
    uint8_t anim_idx = vm.read_u8();
    uint16_t di = vm.global_r(0x42);
    if (vm.ds_read(si + 0x1585) & 0x40) {
        v2_vm_sub_158b9(vm, anim_idx, di);
    } else {
        v2_vm_sub_158aa(vm, anim_idx, di);
    }
    // off_30C8E[2] = loc_144f3: JC → carry = skip 2, no carry = jump (simple, no save)
    if (vm.carry) { vm.pc += 2; } else { v2_vm_do_jump(vm); }
}

// 0x1B (sub_143fe): Set obj0's collision to self + read 2 signed velocity bytes → obj0. 2 bytes.
static void v2_vm_op_1B(V2VM& vm) {
    uint16_t si = 0;
    vm.ds_write(0x141D, vm.global_r(0x42)); // obj0.collision = self
    // loc_14415: 2 signed bytes → velocity
    int8_t vx = (int8_t)vm.read_u8();
    vm.ds_write(si + 0x1945, (uint16_t)(int16_t)vx); // CBW
    int8_t vy = (int8_t)vm.read_u8();
    vm.ds_write(si + 0x196D, (uint16_t)(int16_t)vy); // CBW
}

// --- Field operation helpers (for 0x61-0xBB range) ---
// Pattern A address: indexed byte → off_30CA2 lookup → + [si+0x1995] → addr
static uint16_t v2_field_addr_A(V2VM& vm) {
    uint8_t idx = vm.read_u8();
    uint16_t off = *(uint16_t*)(vm.ds + (uint16_t)(idx - 0x6CBA));
    uint16_t si = vm.global_r(0x42);
    return (uint16_t)(off + vm.ds_read(si + 0x1995));
}
// Pattern B address: indexed byte → off_30CA2 lookup → + ds:0x42 → addr
static uint16_t v2_field_addr_B(V2VM& vm) {
    uint8_t idx = vm.read_u8();
    uint16_t off = *(uint16_t*)(vm.ds + (uint16_t)(idx - 0x6CBA));
    return (uint16_t)(off + vm.global_r(0x42));
}
// Direct address: 2-byte literal address from bytecode
static uint16_t v2_field_addr_D(V2VM& vm) {
    return vm.read_u16();
}

// 0x61 (sub_147cb): AND [field_A + 0x14E5], acc. 1 byte.
static void v2_vm_op_61(V2VM& vm) { uint16_t a = v2_field_addr_A(vm); vm.ds_write(a + 0x14E5, vm.ds_read(a + 0x14E5) & v2_vm_accumulator); }
// 0x65 (sub_14827): XOR [field_B + 0x14E5], acc. 1 byte.
static void v2_vm_op_65(V2VM& vm) { uint16_t a = v2_field_addr_B(vm); vm.ds_write(a + 0x14E5, vm.ds_read(a + 0x14E5) ^ v2_vm_accumulator); }

// 0x15 (sub_150fc): Relative position to active viking → 2 sub_154bf writes. 1 byte.
static void v2_vm_op_15(V2VM& vm) {
    uint16_t di = vm.global_r(0x42);
    uint16_t si = vm.ds_read(0x3C2); // active viking
    // X: direction-aware delta
    int16_t rel_x;
    if (vm.ds_read(di + 0x1585) & 0x40) {
        rel_x = (int16_t)(vm.ds_read(di + 0x173D) - vm.ds_read(si + 0x173D));
    } else {
        rel_x = (int16_t)(vm.ds_read(si + 0x173D) - vm.ds_read(di + 0x173D));
    }
    vm.ds_write(0x6C, (uint16_t)rel_x);
    // Y: direction-aware delta
    int16_t rel_y;
    if (vm.ds_read(di + 0x1585) & 0x80) {
        rel_y = (int16_t)(vm.ds_read(di + 0x1765) - vm.ds_read(si + 0x1765));
    } else {
        rel_y = (int16_t)(vm.ds_read(si + 0x1765) - vm.ds_read(di + 0x1765));
    }
    vm.ds_write(0x6E, (uint16_t)rel_y);
    // Read mode byte, dispatch twice
    uint8_t mode = vm.read_u8();
    uint16_t saved_si = (uint16_t)rel_x;
    v2_vm_sub_154bf(vm, saved_si, mode & 7);
    v2_vm_sub_154bf(vm, (uint16_t)rel_y, (mode >> 3) & 7);
}

// 0x0C (sub_13753): Vertical flip — toggle vflip, mirror Y bounds, update sub-sprites. 0 bytes.
static void v2_vm_op_0C(V2VM& vm) {
    uint16_t si = vm.global_r(0x42);
    // XOR flip flag
    vm.ds_write(si + 0x1585, vm.ds_read(si + 0x1585) ^ 0x80);
    // Mirror Y bounds: new = 2*center - old - 1
    uint16_t y = vm.ds_read(si + 0x1765);
    uint16_t new_14E5 = y + y - vm.ds_read(si + 0x150D) - 1;
    uint16_t new_150D = y + y - vm.ds_read(si + 0x14E5) - 1;
    vm.ds_write(si + 0x150D, new_150D);
    vm.ds_write(si + 0x14E5, new_14E5);
    // Update sub-sprites if present
    if (vm.ds_read(si + 0x1AD5) != 0) {
        uint16_t dx2 = y * 2;
        uint16_t end_di = vm.ds_read(si + 0x1AAD);
        for (uint16_t di = vm.ds_read(si + 0x1A85); (int16_t)di < (int16_t)end_di; di += 2) {
            vm.ds_write(di + 0x74D, dx2 - vm.ds_read(di + 0x74D) - vm.ds_read(di + 0x0C4D));
            vm.ds_write(di + 0x44D, vm.ds_read(di + 0x44D) ^ 0x400);
            vm.ds_write(di + 0x114D, 0x202);
        }
    }
}

// 0x0D (sub_142dc): Clear bit in ds:[byte+0x356]. 0 bytes.
static void v2_vm_op_0D(V2VM& vm) {
    int16_t val = (int16_t)vm.field_r(0x16C5);
    if (val < 0) return;
    uint16_t bit = val & 7;
    uint16_t byte_off = val >> 3;
    uint8_t mask = *(vm.ds + (uint16_t)(bit - 0x6C3C));
    uint16_t addr = byte_off + 0x356;
    if (addr < V2_VM_SHADOW_SIZE)
        vm.shadow[addr] &= mask;
}

// 0x0E (sub_142fc): Set bit in ds:[byte+0x356]. 0 bytes.
static void v2_vm_op_0E(V2VM& vm) {
    int16_t val = (int16_t)vm.field_r(0x16C5);
    if (val < 0) return;
    uint16_t bit = val & 7;
    uint16_t byte_off = val >> 3;
    uint8_t mask = *(vm.ds + (uint16_t)(bit - 0x6C44));
    uint16_t addr = byte_off + 0x356;
    if (addr < V2_VM_SHADOW_SIZE)
        vm.shadow[addr] |= mask;
}

// 0x06: Load saved alt PC — from v2 state
// 0x06 = sub_142d3: si = ds:0x42; bx = [si+0x137D]
static void v2_vm_op_load_alt_pc(V2VM& vm) {
    uint16_t si = vm.global_r(0x42);
    vm.pc = vm.ds_read(si + 0x137D);  // read alt_pc from shadow DS
}

// 0x9C (sub_14b71): Conditional acc load + AND/OR indexed field. 2 bytes.
// Exact original logic:
// 1. si = es:[bx] & 0xFF; bx++
// 2. if ds:0x8A != 0: ds:0x8A = ds:[si - 0x6C34]
// 3. dx = ds:[si - 0x6C14]
// 4. si = es:[bx] & 0xFF; bx++
// 5. si = ds:[si - 0x6CBA]; si += ds:0x42
// 6. ds:[si + 0x14E5] &= dx
// 7. ds:[si + 0x14E5] |= ds:0x8A
static void v2_vm_op_9C(V2VM& vm) {
    uint8_t idx1 = vm.read_u8();
    if (v2_vm_accumulator != 0)
        v2_vm_accumulator = *(uint16_t*)(vm.ds + (uint16_t)(idx1 - 0x6C34));
    uint16_t dx = *(uint16_t*)(vm.ds + (uint16_t)(idx1 - 0x6C14));
    uint8_t idx2 = vm.read_u8();
    uint16_t si = *(uint16_t*)(vm.ds + (uint16_t)(idx2 - 0x6CBA));
    si += vm.global_r(0x42);
    uint16_t addr = (uint16_t)(si + 0x14E5);
    vm.ds_write(addr, (vm.ds_read(addr) & dx) | v2_vm_accumulator);
}

// 0x12 (sub_14340): Release animation object. 0 bytes.
// di=ds:0x42; si=ds:[di+0x1995]; calls sub_15505 (sprite release)
static void v2_vm_op_12(V2VM& vm) {
    // sub_15505 releases sprite slots — sets ds:[si+0x1355]=0 etc.
    // For v2: mark object as released in shadow
    uint16_t di = vm.global_r(0x42);
    uint16_t si = vm.ds_read(di + 0x1995);
    // sub_15505: clears the animation object at si
    if (si < 128 * 2) {
        vm.ds_write(si + 0x1355, 0); // deactivate
    }
}

// ============================================================================
// Animation load helper: loc_15A70 — search objects for type/position match.
// Input: si=filter, di=current obj. Output: carry=found, ds:0x3B2=type.
// ============================================================================
// Common tile search starting at loc_15a87. 4 entry points differ only in y_start:
// sub_15a57: y = ds:[di+0x14E5] - 1  (Y_start - 1)
// loc_15a64: y = ds:[di+0x14E5]      (Y_start)
// loc_15a70: y = ds:[di+0x150D] + 1  (Y_end + 1)
// loc_15a7d: y = ds:[di+0x150D]      (Y_end)
static bool v2_vm_tile_search(V2VM& vm, uint16_t filter_si, uint16_t obj_di, uint16_t y_start) {
    vm.ds_write(0x34, filter_si);
    uint8_t* rds = vm.ds;
    uint16_t x_right = *(uint16_t*)(rds + obj_di + 0x155D);
    uint16_t x_left = *(uint16_t*)(rds + obj_di + 0x1535);
    uint16_t table_end = vm.global_r(0x372);

    for (uint16_t si = x_left; ; ) {
        // sub_14199: tile type lookup at (si/16, y_start/16)
        // sub_141a7: sub_141ba → tile map read from shadow tilemap
        // Returns ax = tile type upper bits
        uint16_t si_div16 = si >> 4;
        uint16_t di_div16 = y_start >> 4;

        // sub_141ba: bounds check + tile map read (from shadow tilemap)
        uint16_t tile_val = v2_vm_sub_141ba(vm, si_div16, di_div16);
        // sub_141a7: (tile_val & 0xFC00) >> 8 >> 2 = (tile_val >> 10)
        uint8_t al = (uint8_t)((tile_val & 0xFC00) >> 10);

        // Compare al with filter table at ds:[filter_si - 0x6B34]
        uint16_t flt = filter_si;
        while (true) {
            uint8_t fval = *(uint8_t*)(vm.ds + (uint16_t)(flt - 0x6B34));
            if (al < fval) break;
            if (al == fval) {
                // Debug: compare shadow tilemap vs real tilemap
                static bool loc15a70_dbg = false;
                if (!loc15a70_dbg && v2_vm_real_ds_ptr) {
                    loc15a70_dbg = true;
                    uint16_t real_tile_seg = *(uint16_t*)(v2_vm_real_ds_ptr + 0x2E63);
                    uint16_t di2 = di_div16 << 1, si2 = si_div16 << 1;
                    uint16_t row_off_s = *(uint16_t*)(vm.shadow + (uint16_t)(di2 - 0x7098));
                    uint16_t shadow_tile = *(uint16_t*)(v2_vm_shadow_tilemap + si2 + row_off_s);
                    uint16_t row_off_r = *(uint16_t*)(v2_vm_real_ds_ptr + (uint16_t)(di2 - 0x7098));
                    uint8_t* real_tm = v2_m2c_base + (uint32_t)real_tile_seg * 16;
                    uint16_t real_tile = *(uint16_t*)(real_tm + si2 + row_off_r);
                    printf("V2-DBG-15A70: MATCH! si=%d di=%d al=%d fval=%d shadow_tile=0x%04X real_tile=0x%04X filter=%d obj=%d\n",
                           si, y_start, al, fval, shadow_tile, real_tile, filter_si, vm.obj);
                }
                vm.ds_write(0x3B2, al);
                return true;
            }
            flt++;
        }

        // Advance: loc_15AA7: si += 0x10; if si >= cx → try cx, then exit
        if ((int16_t)si == (int16_t)x_right) break; // already at end
        si += 0x10;
        if ((int16_t)si >= (int16_t)x_right) {
            si = x_right; // last attempt
        }
    }
    return false; // carry clear — no match
}

// Entry point wrappers for tile search
static bool v2_vm_sub_15a57(V2VM& vm, uint16_t filter_si, uint16_t obj_di) {
    uint16_t y = vm.ds_read(obj_di + 0x14E5) - 1;
    return v2_vm_tile_search(vm, filter_si, obj_di, y);
}
static bool v2_vm_loc_15a64(V2VM& vm, uint16_t filter_si, uint16_t obj_di) {
    uint16_t y = vm.ds_read(obj_di + 0x14E5);
    return v2_vm_tile_search(vm, filter_si, obj_di, y);
}
static bool v2_vm_loc_15A70(V2VM& vm, uint16_t filter_si, uint16_t obj_di) {
    uint16_t y = vm.ds_read(obj_di + 0x150D) + 1;
    return v2_vm_tile_search(vm, filter_si, obj_di, y);
}
static bool v2_vm_loc_15a7d(V2VM& vm, uint16_t filter_si, uint16_t obj_di) {
    uint16_t y = vm.ds_read(obj_di + 0x150D);
    return v2_vm_tile_search(vm, filter_si, obj_di, y);
}

// Common object search starting at loc_15fc9. Two entry points:
// sub_15fb1: y = ds:[di+0x14E5] - 1 (Y_start - 1)
// sub_15fbe: y = ds:[di+0x150D] + 1 (Y_end + 1)
static bool v2_vm_obj_search(V2VM& vm, uint16_t filter_si, uint16_t obj_di, uint16_t y_check) {
    vm.ds_write(0x34, filter_si);
    uint8_t* rds = vm.ds;
    vm.ds_write(0x36, y_check);
    uint16_t table_end = *(uint16_t*)(rds + 0x372);

    for (uint16_t si = 0; (int16_t)si < (int16_t)table_end; si += 2) {
        if (*(uint16_t*)(rds + si + 0x1355) == 0) continue;
        if (si == *(uint16_t*)(rds + 0x42)) continue;
        vm.ds_write(0x3A, si);
        uint8_t obj_type = (uint8_t)*(uint16_t*)(rds + si + 0x17DD);
        uint16_t flt = filter_si; // use original param, not shadow (which may be modified)
        // Type match search
        bool match = false;
        while (true) {
            uint8_t fval = *(uint8_t*)(vm.ds + (uint16_t)(flt - 0x6B34));
            if (obj_type < fval) break;
            if (obj_type == fval) { match = true; break; }
            flt++;
        }
        if (!match) continue;
        // Position bounds check — read from real DS
        if ((int16_t)y_check < (int16_t)*(uint16_t*)(rds + si + 0x14E5)) continue;
        if ((int16_t)(y_check - 1) >= (int16_t)*(uint16_t*)(rds + si + 0x150D)) continue;
        if ((int16_t)*(uint16_t*)(rds + obj_di + 0x155D) < (int16_t)*(uint16_t*)(rds + si + 0x1535)) continue;
        if ((int16_t)*(uint16_t*)(rds + si + 0x155D) < (int16_t)*(uint16_t*)(rds + obj_di + 0x1535)) continue;
        // Found!
        vm.ds_write(0x3B2, obj_type);
        vm.ds_write(0x3B4, si);
        return true; // carry set
    }
    return false; // carry clear
}

// Entry point wrappers for object search
static bool v2_vm_sub_15fb1(V2VM& vm, uint16_t filter_si, uint16_t obj_di) {
    return v2_vm_obj_search(vm, filter_si, obj_di, vm.ds_read(obj_di + 0x14E5) - 1);
}
static bool v2_vm_sub_15fbe(V2VM& vm, uint16_t filter_si, uint16_t obj_di) {
    return v2_vm_obj_search(vm, filter_si, obj_di, vm.ds_read(obj_di + 0x150D) + 1);
}

// sub_15ae9: tile search at ds:0x6C, ds:0x6E (single point, not range).
// Entry to loc_15a93 with si=ds:0x6C, di=ds:0x6E, dx=di, cx=si.
static bool v2_vm_sub_15ae9(V2VM& vm, uint16_t filter_si) {
    vm.ds_write(0x34, filter_si);
    uint16_t x = vm.ds_read(0x6C);
    uint16_t y = vm.ds_read(0x6E);
    // Single point tile check: x_left=x, x_right=x, y_start=y
    // Reuse tile search with y as-is. The loop checks one tile at (x,y).
    uint16_t si_div16 = x >> 4;
    uint16_t di_div16 = y >> 4;
    uint16_t tile_val = v2_vm_sub_141ba(vm, si_div16, di_div16);
    uint8_t al = (uint8_t)((tile_val & 0xFC00) >> 10);
    // Filter comparison
    uint16_t flt = filter_si;
    while (true) {
        uint8_t fval = *(uint8_t*)(vm.ds + (uint16_t)(flt - 0x6B34));
        if (al < fval) break;
        if (al == fval) {
            vm.ds_write(0x3B2, al);
            return true;
        }
        flt++;
    }
    return false;
}

// sub_160cf: object search using ds:0x6C (X) and ds:0x6E (Y) as reference.
// Checks: X in [obj.X_start, obj.X_end), Y in [obj.Y_start, obj.Y_end).
static bool v2_vm_sub_160cf(V2VM& vm, uint16_t filter_si) {
    vm.ds_write(0x34, filter_si);
    uint16_t ref_x = vm.ds_read(0x6C);
    uint16_t ref_y = vm.ds_read(0x6E);
    vm.ds_write(0x36, ref_x);
    vm.ds_write(0x38, ref_y);
    uint8_t* rds = vm.ds;
    uint16_t table_end = *(uint16_t*)(rds + 0x372);

    for (uint16_t si = 0; (int16_t)si < (int16_t)table_end; si += 2) {
        if (*(uint16_t*)(rds + si + 0x1355) == 0) continue;
        if (si == *(uint16_t*)(rds + 0x42)) continue;
        vm.ds_write(0x3A, si);

        // Type match via filter table
        uint8_t obj_type = (uint8_t)*(uint16_t*)(rds + si + 0x17DD);
        uint16_t flt = filter_si;
        bool match = false;
        while (true) {
            uint8_t fval = *(uint8_t*)(vm.ds + (uint16_t)(flt - 0x6B34));
            if (obj_type < fval) break;
            if (obj_type == fval) { match = true; break; }
            flt++;
        }
        if (!match) continue;

        // X bounds: ref_x >= obj.X_start AND ref_x-1 < obj.X_end
        if ((int16_t)ref_x < (int16_t)*(uint16_t*)(rds + si + 0x1535)) continue;
        if ((int16_t)(ref_x - 1) >= (int16_t)*(uint16_t*)(rds + si + 0x155D)) continue;

        // Y bounds: ref_y >= obj.Y_start AND ref_y-1 < obj.Y_end
        if ((int16_t)ref_y < (int16_t)*(uint16_t*)(rds + si + 0x14E5)) continue;
        if ((int16_t)(ref_y - 1) >= (int16_t)*(uint16_t*)(rds + si + 0x150D)) continue;

        // Found!
        vm.ds_write(0x3B2, *(uint16_t*)(rds + si + 0x17DD));
        vm.ds_write(0x3B4, si);
        return true;
    }
    return false;
}

// sub_1589b: animation load using ds:0x6C/0x6E position search.
// ds:0x3B4=0xFFFF; call sub_15ae9 (tile at 6C/6E); JC→ret; call sub_160cf (obj at 6C/6E); ret.
static void v2_vm_sub_1589b(V2VM& vm, uint16_t filter_si) {
    vm.ds_write(0x3B4, 0xFFFF);
    bool found_tile = v2_vm_sub_15ae9(vm, filter_si);
    if (found_tile) {
        vm.carry = true;
        return;
    }
    vm.carry = v2_vm_sub_160cf(vm, filter_si);
}

// loc_159f6: X-axis tile search core (vertical scan at given X).
// Used by sub_159df (X_end+1), sub_159d3 (X_start), loc_159ec (X_end).
static bool v2_vm_loc_159f6(V2VM& vm, uint16_t filter_si, uint16_t obj_di, uint16_t dx) {
    vm.ds_write(0x34, filter_si);
    int16_t y_top = (int16_t)vm.ds_read(obj_di + 0x150D) - (int16_t)vm.ds_read(obj_di + 0x196D);
    if (y_top < 0) y_top = 0;
    uint16_t cx = (uint16_t)y_top;

    // First check: tile at (obj.X, y_top) — if slope (>= 0x30) → exit no match
    {
        uint16_t obj_x = vm.ds_read(obj_di + 0x173D);
        uint16_t tile_val = v2_vm_sub_141ba(vm, obj_x >> 4, cx >> 4);
        uint16_t tt = (tile_val & 0xFC00) >> 10;
        if (tt >= 0x30) return false;
    }

    // Y_start for vertical scan
    int16_t y_start = (int16_t)vm.ds_read(obj_di + 0x14E5) - (int16_t)vm.ds_read(obj_di + 0x196D);
    if (y_start <= 0) y_start = 0;

    // Vertical scan from y_start to y_top
    for (uint16_t di = (uint16_t)y_start; ; ) {
        uint16_t tile_val = v2_vm_sub_141ba(vm, dx >> 4, di >> 4);
        uint8_t al = (uint8_t)((tile_val & 0xFC00) >> 10);

        // Filter comparison
        uint16_t flt = filter_si;
        while (true) {
            uint8_t fval = *(uint8_t*)(vm.ds + (uint16_t)(flt - 0x6B34));
            if (al < fval) break;
            if (al == fval) {
                vm.ds_write(0x3B2, al);
                return true;
            }
            flt++;
        }

        // Advance Y
        if (di == cx) return false;
        di += 0x10;
        if ((int16_t)di >= (int16_t)cx) {
            di = cx; // last attempt
        }
    }
    return false;
}

// Entry points for loc_159f6:
static bool v2_vm_sub_159df(V2VM& vm, uint16_t filter_si, uint16_t obj_di) {
    return v2_vm_loc_159f6(vm, filter_si, obj_di, vm.ds_read(obj_di + 0x155D) + 1);
}
static bool v2_vm_sub_159df_at(V2VM& vm, uint16_t filter_si, uint16_t obj_di, uint16_t x) {
    return v2_vm_loc_159f6(vm, filter_si, obj_di, x);
}

// loc_15dfd: X-axis object search core (common for sub_15df2 and sub_15de5).
static bool v2_vm_loc_15dfd(V2VM& vm, uint16_t filter_si, uint16_t obj_di, uint16_t x_search) {
    vm.ds_write(0x34, filter_si);
    vm.ds_write(0x36, x_search);
    uint8_t* rds = vm.ds;
    uint16_t table_end = *(uint16_t*)(rds + 0x372);
    for (uint16_t si = 0; (int16_t)si < (int16_t)table_end; si += 2) {
        if (*(uint16_t*)(rds + si + 0x1355) == 0) continue;
        if (si == *(uint16_t*)(rds + 0x42)) continue;
        vm.ds_write(0x3A, si);
        uint8_t obj_type = (uint8_t)*(uint16_t*)(rds + si + 0x17DD);
        uint16_t flt = filter_si;
        bool match = false;
        while (true) {
            uint8_t fval = *(uint8_t*)(rds + (uint16_t)(flt - 0x6B34));
            if (obj_type < fval) break;
            if (obj_type == fval) { match = true; break; }
            flt++;
        }
        if (!match) continue;
        // X bounds: x_search >= target.X_start AND x_search - 1 < target.X_end
        if ((int16_t)x_search < (int16_t)*(uint16_t*)(rds + si + 0x1535)) continue;
        if ((int16_t)(x_search - 1) >= (int16_t)*(uint16_t*)(rds + si + 0x155D)) continue;
        // Y bounds: self adjusted Y range overlaps target Y range
        // self.Y_end - self.vel_Y >= target.Y_start
        int16_t self_y_end_adj = (int16_t)vm.ds_read(obj_di + 0x150D) - (int16_t)vm.ds_read(obj_di + 0x196D);
        if (self_y_end_adj < (int16_t)*(uint16_t*)(rds + si + 0x14E5)) continue;
        // self.Y_start - self.vel_Y <= target.Y_end (equal counts as match)
        int16_t self_y_start_adj = (int16_t)vm.ds_read(obj_di + 0x14E5) - (int16_t)vm.ds_read(obj_di + 0x196D);
        int16_t target_y_end = (int16_t)*(uint16_t*)(rds + si + 0x150D);
        if (self_y_start_adj > target_y_end) continue; // jz passes, jge skips
        // Found!
        vm.ds_write(0x3B2, *(uint16_t*)(rds + si + 0x17DD));
        vm.ds_write(0x3B4, si);
        return true;
    }
    return false;
}

// sub_15de5/sub_15df2 wrappers:
static bool v2_vm_sub_15de5(V2VM& vm, uint16_t filter_si, uint16_t obj_di) {
    return v2_vm_loc_15dfd(vm, filter_si, obj_di, vm.ds_read(obj_di + 0x1535) - 1);
}

// sub_158b9: animation load using X-axis search paths (X_end + 1).
static void v2_vm_sub_158b9(V2VM& vm, uint16_t filter_si, uint16_t obj_di) {
    vm.ds_write(0x3B4, 0xFFFF);
    bool found = v2_vm_sub_159df(vm, filter_si, obj_di);
    if (found) {
        vm.carry = true;
        return;
    }
    vm.carry = v2_vm_loc_15dfd(vm, filter_si, obj_di, vm.ds_read(obj_di + 0x155D) + 1);
}

// sub_158c8: animation load using Y_start-1 search paths.
// ds:0x3B4 = 0xFFFF; call sub_15a57 (tile search Y_start-1);
// if carry → return; call sub_15fb1 (obj search Y_start-1); return carry.
static void v2_vm_sub_158c8(V2VM& vm, uint16_t filter_si, uint16_t obj_di) {
    vm.ds_write(0x3B4, 0xFFFF);
    bool found_a = v2_vm_sub_15a57(vm, filter_si, obj_di);
    if (found_a) {
        vm.carry = true;
        return;
    }
    vm.carry = v2_vm_sub_15fb1(vm, filter_si, obj_di);
}

// sub_158d7: animation load function. Sets carry based on search results.
static void v2_vm_sub_158d7(V2VM& vm, uint16_t filter_si, uint16_t obj_di) {
    vm.ds_write(0x3B4, 0xFFFF);
    bool found_a = v2_vm_loc_15A70(vm, filter_si, obj_di);
    if (found_a) {
        vm.carry = true;
        static bool dbg158 = false;
        if (!dbg158) { dbg158 = true;
            printf("V2-DBG: sub_158d7 obj=%d filter=%d: loc_15A70 found → carry=true\n", vm.obj, filter_si);
        }
        return;
    }
    bool found_b = v2_vm_sub_15fbe(vm, filter_si, obj_di);
    vm.carry = found_b;
    static bool dbg158b = false;
    if (!dbg158b && found_b) { dbg158b = true;
        printf("V2-DBG: sub_158d7 obj=%d filter=%d: sub_15fbe found → carry=true\n", vm.obj, filter_si);
    }
}

// 0x14 (sub_14f59): Object creation. Full implementation.
// Reads 3 mode bytes + dispatches, then calls sub_13809 to create a new object.
static void v2_vm_op_14(V2VM& vm) {
    // Step 1: Read mode1 byte → dual dispatch → X position (ds:0x6C), Y position (ds:0x6E)
    uint16_t word1 = *(uint16_t*)(vm.es + vm.pc);
    vm.pc += 1;
    uint8_t mode1 = (uint8_t)(word1 & 0xFF);
    uint16_t x_pos = v2_vm_dispatch_30C98(vm, mode1);
    vm.ds_write(0x6C, x_pos);
    uint16_t y_pos = v2_vm_dispatch_30C98(vm, mode1 >> 3);
    vm.ds_write(0x6E, y_pos);

    // Step 2: Read mode2 byte → dual dispatch → ds:0x374 and ds:0x32 (flags)
    uint16_t word2 = *(uint16_t*)(vm.es + vm.pc);
    vm.pc += 1;
    uint8_t mode2 = (uint8_t)(word2 & 0xFF);
    uint16_t val374 = v2_vm_dispatch_30C98(vm, mode2);
    vm.ds_write(0x374, val374);
    uint16_t flags_raw = v2_vm_dispatch_30C98(vm, mode2 >> 3);
    vm.ds_write(0x32, flags_raw & 0x801);

    // Step 3: Compute flags (si) = (current_obj.flags & 0xFE) | ds:0x32
    uint16_t cur_obj = vm.global_r(0x42);
    uint16_t si_flags = (vm.ds_read(cur_obj + 0x1585) & 0xFE) | vm.ds_read(0x32);

    // Step 4: Read animation type byte (1 byte)
    uint8_t anim_type = vm.read_u8();

    // Step 5: sub_13809 — create object
    // Inputs: ax = anim_type, di = 0xFFFF, si = flags
    vm.ds_write(0x3E0, 0xFFFF);

    // sub_13d30: pre-check
    uint16_t ds_32F = vm.ds_read(0x32F);
    if (ds_32F != 0) {
        static bool d14a = false; if (!d14a) { d14a = true;
            printf("V2-DBG-14: FAIL at sub_13d30: ds:0x32F=0x%04X\n", ds_32F); }
        return;
    }

    // sub_13d52: find free slot (0 to 0x28, step 2)
    int16_t new_slot = -1;
    for (uint16_t s = 0; s < 0x28; s += 2) {
        if (vm.ds_read(s + 0x1355) == 0) {
            new_slot = s;
            break;
        }
    }
    if (new_slot < 0) {
        static bool d14b = false; if (!d14b) { d14b = true;
            printf("V2-DBG-14: FAIL at sub_13d52: no free slot\n"); }
        return;
    }

    uint16_t si_slot = (uint16_t)new_slot;

    // sub_13e52: init object fields
    // es = ds:0x2E67 (animation data segment)
    uint16_t anim_seg = vm.ds_read(0x2E67);
    uint8_t* anim_es = v2_resolve_segment(anim_seg, vm.shadow);
    uint16_t bx_anim = (uint16_t)(anim_type * 0x15);

    // Read animation table entry at anim_es:[bx_anim]
    vm.ds_write(0x34, anim_type);
    vm.ds_write(0x36, 0xFFFF); // di from sub_14f59
    vm.ds_write(0x38, si_flags);

    // Set object fields from animation table
    vm.ds_write(si_slot + 0x16ED, anim_type);                    // anim index
    vm.ds_write(si_slot + 0x16C5, 0xFFFF);                       // bit flag (di)
    vm.ds_write(si_slot + 0x1585, si_flags);                      // flags
    vm.ds_write(si_slot + 0x169D, vm.ds_read(0x374));            // from ds:0x374

    vm.ds_write(0x374, 0);

    // sub_12F82: resource check — reads es:[bx] chunk ID
    uint16_t chunk_id = *(uint16_t*)(anim_es + bx_anim);
    uint16_t sprite_base = 0;
    if (chunk_id == 0xFFFF) {
        // Special: no sprite needed, sprite_base = 0, success
        sprite_base = 0;
    } else if (chunk_id == 0xFFFE) {
        // Special: increment ds:0x374, sprite_base = 0, success
        vm.ds_write(0x374, vm.ds_read(0x374) + 1);
        sprite_base = 0;
    } else {
        // Normal: search loaded resources table at ds:0x12AD
        bool resource_found = false;
        for (uint16_t di_r = 0; di_r < 0x40; di_r += 2) {
            if (vm.ds_read(di_r + 0x12AD) == chunk_id) {
                sprite_base = vm.ds_read(di_r + 0x12ED);
                resource_found = true;
                break;
            }
        }
        if (!resource_found) {
            vm.ds_write(si_slot + 0x1355, 0);
            return;
        }
    }

    vm.ds_write(si_slot + 0x1855, sprite_base);

    // es:[bx+2]: sub-sprite count + flags
    uint16_t sub_count_raw = *(uint16_t*)(anim_es + bx_anim + 2);
    if (sub_count_raw & 0x80) {
        vm.ds_write(0x374, vm.ds_read(0x374) + 2);
    }
    vm.ds_write(si_slot + 0x1AD5, sub_count_raw & 0x7F);

    // es:[bx+3]: initial PC (bytecode pointer) + 3
    uint16_t init_pc = *(uint16_t*)(anim_es + bx_anim + 3) + 3;
    vm.ds_write(si_slot + 0x132D, init_pc);


    // CODE SEGMENT = ds:0x2E67 (animation segment)
    vm.ds_write(si_slot + 0x1355, anim_seg);

    // More fields from animation table
    vm.ds_write(si_slot + 0x15AD, *(uint16_t*)(anim_es + bx_anim + 7));    // type
    vm.ds_write(si_slot + 0x1445, *(uint8_t*)(anim_es + bx_anim + 9));     // width
    vm.ds_write(si_slot + 0x146D, *(uint8_t*)(anim_es + bx_anim + 0xA));   // height
    vm.ds_write(si_slot + 0x15D5, *(uint16_t*)(anim_es + bx_anim + 0xB));  // collision type
    vm.ds_write(si_slot + 0x15FD, *(uint16_t*)(anim_es + bx_anim + 0xD));  // filter
    vm.ds_write(si_slot + 0x1625, *(uint16_t*)(anim_es + bx_anim + 0xF));  // collision mask
    vm.ds_write(si_slot + 0x178D, *(uint16_t*)(anim_es + bx_anim + 0x11)); // field
    vm.ds_write(si_slot + 0x17B5, *(uint16_t*)(anim_es + bx_anim + 0x13)); // field

    // Position from ds:0x6C/0x6E
    vm.ds_write(si_slot + 0x173D, vm.ds_read(0x6C));   // X
    vm.ds_write(si_slot + 0x13A5, vm.ds_read(0x6C));   // saved X
    vm.ds_write(si_slot + 0x1765, vm.ds_read(0x6E));   // Y
    vm.ds_write(si_slot + 0x13CD, vm.ds_read(0x6E));   // saved Y

    // Parent link
    vm.ds_write(si_slot + 0x1805, vm.global_r(0x42));

    // Zero-init fields
    vm.ds_write(si_slot + 0x19BD, 0);
    vm.ds_write(si_slot + 0x19E5, 0);
    vm.ds_write(si_slot + 0x1715, 0);
    vm.ds_write(si_slot + 0x164D, 0);
    vm.ds_write(si_slot + 0x1675, 0);
    vm.ds_write(si_slot + 0x1945, 0);
    vm.ds_write(si_slot + 0x196D, 0);
    vm.ds_write(si_slot + 0x17DD, 0);
    vm.ds_write(si_slot + 0x18A5, 0);
    vm.ds_write(si_slot + 0x18CD, 0);
    vm.ds_write(si_slot + 0x187D, 0);
    vm.ds_write(si_slot + 0x18F5, 0);
    vm.ds_write(si_slot + 0x191D, 0xFFFF);
    vm.ds_write(si_slot + 0x182D, 0xFFFF);
    vm.ds_write(si_slot + 0x1A0D, 0xFFFF);
    vm.ds_write(si_slot + 0x141D, 0xFFFF);

    // Compute bounding box from width/height and position
    uint16_t half_w = vm.ds_read(si_slot + 0x1445) >> 1;
    uint16_t x = vm.ds_read(si_slot + 0x173D);
    vm.ds_write(si_slot + 0x1535, x - half_w);
    vm.ds_write(si_slot + 0x155D, x + vm.ds_read(si_slot + 0x1445) - half_w - 1);

    uint16_t half_h = vm.ds_read(si_slot + 0x146D) >> 1;
    uint16_t y = vm.ds_read(si_slot + 0x1765);
    vm.ds_write(si_slot + 0x14E5, y - half_h);
    vm.ds_write(si_slot + 0x150D, y + vm.ds_read(si_slot + 0x146D) - half_h - 1);

    // sub_13e52 eip 0x3F9D..0x3FBB: exact replica
    {
        uint16_t ax = vm.ds_read(0x3E0);
        if ((int16_t)ax < 0) {
            // height/2 → ds:0x3E2, width/2 → ax
            ax = vm.ds_read(si_slot + 0x146D) >> 1;
            vm.ds_write(0x3E2, ax); // MOV ds:3E2h, ax
            ax = vm.ds_read(si_slot + 0x1445) >> 1;
        }
        // loc_13fb4:
        vm.ds_write(si_slot + 0x14BD, ax);          // [si+14BDh] = ax
        vm.ds_write(si_slot + 0x1495, vm.ds_read(0x3E2)); // [si+1495h] = ds:3E2h
    }

    // sub_13d68: allocate sub-sprites. Exact replica of original algorithm.
    if (vm.ds_read(si_slot + 0x1AD5) != 0) {
        // Determine search range based on ds:0x374
        uint16_t di_alloc, limit_32;
        uint16_t ds374 = vm.ds_read(0x374);
        if (ds374 == 0) {
            di_alloc = 0x48; limit_32 = 0x100;
        } else if (ds374 == 1) {
            di_alloc = 0x30; limit_32 = 0x50;
        } else {
            di_alloc = 0; limit_32 = 0x30;
        }

        // Phase 1 (loc_13d95): Find first free slot
        uint16_t di_found = 0xFFFF;
        while (true) {
            // loc_13d95: check [di+44D] | [di+114D]
            if ((vm.ds_read(di_alloc + 0x44D) | vm.ds_read(di_alloc + 0x114D)) == 0) {
                // Found first free → Phase 2 (loc_13daa)
                uint16_t start = di_alloc;
                uint16_t need = vm.ds_read(si_slot + 0x1AD5);
                uint16_t end_needed = start + need * 2;

                // Phase 3 (loc_13db9): verify contiguous
                di_alloc += 2;
                bool ok = true;
                while (di_alloc != end_needed) {
                    if (di_alloc == limit_32) { ok = false; break; } // hit limit → FAIL
                    if ((vm.ds_read(di_alloc + 0x44D) | vm.ds_read(di_alloc + 0x114D)) != 0) {
                        // Not free → restart search from THIS occupied slot (JMP loc_13d95)
                        ok = false;
                        // Don't increment — next iteration of outer while checks this di_alloc
                        goto restart_search;
                    }
                    di_alloc += 2; // free → continue checking
                }
                if (ok) {
                    di_found = start;
                    break; // SUCCESS
                }
            }
        restart_search:
            di_alloc += 2;
            if ((int16_t)di_alloc >= (int16_t)limit_32) break; // past limit → FAIL
        }

        if (di_found == 0xFFFF) {
            // Allocation failed — destroy object
            vm.ds_write(si_slot + 0x1355, 0);
            return;
        }

        // Set sub-sprite range
        uint16_t need_count = vm.ds_read(si_slot + 0x1AD5);
        vm.ds_write(si_slot + 0x1A85, di_found);
        vm.ds_write(si_slot + 0x1AAD, di_found + need_count * 2);

        // sub_13dd6: initialize sub-sprite base fields
        {
            uint16_t flags_init = (vm.ds_read(si_slot + 0x1585) & 0xCE) << 3;
            flags_init |= 0x8000;
            uint16_t spr_seg = vm.ds_read(0x2E73);
            uint16_t spr_base = vm.ds_read(si_slot + 0x1855);
            uint16_t cx_end = vm.ds_read(si_slot + 0x1AAD);
            for (uint16_t d = vm.ds_read(si_slot + 0x1A85); (int16_t)d < (int16_t)cx_end; d += 2) {
                vm.ds_write(d + 0x44D, flags_init);
                vm.ds_write(d + 0x54D, 0);
                vm.ds_write(d + 0x114D, 0x204);
                vm.ds_write(d + 0x94D, spr_seg);
                vm.ds_write(d + 0x84D, spr_base);
            }
        }
        // sub_13e15: set sprite type (8x8 or 16x16) based on flags bit 0
        {
            uint16_t bp_type = (vm.ds_read(si_slot + 0x1585) & 1) << 1;
            uint16_t cx_end = vm.ds_read(si_slot + 0x1AAD);
            for (uint16_t d = vm.ds_read(si_slot + 0x1A85); (int16_t)d < (int16_t)cx_end; d += 2) {
                if (bp_type != 0) {
                    vm.ds_write(d + 0x0C4D, 0x20);  // 16x16
                    vm.ds_write(d + 0x44D, vm.ds_read(d + 0x44D) | 2);
                } else {
                    vm.ds_write(d + 0x0C4D, 8);     // 8x8
                    vm.ds_write(d + 0x44D, vm.ds_read(d + 0x44D) | 1);
                }
            }
        }
    }

    // Adjust ds:0x372 (max active object) if needed
    if ((int16_t)si_slot >= (int16_t)vm.ds_read(0x372)) {
        vm.ds_write(0x372, si_slot + 2);
    }

    uint16_t di_new = si_slot;

    // Back in sub_14f59: ds:[obj+0x182D] = di (link to child)
    uint16_t obj = vm.global_r(0x42);
    vm.ds_write(obj + 0x182D, di_new);

    // If di < ds:0x42: ds:[ds:0x376 + 0x378] = di; ds:0x376++
    if ((int16_t)di_new < (int16_t)vm.global_r(0x42)) {
        uint16_t idx376 = vm.ds_read(0x376);
        vm.ds_write(idx376 + 0x378, di_new);
        vm.ds_write(0x376, idx376 + 1);
    }
}

// Animation load opcodes: read 1 byte (anim index), call sub_158d7, dispatch via off_30C8E.
// sub_158d7 sets carry flag. off_30C8E entries use carry for conditional branching.
// off_30C8E[0] = loc_144e9: no carry → skip 2, carry → jump
// off_30C8E[2] = loc_144f3: carry → skip 2, no carry → jump

// 0x1F (sub_14469): Animation load + off_30C8E[0] dispatch. 1 byte.
static void v2_vm_op_1F(V2VM& vm) {
    uint8_t anim_idx = vm.read_u8();
    uint16_t di = vm.global_r(0x42);
    v2_vm_sub_158d7(vm, anim_idx, di);
    // off_30C8E[0] = loc_144e9: no carry → skip 2, carry → jump
    uint16_t cs_addr = *(uint16_t*)(vm.ds + 0x87AE + 0); // runtime read
    if (cs_addr == 0x44E9) {
        if (!vm.carry) { vm.pc += 2; } else { v2_vm_do_jump(vm); }
    } else if (cs_addr == 0x44F3) {
        if (vm.carry) { vm.pc += 2; } else { v2_vm_do_jump(vm); }
    } else if (cs_addr == 0x42CF) {
        v2_vm_do_jump(vm);
    } else {
        v2_vm_runtime_dispatch(vm, 0x87AE, 0);
    }
}

// 0xC2 (sub_151b8): Player-proximity search + off_30C8E[0]. 1 byte.
// Saves ds:0x372, sets to 6 (search only player objects 0-5).
// NOT flipped → sub_15df2, flipped → sub_15de5.
static void v2_vm_op_C2(V2VM& vm) {
    vm.ds_write(0x3B4, 0xFFFF);
    uint16_t saved_372 = vm.ds_read(0x372);
    vm.ds_write(0x372, 6); // limit search to players
    uint16_t si_obj = vm.global_r(0x42);
    bool flipped = vm.ds_read(si_obj + 0x1585) & 0x40;
    uint8_t anim_idx = vm.read_u8();
    uint16_t di = vm.global_r(0x42);
    bool found;
    if (!flipped) {
        // sub_15df2: x_search = [di+0x155D] + 1
        found = v2_vm_loc_15dfd(vm, anim_idx, di, vm.ds_read(di + 0x155D) + 1);
    } else {
        // sub_15de5: x_search = [di+0x1535] - 1
        found = v2_vm_loc_15dfd(vm, anim_idx, di, vm.ds_read(di + 0x1535) - 1);
    }
    vm.carry = found;
    vm.ds_write(0x372, saved_372); // restore
    // off_30C8E[0]
    uint16_t cs_addr = *(uint16_t*)(vm.ds + 0x87AE + 0);
    if (cs_addr == 0x44E9) {
        if (!vm.carry) { vm.pc += 2; } else { v2_vm_do_jump(vm); }
    } else if (cs_addr == 0x44F3) {
        if (vm.carry) { vm.pc += 2; } else { v2_vm_do_jump(vm); }
    } else if (cs_addr == 0x42CF) {
        v2_vm_do_jump(vm);
    } else {
        v2_vm_runtime_dispatch(vm, 0x87AE, 0);
    }
}

// 0x21 (sub_14483): Anim load + off_30C8E[0]. 1 byte.
// NOT flipped → sub_158b9, flipped → sub_158aa.
static void v2_vm_op_21(V2VM& vm) {
    uint16_t si = vm.global_r(0x42);
    bool flipped = vm.ds_read(si + 0x1585) & 0x40;
    uint8_t anim_idx = vm.read_u8();
    uint16_t di = vm.global_r(0x42);
    if (!flipped) {
        v2_vm_sub_158b9(vm, anim_idx, di);
    } else {
        v2_vm_sub_158aa(vm, anim_idx, di);
    }
    // off_30C8E[0]
    uint16_t cs_addr = *(uint16_t*)(vm.ds + 0x87AE + 0);
    if (cs_addr == 0x44E9) {
        if (!vm.carry) { vm.pc += 2; } else { v2_vm_do_jump(vm); }
    } else if (cs_addr == 0x44F3) {
        if (vm.carry) { vm.pc += 2; } else { v2_vm_do_jump(vm); }
    } else if (cs_addr == 0x42CF) {
        v2_vm_do_jump(vm);
    } else {
        v2_vm_runtime_dispatch(vm, 0x87AE, 0);
    }
}

// 0x23 (sub_1446d): Animation load + off_30C8E[2] dispatch. 1 byte.
// 0x22 (sub_14453): Anim load via sub_158c8 (Y_start-1 paths) + off_30C8E[2]. 1 byte.
static void v2_vm_op_22(V2VM& vm) {
    uint8_t anim_idx = vm.read_u8();
    uint16_t di = vm.global_r(0x42);
    v2_vm_sub_158c8(vm, anim_idx, di);
    // off_30C8E[si=2]: carry → skip 2, no carry → jump
    uint16_t cs_addr = *(uint16_t*)(vm.ds + 0x87AE + 2);
    if (cs_addr == 0x44F3) {
        if (vm.carry) { vm.pc += 2; } else { v2_vm_do_jump(vm); }
    } else if (cs_addr == 0x44E9) {
        if (!vm.carry) { vm.pc += 2; } else { v2_vm_do_jump(vm); }
    } else if (cs_addr == 0x42CF) {
        v2_vm_do_jump(vm);
    } else {
        v2_vm_runtime_dispatch(vm, 0x87AE, 2);
    }
}

static void v2_vm_op_23(V2VM& vm) {
    uint8_t anim_idx = vm.read_u8();
    uint16_t di = vm.global_r(0x42);
    v2_vm_sub_158d7(vm, anim_idx, di);
    // off_30C8E[si=2] = loc_144f3: carry → skip 2, no carry → jump
    uint16_t cs_addr = *(uint16_t*)(vm.ds + 0x87AE + 2); // si=2 → byte offset 2
    if (cs_addr == 0x44F3) {
        if (vm.carry) { vm.pc += 2; } else { v2_vm_do_jump(vm); }
    } else if (cs_addr == 0x44E9) {
        if (!vm.carry) { vm.pc += 2; } else { v2_vm_do_jump(vm); }
    } else if (cs_addr == 0x42CF) {
        v2_vm_do_jump(vm);
    } else {
        v2_vm_runtime_dispatch(vm, 0x87AE, 2);
    }
}

// 0x08 (sub_1367c): Horizontal flip if flag 0x40 set. 0 bytes.
// Like 0x0C but for horizontal direction. Calls sub_136a0 if flag set.
static void v2_vm_op_08(V2VM& vm) {
    uint16_t si = vm.global_r(0x42);
    if (!(vm.ds_read(si + 0x1585) & 0x40)) return;
    // sub_136a0: horizontal flip logic
    // XOR flag 0x40
    vm.ds_write(si + 0x1585, vm.ds_read(si + 0x1585) ^ 0x40);
    // Mirror X bounds: new = 2*center - old - 1
    uint16_t x = vm.ds_read(si + 0x173D);
    uint16_t new_1535 = x + x - vm.ds_read(si + 0x155D) - 1;
    uint16_t new_155D = x + x - vm.ds_read(si + 0x1535) - 1;
    vm.ds_write(si + 0x155D, new_155D);
    vm.ds_write(si + 0x1535, new_1535);
    // Update sub-sprites if present
    if (vm.ds_read(si + 0x1AD5) != 0) {
        uint16_t dx2 = x * 2;
        uint16_t end_di = vm.ds_read(si + 0x1AAD);
        for (uint16_t di = vm.ds_read(si + 0x1A85); (int16_t)di < (int16_t)end_di; di += 2) {
            vm.ds_write(di + 0x64D, dx2 - vm.ds_read(di + 0x64D) - vm.ds_read(di + 0x0C4D));
            vm.ds_write(di + 0x44D, vm.ds_read(di + 0x44D) ^ 0x200);
            vm.ds_write(di + 0x114D, 0x202);
        }
    }
}

// 0x5D (sub_14771): Subtract acc from value at address. 2 bytes.
// si=es:[bx]; bx+=2; ds:[si] -= ds:0x8A
static void v2_vm_op_5D(V2VM& vm) {
    uint16_t addr = vm.read_u16();
    vm.ds_write(addr, vm.ds_read(addr) - v2_vm_accumulator);
}

// 0x7C (sub_1491b): Signed >= literal (2B). acc >= val → JUMP. acc < val → skip 2.
static void v2_vm_op_7C(V2VM& vm) {
    uint16_t val = v2_vm_read_literal(vm);
    if ((int16_t)v2_vm_accumulator >= (int16_t)val) {
        v2_vm_do_jump(vm);
    } else {
        vm.pc += 2;
    }
}

static uint16_t v2_vm_read_random(V2VM& vm); // forward decl
static uint16_t v2_vm_read_indexed_field_1995(V2VM& vm); // forward decl

// 0x7F (sub_14963): Signed >= indexed+1995 (1B). acc >= val → JUMP. Variant A.
static void v2_vm_op_7F(V2VM& vm) {
    uint16_t val = v2_vm_read_indexed_field_1995(vm);
    if ((int16_t)v2_vm_accumulator >= (int16_t)val) {
        v2_vm_do_jump(vm);
    } else {
        vm.pc += 2;
    }
}

// 0x80 (sub_1497b): Signed >= random (0B). acc >= val → JUMP. Variant A.
static void v2_vm_op_80(V2VM& vm) {
    uint16_t val = v2_vm_read_random(vm);
    if ((int16_t)v2_vm_accumulator >= (int16_t)val) {
        v2_vm_do_jump(vm);
    } else {
        vm.pc += 2;
    }
}

// 0x85 (sub_149f3): Signed >= random (0B). acc >= val → SKIP 2. Variant B.
static void v2_vm_op_85(V2VM& vm) {
    uint16_t val = v2_vm_read_random(vm);
    if ((int16_t)v2_vm_accumulator >= (int16_t)val) {
        vm.pc += 2;
    } else {
        v2_vm_do_jump(vm);
    }
}

// 0x81 (sub_14993): Signed >= literal (2B). acc >= val → SKIP 2. acc < val → jump. Variant B.
static void v2_vm_op_81(V2VM& vm) {
    uint16_t val = v2_vm_read_literal(vm);
    if ((int16_t)v2_vm_accumulator >= (int16_t)val) {
        vm.pc += 2;
    } else {
        v2_vm_do_jump(vm);
    }
}

// 0x10 (sub_14327): Destroy object via sub_13c93 + exit VM. 0 bytes consumed.
// sub_14327: di = ds:0x42; call sub_13c93; si = ds:0x42; POP ax; RETN
// POP ax + RETN = exit opcode loop (skip caller's return address)
static void v2_vm_op_10(V2VM& vm) {
    uint16_t di = vm.global_r(0x42);

    // === sub_13c93 logic ===
    // 1. Clear all sub-sprites if 0x1AD5 != 0
    if (vm.ds_read(di + 0x1AD5) != 0) {
        uint16_t end_si = vm.ds_read(di + 0x1AAD);
        for (uint16_t si = vm.ds_read(di + 0x1A85); (int16_t)si < (int16_t)end_si; si += 2) {
            vm.ds_write(si + 0x44D, 0);          // clear flags
            vm.ds_write(si + 0x114D, 0x400);      // set dirty = 0x400
        }
    }

    // 2. Unlink from linked list (0x1805 = prev, 0x182D = next)
    uint16_t prev = vm.ds_read(di + 0x1805);
    if (prev != 0xFFFF) {
        vm.ds_write(prev + 0x182D, 0xFFFF);
    }
    uint16_t next = vm.ds_read(di + 0x182D);
    if (next != 0xFFFF) {
        vm.ds_write(next + 0x1805, 0xFFFF);
    }

    // 3. Clear object: code_seg=0, anim_data=0xFFFF
    vm.ds_write(di + 0x1355, 0);
    vm.ds_write(di + 0x1A0D, 0xFFFF);

    // 4. Adjust ds:0x372 (max active object index)
    // if di+2 == ds:0x372, scan backwards to find new max
    uint16_t di_plus2 = di + 2;
    if (vm.ds_read(0x372) == di_plus2) {
        uint16_t scan = di_plus2;
        while (true) {
            scan -= 2;
            if ((int16_t)scan < 0) break;
            if (vm.ds_read(scan + 0x1355) != 0) break;
        }
        vm.ds_write(0x372, scan + 2);
    }

    // 5. Check 0x16C5 bit flag (optional: calls sub_139ef + sub_13ae0)
    // These modify ds:0x356 bit table — complex and rarely affects VM flow.
    // We replicate the bit-clearing part for correctness:
    if (vm.ds_read(di + 0x16C5) != 0xFFFF) {
        uint16_t flags_1585 = vm.ds_read(di + 0x1585);
        if (flags_1585 & 0x100) {
            // loc_13cfc: sub_139ef (viewport bounds) + sub_13ae0 (re-spawn from table)
            uint8_t* s = v2_vm_shadow_ds;
            // sub_139ef: set viewport bounds
            uint16_t vx = *(uint16_t*)(s + 0x44) - 0x10;
            *(uint16_t*)(s + 0x34) = vx;
            *(uint16_t*)(s + 0x36) = vx + 0x160;
            printf("V2-TRACE: sub_139ef ds:0x36=%04X (vp_x=%04X) from opcode kill path\n", (uint16_t)(vx + 0x160), *(uint16_t*)(s + 0x44));
            uint16_t vy = *(uint16_t*)(s + 0x46) - 0x10;
            *(uint16_t*)(s + 0x38) = vy;
            *(uint16_t*)(s + 0x3A) = vy + 0xD0;
            // sub_13ae0: try to re-spawn object from spawn table
            uint16_t spawn_idx = vm.ds_read(di + 0x16C5);
            uint16_t di_off = spawn_idx * 0x0E;
            uint16_t save_42 = *(uint16_t*)(s + 0x42);
            *(uint16_t*)(s + 0x42) = 0xFFFF;
            // sub_13ae0 checks bounds + calls sub_13809 for matching spawn entry
            uint16_t sx = *(uint16_t*)(s + di_off + 0x25F6);
            if (sx != 0xFFFF) {
                uint16_t hw = *(uint16_t*)(s + di_off + 0x25FA);
                uint16_t sy = *(uint16_t*)(s + di_off + 0x25F8);
                uint16_t hh = *(uint16_t*)(s + di_off + 0x25FC);
                bool in_vp = true;
                if ((int16_t)(sx + hw - *(uint16_t*)(s + 0x34)) < 0) in_vp = false;
                if ((int16_t)(sx - hw - *(uint16_t*)(s + 0x36)) >= 0) in_vp = false;
                if ((int16_t)(sy + hh - *(uint16_t*)(s + 0x38)) < 0) in_vp = false;
                if ((int16_t)(sy - hh - *(uint16_t*)(s + 0x3A)) >= 0) in_vp = false;
                if (in_vp) {
                    *(uint16_t*)(s + 0x6C) = sx;
                    *(uint16_t*)(s + 0x6E) = sy;
                    *(uint16_t*)(s + 0x3E0) = hw;
                    *(uint16_t*)(s + 0x3E2) = hh;
                    *(uint16_t*)(s + 0x374) = *(uint16_t*)(s + di_off + 0x2602);
                    v2_sub_13809(s, *(uint16_t*)(s + di_off + 0x25FE),
                                 spawn_idx, *(uint16_t*)(s + di_off + 0x2600), sx, sy);
                }
            }
            *(uint16_t*)(s + 0x42) = save_42;
        }
    }

    // Exit VM: POP ax + RETN in original skips the opcode loop return
    vm.running = false;
}

// 0x1C (sub_1443d): If ds:[obj+0x1A35] != 0 → skip 2, else jump. 0 bytes.
static void v2_vm_op_1C(V2VM& vm) {
    if (vm.field_r(0x1A35) != 0) { vm.pc += 2; } else { v2_vm_do_jump(vm); }
}

// 0x3F (sub_145e5): OR 0x4000 on all sub-sprites + set dirty. 0 bytes.
// Loop from [obj+0x1A85] to [obj+0x1AAD], OR flags with 0x4000,
// OR byte [si+0x114E] with 2 (equivalent to OR word 0x114D with 0x0200).
static void v2_vm_op_3F(V2VM& vm) {
    uint16_t obj = vm.global_r(0x42);
    uint16_t end = vm.ds_read(obj + 0x1AAD);
    for (uint16_t si = vm.ds_read(obj + 0x1A85); (int16_t)si < (int16_t)end; si += 2) {
        vm.ds_write(si + 0x44D, vm.ds_read(si + 0x44D) | 0x4000);
        // OR byte at [si+0x114E] with 2 — high byte of word at 0x114D
        uint16_t dirty = vm.ds_read(si + 0x114D);
        dirty |= 0x0200; // 2 << 8 = OR on high byte
        vm.ds_write(si + 0x114D, dirty);
    }
}

// 0x40 (sub_14604): Clear bits 13-14 on all sub-sprites. 0 bytes.
static void v2_vm_op_40(V2VM& vm) {
    uint16_t obj = vm.global_r(0x42);
    uint16_t end = vm.ds_read(obj + 0x1AAD);
    for (uint16_t si = vm.ds_read(obj + 0x1A85); (int16_t)si < (int16_t)end; si += 2) {
        vm.ds_write(si + 0x44D, vm.ds_read(si + 0x44D) & 0x9FFF);
        vm.ds_write(si + 0x114D, 2);
    }
}

// 0x5C (sub_1474b): Subtract acc from indexed field. 1 byte.
static void v2_vm_op_5C(V2VM& vm) {
    uint16_t idx = vm.read_u8();
    uint16_t lookup = (uint16_t)(idx - 0x6CBA);
    uint16_t si = *(uint16_t*)(vm.ds + lookup);
    si += vm.global_r(0x42);
    uint16_t addr = (uint16_t)(si + 0x14E5);
    vm.ds_write(addr, vm.ds_read(addr) - v2_vm_accumulator);
}

// sub_15972: Y position snap after tile collision (sub_15AFD carry path).
// ax = collision result from sub_15AFD: 0 = plain match, negative = slope (|ax|=adjustment), positive = moved up
// di = object index
static void v2_vm_sub_15972(V2VM& vm, int16_t ax, uint16_t di) {
    uint16_t dx;
    if (ax == 0) {
        // loc_15984: snap Y_end to tile boundary
        uint16_t y_end = vm.ds_read(di + 0x150D);
        uint16_t snapped = (y_end & 0xFFF0) - 1;
        vm.ds_write(di + 0x150D, snapped);
        dx = y_end - snapped;
    } else if (ax < 0) {
        // ax < 0: slope adjustment. dx = ax & 0xFF. Y_end -= dx.
        dx = (uint16_t)(ax & 0xFF);
        vm.ds_write(di + 0x150D, vm.ds_read(di + 0x150D) - dx);
    } else {
        // ax > 0: snap Y_start up to next tile boundary
        uint16_t y_start = vm.ds_read(di + 0x14E5);
        uint16_t snapped = (y_start | 0xF) + 1;
        vm.ds_write(di + 0x14E5, snapped);
        dx = y_start - snapped; // negative (wraps as uint16_t)
        vm.ds_write(di + 0x1765, vm.ds_read(di + 0x1765) - dx);
        vm.ds_write(di + 0x150D, vm.ds_read(di + 0x150D) - dx);
        vm.ds_write(di + 0x19E5, 0);
        return;
    }
    // Common path for ax == 0 and ax < 0
    vm.ds_write(di + 0x1765, vm.ds_read(di + 0x1765) - dx);
    vm.ds_write(di + 0x14E5, vm.ds_read(di + 0x14E5) - dx);
    vm.ds_write(di + 0x19E5, 0);
}

// sub_161a1: bounding box check for sub_1614E. di=self, si=target.
// Returns true (carry) if collision.
static bool v2_vm_sub_161a1(V2VM& vm, uint16_t di, uint16_t si) {
    // X overlap check
    if ((int16_t)vm.ds_read(di + 0x155D) < (int16_t)vm.ds_read(si + 0x1535)) return false;
    if ((int16_t)vm.ds_read(si + 0x155D) < (int16_t)vm.ds_read(di + 0x1535)) return false;

    // Y_end clamped to >= 0
    int16_t y_end = (int16_t)vm.ds_read(di + 0x150D);
    if (y_end < 0) y_end = 0;
    uint16_t ds32 = (uint16_t)y_end; // ds:0x32

    // Adjusted Y = Y_end - Y_center + old_Y_center, clamp to > 0
    int16_t adj_y = (int16_t)vm.ds_read(di + 0x150D) - (int16_t)vm.ds_read(di + 0x1765)
                    + (int16_t)vm.ds_read(di + 0x13CD);
    if (adj_y <= 0) adj_y = 0;

    // ds:0x34 = min, ds:0x36 = max of (ds32, adj_y)
    uint16_t ds34, ds36;
    if ((uint16_t)adj_y < ds32) {
        ds34 = (uint16_t)adj_y;
        ds36 = ds32;
    } else {
        ds36 = (uint16_t)adj_y;
        ds34 = ds32;
    }

    // Target's adjusted Y: [si+14E5] - [si+1765] + [si+13CD]
    int16_t target_adj = (int16_t)vm.ds_read(si + 0x14E5) - (int16_t)vm.ds_read(si + 0x1765)
                         + (int16_t)vm.ds_read(si + 0x13CD);
    if ((int16_t)target_adj < (int16_t)vm.ds_read(si + 0x14E5)) {
        // target_adj < target.Y_start: check ds:0x36 >= target_adj AND target.Y_start >= ds:0x34
        if ((int16_t)ds36 < target_adj) return false;
        if ((int16_t)vm.ds_read(si + 0x14E5) < (int16_t)ds34) return false;
    } else {
        // target_adj >= target.Y_start: check ds:0x36 >= target.Y_start AND target_adj >= ds:0x34
        if ((int16_t)ds36 < (int16_t)vm.ds_read(si + 0x14E5)) return false;
        if ((int16_t)target_adj < (int16_t)ds34) return false;
    }
    return true;
}

// sub_1614E: object collision detection for sub_1584e.
// Scans objects for type match + Y velocity comparison + sub_161a1 bounding box.
static bool v2_vm_sub_1614e(V2VM& vm, uint16_t filter_si, uint16_t obj_di) {
    vm.ds_write(0x3A, filter_si);
    uint8_t* rds = vm.ds;
    uint16_t table_end = *(uint16_t*)(rds + 0x372);
    for (uint16_t si = 0; (int16_t)si < (int16_t)table_end; si += 2) {
        if (*(uint16_t*)(rds + si + 0x1355) == 0) continue;
        if (si == *(uint16_t*)(rds + 0x42)) continue;
        vm.ds_write(0x38, si);
        uint8_t obj_type = (uint8_t)*(uint16_t*)(rds + si + 0x17DD);
        // Filter match
        uint16_t flt = filter_si;
        bool match = false;
        while (true) {
            uint8_t fv = *(uint8_t*)(rds + (uint16_t)(flt - 0x6B34));
            if (obj_type < fv) break;
            if (obj_type == fv) { match = true; break; }
            flt++;
        }
        if (!match) continue;
        // Y velocity check: self.vel_Y - target.vel_Y
        int16_t vel_diff = (int16_t)vm.ds_read(obj_di + 0x196D) - (int16_t)*(uint16_t*)(rds + si + 0x196D);
        if (vel_diff <= 0) continue; // JZ or JL → skip
        // sub_161a1: bounding box check
        if (v2_vm_sub_161a1(vm, obj_di, si)) {
            // Found! ax = 0, STC
            return true;
        }
    }
    return false;
}

// sub_15DA8: Y position snap after object collision.
// ax = direction (0 = moved down, !0 = moved up). si = partner, di = self.
static void v2_vm_sub_15DA8(V2VM& vm, int16_t ax_dir, uint16_t si_partner, uint16_t di) {
    uint16_t dx;
    if (ax_dir == 0) {
        // Moved down: snap up. dx = self.Y_end - partner.Y_start + 1
        dx = vm.ds_read(di + 0x150D) - vm.ds_read(si_partner + 0x14E5) + 1;
        vm.ds_write(di + 0x1765, vm.ds_read(di + 0x1765) - dx);
        vm.ds_write(di + 0x14E5, vm.ds_read(di + 0x14E5) - dx);
        vm.ds_write(di + 0x150D, vm.ds_read(di + 0x150D) - dx);
    } else {
        // Moved up: snap down. dx = partner.Y_end - self.Y_start + 1
        dx = vm.ds_read(si_partner + 0x150D) - vm.ds_read(di + 0x14E5) + 1;
        vm.ds_write(di + 0x1765, vm.ds_read(di + 0x1765) + dx);
        vm.ds_write(di + 0x14E5, vm.ds_read(di + 0x14E5) + dx);
        vm.ds_write(di + 0x150D, vm.ds_read(di + 0x150D) + dx);
    }
    vm.ds_write(di + 0x19E5, 0);
}

// sub_15d6b: X position snap for sub_15788 (object collision from sub_15c37).
// ax = direction (0 = moved right, !0 = moved left). si = partner, di = self.
static void v2_vm_sub_15d6b(V2VM& vm, int16_t ax_dir, uint16_t si_partner, uint16_t di) {
    uint16_t dx;
    if (ax_dir == 0) {
        // Moved right: snap self left. dx = self.X_end - partner.X_start + 1
        dx = vm.ds_read(di + 0x155D) - vm.ds_read(si_partner + 0x1535) + 1;
        vm.ds_write(di + 0x173D, vm.ds_read(di + 0x173D) - dx);
        vm.ds_write(di + 0x1535, vm.ds_read(di + 0x1535) - dx);
        vm.ds_write(di + 0x155D, vm.ds_read(di + 0x155D) - dx);
    } else {
        // Moved left: snap self right. dx = partner.X_end - self.X_start + 1
        dx = vm.ds_read(si_partner + 0x155D) - vm.ds_read(di + 0x1535) + 1;
        vm.ds_write(di + 0x173D, vm.ds_read(di + 0x173D) + dx);
        vm.ds_write(di + 0x1535, vm.ds_read(di + 0x1535) + dx);
        vm.ds_write(di + 0x155D, vm.ds_read(di + 0x155D) + dx);
    }
    vm.ds_write(di + 0x19BD, 0); // clear X fractional accumulator
}

// sub_1584e collision check helper — different from sub_155d6!
// All paths consume 1 byte. Returns carry flag (true = collision).
// Path 1 (ds:0x390 > 0): full bounding box check via sub_15AFD etc.
// Path 2 (ds:0x390 < 0): no collision, just consume byte
// Path 3 (ds:0x390 == 0): check bit in [obj+0x13F5] → collision if set
static bool v2_vm_collision_check_1584e(V2VM& vm) {
    int16_t state = (int16_t)vm.global_r(0x390);

    if (state == 0) {
        // loc_15886: INC bx + check bit in collision flags
        vm.pc += 1;
        uint16_t di = vm.global_r(0x42);
        uint16_t si = vm.global_r(0x38E);
        uint16_t mask = *(uint16_t*)(vm.ds + (uint16_t)(si - 0x6C34));
        uint16_t flags = vm.ds_read(di + 0x13F5);
        if (flags & mask) {
            return true;   // STC — collision
        }
        return false;      // CLC — no collision
    }

    if (state < 0) {
        // loc_15883: INC bx + CLC
        vm.pc += 1;
        return false;
    }

    // state > 0: full collision check path.
    // sub_15AFD (tile) → sub_15972 (snap), or sub_1614E (obj) → sub_15DA8 (snap).
    // Set collision bit if found. ALWAYS returns CLC (false).
    {
        uint8_t filter = vm.read_u8();
        uint16_t di = vm.global_r(0x42);
        uint16_t filter_si = (uint16_t)filter;
        bool tile_found = false;
        int16_t tile_ax = 0; // result for sub_15972

        // --- sub_15AFD: downward tile collision ---
        int16_t y_moved = (int16_t)vm.ds_read(di + 0x1765) - (int16_t)vm.ds_read(di + 0x13CD);
        if (y_moved > 0) {
            vm.ds_write(0x6C, filter_si);
            vm.ds_write(0x6E, di);

            // Scan filter for slope type
            uint16_t flt = filter_si;
            uint8_t filt_val;
            while (true) {
                filt_val = vm.shadow[(uint16_t)(flt - 0x6B34)];
                if (filt_val == 0xFF || filt_val >= 0x30) break;
                flt++;
            }

            if (filt_val >= 0x30 && filt_val != 0xFF) {
                uint16_t obj_x = vm.ds_read(di + 0x173D);
                int16_t adj_y = (int16_t)vm.ds_read(di + 0x13CD) - (int16_t)vm.ds_read(di + 0x1765)
                                + (int16_t)vm.ds_read(di + 0x150D);
                uint16_t tt = (v2_vm_sub_141ba(vm, obj_x >> 4, (uint16_t)adj_y >> 4) & 0xFC00) >> 10;
                if (tt >= 0x30) {
                    vm.ds_write(0x3A, tt);
                    di = vm.ds_read(0x6E);
                    uint16_t cur_y_end = vm.ds_read(di + 0x150D);
                    uint16_t tt2 = (v2_vm_sub_141ba(vm, obj_x >> 4, cur_y_end >> 4) & 0xFC00) >> 10;
                    if (tt2 < 0x30) {
                        uint16_t saved = cur_y_end;
                        uint16_t temp = (tt2 & 0xFFF0) - 1;
                        vm.ds_write(di + 0x150D, temp);
                        uint16_t slope_tt = vm.ds_read(0x3A);
                        uint16_t sidx = ((slope_tt & 0xF) << 4) + (obj_x & 0xF);
                        uint8_t sv = vm.shadow[(uint16_t)(sidx - 0x7684)] & 0xF;
                        int16_t sr = (int16_t)((temp & 0xF) - sv);
                        vm.ds_write(di + 0x150D, saved);
                        if (sr >= 0) {
                            tile_ax = (int16_t)((uint16_t)((saved & 0xF) + (uint16_t)sr + 1) | 0x8000);
                            tile_found = true;
                        }
                    }
                }
            }

            if (!tile_found) {
                // loc_15b88: check slope at current position
                di = vm.ds_read(0x6E);
                uint16_t obj_x = vm.ds_read(di + 0x173D);
                uint16_t cur_y = vm.ds_read(di + 0x150D);
                uint16_t tt3 = (v2_vm_sub_141ba(vm, obj_x >> 4, cur_y >> 4) & 0xFC00) >> 10;
                if (tt3 >= 0x30) {
                    uint16_t sidx = ((tt3 & 0xF) << 4) + (obj_x & 0xF);
                    uint8_t sv = vm.shadow[(uint16_t)(sidx - 0x7684)] & 0xF;
                    int16_t sr = (int16_t)((cur_y & 0xF) - sv);
                    if (sr >= 0) { tile_ax = (int16_t)((uint16_t)sr | 0x8000); tile_found = true; }
                }
            }

            if (!tile_found) {
                // loc_15bb8: horizontal tile scan
                di = vm.ds_read(0x6E);
                filter_si = vm.ds_read(0x6C);
                uint16_t ye = vm.ds_read(di + 0x150D);
                uint16_t old_ye = (uint16_t)((int16_t)ye - (int16_t)vm.ds_read(di + 0x1765) + (int16_t)vm.ds_read(di + 0x13CD));
                if ((old_ye & 0xFFF0) != (ye & 0xFFF0)) {
                    uint16_t xe = vm.ds_read(di + 0x155D);
                    vm.ds_write(0x36, ye); vm.ds_write(0x38, xe);
                    for (uint16_t s = vm.ds_read(di + 0x1535); ; ) {
                        uint8_t al = (uint8_t)((v2_vm_sub_141ba(vm, s >> 4, vm.ds_read(0x36) >> 4) & 0xFC00) >> 10);
                        if (al >= 0x30) {
                            di = vm.ds_read(0x6E);
                            uint16_t sidx = ((al & 0xF) << 4) + (s & 0xF);
                            uint8_t sv = vm.shadow[(uint16_t)(sidx - 0x7684)] & 0xF;
                            int16_t sr = (int16_t)((vm.ds_read(di + 0x150D) & 0xF) - sv);
                            if (sr >= 0) { tile_ax = (int16_t)((uint16_t)sr | 0x8000); tile_found = true; }
                            break;
                        }
                        uint16_t f = filter_si; bool m = false;
                        while (true) {
                            uint8_t fv = vm.shadow[(uint16_t)(f - 0x6B34)];
                            if (al < fv) break;
                            if (al == fv) { m = true; break; }
                            f++;
                        }
                        if (m) { tile_ax = 0; tile_found = true; break; }
                        if (s == xe) break;
                        s += 0x10; if ((int16_t)s >= (int16_t)xe) s = xe;
                    }
                }
            }
        }

        di = vm.global_r(0x42);
        bool collision_found = false;
        if (tile_found) {
            // sub_15972: Y tile snap
            v2_vm_sub_15972(vm, tile_ax, di);
            collision_found = true;
        } else {
            // sub_1614E: object collision check
            if (v2_vm_sub_1614e(vm, filter_si, di)) {
                uint16_t partner = vm.ds_read(0x3B4);
                v2_vm_sub_15DA8(vm, 0, partner, di); // ax=0 for sub_1584e (downward)
                collision_found = true;
            }
        }

        if (collision_found) {
            // loc_15875: set collision bit
            uint16_t si_38e = vm.global_r(0x38E);
            uint16_t mask = *(uint16_t*)(vm.ds + (uint16_t)(si_38e - 0x6C34));
            vm.ds_write(di + 0x13F5, vm.ds_read(di + 0x13F5) | mask);
        }
        return false; // ALWAYS CLC
    }
}

// 0x3C (sub_15838): Collision check via sub_1584e + ds:0x38E increment.
// BOTH paths (collision and no-collision) add 2 to ds:0x38E.
// On collision: call-jump. On no collision: skip 2.
static void v2_vm_op_3C(V2VM& vm) {
    bool collision = v2_vm_collision_check_1584e(vm);
    // Both paths: ADD ds:38Eh, 2
    vm.ds_write(0x38E, vm.ds_read(0x38E) + 2);
    if (!collision) { vm.pc += 2; } else { v2_vm_do_call_jump(vm); }
}

// 0xB2 (sub_14e37): sub_153ea (3 bytes bit test) + conditional.
// If ne → skip 2, eq → call-jump.
static void v2_vm_op_B2(V2VM& vm) {
    // sub_153ea: byte idx + word VALUE (bytecode literal!) → bit test
    uint8_t idx1 = vm.read_u8();
    uint16_t val = vm.read_u16();  // bytecode literal, NOT ds:[addr]
    uint16_t mask = *(uint16_t*)(vm.ds + (uint16_t)(idx1 - 0x6C34));
    uint16_t result = (val & mask) ? 1 : 0;
    if (result != v2_vm_accumulator) { vm.pc += 2; } else { v2_vm_do_call_jump(vm); }
}

// 0x7E (sub_1494b): Signed >= comparison with indirect value (2 bytes).
// acc >= val (signed) → jump, else skip 2.
static void v2_vm_op_7E(V2VM& vm) {
    uint16_t val = v2_vm_read_indirect(vm);
    int16_t dx = (int16_t)v2_vm_accumulator - (int16_t)val;
    if ((int16_t)v2_vm_accumulator >= (int16_t)val) {
        v2_vm_do_jump(vm);
    } else {
        vm.pc += 2;
    }
}

// 0x83 (sub_149c3): Signed < comparison with indirect value (2 bytes).
// acc < val (signed) → jump, acc >= val → skip 2. Opposite of 0x7E.
static void v2_vm_op_83(V2VM& vm) {
    uint16_t val = v2_vm_read_indirect(vm);
    if ((int16_t)v2_vm_accumulator < (int16_t)val) {
        v2_vm_do_jump(vm);
    } else {
        vm.pc += 2;
    }
}

// 0x91 (sub_146f6): Conditional add/subtract accumulator to/from ds:[addr].
// If NOT hflip (flag 0x40 clear) → ds:[addr] += acc (sub_14704)
// If hflip (flag 0x40 set) → ds:[addr] -= acc (sub_14771)
// Consumes 2 bytes (word address from bytecode).
static void v2_vm_op_91(V2VM& vm) {
    uint16_t si = vm.global_r(0x42);
    uint16_t addr = vm.read_u16();
    if (!(vm.ds_read(si + 0x1585) & 0x40)) {
        // sub_14704: ds:[addr] += acc
        vm.ds_write(addr, vm.ds_read(addr) + v2_vm_accumulator);
    } else {
        // sub_14771: ds:[addr] -= acc
        vm.ds_write(addr, vm.ds_read(addr) - v2_vm_accumulator);
    }
}

// 0x98 (sub_14b52): Load accumulator from sub_15403 (indexed field bit test, 2 bytes).
// sub_15403: byte idx1 (mask), byte idx2 (field) → pattern B (ds:0x42-relative).
// si = ds:[idx2 - 0x6CBA] + ds:0x42; ax = ds:[si + 0x14E5] & ds:[idx1 - 0x6C34]; acc = 0 or 1.
static void v2_vm_op_98(V2VM& vm) {
    uint8_t idx1 = vm.read_u8();
    uint8_t idx2 = vm.read_u8();
    uint16_t field_off = *(uint16_t*)(vm.ds + (uint16_t)(idx2 - 0x6CBA));
    uint16_t si = field_off + vm.global_r(0x42);
    uint16_t val = vm.ds_read(si + 0x14E5);
    uint16_t mask = *(uint16_t*)(vm.ds + (uint16_t)(idx1 - 0x6C34));
    v2_vm_accumulator = (val & mask) ? 1 : 0;
}

// 0xAD (sub_14de4): sub_153ea bit test (3 bytes) + if eq → skip 2, ne → jump.
// Opposite of 0xB2 (which does call-jump instead of jump).
static void v2_vm_op_AD(V2VM& vm) {
    // sub_153ea: bytecode literal bit test
    uint8_t idx1 = vm.read_u8();
    uint16_t val = vm.read_u16();  // bytecode literal, NOT ds:[addr]
    uint16_t mask = *(uint16_t*)(vm.ds + (uint16_t)(idx1 - 0x6C34));
    uint16_t result = (val & mask) ? 1 : 0;
    if (result == v2_vm_accumulator) { vm.pc += 2; } else { v2_vm_do_jump(vm); }
}

// 0xCF (sub_152c6): Viewport bounds check on sub-object → outside viewport: jump, within: skip 2.
// Checks ds:[obj+0x1995] object's X/Y against viewport ds:0x44/0x46.
// PUSH 2 → if within: off_30C92[2] = skip 2, if outside: off_30C94[2] = jump
static void v2_vm_op_CF(V2VM& vm) {
    uint16_t si_obj = vm.global_r(0x42);
    uint16_t si = vm.ds_read(si_obj + 0x1995); // sub-object

    uint16_t vp_x = vm.ds_read(0x44);
    uint16_t vp_y = vm.ds_read(0x46);
    uint16_t obj_x = vm.ds_read(si + 0x173D);
    uint16_t obj_y = vm.ds_read(si + 0x1765);

    bool within = true;
    // X bounds: vp_x + 0x1F <= obj_x AND vp_x + 0x1F + 0x102 >= obj_x
    if ((int16_t)(vp_x + 0x1F) >= (int16_t)obj_x) within = false;
    if ((int16_t)(vp_x + 0x1F + 0x102) < (int16_t)obj_x) within = false;
    // Y bounds: vp_y + 0x1F <= obj_y AND vp_y + 0x1F + 0x72 >= obj_y
    if (within) {
        if ((int16_t)(vp_y + 0x1F) >= (int16_t)obj_y) within = false;
        if ((int16_t)(vp_y + 0x1F + 0x72) < (int16_t)obj_y) within = false;
    }

    // PUSH 2: off_30C92[2] = skip 2 (within), off_30C94[2] = jump (outside)
    if (within) { vm.pc += 2; } else { v2_vm_do_jump(vm); }
}

// 0x6A (sub_1488b): Unsigned >= comparison with INDIRECT value (2 bytes).
// CALL sub_1549a (read indirect, 2B) → acc >= val → jump, else skip 2.
static void v2_vm_op_6A(V2VM& vm) {
    uint16_t val = v2_vm_read_indirect(vm); // sub_1549a: 2 bytes
    if (v2_vm_accumulator >= val) {
        v2_vm_do_jump(vm);
    } else {
        vm.pc += 2;
    }
}

// 0x94 (sub_14763): Conditional add/sub acc to addr — OPPOSITE of 0x91.
// NOT hflip → subtract (sub_14771), hflip → add (sub_14704). 2 bytes.
static void v2_vm_op_94(V2VM& vm) {
    uint16_t si = vm.global_r(0x42);
    uint16_t addr = vm.read_u16();
    if (!(vm.ds_read(si + 0x1585) & 0x40)) {
        // sub_14771: ds:[addr] -= acc
        vm.ds_write(addr, vm.ds_read(addr) - v2_vm_accumulator);
    } else {
        // sub_14704: ds:[addr] += acc
        vm.ds_write(addr, vm.ds_read(addr) + v2_vm_accumulator);
    }
}

// 0xA8 (sub_14d91): sub_153ea bit test (3 bytes). If ne → skip 2, eq → jump.
// sub_153ea: byte idx + word VALUE (bytecode literal, NOT ds:[addr]!) → test
static void v2_vm_op_A8(V2VM& vm) {
    uint8_t idx1 = vm.read_u8();
    uint16_t val = vm.read_u16();  // bytecode literal VALUE
    uint16_t mask = *(uint16_t*)(vm.ds + (uint16_t)(idx1 - 0x6C34));
    uint16_t result = (val & mask) ? 1 : 0;
    if (result != v2_vm_accumulator) { vm.pc += 2; } else { v2_vm_do_jump(vm); }
}

// 0xB3 (sub_14e47): sub_15403 bit test (2B, pattern B). If ne → skip 2, eq → call-jump.
static void v2_vm_op_B3(V2VM& vm) {
    uint8_t idx1 = vm.read_u8();
    uint8_t idx2 = vm.read_u8();
    uint16_t field_off = *(uint16_t*)(vm.ds + (uint16_t)(idx2 - 0x6CBA));
    uint16_t si = field_off + vm.global_r(0x42); // pattern B: NO +0x1995
    uint16_t val = vm.ds_read(si + 0x14E5);
    uint16_t mask = *(uint16_t*)(vm.ds + (uint16_t)(idx1 - 0x6C34));
    uint16_t result = (val & mask) ? 1 : 0;
    if (result != v2_vm_accumulator) { vm.pc += 2; } else { v2_vm_do_call_jump(vm); }
}

// 0x0B (sub_1369c): Unconditional hflip. 0 bytes. Always flips.
static void v2_vm_op_0B(V2VM& vm) {
    uint16_t si = vm.global_r(0x42);
    // sub_136a0: XOR flag + mirror bounds — always, no condition check
    vm.ds_write(si + 0x1585, vm.ds_read(si + 0x1585) ^ 0x40);
    uint16_t x = vm.ds_read(si + 0x173D);
    uint16_t new_1535 = x + x - vm.ds_read(si + 0x155D) - 1;
    uint16_t new_155D = x + x - vm.ds_read(si + 0x1535) - 1;
    vm.ds_write(si + 0x155D, new_155D);
    vm.ds_write(si + 0x1535, new_1535);
}

// 0x17 (sub_143f2): Set [obj+141D]=0, then read 2 signed bytes as X/Y velocity. 2 bytes.
static void v2_vm_op_17(V2VM& vm) {
    uint16_t si = vm.global_r(0x42);
    vm.ds_write(si + 0x141D, 0);
    // loc_14415: read signed byte → X velocity, read signed byte → Y velocity
    int8_t vx = (int8_t)vm.es[vm.pc]; vm.pc++;
    vm.ds_write(si + 0x1945, (uint16_t)(int16_t)vx); // CBW: sign-extend
    int8_t vy = (int8_t)vm.es[vm.pc]; vm.pc++;
    vm.ds_write(si + 0x196D, (uint16_t)(int16_t)vy);
}

static void v2_vm_sub_10e99(V2VM& vm); // forward decl

// 0x3E (sub_14590): Clear palette base. 0 bytes.
// Clears ds:0x342-0x344, AND ds:0x7EFD & 0xFE, conditional ds:0x7F00, then sub_10e99.
static void v2_vm_op_3E(V2VM& vm) {
    vm.ds_write_b(0x342, 0);
    vm.ds_write_b(0x343, 0);
    vm.ds_write_b(0x344, 0);
    uint8_t flags = vm.shadow[0x7EFD] & 0xFE;
    vm.ds_write_b(0x7EFD, flags);
    if (flags == 0) {
        vm.ds_write(0x7F00, 0x7F02);
    }
    vm.ds_write(0x7EFE, 4);
    v2_vm_sub_10e99(vm);
}

// 0x29 (sub_15017): Write tile to map + mark dirty. 2 mode bytes + 3 dispatches.
// mode1: bits 0-2 = X dispatch, bits 3-5 = Y dispatch
// mode2: bits 0-2 = tile value dispatch
// Calls sub_141e0 (tile map write) + sub_13fc2 (mark tile dirty, update tracking)
static void v2_vm_op_29(V2VM& vm) {
    printf("V2-TRACE: opcode 0x29 at pc=0x%04X obj=%d\n", vm.pc-1, vm.obj);
    // Mode byte 1: X/Y via dual dispatch
    uint16_t word1 = *(uint16_t*)(vm.es + vm.pc); vm.pc += 1;
    uint8_t mode1 = (uint8_t)(word1 & 0xFF);
    uint16_t si = v2_vm_dispatch_30C98(vm, mode1);       // X coord (tile units)
    uint16_t di = v2_vm_dispatch_30C98(vm, mode1 >> 3);  // Y coord (tile units)

    // Mode byte 2: tile value
    uint16_t word2 = *(uint16_t*)(vm.es + vm.pc); vm.pc += 1;
    uint8_t mode2 = (uint8_t)(word2 & 0xFF);
    uint16_t tile_val = v2_vm_dispatch_30C98(vm, mode2);

    v2_vm_sub_141e0(vm, si, di, tile_val);
    v2_vm_sub_13fc2(vm, si, di, tile_val);
}

// 0x2C (sub_15f2c): Collision search with type filter + jump. 3 bytes (1 mode + 2 target).
// Searches objects 0,2,4 for matching type + bounds overlap.
// On match: bx = jump target, stores matched obj. On no match: bx += 4 (3 params + 1 extra).
static void v2_vm_op_2C(V2VM& vm) {
    // ds:0x3AE = ds:[si+0x14E5] - 1 (self Y - 1)
    uint16_t self_si = vm.global_r(0x42);
    vm.ds_write(0x3AE, vm.ds_read(self_si + 0x14E5) - 1);

    // Read params: filter type index (1 byte) + jump target word (2 bytes)
    uint16_t ax_word = *(uint16_t*)(vm.es + vm.pc); vm.pc += 1;
    uint16_t filter_idx = ax_word & 0xFF;
    vm.ds_write(0x3AA, filter_idx);
    uint16_t jump_target = *(uint16_t*)(vm.es + vm.pc); vm.pc += 2;
    vm.ds_write(0x3AC, jump_target);

    // Search objects 0, 2, 4
    for (uint16_t si = 0; si < 6; si += 2) {
        if (vm.ds_read(si + 0x1355) == 0) continue;  // inactive
        if (si == self_si) continue;                   // skip self

        // Type match via sorted filter table at ds:[(uint16_t)(di - 0x6B34)]
        uint8_t obj_type = (uint8_t)vm.ds_read(si + 0x17DD);
        uint16_t di = filter_idx;
        bool matched = false;
        for (;;) {
            uint8_t filt = vm.shadow[(uint16_t)(di - 0x6B34)];
            if (obj_type < filt) break;     // below range → no match
            if (obj_type == filt) { matched = true; break; }
            di++;  // scan next entry
        }
        if (!matched) continue;

        // Y bounds: 3AE >= [si+14E5] AND (3AE-1) < [si+150D]
        uint16_t y_ref = vm.ds_read(0x3AE);
        if (y_ref < vm.ds_read(si + 0x14E5)) continue;
        if ((uint16_t)(y_ref - 1) >= vm.ds_read(si + 0x150D)) continue;

        // X bounds: [di+155D] >= [si+1535] AND [si+155D] >= [di+1535]
        uint16_t di2 = self_si;
        if (vm.ds_read(di2 + 0x155D) < vm.ds_read(si + 0x1535)) continue;
        if (vm.ds_read(si + 0x155D) < vm.ds_read(di2 + 0x1535)) continue;

        // Match found
        vm.ds_write(0x3B0, si);
        vm.ds_write(di2 + 0x1995, si);
        vm.ds_write(di2 + 0x137D, vm.pc);
        vm.pc = jump_target;
        return;
    }
    // No match — extra INC bx
    vm.pc += 1;
}

// 0x2D (sub_15f25): Continue collision search from previous match. 0 bytes on miss.
// Resumes search from ds:0x3B0 + 2. On match: jump to ds:0x3AC. On miss: 0 bytes.
static void v2_vm_op_2D(V2VM& vm) {
    // DEC bx (original decrements PC by 1)
    vm.pc -= 1;
    uint16_t self_si = vm.global_r(0x42);
    uint16_t start_si = vm.ds_read(0x3B0);
    uint16_t filter_idx = vm.ds_read(0x3AA);

    for (uint16_t si = start_si + 2; si < 6; si += 2) {
        if (vm.ds_read(si + 0x1355) == 0) continue;
        if (si == self_si) continue;

        uint8_t obj_type = (uint8_t)vm.ds_read(si + 0x17DD);
        uint16_t di = filter_idx;
        bool matched = false;
        for (;;) {
            uint8_t filt = vm.shadow[(uint16_t)(di - 0x6B34)];
            if (obj_type < filt) break;
            if (obj_type == filt) { matched = true; break; }
            di++;
        }
        if (!matched) continue;

        uint16_t y_ref = vm.ds_read(0x3AE);
        if (y_ref < vm.ds_read(si + 0x14E5)) continue;
        if ((uint16_t)(y_ref - 1) >= vm.ds_read(si + 0x150D)) continue;

        uint16_t di2 = self_si;
        if (vm.ds_read(di2 + 0x155D) < vm.ds_read(si + 0x1535)) continue;
        if (vm.ds_read(si + 0x155D) < vm.ds_read(di2 + 0x1535)) continue;

        vm.ds_write(0x3B0, si);
        vm.ds_write(di2 + 0x1995, si);
        vm.ds_write(di2 + 0x137D, vm.pc);  // bx = B-1
        vm.pc = vm.ds_read(0x3AC);
        return;
    }
    // No match — INC bx restores PC
    vm.pc += 1;
}

// sub_10e99: Palette color correction. Copies ds:0x7F02 → ds:0x8202 (256×3 bytes)
// with per-channel subtraction and clamping to [0, 0x3F].
// Color offsets: R = ds:0x342 | ds:0x345, G = ds:0x343 | ds:0x346, B = ds:0x344 | ds:0x347.
// First 3 bytes get color correction, then 45 bytes copied raw, then 240×3 bytes corrected.
static void v2_vm_sub_10e99(V2VM& vm) {
    uint8_t r_off = vm.shadow[0x342] | vm.shadow[0x345];
    uint8_t g_off = vm.shadow[0x343] | vm.shadow[0x346];
    uint8_t b_off = vm.shadow[0x344] | vm.shadow[0x347];

    // Note: original m2c sub_10e99 doesn't correctly update palette buffer
    // (STOSB/es segment issue in m2c translation). v2 implementation is more correct.

    uint16_t si = 0x7F02; // source
    uint16_t di = 0x8202; // destination

    // First iteration: 3 corrected bytes + 45 raw bytes
    // R
    int8_t v = (int8_t)(vm.shadow[si++] - r_off);
    vm.shadow[di++] = (v < 0) ? 0 : ((v & 0x40) ? 0x3F : (uint8_t)v);
    // G
    v = (int8_t)(vm.shadow[si++] - g_off);
    vm.shadow[di++] = (v < 0) ? 0 : ((v & 0x40) ? 0x3F : (uint8_t)v);
    // B
    v = (int8_t)(vm.shadow[si++] - b_off);
    vm.shadow[di++] = (v < 0) ? 0 : ((v & 0x40) ? 0x3F : (uint8_t)v);
    // 45 raw bytes
    memcpy(vm.shadow + di, vm.shadow + si, 0x2D);
    si += 0x2D;
    di += 0x2D;

    // Remaining 239 iterations: 3 corrected bytes each (colors 16-254; color 255 not processed)
    for (int i = 0; i < 239; i++) {
        v = (int8_t)(vm.shadow[si++] - r_off);
        vm.shadow[di++] = (v < 0) ? 0 : ((v & 0x40) ? 0x3F : (uint8_t)v);
        v = (int8_t)(vm.shadow[si++] - g_off);
        vm.shadow[di++] = (v < 0) ? 0 : ((v & 0x40) ? 0x3F : (uint8_t)v);
        v = (int8_t)(vm.shadow[si++] - b_off);
        vm.shadow[di++] = (v < 0) ? 0 : ((v & 0x40) ? 0x3F : (uint8_t)v);
    }
}

// Helper: compute indexed+1995 write target address.
// Pattern A: idx byte → di = ds:[idx-0x6CBA]; si = ds:0x42; di += ds:[si+0x1995]; target = di + 0x14E5
// Used by 0x5B (ADD), 0x5E (SUB), 0x64 (OR), 0x67 (AND) indexed+1995 variants.
static uint16_t v2_vm_indexed_1995_target(V2VM& vm) {
    uint16_t idx = vm.read_u8();
    uint16_t di = *(uint16_t*)(vm.ds + (uint16_t)(idx - 0x6CBA));
    uint16_t si = vm.global_r(0x42);
    di += vm.ds_read(si + 0x1995);
    return (uint16_t)(di + 0x14E5);
}

// 0x5B (sub_14721): ADD acc to indexed+1995 field. 1 byte.
static void v2_vm_op_5B(V2VM& vm) {
    uint16_t addr = v2_vm_indexed_1995_target(vm);
    vm.ds_write(addr, vm.ds_read(addr) + v2_vm_accumulator);
}

// 0x5E (sub_1478b): SUB acc from indexed+1995 field. 1 byte.
static void v2_vm_op_5E(V2VM& vm) {
    uint16_t addr = v2_vm_indexed_1995_target(vm);
    vm.ds_write(addr, vm.ds_read(addr) - v2_vm_accumulator);
}

// 0x64 (sub_1480b): OR acc into indexed+1995 field. 1 byte.
static void v2_vm_op_64(V2VM& vm) {
    uint16_t addr = v2_vm_indexed_1995_target(vm);
    vm.ds_write(addr, vm.ds_read(addr) | v2_vm_accumulator);
}

// 0x66 (sub_1483f): XOR acc with ds:[addr]. 2 bytes (direct address).
// si = es:[bx]; bx += 2; ds:[si] ^= acc
static void v2_vm_op_66(V2VM& vm) {
    uint16_t addr = vm.read_u16();
    vm.ds_write(addr, vm.ds_read(addr) ^ v2_vm_accumulator);
}

// Helper: compute indexed field B target address (ds:0x42-relative).
// Pattern B: idx byte → si = ds:[idx-0x6CBA]; si += ds:0x42; target = si + 0x14E5
// Used by 0x90, 0x93 conditional ADD/SUB variants.
static uint16_t v2_vm_indexed_field_b_target(V2VM& vm) {
    uint16_t idx = vm.read_u8();
    uint16_t si = *(uint16_t*)(vm.ds + (uint16_t)(idx - 0x6CBA));
    si += vm.global_r(0x42);
    return (uint16_t)(si + 0x14E5);
}

// sub_146de: indexed field ADD (pattern B). 1 byte.
static void v2_vm_sub_146de(V2VM& vm) {
    uint16_t addr = v2_vm_indexed_field_b_target(vm);
    vm.ds_write(addr, vm.ds_read(addr) + v2_vm_accumulator);
}

// sub_1474b: indexed field SUB (pattern B). 1 byte.
static void v2_vm_sub_1474b(V2VM& vm) {
    uint16_t addr = v2_vm_indexed_field_b_target(vm);
    vm.ds_write(addr, vm.ds_read(addr) - v2_vm_accumulator);
}

// 0x6E (sub_148d3): indexed field (sub_15485, 1B), unsigned acc < val → jump, else skip 2.
static void v2_vm_op_6E(V2VM& vm) {
    uint16_t val = v2_vm_read_indexed_field(vm);
    if (v2_vm_accumulator < val)
        v2_vm_do_jump(vm);
    else
        vm.pc += 2;
}

// 0x70 (sub_148f7): indexed+1995 (sub_154a3, 1B), unsigned acc < val → jump, else skip 2.
static void v2_vm_op_70(V2VM& vm) {
    uint16_t val = v2_vm_read_indexed_field_1995(vm);
    if (v2_vm_accumulator < val)
        v2_vm_do_jump(vm);
    else
        vm.pc += 2;
}

// 0x75 (sub_14a3b): indexed+1995 (sub_154a3, 1B), val != acc → skip 2, val == acc → jump.
static void v2_vm_op_75(V2VM& vm) {
    uint16_t val = v2_vm_read_indexed_field_1995(vm);
    if (val != v2_vm_accumulator)
        vm.pc += 2;
    else
        v2_vm_do_jump(vm);
}

// 0x7A (sub_14a8b): indexed+1995 (sub_154a3, 1B), val == acc → skip 2, val != acc → jump.
static void v2_vm_op_7A(V2VM& vm) {
    uint16_t val = v2_vm_read_indexed_field_1995(vm);
    if (val == v2_vm_accumulator)
        vm.pc += 2;
    else
        v2_vm_do_jump(vm);
}

// 0x84 (sub_149db): indexed+1995 (sub_154a3, 1B), signed acc >= val → skip 2, else → jump.
static void v2_vm_op_84(V2VM& vm) {
    uint16_t val = v2_vm_read_indexed_field_1995(vm);
    if ((int16_t)v2_vm_accumulator >= (int16_t)val)
        vm.pc += 2;
    else
        v2_vm_do_jump(vm);
}

// 0x90 (sub_146d0): conditional ADD/SUB based on bit 0x40 flag. 1 byte.
// If bit 0x40 clear → ADD (sub_146de). If set → SUB (sub_1474b).
static void v2_vm_op_90(V2VM& vm) {
    uint16_t si = vm.global_r(0x42);
    if (vm.ds_read(si + 0x1585) & 0x40)
        v2_vm_sub_1474b(vm);
    else
        v2_vm_sub_146de(vm);
}

// 0x93 (sub_1473d): conditional SUB/ADD — opposite of 0x90. 1 byte.
// If bit 0x40 clear → SUB (sub_1474b). If set → ADD (sub_146de).
static void v2_vm_op_93(V2VM& vm) {
    uint16_t si = vm.global_r(0x42);
    if (vm.ds_read(si + 0x1585) & 0x40)
        v2_vm_sub_146de(vm);
    else
        v2_vm_sub_1474b(vm);
}

// 0xCE (sub_152d6): Viewport visibility check. 2 bytes (jump target).
// Checks if object at ds:0x42 is within extended viewport bounds.
// In viewport → off_30C92[2] = skip 2. Not in viewport → off_30C94[2] = jump.
// Viewport check: (ds:0x44+0x1F < obj_X < ds:0x44+0x121) AND (ds:0x46+0x1F < obj_Y < ds:0x46+0x91)
static void v2_vm_op_CE(V2VM& vm) {
    uint16_t si = vm.global_r(0x42);
    uint16_t obj_x = vm.ds_read(si + 0x173D);
    uint16_t obj_y = vm.ds_read(si + 0x1765);
    uint16_t vp_x = vm.ds_read(0x44);
    uint16_t vp_y = vm.ds_read(0x46);

    // Check X: (vp_x + 0x1F) < obj_x AND obj_x < (vp_x + 0x121)
    // Check Y: (vp_y + 0x1F) < obj_y AND obj_y < (vp_y + 0x91)
    bool in_viewport = true;
    int16_t ax = (int16_t)(vp_x + 0x1F);
    if (ax >= (int16_t)obj_x) in_viewport = false;
    else {
        ax += 0x102;
        if (ax < (int16_t)obj_x) in_viewport = false;
    }
    if (in_viewport) {
        ax = (int16_t)(vp_y + 0x1F);
        if (ax >= (int16_t)obj_y) in_viewport = false;
        else {
            ax += 0x72;
            if (ax < (int16_t)obj_y) in_viewport = false;
        }
    }

    if (in_viewport) {
        // off_30C92[2] = loc_15318 = skip 2
        vm.pc += 2;
    } else {
        // off_30C94[2] = sub_142cf = jump
        v2_vm_do_jump(vm);
    }
}

// 0x4C (sub_14561): Set color shading. 3 bytes (RGB offsets).
// Reads 3 bytes, SHL each by 1, stores to ds:0x345-347.
// Sets ds:0x7EFD |= 2, ds:0x7EFE = 4, ds:0x7F00 = 0x8202.
// Then calls sub_10e99 (palette color correction).
static void v2_vm_op_4C(V2VM& vm) {
    uint8_t r = vm.read_u8();
    vm.ds_write_b(0x345, (uint8_t)(r << 1));
    uint8_t g = vm.read_u8();
    vm.ds_write_b(0x346, (uint8_t)(g << 1));
    uint8_t b = vm.read_u8();
    vm.ds_write_b(0x347, (uint8_t)(b << 1));
    uint8_t flags = vm.shadow[0x7EFD];
    vm.ds_write_b(0x7EFD, flags | 2);
    vm.ds_write(0x7EFE, 4);
    vm.ds_write(0x7F00, 0x8202);
    v2_vm_sub_10e99(vm);
}

// 0x4D (sub_145b5): Clear color shading. 0 bytes.
// Clears ds:0x345-347 to 0, AND ds:0x7EFD with 0xFD.
// If result is 0: ds:0x7F00 = 0x7F02. ds:0x7EFE = 4.
// Then calls sub_10e99 (palette color correction).
static void v2_vm_op_4D(V2VM& vm) {
    vm.ds_write_b(0x345, 0);
    vm.ds_write_b(0x346, 0);
    vm.ds_write_b(0x347, 0);
    uint8_t flags = vm.shadow[0x7EFD] & 0xFD;
    vm.ds_write_b(0x7EFD, flags);
    if (flags == 0) {
        vm.ds_write(0x7F00, 0x7F02);
    }
    vm.ds_write(0x7EFE, 4);
    v2_vm_sub_10e99(vm);
}

// 0x3B (sub_1524a): Set sprite/animation params. 2 bytes.
// ds:0x39C = ax & 0xFF; ds:0x3A0 = 0; ds:0x3A4 = (ax >> 6) & 0x3FC
static void v2_vm_op_3B(V2VM& vm) {
    uint16_t ax = vm.read_u16();
    vm.ds_write(0x39C, ax & 0xFF);
    vm.ds_write(0x3A0, 0);
    vm.ds_write(0x3A4, (ax >> 6) & 0x3FC);
}

// 0x2A (sub_15078): Tilemap modification — set lower bits. 2 mode bytes + 3 dispatches.
// Reads X/Y position + tile index. Reads current tile, keeps upper 6 bits, ORs new lower 10 bits.
// Calls sub_141e0 (write) + sub_13fc2 (mark dirty).
static void v2_vm_op_2A(V2VM& vm) {
    printf("V2-TRACE: opcode 0x2A at pc=0x%04X obj=%d\n", vm.pc-1, vm.obj);
    // Mode byte 1: X/Y position via dual dispatch
    uint16_t word1 = *(uint16_t*)(vm.es + vm.pc); vm.pc += 1;
    uint8_t mode1 = (uint8_t)(word1 & 0xFF);
    uint16_t x_pos = v2_vm_dispatch_30C98(vm, mode1);
    vm.ds_write(0x6C, x_pos);
    uint16_t y_pos = v2_vm_dispatch_30C98(vm, mode1 >> 3);
    vm.ds_write(0x6E, y_pos);
    // Mode byte 2: tile index via single dispatch
    uint16_t word2 = *(uint16_t*)(vm.es + vm.pc); vm.pc += 1;
    uint8_t mode2 = (uint8_t)(word2 & 0xFF);
    uint16_t tile_val = v2_vm_dispatch_30C98(vm, mode2);
    vm.ds_write(0x34, tile_val);
    // sub_141ba: read current tile, AND FC00 (keep upper flags), OR with tile_val
    uint16_t si = x_pos, di = y_pos;
    uint16_t current = v2_vm_sub_141ba(vm, si, di);
    uint16_t merged = (current & 0xFC00) | tile_val;
    // sub_141e0: write merged tile back
    v2_vm_sub_141e0(vm, si, di, merged);
    // sub_13fc2: mark tile dirty
    v2_vm_sub_13fc2(vm, si, di, merged);
}

// 0x2B (sub_15039): Set upper tile flags. 2 mode bytes + 3 dispatches.
// Dispatches X, Y, tile_val. Transforms tile_val (xchg ah,al; shl 2; and FC00) to upper 6 bits.
// Reads current tile via sub_141b3 (& 0x3FF = lower 10 bits), ORs with new flags.
// Writes back via sub_141e0. No sub_13fc2 call.
static void v2_vm_op_2B(V2VM& vm) {
    // Mode byte 1: X/Y via dual dispatch
    uint16_t word1 = *(uint16_t*)(vm.es + vm.pc); vm.pc += 1;
    uint8_t mode1 = (uint8_t)(word1 & 0xFF);
    uint16_t x_pos = v2_vm_dispatch_30C98(vm, mode1);
    vm.ds_write(0x6C, x_pos);
    uint16_t y_pos = v2_vm_dispatch_30C98(vm, mode1 >> 3);
    vm.ds_write(0x6E, y_pos);
    // Mode byte 2: tile flags value
    uint16_t word2 = *(uint16_t*)(vm.es + vm.pc); vm.pc += 1;
    uint8_t mode2 = (uint8_t)(word2 & 0xFF);
    uint16_t tile_raw = v2_vm_dispatch_30C98(vm, mode2);
    // Transform: xchg ah,al → shl ax,2 → and ax,0xFC00
    uint16_t swapped = (uint16_t)((tile_raw >> 8) | (tile_raw << 8));
    uint16_t transformed = (swapped << 2) & 0xFC00;
    vm.ds_write(0x34, transformed);
    // sub_141b3: read current tile (& 0x3FF = lower 10 bits)
    uint16_t si = x_pos, di = y_pos;
    uint16_t current_low = v2_vm_sub_141b3(vm, si, di);
    uint16_t merged = current_low | transformed;
    // sub_141e0: write merged tile back
    v2_vm_sub_141e0(vm, si, di, merged);
    // No sub_13fc2 call in sub_15039
}

// 0x28 (sub_14f27): Tile-aligned position write. 2 mode bytes + 4 dispatches.
// Mode1: read X/Y from off_30C98, align to 16px grid (& 0xFFF0 | 8)
// Mode2: write via sub_154bf (X), loc_154bc (Y)
static void v2_vm_op_28(V2VM& vm) {
    // Read mode1 byte
    uint16_t word1 = *(uint16_t*)(vm.es + vm.pc); vm.pc += 1;
    uint8_t mode1 = (uint8_t)(word1 & 0xFF);
    // Dispatch X: (mode1 & 7) → align to tile
    uint16_t cx_val = (v2_vm_dispatch_30C98(vm, mode1) & 0xFFF0) | 8;
    // Dispatch Y: ((mode1 >> 3) & 7) → align to tile
    uint16_t dx_val = (v2_vm_dispatch_30C98(vm, mode1 >> 3) & 0xFFF0) | 8;
    // Read mode2 byte
    uint8_t mode2 = vm.read_u8();
    // sub_154bf(cx_val, mode2) — X write
    v2_vm_sub_154bf(vm, cx_val, mode2);
    // loc_154bc: SHR mode2,3 → sub_154bf(dx_val, mode2>>3) — Y write
    v2_vm_sub_154bf(vm, dx_val, mode2 >> 3);
}

// Helper: collision search loop (shared by 0xD0, 0xD1, 0x35, 0x36).
// Iterates objects from si_start, checks type/bounds match, sets ds:0x3B0/0x1995/0x137D.
static void v2_vm_collision_search_loop(V2VM& vm, uint16_t si_start) {
    uint16_t table_end = vm.global_r(0x372);
    uint16_t di = vm.global_r(0x42);

    for (uint16_t si = si_start + 2; (int16_t)si < (int16_t)table_end; si += 2) {
        if (vm.ds_read(si + 0x1355) == 0) continue;
        if (si == di) continue;
        // Type match
        uint8_t obj_type = (uint8_t)vm.ds_read(si + 0x17DD);
        uint16_t flt = vm.ds_read(0x3AA);
        bool match = false;
        while (true) {
            uint8_t fval = *(uint8_t*)(vm.ds + (uint16_t)(flt - 0x6B34));
            if (obj_type < fval) break;
            if (obj_type == fval) { match = true; break; }
            flt++;
        }
        if (!match) continue;
        // Bounds check
        if ((int16_t)vm.ds_read(0x3AE) < (int16_t)vm.ds_read(si + 0x14E5)) continue;
        if ((int16_t)(vm.ds_read(0x3AE) - 1) < (int16_t)vm.ds_read(si + 0x150D)) continue;
        if ((int16_t)vm.ds_read(di + 0x155D) < (int16_t)vm.ds_read(si + 0x1535)) continue;
        if ((int16_t)vm.ds_read(si + 0x155D) < (int16_t)vm.ds_read(di + 0x1535)) continue;
        // Match found
        vm.ds_write(0x3B0, si);
        vm.ds_write(di + 0x1995, si);
        vm.ds_write(di + 0x137D, vm.pc);
        vm.pc = vm.ds_read(0x3AC);
        return;
    }
    // No match: INC bx (extra byte consumed — original has INC bx after loop exit)
    vm.pc += 1;
}

// 0xD0 (sub_15e7c): Collision search. 3 bytes consumed (1 filter + 2 target addr).
// Sets up ds:0x3AE (Y bound), ds:0x3AA (filter), ds:0x3AC (jump target), then searches.
static void v2_vm_op_D0(V2VM& vm) {
    uint16_t si = vm.global_r(0x42);
    vm.ds_write(0x3AE, vm.ds_read(si + 0x150D) + 1);
    // Read 1 byte filter → ds:0x3AA
    uint8_t filter = vm.read_u8();
    vm.ds_write(0x3AA, filter);
    // Read 2 bytes jump target → ds:0x3AC
    uint16_t target = vm.read_u16();
    vm.ds_write(0x3AC, target);
    // Search from si=0
    v2_vm_collision_search_loop(vm, 0xFFFE); // -2 so first iteration = 0
}

// 0xD1 (sub_15f17): Same as 0xD0 but different continuation. 3 bytes.
static void v2_vm_op_D1(V2VM& vm) {
    uint16_t si = vm.global_r(0x42);
    vm.ds_write(0x3AE, vm.ds_read(si + 0x150D) + 1);
    uint8_t filter = vm.read_u8();
    vm.ds_write(0x3AA, filter);
    uint16_t target = vm.read_u16();
    vm.ds_write(0x3AC, target);
    v2_vm_collision_search_loop(vm, 0xFFFE);
}

// 0x43 (sub_1267b): Command buffer write type=4. 0 bytes.
static void v2_vm_op_43(V2VM& vm) {
    uint16_t bx_cmd = vm.ds_read(0x218F);
    vm.ds_write(bx_cmd + 0x1DA7, 4);
    vm.ds_write(0x218F, bx_cmd + 2);
}

// sub_163ac: Tile type check for platform detection. 0 bytes. Sets carry.
// sub_163ac: Exact tile check chain. 3 sub_14199 calls with conditional logic.
static bool v2_vm_sub_163ac(V2VM& vm) {
    uint16_t obj = vm.global_r(0x42);
    uint16_t si_x;
    if (!(vm.ds_read(obj + 0x1585) & 0x40)) {
        si_x = vm.ds_read(obj + 0x173D) + 0x10;
    } else {
        si_x = vm.ds_read(obj + 0x173D) - 0x10;
    }
    uint16_t di_y = vm.ds_read(obj + 0x150D);

    // Helper: tile type at (si, di) via sub_14199
    auto tile_type_at = [&](uint16_t sx, uint16_t dy) -> uint16_t {
        uint16_t tv = v2_vm_sub_141ba(vm, sx >> 4, dy >> 4);
        return (tv & 0xFC00) >> 10;
    };

    // Check 1: tile at (si_x, di_y)
    uint16_t ax = tile_type_at(si_x, di_y);
    if (ax >= 0x30) return true; // carry

    if (ax == 1) {
        // Type 1: check tile above (di_y - 0x10)
        ax = tile_type_at(si_x, di_y - 0x10);
        if (ax >= 0x30) return true;
        if (ax == 0 || ax == 0x0C || ax == 3) return true;
        // Fall through to check 2
    }

    // Check 2: tile at original X position (si from obj+0x173D, NOT si_x)
    // Original reloads: di = ds:0x42; di = ds:[di+0x150D]; si preserved from check 1
    uint16_t di_y2 = vm.ds_read(vm.global_r(0x42) + 0x150D);
    ax = tile_type_at(si_x, di_y2);
    if (ax == 0 || ax == 0x0C || ax == 3) {
        // Check 3: tile below (di_y2 + 0x10)
        ax = tile_type_at(si_x, di_y2 + 0x10);
        if (ax >= 0x30) return true;
        if (ax == 1 || ax == 5 || ax == 0x20 || ax == 4 || ax == 2) return true;
        return false; // no carry
    }
    return false; // no carry
}

// 0x13 (sub_1434c): Level/palette command. 3 bytes consumed (always ADD bx,3).
// al byte determines action: 0xD9=palette copy, 0x11=text menu, 0x01=level transition.
// All paths end with ADD bx,3.
static void v2_vm_op_13(V2VM& vm) {
    uint16_t word = *(uint16_t*)(vm.es + vm.pc);
    uint8_t al = (uint8_t)(word & 0xFF);

    if (al == 0xD9) {
        // Palette copy: reads 48 bytes from bytecode (es:[bx+1]) into ds:0x8142/0x81A2
        // Uses es:[bx+1] as source, copies 48 bytes (12 dwords) to palette areas
        uint16_t src_off = vm.pc + 1;
        for (int i = 0; i < 48; i++) {
            vm.ds_write_b(0x8142 + i, vm.es[src_off + i]);
        }
        for (int i = 0; i < 48; i++) {
            vm.ds_write_b(0x81A2 + i, vm.es[src_off + i]);
        }
        // sub_10e99: palette color correction (copies 7F02 → 8202 with shading)
        v2_vm_sub_10e99(vm);
        vm.ds_write(0x7EFE, 4);
        vm.ds_write(0x7F00, 0x8202);
    } else if (al == 0x11) {
        // Text menu: triggers redraw. Game state, minimal VM impact.
    } else if (al == 0x01) {
        // Level transition: modifies word_2aaa9 (level number)
        uint16_t level = *(uint16_t*)(vm.es + vm.pc + 1);
        vm.ds_write(0xAAA9, level);
    }

    // All paths: ADD bx, 3
    vm.pc += 3;
}

// 0x4E (sub_144fd): sub_163ac tile check + off_30C8E[0]. 0 bytes.
static void v2_vm_op_4E(V2VM& vm) {
    vm.carry = v2_vm_sub_163ac(vm);
    // off_30C8E[0] = loc_144e9: no carry → skip 2, carry → jump
    uint16_t cs_addr = *(uint16_t*)(vm.ds + 0x87AE + 0);
    if (cs_addr == 0x44E9) {
        if (!vm.carry) { vm.pc += 2; } else { v2_vm_do_jump(vm); }
    } else if (cs_addr == 0x44F3) {
        if (vm.carry) { vm.pc += 2; } else { v2_vm_do_jump(vm); }
    } else {
        v2_vm_runtime_dispatch(vm, 0x87AE, 0);
    }
}

// 0xD3 (sub_12829): Password verify. 0 bytes. Searches password table, sets level.
static void v2_vm_op_D3(V2VM& vm) {
    // Search table at ds:[(uint16_t)(si - 0x7A5B)] for 4-byte match
    // with word_287F0..287F6 (at ds:0x87F0..0x87F6)
    uint16_t pw0 = vm.ds_read(0x87F0);
    uint16_t pw1 = vm.ds_read(0x87F2);
    uint16_t pw2 = vm.ds_read(0x87F4);
    uint16_t pw3 = vm.ds_read(0x87F6);

    for (uint16_t si = 0; (int16_t)si < 0x94; si += 4) {
        uint16_t addr_base = (uint16_t)(si - 0x7A5B);
        uint8_t c0 = *(vm.ds + addr_base) & 0x7F;
        uint8_t c1 = *(vm.ds + (uint16_t)(addr_base + 1)) & 0x7F;
        uint8_t c2 = *(vm.ds + (uint16_t)(addr_base + 2)) & 0x7F;
        uint8_t c3 = *(vm.ds + (uint16_t)(addr_base + 3)) & 0x7F;

        if (c0 == (pw0 & 0xFF) && c1 == (pw1 & 0xFF) &&
            c2 == (pw2 & 0xFF) && c3 == (pw3 & 0xFF)) {
            // Found: level = si / 4
            uint16_t level = si >> 2;
            // word_2AAA9 at ds:0xAAA9 (in full shadow now)
            vm.ds_write(0xAAA9, level);
            // byte_287E8 = 0
            vm.ds_write_b(0x87E8, 0);
            return;
        }
    }
    // Not found
    vm.ds_write_b(0x87E8, 1);
}

// 0x2E (sub_1522c): Set collision search params. 2 bytes consumed.
static void v2_vm_op_2E(V2VM& vm) {
    uint16_t word = vm.read_u16();
    vm.ds_write(0x39A, word & 0xFF);
    vm.ds_write(0x39E, 0);
    vm.ds_write(0x3A2, (word >> 7) & 0x1FE);
}

// 0x50 (sub_126a9): Text position cmd. 2 mode bytes + 3 off_30C98 dispatches.
static void v2_vm_op_50(V2VM& vm) {
    // Mode byte 1: dual dispatch
    uint16_t word1 = *(uint16_t*)(vm.es + vm.pc); vm.pc += 1;
    uint8_t mode1 = (uint8_t)(word1 & 0xFF);
    v2_vm_dispatch_30C98(vm, mode1);           // → word_28514
    v2_vm_dispatch_30C98(vm, mode1 >> 3);      // → word_2854C
    // Mode byte 2: single dispatch
    uint16_t word2 = *(uint16_t*)(vm.es + vm.pc); vm.pc += 1;
    uint8_t mode2 = (uint8_t)(word2 & 0xFF);
    v2_vm_dispatch_30C98(vm, mode2);           // → word_2854E
    // Command buffer writes — game state, not VM state
}

// 0x6D (sub_148c1): sub_1547e (literal, 2B). Unsigned acc < val → jump, acc >= val → skip 2.
static void v2_vm_op_6D(V2VM& vm) {
    uint16_t val = v2_vm_read_literal(vm);
    if (v2_vm_accumulator < val) { v2_vm_do_jump(vm); } else { vm.pc += 2; }
}

// 0xC8 (sub_1527b): write acc to state[idx*14+2]. 0 bytes.
static void v2_vm_op_C8(V2VM& vm) {
    uint16_t si = vm.global_r(0x42);
    uint16_t idx = vm.ds_read(si + 0x16C5);
    if (idx & 0x8000) return;
    vm.ds_write(idx * 0x0E + 2 + 0x25F6, v2_vm_accumulator);
}

// 0xCA (sub_152b3): write acc to state[idx*14+0xC]. 0 bytes.
static void v2_vm_op_CA(V2VM& vm) {
    uint16_t si = vm.global_r(0x42);
    uint16_t idx = vm.ds_read(si + 0x16C5);
    if (idx & 0x8000) return;
    vm.ds_write(idx * 0x0E + 0x0C + 0x25F6, v2_vm_accumulator);
}

// 0x55 (sub_1466e): acc = sub_12312 (random). 0 bytes consumed.
// sub_12312: if word_288AC != 0 → alternate XOR path. Else LCG.
// Both paths read/write game globals outside DS shadow range.
static void v2_vm_op_55(V2VM& vm) {
    // word_288AC, word_28832, dword_30B19 are all in real DS or CS space.
    // Access via vm.ds (real DS pointer for out-of-shadow reads).

    // word_288AC is at a high DS address. Check if non-zero for alternate path.
    // Address: word_288AC = DS global. Using real DS.
    uint16_t check = *(uint16_t*)(vm.ds + 0x88AC);
    if (check != 0) {
        // Alternate XOR random: word_28832
        // Original: ax = word_28832; XCHG ah,al; word_28832 = ax; RCL ax,3; XOR word_28832, ax
        // v2 uses own copy to avoid corrupting original state
        static uint16_t v2_word_28832 = 0;
        static bool v2_28832_init = false;
        if (!v2_28832_init) { v2_word_28832 = *(uint16_t*)(vm.ds + 0x8832); v2_28832_init = true; }
        uint16_t ax = v2_word_28832;
        ax = (ax >> 8) | (ax << 8); // XCHG ah,al
        v2_word_28832 = ax;
        // RCL ax,3 — rotate left through carry, 3 times. Approximate:
        ax = (ax << 3) | (ax >> 13);
        v2_word_28832 ^= ax;
        v2_vm_accumulator = v2_word_28832;
    } else {
        // LCG: own seed copy to avoid corrupting original
        static uint32_t v2_random_seed = 0;
        static bool v2_seed_init = false;
        if (!v2_seed_init) {
            v2_random_seed = *(uint32_t*)(vm.cs_base + 0x30B19);
            v2_seed_init = true;
        }
        uint64_t tmp = (uint64_t)v2_random_seed * 0x15A4E35;
        v2_random_seed = (uint32_t)(tmp + 1);
        // ROR 16 = swap halves
        uint32_t result = (v2_random_seed >> 16) | (v2_random_seed << 16);
        v2_vm_accumulator = (uint16_t)result;
    }
}

// 0xC9 (sub_1529a): AND acc 0xCDFF + write to state[idx*14+0xA]. 0 bytes.
static void v2_vm_op_C9(V2VM& vm) {
    v2_vm_accumulator &= 0xCDFF;
    uint16_t si = vm.global_r(0x42);
    uint16_t idx = vm.ds_read(si + 0x16C5);
    if (idx & 0x8000) return;
    vm.ds_write(idx * 0x0E + 0x0A + 0x25F6, v2_vm_accumulator);
}

// 0x69 (sub_14879): sub_15485 (indexed field, 1B). Unsigned acc >= val → jump, else skip 2.
static void v2_vm_op_69(V2VM& vm) {
    uint16_t val = v2_vm_read_indexed_field(vm);
    if (v2_vm_accumulator >= val) { v2_vm_do_jump(vm); } else { vm.pc += 2; }
}

// 0xB0 (sub_14e14): sub_15445 (indexed+1995 bit test, 2 bytes) + eq→skip, ne→jump.
static void v2_vm_op_B0(V2VM& vm) {
    // sub_15445: 2 bytes consumed
    uint16_t val = v2_vm_read_indexed_field_15445(vm);
    if (val == v2_vm_accumulator) { vm.pc += 2; } else { v2_vm_do_jump(vm); }
}

// 0xBC (sub_14681): SHL acc<<8 + store to indexed field. 1 byte.
static void v2_vm_op_BC(V2VM& vm) {
    v2_vm_accumulator <<= 8;
    uint8_t idx = vm.read_u8();
    uint16_t si = *(uint16_t*)(vm.ds + (uint16_t)(idx - 0x6CBA));
    si += vm.global_r(0x42);
    vm.ds_write((uint16_t)(si + 0x14E5), v2_vm_accumulator);
}

// 0x07 (sub_1368c): Horizontal flip if NOT hflip. 0 bytes. Opposite of 0x08.
static void v2_vm_op_07(V2VM& vm) {
    // sub_1368c: if NOT flipped, call sub_136a0
    uint16_t si = vm.global_r(0x42);
    if (vm.ds_read(si + 0x1585) & 0x40) return;
    // sub_136a0: exact replica (same code as op_08)
    vm.ds_write(si + 0x1585, vm.ds_read(si + 0x1585) ^ 0x40);
    uint16_t dx = vm.ds_read(si + 0x173D) << 1;
    uint16_t new_1535 = dx - vm.ds_read(si + 0x155D) - 1;
    uint16_t new_155D = dx - vm.ds_read(si + 0x1535) - 1;
    vm.ds_write(si + 0x155D, new_155D);
    vm.ds_write(si + 0x1535, new_1535);
    if (vm.ds_read(si + 0x1AD5) != 0) {
        uint16_t dx2 = vm.ds_read(si + 0x173D) << 1;
        uint16_t cx = vm.ds_read(si + 0x1AAD);
        for (uint16_t di = vm.ds_read(si + 0x1A85); (int16_t)di < (int16_t)cx; di += 2) {
            vm.ds_write(di + 0x64D, dx2 - vm.ds_read(di + 0x64D) - vm.ds_read(di + 0xC4D));
            vm.ds_write(di + 0x44D, vm.ds_read(di + 0x44D) ^ 0x200);
            vm.ds_write(di + 0x114D, 0x202);
        }
    }
}

// 0x26 (sub_14edd): Read 2 mode bytes, 4 dispatches (2×off_30C98 + 2×off_30CA2).
// Mode1: dispatch (mode1 & 7) → >> 4 → ds:0x6C, dispatch ((mode1 >> 3) & 7) → >> 4 → ds:0x6E
// Mode2: sub_154bf(ds:0x6C, mode2), sub_154bf(ds:0x6E, mode2 >> 3)
static void v2_vm_op_26(V2VM& vm) {
    // Read mode1 byte
    uint16_t word1 = *(uint16_t*)(vm.es + vm.pc);
    vm.pc += 1;
    uint8_t mode1 = (uint8_t)(word1 & 0xFF);

    // First dispatch: (mode1 & 7) → value >> 4 → ds:0x6C
    uint16_t val1 = v2_vm_dispatch_30C98(vm, mode1);
    vm.ds_write(0x6C, val1 >> 4);

    // Second dispatch: ((mode1 >> 3) & 7) → value >> 4 → ds:0x6E
    uint16_t val2 = v2_vm_dispatch_30C98(vm, mode1 >> 3);
    vm.ds_write(0x6E, val2 >> 4);

    // Read mode2 byte
    uint8_t mode2 = vm.read_u8();

    // sub_154bf(ds:0x6C, mode2) → X axis write
    v2_vm_sub_154bf(vm, vm.ds_read(0x6C), mode2);

    // loc_154bc: SHR mode2,3 → sub_154bf(ds:0x6E, mode2 >> 3) → Y axis write
    v2_vm_sub_154bf(vm, vm.ds_read(0x6E), mode2 >> 3);
}

// 0x63 (sub_147ff): OR acc with ds:[addr]. 2 bytes.
// si = es:[bx]; ADD bx,2; ax = ds:0x8A; OR ds:[si], ax
static void v2_vm_op_63(V2VM& vm) {
    uint16_t addr = vm.read_u16();
    vm.ds_write(addr, vm.ds_read(addr) | v2_vm_accumulator);
}

// 0x7D (sub_14933): Signed >= indexed field (1 byte). acc >= val → jump, else skip 2.
static void v2_vm_op_7D(V2VM& vm) {
    uint16_t val = v2_vm_read_indexed_field(vm);
    if ((int16_t)v2_vm_accumulator >= (int16_t)val) {
        v2_vm_do_jump(vm);
    } else {
        vm.pc += 2;
    }
}

// 0x9D (sub_14ba7): Conditional mask set + AND/OR field. 3 bytes total.
// 1. Read byte idx (1B). If acc != 0: acc = mask[idx-0x6C34].
// 2. dx = ds:[idx-0x6C14] (clear mask).
// 3. Read word addr (2B). AND ds:[addr], dx. OR ds:[addr], acc.
static void v2_vm_op_9D(V2VM& vm) {
    uint8_t idx = vm.read_u8();
    if (v2_vm_accumulator != 0) {
        v2_vm_accumulator = *(uint16_t*)(vm.ds + (uint16_t)(idx - 0x6C34));
    }
    uint16_t clear_mask = *(uint16_t*)(vm.ds + (uint16_t)(idx - 0x6C14));
    uint16_t addr = vm.read_u16();
    uint16_t val = vm.ds_read(addr);
    val &= clear_mask;
    val |= v2_vm_accumulator;
    vm.ds_write(addr, val);
}

// 0x9E (sub_14bcf): Conditional mask set + AND/OR indexed+1995 field. 2 bytes.
// idx1 (mask), idx2 (field via indexed+1995 pattern A).
static void v2_vm_op_9E(V2VM& vm) {
    uint8_t idx1 = vm.read_u8();
    if (v2_vm_accumulator != 0) {
        v2_vm_accumulator = *(uint16_t*)(vm.ds + (uint16_t)(idx1 - 0x6C34));
    }
    uint16_t clear_mask = *(uint16_t*)(vm.ds + (uint16_t)(idx1 - 0x6C14));
    // idx2 → indexed+1995 field (pattern A)
    uint16_t addr = v2_vm_indexed_1995_target(vm); // reads 1 byte
    uint16_t val = vm.ds_read(addr);
    val &= clear_mask;
    val |= v2_vm_accumulator;
    vm.ds_write(addr, val);
}

// 0xB5 (sub_14e67): sub_15445 (indexed+1995 bit test, 2 bytes). If ne → skip 2, eq → call-jump.
static void v2_vm_op_B5(V2VM& vm) {
    uint16_t result = v2_vm_read_indexed_field_15445(vm);
    if (result != v2_vm_accumulator) { vm.pc += 2; } else { v2_vm_do_call_jump(vm); }
}

// 0x5F (sub_147a7): AND acc with indexed field. 1 byte.
// ds:[si+0x14E5] &= acc, where si = lookup[idx] + ds:0x42.
static void v2_vm_op_5F(V2VM& vm) {
    uint8_t idx = vm.read_u8();
    uint16_t si = *(uint16_t*)(vm.ds + (uint16_t)(idx - 0x6CBA));
    si += vm.global_r(0x42);
    uint16_t addr = (uint16_t)(si + 0x14E5);
    vm.ds_write(addr, vm.ds_read(addr) & v2_vm_accumulator);
}

// 0xAA (sub_14db1): sub_1542a (ds:[addr] bit test, 3 bytes). ne → skip 2, eq → jump.
static void v2_vm_op_AA(V2VM& vm) {
    // sub_1542a: byte idx + word addr → val=ds:[addr], mask=ds:[idx-0x6C34]
    uint8_t idx = vm.read_u8();
    uint16_t addr = vm.read_u16();
    uint16_t val = vm.ds_read(addr);
    uint16_t mask = *(uint16_t*)(vm.ds + (uint16_t)(idx - 0x6C34));
    uint16_t result = (val & mask) ? 1 : 0;
    if (result != v2_vm_accumulator) { vm.pc += 2; } else { v2_vm_do_jump(vm); }
}

// 0x27 (sub_14f09): Tile lookup + position write. 2 mode bytes + dispatches.
// Read mode1: dual off_30C98 dispatch → X/Y coords. Call sub_141a7 (tile type lookup).
// Read mode2: single sub_154bf dispatch with result.
static void v2_vm_op_27(V2VM& vm) {
    // sub_14f09: exact replica
    uint16_t word1 = *(uint16_t*)(vm.es + vm.pc);
    vm.pc += 1;
    uint8_t mode1 = (uint8_t)(word1 & 0xFF);
    uint16_t si_x = v2_vm_dispatch_30C98(vm, mode1);       // dx = ax (X coord)
    uint16_t di_y = v2_vm_dispatch_30C98(vm, mode1 >> 3);  // di = ax (Y coord)
    // sub_141a7: tile type lookup
    uint16_t tile_word = v2_vm_sub_141ba(vm, si_x, di_y);
    // AND 0xFC00; XCHG ah,al; SHR 2
    uint16_t tile_type = tile_word & 0xFC00;
    tile_type = (uint16_t)((tile_type >> 8) | (tile_type << 8)); // XCHG ah,al
    tile_type >>= 2;
    // si = tile_type; read mode2; JMP sub_154bf
    uint8_t mode2 = vm.read_u8();
    v2_vm_sub_154bf(vm, tile_type, mode2);
}

// 0x59 (sub_146de): Add acc to indexed field. 1 byte.
static void v2_vm_op_59(V2VM& vm) {
    uint8_t idx = vm.read_u8();
    uint16_t lookup = (uint16_t)(idx - 0x6CBA);
    uint16_t si = *(uint16_t*)(vm.ds + lookup);
    si += vm.global_r(0x42);
    uint16_t addr = (uint16_t)(si + 0x14E5);
    vm.ds_write(addr, vm.ds_read(addr) + v2_vm_accumulator);
}

// 0xAE (sub_14df4): sub_15403 bit test (2B, pattern B). If eq → skip 2, ne → jump.
static uint16_t v2_vm_read_indexed_field_15403(V2VM& vm); // forward decl
static void v2_vm_op_AE(V2VM& vm) {
    uint16_t result = v2_vm_read_indexed_field_15403(vm);
    if (result == v2_vm_accumulator) { vm.pc += 2; } else { v2_vm_do_jump(vm); }
}

// 0x36 (sub_15e8a): Search objects for collision match starting from ds:0x3B0.
// DEC bx first. If match found: save bx to alt_pc, set ds:0x1995=matched obj, jump to ds:0x3AC.
// If no match: INC bx (net 0 bytes).
static void v2_vm_op_36(V2VM& vm) {
    vm.pc -= 1; // DEC bx
    uint16_t si = vm.ds_read(0x3B0);
    uint16_t table_end = vm.global_r(0x372);
    uint16_t di = vm.global_r(0x42);

    for (si += 2; (int16_t)si < (int16_t)table_end; si += 2) {
        if (vm.ds_read(si + 0x1355) == 0) continue;
        if (si == di) continue;
        // Type match: compare ds:[si+0x17DD] with filter table at ds:0x3AA
        uint16_t obj_type = vm.ds_read(si + 0x17DD);
        uint16_t filter_idx = vm.ds_read(0x3AA);
        // Search filter table
        bool type_match = false;
        while (true) {
            uint8_t fval = *(uint8_t*)(vm.ds + (uint16_t)(filter_idx - 0x6B34));
            if ((uint8_t)obj_type < fval) break;           // below range
            if ((uint8_t)obj_type == fval) { type_match = true; break; }
            filter_idx++;
        }
        if (!type_match) continue;

        // Y bounds check
        int16_t ds_3AE = (int16_t)vm.ds_read(0x3AE);
        if (ds_3AE < (int16_t)vm.ds_read(si + 0x14E5)) continue;
        if (ds_3AE - 1 < (int16_t)vm.ds_read(si + 0x150D)) {} else continue; // actually: ds_3AE-1 < 150D → skip
        // Wait, re-read: CMP ax, [si+150D]; JL → skip. So if (ds_3AE-1) < [si+150D] → skip

        // Actually let me re-read the comparisons:
        // CMP ax(=ds:3AE), [si+14E5]; JL → skip (ax < y_top → skip)
        // DEC ax; CMP ax, [si+150D]; JL → skip (ax-1 < y_bot → skip, i.e. ax <= y_bot)
        // Hmm no: JL after CMP means skip if ax < operand.
        // So: ds_3AE < [si+0x14E5] → skip; (ds_3AE-1) < [si+0x150D] → skip
        // That doesn't make sense for an overlap test. Let me re-read carefully:
        // CMP ax, [si+14E5h]; JL loc_15f0c → if ax < y_top → no match → continue
        // DEC ax; CMP ax, [si+150Dh]; JL loc_15f0c → if (ax-1) < y_bot → skip...
        // This checks ds:3AE >= [si+14E5] AND (ds:3AE-1) >= [si+150D]
        // Actually that's: ds:3AE >= y_top AND ds:3AE > y_bot (since DEC)
        // Hmm, let me just translate literally:

        // Actually I'll just do it literally without over-thinking:
        // (already checked type match)
        if ((int16_t)vm.ds_read(0x3AE) < (int16_t)vm.ds_read(si + 0x14E5)) continue;
        if ((int16_t)(vm.ds_read(0x3AE) - 1) < (int16_t)vm.ds_read(si + 0x150D)) continue;

        // X bounds: di=ds:0x42
        if ((int16_t)vm.ds_read(di + 0x155D) < (int16_t)vm.ds_read(si + 0x1535)) continue;
        if ((int16_t)vm.ds_read(si + 0x155D) < (int16_t)vm.ds_read(di + 0x1535)) continue;

        // Match found!
        vm.ds_write(0x3B0, si);
        vm.ds_write(di + 0x1995, si);
        vm.ds_write(di + 0x137D, vm.pc); // save current PC as alt_pc
        vm.pc = vm.ds_read(0x3AC);        // jump to ds:0x3AC
        return;
    }

    // No match: INC bx (restore the DEC we did)
    vm.pc += 1;
}

// 0x45 (sub_12634): Text display variant. sub_1250b (1+N1) + sub_125fa (1+N2+N3).
// 0x45 (sub_12634): Text display + command buffer write type=0x0A.
// sub_1250b → first dispatch; sub_125fa → mode byte + 2 dispatches → si,di.
// Then writes to command buffer: type=0x0A, params=si,di,ds:0x002A. Advances cmd ptr by 8.
static void v2_vm_op_45(V2VM& vm) {
    // sub_1250b: read mode byte + dispatch (result unused by sub_12634)
    uint8_t mode1;
    v2_vm_sub_1250b(vm, mode1);

    // sub_125fa: mode byte + 2 dispatches → si (word_2854C = ds:0x6C), di
    uint16_t word2 = *(uint16_t*)(vm.es + vm.pc); vm.pc += 1;
    uint8_t mode2 = (uint8_t)(word2 & 0xFF);
    uint16_t si_val = v2_vm_dispatch_30C98(vm, mode2);
    vm.ds_write(0x6C, si_val);  // word_2854C at DS:0x006C
    uint16_t di_val = v2_vm_dispatch_30C98(vm, mode2 >> 3);

    // Command buffer write: type=0x0A, si, di, ds:0x002A
    uint16_t bx_cmd = vm.ds_read(0x218F);  // word_2A66F
    vm.ds_write(bx_cmd + 0x1DA7, 0x0A);
    vm.ds_write(bx_cmd + 0x1DA9, si_val);
    vm.ds_write(bx_cmd + 0x1DAB, di_val);
    vm.ds_write(bx_cmd + 0x1DAD, vm.ds_read(0x002A));  // word_2850A
    vm.ds_write(0x218F, bx_cmd + 8);
}

// 0x87 (sub_14abb): Indexed field (1 byte) conditional. ne → skip 2, eq → call-jump.
static void v2_vm_op_87(V2VM& vm) {
    uint16_t val = v2_vm_read_indexed_field(vm); // sub_15485: 1 byte
    if (val != v2_vm_accumulator) { vm.pc += 2; } else { v2_vm_do_call_jump(vm); }
}

// 0xC2 (sub_15268): Write acc to animation state table. 0 bytes.
// ds:[obj+0x16C5] * 14 + 0x25F6 = target address; write acc there.
static void v2_vm_op_C7(V2VM& vm) {
    uint16_t si = vm.global_r(0x42);
    uint16_t idx = vm.ds_read(si + 0x16C5);
    if (idx & 0x8000) return;
    uint16_t offset = idx * 0x0E;
    vm.ds_write(offset + 0x25F6, v2_vm_accumulator);
}

// 0x82 (sub_149ab): Signed >= comparison with indexed field (1 byte).
// If acc >= val (signed) → skip 2, else → jump. (acc < val → jump)
static void v2_vm_op_82(V2VM& vm) {
    uint16_t val = v2_vm_read_indexed_field(vm);
    if ((int16_t)v2_vm_accumulator >= (int16_t)val) {
        vm.pc += 2;
    } else {
        v2_vm_do_jump(vm);
    }
}

// 0x37 (sub_155c0): Collision check sub_155d6 (1 byte) + skip/call-jump.
// Like 0x1A but without PUSH/POP ds:0x372 (no temporary limit change).
static void v2_vm_op_37(V2VM& vm) {
    bool collision = v2_vm_collision_check_155d6(vm);
    vm.ds_write(0x38E, vm.ds_read(0x38E) + 2); // ALWAYS increment
    if (collision) { v2_vm_do_call_jump(vm); } else { vm.pc += 2; }
}

// 0x42 (sub_12669): Command buffer write type=2. 0 bytes.
static void v2_vm_op_42(V2VM& vm) {
    uint16_t bx_cmd = vm.ds_read(0x218F);
    vm.ds_write(bx_cmd + 0x1DA7, 2);
    vm.ds_write(0x218F, bx_cmd + 2);
}

// 0xD2 (sub_1287a): Password system setup. 0 bytes. Reads level password from table.
static void v2_vm_op_D2(V2VM& vm) {
    // si = word_2AA8D (current level index) = DS:0x25AD
    uint16_t si = vm.ds_read(0x25AD);
    si <<= 2; // * 4 (4 bytes per password entry)
    // Read 4 password characters from table at ds:[(uint16_t)(si - 0x7A5B + N)]
    uint8_t c0 = *(vm.ds + (uint16_t)(si - 0x7A5B)) & 0x7F;
    uint8_t c1 = *(vm.ds + (uint16_t)(si - 0x7A5A)) & 0x7F;
    uint8_t c2 = *(vm.ds + (uint16_t)(si - 0x7A59)) & 0x7F;
    uint8_t c3 = *(vm.ds + (uint16_t)(si - 0x7A58)) & 0x7F;
    // Write to password display words: word_287F0..287F6 = DS:0x0310..0x0316
    vm.ds_write(0x0310, c0);
    vm.ds_write(0x0312, c1);
    vm.ds_write(0x0314, c2);
    vm.ds_write(0x0316, c3);
}

// 0x25 (sub_14487): Animation load + off_30C8E dispatch. 1 byte + dispatch.
// 0x25 (sub_14487): test flag 0x40 → if NOT set: read filter, call sub_158b9, off_30C8E[2]
//                                     if SET: fall to loc_144bb (different path)
static void v2_vm_op_25(V2VM& vm) {
    uint16_t si = vm.global_r(0x42);
    bool flag40 = vm.ds_read(si + 0x1585) & 0x40;
    uint8_t anim_idx = vm.read_u8();
    uint16_t di = vm.global_r(0x42);
    if (!flag40) {
        v2_vm_sub_158b9(vm, anim_idx, di);
    } else {
        v2_vm_sub_158aa(vm, anim_idx, di);
    }
    // off_30C8E[si=2]: carry → skip 2, no carry → jump
    uint16_t cs_addr = *(uint16_t*)(vm.ds + 0x87AE + 2);
    if (cs_addr == 0x44F3) {
        if (vm.carry) { vm.pc += 2; } else { v2_vm_do_jump(vm); }
    } else if (cs_addr == 0x44E9) {
        if (!vm.carry) { vm.pc += 2; } else { v2_vm_do_jump(vm); }
    } else if (cs_addr == 0x42CF) {
        v2_vm_do_jump(vm);
    } else {
        v2_vm_runtime_dispatch(vm, 0x87AE, 2);
    }
}

// 0x4B (sub_16252): OR flag 0x2000 on object. 0 bytes.
// si=ds:0x42; ds:[si+0x1585] |= 0x2000
static void v2_vm_op_4B(V2VM& vm) {
    uint16_t si = vm.global_r(0x42);
    vm.ds_write(si + 0x1585, vm.ds_read(si + 0x1585) | 0x2000);
}

// 0x19 (sub_14428): Set anim state. 2 bytes.
// ax=es:[bx]; bx+=2; ds:[obj+0x1A0D]=ax; ds:[obj+0x1A35]=1
static void v2_vm_op_19(V2VM& vm) {
    uint16_t val = vm.read_u16();
    vm.field_w(0x1A0D, val);
    vm.field_w(0x1A35, 1);
}

// Animation frame bytecode interpreter — replicates sub_13084.
// Reads animation commands from es:anim_bx via off_30BC6 dispatch (ds:0x86E6).
// Timer at ds:0x78 delays between frames.
// Debug ring buffer for anim cmd trace (last N commands before error)
struct AnimCmdTrace {
    uint8_t cmd;
    uint16_t handler;
    uint16_t bx_before;
    uint16_t bx_after;
};
static AnimCmdTrace v2_anim_trace[16];
static int v2_anim_trace_idx = 0;

static void v2_vm_run_anim_frame(V2VM& vm, uint16_t& anim_bx) {
    uint16_t timer = vm.ds_read(0x78);
    if (timer > 0) {
        vm.ds_write(0x78, timer - 1);
        if (timer - 1 > 0) return;
    }
    int max = 200;
    while (max-- > 0) {
        v2_v2_anim_cmd_count++;
        uint16_t bx_before = anim_bx;
        uint8_t cmd = vm.es[anim_bx++];
        uint16_t handler = *(uint16_t*)(vm.ds + 0x86E6 + cmd * 2);

        // Debug: catch invalid anim commands
        if (cmd > 0x1A) {
            static bool dbg = false;
            if (!dbg) {
                dbg = true;
                uint16_t di = vm.global_r(0x42);
                printf("V2-VM: BAD anim cmd 0x%02X (cs:0x%04X) at bx=0x%04X obj=%d di=%d\n",
                       cmd, handler, bx_before, vm.obj, di);
                printf("  es=%p cs_base=%p es-cs_base=0x%lX\n",
                       (void*)vm.es, (void*)vm.cs_base,
                       vm.es ? (long)(vm.es - vm.cs_base) : -1);
                printf("  ds:[di+0x1A0D]=0x%04X ds:[di+0x1355]=0x%04X ds:0x78=0x%04X ds:[di+0x1A35]=0x%04X\n",
                       vm.ds_read(di + 0x1A0D), vm.ds_read(di + 0x1355),
                       vm.ds_read(0x78), vm.ds_read(di + 0x1A35));
                printf("  bytes at es:bx-2..bx+4: %02X %02X [%02X] %02X %02X %02X %02X\n",
                       vm.es[bx_before-2], vm.es[bx_before-1],
                       vm.es[bx_before], vm.es[bx_before+1],
                       vm.es[bx_before+2], vm.es[bx_before+3], vm.es[bx_before+4]);
                printf("V2-VM: last anim cmds before error:\n");
                for (int i = 0; i < 16; i++) {
                    int j = (v2_anim_trace_idx + i) & 15;
                    AnimCmdTrace& t = v2_anim_trace[j];
                    if (t.handler == 0 && t.cmd == 0) continue;
                    printf("  cmd=0x%02X handler=0x%04X bx: 0x%04X -> 0x%04X (consumed %d)\n",
                           t.cmd, t.handler, t.bx_before, t.bx_after,
                           (int)t.bx_after - (int)t.bx_before);
                }
            }
        }

        if (!v2_vm_exec_anim_cmd(vm, handler, anim_bx)) {
            // Record trace entry
            AnimCmdTrace& t = v2_anim_trace[v2_anim_trace_idx & 15];
            t.cmd = cmd; t.handler = handler; t.bx_before = bx_before; t.bx_after = anim_bx;
            v2_anim_trace_idx++;
            return;
        }

        // Record trace entry
        AnimCmdTrace& t = v2_anim_trace[v2_anim_trace_idx & 15];
        t.cmd = cmd; t.handler = handler; t.bx_before = bx_before; t.bx_after = anim_bx;
        v2_anim_trace_idx++;
    }
}

// Execute a single anim cmd. Returns true = continue (next cmd), false = exit (end/delay).
static bool v2_vm_exec_anim_cmd(V2VM& vm, uint16_t handler, uint16_t& anim_bx) {
    uint8_t cmd = 0; // used by default case
    switch (handler) {
        case 0x34D3: // [3] sub_134d3: JUMP — bx = es:[bx]
            anim_bx = *(uint16_t*)(vm.es + anim_bx);
            return true;

        case 0x34CA: // [5] sub_134ca: LOOP START — save bx+2, jump
            vm.ds_write(0x7A, anim_bx + 2);
            anim_bx = *(uint16_t*)(vm.es + anim_bx);
            return true;

        case 0x34D7: // [6] sub_134d7: LOOP BACK — bx = ds:0x7A
            anim_bx = vm.ds_read(0x7A);
            return true;

        case 0x3674: // [4] sub_13674: SKIP 1 BYTE
            anim_bx += 1;
            return true;

        case 0x77B2: // [2] sub_177b2: PLAY SOUND — 2 bytes
            // Original: reads byte from anim data → sound slot index
            // Then calls sub_177bb(ax=slot) which dispatches AIL sound play:
            //   AIL: sub_176bd(si=slot, ax=seq, bx=ds:0x2E6B) → allocate + start XMIDI sequence
            // play_xmidi_external(sound_segment, size, sequence_id); — SDL sound, commented
            anim_bx += 2;
            return true;

        case 0x31A4: { // [8] sub_131a4: Set sub-sprite X absolute — 2 bytes PER sub-sprite (LOOPS!)
            uint16_t si = vm.ds_read(0x7C);
            uint16_t di = vm.global_r(0x42);
            uint16_t dx_base = vm.ds_read(di + 0x173D);
            uint16_t end_si = vm.ds_read(0x80);
            for (; (int16_t)si < (int16_t)end_si; si += 2) {
                int16_t off = *(int16_t*)(vm.es + anim_bx); anim_bx += 2;
                uint16_t x = (uint16_t)(dx_base + off);
                vm.ds_write(si + 0x64D, x);
                if (!(vm.ds_read(si + 0x44D) & 0x1000)) {
                    vm.ds_write(si + 0x0D4D, x);
                    vm.ds_write(si + 0x0F4D, x);
                    vm.ds_write(si + 0x44D, vm.ds_read(si + 0x44D) | 0x1000);
                }
                vm.ds_write(si + 0x114D, 0x202);
            }
            // After loop: hflip check → JMP loc_136FC if flag 0x40 set
            di = vm.global_r(0x42);
            if (vm.ds_read(di + 0x1585) & 0x40) {
                // loc_136FC: flip X for all sub-sprites using [1A85,1AAD) range
                if (vm.ds_read(di + 0x1AD5) != 0) {
                    uint16_t dx2 = vm.ds_read(di + 0x173D) * 2; // parent X * 2
                    uint16_t cx_end = vm.ds_read(di + 0x1AAD);
                    for (uint16_t d = vm.ds_read(di + 0x1A85); (int16_t)d < (int16_t)cx_end; d += 2) {
                        uint16_t ax_flip = dx2 - vm.ds_read(d + 0x64D) - vm.ds_read(d + 0x0C4D);
                        vm.ds_write(d + 0x64D, ax_flip);
                        vm.ds_write(d + 0x44D, vm.ds_read(d + 0x44D) | 0x200); // OR, not XOR!
                        vm.ds_write(d + 0x114D, 0x202);
                    }
                }
            }
            return true;
        }

        case 0x323D: { // [10] sub_1323d: Set sub-sprite Y absolute — 2 bytes PER sub-sprite (LOOPS!)
            uint16_t si = vm.ds_read(0x7C);
            uint16_t di = vm.global_r(0x42);
            uint16_t dx_base = vm.ds_read(di + 0x1765);
            uint16_t end_si = vm.ds_read(0x80);
            for (; (int16_t)si < (int16_t)end_si; si += 2) {
                int16_t off = *(int16_t*)(vm.es + anim_bx); anim_bx += 2;
                uint16_t y = (uint16_t)(dx_base + off);
                vm.ds_write(si + 0x74D, y);
                if (!(vm.ds_read(si + 0x44D) & 0x800)) {
                    vm.ds_write(si + 0x0E4D, y);
                    vm.ds_write(si + 0x104D, y);
                    vm.ds_write(si + 0x44D, vm.ds_read(si + 0x44D) | 0x800);
                }
                vm.ds_write(si + 0x114D, 0x202);
            }
            // After loop: vflip check using si (loop exit value, NOT ds:42h!)
            // Original: TEST [si+1585h], 80h; JZ ret; JMP loc_137B8
            // si at this point = end_si (post-loop). loc_137B8 uses si as object.
            if (vm.ds_read(si + 0x1585) & 0x80) {
                // loc_137B8: flip Y for all sub-sprites
                if (vm.ds_read(si + 0x1AD5) != 0) {
                    uint16_t dx2 = vm.ds_read(si + 0x1765) * 2;
                    uint16_t cx_end = vm.ds_read(si + 0x1AAD);
                    for (uint16_t d = vm.ds_read(si + 0x1A85); (int16_t)d < (int16_t)cx_end; d += 2) {
                        uint16_t ay = dx2 - vm.ds_read(d + 0x74D) - vm.ds_read(d + 0x0C4D);
                        vm.ds_write(d + 0x74D, ay);
                        vm.ds_write(d + 0x44D, vm.ds_read(d + 0x44D) | 0x400);
                        vm.ds_write(d + 0x114D, 0x202);
                    }
                }
            }
            return true;
        }

        case 0x3158: { // [7] sub_13158: X signed offset — 1 byte.
            // Masked: loop sub-sprites, ADD offset to each [si+64D].
            // Unmasked: ADD (offset & 0xFF) << 8 to OBJECT X velocity [di+1945].
            uint16_t si = vm.ds_read(0x7C);
            int8_t raw = (int8_t)vm.es[anim_bx++];
            int16_t off = raw;
            uint16_t di = vm.global_r(0x42);
            if (vm.ds_read(di + 0x1585) & 0x40) off = -off;
            uint16_t end_s = vm.ds_read(0x80);
            if (vm.ds_read(0x38C) != 0) {
                uint16_t dx = vm.ds_read(0x38C);
                for (; (int16_t)si < (int16_t)end_s; si += 2) {
                    if (vm.ds_read(si + 0x54D) & dx) {
                        vm.ds_write(si + 0x64D, vm.ds_read(si + 0x64D) + (uint16_t)off);
                        vm.ds_write(si + 0x114D, 0x202);
                    }
                }
            } else {
                uint16_t vel_add = ((uint16_t)(off & 0xFF)) << 8;
                vm.ds_write(di + 0x1945, vm.ds_read(di + 0x1945) + vel_add);
                vm.ds_write(si + 0x114D, 0x202);
            }
            return true;
        }

        case 0x31F1: { // [9] sub_131f1: Y signed offset — 1 byte.
            // Masked: loop sub-sprites, ADD signed offset to each [si+74D].
            // Unmasked: ADD (offset & 0xFF) << 8 to OBJECT Y velocity [di+196D].
            uint16_t si = vm.ds_read(0x7C);
            int8_t raw = (int8_t)vm.es[anim_bx++]; // CBW
            int16_t off = raw; // sign-extend to 16-bit
            uint16_t di = vm.global_r(0x42);
            if (vm.ds_read(di + 0x1585) & 0x80) off = -off; // vflip
            uint16_t end_s = vm.ds_read(0x80);
            if (vm.ds_read(0x38C) != 0) {
                // Masked: loop, ADD offset to matching sub-sprites' Y position
                uint16_t dx = vm.ds_read(0x38C);
                for (; (int16_t)si < (int16_t)end_s; si += 2) {
                    if (vm.ds_read(si + 0x54D) & dx) {
                        vm.ds_write(si + 0x74D, vm.ds_read(si + 0x74D) + (uint16_t)off);
                        vm.ds_write(si + 0x114D, 0x202);
                    }
                }
            } else {
                // Unmasked: add (off & 0xFF) << 8 to object Y velocity
                uint16_t vel_add = ((uint16_t)(off & 0xFF)) << 8;
                vm.ds_write(di + 0x196D, vm.ds_read(di + 0x196D) + vel_add);
                vm.ds_write(si + 0x114D, 0x202);
            }
            return true;
        }

        case 0x345E: { // [13] loc_1345e: Set mask — 1 byte → ds:0x38C
            uint8_t mask = vm.es[anim_bx++];
            vm.ds_write(0x38C, mask);
            return true;
        }

        case 0x3474: { // [15] loc_13474: Set delay timer — 1 byte → ds:0x78 (BYTE write!), EXIT
            uint8_t delay = vm.es[anim_bx++];
            vm.ds_write_b(0x78, delay);  // BYTE write, not word! Original: MOV byte [78h], al
            return false;
        }

        case 0x346D: // [14] loc_1346d: END FRAME — POP + RETN
            return false;

        case 0x3286: // [11] loc_13286: INT 3 (debug) — nop
            return true;

        case 0x30A2: { // [0] sub_130a2: Advance sprite data offset. 1 byte.
            // byte * 72 = byte*8 + byte*64 added to sub-sprite ds:[si+0x84D]
            uint8_t frm = vm.es[anim_bx++];
            uint16_t offset = (uint16_t)(frm * 72);
            uint16_t mask_val = vm.ds_read(0x38C);
            uint16_t si = vm.ds_read(0x7C);
            uint16_t end = vm.ds_read(0x80);
            if (mask_val != 0) {
                // Masked: only update matching sub-sprites
                for (; (int16_t)si < (int16_t)end; si += 2) {
                    if (vm.ds_read(si + 0x54D) & mask_val) {
                        vm.ds_write(si + 0x84D, vm.ds_read(si + 0x84D) + offset);
                        vm.ds_write(si + 0x114D, 0x202);
                    }
                }
            } else {
                // Unmasked: update all sub-sprites
                for (; (int16_t)si < (int16_t)end; si += 2) {
                    vm.ds_write(si + 0x84D, vm.ds_read(si + 0x84D) + offset);
                    vm.ds_write(si + 0x114D, 0x202);
                }
            }
            return true;
        }

        case 0x30EF: { // [1] sub_130ef: Advance sprite with conditional mask. 1 byte.
            // Same as 0x30A2 but different mask source: uses ds:[obj+0x1855] as cx
            // and checks ds:0x38C for mask filtering
            uint16_t si = vm.ds_read(0x7C);
            uint16_t di = vm.global_r(0x42);
            uint16_t cx = vm.ds_read(di + 0x1855);
            uint16_t mask_val = vm.ds_read(0x38C);
            uint16_t end = vm.ds_read(0x80);
            if (mask_val != 0) {
                // Masked path: read byte per matching sub-sprite
                uint16_t mdi = mask_val;
                for (; (int16_t)si < (int16_t)end; si += 2) {
                    if (vm.ds_read(si + 0x54D) & mdi) {
                        uint8_t frm = vm.es[anim_bx++];
                        uint16_t offset = (uint16_t)(frm * 72);
                        vm.ds_write(si + 0x84D, cx + offset);
                        vm.ds_write(si + 0x114D, 0x202);
                    }
                }
            } else {
                // Unmasked: 1 byte PER sub-sprite (each gets own frame byte!)
                for (; (int16_t)si < (int16_t)end; si += 2) {
                    uint8_t frm = vm.es[anim_bx++];
                    uint16_t offset = (uint16_t)(frm * 72);
                    vm.ds_write(si + 0x84D, cx + offset);
                    vm.ds_write(si + 0x114D, 0x202);
                }
            }
            return true;
        }

        case 0x34DC: { // [20] loc_134dc: Sprite data decompression. 1 byte.
            // Full decompression into v2 shadow sprite buffer.
            uint16_t si_s = vm.ds_read(0x7C);
            uint16_t obj_d = vm.global_r(0x42);
            uint8_t spr_idx = vm.es[anim_bx] & 0xFF;
            anim_bx += 1;

            // Skip if same sprite already loaded
            if (spr_idx == (uint8_t)vm.ds_read(obj_d + 0x191D))
                return true;
            vm.ds_write(obj_d + 0x191D, spr_idx);
            vm.ds_write(si_s + 0x114D, 0x202); // dirty

            // Source setup
            uint16_t src_seg_val = vm.ds_read(si_s + 0x0B4D);
            uint16_t src_base = vm.ds_read(si_s + 0x0A4D);
            uint8_t* src_seg_ptr = v2_resolve_segment(src_seg_val);
            uint16_t lookup = *(uint16_t*)(src_seg_ptr + (uint16_t)(spr_idx * 2 + src_base));
            uint8_t* src = src_seg_ptr + src_base + lookup;

            // Destination setup in v2 shadow sprite buffer
            uint16_t dst_seg = vm.ds_read(si_s + 0x94D);
            uint16_t dst_off = vm.ds_read(si_s + 0x84D) - 1;
            uint32_t dst_abs = ((uint32_t)dst_seg << 4) + dst_off;

            // Resolve destination to correct shadow buffer via v2_resolve_segment.
            // dst_seg may be the sprite segment (0x2E73) OR another segment (gs_tiledata etc.)
            uint8_t* dst_base = v2_resolve_segment(dst_seg);
            if (!dst_base) return true;
            uint8_t* dst = dst_base + dst_off;
            uint8_t flags = (0x70 & (uint8_t)vm.ds_read(si_s + 0x44D)) | 0x80;

            // Decompress 128 strips
            // Each strip: 1 mask byte + up to 8 data bytes
            // Mask bit=0 → write 0 (transparent)
            // Mask bit=1 (first after 0 or first): read source byte, split nibbles
            //   Write first nibble | flags, save second
            // Mask bit=1 (consecutive): write saved second nibble | flags
            for (int strip = 0; strip < 128; strip++) {
                uint8_t mask = *src++;
                *dst++ = mask;

                uint8_t ah_save = 0; // saved second nibble
                bool have_second = false; // have pending second nibble

                for (int bit = 7; bit >= 0; bit--) {
                    if (mask & (1 << bit)) {
                        if (!have_second) {
                            // Read new byte, split into two nibbles
                            uint8_t data = *src++;
                            // Original: SHL ax,4; SHR al,4; XCHG ah,al; OR ax,dx
                            // data=0xAB → ax=0x00AB → SHL4=0x0AB0 → SHR al,4=0x0A0B
                            // XCHG=0x0B0A → OR dx → ah=0x0B|flags, al=0x0A|flags
                            uint8_t lo = (data & 0x0F) | flags;
                            uint8_t hi = (data >> 4) | flags;
                            *dst++ = hi;
                            ah_save = lo;
                            have_second = true;
                        } else {
                            // Use saved second nibble
                            *dst++ = ah_save;
                            have_second = false;
                        }
                    } else {
                        *dst++ = 0;
                        // After a 0, the next 1 reads new byte (have_second stays as is)
                    }
                }
            }
            return true;
        }

        case 0x3288: { // [12] loc_13288: Set palette/layer bits (4-6) in sub-sprite flags.
            // Masked: test [di+54Dh] (di=cmd*2 from dispatch, fixed) as gate.
            //         If gate passes: read byte per matching si, set bits.
            // Unmasked: read 1 byte PER sub-sprite, set bits.
            // NOTE: di at entry = cmd byte * 2 = 12*2 = 24 (from dispatch loop).
            // But in replay, di is not passed. We need the dispatch di.
            // Actually: di is NOT used as sub-sprite index here. The masked TEST uses
            // the dispatch's di which equals cmd*2. For simplicity and correctness,
            // we use the mask check on EACH sub-sprite (matching original's intent).
            // UPDATE: re-reading original — masked path uses di from dispatch (cmd*2),
            // NOT iterating si. The test is a GATE on one fixed sub-sprite.
            // For unmasked: reads 1 byte per sub-sprite.
            uint16_t si = vm.ds_read(0x7C);
            uint16_t end = vm.ds_read(0x80);
            if (vm.ds_read(0x38C) != 0) {
                // Masked: same read-per-matching pattern but test is on [di+54D]
                // We don't have dispatch's di here. Approximate with iterating mask check.
                uint16_t dx = vm.ds_read(0x38C);
                for (; (int16_t)si < (int16_t)end; si += 2) {
                    if (vm.ds_read(si + 0x54D) & dx) {
                        uint8_t val = vm.es[anim_bx++];
                        uint16_t bits = ((uint16_t)val << 3) & 0x70;
                        vm.ds_write(si + 0x44D, (vm.ds_read(si + 0x44D) & 0xFF8F) | bits);
                    }
                }
            } else {
                // Unmasked: 1 byte PER sub-sprite (NO dirty write — original has none)
                for (; (int16_t)si < (int16_t)end; si += 2) {
                    uint8_t val = vm.es[anim_bx++];
                    uint16_t bits = ((uint16_t)val << 3) & 0x70;
                    vm.ds_write(si + 0x44D, (vm.ds_read(si + 0x44D) & 0xFF8F) | bits);
                }
            }
            return true;
        }

        case 0x32DF: { // [21] sub_132df: Sprite type/data setup. 1 byte.
            // Original: di=ds:0x7C; si=byte*2; ax=cs:[si+0x32D7];
            // if si==0: si=1; byte_1339e=0; if si>4: si=2, byte_1339e=0xFF
            // Loop sub-sprites: set type (ds:[di+0x44D] & 0xFFF8 | si/2)
            // set height (ds:[di+0xC4D] = ax)
            // if byte_1339e: reset data ptr (ds:[di+0x84D] = ds:[di+0xA4D]+1)
            uint16_t di_s = vm.ds_read(0x7C);
            uint8_t type_byte = vm.es[anim_bx++] & 0xFF;
            uint16_t si_v = type_byte * 2;

            // Lookup height from cs:[si+0x32D7]
            uint16_t ax_height = *(uint16_t*)(vm.cs_base + 0x32D7 + si_v);

            // Original: if si==0 → si=1. if si>4 → si=2, byte_1339e=0xFF
            if (si_v == 0) si_v = 1;
            bool reset_data = false;
            if ((int16_t)si_v > 4) {
                si_v = 2;
                reset_data = true;
            }
            uint16_t type_val = si_v; // OR directly with si (NOT si/2!)

            uint16_t mask_v = vm.ds_read(0x38C);
            uint16_t end_di = vm.ds_read(0x80);

            if (mask_v != 0) {
                // Masked: only matching sub-sprites
                for (; (int16_t)di_s < (int16_t)end_di; di_s += 2) {
                    if (!(vm.ds_read(di_s + 0x54D) & mask_v)) continue;
                    vm.ds_write(di_s + 0x0C4D, ax_height);
                    vm.ds_write(di_s + 0x44D, (vm.ds_read(di_s + 0x44D) & 0xFFF8) | type_val);
                    vm.ds_write(di_s + 0x114D, 0x202);
                    if (reset_data) {
                        uint16_t d = vm.ds_read(di_s + 0x0A4D) + 1;
                        vm.ds_write(di_s + 0x84D, d);
                        vm.ds_write(vm.global_r(0x42) + 0x1855, d);
                        vm.ds_write(di_s + 0x94D, vm.ds_read(di_s + 0x0B4D));
                    }
                }
            } else {
                // Unmasked: all sub-sprites
                // First sub-sprite: if reset_data, set data ptr
                if (reset_data) {
                    uint16_t d = vm.ds_read(di_s + 0x0A4D) + 1;
                    vm.ds_write(di_s + 0x84D, d);
                    vm.ds_write(vm.global_r(0x42) + 0x1855, d);
                    vm.ds_write(di_s + 0x94D, vm.ds_read(di_s + 0x0B4D));
                }
                for (uint16_t d = di_s; (int16_t)d < (int16_t)end_di; d += 2) {
                    vm.ds_write(d + 0x0C4D, ax_height);
                    vm.ds_write(d + 0x44D, (vm.ds_read(d + 0x44D) & 0xFFF8) | type_val);
                    vm.ds_write(d + 0x114D, 0x202);
                }
            }
            return true;
        }

        case 0x3480: // [16] XOR flags 0x200 on sub-sprites. 0 bytes.
        case 0x3485: // [17] XOR flags 0x400
        case 0x348A: { // [18] XOR flags 0x600
            // Shared handler: ax = 0x200/0x400/0x600 depending on entry
            uint16_t xor_val = (handler == 0x3480) ? 0x200 : (handler == 0x3485) ? 0x400 : 0x600;
            uint16_t si_f = vm.ds_read(0x7C);
            uint16_t mask_f = vm.ds_read(0x38C);
            uint16_t end_f = vm.ds_read(0x80);
            for (; (int16_t)si_f < (int16_t)end_f; si_f += 2) {
                if (mask_f != 0 && !(vm.ds_read(si_f + 0x54D) & mask_f)) continue;
                vm.ds_write(si_f + 0x44D, vm.ds_read(si_f + 0x44D) ^ xor_val);
                vm.ds_write(si_f + 0x114D, 0x202);
            }
            return true;
        }

        case 0x341F: { // [19] Set sub-sprite mask (ds:[si+0x54D]). Variable bytes.
            // Reads 1 byte per sub-sprite (masked or all)
            uint16_t si_m = vm.ds_read(0x7C);
            uint16_t mask_m = vm.ds_read(0x38C);
            uint16_t end_m = vm.ds_read(0x80);
            if (mask_m != 0) {
                for (; (int16_t)si_m < (int16_t)end_m; si_m += 2) {
                    if (vm.ds_read(si_m + 0x54D) & mask_m) {
                        uint8_t val = vm.es[anim_bx++] & 0xFF;
                        vm.ds_write(si_m + 0x54D, val);
                    }
                }
            } else {
                for (; (int16_t)si_m < (int16_t)end_m; si_m += 2) {
                    uint8_t val = vm.es[anim_bx++] & 0xFF;
                    vm.ds_write(si_m + 0x54D, val);
                }
            }
            return true;
        }

        case 0x346F: // [26] End animation — bx=0xFFFF, exit
            anim_bx = 0xFFFF;
            return false;


        case 0x356E: { // [23] loc_1356e: Sprite resource lookup. 2 bytes consumed.
            // Search resource table, set sprite data pointers for all sub-sprites.
            uint16_t si_r = vm.ds_read(0x7C);
            uint16_t res_id = *(uint16_t*)(vm.es + anim_bx); anim_bx += 2;
            // Search ds:[di+0x124D] table (same as sub_12F82 but different base)
            uint16_t di_r = 0;
            for (; di_r < 0x40; di_r += 2) {
                if (vm.ds_read(di_r + 0x124D) == res_id) break;
            }
            if (di_r >= 0x40) di_r = 0; // not found → use entry 0
            // Get resource info
            uint16_t base_off = vm.ds_read(di_r + 0x126D);
            uint16_t base_seg = vm.ds_read(di_r + 0x128D);
            uint16_t sprite_seg = vm.ds_read(0x2E5D);
            // Loop sub-sprites
            uint16_t end_r = vm.ds_read(0x80);
            for (; (int16_t)si_r < (int16_t)end_r; si_r += 2) {
                vm.ds_write(si_r + 0x0A4D, base_off);
                vm.ds_write(si_r + 0x0B4D, base_seg);
                vm.ds_write(si_r + 0x94D, sprite_seg);
                // Data ptr: bp = ds:[(si-0x30)-0x78E4] — addr typically outside shadow, use real DS
                uint16_t bp_addr = (uint16_t)((si_r - 0x30) - 0x78E4);
                // vm.ds points to either real DS or ds_before snapshot (in replay).
                // For addresses outside shadow range, MUST read from real DS.
                // Use ds_read which handles shadow/real split correctly.
                uint16_t data_ptr = *(uint16_t*)(vm.ds + bp_addr);
                vm.ds_write(si_r + 0x84D, data_ptr);
                vm.ds_write(si_r + 0x44D, vm.ds_read(si_r + 0x44D) | 0x0A);
                vm.ds_write_b(si_r + 0x114D, 2);
            }
            // After loop: reset sprite dedup index
            uint16_t di_obj = vm.global_r(0x42);
            vm.ds_write(di_obj + 0x191D, 0xFFFF);
            return true;
        }

        case 0x339F: { // [24] OR flag 0x4000 + set dirty byte. 0 bytes.
            uint16_t si_o = vm.ds_read(0x7C);
            uint16_t mask_o = vm.ds_read(0x38C);
            uint16_t end_o = vm.ds_read(0x80);
            for (; (int16_t)si_o < (int16_t)end_o; si_o += 2) {
                if (mask_o != 0 && !(vm.ds_read(si_o + 0x54D) & mask_o)) continue;
                vm.ds_write(si_o + 0x44D, vm.ds_read(si_o + 0x44D) | 0x4000);
                vm.ds_write_b(si_o + 0x114E, 2);
            }
            return true;
        }

        case 0x33DE: { // [25] AND flags 0x9FFF (clear bits 13-14) + set dirty. 0 bytes.
            uint16_t si_a = vm.ds_read(0x7C);
            uint16_t mask_a = vm.ds_read(0x38C);
            uint16_t end_a = vm.ds_read(0x80);
            for (; (int16_t)si_a < (int16_t)end_a; si_a += 2) {
                if (mask_a != 0 && !(vm.ds_read(si_a + 0x54D) & mask_a)) continue;
                vm.ds_write(si_a + 0x44D, vm.ds_read(si_a + 0x44D) & 0x9FFF);
                vm.ds_write(si_a + 0x114D, 2);
            }
            return true;
        }

        default: {
            fprintf(stderr, "FATAL: unimplemented anim cmd 0x%02X (handler=0x%04X) obj=%d pc=%04X\n",
                cmd, handler, vm.obj, vm.pc);
            exit(1);
        }
    } // end switch
    return true; // default: continue
}

// sub_1303a: setup globals → run anim interpreter → copy results back
static void v2_vm_sub_1303a(V2VM& vm) {
    uint16_t di = vm.global_r(0x42);
    uint16_t anim_bx = vm.ds_read(di + 0x1A0D);
    if (anim_bx == 0xFFFF) return;

    // Debug: log anim frame entry for problematic cases
    static bool anim_debug = false;
    if (!anim_debug && vm.obj == 0) {
        uint8_t first_byte = vm.es[anim_bx];
        if (first_byte > 0x1A) {
            anim_debug = true;
            printf("V2-VM: anim frame entry: obj=%d anim_bx=0x%04X first_byte=0x%02X timer(0x78)=0x%04X timer_saved(0x1A35)=0x%04X\n",
                   vm.obj, anim_bx, first_byte, vm.ds_read(0x78), vm.ds_read(di + 0x1A35));
            printf("  es=%p code_seg=0x%04X\n", (void*)vm.es,
                   vm.ds_read(di + 0x1355));
        }
    }

    vm.ds_write(0x78, vm.ds_read(di + 0x1A35));
    vm.ds_write(0x7A, vm.ds_read(di + 0x1A5D));
    vm.ds_write(0x7C, vm.ds_read(di + 0x1A85));
    vm.ds_write(0x80, vm.ds_read(di + 0x1AAD));
    vm.ds_write(0x38C, 0);

    v2_vm_run_anim_frame(vm, anim_bx);

    vm.ds_write(di + 0x1A0D, anim_bx);
    vm.ds_write(di + 0x1A35, vm.ds_read(0x78));
    vm.ds_write(di + 0x1A5D, vm.ds_read(0x7A));
}

// 0x2F (sub_13031): Animation update. 0 bytes. PUSH bx, calls, POP bx.
// sub_135cf: velocity/bounds update after animation frame.
// Clamps velocity to max values, applies flip, adds to velocity accumulators.
static void v2_vm_sub_135cf(V2VM& vm) {
    uint16_t di = vm.global_r(0x42);
    // Check if update needed
    if (vm.ds_read(di + 0x141D) != 0xFFFF) return;

    // Add scroll delta if flags set
    if (vm.ds_read(di + 0x1585) & 0x8000) {
        vm.ds_write(di + 0x1675, vm.ds_read(di + 0x1675) + vm.ds_read(0x25B5));
    }
    if (vm.ds_read(di + 0x1585) & 0x4000) {
        vm.ds_write(di + 0x164D, vm.ds_read(di + 0x164D) + vm.ds_read(0x25B3));
    }

    // Clamp X velocity (164D) to [-178D, +178D]
    int16_t vx = (int16_t)vm.ds_read(di + 0x164D);
    int16_t max_vx = (int16_t)vm.ds_read(di + 0x178D);
    if (vx >= 0) {
        if (vx >= max_vx) vx = max_vx;
    } else {
        if (-vx >= max_vx) vx = -max_vx;
    }
    vm.ds_write(di + 0x164D, (uint16_t)vx);

    // Clamp Y velocity (1675) to [-17B5, +17B5]
    int16_t vy = (int16_t)vm.ds_read(di + 0x1675);
    int16_t max_vy = (int16_t)vm.ds_read(di + 0x17B5);
    if (vy >= 0) {
        if (vy >= max_vy) vy = max_vy;
    } else {
        if (-vy >= max_vy) vy = -max_vy;
    }
    vm.ds_write(di + 0x1675, (uint16_t)vy);

    // Apply velocity to accumulators with flip
    int16_t dx_acc = (vm.ds_read(di + 0x1585) & 0x40) ? -vx : vx;
    vm.ds_write(di + 0x1945, vm.ds_read(di + 0x1945) + (uint16_t)dx_acc);

    int16_t dy_acc = (vm.ds_read(di + 0x1585) & 0x80) ? -vy : vy;
    vm.ds_write(di + 0x196D, vm.ds_read(di + 0x196D) + (uint16_t)dy_acc);
}

static void v2_vm_op_2F(V2VM& vm) {
    v2_vm_sub_1303a(vm);
    v2_vm_sub_135cf(vm);
}

// 0x46 (sub_1268d): Command buffer write type=6 with param. 2 bytes.
// word_2A66F is at DS:0x218F (NOT 0xA66F — that was the linear address).
static void v2_vm_op_46(V2VM& vm) {
    uint16_t param = vm.read_u16();
    uint16_t bx_cmd = vm.ds_read(0x218F);
    vm.ds_write(bx_cmd + 0x1DA7, 6);
    vm.ds_write(bx_cmd + 0x1DA9, param);
    vm.ds_write(0x218F, bx_cmd + 4);
}

// 0x96 (sub_14675): Store acc to obj field 0x1995. 0 bytes.
static void v2_vm_op_96(V2VM& vm) {
    vm.field_w(0x1995, v2_vm_accumulator);
}

// 0x56 (sub_14686): Store acc to indexed field. 1 byte.
static void v2_vm_op_56(V2VM& vm) {
    uint16_t idx = vm.read_u8();
    uint16_t lookup = (uint16_t)(idx - 0x6CBA);
    uint16_t si = *(uint16_t*)(vm.ds + lookup);
    si += vm.global_r(0x42);
    vm.ds_write((uint16_t)(si + 0x14E5), v2_vm_accumulator);
}

// 0x58 (sub_146b4): Store acc to indexed+0x1995 field. 1 byte.
static void v2_vm_op_58(V2VM& vm) {
    uint16_t idx = vm.read_u8();
    uint16_t lookup = (uint16_t)(idx - 0x6CBA);
    uint16_t di = *(uint16_t*)(vm.ds + lookup);
    di += vm.ds_read(vm.global_r(0x42) + 0x1995);
    vm.ds_write((uint16_t)(di + 0x14E5), v2_vm_accumulator);
}

// 0x5A (sub_14704): Add acc to value at address. 2 bytes.
static void v2_vm_op_5A(V2VM& vm) {
    uint16_t addr = vm.read_u16();
    uint16_t val = vm.ds_read(addr);
    vm.ds_write(addr, val + v2_vm_accumulator);
}

// 0x60 (sub_147bf): AND value at address with acc. 2 bytes.
static void v2_vm_op_60(V2VM& vm) {
    uint16_t addr = vm.read_u16();
    vm.ds_write(addr, vm.ds_read(addr) & v2_vm_accumulator);
}

// 0x62 (sub_147e7): OR indexed field with acc. 1 byte.
static void v2_vm_op_62(V2VM& vm) {
    uint16_t idx = vm.read_u8();
    uint16_t lookup = (uint16_t)(idx - 0x6CBA);
    uint16_t si = *(uint16_t*)(vm.ds + lookup);
    si += vm.global_r(0x42);
    uint16_t addr = (uint16_t)(si + 0x14E5);
    vm.ds_write(addr, vm.ds_read(addr) | v2_vm_accumulator);
}

// 0x67 (sub_1484b): AND indexed+0x1995 field with acc. 1 byte.
static void v2_vm_op_67(V2VM& vm) {
    uint16_t idx = vm.read_u8();
    uint16_t lookup = (uint16_t)(idx - 0x6CBA);
    uint16_t di = *(uint16_t*)(vm.ds + lookup);
    di += vm.ds_read(vm.global_r(0x42) + 0x1995);
    uint16_t addr = (uint16_t)(di + 0x14E5);
    vm.ds_write(addr, vm.ds_read(addr) & v2_vm_accumulator);
}

// 0x18 (sub_14409): Set velocity + parent ref. 2 bytes (two signed bytes).
// ds:[obj+0x141D]=ds:[obj+0x1995]; vx=CBW(byte); ds:[obj+0x1945]=vx; vy=CBW(byte); ds:[obj+0x196D]=vy
static void v2_vm_op_18(V2VM& vm) {
    vm.field_w(0x141D, vm.field_r(0x1995));
    int8_t vx = (int8_t)vm.read_u8();
    vm.field_w(0x1945, (uint16_t)(int16_t)vx);
    int8_t vy = (int8_t)vm.read_u8();
    vm.field_w(0x196D, (uint16_t)(int16_t)vy);
}

// 0x9A (sub_14b60→sub_15445): Bit test with indexed field + 0x1995. 2 bytes consumed. Sets acc.
// Reads: byte idx1 (bitmask), byte idx2 (field index)
// di = table[idx2] + ds:[obj+0x1995]; val = ds:[di+0x14E5]; mask = ds:[idx1-0x6C34]
// acc = (val & mask) ? 1 : 0
static void v2_vm_op_bit_test_indexed(V2VM& vm) {
    uint8_t idx1 = vm.read_u8();  // bitmask index
    uint8_t idx2 = vm.read_u8();  // field index
    uint16_t lookup = (uint16_t)(idx2 - 0x6CBA);
    uint16_t di = *(uint16_t*)(vm.ds + lookup);
    uint16_t obj = vm.global_r(0x42);
    di += *(uint16_t*)(vm.ds + obj + 0x1995);
    uint16_t val = *(uint16_t*)(vm.ds + (uint16_t)(di + 0x14E5));
    uint16_t mask = *(uint16_t*)(vm.ds + (uint16_t)(idx1 - 0x6C34));
    v2_vm_accumulator = (val & mask) ? 1 : 0;
}

// 0x99 (sub_14b59→sub_1542a): Bit test. 3 bytes consumed (1+2). Sets acc to 0 or 1.
// Original: read byte idx, read word addr, val=ds:[addr], mask=ds:[(idx-0x6C34)], acc = (val&mask)?1:0
// sub_14b4b calls sub_153ea:
// ax = es:[bx]; INC bx; AND ax,0xFF → idx
// PUSH ax; ax = es:[bx]; ADD bx,2 → val (WORD from bytecode, NOT ds:[addr]!)
// POP si=idx; AND ax, ds:[si-0x6C34] → bit test of bytecode value
// JZ → ax=0; else ax=1; MOV ds:8A, ax
static void v2_vm_op_bit_test(V2VM& vm) {
    uint8_t idx = vm.read_u8();          // 1 byte consumed
    uint16_t val = vm.read_u16();        // 2 bytes consumed — VALUE from bytecode!
    uint16_t mask = *(uint16_t*)(vm.ds + (uint16_t)(idx - 0x6C34));
    v2_vm_accumulator = (val & mask) ? 1 : 0;
}

// 0x99 (sub_14b59→sub_1542a): bit test of ds:[addr]. 3 bytes.
// byte idx + word addr → val=ds:[addr], mask=ds:[idx-0x6C34], acc=(val&mask)?1:0
static void v2_vm_op_bit_test_addr(V2VM& vm) {
    uint8_t idx = vm.read_u8();
    uint16_t addr = vm.read_u16();
    uint16_t val = vm.ds_read(addr);  // dereference: read from DS at addr
    uint16_t mask = *(uint16_t*)(vm.ds + (uint16_t)(idx - 0x6C34));
    v2_vm_accumulator = (val & mask) ? 1 : 0;

}

// 0x54 (sub_14652): Load accumulator from indexed field + ds:[obj+0x1995] offset. 1 byte.
// Like sub_15485 but adds ds:[obj+0x1995] to the table lookup result.
static void v2_vm_op_load_acc_indexed_1995(V2VM& vm) {
    uint16_t idx = vm.read_u8();
    uint16_t lookup_addr = (uint16_t)(idx - 0x6CBA);
    uint16_t di = *(uint16_t*)(vm.ds + lookup_addr);
    uint16_t obj = vm.global_r(0x42);
    di += *(uint16_t*)(vm.ds + obj + 0x1995);
    v2_vm_accumulator = *(uint16_t*)(vm.ds + (uint16_t)(di + 0x14E5));
}

// 0x57 (sub_146a3): Store acc to address. 2 bytes.
// si=es:[bx]; bx+=2; ax=ds:0x8A; ds:[si]=ax
static void v2_vm_op_57(V2VM& vm) {
    uint16_t addr = vm.read_u16();
    vm.ds_write(addr, v2_vm_accumulator);
}

// 0xBD (sub_1469e): SHL acc << 8, then store (falls through to 0x57).
// SHL ds:0x8A, 8; then si=es:[bx]; bx+=2; ds:[si]=ds:0x8A
static void v2_vm_op_BD(V2VM& vm) {
    v2_vm_accumulator <<= 8;
    uint16_t addr = vm.read_u16();
    vm.ds_write(addr, v2_vm_accumulator);
}

// ============================================================================
// Value-reading helpers (replicate sub_1547e, sub_15485, sub_1549a)
// ============================================================================

// sub_1547e: read literal 16-bit value from bytecode (consumes 2 bytes)
static uint16_t v2_vm_read_literal(V2VM& vm) {
    return vm.read_u16();
}

// sub_15485: read 1-byte index → table lookup → add obj → read field (consumes 1 byte)
// Original:
//   si = es:[bx] & 0xFF; bx++
//   si = ds:[(uint16_t)(si - 0x6CBA)]   ← table lookup (outside shadow range, use real DS)
//   si += ds:0x42                         ← add current object
//   ax = ds:[si + 0x14E5]                 ← read field value (in shadow range)
static uint16_t v2_vm_read_indexed_field(V2VM& vm) {
    uint16_t idx = vm.read_u8();
    uint16_t lookup_addr = (uint16_t)(idx - 0x6CBA); // 16-bit wrap — outside shadow range
    uint16_t si = *(uint16_t*)(vm.ds + lookup_addr);
    si += vm.global_r(0x42);
    return vm.ds_read((uint16_t)(si + 0x14E5));
}

// sub_1549a: read 16-bit address from bytecode, dereference (consumes 2 bytes)
static uint16_t v2_vm_read_indirect(V2VM& vm) {
    uint16_t addr = vm.read_u16();
    return vm.ds_read(addr);
}

// sub_154a3: indexed field + 0x1995, consumes 1 byte
// Original: si = es:[bx] & 0xFF; bx++; di = ds:[si-0x6CBA]; si = ds:0x42;
//           di += ds:[si+0x1995]; ax = ds:[di+0x14E5]
static uint16_t v2_vm_read_indexed_field_1995(V2VM& vm) {
    uint16_t idx = vm.read_u8();
    uint16_t di = *(uint16_t*)(vm.ds + (uint16_t)(idx - 0x6CBA));
    uint16_t si = vm.global_r(0x42);
    di += vm.ds_read(si + 0x1995);
    return vm.ds_read(di + 0x14E5);
}

// sub_12312: pseudo-random number generator, consumes 0 bytes
// Original: if word_288AC != 0 → return word_288AC
// else: EAX = dword_30B19 * 0x15A4E35 + 1; store; return (AX >> 8) & 0x7FFF
// sub_12312: exact replica using shadow DS for state.
// dword_30B19 (LCG seed) at DS:0x8639, word_288AC at DS:0x03CC, word_28832 at DS:0x0352.
static uint16_t v2_vm_read_random(V2VM& vm) {
    uint16_t mode_flag = vm.ds_read(0x03CC); // word_288AC
    if (mode_flag != 0) {
        // Path 2: LFSR — xchg ah,al; word_28832 = ax; RCL ax,3; word_28832 ^= ax
        uint16_t val = vm.ds_read(0x0352);    // word_28832
        val = (uint16_t)((val >> 8) | (val << 8)); // xchg ah, al
        vm.ds_write(0x0352, val);
        // RCL ax, 3: rotate left through carry 3 times. Carry starts at 0 for v2.
        // Approximate: ROL (close enough for non-crypto PRNG)
        uint16_t rotated = (val << 3) | (val >> 13);
        vm.ds_write(0x0352, vm.ds_read(0x0352) ^ rotated);
        return vm.ds_read(0x0352);
    }
    // Path 1: LCG — eax = eax * 0x15A4E35 + 1; return ROR(eax, 16)
    uint32_t seed = *(uint32_t*)(vm.shadow + 0x8639); // dword_30B19
    seed = seed * 0x15A4E35 + 1;
    *(uint32_t*)(vm.shadow + 0x8639) = seed;
    uint32_t rot = (seed >> 16) | (seed << 16); // ROR eax, 16
    return (uint16_t)rot;
}

// ============================================================================
// off_30C98 unified dispatcher (sub_15473 / sub_15470)
// Reads a value from bytecode using one of 5 methods based on (mode & 7).
// sub_15473: dispatch on (ax & 7)
// sub_15470: SHR ax,3 then dispatch on (ax & 7) — used for second value
// ============================================================================
static uint16_t v2_vm_dispatch_30C98(V2VM& vm, uint8_t mode) {
    switch (mode & 7) {
    case 0: return v2_vm_read_literal(vm);           // sub_1547e: 2 bytes
    case 1: return v2_vm_read_indexed_field(vm);     // sub_15485: 1 byte
    case 2: return v2_vm_read_indirect(vm);          // sub_1549a: 2 bytes
    case 3: return v2_vm_read_indexed_field_1995(vm);// sub_154a3: 1 byte
    case 4: return v2_vm_read_random(vm);            // sub_12312: 0 bytes
    default: return 0; // entries 5-7 not used
    }
}

// ============================================================================
// sub_1250b: read mode byte + first dispatch
// Reads 1 byte (mode), dispatches (mode & 7) through off_30C98.
// Saves mode to word_28512. Returns the dispatch result.
// The mode byte is also used later by sub_12543 for second dispatch.
// ============================================================================
static uint16_t v2_vm_sub_1250b(V2VM& vm, uint8_t& out_mode) {
    // ax = es:[bx]; INC bx
    uint16_t word = *(uint16_t*)(vm.es + vm.pc);
    vm.pc += 1;
    out_mode = (uint8_t)(word & 0xFF);
    // word_28512 (DS:0x0032) = mode word
    vm.ds_write(0x0032, word);
    // sub_15473: dispatch (word & 7)
    uint16_t result = v2_vm_dispatch_30C98(vm, out_mode);
    // sub_12515: ax = result * 2; read word from seg001:[ax] → word_2850A (DS:0x002A)
    uint16_t seg001_off = result * 2;
    uint16_t text_ptr = *(uint16_t*)(v2_m2c_base + 0x9480 + seg001_off);
    vm.ds_write(0x002A, text_ptr);
    return result;
}

// ============================================================================
// Full opcode 0x41 (sub_1242e): Text/dialog display
// Reads: 1 byte mode + dispatch1 + dispatch2 + sub_125a3(1 byte + dispatch3 + dispatch4) + sub_12613(?)
// All bytecode reads are through off_30C98 dispatch.
// State changes: writes to command buffer at word_2A66F, text rendering globals.
// ============================================================================

// sub_125a3: reads 1 byte + 2 dispatches for X position
// sub_125a3: X/Y position from bytecode.
// Writes word_2854C (DS:0x006C). Returns si = word_2854C, di = Y result.
static void v2_vm_sub_125a3(V2VM& vm, uint16_t& out_si, uint16_t& out_di) {
    uint16_t word = *(uint16_t*)(vm.es + vm.pc);
    vm.pc += 1;
    uint8_t mode = (uint8_t)(word & 0xFF);

    // X computation: dispatch → add obj X → sub viewport X → shr 3 → sub word_28518 → clamp
    uint16_t x_raw = v2_vm_dispatch_30C98(vm, mode);
    uint16_t si_obj = vm.ds_read(0x42);  // word_28522 = ds:0x42 (current obj from sub_1250b context)
    int16_t ax = (int16_t)(x_raw + vm.ds_read(si_obj + 0x173D) - vm.ds_read(0x44));
    if (ax < 0) { ax = 2; }
    else {
        ax = (uint16_t)ax >> 3;
        ax -= (int16_t)vm.ds_read(0x38); // word_28518
        if (ax < 0) ax = 2;
        else if (ax < 2) ax = 2;
    }
    vm.ds_write(0x6C, (uint16_t)ax); // word_2854C

    // Y computation: dispatch → add obj Y → sub viewport Y → shr 3 → sub word_2851A → clamp
    uint16_t y_raw = v2_vm_dispatch_30C98(vm, mode >> 3);
    int16_t ay = (int16_t)(y_raw + vm.ds_read(si_obj + 0x1765) - vm.ds_read(0x46));
    if (ay < 0) { ay = 2; }
    else {
        ay = (uint16_t)ay >> 3;
        ay -= (int16_t)vm.ds_read(0x3A); // word_2851A
        if (ay < 0) ay = 2;
        else if (ay < 2) ay = 2;
    }
    out_si = vm.ds_read(0x6C); // word_2854C = X result
    out_di = (uint16_t)ay;      // di = Y result
}

// 0x41 (sub_1242e): Full text/dialog display opcode.
// Bytecode consumption: sub_1250b(1 + N1) + sub_12543(N2) + sub_125a3(1 + N3 + N4) = 2 + N1+N2+N3+N4
// Each Ni is determined by off_30C98 dispatch (0-2 bytes each).
// After all reads: writes to command buffer, text rendering. No further bytecode consumed.
static void v2_vm_op_41(V2VM& vm) {
    // sub_1242e: exact replica

    // 1. sub_1250b: read mode byte + first value dispatch → text index, sets word_2850A (0x2A)
    uint8_t mode1;
    v2_vm_sub_1250b(vm, mode1);

    // 2. sub_12529: read text dimensions from seg001 → word_28514 (0x34), word_28516 (0x36)
    {
        uint16_t text_ptr = vm.ds_read(0x2A); // word_2850A
        // seg001 base: 0x9480 from m2c_base
        uint8_t* seg001 = v2_m2c_base + 0x9480;
        uint16_t w = seg001[text_ptr];      // byte → word (ah=0)
        vm.ds_write(0x34, w);                // word_28514
        uint16_t h = seg001[text_ptr + 1];
        vm.ds_write(0x36, h);                // word_28516
    }

    // 3. sub_12543: second dispatch (mode1 >> 3)
    uint16_t val2 = v2_vm_dispatch_30C98(vm, mode1 >> 3);

    // 4. sub_12549: position computation from val2 (ax on entry)
    {
        uint16_t w14 = vm.ds_read(0x34); // word_28514
        uint16_t w16 = vm.ds_read(0x36); // word_28516
        uint16_t w38, w3A;
        if (val2 == 5) { w38 = w14 - 2; w3A = 0; }
        else if (val2 == 4) { w38 = 1; w3A = 0; }
        else if (val2 == 1) { w38 = 1; w3A = w16 - 1; }
        else if (val2 == 2) { w38 = w14 - 2; w3A = w16 - 1; }
        else if (val2 == 0 || val2 == 6) { w38 = w14 >> 1; w3A = w16 - 1; }
        else { w38 = w14 >> 1; w3A = 0; } // default (3, 7, etc.)
        vm.ds_write(0x38, w38); // word_28518
        vm.ds_write(0x3A, w3A); // word_2851A
    }

    // 5. sub_125a3: X/Y position from bytecode
    uint16_t si_pos, di_pos;
    v2_vm_sub_125a3(vm, si_pos, di_pos);

    // 6. sub_12613: text bounds clamping
    {
        uint16_t w14 = vm.ds_read(0x34);
        uint16_t w16 = vm.ds_read(0x36);
        si_pos += w14;
        if ((int16_t)si_pos >= 0x27) si_pos = 0x26;
        si_pos -= w14;
        di_pos += w16;
        if ((int16_t)di_pos >= 0x16) di_pos = 0x15;
        di_pos -= w16;
    }

    // 7. Command buffer write
    {
        uint16_t bx = vm.ds_read(0x218F); // word_2A66F
        vm.ds_write((uint16_t)(bx + 0x1DA7), 0);       // command type = 0
        vm.ds_write((uint16_t)(bx + 0x1DA9), si_pos);   // X
        vm.ds_write((uint16_t)(bx + 0x1DAB), di_pos);   // Y
        vm.ds_write((uint16_t)(bx + 0x1DAD), val2);     // text param (ax from sub_12549)
        vm.ds_write((uint16_t)(bx + 0x1DAF), vm.ds_read(0x2A)); // word_2850A
        vm.ds_write(0x218F, bx + 0xA);                  // word_2A66F += 0xA
    }
}

static void v2_vm_op_44(V2VM& vm) {
    // sub_1246d: exact replica — like 0x41 but uses sub_125fa instead of sub_125a3, no sub_12549

    // 1. sub_1250b
    uint8_t mode1;
    v2_vm_sub_1250b(vm, mode1);

    // 2. sub_12529
    {
        uint16_t text_ptr = vm.ds_read(0x2A);
        uint8_t* seg001 = v2_m2c_base + 0x9480;
        vm.ds_write(0x34, (uint16_t)seg001[text_ptr]);
        vm.ds_write(0x36, (uint16_t)seg001[text_ptr + 1]);
    }

    // 3. sub_12543: second dispatch
    uint16_t val2 = v2_vm_dispatch_30C98(vm, mode1 >> 3);

    // 4. sub_125fa: simple position dispatch — writes word_2854C (0x6C)
    uint16_t si_pos, di_pos;
    {
        uint16_t word = *(uint16_t*)(vm.es + vm.pc);
        vm.pc += 1;
        uint8_t mode = (uint8_t)(word & 0xFF);
        uint16_t x_val = v2_vm_dispatch_30C98(vm, mode);
        vm.ds_write(0x6C, x_val); // word_2854C
        uint16_t y_val = v2_vm_dispatch_30C98(vm, mode >> 3);
        si_pos = vm.ds_read(0x6C); // word_2854C
        di_pos = y_val;
    }

    // 5. sub_12613: text bounds clamping
    {
        uint16_t w14 = vm.ds_read(0x34);
        uint16_t w16 = vm.ds_read(0x36);
        si_pos += w14;
        if ((int16_t)si_pos >= 0x27) si_pos = 0x26;
        si_pos -= w14;
        di_pos += w16;
        if ((int16_t)di_pos >= 0x16) di_pos = 0x15;
        di_pos -= w16;
    }

    // 6. Command buffer write
    {
        uint16_t bx = vm.ds_read(0x218F);
        vm.ds_write((uint16_t)(bx + 0x1DA7), 0);
        vm.ds_write((uint16_t)(bx + 0x1DA9), si_pos);
        vm.ds_write((uint16_t)(bx + 0x1DAB), di_pos);
        vm.ds_write((uint16_t)(bx + 0x1DAD), val2);
        vm.ds_write((uint16_t)(bx + 0x1DAF), vm.ds_read(0x2A));
        vm.ds_write(0x218F, bx + 0xA);
    }
}

// ============================================================================
// Branch operations (replicate sub_142cf and sub_142c1)
// ============================================================================

// sub_142cf: simple jump — pc = es:[pc]
static void v2_vm_do_jump(V2VM& vm) {
    if (vm.pc >= 0xC000) { static int _h=0; _h++; if(_h<=5) fprintf(stderr,"V2-ES-JUMP: pc=%04X obj=%d\n",vm.pc,vm.obj); }
    vm.pc = *(uint16_t*)(vm.es + vm.pc);
}

// sub_142c1: ADD bx,2; si=ds:0x42; [si+0x137D]=bx; SUB bx,2; bx=es:[bx]
static void v2_vm_do_call_jump(V2VM& vm) {
    uint16_t si = vm.global_r(0x42);
    vm.ds_write(si + 0x137D, vm.pc + 2);  // save alt_pc to shadow DS
    vm.pc = *(uint16_t*)(vm.es + vm.pc);   // jump
}

// ============================================================================
// Conditional branch opcodes — CMP ax, accumulator
//
// Pattern A (JNZ + sub_142cf): if ax != acc → skip 2, else → jump
// Pattern B (JZ + sub_142cf):  if ax == acc → skip 2, else → jump
// Pattern C (JZ + sub_142c1):  if ax == acc → skip 2, else → call-jump
// ============================================================================

// --- Pattern A: if (val != acc) skip else jump ---
// 0x72: literal
static void v2_vm_op_72(V2VM& vm) {
    uint16_t val = v2_vm_read_literal(vm);
    if (val != v2_vm_accumulator) { vm.pc += 2; } else { v2_vm_do_jump(vm); }
}
// 0x73: indexed field
static void v2_vm_op_73(V2VM& vm) {
    uint16_t val = v2_vm_read_indexed_field(vm);
    if (val != v2_vm_accumulator) { vm.pc += 2; } else { v2_vm_do_jump(vm); }
}
// 0x74: indirect
static void v2_vm_op_74(V2VM& vm) {
    uint16_t val = v2_vm_read_indirect(vm);
    if (val != v2_vm_accumulator) { vm.pc += 2; } else { v2_vm_do_jump(vm); }
}

// --- 0x68: if (acc >= literal) jump, else skip 2 (JB = unsigned below) ---
static void v2_vm_op_68(V2VM& vm) {
    uint16_t val = v2_vm_read_literal(vm);
    if (v2_vm_accumulator >= val) { v2_vm_do_jump(vm); } else { vm.pc += 2; }
}

// --- Pattern B: if (val == acc) skip else jump ---
// 0x77: literal
static void v2_vm_op_77(V2VM& vm) {
    uint16_t val = v2_vm_read_literal(vm);
    if (val == v2_vm_accumulator) { vm.pc += 2; } else { v2_vm_do_jump(vm); }
}
// 0x78: indexed field
static void v2_vm_op_78(V2VM& vm) {
    uint16_t val = v2_vm_read_indexed_field(vm);
    if (val == v2_vm_accumulator) { vm.pc += 2; } else { v2_vm_do_jump(vm); }
}

// --- Pattern C: if (val == acc) skip else call-jump ---
// 0x8B: literal
static void v2_vm_op_8B(V2VM& vm) {
    uint16_t val = v2_vm_read_literal(vm);
    if (val == v2_vm_accumulator) { vm.pc += 2; } else { v2_vm_do_call_jump(vm); }
}
// 0x8C: indexed field
static void v2_vm_op_8C(V2VM& vm) {
    uint16_t val = v2_vm_read_indexed_field(vm);
    if (val == v2_vm_accumulator) { vm.pc += 2; } else { v2_vm_do_call_jump(vm); }
}

// --- 0x79: indirect, if eq skip else jump ---
static void v2_vm_op_79(V2VM& vm) {
    uint16_t val = v2_vm_read_indirect(vm);
    if (val == v2_vm_accumulator) { vm.pc += 2; } else { v2_vm_do_jump(vm); }
}

// --- 0x86: literal, if ne skip else call-jump ---
static void v2_vm_op_86(V2VM& vm) {
    uint16_t val = v2_vm_read_literal(vm);
    if (val != v2_vm_accumulator) { vm.pc += 2; } else { v2_vm_do_call_jump(vm); }
}

// --- 0x88: indirect, if ne skip else call-jump ---
static void v2_vm_op_88(V2VM& vm) {
    uint16_t val = v2_vm_read_indirect(vm);
    if (val != v2_vm_accumulator) { vm.pc += 2; } else { v2_vm_do_call_jump(vm); }
}

// sub_15403 helper: reads 2 bytes (two INC bx), pattern B (ds:0x42, NO 0x1995).
// idx1=mask index, idx2=field index. val = ds:[ds:[idx2-0x6CBA] + ds:0x42 + 0x14E5] & ds:[idx1-0x6C34].
static uint16_t v2_vm_read_indexed_field_15403(V2VM& vm) {
    uint8_t idx1 = vm.read_u8();
    uint8_t idx2 = vm.read_u8();
    uint16_t lookup = (uint16_t)(idx2 - 0x6CBA);
    uint16_t si = *(uint16_t*)(vm.ds + lookup);
    si += vm.global_r(0x42);
    uint16_t val = vm.ds_read((uint16_t)(si + 0x14E5));
    // AND with mask from idx1 table, return 0 or 1
    uint16_t mask = *(uint16_t*)(vm.ds + (uint16_t)(idx1 - 0x6C34));
    return (val & mask) ? 1 : 0;
}

// --- 0xA9: sub_15403 (2B indexed), if ne skip else jump ---
static void v2_vm_op_A9(V2VM& vm) {
    uint16_t val = v2_vm_read_indexed_field_15403(vm);
    if (val != v2_vm_accumulator) { vm.pc += 2; } else { v2_vm_do_jump(vm); }
}

// sub_15445 helper: reads 2 bytes (two INC bx), indexed + 0x1995
static uint16_t v2_vm_read_indexed_field_15445(V2VM& vm) {
    uint8_t idx1 = vm.read_u8();
    uint8_t idx2 = vm.read_u8();
    uint16_t lookup = (uint16_t)(idx2 - 0x6CBA);
    uint16_t di = *(uint16_t*)(vm.ds + lookup);
    uint16_t obj = vm.global_r(0x42);
    di += vm.ds_read(obj + 0x1995);
    uint16_t val = vm.ds_read((uint16_t)(di + 0x14E5));
    uint16_t mask = *(uint16_t*)(vm.ds + (uint16_t)(idx1 - 0x6C34));
    return (val & mask) ? 1 : 0;
}

// --- 0xAB: sub_15445 (2B indexed+1995), if ne skip else jump ---
static void v2_vm_op_AB(V2VM& vm) {
    uint16_t val = v2_vm_read_indexed_field_15445(vm);
    if (val != v2_vm_accumulator) { vm.pc += 2; } else { v2_vm_do_jump(vm); }
}

// ============================================================================
// Load accumulator opcodes — exact replicas
// ============================================================================

// 0x51: load literal word to accumulator
static void v2_vm_op_load_acc_literal(V2VM& vm) {
    v2_vm_accumulator = vm.read_u16();
}

// 0x52: load indexed field to accumulator
static void v2_vm_op_load_acc_indexed(V2VM& vm) {
    v2_vm_accumulator = v2_vm_read_indexed_field(vm);
}

// 0x53: load indirect to accumulator
static void v2_vm_op_load_acc_indirect(V2VM& vm) {
    v2_vm_accumulator = v2_vm_read_indirect(vm);
}

// ============================================================================
// Collision check helper — replicates sub_155d6 (for opcode 0x1A)
// Returns true if collision found (carry set in original).
// Always consumes 1 byte from bytecode.
// ============================================================================
// sub_155d6: Full collision check. ALL paths consume 1 byte.
// 3 paths based on ds:0x390:
//   state == 0 (loc_1566a): check collision BIT in [di+0x13F5] → STC if set
//   state > 0 (loc_15667): CLC always (no collision)
//   state < 0 (loc_155e5): bounding box check, SET collision bit, CLC always
static bool v2_vm_collision_check_155d6(V2VM& vm) {
    int16_t state = (int16_t)vm.global_r(0x390);
    uint8_t filter = vm.read_u8(); // ALL paths consume 1 byte

    uint16_t di = vm.global_r(0x42);

    if (state == 0) {
        // loc_1566a: check if collision bit was previously set
        uint16_t si_38e = vm.global_r(0x38E);
        uint16_t mask = *(uint16_t*)(vm.ds + (uint16_t)(si_38e - 0x6C34));
        uint16_t flags = vm.ds_read(di + 0x13F5);
        if (flags & mask) {
            // sub_16243: read collided object from table
            // di_idx = di * 16 + ds:0x38E; ax = ds:[di_idx + 0x1B25]
            uint16_t di_idx = (uint16_t)(di * 16 + vm.global_r(0x38E));
            uint16_t collided_obj = vm.ds_read(di_idx + 0x1B25);
            vm.ds_write(di + 0x1995, collided_obj);
            return true;  // STC → collision (caller does call-jump which saves 0x137D)
        }
        return false;  // CLC → no collision
    }

    if (state > 0) {
        // loc_15667: INC bx already done, CLC
        return false;
    }

    // state < 0 (loc_155e5): bounding box collision check
    // Read bounding box of current object
    uint16_t x_left  = vm.ds_read(di + 0x1535);  // ds:0x34
    uint16_t x_right = vm.ds_read(di + 0x155D);  // ds:0x36
    uint16_t y_top   = vm.ds_read(di + 0x14E5);  // ds:0x38
    uint16_t y_bot   = vm.ds_read(di + 0x150D);  // ds:0x3A

    uint16_t table_end = vm.global_r(0x372);
    for (uint16_t si = 0; (int16_t)si < (int16_t)table_end; si += 2) {
        if (vm.ds_read(si + 0x1355) == 0) continue;              // inactive
        if (vm.ds_read(si + 0x15FD) != filter) continue;         // wrong type
        if (si == di) continue;                                    // self
        // Bounding box overlap check (signed comparisons, exact original order)
        if ((int16_t)x_right < (int16_t)vm.ds_read(si + 0x1535)) continue;
        if ((int16_t)vm.ds_read(si + 0x155D) < (int16_t)x_left) continue;
        if ((int16_t)y_bot < (int16_t)vm.ds_read(si + 0x14E5)) continue;
        if ((int16_t)vm.ds_read(si + 0x150D) < (int16_t)y_top) continue;

        // Collision found! Set bit in [di+0x13F5]
        uint16_t si_38e = vm.global_r(0x38E);
        uint16_t mask = *(uint16_t*)(vm.ds + (uint16_t)(si_38e - 0x6C34));
        vm.ds_write(di + 0x13F5, vm.ds_read(di + 0x13F5) | mask);
        // sub_16235: store collided object in table
        // di_idx = di * 16 + ds:0x38E; ds:[di_idx + 0x1B25] = si
        {
            uint16_t di_idx = (uint16_t)(di * 16 + vm.global_r(0x38E));
            vm.ds_write(di_idx + 0x1B25, si);
        }
        return false;  // CLC — collision only recorded via bit
    }
    return false;  // CLC — no collision found
}

// sub_156c0: Full collision check. ALL paths consume 2 bytes.
// Same 2-phase system as sub_155d6 but:
// - Filter is a 2-byte word (not 1 byte)
// - Filter check: TEST [si+0x1625], dx (bit test, not equality)
static bool v2_vm_collision_check_156c0(V2VM& vm) {
    int16_t state = (int16_t)vm.global_r(0x390);
    uint16_t di = vm.global_r(0x42);

    if (state == 0) {
        // loc_15754: ADD bx,2; check bit in [di+0x13F5]
        vm.pc += 2;
        uint16_t si_38e = vm.global_r(0x38E);
        uint16_t mask = *(uint16_t*)(vm.ds + (uint16_t)(si_38e - 0x6C34));
        uint16_t flags = vm.ds_read(di + 0x13F5);
        if (flags & mask) {
            // sub_16243 → ds:[di+0x1995] = collided obj
            return true;  // STC
        }
        return false;  // CLC
    }

    if (state > 0) {
        // loc_1574f: ADD bx,2; CLC
        vm.pc += 2;
        return false;
    }

    // state < 0 (loc_156cf): full bounding box check, 2-byte filter
    uint16_t dx_filter = vm.read_u16();
    uint16_t x_left  = vm.ds_read(di + 0x1535);
    uint16_t x_right = vm.ds_read(di + 0x155D);
    uint16_t y_top   = vm.ds_read(di + 0x14E5);
    uint16_t y_bot   = vm.ds_read(di + 0x150D);

    uint16_t table_end = vm.global_r(0x372);
    for (uint16_t si = 0; (int16_t)si < (int16_t)table_end; si += 2) {
        if (vm.ds_read(si + 0x1355) == 0) continue;
        if (!(vm.ds_read(si + 0x1625) & dx_filter)) continue;  // TEST, not CMP
        if (si == di) continue;
        if ((int16_t)x_right < (int16_t)vm.ds_read(si + 0x1535)) continue;
        if ((int16_t)vm.ds_read(si + 0x155D) < (int16_t)x_left) continue;
        if ((int16_t)y_bot < (int16_t)vm.ds_read(si + 0x14E5)) continue;
        if ((int16_t)vm.ds_read(si + 0x150D) < (int16_t)y_top) continue;

        // Collision: set bit + CLC
        uint16_t si_38e = vm.global_r(0x38E);
        uint16_t mask = *(uint16_t*)(vm.ds + (uint16_t)(si_38e - 0x6C34));
        vm.ds_write(di + 0x13F5, vm.ds_read(di + 0x13F5) | mask);
        return false;  // CLC
    }
    return false;  // CLC
}

// sub_15788: Full collision check. ALL paths consume 1 byte.
// Same 2-phase system. Filter via 1-byte index.
// state > 0: calls sub_158f5 (search) + sub_15c37 (directional check). Complex.
// Phase 1 (state==0) and phase-skip (state<0) fully implemented.
// Phase 2 (state>0): full search + directional check → always returns CLC with collision bit setting.
static bool v2_vm_collision_check_15788(V2VM& vm) {
    int16_t state = (int16_t)vm.global_r(0x390);
    uint16_t di = vm.global_r(0x42);

    if (state == 0) {
        // loc_157c0: INC bx + check bit
        vm.pc += 1;
        uint16_t si_38e = vm.global_r(0x38E);
        uint16_t mask = *(uint16_t*)(vm.ds + (uint16_t)(si_38e - 0x6C34));
        if (vm.ds_read(di + 0x13F5) & mask) {
            return true;  // STC
        }
        return false;  // CLC
    }

    if (state < 0) {
        // loc_157bd: INC bx + CLC
        vm.pc += 1;
        return false;
    }

    // state > 0: read filter, call sub_158f5/sub_15c37, set collision bit.
    // This path ALWAYS returns CLC (false). Collision is recorded via [di+13F5] bits.
    uint8_t filter = vm.read_u8();
    di = vm.global_r(0x42);
    uint16_t filter_si = (uint16_t)filter; // si from bytecode
    bool found = false;

    // sub_158f5: X direction tile check
    {
        int16_t cur_x = (int16_t)vm.ds_read(di + 0x173D);
        int16_t old_x = (int16_t)vm.ds_read(di + 0x13A5);
        bool x_carry = false;
        uint16_t x_dir = 0;
        if (cur_x < old_x) {
            // Moved left: sub_159d3 = vertical tile scan at X_start
            x_carry = v2_vm_sub_159df_at(vm, filter_si, di, vm.ds_read(di + 0x1535));
            x_dir = 1;
        } else if (cur_x > old_x) {
            // Moved right: loc_159ec = vertical tile scan at X_end
            x_carry = v2_vm_sub_159df_at(vm, filter_si, di, vm.ds_read(di + 0x155D));
            x_dir = 0;
        }
        if (x_carry) {
            // sub_1592d: X position snap based on direction
            if (x_dir == 0) {
                // Moved right: snap X_end to tile boundary
                uint16_t x_end = vm.ds_read(di + 0x155D);
                uint16_t snapped = (x_end & 0xFFF0) - 1;
                vm.ds_write(di + 0x155D, snapped);
                uint16_t delta = x_end - snapped;
                vm.ds_write(di + 0x173D, vm.ds_read(di + 0x173D) - delta);
                vm.ds_write(di + 0x1535, vm.ds_read(di + 0x1535) - delta);
                vm.ds_write(di + 0x19BD, 0);
            } else {
                // Moved left: snap X_start to tile boundary
                uint16_t x_start = vm.ds_read(di + 0x1535);
                uint16_t snapped = (x_start | 0xF) + 1;
                vm.ds_write(di + 0x1535, snapped);
                uint16_t delta = snapped - x_start;
                vm.ds_write(di + 0x173D, vm.ds_read(di + 0x173D) + delta);
                vm.ds_write(di + 0x155D, vm.ds_read(di + 0x155D) + delta);
                vm.ds_write(di + 0x19BD, 0);
            }
            found = true;
        }
    }

    if (!found) {
        // sub_15c37: X-velocity object search.
        // Scans objects for type match, then compares X velocities.
        // If X velocity difference != 0 → calls sub_15cef/sub_15cf5 for bounding box check.
        uint8_t* rds = vm.ds;
        uint16_t table_end = *(uint16_t*)(rds + 0x372);
        vm.ds_write(0x3A, filter_si);
        for (uint16_t si2 = 0; (int16_t)si2 < (int16_t)table_end; si2 += 2) {
            if (*(uint16_t*)(rds + si2 + 0x1355) == 0) continue;
            if (si2 == *(uint16_t*)(rds + 0x42)) continue;
            vm.ds_write(0x38, si2);
            uint8_t obj_type = (uint8_t)*(uint16_t*)(rds + si2 + 0x17DD);
            uint16_t f = filter_si;
            bool match = false;
            while (true) {
                uint8_t fv = *(uint8_t*)(rds + (uint16_t)(f - 0x6B34));
                if (obj_type < fv) break;
                if (obj_type == fv) { match = true; break; }
                f++;
            }
            if (!match) continue;
            // Type matches. Compare X velocities.
            int16_t vel_diff = (int16_t)vm.ds_read(di + 0x1945) - (int16_t)*(uint16_t*)(rds + si2 + 0x1945);
            if (vel_diff == 0) continue;

            // sub_15cef (vel_diff < 0): ax = self.X_start
            // sub_15cf5 (vel_diff > 0): ax = self.X_end
            uint16_t ax_x;
            int16_t snap_dir;
            if (vel_diff < 0) {
                ax_x = vm.ds_read(di + 0x1535); // sub_15cef
                snap_dir = 1; // moved left
            } else {
                ax_x = vm.ds_read(di + 0x155D); // sub_15cf5
                snap_dir = 0; // moved right
            }

            // loc_15cf9: X point in partner range?
            if ((int16_t)ax_x < (int16_t)*(uint16_t*)(rds + si2 + 0x1535)) continue;
            if ((int16_t)(ax_x - 1) >= (int16_t)*(uint16_t*)(rds + si2 + 0x155D)) continue;

            // Y overlap with velocity adjustment:
            // self.Y_end_adj >= partner.Y_start_adj?
            int16_t partner_ys_adj = (int16_t)*(uint16_t*)(rds + si2 + 0x14E5) - (int16_t)*(uint16_t*)(rds + si2 + 0x196D);
            int16_t self_ye_adj = (int16_t)vm.ds_read(di + 0x150D) - (int16_t)vm.ds_read(di + 0x196D);
            if (self_ye_adj < partner_ys_adj) continue;

            // partner.Y_end_adj >= self.Y_start_adj?
            int16_t self_ys_adj = (int16_t)vm.ds_read(di + 0x14E5) - (int16_t)vm.ds_read(di + 0x196D);
            int16_t partner_ye_adj = (int16_t)*(uint16_t*)(rds + si2 + 0x150D) - (int16_t)*(uint16_t*)(rds + si2 + 0x196D);
            if (partner_ye_adj < self_ys_adj) continue;

            // Collision found! sub_15d6b: X position snap
            v2_vm_sub_15d6b(vm, snap_dir, si2, di);
            found = true;
            break;
        }
    }

    if (found) {
        // loc_157af: set collision bit
        uint16_t si_38e = vm.global_r(0x38E);
        uint16_t mask = *(uint16_t*)(vm.ds + (uint16_t)(si_38e - 0x6C34));
        vm.ds_write(di + 0x13F5, vm.ds_read(di + 0x13F5) | mask);
    }
    return false; // ALWAYS CLC — collision recorded via bits only
}

// 0x32 (sub_15772): collision_15788 + ds:0x38E += 2 (always) + skip/call-jump
static void v2_vm_op_32(V2VM& vm) {
    bool collision = v2_vm_collision_check_15788(vm);
    vm.ds_write(0x38E, vm.ds_read(0x38E) + 2); // ALWAYS increment
    if (collision) { v2_vm_do_call_jump(vm); } else { vm.pc += 2; }
}

// 0x33 (sub_157d5): collision_157eb (sub_1584e) + ds:0x38E += 2 (always) + skip/call-jump
static void v2_vm_op_33(V2VM& vm) {
    bool collision = v2_vm_collision_check_1584e(vm); // sub_157eb → sub_1584e pattern
    vm.ds_write(0x38E, vm.ds_read(0x38E) + 2); // ALWAYS increment
    if (collision) { v2_vm_do_call_jump(vm); } else { vm.pc += 2; }
}

// (old 0x9C stub removed — full impl at line ~228)

// 0x1A (sub_1559c): PUSH ds:0x372, set 6, collision_155d6, POP, branch
// helper consumes 1 byte; on no collision: skip 2 more; on collision: call-jump
static void v2_vm_op_1A(V2VM& vm) {
    bool collision = v2_vm_collision_check_155d6(vm);
    vm.ds_write(0x38E, vm.ds_read(0x38E) + 2); // ALWAYS increment
    if (collision) { v2_vm_do_call_jump(vm); } else { vm.pc += 2; }
}

// 0x1D (sub_15686): collision_156c0 + ds:0x38E += 2
static void v2_vm_op_1D(V2VM& vm) {
    bool collision = v2_vm_collision_check_156c0(vm);
    vm.ds_write(0x38E, vm.ds_read(0x38E) + 2); // ALWAYS increment
    if (collision) { v2_vm_do_call_jump(vm); } else { vm.pc += 2; }
}

// 0x38 (sub_156aa): collision_156c0 + ds:0x38E += 2
static void v2_vm_op_38(V2VM& vm) {
    bool collision = v2_vm_collision_check_156c0(vm);
    vm.ds_write(0x38E, vm.ds_read(0x38E) + 2); // ALWAYS increment
    if (collision) { v2_vm_do_call_jump(vm); } else { vm.pc += 2;
    }
}

// ============================================================================
// sub_154bf dispatch (off_30CA2 table, 5 entries)
// Called by opcodes 0x16, 0x14 and others for position/field writes.
// ax = value to write, si (pushed) = dispatch index.
// off_30CA2 entries:
//   [0] locret_15504: RETN (no-op, 0 bytes)
//   [1] loc_154cb: indexed field write → [si+0x14E5], 1 byte consumed
//   [2] loc_154e1: direct addr write → [addr], 2 bytes consumed
//   [3] loc_154eb: indexed field+0x1995 write → [di+0x14E5], 1 byte consumed
//   [4] locret_15504: RETN (no-op, 0 bytes)
// ============================================================================
static void v2_vm_sub_154bf(V2VM& vm, uint16_t ax_val, uint8_t mode) {
    // sub_154bf: PUSH si, AND ax,7, SHL 1, JMP off_30CA2[mode & 7]
    uint8_t entry = mode & 7;

    switch (entry) {
    case 0: // locret_15504: no-op
    case 4: // same as [0]
        // POP ax (restore si), no bytes consumed
        break;

    case 1: { // loc_154cb: indexed field write, 1 byte
        // MOV si, es:[bx]; INC bx; AND si,0xFF; MOV si,[si-0x6CBA]; ADD si,ds:0x42
        // POP ax; MOV [si+0x14E5], ax
        uint8_t idx = vm.read_u8();
        uint16_t field_off = *(uint16_t*)(vm.ds + (uint16_t)(idx - 0x6CBA));
        uint16_t target = field_off + vm.global_r(0x42);
        vm.ds_write(target + 0x14E5, ax_val);
        break;
    }

    case 2: { // loc_154e1: direct addr write, 2 bytes
        // MOV si, es:[bx]; ADD bx,2; POP ax; MOV [si], ax
        uint16_t addr = vm.read_u16();
        vm.ds_write(addr, ax_val);
        break;
    }

    case 3: { // loc_154eb: indexed field + 0x1995 write, 1 byte
        // MOV si, es:[bx]; INC bx; AND si,0xFF; MOV di,[si-0x6CBA];
        // MOV si,ds:0x42; ADD di,[si+0x1995]; POP ax; MOV [di+0x14E5], ax
        uint8_t idx = vm.read_u8();
        uint16_t field_off = *(uint16_t*)(vm.ds + (uint16_t)(idx - 0x6CBA));
        uint16_t si_obj = vm.global_r(0x42);
        uint16_t di_addr = field_off + vm.ds_read(si_obj + 0x1995);
        vm.ds_write(di_addr + 0x14E5, ax_val);
        break;
    }

    default:
        // Entries 5-7 shouldn't be used (table only has 5 entries)
        printf("V2-VM: sub_154bf unknown entry %d\n", entry);
        break;
    }
}

// 0x49 (sub_14fc4): Read X/Y values + animation load + off_30C8E[0] dispatch.
// 1. Read mode byte, dispatch (mode & 7) → ds:0x6C, dispatch ((mode >> 3) & 7) → ds:0x6E
// 2. Read anim byte
// 3. Call sub_1589B (animation load at 6C/6E position)
// 4. Dispatch off_30C8E[0]
static void v2_vm_op_49(V2VM& vm) {
    // Read mode byte (1 byte consumed)
    uint16_t word = *(uint16_t*)(vm.es + vm.pc);
    vm.pc += 1;
    uint8_t mode = (uint8_t)(word & 0xFF);

    // First dispatch: (mode & 7) → X value → ds:0x6C
    uint16_t x_val = v2_vm_dispatch_30C98(vm, mode);
    vm.ds_write(0x6C, x_val);

    // Second dispatch: ((mode >> 3) & 7) → Y value → ds:0x6E
    uint16_t y_val = v2_vm_dispatch_30C98(vm, mode >> 3);
    vm.ds_write(0x6E, y_val);

    // Read animation index (1 byte consumed)
    uint8_t anim_idx = vm.read_u8();

    // sub_1589B: tile search at (6C,6E) + object search at (6C,6E)
    v2_vm_sub_1589b(vm, anim_idx);

    // off_30C8E[0] = loc_144e9: no carry → skip 2, carry → jump
    if (vm.carry)
        v2_vm_do_jump(vm);
    else
        vm.pc += 2;
}

// 0x4A (sub_14fc8): Same as 0x49 but PUSH 2 → off_30C8E[2] dispatch.
static void v2_vm_op_4A(V2VM& vm) {
    uint16_t word = *(uint16_t*)(vm.es + vm.pc);
    vm.pc += 1;
    uint8_t mode = (uint8_t)(word & 0xFF);
    uint16_t x_val = v2_vm_dispatch_30C98(vm, mode);
    vm.ds_write(0x6C, x_val);
    uint16_t y_val = v2_vm_dispatch_30C98(vm, mode >> 3);
    vm.ds_write(0x6E, y_val);
    uint8_t anim_idx = vm.read_u8();
    // sub_1589B: tile search at (6C,6E) + object search at (6C,6E)
    v2_vm_sub_1589b(vm, anim_idx);
    uint16_t cs_addr = *(uint16_t*)(vm.ds + 0x87AE + 2); // si=2 → byte offset 2
    if (cs_addr == 0x44F3) {
        if (vm.carry) { vm.pc += 2; } else { v2_vm_do_jump(vm); }
    } else if (cs_addr == 0x44E9) {
        if (!vm.carry) { vm.pc += 2; } else { v2_vm_do_jump(vm); }
    } else {
        v2_vm_runtime_dispatch(vm, 0x87AE, 2);
    }
}

// 0x3D (sub_14532): Palette set. 3 bytes. Writes RGB*2 to ds:0x342-0x344.
// Then OR ds:0x7EFD |= 1, ds:0x7EFE = 4, ds:0x7F00 = 0x8202, JMP sub_10e99.
static void v2_vm_op_3D(V2VM& vm) {
    uint8_t r = vm.read_u8();
    uint8_t g = vm.read_u8();
    uint8_t b = vm.read_u8();
    vm.ds_write_b(0x342, (uint8_t)(r << 1));
    vm.ds_write_b(0x343, (uint8_t)(g << 1));
    vm.ds_write_b(0x344, (uint8_t)(b << 1));
    uint8_t flags = vm.shadow[0x7EFD];
    vm.ds_write_b(0x7EFD, flags | 1);
    vm.ds_write(0x7EFE, 4);
    vm.ds_write(0x7F00, 0x8202);
    v2_vm_sub_10e99(vm);
}

// 0x34 (sub_150b5): Find nearest player + position delta + dual sub_154bf dispatch.
// Iterates objects 0,2,4 (3 players), finds nearest by Manhattan distance.
// Then falls through to 0x16's logic (loc_1510E).
// Consumes same bytes as 0x16: 1 mode byte + N + M.
static void v2_vm_op_34(V2VM& vm) {
    uint16_t di = vm.global_r(0x42); // current object
    uint16_t best_dist = 0xFFFF;
    uint16_t best_si = 0;

    for (uint16_t si = 0; si < 6; si += 2) {
        if (vm.ds_read(si + 0x15AD) == 0) continue;
        // Manhattan distance: |di.X - si.X| + |di.Y - si.Y|
        int16_t dx_val = (int16_t)(vm.ds_read(di + 0x173D) - vm.ds_read(si + 0x173D));
        if (dx_val < 0) dx_val = -dx_val;
        int16_t dy_val = (int16_t)(vm.ds_read(di + 0x1765) - vm.ds_read(si + 0x1765));
        if (dy_val < 0) dy_val = -dy_val;
        uint16_t dist = (uint16_t)(dx_val + dy_val);
        if (dist < best_dist) {
            best_dist = dist;
            best_si = si;
        }
    }
    vm.ds_write(0x3CA, best_si);
    uint16_t si_sub = best_si; // nearest player is the "sub-object" for delta computation

    // loc_1510E: same as 0x16
    int16_t x_delta;
    if (vm.ds_read(di + 0x1585) & 0x40) {
        x_delta = (int16_t)(vm.ds_read(di + 0x173D) - vm.ds_read(si_sub + 0x173D));
    } else {
        x_delta = (int16_t)(vm.ds_read(si_sub + 0x173D) - vm.ds_read(di + 0x173D));
    }
    vm.ds_write(0x6C, (uint16_t)x_delta);

    int16_t y_delta;
    if (vm.ds_read(di + 0x1585) & 0x80) {
        y_delta = (int16_t)(vm.ds_read(di + 0x1765) - vm.ds_read(si_sub + 0x1765));
    } else {
        y_delta = (int16_t)(vm.ds_read(si_sub + 0x1765) - vm.ds_read(di + 0x1765));
    }
    vm.ds_write(0x6E, (uint16_t)y_delta);

    // Read mode byte + dual dispatch
    uint8_t mode = vm.read_u8();
    v2_vm_sub_154bf(vm, (uint16_t)x_delta, mode);
    v2_vm_sub_154bf(vm, (uint16_t)y_delta, mode >> 3);
}

// 0x16 (sub_15106): Position delta computation + dual sub_154bf dispatch.
// Reads 1 mode byte, then dispatches twice (X axis, Y axis) through off_30CA2.
// Total bytes consumed: 1 (mode) + N (X handler) + M (Y handler).
static void v2_vm_op_16(V2VM& vm) {
    uint16_t di = vm.global_r(0x42);
    uint16_t si_sub = vm.ds_read(di + 0x1995); // sub-object index

    // Compute X delta based on hflip flag (bit 6 = 0x40) → write to DS:0x6C
    int16_t x_delta;
    if (vm.ds_read(di + 0x1585) & 0x40) {
        x_delta = (int16_t)(vm.ds_read(di + 0x173D) - vm.ds_read(si_sub + 0x173D));
    } else {
        x_delta = (int16_t)(vm.ds_read(si_sub + 0x173D) - vm.ds_read(di + 0x173D));
    }
    vm.ds_write(0x6C, (uint16_t)x_delta); // MOV ds:6Ch, ax

    // Compute Y delta based on vflip flag (bit 7 = 0x80) → write to DS:0x6E
    int16_t y_delta;
    if (vm.ds_read(di + 0x1585) & 0x80) {
        y_delta = (int16_t)(vm.ds_read(di + 0x1765) - vm.ds_read(si_sub + 0x1765));
    } else {
        y_delta = (int16_t)(vm.ds_read(si_sub + 0x1765) - vm.ds_read(di + 0x1765));
    }
    vm.ds_write(0x6E, (uint16_t)y_delta); // MOV ds:6Eh, ax

    // Read mode byte
    uint8_t mode = vm.read_u8();

    // First dispatch: si = ds:0x6C, then sub_154bf
    v2_vm_sub_154bf(vm, vm.ds_read(0x6C), mode);

    // Second dispatch: si = ds:0x6E, then sub_154bf (mode >> 3)
    v2_vm_sub_154bf(vm, vm.ds_read(0x6E), mode >> 3);
}

// 0xA3 (sub_14d3d): conditional OR mask to DS address. 3 bytes (1 byte idx + 2 byte addr).
// If acc != 0: acc = ds:[idx - 0x6C34] (load mask from table).
// Then: ds:[addr] |= acc.
static void v2_vm_op_A3(V2VM& vm) {
    uint8_t idx = vm.read_u8();
    if (v2_vm_accumulator != 0) {
        v2_vm_accumulator = *(uint16_t*)(vm.ds + (uint16_t)(idx - 0x6C34));
    }
    uint16_t addr = vm.read_u16();
    vm.ds_write(addr, vm.ds_read(addr) | v2_vm_accumulator);
}

// 0xAF (sub_14e04): sub_1542a bit test (3 bytes), compare with acc, eq → skip 2, ne → jump.
static void v2_vm_op_AF(V2VM& vm) {
    // sub_1542a: byte idx + word addr → val=ds:[addr], mask=ds:[idx-0x6C34]
    uint8_t idx = vm.read_u8();
    uint16_t addr = vm.read_u16();
    uint16_t val = vm.ds_read(addr);
    uint16_t mask = *(uint16_t*)(vm.ds + (uint16_t)(idx - 0x6C34));
    uint16_t result = (val & mask) ? 1 : 0;
    // cmp result, acc: equal → skip 2, not equal → jump
    if (result == v2_vm_accumulator) {
        vm.pc += 2; // skip jump target
    } else {
        v2_vm_do_jump(vm); // read 2-byte target and jump
    }
}

// 0x30 (sub_144cf): push 0, sub_158e6 (flip-aware X tile search), off_30C8E[0].
// 1 byte param (filter index) + 2 byte jump target. Carry → jump, no carry → skip 2.
static void v2_vm_op_30(V2VM& vm) {
    uint8_t filter = vm.read_u8();
    uint16_t obj_di = vm.global_r(0x42);
    v2_vm_sub_158e6(vm, filter, obj_di);
    // off_30C8E[0] = loc_144e9: no carry → skip 2, carry → jump
    if (vm.carry) { v2_vm_do_jump(vm); } else { vm.pc += 2; }
}

// 0x39 (sub_145da): skip 3 bytes. Reads byte (discarded) + word (into ax, NOT acc).
// ax return value not used by VM loop. Pure 3-byte skip.
static void v2_vm_op_39(V2VM& vm) {
    vm.read_u8();  // byte (discarded)
    vm.read_u16(); // word (into ax, NOT ds:0x8A)
}

// 0x4F (sub_14501): push 2, sub_163ac (tile check), off_30C8E[2].
// 0 byte params + 2 byte jump target. Same as 0x4E but dispatch index 2.
static void v2_vm_op_4F(V2VM& vm) {
    vm.carry = v2_vm_sub_163ac(vm);
    // off_30C8E[2] = loc_144f3: JC → carry = skip 2, no carry = jump
    // (opposite of off_30C8E[0] which is JNC → no carry = skip, carry = jump)
    if (!vm.carry) { v2_vm_do_jump(vm); } else { vm.pc += 2; }
}

// 0xB6 (sub_14e77): sub_12312 (PRNG/timer, 0 bytes consumed) → AND ax,1.
// cmp ax, acc. ne → skip 2, eq → call (sub_142c1: save return + jump).
static void v2_vm_op_B6(V2VM& vm) {
    // sub_12312: exact replica
    uint16_t ax;
    if (vm.ds_read(0x3CC) == 0) {
        // PRNG path: dword at ds:0x8639 (dword_30B19)
        // mov eax, dword_30B19; mov edx, 15A4E35h; mul edx → edx:eax
        // add eax, 1; mov dword_30B19, eax; ror eax, 10h
        uint32_t val = *(uint32_t*)(vm.ds + 0x8639);
        uint64_t product = (uint64_t)val * 0x15A4E35ULL;
        val = (uint32_t)(product & 0xFFFFFFFF) + 1;
        *(uint32_t*)(vm.ds + 0x8639) = val;
        // ror eax, 16 = swap high/low words; ax = high word of stored value
        ax = (uint16_t)(val >> 16);
    } else {
        // Timer path: ds:0x352 (word_28832)
        // xchg ah, al; mov word_28832, ax; rcl ax, 3; xor word_28832, ax
        uint16_t v = vm.ds_read(0x352);
        v = (v >> 8) | (v << 8); // xchg ah, al
        vm.ds_write(0x352, v);
        // rcl ax, 3: rotate left through carry, 3 times.
        // CF=0 at entry (from CMP word_288ac, 0 — non-zero minus 0 = no borrow)
        uint16_t r = v;
        bool cf = false;
        for (int i = 0; i < 3; i++) {
            bool new_cf = (r >> 15) & 1;
            r = (r << 1) | (cf ? 1 : 0);
            cf = new_cf;
        }
        // xor word_28832, ax — ax still holds rcl result
        vm.ds_write(0x352, vm.ds_read(0x352) ^ r);
        ax = r;
    }
    ax &= 1;
    if (ax != v2_vm_accumulator) {
        vm.pc += 2;
    } else {
        // sub_142c1: save return address (bx+2) then jump
        uint16_t obj = vm.global_r(0x42);
        vm.ds_write(obj + 0x137D, vm.pc + 2);
        v2_vm_do_jump(vm);
    }
}

// 0xB8 (sub_14e9a): sub_15403 (indexed bit test, 2 bytes consumed).
// Reads 2 bytes: byte(mask_idx) + byte(field_idx).
// field_off = ds:[field_idx - 0x6CBA] + ds:0x42. val = ds:[field_off + 0x14E5].
// result = (val & mask_table[mask_idx]) ? 1 : 0.
// cmp result, acc. eq → skip 2, ne → call (sub_142c1: save return + jump).
static void v2_vm_op_B8(V2VM& vm) {
    // sub_15403: 2 bytes
    uint8_t mask_idx = vm.read_u8();
    uint8_t field_idx = vm.read_u8();
    uint16_t field_off = *(uint16_t*)(vm.ds + (uint16_t)(field_idx - 0x6CBA));
    field_off += vm.global_r(0x42);
    uint16_t val = vm.ds_read((uint16_t)(field_off + 0x14E5));
    uint16_t mask = *(uint16_t*)(vm.ds + (uint16_t)(mask_idx - 0x6C34));
    uint16_t ax = (val & mask) ? 1 : 0;
    if (ax == v2_vm_accumulator) {
        vm.pc += 2; // skip
    } else {
        // sub_142c1: save return address, then jump
        uint16_t obj = vm.global_r(0x42);
        vm.ds_write(obj + 0x137D, vm.pc + 2);
        v2_vm_do_jump(vm);
    }
}

// ============================================================================
// Unimplemented opcode handler
// ============================================================================
static void v2_vm_op_unimpl(V2VM& vm) {
    // Skip unknown opcodes — log once per opcode
    static bool logged[256] = {};
    uint8_t op = vm.es[vm.pc - 1]; // pc already advanced past opcode
    if (!logged[op]) {
        printf("V2-VM: unimplemented opcode 0x%02X at pc=0x%04X obj=%d\n", op, vm.pc - 1, vm.obj);
        logged[op] = true;
    }
    // Most opcodes read parameters — we don't know how many bytes to skip.
    // Stop execution to avoid desync.
    vm.running = false;
}

// ============================================================================
// Initialize opcode table
// ============================================================================
// sub_15569: collision detection VM — runs from [si+0x132D] using off_30CAC table.
// Stops when opcode 0x01 is encountered (checked before dispatch, unlike main VM).
static void v2_run_collision_vm(uint8_t* shadow, uint16_t obj_si) {
    *(uint16_t*)(shadow + 0x42) = obj_si;
    *(uint16_t*)(shadow + 0x38E) = 0;
    uint16_t code_seg = *(uint16_t*)(shadow + obj_si + 0x1355);
    uint16_t pc = *(uint16_t*)(shadow + obj_si + 0x132D);

    V2VM vm;
    vm.ds = shadow; vm.shadow = shadow;
    vm.es = v2_resolve_segment(code_seg, shadow);
    vm.cs_base = v2_m2c_base ? v2_m2c_base + 0x1A20 : nullptr; // seg000 CS base
    if (!vm.es) return;
    vm.obj = obj_si; vm.slot = obj_si / 2;
    vm.running = true; vm.carry = false;
    vm.pc = pc;

    int max_ops = 5000;
    while (vm.running && max_ops-- > 0) {
        uint8_t opcode = vm.es[vm.pc];
        vm.pc++;
        opcode &= 0xFF;
        if (opcode == 0x01) break; // collision VM yield
        if (opcode > 0xD7) {
            fprintf(stderr, "FATAL: collision VM opcode 0x%02X > 0xD7 at pc=%04X obj=%d\n", opcode, vm.pc-1, obj_si);
            exit(1);
        }
        if (!v2_vm_optable[opcode]) {
            fprintf(stderr, "FATAL: unimplemented collision VM opcode 0x%02X at pc=%04X obj=%d\n", opcode, vm.pc-1, obj_si);
            exit(1);
        }
        v2_vm_optable[opcode](vm);
    }
}

static void v2_vm_init_table() {
    if (v2_vm_table_initialized) return;

    // Default: all unimplemented
    for (int i = 0; i < 256; i++)
        v2_vm_optable[i] = v2_vm_op_unimpl;

    // Control flow
    v2_vm_optable[0x00] = v2_vm_op_yield;
    v2_vm_optable[0x01] = v2_vm_op_nop;
    v2_vm_optable[0x03] = v2_vm_op_jump;
    v2_vm_optable[0x04] = v2_vm_op_sound1;
    v2_vm_optable[0x05] = v2_vm_op_save_alt_pc;
    v2_vm_optable[0x06] = v2_vm_op_load_alt_pc;
    v2_vm_optable[0x0C] = v2_vm_op_0C;  // vertical flip — full logic
    v2_vm_optable[0x0D] = v2_vm_op_0D;  // clear bit — full logic
    v2_vm_optable[0x0E] = v2_vm_op_0E;  // set bit — full logic
    v2_vm_optable[0x0F] = v2_vm_op_exit_with_flag; // exit VM + flag

    v2_vm_optable[0x08] = v2_vm_op_08;  // horizontal flip
    v2_vm_optable[0x12] = v2_vm_op_12;  // release animation
    v2_vm_optable[0x14] = v2_vm_op_14;  // object search/creation
    v2_vm_optable[0x1E] = v2_vm_op_1F;  // anim load + off_30C8E[0] (sub_158c8 variant)
    v2_vm_optable[0x1F] = v2_vm_op_1F;  // anim load + off_30C8E[0] (sub_158d7 variant)
    v2_vm_optable[0x23] = v2_vm_op_23;  // anim load + off_30C8E[2] (sub_158d7)
    v2_vm_optable[0x31] = v2_vm_op_23;  // anim load + off_30C8E[2] (sub_158e6 variant)
    v2_vm_optable[0x36] = v2_vm_op_36;  // sub_15e8a: search objects + conditional jump
    v2_vm_optable[0x37] = v2_vm_op_37;  // collision check sub_155d6
    v2_vm_optable[0x41] = v2_vm_op_41;   // sub_1242e: text display, full bytecode consumption
    v2_vm_optable[0x45] = v2_vm_op_45;   // sub_12634: text display variant (sub_1250b + sub_125fa)
    v2_vm_optable[0x87] = v2_vm_op_87;   // sub_14abb: indexed field, ne→skip eq→call-jump
    v2_vm_optable[0x42] = v2_vm_op_42;  // cmd buffer type 2
    v2_vm_optable[0x5D] = v2_vm_op_5D;  // subtract acc from addr
    v2_vm_optable[0x7C] = v2_vm_op_7C;  // signed comparison >= (literal, jump)
    v2_vm_optable[0x81] = v2_vm_op_81;  // signed >= literal: acc>=val → skip, acc<val → jump (opposite of 0x7C)
    v2_vm_optable[0x82] = v2_vm_op_82;  // signed >= indexed, jump
    v2_vm_optable[0x22] = v2_vm_op_22;   // sub_14453: anim load (sub_158c8) + off_30C8E[2]
    v2_vm_optable[0x49] = v2_vm_op_49;   // sub_14fc4: X/Y dispatch + anim load + off_30C8E[0]
    v2_vm_optable[0x4A] = v2_vm_op_4A;   // sub_14fc8: same as 0x49 but off_30C8E[2]
    // 0x4E = sub_144fd: PUSH 0 + sub_163ac (0 bytes) + off_30C8E[0]
    // off_30C8E[0] = loc_144e9: no carry → skip 2, carry → jump
    // sub_163ac sets carry based on position check (not animation load)
    // Dispatch off_30C8E[0] with carry state from sub_163ac tile check
    v2_vm_optable[0x4E] = v2_vm_op_4E;  // sub_144fd: sub_163ac (tile check) + off_30C8E[0]
    v2_vm_optable[0x7E] = v2_vm_op_7E;   // sub_1494b: signed >= indirect, jump
    v2_vm_optable[0x83] = v2_vm_op_83;   // sub_149c3: signed < indirect, jump
    v2_vm_optable[0x91] = v2_vm_op_91;   // sub_146f6: hflip → add/sub acc to addr, 2 bytes
    v2_vm_optable[0x98] = v2_vm_op_98;   // sub_14b52: load acc from indexed+1995 bit test, 2 bytes
    v2_vm_optable[0x6A] = v2_vm_op_6A;   // sub_1489d: unsigned >= indexed+1995, jump
    v2_vm_optable[0x94] = v2_vm_op_94;   // sub_14763: hflip → sub/add acc to addr (opposite of 0x91)
    v2_vm_optable[0xA8] = v2_vm_op_A8;   // sub_14d91: sub_153ea bit test, ne→skip eq→jump
    v2_vm_optable[0xAD] = v2_vm_op_AD;   // sub_14de4: sub_153ea bit test, eq→skip ne→jump
    v2_vm_optable[0xB3] = v2_vm_op_B3;   // sub_14e47: sub_15403 bit test, ne→skip eq→call-jump
    v2_vm_optable[0x07] = v2_vm_op_07;   // sub_1368c: hflip if NOT flipped (opposite of 0x08)
    v2_vm_optable[0x09] = v2_vm_op_09;  // sub_13743: vflip if flag 0x80 NOT set, 0 bytes
    v2_vm_optable[0x0A] = v2_vm_op_0A;  // sub_13733: vflip if flag 0x80 set, 0 bytes
    v2_vm_optable[0x0B] = v2_vm_op_0B;  // sub_1369c: unconditional hflip
    v2_vm_optable[0x11] = v2_vm_op_11;  // sub_14334: sub_15505(self, linked), 0 bytes
    v2_vm_optable[0x15] = v2_vm_op_15;  // sub_150fc: relative pos to viking → 2x sub_154bf, 1 byte
    v2_vm_optable[0x1B] = v2_vm_op_1B;  // sub_143fe: obj0 collision + 2 vel bytes, 2 bytes
    v2_vm_optable[0x24] = v2_vm_op_24;  // sub_144ad: flip-aware anim load + off_30C8E[2], 1 byte
    v2_vm_optable[0x3A] = v2_vm_op_3A;  // sub_14340: sub_15505(linked, self), 0 bytes
    v2_vm_optable[0x47] = v2_vm_op_nop;  // nullsub_4: NOP, 0 bytes
    v2_vm_optable[0x17] = v2_vm_op_17;  // sub_143f2: set 141D=0 + read 2 velocity bytes
    v2_vm_optable[0x3E] = v2_vm_op_3E;  // sub_14590: clear palette, 0 bytes
    v2_vm_optable[0x26] = v2_vm_op_26;   // sub_14edd: 2 mode bytes + 4 dispatches
    v2_vm_optable[0x63] = v2_vm_op_63;   // sub_146b4: store acc to indexed+1995 field, 1 byte
    v2_vm_optable[0x7D] = v2_vm_op_7D;   // sub_14933: signed >= indexed, jump, 1 byte
    v2_vm_optable[0x9D] = v2_vm_op_9D;   // sub_14ba7: conditional mask load, 1 byte
    v2_vm_optable[0x9E] = v2_vm_op_9E;   // sub_14bcf: conditional mask load, 1 byte
    v2_vm_optable[0xB5] = v2_vm_op_B5;   // sub_14e67: indexed+1995 bit test, ne→skip eq→call-jump
    v2_vm_optable[0x27] = v2_vm_op_27;   // sub_14f09: tile lookup + position write, 2 mode bytes
    v2_vm_optable[0x5F] = v2_vm_op_5F;   // sub_147a7: AND acc with indexed field, 1 byte
    v2_vm_optable[0xA3] = v2_vm_op_A3;   // sub_14d3d: conditional OR mask to DS address, 3 bytes
    v2_vm_optable[0xAA] = v2_vm_op_AA;   // sub_14da1: indexed+1995 bit test, ne→skip eq→jump
    v2_vm_optable[0xAF] = v2_vm_op_AF;   // sub_14e04: sub_1542a bit test, eq→skip ne→jump
    v2_vm_optable[0x30] = v2_vm_op_30;   // sub_144cf: sub_158e6 flip search + off_30C8E[0], 3 bytes
    v2_vm_optable[0x39] = v2_vm_op_39;   // sub_145da: skip 3 bytes (byte+word, not stored)
    v2_vm_optable[0x4F] = v2_vm_op_4F;   // sub_14501: sub_163ac tile check + off_30C8E[2], 2 bytes
    v2_vm_optable[0xB6] = v2_vm_op_B6;   // sub_14e77: sub_12312 PRNG + conditional call, 2 bytes
    v2_vm_optable[0xB8] = v2_vm_op_B8;   // sub_14e9a: sub_15403 indexed bit test + conditional call, 4 bytes
    v2_vm_optable[0x59] = v2_vm_op_59;   // sub_146de: add acc to indexed field, 1 byte
    v2_vm_optable[0xAE] = v2_vm_op_AE;   // sub_14df4: indexed+1995 bit test, eq→skip ne→jump
    v2_vm_optable[0xC2] = v2_vm_op_C2;   // sub_151b8: player-search + off_30C8E[0], 1 byte
    v2_vm_optable[0xCB] = v2_vm_op_43;  // sub_1267b: same as 0x43
    v2_vm_optable[0xCF] = v2_vm_op_CF;   // sub_152c6: viewport bounds check, within→skip outside→jump
    v2_vm_optable[0xD2] = v2_vm_op_D2;  // password system
    v2_vm_optable[0xD3] = v2_vm_op_D3;  // sub_12829: password verify, 0 bytes, full logic

    // Sound
    v2_vm_optable[0x02] = v2_vm_op_sound;

    // State operations
    v2_vm_optable[0x19] = v2_vm_op_19;
    v2_vm_optable[0x2E] = v2_vm_op_2E;  // sub_1522c: collision search params, 2 bytes
    v2_vm_optable[0x2F] = v2_vm_op_2F;
    v2_vm_optable[0x46] = v2_vm_op_46;
    v2_vm_optable[0x97] = v2_vm_op_bit_test;  // sub_14b4b→sub_153ea: bytecode literal bit test

    // Load accumulator (exact)
    v2_vm_optable[0x51] = v2_vm_op_load_acc_literal;
    v2_vm_optable[0x52] = v2_vm_op_load_acc_indexed;
    v2_vm_optable[0x53] = v2_vm_op_load_acc_indirect;

    // Accumulator-based conditional branches (exact condition evaluation)
    v2_vm_optable[0x72] = v2_vm_op_72;
    v2_vm_optable[0x73] = v2_vm_op_73;
    v2_vm_optable[0x74] = v2_vm_op_74;
    v2_vm_optable[0x77] = v2_vm_op_77;
    v2_vm_optable[0x78] = v2_vm_op_78;
    v2_vm_optable[0x8B] = v2_vm_op_8B;
    v2_vm_optable[0x8C] = v2_vm_op_8C;

    // Collision conditionals (verified implementations)
    v2_vm_optable[0x1A] = v2_vm_op_1A;  // sub_155d6 (1B) + skip/call-jump
    v2_vm_optable[0x1D] = v2_vm_op_1D;  // sub_156c0 (2B) + skip/call-jump
    v2_vm_optable[0x38] = v2_vm_op_38;  // sub_156c0 (2B) + skip/call-jump

    // Collision with 1-byte read (like sub_155d6/sub_15788 pattern)
    v2_vm_optable[0x32] = v2_vm_op_32;   // sub_15788 (1B) + skip/call-jump
    v2_vm_optable[0x33] = v2_vm_op_33;   // sub_157eb (1B) + skip/call-jump

    // Destroy object + exit VM
    v2_vm_optable[0x10] = v2_vm_op_10;   // sub_14327: full sub_13c93 logic
    // Conditional on animation state
    v2_vm_optable[0x1C] = v2_vm_op_1C;   // sub_1443d: [obj+0x1A35] != 0 ? skip : jump
    // Animation load variants — all consume 1 byte + off_30C8E dispatch
    v2_vm_optable[0x20] = v2_vm_op_1F;   // sub_144a9: anim load + off_30C8E[0] (reversed flip)
    v2_vm_optable[0x21] = v2_vm_op_21;   // sub_14483: anim load + off_30C8E[0]
    v2_vm_optable[0x13] = v2_vm_op_13;  // sub_1434c: level/palette cmd, 3 bytes, full logic
    v2_vm_optable[0x3D] = v2_vm_op_3D;   // sub_14532: palette set, 3 bytes
    v2_vm_optable[0x34] = v2_vm_op_34;   // sub_150b5: find nearest player + position delta
    v2_vm_optable[0x3F] = v2_vm_op_3F;   // sub_145e5: OR 0x4000 on all sub-sprites
    v2_vm_optable[0x40] = v2_vm_op_40;   // sub_14604: AND 0x9FFF on all sub-sprites
    // Subtract accumulator from indexed field
    v2_vm_optable[0x5C] = v2_vm_op_5C;   // sub_1474b: field -= acc, 1 byte
    // Collision check via sub_1584e
    v2_vm_optable[0x3C] = v2_vm_op_3C;   // sub_15838: sub_1584e + ds:0x38E += 2
    // Bit test + call-jump
    v2_vm_optable[0xB2] = v2_vm_op_B2;   // sub_14e37: sub_153ea (3B) + cmp acc

    // 0 bytes consumed — state modifications only
    v2_vm_optable[0x16] = v2_vm_op_16;    // sub_15106: position delta + dual sub_154bf dispatch
    v2_vm_optable[0x43] = v2_vm_op_43;   // sub_1267b: cmd buffer write type=4, 0 bytes
    v2_vm_optable[0x4B] = v2_vm_op_4B;    // OR 0x2000 flag — full logic
    v2_vm_optable[0x9C] = v2_vm_op_9C;   // cond acc load + AND/OR field — full logic

    // Skip 1 byte
    v2_vm_optable[0x25] = v2_vm_op_25;  // anim load + off_30C8E[2] dispatch
    v2_vm_optable[0x28] = v2_vm_op_28;   // sub_14f27: tile-aligned position write, 2 modes + dispatches
    // 0x56, 0x57 registered below with full implementations

    // No bytecode consumed
    // 0x0C already set above
    v2_vm_optable[0x44] = v2_vm_op_44;    // sub_1246d: text display variant (sub_125fa instead of sub_125a3)
    v2_vm_optable[0x96] = v2_vm_op_96;  // store acc to 0x1995 — full logic
    v2_vm_optable[0x99] = v2_vm_op_bit_test_addr;  // sub_14b59→sub_1542a: ds:[addr] bit test
    // 0xD1 registered below as v2_vm_op_D1

    // Full implementations (state modifications)
    v2_vm_optable[0x18] = v2_vm_op_18;   // set velocity, 2 bytes
    v2_vm_optable[0x54] = v2_vm_op_load_acc_indexed_1995; // load acc, 1 byte
    v2_vm_optable[0x56] = v2_vm_op_56;   // store acc to field, 1 byte
    v2_vm_optable[0x57] = v2_vm_op_57;   // store acc to addr, 2 bytes
    v2_vm_optable[0x58] = v2_vm_op_58;   // store acc to indexed+1995, 1 byte
    v2_vm_optable[0x5A] = v2_vm_op_5A;   // add acc to addr, 2 bytes
    v2_vm_optable[0x60] = v2_vm_op_60;   // AND acc with addr, 2 bytes
    v2_vm_optable[0x62] = v2_vm_op_62;   // OR field with acc, 1 byte
    v2_vm_optable[0x67] = v2_vm_op_67;   // AND field with acc, 1 byte
    v2_vm_optable[0x68] = v2_vm_op_68;   // if acc>=lit jump
    v2_vm_optable[0x79] = v2_vm_op_79;   // indirect, eq skip else jump
    v2_vm_optable[0x86] = v2_vm_op_86;   // literal, ne skip else call-jump
    v2_vm_optable[0x88] = v2_vm_op_88;   // indirect, ne skip else call-jump
    v2_vm_optable[0x96] = v2_vm_op_96;   // store acc to 0x1995
    v2_vm_optable[0xA9] = v2_vm_op_A9;   // indexed, ne skip else jump
    v2_vm_optable[0xAB] = v2_vm_op_AB;   // indexed+1995, ne skip else jump
    v2_vm_optable[0x9A] = v2_vm_op_bit_test_indexed; // bit test, 2 bytes
    v2_vm_optable[0x69] = v2_vm_op_69;   // sub_14879: indexed (1B), unsigned acc>=val → jump
    v2_vm_optable[0x6D] = v2_vm_op_6D;   // sub_148c1: literal (2B), unsigned acc<val → jump
    v2_vm_optable[0x50] = v2_vm_op_50;  // sub_126a9: text position cmd, 2 mode bytes + dispatches
    v2_vm_optable[0x55] = v2_vm_op_55;  // sub_1466e: acc = random (LCG from shadow DS seed), 0 bytes
    v2_vm_optable[0xC7] = v2_vm_op_C7;  // sub_15268: write acc to state[idx*14+0], 0 bytes
    v2_vm_optable[0xC8] = v2_vm_op_C8;  // sub_1527b: write acc to state[idx*14+2], 0 bytes
    v2_vm_optable[0xC9] = v2_vm_op_C9;  // sub_1529a: AND acc 0xCDFF + state[idx*14+0xA], 0 bytes
    v2_vm_optable[0xCA] = v2_vm_op_CA;  // sub_152b3: write acc to state[idx*14+0xC], 0 bytes
    v2_vm_optable[0xB0] = v2_vm_op_B0;   // sub_14e14: sub_15445 (2B), eq→skip ne→jump
    v2_vm_optable[0xBC] = v2_vm_op_BC;   // sub_14681: SHL8 + store to indexed field, 1 byte
    v2_vm_optable[0xBD] = v2_vm_op_BD;   // SHL8+store, 2 bytes

    // Complex opcodes — full implementations
    v2_vm_optable[0x29] = v2_vm_op_29;   // sub_15017: tile write + mark dirty, 2 modes + 3 dispatches
    v2_vm_optable[0x2A] = v2_vm_op_2A;   // sub_15078: set lower tile bits + mark dirty
    v2_vm_optable[0x2B] = v2_vm_op_2B;   // sub_15039: set upper tile flags, no dirty mark
    v2_vm_optable[0x2C] = v2_vm_op_2C;   // sub_15f2c: collision search + jump, 3 bytes
    v2_vm_optable[0x2D] = v2_vm_op_2D;   // sub_15f25: continue collision search, 0 bytes
    v2_vm_optable[0x3B] = v2_vm_op_3B;   // sub_1524a: set sprite params, 2 bytes
    v2_vm_optable[0x4C] = v2_vm_op_4C;   // sub_14561: set color shading, 3 bytes
    v2_vm_optable[0x4D] = v2_vm_op_4D;   // sub_145b5: clear color shading, 0 bytes
    v2_vm_optable[0x5B] = v2_vm_op_5B;   // sub_14721: ADD acc to indexed+1995 field, 1 byte
    v2_vm_optable[0x5E] = v2_vm_op_5E;   // sub_1478b: SUB acc from indexed+1995 field, 1 byte
    v2_vm_optable[0x64] = v2_vm_op_64;   // sub_1480b: OR acc into indexed+1995 field, 1 byte
    v2_vm_optable[0x66] = v2_vm_op_66;   // sub_1483f: XOR acc with ds:[addr], 2 bytes
    v2_vm_optable[0x6E] = v2_vm_op_6E;   // sub_148d3: indexed (1B), unsigned acc<val → jump
    v2_vm_optable[0x70] = v2_vm_op_70;   // sub_148f7: indexed+1995 (1B), unsigned acc<val → jump
    v2_vm_optable[0x75] = v2_vm_op_75;   // sub_14a3b: indexed+1995 (1B), ne → skip, eq → jump
    v2_vm_optable[0x7A] = v2_vm_op_7A;   // sub_14a8b: indexed+1995 (1B), eq → skip, ne → jump
    v2_vm_optable[0x7F] = v2_vm_op_7F;   // sub_14963: indexed+1995 (1B), signed acc>=val → jump
    v2_vm_optable[0x80] = v2_vm_op_80;   // sub_1497b: random (0B), signed acc>=val → jump
    v2_vm_optable[0x84] = v2_vm_op_84;   // sub_149db: indexed+1995 (1B), signed acc>=val → skip
    v2_vm_optable[0x85] = v2_vm_op_85;   // sub_149f3: random (0B), signed acc>=val → skip
    v2_vm_optable[0x90] = v2_vm_op_90;   // sub_146d0: conditional ADD/SUB (bit 0x40), 1 byte
    v2_vm_optable[0x93] = v2_vm_op_93;   // sub_1473d: conditional SUB/ADD (bit 0x40), 1 byte
    v2_vm_optable[0xCE] = v2_vm_op_CE;   // sub_152d6: viewport visibility → skip/jump, 2 bytes
    v2_vm_optable[0xD0] = v2_vm_op_D0;    // sub_15e7c: collision search setup + search, 3 bytes
    v2_vm_optable[0xD1] = v2_vm_op_D1;    // sub_15f17: collision search setup + search, 3 bytes

    v2_vm_optable[0xCC] = v2_vm_op_CC;  // sub_152de: viewport check (self), within→jump outside→skip2, 2 bytes
    v2_vm_optable[0xCD] = v2_vm_op_CD;  // sub_152ca: viewport check (linked obj), within→jump outside→skip2, 2 bytes
    v2_vm_optable[0xD4] = v2_vm_op_D4;  // sub_1531c: velocity from position delta, 2+ bytes (mode+threshold+dispatch)
    v2_vm_optable[0xD5] = v2_vm_op_D5;  // sub_178d6: sound play, 1 byte consumed (sound skipped for v2)
    v2_vm_optable[0xD6] = v2_vm_op_D6;  // sub_178f1: timer delay, 0 bytes (NOP for v2)
    v2_vm_optable[0xD7] = v2_vm_op_D7;  // sub_1787f: sound sequence, 3 bytes consumed (sound skipped)
    v2_vm_optable[0xC0] = v2_vm_op_C0;  // sub_1518a: vikings sub_15fbe + off_30C8E[0], 1 byte
    v2_vm_optable[0xC1] = v2_vm_op_C1;  // sub_151f2: vikings flip-aware obj + off_30C8E[0], 1 byte
    v2_vm_optable[0xC3] = v2_vm_op_C3;  // sub_15160: vikings sub_15fb1 + off_30C8E[2], 1 byte
    v2_vm_optable[0xC4] = v2_vm_op_C4;  // sub_1518e: vikings sub_15fbe + off_30C8E[2], 1 byte
    v2_vm_optable[0xC5] = v2_vm_op_C5;  // sub_151f6: vikings flip-inv obj + off_30C8E[2], 1 byte
    v2_vm_optable[0xC6] = v2_vm_op_C6;  // sub_151bc: vikings flip obj + off_30C8E[2], 1 byte

    v2_vm_table_initialized = true;
    printf("V2-VM: opcode table initialized\n");
}

// ============================================================================
// Execute VM for one animation object
// ============================================================================
static void v2_vm_execute_object(uint8_t* shadow, uint16_t obj_idx) {

    uint16_t code_seg = *(uint16_t*)(shadow + obj_idx + 0x1355);
    if (!code_seg) return;

    // Debug: verify shadow vs real DS for animation update flag
    if (v2_vm_real_ds_ptr) {
        uint8_t* real = v2_vm_real_ds_ptr;
        uint16_t shadow_flags = *(uint16_t*)(shadow + obj_idx + 0x1585);
        uint16_t real_flags = *(uint16_t*)(real + obj_idx + 0x1585);
        uint16_t shadow_32F = *(uint16_t*)(shadow + 0x32F);
        uint16_t real_32F = *(uint16_t*)(real + 0x32F);
        bool shadow_update = (shadow_flags & 0x200) || (shadow_32F != 0);
        bool real_update = (real_flags & 0x200) || (real_32F != 0);
        if (obj_idx == 0 && shadow_update != real_update) {
            static bool flag_dbg = false;
            if (!flag_dbg) {
                flag_dbg = true;
                printf("V2-VM: FLAG MISMATCH obj=0: shadow=0x%04X/0x%04X real=0x%04X/0x%04X\n",
                       shadow_flags, shadow_32F, real_flags, real_32F);
            }
        }
    }

    // Exact replica of sub_1424c init logic:
    // 1. Check timer flag (0x1000) + timer countdown
    uint16_t flags = *(uint16_t*)(shadow + obj_idx + 0x1585);
    { static int _xdbg = 0; _xdbg++;
      if (obj_idx == 0 && _xdbg <= 50)
        fprintf(stderr, "V2-EXEC[%d] obj=0: flags=%04X cs=%04X pc=%04X 32F=%04X anim=%04X\n",
                _xdbg, flags, code_seg, *(uint16_t*)(shadow + obj_idx + 0x132D),
                *(uint16_t*)(shadow + 0x32F), *(uint16_t*)(shadow + obj_idx + 0x16ED));
    }
    if (flags & 0x1000) {
        uint16_t timer = *(uint16_t*)(shadow + obj_idx + 0x1715);
        if (timer != 0) {
            // Original: DEC [si+0x1715] then FALL THROUGH to loc_14266 (continue VM processing).
            // Does NOT skip execution — timer is just decremented.
            *(uint16_t*)(shadow + obj_idx + 0x1715) = timer - 1;
        }
    }

    // 2. Store current obj to ds:0x42
    *(uint16_t*)(shadow + 0x42) = obj_idx;

    // 3. Clear ds:0x38E
    *(uint16_t*)(shadow + 0x38E) = 0;

    int slot = obj_idx / 2;
    if (slot >= 128) return;

    // Record pre-animation DS hash (opcode=0xFE marker, before animation update)
    { extern void v2_vm_trace_record_v2(uint16_t, uint16_t, uint8_t, uint16_t, uint16_t, uint16_t, uint16_t, uint8_t*);
      extern uint16_t v2_vm_step_per_obj[128];
      v2_vm_trace_record_v2(obj_idx, v2_vm_step_per_obj[slot], 0xFE,
                            0, 0, 0, 0, shadow); // opcode 0xFE = pre-anim marker
    }

    // 4. Exact replica of sub_1424c loc_14270..loc_142a2
    // loc_14270: es = ds:[si+0x1355]  — ALWAYS set from code_seg
    uint16_t es_seg = code_seg;

    // loc_14274: test [si+0x1585], 0x200; JNZ → animation update
    // loc_1427C: cmp ds:0x32F, 0; JZ → skip to loc_142a2
    bool anim_update = (flags & 0x200) || *(uint16_t*)(shadow + 0x32F) != 0;
    if (anim_update) {
        uint16_t anim_idx = *(uint16_t*)(shadow + obj_idx + 0x16ED);
        if (anim_idx & 0x8000) return;
        uint16_t bx_anim = anim_idx * 0x15;
        uint16_t anim_seg = *(uint16_t*)(shadow + 0x2E67);
        es_seg = anim_seg;
        uint8_t* anim_es = v2_resolve_segment(anim_seg, shadow);
        uint16_t new_pc = *(uint16_t*)(anim_es + bx_anim + 3);
        { static int _au = 0; _au++; if (_au <= 60 && obj_idx == 0)
            fprintf(stderr, "V2-ANIMUPD[%d]: obj=0 reason=%s anim=%d bx=%04X new_pc=%04X old_pc=%04X seg=%04X\n",
                    _au, (flags & 0x200) ? "flag200" : "32F!=0", anim_idx, bx_anim, new_pc,
                    *(uint16_t*)(shadow + obj_idx + 0x132D), anim_seg);
        }
        *(uint16_t*)(shadow + obj_idx + 0x132D) = new_pc;
    }

    // loc_142a2: bx = [si+0x132D]  — read PC from shadow (may have been updated above)
    uint16_t init_pc = *(uint16_t*)(shadow + obj_idx + 0x132D);

    V2VM vm;
    vm.ds = shadow;      // Full 64KB shadow — all reads/writes go through shadow
    vm.shadow = shadow;
    vm.es = v2_resolve_segment(es_seg, shadow);
    vm.cs_base = v2_m2c_base ? v2_m2c_base + 0x1A20 : nullptr; // seg000 CS base
    vm.obj = obj_idx;
    vm.slot = slot;
    vm.running = true;
    vm.carry = false;
    vm.pc = init_pc;

    int max_ops = 10000; // safety limit
    while (vm.running && max_ops-- > 0) {
        if (vm.pc > 0xFFFF) {
            printf("V2-VM: PC out of bounds 0x%x obj=%d\n", vm.pc, obj_idx);
            break;
        }
        uint16_t pc_before = vm.pc;
        uint16_t acc_before = v2_vm_accumulator; // save BEFORE opcode
        uint8_t opcode = vm.read_u8();
        // Trace ES reads during transition VM pass (ds:0x32F != 0)
        if (*(uint16_t*)(shadow + 0x32F) != 0 && pc_before >= 0xC000) {
            static int _es_hi = 0; _es_hi++;
            if (_es_hi <= 20) fprintf(stderr, "V2-ES-HI: obj=%d pc=%04X op=%02X (ES read >= 0xC000!)\n",
                                      obj_idx, pc_before, opcode);
        }

        if (opcode > 0xD7) {
            fprintf(stderr, "FATAL: VM opcode 0x%02X > 0xD7 at pc=%04X obj=%d\n", opcode, pc_before, obj_idx);
            exit(1);
        }
        if (!v2_vm_optable[opcode]) {
            fprintf(stderr, "FATAL: unimplemented VM opcode 0x%02X at pc=%04X obj=%d\n", opcode, pc_before, obj_idx);
            exit(1);
        }
        v2_vm_optable[opcode](vm);

        // Per-object detailed trace (configurable via v2_trace_object)
        if (obj_idx == v2_trace_object) {
            fprintf(stderr, "V2-OBJ-TRACE: obj=%02X op=%02X pc=%04X→%04X acc=%04X→%04X es=%04X\n",
                obj_idx, opcode, pc_before, vm.pc, acc_before, v2_vm_accumulator,
                *(uint16_t*)(shadow + obj_idx + 0x1355));
        }

        // Record trace entry for comparison with original
        { extern void v2_vm_trace_record_v2(uint16_t, uint16_t, uint8_t, uint16_t, uint16_t, uint16_t, uint16_t, uint8_t*);
          extern uint16_t v2_vm_step_per_obj[128];
          v2_vm_trace_record_v2(obj_idx, v2_vm_step_per_obj[vm.slot]++, opcode,
                                pc_before, vm.pc, acc_before, v2_vm_accumulator, shadow);
        }

        // Record trace for per-opcode verification
        int& cnt = v2_vm_trace_count[vm.slot];
        if (cnt < V2_VM_TRACE_MAX) {
            v2_vm_trace[vm.slot][cnt].opcode = opcode;
            v2_vm_trace[vm.slot][cnt].pc_before = pc_before + 1;
            v2_vm_trace[vm.slot][cnt].pc_after = vm.pc;
            v2_vm_trace[vm.slot][cnt].acc_before = acc_before;
            v2_vm_trace[vm.slot][cnt].acc_after = v2_vm_accumulator;
            // Compute es segment value from vm.es pointer
            v2_vm_trace[vm.slot][cnt].es_seg = (uint16_t)((vm.es - v2_m2c_base) >> 4);
            cnt++;
        }
    }
}

// ============================================================================
// Main entry point — parallel to sub_14207
// Called each frame from the game loop.
// Sync shadow DS from real DS for rendering. Called right before v2_draw_tiles/sprites.
// Copies all game-loop-updated fields that the renderer needs but v2 VM doesn't produce.
void v2_vm_sync_for_render() {
    // PERSISTENT shadow — NO memcpy. Renderer reads from shadow as-is.
    // Shadow DS, tilemap, tilegfx maintained by v2 game loop.
}

// Accessor for shadow DS — used by v2 renderer when V2_RENDER_FROM_SHADOW is enabled.
uint8_t* v2_vm_get_shadow_ds() {
    return v2_vm_shadow_ds;
}

uint8_t* v2_vm_get_shadow_tilemap() {
    return v2_vm_shadow_tilemap;
}

bool v2_vm_is_tilemap_shadow_valid() {
    return v2_tilemap_shadow_valid;
}

uint8_t* v2_vm_get_shadow_tilegfx() {
    return v2_vm_shadow_tilegfx;
}

bool v2_vm_is_tilegfx_shadow_valid() {
    return v2_tilegfx_shadow_valid;
}

uint8_t* v2_vm_get_shadow_animdata() { return v2_vm_shadow_animdata; }
bool v2_vm_is_animdata_shadow_valid() { return v2_animdata_shadow_valid; }
uint8_t* v2_vm_get_shadow_gs() { return v2_vm_shadow_gs; }
bool v2_vm_is_gs_shadow_valid() { return v2_gs_shadow_valid; }
uint8_t* v2_vm_get_shadow_sound() { return v2_vm_shadow_sound; }
bool v2_vm_is_sound_shadow_valid() { return v2_sound_shadow_valid; }
uint8_t* v2_vm_get_shadow_chunk() { return v2_vm_shadow_chunk; }
bool v2_vm_is_chunk_shadow_valid() { return v2_chunk_shadow_valid; }

uint8_t* v2_vm_get_shadow_sprite(uint32_t linear_addr) {
    if (!v2_sprite_shadow_active) return nullptr;
    if (linear_addr < v2_sprite_shadow_base) return nullptr;
    uint32_t off = linear_addr - v2_sprite_shadow_base;
    if (off >= V2_SPRITE_SHADOW_SIZE) return nullptr;
    return v2_sprite_shadow + off;
}

// ============================================================================
// ============================================================================
// Post-game-loop verification functions
// ============================================================================

// Post-init verification: compare ALL segments after init, before game loop.
void v2_vm_verify_after_init(uint16_t ds_val) {
    if (!v2_vm_real_ds_ptr || !v2_shadow_initialized) return;
    uint8_t* real = v2_vm_real_ds_ptr;
    uint8_t* shadow = v2_vm_shadow_ds;
    printf("V2-INIT-VERIFY: shadow 92F7=%04X 92F9=%04X 92FB=%04X\n",
           *(uint16_t*)(shadow+0x92F7), *(uint16_t*)(shadow+0x92F9), *(uint16_t*)(shadow+0x92FB));
    printf("V2-INIT-VERIFY: real   92F7=%04X 92F9=%04X 92FB=%04X\n",
           *(uint16_t*)(real+0x92F7), *(uint16_t*)(real+0x92F9), *(uint16_t*)(real+0x92FB));
    printf("V2-INIT-VERIFY: Comparing all segments after init...\n");
    int ds_diffs = 0;
    int ds_expected = 0;
    for (uint32_t i = 0; i < 0x10000; i += 2) {
        if (*(uint16_t*)(real + i) != *(uint16_t*)(shadow + i)) {
            // ---- Expected diffs (standalone v2) ----

            // Segment addresses: ds:0x2E5C..0x2E7C
            // v2 uses fake sequential segments (0x1000+), original has DOS-allocated.
            // v2_resolve_segment maps fake values to shadow buffers correctly.
            if (i >= 0x2E5C && i <= 0x2E7C) { ds_expected++; continue; }

            // PRNG seed: ds:0x8638..0x863C (dword at ds:0x8639)
            // v2 uses time(), original uses INT 21h/2Ch (DOS get time).
            // Different seed → different randomization, but game logic identical.
            if (i >= 0x8638 && i <= 0x863C) { ds_expected++; continue; }

            // Render callback counter: ds:0xA39C (word_3287C)
            // Original render thread DECs asynchronously on vsync.
            // Threading race condition — inherently non-deterministic.
            if (i == 0xA39C) { ds_expected++; continue; }

            // DOS INT 24h vector: ds:0x86AC..0x86AE
            // sub_12948 saves old critical error handler vector.
            // v2 has no DOS — writes 0.
            if (i >= 0x86AC && i <= 0x86AE) { ds_expected++; continue; }

            // BIOS checksum: ds:0x86D0
            // sub_12989 computes checksum from BIOS ROM + DOS version.
            // Copy protection check, not used by game logic. v2 writes 0.
            if (i == 0x86D0) { ds_expected++; continue; }

            // VGA video mode: ds:0x9300
            // sub_167ff saves current video mode before Mode X init.
            // v2 has no VGA — writes 0x03 (text mode placeholder).
            if (i == 0x9300) { ds_expected++; continue; }

            // Sound driver state: ds:0x98E8..0x9950, ds:0xA39A
            // sub_17561 initializes AIL sound driver, writes handles/buffers.
            // v2 uses SDL audio, not AIL — these fields stay 0.
            if (i >= 0x98E8 && i <= 0x9950) { ds_expected++; continue; }
            if (i == 0xA39A) { ds_expected++; continue; }

            // Object active/code_seg field: ds:[si + 0x1355]
            // sub_13e52 writes ds:0x2E67 (animdata segment) to ds:[si+0x1355].
            // With fake segment 0x7000 vs real 0x9177, the stored value differs.
            // Used as: (1) active flag (!=0 → alive, works with any non-zero),
            //          (2) VM code segment (v2_resolve_segment maps correctly).
            // Affects words at 0x1354 and 0x1356 (unaligned field at 0x1355).
            if (i == 0x1354 || i == 0x1356) { ds_expected++; continue; }

            // ---- Unexpected diffs ----
            if (ds_diffs < 30)
                printf("  DS[0x%04X]: real=0x%04X shadow=0x%04X\n",
                       (uint16_t)i, *(uint16_t*)(real+i), *(uint16_t*)(shadow+i));
            ds_diffs++;
        }
    }
    printf("  DS: %d unexpected diffs, %d expected diffs\n", ds_diffs, ds_expected);

    auto cmp_seg = [&](const char* name, uint8_t* shadow_buf, uint32_t size, uint16_t ds_addr) {
        uint16_t seg = *(uint16_t*)(real + ds_addr);
        if (!seg || !v2_m2c_base) { printf("  %s: segment=0 (skip)\n", name); return; }
        uint8_t* real_buf = v2_m2c_base + (uint32_t)seg * 16;
        int diffs = 0;
        for (uint32_t i = 0; i < size; i += 2) {
            if (*(uint16_t*)(real_buf + i) != *(uint16_t*)(shadow_buf + i)) diffs++;
        }
        printf("  %s: %d diffs (seg=0x%04X)\n", name, diffs, seg);
    };
    // Compare segments using ALLOC sizes (not full 64KB — reading past alloc hits adjacent segments)
    cmp_seg("TILEMAP", v2_vm_shadow_tilemap, 0x3130, 0x2E63);       // 0x313 para
    cmp_seg("TILEGFX", v2_vm_shadow_tilegfx, 0x8B80, 0x2E5F);       // 0x8B8 para
    cmp_seg("ANIMDATA", v2_vm_shadow_animdata, 0xC000, 0x2E67);      // 0xC00 para
    cmp_seg("GS", v2_vm_shadow_gs, 0x2000, 0x2E61);                  // 0x200 para
    cmp_seg("SOUND", v2_vm_shadow_sound, 0x7000, 0x2E6B);            // 0x700 para
    cmp_seg("CHUNK", v2_vm_shadow_chunk, V2_CHUNK_SHADOW_SIZE, 0x2E77); // 0x2ABA para
    cmp_seg("GS_TILEDATA", v2_vm_shadow_gs_tiledata, 0x3600, 0x2E5D); // 0x360 para
    printf("  v2_sub_13fc2 calls: %d\n", v2_13fc2_count);
    // FS detailed diffs — dump first 20, track range
    {
        uint16_t seg = *(uint16_t*)(real + 0x2E69);
        if (seg && v2_m2c_base) {
            uint8_t* rb = v2_m2c_base + (uint32_t)seg * 16;
            int cnt = 0;
            uint32_t first_diff = 0xFFFF, last_diff = 0;
            int total = 0;
            int real_nonzero = 0, shadow_nonzero = 0;
            for (uint32_t i = 0; i < 0xC080; i += 2) {
                uint16_t rv = *(uint16_t*)(rb+i);
                uint16_t sv = *(uint16_t*)(v2_vm_shadow_fs+i);
                if (rv != sv) {
                    total++;
                    if (i < first_diff) first_diff = i;
                    if (i > last_diff) last_diff = i;
                    if (rv != 0) real_nonzero++;
                    if (sv != 0) shadow_nonzero++;
                    if (cnt < 20) {
                        printf("  FS[0x%04X]: real=%04X shadow=%04X\n",
                               (uint16_t)i, rv, sv);
                    }
                    cnt++;
                }
            }
            printf("  FS diffs: total=%d range=[0x%04X..0x%04X] real_nonzero=%d shadow_nonzero=%d\n",
                   total, (uint16_t)first_diff, (uint16_t)last_diff, real_nonzero, shadow_nonzero);
            // Check: cols and rows
            uint16_t fs_cols = *(uint16_t*)(real + 0x25DC);
            uint16_t fs_rows = *(uint16_t*)(real + 0x25DE);
            uint16_t stride = *(uint16_t*)(real + 0x8F6C);
            printf("  FS layout: cols=%d rows=%d used=%d stride=0x%04X\n",
                   fs_cols, fs_rows, fs_rows*fs_cols*8, stride);
        }
    }
    // FS: v2_sub_173c7 fills rows*cols*8 bytes. Rest is VGA 3-page data (sub_16ded).
    // Compare only the portion v2 fills: rows * (cols*4 + cols*4) = rows * cols * 8.
    {
        uint16_t fs_cols = *(uint16_t*)(shadow + 0x25DC);
        uint16_t fs_rows = *(uint16_t*)(shadow + 0x25DE);
        uint32_t fs_used = (uint32_t)fs_rows * fs_cols * 8;
        if (fs_used > 0xC080) fs_used = 0xC080;
        cmp_seg("FS(used)", v2_vm_shadow_fs, fs_used, 0x2E69);
        // Also show total for reference
        cmp_seg("FS(total)", v2_vm_shadow_fs, 0xC080, 0x2E69);
    }
    // Full DS compare: shadow vs real
    {
        int ds_diffs = 0;
        for (uint32_t i = 0; i < 0x10000; i += 2) {
            uint16_t rv = *(uint16_t*)(real + i);
            uint16_t sv = *(uint16_t*)(shadow + i);
            if (rv != sv) {
                if (i == 0xA39C) continue; // VGA interrupt race
                if (i >= 0x990C && i <= 0x991E) continue; // sound handles
                if (ds_diffs < 50) {
                    printf("V2-INIT-DS: 0x%04X: real=%04X shadow=%04X\n", (uint16_t)i, rv, sv);
                }
                ds_diffs++;
            }
        }
        printf("V2-INIT-DS: total diffs=%d\n", ds_diffs);
    }
    printf("V2-INIT-VERIFY: Done.\n");
}

// #1: Full DS compare after game loop (sub_1386b..sub_1064b)
void v2_vm_verify_game_loop(uint16_t ds_val) {
    if (!v2_vm_real_ds_ptr) return;
    v2_hw_wp_drain(); // drain HW watchpoint right before comparison
    uint8_t* real = v2_vm_real_ds_ptr;
    uint8_t* shadow = v2_vm_shadow_ds;
    static int gl_err = 0;
    static int gl_frame = 0;
    gl_frame++;
    if (gl_frame > 200) return;
    if (!v2_frame_active) {
        printf("V2-GAMELOOP[%d]: SKIPPED (transition frame, v2_frame_active=false) real_level=0x%04X shadow_level=0x%04X real_0334=0x%04X shadow_0334=0x%04X\n",
               gl_frame, *(uint16_t*)(real + 0x25AD), *(uint16_t*)(shadow + 0x25AD),
               *(uint16_t*)(real + 0x0334), *(uint16_t*)(shadow + 0x0334));
        return;
    }
    for (uint32_t i = 0; i < 0x10000 && gl_err < 200; i += 2) {
        uint16_t rv = *(uint16_t*)(real + i);
        uint16_t sv = *(uint16_t*)(shadow + i);
        if (rv != sv) {
            // Skip VGA interrupt race: word_3287C set by sub_16775, cleared by VGA interrupt handler.
            // v2 has no VGA interrupt — clears immediately. Real DS may still be 1 (pending interrupt).
            if (i == 0xA39C) continue;
            // Skip AIL sound handles (4 slots × 2 words each: handle at [si-0x66F4], seq at [si-0x66EA]).
            // si=2..8 → handles at 0x990E..0x9914, sequences at 0x9918..0x991E.
            // v2 has no AIL → handles stay 0xFFFF, real game allocates real handles.
            // Segment addresses from DosMemAlloc (v2 fake 0x1000+, original DOS heap).
            // Cannot match without calling same allocator. Fields:
            // ds:0x2E5C-0x2E7C (segment pointer table from sub_12ab8)
            // Sound system (v2 doesn't fully replicate AIL):
            // ds:0x2E6A-0x2E70 = sound segment pointers (sub_10E85 normalize results)
            // ds:0x990C-0x991E = AIL handles/sequences
            // ds:0x9934 = XMI buffer (TODO: implement sub_10E85 normalize)
            if (i >= 0x2E6A && i <= 0x2E70) continue;
            if (i >= 0x990C && i <= 0x991E) continue;
            if (i == 0x9934) continue;
            printf("V2-GAMELOOP[%d]: DS DIFF at 0x%04X: real=0x%04X shadow=0x%04X\n",
                   gl_frame, (uint16_t)i, rv, sv);

            gl_err++;
        }
    }
}

// Sub-sprite field verify — compare specific fields for all active sub-sprites
void v2_vm_verify_subsprites(uint16_t ds_val) {
    if (!v2_vm_real_ds_ptr) return;
    uint8_t* r = v2_vm_real_ds_ptr;
    uint8_t* s = v2_vm_shadow_ds;
    static int ss_frame = 0;
    ss_frame++;
    if (ss_frame > 200) return;
    // Only check when we have active objects
    uint16_t te = *(uint16_t*)(s + 0x372);
    if (te == 0) return;

    static int ss_errs = 0;
    // Field offsets to check: flags, mode, sprite_base, X, Y
    struct { uint16_t off; const char* name; } fields[] = {
        {0x044D, "flags"}, {0x114D, "mode"}, {0x054D, "sprite"},
        {0x064D, "X"}, {0x074D, "Y"}, {0x084D, "data_ptr"},
        {0x094D, "spr_seg"}, {0x0A4D, "base_off"}, {0x0B4D, "base_seg"},
    };
    // Check slots 0..0x100 (all possible sub-sprites)
    for (uint16_t slot = 0; slot < 0x100 && ss_errs < 100; slot += 2) {
        uint16_t r_flags = *(uint16_t*)(r + slot + 0x44D);
        uint16_t s_flags = *(uint16_t*)(s + slot + 0x44D);
        // Only check active sub-sprites (either side has flags != 0)
        if (r_flags == 0 && s_flags == 0) continue;
        for (auto& f : fields) {
            uint16_t rv = *(uint16_t*)(r + slot + f.off);
            uint16_t sv = *(uint16_t*)(s + slot + f.off);
            if (rv != sv) {
                fprintf(stderr, "V2-SS[f%d]: slot=%04X %s: real=%04X shadow=%04X\n",
                    ss_frame, slot, f.name, rv, sv);
                ss_errs++;
            }
        }
    }
}

// FS segment verify — compare shadow FS with real FS
void v2_vm_verify_fs(uint16_t ds_val) {
    if (!v2_vm_real_ds_ptr || !v2_m2c_base) return;
    static int fs_frame = 0;
    fs_frame++;
    if (fs_frame > 200) return;

    uint16_t fs_seg = *(uint16_t*)(v2_vm_real_ds_ptr + 0x2E69);
    if (fs_seg == 0) return;
    uint8_t* real_fs = v2_m2c_base + ((uint32_t)fs_seg << 4);
    uint8_t* shad_fs = v2_vm_shadow_fs;

    // FS grid: 0x2B columns × 0x19 rows (hardcoded in sub_1DE05), word per tile
    uint16_t cols = 0x2B; // 43 tiles wide
    uint16_t rows = 0x19; // 25 tiles tall
    uint32_t used = (uint32_t)cols * rows * 2;
    if (used > V2_FS_SHADOW_SIZE) used = V2_FS_SHADOW_SIZE;

    int diffs = 0;
    static int fs_errs = 0;
    for (uint32_t i = 0; i < used && fs_errs < 100; i += 2) {
        uint16_t rv = *(uint16_t*)(real_fs + i);
        uint16_t sv = *(uint16_t*)(shad_fs + i);
        if (rv != sv) {
            if (diffs == 0)
                fprintf(stderr, "V2-FS-VERIFY[f%d]: cols=%d rows=%d used=%d seg=%04X\n",
                    fs_frame, cols, rows, used, fs_seg);
            if (diffs < 10)
                fprintf(stderr, "  FS[0x%04X]: real=%04X shadow=%04X\n", (uint16_t)i, rv, sv);
            diffs++;
            fs_errs++;
        }
    }
    if (diffs > 0)
        fprintf(stderr, "V2-FS-VERIFY[f%d]: total diffs=%d\n", fs_frame, diffs);
}

// #2: Collision bits verify after sub_15546
void v2_vm_verify_collision(uint16_t ds_val) {
    if (!v2_vm_real_ds_ptr) return;
    uint8_t* real = v2_vm_real_ds_ptr;
    uint8_t* shadow = v2_vm_shadow_ds;
    static int coll_err = 0;
    uint16_t table_end = *(uint16_t*)(real + 0x372);
    for (uint16_t si = 0; (int16_t)si < (int16_t)table_end && coll_err < 20; si += 2) {
        uint16_t r_bits = *(uint16_t*)(real + si + 0x13F5);
        uint16_t s_bits = *(uint16_t*)(shadow + si + 0x13F5);
        if (r_bits != s_bits) {
            printf("V2-COLLISION: obj=%d bits DIFF: real=0x%04X shadow=0x%04X\n", si, r_bits, s_bits);
            coll_err++;
        }
    }
}

// #3: Tilemap verify — compare shadow tilemap with real FS segment
void v2_vm_verify_tilemap(uint16_t ds_val) {
    if (!v2_vm_real_ds_ptr || !v2_m2c_base) return;
    uint16_t tile_seg = *(uint16_t*)(v2_vm_real_ds_ptr + 0x2E63);
    if (tile_seg == 0) return;
    uint8_t* real_tm = v2_m2c_base + (uint32_t)tile_seg * 16;
    static int tm_err = 0;
    for (uint32_t i = 0; i < V2_TILEMAP_SHADOW_SIZE && tm_err < 20; i += 2) {
        uint16_t rv = *(uint16_t*)(real_tm + i);
        uint16_t sv = *(uint16_t*)(v2_vm_shadow_tilemap + i);
        if (rv != sv) {
            printf("V2-TILEMAP: DIFF at 0x%04X: real=0x%04X shadow=0x%04X\n", (uint16_t)i, rv, sv);
            tm_err++;
        }
    }
}

// #4: Object spawn table coverage — objects in viewport should exist in object table
static void v2_vm_verify_spawn_coverage(uint8_t* shadow) {
    static int spawn_err = 0;
    uint16_t vp_x = *(uint16_t*)(shadow + 0x44);
    uint16_t vp_y = *(uint16_t*)(shadow + 0x46);
    uint16_t table_end = *(uint16_t*)(shadow + 0x372);
    for (uint16_t di_off = 0; ; di_off += 0x0E) {
        uint16_t sx = *(uint16_t*)(shadow + di_off + 0x25F6);
        if (sx == 0xFFFF) break;
        uint16_t sy = *(uint16_t*)(shadow + di_off + 0x25F8);
        uint16_t hw = *(uint16_t*)(shadow + di_off + 0x25FA);
        uint16_t hh = *(uint16_t*)(shadow + di_off + 0x25FC);
        // Check if in viewport (rough)
        if ((int16_t)(sx + hw) < (int16_t)vp_x) continue;
        if ((int16_t)(sx - hw) > (int16_t)(vp_x + 0x140)) continue;
        if ((int16_t)(sy + hh) < (int16_t)vp_y) continue;
        if ((int16_t)(sy - hh) > (int16_t)(vp_y + 0xC0)) continue;
        // In viewport — check if spawned
        uint16_t spawn_idx = di_off / 0x0E;
        bool found = false;
        for (uint16_t si = 0; (int16_t)si < (int16_t)table_end; si += 2) {
            if (*(uint16_t*)(shadow + si + 0x1355) == 0) continue;
            if (*(uint16_t*)(shadow + si + 0x16C5) == spawn_idx) { found = true; break; }
        }
        if (!found && spawn_err < 10) {
            printf("V2-SPAWN: obj spawn_idx=%d at (%d,%d) in viewport but NOT in object table\n",
                   spawn_idx, sx, sy);
            spawn_err++;
        }
    }
}

// #5: Object table consistency check
static void v2_vm_verify_object_consistency(uint8_t* shadow) {
    static int obj_err = 0;
    uint16_t table_end = *(uint16_t*)(shadow + 0x372);
    for (uint16_t si = 0; (int16_t)si < (int16_t)table_end && obj_err < 20; si += 2) {
        if (*(uint16_t*)(shadow + si + 0x1355) == 0) continue;
        // Bounds check: X_start <= X_end, Y_start <= Y_end
        int16_t x_start = (int16_t)*(uint16_t*)(shadow + si + 0x1535);
        int16_t x_end = (int16_t)*(uint16_t*)(shadow + si + 0x155D);
        int16_t y_start = (int16_t)*(uint16_t*)(shadow + si + 0x14E5);
        int16_t y_end = (int16_t)*(uint16_t*)(shadow + si + 0x150D);
        if (x_start > x_end && obj_err < 20) {
            printf("V2-OBJ: obj=%d X bounds inverted: start=%d end=%d\n", si, x_start, x_end);
            obj_err++;
        }
        if (y_start > y_end && obj_err < 20) {
            printf("V2-OBJ: obj=%d Y bounds inverted: start=%d end=%d\n", si, y_start, y_end);
            obj_err++;
        }
        // Sub-sprite range check
        uint16_t ss_count = *(uint16_t*)(shadow + si + 0x1AD5);
        if (ss_count > 0) {
            uint16_t ss_start = *(uint16_t*)(shadow + si + 0x1A85);
            uint16_t ss_end = *(uint16_t*)(shadow + si + 0x1AAD);
            if (ss_start >= 0x100 || ss_end > 0x100 || ss_start >= ss_end) {
                if (obj_err < 20) {
                    printf("V2-OBJ: obj=%d sub-sprite range invalid: start=0x%X end=0x%X count=%d\n",
                           si, ss_start, ss_end, ss_count);
                    obj_err++;
                }
            }
        }
    }
}

// #6: VGA page state consistency
static void v2_vm_verify_page_state(uint8_t* shadow) {
    uint16_t p1 = *(uint16_t*)(shadow + 0x92F9);
    uint16_t p2 = *(uint16_t*)(shadow + 0x92FB);
    static int page_err = 0;
    if (p1 != 0 && p1 != 0x34 && p1 != 0x68 && page_err < 5) {
        printf("V2-PAGE: ds:0x92F9 = 0x%04X (expected 0/0x34/0x68)\n", p1);
        page_err++;
    }
    if (p2 != 0 && p2 != 0x34 && p2 != 0x68 && page_err < 5) {
        printf("V2-PAGE: ds:0x92FB = 0x%04X (expected 0/0x34/0x68)\n", p2);
        page_err++;
    }
}

// #7: Tile graphics verify — compare shadow tilegfx with real segment
static void v2_vm_verify_tilegfx(uint8_t* real_ds) {
    if (!v2_m2c_base) return;
    uint16_t tgfx_seg = *(uint16_t*)(real_ds + 0x2E5F);
    if (tgfx_seg == 0) return;
    uint8_t* real_tg = v2_m2c_base + (uint32_t)tgfx_seg * 16;
    static int tg_err = 0;
    for (uint32_t i = 0; i < V2_TILEGFX_SHADOW_SIZE && tg_err < 10; i += 2) {
        if (*(uint16_t*)(real_tg + i) != *(uint16_t*)(v2_vm_shadow_tilegfx + i)) {
            printf("V2-TILEGFX: DIFF at 0x%04X: real=0x%04X shadow=0x%04X\n",
                   (uint16_t)i, *(uint16_t*)(real_tg + i), *(uint16_t*)(v2_vm_shadow_tilegfx + i));
            tg_err++;
        }
    }
}

// #8: FS vs ES tilemap — verify render tilemap (0x2E69) matches game tilemap (0x2E63)
static void v2_vm_verify_fs_vs_es(uint8_t* real_ds) {
    if (!v2_m2c_base) return;
    uint16_t es_seg = *(uint16_t*)(real_ds + 0x2E63);
    uint16_t fs_seg = *(uint16_t*)(real_ds + 0x2E69);
    if (es_seg == 0 || fs_seg == 0) return;
    if (es_seg == fs_seg) return; // same segment, no check needed
    uint8_t* es_ptr = v2_m2c_base + (uint32_t)es_seg * 16;
    uint8_t* fs_ptr = v2_m2c_base + (uint32_t)fs_seg * 16;
    static int fs_err = 0;
    static int fs_frame = 0;
    fs_frame++;
    for (uint32_t i = 0; i < V2_TILEMAP_SHADOW_SIZE && fs_err < 10; i += 2) {
        if (*(uint16_t*)(es_ptr + i) != *(uint16_t*)(fs_ptr + i)) {
            printf("V2-FS_VS_ES[%d]: DIFF at 0x%04X: ES=0x%04X FS=0x%04X\n",
                   fs_frame, (uint16_t)i, *(uint16_t*)(es_ptr + i), *(uint16_t*)(fs_ptr + i));
            fs_err++;
        }
    }
}

// #10: Sprite base verify — object sprite_base points to valid resource
static void v2_vm_verify_sprite_bases(uint8_t* shadow) {
    static int spr_err = 0;
    uint16_t table_end = *(uint16_t*)(shadow + 0x372);
    for (uint16_t si = 0; (int16_t)si < (int16_t)table_end && spr_err < 10; si += 2) {
        if (*(uint16_t*)(shadow + si + 0x1355) == 0) continue;
        uint16_t sprite_base = *(uint16_t*)(shadow + si + 0x1855);
        // sprite_base should be in the loaded resource table ds:0x12ED
        if (sprite_base == 0) continue; // 0 is valid (no sprite / 0xFFFF chunk)
        bool found = false;
        for (uint16_t rdi = 0; rdi < 0x40; rdi += 2) {
            if (*(uint16_t*)(shadow + rdi + 0x12ED) == sprite_base) { found = true; break; }
        }
        if (!found && spr_err < 10) {
            printf("V2-SPRITE: obj=%d sprite_base=0x%04X not in resource table ds:0x12ED\n",
                   si, sprite_base);
            spr_err++;
        }
    }
}

// #11: Animation data segment verify (0x2E67) — should be read-only
static void v2_vm_verify_animdata(uint8_t* real_ds) {
    if (!v2_m2c_base || !v2_animdata_shadow_valid) return;
    uint16_t anim_seg = *(uint16_t*)(real_ds + 0x2E67);
    if (anim_seg == 0) return;
    uint8_t* real_ad = v2_m2c_base + (uint32_t)anim_seg * 16;
    static int ad_err = 0;
    for (uint32_t i = 0; i < V2_ANIMDATA_SHADOW_SIZE && ad_err < 10; i += 2) {
        if (*(uint16_t*)(real_ad + i) != *(uint16_t*)(v2_vm_shadow_animdata + i)) {
            printf("V2-ANIMDATA: DIFF at 0x%04X: real=0x%04X shadow=0x%04X\n",
                   (uint16_t)i, *(uint16_t*)(real_ad + i), *(uint16_t*)(v2_vm_shadow_animdata + i));
            ad_err++;
        }
    }
}

// #12: GS segment verify (0x2E61) — tile masks, should be read-only
static void v2_vm_verify_gs(uint8_t* real_ds) {
    if (!v2_m2c_base || !v2_gs_shadow_valid) return;
    uint16_t gs_seg = *(uint16_t*)(real_ds + 0x2E61);
    if (gs_seg == 0) return;
    uint8_t* real_gs = v2_m2c_base + (uint32_t)gs_seg * 16;
    static int gs_err = 0;
    for (uint32_t i = 0; i < V2_GS_SHADOW_SIZE && gs_err < 10; i += 2) {
        if (*(uint16_t*)(real_gs + i) != *(uint16_t*)(v2_vm_shadow_gs + i)) {
            printf("V2-GS: DIFF at 0x%04X: real=0x%04X shadow=0x%04X\n",
                   (uint16_t)i, *(uint16_t*)(real_gs + i), *(uint16_t*)(v2_vm_shadow_gs + i));
            gs_err++;
        }
    }
}

// #13: Sound data segment verify (0x2E6B) — warn if differs
static void v2_vm_verify_sound(uint8_t* real_ds) {
    if (!v2_m2c_base || !v2_sound_shadow_valid) return;
    uint16_t snd_seg = *(uint16_t*)(real_ds + 0x2E6B);
    if (snd_seg == 0) return;
    uint8_t* real_snd = v2_m2c_base + (uint32_t)snd_seg * 16;
    static int snd_err = 0;
    for (uint32_t i = 0; i < V2_SOUND_SHADOW_SIZE && snd_err < 5; i += 2) {
        if (*(uint16_t*)(real_snd + i) != *(uint16_t*)(v2_vm_shadow_sound + i)) {
            printf("V2-SOUND: WARNING — DIFF at 0x%04X: real=0x%04X shadow=0x%04X (sound not implemented)\n",
                   (uint16_t)i, *(uint16_t*)(real_snd + i), *(uint16_t*)(v2_vm_shadow_sound + i));
            snd_err++;
        }
    }
}

// #14: Chunk buffer segment verify (0x2E77)
static void v2_vm_verify_chunk(uint8_t* real_ds) {
    if (!v2_m2c_base || !v2_chunk_shadow_valid) return;
    uint16_t chunk_seg = *(uint16_t*)(real_ds + 0x2E77);
    if (chunk_seg == 0) return;
    uint8_t* real_ch = v2_m2c_base + (uint32_t)chunk_seg * 16;
    static int ch_err = 0;
    for (uint32_t i = 0; i < V2_CHUNK_SHADOW_SIZE && ch_err < 10; i += 2) {
        if (*(uint16_t*)(real_ch + i) != *(uint16_t*)(v2_vm_shadow_chunk + i)) {
            printf("V2-CHUNK: DIFF at 0x%04X: real=0x%04X shadow=0x%04X\n",
                   (uint16_t)i, *(uint16_t*)(real_ch + i), *(uint16_t*)(v2_vm_shadow_chunk + i));
            ch_err++;
        }
    }
}

// Combined: verify all segments at once
void v2_vm_verify_all_segments(uint16_t ds_val) {
    if (!v2_vm_real_ds_ptr) return;
    v2_vm_verify_tilegfx(v2_vm_real_ds_ptr);
    v2_vm_verify_fs_vs_es(v2_vm_real_ds_ptr);
    v2_vm_verify_sprite_bases(v2_vm_shadow_ds);
    v2_vm_verify_animdata(v2_vm_real_ds_ptr);
    v2_vm_verify_gs(v2_vm_real_ds_ptr);
    v2_vm_verify_sound(v2_vm_real_ds_ptr);
    v2_vm_verify_chunk(v2_vm_real_ds_ptr);
}

// #9: Object cross-reference verify — parent/linked object pointers valid
static void v2_vm_verify_object_refs(uint8_t* shadow) {
    static int ref_err = 0;
    uint16_t table_end = *(uint16_t*)(shadow + 0x372);
    for (uint16_t si = 0; (int16_t)si < (int16_t)table_end && ref_err < 10; si += 2) {
        if (*(uint16_t*)(shadow + si + 0x1355) == 0) continue;
        // Check linked object (0x1995)
        uint16_t linked = *(uint16_t*)(shadow + si + 0x1995);
        if (linked != 0 && linked < table_end) {
            if (*(uint16_t*)(shadow + linked + 0x1355) == 0 && ref_err < 10) {
                printf("V2-REF: obj=%d linked=0x%04X points to dead object\n", si, linked);
                ref_err++;
            }
        }
        // Check parent (0x1805)
        uint16_t parent = *(uint16_t*)(shadow + si + 0x1805);
        if (parent != 0xFFFF && parent < table_end && parent != si) {
            if (*(uint16_t*)(shadow + parent + 0x1355) == 0 && ref_err < 10) {
                printf("V2-REF: obj=%d parent=0x%04X points to dead object\n", si, parent);
                ref_err++;
            }
        }
    }
}

// v2 full frame render helper — callable from anywhere in game loop.
// Reads from shadow DS/tilemap/sprites — no real DS dependency.
// ds_val parameter passed through for API compatibility but ignored under V2_RENDER_FROM_SHADOW.
// v2_current_ds_val defined at line ~157 (forward decl area)

static void v2_do_render() {
    v2_draw_tiles(v2_current_ds_val);
    v2_draw_sprites(v2_current_ds_val);
    v2_sub_1E0C7(v2_vm_shadow_ds);  // CALLF sub_1E0C7 (DS side effects before UI render)
    v2_draw_ui(v2_current_ds_val);

    // Debug: count active sprites in shadow DS
    static int dbg_cnt = 0;
    dbg_cnt++;
    if (dbg_cnt <= 5 || (dbg_cnt % 120 == 0 && dbg_cnt <= 3600)) {
        int active = 0;
        for (int di = 0xFE; di >= 0; di -= 2) {
            uint16_t f = *(uint16_t*)(v2_vm_shadow_ds + di + 0x44D);
            if (f & 0x8000) active++;
        }
        printf("V2-RENDER[%d]: lvl=%d spr=%d vp=(%d,%d) scr=(%d,%d)\n",
               dbg_cnt, v2_current_level, active,
               *(int16_t*)(v2_vm_shadow_ds + 0x44),
               *(int16_t*)(v2_vm_shadow_ds + 0x46),
               *(uint16_t*)(v2_vm_shadow_ds + 0x2581),
               *(uint16_t*)(v2_vm_shadow_ds + 0x257F));
    }
}

// ======================================================================
// sub_16775: VGA page flip (eip 0x6775-0x67FE)
// ======================================================================
// Original: compute VGA CRTC start address from viewport + scroll + page offset.
// Write CRTC registers (OUT 0x3D4), save viewport/scroll state, set word_3287C=1.
// sub_1797b (render callback) fires from VGA vsync: pixel panning + DEC word_3287C + palette.
// v2: word_3287C=1 triggers dispatch in next v2_sub_10130 call.
// ======================================================================
static void v2_sub_16775(uint8_t* s) {
    // VGA page flip registers:
    // PUSHF; CLI;
    // OUT(0x3D4, 0x0D | (bl << 8));  // CRTC start address low
    // OUT(0x3D4, 0x0C | (bh << 8));  // CRTC start address high
    // POPF;
    // ds:0x92EE = x_low_bits * 2;  // pixel panning value
    // sub_1797b (render callback, called from vsync interrupt):
    //   OUT(0x3C0, 0x33);            // attribute controller: pixel panning register
    //   OUT(0x3C0, byte_317CE);      // pixel panning value
    // DS writes (verified from set_display_memory_addr, lines 1193-1197):
    *(uint16_t*)(s + 0xA39C) = 1;                                     // word_3287C = 1
    *(uint16_t*)(s + 0x257B) = *(uint16_t*)(s + 0x44);               // save viewport X
    *(uint16_t*)(s + 0x257D) = *(uint16_t*)(s + 0x46);               // save viewport Y
    *(uint16_t*)(s + 0x92EF) = *(uint16_t*)(s + 0x257F);             // scroll row state
    *(uint16_t*)(s + 0x92F1) = *(uint16_t*)(s + 0x2581);             // scroll col state
    // VGA buffer swap (equivalent of page flip)
    v2_swap_render_buf();
}

static void v2_do_render_and_swap() {
    v2_do_render();
    v2_sub_16775(v2_vm_shadow_ds);
}

// Exact replica of original main loop order:
//   PRE-VM: sub_12352..sub_10813(+sub_1086f)..sub_1673c
//   VM:     sub_14207
//   POST-VM: sub_1386b..sub_1064b, sub_12fc6, sub_10130
//   RENDER: sub_1DE05
//   POST-RENDER: sub_165aa, sub_16661, sub_1406d, sub_1DD9C, sub_1C8F1
//   UI: sub_1E0C7
//   PAGE FLIP: sub_16775
void v2_run_animation_vm(uint16_t ds_val) {
    if (!v2_m2c_base || !myDrawInfo_v2) return;
#ifdef V2_RENDER_FROM_SHADOW
    v2_vm_in_frame = true;
#endif

    v2_vm_init_table();

    uint8_t* ds = v2_m2c_base + ((uint32_t)ds_val << 4);

    // Reset per-frame shadow state from current game state
    v2_vm_reset_frame_state(ds);
    v2_current_ds_val = ds_val;

    // One-time startup: load ds_static.bin + sub_12948..sub_108b8.
    v2_startup(v2_vm_shadow_ds);

    // Level change detection: if level changed, run full v2 init.
    // Original: sub_10138 → sub_11080 when level transition happens.
    {
        uint16_t cur_level = *(uint16_t*)(v2_vm_shadow_ds + 0x25C9);
        if (cur_level != v2_current_level) {
            printf("V2: level change %d → %d, running v2_sub_11080\n",
                   v2_current_level, cur_level);
            v2_sub_11080(v2_vm_shadow_ds);
            v2_current_level = cur_level;
#ifdef V2_RENDER_FROM_SHADOW
            v2_vm_in_frame = false;
#endif
            return;
        }
    }

    // ====== PRE-VM (eip 0x001E..0x0036) ======
    v2_game_loop_pre_vm(v2_vm_shadow_ds, ds_val);
    // sub_10813 + sub_1086f are inside v2_game_loop_pre_vm
    // sub_1086f calls v2_do_render_and_swap for inter-command render

    // ====== VM (eip 0x0039) ======
    // Re-read ds:0x372 each iteration — VM opcode 0x14 creates objects
    for (uint16_t si = 0; si < *(uint16_t*)(v2_vm_shadow_ds + 0x372); si += 2) {
        v2_vm_execute_object(v2_vm_shadow_ds, si);
    }

    // ====== POST-VM (eip 0x003C..0x004E) ======
    v2_game_loop_post_vm(v2_vm_shadow_ds);
    // sub_12fc6 (resource tick) — NOP for v2
    v2_sub_10130(v2_vm_shadow_ds); // sub_10130: VGA vsync wait

    // ====== RENDER (eip 0x0051..0x0056) ======
    // Original: CALLF sub_1DE05 (seg003 dirty rect update)
    // sub_1de05: updates VGA dirty rectangles — for v2: full-frame rendering instead.
    // sub_1de05_dirty_update_position(NULL); // seg003 — commented for v2
    v2_do_render();

    // ====== POST-RENDER (eip 0x0056..0x006C) ======
    v2_game_loop_post_render(v2_vm_shadow_ds);
    // CALLF sub_1DD9C (seg003 sprite rendering to VGA)
    // sub_1dd9c_main_render_loop_with_state(_state); // seg003 — commented for v2
    // CALLF sub_1C8F1 (seg003 flagged/door tile rendering) — v2 equivalent:
    // sub_1c8f1_door_rendering_with_state(_state); // seg003 — commented for v2
    v2_sub_1C8F1(v2_vm_shadow_ds, 0xFFFE); v2_draw_flagged_tiles(v2_current_ds_val);
    // CALLF sub_1E0C7 (seg003 UI glyph rendering)
    v2_sub_1E0C7(v2_vm_shadow_ds);
    v2_draw_ui(v2_current_ds_val);

    // ====== PAGE FLIP 1 (eip 0x0071) ======
    v2_sub_16775(v2_vm_shadow_ds);

    // ====== POST-FLIP 1 (eip 0x0074..0x00B8) — game logic between render passes ======
    // sub_12e16: viking death check — find next alive viking, set game over if all dead
    {
        uint8_t* s = v2_vm_shadow_ds;
        // sub_12e16 + sub_12e2d: exact 2-step search
        uint16_t si = *(uint16_t*)(s + 0x3C2);
        uint16_t start_si = si;
        if (si < 6 && (int16_t)*(uint16_t*)(s + si + 0x16ED) < 0) {
            // Current dead — set blink
            *(uint16_t*)(s + 0x34E) = 5;
            *(uint16_t*)(s + 0x350) = 5;
            // Step 1: advance, check (skip if wrapped to start)
            si += 2; if (si >= 6) si = 0;
            if (si == start_si) goto step2; // wrapped → skip to step 2
            if ((int16_t)*(uint16_t*)(s + si + 0x16ED) >= 0) goto done_search; // alive
        step2:
            // Step 2: advance again, check
            si += 2; if (si >= 6) si = 0;
            if (si == start_si) { si = 0xFFFF; goto done_search; } // all dead
            if ((int16_t)*(uint16_t*)(s + si + 0x16ED) >= 0) goto done_search; // alive
            si = 0xFFFF; // dead
        done_search:;
        }
        *(uint16_t*)(s + 0x3C2) = si;
        if (si == 0xFFFF) {
            *(uint16_t*)(s + 0x334) |= 2; // game over flag
        }

        // sub_15530: collision detection pass 2 (ds:0x390 = 0xFFFF)
        *(uint16_t*)(s + 0x390) = 0xFFFF;
        {
            uint16_t te = *(uint16_t*)(s + 0x372);
            for (uint16_t si2 = 0; (int16_t)si2 < (int16_t)te; si2 += 2) {
                if (*(uint16_t*)(s + si2 + 0x1355) == 0) continue;
                v2_run_collision_vm(s, si2);
            }
        }

        // sub_10704: scroll clamp — reads scroll speeds from ds:0x3D8-0x3DE,
        // looks up pixel amount from ds:[si*2+0x2B82], calls scroll functions.
        // sub_17496/sub_1746c/loc_174E9/loc_174BF: clamp viewport + update scroll state.
        // DS writes: ds:0x44/0x46, ds:0x257F/0x2581, ds:0x34E/0x350
        // VGA OUT: CRTC start address (0x3D4) — commented for v2
        {
            // Helper: 4 scroll directions
            auto scroll_left = [&](uint16_t amount) { // sub_17496
                if (*(uint16_t*)(s + 0x394) != 0) return;
                int16_t ax = (int16_t)*(uint16_t*)(s + 0x44) - (int16_t)amount;
                if (ax < 0) ax = 0;
                uint16_t dx = *(uint16_t*)(s + 0x44) - (uint16_t)ax;
                *(uint16_t*)(s + 0x44) = (uint16_t)ax;
                *(uint16_t*)(s + 0x257F) = (uint16_t)ax >> 3;
                *(uint16_t*)(s + 0x34E) = dx;
                // No VGA OUT in sub_17496 — CRTC programming is in sub_16775
            };
            auto scroll_right = [&](uint16_t amount) { // sub_1746c
                if (*(uint16_t*)(s + 0x394) != 0) return;
                uint16_t ax = *(uint16_t*)(s + 0x44) + amount;
                uint16_t limit = *(uint16_t*)(s + 0x25A4);
                if (ax >= limit) ax = limit;
                uint16_t dx = ax - *(uint16_t*)(s + 0x44);
                *(uint16_t*)(s + 0x44) = ax;
                *(uint16_t*)(s + 0x257F) = ax >> 3;
                *(uint16_t*)(s + 0x34E) = dx;
                // No VGA OUT here — CRTC programming is in sub_16775
            };
            auto scroll_up = [&](uint16_t amount) { // loc_174E9
                if (*(uint16_t*)(s + 0x396) != 0) return;
                int16_t ax = (int16_t)*(uint16_t*)(s + 0x46) - (int16_t)amount;
                if (ax < 0) ax = 0;
                uint16_t dx = *(uint16_t*)(s + 0x46) - (uint16_t)ax;
                *(uint16_t*)(s + 0x46) = (uint16_t)ax;
                *(uint16_t*)(s + 0x2581) = (uint16_t)ax >> 3;
                *(uint16_t*)(s + 0x350) = dx;
                // No VGA OUT here — CRTC programming is in sub_16775
            };
            auto scroll_down = [&](uint16_t amount) { // loc_174BF
                if (*(uint16_t*)(s + 0x396) != 0) return;
                uint16_t ax = *(uint16_t*)(s + 0x46) + amount;
                uint16_t limit = *(uint16_t*)(s + 0x25A6);
                if (ax >= limit) ax = limit;
                uint16_t dx = ax - *(uint16_t*)(s + 0x46);
                *(uint16_t*)(s + 0x46) = ax;
                *(uint16_t*)(s + 0x2581) = ax >> 3;
                *(uint16_t*)(s + 0x350) = dx;
                // No VGA OUT here — CRTC programming is in sub_16775
            };
            // sub_10704: lookup table at ds:[si*2+0x2B82]
            auto do_scroll = [&](uint16_t table_off) {
                uint16_t v;
                v = *(uint16_t*)(s + 0x3D8); // word_288B8 (scroll left speed)
                if (v != 0) { scroll_left(*(uint16_t*)(s + v * 2 + table_off)); }
                else {
                    v = *(uint16_t*)(s + 0x3DA); // word_288BA (scroll right speed)
                    if (v != 0) scroll_right(*(uint16_t*)(s + v * 2 + table_off));
                }
                v = *(uint16_t*)(s + 0x3DE); // word_288BE (scroll up speed)
                if (v != 0) { scroll_up(*(uint16_t*)(s + v * 2 + table_off)); }
                else {
                    v = *(uint16_t*)(s + 0x3DC); // word_288BC (scroll down speed)
                    if (v != 0) scroll_down(*(uint16_t*)(s + v * 2 + table_off));
                }
            };
            do_scroll(0x2B82); // sub_10704 (eip 0x007A)
        }

        // sub_12fcb (eip 0x007D): per-object sub-sprite position update type 2.
        // sub_12fc6/sub_12fcb/sub_12fd0: per-object sub-sprite position update.
        // off_30BC0 dispatch: sub_1227e(bx=0), sub_122c0(bx=2), sub_122f3(bx=4)
        // All compute delta/3 with different rounding. Apply to all sub-sprites.
        {
            // Delta transform functions (exact replicas of sub_1227e/sub_122c0/sub_122f3)
            auto delta_type0 = [](int16_t d) -> int16_t { // sub_1227e: delta - (|d|/3)*2 ± round
                if (d == 0) return 0;
                int16_t a = (d < 0) ? -d : d;
                int16_t q = a / 3, r = a % 3;
                int16_t red = q * 2 + (r >= 2 ? 1 : 0);
                return (d < 0) ? (d + red) : (d - red);
            };
            auto delta_type1 = [](int16_t d) -> int16_t { // sub_122c0: d/3 with round
                if (d == 0) return 0;
                int16_t a = (d < 0) ? -d : d;
                int16_t q = a / 3, r = a % 3;
                int16_t res = q + (r >= 2 ? 1 : 0);
                return (d < 0) ? -res : res;
            };
            auto delta_type2 = [](int16_t d) -> int16_t { // sub_122f3: d/3 WITHOUT rounding
                if (d == 0) return 0;
                int16_t a = (d < 0) ? -d : d;
                int16_t q = a / 3;
                return (d < 0) ? -(int16_t)q : q;
            };

            // sub_12fe5: per-object sub-sprite update
            auto sub_12fe5 = [&](uint16_t di_obj, int bx_type) {
                if (*(uint16_t*)(s + di_obj + 0x1355) == 0) return;
                if (*(uint16_t*)(s + di_obj + 0x1AD5) == 0) return;
                int16_t dy = (int16_t)(*(uint16_t*)(s + di_obj + 0x1765) - *(uint16_t*)(s + di_obj + 0x13CD));
                int16_t dx_val = (int16_t)(*(uint16_t*)(s + di_obj + 0x173D) - *(uint16_t*)(s + di_obj + 0x13A5));
                int16_t ty, tx;
                if (bx_type == 0) { ty = delta_type0(dy); tx = delta_type0(dx_val); }
                else { ty = delta_type1(dy); tx = delta_type1(dx_val); }
                if (tx == 0 && ty == 0) return;
                uint16_t ss_end = *(uint16_t*)(s + di_obj + 0x1AAD);
                for (uint16_t si2 = *(uint16_t*)(s + di_obj + 0x1A85); (int16_t)si2 < (int16_t)ss_end; si2 += 2) {
                    *(uint16_t*)(s + si2 + 0x64D) += (uint16_t)tx;
                    *(uint16_t*)(s + si2 + 0x74D) += (uint16_t)ty;
                    *(uint16_t*)(s + si2 + 0x114D) = 0x202;
                }
            };

            // sub_12fc6(bx=0): already called in POST-VM as sub_12fc6 viking 1
            // Called at eip 0x004B, which IS in our v2_game_loop_post_vm.
            // sub_12fcb(bx=2): viking 2 — called at eip 0x007D (post-flip 1)
            {
                uint16_t te = *(uint16_t*)(s + 0x372);
                for (int16_t di2 = te - 2; di2 >= 0; di2 -= 2)
                    sub_12fe5(di2, 2);
            }
        }
        // sub_12d2c: invincibility/flash timer (called from eip 0x0080)
        // Timer 1: DEC ds:0x3A2, if nonzero XOR ds:0x39E with ds:0x39A; if zero ds:0x39E=0
        // Timer 2: DEC ds:0x3A4, if nonzero XOR ds:0x3A0 with ds:0x39C; if zero ds:0x3A0=0
        if (*(uint16_t*)(s + 0x3A2) != 0) {
            *(uint16_t*)(s + 0x3A2) -= 1;
            if (*(uint16_t*)(s + 0x39A) != 0)
                *(uint16_t*)(s + 0x39E) ^= *(uint16_t*)(s + 0x39A);
            else
                *(uint16_t*)(s + 0x39E) = 0;
        } else {
            *(uint16_t*)(s + 0x39E) = 0;
        }
        if (*(uint16_t*)(s + 0x3A4) != 0) {
            *(uint16_t*)(s + 0x3A4) -= 1;
            if (*(uint16_t*)(s + 0x39C) != 0)
                *(uint16_t*)(s + 0x3A0) ^= *(uint16_t*)(s + 0x39C);
            else
                *(uint16_t*)(s + 0x3A0) = 0;
        } else {
            *(uint16_t*)(s + 0x3A0) = 0;
        }

        // ====== PASS 2: RENDER 2 + POST-RENDER 2 (eip 0x0086..0x00A6) ======
        // CALLF sub_1DE05 (eip 0x0086)
        v2_sub_1DE05(s);
        // sub_165aa + sub_16661 + sub_1406d (eip 0x008B..0x0091)
        v2_game_loop_post_render(s);
        // CALLF sub_1DD9C (eip 0x0094)
        v2_sub_1DD9C(s);
        // sub_1C8F1 (eip 0x0099) — flagged tiles
        v2_sub_1C8F1(v2_vm_shadow_ds, 0xFFFE); v2_draw_flagged_tiles(v2_current_ds_val);
        // sub_1E0C7 (eip 0x00A1) — UI
        v2_sub_1E0C7(v2_vm_shadow_ds);
        v2_draw_ui(v2_current_ds_val);
        // sub_16775 (eip 0x00A6) — page flip 2
        v2_sub_16775(s);

        // ====== POST-FLIP 2 (eip 0x00A9..0x00B8) ======
        // sub_10753 (eip 0x00A9): scroll clamp 2 — same as sub_10704 but table at 0x2B80
        {
            auto scroll_left2 = [&](uint16_t amount) {
                if (*(uint16_t*)(s + 0x394) != 0) return;
                int16_t ax = (int16_t)*(uint16_t*)(s + 0x44) - (int16_t)amount;
                if (ax < 0) ax = 0;
                uint16_t dx = *(uint16_t*)(s + 0x44) - (uint16_t)ax;
                *(uint16_t*)(s + 0x44) = (uint16_t)ax;
                *(uint16_t*)(s + 0x257F) = (uint16_t)ax >> 3;
                *(uint16_t*)(s + 0x34E) = dx;
            };
            auto scroll_right2 = [&](uint16_t amount) {
                if (*(uint16_t*)(s + 0x394) != 0) return;
                uint16_t ax = *(uint16_t*)(s + 0x44) + amount;
                uint16_t limit = *(uint16_t*)(s + 0x25A4);
                if (ax >= limit) ax = limit;
                uint16_t dx = ax - *(uint16_t*)(s + 0x44);
                *(uint16_t*)(s + 0x44) = ax;
                *(uint16_t*)(s + 0x257F) = ax >> 3;
                *(uint16_t*)(s + 0x34E) = dx;
            };
            auto scroll_up2 = [&](uint16_t amount) {
                if (*(uint16_t*)(s + 0x396) != 0) return;
                int16_t ax = (int16_t)*(uint16_t*)(s + 0x46) - (int16_t)amount;
                if (ax < 0) ax = 0;
                uint16_t dx = *(uint16_t*)(s + 0x46) - (uint16_t)ax;
                *(uint16_t*)(s + 0x46) = (uint16_t)ax;
                *(uint16_t*)(s + 0x2581) = (uint16_t)ax >> 3;
                *(uint16_t*)(s + 0x350) = dx;
            };
            auto scroll_down2 = [&](uint16_t amount) {
                if (*(uint16_t*)(s + 0x396) != 0) return;
                uint16_t ax = *(uint16_t*)(s + 0x46) + amount;
                uint16_t limit = *(uint16_t*)(s + 0x25A6);
                if (ax >= limit) ax = limit;
                uint16_t dx = ax - *(uint16_t*)(s + 0x46);
                *(uint16_t*)(s + 0x46) = ax;
                *(uint16_t*)(s + 0x2581) = ax >> 3;
                *(uint16_t*)(s + 0x350) = dx;
            };
            uint16_t v;
            v = *(uint16_t*)(s + 0x3D8);
            if (v != 0) scroll_left2(*(uint16_t*)(s + v * 2 + 0x2B80));
            else { v = *(uint16_t*)(s + 0x3DA); if (v != 0) scroll_right2(*(uint16_t*)(s + v * 2 + 0x2B80)); }
            v = *(uint16_t*)(s + 0x3DE);
            if (v != 0) scroll_up2(*(uint16_t*)(s + v * 2 + 0x2B80));
            else { v = *(uint16_t*)(s + 0x3DC); if (v != 0) scroll_down2(*(uint16_t*)(s + v * 2 + 0x2B80)); }
        }

        // sub_13c0c (eip 0x00AC): viewport bounds update + object visibility marking
        // Sets ds:0x34/0x36/0x38/0x3A from viewport position (ds:0x44/0x46).
        // sub_13c0c exact: X uses unclamped ax for ds:0x36, Y clamps ax for ds:0x3A
        {
            uint16_t ax_x = *(uint16_t*)(s + 0x44) - 0x10;
            if ((int16_t)ax_x >= 0)
                *(uint16_t*)(s + 0x34) = ax_x;
            else
                *(uint16_t*)(s + 0x34) = 0;
            *(uint16_t*)(s + 0x36) = ax_x + 0x160; // unclamped ax
            uint16_t ax_y = *(uint16_t*)(s + 0x46) - 0x10;
            if ((int16_t)ax_y < 0) ax_y = 0;
            *(uint16_t*)(s + 0x38) = ax_y;
            *(uint16_t*)(s + 0x3A) = ax_y + 0xD0;
            // Object visibility check — mark out-of-viewport objects with flag 0x200
            // Original: OR [si+1585h], 200h for objects outside viewport bounds.
            // Flag 0x200 triggers animation VM re-evaluation.
            uint16_t te = *(uint16_t*)(s + 0x372);
            for (uint16_t si_v = 6; (int16_t)si_v < (int16_t)te; si_v += 2) {
                if (*(uint16_t*)(s + si_v + 0x1355) == 0) continue;
                if (*(uint16_t*)(s + si_v + 0x1585) & 0x800) continue; // permanent → skip
                uint16_t ox = *(uint16_t*)(s + si_v + 0x173D);
                uint16_t oy = *(uint16_t*)(s + si_v + 0x1765);
                uint16_t obx = *(uint16_t*)(s + si_v + 0x14BD);
                uint16_t oby = *(uint16_t*)(s + si_v + 0x1495);
                bool outside = false;
                if ((int16_t)(ox + obx - *(uint16_t*)(s + 0x34)) < 0) outside = true;
                else if ((int16_t)(ox - obx - *(uint16_t*)(s + 0x36)) >= 0) outside = true;
                else if ((int16_t)(oy + oby - *(uint16_t*)(s + 0x38)) < 0) outside = true;
                else if ((int16_t)(oy - oby - *(uint16_t*)(s + 0x3A)) >= 0) outside = true;
                if (outside)
                    *(uint16_t*)(s + si_v + 0x1585) |= 0x200; // mark for animation update
            }
        }

        // sub_12fd0(bx=4): HUD viking 3 sub-sprite update (eip 0x00AF)
        {
            // sub_122f3: simple d/3 WITHOUT rounding (unlike sub_122c0 which rounds up at rem>=2)
            auto delta_type2 = [](int16_t d) -> int16_t {
                if (d == 0) return 0;
                int16_t a = (d < 0) ? -d : d;
                int16_t q = a / 3;
                return (d < 0) ? -(int16_t)q : q;
            };
            uint16_t te = *(uint16_t*)(s + 0x372);
            for (int16_t di3 = te - 2; di3 >= 0; di3 -= 2) {
                if (*(uint16_t*)(s + di3 + 0x1355) == 0) continue;
                if (*(uint16_t*)(s + di3 + 0x1AD5) == 0) continue;
                int16_t dy = (int16_t)(*(uint16_t*)(s + di3 + 0x1765) - *(uint16_t*)(s + di3 + 0x13CD));
                int16_t dx_v = (int16_t)(*(uint16_t*)(s + di3 + 0x173D) - *(uint16_t*)(s + di3 + 0x13A5));
                int16_t ty = delta_type2(dy), tx = delta_type2(dx_v);
                if (tx == 0 && ty == 0) continue;
                uint16_t ss_end = *(uint16_t*)(s + di3 + 0x1AAD);
                for (uint16_t si2 = *(uint16_t*)(s + di3 + 0x1A85); (int16_t)si2 < (int16_t)ss_end; si2 += 2) {
                    *(uint16_t*)(s + si2 + 0x64D) += (uint16_t)tx;
                    *(uint16_t*)(s + si2 + 0x74D) += (uint16_t)ty;
                    *(uint16_t*)(s + si2 + 0x114D) = 0x202;
                }
            }
        }

        // sub_11792: HUD full update (if flag &1 and level != 0x2C)
        // Calls sub_120ff (healthbar), sub_12199 (items), loc_1205b (?), sub_11b0b (portrait)
        // For v2: HUD rendered each frame by v2_draw_ui, state in shadow DS.
        if ((s[0x25CF] & 1) && *(uint16_t*)(s + 0x25AD) != 0x2C) {
            // sub_120ff: healthbar state tracking + rendering (3 vikings)
            // For each viking: compare current health state with previous,
            // if changed: update previous + render via v2_draw_hud_healthbar.
            // Viking 1 (ds:0x0435/0x043B)
            {
                uint16_t prev = *(uint16_t*)(s + 0x0435);
                *(uint16_t*)(s + 0x043B) = prev;
                int16_t health_val = (int16_t)*(uint16_t*)(s + 0x16ED); // word_29BCD (obj0 code_seg)
                uint16_t ax;
                if (health_val < 0) ax = 2;
                else if (*(uint16_t*)(s + 0x3C2) != 0) ax = 1;
                else ax = 0;
                *(uint16_t*)(s + 0x0435) = ax;
                if (ax != prev)
                    v2_draw_hud_healthbar(v2_current_ds_val, ax, 0, 0);
            }
            // Viking 2 (ds:0x0437/0x043D)
            {
                uint16_t prev = *(uint16_t*)(s + 0x0437);
                *(uint16_t*)(s + 0x043D) = prev;
                int16_t health_val = (int16_t)*(uint16_t*)(s + 0x16EF);
                uint16_t ax;
                if (health_val < 0) ax = 2;
                else if (*(uint16_t*)(s + 0x3C2) != 2) ax = 1;
                else ax = 0;
                *(uint16_t*)(s + 0x0437) = ax;
                if (ax != prev)
                    v2_draw_hud_healthbar(v2_current_ds_val, ax, 1, 1);
            }
            // Viking 3 (ds:0x0439/0x043F)
            {
                uint16_t prev = *(uint16_t*)(s + 0x0439);
                *(uint16_t*)(s + 0x043F) = prev;
                int16_t health_val = (int16_t)*(uint16_t*)(s + 0x16F1);
                uint16_t ax;
                if (health_val < 0) ax = 2;
                else if (*(uint16_t*)(s + 0x3C2) != 4) ax = 1;
                else ax = 0;
                *(uint16_t*)(s + 0x0439) = ax;
                if (ax != prev)
                    v2_draw_hud_healthbar(v2_current_ds_val, ax, 2, 2);
            }
            // sub_12199: item/HUD redraw — reads current items and re-renders
            // DS writes: [di+0x3FC] = [di+0x3E4] (copy current→previous for 12 entries)
            for (uint16_t di2 = 0; di2 < 0x18; di2 += 2) {
                uint16_t item = *(uint16_t*)(s + di2 + 0x3E4);
                if (item != *(uint16_t*)(s + di2 + 0x3FC)) {
                    *(uint16_t*)(s + di2 + 0x3FC) = item;
                    v2_draw_hud_item(v2_current_ds_val, di2, item);
                }
            }
            // loc_1205b: HUD selector sync (same logic as sub_120d1 but with change detection)
            // Viking 1: compare word_288F4 (ds:0x414) vs word_288FA (ds:0x41A)
            if (*(uint16_t*)(s + 0x0414) != *(uint16_t*)(s + 0x041A)) {
                // Draw OLD selector (clear), then update + draw new
                uint16_t old_di = *(uint16_t*)(s + 0x041A) * 2;
                v2_draw_hud_item(v2_current_ds_val, old_di, *(uint16_t*)(s + old_di + 0x3E4));
                *(uint16_t*)(s + 0x041A) = *(uint16_t*)(s + 0x0414);
                v2_draw_hud_selector(v2_current_ds_val, *(uint16_t*)(s + 0x0414) * 2);
            }
            // Viking 2
            if (*(uint16_t*)(s + 0x0416) != *(uint16_t*)(s + 0x041C)) {
                uint16_t old_di = (*(uint16_t*)(s + 0x041C) + 4) * 2;
                v2_draw_hud_item(v2_current_ds_val, old_di, *(uint16_t*)(s + old_di + 0x3E4));
                *(uint16_t*)(s + 0x041C) = *(uint16_t*)(s + 0x0416);
                v2_draw_hud_selector(v2_current_ds_val, (*(uint16_t*)(s + 0x0416) + 4) * 2);
            }
            // Viking 3
            if (*(uint16_t*)(s + 0x0418) != *(uint16_t*)(s + 0x041E)) {
                uint16_t old_di = (*(uint16_t*)(s + 0x041E) + 8) * 2;
                v2_draw_hud_item(v2_current_ds_val, old_di, *(uint16_t*)(s + old_di + 0x3E4));
                *(uint16_t*)(s + 0x041E) = *(uint16_t*)(s + 0x0418);
                v2_draw_hud_selector(v2_current_ds_val, (*(uint16_t*)(s + 0x0418) + 8) * 2);
            }
            // sub_11b0b: portrait/sound state sync (3 vikings). Exact replica.
            // Viking 1: compare ds:0x429 vs ds:0x42F AND ds:0x15AD vs ds:0x423
            if (*(uint16_t*)(s + 0x0429) != *(uint16_t*)(s + 0x042F) ||
                *(uint16_t*)(s + 0x15AD) != *(uint16_t*)(s + 0x0423)) {
                uint16_t ps = *(uint16_t*)(s + 0x15AD);
                if (*(uint16_t*)(s + 0x0429) != 0) ps += 4;
                v2_draw_hud_portrait(v2_current_ds_val, 0, ps);
                *(uint16_t*)(s + 0x042F) = *(uint16_t*)(s + 0x0429);
                *(uint16_t*)(s + 0x0423) = *(uint16_t*)(s + 0x15AD);
            }
            // Viking 2
            if (*(uint16_t*)(s + 0x042B) != *(uint16_t*)(s + 0x0431) ||
                *(uint16_t*)(s + 0x15AF) != *(uint16_t*)(s + 0x0425)) {
                uint16_t ps = *(uint16_t*)(s + 0x15AF);
                if (*(uint16_t*)(s + 0x042B) != 0) ps += 4;
                v2_draw_hud_portrait(v2_current_ds_val, 2, ps);
                *(uint16_t*)(s + 0x0431) = *(uint16_t*)(s + 0x042B);
                *(uint16_t*)(s + 0x0425) = *(uint16_t*)(s + 0x15AF);
            }
            // Viking 3
            if (*(uint16_t*)(s + 0x042D) != *(uint16_t*)(s + 0x0433) ||
                *(uint16_t*)(s + 0x15B1) != *(uint16_t*)(s + 0x0427)) {
                uint16_t ps = *(uint16_t*)(s + 0x15B1);
                if (*(uint16_t*)(s + 0x042D) != 0) ps += 4;
                v2_draw_hud_portrait(v2_current_ds_val, 4, ps);
                *(uint16_t*)(s + 0x0433) = *(uint16_t*)(s + 0x042D);
                *(uint16_t*)(s + 0x0427) = *(uint16_t*)(s + 0x15B1);
            }
        }

        // sub_101be: palette animation timer — per-slot timer decrement + palette rotation
        // 8 slots (si=7→0). When timer hits 0: rotate palette in BOTH ds:0x8202 and ds:0x7F02.
        if (s[0x2583] != 0) { // byte_2AA63 — any animation enabled?
            for (int16_t si_pal = 7; si_pal >= 0; si_pal--) {
                uint8_t mask = s[(uint16_t)(si_pal - 0x6C44)];
                if (!(s[0x2583] & mask)) continue;
                if (s[si_pal + 0x258C] == 0) continue; // already expired
                s[si_pal + 0x258C]--;                   // DEC timer
                if (s[si_pal + 0x258C] != 0) continue;  // not yet 0
                // Timer just hit 0 — rotate palette
                uint8_t end_color = s[si_pal + 0x259C];
                uint8_t start_color = s[si_pal + 0x2594];
                // Rotate in both shaded (0x8202) and source (0x7F02) palettes
                auto rotate_palette = [&](uint16_t pal_base) {
                    uint16_t end_off = (uint16_t)end_color * 3 + pal_base;
                    uint16_t start_off = (uint16_t)start_color * 3 + pal_base;
                    if (end_color < start_color) {
                        // sub_10255: rotate left — save end, shift left, put at start-3
                        uint8_t save[3] = {s[end_off], s[end_off+1], s[end_off+2]};
                        uint16_t count = start_off - end_off;
                        memmove(s + end_off, s + end_off + 3, count);
                        s[end_off + count] = save[0];   // di after REP MOVSB = end_off + count
                        s[end_off + count + 1] = save[1];
                        s[end_off + count + 2] = save[2];
                    } else {
                        // sub_1020f: rotate right — save end, shift right, put at start
                        uint8_t save[3] = {s[end_off], s[end_off+1], s[end_off+2]};
                        uint16_t count = (end_color - start_color) * 3;
                        memmove(s + start_off + 3, s + start_off, count);
                        s[start_off] = save[0];
                        s[start_off+1] = save[1];
                        s[start_off+2] = save[2];
                    }
                };
                rotate_palette(0x8202); // shaded palette
                rotate_palette(0x7F02); // source palette
            }
            *(uint16_t*)(s + 0x7EFE) = 2; // word_303DE = 2 (request sub_10ffc in render callback)
        }
        // word_30C14 = 0 (eip 0x00DB — end of frame marker)
        *(uint16_t*)(s + 0x8734) = 0;

        // sub_108c8: sound crossfade management (eip 0x00E1). Exact replica.
        // ds:0x302 (word_287E2) & ds:0x304 (word_287E4) bit 15 → skip.
        {
            if (!((*(uint16_t*)(s + 0x302) & *(uint16_t*)(s + 0x304)) & 0x8000)) {
                // Part 1: crossfade check (byte_31684 ds:0x91A4)
                if (s[0x91A4] == 1) {
                    if (s[0x918B] == 1) {
                        // byte_3166B == 1 → not ready, skip to part 2
                    } else {
                        s[0x918B] = 0;   // byte_3166B = 0
                        s[0x304] ^= 1;   // word_287E4 ^= 1
                        if (s[0x304] & 1) {
                            // Toggle set → stop sounds on channel si=2..8
                            for (uint16_t si_s = 2; (int16_t)si_s < 0x0A; si_s += 2) {
                                uint16_t h = (uint16_t)(si_s - 0x66F4);
                                if (*(uint16_t*)(s + h) != 0xFFFF) {
                                    // sub_1C79F + sub_1C769 — AIL stop/release, skipped
                                    *(uint16_t*)(s + h) = 0xFFFF;
                                    *(uint16_t*)(s + (uint16_t)(si_s - 0x66EA)) = 0xFFFF;
                                }
                            }
                        }
                    }
                }
                // Part 2: music channel toggle (byte_3167E ds:0x919E)
                if (s[0x919E] == 1) {
                    s[0x919E] = 0;   // byte_3167E = 0
                    s[0x302] ^= 1;   // word_287E2 ^= 1
                    if (!(s[0x302] & 1)) {
                        // sub_176bd(si=0, ax=0, bx=ds:0x2E6B) — AIL play, skipped
                    } else if (!(*(uint16_t*)(s + 0x302) & 0x8000)) {
                        // Stop channel 0 sounds (si=0 only)
                        uint16_t h = (uint16_t)(0 - 0x66F4);
                        if (*(uint16_t*)(s + h) != 0xFFFF) {
                            *(uint16_t*)(s + h) = 0xFFFF;
                            *(uint16_t*)(s + (uint16_t)(0 - 0x66EA)) = 0xFFFF;
                        }
                    }
                }
            }
        }

        // sub_10350: level transition check (eip 0x00E4). Exact replica.
        // word_2A66F (ds:0x218F) != 0 → return (game busy/in transition)
        if (*(uint16_t*)(s + 0x218F) == 0) {
            // Check status bytes:
            // byte_31690(ds:0x91B0)==1 → trigger
            // OR byte_31684(ds:0x91A4)==1 AND (byte_31679(ds:0x9199)==1 OR byte_3165C(ds:0x917C)==1)
            bool trigger = false;
            if (s[0x91B0] == 1) trigger = true;
            else if (s[0x91A4] == 1) {
                if (s[0x9199] == 1 || s[0x917C] == 1) trigger = true;
            }
            if (trigger) {
                if (*(uint16_t*)(s + 0x3CC) == 0x8000 || (s[0x25CF] & 8)) {
                    // loc_10e35: cleanup + reload level
                    // sub_16546: clear VGA dirty state (VGA only, no DS writes)
                    // sub_1754c: sound cleanup (AIL exit — skipped for v2)
                    // INT 21h/49: free DOS memory segments — for v2: shadow buffers reloaded on init
                    // Then falls through to sub_11080 (level reload)
                    // For v2: trigger level reload
                    v2_sub_11080(s);
                } else {
                    // Normal level complete: palette transition + interactive UI
                    // DS writes before UI:
                    s[0x7F0B] = 0;                                   // byte ptr word_303EB = 0
                    s[0x7F0C] = 0;                                   // byte ptr word_303EB+1 = 0
                    s[0x7F0D] = 0;                                   // byte_303ED = 0
                    // OUT(0x3C8, 3); OUT(0x3C9, 0); OUT(0x3C9, 0); OUT(0x3C9, 0); — VGA color 3 = black

                    if (!(s[0x342] | s[0x343] | s[0x344])) {
                        // No shade active → sub_1450b first
                        v2_sub_1450b(s, 4, 4, 4);                   // sub_1450b(ax=4, si=4, di=4)
                    }
                    // sub_103ca: blocking palette transition UI
                    // sub_177bb(ax=0): stop music
                    // AIL: stop_xmidi_external(); — commented for v2
                    // OUT(0x3C8, 3); OUT(0x3C9, 3); OUT(0x3C9, 13); OUT(0x3C9, 12);
                    //   — VGA: set color 3 to {3,13,12}
                    s[0x7F0B] = 3;                                   // palette color 3 R
                    s[0x7F0C] = 13;                                  // palette color 3 G
                    s[0x7F0D] = 12;                                  // palette color 3 B
                    // loc_124a9: render transition text. Verified with seg000 sub_103ca.
                    // ax=3 → sub_12515(3) + sub_12529 + sub_12388 + loc_124c5
                    v2_sub_12515(s, 3);
                    { uint16_t bx_t = *(uint16_t*)(s + 0x2A);
                      v2_sub_12529(s, bx_t);
                      uint16_t si_t = 0x0D, di_t = 0x0C;
                      v2_sub_12388(s, si_t, di_t, 0);
                      v2_loc_124c5(s, si_t + 1, di_t + 1, bx_t); }
                    // sub_1265b: display password. ax=5 → sub_12515(5) + loc_124c5
                    v2_sub_12515(s, 5);
                    { uint16_t bx_p = *(uint16_t*)(s + 0x2A);
                      v2_sub_12529(s, bx_p);
                      v2_loc_124c5(s, 0x10 + 1, 0x0F + 1, bx_p); }
                    // sub_1241e × 4: draw password characters from ds:0x310-0x316
                    { uint16_t si_pw = 0x12, di_pw = 0x11;
                      v2_sub_1241e(s, (uint8_t)*(uint16_t*)(s + 0x310), si_pw, di_pw);
                      v2_sub_1241e(s, (uint8_t)*(uint16_t*)(s + 0x312), si_pw, di_pw);
                      v2_sub_1241e(s, (uint8_t)*(uint16_t*)(s + 0x314), si_pw, di_pw);
                      v2_sub_1241e(s, (uint8_t)*(uint16_t*)(s + 0x316), si_pw, di_pw); }
                    s[0x956B] = 1;                                       // byte_31A4B = 1
                    // sub_104a1: blocking input wait. Verified with seg000 lines 2447-2515.
                    // Sets word_28925=0x11, word_28923=1, byte_31A4B=1.
                    // Then blocking loop: sub_10130×3 + sub_1DE05 + sub_12352 + sub_10555/sub_105cb
                    // Exit on word_28898 bits 0x8000|0x1000 or byte_31661
                    *(uint16_t*)(s + 0x0445) = 0x11;                     // word_28925 (0x28925 - 0x284E0 = 0x445)
                    *(uint16_t*)(s + 0x0443) = 1;                        // word_28923 (0x28923 - 0x284E0 = 0x443)
                    // OUT(0x3C9, 0x3F); OUT(0x3C9, 0x3F); OUT(0x3C9, 0x3F); — VGA palette, commented
                    v2_sub_1E0C7(s);                                     // CALLF sub_1E0C7
                    v2_sub_16775(s);                                     // CALL sub_16775
                    // Blocking input wait loop
                    while (true) {
                        v2_sub_10130(s); v2_sub_10130(s); v2_sub_10130(s); // sub_10130 × 3
                        // sub_1DE05: dirty rect
                        v2_sub_1DE05(s);
                        // sub_12352: input
                        {
                            extern uint16_t v2_input_snapshot;
                            uint16_t ax = 0;
                            ax |= *(uint16_t*)(s + 0x86DE);
                            ax |= v2_input_snapshot;
                            *(uint16_t*)(s + 0x03B6) = ax;
                            uint16_t prev = *(uint16_t*)(s + 0x03BA);
                            *(uint16_t*)(s + 0x03B8) = (ax ^ prev) & ax;
                            *(uint16_t*)(s + 0x03BA) = ax;
                        }
                        // sub_105cb: check exit (word_28898 & 0x9000 or byte_31661)
                        if (*(uint16_t*)(s + 0x3B8) & 0x9000) break;
                        if (s[0x9181] != 0) break; // byte_31661
                        v2_do_render();
                        SDL_Delay(16);
                    }
                    // loc_104f0: post-exit. Verified with seg000 lines 2477-2515.
                    // sub_12352 (final input read)
                    {
                        extern uint16_t v2_input_snapshot;
                        uint16_t ax = 0;
                        ax |= *(uint16_t*)(s + 0x86DE);
                        ax |= v2_input_snapshot;
                        *(uint16_t*)(s + 0x03B6) = ax;
                        uint16_t prev = *(uint16_t*)(s + 0x03BA);
                        *(uint16_t*)(s + 0x03B8) = (ax ^ prev) & ax;
                        *(uint16_t*)(s + 0x03BA) = ax;
                        // if ax==0: OR word_28814, 2
                        if (ax == 0) *(uint16_t*)(s + 0x0334) |= 2;
                    }
                    // word_31A49=1, word_31DBC=0
                    *(uint16_t*)(s + 0x9569) = 1;
                    *(uint16_t*)(s + 0x98DC) = 0;
                    // Render pass 1: sub_10130 + sub_1DE05 + sub_165aa + sub_1DD9C + sub_1C8F1
                    v2_sub_10130(s);
                    v2_sub_1DE05(s);
                    v2_game_loop_post_render(s);
                    v2_sub_1DD9C(s);
                    v2_sub_1C8F1(v2_vm_shadow_ds, 0xFFFE); v2_draw_flagged_tiles(v2_current_ds_val);
                    // sub_1E0C7 + sub_16775 + sub_10130
                    v2_sub_1E0C7(s);
                    v2_draw_ui(v2_current_ds_val);
                    v2_sub_16775(s);
                    v2_sub_10130(s);
                    // Render pass 2: sub_1DE05 + sub_165aa + sub_1DD9C + sub_1C8F1
                    v2_sub_1DE05(s);
                    v2_game_loop_post_render(s);
                    v2_sub_1DD9C(s);
                    v2_sub_1C8F1(v2_vm_shadow_ds, 0xFFFE); v2_draw_flagged_tiles(v2_current_ds_val);
                    // sub_1E0C7 + sub_16775
                    v2_sub_1E0C7(s);
                    v2_draw_ui(v2_current_ds_val);
                    v2_sub_16775(s);
                    // word_31A49=0
                    *(uint16_t*)(s + 0x9569) = 0;
                    // sub_12816: clear glyph list
                    s[0x956B] = 0;
                    memset(s + 0x956C, 0, 0x1B8 * 2);

                    // After input: sub_1450b(ax=5, si=0x10, di=0x0F) + sub_14590
                    v2_sub_1450b(s, 5, 0x10, 0x0F);                 // save game state after input
                    // sub_14590: finalize save — similar to sub_1450b but different params
                    // ds:0x342 = saved_al<<1; ds:0x7EFD |= 2
                    // For v2: just set the flags
                    s[0x7EFD] |= 2;                                  // OR byte ptr ds:7EFDh, 2
                    *(uint16_t*)(s + 0x7EFE) = 4;
                    *(uint16_t*)(s + 0x7F00) = 0x8202;
                    // Load new level
                    v2_sub_11080(s);
                }
            }
        }
    }

    // ====== Verify ======
    // v2_vm_verify_game_loop called from seg000 AFTER original VM (line 1918) — correct timing.
    v2_vm_verify_object_consistency(v2_vm_shadow_ds);
    v2_vm_verify_page_state(v2_vm_shadow_ds);
    v2_vm_verify_object_refs(v2_vm_shadow_ds);

#ifdef V2_RENDER_FROM_SHADOW
    v2_vm_in_frame = false;
#endif
}

// ============================================================================
// Phase-based game loop: each phase called from seg000 at matching eip.
// This ensures ds:0x92F9 etc. are in sync with the original at every point.
// ============================================================================
// v2_frame_active declared earlier (before v2_game_loop_pre_vm which uses it)

void v2_phase_frame_begin(uint16_t ds_val) {
    if (!v2_m2c_base || !myDrawInfo_v2) return;
    // v2_input_snapshot set by seg000 right after orig sub_12352 reads input_keys
#ifdef V2_RENDER_FROM_SHADOW
    v2_vm_in_frame = true;
#endif
    v2_vm_init_table();
    uint8_t* ds = v2_m2c_base + ((uint32_t)ds_val << 4);
    v2_vm_reset_frame_state(ds);
    v2_current_ds_val = ds_val;

    // Level change detection — only for initial load.
    // Subsequent level transitions are driven by v2_phase_frame_end's end-of-frame check
    // (sets bit 0 of shadow[0x0334]) → PRE_VM sub_10138 → v2_sub_11080.
    // This matches the original game architecture.
    if (v2_current_level == 0xFFFF) {
        uint16_t target_level = *(uint16_t*)(v2_vm_shadow_ds + 0x25C9);
        printf("V2: initial level load → %d, running v2_sub_11080\n", target_level);
        *(uint16_t*)(v2_vm_shadow_ds + 0x25C9) = target_level;
        v2_sub_11080(v2_vm_shadow_ds);
        v2_current_level = target_level;
    }
    v2_frame_active = true;
}

void v2_phase_pre_vm(uint16_t ds_val) {
    if (!v2_frame_active) return;
    v2_game_loop_pre_vm(v2_vm_shadow_ds, ds_val);
    v2_hw_wp_drain(); // read HW watchpoint samples

    // Post-pre_vm DS compare: both orig and v2 have done their pre-VM functions.
    // Real DS = after orig sub_12352..sub_1673c. Shadow DS = after v2 pre_vm.
    static int pre_vm_frame = 0;
    pre_vm_frame++;
    if (v2_vm_real_ds_ptr) {
        uint8_t* real = v2_vm_real_ds_ptr;
        uint8_t* shad = v2_vm_shadow_ds;
        int ds_diffs = 0;
        for (uint32_t i = 0; i < 0x10000; i += 2) {
            uint16_t rv = *(uint16_t*)(real + i);
            uint16_t sv = *(uint16_t*)(shad + i);
            if (rv != sv) {
                if (i == 0xA39C) continue; // VGA interrupt race
                if (i >= 0x990C && i <= 0x991E) continue; // sound handles
                if (ds_diffs == 0) {
                    fprintf(stderr, "V2-PRE-VM-CMP[f%d]: level=0x%04X DIFFS:\n", pre_vm_frame,
                            *(uint16_t*)(real + 0x25AD));
                }
                if (ds_diffs < 20)
                    fprintf(stderr, "  0x%04X: real=%04X shadow=%04X\n", (uint16_t)i, rv, sv);
                ds_diffs++;
            }
        }
        fprintf(stderr, "V2-PRE-VM-CMP[f%d]: total diffs=%d\n", pre_vm_frame, ds_diffs);
        fflush(stderr);
        // Also compare animation segment (ES data)
        uint16_t anim_seg = *(uint16_t*)(real + 0x2E67);
        if (anim_seg != 0 && v2_m2c_base) {
            uint8_t* real_anim = v2_m2c_base + ((uint32_t)anim_seg << 4);
            int anim_diffs = 0;
            for (uint32_t j = 0; j < 0xC000 && anim_diffs < 5; j += 2) { // only bytecode range, 0xC000+ is VGA rendering data
                uint16_t ra = *(uint16_t*)(real_anim + j);
                uint16_t sa = *(uint16_t*)(v2_vm_shadow_animdata + j);
                if (ra != sa) {
                    if (anim_diffs == 0)
                        fprintf(stderr, "V2-ANIM-CMP[f%d]: seg=%04X DIFFS:\n", pre_vm_frame, anim_seg);
                    fprintf(stderr, "  anim[0x%04X]: real=%04X shadow=%04X\n", (uint16_t)j, ra, sa);
                    anim_diffs++;
                }
            }
            if (anim_diffs > 0)
                fprintf(stderr, "V2-ANIM-CMP: total anim diffs=%d+\n", anim_diffs);
        }
    }
}

uint16_t v2_vm_step_per_obj[128] = {};
void v2_phase_vm(uint16_t ds_val) {
    if (!v2_frame_active) return;
    memset(v2_vm_step_per_obj, 0, sizeof(v2_vm_step_per_obj));
    extern int v2_trace_len;
    static int _vm_frame = 0; _vm_frame++;
    if (_vm_frame <= 2) fprintf(stderr, "V2-VM-START[%d]: trace_len_before_reset=%d table_end=%04X\n",
                                _vm_frame, v2_trace_len, *(uint16_t*)(v2_vm_shadow_ds + 0x372));
    v2_trace_len = 0;

    // Moved to v2_phase_pre_vm
    // Pre-VM DS compare removed from here
    if (false) {
        uint8_t* real = v2_vm_real_ds_ptr;
        uint8_t* shad = v2_vm_shadow_ds;
        fprintf(stderr, "V2-INIT-CMP: table_end real=%04X shadow=%04X\n",
               *(uint16_t*)(real + 0x372), *(uint16_t*)(shad + 0x372));
        uint16_t te = *(uint16_t*)(real + 0x372);
        for (uint16_t si = 0; si < te && si < 20; si += 2) {
            uint16_t r_pc = *(uint16_t*)(real + si + 0x132D);
            uint16_t s_pc = *(uint16_t*)(shad + si + 0x132D);
            uint16_t r_fl = *(uint16_t*)(real + si + 0x1585);
            uint16_t s_fl = *(uint16_t*)(shad + si + 0x1585);
            uint16_t r_cs = *(uint16_t*)(real + si + 0x1355);
            uint16_t s_cs = *(uint16_t*)(shad + si + 0x1355);
            uint16_t r_an = *(uint16_t*)(real + si + 0x16ED);
            uint16_t s_an = *(uint16_t*)(shad + si + 0x16ED);
            uint16_t r_acc = *(uint16_t*)(real + 0x8A);
            uint16_t s_acc = *(uint16_t*)(shad + 0x8A);
            if (r_pc != s_pc || r_fl != s_fl || r_cs != s_cs || r_an != s_an) {
                fprintf(stderr, "  obj=%d: PC r=%04X s=%04X  FL r=%04X s=%04X  CS r=%04X s=%04X  AN r=%04X s=%04X\n",
                       si, r_pc, s_pc, r_fl, s_fl, r_cs, s_cs, r_an, s_an);
            }
        }
        uint16_t r_acc = *(uint16_t*)(real + 0x8A);
        uint16_t s_acc = *(uint16_t*)(shad + 0x8A);
        if (r_acc != s_acc) fprintf(stderr, "  ACC: real=%04X shadow=%04X\n", r_acc, s_acc);
    }

    // sub_14207 init: sub_15517 + clear priority + collision (eip 0x4207-0x4210)
    v2_sub_14207_init(v2_vm_shadow_ds);

    // TEMP: enable obj 0 trace at first diff frame
    { static int _tf = 0; _tf++;
      extern uint16_t v2_trace_object;
      if (_tf >= 40 && _tf <= 45) v2_trace_object = 0;
      else v2_trace_object = 0xFFFF;
    }

    { static int _vf = 0; _vf++;
      uint16_t r3CC = v2_vm_real_ds_ptr ? *(uint16_t*)(v2_vm_real_ds_ptr + 0x3CC) : 0xDEAD;
      if (_vf <= 100)
        printf("V2-VM-PHASE: f=%d te=%d lv=%04X s3CC=%04X r3CC=%04X s334=%04X\n",
          _vf, *(uint16_t*)(v2_vm_shadow_ds + 0x372), *(uint16_t*)(v2_vm_shadow_ds + 0x25AD), *(uint16_t*)(v2_vm_shadow_ds + 0x3CC),
          r3CC, *(uint16_t*)(v2_vm_shadow_ds + 0x0334)); }
    // Re-read ds:0x372 each iteration — VM opcode 0x14 creates objects and increases table_end
    for (uint16_t si = 0; si < *(uint16_t*)(v2_vm_shadow_ds + 0x372); si += 2) {
        v2_vm_execute_object(v2_vm_shadow_ds, si);
        // Priority object loop: exact replica of sub_14207 loc_14219..loc_14241
        uint16_t prio_count = *(uint16_t*)(v2_vm_shadow_ds + 0x376);
        if (prio_count != 0) {
            for (uint16_t di = 0; (int16_t)di < (int16_t)prio_count; di++) {
                uint16_t prio_obj = *(uint16_t*)(v2_vm_shadow_ds + di + 0x378) & 0xFF;
                v2_vm_execute_object(v2_vm_shadow_ds, prio_obj);
            }
            *(uint16_t*)(v2_vm_shadow_ds + 0x376) = 0;
        }
    }
    // Anim cmd count verify
    { static int _acf = 0; _acf++;
      extern int v2_orig_anim_cmd_count;
      if (_acf <= 200 && v2_orig_anim_cmd_count != v2_v2_anim_cmd_count) {
          fprintf(stderr, "V2-ANIM-COUNT[f%d]: MISMATCH orig=%d v2=%d\n",
              _acf, v2_orig_anim_cmd_count, v2_v2_anim_cmd_count);
      }
      v2_orig_anim_cmd_count = 0;
      v2_v2_anim_cmd_count = 0;
    }
    // Full DS compare right after both VMs complete, before post-VM
    { static int _pvf = 0; _pvf++;
      if (_pvf <= 200 && v2_vm_real_ds_ptr) {
          uint8_t* r = v2_vm_real_ds_ptr;
          uint8_t* s = v2_vm_shadow_ds;
          int dc = 0;
          for (uint32_t i = 0; i < 0x10000 && dc < 10; i += 2) {
              if (i == 0xA39C || (i >= 0x990C && i <= 0x991E)) continue;
              uint16_t rv = *(uint16_t*)(r + i), sv = *(uint16_t*)(s + i);
              if (rv != sv) {
                  fprintf(stderr, "V2-AFTER-VM[f%d]: DIFF 0x%04X r=%04X s=%04X\n", _pvf, (uint16_t)i, rv, sv);
                  dc++;
              }
          }
          if (dc > 0) fprintf(stderr, "V2-AFTER-VM[f%d]: level=0x%04X %d diffs\n",
              _pvf, *(uint16_t*)(s + 0x25AD), dc);
      }
    }
}

// Post-VM DS compare: check specific fields that appear as diffs at GAMELOOP
// Universal per-phase DS verify. Call at END of each phase.
// Compares full DS, reports first mismatch with phase name.
static bool v2_ds_hash_skip(uint32_t i); // forward decl
static int v2_phase_verify_frame = 0;
static void v2_phase_verify(const char* phase) {
    if (!v2_vm_real_ds_ptr) return;
    if (v2_phase_verify_frame > 100) return;
    uint8_t* r = v2_vm_real_ds_ptr;
    uint8_t* s = v2_vm_shadow_ds;
    int dc = 0;
    for (uint32_t i = 0; i < 0x10000 && dc < 5; i += 2) {
        if (v2_ds_hash_skip(i)) continue;
        // AIL sound handles (v2 writes real seq offsets, AIL stub returns 0xFFFF)
        if (i >= 0x990C && i <= 0x991E) continue;
        // XMI buffer (sub_10d96 alloc — DosMemAlloc derived, in ds_hash_skip)
        if (i == 0x9934) continue;
        uint16_t rv = *(uint16_t*)(r + i);
        uint16_t sv = *(uint16_t*)(s + i);
        if (rv != sv) {
            if (dc == 0)
                fprintf(stderr, "V2-PHASE[%s f%d]: DS DIFF:\n", phase, v2_phase_verify_frame);
            fprintf(stderr, "  0x%04X: orig=%04X v2=%04X\n", (uint16_t)i, rv, sv);
            dc++;
        }
    }
}

static void v2_post_vm_field_check() {
    if (!v2_vm_real_ds_ptr) return;
    uint8_t* r = v2_vm_real_ds_ptr;
    uint8_t* s = v2_vm_shadow_ds;
    static int pvf = 0; pvf++;
    if (pvf > 80) return;
    // Check 0x077E (sub-sprite Y position) and 0x003A (text scratch)
    uint16_t r77e = *(uint16_t*)(r + 0x077E), s77e = *(uint16_t*)(s + 0x077E);
    uint16_t r03a = *(uint16_t*)(r + 0x003A), s03a = *(uint16_t*)(s + 0x003A);
    uint16_t r042 = *(uint16_t*)(r + 0x0042), s042 = *(uint16_t*)(s + 0x0042);
    if (r77e != s77e || r03a != s03a || r042 != s042) {
        fprintf(stderr, "V2-POST-VM-CMP[f%d]: 077E r=%04X s=%04X | 003A r=%04X s=%04X | 0042 r=%04X s=%04X\n",
            pvf, r77e, s77e, r03a, s03a, r042, s042);
    }
}

void v2_vm_trace_compare(); // forward decl
void v2_phase_post_vm(uint16_t ds_val) {
    if (!v2_frame_active) return;
    v2_game_loop_post_vm(v2_vm_shadow_ds);
    v2_vm_verify_subsprites(ds_val);
    v2_vm_verify_fs(ds_val);
    // Trace comparison: both orig and v2 VM have finished, compare opcode traces
    v2_vm_trace_compare();
}

void v2_phase_render1(uint16_t ds_val) {
    if (!v2_frame_active) return;
    uint8_t* s = v2_vm_shadow_ds;
    // Debug: trace sub-sprite Y inputs for transition frame
    // sub_111a1 clears 0x44D..0x204D. Sub-sprite Y at 0x77F is in this range.
    // After sub_11080, 0x77F should be 0. Any non-zero value is a residual from sub_115d2.
    // Both orig and v2 leave residuals — they just differ by 2.
    // Fix: clear sub-sprite residuals after sub_11080 transition, before first render.
    // This matches what happens in the original when the game loop restarts.

    // sub_12fc6(bx=0): sub-sprite position update type 0 (eip 0x004B)
    // Uses delta_type0: d - (|d|/3)*2 ± round (keeps 1/3 of delta)
    {
        auto delta_type0 = [](int16_t d) -> int16_t {
            if (d == 0) return 0;
            int16_t a = (d < 0) ? -d : d;
            int16_t q = a / 3, r = a % 3;
            int16_t red = q * 2 + (r >= 2 ? 1 : 0);
            return (d < 0) ? (d + red) : (d - red);
        };
        uint16_t te = *(uint16_t*)(s + 0x372);
        for (int16_t di2 = te - 2; di2 >= 0; di2 -= 2) {
            if (*(uint16_t*)(s + di2 + 0x1355) == 0) continue;
            if (*(uint16_t*)(s + di2 + 0x1AD5) == 0) continue;
            int16_t dy = (int16_t)(*(uint16_t*)(s + di2 + 0x1765) - *(uint16_t*)(s + di2 + 0x13CD));
            int16_t dx_v = (int16_t)(*(uint16_t*)(s + di2 + 0x173D) - *(uint16_t*)(s + di2 + 0x13A5));
            int16_t ty = delta_type0(dy), tx = delta_type0(dx_v);
            if (tx == 0 && ty == 0) continue;
            uint16_t ss_end = *(uint16_t*)(s + di2 + 0x1AAD);
            for (uint16_t si2 = *(uint16_t*)(s + di2 + 0x1A85); (int16_t)si2 < (int16_t)ss_end; si2 += 2) {
                *(uint16_t*)(s + si2 + 0x64D) += (uint16_t)tx;
                *(uint16_t*)(s + si2 + 0x74D) += (uint16_t)ty;
                *(uint16_t*)(s + si2 + 0x114D) = 0x202;
            }
        }
    }

    // sub_10130: VGA vsync wait (eip 0x004E)
    v2_sub_10130(s);

    // sub_1DE05 (render 1, eip 0x0051)
    v2_sub_1DE05(s);
    // sub_1DE05 equivalent: full-frame render
    v2_do_render();
    // sub_165aa + sub_16661 + sub_1406d
    v2_game_loop_post_render(v2_vm_shadow_ds);
    // sub_1DD9C (sprite render)
    v2_sub_1DD9C(v2_vm_shadow_ds);

    // FS compare after sub_1DD9C in render1
    if (v2_vm_real_ds_ptr && v2_m2c_base) {
        static int _fsc = 0; _fsc++;
        if (_fsc <= 100) {
            uint16_t fs_seg = *(uint16_t*)(v2_vm_real_ds_ptr + 0x2E69);
            if (fs_seg) {
                uint8_t* rfs = v2_m2c_base + (uint32_t)fs_seg * 16;
                int fd = 0;
                uint32_t fs_size = (uint32_t)*(uint16_t*)(v2_vm_shadow_ds+0x25DC) * *(uint16_t*)(v2_vm_shadow_ds+0x25DE) * 8;
                if (fs_size > 0xC080) fs_size = 0xC080;
                for (uint32_t i = 0; i < fs_size && fd < 5; i += 2) {
                    uint16_t rv = *(uint16_t*)(rfs + i);
                    uint16_t sv = *(uint16_t*)(v2_vm_shadow_fs + i);
                    if (rv != sv) {
                        if (fd == 0) fprintf(stderr, "V2-FS-RENDER1[f%d]:\n", _fsc);
                        fprintf(stderr, "  FS[%04X]: orig=%04X v2=%04X\n", (uint16_t)i, rv, sv);
                        fd++;
                    }
                }
            }
        }
    }

    // sub_1C8F1: flagged tiles
    v2_sub_1C8F1(v2_vm_shadow_ds, 0xFFFE); v2_draw_flagged_tiles(v2_current_ds_val);
    // sub_1E0C7: UI
    v2_sub_1E0C7(v2_vm_shadow_ds);
    v2_draw_ui(v2_current_ds_val);
    // sub_16775: page flip 1
    v2_sub_16775(v2_vm_shadow_ds);
}

static bool v2_ds_hash_skip(uint32_t i);
static uint32_t v2_ds_hash(uint8_t* ds);

void v2_phase_post_flip1(uint16_t ds_val) {
    if (!v2_frame_active) return;
    uint8_t* s = v2_vm_shadow_ds;
    // sub_12e16: viking death check
    {
        uint16_t si = *(uint16_t*)(s + 0x3C2);
        uint16_t start_si = si;
        if (si < 6 && (int16_t)*(uint16_t*)(s + si + 0x16ED) < 0) {
            *(uint16_t*)(s + 0x34E) = 5;
            *(uint16_t*)(s + 0x350) = 5;
            si += 2; if (si >= 6) si = 0;
            if (si != start_si && (int16_t)*(uint16_t*)(s + si + 0x16ED) >= 0) goto pf1_done;
            si += 2; if (si >= 6) si = 0;
            if (si == start_si) { si = 0xFFFF; goto pf1_done; }
            if ((int16_t)*(uint16_t*)(s + si + 0x16ED) >= 0) goto pf1_done;
            si = 0xFFFF;
        }
    pf1_done:
        *(uint16_t*)(s + 0x3C2) = si;
        if (si == 0xFFFF) *(uint16_t*)(s + 0x334) |= 2;
    }
    // sub_15530: collision pass 2
    *(uint16_t*)(s + 0x390) = 0xFFFF;
    {
        uint16_t te = *(uint16_t*)(s + 0x372);
        for (uint16_t si2 = 0; (int16_t)si2 < (int16_t)te; si2 += 2) {
            if (*(uint16_t*)(s + si2 + 0x1355) == 0) continue;
            v2_run_collision_vm(s, si2);
        }
    }
    // sub_10704: scroll clamp 1
    {
        auto scroll_lr = [&](int dir, uint16_t amount) {
            if (*(uint16_t*)(s + 0x394) != 0) return;
            int16_t ax;
            if (dir < 0) {
                ax = (int16_t)*(uint16_t*)(s + 0x44) - (int16_t)amount;
                if (ax < 0) ax = 0;
            } else {
                ax = *(uint16_t*)(s + 0x44) + amount;
                uint16_t limit = *(uint16_t*)(s + 0x25A4);
                if ((uint16_t)ax >= limit) ax = limit;
            }
            uint16_t dx = (dir < 0) ? (*(uint16_t*)(s + 0x44) - (uint16_t)ax) : ((uint16_t)ax - *(uint16_t*)(s + 0x44));
            *(uint16_t*)(s + 0x44) = (uint16_t)ax;
            *(uint16_t*)(s + 0x257F) = (uint16_t)ax >> 3;
            *(uint16_t*)(s + 0x34E) = dx;
        };
        auto scroll_ud = [&](int dir, uint16_t amount) {
            if (*(uint16_t*)(s + 0x396) != 0) return;
            int16_t ax;
            if (dir < 0) {
                ax = (int16_t)*(uint16_t*)(s + 0x46) - (int16_t)amount;
                if (ax < 0) ax = 0;
            } else {
                ax = *(uint16_t*)(s + 0x46) + amount;
                uint16_t limit = *(uint16_t*)(s + 0x25A6);
                if ((uint16_t)ax >= limit) ax = limit;
            }
            uint16_t dx = (dir < 0) ? (*(uint16_t*)(s + 0x46) - (uint16_t)ax) : ((uint16_t)ax - *(uint16_t*)(s + 0x46));
            *(uint16_t*)(s + 0x46) = (uint16_t)ax;
            *(uint16_t*)(s + 0x2581) = (uint16_t)ax >> 3;
            *(uint16_t*)(s + 0x350) = dx;
        };
        uint16_t v;
        v = *(uint16_t*)(s + 0x3D8);
        if (v != 0) scroll_lr(-1, *(uint16_t*)(s + v * 2 + 0x2B82));
        else { v = *(uint16_t*)(s + 0x3DA); if (v != 0) scroll_lr(1, *(uint16_t*)(s + v * 2 + 0x2B82)); }
        v = *(uint16_t*)(s + 0x3DE);
        if (v != 0) scroll_ud(-1, *(uint16_t*)(s + v * 2 + 0x2B82));
        else { v = *(uint16_t*)(s + 0x3DC); if (v != 0) scroll_ud(1, *(uint16_t*)(s + v * 2 + 0x2B82)); }
    }
    // sub_12fcb: sub-sprite position update type 2
    {
        auto delta_type1 = [](int16_t d) -> int16_t {
            if (d == 0) return 0;
            int16_t a = (d < 0) ? -d : d;
            int16_t q = a / 3, r = a % 3;
            int16_t res = q + (r >= 2 ? 1 : 0);
            return (d < 0) ? -res : res;
        };
        uint16_t te = *(uint16_t*)(s + 0x372);
        for (int16_t di2 = te - 2; di2 >= 0; di2 -= 2) {
            if (*(uint16_t*)(s + di2 + 0x1355) == 0) continue;
            if (*(uint16_t*)(s + di2 + 0x1AD5) == 0) continue;
            int16_t dy = (int16_t)(*(uint16_t*)(s + di2 + 0x1765) - *(uint16_t*)(s + di2 + 0x13CD));
            int16_t dx_v = (int16_t)(*(uint16_t*)(s + di2 + 0x173D) - *(uint16_t*)(s + di2 + 0x13A5));
            int16_t ty = delta_type1(dy), tx = delta_type1(dx_v);
            if (tx == 0 && ty == 0) continue;
            uint16_t ss_end = *(uint16_t*)(s + di2 + 0x1AAD);
            for (uint16_t si2 = *(uint16_t*)(s + di2 + 0x1A85); (int16_t)si2 < (int16_t)ss_end; si2 += 2) {
                *(uint16_t*)(s + si2 + 0x64D) += (uint16_t)tx;
                *(uint16_t*)(s + si2 + 0x74D) += (uint16_t)ty;
                *(uint16_t*)(s + si2 + 0x114D) = 0x202;
            }
        }
    }
    // sub_12d2c: invincibility timer
    if (*(uint16_t*)(s + 0x3A2) != 0) {
        *(uint16_t*)(s + 0x3A2) -= 1;
        if (*(uint16_t*)(s + 0x39A) != 0) *(uint16_t*)(s + 0x39E) ^= *(uint16_t*)(s + 0x39A);
        else *(uint16_t*)(s + 0x39E) = 0;
    } else *(uint16_t*)(s + 0x39E) = 0;
    if (*(uint16_t*)(s + 0x3A4) != 0) {
        *(uint16_t*)(s + 0x3A4) -= 1;
        if (*(uint16_t*)(s + 0x39C) != 0) *(uint16_t*)(s + 0x3A0) ^= *(uint16_t*)(s + 0x39C);
        else *(uint16_t*)(s + 0x3A0) = 0;
    } else *(uint16_t*)(s + 0x3A0) = 0;

    // sub_10130: VGA vsync wait (eip 0x0083) — last in POST_FLIP1 block
    v2_sub_10130(s);

    // Post-flip1 DS compare: detect render-phase diffs
    if (v2_vm_real_ds_ptr) {
        static int _pf1f = 0; _pf1f++;
        if (_pf1f <= 100) {
            uint32_t h_orig = v2_ds_hash(v2_vm_real_ds_ptr);
            uint32_t h_v2 = v2_ds_hash(s);
            if (h_orig != h_v2) {
                fprintf(stderr, "V2-POST-FLIP1[f%d]: DS HASH MISMATCH orig=%08X v2=%08X\n", _pf1f, h_orig, h_v2);
                // Find first differing addresses (NO skip filter — show ALL diffs)
                int dc = 0;
                for (uint32_t i = 0; i < 0x10000 && dc < 10; i += 2) {
                    uint16_t rv = *(uint16_t*)(v2_vm_real_ds_ptr + i);
                    uint16_t sv = *(uint16_t*)(s + i);
                    if (rv != sv) {
                        fprintf(stderr, "  0x%04X: orig=%04X v2=%04X%s\n", (uint16_t)i, rv, sv,
                            v2_ds_hash_skip(i) ? " [SEG]" : "");
                        dc++;
                    }
                }
            }
        }
    }
}

void v2_phase_render2(uint16_t ds_val) {
    if (!v2_frame_active) return;
    // sub_1DE05 (render 2)
    v2_sub_1DE05(v2_vm_shadow_ds);
    // sub_165aa + sub_16661 + sub_1406d
    v2_game_loop_post_render(v2_vm_shadow_ds);
    // sub_1DD9C (sprite render)
    v2_sub_1DD9C(v2_vm_shadow_ds);
    // sub_1C8F1
    v2_sub_1C8F1(v2_vm_shadow_ds, 0xFFFE); v2_draw_flagged_tiles(v2_current_ds_val);
    // sub_1E0C7
    v2_sub_1E0C7(v2_vm_shadow_ds);
    v2_draw_ui(v2_current_ds_val);
    // sub_16775: page flip 2
    v2_sub_16775(v2_vm_shadow_ds);
}

void v2_phase_post_flip2(uint16_t ds_val) {
    if (!v2_frame_active) return;
    uint8_t* s = v2_vm_shadow_ds;
    // Render callback fires between RENDER2's sub_16775 and here.
    v2_sub_10130(s);
    // sub_10753: scroll clamp 2 (table 0x2B80)
    {
        auto scroll_lr = [&](int dir, uint16_t amount) {
            if (*(uint16_t*)(s + 0x394) != 0) return;
            int16_t ax;
            if (dir < 0) { ax = (int16_t)*(uint16_t*)(s + 0x44) - (int16_t)amount; if (ax < 0) ax = 0; }
            else { ax = *(uint16_t*)(s + 0x44) + amount; uint16_t l = *(uint16_t*)(s + 0x25A4); if ((uint16_t)ax >= l) ax = l; }
            uint16_t dx = (dir < 0) ? (*(uint16_t*)(s + 0x44) - (uint16_t)ax) : ((uint16_t)ax - *(uint16_t*)(s + 0x44));
            *(uint16_t*)(s + 0x44) = (uint16_t)ax; *(uint16_t*)(s + 0x257F) = (uint16_t)ax >> 3; *(uint16_t*)(s + 0x34E) = dx;
        };
        auto scroll_ud = [&](int dir, uint16_t amount) {
            if (*(uint16_t*)(s + 0x396) != 0) return;
            int16_t ax;
            if (dir < 0) { ax = (int16_t)*(uint16_t*)(s + 0x46) - (int16_t)amount; if (ax < 0) ax = 0; }
            else { ax = *(uint16_t*)(s + 0x46) + amount; uint16_t l = *(uint16_t*)(s + 0x25A6); if ((uint16_t)ax >= l) ax = l; }
            uint16_t dx = (dir < 0) ? (*(uint16_t*)(s + 0x46) - (uint16_t)ax) : ((uint16_t)ax - *(uint16_t*)(s + 0x46));
            *(uint16_t*)(s + 0x46) = (uint16_t)ax; *(uint16_t*)(s + 0x2581) = (uint16_t)ax >> 3; *(uint16_t*)(s + 0x350) = dx;
        };
        uint16_t v;
        v = *(uint16_t*)(s + 0x3D8);
        if (v != 0) scroll_lr(-1, *(uint16_t*)(s + v * 2 + 0x2B80));
        else { v = *(uint16_t*)(s + 0x3DA); if (v != 0) scroll_lr(1, *(uint16_t*)(s + v * 2 + 0x2B80)); }
        v = *(uint16_t*)(s + 0x3DE);
        if (v != 0) scroll_ud(-1, *(uint16_t*)(s + v * 2 + 0x2B80));
        else { v = *(uint16_t*)(s + 0x3DC); if (v != 0) scroll_ud(1, *(uint16_t*)(s + v * 2 + 0x2B80)); }
    }
    // sub_13c0c: viewport bounds
    {
        uint16_t ax_x = *(uint16_t*)(s + 0x44) - 0x10;
        if ((int16_t)ax_x >= 0) *(uint16_t*)(s + 0x34) = ax_x; else *(uint16_t*)(s + 0x34) = 0;
        *(uint16_t*)(s + 0x36) = ax_x + 0x160;
        uint16_t ax_y = *(uint16_t*)(s + 0x46) - 0x10;
        if ((int16_t)ax_y < 0) ax_y = 0;
        *(uint16_t*)(s + 0x38) = ax_y;
        *(uint16_t*)(s + 0x3A) = ax_y + 0xD0;
        uint16_t te = *(uint16_t*)(s + 0x372);
        for (uint16_t si_v = 6; (int16_t)si_v < (int16_t)te; si_v += 2) {
            if (*(uint16_t*)(s + si_v + 0x1355) == 0) continue;
            if (*(uint16_t*)(s + si_v + 0x1585) & 0x800) continue;
            uint16_t ox = *(uint16_t*)(s + si_v + 0x173D), oy = *(uint16_t*)(s + si_v + 0x1765);
            uint16_t obx = *(uint16_t*)(s + si_v + 0x14BD), oby = *(uint16_t*)(s + si_v + 0x1495);
            bool outside = false;
            if ((int16_t)(ox + obx - *(uint16_t*)(s + 0x34)) < 0) outside = true;
            else if ((int16_t)(ox - obx - *(uint16_t*)(s + 0x36)) >= 0) outside = true;
            else if ((int16_t)(oy + oby - *(uint16_t*)(s + 0x38)) < 0) outside = true;
            else if ((int16_t)(oy - oby - *(uint16_t*)(s + 0x3A)) >= 0) outside = true;
            if (outside) *(uint16_t*)(s + si_v + 0x1585) |= 0x200;
        }
    }
    // sub_12fd0: sub-sprite update type 3
    {
        auto delta_type2 = [](int16_t d) -> int16_t {
            if (d == 0) return 0;
            int16_t a = (d < 0) ? -d : d;
            int16_t q = a / 3;
            return (d < 0) ? -(int16_t)q : q;
        };
        uint16_t te = *(uint16_t*)(s + 0x372);
        for (int16_t di3 = te - 2; di3 >= 0; di3 -= 2) {
            if (*(uint16_t*)(s + di3 + 0x1355) == 0) continue;
            if (*(uint16_t*)(s + di3 + 0x1AD5) == 0) continue;
            int16_t dy = (int16_t)(*(uint16_t*)(s + di3 + 0x1765) - *(uint16_t*)(s + di3 + 0x13CD));
            int16_t dx_v = (int16_t)(*(uint16_t*)(s + di3 + 0x173D) - *(uint16_t*)(s + di3 + 0x13A5));
            int16_t ty = delta_type2(dy), tx = delta_type2(dx_v);
            if (tx == 0 && ty == 0) continue;
            uint16_t ss_end = *(uint16_t*)(s + di3 + 0x1AAD);
            for (uint16_t si2 = *(uint16_t*)(s + di3 + 0x1A85); (int16_t)si2 < (int16_t)ss_end; si2 += 2) {
                *(uint16_t*)(s + si2 + 0x64D) += (uint16_t)tx;
                *(uint16_t*)(s + si2 + 0x74D) += (uint16_t)ty;
                *(uint16_t*)(s + si2 + 0x114D) = 0x202;
            }
        }
    }
    // sub_11792: HUD update
    if ((s[0x25CF] & 1) && *(uint16_t*)(s + 0x25AD) != 0x2C) {
        for (int vk = 0; vk < 3; vk++) {
            uint16_t prev = *(uint16_t*)(s + 0x0435 + vk * 2);
            *(uint16_t*)(s + 0x043B + vk * 2) = prev;
            int16_t hv = (int16_t)*(uint16_t*)(s + 0x16ED + vk * 2);
            uint16_t ax = (hv < 0) ? 2 : (*(uint16_t*)(s + 0x3C2) != (uint16_t)(vk * 2)) ? 1 : 0;
            *(uint16_t*)(s + 0x0435 + vk * 2) = ax;
            if (ax != prev) v2_draw_hud_healthbar(v2_current_ds_val, ax, vk, vk);
        }
    }
}

void v2_phase_render3(uint16_t ds_val) {
    if (!v2_frame_active) return;
    // sub_1DE05 (render 3)
    v2_sub_1DE05(v2_vm_shadow_ds);
    // sub_165aa + sub_16661
    v2_game_loop_post_render(v2_vm_shadow_ds);
    // sub_1DD9C (sprite render)
    v2_sub_1DD9C(v2_vm_shadow_ds);
    // sub_1C8F1 (flagged tiles)
    v2_sub_1C8F1(v2_vm_shadow_ds, 0xFFFE); v2_draw_flagged_tiles(v2_current_ds_val);
    // sub_1E0C7 (UI)
    v2_sub_1E0C7(v2_vm_shadow_ds);
    v2_draw_ui(v2_current_ds_val);
    // sub_16775 (page flip 3)
    v2_sub_16775(v2_vm_shadow_ds);
}

void v2_phase_post_flip3(uint16_t ds_val) {
    if (!v2_frame_active) return;
    uint8_t* s = v2_vm_shadow_ds;
    // Render callback (sub_1797b) fires between RENDER3's sub_16775 and here.
    // v2: dispatch palette synchronously (equivalent of render callback timing).
    v2_sub_10130(s);
    // word_30C14 = 0 (eip 0x00DB)
    *(uint16_t*)(s + 0x8734) = 0;
    // sub_108c8: sound crossfade (eip 0x00E1) — kept in shadow state
    {
        if (!((*(uint16_t*)(s + 0x302) & *(uint16_t*)(s + 0x304)) & 0x8000)) {
            if (s[0x91A4] == 1) {
                if (s[0x918B] != 1) {
                    s[0x918B] = 0;
                    s[0x304] ^= 1;
                    if (s[0x304] & 1) {
                        for (uint16_t si_s = 2; (int16_t)si_s < 0x0A; si_s += 2) {
                            uint16_t h = (uint16_t)(si_s - 0x66F4);
                            if (*(uint16_t*)(s + h) != 0xFFFF) {
                                *(uint16_t*)(s + h) = 0xFFFF;
                                *(uint16_t*)(s + (uint16_t)(si_s - 0x66EA)) = 0xFFFF;
                            }
                        }
                    }
                }
            }
            if (s[0x919E] == 1) {
                s[0x919E] = 0;
                s[0x302] ^= 1;
                if (s[0x302] & 1 && !(*(uint16_t*)(s + 0x302) & 0x8000)) {
                    uint16_t h = (uint16_t)(0 - 0x66F4);
                    if (*(uint16_t*)(s + h) != 0xFFFF) {
                        *(uint16_t*)(s + h) = 0xFFFF;
                        *(uint16_t*)(s + (uint16_t)(0 - 0x66EA)) = 0xFFFF;
                    }
                }
            }
        }
    }
    // sub_10350: level transition check (eip 0x00E4)
    if (*(uint16_t*)(s + 0x218F) == 0) {
        bool trigger = false;
        if (s[0x91B0] == 1) trigger = true;
        else if (s[0x91A4] == 1) {
            if (s[0x9199] == 1 || s[0x917C] == 1) trigger = true;
        }
        if (trigger) {
            if (*(uint16_t*)(s + 0x3CC) == 0x8000 || (s[0x25CF] & 8)) {
                v2_sub_11080(s);
            } else {
                s[0x7F0B] = 0; s[0x7F0C] = 0; s[0x7F0D] = 0;
                v2_sub_11080(s);
            }
        }
    }
    // sub_1086f: command buffer dispatch (eip 0x00E7)
    // Same function as in v2_game_loop_pre_vm — called AGAIN here in POST_FLIP3.
    // Uses same logic: process command buffer entries, render text/glyphs.
    {
        uint16_t bx_read = *(uint16_t*)(s + 0x2B64);  // word_2B044
        uint16_t bx_write = *(uint16_t*)(s + 0x218F); // word_2A66F
        while (bx_read != bx_write) {
            uint16_t si_v = *(uint16_t*)(s + 0x3C2);
            uint16_t di_v = *(uint16_t*)(s + si_v + 0x1A85);
            *(uint16_t*)(s + di_v + 0x44D) &= 0xDFFF;
            s[di_v + 0x114D] = 2;

            uint16_t cmd_type = *(uint16_t*)(s + (uint16_t)(bx_read + 0x1DA7));

            if (cmd_type == 0) {
                uint16_t ax_align = *(uint16_t*)(s + (uint16_t)(bx_read + 0x1DAD));
                uint16_t di_pos = *(uint16_t*)(s + (uint16_t)(bx_read + 0x1DAB));
                uint16_t si_pos = *(uint16_t*)(s + (uint16_t)(bx_read + 0x1DA9));
                uint16_t bx_text = *(uint16_t*)(s + (uint16_t)(bx_read + 0x1DAF));
                v2_sub_12529(s, bx_text);
                v2_sub_12549(s, ax_align);
                uint16_t save_si = si_pos, save_di = di_pos;
                v2_sub_12388(s, si_pos, di_pos, (uint8_t)ax_align);
                v2_loc_124c5(s, save_si + 1, save_di + 1, bx_text);
                s[0x956B] = 1;
                bx_read += 0x0A;
            } else if (cmd_type == 0x0A) {
                uint16_t di_pos = *(uint16_t*)(s + (uint16_t)(bx_read + 0x1DAB));
                uint16_t si_pos = *(uint16_t*)(s + (uint16_t)(bx_read + 0x1DA9));
                uint16_t bx_text = *(uint16_t*)(s + (uint16_t)(bx_read + 0x1DAD));
                *(uint16_t*)(s + 0x34) = 2;
                v2_loc_124c5(s, si_pos, di_pos, bx_text);
                s[0x956B] = 1;
                bx_read += 0x08;
            } else if (cmd_type == 6) {
                uint16_t cx = *(uint16_t*)(s + (uint16_t)(bx_read + 0x1DA9));
                cx <<= 1;
                // OUT(0x3C8, 3); — VGA palette write addr
                uint8_t r = (uint8_t)(cx & 0x3E); s[0x7F0B] = r;
                cx >>= 5;
                uint8_t g = (uint8_t)(cx & 0x3E); s[0x7F0C] = g;
                cx >>= 5;
                uint8_t b = (uint8_t)(cx & 0x3E); s[0x7F0D] = b;
                // OUT(0x3C9, r); OUT(0x3C9, g); OUT(0x3C9, b); — VGA DAC
                bx_read += 4;
            } else if (cmd_type == 8) {
                uint8_t ch = (uint8_t)*(uint16_t*)(s + (uint16_t)(bx_read + 0x1DA9));
                uint16_t si_pos = *(uint16_t*)(s + (uint16_t)(bx_read + 0x1DAB));
                uint16_t di_pos = *(uint16_t*)(s + (uint16_t)(bx_read + 0x1DAD));
                v2_sub_1241e(s, ch, si_pos, di_pos);
                s[0x956B] = 1;
                bx_read += 0x08;
            } else if (cmd_type == 2) {
                // off_2B086[2] = loc_12758: full screen text clear. bx += 2.
                // Exact replica with all render passes (DS side effects).
                *(uint16_t*)(s + 0x9569) = 1;             // word_31A49 = 1
                *(uint16_t*)(s + 0x98DC) = 0;             // word_31DBC = 0
                // Conditional first render pass
                if (s[0x25CF] & 0xE0) {
                    v2_sub_16775(s);
                    v2_sub_10130(s);
                    v2_sub_1E0C7(s);
                }
                // Render pass 1
                v2_sub_16775(s);
                v2_sub_10130(s);
                v2_sub_1DE05(s);
                v2_game_loop_post_render(s);              // sub_165aa + sub_16661
                v2_sub_1DD9C(s);
                v2_sub_1C8F1(v2_vm_shadow_ds, 0xFFFE); v2_draw_flagged_tiles(v2_current_ds_val);
                v2_sub_1E0C7(s);
                v2_draw_ui(v2_current_ds_val);
                // Render pass 2
                v2_sub_16775(s);
                v2_sub_10130(s);
                v2_sub_1DE05(s);
                v2_game_loop_post_render(s);
                v2_sub_1DD9C(s);
                v2_sub_1C8F1(v2_vm_shadow_ds, 0xFFFE); v2_draw_flagged_tiles(v2_current_ds_val);
                // sub_16775 + sub_10130
                v2_sub_16775(s);
                v2_sub_10130(s);
                // Cleanup
                *(uint16_t*)(s + 0x9569) = 0;             // word_31A49 = 0
                s[0x956B] = 0;                            // byte_31A4B = 0
                *(uint16_t*)(s + 0x98DC) = 0;             // word_31DBC = 0
                memset(s + 0x956C, 0, 0x1B8 * 2);        // clear glyph buffer
                bx_read += 2;
            } else if (cmd_type == 4) {
                *(uint16_t*)(s + 0x0334) |= 4;
                bx_read += 2;
            } else {
                bx_read += 2;
            }
            *(uint16_t*)(s + 0x2B64) = bx_read;
            v2_sub_1E0C7(s);
            // CALLF sub_1E0C7 (already done above)
            // CALL sub_12352: input processing
            {
                extern uint16_t v2_input_snapshot;
                uint16_t ax = 0;
                if (*(uint16_t*)(s + 0x86DA) != 0)
                    ax = *(uint16_t*)(s + 0x86DC);
                ax |= *(uint16_t*)(s + 0x86DE);
                ax |= v2_input_snapshot;
                *(uint16_t*)(s + 0x03B6) = ax;
                uint16_t prev = *(uint16_t*)(s + 0x03BA);
                *(uint16_t*)(s + 0x03B8) = (ax ^ prev) & ax;
                *(uint16_t*)(s + 0x03BA) = ax;
            }
            // CALL sub_10138: transition check
            {
                uint16_t btns = *(uint16_t*)(s + 0x0334);
                if (btns & 0x7) break;
            }
        }
        // loc_108a5: clear pointers + page flip + frame sync
        *(uint16_t*)(s + 0x218F) = 0;                    // MOV word_2A66F, 0
        *(uint16_t*)(s + 0x2B64) = 0;                    // MOV word_2B044, 0
        v2_sub_16775(s);                                  // CALL sub_16775
        v2_sub_10130(s);                                  // sub_10130: VGA vsync wait
    }
}

void v2_phase_frame_end(uint16_t ds_val) {
    if (!v2_frame_active) return;
    v2_frame_active = false;

    // End-of-frame level transition check.
    // Verified with seg000 lines 122-146 (eip 0x00F7..0x012D, loc_100f7).
    // Original: after pass 3, check transition conditions:
    //   1. word_2AA8D < 0x25 (not a special level)
    //   2. word_286E2 != 0 (transition requested by VM)
    //   3. byte_3168B == 1 (all dead → restart) or byte_3168C == 1 (level complete → advance)
    //   If byte_3168B: next_level = max(current - 1, 0); OR word_28814, 1
    //   If byte_3168C: OR word_28814, 1 (next_level set by VM)
    //   Then JMP loc_1001E to restart outer loop (sub_10138 sees bit 0 and handles transition).
    {
        uint8_t* s = v2_vm_shadow_ds;
        int16_t level = (int16_t)*(uint16_t*)(s + 0x25AD); // word_2AA8D
        if (level < 0x25) {
            uint16_t w286e2 = *(uint16_t*)(s + 0x0202); // word_286E2
            if (w286e2 != 0) {
                if (s[0x91AB] == 1) { // byte_3168B == 1 (all dead)
                    *(uint16_t*)(s + 0x0334) |= 1; // OR word_28814, 1
                    int16_t ax = level;
                    ax -= 1; // DEC ax
                    if (ax < 0) ax = 0;
                    *(uint16_t*)(s + 0x25C9) = (uint16_t)ax; // MOV word_2AAA9, ax
                } else if (s[0x91AC] == 1) { // byte_3168C == 1 (level complete)
                    *(uint16_t*)(s + 0x0334) |= 1; // OR word_28814, 1
                    // word_2AAA9 already set by VM (next level destination)
                }
            }
        }
    }

    v2_vm_verify_object_consistency(v2_vm_shadow_ds);
    v2_vm_verify_page_state(v2_vm_shadow_ds);
    v2_vm_verify_object_refs(v2_vm_shadow_ds);
#ifdef V2_RENDER_FROM_SHADOW
    v2_vm_in_frame = false;
#endif
}

// ============================================================================
// V2 game thread — barrier-synchronized with original game loop.
// v2 runs in its own thread. seg000 signals each phase via v2_signal_phase().
// Both threads process the same phase simultaneously on different data.
// ============================================================================
#include <thread>
#include <condition_variable>

static std::mutex v2_barrier_mutex;
static std::condition_variable v2_cv_start, v2_cv_done;
static int v2_pending_phase = -1;    // -1=idle, -2=quit
static bool v2_phase_complete = true;
static uint16_t v2_barrier_ds = 0;
static std::thread v2_game_thread;

static void v2_game_thread_func() {
    while (true) {
        std::unique_lock<std::mutex> lock(v2_barrier_mutex);
        v2_cv_start.wait(lock, []{ return v2_pending_phase != -1; });

        int phase = v2_pending_phase;
        uint16_t ds = v2_barrier_ds;
        lock.unlock();

        if (phase == -2) break; // quit

        static const char* phase_names[] = {
            "FRAME_BEGIN", "PRE_VM", "VM", "POST_VM",
            "RENDER1", "POST_FLIP1", "RENDER2", "POST_FLIP2",
            "RENDER3", "POST_FLIP3", "FRAME_END"
        };
        switch (phase) {
            case V2_PHASE_FRAME_BEGIN:  v2_phase_frame_begin(ds); break;
            case V2_PHASE_PRE_VM:       v2_phase_pre_vm(ds); break;
            case V2_PHASE_VM:           v2_phase_vm(ds); break;
            case V2_PHASE_POST_VM:      v2_phase_post_vm(ds); break;
            case V2_PHASE_RENDER1:      v2_phase_render1(ds); break;
            case V2_PHASE_POST_FLIP1:   v2_phase_post_flip1(ds); break;
            case V2_PHASE_RENDER2:      v2_phase_render2(ds); break;
            case V2_PHASE_POST_FLIP2:   v2_phase_post_flip2(ds); break;
            case V2_PHASE_RENDER3:      v2_phase_render3(ds); break;
            case V2_PHASE_POST_FLIP3:   v2_phase_post_flip3(ds); break;
            case V2_PHASE_FRAME_END:    v2_phase_frame_end(ds); break;
        }
        // Universal per-phase DS verify after each phase completes
        if (phase >= 0 && phase <= 10)
            v2_phase_verify(phase_names[phase]);
        if (phase == V2_PHASE_FRAME_END)
            v2_phase_verify_frame++;

        lock.lock();
        v2_pending_phase = -1;
        v2_phase_complete = true;
        v2_cv_done.notify_one();
    }
}

// Called from seg000 right after orig sub_115d2 completes.
// Saves exact orig DS state for v2 comparison.
void v2_save_115d2_snapshot(uint8_t* orig_ds) {
    memcpy(v2_115d2_snapshot, orig_ds, 0x10000);
    v2_115d2_snapshot_valid = true;
}

void v2_signal_phase(V2Phase phase, uint16_t ds_val) {
    if (!v2_m2c_base || !myDrawInfo_v2) return;
    std::unique_lock<std::mutex> lock(v2_barrier_mutex);
    v2_barrier_ds = ds_val;
    v2_pending_phase = (int)phase;
    v2_phase_complete = false;
    v2_cv_start.notify_one();
    v2_cv_done.wait(lock, []{ return v2_phase_complete; });
}

void v2_game_thread_start() {
    v2_game_thread = std::thread(v2_game_thread_func);
}

void v2_game_thread_stop() {
    {
        std::lock_guard<std::mutex> lock(v2_barrier_mutex);
        v2_pending_phase = -2;
        v2_cv_start.notify_one();
    }
    if (v2_game_thread.joinable()) v2_game_thread.join();
}

// ============================================================================
// Deferred init verification: save original's values THIS frame,
// compare with v2's values NEXT frame.
// ============================================================================
struct V2VMInitSaved {
    uint16_t es;
    uint16_t pc;
    bool valid;
};
static V2VMInitSaved v2_vm_prev_init[128] = {};

// Called from original VM's loc_142a2: saves original's es + pc for this object.
// On the NEXT frame, v2 will start from DS that reflects these values,
// so we can compare v2's init with these saved values.
// Direct same-frame comparison: v2 shadow should match original's init.
// Both read from the same DS (shadow = copy of real DS). Both apply the same
// animation update logic. Any difference means v2's init code is wrong.
void v2_vm_verify_init(uint16_t obj_idx, uint16_t orig_es, uint16_t orig_pc, uint16_t orig_obj42) {
    int slot = obj_idx / 2;
    if (slot < 0 || slot >= 128) return;

    // Use REAL DS (not shadow!) — shadow was modified by v2 VM execution.
    // Real DS still has the original values since v2 only writes to shadow.
    if (!v2_vm_real_ds_ptr) return;
    uint16_t code_seg = *(uint16_t*)(v2_vm_real_ds_ptr + obj_idx + 0x1355);
    if (code_seg == 0) return;

    uint16_t flags = *(uint16_t*)(v2_vm_real_ds_ptr + obj_idx + 0x1585);
    uint16_t v2_es = code_seg;
    uint16_t v2_pc;

    if ((flags & 0x200) || *(uint16_t*)(v2_vm_real_ds_ptr + 0x32F) != 0) {
        uint16_t anim_idx = *(uint16_t*)(v2_vm_real_ds_ptr + obj_idx + 0x16ED);
        if (anim_idx & 0x8000) return;
        v2_es = *(uint16_t*)(v2_vm_real_ds_ptr + 0x2E67);
        uint16_t bx_anim = anim_idx * 0x15;
        uint8_t* anim_ptr = v2_m2c_base + ((uint32_t)v2_es << 4);
        v2_pc = *(uint16_t*)(anim_ptr + bx_anim + 3);
    } else {
        v2_pc = *(uint16_t*)(v2_vm_real_ds_ptr + obj_idx + 0x132D);
    }

    static int init_mismatch = 0;
    if (orig_pc != v2_pc && init_mismatch < 10) {
        // Check if shadow differs from current real DS
        uint16_t real_pc = *(uint16_t*)(v2_vm_real_ds_ptr + obj_idx + 0x132D);
        uint16_t shadow_pc = *(uint16_t*)(v2_vm_shadow_ds + obj_idx + 0x132D);
        printf("V2-INIT: obj=%d PC: orig=0x%04X v2=0x%04X shadow_raw=0x%04X real_now=0x%04X flags=0x%04X\n",
               obj_idx, orig_pc, v2_pc, shadow_pc, real_pc, flags);
        init_mismatch++;
    }
    if (orig_es != v2_es && init_mismatch < 10) {
        printf("V2-INIT: obj=%d ES mismatch: orig=0x%04X v2=0x%04X\n",
               obj_idx, orig_es, v2_es);
        init_mismatch++;
    }
}

// ============================================================================
// Trace-based VM verify: both VMs record per-opcode traces, then compare.
// ============================================================================
// Fast DS hash: polynomial rolling hash over [0, 0x1C00)
static uint32_t vm_ds_hash(uint8_t* ds) {
    uint32_t h = 0;
    for (uint32_t i = 0; i < 0x1C00; i += 2)
        h = h * 131 + *(uint16_t*)(ds + i);
    return h;
}

// DS hash for per-opcode comparison.
// Skips same ranges as GAMELOOP verify (DosMemAlloc segments + AIL sound + DOS/VGA state).
static bool v2_ds_hash_skip(uint32_t i) {
    if (i >= 0x2E5C && i <= 0x2E7C) return true; // segment pointer table
    if (i >= 0x1354 && i <= 0x1454) return true; // per-object code_seg
    if (i >= 0x094C && i <= 0x0A4C) return true; // per-sub-sprite sprite_seg
    if (i >= 0x0B4C && i <= 0x0C4C) return true; // per-sub-sprite base_seg
    if (i >= 0x990C && i <= 0x9944) return true; // AIL sound handles + driver buffer
    if (i >= 0x86AC && i <= 0x86B0) return true; // DOS INT 24h vector
    if (i >= 0x8638 && i <= 0x863C) return true; // PRNG seed
    if (i >= 0x98E4 && i <= 0x98EC) return true; // AIL GTL handle (far ptr)
    if (i == 0x9300) return true;                 // VGA mode byte
    if (i >= 0xA398 && i <= 0xA39C) return true;  // VGA page flip counter
    return false;
}

static uint32_t v2_ds_hash(uint8_t* ds) {
    uint32_t h = 0;
    for (uint32_t i = 0; i < 0x10000; i += 4) {
        if (v2_ds_hash_skip(i)) continue;
        h = h * 131 + *(uint32_t*)(ds + i);
    }
    return h;
}

struct VMTraceEntry {
    uint16_t obj;
    uint16_t step;
    uint8_t opcode;
    uint16_t pc_before;
    uint16_t pc_after;
    uint16_t acc_before;
    uint16_t acc_after;
    uint16_t es_seg;
    uint16_t flags;
    uint16_t x;
    uint16_t y;
    uint16_t ds_42;
    uint16_t ds_8A;
    uint16_t ds_6C;
    uint16_t ds_334;
    uint32_t ds_hash;   // hash of DS[0..0x1C00) AFTER opcode execution
};

static const int VM_TRACE_MAX = 50000;
static VMTraceEntry v2_trace[VM_TRACE_MAX];
static VMTraceEntry orig_trace[VM_TRACE_MAX];
int v2_trace_len = 0;
int orig_trace_len = 0;
int g_v2_verify_step = 0;

void v2_vm_trace_record_v2(uint16_t obj, uint16_t step, uint8_t opcode,
                           uint16_t pc_before, uint16_t pc_after,
                           uint16_t acc_before, uint16_t acc_after,
                           uint8_t* ds) {
    if (v2_trace_len < VM_TRACE_MAX) {
        auto& e = v2_trace[v2_trace_len++];
        e.obj = obj; e.step = step; e.opcode = opcode;
        e.pc_before = pc_before; e.pc_after = pc_after;
        e.acc_before = acc_before; e.acc_after = acc_after;
        e.es_seg = *(uint16_t*)(ds + obj + 0x1355);
        e.flags = *(uint16_t*)(ds + obj + 0x1585);
        e.x = *(uint16_t*)(ds + obj + 0x173D);
        e.y = *(uint16_t*)(ds + obj + 0x1765);
        e.ds_42 = *(uint16_t*)(ds + 0x42);
        e.ds_8A = *(uint16_t*)(ds + 0x8A);
        e.ds_6C = *(uint16_t*)(ds + 0x6C);
        e.ds_334 = *(uint16_t*)(ds + 0x334);
        e.ds_hash = v2_ds_hash(ds);
    }
}

// Called from original VM loop after each opcode
void v2_vm_replay_verify(uint8_t* ds_before, uint8_t* ds_after,
                         uint8_t* es_ptr, uint16_t obj_idx, int step,
                         uint8_t opcode, uint16_t pc_before, uint16_t acc_before,
                         uint16_t orig_pc_after, uint16_t orig_acc_after) {
    if (orig_trace_len < VM_TRACE_MAX) {
        auto& e = orig_trace[orig_trace_len++];
        e.obj = obj_idx; e.step = (uint16_t)step; e.opcode = opcode;
        e.pc_before = pc_before; e.pc_after = orig_pc_after;
        e.acc_before = acc_before; e.acc_after = orig_acc_after;
        e.es_seg = *(uint16_t*)(ds_after + obj_idx + 0x1355);
        e.flags = *(uint16_t*)(ds_after + obj_idx + 0x1585);
        e.x = *(uint16_t*)(ds_after + obj_idx + 0x173D);
        e.y = *(uint16_t*)(ds_after + obj_idx + 0x1765);
        e.ds_42 = *(uint16_t*)(ds_after + 0x42);
        e.ds_8A = *(uint16_t*)(ds_after + 0x8A);
        e.ds_6C = *(uint16_t*)(ds_after + 0x6C);
        e.ds_334 = *(uint16_t*)(ds_after + 0x334);
        e.ds_hash = v2_ds_hash(ds_after);
    }
}

// Called after both VMs finish (at V2_PHASE_POST_VM)
void v2_vm_trace_compare() {
    static int frame = 0;
    frame++;
    if (frame > 200) return;
    // Dump full traces for frames with issues
    if (frame == 1 || frame == 18 || frame == 122 || frame == 123) {
        fprintf(stderr, "=== ORIG TRACE f1 (len=%d) ===\n", orig_trace_len);
        for (int i = 0; i < orig_trace_len && i < 30; i++) {
            auto& e = orig_trace[i];
            fprintf(stderr, "  [%d] obj=%d op=%02X pc=%04X→%04X acc=%04X→%04X fl=%04X\n",
                    i, e.obj, e.opcode, e.pc_before, e.pc_after, e.acc_before, e.acc_after, e.flags);
        }
        fprintf(stderr, "=== V2 TRACE f1 (len=%d) ===\n", v2_trace_len);
        for (int i = 0; i < v2_trace_len && i < 30; i++) {
            auto& e = v2_trace[i];
            fprintf(stderr, "  [%d] obj=%d op=%02X pc=%04X→%04X acc=%04X→%04X fl=%04X\n",
                    i, e.obj, e.opcode, e.pc_before, e.pc_after, e.acc_before, e.acc_after, e.flags);
        }
    }

    int len = (v2_trace_len < orig_trace_len) ? v2_trace_len : orig_trace_len;
    static int total_err = 0;
    static bool first_mismatch_printed = false;
    for (int i = 0; i < len && total_err < 100; i++) {
        auto& o = orig_trace[i];
        auto& v = v2_trace[i];
        bool obj_match = (v.obj == o.obj);
        bool op_match = (v.opcode == o.opcode);
        bool pc_match = (v.pc_before == o.pc_before && v.pc_after == o.pc_after);
        bool acc_match = (v.acc_after == o.acc_after);
        bool hash_match = (v.ds_hash == o.ds_hash);
        if (!obj_match || !op_match || !pc_match || !acc_match) {
            if (!first_mismatch_printed) {
                first_mismatch_printed = true;
                // Print context: up to 5 preceding matching entries
                int ctx_start = (i > 5) ? i - 5 : 0;
                fprintf(stderr, "V2-TRACE: FIRST MISMATCH at frame %d index %d (orig_len=%d v2_len=%d)\n", frame, i, orig_trace_len, v2_trace_len);
                for (int j = ctx_start; j < i; j++) {
                    fprintf(stderr, "  [%d] OK  ORIG obj=%d op=%02X pc=%04X→%04X acc=%04X→%04X fl=%04X\n",
                           j, orig_trace[j].obj, orig_trace[j].opcode, orig_trace[j].pc_before, orig_trace[j].pc_after,
                           orig_trace[j].acc_before, orig_trace[j].acc_after, orig_trace[j].flags);
                    fprintf(stderr, "           V2  obj=%d op=%02X pc=%04X→%04X acc=%04X→%04X fl=%04X\n",
                           v2_trace[j].obj, v2_trace[j].opcode, v2_trace[j].pc_before, v2_trace[j].pc_after,
                           v2_trace[j].acc_before, v2_trace[j].acc_after, v2_trace[j].flags);
                }
                fprintf(stderr, "  [%d] BAD ORIG obj=%d op=%02X pc=%04X→%04X acc=%04X→%04X es=%04X fl=%04X x=%04X y=%04X 42=%04X 6C=%04X hash=%08X\n",
                       i, o.obj, o.opcode, o.pc_before, o.pc_after, o.acc_before, o.acc_after,
                       o.es_seg, o.flags, o.x, o.y, o.ds_42, o.ds_6C, o.ds_hash);
                fprintf(stderr, "           V2  obj=%d op=%02X pc=%04X→%04X acc=%04X→%04X es=%04X fl=%04X x=%04X y=%04X 42=%04X 6C=%04X hash=%08X\n",
                       v.obj, v.opcode, v.pc_before, v.pc_after, v.acc_before, v.acc_after,
                       v.es_seg, v.flags, v.x, v.y, v.ds_42, v.ds_6C, v.ds_hash);
                fflush(stderr);
                fflush(stdout);
                exit(1);
            }
            total_err++;
            if (!obj_match) break;
        }
    }
    // DS hash mismatch detection (catches writes that don't affect acc/pc)
    if (total_err == 0) {
        static bool hash_mismatch_found = false;
        if (!hash_mismatch_found) {
            for (int i = 0; i < len; i++) {
                if (v2_trace[i].ds_hash != orig_trace[i].ds_hash) {
                    hash_mismatch_found = true;
                    fprintf(stderr, "V2-TRACE: DS HASH MISMATCH at frame %d index %d (obj=%d op=%02X pc=%04X)\n",
                        frame, i, orig_trace[i].obj, orig_trace[i].opcode, orig_trace[i].pc_before);
                    fprintf(stderr, "  ORIG hash=%08X  V2 hash=%08X\n", orig_trace[i].ds_hash, v2_trace[i].ds_hash);
                    if (i > 0)
                        fprintf(stderr, "  prev[%d] obj=%d op=%02X hash_orig=%08X hash_v2=%08X\n",
                            i-1, orig_trace[i-1].obj, orig_trace[i-1].opcode, orig_trace[i-1].ds_hash, v2_trace[i-1].ds_hash);
                    break;
                }
            }
        }
    }

    if (v2_trace_len != orig_trace_len && total_err < 100) {
        if (!first_mismatch_printed) {
            fprintf(stderr, "V2-TRACE: LENGTH MISMATCH at frame %d: orig=%d v2=%d\n", frame, orig_trace_len, v2_trace_len);
        }
        total_err++;
    }
    v2_trace_len = 0;
    orig_trace_len = 0;
}

// ============================================================================
// Per-anim-cmd replay: run v2's anim cmd handler with original's exact state.
// ============================================================================
void v2_vm_replay_anim_cmd(uint8_t* ds_before, uint8_t* ds_after, uint8_t* es_ptr,
                           uint16_t obj_idx, uint8_t cmd, uint16_t bx_before, uint16_t bx_after) {
    if (!v2_vm_table_initialized || cmd > 0x1A) return;

    // Set up temp VM with ds_before state
    static uint8_t anim_replay_shadow[0x10000];
    memcpy(anim_replay_shadow, ds_before, 0x10000);

    V2VM vm;
    // vm.ds must point to FULL DS (not the 0x1C00 snapshot) for out-of-shadow reads
    vm.ds = anim_replay_shadow; // Full 64KB copy
    vm.shadow = anim_replay_shadow;
    vm.es = es_ptr;
    vm.cs_base = v2_m2c_base ? v2_m2c_base + 0x1A20 : nullptr;
    vm.obj = obj_idx;
    vm.slot = obj_idx / 2;
    vm.running = true;
    vm.carry = false;
    vm.pc = 0; // not used by anim cmds

    // Run v2's anim cmd handler
    uint16_t anim_bx = bx_before;
    // Handler lookup from REAL DS (table at 0x86E6 is outside 0x1C00 snapshot range)
    uint16_t handler = *(uint16_t*)(v2_vm_real_ds_ptr + 0x86E6 + cmd * 2);

    // Execute the handler via the same switch as v2_vm_run_anim_frame
    // For simplicity: call v2_vm_run_anim_frame with a single-cmd sequence
    // Actually easier: just inline the switch dispatch for one cmd

    // We need to run exactly ONE anim cmd from v2's switch. Set up state and dispatch.
    // Save/restore accumulator
    // Switch accumulator to anim replay shadow
    uint8_t* saved_acc_base = v2_vm_acc_base;
    v2_vm_acc_base = anim_replay_shadow;
    v2_vm_accumulator = *(uint16_t*)(ds_before + 0x8A);

    // Run v2's anim cmd handler on snapshot of ds_before
    // The v2 handler reads the cmd byte from anim_bx-1, but we already have the cmd.
    // Set up anim_bx for v2: the handler will read params from es:anim_bx.
    uint16_t v2_anim_bx = bx_before;

    // Run v2 handler via switch dispatch
    // (calling the same switch as v2_vm_run_anim_frame but for just one cmd)
    v2_vm_exec_anim_cmd(vm, handler, v2_anim_bx);

    // Compare v2 result (anim_replay_shadow) with original's ds_after
    static int anim_diff_count = 0;
    // Restore accumulator to main shadow
    v2_vm_acc_base = saved_acc_base;

    if (anim_diff_count >= 10) return;

    // Check bx advancement
    if (v2_anim_bx != bx_after && anim_diff_count < 10) {
        printf("V2-ANIM: obj=%d cmd=%d (0x%04X) BX MISMATCH: orig=0x%04X v2=0x%04X (from 0x%04X)"
               " 7C=0x%04X 80=0x%04X 38C=0x%04X\n",
               obj_idx, cmd, handler, bx_after, v2_anim_bx, bx_before,
               *(uint16_t*)(anim_replay_shadow + 0x7C), *(uint16_t*)(anim_replay_shadow + 0x80),
               *(uint16_t*)(anim_replay_shadow + 0x38C));
        anim_diff_count++;
    }

    // Check DS writes (full 64KB range to catch all sub-sprite fields)
    for (uint32_t i = 0; i < 0x10000 && anim_diff_count < 10; i += 2) {
        uint16_t orig_val = *(uint16_t*)(ds_after + i);
        uint16_t v2_val = *(uint16_t*)(anim_replay_shadow + i);
        if (orig_val != v2_val) {
            printf("V2-ANIM: obj=%d cmd=%d (0x%04X) DS[0x%04X]: orig=0x%04X v2=0x%04X\n",
                   obj_idx, cmd, handler, i, orig_val, v2_val);
            anim_diff_count++;
        }
    }
}

// Forward: execute single anim cmd. Returns true = continue, false = exit.
static bool v2_vm_exec_anim_cmd(V2VM& vm, uint16_t handler, uint16_t& anim_bx);

// Per-opcode verification: called from original VM's opcode loop.
// obj_idx = current object, step = opcode index within this object's frame,
// orig_opcode = opcode the original just executed,
// orig_pc_before = bx before opcode dispatch (after reading opcode byte),
// orig_pc_after = bx after execution,
// orig_acc = ds:0x8A after execution.
// Returns true if match, false on mismatch.
bool v2_vm_verify_opcode(uint16_t obj_idx, int step,
                         uint8_t orig_opcode, uint16_t orig_pc_before,
                         uint16_t orig_pc_after, uint16_t orig_acc_before,
                         uint16_t orig_acc_after, uint16_t orig_es) {
    int slot = obj_idx / 2;
    if (slot < 0 || slot >= 128) return true;
    if (step >= v2_vm_trace_count[slot]) {
        // v2 ran fewer opcodes than original — trace exhausted
        static bool logged = false;
        if (!logged) {
            printf("V2-VERIFY: obj=%d step=%d — v2 trace exhausted (v2 ran %d ops, orig opcode=0x%02X)\n",
                   obj_idx, step, v2_vm_trace_count[slot], orig_opcode);
            logged = true;
        }
        return false;
    }
    V2VMTraceEntry& e = v2_vm_trace[slot][step];
    // Check es segment first
    if (e.es_seg != orig_es) {
        static int es_count = 0;
        if (es_count < 5) {
            printf("V2-VERIFY: obj=%d step=%d ES MISMATCH: orig=0x%04X v2=0x%04X (pc orig=0x%04X v2=0x%04X)\n",
                   obj_idx, step, orig_es, e.es_seg, orig_pc_before, e.pc_before);
        }
        es_count++;
        return false;
    }
    // Check pc_before — catches silent divergences where opcodes coincidentally match
    if (e.pc_before != orig_pc_before) {
        static int silent_count = 0;
        if (silent_count < 5) {
            printf("V2-VERIFY: obj=%d step=%d PC_BEFORE diverged: orig=0x%04X v2=0x%04X"
                   " (opcode orig=0x%02X v2=0x%02X)\n",
                   obj_idx, step, orig_pc_before, e.pc_before,
                   orig_opcode, e.opcode);
        }
        silent_count++;
        return false;
    }
    if (e.opcode != orig_opcode || e.pc_after != orig_pc_after) {
        static int mismatch_count = 0;
        if (mismatch_count < 10) {
            printf("V2-VERIFY: obj=%d step=%d MISMATCH: orig opcode=0x%02X pc=0x%04X->0x%04X acc=0x%04X->0x%04X"
                   " | v2 opcode=0x%02X pc=0x%04X->0x%04X acc=0x%04X->0x%04X\n",
                   obj_idx, step, orig_opcode, orig_pc_before, orig_pc_after, orig_acc_before, orig_acc_after,
                   e.opcode, e.pc_before, e.pc_after, e.acc_before, e.acc_after);
        }
        mismatch_count++;
        return false;
    }
    // Check acc: compare before AND after states
    // If acc_before matches → acc_after MUST match (opcode bug if not)
    // If acc_before differs → input was different, can't judge opcode correctness
    if (e.acc_before == orig_acc_before && e.acc_after != orig_acc_after) {
        // SAME input, DIFFERENT output → OPCODE BUG
        static int acc_bug = 0;
        if (acc_bug < 5) {
            printf("V2-VERIFY: obj=%d step=%d ACC BUG: same input=0x%04X, orig_out=0x%04X v2_out=0x%04X (opcode=0x%02X)\n",
                   obj_idx, step, orig_acc_before, orig_acc_after, e.acc_after, orig_opcode);
        }
        acc_bug++;
    } else if (e.acc_before != orig_acc_before) {
        // Different input — expected from execution order
        static int acc_input = 0;
        if (acc_input < 3) {
            printf("V2-VERIFY: obj=%d step=%d ACC input differs: orig=0x%04X v2=0x%04X (opcode=0x%02X)\n",
                   obj_idx, step, orig_acc_before, e.acc_before, orig_opcode);
        }
        acc_input++;
    }
    return true;
}
