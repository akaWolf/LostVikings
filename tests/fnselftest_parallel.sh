#!/usr/bin/env bash
# Run FNSELFTEST units in parallel, one process per unit (isolated DS each).
# Heavy exhaustive units (X-axis searches sub_158aa/sub_158b9: slow Y-walk
# oracle) are sharded across processes via FNSELFTEST_SHARD=i/N.
# Usage: tests/fnselftest_parallel.sh [unit ...]   (default: all registered)
# Logs: /tmp/fnst_<unit>[.shard].log ; exit 0 iff every summary has fail=0.
cd "$(dirname "$0")/.."
BIN=${FNST_BIN:-./vikings_headless}
[ -x "$BIN" ] || { echo "FAIL: $BIN not built"; exit 1; }

UNITS=${*:-"sub_15972 sub_161a1 sub_15da8 sub_15d6b sub_13d68 sub_13dd6 sub_13e15 \
sub_13c0c sub_17496 sub_1746c sub_10704 sub_10753 sub_1064b sub_101be vmops \
sub_10255 sub_1020f sub_12fc6 sub_12fcb sub_12fd0 \
sub_158aa sub_158b9 sub_158c8 sub_158d7 sub_158e6 sub_1303a sub_13031 \
sub_1614e sub_15c37 sub_15c93 sub_15afd sub_10982 sub_10cd8 \
sub_10fe6 sub_10ffc \
sub_11192 sub_111a1 sub_111df sub_11784 sub_137f1 sub_12fb3 \
sub_12ca3 sub_12ce4 sub_108b8 \
sub_11397 sub_113b0 sub_113d8 sub_116e3 \
sub_11383 sub_1133a sub_1241e sub_12816 sub_12515 sub_12529 \
sub_1450b sub_10e99 sub_15d3c sub_15d42 sub_11c52 \
sub_13a0e sub_13ba5 sub_11446 sub_11569 sub_15911 sub_12549 sub_11cbb \
sub_173c7 sub_14207 sub_112ae sub_12d2c sub_1167a sub_116ae sub_11b0b sub_10813 \
sub_15cef sub_15cf5 sub_15de5 sub_15df2 sub_15fb1 sub_15fbe sub_1603e sub_160cf sub_15ae9 sub_1589b sub_159c6 sub_159d3 sub_159df sub_15a57 sub_15ac4 sub_158f5 sub_1592d sub_15505 sub_15517 sub_1555c sub_15569 sub_15530 sub_15546 sub_155d6 sub_156c0 sub_1584e sub_157eb sub_16235 sub_16243 sub_16390 sub_163ac"}
SHARDED="sub_158aa sub_158b9"
NSH=6

logs=""
for u in $UNITS; do
if echo "$SHARDED" | grep -qw "$u"; then
for i in $(seq 0 $((NSH-1))); do
setsid timeout 7200 env FNSELFTEST=$u FNSELFTEST_SHARD=$i/$NSH stdbuf -o0 -e0 "$BIN" > /tmp/fnst_$u.$i.log 2>&1 < /dev/null &
logs="$logs /tmp/fnst_$u.$i.log"
done
else
setsid timeout 7200 env FNSELFTEST=$u stdbuf -o0 -e0 "$BIN" > /tmp/fnst_$u.log 2>&1 < /dev/null &
logs="$logs /tmp/fnst_$u.log"
fi
done
wait

rc=0
for l in $logs; do
line=$(grep -h "FNSELFTEST-SUMMARY" "$l" 2>/dev/null | tail -1)
if [ -z "$line" ]; then echo "FAIL(no-summary): $l"; rc=1
elif echo "$line" | grep -q "fail=0"; then echo "PASS[$(basename $l)]: $line"
else echo "FAIL[$(basename $l)]: $line"; rc=1
fi
done
exit $rc
