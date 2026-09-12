#!/usr/bin/env bash
# Co-op smoke test (UX stage 8): a 3-player replay on the V2_ONLY+HEADLESS
# build must be deterministic (two runs, identical golden dumps) and must move
# each viking of players 2 and 3, which the same replay without its `# coop 3`
# header (a one-player game: the P2:/P3: keys feed nobody there) leaves put.
# V2_COOP_TRACE=1 in the environment lands in the run logs ($OUT/*.log).
# Not part of tests/scenarios.sh: the default-mode headless binary mirrors the
# DOS engine, which has one player.
#   BIN=vikings_coop_test ./tests/coop_smoke.sh      (build: HEADLESS=1 V2_ONLY=1 RELEASE=1 make CXX=clang++ EXE_NAME=vikings_coop_test)
cd "$(dirname "$0")/.."
BIN=${BIN:-vikings_coop_test}
[ -x "./$BIN" ] || { echo "FAIL: ./$BIN not built"; exit 2; }
export V2_FAST_VSYNC=${V2_FAST_VSYNC:-1} SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
OUT="/tmp/coop_smoke_$$"; rm -rf "$OUT"; mkdir -p "$OUT"
fail=0
for inp in tests/coop/*.inp; do
    name=$(basename "$inp" .inp); F=900; [ -f "tests/coop/$name.frames" ] && F=$(cat "tests/coop/$name.frames")
    for p in a b; do
        V2_GOLDEN_DUMP="$OUT/${name}_$p.txt" timeout 600 "./$BIN" --replay-input="$inp" --max-frames="$F" > "$OUT/${name}_$p.log" 2>&1 || { echo "FAIL $name run $p (exit $?)"; fail=1; }
    done
    grep -v '^# coop' "$inp" > "$OUT/${name}_solo.inp"
    V2_GOLDEN_DUMP="$OUT/${name}_solo.txt" timeout 600 "./$BIN" --replay-input="$OUT/${name}_solo.inp" --max-frames="$F" > "$OUT/${name}_solo.log" 2>&1 || { echo "FAIL $name solo run"; fail=1; }
    if ! cmp -s "$OUT/${name}_a.txt" "$OUT/${name}_b.txt"; then echo "FAIL $name: non-deterministic"; fail=1; continue; fi
    xa=$(grep '^obj_world_x' "$OUT/${name}_a.txt" | sed 's/.*: //'); xs=$(grep '^obj_world_x' "$OUT/${name}_solo.txt" | sed 's/.*: //')
    echo "$name: coop x = ${xa:0:14} | solo x = ${xs:0:14}"
    set -- $xa; ca1=$1; ca2=$2; ca3=$3; set -- $xs; sa1=$1; sa2=$2; sa3=$3
    if [ "$ca2" = "$sa2" ] || [ "$ca3" = "$sa3" ]; then echo "FAIL $name: a viking of player 2 or 3 did not move (P2 $sa2 -> $ca2, P3 $sa3 -> $ca3)"; fail=1; else echo "PASS $name"; fi
done
[ "$fail" = 0 ]
