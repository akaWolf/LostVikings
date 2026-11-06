"""Exercise scenario-runner policy without copyrighted assets or a game build."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).with_name("scenarios.sh")


class ScenarioRunnerTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        (self.root / "tests/replays").mkdir(parents=True)
        (self.root / "tests/golden_states").mkdir()
        (self.root / "tests/scenarios.sh").write_text(SCRIPT.read_text())
        (self.root / "tests/replays/case.inp").write_text("# empty\n")
        (self.root / "tests/replays/case.frames").write_text("10\n")
        (self.root / "tests/golden_states/case.txt").write_text("expected\n")
        (self.root / "DATA.DAT").write_text("actual")
        (self.root / "vikings_keymap.cfg").write_text("custom keymap")
        (self.root / "v2_options.cfg").write_text("custom options")
        (self.root / "tests/reference_dat.py").write_text('''
import os, pathlib, sys
status = os.environ["FAKE_STATUS"]
if status == "normalized":
    pathlib.Path(sys.argv[2]).write_text("reference")
print(status)
''')
        binary = self.root / "vikings_headless"
        binary.write_text('''#!/usr/bin/env python3
import os, pathlib, sys
kind = pathlib.Path("DATA.DAT").read_text()
with open(os.environ["FAKE_TRACE"], "a") as f:
    f.write(kind + "\\n")
if pathlib.Path("vikings_keymap.cfg").read_text() != "custom keymap": sys.exit(8)
if pathlib.Path("v2_options.cfg").read_text() != "custom options": sys.exit(8)
if not pathlib.Path(next(a.split("=", 1)[1] for a in sys.argv if a.startswith("--replay-input="))).is_file(): sys.exit(9)
if os.environ.get("FAKE_FAIL") == kind: sys.exit(3)
if os.environ.get("FAKE_MISSING") != kind:
    pathlib.Path(os.environ["V2_GOLDEN_DUMP"]).write_text("expected\\n")
''')
        binary.chmod(0o755)

    def run_suite(self, status, **overrides):
        env = os.environ.copy()
        env.update(FAKE_STATUS=status, FAKE_TRACE=str(self.root / "trace"), JOBS="1", GOLDEN="")
        env.update(overrides)
        return subprocess.run(["bash", str(self.root / "tests/scenarios.sh")],
                              cwd=self.root, env=env, capture_output=True, text=True)

    def test_exact_and_unknown_run_once(self):
        for status in ("exact", "unknown"):
            with self.subTest(status=status):
                result = self.run_suite(status)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("1 pass / 0 fail", result.stdout)
        self.assertEqual((self.root / "trace").read_text().splitlines(), ["actual", "actual"])

    def test_variant_runs_both_and_preserves_archive(self):
        result = self.run_suite("normalized")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.root / "trace").read_text().splitlines(), ["actual", "reference"])
        self.assertEqual((self.root / "DATA.DAT").read_text(), "actual")

    def test_failed_replays_and_missing_dumps_fail(self):
        for env in ({"FAKE_FAIL": "actual"}, {"FAKE_FAIL": "reference"},
                    {"FAKE_MISSING": "actual"}, {"FAKE_MISSING": "reference"}):
            with self.subTest(env=env):
                result = self.run_suite("normalized", **env)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn("0 pass / 1 fail", result.stdout)

    def test_update_rejects_variants_and_unknown(self):
        for status in ("normalized", "unknown"):
            with self.subTest(status=status):
                result = self.run_suite(status, GOLDEN="update")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("requires the unmodified reference", result.stdout)


if __name__ == "__main__":
    unittest.main()
