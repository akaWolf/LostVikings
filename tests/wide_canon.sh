#!/bin/bash
# Wide-screen determinism canon (UX stage 9, step 4). The wide view is a
# different deterministic game (objects activate at W+16, the camera centres on
# W/2), and a 320-recorded replay does not necessarily complete a wide level —
# so this does NOT compare against the 320 catalog. It builds a HEADLESS +
# V2_ONLY binary (the plain vikings_headless is the 320 dual-run judge and
# cannot go wide — its orig half stays 320), runs every replay TWICE at the WIDE
# width via V2_VIEW_W, and requires: (1) no crash, (2) the two golden end-states
# identical (the wide code paths are deterministic — the same as the 320 paths,
# only the width constant differs).
#   WIDE=2 (16:9 = 426, default) | WIDE=1 (16:10 = 384) | a raw pixel width
#   JOBS=k parallel (default nproc)
cd "$(dirname "$0")/.."
case "${WIDE:-2}" in 1) W=384;; 2) W=426;; *) W=${WIDE};; esac
JOBS=${JOBS:-$(nproc)}
BIN=vikings_headless_wide
echo "building $BIN (HEADLESS V2_ONLY)…"
( ulimit -s unlimited; HEADLESS=1 V2_ONLY=1 RELEASE=1 make CXX=clang++ -j"$JOBS" ) > /tmp/wide_build_$$.log 2>&1 || { echo "FAIL: build"; tail -5 /tmp/wide_build_$$.log; exit 2; }
cp -f vikings_headless "$BIN"   # HEADLESS+V2_ONLY writes vikings_headless in .obj-headless-v2only; keep it apart from the judge
OUT="/tmp/wide_canon_$$"; rm -rf "$OUT"; mkdir -p "$OUT"
export V2_FAST_VSYNC=${V2_FAST_VSYNC:-1} V2_VIEW_W=$W
run_one() {
    inp="$1"; name=$(basename "$inp" .inp); F=6000
    [ -f "tests/replays/$name.frames" ] && F=$(cat "tests/replays/$name.frames")
    local ok=1
    for p in a b; do
        V2_GOLDEN_DUMP="$OUT/${name}_$p.txt" timeout 900 "./$BIN" --replay-input="$inp" --max-frames="$F" > "$OUT/${name}_$p.log" 2>&1 || ok=0
    done
    if [ "$ok" = 0 ]; then echo "FAIL $name (crash/timeout)"; return 1; fi
    if ! cmp -s "$OUT/${name}_a.txt" "$OUT/${name}_b.txt"; then echo "FAIL $name (non-deterministic)"; return 1; fi
    echo "PASS $name"
}
export -f run_one; export OUT
ls tests/replays/*.inp | xargs -P "$JOBS" -I{} bash -c 'run_one {}' | tee "$OUT/results.txt"
pass=$(grep -c '^PASS' "$OUT/results.txt"); fail=$(grep -c '^FAIL' "$OUT/results.txt")
echo "wide canon (W=$W): $pass deterministic, $fail failed"
[ "$fail" -eq 0 ]
