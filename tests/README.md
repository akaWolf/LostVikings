# HEADLESS Test Infrastructure

Automated correctness testing for Lost Vikings v2 port via headless build that
runs orig (ground truth) + v2 (mirror) in parallel, ловит divergences,
дампит buffers как PPM images.

См. также: `../HEADLESS_MODE_ANALYSIS.md` — архитектурный анализ + design rationale.

---

## Quick Start

```bash
# 1. Build headless binary (test mode without SDL display / MIDI)
HEADLESS=1 RELEASE=1 make -j$(nproc)
# → produces vikings_headless (~8.6MB)

# 2. Run smoke test (1 sec, basic sanity)
./tests/smoke.sh
# → PASS: smoke test ...  OR  FAIL: divergence detected (dump path printed)

# 3. Record a curated scenario (test mode, needs DATA.DAT) — see SCENARIOS.md
make -j$(nproc)
./tests/record.sh level0_walk     # play, then quit (Alt+X). Writes replays/level0_walk.inp

# 4. Run all curated scenarios under headless verify
./tests/scenarios.sh
# → PASS/FAIL per replay file

# 4b. Golden end-state channel (direction V, phase-D oracle)
# Every scenarios.sh run also dumps the FINAL shadow-DS named-field snapshot
# (V2_GOLDEN_DUMP → v2_gs_dump_text at the max-frames exit) and compares it
# against tests/golden_states/<name>.txt when that file exists — a mismatch
# is a FAIL with an inline diff head. The catalog is the state oracle that
# survives the verify-scaffolding teardown: "replay N to its frame budget →
# end state == golden".
GOLDEN=update ./tests/scenarios.sh   # (re)take the catalog from a green run
./tests/scenarios.sh                 # normal runs now also check golden

# 4c. Teleport save/load (direction V step 2, phase-D tooling)
# Any headless run can snapshot the FULL v2 world at its clean exit:
V2_SAVE_STATE=/tmp/state.bin ./vikings_headless --replay-input=... --max-frames=...
# A V2_ONLY build loads it before the first frame (game thread parked):
V2_LOAD_STATE=/tmp/state.bin ./vikings          # play on from that point
# With BOTH env vars set, V2_ONLY saves back immediately after loading —
# the file pair must be byte-identical (load/save roundtrip channel; the
# DS block passes through the phase-D serializer in both directions, so
# every save AND load re-proves the typed model).
# v1 limits: AIL/audio state is not captured; input latches reset on load.

# 4d. Run random fuzz (100 seeds, ~10 min)
./tests/fuzz_harness.sh 100

# 5. Run coverage-guided fuzz (long-running, finds rare paths)
python3 tests/fuzz_coverage.py 10000 5000
```

---

## How It Works

**Setup**: Test mode runs orig m2c VM + v2 mirror in parallel on shared
shadow DS, with verify infrastructure comparing every state transition.

**Headless build**: skips SDL display + audio (uses SDL dummy drivers).
adlmidi excluded from link (stubbed). Input from replay file (frame-based,
deterministic). Game logic + render + verify run as in test mode.

**Divergence detection**: 6 verify mechanisms catch any orig↔v2 mismatch:
- PSNAP — DS state at 22 phase boundaries (350+ watch addresses)
- A2 render — pixel-level orig drawBuffer vs v2_render_buf
- Audit — SFX/music event ordering
- Audio symmetry — DS slot table sync
- Game-loop verify — full DS + 7 segment compares
- Trace compare — per-opcode (PC, accumulator, hashes)

**On first divergence**: dump both render buffers as PPM, DS bin dumps,
diff text, context.txt → exit code 1.

---

## Scripts

| Script | Purpose | Runtime | When |
|--------|---------|---------|------|
| `smoke.sh` | Quick sanity (empty input, 200 frames) | ~1 sec | Every commit |
| `scenarios.sh` | Iterate all `replays/*.inp`, pass/fail each | ~10 sec × N replays | Every PR |
| `fuzz_harness.sh [N=100]` | N random-seed sessions | ~30 sec × N | Nightly |
| `fuzz_coverage.py [iter=10000]` | Coverage-guided mutation, B5-driven | Hours | Weekly |

---

## Replay File Format

Frame-based event log. SDL-independent (action names not keycodes).

```
# Comments start with #
# Format: <frame> <KD|KU|KR> <ACTION> [seq]
13 KD SPACE      # press SPACE at frame 13 (v2_dbg_pre_vm_iter == 13)
15 KU SPACE      # release at frame 15
21 KD LEFT
45 KU LEFT
100 KD F10       # menu trigger
102 KU F10
```

The optional 4th column (`seq`) is the sub_12352 call number — recordings
made by the current recorder carry it on every event and replay at exact
intra-frame positions. Legacy 3-column files replay on the frame clock.
`KR` (#86) is a typematic repeat of a held key: recorded only for keys the
INT9 [28C] letter channel maps, and replayed as exactly that — the [28C]
note with no edge/latch side effects (mirrors the live event loops, where
everything except the [28C] note is `!repeat`-gated).

**Actions** (game-action names; mapped to SDL keycodes by recorder):
- Movement: `LEFT`, `RIGHT`, `UP`, `DOWN`
- Action: `SPACE`, `RETURN`, `TAB`, `LCTRL`, `RCTRL`, `LALT`, `RALT`
- Letters: `E`, `S`, `D`, `F`, `Q`, `R`, `Y`, `A`, `N`
- Function: `F4`, `F5`, `F6`, `F10`, `ESC`, `M`, `X`, `DEL`
- Numbers: `1`, `2`, `3`
- Debug: `F12` (PGM dump)
- Plus raw single letters/digits (`a`-`z`, `0`-`9`) for password typing —
  the recorder logs any such key even without a keymap entry.

Frame `0` = before any sub_12352 input call has run.

---

## Recording New Scenarios

Record in **test mode** (orig + v2 mirror), so the replay reproduces
bit-for-bit under headless verify. Requires `DATA.DAT` in the repo root.
See `replays/SCENARIOS.md` for the scenario table.

```bash
# 1. Build test mode (orig + v2 mirror)
make -j$(nproc)

# 2. Record — play the scenario, then quit (Alt+X or close window)
./tests/record.sh my_scenario
#   → writes replays/my_scenario.inp  + replays/my_scenario.frames

# 3. Verify it replays clean under headless
HEADLESS=1 RELEASE=1 make -j$(nproc)
./tests/scenarios.sh
# Exit 0 = OK, Exit 1 = divergence found (good — bug to investigate!)

# 4. Commit (auto-picked up by scenarios.sh)
git add tests/replays/my_scenario.inp tests/replays/my_scenario.frames
```

`record.sh` runs test-mode `vikings --record-input=...`. The recorder
is also available directly (`./vikings --record-input=<f>`), plus with
`--debug` if the scenario needs the F4/F5/F6 cheats. V2_ONLY mode
(`v2_main.cpp`) records too, but only covers v2-implemented paths — use
test mode for full gameplay capture.

**Recommended scenario library** (full list + what each exercises in
`replays/SCENARIOS.md`):
- `empty.inp` — no input baseline ✓ (exists)
- `level0_walk` — walk/scroll the opening level
- `viking_switch` — cycle vikings 1/2/3
- `pause` — F10 pause loop entry/exit
- `inventory` — Tab inventory nav + pickup
- `dialog` — open + scroll + dismiss dialog
- `jump_fall` — gravity / collision
- `item_use` — per-viking action
- `level_transition` — complete level → next level load

---

## Dump Directory Format

On divergence, dump created at `--dump-dir=<path>/diverge_f<N>_<source>/`:

```
diverge_f1_A2-render/
├── context.txt           # Human-readable summary (source, frame, level, addr)
├── ds_orig.bin           # 64KB orig DS dump
├── ds_v2.bin             # 64KB v2 shadow DS dump
├── ds_diff.txt           # ASCII: first 100 byte differences (addr, orig, v2)
├── orig_buffer.ppm       # 320×176 PPM of orig drawBuffer (current page)
├── v2_buffer.ppm         # 320×176 PPM of v2_render_buf
└── diff.ppm              # RED pixels where buffers differ, original elsewhere
```

Plus parent dump dir has `run.log` (full stderr/stdout).

**Sources** (verify mechanism that triggered):
- `PSNAP` — DS state divergence at phase boundary
- `A2-render` — pixel difference in render output
- `gameloop` — full DS compare after game loop
- `audit` — SFX event sequence mismatch
- `audio-sym` — DS slot table asymmetry
- `trace` — per-opcode trace mismatch
- `trace-len` — opcode count mismatch
- `SEGFAULT` — process crash (best-effort dump)

**Inspect PPM** (any image viewer):
```bash
feh diverge_f1_A2-render/diff.ppm        # RED = diff pixels
xdg-open diverge_f1_A2-render/orig_buffer.ppm
# Compare side by side: orig_buffer.ppm vs v2_buffer.ppm
```

**Inspect DS binary diff**:
```bash
cmp -l diverge_f1_A2-render/ds_orig.bin diverge_f1_A2-render/ds_v2.bin | head -20
# Or use xxd for hex view
xxd diverge_f1_A2-render/ds_orig.bin > /tmp/orig.hex
xxd diverge_f1_A2-render/ds_v2.bin   > /tmp/v2.hex
diff /tmp/orig.hex /tmp/v2.hex | less
```

---

## Exit Codes

| Code | Meaning | Action |
|------|---------|--------|
| **0** | Clean (replay ran, no divergence) | Test passed |
| **1** | Divergence detected | Investigate dump dir |
| **2** | Setup error (missing replay, etc.) | Fix invocation |
| **3** | Max frames timeout reached | Adjust `--max-frames` or expected |
| **4** | Process crashed (SIGSEGV/SIGBUS/SIGABRT) | Investigate dump + log |

CI template:
```bash
./tests/smoke.sh
case $? in
    0) echo "PASS";;
    1) echo "FAIL: divergence"; exit 1;;
    2|3|4) echo "ERROR: setup/timeout/crash"; exit 2;;
esac
```

---

## CLI Reference

```
./vikings_headless --replay-input=<file> [options]

Required:
  --replay-input=<file>       Recorded input file (frame-based events)

Options:
  --dump-dir=<path>           Output dir for divergence dumps
                              (default: /tmp/headless_<pid>/)
  --max-frames=<N>            Exit cleanly after N frames (default: from env)
  --seed=<N>                  PRNG seed for determinism (default: 0)
  --debug                     Enable F4/F5/F6 debug cheats
```

---

## Coverage-Guided Fuzz Deep Dive (`fuzz_coverage.py`)

**Concept**: Use B5 opcode coverage report to direct fuzz toward rare code paths.

**Algorithm**:
1. Bootstrap: run existing corpus inputs (`tests/replays/*.inp` + `/tmp/headless_corpus/*.inp`)
   to establish baseline coverage (which opcodes execute).
2. Each iteration:
   - Pick random corpus input, mutate (insert/delete/swap/duplicate events)
   - Run through headless, parse B5 coverage report from `run.log`
   - If mutated input executes new opcodes → save to corpus
   - If divergence/crash → save to `/tmp/headless_bugs/`
3. Periodically print stats: coverage growth, corpus size, bugs found

**Target**: 209/216 main VM opcodes never executed in typical gameplay → massive
untested surface. Coverage-guided fuzz progressively reaches into these branches.

**Output**:
- `/tmp/headless_corpus/cov_<timestamp>_<seed>.inp` — inputs that found new coverage
- `/tmp/headless_bugs/cov_bug_<timestamp>_<seed>/` — failing inputs + dump

**Mutation operators**:
- `insert` — add random event at random frame
- `delete` — remove random event
- `swap_frame` — shift event timing ±50 frames
- `change_action` — swap action (LEFT → RIGHT etc.)
- `duplicate` — clone event nearby

**Stop conditions**:
- N iterations completed
- Ctrl-C (corpus + bugs preserved)

---

## Bug Triage Workflow

When fuzz finds divergence:

```bash
# 1. Reproduce locally with exact seed + input
./vikings_headless \
    --replay-input=/tmp/headless_bugs/cov_bug_*/replay.inp \
    --seed=<same-seed-from-dir-name> \
    --dump-dir=/tmp/triage
# Same divergence at same frame → 100% reproducible

# 2. Examine dump
cat /tmp/triage/diverge_*/context.txt
# source: ... frame: ... detail: ...

# 3. Visual diff
feh /tmp/triage/diverge_*/diff.ppm

# 4. DS state diff
head -20 /tmp/triage/diverge_*/ds_diff.txt

# 5. Minimize input (binary search):
# Copy replay, delete second half → re-run, if still fails → smaller
# Iterate until minimal reproducer

# 6. Classify:
#   - PSNAP divergence → look at addr → known bug class? (#114 page-flip, #156 selector, etc.)
#   - A2-render → render bug → check ladder/dialog/HUD area
#   - audit → SFX timing
#   - trace → per-opcode → exact opcode at index N

# 7. Fix root cause OR document expected divergence with --skip pattern
```

---

## Performance Notes

**Headless speed** (no SDL_Delay, no display sync):
- Single empty replay run: ~0.5 sec for 200 frames
- Full level walkthrough (~5000 frames): ~10 sec
- Fuzz harness 100 seeds: ~5-10 min
- Coverage fuzz 10000 iter: ~2-3 hours

**Determinism**:
- Same `--seed` + same input → IDENTICAL output every run
- Verified: rerun seed=42 produces bit-exact same divergence at same frame
- Required for CI: failed test must repro on developer machine

---

## File Layout

```
tests/
├── README.md              # This file
├── smoke.sh               # Tier 0
├── scenarios.sh           # Tier 1
├── fuzz_harness.sh        # Tier 2 (random)
├── fuzz_gen.py            # Random input generator (used by fuzz_harness)
├── fuzz_coverage.py       # Tier 3 (coverage-guided)
└── replays/
    ├── empty.inp          # baseline: no input
    └── <your_scenarios>.inp
```

Implementation source files:
```
src/sdl/headless/
├── headless_main.cpp      # CLI parse + init
├── headless_dump.h        # PPM writer + divergence hook API
├── headless_dump.cpp      # Implementation
└── headless_audio_stub.cpp # AudioPool stubs (no adlmidi)
```

---

## CI Integration

### GitHub Actions example

```yaml
# .github/workflows/headless-tests.yml
name: HEADLESS tests
on: [push, pull_request]
jobs:
  smoke:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - run: sudo apt-get install -y libsdl2-dev
      - run: HEADLESS=1 RELEASE=1 make -j$(nproc)
      - run: ./tests/smoke.sh

  scenarios:
    if: github.event_name == 'pull_request'
    runs-on: ubuntu-latest
    needs: smoke
    steps:
      - uses: actions/checkout@v4
      - run: sudo apt-get install -y libsdl2-dev
      - run: HEADLESS=1 RELEASE=1 make -j$(nproc)
      - run: ./tests/scenarios.sh
      - uses: actions/upload-artifact@v4
        if: failure()
        with:
          name: divergence-dumps
          path: /tmp/headless_scenario_*/
```

### Pre-commit hook

```bash
# .git/hooks/pre-commit
#!/bin/bash
HEADLESS=1 RELEASE=1 make -j$(nproc) > /dev/null 2>&1 || { echo "Build broken"; exit 1; }
./tests/smoke.sh || { echo "Smoke test failed — see /tmp/headless_smoke_*/run.log"; exit 1; }
```

### `git bisect` integration

```bash
# Find which commit broke smoke test
git bisect start
git bisect bad HEAD
git bisect good <known-good-sha>
git bisect run ./tests/smoke.sh
# → identifies regressing commit automatically
```

---

## Math Contract

```
HEADLESS_PASS(input, seed) ⟺ ∀ frame f ∈ [0, max_frames]:
    π_DS(T_orig^f(S₀(seed), input))    ≡ π_DS(T_v2^f(S₀(seed), input))
  ∧ π_render(T_orig^f(S₀(seed), input)) ≡ π_render(T_v2^f(S₀(seed), input))
  ∧ π_audio(T_orig^f(S₀(seed), input))  ≡ π_audio(T_v2^f(S₀(seed), input))

Exit code 0 ⟺ HEADLESS_PASS
```

I.e., headless reduces "is v2 correct for input I" to a decidable property
with shell exit code. See `HEADLESS_MODE_ANALYSIS.md` §3.2 for full formalism.

---

## Troubleshooting

**`vikings_headless: command not found`** → не собран. `HEADLESS=1 RELEASE=1 make -j$(nproc)`.

**`--replay-input is REQUIRED`** → headless всегда нужен replay file. Use `empty.inp` для smoke.

**Smoke test always fails at f=1** → baseline render divergence (known issue,
orig/v2 first-frame mismatch). Investigation needed before fuzz finds new bugs.

**No B5 report in run.log** → divergence fired before any opcodes executed.
Common with f=1 render divergence. Run smaller test scenarios to gather coverage.

**Fuzz finds only same bug repeatedly** → all mutations hit same baseline issue.
Fix baseline first, then fuzz finds new unique bugs.

**Dump dir empty after FAIL** → headless crashed before reaching divergence hook
(SIGSEGV during init). Check `run.log` for backtrace.

---

## Related Docs

- **`../HEADLESS_MODE_ANALYSIS.md`** — full architectural analysis + math
- **`../V2_ONLY_REIMPLEMENTATION_AUDIT.md`** — V2_ONLY divergence sources
- **`../CLAUDE.md`** — project rules (100% bit-exact orig)
