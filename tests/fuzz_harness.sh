#!/bin/bash
# HEADLESS fuzz harness — run N random-seed sessions, collect divergences.
#
# Usage: ./fuzz_harness.sh [N=100] [duration=10000]
# Runs seeds in parallel across all cores (each run is single-threaded).
# Override core count with JOBS=k. Failures saved to
# /tmp/headless_bugs/seed_<N>.inp + dump dir.

cd "$(dirname "$0")/.."
N=${1:-100}
DURATION=${2:-10000}
JOBS=${JOBS:-$(nproc)}
BUGS_DIR=/tmp/headless_bugs
mkdir -p "$BUGS_DIR"

if [ ! -x ./vikings_headless ]; then
    echo "FAIL: vikings_headless not built"
    exit 2
fi

# Worker: generate + run one seed. Keeps dump dir only on failure.
run_seed() {
    seed="$1"
    DUMP_DIR="$BUGS_DIR/seed_${seed}"
    rm -rf "$DUMP_DIR"; mkdir -p "$DUMP_DIR"
    inp="$DUMP_DIR/replay.inp"
    LOG="$DUMP_DIR/run.log"
    python3 tests/fuzz_gen.py "$seed" "$DURATION" > "$inp"

    if timeout 60 ./vikings_headless \
        --replay-input="$inp" \
        --max-frames="$DURATION" \
        --dump-dir="$DUMP_DIR" \
        --seed="$seed" \
        > "$LOG" 2>&1; then
        rm -rf "$DUMP_DIR"
    else
        EXIT=$?
        echo "FAIL seed=$seed exit=$EXIT → $DUMP_DIR/"
    fi
}
export -f run_seed
export BUGS_DIR DURATION

echo "Running $N seeds (duration=$DURATION frames per seed) on $JOBS cores..."
seq 1 "$N" | xargs -P"$JOBS" -I{} bash -c 'run_seed "$@"' _ {}

FAIL=$(find "$BUGS_DIR" -maxdepth 1 -name 'seed_*' -type d | wc -l)
PASS=$((N - FAIL))

echo ""
echo "=== Done: $PASS pass / $FAIL fail (of $N seeds) ==="
[ "$FAIL" -gt 0 ] && echo "Failures preserved at: $BUGS_DIR/seed_*/"
[ "$FAIL" -gt 0 ] && exit 1
exit 0
