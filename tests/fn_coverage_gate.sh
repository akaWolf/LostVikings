#!/usr/bin/env bash
# CI-гейт покрытия (#47, шаг 5): сравнение per-sub exec-счётчиков с
# baseline. Новые missed-инструкции или падение exec любого sub_ → FAIL.
#   Снять baseline:  bash tests/fn_coverage_gate.sh --update
#   Проверка:        bash tests/fn_coverage_gate.sh   (после fn_coverage.sh)
set -u
cd "$(dirname "$0")/.."
BASE=tests/fn_coverage_baseline.csv
CUR=/tmp/fn_coverage.csv
[ -f "$CUR" ] || { echo "run tests/fn_coverage.sh first (produces $CUR)"; exit 1; }
if [ "${1:-}" = "--update" ]; then
  cp "$CUR" "$BASE"; echo "baseline updated: $BASE"; exit 0
fi
[ -f "$BASE" ] || { echo "no baseline — run with --update once"; exit 1; }
python3 - "$BASE" "$CUR" <<'PY'
import sys, csv
base = {r['sub']: int(r['executed']) for r in csv.DictReader(open(sys.argv[1]))}
cur  = {r['sub']: int(r['executed']) for r in csv.DictReader(open(sys.argv[2]))}
rc = 0
for sub, b in sorted(base.items()):
    c = cur.get(sub)
    if c is None:
        print(f"GATE-FAIL: {sub} disappeared from the report"); rc = 1
    elif c < b:
        print(f"GATE-FAIL: {sub} coverage dropped {b} -> {c}"); rc = 1
for sub in sorted(set(cur) - set(base)):
    print(f"GATE-NOTE: new sub {sub} (exec {cur[sub]}) — update baseline")
print("GATE:", "FAIL" if rc else "OK")
sys.exit(rc)
PY
