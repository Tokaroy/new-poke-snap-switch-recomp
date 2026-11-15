"""Upscaler fetch tests: license first, no install without acceptance, hash checks, zip member extraction. No network."""
import io
import json
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE))
from snapbuild import fetch, upscalers as up  # noqa: E402
from test_fetch import FakeNet, sha  # noqa: E402


def make_table():
    dll = b'DLL' * 100
    lic = b'license text'
    note = b'notice'
    zbuf = io.BytesIO()
    with zipfile.ZipFile(zbuf, 'w') as z:
        z.writestr('bin/x.dll', b'XESS' * 50)
        z.writestr('third.txt', b'third')
        z.writestr('junk/huge.bin', b'J' * 5000)
    zdata = zbuf.getvalue()
    table = {'upscalers': {
        'one': {'title': 'One', 'vendor': 'A', 'default': True,
                'license': {'filename': 'A-LICENSE.txt', 'url': 'https://h/a-license', 'sha256': sha(lic), 'size': len(lic)},
                'files': [{'url': 'https://h/one.dll', 'dest': 'upscalers/one.dll', 'sha256': sha(dll), 'size': len(dll)},
                          {'url': 'https://h/note', 'dest': 'licenses/upscalers/one-note.md', 'sha256': sha(note), 'size': len(note)}]},
        'two': {'title': 'Two', 'vendor': 'B', 'default': False,
                'license': {'filename': 'B-LICENSE.txt', 'url': 'https://h/b-license', 'sha256': sha(lic), 'size': len(lic)},
                'archive': {'url': 'https://h/two.zip', 'filename': 'two.zip', 'sha256': sha(zdata), 'size': len(zdata)},
                'files': [{'member': 'bin/x.dll', 'dest': 'x.dll', 'sha256': sha(b'XESS' * 50), 'size': 200},
                          {'member': 'third.txt', 'dest': 'licenses/upscalers/third.txt', 'sha256': sha(b'third'), 'size': 5}]},
    }}
    net = {'https://h/a-license': lic, 'https://h/b-license': lic, 'https://h/one.dll': dll, 'https://h/note': note, 'https://h/two.zip': zdata}
    return table, net


class UpscalerTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name)
        self.table, self.files = make_table()
        self.inst = self.d / 'out'
        self.dl = self.d / 'dl'
        self.say = lambda s: None

    def tearDown(self):
        self.tmp.cleanup()

    def test_default_names(self):
        self.assertEqual(up.default_names(self.table), ['one'])

    def test_declined_license_installs_nothing(self):
        net = FakeNet(self.files)
        r = up.run(self.table, ['one'], self.inst, self.dl, set(), lambda q: False, opener=net, say=self.say)
        self.assertEqual(r, {'one': 'declined'})
        self.assertEqual(net.calls, ['https://h/a-license'])   # only the license text was fetched
        self.assertFalse((self.inst / 'upscalers' / 'one.dll').exists())

    def test_license_is_shown_before_asking(self):
        order = []
        up.run(self.table, ['one'], self.inst, self.dl, set(), lambda q: (order.append('ask'), False)[1], opener=FakeNet(self.files),
               say=self.say, show_license=lambda n, p: order.append('show'))
        self.assertEqual(order, ['show', 'ask'])

    def test_accepted_installs_files_license_and_receipt(self):
        r = up.run(self.table, ['one'], self.inst, self.dl, {'one'}, lambda q: self.fail('no question when accepted'), opener=FakeNet(self.files), say=self.say)
        self.assertEqual(r, {'one': 'installed'})
        self.assertEqual((self.inst / 'upscalers' / 'one.dll').read_bytes(), b'DLL' * 100)
        self.assertTrue((self.inst / 'licenses' / 'upscalers' / 'A-LICENSE.txt').exists())
        rec = json.loads((self.inst / 'upscalers.json').read_text())
        self.assertTrue(rec['one']['license_accepted_by_user'])
        self.assertTrue(up.installed(self.table['upscalers']['one'], self.inst))

    def test_second_run_downloads_nothing(self):
        up.run(self.table, ['one'], self.inst, self.dl, {'one'}, lambda q: True, opener=FakeNet(self.files), say=self.say)
        net = FakeNet(fail=True)
        r = up.run(self.table, ['one'], self.inst, self.dl, set(), lambda q: self.fail('no'), opener=net, say=self.say)
        self.assertEqual(r, {'one': 'already installed'})
        self.assertEqual(net.calls, [])

    def test_tampered_download_is_refused_and_leaves_nothing(self):
        files = dict(self.files)
        files['https://h/one.dll'] = b'EVIL' * 100
        with self.assertRaises(fetch.FetchError):
            up.run(self.table, ['one'], self.inst, self.dl, {'one'}, lambda q: True, opener=FakeNet(files), say=self.say)
        self.assertFalse((self.inst / 'upscalers' / 'one.dll').exists())

    def test_tampered_license_is_refused(self):
        files = dict(self.files)
        files['https://h/a-license'] = b'different'
        with self.assertRaises(fetch.FetchError):
            up.run(self.table, ['one'], self.inst, self.dl, {'one'}, lambda q: True, opener=FakeNet(files), say=self.say)

    def test_zip_members_only_and_zip_deleted(self):
        r = up.run(self.table, ['two'], self.inst, self.dl, {'two'}, lambda q: True, opener=FakeNet(self.files), say=self.say)
        self.assertEqual(r, {'two': 'installed'})
        self.assertEqual((self.inst / 'x.dll').read_bytes(), b'XESS' * 50)
        self.assertFalse((self.inst / 'junk').exists())          # the rest of the archive is not kept
        self.assertFalse((self.dl / 'two.zip').exists())          # and the archive is deleted

    def test_zip_member_with_wrong_hash(self):
        self.table['upscalers']['two']['files'][0]['sha256'] = '0' * 64
        with self.assertRaises(fetch.FetchError):
            up.run(self.table, ['two'], self.inst, self.dl, {'two'}, lambda q: True, opener=FakeNet(self.files), say=self.say)
        self.assertFalse((self.inst / 'x.dll').exists())

    def test_unknown_name(self):
        with self.assertRaises(fetch.FetchError):
            up.run(self.table, ['nope'], self.inst, self.dl, set(), lambda q: True, opener=FakeNet(self.files), say=self.say)

    def test_real_table_pins_are_complete_and_inside_install_dir(self):
        table = json.loads((HERE.parent / 'versions.json').read_text(encoding='utf-8'))
        e = table['upscalers']
        self.assertEqual(set(e), {'fsr', 'dlss', 'xess'})
        self.assertEqual(up.default_names(table), ['fsr'])   # FSR is on by default; DLSS and XeSS are offered, not ticked
        for k, v in e.items():
            self.assertTrue(v['license']['url'].startswith('https://'), k)
            self.assertRegex(v['license']['sha256'], r'^[0-9a-f]{64}$')
            self.assertTrue(v.get('source'), k)
            for f in v['files']:
                self.assertRegex(f['sha256'], r'^[0-9a-f]{64}$')
                self.assertNotIn('..', f['dest'])
                self.assertFalse(Path(f['dest']).is_absolute())
                if 'archive' not in v:
                    self.assertTrue(f['url'].startswith('https://'))
            if 'archive' in v:
                self.assertRegex(v['archive']['sha256'], r'^[0-9a-f]{64}$')


if __name__ == '__main__':
    unittest.main()
