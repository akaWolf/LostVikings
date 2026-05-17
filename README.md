# Lost Vikings

A native Linux / Windows port of the 1992 DOS game **The Lost Vikings**
(Silicon & Synapse / Interplay). Built by feeding the original 16-bit MS-DOS
executable through an m2c decompiler and wrapping the resulting C++ in an
SDL2 shell. A parallel **v2 mirror** is being grown alongside the
m2c-decompiled code with the goal of fully replacing the original VM
bytecode-for-bytecode, frame-for-frame.

[![Build](https://github.com/akaWolf/LostVikings/actions/workflows/build.yml/badge.svg)](https://github.com/akaWolf/LostVikings/actions/workflows/build.yml)

![Intro](imgs/intro.png)
![Level 1](imgs/level_1.png)

> ⚠️ Requires the copyrighted `DATA.DAT` from a legal copy of the original
> game. All build modes need it — `DATA.DAT` holds the game content
> (levels, sprites, audio); the bundled `*_static.bin` files only replace
> the static EXE image, not the game assets.

---

## Pre-built downloads

Every push to any branch produces a GitHub Release with four artifacts:

| File | Platform | Mode |
| --- | --- | --- |
| `vikings-linux-x86_64.tar.gz`        | Linux x86_64   | default (orig + v2 mirror) |
| `vikings-windows-x86_64.zip`         | Windows x86_64 | default |
| `vikings-linux-x86_64-v2only.tar.gz` | Linux x86_64   | V2_ONLY (standalone v2) |
| `vikings-windows-x86_64-v2only.zip`  | Windows x86_64 | V2_ONLY |

All four bundles need `DATA.DAT` supplied separately (see below).

Each archive contains:

- `vikings` / `vikings.exe` — the game
- `vikings_keymap_editor` / `.exe` — GUI key-rebind utility
- `vikings_keymap.cfg` — default key bindings (editable)
- `ds_static.bin`, `exe_static.bin` — DS/EXE snapshots used by the v2 mirror
- `README.md`

SDL2 and the C++ runtime are statically linked, so no extra `.so` / `.dll`
needs to live next to the executable.

Get the latest from the [Releases page](https://github.com/akaWolf/LostVikings/releases).

---

## DATA.DAT

`DATA.DAT` is the original 1992 asset file (levels, sprites, audio). It
is copyrighted and **not redistributed** with this project. To run any
build you need a legal copy:

- **GOG** sells the game (cheapest legal route). After install,
  `DATA.DAT` lives in the install dir.
- The Blizzard Arcade Collection ships a different repack and is not a
  drop-in replacement.

Place `DATA.DAT` next to the `vikings` binary and launch.

The bundled `ds_static.bin` / `exe_static.bin` snapshots are not a
DATA.DAT substitute — they hold the static EXE image (initial DS, the
seg001 text/menu data, lookup tables). Default mode reconstructs this
from `m2c::m[]` populated by C++ static initialisers; V2_ONLY skips
m2c entirely and loads it from the snapshot files at startup. Game
content (levels, sprites, sound) still comes from `DATA.DAT` in both
modes.

---

## Build from source

### Linux

```sh
git submodule update --init
sudo apt-get install build-essential libsdl2-dev pkg-config   # Debian/Ubuntu
make -j$(nproc)
./vikings
```

### Windows (mingw-w64 cross-compile)

Toolchain + SDL2 for mingw:

- **Arch x86_64**: `pacman -S mingw-w64-gcc mingw-w64-sdl2 mingw-w64-pkg-config`
- **Arch aarch64**: same packages from AUR (`yay -S …`); building gcc-mingw
  on ARM takes hours.
- **Debian/Ubuntu**: `apt install g++-mingw-w64-x86-64 mingw-w64-tools` plus
  manually unpacking the SDL2 mingw devel tarball into the mingw sysroot
  (`/usr/x86_64-w64-mingw32/`).

```sh
make clean
make WIN=1 -j$(nproc)
```

Output: `vikings.exe`. Linux-only debug instrumentation (execinfo
backtraces, `perf_event_open` watchpoints, `dladdr`) is compiled out under
`#ifdef __linux__` on Windows — gameplay is unaffected.

Override the triplet (e.g. for i686) with `make WIN=1 MINGW_TRIPLET=i686-w64-mingw32`.

### Build flags reference

All flags compose. `make clean` between mode changes.

| Flag | Effect |
| --- | --- |
| `RELEASE=1`    | `-O2 -fno-omit-frame-pointer` (default `-O0`) |
| `STATIC=1`     | static-link `libgcc` + `libstdc++` |
| `SDL_STATIC=1` | static-link `libSDL2.a` via `pkg-config --static` |
| `WIN=1`        | mingw-w64 cross-compile → `vikings.exe` |
| `V2_ONLY=1`    | standalone v2 build; no m2c sources, no `DATA.DAT` |
| `HEADLESS=1`   | no-display testing build → `vikings_headless` |

Example: full static, optimised, Windows release:

```sh
make WIN=1 RELEASE=1 STATIC=1 SDL_STATIC=1 -j$(nproc)
```

### Keymap editor

Standalone SDL2 utility for editing `vikings_keymap.cfg`:

```sh
make keymap_editor          # → ./vikings_keymap_editor
./vikings_keymap_editor     # opens an editor window
```

Click a row → press a key to rebind it → **Save**. ESC cancels a bind.
Ctrl+S = save, Ctrl+Q = quit.

---

## Modes

### Default mode

Runs the m2c-decompiled original VM and the **v2 mirror** in lockstep.
Two SDL windows open — one rendered by the orig path, one by the v2 path. The
verify infrastructure cross-checks every frame and flags any divergence
in DS state, VM state, audio, or render output. This is the primary
development mode.

### V2_ONLY mode

Builds only the v2 reimplementation. m2c-decompiled sources are excluded
from the build entirely. Faster compile, roughly half the binary size.
Still needs `DATA.DAT` for game content; the bundled `*_static.bin`
files supply the static EXE image that default mode gets from m2c's C++
initialisers. Useful for fast iteration on the v2 code path and as the
eventual delivery vehicle once feature parity is complete.

### HEADLESS mode

No display, no audio device — uses SDL's dummy drivers. Drives the game
deterministically from a replay file and exits with code `1` on the
first verify divergence. Built for CI and reproducible bug hunting:

```sh
HEADLESS=1 make -j$(nproc)
./vikings_headless --replay-input=tests/replays/empty.inp --max-frames=200
```

Divergence dumps land in `--dump-dir=<path>` (defaults to
`/tmp/headless_<pid>/`) as PPM frame pairs, DS binary diffs, and a
context.txt explaining what diverged.

---

## CLI options

The three entry points have different surfaces.

**Default `vikings`** (built without `V2_ONLY=1`/`HEADLESS=1`):

| Flag | Description |
| --- | --- |
| `--debug`         | enable orig debug-build cheats (F4 INT3, F5/F6 level skip) |
| `--keymap=<path>` | load a custom keymap (default `./vikings_keymap.cfg`) |

**V2_ONLY `vikings`** (`make V2_ONLY=1`):

| Flag | Description |
| --- | --- |
| `--debug`                | same as default mode |
| `--keymap=<path>`        | same as default mode |
| `--record-input=<file>`  | record SDL input to a frame-based `.inp` log |
| `--replay-input=<file>`  | replay an `.inp` log instead of live keyboard |
| `--replay-strict`        | ignore live keyboard even after the replay queue is exhausted |

**`vikings_headless`** (`make HEADLESS=1`):

| Flag | Description |
| --- | --- |
| `--replay-input=<file>` | REQUIRED. Frame-based `.inp` replay driving input |
| `--keymap=<path>`       | optional keymap override |
| `--dump-dir=<path>`     | divergence dump output dir (default `/tmp/headless_<pid>/`) |
| `--max-frames=<N>`      | exit cleanly after N game frames (default 10000) |
| `--seed=<N>`            | PRNG seed (default 0) |

Replay files are frame-based and SDL-independent (record actions by
name) so they reproduce identically across machines and runs.

---

## Default key bindings

Only the entries whose meaning is documented in the source or the
`vikings_keymap.cfg` comments are listed; the full mapping (32 entries)
is in `vikings_keymap.cfg`.

| Key                      | Function |
| --- | --- |
| Arrow keys               | direction input |
| Space / Enter / F        | primary action button |
| D                        | secondary action button |
| Tab                      | inventory |
| Esc                      | menu / cancel |
| Ctrl (Left/Right)        | game action bit + spec key |
| Alt (Left/Right)         | spec key (used in Alt-combos) |
| F10                      | pause menu |
| 1 / 2 / 3                | switch active Viking |
| Y / N                    | yes / no in dialogs |
| S                        | mute SFX (also game input bit) |
| M                        | mute music |
| F4                       | debug INT 3 (needs `--debug`) |
| F5 / F6                  | prev / next level cheat (needs `--debug`) |
| Q (with Alt)             | reset |

Edit via `vikings_keymap_editor` or hand-edit `vikings_keymap.cfg`. The
config format is documented inline at the top of that file.

---

## Testing

```sh
HEADLESS=1 make -j$(nproc)

./tests/smoke.sh                              # 200-frame baseline (empty.inp)
./tests/scenarios.sh                          # all replays in tests/replays/
./tests/fuzz_harness.sh 600 5                 # 5×600-frame random fuzz
./tests/fuzz_coverage.py --max-frames 1200    # coverage-guided fuzz
```

Without `DATA.DAT` in cwd the smoke run still verifies that the binary
boots, SDL dummy drivers work, the replay parser handles `empty.inp`, no
segfault during init, and the dump dir is created. With `DATA.DAT`
present the same scripts exercise real gameplay.

More detail: `tests/README.md` and `HEADLESS_MODE_ANALYSIS.md`.

---

## CI

`.github/workflows/build.yml` runs on every push (any branch) and every
PR, producing the four artifacts described above. Every push to any
branch also publishes a GitHub Release:

- Branch push → **pre-release** named `Build N (branch @ short_sha)`.
- Tag push `v*` → **stable release** named after the tag (becomes "Latest").
- PR events do not create releases.

### Optional: full-gameplay CI smoke

The CI Linux job runs `tests/smoke.sh`. Without access to `DATA.DAT` it
runs the init-only path (still useful for catching link / SDL / replay
parser regressions). To upgrade CI to a full gameplay smoke, host
`DATA.DAT` on a server you control and add the URL as a repo secret:

```sh
gh secret set DATA_DAT_URL --body 'https://user:pass@yourserver/DATA.DAT'
```

CI then `curl`s `DATA.DAT` from that URL once and caches it. The secret
is masked in logs by GitHub Actions automatically.

---

## Project layout

```
src/
  aux/asm.cpp             entry point for default mode
  vikings.exe*.cpp        m2c-decompiled DOS executable (do not hand-edit)
  _data.cpp               m2c-decompiled DS image
  sdl/v2_main.cpp         entry point for V2_ONLY mode
  sdl/v2_vm.cpp           v2 mirror VM — the reimplementation
  sdl/v2_render_funcs.cpp v2 render path (tiles, sprites, HUD, glyphs)
  sdl/v2_keymap.{h,cpp}   runtime keymap loader / saver
  sdl/v2_input_recorder*  frame-based input record/replay
  sdl/headless/           HEADLESS mode entry + dump infra
  sdl/keymap_editor/      standalone keymap editor binary
  sdl/render.cpp          shared SDL window / event loop for orig mode
  sdl/play.cpp            audio (XMI / SFX via adlmidi)
  rendering/seg003_*.{c,h}  hand-written seg003 helpers
  adlmidi/                git submodule, OPL3 synthesis
tests/                    HEADLESS test scripts + replays
.github/workflows/build.yml  CI: linux + windows × {default, V2_ONLY}
```

`docs2/` holds reverse-engineering notes generated during the
m2c-decompile audit; safe to ignore for everyday work.

---

## License

The port code in `src/sdl/`, `src/aux/`, `src/rendering/` and the build
infrastructure is the work of this project. The m2c output under
`src/vikings.exe*.cpp` and `src/_data.cpp` is mechanically derived from
the original 1992 Lost Vikings executable, which remains the property of
Blizzard Entertainment (formerly Silicon & Synapse). The `DATA.DAT`
asset file is also Blizzard's property and is not redistributed.

The `src/adlmidi/` submodule keeps its upstream license
(see [libADLMIDI](https://github.com/Wohlstand/libADLMIDI)).
