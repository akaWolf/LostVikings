# .lvd — the logic language of the world scripts (level C, minimum)

`.lvd` is what the logic editor shows for a world script (`/logic/<cid>?lang=lvd`
in `tools/assets/edit_server.py`) and what `tools/data/lvsd.py` reads and writes.
It is a readable layer over the free-form `.lvsf` (docs2/DSL_SPEC.md, v1.5):

* one statement = one object-code instruction (an `o XX …` line of the .lvsf);
* the compiler lowers every statement back to exactly that instruction and hands
  the text to `lvs_full.compile_free`, the only place that lays bytes out;
* anim code, data blobs, aliases and palette anchors pass through verbatim.

So the opcode logic is the verified one of the engine (`src/sdl/v2_vm.cpp`), the
language only names things. `python3 tools/data/lvsd.py check` decompiles the
six scripts and compiles them back: every chunk must be byte-identical.

## File shape

```
chunk 01C1 size 48972              ; a lower bound: the size follows the content
alias S_1500 = S_14FF+1            ; two decode frames sharing bytes (rare)
P_3BF6:                            ; a palette pointer target (op13 d9) — a label
blob <hex>                         ; on the 48-byte block that follows
class baleog record=00 sprite=FFFE flags=01 entry=baleog_anim rest=0300…
state baleog_anim:                 ; @3850
    goto S_5C87
state baleog_spawn:
    self.state_187d = 0x63
    self.type_id = 0x2F
    anim A_403A
    self.flags |= 0x2000
    anim_step
    if 0x2F == [level] goto S_13DB
    yield
A_2618:
a 14 00                            ; anim code, verbatim
blob <hex>                         ; data with no reference from the code (dead)
```

* `;` starts a comment. Statements are indented by four spaces; everything else
  starts at column 0.
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
`anim A_xxxx` (or `anim =HHHH` for a raw pointer) `op13 …` (raw sub-command;
`op13 d9 P_xxxx` = palette pointer).

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

Channel forms (opcodes whose operands are typed channels): `spawn(t=10, x=…,
y=…, pool=…, fl=…)`, `tile[x,y] = v`, `a, b = delta(nearest_vik)`,
`a, b = quad(x, y)`, `l = tile_type(x, y)`, `text(id=…, edge=…, x=…, y=…)`,
`vel_to(x=…, y=…)`, `aim(x=…, y=…, thr=N)`, `if probe_at(NN, x=…, y=…) goto L`.
A channel value is a literal, `self.f`, `[g]`, `partner.f`, `random()`, or the
low-level `ch5` / `ub6(XX)` / `ub7(XXXX)`; a channel target is `self.f`,
`[g]`, `partner.f` or `drop`.

Anything without a nicer form keeps its engine name (`res_deduct(partner)`,
`cmdq_push(6, N)`, `pal_shade(r,g,b)`, `mark_anim_sub`, …) or the raw
`opXX <hex>`.

## Operand tokens

* Fields: the `OBJ_*` names of `src/sdl/v2_ds_layout.h`, lower-case (`world_x`,
  `timer`, `state_187d`). The byte in the instruction is a field INDEX; when a
  name is reached by several indices the token carries it: `flags#08`.
* Globals: `[name]` with the layout names (`[level]`) or `[HHHH]`.
* Bit masks: `0xMASK`, with `#idx` when several mask indices share the value
  (`0x8000#1E`).
* Literals: decimal or `0x…`; signed compares take negative literals.
* Anim pointers: `A_xxxx` (labels of the anim code), sounds: `sfx N`.

## Names

`tools/data/lvs_names.json` — `states` (`"1C1:3853": "baleog_spawn"`),
`classes` (`"*:01": "erik"`, or per script `"1C1:10"`), `anims`, `sfx`.
`lvsd.py seed` fills the record entries (`<class>_anim` / `<class>_spawn`);
everything else is named by hand as the scripts get understood. A name is a
view: renaming a state changes no byte.

## Editor

`/logic/<cid>?lang=lvd`: the left pane is the .lvd listing with links on
`goto`/`call`/`entry` targets, the right pane edits the text; check / compile →
scratch / pack / play / revert are the level-B loop. Compile stores the lowered
`.lvsf` (what the packer and the scene templates consume) and the `.lvd` source
next to it; a later direct `.lvsf` edit supersedes the `.lvd` copy.
