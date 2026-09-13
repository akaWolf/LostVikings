#!/usr/bin/env bash
# Co-op lockstep bench (UX stage 8 step 3): three headless game instances
# of the same 3-player replay, one per player, over loopback TCP (the host is
# player 1) must end in the same golden dump as each other AND as the solo
# lockstep (one process, the same input delay, no peers). Every instance reads
# the same replay file and captures only its own player's events; the batches
# travel through the host; the host compares the per-frame hashes.
#   BIN=vikings_coop_test bash tests/coop_net.sh [scenario]   (default coop3_walk)
cd "$(dirname "$0")/.."
BIN=${BIN:-vikings_coop_test}
NAME=${1:-coop3_walk}
DELAY=${DELAY:-2}
PORT=${PORT:-$((7400 + RANDOM % 500))}
[ -x "./$BIN" ] || { echo "FAIL: ./$BIN not built"; exit 2; }
INP="tests/coop/$NAME.inp"; F=900; [ -f "tests/coop/$NAME.frames" ] && F=$(cat "tests/coop/$NAME.frames")
export V2_FAST_VSYNC=${V2_FAST_VSYNC:-1} SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
export V2_CONTENT=0   # the canon content: never the content/ pack of the repo root (v2_main.cpp)
OUT="/tmp/coop_net_$$"; rm -rf "$OUT"; mkdir -p "$OUT"
# the reference: the solo lockstep
V2_GOLDEN_DUMP="$OUT/solo.txt" timeout 900 "./$BIN" --replay-input="$INP" --max-frames="$F" --delay=$DELAY > "$OUT/solo.log" 2>&1 || { echo "FAIL: solo run (exit $?)"; exit 1; }
# the three peers
V2_GOLDEN_DUMP="$OUT/p1.txt" timeout 900 "./$BIN" --replay-input="$INP" --max-frames="$F" --coop=3 --host=$PORT --delay=$DELAY > "$OUT/p1.log" 2>&1 &
H=$!
sleep 1
V2_GOLDEN_DUMP="$OUT/p2.txt" timeout 900 "./$BIN" --replay-input="$INP" --max-frames="$F" --join=127.0.0.1:$PORT > "$OUT/p2.log" 2>&1 &
C2=$!
V2_GOLDEN_DUMP="$OUT/p3.txt" timeout 900 "./$BIN" --replay-input="$INP" --max-frames="$F" --join=127.0.0.1:$PORT > "$OUT/p3.log" 2>&1 &
C3=$!
fail=0
wait $H  || { echo "FAIL: host exited $?"; fail=1; }
wait $C2 || { echo "FAIL: player 2 exited $?"; fail=1; }
wait $C3 || { echo "FAIL: player 3 exited $?"; fail=1; }
for p in p1 p2 p3; do
    [ -s "$OUT/$p.txt" ] || { echo "FAIL: no golden dump for $p"; fail=1; continue; }
    if cmp -s "$OUT/solo.txt" "$OUT/$p.txt"; then echo "$p: identical to the solo lockstep"; else echo "FAIL: $p differs from the solo lockstep"; diff "$OUT/solo.txt" "$OUT/$p.txt" | head -6; fail=1; fi
done
if grep -q "V2-NET-DESYNC" "$OUT"/p*.log; then grep -h "V2-NET-DESYNC" "$OUT"/p*.log | head -3; echo "FAIL: desync reported"; fail=1; fi
grep -q "V2-NET: closed" "$OUT/p1.log" || { echo "FAIL: the host never reported a clean close"; fail=1; }
grep -h "V2-NET: closed" "$OUT"/p1.log
echo "logs: $OUT"
[ "$fail" = 0 ] && echo "PASS $NAME (delay $DELAY, port $PORT)"
[ "$fail" = 0 ]
