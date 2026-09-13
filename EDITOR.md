# Level editor, logic editor and mods

This is the guide for people who want to change the game: move a platform,
add a monster, redraw a tile, rewrite a dialogue, change what a switch does,
or build a whole new level in an existing slot — and hand the result to
someone else as a mod package. Everything here works on the open asset tree
that the tools derive from your own `DATA.DAT`; the game plays that tree
through its content pack mechanism, and nothing in the repository ships game
data.

The tools are Python 3 scripts with no third-party dependencies (only the two
font rasterisers of the localisation build need Pillow). They live in
`tools/assets/` and `tools/data/` and are also shipped in every release bundle
as `content-tools/`.

Contents:

1. [What you can change](#what-you-can-change)
2. [Setup](#setup)
3. [The editor server](#the-editor-server)
4. [The level page](#the-level-page)
5. [Sprites and animation banks](#sprites-and-animation-banks)
6. [Dialogue texts](#dialogue-texts)
7. [Logic: the world scripts](#logic-the-world-scripts)
8. [Testing a level](#testing-a-level)
9. [Command-line tools](#command-line-tools)
10. [Sharing: mod packages](#sharing-mod-packages)
11. [Limits](#limits)
12. [Under the hood](#under-the-hood)

## What you can change

| Material | Where | How it is stored |
| --- | --- | --- |
| The map of a level (16×16 quads) | level page, stamp / set type / select tools | `tilemaps/<id>.json` |
| Spawn table: every object of a level with its position, size, class, animation and pool | level page, spawn tool | `level_headers/<id>.json` |
| Level size | level page, resize | tilemap + header |
| Quad templates (the 2×2 tile blocks the map is made of, with flips) | level page, template editor | `bg_tilesets/<id>.json` + `.png` |
| Tile pixels and collision masks | level page, tile pixels panel | `tilesets/<id>.png`, `tile_masks/<id>.json` |
| Palette list and palette animations of a level | level page, palette panels | header |
| Sprite banks (uncompressed, the small objects) | `/sprites/<id>` | `sprite_banks/<id>.bin` |
| Animation banks (compressed frames: vikings, monsters) | `/anim/<id>` | `misc/anim_bank/<id>.bin` |
| Dialogue and menu texts | `/texts` | `texts_exe.json`, baked into the EXE image |
| Object logic: the world scripts that drive every class | `/logic/<id>?lang=lvd` | `level_scripts/<id>.lvsf` |
| A copy of a level in another slot of the same world | level list, clone | header + tilemap |

Every converter is verified in both directions: the open tree compiles back
to the archive byte for byte, so anything you do not touch stays exactly as
the 1992 game had it.

## Setup

You need:

- Python 3 (3.8 or newer).
- A build of the game (`make` → `vikings`; `HEADLESS=1 RELEASE=1 make`
  → `vikings_headless` if you want scripted test runs). The editor starts the
  game from the repository root, or from the bundle directory when run from a
  release bundle.
- Your `DATA.DAT` in the repository root (see README, "DATA.DAT").

### The open tree

The editor works on a scratch copy of the canonical open tree `assets/`.
Create that tree once from the archive:

```sh
python3 tools/data/extract_datadat.py DATA.DAT    # DATA.DAT -> assets_raw/ (535 chunks, round trip verified)
python3 tools/assets/assetc.py                    # assets_raw/ -> assets/ (every chunk converted and compiled back byte for byte)
```

Both directories are git-ignored; `assets_raw/` already carries the two
inputs the converters need (`chunk_map.json`, the chunk roles, and `lvs/`,
the text of the six world scripts).

### Editing the console content pack instead

If you built the console content (`make content`, README "Console content"),
`content/` is an open tree of the same layout with the SNES and Genesis
material inside, plus `.compiled/` and `exe_static.bin`, and the game picks
it up by itself. Point the editor at it and the pack is edited in place:

```sh
python3 tools/assets/edit_server.py --scratch content
```

After "pack" the running copy of the game sees the change at its next start.
Keep in mind that a mod package exported from such a tree contains every file
that differs from the plain PC tree — the whole console material, which is
derived from the ROM images and must not be redistributed. See
[Sharing](#sharing-mod-packages).

### From a release bundle

The bundle's `content-tools/` holds the same scripts with their tables and the
six script texts. Run them from inside that directory; the tree is created
there:

```sh
cd content-tools
python3 tools/data/extract_datadat.py ../DATA.DAT assets_raw
python3 tools/assets/assetc.py
python3 tools/assets/edit_server.py
```

The editor's **play** button starts `vikings` from the directory it considers
its root, and the game reads `DATA.DAT`, `exe_static.bin` and `ds_static.bin`
from its working directory: the bundle already holds the two images in
`content-tools/`, so put (or symlink) the game executable and `DATA.DAT`
there as well.

One thing the bundle lacks: the `.lvsf` listing page shows the decompiler's
expression next to each instruction only when the anchored listings
`assets_raw/lvs/*.lvs` are present (they are built from the engine's runtime
traces and are not part of the bundle, nor of a fresh checkout). The `.lvd`
page and the `lvsd.py` commands need nothing beyond the bundle.

## The editor server

```sh
python3 tools/assets/edit_server.py [--port 8137] [--scratch DIR] [--fresh]
```

Open `http://127.0.0.1:8137/`. The server creates the scratch tree
(`/tmp/lv_edit_scratch` by default) from `assets/` on the first start and
reuses it afterwards, so edits accumulate across sessions; `--fresh` throws it
away and starts over. The canonical `assets/` is never written.

The level list shows every level of the game's level table that exists in the
scratch tree: its slot number, its password, the id of its header chunk, the
id of the world script that drives it, and a thumbnail. Below the table:

- **dialog texts** — the `/texts` page.
- **SNES exclusives** — the conversion page of the five SNES-only levels
  (needs the SNES DE image; the `make content` build does the same for you).
- **logic** — the six world scripts, as annotated `.lvsf` listings and as
  `.lvd` source (level C); a `*` after an id marks a script that already has
  an edited copy in the scratch tree.
- **clone level (same world)** — copies the level of slot `src` into slot
  `dst`: the destination header becomes the source's, retargeted to the
  destination's own tilemap chunk, so the copy is editable on its own. The two
  slots must belong to the same world because the script that drives a level
  comes from the level table, not from the header.
- **mod package** — export the differences to a JSON file, or import one
  (then press pack on any level page). The same two operations exist on the
  command line as `--export-mod OUT.json` and `--import-mod MOD.json`; the
  import needs an existing scratch tree (start the server once to create it —
  on a directory that does not exist yet the import writes only the mod's own
  files, which is not a playable tree).

Every page has the same three verbs. **save** writes the page's material into
the scratch tree. **pack** compiles the whole scratch tree into
`.compiled/` (the store the engine reads) and bakes the EXE image with the
current texts. **play** starts the windowed game against the packed scratch
tree. The level page has a **save+pack+play** button for the whole loop.

## The level page

`/edit/<header id>`, for instance `/edit/0028`. The level is drawn on a canvas
with a 16 px quad grid and a minimap for navigation. The right column holds
the spawn form and the collapsible panels; the bottom shows the template
palette — every quad template of the world's tileset, click to select one.

Tools (the drop-down or a key while the canvas has focus):

| Key | Tool | What it does |
| --- | --- | --- |
| `s` | stamp | paint the selected template; click, or drag a rectangle (one undo entry per rectangle) |
| `t` | set type | set the attribute bits of the quads (the collision / behaviour type) without changing the template |
| `p` | pick | take the template and type under the cursor into the selection |
| `w` | spawn | select a spawn marker; drag it to move the object |
| `e` | select | drag a rectangle of quads; `Ctrl+C` copies it, `Ctrl+V` pastes it where you click next |

`Ctrl+Z` undoes the last edit; `Esc` cancels a pending placement, paste or
rectangle. Zoom 1 to 3.

**Spawns.** Selecting a marker fills the form with the record's fields: x, y,
half width, half height, class, animation and pool — the same layout the
engine's spawner reads. **apply** writes the form back, **add (click map)**
places a new record where you click next, **delete** removes the selected one;
the table's length is free, records are terminator-scanned. Under the form the
page names the class and the sprite bank it needs; when the level's header
does not list that bank, **add bank** appends it to the header's bank list.

**Resize.** Enter new dimensions in quads and press **resize+save+reload**:
the tilemap rows and the header's size fields are rebuilt (server mode only).

**Tile pixels.** Select a quad, then a tile of it, and edit its 8×8 pixels in
the level's palette: the left button paints the current colour, the right
button picks a colour from the tile. The **mask layer** checkbox switches to
the collision mask of the tile (left button sets a pixel opaque, right button
clears it). **load sel tile** edits an existing tile in place, **add new
tile** appends one to the tileset.

**Template editor.** A template is four tiles with per-corner horizontal and
vertical flips. **load sel tpl** takes the selected template into the editor,
**apply to sel** rewrites it (every quad using it changes), **add as new**
appends a new template to the world's template chunk, which grows as needed.

**Palette list / palette anims.** The header's palette entries
(chunk + start colour) and the colour-cycling ranges the game animates, each
with **add entry** / **add anim** and a **del** per row.

**Export.** Without the server, the page can download the tilemap as `.json`
or `.bin` and the header as `.json`; `playtest.py` takes those files (see
[Command-line tools](#command-line-tools)).

## Sprites and animation banks

`/sprites/<id>` opens an uncompressed sprite bank — the small objects: items,
switches, projectiles. It is a grid of 72-byte units, each four planes by two
strips of one mask byte and eight data bytes, shown as pixels in the palette
of the first level whose header lists the bank. Frames are assembled from
units by the animations, so the page shows the raw unit grid. **save** writes
the bank.

`/anim/<id>` opens a compressed animation bank — the vikings and the
monsters: 32×32 frames of 4-bit colours (the palette layer is added when the
game draws them). **save** re-encodes the bank; the encoder is the exact
mirror of the game's decoder and reproduces every original bank byte for
byte.

The bank ids come from the spawn form of the level page (the class's bank is
named under the form) and from the header's bank list.

## Dialogue texts

`/texts` lists every string of the EXE image with its index and its box size
in characters, each in an editable field. **save** writes `texts_exe.json` to
the scratch tree, and the next **pack** bakes the strings into the scratch
copy of the EXE image. A string must fit its original place: the compiler
refuses a longer one and names the entry.

The twelve translated languages of the console content are built from the
Blizzard Arcade Collection and are not editable here.

## Logic: the world scripts

Every object in the game — a viking, a monster, a switch, a platform, a
bubble — runs a script of its class inside the game's own bytecode VM. The
scripts of all classes of a world live in one chunk, the world script; the
level list's "lvs" column names it for each level. Six such scripts drive the
game: `01C1` to `01C6`.

The editor shows a world script in two forms:

- `/logic/<id>` — the free-form assembler listing (`.lvsf`): one line per
  instruction, annotated with its address, the decompiler's reading of it, and
  the class records that enter at each label.
- `/logic/<id>?lang=lvd` — the readable language (`.lvd`): named states,
  `if … goto`, field names, animation statements, functions, templates. This
  is the form to write.

Both pages have **check** (compile without writing; reports the size and any
error with its line), **compile → scratch** (writes the compiled text into the
scratch tree; the next pack uses it), **pack**, **play** and **revert to
canonical** (removes the edited copy; the archive's chunk returns).

The language is documented in `tools/data/LVD_LANGUAGE.md` (the file shape,
statements, functions, templates, animations, palettes, names) and every
statement with its operands is listed in `tools/data/LVD_REFERENCE.md`,
generated from the compiler's tables. The short version:

```
state snail_walk:                  ; a code label, four-space indent for statements
    if bit([input_keys] & 0x100) != 1 goto snail_idle
    self.x_vel = 2
    anim snail_a_walk
    yield                          ; end of this tick
anim snail_a_walk:                 ; an animation stream
    sprite 3
    wait 4
    goto snail_a_walk
```

Names come from a dictionary (`tools/data/lvs_names.json`): 127 classes and
their states and animations are named already, the rest carry automatic names
derived from their class. A name you write in the text is kept. The
decompile → compile round trip of all six scripts is byte-identical, which is
what `python3 tools/data/lvsd.py check` verifies; run it after changing the
tools, not after changing a script.

Command line:

```sh
python3 tools/data/lvsd.py decompile 01C1 out.lvd      # assets_raw/lvs/01C1.lvsf -> readable text
python3 tools/data/lvsd.py compile in.lvd out.bin      # text -> chunk bytes (reports the size)
python3 tools/data/lvsd.py check                       # all six scripts round-trip byte-identical
python3 tools/data/lvsd.py ref                         # regenerate LVD_REFERENCE.md
python3 tools/data/lvsd.py rename 01C1 t84_3 door_open # name a state in the dictionary
python3 tools/data/lvsd.py rename-class 84 door        # name a class, all its automatic names follow
```

A script may grow: code, animation code and data are laid out in file order
and every reference is a label, so inserting a statement moves what follows.
The one limit is the engine's script buffer, 49152 bytes; the compiler
reports by how much a script exceeds it.

## Testing a level

- **play** on any page starts the windowed game against the packed scratch
  tree. The game starts from the title as usual; use the level's password
  (the level list shows it).
- `V2_START_LEVEL=<slot>` in the environment of the game jumps straight to a
  slot a few frames after the start — the slot numbers are the level list's
  first column:

```sh
V2_ASSETS_DIR=/tmp/lv_edit_scratch/.compiled V2_EXE_STATIC=/tmp/lv_edit_scratch/exe_static.bin V2_START_LEVEL=5 ./vikings
```

- `./vikings --debug` adds to the F1 menu a level chooser (`LEVEL < slot
  password > ENTER`), save and load slots (F2 / F3, nine slots), and a rewind
  (hold F8); F5 / F6 skip to the previous / next level.
- **Scripted runs.** `playtest.py` overlays editor exports on a scratch copy,
  packs it, and either starts the game or replays a recorded input headlessly
  and saves a frame as PNG — the way to compare before and after an edit
  without playing:

```sh
tools/assets/playtest.py 00CB.json                                          # play with an edited tilemap
tools/assets/playtest.py 00CB.json 00CA.json --replay tests/replays/level1.inp --snap 1300 -o out.png
```

  The same flow is behind the server's `POST /api/play_replay` (`{replay,
  frames, snap, headless}`), which returns the snapshot as a PPM.
- Input recordings: `./vikings --record-input=my.inp` records a session,
  `--replay-input=my.inp` replays it; the files are frame-based and reproduce
  identically on any machine (README, "CLI options").

## Command-line tools

All paths relative to the repository root (or the bundle's `content-tools/`).

| Tool | Purpose |
| --- | --- |
| `tools/data/extract_datadat.py DATA.DAT [outdir]` | the archive into raw chunks + a manifest with sizes and hashes; aborts unless the re-concatenation equals the archive |
| `tools/assets/assetc.py` | raw chunks into the open tree, each chunk compiled back and compared; `--pack` packs the open tree into `.compiled/` |
| `tools/assets/level_render.py 0028 [-o out.png]`, `--all [-d dir]` | a PNG of a level from the open tree alone (no game run), the engine's own map expansion and palette rules |
| `tools/assets/level_inspect.py 00CA [-o out.html]` | a self-contained HTML inspector: hover shows the tilemap word and the spawn records under the cursor |
| `tools/assets/level_atlas_index.py` | `build/levels_atlas/index.html`, all levels in game order with links to inspector and editor |
| `tools/assets/level_edit.py 00CA [-o out.html]` | the level page as a standalone HTML file (downloads instead of the server verbs) |
| `tools/assets/playtest.py` | edit-and-run loop from exported files, optional headless replay + snapshot |
| `tools/assets/edit_server.py --export-mod OUT.json` / `--import-mod MOD.json` | mod packages without the browser |
| `tools/assets/build_content.py --mod MOD.json --fresh` | a `content/` pack from the archive plus a package that carries the console content (its `extras.json` and EXE image); plain mods go through import + pack, see [Sharing](#sharing-mod-packages) |
| `tools/data/lvsd.py` | the `.lvd` logic language: decompile, compile, check, names |
| `tools/data/lvs_annotate.py 01C1 [file.lvsf]` | the annotated assembler listing of a world script on stdout |
| `tools/data/lvsc.py build FILE.lvsf 01C1` | compile a free-form `.lvsf` and compare with the extracted chunk |
| `tools/assets/anim_bank.py info|judge <id>` | frame directory of an animation bank; `judge` proves the encoder reproduces every frame |
| `tools/assets/state_render.py x.state [-o out.png]` | draw the level exactly as the engine did from a `V2_SAVE_STATE` snapshot |

The binary formats of the archive — tiles, sprites, palettes, screens, the
LZSS container, the `.compiled` store — are described in
`tools/assets/FORMATS.md`; the script language's compile model in
`docs2/DSL_SPEC.md`.

## Sharing: mod packages

A mod package is a JSON file with every file of the scratch tree that differs
from the canonical `assets/`, base64 encoded, plus three carriers when
present: `extras.json` (new chunk ids), `texts_exe.json` (dialogue edits) and
`exe_static.bin` (the EXE image with the texts baked and the extra-level
trailer). Export it from the level list page (**export mod.json**) or:

```sh
python3 tools/assets/edit_server.py --scratch /tmp/lv_edit_scratch --export-mod my_mod.json
```

A one-level mod is small: a tilemap is 7 to 16 KB, a header about 1.3 KB; an
edited world script is the largest item, 150 to 190 KB of text.

To play someone's mod, make a pack out of it: a scratch tree named `content`
in the game's directory, the package imported, packed once. From the
repository root (the open tree `assets/` created as in [Setup](#setup)):

```sh
python3 tools/assets/edit_server.py --scratch content --fresh      # creates content/ from assets/; leave it running
python3 tools/assets/edit_server.py --scratch content --import-mod their_mod.json
curl -s -X POST http://127.0.0.1:8137/api/pack                     # or press pack on any level page, then stop the server
./vikings                                                          # "console content pack content" in the start-up log
```

From a release bundle the same commands run inside `content-tools/`, with
`--scratch ../content`; a tree created there has no `texts_exe.json`, so the
pack step bakes no EXE image — copy the canonical one beside the store
(`cp content-tools/exe_static.bin content/`) before starting the game, which
needs both `content/.compiled/` and `content/exe_static.bin` to accept the
directory. Verified: a one-file mod imported this way changes the packed
record and the game starts on it from both a checkout and a bundle.

What this means today, plainly:

- The game loads one pack: `content/` beside the executable or in the
  working directory, or the tree the `V2_ASSETS_DIR` / `V2_EXE_STATIC`
  variables point at. To switch between mods keep several directories and
  rename or point the variables at one. `V2_CONTENT=0` plays the plain
  archive.
- A pack made this way holds the archive and the mod alone; the console
  material is not in it. `build_content.py --mod` (the builder's own way to
  apply a package instead of converting the console images) accepts only a
  package that carries the console content's `extras.json` and its EXE image
  with the extra-level trailer — a package exported from a tree that had the
  console content imported. It stops with a missing `extras.json` on a plain
  mod.
- A mod authored on a tree that holds the console content carries that
  material inside (every console-derived file counts as a difference from the
  PC tree) — such a package must not be shared, since the material is derived
  from the console ROM images. Author mods for sharing on a scratch created
  from the plain `assets/`.
- Two mods cannot be combined by the tools; import both into one scratch tree
  (later imports overwrite earlier files of the same name) and export the
  union.
- New level slots with their own passwords are not created by the editor
  (the console levels use that mechanism through the content build); a new
  level goes into an existing slot of its world, via clone or by editing the
  slot directly.

## Limits

| What | Limit | Where it is checked |
| --- | --- | --- |
| Any chunk in the store | 0xB080 bytes packed (the store uses an all-literal LZSS stream: raw size + 12.5 %) | pack |
| A world script | 49152 bytes | check / compile |
| A dialogue text | its original box, in place | pack (texts) |
| Spawn table | free length, terminator-scanned | – |
| Template chunk, tilesets | grow as needed | – |
| Extra level slots | 48 to 63, defined by the content build | content build |
| Clone | same world only (the script comes from the level table) | clone |

## Under the hood

The open tree has one directory per chunk role; the file name is the chunk id
in hex. `tilemaps/0029.json` is a tilemap (a 16-bit word per quad: template
index and attribute bits), `level_headers/0028.json` a header stripe (size,
tilemap / tileset / template ids, spawn table, palette list, palette
animations, bank list, music, the next level), `bg_tilesets/` the template
chunks with a rendered PNG beside the JSON, `tilesets/` the tile graphics as
indexed PNG, `tile_masks/` the collision masks, `sprite_banks/` and
`misc/anim_bank/` the sprites, `level_scripts/` the edited world scripts,
`palettes/`, `screens/`, `music/`, `sound_banks/`, `texts/` the rest.
`extras.json` at the root registers chunk ids that are not in the archive
(the archive has 535, ids 0 to 0x216); a new id needs an entry with its role.

**pack** runs `assetc.pack`: every role converter compiles its files back into
the chunk's bytes, the bytes go into a store-mode LZSS container (a valid
stream for the game's decompressor, no compression), and each record lands in
`.compiled/<id>.bin`. The EXE image is then rebuilt on the scratch copy: the
texts compiled into the string zone, anything past the static image — the
`LVX5` trailer with the extra level records (slot, header, template, password,
flags) — kept as is.

The game takes the store from `V2_ASSETS_DIR` and the image from
`V2_EXE_STATIC`, which is what **play** sets; without them, a `content/`
directory beside the executable or in the working directory. An edited world
script is detected by its checksum and runs in the bytecode interpreter
instead of the pre-generated executor of the original script, so logic edits
need no rebuild of the game.
