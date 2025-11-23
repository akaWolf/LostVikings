#!/bin/bash
# HEADLESS fuzz harness — run N random-seed sessions, collect divergences.
#
# Usage: ./fuzz_harness.sh [N=100] [duration=10000]
# Failures saved to /tmp/headless_bugs/seed_<N>.inp + dump dir.

cd "$(dirname "$0")/.."
N=${1:-100}
DURATION=${2:-10000}
BUGS_DIR=/tmp/headless_bugs
mkdir -p "$BUGS_DIR"

if [ ! -x ./vikings_headless ]; then
    echo "FAIL: vikings_headless not built"
    exit 2
fi

PASS=0
FAIL=0
echo "Running $N seeds (duration=$DURATION frames per seed)..."

for seed in $(seq 1 $N); do
    DUMP_DIR="$BUGS_DIR/seed_${seed}"
    rm -rf "$DUMP_DIR"
    mkdir -p "$DUMP_DIR"
    inp="$DUMP_DIR/replay.inp"
    LOG="$DUMP_DIR/run.log"
    python3 tests/fuzz_gen.py $seed $DURATION > "$inp"

    if timeout 60 ./vikings_headless \
        --replay-input="$inp" \
        --max-frames="$DURATION" \
        --dump-dir="$DUMP_DIR" \
        --seed=$seed \
        > "$LOG" 2>&1; then
        PASS=$((PASS+1))
        rm -rf "$DUMP_DIR"
    else
        EXIT=$?
        FAIL=$((FAIL+1))
        echo "FAIL seed=$seed exit=$EXIT → $DUMP_DIR/"
    fi

    # Progress every 10 seeds
    if [ $((seed % 10)) -eq 0 ]; then
        echo "  Progress: $seed/$N (pass=$PASS fail=$FAIL)"
    fi
done

echo ""
echo "=== Done: $PASS pass / $FAIL fail (of $N seeds) ==="
echo "Failures preserved at: $BUGS_DIR/seed_*.{inp,/}"
[ $FAIL -gt 0 ] && exit 1
exit 0
