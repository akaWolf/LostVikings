# Phase D — Typed GameState & the Carrier Endgame

Status 2026-08-05: **DS typing 100.0%** (65536/65536 bytes owned by exactly
one named field/zone), **view layer adopted** (~1400 call sites), roundtrip
identity green on the full 15-scenario set + unit set. Commits
`cd665f5..6deb1a8`.

## Components

| Piece | File | Role |
|---|---|---|
| Field lists | `src/sdl/v2_gamestate.h` | X-macro: one entry generates struct member + (de)serialize + coverage + view accessors |
| Serializer | `src/sdl/v2_gamestate.cpp` | byte-exact DS↔struct; coverage bitmap aborts on overlap; scramble-hardened roundtrip |
| Views | `V2StateView` / `V2StateViewC` / `v2gs()` | typed accessors over the LIVE byte image (same names as the struct) |
| Aliases | `V2_GS_ALIASES` | view-only channels for phase-A names that overlap covering fields |
| Object grid | `src/sdl/v2_obj_view.h` | `ObjMem` (raw DS) / `ObjRef` (via `vm.ds_read/ds_write`, traps preserved) |
| Inspector | `v2_gs_dump_text` | named-field text snapshot (V2_GS_DUMP_TEXT=path, V2_GS_DUMP_FRAME=N) |
| Live probes | `V2_GS_DUMP_DS` | one-shot binary DS dump at a chosen frame |
| Verify | `v2_gs_roundtrip_check` | per-frame DS→struct→DS' memcmp under V2_GS_ROUNDTRIP=1; diffs are a headless divergence |

## Proof protocol used for zone classes ("three-dump protocol")

A DS byte range was only carved with evidence:
1. **ds_static.bin** — the EXE image content (initialized data vs zeros);
2. **live dumps** — empty-replay and level1-END shadow DS snapshots;
3. **verify history** — the project-wide full-DS hash compare (orig vs v2)
   that has run for the project's entire life;
plus, where applicable, DATA.DAT decompressed chunk sizes matched against
loader ladders, and consumer/builder code read line-by-line.

Zone classes: `zero_*` (dead reserve: zero in all three channels),
`image_*` (immutable data: byte-identical across all dumps), `rt_*`
(runtime areas with proven-neighbor bounds), plus semantically named data
(tables, palettes, chunk-loaded blocks, AIL state, spawn area).

## Carrier map (who touches shadow DS bytes in the v2 world)

1. `V2StateView(C)` getters/setters — all scalar/global traffic (~1400 sites).
2. `ObjMem::u16/w16` — object grid, phase functions and unit runners.
3. `V2VM::ds_read/ds_write(_b)` — opcode handlers and `ObjRef`.
4. Chunk/level loaders (`v2_read_chunk(s + off, ...)` bulk writes).
5. Verify readers (PSNAP, DS-hash, A2) — MUST see the byte image.
6. `v2_ail.cpp` rdw/wrw mini-accessors (driver mirrors; slot addresses are
   computed `si-0x66F4` forms).
7. The m2c orig world (`raddr`) — scaffolding, never switched.

## Endgame decision

Switching the carrier to `V2GameState` (views reading struct fields, byte
image produced only at verify barriers) is a single-point change *per layer*
(view generators + ObjMem/ObjRef backends + loader routing). It is BLOCKED
on purpose: the user's standing rule is that verification lives on the byte
image until the scaffolding comes down. Until then the byte image stays the
carrier and this document is the checklist of the switch points.

Remaining refinement queue (all optional before the switch):
- interior carving of `spawn_area` / `rt_*` zones, semantics for `image_*`
  tables (0x8E68 row LUT candidate, 0x2BA6 CS-address table, 0x863D block);
- the deliberate special word forms + named element byte forms
  (re-inventoried 2026-08-06, #84: zero raw-constant sites remain; the
  computed forms all address through named DS_*/OBJ_* constants or the
  ObjMem/view layers — e.g. cmd-entry writes go via `bx + DS_CMD_ENTRY_*`,
  scroll steps via `v*2 + DS_SCROLL_STEP1_TBL`. Wrapping those into
  dedicated accessors is pure cosmetics for the carrier switch, not a
  verification debt);
- extending the ObjMem/ObjRef named-getter vocabulary as code needs it.
