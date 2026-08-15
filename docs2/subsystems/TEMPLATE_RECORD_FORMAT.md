# Template record format (0x15 bytes) — the object/anim descriptor

Source of truth: `v2_obj_template_init_13e52` (spawn-side reader, byte
for byte against orig eips 0x3E52..0x3F2A) and the anim-switch site
(`v2_vm.cpp:17559`). Records live at the head of the six template chunks
(0x1C1..0x1C6); `template_index * 0x15` addresses a record.

| off | size | field | consumer |
|-----|------|-------|----------|
| +0  | word | sprite chunk id (via `sub_12f82` lookup → OBJ_SPRITE_BASE; CF-fail aborts spawn) | spawn |
| +2  | byte | sub-sprite count (low 7 bits) \| 0x80 = "big pool" flag (spawn_pool_sel += 2) | spawn |
| +3  | word | code pointer P | spawn sets `OBJ_PC = P + 3`; anim-switch sets `OBJ_PC = P` |
| +5  | word | (not read by spawn — anim-VM side, TBD) | anim |
| +7  | word | OBJ_RES_HANDLE | spawn |
| +9  | byte | OBJ_WIDTH | spawn |
| +A  | byte | OBJ_HEIGHT | spawn |
| +B  | word | OBJ_RES_COST | spawn |
| +D  | word | OBJ_STATE_IDX | spawn |
| +F  | word | OBJ_CLASS_BITS | spawn |
| +11 | word | OBJ_VEL_X_MAX clamp | spawn |
| +13 | word | OBJ_VEL_Y_MAX clamp | spawn |

## The P / P+3 asymmetry (why every script "starts" with a jump)

The word at +3 points at a 3-byte `03 xx xx` (JMP) instruction — the
record's *anim redirect*. An animation switch jumps THROUGH it
(`OBJ_PC = P`, the jump executes, landing wherever this record's
behavior lives), while a fresh spawn skips it (`OBJ_PC = P + 3`),
falling into the code right AFTER the redirect — the spawn-time
constructor path. One record therefore carries two entry points sharing
one address: `P` (anim entry, via the jump) and `P+3` (spawn entry).

Disassembler consequence: both `P` and `P+3` are roots; the record table
extent needs no explicit count — records are referenced by spawn tables
(+8 field), op_14 operands and anim switches, all walkable.
