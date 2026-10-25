#!/usr/bin/env bash
# tests/content_regress.sh — regression checks of the content pack on the game binary: each case is a
# level of the pack played from a replay under SDL's dummy drivers with the engine's own traces, and an
# assertion on what the traces show. The static judge (tools/assets/content_check.py) and the spawn
# sweep (tests/content_smoke.sh) are the general checks; the cases here pin the bugs that were reported.
#
#   tmpl_column   issue #3: the sand column of TMPL (class 73, the level's spawn 2 at (456,448)) must
#                 stand while the vikings walk up to it — it holds the wall the player crosses later.
#                 The column reads the level's initial switch bits (head +0x12 -> ds:25C5) when it
#                 spawns: with the SNES head's 0002 it stands, with the donor PC level's 0000 (the
#                 converter's word before a081abb) it crumbled as the level began and the wall was
#                 passable. The run is tests/content/tmpl_walk.inp for 600 frames with V2_VIK_DBG=3
#                 (every object slot every frame: `[n]cs=.. fl=.. pc=.. anim=<class> .. x=.. y=..`);
#                 the class-73 object must appear (the vikings reach it around frame 50) and its y
#                 must never leave 448 (447 on its spawn frame): a crumbled column falls.
#
# Needs: the game build (BIN, default ./vikings — make), content/ (make content).
#   bash tests/content_regress.sh
#   CONTENT=dir BIN=vikings bash tests/content_regress.sh
set -u
export V2_CONTENT=0   # the pack is given explicitly (V2_ASSETS_DIR / V2_EXE_STATIC)
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${BIN:-vikings}
CONTENT=${CONTENT:-$ROOT/content}
if [ ! -x "$ROOT/$BIN" ]; then echo "FAIL: $BIN not built (make)"; exit 1; fi
if [ ! -d "$CONTENT/.compiled" ] || [ ! -f "$CONTENT/exe_static.bin" ]; then
    echo "SKIP: no content pack at $CONTENT (make content)"; exit 0
fi
D=$(mktemp -d /tmp/lv_cregress_XXXXXX)
fail=0

stand() {   # stand <dir> <snes_balance>: the run directory of one case (the game finds DATA.DAT beside the binary)
    mkdir -p "$1"
    ln -s "$ROOT/$BIN" "$1/vikings"; ln -s "$ROOT/ds_static.bin" "$1/ds_static.bin"; ln -s "$ROOT/vikings_keymap.cfg" "$1/vikings_keymap.cfg"
    printf 'parallax=1\nscenes=1\nconsole_finale=0\nsnes_balance=%s\n' "$2" > "$1/v2_options.cfg"
}

# ---- tmpl_column ----
R="$D/tmpl_column"; stand "$R" 0
( cd "$R" && env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy V2_FAST_VSYNC=1 V2_NOVSYNC=1 \
    V2_START_LEVEL=50 V2_VIK_DBG=3 V2_ASSETS_DIR="$CONTENT/.compiled" V2_EXE_STATIC="$CONTENT/exe_static.bin" \
    timeout -k 5 300 ./vikings --replay-input="$ROOT/tests/content/tmpl_walk.inp" --max-frames=600 > run.log 2>&1 )
rc=$?
lines=$(grep -a -c 'anim=0073 ' "$R/run.log")
ys=$(grep -a -o 'anim=0073 as=[0-9A-F]* apc=[0-9A-F]* x=[0-9-]* y=[0-9-]*' "$R/run.log" | sed 's/.* y=//' | sort -n | uniq | tr '\n' ' ')
ymax=$(grep -a -o 'anim=0073 as=[0-9A-F]* apc=[0-9A-F]* x=[0-9-]* y=[0-9-]*' "$R/run.log" | sed 's/.* y=//' | sort -n | tail -1)
if [ "$rc" -ne 0 ]; then echo "FAIL tmpl_column: exit $rc (see $R/run.log)"; fail=1
elif [ "$lines" -lt 100 ]; then echo "FAIL tmpl_column: the column (class 73) is in $lines trace lines, expected hundreds — not spawned, or gone (see $R/run.log)"; fail=1
elif [ "${ymax:-0}" -gt 448 ]; then echo "FAIL tmpl_column: the column fell — y reached $ymax (y values: $ys)"; fail=1
else echo "PASS tmpl_column: the column stands at y 448 through $lines trace lines (y values: $ys)"
fi

if [ "$fail" -eq 0 ]; then rm -rf "$D"; fi
exit "$fail"
