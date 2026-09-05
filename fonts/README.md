# Fonts of the localisation (UX stage 6)

- `PressStart2P-Regular.ttf` — Press Start 2P (Cody "CodeMan38" Boisclair), SIL Open
  Font License 1.1 (`OFL-PressStart2P.txt`). Rasterised at its native 8 px by
  `tools/assets/ps2p_glyphs.py` into `tools/assets/ps2p_glyphs.json`: the 8x8 face of
  the Latin and Cyrillic language banks (build_locale.py).
- GNU Unifont (`unifont.otf`, GPLv2+ with the font-embedding exception / SIL OFL 1.1) is
  NOT bundled: `tools/assets/unifont_glyphs.py` reads it from the nixpkgs package
  (`nix-build '<nixpkgs>' -A unifont`, `share/fonts/opentype/unifont.otf`) and the
  rasterised 16 px bitmaps it needs live in `tools/assets/unifont16_glyphs.json` — the
  CJK language banks are built from that cache alone.
