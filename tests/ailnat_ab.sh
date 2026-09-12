#!/usr/bin/env bash
# stage 6.1 w3 increment 7: OPL-stream A/B of the NATIVE sequencer.
#
# Runs every replay twice on the V2_ONLY binary — once through the
# interpreted driver, once with V2_AIL_NATIVE=1 — under identical
# deterministic settings (V2_AIL_FRAME_TICKS=1) and byte-compares the
# V2_OPL_TRACE streams. The stored assets_raw/opl_ref shas are reported
# as a secondary diagnostic (they pin the historical baseline).
#
#   ./tests/ailnat_ab.sh [replay-glob] [jobs]
set -u
export V2_CONTENT=0   # the canon content: never the content/ pack of the repo root (v2_main.cpp)
cd "$(dirname "$0")/.."
GLOB="${1:-tests/replays/*.inp}"
JOBS="${2:-4}"
OUT=/dev/shm/ailnat_ab
mkdir -p "$OUT"

if [ ! -x ./vikings ]; then
echo "FAIL: ./vikings not built (V2_ONLY=1 make)"; exit 2
fi

run_one() {
inp="$1"
name=$(basename "$inp" .inp)
frames=6000
[ -f "tests/replays/${name}.frames" ] && frames=$(cat "tests/replays/${name}.frames")
env0="SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy V2_AIL_FRAME_TICKS=1 V2_NOVSYNC=1"
eval $env0 V2_OPL_TRACE="$OUT/$name.i.trace" timeout 3600 ./vikings \
--replay "$inp" --max-frames="$frames" > "$OUT/$name.i.log" 2>&1
ri=$?
eval $env0 V2_AIL_NATIVE=1 V2_OPL_TRACE="$OUT/$name.n.trace" timeout 3600 ./vikings \
--replay "$inp" --max-frames="$frames" > "$OUT/$name.n.log" 2>&1
rn=$?
if cmp -s "$OUT/$name.i.trace" "$OUT/$name.n.trace"; then
verdict="PASS"
else
verdict="DIFF"
fi
refnote=""
if [ -f "assets_raw/opl_ref/$name.trace" ]; then
if cmp -s "$OUT/$name.i.trace" "assets_raw/opl_ref/$name.trace"; then
refnote=" ref=match"
else
refnote=" ref=other"
fi
fi
lines=$(wc -l < "$OUT/$name.n.trace" 2>/dev/null || echo 0)
echo "$verdict $name exits=$ri/$rn lines=$lines$refnote"
}
export -f run_one
export OUT

ls $GLOB | xargs -P "$JOBS" -I{} bash -c 'run_one "$@"' _ {} | tee "$OUT/summary.txt"
echo "---"
grep -c '^PASS' "$OUT/summary.txt" | xargs echo "PASS:"
grep -c '^DIFF' "$OUT/summary.txt" | xargs echo "DIFF:"
