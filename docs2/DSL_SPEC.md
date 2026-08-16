# .lvs — Lost Vikings Script DSL (draft v0)

The single source of truth for game behavior (user decision 2026-08).
Decompiled FROM the six template chunks; compiled BACK to byte-identical
bytecode (stage 2.3 proof) until stage 3 replaces the VM with generated
code. This draft maps every DSL construct to its bytecode counterpart —
nothing here invents semantics that the opcodes don't have.

## Layout of a script file

```
template dino_small {            # one 0x15-byte record
    sprite_chunk 0x132
    subsprites   4               # +2 low bits (0x80 pool flag -> `bigpool`)
    res_handle   0x00A1
    size         16 x 28         # +9/+A
    cost         2
    state_idx    5
    class        %0000_0010_0001 # +F class bits
    vmax         320, 200        # velocity clamps

    anim walk {                  # anim redirect target (record +3 -> jmp)
        ...anim commands...
    }
    on_spawn {                   # spawn entry (P+3, constructor path)
        ...object bytecode...
    }
    state ...                    # decompiled control flow
}
```

## Object-code constructs ↔ opcode families

| DSL | bytecode |
|-----|----------|
| `yield` | 00 (PC saved, resumes next tick) |
| `goto L` / structured if/while | 03 + cmp-branch families |
| `call L` / `return` | 05 (ALT_PC save) / 06 |
| `acc = <expr>` | 51/52/53/54-family loads (literal/indexed/indirect/1995) |
| `field X = acc`, `[NAME] op= acc` | store/RMW families (57/5A/5D/60/63/66...) — NAME from the phase-D layout |
| `if <cmp> goto L` | 6A..8D signed/unsigned cmp-jump families |
| `if bit(MASKS[i], <val>) ...` | 97/98/99-bit-test family (153ea/15403/1542a/15445 fetchers) |
| `spawn T at (x,y) flags F` | op_14 (channel operands) |
| `probe/search ... goto L` | search families 2C/35/BF/C0/C1/D0/D1 (+ 2D/36 continuations) |
| `collide(filter) call L` | 1A/1D/32/37/38 |
| `anim = A` | op_19 (sets OBJ_ANIM_PC) |
| `sfx N` | 04/02-sound ops |
| `exit` / `despawn` | 0F / 10 |

Channel expressions (30C98 modes) appear wherever an operand slot allows
them: `literal`, `obj.FIELD`, `[NAME]`, `partner.FIELD`, `random`.
Modes 6/7 are write-back escapes — kept as explicit low-level nodes
(`ch6!`/`ch7!`) because the original RETN-abuse is semantic.

## Anim-code constructs ↔ 27 commands

| DSL | cmd |
|-----|-----|
| `frame end` / `delay N` | 0E / 0F |
| `loop { ... }` | 05/06 pair |
| `goto` | 03 |
| `sprite advance N` / cond-variants | 00/01 |
| `sfx N` | 02 |
| `x += d` / `y += d` | 07/09 |
| `x abs [d0, d1, ...]` / `y abs [...]` | 08/0A (one delta per sub-sprite) |
| `flags xor 0x200/0x400/0x600` | 10/11/12 |
| `mask ...`, `palette ...`, `type ...` | 0D/13/0C/15 |
| `res N` / decompress | 17/14 |
| `end` | 1A |

## Compile-back requirement

`lvsc template.lvs -o chunk.bin` must reproduce the original chunk
byte-for-byte (same record table, same code layout — layout hints are
carried as `@0x13DB`-style anchors emitted by the decompiler until the
byte-identity stage is retired in favor of behavioral golden).

## Implemented format — .lvs v1 (anchored, states + expressions)

What `tools/data/lvs_full.py emit2` produces today and `compile_lvs`
compiles back byte-identically (proof: `roundtrip2` on all six chunks,
`diff_bytes=0`). Everything after `;` is comment — the compiler uses
only the anchored tokens, so the whole readability layer is free.

```
chunk 01C1 size 48972
record 00 sprite=FFFE flags=01 code=3850 rest=<hex>   ; 0x15-byte record
state S_3853 {  ; end=yield  ; entry t00.spawn
  op @3853 51 0000  ; acc = 0
  op @3856 73 16 T3863  ; when acc == self.spawn_pool -> S_3863
  op @385A 00   ; yield
}
an @2618 14 00  ; sprite 0
an @261A 0F 02  ; delay 2; end_frame
blob @0607 <hex>              ; data tables / dead code / unmeasured VARs
```

Line grammar (compiler side):
- `chunk HEX size N` — allocates the image.
- `record T sprite=W flags=B code=W rest=<hex>` — one template record.
- `op @PC OP [operand-hex] [Ttgt]` — object-code instruction; `Ttgt`
  re-encodes as the trailing LE word (branch/jump target).
- `an @PC CMD [operand-hex]` — anim-VM instruction; jump targets live
  inside the operand bytes (no T token).
- `blob @ADDR <hex>` — raw span (byte identity for everything the
  walkers don't claim).
- `state NAME {` / `}` — structural grouping, stripped by the compiler.

Readability layers (comment-only, sourced from verified models):
- Object operands render via `tools/data/expr.py`: 100% of the walked
  corpus (54.9k instructions) — acc forms, `when` conditions, bit
  tests with masks resolved from the static-DS LUTs (fields
  `0x9346+idx`, masks `0x93CC`, clears `0x93EC`), channel operands
  (`spawn(t, x, y, pool, fl)`, `probe_at`, tile/text/aim forms).
- Field names come from `LUT16[(idx-0x6CBA)&0xFFFF]+0x14E5` mapped
  through the `OBJ_*` constants of `src/sdl/v2_ds_layout.h`; global
  addresses through the phase-D layout names.
- State heads = record entries (P/P+3) + every control-flow target;
  ends are `yield` (next tick state), `exit`, `goto`, `fallinto`
  (shared tails), `edge` (dead tail past dyn coverage).
- Anim lengths: fixed from the handler bodies; VAR commands
  (01/08/0A/0C/13) take uniquely-measured lengths from the
  V2_ANIM_DUMP corpus (`bx_before-1` is the command pc). Unmeasured or
  ambiguous VAR sites stay inside blobs — lengths are never guessed
  (the 08/0A `2*max(1,subcnt)` ownership model validates 251/252
  against dyn but over-approximates owners, so it is not applied).

## Free-form — .lvs v1.5 (`emit3` / `compile_free`, .lvsf)

Code lines lose their anchors; layout is computed:

```
S_37DD:
o 2F            ; anim_step
o 00            ; yield
o 38 08 S_0C00  ; collision -> label
o 38 38 =0004   ; dead branch: raw word (target below 0x600)
A_2618:
a 14 00         ; sprite 0
record 00 sprite=FFFE flags=01 code=S_3850 rest=<hex>
S_1500 = S_14FF+1   ; alias: secondary decode frame inside another line
```

- Two-pass assembly: pass 1 lays out sequentially (records at their
  table slots, blobs at their anchors, code filling the gaps in file
  order) and binds labels; pass 2 encodes with resolved words.
- `=HEX` = raw target word (dead branches into the record zone,
  out-of-range values). `S_x = S_y+n` = alias into an owning line for
  overlapping decode frames (real dead-branch frames that share bytes).
- op 19 renders its anim operand as `A_xxxx`; anim jump/loop targets
  are symbolic the same way.
- Invariant: an unedited `emit3` text compiles byte-identically (all
  six chunks). Edits recompute every reference; a collision map raises
  when inserted code overflows its gap into an anchored element —
  moving data blobs (gap management) is the v2 roadmap item.

## Authoring pipeline — `tools/data/lvsc.py`

```
lvsc build FILE.lvsf CHUNK_HEX          # compile + verify vs extracted chunk
lvsc pack 1C1=mod.lvsf -o DATA_NEW.DAT  # rebuild DATA.DAT with replacements
```

Replaced chunks are packed with a greedy LZSS equivalent to the
engine decompressor (sub_10982). Engine constraints honored: the u16
size field is `len-1` (the decoder emits field+1 bytes), compressed
blocks must stay `< 0xB080` (the original aborts otherwise). `pack`
with no replacements reproduces DATA.DAT byte-identically. Proof: a
full level1 canon replay on a repacked (recompressed-1C1) DATA.DAT
ends in a state identical to the golden except the ds:2BB4 mirror of
the file offsets themselves.

Known non-goals of v1.5 (roadmap for v2):
- Gap management / movable data blobs (today an insertion that
  outgrows its gap is a hard error, not a relayout).
- Semantic state names (`S_walk` instead of `S_3853`).
- Data-table decoding of the remaining blobs (14-byte records with
  `db13` markers, palette blocks).
