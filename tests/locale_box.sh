#!/usr/bin/env bash
# tests/locale_box.sh — the language banks reach the level dialogues.
#
# The regression this guards (2026-09-10): the transpiled executors of the
# canonical worlds read the string table directly and every level dialogue
# stayed English while the interpreted intro translated. The canon cannot see
# it — it runs without a language bank — so this bench runs the windowed
# V2_ONLY binary under SDL's dummy drivers with the content pack, jumps to the
# first level (V2_START_LEVEL=0: the opening line of the three vikings), lets
# V2_UI_BOXSHOT dump the frame of the first dialogue box and compares the box
# area (x 8..160, y 60..94 of the 320x200 frame, cropped as P6) with the
# golden crop of each language in tests/golden_locale/. The crop is
# deterministic across runs (the frame is the game's own, no presenter).
#
# Needs: a V2_ONLY build (BIN, default ./vikings — build: V2_ONLY=1 make),
# content/ with the language banks (make content, with the collection's
# locale.strings + lv_snes_strings.json at hand — otherwise SKIP), python3.
#   bash tests/locale_box.sh              # compare en ru ja against the goldens
#   UPDATE=1 bash tests/locale_box.sh     # rewrite the goldens from this build
#   LANGS="ru ja zh-CN" bash tests/locale_box.sh
set -u
export V2_CONTENT=0   # the pack is given explicitly below (the store and the image of ONE tree)
BIN=${BIN:-vikings}
LANGS=${LANGS:-en ru ja}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
GOLD=$ROOT/tests/golden_locale
CONTENT=${CONTENT:-$ROOT/content}
CROP="8 60 160 94"
if [ ! -x "$ROOT/$BIN" ]; then echo "FAIL: $BIN not built (V2_ONLY=1 make)"; exit 1; fi
if [ ! -f "$CONTENT/.compiled/0768.bin" ]; then echo "SKIP: no content pack with language banks at $CONTENT (make content with locale.strings + lv_snes_strings.json beside DATA.DAT)"; exit 0; fi
D=$(mktemp -d /tmp/lv_locale_XXXXXX)
ln -s "$ROOT/$BIN" "$D/vikings"; ln -s "$ROOT/ds_static.bin" "$D/ds_static.bin"; ln -s "$ROOT/vikings_keymap.cfg" "$D/vikings_keymap.cfg"
mkdir -p "$GOLD"
fail=0
for L in $LANGS; do
    rm -rf "$D/shots"; mkdir -p "$D/shots"
    printf 'language=%s\n' "$L" > "$D/v2_options.cfg"
    ( cd "$D" && env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy V2_FAST_VSYNC=1 V2_NOVSYNC=1 V2_START_LEVEL=0 \
        V2_ASSETS_DIR="$CONTENT/.compiled" V2_EXE_STATIC="$CONTENT/exe_static.bin" V2_UI_BOXSHOT="$D/shots" \
        timeout 300 ./vikings --max-frames=1500 > "$D/run_$L.log" 2>&1 )
    if [ ! -f "$D/shots/uibox_0.ppm" ]; then echo "FAIL: $L — no dialogue box within 1500 frames (see $D/run_$L.log)"; fail=1; continue; fi
    python3 - "$D/shots/uibox_0.ppm" "$D/box_$L.ppm" $CROP <<'EOF'
import sys
def read_ppm(p):
    d = open(p, "rb").read(); parts = []; i = 0
    while len(parts) < 4:
        while d[i:i+1].isspace(): i += 1
        if d[i:i+1] == b"#":
            while d[i:i+1] not in (b"\n", b""): i += 1
            continue
        j = i
        while not d[j:j+1].isspace(): j += 1
        parts.append(d[i:j]); i = j
    assert parts[0] == b"P6", "P6 expected"
    w, h = int(parts[1]), int(parts[2]); i += 1
    return w, h, d[i:i + w * h * 3]
w, h, px = read_ppm(sys.argv[1]); x0, y0, x1, y1 = map(int, sys.argv[3:7])
out = b"".join(px[(y * w + x0) * 3:(y * w + x1) * 3] for y in range(y0, y1))
open(sys.argv[2], "wb").write(b"P6\n%d %d\n255\n" % (x1 - x0, y1 - y0) + out)
EOF
    G="$GOLD/level0_$L.ppm"
    if [ "${UPDATE:-0}" = 1 ]; then cp "$D/box_$L.ppm" "$G"; echo "UPDATED: $G"; continue; fi
    if [ ! -f "$G" ]; then echo "FAIL: $L — no golden $G (UPDATE=1 to create)"; fail=1; continue; fi
    if cmp -s "$D/box_$L.ppm" "$G"; then echo "PASS: $L"; else echo "FAIL: $L — the dialogue box differs from $G (crop kept: $D/box_$L.ppm)"; fail=1; fi
done
if [ "${UPDATE:-0}" = 1 ] || [ $fail = 0 ]; then rm -rf "$D"; fi
[ $fail = 0 ] && echo "locale_box: all PASS" || echo "locale_box: FAIL"
exit $fail
