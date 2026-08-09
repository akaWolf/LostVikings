// v2_hash_hot — the replay-verify hash kernels, compiled -O2 ALWAYS.
//
// (direction IV) perf: the per-opcode trace channel hashes the whole DS (and
// the ES/FS segments) before and after EVERY VM opcode — at the project-wide
// -O0 these pure loops were ~77% of total CPU (attract profile: ds_hash_skip
// 30% + ds_hash 22% + es 12% + fs 13%). The m2c world must stay -O0 (RELEASE
// -O2 breaks the shadowstack CALL model — "Return address wasn't created by
// native CALL"), so ONLY these pure functions move to a separate translation
// unit with a per-file -O2 override in the Makefile.
//
// Semantics are bit-identical to the old v2_vm.cpp bodies: same 131
// polynomial, same dword order, same skip set (byte-granular bitmap built
// from the same ranges; the dword-step hash tests the dword's FIRST byte,
// exactly as v2_ds_hash_skip did).
#pragma once
#include <stdint.h>

struct V2hRange { uint16_t start, end; };   // end inclusive

// Build the skip bitmap + the dword-grid non-skip interval list. Called once
// (idempotent) from v2_vm.cpp with the v2_ds_skip_ranges table.
void v2h_skip_build(const V2hRange* rs, int n);

// Byte-granular skip predicate (the old v2_ds_hash_skip semantics; also used
// by the byte/word-step verify loops).
bool v2h_ds_skip(uint32_t i);

// Full-DS hash honoring the skip set (the old v2_ds_hash).
uint32_t v2h_ds_hash(const uint8_t* ds);

// Plain segment hash: h = h*131 + dword over [0, bytes) (the shared loop of
// the old v2_es_hash / v2_fs_hash / v2_fs_hash_shadow bodies).
uint32_t v2h_mem_hash(const uint8_t* p, uint32_t bytes);
