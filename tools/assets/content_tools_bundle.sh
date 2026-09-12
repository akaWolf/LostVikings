#!/usr/bin/env bash
# content_tools_bundle.sh DEST — the console content builder as it ships in the
# V2_ONLY release bundles (content-tools/): everything tools/assets/build_content.py
# needs, in the repo's own layout so its path logic (ROOT = two levels above the
# script) holds — the asset tools, the lvsc chain of tools/data, the mod package
# and its two repo inputs (the chunk role map, the DSL text of the six world
# scripts). No game data: the user runs it against their own DATA.DAT
# (README, "Console content").
set -eu
[ $# -eq 1 ] || { echo "usage: $0 DEST" >&2; exit 2; }
DEST=$1
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$DEST/tools/assets" "$DEST/tools/data" "$DEST/mods" "$DEST/assets_raw/lvs"
cp "$ROOT"/tools/assets/*.py "$DEST/tools/assets/"
cp "$ROOT"/tools/data/*.py "$DEST/tools/data/"
cp "$ROOT/mods/console_content.mod.json" "$DEST/mods/"
cp "$ROOT/assets_raw/chunk_map.json" "$DEST/assets_raw/"
cp "$ROOT"/assets_raw/lvs/*.lvsf "$DEST/assets_raw/lvs/"
cat > "$DEST/README.txt" <<'EOF'
Console content builder (SNES / Genesis material for the V2_ONLY engine).

From the directory that holds vikings and your DATA.DAT:

    python3 content-tools/tools/assets/build_content.py

writes content/ (~25 MB) there; vikings picks it up by itself. Needs python3.
See README.md, "Console content".
EOF
echo "content-tools: $(find "$DEST" -type f | wc -l) files -> $DEST"
