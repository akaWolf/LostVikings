"""Opt-in standalone regression: set VIKINGS_TEST_EXE to a built port executable."""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent


class ChunkFatalTest(unittest.TestCase):
    def run_bad_chunk(self, offsets):
        exe = Path(os.environ['VIKINGS_TEST_EXE']).resolve()
        with tempfile.TemporaryDirectory(prefix='vikings-chunk-fatal-') as tmp:
            directory = Path(tmp)
            # The executable also searches its own directory for DATA.DAT.
            shutil.copy2(exe, directory / exe.name)
            for name in ('DATA.DAT', 'ds_static.bin', 'exe_static.bin'):
                shutil.copy2(ROOT / name, directory / name)
            with (directory / 'DATA.DAT').open('r+b') as archive:
                archive.seek(4 * 4)
                offset = struct.unpack('<I', archive.read(4))[0]
                archive.seek(4 * 4)
                archive.write(struct.pack('<II', *offsets(offset)))
            env = os.environ.copy()
            env.update(V2_CONTENT='0', SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')
            env.pop('V2_ASSETS_DIR', None)
            env.pop('V2_EXE_STATIC', None)
            result = subprocess.run([str(directory / exe.name), '--max-frames=30'],
                                    cwd=directory, env=env, capture_output=True, timeout=30)
            output = result.stdout + result.stderr
            self.assertEqual(result.returncode, 0, output[-4000:])
            return output

    @unittest.skipUnless(os.environ.get('VIKINGS_TEST_EXE'), 'set VIKINGS_TEST_EXE to run')
    def test_oversized_chunk_aborts_startup(self):
        output = self.run_bad_chunk(lambda offset: (offset, offset + 0xB080))
        self.assertIn(b'V2-CHUNK-FATAL: id=0x4', output)
        self.assertIn(b'*** An Error has occured', output)
        self.assertNotIn(b'V2-STARTUP: complete', output)
        self.assertNotIn(b'max-frames reached', output)

    @unittest.skipUnless(os.name == 'nt' and os.environ.get('VIKINGS_TEST_EXE'),
                         'requires Windows and VIKINGS_TEST_EXE')
    def test_failed_seek_aborts_startup(self):
        # Windows long is 32-bit: this offset becomes -1 for fseek(SEEK_SET).
        output = self.run_bad_chunk(lambda offset: (0xFFFFFFFF, 0))
        self.assertIn(b'Unable to read data file', output)
        self.assertIn(b'Error code is: 0019', output)
        self.assertNotIn(b'V2-STARTUP: complete', output)
        self.assertNotIn(b'max-frames reached', output)



if __name__ == '__main__':
    unittest.main()
