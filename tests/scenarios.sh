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
# (direction IV) fast verify path by default: 200us vsync nap instead of the
# historical 4ms (validated: unit set 501 fail=0, canon x2 52/52, golden
# end-states byte-identical). Override with V2_FAST_VSYNC=0 to fall back.
export V2_FAST_VSYNC=${V2_FAST_VSYNC:-1}
ROOT=$(pwd)
GOLDEN_WORK=""
if [ -f DATA.DAT ]; then
    GOLDEN_WORK=$(mktemp -d "${TMPDIR:-/tmp}/vikings-golden-XXXXXXXX") || exit 2
    trap 'rm -rf "$GOLDEN_WORK"' EXIT
    archive_status=$(python3 tests/reference_dat.py DATA.DAT "$GOLDEN_WORK/DATA.DAT") || exit 2
    case "$archive_status" in
        exact) rm -rf "$GOLDEN_WORK"; GOLDEN_WORK="" ;;
        normalized)
            if [ "$GOLDEN" = update ]; then
                echo "FAIL: GOLDEN=update requires the unmodified reference DATA.DAT"
                exit 2
            fi
            for file in ds_static.bin exe_static.bin vikings_keymap.cfg v2_options.cfg; do
                [ ! -f "$file" ] || ln -s "$ROOT/$file" "$GOLDEN_WORK/$file"
            done
            echo "SETUP audio differs: testing the supplied archive and checking goldens in a separate reference-settings run."
            ;;
        unknown)
            if [ "$GOLDEN" = update ]; then
                echo "FAIL: GOLDEN=update requires the unmodified reference DATA.DAT"
                exit 2
            fi
            echo "WARNING: unknown DATA.DAT; comparing goldens without normalization."
            rm -rf "$GOLDEN_WORK"; GOLDEN_WORK="" ;;
        *) echo "FAIL: unexpected archive status: $archive_status"; exit 2 ;;
    esac
fi
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
        if [ -f "$GOLDEN_REF" ] && [ ! -s "$DUMP_DIR/golden.txt" ]; then
            echo "FAIL: $name (supplied-archive replay did not produce a golden dump)"
            echo "  dump dir: $DUMP_DIR/"
            echo "FAIL missing-dump" > "$RESULTS_DIR/$name.status"
            return
        fi
        if [ -n "$GOLDEN_WORK" ] && [ -f "$GOLDEN_REF" ]; then
            mkdir -p "$DUMP_DIR/reference"
            if ! (cd "$GOLDEN_WORK" && V2_GOLDEN_DUMP="$DUMP_DIR/reference/golden.txt" "$ROOT/vikings_headless" \
                --replay-input="$ROOT/$inp" --max-frames="$FRAMES" \
                --dump-dir="$DUMP_DIR/reference" > "$DUMP_DIR/reference/run.log" 2>&1); then
                echo "FAIL: $name (reference-settings replay failed)"
                echo "  dump dir: $DUMP_DIR/"
                echo "FAIL reference" > "$RESULTS_DIR/$name.status"
                return
            fi
            GOLDEN_DUMP="$DUMP_DIR/reference/golden.txt"
        else
            GOLDEN_DUMP="$DUMP_DIR/golden.txt"
        fi
        if [ "$GOLDEN" = "update" ] && [ -s "$GOLDEN_DUMP" ]; then
            cp "$GOLDEN_DUMP" "$GOLDEN_REF"
        fi
        if [ -f "$GOLDEN_REF" ] && { [ ! -s "$GOLDEN_DUMP" ] || ! cmp -s "$GOLDEN_DUMP" "$GOLDEN_REF"; }; then
            echo "FAIL: $name (golden end-state mismatch)"
            diff "$GOLDEN_REF" "$GOLDEN_DUMP" | head -12 | sed 's/^/    /'
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
export ROOT GOLDEN_WORK

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
