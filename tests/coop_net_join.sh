#!/usr/bin/env bash
# Co-op lockstep bench 2 (UX stage 8 tails): joining a running game. The host
# starts with player 2 in its lobby (--lobby=1) and runs; player 3 joins a few
# seconds later, gets the host's state image at a main read and takes part
# from there. All three must end in the golden dump of the solo lockstep on
# the same replay — the joiner's own events (player 3's) all lie after the
# join, so the join itself must be invisible to the simulation.
#   BIN=vikings_coop_test bash tests/coop_net_join.sh [scenario]   (default coop3_walk; player 3 acts from frame 700)
cd "$(dirname "$0")/.."
BIN=${BIN:-vikings_coop_test}
NAME=${1:-coop3_walk}
DELAY=${DELAY:-2}
PORT=${PORT:-$((7400 + RANDOM % 500))}
JOIN_AFTER=${JOIN_AFTER:-4}
[ -x "./$BIN" ] || { echo "FAIL: ./$BIN not built"; exit 2; }
INP="tests/coop/$NAME.inp"; F=900; [ -f "tests/coop/$NAME.frames" ] && F=$(cat "tests/coop/$NAME.frames")
export V2_FAST_VSYNC=${V2_FAST_VSYNC:-1} SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
OUT="/tmp/coop_join_$$"; rm -rf "$OUT"; mkdir -p "$OUT"
V2_GOLDEN_DUMP="$OUT/solo.txt" timeout 900 "./$BIN" --replay-input="$INP" --max-frames="$F" --delay=$DELAY > "$OUT/solo.log" 2>&1 || { echo "FAIL: solo run (exit $?)"; exit 1; }
V2_GOLDEN_DUMP="$OUT/p1.txt" timeout 900 "./$BIN" --replay-input="$INP" --max-frames="$F" --coop=3 --host=$PORT --lobby=1 --delay=$DELAY > "$OUT/p1.log" 2>&1 &
H=$!
sleep 1
V2_GOLDEN_DUMP="$OUT/p2.txt" timeout 900 "./$BIN" --replay-input="$INP" --max-frames="$F" --join=127.0.0.1:$PORT > "$OUT/p2.log" 2>&1 &
C2=$!
sleep "$JOIN_AFTER"
V2_GOLDEN_DUMP="$OUT/p3.txt" timeout 900 "./$BIN" --replay-input="$INP" --max-frames="$F" --join=127.0.0.1:$PORT > "$OUT/p3.log" 2>&1 &
C3=$!
fail=0
wait $H  || { echo "FAIL: host exited $?"; fail=1; }
wait $C2 || { echo "FAIL: player 2 exited $?"; fail=1; }
wait $C3 || { echo "FAIL: player 3 exited $?"; fail=1; }
grep -h "LOCKSTEP joined\|takes part from\|joined at read" "$OUT/p1.log" "$OUT/p3.log" | head -4
jf=$(sed -n 's/.*LOCKSTEP joined at read [0-9]*, frame \([0-9]*\).*/\1/p' "$OUT/p3.log" | head -1)
[ -n "$jf" ] || { echo "FAIL: player 3 never joined the running game"; fail=1; }
[ -n "$jf" ] && [ "$jf" -ge 700 ] && { echo "FAIL: the join landed at frame $jf, after player 3's first event (700) — the bench is not meaningful"; fail=1; }
for p in p1 p2 p3; do
    [ -s "$OUT/$p.txt" ] || { echo "FAIL: no golden dump for $p"; fail=1; continue; }
    if cmp -s "$OUT/solo.txt" "$OUT/$p.txt"; then echo "$p: identical to the solo lockstep"; else echo "FAIL: $p differs from the solo lockstep"; diff "$OUT/solo.txt" "$OUT/$p.txt" | head -6; fail=1; fi
done
if grep -q "V2-NET-DESYNC" "$OUT"/p*.log; then grep -h "V2-NET-DESYNC" "$OUT"/p*.log | head -3; echo "FAIL: desync reported"; fail=1; fi
grep -h "V2-NET: closed" "$OUT/p1.log"
echo "logs: $OUT"
[ "$fail" = 0 ] && echo "PASS $NAME join (delay $DELAY, port $PORT, joined at frame $jf)"
[ "$fail" = 0 ]
