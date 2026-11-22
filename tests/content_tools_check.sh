#!/usr/bin/env bash
# tests/content_tools_check.sh — the console content pack does not depend on where it is built.
#
# tools/assets/build_content.py is documented to need DATA.DAT, the two console images and nothing
# else: `make content` in a checkout, `python3 content-tools/tools/assets/build_content.py` in a
# release bundle (README, "Console content"). A development tree holds more beside the tools — the
# open tree assets/ and the extraction assets_raw/chunks/ of the editor's setup, both ignored by git —
# and the build had come to read them: the converter took the PC palette chunks from the level
# headers of assets/, the pack's judge the archive's chunks from assets_raw/chunks/dec and its LZSS
# decoder from python/, which the bundle does not carry. In a fresh checkout and in a release bundle
# the build stopped in step 3; nothing in a development tree showed it.
#
# Three builds of the pack from the same DATA.DAT, images and translations:
#   here      tools/assets/build_content.py of this tree, as it stands (the reference);
#   checkout  the tracked files of this tree alone (git ls-files: what a clone holds), the build run
#             as `make content` runs it;
#   bundle    content-tools/ as tools/assets/content_tools_bundle.sh ships it in the release bundles,
#             the build run from the bundle directory by the README's command.
# Each must finish with the judge's verdict, and the engine-facing store of the two clean builds —
# .compiled/ and exe_static.bin — must equal the reference byte for byte. The bundle's own judge then
# reads the finished pack by itself (content_check.py --content content, what `make content-check`
# does) and must give the verdict of the build.
#
# Needs: DATA.DAT in the repository root, the SNES DE and Genesis images where build_content.py finds
# them (roms/), python3, git. Without the data: SKIP.
#   bash tests/content_tools_check.sh          (make content-tools-check)
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
if [ ! -f "$ROOT/DATA.DAT" ]; then echo "SKIP: no DATA.DAT in $ROOT"; exit 0; fi

# the inputs, by the builder's own search (the images by their SHA-256, the translations if there are any)
inputs=$(cd "$ROOT" && python3 - <<'EOF'
import contextlib, io, os, sys
sys.path.insert(0, os.path.join("tools", "assets"))
import build_content as BC
root = os.getcwd()
try:
    with contextlib.redirect_stderr(io.StringIO()):
        snes = BC.find_rom(None, BC.SNES_ROM_SHA256, "SNES DE", "snes-rom", root, False)
        gen = BC.find_rom(None, BC.GENESIS_ROM_SHA256, "Genesis", "genesis-rom", root, False)
except SystemExit:
    sys.exit(3)
bac = BC.find_bac(None, root)
print(snes); print(gen); print(bac[0] if bac else ""); print(bac[1] if bac else "")
EOF
)
rc=$?
if [ "$rc" -eq 3 ]; then echo "SKIP: the SNES DE / Genesis images were not found (roms/)"; exit 0; fi
if [ "$rc" -ne 0 ]; then echo "FAIL: the builder's search of its inputs failed"; exit 1; fi
SNES=$(printf '%s\n' "$inputs" | sed -n 1p); GEN=$(printf '%s\n' "$inputs" | sed -n 2p)
BAC_LOC=$(printf '%s\n' "$inputs" | sed -n 3p); BAC_KEYS=$(printf '%s\n' "$inputs" | sed -n 4p)

D=$(mktemp -d /tmp/lv_ctools_XXXXXX)
fail=0

stand() {   # stand <dir>: the user's files of a build directory — the archive, the images under roms/, the translations
    mkdir -p "$1/roms"
    ln -s "$ROOT/DATA.DAT" "$1/DATA.DAT"
    ln -s "$SNES" "$1/roms/$(basename "$SNES")"; ln -s "$GEN" "$1/roms/$(basename "$GEN")"
    if [ -n "$BAC_LOC" ]; then ln -s "$BAC_LOC" "$1/locale.strings"; ln -s "$BAC_KEYS" "$1/lv_snes_strings.json"; fi
}

# ---- the three trees ----
mkdir -p "$D/here"
stand "$D/checkout"
if ! (cd "$ROOT" && git ls-files -z -- tools python assets_raw src/sdl/v2_ds_layout.h exe_static.bin ds_static.bin Makefile \
        | xargs -0 cp --parents -t "$D/checkout") 2> "$D/checkout_copy.log"; then
    echo "FAIL: could not copy the tracked files (git ls-files) — see $D/checkout_copy.log"; exit 1
fi
stand "$D/bundle"
if ! bash "$ROOT/tools/assets/content_tools_bundle.sh" "$D/bundle/content-tools" > "$D/bundle_stage.log" 2>&1; then
    echo "FAIL: content_tools_bundle.sh failed — see $D/bundle_stage.log"; exit 1
fi
for p in checkout/assets checkout/assets_raw/chunks bundle/content-tools/assets bundle/content-tools/assets_raw/chunks; do
    if [ -e "$D/$p" ]; then echo "FAIL: the clean tree holds $p — not a clean tree"; exit 1; fi
done

# ---- the three builds, side by side ----
bac_arg=()
[ -n "$BAC_KEYS" ] && bac_arg=(--bac "$(dirname "$BAC_KEYS")")
( cd "$ROOT" && python3 tools/assets/build_content.py --data "$ROOT/DATA.DAT" --out "$D/here/content" \
    --snes-rom "$SNES" --genesis-rom "$GEN" "${bac_arg[@]}" > "$D/here/build.log" 2>&1; echo $? > "$D/here/rc" ) &
( cd "$D/checkout" && python3 tools/assets/build_content.py > build.log 2>&1; echo $? > rc ) &
( cd "$D/bundle" && python3 content-tools/tools/assets/build_content.py > build.log 2>&1; echo $? > rc ) &
wait

verdict() { grep -a -o 'content_check: [0-9]* error(s), [0-9]* warning(s)' "$1" | tail -1; }

for t in here checkout bundle; do
    rc=$(cat "$D/$t/rc" 2>/dev/null || echo "?")
    if [ "$rc" != 0 ]; then
        echo "FAIL $t: build_content.py exit $rc"
        tail -4 "$D/$t/build.log" | cut -c1-240 | sed 's/^/    /'
        fail=1
    fi
done
ref_verdict=$(verdict "$D/here/build.log")
if [ "$fail" -eq 0 ]; then
    for t in checkout bundle; do
        bad=0
        ndiff=$(diff -rq "$D/here/content/.compiled" "$D/$t/content/.compiled" 2>&1 | wc -l)
        [ "$ndiff" -eq 0 ] || { echo "  $t: .compiled differs from the reference in $ndiff record(s)"; bad=1; }
        cmp -s "$D/here/content/exe_static.bin" "$D/$t/content/exe_static.bin" || { echo "  $t: exe_static.bin differs from the reference"; bad=1; }
        v=$(verdict "$D/$t/build.log")
        [ "$v" = "$ref_verdict" ] || { echo "  $t: the judge says '$v', the reference '$ref_verdict'"; bad=1; }
        if [ "$bad" -eq 0 ]; then
            echo "PASS $t: the pack equals the reference ($(ls "$D/$t/content/.compiled" | wc -l) records, exe_static.bin; $v)"
        else
            echo "FAIL $t (see $D/$t/build.log)"; fail=1
        fi
    done
fi

# ---- the bundle's judge on the finished pack, by itself ----
if [ "$fail" -eq 0 ]; then
    ( cd "$D/bundle" && python3 content-tools/tools/assets/content_check.py --content content --snes-rom "$SNES" > judge.log 2>&1; echo $? > judge.rc )
    rc=$(cat "$D/bundle/judge.rc"); v=$(verdict "$D/bundle/judge.log")
    if [ "$rc" = 0 ] && [ "$v" = "$ref_verdict" ]; then
        echo "PASS judge: content_check.py of the bundle on its pack — $v"
    else
        echo "FAIL judge: exit $rc, '$v' (the build: '$ref_verdict') — see $D/bundle/judge.log"
        tail -3 "$D/bundle/judge.log" | cut -c1-240 | sed 's/^/    /'
        fail=1
    fi
fi

if [ "$fail" -eq 0 ]; then rm -rf "$D"; fi
exit "$fail"
