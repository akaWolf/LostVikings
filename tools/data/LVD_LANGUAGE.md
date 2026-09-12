# .lvd — the logic language of the world scripts (level C, minimum)

`.lvd` is what the logic editor shows for a world script (`/logic/<cid>?lang=lvd`
in `tools/assets/edit_server.py`) and what `tools/data/lvsd.py` reads and writes.
It is a readable layer over the free-form `.lvsf` (docs2/DSL_SPEC.md, v1.5):

* one statement = one object-code instruction (an `o XX …` line of the .lvsf)
  or one anim-code command (an `a XX …` line);
* the compiler lowers every statement back to exactly that instruction and hands
  the text to `lvs_full.compile_free`, the only place that lays bytes out;
* data blobs, aliases and palette anchors pass through verbatim.

So the opcode logic is the verified one of the engine (`src/sdl/v2_vm.cpp`), the
language only names things. `python3 tools/data/lvsd.py check` decompiles the
six scripts and compiles them back: every chunk must be byte-identical.

## File shape

```
chunk 01C1 size 48972              ; a lower bound: the size follows the content
alias S_1500 = S_14FF+1            ; two decode frames sharing bytes (rare)
palette P_3BF6:                    ; @3BF6 — a palette block (see Palettes)
    rgb 0, 0, 0   ; 0
    rgb 62, 62, 62   ; 1
    ...
class baleog record=00 sprite=FFFE flags=01 entry=baleog_anim rest=0300…
state baleog_anim:                 ; @3850
    goto S_5C87
state baleog_spawn:
    self.state_187d = 0x63
    self.type_id = 0x2F
    anim baleog_a7
    self.flags |= 0x2000
    anim_step
    if 0x2F == [level] goto S_13DB
    yield
anim baleog_a7:                    ; @403A — an anim stream (see Animations)
    sprite 0
    wait 4
    goto baleog_a7
blob <hex>                         ; data with no reference from the code (dead)
```

* `;` starts a comment. Statements are indented by four spaces (switch cases
  by eight), the inner labels of a func by two; everything else starts at
  column 0.
* `state NAME:` names a code label. NAME is a dictionary name, an automatic
  owner name, or a name you write yourself (letters, digits, `_`; not four hex
  digits). Targets of `goto` / `call` / `entry=` use the same names. The
  automatic names come from the class that owns the code: `erik_12` is the
  12th state (by address) of the class named `erik`, `t84_3` the 3rd of the
  unnamed record 0x84, `shared_67_79_84_1` code reached from three classes.
  Rename a class in `lvs_names.json` and all its automatic names follow; a
  hand-written name in the dictionary or in the text always wins.
* `class NAME record=T sprite=W flags=B entry=STATE rest=<hex>` is one record
  of the class table (the 0x15-byte template record). The name comes from
  `lvs_names.json` (`t10` when unnamed).
* Layout: the class records sit at their table slots; everything else — code,
  anim code, `blob <hex>` data — is laid out in file order, and every reference
  is a label, so inserting or deleting a statement anywhere just moves what
  follows. A dead `blob` line can simply be deleted. The one limit is the
  template buffer: a script larger than 49152 bytes is a compile error that
  says by how much.

## Statements

Control: `yield` `nop` `return` `exit` `despawn` `goto L` `call L`
`anim NAME` (or `anim =HHHH` for a raw pointer) `palette NAME` (op 13 d9: the
dialogue palette of the finale), `quit_to_dos pad a,b`, `hud_to_viewport pad a,b`
(the other op 13 sub-commands; the two pad bytes are never read).

Branches: `if <cond> goto L` and `if <cond> call L` (the call families push the
return address). `search_*(f=N) goto L` are the object searches. The condition
forms are the engine's compare, bit-test, probe, collision and scan opcodes —
see the table `OPS` in `lvsd.py`, e.g. `if acc == 0x2F`, `if acc >=s self.timer`,
`if acc != bit(self.flags & 0x40)`, `if probe_down(5)`, `if coll_155d6(f=2)`,
`if !in_viewport(self)`.

Accumulator: the VM has one accumulator, `acc`. Loads: `acc = 5`, `acc = self.f`,
`acc = [global]`, `acc = partner.f`, `acc = random()`, `acc = bit(self.f & MASK)`.
Stores / read-modify-write: `self.f = acc`, `[g] += acc`, `partner.f &= acc`,
`self.f +=hflip acc` (add or subtract by the flip bit), `self.f = (acc <<= 8)`,
`set_partner acc`, `spawn_rec[+0] = acc`, `self.f = setbit(self.f, MASK, acc)`,
`[g] |= (acc ? MASK : 0)`.

Folded form: when a load is immediately followed by a statement that uses `acc`
once and that statement is not a jump target, the two are written as one:

```
self.timer = 0x40            ;  acc = 0x40  +  self.timer = acc
if 0 != self.state_18a5 call S_197F   ;  acc = 0  +  if acc != self.state_18a5 call …
[0208] += partner.world_x    ;  acc = partner.world_x  +  [0208] += acc
```

The left operand is always the accumulator load; the compiler expands the
folded form back into the two instructions (which load it is follows from the
shape of the value). Write `acc = X` on its own line when the next statement
must stay a jump target or uses `acc` twice. The bit stores fold too:
`self.f = setbit(self.f, 0x40, bit(0x40 & 0x40))`, `[g] |= (bit(...) ? M : 0)`.

Switch blocks: two or more consecutive compare-and-branch statements on the
same value fold into a block, one case per line, indented by eight spaces:

```
    switch self.state_18f5:        ;  acc = self.state_18f5; if acc == 1 goto …  (per case)
        1 -> t67_23
        2 -> t67_24
    select self.state_18f5:        ;  acc = 1; if acc == self.state_18f5 goto …  (per case)
        1 -> t67_23
```

`switch` loads the value and compares it with each literal (the field-loaded
opcodes), `select` loads each literal and compares it with the value (the
literal-loaded opcodes). Both test the same thing; the two keywords keep the
two opcode sequences apart, so the fold is exact. A case line is `N -> label`.

Speech: the seven-statement bubble idiom folds into one line —

```
    say partner=4 dy=-0x1C cmd=0x110D id=0x5D edge=2
```

stands for `set_partner 4` (the viking spoken to: object slot 0/2/4),
`[0206], [0208] = delta(partner)` (the bubble's column/row from the partner's
position), `[0208] -= 0x1C` (`dy=+N` for `+=`), `cmdq_push(6, 0x110D)`,
`text(id=0x5D, edge=2, x=[0206], y=[0208])` (`edge` a literal or `self.f`),
`cmdq_push(4)`, `cmdq_push(2)` — in that order, with nothing between. Any
deviation stays written out.

Idioms (level C): fixed statement runs the decompiler folds into one line and
the compiler writes back byte for byte — the low form is always accepted too.

```
    if self.flags#08 & 0x40#0C goto L        ;  acc = bit(1 & 0x1#00); if acc == bit(self.flags#08 & 0x40#0C) goto L
    if !(self.flags#08 & 0x40#0C) goto L     ;  acc = bit(1 & 0x1#00); if acc != bit(...) goto L
    if bit(self.flags#08 & 0x40#0C) == 0 goto L   ;  acc = bit(0 & 0x1#00); if acc == bit(...) goto L   (!= 0: the != branch)
    if same_facing(partner) goto L           ;  acc = bit(self.flags#08 & 0x40#0C); if acc == bit(partner.flags#08 & 0x40#0C) goto L
    if self.spawn_pool & [switches] == self.spawn_pool goto L   ;  [tmp_a] = self.spawn_pool; [tmp_a] &= [switches]; if self.spawn_pool == [tmp_a] goto L
    hurt partner event=7 amount=2 facing     ;  partner.event = 7; partner.event_arg = 2; partner.event_arg = setbit(partner.event_arg, 0x8000#1E, bit(self.flags#08 & 0x40#0C))
    anim_by_viking erik=A, baleog=B, olaf=C goto L   ;  select self.anim_idx: 0 -> b, 2 -> c; anim A; goto L;  b: anim B; goto L;  c: anim C; goto L
```

The four bit-test spellings keep the four opcode pairs apart (the constant bit
0 or 1, `==` or `!=`); `call` replaces `goto` for the call families. The mask
gate reads "every bit of A is set in B" (`!=`: not every bit) — the switch
words `[switches]` / `[switches_b]` / `[hints_done]` against an object's pool.
`hurt`: the event kind, the amount, and the direction bit of the amount taken
from the hurter's facing. `anim_by_viking` also stands for the two side states
(`b`, `c`) the compiler writes right after the block; `fallthrough` at the end
means the olaf state has no `goto` and falls into the next block. A state whose
last statement is a jump to itself is written `loop NAME:` with the jump
implied. Field names come from the dictionary too (`fields`: `event` /
`event_arg` for the engine's `state_18a5` / `state_18cd`).

Channel forms (opcodes whose operands are typed channels): `spawn(t=10, x=…,
y=…, pool=…, fl=…)`, `tile[x,y] = v`, `a, b = delta(nearest_vik)`,
`a, b = quad(x, y)`, `l = tile_type(x, y)`, `text(id=…, edge=…, x=…, y=…)`,
`vel_to(x=…, y=…)`, `aim(x=…, y=…, thr=N)`, `if probe_at(NN, x=…, y=…) goto L`.
A channel value is a literal, `self.f`, `[g]`, `partner.f`, `random()`, or the
low-level `ch5` / `ub6(XX)` / `ub7(XXXX)`; a channel target is `self.f`,
`[g]`, `partner.f` or `drop`.

Sounds: `sfx NAME vol V` plays sequence NAME (op 02; the word is V<<8 |
sequence — the PC driver reads the sequence byte, V is the console's volume
byte kept for the SNES build), `sfx_stop NAME` stops it (op 04). NAME comes
from the `sfx` dictionary (`speech_blip`, `arrow_break`, `slide_loop`, …) or
is the sequence number.

Anything without a nicer form keeps its engine name (`res_deduct(partner)`,
`cmdq_push(6, N)`, `pal_shade(r,g,b)`, `mark_anim_sub`, …) or the raw
`opXX <hex>`.

## Functions

The VM has `call` (op 05: the return address goes to the object's single
OBJ_ALT_PC field) and `return` (op 06). There is no stack: a second call — or
any call-branch such as the collision families — before the return overwrites
the address. A `func` is a subroutine that respects that:

```
func t60_5:                     ; @5C1E
    if self.timer == 0 goto t60_6
    self.timer -= 1
    return
  t60_6:                        ; an inner label: a block of the same func
    anim A_5F10
    return
    ...
    call t60_5(self.state_187d = 3, acc = 1)
```

* `func NAME:` declares it; the blocks that follow under inner labels
  `  NAME:` (two spaces, then the name) belong to it, up to the next `state`
  / `func` / class / data line. The decompiler writes `func` for every call
  target whose run of blocks qualifies (the longest such run), everything
  else stays `state` (it can still be called — it just is not checked).
* The rules, checked by the compiler on every `func` (each violation names
  its line): the header is entered by `call` only (no goto, no case, no
  class entry, and the code before it does not fall into it — a `yield`
  falls through in time); an inner label is entered from inside the func
  only, is never called and is no class entry; no `call` / `if … call`
  anywhere inside; the last block ends in `return`, `goto`, `exit` or
  `despawn` (nothing falls out of a func). Branches OUT of a func to a state
  are allowed: the flow leaves without returning, which the VM does not mind.
  Loops inside (`goto` back to the header or an inner label) are allowed.
* Arguments: `call F(self.f = N, [g] = N, partner.f = N, acc = X)` is exactly
  the literal stores, then the optional `acc = X`, then `call F` — in that
  order, because a store uses the accumulator. The value a function leaves in
  `acc` is its result by convention; nothing enforces it.

## Animations

An anim stream is the anim VM's program (v2_vm_exec_anim_cmd): the object
code starts one with `anim NAME`, the engine runs one frame of it per tick
until a frame end, and the sub-sprites of the object are what it moves.
Every `A_xxxx:` label of the .lvsf is an `anim NAME:` header (dictionary
name from `anims` in lvs_names.json, else the automatic owner name
`<class>_a<k>` — the k-th anim label, by address, reached from that class's
code — else `A_xxxx`); its commands are indented statements:

```
anim erik_a3:                   ; @2618
    bank 0xE3                   ; 17: sprite bank = chunk id
    type 2                      ; 15: sprite type (renderer) + strip count
    sprite 5                    ; 14: decompress image 5 into the sub-sprite buffer
    frame 0                     ; 01: sub-sprite frame(s), one per sub-sprite
    x -16, 0                    ; 08: sub-sprite x = object x + N, one per sub-sprite
    y 0, 0                      ; 0A
    pal 6                       ; 0C: colour bank bits of the sprite flags ((N << 3) & 0x70)
    sfx 0x50 vol 0x7F           ; 02: play sequence 0x50 (the vol byte is the console's; the PC ignores it)
    wait 4                      ; 0F: end of frame, 4 ticks
    yield                       ; 0E: end of frame, the next tick continues here
    dx -4                       ; 07: masked: sub-sprite x += N; unmasked: object velocity x += N
    dy 2                        ; 09
    frame += 1                  ; 00: advance every (masked: matching) sub-sprite by N frames
    mask 2                      ; 0D: sub-sprite class mask for the rest of this frame
    class 1, 2                  ; 13: the sub-sprite classes that `mask` selects
    flip_x                      ; 10 (flip_y = 11, flip_xy = 12): XOR the flip bits
    hide                        ; 18 (show = 19): OR 0x4000 / AND 0x9FFF — a hidden sprite is skipped by every draw pass
    call erik_a9                ; 05: run another stream, `return` (06) comes back here
    goto erik_a3                ; 03
    stop                        ; 1A: the anim ends (pc = FFFF)
    skip 0x58                   ; 04: one dead byte (`skip N #16` = command 16, the same effect)
    int3                        ; 0B
```

The per-sub-sprite lists (`frame`, `x`, `y`, `pal`, `class`) carry as many
values as the command consumes for that object — the decoder's static run
of the stream (tools/data/anim_static.py) — so a list is copied as it is and
a new one gets one value per sub-sprite the class has (one per matching
sub-sprite under a `mask`). `goto`, `call` and `return` inside a stream are
the anim VM's, not the object code's: the line is under an `anim` header.

## Palettes

`palette NAME:` heads a palette block — the 48 bytes an op 13 d9 statement
(`palette NAME`) copies into the dialogue rows of the DAC (v2_vm_op_13):
sixteen `rgb r, g, b` lines, 6-bit VGA values (0..63), the colour index in
the comment. The name is the machine `P_xxxx` unless the author names it
(`palette gold_text:`). A block of another length is written the same way,
three bytes per line, a remainder as `blob`.

## Operand tokens

* Fields: the `OBJ_*` names of `src/sdl/v2_ds_layout.h`, lower-case (`world_x`,
  `timer`, `state_187d`). The byte in the instruction is a field INDEX; when a
  name is reached by several indices the token carries it: `flags#08`.
* Globals: `[name]` with the layout names (`[level]`), the dictionary names
  of `mem` in lvs_names.json (`[switches]`, `[tmp_a]`, `[crane_ctl_0]` — they
  override the layout name of that word) or `[HHHH]`. A name must map to one
  address; an ambiguous one falls back to `[HHHH]`.
* Bit masks: `0xMASK`, with `#idx` when several mask indices share the value
  (`0x8000#1E`).
* Literals: decimal or `0x…`; signed compares take negative literals.
* Anim pointers: `A_xxxx` (labels of the anim code), sounds: `sfx N`.

## Names

`tools/data/lvs_names.json` — `states` (`"1C1:3853": "baleog_spawn"`),
`classes` (`"*:01": "erik"`, or per script `"1C1:10"`), `anims`
(`"1C1:2618": "erik_a_walk"`), `pals` (`"1C6:3BF6": "pal_trex_blue"`),
`mem` (`"023C": "crane_ctl_0"` — DS words, global to the six scripts), `sfx`
(`"1C": "arrow_break"` — the sound sequences: `sfx arrow_break vol 0x7F` /
`sfx_stop slide_loop` in object code and `sfx zap vol 0x7F` in anim streams;
a number is always accepted instead of the name), `fields` (`"state_18a5":
"event"` — object fields over the OBJ_* names).
Anim names come from the states that set the stream (`scorpion_a_attack` from
`scorpion_attack`), `_cont` for a continuation reached only from another
stream, `_sub` for a called sub-stream, `<class>_a_default` for a stream no
code sets (the record's own).
`lvsd.py seed` fills the record entries (`<class>_anim` / `<class>_spawn`);
everything else is named by hand as the scripts get understood. A name is a
view: renaming a state changes no byte. Class names are global (`*:XX`) —
the class table is the same in the six scripts — unless a type is a different
object in one script (`1C2:4A`); they follow the manual and the walkthroughs
(keys, bombs, force fields, teleporters, striped doors; green/red aliens,
T-rex, caveman, snail, scorpion, mummy, spear guard, pounders, gun robot).
Hand anim names take `<class>_a_<what>` so they never collide with a state
name (`lvsd check` refuses a name shared by a state and an anim, and flags a
`_w` tail or a double underscore left by a batch rename).
Change names with the tool, not by editing the automatic numbers: the
`shared_XX_N` / `<class>_N` numbering shifts whenever a dictionary name is
added, so a batch keyed by those numbers lands on the wrong states.
`lvsd.py rename CID NAME NEW` (or `@ADDR NEW`, `--all` for every script that
shows NAME) writes the entry by address; `lvsd.py rename-class XX NEW [CID]`
renames a class; `lvsd.py propagate SRC [DST ...]` copies the state names of
one script to the others where the same code (owner set + normalised
statements) has no name yet.

## Editor

`/logic/<cid>?lang=lvd`: the left pane is the .lvd listing with links on
`goto`/`call`/`entry` targets, the right pane edits the text; check / compile →
scratch / pack / play / revert are the level-B loop. Compile stores the lowered
`.lvsf` (what the packer and the scene templates consume) and the `.lvd` source
next to it; a later direct `.lvsf` edit supersedes the `.lvd` copy.
