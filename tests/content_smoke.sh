#!/usr/bin/env bash
# tests/content_smoke.sh — the dynamic half of the content checks (the static judge is
# tools/assets/content_check.py, run by build_content.py after packing and by
# `make content-check`).
#
# Every level the pack defines is started in the game binary under SDL's dummy drivers
# (V2_START_LEVEL) with V2_SPAWN_SWEEP=1: the engine spawns every entry of the level's
# spawn table once, one entry per frame, through the game's own spawn path (v2_vm.cpp
# v2_content_sweep_tick) and reports on stderr
#   V2-SPAWN-FAIL        a class's sprite bank the level does not load (sub_12f82 miss
#                        under sub_13e52: the object never spawns)
#   V2-ANIM-RES-MISS     an anim chunk the level does not load (anim command 23: the
#                        object takes entry 0's sprites)
#   V2-SWEEP: ... done   the end of the sweep with its counts; an entry that spawned
#                        nothing (table full, template init failed) has a line of its own.
# A slot passes when the run ends by itself (exit 0 at --max-frames), the sweep reached
# the end of the table with failed=0, and every miss line of that level is one the judge
# lists as dormant for the slot (content_check.py --dormant: the lookups the original's
# own levels leave unresolved, inherited by the classes the pack copies unchanged). The
# replay presses SPACE every 40 frames to take the level dialogues away.
#
# Needs: the game build (BIN, default ./vikings — make), content/ (make content), python3.
#   bash tests/content_smoke.sh            # the pack's slots: the console levels, the scenes,
#                                          # the finale, the SNES-balance variants
#   SLOTS=all bash tests/content_smoke.sh  # plus the 37 PC levels and the ending — the
#                                          # calibration of the dynamic check on the original
#   SLOTS="50 51" bash tests/content_smoke.sh   # these slots (a slot with a PC and a balance
#                                               # variant runs both)
#   FRAMES=2400 (per slot) JOBS=$(nproc) CONTENT=dir BIN=vikings
set -u
export V2_CONTENT=0   # the pack is given explicitly (V2_ASSETS_DIR / V2_EXE_STATIC)
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${BIN:-vikings}
CONTENT=${CONTENT:-$ROOT/content}
FRAMES=${FRAMES:-2400}
JOBS=${JOBS:-$(nproc)}
SLOTS=${SLOTS:-}
if [ ! -x "$ROOT/$BIN" ]; then echo "FAIL: $BIN not built (make)"; exit 1; fi
if [ ! -d "$CONTENT/.compiled" ] || [ ! -f "$CONTENT/exe_static.bin" ]; then
    echo "SKIP: no content pack at $CONTENT (make content)"; exit 0
fi
D=$(mktemp -d /tmp/lv_csmoke_XXXXXX)
if [ "$SLOTS" = all ]; then
    python3 "$ROOT/tools/assets/content_check.py" --content "$CONTENT" --list-all > "$D/slots.txt" || exit 2
else
    python3 "$ROOT/tools/assets/content_check.py" --content "$CONTENT" --list > "$D/slots.txt" || exit 2
    if [ -n "$SLOTS" ]; then
        python3 "$ROOT/tools/assets/content_check.py" --content "$CONTENT" --list-all > "$D/all.txt" || exit 2
        : > "$D/slots.txt"
        for s in $SLOTS; do grep "^slot=$s " "$D/all.txt" >> "$D/slots.txt"; done
    fi
fi
python3 "$ROOT/tools/assets/content_check.py" --content "$CONTENT" --dormant > "$D/dormant.txt" || exit 2
python3 - "$D/sweep.inp" "$FRAMES" <<'EOF'
import sys
out, frames = sys.argv[1], int(sys.argv[2])
with open(out, "w") as f:
    for fr in range(100, frames, 40):   # a key press every 40 frames: the dialogue boxes step on
        f.write(f"{fr} KD SPACE\n{fr + 2} KU SPACE\n")
EOF
run_one() {   # "slot=N name=X script=S alt=A"
    local slot name alt f
    for f in $1; do case "$f" in slot=*) slot=${f#slot=};; name=*) name=${f#name=};; alt=*) alt=${f#alt=};; esac; done
    local R="$D/s${slot}_${name}"   # slot 47 runs twice (the ending and the finale), slot 4 twice (PC and balance)
    mkdir -p "$R"
    ln -s "$ROOT/$BIN" "$R/vikings"; ln -s "$ROOT/ds_static.bin" "$R/ds_static.bin"; ln -s "$ROOT/vikings_keymap.cfg" "$R/vikings_keymap.cfg"
    local fin=0; [ "$name" = FIN ] && fin=1   # slot 47 is the PC ending or, with the option, the console finale
    printf 'parallax=1\nscenes=1\nconsole_finale=%s\nsnes_balance=%s\n' "$fin" "$alt" > "$R/v2_options.cfg"
    ( cd "$R" && env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy V2_FAST_VSYNC=1 V2_NOVSYNC=1 \
        V2_START_LEVEL="$slot" V2_SPAWN_SWEEP=1 V2_ASSETS_DIR="$CONTENT/.compiled" V2_EXE_STATIC="$CONTENT/exe_static.bin" \
        timeout -k 5 900 ./vikings --replay-input="$D/sweep.inp" --max-frames="$FRAMES" > run.log 2>&1 )
    local rc=$? tag="slot=$slot $name" done_line failed bad=0 extra
    if [ $rc -ne 0 ]; then echo "FAIL $tag: exit $rc (see $R/run.log)"; return; fi
    # the first sweep of the level (a death sends the game through the TRY AGAIN shore and
    # back into the level, where the sweep runs again — the same thing, later)
    done_line=$(grep -a "^V2-SWEEP: level=$slot done" "$R/run.log" | head -1)
    if [ -z "$done_line" ]; then echo "FAIL $tag: the sweep did not reach the end of the spawn table in $FRAMES frames (see $R/run.log)"; return; fi
    failed=${done_line##*failed=}; failed=${failed%% *}
    extra=$(grep -a "^V2-SWEEP: level=$slot entry=" "$R/run.log")
    if [ "$failed" != 0 ]; then bad=1; fi
    # every miss of this level must be a dormant lookup of the slot
    local kind cls chunk line n_dorm=0
    while read -r line; do
        [ -z "$line" ] && continue
        case "$line" in V2-SPAWN-FAIL*) kind=sprite;; *) kind=anim;; esac
        cls=${line##*class=}; cls=${cls%% *}; chunk=${line##*chunk=}; chunk=${chunk%% *}
        if grep -q "^slot=$slot kind=$kind class=$cls chunk=$chunk$" "$D/dormant.txt"; then n_dorm=$((n_dorm + 1)); else bad=1; extra="$extra
$line"; fi
    done <<< "$(grep -a "^V2-SPAWN-FAIL level=$slot \|^V2-ANIM-RES-MISS level=$slot " "$R/run.log")"
    if [ $bad -ne 0 ]; then
        echo "FAIL $tag: ${done_line#V2-SWEEP: }"
        printf '%s\n' "$extra" | grep -v '^$' | sed 's/^/      /'
        return
    fi
    echo "PASS $tag: ${done_line#V2-SWEEP: } dormant=$n_dorm"
}
export -f run_one; export D ROOT BIN CONTENT FRAMES
xargs -P "$JOBS" -I{} bash -c 'run_one "{}"' < "$D/slots.txt" | tee "$D/results.txt"
pass=$(grep -c '^PASS' "$D/results.txt"); fail=$(grep -c '^FAIL' "$D/results.txt")
echo "content smoke: $pass passed, $fail failed ($D)"
[ "$fail" -eq 0 ]
