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
