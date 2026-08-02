#!/usr/bin/env bash
# #61 native AIL smoke: run the V2_ONLY binary with the interpreted Miles
# driver enabled over the level1 replay and assert the driver-health
# invariants. Needs ./vikings built with V2_ONLY=1.
#
#   ./tests/native_ail_check.sh [max_frames] [timeout_s]
#
# Asserts:
#   - the driver boots (V2-AIL: boot OK)
#   - music registers through fn97 (V2-AIL-START with si=0)
#   - zero interpreter faults
#   - the blob slot counter never exceeds 5 (music + 4 SFX = DOS model)
#   - OPL register writes actually flow (trace lines)
set -u
cd "$(dirname "$0")/.."

if [ ! -x ./vikings ]; then
echo "FAIL: ./vikings not built (V2_ONLY=1 make)"; exit 2
fi

MAX_FRAMES="${1:-1200}"
TIMEOUT_S="${2:-300}"
LOG=$(mktemp /tmp/native_ail_check.XXXXXX.log)
TRACE=$(mktemp /tmp/native_ail_check.XXXXXX.opl)

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy V2_NATIVE_AIL=1 V2_OPL_TRACE="$TRACE" \
timeout "$TIMEOUT_S" ./vikings --replay tests/replays/level1.inp \
--max-frames "$MAX_FRAMES" > "$LOG" 2>&1
RC=$?   # 124 (timeout budget) is fine — the assertions below are the verdict

fail=0
if ! grep -q 'V2-AIL: boot OK' "$LOG"; then
echo "FAIL: driver did not boot"; fail=1
fi
if ! grep -q 'V2-AIL-START: .* si=0 ' "$LOG"; then
echo "FAIL: music never registered (no fn97 start with si=0)"; fail=1
fi
faults=$(grep -c 'AIL-INTERP FAULT' "$LOG")
if [ "$faults" -ne 0 ]; then
echo "FAIL: $faults interpreter faults"; grep 'AIL-INTERP FAULT' "$LOG" | head -3; fail=1
fi
if grep -qE 'cnt=000[6-9A-F]|cnt=00[1-9A-F][0-9A-F]' "$LOG"; then
echo "FAIL: blob slot counter exceeded 5 (slot leak)"; fail=1
fi
opl=$(wc -l < "$TRACE" 2>/dev/null || echo 0)
if [ "$opl" -lt 1000 ]; then
echo "FAIL: only $opl OPL writes traced (expected >=1000)"; fail=1
fi

if [ "$fail" -eq 0 ]; then
echo "PASS: native AIL — boot+music+SFX, faults=0, slot cap ok, opl_writes=$opl (run rc=$RC)"
rm -f "$LOG" "$TRACE"
exit 0
fi
echo "logs: $LOG $TRACE"
exit 1
