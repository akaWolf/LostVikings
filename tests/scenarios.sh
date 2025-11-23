#!/bin/bash
# HEADLESS scenario tests — run all replays in tests/replays/ + report pass/fail.

cd "$(dirname "$0")/.."

if [ ! -x ./vikings_headless ]; then
    echo "FAIL: vikings_headless not built"
    exit 2
fi

PASS=0
FAIL=0
FAILED_TESTS=()

for inp in tests/replays/*.inp; do
    name=$(basename "$inp" .inp)
    DUMP_DIR="/tmp/headless_scenario_${name}_$$"
    rm -rf "$DUMP_DIR"
    mkdir -p "$DUMP_DIR"
    LOG="$DUMP_DIR/run.log"

    if ./vikings_headless \
        --replay-input="$inp" \
        --max-frames=10000 \
        --dump-dir="$DUMP_DIR" \
        > "$LOG" 2>&1; then
        echo "PASS: $name"
        PASS=$((PASS+1))
        rm -rf "$DUMP_DIR"
    else
        EXIT=$?
        echo "FAIL: $name (exit $EXIT)"
        echo "  dump dir: $DUMP_DIR/"
        grep -E "HEADLESS DIVERGENCE|detail:" "$LOG" | sed 's/^/    /'
        FAIL=$((FAIL+1))
        FAILED_TESTS+=("$name")
    fi
done

echo ""
echo "=== Results: $PASS pass / $FAIL fail ==="
if [ $FAIL -gt 0 ]; then
    echo "Failed: ${FAILED_TESTS[@]}"
    exit 1
fi
