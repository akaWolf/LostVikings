#!/usr/bin/env python3
"""Stage 5.0: fact-based chunk role map.

Inputs:
  /tmp/s5_chunk_trace.txt  lines: {C|R} <id-hex> <destclass> <off> <size> <RA>
                           (written by v2_read_chunk/v2_read_raw_chunk under
                           V2_CHUNK_TRACE across the full replay canon)
  vikings_headless         for addr2line RA resolution
  assets_raw/manifest.json for sizes/hashes of all 535 chunks

Output:
  assets_raw/chunk_map.json  per-id: readers (caller fn, dest class/off),
                             role (derived from caller+dest facts), sizes.

Role rules are FACTS from the engine sources (каждое правило — конкретный
загрузчик в v2_vm.cpp), not guesses; unseen chunks stay role="unreferenced".
"""
import json, os, re, subprocess, sys, collections

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TRACE = sys.argv[1] if len(sys.argv) > 1 else "/tmp/s5_chunk_trace.txt"
BIN = os.path.join(ROOT, "vikings_headless")
A2L = "/nix/store/8v97ngkcpfzgghwnnr7fsz33p2x22gy9-gcc-wrapper-14.3.0/bin/addr2line"

recs = []
for ln in open(TRACE):
    p = ln.split()
    if len(p) != 6: continue
    recs.append((p[0], int(p[1], 16), p[2], int(p[3]), int(p[4]), p[5]))
print(f"trace records: {len(recs)}")

ras = sorted({r[5] for r in recs})
out = subprocess.run([A2L, "-e", BIN, "-f", "-C"] + ras,
                     capture_output=True, text=True).stdout.splitlines()
ra_fn = {}
for i, ra in enumerate(ras):
    fn = out[2 * i] if 2 * i < len(out) else "??"
    fn = re.sub(r'\(.*\)', '', fn)
    ra_fn[ra] = fn
print(f"unique callers: {len(set(ra_fn.values()))}: {sorted(set(ra_fn.values()))}")

# --- role from (caller, dest) facts --------------------------------------
def role_of(caller, cls, off, kind):
    if cls == "sound":
        return "sound_driver_or_bank"          # v2_sound_init loader chain
    if cls == "animdata":
        return "level_script"                  # VM bytecode template/level
    if cls == "sprite":
        return "sprite_gfx"                    # sprite loader 5320
    if cls == "chunk":
        # kind R = sub_10cd8 raw screens; kind C = LZSS anim/tile banks
        if kind == "R":
            return "screen_gfx"
        return "anim_bank"
    if cls == "tilegfx":
        return "tileset"                       # level tile graphics (11204 step 1)
    if cls == "gsmask":
        # 8 bytes per 64B tile (off>>3): per-pixel transparency bits for the
        # masked-tile pass (v2_render_tile_masked) — NOT collision data.
        return "tile_masks"
    if cls == "tilemap":
        return "tilemap"                       # level map (11204 step 3)
    if cls == "gstiledata":
        return "bg_tileset"                    # background tile data (11204 step 4)
    if cls == "ds":
        if off == 0x687D: return "level_password_table"
        if off == 0x25B3: return "level_header_stripe"   # per-level DS stripe
        if off == 0x2193: return "transition_text"
        if off is not None and 0x7F02 <= off < 0x8202: return "palette"
        if off == 0x2E7D: return "level_palette_chunks"
        if off is not None and 0x317D <= off <= 0x497D and (off - 0x317D) % 0x300 == 0:
            return "builtin_palette"            # startup bank: chunks 5..13 -> 9x768 DS palettes (12ab8)
        if off == 0x507D: return "hud_item_gfx"        # DS_HUD_ITEM_GFX: item_id*256, 4 planes x 16 rows x 4 bytes (12ab8 startup)
        return f"ds@{off:04X}"
    if kind == "R":
        return "raw_screen_or_font"            # 10cd8 raw reads (screens)
    return "ext"

per_id = collections.defaultdict(lambda: {"readers": [], "roles": set()})
for kind, cid, cls, off, size, ra in recs:
    e = per_id[cid]
    caller = ra_fn.get(ra, "??")
    r = (kind, caller, cls, off)
    key = f"{kind} {caller} {cls} {off}"
    if key not in {f"{a} {b} {c} {d}" for a, b, c, d in e["readers"]}:
        e["readers"].append(r)
    e["roles"].add(role_of(caller, cls, off if off >= 0 else None, kind))

# --- static layer: roles derivable from engine formulas (facts, not guesses)
# sound driver: chunk = 0x1C7 + sound_card (v2_vm.cpp 6336 chain)
# music banks:  chunk = 0x20C + music_card and 0x207 + music_card
# XMID tracks:  chunk = track_tbl[track] + music_card (v2_music_load_1775d);
#   track_tbl @A384 (11 words) = 1EE 1E9 1D0 1D5 1DA 1DF 1F3 1E4 1F8 1FD 202
#   (from tests/golden_states — the static DS image), music_card 0..4 spans
#   every id 1D0..206; drivers span 1C7..1CF (all present in DATA.DAT).
STATIC_ROLES = {}
for sc in range(9):
    STATIC_ROLES[0x1C7 + sc] = f"sound_driver[card={sc}]"
for base in (0x1D0,0x1D5,0x1DA,0x1DF,0x1E4,0x1E9,0x1EE,0x1F3,0x1F8,0x1FD,0x202):
    for mc in range(5):
        STATIC_ROLES[base + mc] = f"xmid_track[base={base:03X},card={mc}]"
for mc in range(5):
    STATIC_ROLES[0x207 + mc] = f"sound_bank_207[card={mc}]"
    STATIC_ROLES[0x20C + mc] = f"sound_bank_20c[card={mc}]"
# screens hard-referenced by v2_load_level_data (flag paths not hit by the canon)
STATIC_ROLES[0x211] = "screen_hud_only"
STATIC_ROLES[0x213] = "screen_intro2"
# signature facts for canon-unreachable content (verified structurally):
# 0x212: comp == [u16 ps][ps*4] EXACTLY -> raw 344x176 screen (the 211/213 pair's sibling)
# 0x215: FORM/XDIR header -> an XMID sequence outside the music table (jingle)
# 48-byte chunks with every byte <= 0x3F -> 16-color palettes
STATIC_ROLES[0x212] = "screen_by_sig"
STATIC_ROLES[0x215] = "xmid_extra"
for pc in (0x0AD, 0x0F1, 0x1B8, 0x1BE):
    STATIC_ROLES[pc] = "palette16_by_sig"

man = json.load(open(os.path.join(ROOT, "assets_raw/manifest.json")))
entries = {e["id"]: e for e in man["entries"]}

out_map = {}
for cid in sorted(entries):
    e = entries[cid]
    m = per_id.get(cid)
    roles = sorted(m["roles"]) if m else []
    if cid in STATIC_ROLES:
        base_static = STATIC_ROLES[cid].split('[')[0]
        if not any(r.startswith(base_static) or r.startswith('sound_driver_or_bank')
                   or r.startswith('xmid') for r in roles):
            roles.append(STATIC_ROLES[cid])
        else:
            roles = [STATIC_ROLES[cid] if r == 'sound_driver_or_bank' else r for r in roles]
    if not roles:
        roles = ["unreferenced"]
    kinds = {k for k, _, _, _ in m["readers"]} if m else set()
    container = "raw" if kinds == {"R"} else ("lzss" if kinds else "lzss?")
    if cid in (0x211, 0x212, 0x213):
        container = "raw"   # static screens: the 10cd8 (raw) path / exact-fit signature
    out_map[f"{cid:04X}"] = {
        "comp_size": e["comp_size"], "decomp_size": e["decomp_size"],
        "container": container,
        "roles": roles,
        "readers": [f"{k} {fn} {cls}@{off}" for k, fn, cls, off in m["readers"]] if m else [],
    }
# --- enrichment: level_header stripes carry width/height and the three
# resource chunk ids (verified offsets 0x29/0x2B/0x2E/0x30/0x32 = DS
# 25DC/25DE/25E1/25E3/25E5 minus the 25B3 stripe base; 8/8 spot-check OK).
def _w16(b, o): return b[o] | (b[o+1] << 8)
for cid_hex, v in out_map.items():
    if v["roles"][0] != "level_header_stripe": continue
    d = open(os.path.join(ROOT, f"assets_raw/chunks/dec/{int(cid_hex,16):04d}.bin"), "rb").read()
    if len(d) < 0x34: continue
    W, H = _w16(d, 0x29), _w16(d, 0x2B)
    cur, tile, bg = _w16(d, 0x2E), _w16(d, 0x30), _w16(d, 0x32)
    v["level"] = {"width": W, "height": H,
                  "tilemap": f"{cur:04X}", "tileset": f"{tile:04X}",
                  "bg_tileset": f"{bg:04X}"}
    tm = out_map.get(f"{cur:04X}")
    if tm is not None:
        tm["width"] = W; tm["height"] = H; tm["header"] = cid_hex

seen = sum(1 for v in out_map.values() if v["roles"] != ["unreferenced"])
print(f"referenced: {seen}/{len(out_map)}")
json.dump(out_map, open(os.path.join(ROOT, "assets_raw/chunk_map.json"), "w"), indent=1)
hist = collections.Counter(r for v in out_map.values() for r in v["roles"])
for r, n in hist.most_common():
    print(f"{n:5d}  {r}")
