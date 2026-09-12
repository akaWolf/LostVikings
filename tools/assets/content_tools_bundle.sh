#!/usr/bin/env bash
# content_tools_bundle.sh DEST — the console content builder as it ships in the
# V2_ONLY release bundles (content-tools/): everything tools/assets/build_content.py
# needs, in the repo's own layout so its path logic (ROOT = two levels above the
# script) holds — the asset tools and converters, the lvsc chain of tools/data,
# the two repo inputs of the archive conversion (the chunk role map, the DSL
# text of the six world scripts) and the static images the tools read from the
# repo root. No game data and no console data: the user runs it against their
# own DATA.DAT and their own SNES DE / Genesis images (README, "Console content").
set -eu
[ $# -eq 1 ] || { echo "usage: $0 DEST" >&2; exit 2; }
DEST=$1
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$DEST/tools/assets" "$DEST/tools/data" "$DEST/assets_raw/lvs"
cp "$ROOT"/tools/assets/*.py "$DEST/tools/assets/"
cp "$ROOT"/tools/assets/*.json "$DEST/tools/assets/"   # the converters' tables: the BAC translations cache, the glyph tables, the effect map, the SMD map
cp "$ROOT"/tools/data/*.py "$DEST/tools/data/"
cp "$ROOT"/tools/data/*.json "$DEST/tools/data/"     # the op tables of the script disassembler (tools/data/disasm.py load_draft)
cp "$ROOT/assets_raw/chunk_map.json" "$DEST/assets_raw/"
cp "$ROOT"/assets_raw/lvs/*.lvsf "$DEST/assets_raw/lvs/"
cp "$ROOT/exe_static.bin" "$ROOT/ds_static.bin" "$DEST/"   # level_render / texts_exe read them from ROOT
cat > "$DEST/README.txt" <<'EOF'
Console content builder (SNES / Genesis material for the V2_ONLY engine).

From the directory that holds vikings, your DATA.DAT and your two console
images (the SNES DE ROM and the Genesis ROM — any file name, they are found by
their SHA-256):

    python3 content-tools/tools/assets/build_content.py

writes content/ (~25 MB) there; vikings picks it up by itself. Needs python3.
For the twelve languages put strings/locale.strings and lv_snes_strings.json
from the Blizzard Arcade Collection's assets/ folder beside DATA.DAT as well
(or pass --bac <that assets folder>); without them the pack is English only.
See README.md, "Console content". The SNES sound option needs this pack too; the
SC55 and MT32 options need the modules' ROM images in roms/sc55/ and roms/mt32/
beside vikings — README.md, "Sound options".
EOF
echo "content-tools: $(find "$DEST" -type f | wc -l) files -> $DEST"
