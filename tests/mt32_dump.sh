#!/usr/bin/env bash
# UX stage 11 regression: the MT-32 world (the game's MT-32 driver in the AIL interpreter) and the
# MIDI lane of the audible driver, judged on their MIDI streams — no ROMs, no ears needed.
#
# The MT-32 world lives on the audio thread and is paced by the audio clock, so this runs the
# WINDOWED V2_ONLY binary under SDL's dummy video/audio in real time for ~30 s of tests/replays/level1.inp
# (the game boots, the title plays, level 1 starts) with SOUND=MT32 in a scratch options file, and
# collects two dumps from the one run:
#   V2_MT32_DUMP  — the MT-32 driver's MPU-401 stream (MT-32 reset, the 64 timbres, the MT-32 arrangement
#                   01EC of level 1's track) — tools/assets/midi_check.py mt32
#   V2_MIDI_DUMP  — the FM driver's channel messages (the lane) against the FM arrangement 01EB — midi_check.py lane
#
#   BIN=vikings ./tests/mt32_dump.sh      (build: V2_ONLY=1 RELEASE=1 make; the assets/ tree or DATA.DAT must be in place)
set -u
export V2_CONTENT=0   # the canon content: never the content/ pack of the repo root (v2_main.cpp)
cd "$(dirname "$0")/.."
BIN=${BIN:-vikings}
if [ ! -x "./$BIN" ]; then echo "FAIL: $BIN not built (V2_ONLY=1 RELEASE=1 make)"; exit 1; fi
OUT=${OUT:-$(mktemp -d /tmp/lv_mt32_XXXXXX)}
D="$OUT/cwd"; mkdir -p "$D"
for f in *; do [ "$f" = v2_options.cfg ] || ln -sfn "$PWD/$f" "$D/$f"; done   # the tree, minus the user's options
printf 'sound=3\n' > "$D/v2_options.cfg"
SECS=${SECS:-36}
( cd "$D" && V2_MT32_DUMP="$OUT/mt32.mid" V2_MIDI_DUMP="$OUT/lane.mid" SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    timeout -s TERM "$SECS" "./$BIN" --replay-input=tests/replays/level1.inp --max-frames=100000 > "$OUT/run.log" 2>&1 )
fail=0
if [ ! -s "$OUT/mt32.mid" ]; then echo "FAIL: no MT-32 dump (see $OUT/run.log)"; fail=1; fi
if [ ! -s "$OUT/lane.mid" ]; then echo "FAIL: no lane dump (see $OUT/run.log)"; fail=1; fi
grep -q "V2-MT32: 0x215 preload" "$OUT/run.log" || { echo "FAIL: the MT-32 world never preloaded (see $OUT/run.log)"; fail=1; }
if grep -q "AIL-MT32: fn @" "$OUT/run.log"; then echo "FAIL: the MT-32 driver faulted:"; grep "AIL-MT32: fn @" "$OUT/run.log" | head -3; fail=1; fi
if [ $fail = 0 ]; then
    echo "== MT-32 world vs assets/music/01EC (level 1, the MT-32 set; 01EB = the FM set must not match)"
    python3 tools/assets/midi_check.py mt32 "$OUT/mt32.mid" assets/music/01EC.derived.mid assets/music/01EB.derived.mid || fail=1
    echo "== the lane vs assets/music/01EB (the FM driver's own messages)"
    python3 tools/assets/midi_check.py lane "$OUT/lane.mid" assets/music/01EB.derived.mid || fail=1
fi
[ $fail = 0 ] && echo "PASS: mt32_dump ($OUT)" || echo "FAILED: mt32_dump ($OUT)"
exit $fail
