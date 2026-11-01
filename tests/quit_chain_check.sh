#!/usr/bin/env bash
# tests/quit_chain_check.sh — the quit to DOS of the game build. The original's loc_10e35 (seg000 0E35),
# the way out of the F10 / Alt-X quit (sub_10350), of the quit prompt's "End game" (sub_103ca) and of
# the menu's Quit (sub_1434c), closes the archive, restores the VGA mode (sub_1686f: [92FF] = 0 unless
# [9300] == FF) and the INT 24h vector (sub_1292f) before INT 21h/4C. The port's quit sites went
# straight to the exit, so a quit's end state kept [92FF] = 1 (the VGA init's value) where the original
# leaves 0; the port's chain is v2_quit_to_dos_10e35 (v2_vm.cpp), which prints a V2-QUIT line.
#
# Two replays of the canon on the game binary, each under SDL's dummy drivers with V2_SAVE_STATE, which
# writes the full state image at the clean exit (headless_golden_dump):
#   title_f10    tests/replays/synth_title_f10.inp — F10 on the title screen (frame 400): the direct
#                quit (game mode 8000h) of v2_phase_post_flip3's PART B;
#   prompt_quit  tests/replays/level5.inp — level 5 played to its end, then Alt-X (frame 3103), LEFT,
#                RETURN (frame 3139): the quit prompt's "End game" — the post-loop check of
#                word_28814 & 2 (synth_quit_f10 answers the prompt with Y, which it ignores: it stays
#                open to the frame budget, so that replay is no quit).
# Each run must end by the quit (exit code 0, a V2-QUIT line, no max-frames exit) and the DS block of
# its state image must read [92FF] = 00 with [9300] = 00 — the gate: the port's INT 10h/0F returns 0
# (v2_vga_init), so sub_1686f's write happens; a state with [9300] = FF would not test it.
#
# Needs: the game build (BIN, default ./vikings — make), DATA.DAT beside it, python3.
#   bash tests/quit_chain_check.sh
set -u
export V2_CONTENT=0   # the canon content: never the content/ pack of the repo root (v2_main.cpp)
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${BIN:-vikings}
if [ ! -x "$ROOT/$BIN" ]; then echo "FAIL: $BIN not built (make)"; exit 1; fi
if [ ! -f "$ROOT/DATA.DAT" ]; then echo "SKIP: no DATA.DAT beside $BIN"; exit 0; fi
D=$(mktemp -d /tmp/lv_quit_XXXXXX)
fail=0

run_case() {   # run_case <name> <replay> <frame cap>: the replay on a stand of its own, the log and the state image there
    local R="$D/$1"
    mkdir -p "$R"
    ln -s "$ROOT/$BIN" "$R/vikings"; ln -s "$ROOT/ds_static.bin" "$R/ds_static.bin"; ln -s "$ROOT/exe_static.bin" "$R/exe_static.bin"
    ln -s "$ROOT/vikings_keymap.cfg" "$R/vikings_keymap.cfg"; ln -s "$ROOT/DATA.DAT" "$R/DATA.DAT"
    printf '# options\n' > "$R/v2_options.cfg"
    # V2_VIEW_W=320: the canon's replays are recordings at 320 px — at the wide view the intro and the
    # level begin later and the in-game keys of synth_quit_f10 fall elsewhere (tests/wide_canon.sh)
    ( cd "$R" && env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy V2_FAST_VSYNC=1 V2_NOVSYNC=1 V2_VIEW_W=320 V2_SAVE_STATE="$R/end.v2s" \
        timeout -k 5 300 ./vikings --replay-input="$ROOT/$2" --max-frames="$3" > run.log 2>&1; echo $? > rc.txt )
}

check_case() {   # check_case <name>
    local name=$1 R="$D/$1" rc quit maxf st bad=0
    rc=$(cat "$R/rc.txt")
    quit=$(grep -a -c '^V2-QUIT: loc_10e35' "$R/run.log")
    maxf=$(grep -a -c 'max-frames reached' "$R/run.log")
    st=$(python3 -I - "$R/end.v2s" <<'EOF'
import struct, sys
try:
    d = open(sys.argv[1], 'rb').read()
except OSError:
    print("no-state"); sys.exit(0)
if d[:4] != b"V2S1": print("bad-header"); sys.exit(0)
n = struct.unpack_from('<I', d, 4)[0]; off = 8; ds = None     # "V2S1", u32 blocks, {tag[4], u32 len, bytes}
for _ in range(n):
    tag = d[off:off + 4]; ln = struct.unpack_from('<I', d, off + 4)[0]
    if tag == b"DS  ": ds = d[off + 8:off + 8 + ln]
    off += 8 + ln
if ds is None or len(ds) < 0x9301: print("no-ds"); sys.exit(0)
print(f"{ds[0x92FF]:02X} {ds[0x9300]:02X}")
EOF
)
    [ "$rc" -eq 0 ] || { echo "  $name: exit $rc"; bad=1; }
    [ "$quit" -ge 1 ] || { echo "  $name: no V2-QUIT line — the quit did not run the loc_10e35 chain"; bad=1; }
    [ "$maxf" -eq 0 ] || { echo "  $name: the run ended by max-frames, not by the quit"; bad=1; }
    case "$st" in
        "00 00") ;;
        no-state|bad-header|no-ds) echo "  $name: no state image at the exit ($st)"; bad=1 ;;
        *) echo "  $name: the end state reads [92FF] [9300] = $st, expected 00 00 (sub_1686f's write at the quit)"; bad=1 ;;
    esac
    if [ "$bad" -eq 0 ]; then
        echo "PASS $name: the quit ran the chain, the end state reads [92FF] [9300] = $st"
    else
        echo "FAIL $name (see $R/run.log)"; fail=1
    fi
}

run_case title_f10 tests/replays/synth_title_f10.inp 1200 & run_case prompt_quit tests/replays/level5.inp 3400 & wait
check_case title_f10
check_case prompt_quit
if [ "$fail" -eq 0 ]; then rm -rf "$D"; fi
exit "$fail"
