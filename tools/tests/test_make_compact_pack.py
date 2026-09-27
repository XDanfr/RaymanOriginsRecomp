"""Tests for tools/make_compact_pack.py on a tiny made-up game folder (no game
data): two bundles that carry the same entry, as the real levels do.

Run: python -m unittest discover -s tools/tests
"""
import os
import re
import struct
import subprocess
import sys
import tempfile
import unittest

SCRIPT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'make_compact_pack.py')


def ipk(tag, entry):
    """A minimal UbiArt IPK v3: one entry whose data starts right after the index."""
    base = 0x50
    head = struct.pack('>I', 0x50EC12BA) + tag.ljust(8, b'\0') + struct.pack('>I', base)
    head = head.ljust(0x30, b'\0')
    index = struct.pack('>III', 1, len(entry), 0) + b'\0' * 8 + struct.pack('>Q', 0) + struct.pack('>I', 0)
    assert len(head) + len(index) == base
    return head + index + entry


class CompactPackTest(unittest.TestCase):
    def pack(self, files):
        with tempfile.TemporaryDirectory() as tmp:
            game = os.path.join(tmp, 'game')
            for rel, data in files.items():
                path = os.path.join(game, rel)
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with open(path, 'wb') as f:
                    f.write(data)
            out = os.path.join(tmp, 'out.rdpk')
            run = subprocess.run([sys.executable, SCRIPT, game, out], capture_output=True, text=True)
            with open(out, 'rb') as f:
                return run, f.read()

    def test_shared_entries_are_stored_once_and_files_rebuild(self):
        shared = os.urandom(4096)
        files = {
            'default.xex': os.urandom(1000),
            'world/level1.ipk': ipk(b'level1', shared),
            'world/level2.ipk': ipk(b'level2', shared),
        }
        run, pack = self.pack(files)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn('every file rebuilds byte for byte', run.stdout)
        self.assertEqual(pack[:4], b'RDPK')
        version, count = struct.unpack('<II', pack[4:12])
        self.assertEqual((version, count), (1, 3))
        # xex, the two bundle headers and the shared entry: 4 runs, not 5.
        self.assertEqual(int(re.search(r'\((\d+) unique runs\)', run.stdout).group(1)), 4)
        self.assertLess(len(pack), sum(len(d) for d in files.values()))

    def test_refuses_a_folder_without_the_game(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = subprocess.run([sys.executable, SCRIPT, tmp, os.path.join(tmp, 'out.rdpk')],
                                 capture_output=True, text=True)
            self.assertNotEqual(run.returncode, 0)
            self.assertIn('default.xex not found', run.stderr)


if __name__ == '__main__':
    unittest.main()
