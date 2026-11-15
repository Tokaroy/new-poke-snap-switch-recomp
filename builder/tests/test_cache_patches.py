import hashlib
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from snapbuild import cache, patches


def mk(folder, files):
    for n, t in files.items():
        p = Path(folder) / n
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(t)


class CacheTests(unittest.TestCase):
    def setUp(self):
        self.t = Path(tempfile.mkdtemp())
        self.root = self.t / 'cache'

    def test_store_restore(self):
        src = self.t / 'src'
        mk(src, {'a.txt': 'x', 'sub/b.txt': 'y'})
        cache.store(self.root, 'maps', 'k1', src)
        self.assertTrue(cache.has(self.root, 'maps', 'k1'))
        dst = self.t / 'dst'
        self.assertTrue(cache.restore(self.root, 'maps', 'k1', dst))
        self.assertEqual((dst / 'sub' / 'b.txt').read_text(), 'y')
        self.assertFalse((dst / '.complete').exists())
        self.assertFalse(cache.restore(self.root, 'maps', 'nokey', self.t / 'd2'))

    def test_keeps_two_snapshots(self):
        src = self.t / 'src'
        mk(src, {'a': '1'})
        for i, k in enumerate(('k1', 'k2', 'k3')):
            cache.store(self.root, 'gen', k, src)
            os.utime(self.root / 'gen' / k / '.complete', (1000 + i, 1000 + i))
        cache.evict(self.root, 'gen', keep_key='k3')
        self.assertEqual(sorted(p.name for p in (self.root / 'gen').iterdir()), ['k2', 'k3'])

    def test_size_cap(self):
        src = self.t / 'src'
        mk(src, {'a': 'x' * 5000})
        cache.store(self.root, 'a', 'k1', src)
        os.utime(self.root / 'a' / 'k1' / '.complete', (1000, 1000))
        cache.store(self.root, 'b', 'k2', src)
        cache.evict(self.root, 'b', keep_key='k2', cap=7000)
        self.assertFalse(cache.has(self.root, 'a', 'k1'))
        self.assertTrue(cache.has(self.root, 'b', 'k2'))

    def test_refuses_unmarked_nonempty_root(self):
        self.root.mkdir()
        (self.root / 'precious.txt').write_text('keep')
        with self.assertRaises(cache.CacheError):
            cache.open_root(self.root)
        self.assertTrue((self.root / 'precious.txt').exists())

    def test_refuses_link_in_source(self):
        src = self.t / 'src'
        mk(src, {'a': '1'})
        tgt = self.t / 'target'
        tgt.mkdir()
        try:
            os.symlink(tgt, src / 'lnk', target_is_directory=True)
        except (OSError, NotImplementedError):
            self.skipTest('no symlink privilege')
        with self.assertRaises(cache.CacheError):
            cache.store(self.root, 'maps', 'k', src)

    def test_incomplete_entry_not_used(self):
        cache.open_root(self.root)
        mk(self.root / 'maps' / 'k', {'a': '1'})
        self.assertFalse(cache.has(self.root, 'maps', 'k'))
        self.assertFalse(cache.restore(self.root, 'maps', 'k', self.t / 'd'))


class PatchTests(unittest.TestCase):
    def setUp(self):
        self.img = bytes(range(256)) * 4
        self.p = {'name': 't', 'image_sha256': hashlib.sha256(self.img).hexdigest(), 'build_id': 'AB12', 'offset': 16,
                  'original_sha256': hashlib.sha256(self.img[16:20]).hexdigest(), 'replacement_hex': '00112233'}

    def test_ok(self):
        self.assertTrue(patches.check(self.p, self.img, build_id='ab12'))

    def test_wrong_image(self):
        with self.assertRaises(patches.PatchError):
            patches.check(self.p, self.img + b'x')

    def test_wrong_bytes(self):
        with self.assertRaises(patches.PatchError):
            patches.check(dict(self.p, offset=17), self.img)

    def test_wrong_build_id(self):
        with self.assertRaises(patches.PatchError):
            patches.check(self.p, self.img, build_id='ffff')

    def test_span_outside(self):
        with self.assertRaises(patches.PatchError):
            patches.check(dict(self.p, offset=10000), self.img)

    def test_expected_set(self):
        d = Path(tempfile.mkdtemp())
        (d / 'a.json').write_text('{}')
        self.assertTrue(patches.check_expected_set(d, ['a.json']))
        with self.assertRaises(patches.PatchError):
            patches.check_expected_set(d, ['a.json', 'b.json'])


if __name__ == '__main__':
    unittest.main()
