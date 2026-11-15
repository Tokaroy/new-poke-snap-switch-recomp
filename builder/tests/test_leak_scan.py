"""Tests for builder/leak_scan.py with fake packages (no game data)."""
import hashlib
import json
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import leak_scan  # noqa: E402


def sha(b):
    return hashlib.sha256(b).hexdigest()


class LeakScanTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name)
        self.pkg = self.d / 'pkg'
        (self.pkg / 'bin').mkdir(parents=True)
        (self.pkg / 'obj').mkdir()
        self.romfs = self.d / 'romfs'
        (self.romfs / 'a').mkdir(parents=True)
        (self.romfs / 'a' / 'secret_level.drp').write_bytes(b'x')
        (self.romfs / 'a' / 'voice_001.wem').write_bytes(b'x')
        self.dll = b'MZ fake dll'
        (self.pkg / 'bin' / 'lib.dll').write_bytes(self.dll)
        (self.pkg / 'obj' / 'a.obj').write_bytes(b'clean object bytes')
        self.manifest = {'dlls': [{'name': 'lib.dll', 'sha256': sha(self.dll), 'license': 'MIT (licenses/lib-LICENSE.txt)'}]}
        (self.pkg / 'licenses').mkdir()
        (self.pkg / 'licenses' / 'lib-LICENSE.txt').write_bytes(b'MIT text')
        (self.pkg / 'manifest.json').write_text(json.dumps(self.manifest))

    def tearDown(self):
        self.tmp.cleanup()

    def scan(self, target=None, **kw):
        kw.setdefault('terms', ['Zork', 'Quux'])
        kw.setdefault('romfs', self.romfs)
        kw.setdefault('manifest', self.manifest)
        return leak_scan.scan(target or self.pkg, **kw)

    def rules(self, **kw):
        return sorted({f[1] for f in self.scan(**kw)[0]})

    def test_clean_package_passes(self):
        f, n, nd = self.scan()
        self.assertEqual(f, [])
        self.assertEqual(nd, 1)

    def test_bad_extensions(self):
        for name in ('x.tsv', 'm.nso', 'k.keys', 's.spv', 'p.profdata', 'c.nca', 'g.xci', 'n.nsp'):
            (self.pkg / name).write_bytes(b'x')
        f = self.scan()[0]
        self.assertEqual(len([x for x in f if x[1] == 'extension']), 8)

    def test_game_file_name_as_file_and_in_content(self):
        (self.pkg / 'obj' / 'voice_001.wem.txt').write_bytes(b'x')
        (self.pkg / 'obj' / 'a.obj').write_bytes(b'blah blah secret_level.drp blah')
        r = self.rules()
        self.assertIn('game-file-name-in-content', r)
        (self.pkg / 'voice_001.wem').write_bytes(b'x')
        self.assertIn('game-file-name', self.rules())

    def test_allowed_names_pass(self):
        (self.pkg / 'obj' / 'a.obj').write_bytes(b'path/secret_level.drp')
        self.assertEqual(self.scan(allow_names=['secret_level.drp'])[0], [])

    def test_personal_terms_and_home_paths(self):
        (self.pkg / 'a.txt').write_bytes(b'made by zork the great')
        (self.pkg / 'b.txt').write_bytes('Q\x00u\x00u\x00x\x00'.encode('latin-1'))
        (self.pkg / 'c.txt').write_bytes(b'C:\\Users\\somebody\\project\\file.cpp')
        (self.pkg / 'd.txt').write_bytes(b'contact me: someone@example.com')
        (self.pkg / 'e.txt').write_bytes(b'12345678+user@users.noreply.github.com')
        (self.pkg / 'Zork.txt').write_bytes(b'ok')
        f = self.scan()[0]
        by_file = {(x[0], x[1]) for x in f}
        self.assertIn(('a.txt', 'personal'), by_file)
        self.assertIn(('b.txt', 'personal'), by_file)
        self.assertIn(('c.txt', 'home-path'), by_file)
        self.assertIn(('d.txt', 'email'), by_file)
        self.assertNotIn(('e.txt', 'email'), by_file)
        self.assertIn(('Zork.txt', 'personal'), by_file)

    def test_utf16_home_path(self):
        (self.pkg / 'w.bin').write_bytes('C:\\Users\\somebody\\x'.encode('utf-16-le'))
        self.assertIn('home-path', self.rules())

    def test_size_cap_and_unlisted_dll(self):
        (self.pkg / 'big.bin').write_bytes(b'0' * (2 * 2**20))
        (self.pkg / 'other.dll').write_bytes(b'MZ')
        f = self.scan(size_cap_mb=1)[0]
        rules = [(x[0], x[1]) for x in f]
        self.assertIn(('big.bin', 'size'), rules)
        self.assertIn(('other.dll', 'dll'), rules)

    def test_dll_hash_mismatch_and_missing(self):
        (self.pkg / 'bin' / 'lib.dll').write_bytes(b'MZ changed')
        self.assertIn('dll', self.rules())
        (self.pkg / 'bin' / 'lib.dll').unlink()
        self.assertIn('dll', self.rules())

    def test_big_listed_dll_is_fine(self):
        big = b'MZ' + b'0' * (3 * 2**20)
        (self.pkg / 'bin' / 'lib.dll').write_bytes(big)
        man = {'dlls': [{'name': 'lib.dll', 'sha256': sha(big), 'license': 'MIT'}]}
        self.assertEqual(self.scan(size_cap_mb=1, manifest=man)[0], [])

    def test_game_title_in_file_name(self):
        (self.pkg / 'Pokemon_Tool.txt').write_bytes(b'x')
        (self.pkg / 'pokémon.txt').write_bytes(b'x')
        f = [x for x in self.scan()[0] if x[1] == 'name']
        self.assertEqual(len(f), 2)

    def test_allow_content_is_pinned_to_file_and_kind(self):
        (self.pkg / 'notes.txt').write_bytes(b'this mentions Zork once')
        (self.pkg / 'other.txt').write_bytes(b'this mentions Zork too')
        self.assertEqual(len([x for x in self.scan()[0] if x[1] == 'personal']), 2)
        f = self.scan(allow_content=['notes.txt|personal'])[0]
        self.assertEqual([x[0] for x in f if x[1] == 'personal'], ['other.txt'])
        f = self.scan(allow_content=['notes.txt|home-path'])[0]   # another kind: still flagged
        self.assertEqual(len([x for x in f if x[1] == 'personal']), 2)

    def test_dll_license_file_must_ship(self):
        (self.pkg / 'licenses' / 'lib-LICENSE.txt').unlink()
        f = [x for x in self.scan()[0] if x[1] == 'license']
        self.assertEqual(len(f), 1)
        self.assertIn('lib-LICENSE.txt', f[0][2])

    def test_dll_without_license_entry(self):
        man = {'dlls': [{'name': 'lib.dll', 'sha256': sha(self.dll)}]}
        self.assertEqual(len([x for x in self.scan(manifest=man)[0] if x[1] == 'license']), 1)

    def test_vendor_upscaler_binaries_are_never_shipped(self):
        for n in ('amd_fidelityfx_upscaler_vk.dll', 'nvngx_dlss.dll', 'libxess.dll'):
            (self.pkg / 'bin' / n).write_bytes(b'MZ x')
        f = [x for x in self.scan()[0] if x[1] == 'vendor-binary']
        self.assertEqual(len(f), 3)

    def test_zip_target(self):
        z = self.d / 'rel.zip'
        with zipfile.ZipFile(z, 'w') as zf:
            zf.writestr('ok.txt', 'fine')
            zf.writestr('keys/prod.keys', 'secret')
            zf.writestr('lib.dll', self.dll)
        f = self.scan(target=z)[0]
        self.assertEqual({x[1] for x in f}, {'extension', 'dll', 'license'})   # lib.dll sits outside bin/ and no manifest is in the zip


if __name__ == '__main__':
    unittest.main()
