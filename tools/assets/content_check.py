#!/usr/bin/env python3
"""content_check.py — the judge of the console content pack (content/).

Every level the pack adds or replaces (the LVX trailer of content/exe_static.bin:
the five console levels, the interlude scenes, the finale, the SNES-balance
variants, a mod's levels) is checked against what the ENGINE will do with it
and against the console source it was converted from. The checks derive the
level's needs from its own data — the spawn table, the class records of its
script, the animation code of those classes — instead of trusting the
converter's choice of a donor head:

  records   every spawn class has a template record (0x15 bytes at cls*0x15
            in the script) with a non-zero width and height;
  sprites   the sprite chunk of every spawned class (record +0; FFFF = none,
            FFFE = pool sprite) is in the stripe's sprite bank list — the
            spawner looks the chunk up in the per-level table ds:12AD
            (sub_12f82) and a miss frees the slot (sub_13809): the object
            never appears. The classes are the level's own — its spawn table,
            the three vikings, the head's class (+0x0C) — and, transitively,
            the children their code spawns (VM op 14, the class in the last
            operand byte), transitively;
  anims     every `res_lookup` of the animation code reachable from a spawned
            class (anim command 23, loc_1356e: the id is searched in the
            per-level table ds:124D) names a chunk of the stripe's anim chunk
            list — a miss silently takes entry 0 and the object is drawn with
            another chunk's frames. What a class reaches is the disassembler's
            own walk of the level's OWN script (tools/data/disasm.py, the
            STRICT entry model of the .lvs listings): from the record's entries
            through jumps, branches and the children of its op 14s, and from
            the op 19 sites of that code through the animation streams
            (tools/data/lvs_full's strict anim walk with anim_static's
            lengths) — not an address range, so a copied class whose old code
            was left behind unreferenced, or a routine several classes share,
            is attributed to the classes that actually run it;
  tables    at most 32 sprite banks (ds:12AD, 0x40 bytes) and 16 anim chunks
            (ds:124D, 0x20 bytes) — more overrun the tables;
  budgets   the sprite banks decompress one after another into the sprite
            segment (sub_1167a; 0x700 paragraphs = 28672 bytes), the tilemap
            into its segment (0x313 paragraphs = 12592 bytes), the anim chunks
            into the chunk buffer (0x2ABA paragraphs = 175392 bytes): the
            ORIGINAL's allocations (sub_11080's recorded sizes). The engine's
            own shadows are larger, so these are warnings unless --strict;
  head      a level converted from the SNES image keeps every byte of the
            source head except the fields the converter owns on purpose
            (+0x16 next slot, +0x2E..+0x33 chunk refs, +0x39..+0x3E parallax
            refs); its spawn table and palette animation equal the source's;
  palettes  the palette rows above 128 that every PC level of the world loads
            (the viking / HUD rows) are present; a level converted from the
            SNES image loads on every sprite row (128+) the PC palette chunk
            whose colours the SNES level loads there (snes2pc.py
            snes_sprite_palettes: the SNES DE's sprites are the PC's art under
            the PC's palettes, each SNES row matches one PC chunk within the
            BGR555 -> VGA6 rounding and no other) — the row a sprite draws on
            is chosen by its spawn record (a pool sprite's anim word), i.e. by
            the SNES spawn table the converter keeps, so a donor's rows fit the
            donor's spawns and not these (issue #6: SNDS's guard, class 69 on
            row 224, under JMNN's 102 instead of 107 — pink and white for blue
            and gold; PDDY with nothing on row 192, where the SNES loads 104);
            a row the pack loads and the SNES level does not is a warning.

Self-calibration: the same checks run first over every level of the EXE's
own tables (the 37 playable levels and the special slots up to 0x2F — the
ending, the title screens; heads from the open tree or the archive). Whatever
the original itself "fails" is a property of the original, not a defect:
those (script, class, anim chunk) pairs are dormant lookups and are only
reported as information for the pack (toggle_switch -> chunk 0x167 in every
world; the viking walk chunks the ending never loads), and so are the sprite
banks of op-14 children the original's own levels do not load (the arrow,
class 10 / bank 129, is absent from slot 0x16's banks; the title screens load
no 8x8 spark 0F5): a conditional spawn the original leaves to the lookup
miss. A table class without its bank is never dormant; a child whose bank
every original level reaching it loads is an error when it is missing (the
original's own convention); a child no original level reaches at all — a
class of the pack's own scripts, such as a scene's — is a warning, since the
walk cannot tell a branch the level's rows never select (the per-level
spawner dispatches on the row's pool value) from one they do: the dynamic
smoke is the judge of those. A check that fails on a PC level for any other
reason is a defect of the judge and is reported as such (exit 2).

The dynamic half is tests/content_smoke.sh: the game binary with
V2_SPAWN_SWEEP=1 spawns every entry of every level once (v2_vm.cpp
v2_content_sweep_tick) and the engine's V2-SPAWN-FAIL / V2-ANIM-RES-MISS lines
are compared with --dormant.

Reachability: the disassembler reads a script from assets_raw/chunks/dec under
the working directory (with its tables under tools/data), so the judge walks
in a temporary root that links the repo's files and holds the pack's own
scripts; a walk of one class takes well under a millisecond, the whole judge
about a second.

Usage: content_check.py [--content DIR] [--snes-rom PATH] [--strict] [-v]
       content_check.py --list | --list-all | --dormant   (for tests/content_smoke.sh:
       the pack's slots — with --list-all the PC levels too — and the dormant lookups per slot)
Exit code 0 = no errors, 1 = errors, 2 = the judge itself is broken.
build_content.py runs it after packing; `make content-check` runs it alone.
"""
import argparse
import contextlib
import glob
import importlib.util
import json
import os
import shutil
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import level_render as LR  # noqa: E402

# ---- the engine's model (verified facts, v2_vm.cpp / v2_ds_layout.h) --------
SPRITE_RES_MAX = 32          # ds:12AD id table, 0x40 bytes (sub_1167a fills, sub_12f82 looks up)
ANIM_CHUNK_MAX = 16          # ds:124D id table, 0x20 bytes (sub_116ae fills, anim cmd 23 looks up)
SPRITE_ARENA = 0x700 * 16    # sub_11080: seg_sprite = alloc(0x700 paragraphs)
TILEMAP_SEG = 0x313 * 16     # sub_11080: seg_tilemap = alloc(0x313 paragraphs)
CHUNK_BUF = 0x2ABA * 16      # the chunk buffer the anim chunks decompress into
HEAD_LEN = 0x43              # fixed head of the level-header stripe
HEAD_OWNED = {0x16, 0x17} | set(range(0x2E, 0x34)) | set(range(0x39, 0x3F))
DE_LEVEL_TABLE = 0x7686      # SNES DE ROM: 53 words, head chunk ids of slots 0..0x34
DE_EXCLUSIVE_SLOT0 = 0x25    # SNES slots 0x25..0x29 = TR33 SNDS TMPL RVTS PDDY = our slots 48..52
EXE_SLOTS = 0x30             # the EXE's level tables: slots 0..0x2F (0x2F = the ending)
LVX_ALT = 0x10               # trailer flag: an SNES-balance variant of a PC level
VIKING_CLASSES = {0, 1, 2}   # Baleog, Erik, Olaf: spawned by the head (+0x07..), present in every level
HEAD_CLASS = 0x0C            # head +0x0C -> ds:25BF: the class the level spawns by itself (a scene's controller)
OP_SPAWN = 0x14              # VM op 14 (sub_13239): spawns the class of its last operand byte (disasm.decode_op14)
ANIM_RES_LOOKUP = 0x17       # anim command 23 (loc_1356e): [17][chunk id LE]


def _load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


class Pack:
    """A content directory: headers from the open tree, chunks from the archive
    or the compiled records (content/.compiled/NNNN.bin = 8-byte table header
    + [u16 size][LZSS], decimal file names)."""

    def __init__(self, content):
        self.dir = content
        self._lzss = _load_module("lv_lzss", os.path.join(ROOT, "python", "lv_lzss.py"))
        self._chunks = {}

    def header(self, hid):
        p = os.path.join(self.dir, "level_headers", f"{hid:04X}.json")
        with open(p) as f:
            return bytes.fromhex(json.load(f)["raw"])

    def is_archive(self, cid):
        return os.path.exists(os.path.join(ROOT, "assets_raw", "chunks", "dec", f"{cid:04d}.bin"))

    def chunk(self, cid):
        if cid in self._chunks:
            return self._chunks[cid]
        data = None
        if self.is_archive(cid):
            data = LR.open_payload(cid, "lzss")
        else:
            p = os.path.join(self.dir, ".compiled", f"{cid:04d}.bin")
            if os.path.exists(p):
                with open(p, "rb") as f:
                    rec = f.read()
                comp = rec[8:]
                size = struct.unpack_from("<H", comp, 0)[0]
                data = bytes(self._lzss.lv_decompress(comp[2:], size))
        self._chunks[cid] = data
        return data

    def lvx_records(self):
        p = os.path.join(self.dir, "exe_static.bin")
        with open(p, "rb") as f:
            img = f.read()
        m = img.rfind(b"LVX5")
        if m < 0:
            return []
        n = struct.unpack_from("<H", img, m + 4)[0]
        recs = []
        o = m + 6
        for _ in range(n):
            level, hdr, tmpl = struct.unpack_from("<HHH", img, o)
            pw = img[o + 6:o + 10].decode("latin1").strip("\0")
            demo, flags = struct.unpack_from("<HH", img, o + 10)
            recs.append(dict(level=level, hdr=hdr, tmpl=tmpl, pw=pw, demo=demo, flags=flags))
            o += 36
        return recs


class Walker:
    """The disassembler's reachability per class (tools/data/disasm.py walk, the STRICT
    entry model of the .lvs listings): from a class's record entries P and P+3 through
    jumps, branches and the children of its op 14s, and from the op 19 sites of that code
    through the animation streams (lvs_full._walk_anim_strict with anim_static's lengths
    of the VAR commands). Per (script, class): the classes it instantiates (itself and the
    children, transitively), the res_lookups of its animation code as (address, chunk),
    and the (address, length) of every instruction it reaches — the identity of the
    class's behaviour, for the test of a copied class. The disassembler reads a script
    from assets_raw/chunks/dec under the working directory, so a temporary root links the
    repo's tables and chunks and takes the pack's own scripts."""

    def __init__(self):
        self.root = tempfile.mkdtemp(prefix="lv_content_check_")
        for name in ("tools", "assets_raw", "assets_raw/chunks"):
            os.makedirs(os.path.join(self.root, name), exist_ok=True)
        os.symlink(os.path.join(ROOT, "tools", "data"), os.path.join(self.root, "tools", "data"))
        os.symlink(os.path.join(ROOT, "src"), os.path.join(self.root, "src"))
        for f in ("ds_static.bin", "exe_static.bin"):
            os.symlink(os.path.join(ROOT, f), os.path.join(self.root, f))
        dec = os.path.join(self.root, "assets_raw", "chunks", "dec")
        os.makedirs(dec)
        for p in glob.glob(os.path.join(ROOT, "assets_raw", "chunks", "dec", "*.bin")):
            os.symlink(p, os.path.join(dec, os.path.basename(p)))
        # the modules bind their tables relative to the working directory, anim_static to the
        # root above its own file — so they are loaded through the temp root's own tools/data
        tdata = os.path.join(self.root, "tools", "data")
        with self._in_root():
            self._dz = _load_module("dz", os.path.join(tdata, "disasm.py"))
            self._lf = _load_module("lvs_full", os.path.join(tdata, "lvs_full.py"))
            self._ad = _load_module("anim_disasm", os.path.join(tdata, "anim_disasm.py"))
            self._as = _load_module("anim_static", os.path.join(tdata, "anim_static.py"))
            self._table = self._dz.load_draft()
        # anim_static's entries of a script are the EXE tables' spawn classes (lvs_full.full_walk
        # through disasm.spawn_entries); a pack script is entered at the classes the pack's
        # levels give it (register), as the listing generator does for the .lvs of a pack
        self._extra = {}     # cid -> entry classes of the pack's scripts
        lf_dz = self._as.mod("lvs_full").dz   # anim_static's own lvs_full (its mod() cache), loaded under the temp root
        orig = lf_dz.spawn_entries
        extra = self._extra

        def spawn_entries(*a, **k):
            pt = orig(*a, **k)
            for cid, classes in extra.items():
                pt.setdefault(cid, set()).update(classes)
            return pt
        lf_dz.spawn_entries = spawn_entries
        self._lens = {}      # cid -> {anim cmd pc: {next pc}}: the VAR commands' lengths (anim_layer's rule)
        self._needs = {}     # (cid, cls) -> (classes, res, code)

    @contextlib.contextmanager
    def _in_root(self):
        cwd = os.getcwd()
        os.chdir(self.root)
        try:
            yield
        finally:
            os.chdir(cwd)

    def close(self):
        shutil.rmtree(self.root, ignore_errors=True)

    def register(self, cid, data, classes):
        """A pack script: its bytes under the temp root, where the disassembler reads them,
        and the classes it is entered at (every registration before the first walk of it)."""
        p = os.path.join(self.root, "assets_raw", "chunks", "dec", f"{cid:04d}.bin")
        if not os.path.exists(p):
            with open(p, "wb") as f:
                f.write(data)
        self._extra.setdefault(cid, set()).update(classes)

    def needs_of(self, cid, cls):
        key = (cid, cls)
        if key in self._needs:
            return self._needs[key]
        with self._in_root():
            d, _, seen, _, _, _, anim_entries = self._dz.walk(cid, {cls}, self._table)
            lens = self._lens.get(cid)
            if lens is None:
                # lvs_full.anim_layer: the static lengths of the VAR commands, {cmd pc: {next pc}}
                lens = {pc: {pc + 1 + L} for pc, L in self._as.resolved(cid).items()}
                self._lens[cid] = lens
            _, aseen = self._lf._walk_anim_strict(d, set(anim_entries), lens, seen)
        classes = {cls}
        for pc, (op, _) in seen.items():
            if op == OP_SPAWN:
                classes.update(self._dz.decode_op14(d, pc)[1])
        res = sorted({(pc, d[pc + 1] | (d[pc + 2] << 8)) for pc, cmd in aseen.items()
                      if cmd == ANIM_RES_LOOKUP and pc + 3 <= len(d)})
        code = [(pc, ln) for pc, (_, ln) in seen.items()]
        for pc, cmd in aseen.items():
            nxt = lens.get(pc)
            sch = self._ad.ANIM_SCHEME.get(cmd)
            if nxt and len(nxt) == 1:
                ln = next(iter(nxt)) - pc
            elif sch and sch[0] in ("fixed", "stop"):
                ln = 1 + sch[1]
            elif sch and sch[0] in ("jump", "loopstart"):
                ln = 3
            else:
                ln = 1
            code.append((pc, ln))
        self._needs[key] = (classes, res, sorted(code))
        return self._needs[key]


def pal_entries(st):
    out = []
    for e in st["pal_list"]:
        if isinstance(e, dict):
            out.append((e["chunk"], e["start"]))
        else:
            out.append((e[0], e[1]))
    return out


class Level:
    """One level to judge: its stripe, the script the engine runs it on, the
    SNES head it was converted from (None = nothing to compare)."""

    def __init__(self, name, raw, script_cid, script, source=None):
        self.name, self.raw, self.script_cid, self.script = name, raw, script_cid, script
        self.source = source
        self.st = LR.parse_stripe(raw)
        self.qw, self.qh = LR.parse_header(raw)[0:2]
        # the three vikings (classes 0, 1, 2) enter through the head's trio mechanism, not the spawn
        # table; the head's own class (+0x0C) is spawned by the level init (a scene's controller)
        self.classes = sorted(level_classes(raw, self.st))


def level_classes(raw, st):
    return {sp["cls"] for sp in st["spawns"]} | VIKING_CLASSES | {raw[HEAD_CLASS] | (raw[HEAD_CLASS + 1] << 8)}


class Judge:
    def __init__(self, pack, snes_rom=None, strict=False, verbose=False):
        self.pack = pack
        self.strict = strict
        self.verbose = verbose
        self.walker = Walker()
        self.rom = snes_rom
        self.de_table = None
        if snes_rom is not None:
            self.de_table = [struct.unpack_from("<H", snes_rom.rom, DE_LEVEL_TABLE + 2 * i)[0] for i in range(53)]
        self.dormant = set()            # (script, cls, res) lookups the original itself leaves unresolved
        self.dormant_spr = set()        # (script, child cls, sprite chunk) op-14 children the original's levels never load
        self.precedent = set()          # (script, child cls, sprite chunk) every op-14 child the original's levels reach
        self.world_pal_rows = {}        # script -> palette starts >= 128 present in EVERY PC level on it
        self.judge_defects = []

    def close(self):
        self.walker.close()

    # ---- the dormant verdicts and their inheritance ----------------------
    def inherits(self, lv, base, cls):
        """The level's class behaves as the archive script `base`'s class: the same script,
        or a pack script in which the class's record and every instruction the class
        reaches in `base` (object code and animation streams) are the same bytes at the
        same addresses — the scene and finale scripts are copies of a world script with
        their own classes added, and the untouched classes keep the original's verdicts."""
        if lv.script_cid == base:
            return True
        if self.pack.is_archive(lv.script_cid):
            return False
        bs = self.pack.chunk(base)
        if bs is None or lv.script[cls * 0x15:(cls + 1) * 0x15] != bs[cls * 0x15:(cls + 1) * 0x15]:
            return False
        _, _, code = self.walker.needs_of(base, cls)
        n = len(lv.script)
        return bool(code) and all(pc + ln <= n and lv.script[pc:pc + ln] == bs[pc:pc + ln] for pc, ln in code)

    def is_dormant(self, lv, cls, chunk, pairs):
        return any(c == cls and ch == chunk and self.inherits(lv, b, cls) for b, c, ch in pairs)

    # ---- derived needs -------------------------------------------------
    def needs(self, lv):
        """sprites: {chunk: {cls}} over every class the level instantiates (its own and the
        op-14 children, transitively), anims: {res: {direct cls}} — a res_lookup is charged
        to the level's own class whose walk reaches it (through its children too), norec:
        the classes without a usable template record, classes: all of them."""
        sprites, anims, norec = {}, {}, []
        classes = set()
        self.parents = {}   # child class -> the level's own classes whose code spawns it (for the messages)
        for cls in lv.classes:
            sub, res, _ = self.walker.needs_of(lv.script_cid, cls)
            classes |= sub
            for child in sub - {cls}:
                self.parents.setdefault(child, set()).add(cls)
            for _, chunk in res:
                anims.setdefault(chunk, set()).add(cls)
        for cls in sorted(classes):
            r = LR.class_record(lv.script, cls)
            if not r or not (0 < r["pc"] < len(lv.script)) or r["w"] == 0 or r["h"] == 0:
                norec.append(cls)
                continue
            if r["spr"] not in (0xFFFF, 0xFFFE):
                sprites.setdefault(r["spr"], set()).add(cls)
        return sprites, anims, norec, classes

    # ---- the PC levels of the EXE's tables ------------------------------
    def pc_levels(self):
        PX = _load_module("parallax_snes", os.path.join(HERE, "parallax_snes.py"))
        with open(os.path.join(ROOT, "exe_static.bin"), "rb") as f:
            exe = f.read()
        out = []
        for slot in range(EXE_SLOTS):
            try:
                hid = PX.exe_level_head(slot)
            except Exception:
                continue
            o = LR._EXE_DS + LR._SCRIPT_TBL + slot * 2
            scr = exe[o] | (exe[o + 1] << 8)
            if scr == 0xFFFF or not self.pack.is_archive(scr):
                continue
            p = os.path.join(ROOT, "assets", "level_headers", f"{hid:04X}.json")
            if os.path.exists(p):
                with open(p) as f:
                    raw = bytes.fromhex(json.load(f)["raw"])
            else:
                raw = LR.open_payload(hid, "lzss")
            lv = Level(f"PC slot {slot:02X} hdr {hid:04X} script {scr:04X}", raw, scr, self.pack.chunk(scr))
            lv.slot, lv.short, lv.alt = slot, f"PC{slot:02d}", False
            out.append(lv)
        return out

    def calibrate(self):
        rows = {}
        levels = self.pc_levels()
        for lv in levels:
            sprites, anims, norec, classes = self.needs(lv)
            child_only = classes - set(lv.classes)
            banks = {b["chunk"] for b in lv.st["sprite_banks"]}
            achunks = {a["chunk"] for a in lv.st["anim_chunks"]}
            for res, clss in anims.items():
                if res not in achunks:
                    for cls in clss:
                        self.dormant.add((lv.script_cid, cls, res))
            for spr, clss in sprites.items():
                for cls in clss:
                    if cls in child_only:
                        self.precedent.add((lv.script_cid, cls, spr))
                if spr not in banks:
                    direct = [c for c in clss if c not in child_only]
                    if direct:
                        self.judge_defects.append(f"{lv.name}: sprite chunk {spr:03X} of class {' '.join('%02X' % c for c in sorted(direct))} not in its own banks")
                    for cls in clss:
                        if cls in child_only:
                            self.dormant_spr.add((lv.script_cid, cls, spr))
            if norec:
                self.judge_defects.append(f"{lv.name}: classes without a record: {' '.join('%02X' % c for c in norec)}")
            if len(banks) > SPRITE_RES_MAX or len(achunks) > ANIM_CHUNK_MAX:
                self.judge_defects.append(f"{lv.name}: tables over the limits")
            starts = {s for _, s in pal_entries(lv.st) if s >= 128}
            rows[lv.script_cid] = starts if lv.script_cid not in rows else rows[lv.script_cid] & starts
        self.world_pal_rows = rows
        return len(levels)

    def class_list(self, clss, child_only):
        """'37 83 (spawned by 82)': the classes, a child with the classes whose code spawns it."""
        out = []
        for c in sorted(clss):
            if c in child_only:
                out.append("%02X (spawned by %s)" % (c, " ".join("%02X" % p for p in sorted(self.parents.get(c, ())))))
            else:
                out.append("%02X" % c)
        return " ".join(out)

    # ---- the checks ----------------------------------------------------
    def check(self, lv):
        err, warn, info = [], [], []
        sprites, anims, norec, classes = self.needs(lv)
        banks = [b["chunk"] for b in lv.st["sprite_banks"]]
        achunks = [a["chunk"] for a in lv.st["anim_chunks"]]
        bankset, animset = set(banks), set(achunks)
        if norec:
            err.append(f"classes without a template record: {' '.join('%02X' % c for c in norec)}")
        child_only = classes - set(lv.classes)
        for spr, clss in sorted(sprites.items()):
            if spr in bankset:
                continue
            dormant = {c for c in clss if c in child_only and self.is_dormant(lv, c, spr, self.dormant_spr)}
            unseen = {c for c in clss - dormant if c in child_only and not self.is_dormant(lv, c, spr, self.precedent)}
            live = clss - dormant - unseen
            if live:
                err.append(f"sprite bank {spr:03X} missing — classes {self.class_list(live, child_only)} never spawn (sub_12f82 miss)")
            if unseen:
                warn.append(f"sprite bank {spr:03X} missing — the op 14 children {self.class_list(unseen, child_only)} never spawn if their "
                            f"branch runs; no original level reaches them, the walk cannot tell (tests/content_smoke.sh can)")
            if dormant:
                info.append(f"sprite bank {spr:03X}: dormant — the op 14 children {self.class_list(dormant, child_only)} never spawn, as on the original's own levels of their world")
        for res, clss in sorted(anims.items()):
            if res in animset:
                continue
            live = {c for c in clss if not self.is_dormant(lv, c, res, self.dormant)}
            if live:
                err.append(f"anim chunk {res:03X} missing — classes {' '.join('%02X' % c for c in sorted(live))} take entry 0 (res_lookup miss)")
            else:
                info.append(f"anim chunk {res:03X}: dormant lookup of classes {' '.join('%02X' % c for c in sorted(clss))} (the original leaves it unresolved too)")
        if len(banks) > SPRITE_RES_MAX:
            err.append(f"{len(banks)} sprite banks, the table holds {SPRITE_RES_MAX}")
        if len(achunks) > ANIM_CHUNK_MAX:
            err.append(f"{len(achunks)} anim chunks, the table holds {ANIM_CHUNK_MAX}")
        # budgets of the original's allocations
        total = sum(len(self.pack.chunk(b) or b"") for b in banks)
        budget = err if self.strict else warn
        if total > SPRITE_ARENA:
            budget.append(f"sprite banks total {total} B over the original's sprite segment {SPRITE_ARENA} B")
        tm = lv.qw * lv.qh * 2
        if tm > TILEMAP_SEG:
            budget.append(f"tilemap {lv.qw}x{lv.qh} = {tm} B over the original's tilemap segment {TILEMAP_SEG} B")
        atotal = sum(len(self.pack.chunk(a) or b"") for a in achunks)
        if atotal > CHUNK_BUF:
            budget.append(f"anim chunks total {atotal} B over the chunk buffer {CHUNK_BUF} B")
        # the source head
        if lv.source is not None:
            src = lv.source
            diffs = [o for o in range(HEAD_LEN) if src[o] != lv.raw[o] and o not in HEAD_OWNED]
            if diffs:
                err.append("head differs from the SNES source at " + " ".join(f"+{o:02X} ({src[o]:02X}/{lv.raw[o]:02X})" for o in diffs))
            sst = LR.parse_stripe(src)
            if sst["spawns"] != lv.st["spawns"]:
                err.append("spawn table differs from the SNES source")
            if (sst["pal_anim_en"], sst["pal_anims"]) != (lv.st["pal_anim_en"], lv.st["pal_anims"]):
                err.append("palette animation differs from the SNES source")
            # the sprite palette rows (128+): the SNES level's own, as the PC chunks carrying their
            # colours (snes2pc.snes_sprite_palettes) — the row a sprite draws on is the spawn record's
            # choice, and the spawn table is the SNES one, so the SNES palette list is the one that fits
            # it; a donor's fits the donor's spawns (issue #6: SNDS's guard on row 224 under JMNN's 102)
            if self.rom is not None:
                import snes2pc as S2   # lazily: snes2pc imports this module for complete_lists
                want_rows = S2.snes_sprite_palettes(self.rom, src)
                have_rows = {s: c for c, s in pal_entries(lv.st) if s >= 128}
                for s, (chunk, d, d2, n) in sorted(want_rows.items()):
                    if d > S2.PAL_MATCH_MAX or (d2 is not None and d2 < 2 * d):
                        err.append(f"sprite palette row {s}: no PC palette chunk carries the SNES level's colours (closest {chunk:03X} at {d}, next at {d2})")
                    elif s not in have_rows:
                        err.append(f"sprite palette row {s} missing — the SNES level loads {chunk:03X}'s colours there; the sprites on that row draw with whatever the DAC holds")
                    elif have_rows[s] != chunk:
                        err.append(f"sprite palette row {s}: {have_rows[s]:03X} loaded where the SNES level loads {chunk:03X}'s colours — the sprites on that row wear the wrong palette")
                for s in sorted(set(have_rows) - set(want_rows)):
                    warn.append(f"sprite palette row {s}: {have_rows[s]:03X} loaded, the SNES level loads nothing there (a donor's row, unused)")
        # palette rows every PC level of the world loads
        want = self.world_pal_rows.get(lv.script_cid, set())
        have = {s for _, s in pal_entries(lv.st) if s >= 128}
        missing_rows = sorted(want - have)
        if missing_rows:
            warn.append("palette rows missing that every PC level of the world loads: " + " ".join(str(r) for r in missing_rows))
        if self.verbose:
            info.append(f"banks {len(banks)} ({total} B), anim chunks {len(achunks)} ({atotal} B), tilemap {tm} B, spawns {len(lv.st['spawns'])}, "
                        f"classes {' '.join('%02X' % c for c in sorted(classes))} (children included), "
                        f"sprites needed {' '.join('%03X' % s for s in sorted(sprites))}, anims needed {' '.join('%03X' % a for a in sorted(anims))}")
        return err, warn, info

    # ---- the pack's levels ---------------------------------------------
    def levels(self):
        out = []
        recs = self.pack.lvx_records()
        # the pack's own scripts go where the disassembler reads them
        for rec in recs:
            if not self.pack.is_archive(rec["tmpl"]):
                script = self.pack.chunk(rec["tmpl"])
                if script is not None:
                    raw = self.pack.header(rec["hdr"])
                    self.walker.register(rec["tmpl"], script, level_classes(raw, LR.parse_stripe(raw)))
        for rec in recs:
            raw = self.pack.header(rec["hdr"])
            script = self.pack.chunk(rec["tmpl"])
            name = f"slot {rec['level']:2d} {rec['pw'] or 'FIN':4s} hdr {rec['hdr']:04X} script {rec['tmpl']:04X}"
            if script is None:
                out.append((name, None, f"script {rec['tmpl']:04X} not found"))
                continue
            source = None
            if self.de_table is not None:
                if 48 <= rec["level"] <= 52:
                    source = self.rom.chunk(self.de_table[DE_EXCLUSIVE_SLOT0 + rec["level"] - 48])
                elif (rec["flags"] & LVX_ALT) and rec["level"] < DE_EXCLUSIVE_SLOT0:
                    source = self.rom.chunk(self.de_table[rec["level"]])
            lv = Level(name, raw, rec["tmpl"], script, source)
            lv.slot, lv.short, lv.alt = rec["level"], rec["pw"] or "FIN", bool(rec["flags"] & LVX_ALT)
            out.append((name, lv, None))
        return out


def judge_pack(content, snes_rom=None, strict=False, verbose=False, mode="check", out=print):
    """Run the judge over the pack at `content`. mode: "check" (the report; returns 0 = no
    errors, 1 = errors, 2 = the judge fails on the original), "list" / "list-all" (the pack's
    slots, with the PC levels for list-all: slot= name= script= alt= lines), "dormant" (the
    dormant lookups per slot: slot= kind=anim|sprite class= chunk= lines). `out` takes the
    report lines (build_content.py passes its own printer)."""
    j = Judge(Pack(content), snes_rom, strict, verbose)
    try:
        n = j.calibrate()
        if j.judge_defects:
            out(f"content_check: the judge fails on the original ({n} PC levels) — fix the judge, not the pack:")
            for d in j.judge_defects:
                out("  " + d)
            return 2
        if mode in ("list", "list-all", "dormant"):
            lvs = [lv for _, lv, _ in j.levels() if lv is not None]
            if mode != "list":
                lvs += [lv for lv in j.pc_levels() if lv.slot <= 0x24 or lv.slot == EXE_SLOTS - 1]
            lvs.sort(key=lambda lv: (lv.slot, lv.alt))
            for lv in lvs:
                if mode == "dormant":
                    for kind, pairs in (("anim", j.dormant), ("sprite", j.dormant_spr)):
                        for scr, cls, chunk in sorted(pairs):
                            if j.inherits(lv, scr, cls):
                                out(f"slot={lv.slot} kind={kind} class={cls:02X} chunk={chunk:03X}")
                else:
                    out(f"slot={lv.slot} name={lv.short} script={lv.script_cid:03X} alt={int(lv.alt)}")
            return 0
        out(f"content_check: calibrated on {n} PC levels; dormant lookups: " +
            (", ".join(f"script {w:03X} class {c:02X} -> {r:03X}" for w, c, r in sorted(j.dormant)) or "none") +
            "; dormant op-14 children: " +
            (", ".join(f"script {w:03X} class {c:02X} -> bank {r:03X}" for w, c, r in sorted(j.dormant_spr)) or "none"))
        nerr = nwarn = 0
        for name, lv, problem in j.levels():
            if lv is None:
                out(f"ERROR {name}: {problem}")
                nerr += 1
                continue
            err, warn, info = j.check(lv)
            nerr += len(err)
            nwarn += len(warn)
            status = "FAIL" if err else ("warn" if warn else "ok")
            out(f"{status:4s} {name}")
            for e in err:
                out(f"      ERROR {e}")
            for w in warn:
                out(f"      warn  {w}")
            for i in info:
                out(f"      info  {i}")
        out(f"content_check: {nerr} error(s), {nwarn} warning(s)")
        return 1 if nerr else 0
    finally:
        j.close()


def main():
    ap = argparse.ArgumentParser(description="the judge of the console content pack")
    ap.add_argument("--content", default="content", help="the pack directory (default: ./content)")
    ap.add_argument("--snes-rom", help="the SNES DE image (default: roms/LostVikingsDE.sfc, LV_SNES_ROM); none = no source comparison")
    ap.add_argument("--strict", action="store_true", help="the original's byte budgets are errors, not warnings")
    ap.add_argument("-v", "--verbose", action="store_true")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--list", action="store_true", help="the pack's slots (slot= name= script= alt= lines), no report")
    g.add_argument("--list-all", action="store_true", help="--list plus the PC levels of the EXE's table")
    g.add_argument("--dormant", action="store_true", help="the dormant lookups per slot (slot= kind= class= chunk= lines)")
    a = ap.parse_args()
    mode = "list" if a.list else "list-all" if a.list_all else "dormant" if a.dormant else "check"
    if not os.path.isdir(a.content):
        print(f"content_check: no such directory {a.content}")
        return 2
    rom = None
    if mode == "check":
        try:
            sp = _load_module("snes2pc", os.path.join(HERE, "snes2pc.py"))
            rom = sp.SnesRom(a.snes_rom)
        except Exception as e:
            print(f"content_check: SNES image not available ({e}) — the source comparison is skipped")
    return judge_pack(a.content, rom, a.strict, a.verbose, mode)


if __name__ == "__main__":
    sys.exit(main())
