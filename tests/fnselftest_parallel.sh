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
sub_15cef sub_15cf5 sub_15de5 sub_15df2 sub_15fb1 sub_15fbe sub_1603e sub_160cf sub_15ae9 sub_1589b sub_159c6 sub_159d3 sub_159df sub_15a57 sub_15ac4 sub_158f5 sub_1592d sub_15505 sub_15517 sub_1555c sub_15569 sub_15530 sub_15546 sub_155d6 sub_156c0 sub_1584e sub_157eb sub_16235 sub_16243 sub_16390 sub_163ac sub_153ea sub_15403 sub_1542a sub_15445 sub_15473 sub_15470 sub_154bf sub_15788 sub_136a0 sub_13757 sub_1386b sub_1625d sub_13916 sub_12fe5 sub_13c93 sub_13d30 sub_13d52 sub_12f82 sub_13e52 sub_13809 sub_13bbd sub_13fc2 sub_139ef sub_13a14 sub_13a34 sub_1689e sub_16dc1 sub_16dd9 sub_1712b sub_171dc sub_16ded sub_16e75 sub_16f5f sub_17049 sub_170b9 sub_13ae0 sub_141f7 sub_141fb sub_141ff sub_14203 sub_165aa sub_166e8 sub_16710 sub_16661 sub_1673c sub_1406d sub_13084 sub_135cf sub_142b7 sub_142c0 sub_142cf sub_142c1 sub_142d3 sub_142dc sub_142fc sub_1431c sub_14327 sub_14334 sub_14340 sub_141f6 sub_143f2 sub_143fe sub_14409 sub_14428 sub_1443d sub_1444f sub_14453 sub_14469 sub_1446d sub_14483 sub_14487 sub_144a9 sub_144ad sub_144cf sub_144d3 sub_144fd sub_14501 sub_14532 sub_14561 sub_14590 sub_145b5 sub_145da sub_145e5 sub_14604 sub_14624 sub_1462e sub_14646 sub_14652 sub_1466e sub_14675 sub_14681 sub_14686 sub_1469e sub_146a3 sub_146af sub_146b4 sub_146d0 sub_146de sub_146f6 sub_14704 sub_14713 sub_14721 sub_1473d sub_1474b sub_14763 sub_14771 sub_1477d sub_1478b sub_147a7 sub_147bf sub_147cb sub_147e7 sub_147ff sub_1480b sub_14827 sub_1483f sub_1484b sub_14867 sub_14879 sub_1488b sub_1489d sub_148af sub_148c1 sub_148d3 sub_148e5 sub_148f7 sub_14909 sub_1491b sub_14933 sub_1494b sub_14963 sub_1497b sub_14993 sub_149ab sub_149c3 sub_149db sub_149f3 sub_14a0b sub_14a1b sub_14a2b sub_14a3b sub_14a4b sub_14a5b sub_14a6b sub_14a7b sub_14a8b sub_14a9b sub_14aab sub_14abb sub_14acb sub_14adb sub_14aeb sub_14afb sub_14b0b sub_14b1b sub_14b2b sub_14b3b sub_14b4b sub_14b52 sub_14b59 sub_14b60 sub_14b67 sub_14b71 sub_14ba7 sub_14bcf sub_14c09 sub_14c37 sub_14c59 sub_14c8d sub_14cbb sub_14cdd sub_14d0f sub_14d3d sub_14d5f sub_14d91 sub_14da1 sub_14db1 sub_14dc1 sub_14dd1 sub_14de4 sub_14df4 sub_14e04 sub_14e14 sub_14e24 sub_14e37 sub_14e47 sub_14e57 sub_14e67 sub_14e77 sub_14e8a sub_14e9a sub_14eaa sub_14eba sub_14eca sub_14edd sub_14f09 sub_14f27 sub_14f59 sub_14fc4 sub_14fc8 sub_14fec sub_15017 sub_15039 sub_15078 sub_150b5 sub_150fc sub_15106 sub_1515c sub_15160 sub_1518a sub_1518e sub_151b8 sub_151bc sub_151f2 sub_151f6 sub_1522c sub_1524a sub_15268 sub_1527b sub_1529a sub_152b3 sub_152c6 sub_152ca sub_152d6 sub_152de sub_1531c sub_1559c sub_155c0 sub_15686 sub_156aa sub_15772 sub_157d5 sub_15838 sub_15e7c sub_15e8a sub_15e91 sub_15f17 sub_15f25 sub_15f2c sub_16252 sub_177b2 sub_1782a sub_1787f sub_178d6 sub_178f1 \
anim00_130a2 anim01_130ef anim02_177b2 anim03_134d3 anim04_13674 anim05_134ca anim06_134d7 anim07_13158 anim08_131a4 anim09_131f1 anim0a_1323d anim0b_13286 anim0c_13288 anim0d_1345e anim0e_1346d anim0f_13474 anim10_13480 anim11_13485 anim12_1348a anim13_1341f anim14_134dc anim15_132df anim16_13674 anim17_1356e anim18_1339f anim19_133de anim1a_1346f sub_102ad sub_1041c sub_10555 sub_105cb sub_10e85 sub_1106f sub_111b1 sub_11204 sub_11439 sub_11792 sub_117ad sub_117d0 sub_1183d sub_118ad sub_11aa4 sub_11f47 sub_11f93 sub_1200a sub_1201d sub_12034 sub_120d1 sub_120ff sub_12199 sub_121b9 sub_121f6 sub_12250 sub_1227e sub_122c0 sub_122f3 sub_12312 sub_12345 sub_1237f sub_12388 sub_1242e sub_1246d sub_1250b sub_12543 sub_125a3 sub_125fa sub_12613 sub_12634 sub_1265b sub_12669 sub_1267b sub_1268d sub_126a9 sub_12709 sub_12829 sub_1287a sub_12d72 sub_12e16 sub_12e2d sub_12e79 sub_12e84 sub_1367c sub_1368c sub_1369c sub_13733 sub_13743 sub_13753 sub_14199 sub_141a7 sub_141b3 sub_141ba sub_141e0 sub_1424c sub_1434c sub_1547e sub_15485 sub_1549a sub_154a3 sub_16880 sub_12ef8 sub_1292f sub_12948 sub_12989 sub_128a9 sub_10d96 sub_10d9f sub_17512 sub_16528 sub_16546 sub_1686f sub_167ff sub_17912 sub_179a8 sub_108c8 sub_17337 sub_172d3 sub_11ba5 sub_10350 sub_1754c sub_17561 sub_10138 sub_1775d sub_12352 sub_177bb sub_100bb sub_128d1 sub_16440 sub_11080 sub_16563 sub_14207x sub_17a44 sub_17791 sub_101ac sub_124a9 sub_15f2cd sub_1797b sub_179fb sub_10dba sub_16807 sub_12ab8 sub_17749 sub_1774fd sub_10f5d sub_10fa0 sub_10130 sub_104a1 sub_1047c sub_103ca sub_1086fx sub_115d2 input_delivery"}
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
elif echo "$line" | grep -q "total cases=0 "; then echo "FAIL(empty: all cases escaped)[$(basename $l)]: $line"; rc=1
elif echo "$line" | grep -q "SKIPPED-PORT-NATIVE"; then echo "SKIP[$(basename $l)]: $line"
elif echo "$line" | grep -q "fail=0"; then echo "PASS[$(basename $l)]: $line"
else echo "FAIL[$(basename $l)]: $line"; rc=1
fi
done
exit $rc
