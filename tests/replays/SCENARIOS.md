# Curated gameplay scenarios

Each scenario is a recorded `.inp` replay that exercises a specific code path.
Recording must be done by a human (you can't script the play itself); replay +
verification is automated via `tests/scenarios.sh` under `vikings_headless`.

## Recording workflow

```sh
make -j$(nproc)                       # default build (orig + v2 mirror)
# DATA.DAT must be in the repo root.
./tests/record.sh <name> [frames]     # play the scenario, then quit (Alt+X)
```

`record.sh` writes:
- `tests/replays/<name>.inp`     — the frame-based input log
- `tests/replays/<name>.frames`  — frame budget (last event + 120) for scenarios.sh

Then verify it replays clean:

```sh
HEADLESS=1 RELEASE=1 make -j$(nproc)
./tests/scenarios.sh                  # runs every replays/*.inp, reports PASS/FAIL
```

A scenario PASSES when the headless replay produces no orig↔v2 divergence
(PSNAP / audit / gameloop / trace / audio-sym). Render-pixel-only diffs are
logged but non-critical (see HEADLESS_MODE_ANALYSIS.md).

## Scenarios to record

Aim for short, single-purpose runs (~10-60 s each). Keep them deterministic:
avoid timing-dependent randomness where possible.

| Name             | What to do | Exercises |
| --- | --- | --- |
| `level0_walk`    | Walk Erik left/right across the first screen of the opening level | tile scroll, sprite render, camera |
| `viking_switch`  | Press 1, 2, 3 to cycle the active Viking a few times | sub_10138 switch path, HUD portrait/selector |
| `pause`          | Open pause with F10, move the cursor, close with F10/Esc | sub_11ba5 pause loop, HUD selector blink |
| `inventory`      | Tab to open inventory, navigate items, pick one up, exit | sub_11cbb item nav, ds:0x441-44B, sub_120d1 |
| `dialog`         | Trigger an in-level dialog box, scroll through it, dismiss | sub_104a1 text loop, glyph render, scroll |
| `jump_fall`      | Jump and fall with Erik a few times | gravity, collision (sub_15972 / sub_13d68) |
| `item_use`       | Use a Viking-specific item / action (D, action button) | per-character action handlers |
| `level_transition` | Reach a level exit so the game transitions to the next level | sub_115d2 transition, 3 sub-frames, page flips |

Add more as new code paths land. One scenario per behavior keeps a FAIL
pointing at a single suspect area.

## Notes

- Recording happens in **default mode**, which runs orig + v2 in lockstep at
  ~9-18 fps. The `.inp` timestamps are frame numbers (`v2_dbg_pre_vm_iter`),
  not wall-clock, so the replay reproduces identically under headless.
- If a scenario needs the debug cheats (F4/F5/F6) record with
  `./vikings --debug --record-input=...` directly (record.sh keeps it simple
  without --debug).
- `.inp` files are tiny text (action names + frame numbers) — commit them.

## synth_inventory (6216 frames, user-recorded 2026-08-05)
M5 закрытие (VERIFICATION_GAPS_ANALYSIS): mode-0-ветки sub_11cbb с РЕАЛЬНЫМ
предметом — недостижимо синтетикой (стартовый инвентарь пуст). Запись:
level 1, подбор предмета, TAB-инвентарь: стрелки по слотам (вкл. «в упор»),
Action на предмете и пустом слоте, Exit, повторный вход. 55 транзишн-кадров.
Первый прогон: PSNAP 0 диффов по всем фазам, CALLPAR 0, SFX 0 unmatched.
