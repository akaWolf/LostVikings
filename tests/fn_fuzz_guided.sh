#!/usr/bin/env bash
# Coverage-guided fuzz (#47, шаг 4): гоняет юнит(ы) со случайными сидами и
# оставляет те, что зажгли НОВЫЕ gcov-строки оракула. Требует COV-сборку.
#   bash tests/fn_fuzz_guided.sh <unit> [iterations=40]
# Найденные сиды дописываются в tests/fn_seeds/<unit>.txt; суммарный gcda
# аккумулируется (baseline не сбрасывается между итерациями).
set -u
cd "$(dirname "$0")/.."
UNIT=${1:?unit name}
N=${2:-40}
BIN=./vikings_headless
GCDA=.obj-headless/src/vikings.exe_seg000.gcda
[ -f .obj-headless/src/vikings.exe_seg000.gcno ] || { echo "need COV build"; exit 1; }
mkdir -p tests/fn_seeds
lines() {
  rm -f vikings.exe_seg000.gcov.json.gz
  gcov -b --json-format -o .obj-headless/src src/vikings.exe_seg000.cpp >/dev/null 2>&1
  python3 - <<'PY'
import gzip, json
d = json.load(gzip.open('vikings.exe_seg000.gcov.json.gz','rt'))
n = 0
for f in d['files']:
    if f['file'].endswith('vikings.exe_seg000.cpp'):
        n = sum(1 for l in f['lines'] if l['count'] > 0)
print(n)
PY
}
rm -f "$GCDA"
FT_NO_FORK=1 FNSELFTEST=$UNIT "$BIN" >/dev/null 2>&1   # baseline: зашитый сид
BASE=$(lines); echo "baseline lines: $BASE"
for i in $(seq 1 "$N"); do
  SEED=$(od -An -N4 -tu4 /dev/urandom | tr -d ' ')
  FT_NO_FORK=1 FT_FUZZ_SEED=$SEED FNSELFTEST=$UNIT "$BIN" >/dev/null 2>&1
  CUR=$(lines)
  if [ "$CUR" -gt "$BASE" ]; then
    echo "seed $SEED: +$((CUR-BASE)) lines (total $CUR)"
    echo "$SEED" >> tests/fn_seeds/$UNIT.txt
    BASE=$CUR
  fi
done
echo "final lines: $BASE"
