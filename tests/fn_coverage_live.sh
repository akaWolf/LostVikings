#!/usr/bin/env bash
# Merged-профиль покрытия (#47, шаг 1): unit-прогоны + headless-реплеи в
# один gcda. Выход: /tmp/fncov_unit.json.gz (только юниты) и
# /tmp/fncov_merged.json.gz (юниты+live), отчёт с колонкой +live.
set -u
cd "$(dirname "$0")/.."
bash tests/fn_coverage.sh > /tmp/fncov_unitrun.log 2>&1 || true
rm -f vikings.exe_seg000.gcov.json.gz
gcov --json-format -o .obj-headless/src src/vikings.exe_seg000.cpp >/dev/null 2>&1
cp vikings.exe_seg000.gcov.json.gz /tmp/fncov_unit.json.gz
bash tests/scenarios.sh > /tmp/fncov_scen.log 2>&1
echo "scenarios: $(grep -cE '^PASS' /tmp/fncov_scen.log)/4 PASS"
rm -f vikings.exe_seg000.gcov.json.gz
gcov --json-format -o .obj-headless/src src/vikings.exe_seg000.cpp >/dev/null 2>&1
cp vikings.exe_seg000.gcov.json.gz /tmp/fncov_merged.json.gz
python3 python/fn_coverage_report.py /tmp/fncov_unit.json.gz \
    --live /tmp/fncov_merged.json.gz --csv /tmp/fn_coverage.csv
