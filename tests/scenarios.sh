#!/bin/bash
# HEADLESS scenario tests — run all replays in tests/replays/ + report pass/fail.
#
# Runs scenarios in parallel across all cores (each headless run is
# single-threaded, so N replays = N cores busy). Override with JOBS=k.

cd "$(dirname "$0")/.."

if [ ! -x ./vikings_headless ]; then
    echo "FAIL: vikings_headless not built"
    exit 2
fi

JOBS=${JOBS:-$(nproc)}
RESULTS_DIR="/tmp/headless_scenario_results_$$"
rm -rf "$RESULTS_DIR"
mkdir -p "$RESULTS_DIR"

# Worker: run one replay. Writes "PASS" / "FAIL <exit>" to its .status file,
# prints a one-line result, and keeps the dump dir only on failure.
run_one() {
    inp="$1"
    name=$(basename "$inp" .inp)
    DUMP_DIR="/tmp/headless_scenario_${name}_$$"
    rm -rf "$DUMP_DIR"; mkdir -p "$DUMP_DIR"
    LOG="$DUMP_DIR/run.log"

    # Per-scenario frame budget: tests/replays/<name>.frames if present
    # (written by record.sh = last recorded frame + tail), else default.
    FRAMES=10000
    [ -f "tests/replays/${name}.frames" ] && FRAMES=$(cat "tests/replays/${name}.frames")

    # (direction V) golden end-state channel: every run dumps the final
    # shadow-DS named-field snapshot; compared against tests/golden_states/
    # when a golden exists. GOLDEN=update rewrites the catalog from this run.
    GOLDEN_REF="tests/golden_states/${name}.txt"
    if V2_GOLDEN_DUMP="$DUMP_DIR/golden.txt" ./vikings_headless \
        --replay-input="$inp" \
        --max-frames="$FRAMES" \
        --dump-dir="$DUMP_DIR" \
        > "$LOG" 2>&1; then
        if [ "$GOLDEN" = "update" ] && [ -s "$DUMP_DIR/golden.txt" ]; then
            cp "$DUMP_DIR/golden.txt" "$GOLDEN_REF"
        fi
        if [ -f "$GOLDEN_REF" ] && [ -s "$DUMP_DIR/golden.txt" ] \
           && ! cmp -s "$DUMP_DIR/golden.txt" "$GOLDEN_REF"; then
            echo "FAIL: $name (golden end-state mismatch)"
            diff "$GOLDEN_REF" "$DUMP_DIR/golden.txt" | head -12 | sed 's/^/    /'
            echo "  dump dir: $DUMP_DIR/"
            echo "FAIL golden" > "$RESULTS_DIR/$name.status"
            return
        fi
        echo "PASS: $name"
        echo "PASS" > "$RESULTS_DIR/$name.status"
        rm -rf "$DUMP_DIR"
    else
        EXIT=$?
        {
            echo "FAIL: $name (exit $EXIT)"
            echo "  dump dir: $DUMP_DIR/"
            grep -E "HEADLESS DIVERGENCE|detail:" "$LOG" | sed 's/^/    /'
        }
        echo "FAIL $EXIT" > "$RESULTS_DIR/$name.status"
    fi
}
export -f run_one
export RESULTS_DIR
export GOLDEN

shopt -s nullglob
replays=(tests/replays/*.inp)
if [ ${#replays[@]} -eq 0 ]; then
    echo "FAIL: no replays in tests/replays/"
    exit 2
fi

echo "Running ${#replays[@]} scenarios on $JOBS cores..."
printf '%s\n' "${replays[@]}" | xargs -P"$JOBS" -I{} bash -c 'run_one "$@"' _ {}

PASS=$(grep -l '^PASS' "$RESULTS_DIR"/*.status 2>/dev/null | wc -l)
FAIL=$(grep -l '^FAIL' "$RESULTS_DIR"/*.status 2>/dev/null | wc -l)
FAILED_TESTS=$(grep -l '^FAIL' "$RESULTS_DIR"/*.status 2>/dev/null | xargs -r -n1 basename | sed 's/\.status$//' | tr '\n' ' ')
rm -rf "$RESULTS_DIR"

echo ""
echo "=== Results: $PASS pass / $FAIL fail ==="
if [ "$FAIL" -gt 0 ]; then
    echo "Failed: $FAILED_TESTS"
    exit 1
fi
