#!/usr/bin/env bash
# tests/sound_header_check.sh — the DATA.DAT sound header (issue #3): the game build runs the Sound
# Blaster Pro FM configuration whatever the original SETUP wrote into record 0 of the archive
# (v2_open_data_dat_12989 under V2_ONLY). The header's six card words (+6 sound card, +8 music set,
# +A IO, +C IRQ, +E DMA, +10 DRQ) choose the driver chunk and the timbre bank; a file set up for
# "no sound card / no music" (sound 0, music 0 — the reporter's) loaded the PC-speaker driver with the
# music set 0: no timbres, the driver's state words DEAD, not one OPL register written — and the smart
# bomb's effect, whose timbres never came, left the TMPL script waiting on it for ever.
#
# Three copies of DATA.DAT in a scratch directory, each played through tests/replays/level1.inp for
# 1200 frames under SDL's dummy drivers (the title screens and the level start: the music registered
# twice, some twenty effects started):
#   as_is     the file unchanged — prints no V2-STARTUP header line;
#   nosound   record 0 set to sound 0 / music 0 — prints the header line naming "sound card 0, music 0";
#   io240     record 0 with the IO port 240h — prints the header line naming "IO 240h".
# Every copy must boot the FM driver (V2-AIL-MUSIC-PLAY blob=16263 bank=6006: SBPFM.ADV 0x1CC with the
# FM timbre bank), install timbres (the V2-AIL-TIMBRES lines count what a start loads into the driver's
# cache: the music's bank, then the effects' — a start that finds its timbres cached, a repeated effect
# or one sharing the music's instruments, loads none; the file set up for no sound card loaded none at
# all: at least 20 over the run — the as-is file loads 38 — and at least three effect starts that
# loaded some), show no DEAD driver
# state and write OPL registers (the V2_OPL_TRACE file, at least 1000 lines).
#
# Needs: the game build (BIN, default ./vikings — make), DATA.DAT beside it, python3.
#   bash tests/sound_header_check.sh
set -u
export V2_CONTENT=0   # the canon content: never the content/ pack of the repo root (v2_main.cpp)
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${BIN:-vikings}
FRAMES=${FRAMES:-1200}
if [ ! -x "$ROOT/$BIN" ]; then echo "FAIL: $BIN not built (make)"; exit 1; fi
if [ ! -f "$ROOT/DATA.DAT" ]; then echo "SKIP: no DATA.DAT beside $BIN"; exit 0; fi
D=$(mktemp -d /tmp/lv_sndhdr_XXXXXX)
fail=0

make_copy() {   # make_copy <name> <python statement over the record-0 words w[0..15]; "pass" = as it is>
    local R="$D/$1"; mkdir -p "$R"
    ln -s "$ROOT/$BIN" "$R/vikings"; ln -s "$ROOT/ds_static.bin" "$R/ds_static.bin"; ln -s "$ROOT/exe_static.bin" "$R/exe_static.bin"
    ln -s "$ROOT/vikings_keymap.cfg" "$R/vikings_keymap.cfg"; ln -s "$ROOT/tests" "$R/tests"
    printf '# options\n' > "$R/v2_options.cfg"
    python3 - "$ROOT/DATA.DAT" "$R/DATA.DAT" "$2" <<'EOF' || { echo "FAIL: cannot patch DATA.DAT"; exit 2; }
import struct, sys
src, dst, patch = sys.argv[1], sys.argv[2], sys.argv[3]
d = bytearray(open(src, 'rb').read())
off = struct.unpack_from('<I', d, 0)[0]            # the offset table's entry 0 = record 0, read raw (seg000 29BB)
w = list(struct.unpack_from('<16H', d, off))
assert w[10] == 0x6969, f"record 0 magic {w[10]:04X} at {off:#x}: not a DATA.DAT header"
exec(patch)
struct.pack_into('<16H', d, off, *w)
open(dst, 'wb').write(d)
EOF
}

run_copy() {   # run_copy <name>: the level-1 replay on the copy, the log and the OPL trace in its directory
    local R="$D/$1"
    ( cd "$R" && env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy V2_OPL_TRACE="$R/opl.txt" \
        timeout -k 5 300 ./vikings --replay-input=tests/replays/level1.inp --max-frames="$FRAMES" > run.log 2>&1; echo $? > rc.txt )
}

check_copy() {   # check_copy <name> <text the V2-STARTUP header line must carry; "" = no such line>
    local name=$1 expect=$2 R="$D/$1" rc bad=0 hdr music timbres loaded effects dead opl
    rc=$(cat "$R/rc.txt")
    hdr=$(grep -a -c 'V2-STARTUP: the DATA.DAT header' "$R/run.log")
    music=$(grep -a -c 'V2-AIL-MUSIC-PLAY: .*blob=16263 bank=6006' "$R/run.log")
    timbres=$(grep -a -c '^V2-AIL-TIMBRES: ' "$R/run.log")
    loaded=$(grep -a -o '^V2-AIL-TIMBRES: [0-9]* installed' "$R/run.log" | awk '{s += $2} END {print s + 0}')
    effects=$(grep -a '^V2-AIL-TIMBRES: ' "$R/run.log" | grep -a -v ' 0 installed\| si=0$' | wc -l)
    dead=$(grep -a -c 'DEAD:DEAD' "$R/run.log")
    opl=$(wc -l < "$R/opl.txt" 2>/dev/null || echo 0)
    [ "$rc" -eq 0 ] || { echo "  $name: exit $rc"; bad=1; }
    if [ -n "$expect" ]; then
        grep -a -q "V2-STARTUP: the DATA.DAT header .*$expect" "$R/run.log" || { echo "  $name: no V2-STARTUP header line naming \"$expect\""; bad=1; }
    else
        [ "$hdr" -eq 0 ] || { echo "  $name: a V2-STARTUP header line for the file as it is"; bad=1; }
    fi
    [ "$music" -ge 1 ] || { echo "  $name: the FM driver did not play the music (no V2-AIL-MUSIC-PLAY with blob=16263 bank=6006)"; bad=1; }
    [ "$timbres" -ge 10 ] || { echo "  $name: only $timbres V2-AIL-TIMBRES lines (sequence starts), expected at least 10"; bad=1; }
    [ "$loaded" -ge 20 ] || { echo "  $name: only $loaded timbres loaded over the run, expected the music's and the effects' (the frozen smart bomb's world loaded none)"; bad=1; }
    [ "$effects" -ge 3 ] || { echo "  $name: only $effects effect starts loaded timbres, expected at least 3"; bad=1; }
    [ "$dead" -eq 0 ] || { echo "  $name: $dead DEAD driver states"; bad=1; }
    [ "$opl" -ge 1000 ] || { echo "  $name: only $opl OPL register writes traced, expected at least 1000"; bad=1; }
    if [ "$bad" -eq 0 ]; then
        echo "PASS $name: header lines $hdr, music $music, starts $timbres, timbres loaded $loaded ($effects effect starts with some), DEAD $dead, OPL writes $opl"
    else
        echo "FAIL $name (see $R/run.log)"; fail=1
    fi
}

make_copy as_is 'pass'
make_copy nosound 'w[3] = 0; w[4] = 0'
make_copy io240 'w[5] = 0x240'
run_copy as_is & run_copy nosound & run_copy io240 & wait
check_copy as_is ''
check_copy nosound 'selects sound card 0, music 0'
check_copy io240 'IO 240h'
if [ "$fail" -eq 0 ]; then rm -rf "$D"; fi
exit "$fail"
