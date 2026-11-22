"""The console content builder as the release bundles ship it, without game data.

tools/assets/content_tools_bundle.sh writes content-tools/ — the scripts and the repository's inputs
build_content.py needs, nothing else: no open tree (assets/), no extraction of the archive
(assets_raw/chunks/), both of which a development tree keeps beside the tools. The builder has to work
from that directory alone, and it did not: the pack's judge loaded its LZSS decoder from python/, which
the bundle does not carry; it took "is this an archive chunk" and the scripts it walks from
assets_raw/chunks/dec of the tools' root, not from the extraction of the pack it judges; and the
converter matched a SNES level's sprite palette rows against the palette chunks listed by the headers of
the tools' root's open tree. A development tree has all three, so nothing there showed it.

Each case stages the bundle the way the CI's "Package" step does and runs its check in a python of its
own, from an empty directory, with nothing but the staged tools on the path; the archive is a synthetic
one of a few records, written where the tools read theirs."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent

# a level-header stripe with nothing but a palette list (the grammar of level_render.parse_stripe):
# the fixed head, the spawn terminator, {chunk u16, start u8} entries, their terminator, the palette
# animation word and its zero lead byte, the terminators of the sprite banks and of the anim chunks
STRIPE = """
def stripe(pal_list):
    out = bytearray(0x43) + b"\\xff\\xff"
    for chunk, start in pal_list:
        out += bytes((chunk & 0xFF, chunk >> 8, start))
    return bytes(out + b"\\xff\\xff" + b"\\x00\\x00" + b"\\x00" + b"\\xff\\xff" + b"\\xff\\xff")
"""

PRELUDE = """
import json, os, sys
dest, work = sys.argv[1], sys.argv[2]
sys.path.insert(0, os.path.join(dest, "tools", "assets"))
def put(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
"""


class ContentToolsBundleTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="lv_content_tools_")
        cls.dest = os.path.join(cls.tmp.name, "content-tools")
        r = subprocess.run(["bash", str(ROOT / "tools" / "assets" / "content_tools_bundle.sh"), cls.dest],
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError("content_tools_bundle.sh failed:\n" + r.stdout + r.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def py(self, code):
        """Run `code` in a python of its own: isolated from the environment, an empty working directory,
        the staged tools as the only source of modules. Returns its stdout; a failure fails the case
        with the child's traceback."""
        work = tempfile.mkdtemp(dir=self.tmp.name)
        r = subprocess.run([sys.executable, "-I", "-B", "-c", PRELUDE + STRIPE + code, self.dest, work],
                           cwd=work, capture_output=True, text=True)
        if r.returncode != 0:
            self.fail("the staged tools failed:\n" + r.stderr[-3000:])
        return r.stdout

    def test_the_bundle_holds_no_development_trees(self):
        # the premise of the cases below: the staged directory is what a user gets
        for name in ("assets", os.path.join("assets_raw", "chunks")):
            self.assertFalse(os.path.exists(os.path.join(self.dest, name)), name)

    def test_the_builders_modules_load_from_the_bundle(self):
        # what build_content.py imports, at the top or on its way through the steps — but for
        # integrate_snes, which reads the Genesis image as it loads (the full build is the test of it:
        # tests/content_tools_check.sh)
        self.py("""
import build_content, assetc, texts_exe, bac_strings, edit_server, content_check, snes2pc
import level_render, parallax_snes
""")

    def test_the_judge_starts_from_the_bundle(self):
        # Pack loads the LZSS decoder of the compiled records, Judge the disassembler and its tables
        self.py("""
import content_check as CC
pack = os.path.join(work, "pack"); os.makedirs(pack)
j = CC.Judge(CC.Pack(pack))
j.close()
""")

    def test_the_judge_reads_the_archive_of_the_pack(self):
        # a pack carries the extraction it was built from (build_content.py: <pack>/raw); a tree without
        # one — the editor's scratch — is judged against the tools' current archive (assetc.RAW)
        out = self.py("""
import assetc as AC, content_check as CC
pack = os.path.join(work, "pack")
put(os.path.join(pack, "raw", "chunks", "dec", "0005.bin"), b"\\x01\\x02\\x03")
other = os.path.join(work, "other_raw")
put(os.path.join(other, "chunks", "dec", "0007.bin"), b"\\x07")
AC.RAW = other
p = CC.Pack(pack)
j = CC.Judge(p)
linked = sorted(os.listdir(os.path.join(j.walker.root, "assets_raw", "chunks", "dec")))
j.close()
scratch = os.path.join(work, "scratch"); os.makedirs(scratch)
s = CC.Pack(scratch)
print(json.dumps({"own": [p.is_archive(5), p.is_archive(6), p.is_archive(7)], "data": list(p.chunk(5)),
                  "linked": linked, "scratch": [s.is_archive(5), s.is_archive(7)], "sdata": list(s.chunk(7))}))
""")
        got = json.loads(out)
        self.assertEqual(got["own"], [True, False, False])
        self.assertEqual(got["data"], [1, 2, 3])
        self.assertEqual(got["linked"], ["0005.bin"])
        self.assertEqual(got["scratch"], [False, True])
        self.assertEqual(got["sdata"], [7])

    def test_the_sprite_palette_rows_match_against_the_archive(self):
        # the candidates of snes_sprite_palettes are the palette chunks the ARCHIVE's level headers
        # list (the chunk map's level_header_stripe records of the current extraction): 0x20 carries
        # the SNES row's colours exactly, 0x21 others, 0x22 the same colours again but no header
        # lists it, 0x23 is listed and too short to hold a row
        out = self.py("""
import assetc as AC, snes2pc as S2
raw = os.path.join(work, "raw")
words = [i | ((31 - i) << 5) | ((2 * i % 32) << 10) for i in range(16)]            # the SNES row, BGR555
row = b"".join(bytes(S2.bgr555_to_vga6(w)) for w in words)                          # the same colours, VGA6
far = bytes((c + 20) % 64 for c in row)
cmap = {"0010": {"roles": ["level_header_stripe"]}, "0011": {"roles": ["level_header_stripe"]},
        "0020": {"roles": ["palette"]}, "0021": {"roles": ["palette"]}, "0022": {"roles": ["palette"]},
        "0023": {"roles": ["palette"]}}
put(os.path.join(raw, "chunk_map.json"), json.dumps(cmap).encode())
put(os.path.join(raw, "chunks", "dec", "%04d.bin" % 0x10), stripe([(0x21, 144), (0x23, 160)]))
put(os.path.join(raw, "chunks", "dec", "%04d.bin" % 0x11), stripe([(0x20, 128)]))
put(os.path.join(raw, "chunks", "dec", "%04d.bin" % 0x20), row)
put(os.path.join(raw, "chunks", "dec", "%04d.bin" % 0x21), far)
put(os.path.join(raw, "chunks", "dec", "%04d.bin" % 0x22), row)
put(os.path.join(raw, "chunks", "dec", "%04d.bin" % 0x23), row[:24])
AC.RAW = raw

class Rom:                                   # the SNES side: chunk 0x150 = the sprite row, 0x151 = a background row
    def chunk(self, cid):
        return b"".join(bytes((w & 0xFF, w >> 8)) for w in (words if cid == 0x150 else [0x7FFF] * 16))

got = S2.snes_sprite_palettes(Rom(), stripe([(0x151, 0), (0x150, 128)]))
print(json.dumps({"rows": {str(k): list(v) for k, v in got.items()},
                  "far": sum(abs(a - b) for a, b in zip(row, far))}))
""")
        got = json.loads(out)
        self.assertEqual(list(got["rows"]), ["128"])                       # the background entry is no sprite row
        chunk, dist, second, n = got["rows"]["128"]
        self.assertEqual((chunk, dist, n), (0x20, 0, 16))
        self.assertEqual(second, got["far"])                               # the runner-up is 0x21: 0x22 and 0x23 are no candidates
        self.assertGreater(second, 0)


if __name__ == "__main__":
    unittest.main()
