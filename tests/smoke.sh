#!/bin/bash
# HEADLESS smoke test — verify build + game start work.
# Empty replay, run N frames, exit 0 = clean.

cd "$(dirname "$0")/.."

if [ ! -x ./vikings_headless ]; then
    echo "FAIL: vikings_headless not built. Run: make clean && HEADLESS=1 RELEASE=1 make -j\$(nproc)"
    exit 2
fi

DUMP_DIR="/tmp/headless_smoke_$$"
rm -rf "$DUMP_DIR"
mkdir -p "$DUMP_DIR"
LOG="$DUMP_DIR/run.log"

./vikings_headless \
    --replay-input=tests/replays/empty.inp \
    --max-frames=200 \
    --dump-dir="$DUMP_DIR" \
    > "$LOG" 2>&1
EXIT=$?

case $EXIT in
    0) echo "PASS: smoke test (200 frames, no divergence)"
       echo "  log: $LOG";;
    1) echo "FAIL: divergence detected"
       echo "  dump dir: $DUMP_DIR/"
       ls "$DUMP_DIR/" 2>/dev/null | sed 's/^/    /'
       grep -E "HEADLESS DIVERGENCE|detail:" "$LOG" | sed 's/^/    /'
       exit 1;;
    4) echo "FAIL: segfault during run"
       echo "  log: $LOG"
       exit 4;;
    *) echo "FAIL: unexpected exit code $EXIT"
       echo "  log: $LOG"
       exit $EXIT;;
esac
