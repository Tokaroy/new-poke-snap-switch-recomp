"""HD pack compression (snapbuild/packcompress.py): format detection, compact command batches and, on an NTFS drive,
a real compress / undo round trip on synthetic files. No game data."""
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from snapbuild import packcompress as pc  # noqa: E402


def fake_nutexb(path, fmt, size):
    """Compressible stand-in: repeating payload, then a 0xb0 footer with the format byte at footer offset 0x90."""
    body = (b'\x00\x01\x02\x03' * 64) * (size // 256)
    footer = bytearray(0xb0)
    footer[0x90] = fmt
    Path(path).write_bytes(body + bytes(footer))


class PackCompress(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        for i, f in enumerate((0xe0, 0xe0, 0x80, 0xd8)):
            fake_nutexb(self.root / ('t%d.nutexb' % i), f, 256 * 1024)
        (self.root / 'readme.txt').write_text('x')

    def tearDown(self):
        self.tmp.cleanup()

    def test_find_and_format(self):
        files = pc.find_files([str(self.root)])
        self.assertEqual(len(files), 4)
        names = sorted(pc.format_of(p, os.path.getsize(p)) for p in files)
        self.assertEqual(names, ['BC1', 'BC6H signed', 'BC7', 'BC7'])
        self.assertEqual(pc.format_of(str(self.root / 'readme.txt'), 1), 'other')

    def test_calls(self):
        files = pc.find_files([str(self.root)])
        c = pc.build_calls(files, 'lzx')
        self.assertEqual(len(c), 1)
        self.assertEqual(c[0][0][:4], ['compact', '/C', '/EXE:LZX', '/I'])
        u = pc.build_calls(files, undo=True)
        self.assertEqual(len(u), 2)   # plain /U and /U /EXE
        many = [str(self.root / ('f%d.nutexb' % i)) for i in range(250)]
        self.assertEqual(len(pc.build_calls(many)), 3)

    @unittest.skipUnless(os.name == 'nt', 'NTFS compression is Windows only')
    def test_round_trip(self):
        if not pc.is_ntfs(str(self.root)):
            self.skipTest('temp folder is not on NTFS')
        files = pc.find_files([str(self.root)])
        plain = pc.totals(files)['(all)'][2]
        for algo in ('lznt1', 'xpress8k'):
            self.assertEqual(pc.run([str(self.root)], algo, say=lambda s: None), 0)
            self.assertLess(pc.totals(files)['(all)'][2], plain * 0.5, algo)
            self.assertEqual(pc.run([str(self.root)], undo=True, say=lambda s: None), 0)
            self.assertEqual(pc.totals(files)['(all)'][2], plain, algo)
        self.assertEqual(Path(files[0]).stat().st_size, 256 * 1024 + 0xb0)   # logical size untouched


if __name__ == '__main__':
    unittest.main()
