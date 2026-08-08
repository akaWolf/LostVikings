#!/bin/bash
# Phase-D verify amplifier: run every password replay (tests/replays/pw_*)
# in parallel and report per-level verdicts + which level id actually
# loaded (semantic check on top of scenarios.sh: pw_NN must load level NN).
cd "$(dirname "$0")/.."
[ -x ./vikings_headless ] || { echo "FAIL: vikings_headless not built"; exit 2; }
JOBS=${JOBS:-$(nproc)}
R=/tmp/pw_sweep_$$
mkdir -p "$R"

run_one() {
    inp="$1"; name=$(basename "$inp" .inp)
    D="$R/$name"; mkdir -p "$D"
    F=$(cat "${inp%.inp}.frames")
    if ./vikings_headless --replay-input="$inp" --max-frames="$F" \
         --dump-dir="$D" > "$D/run.log" 2>&1; then
        lvl=$(grep -ae 'level init complete for level' "$D/run.log" \
              | grep -vae 'level 3[89]\|level 40' | head -1 \
              | sed -E 's/.*for level ([0-9]+),.*/\1/')
        echo "PASS: $name (level ${lvl:-NONE})"
        [ -n "$lvl" ] || echo "  WARN: no gameplay level seen"
        rm -rf "$D"
    else
        echo "FAIL: $name (exit $?)"
        grep -aE 'HEADLESS DIVERGENCE|detail:' "$D/run.log" | head -3 | sed 's/^/    /'
        echo "  dump: $D/"
    fi
}
export -f run_one; export R

ls tests/replays/pw_*.inp | xargs -P "$JOBS" -I{} bash -c 'run_one "$@"' _ {} | sort
