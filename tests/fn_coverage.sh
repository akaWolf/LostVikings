#!/usr/bin/env bash
# Честное gcov-покрытие оракула (task #47): один процесс на юнит
# (межюнитная изоляция уровня ОС), gcda аккумулируется мёрджем между
# процессами. Дочки fork-режима не флашат gcda, поэтому юниты гоняются с
# FT_NO_FORK=1 — порча внутри юнита возможна, но не влияет на соседей и
# на итоговое покрытие влияет только его собственных кейсов.
# Использование:
#   COV_SEG000=1 HEADLESS=1 make -j$(nproc)   # инструментированная сборка
#   bash tests/fn_coverage.sh                  # прогон + отчёт
set -u
cd "$(dirname "$0")/.."
BIN=./vikings_headless
[ -x "$BIN" ] || { echo "no $BIN"; exit 1; }
[ -f .obj-headless/src/vikings.exe_seg000.gcno ] || {
  echo "no .gcno — build with COV_SEG000=1 HEADLESS=1 make"; exit 1; }
rm -f .obj-headless/src/vikings.exe_seg000.gcda
# Полный список юнитов — из канонического шардера (литеральный grep по
# v2_fn_test.cpp пропускает clear/leaf-семьи, матчащиеся через g_name).
UNITS=$(awk '/^UNITS=\$\{\*:-"/,/"\}$/' tests/fnselftest_parallel.sh \
        | sed 's/^UNITS=\${\*:-"//; s/"}$//; s/\\$//' | tr ' ' '\n' \
        | grep -v '^$' | awk '!seen[$0]++')
n=0
for u in $UNITS; do
  n=$((n+1))
  FT_NO_FORK=1 FNSELFTEST=$u "$BIN" > /tmp/fncov_$u.log 2>&1
  line=$(grep -h 'FNSELFTEST-SUMMARY' /tmp/fncov_$u.log | tail -1)
  case "$line" in
    *"total cases=0 "*) echo "EMPTY[$u]: $line";;
    *fail=0*) ;;
    *) echo "FAIL[$u]: $line";;
  esac
done
echo "units run: $n"
gcov --json-format -o .obj-headless/src src/vikings.exe_seg000.cpp \
  > /dev/null 2>&1
python3 python/fn_coverage_report.py vikings.exe_seg000.gcov.json.gz \
  --csv /tmp/fn_coverage.csv
