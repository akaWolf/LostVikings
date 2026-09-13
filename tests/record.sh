#!/bin/bash
# Record a curated gameplay scenario in TEST mode (orig + v2 mirror).
#
# Usage:  ./tests/record.sh <scenario-name> [max-frames-hint]
#
# Plays the normal game with frame-based input recording. Whatever you do
# is written to tests/replays/<scenario-name>.inp. Quit cleanly (Alt+X or
# close the window) when the scenario is done — events are flushed per
# keystroke, so a hard kill also keeps what was recorded.
#
# The resulting .inp replays bit-for-bit under vikings_headless (same
# asm.cpp code path), which is what tests/scenarios.sh runs in CI.
#
# Requires: ./vikings (default build) + DATA.DAT in the repo root.

cd "$(dirname "$0")/.."

NAME="${1:?usage: record.sh <scenario-name> [max-frames-hint]}"
HINT="${2:-}"
OUT="tests/replays/${NAME}.inp"

if [ ! -x ./vikings ]; then
    echo "Default build missing. Run: make -j\$(nproc)"
    exit 2
fi
if [ ! -f DATA.DAT ]; then
    echo "DATA.DAT required in repo root (copyrighted asset, not in repo)."
    exit 2
fi
if [ -f "$OUT" ]; then
    read -r -p "$OUT exists. Overwrite? [y/N] " ans
    [ "$ans" = "y" ] || { echo "aborted"; exit 0; }
fi

echo "Recording scenario '$NAME' → $OUT"
echo "Play now. Quit (Alt+X / close window) when done."
[ -n "$HINT" ] && echo "Target length hint: ~$HINT frames."

./vikings --record-input="$OUT"

if [ -f "$OUT" ]; then
    EVENTS=$(grep -cvE '^#|^$' "$OUT")
    LASTF=$(grep -vE '^#|^$' "$OUT" | tail -1 | awk '{print $1}')
    # The replay must run for exactly as many frames as the recording session
    # did — to the LAST FRAME REACHED during recording, so verify covers the
    # whole session (incl. frames after the final input). The recorder writes
    # that as "# end-frame N" on clean shutdown. Prefer it; if a hard kill
    # skipped shutdown, fall back to the last input frame (NO arbitrary tail).
    ENDF=$(grep -E '^# end-frame ' "$OUT" | tail -1 | awk '{print $3}')
    FRAMES="${ENDF:-$LASTF}"
    echo "Saved $OUT — $EVENTS events, last input frame ${LASTF:-0}, recording end-frame ${ENDF:-<hard kill, none>}."
    if [ -n "$FRAMES" ]; then
        echo "$FRAMES" > "tests/replays/${NAME}.frames"
        echo "Wrote tests/replays/${NAME}.frames = $FRAMES"
    fi
else
    echo "No file produced — did the game start?"
    exit 1
fi
