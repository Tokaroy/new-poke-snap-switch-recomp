"""Unit tests for the version check with fake inputs (no game data). Run: python -I -m unittest discover -s builder/tests"""
import hashlib
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from snapbuild import nso as nsomod  # noqa: E402
from snapbuild import versioncheck as vc  # noqa: E402


def lz4_literals(raw):
    """A valid LZ4 block made of one literal run (no matches)."""
    n = len(raw)
    tok = bytearray([min(n, 15) << 4])
    if n >= 15:
        r = n - 15
        while r >= 255:
            tok.append(255)
            r -= 255
        tok.append(r)
    return bytes(tok) + raw


def make_nso(path, build_id_hex, segs, compress=(False, False, False), check_hashes=True, magic=b'NSO0', truncate=0):
    flags = sum(1 << i for i in range(3) if compress[i]) | (0x38 if check_hashes else 0)
    blobs = [lz4_literals(s) if c else s for s, c in zip(segs, compress)]
    head = bytearray(0x100)
    head[0:4] = magic
    struct.pack_into('<I', head, 0xC, flags)
    off, mem = 0x100, 0
    for i, (s, b) in enumerate(zip(segs, blobs)):
        struct.pack_into('<3I', head, 0x10 + 0x10 * i, off, mem, len(s))
        struct.pack_into('<I', head, 0x60 + 4 * i, len(b))
        head[0xA0 + 32 * i:0xA0 + 32 * (i + 1)] = hashlib.sha256(s).digest()
        off += len(b)
        mem += len(s)
    head[0x40:0x40 + 20] = bytes.fromhex(build_id_hex)
    data = bytes(head) + b''.join(blobs)
    if truncate:
        data = data[:-truncate]
    Path(path).write_bytes(data)


def image(segs):
    return hashlib.sha256(b''.join(segs)).hexdigest()


MAIN_SEGS = [b'T' * 5000 + b'abc', b'R' * 700, b'D' * 90]
SDK_SEGS = [b'S' * 3000, b'r' * 64, b'd' * 32]
ID_GOOD_MAIN = 'AA' * 20
ID_GOOD_SDK = 'BB' * 20
ID_OLD_MAIN = '11' * 20


def fake_romfs(root, files):
    for rel, size in files.items():
        p = Path(root) / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(b'x' * size)


ROMFS = {f'dir{i % 3}/sub/file{i:02d}.bin': 10 + i for i in range(12)}


def make_table():
    listing = sorted((k.lower(), v) for k, v in ROMFS.items())
    return {
        'nso': {'main': {'build_id': ID_GOOD_MAIN, 'image_sha256': image(MAIN_SEGS)},
                'sdk': {'build_id': ID_GOOD_SDK, 'image_sha256': image(SDK_SEGS)}},
        'known_versions': [
            {'name': '1.0.0 (v0)', 'main_build_id': ID_OLD_MAIN, 'sdk_build_id': ID_GOOD_SDK},
            {'name': '2.0.0 (v196608)', 'main_build_id': ID_GOOD_MAIN, 'sdk_build_id': ID_GOOD_SDK},
            {'name': vc.EXPECTED_NAME, 'main_build_id': ID_GOOD_MAIN, 'sdk_build_id': ID_GOOD_SDK}],
        'romfs': {'file_count': len(listing), 'listing_sha256': vc.listing_hash(listing),
                  'sentinels': [{'path_sha256': vc.path_key(r), 'size': s} for r, s in listing[::2]]},
    }


class NsoTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name)
        self.table = make_table()

    def tearDown(self):
        self.tmp.cleanup()

    def problems(self, name, role='main'):
        return vc.check_nso(self.d / name, role, self.table)[0]

    def test_good_uncompressed_and_compressed_agree(self):
        make_nso(self.d / 'a', ID_GOOD_MAIN, MAIN_SEGS)
        make_nso(self.d / 'b', ID_GOOD_MAIN, MAIN_SEGS, compress=(True, True, True))
        make_nso(self.d / 'c', ID_GOOD_MAIN, MAIN_SEGS, compress=(True, False, True), check_hashes=False)
        for n in 'abc':
            self.assertEqual(self.problems(n), [], n)

    def test_folder(self):
        (self.d / 'f').mkdir()
        p = self.problems('f')
        self.assertIn('folder', p[0])

    def test_missing(self):
        self.assertIn('not found', self.problems('nope')[0])

    def test_encrypted(self):
        (self.d / 'enc').write_bytes(os.urandom(4096))
        self.assertIn('encrypted', self.problems('enc')[0])

    def test_nca(self):
        (self.d / 'nca').write_bytes(b'\0' * 0x200 + b'NCA3' + b'\0' * 1000)
        self.assertIn('NCA', self.problems('nca')[0])

    def test_tiny(self):
        (self.d / 'tiny').write_bytes(b'NSO0')
        self.assertIn('too small', self.problems('tiny')[0])

    def test_truncated(self):
        make_nso(self.d / 't', ID_GOOD_MAIN, MAIN_SEGS, truncate=20)
        self.assertIn('truncated', self.problems('t')[0])

    def test_wrong_known_version_is_named(self):
        make_nso(self.d / 'old', ID_OLD_MAIN, MAIN_SEGS)
        p = self.problems('old')
        self.assertIn('1.0.0', p[0])
        self.assertIn('2.0.1', p[0])

    def test_unknown_build(self):
        make_nso(self.d / 'x', 'CC' * 20, MAIN_SEGS)
        self.assertIn('not from game version 2.0.1', self.problems('x')[0])

    def test_modified_contents(self):
        segs = [MAIN_SEGS[0][:-1] + b'Z', MAIN_SEGS[1], MAIN_SEGS[2]]
        make_nso(self.d / 'm', ID_GOOD_MAIN, segs)
        self.assertIn('modified or damaged', self.problems('m')[0])

    def test_damaged_header_hash(self):
        make_nso(self.d / 'dm', ID_GOOD_MAIN, MAIN_SEGS)
        data = bytearray((self.d / 'dm').read_bytes())
        data[0x100 + 10] ^= 0xFF
        (self.d / 'dm').write_bytes(bytes(data))
        self.assertIn('damaged', self.problems('dm')[0])

    def test_2_0_0_main_matches_both(self):
        make_nso(self.d / 'm', ID_GOOD_MAIN, MAIN_SEGS)
        p, matched = vc.check_nso(self.d / 'm', 'main', self.table)
        self.assertEqual(p, [])
        self.assertTrue(any(v.startswith('2.0.0') for v in matched))

    def test_no_user_hash_in_messages(self):
        segs = [MAIN_SEGS[0][:-1] + b'Z', MAIN_SEGS[1], MAIN_SEGS[2]]
        make_nso(self.d / 'm', ID_GOOD_MAIN, segs)
        make_nso(self.d / 'u', 'CC' * 20, MAIN_SEGS)
        for n in ('m', 'u'):
            for msg in self.problems(n):
                self.assertNotIn(image(segs), msg)
                self.assertNotIn(image(MAIN_SEGS), msg)
                self.assertNotIn('CC' * 20, msg)

    def test_lz4_fallback_decoder(self):
        raw = b'hello world, ' * 40
        # hand-made block: 13 literals "hello world, ", then a match of length 13*39 at offset 13 (overlap handled bytewise)
        ml = len(raw) - 13
        blk = bytearray([0xDF]) + b'hello world, ' + struct.pack('<H', 13)
        r = ml - 4 - 15
        while r >= 255:
            blk.append(255)
            r -= 255
        blk.append(r)
        self.assertEqual(nsomod.lz4_block_decompress(bytes(blk), len(raw)), raw)
        self.assertEqual(nsomod.lz4_block_decompress(lz4_literals(raw), len(raw)), raw)
        try:
            import lz4.block
            data = os.urandom(300) + b'abcabcabc' * 200
            self.assertEqual(nsomod.lz4_block_decompress(lz4.block.compress(data, store_size=False), len(data)), data)
        except ImportError:
            pass


class RomfsTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name)
        self.table = make_table()

    def tearDown(self):
        self.tmp.cleanup()

    def test_good_and_wrapper_folder(self):
        fake_romfs(self.d / 'r', ROMFS)
        self.assertEqual(vc.check_romfs(self.d / 'r', self.table)[0], [])
        fake_romfs(self.d / 'w' / 'romfs', ROMFS)
        p, root = vc.check_romfs(self.d / 'w', self.table)
        self.assertEqual(p, [])
        self.assertEqual(root.name, 'romfs')

    def test_case_insensitive(self):
        fake_romfs(self.d / 'r', {k.upper().replace('.BIN', '.bin'): v for k, v in ROMFS.items()})
        self.assertEqual(vc.check_romfs(self.d / 'r', self.table)[0], [])

    def test_missing_folder_and_empty(self):
        self.assertIn('not found', vc.check_romfs(self.d / 'none', self.table)[0][0])
        (self.d / 'e').mkdir()
        self.assertIn('no files', vc.check_romfs(self.d / 'e', self.table)[0][0])

    def test_wrong_folder_entirely(self):
        fake_romfs(self.d / 'o', {'other/a.txt': 5})
        self.assertIn('does not look like', vc.check_romfs(self.d / 'o', self.table)[0][0])

    def test_base_only(self):
        files = dict(list(ROMFS.items())[:6])
        fake_romfs(self.d / 'b', files)
        msg = vc.check_romfs(self.d / 'b', self.table)[0][0]
        self.assertIn('apply the 2.0.1 update', msg)
        self.assertNotIn('same in 2.0.0', msg)
        msg2 = vc.check_romfs(self.d / 'b', self.table, main_is_2_0_x=True)[0][0]
        self.assertIn('same in 2.0.0 and 2.0.1', msg2)

    def test_wrong_size(self):
        files = dict(ROMFS)
        k = sorted(files)[0]
        files[k] += 1
        fake_romfs(self.d / 's', files)
        self.assertIn('wrong size', vc.check_romfs(self.d / 's', self.table)[0][0])

    def test_extra_file_changes_listing(self):
        fake_romfs(self.d / 'x', dict(ROMFS, **{'extra.bin': 3}))
        self.assertIn('differs', vc.check_romfs(self.d / 'x', self.table)[0][0])

    def test_check_all(self):
        make_nso(self.d / 'main', ID_GOOD_MAIN, MAIN_SEGS)
        make_nso(self.d / 'sdk', ID_GOOD_SDK, SDK_SEGS, compress=(True, True, True))
        fake_romfs(self.d / 'rom', ROMFS)
        r = vc.check_all(self.d / 'main', self.d / 'sdk', self.d / 'rom', self.table)
        self.assertTrue(r.ok, r.problems)
        self.assertTrue(r.notes)
        (self.d / 'sdk').write_bytes(b'junk' * 100)
        r = vc.check_all(self.d / 'main', self.d / 'sdk', self.d / 'rom', self.table)
        self.assertFalse(r.ok)
        self.assertTrue(r.problems[0].startswith('sdk file'))


if __name__ == '__main__':
    unittest.main()
