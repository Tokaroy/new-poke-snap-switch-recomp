"""Unit tests for the fetch and verify logic and the pinned-hash Python environment. No network: downloads go through a fake opener."""
import hashlib
import io
import json
import sys
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
from snapbuild import fetch, pyenv  # noqa: E402


def sha(b):
    return hashlib.sha256(b).hexdigest()


class FakeResp:
    def __init__(self, data):
        self.b = io.BytesIO(data)

    def read(self, n=-1):
        return self.b.read(n)

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False


class FakeNet:
    """opener= replacement that records calls."""

    def __init__(self, files=None, fail=False):
        self.files = files or {}
        self.calls = []
        self.fail = fail

    def __call__(self, url):
        self.calls.append(url)
        if self.fail:
            raise OSError('network down')
        return FakeResp(self.files[url])


def pin_for(name, data, **kw):
    d = {'filename': name, 'url': 'https://example.invalid/' + name, 'sha256': sha(data), 'size': len(data)}
    d.update(kw)
    return d


class FetchTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name)
        self.said = []

    def tearDown(self):
        self.tmp.cleanup()

    def say(self, s):
        self.said.append(s)

    def test_sha256_file(self):
        p = self.d / 'a.bin'
        p.write_bytes(b'abc')
        self.assertEqual(fetch.sha256_file(p), sha(b'abc'))

    def test_is_pinned(self):
        ok = {'url': 'https://x/y.zip', 'sha256': 'a' * 64}
        self.assertTrue(fetch.is_pinned(ok))
        self.assertFalse(fetch.is_pinned({**ok, 'sha256': 'TODO'}))
        self.assertFalse(fetch.is_pinned({**ok, 'url': 'TODO'}))
        self.assertFalse(fetch.is_pinned({**ok, 'url': 'http://x/y.zip'}))   # https only
        self.assertFalse(fetch.is_pinned({**ok, 'sha256': 'a' * 63}))
        self.assertFalse(fetch.is_pinned({}))

    def test_real_table_is_fully_pinned(self):
        table = json.loads((HERE.parent / 'versions.json').read_text(encoding='utf-8'))
        self.assertEqual(fetch.unpinned(table), [])
        keys = [k for k, _ in fetch.list_pins(table)]
        for k in ('llvm', 'ghidra', 'switch_loader', 'jdk', 'python_embed', 'wheel:capstone', 'wheel:numpy', 'wheel:lz4', 'wheel:pyelftools', 'wheel:pip'):
            self.assertIn(k, keys)
        for k, pin in fetch.list_pins(table):
            self.assertTrue(pin['url'].startswith('https://'), k)
            self.assertIn(pin['filename'].replace('+', '%2B'), pin['url'].replace('+', '%2B'), k)
            self.assertTrue(pin.get('source') or k.startswith('wheel:'), k + ' needs a source note')

    def test_unpinned_lists_todo(self):
        t = {'tools': {'a': {'url': 'TODO', 'sha256': 'TODO'}, 'b': {'url': 'https://x/b', 'sha256': 'b' * 64}}}
        self.assertEqual(fetch.unpinned(t), ['a'])

    def test_download_ok(self):
        data = b'payload' * 1000
        net = FakeNet({'https://h/f.bin': data})
        dest = fetch.download('https://h/f.bin', self.d / 'dl' / 'f.bin', sha(data), opener=net, size_hint=len(data))
        self.assertEqual(dest.read_bytes(), data)
        self.assertFalse((self.d / 'dl' / 'f.bin.part').exists())

    def test_download_hash_mismatch_deletes(self):
        net = FakeNet({'https://h/f.bin': b'tampered'})
        with self.assertRaises(fetch.FetchError) as cm:
            fetch.download('https://h/f.bin', self.d / 'f.bin', sha(b'original'), opener=net)
        self.assertIn('mismatch', str(cm.exception))
        self.assertEqual(list(self.d.iterdir()), [])   # neither the file nor a .part is left

    def test_download_size_mismatch_deletes(self):
        net = FakeNet({'https://h/f.bin': b'abc'})
        with self.assertRaises(fetch.FetchError):
            fetch.download('https://h/f.bin', self.d / 'f.bin', sha(b'abc'), opener=net, size_hint=99)
        self.assertEqual(list(self.d.iterdir()), [])

    def test_download_refuses_http(self):
        net = FakeNet()
        with self.assertRaises(fetch.FetchError):
            fetch.download('http://h/f.bin', self.d / 'f.bin', 'a' * 64, opener=net)
        self.assertEqual(net.calls, [])

    def test_download_network_error(self):
        with self.assertRaises(fetch.FetchError):
            fetch.download('https://h/f.bin', self.d / 'f.bin', 'a' * 64, opener=FakeNet(fail=True))
        self.assertEqual(list(self.d.iterdir()), [])

    def test_fetch_pin_asks_first_and_declining_downloads_nothing(self):
        data = b'zzz'
        pin = pin_for('t.zip', data)
        net = FakeNet({pin['url']: data})
        asked = []
        with self.assertRaises(fetch.FetchError) as cm:
            fetch.fetch_pin('tool', pin, self.d / 'dl', lambda q: (asked.append(q), False)[1], opener=net, say=self.say)
        self.assertEqual(net.calls, [])
        self.assertEqual(len(asked), 1)
        self.assertIn(pin['url'], asked[0])   # the question names the URL
        self.assertIn('declined', str(cm.exception))

    def test_fetch_pin_yes_then_reuse_without_asking(self):
        data = b'zzz'
        pin = pin_for('t.zip', data)
        net = FakeNet({pin['url']: data})
        p = fetch.fetch_pin('tool', pin, self.d / 'dl', lambda q: True, opener=net, say=self.say)
        self.assertEqual(p.read_bytes(), data)
        self.assertEqual(len(net.calls), 1)
        asked = []
        p2 = fetch.fetch_pin('tool', pin, self.d / 'dl', lambda q: (asked.append(q), True)[1], opener=net, say=self.say)
        self.assertEqual(p2, p)
        self.assertEqual(asked, [])
        self.assertEqual(len(net.calls), 1)   # cached copy: no second download

    def test_fetch_pin_corrupt_cached_copy_is_not_trusted(self):
        data = b'zzz'
        pin = pin_for('t.zip', data)
        (self.d / 'dl').mkdir()
        (self.d / 'dl' / 't.zip').write_bytes(b'corrupt')
        net = FakeNet({pin['url']: data})
        asked = []
        p = fetch.fetch_pin('tool', pin, self.d / 'dl', lambda q: (asked.append(q), True)[1], opener=net, say=self.say)
        self.assertEqual(len(asked), 1)
        self.assertEqual(p.read_bytes(), data)

    def test_fetch_pin_offline_dir_needs_no_network_and_no_question(self):
        data = b'zzz'
        pin = pin_for('t.zip', data)
        (self.d / 'usb').mkdir()
        (self.d / 'usb' / 't.zip').write_bytes(data)
        net = FakeNet(fail=True)
        p = fetch.fetch_pin('tool', pin, self.d / 'dl', lambda q: self.fail('must not ask'), offline_dirs=[self.d / 'usb'], opener=net, say=self.say)
        self.assertEqual(p, self.d / 'usb' / 't.zip')
        self.assertEqual(net.calls, [])

    def test_fetch_pin_offline_dir_with_wrong_hash_is_ignored(self):
        data = b'zzz'
        pin = pin_for('t.zip', data)
        (self.d / 'usb').mkdir()
        (self.d / 'usb' / 't.zip').write_bytes(b'not it')
        with self.assertRaises(fetch.FetchError):
            fetch.fetch_pin('tool', pin, self.d / 'dl', lambda q: False, offline_dirs=[self.d / 'usb'], opener=FakeNet(), say=self.say)

    def test_fetch_pin_incomplete_pin(self):
        with self.assertRaises(fetch.FetchError):
            fetch.fetch_pin('tool', {'url': 'TODO', 'sha256': 'TODO', 'filename': 'x'}, self.d, lambda q: True, opener=FakeNet(), say=self.say)

    def make_zip(self, path, entries):
        with zipfile.ZipFile(path, 'w') as z:
            for n, b in entries.items():
                z.writestr(n, b)

    def test_extract_zip_and_strip(self):
        z = self.d / 'a.zip'
        self.make_zip(z, {'top/bin/x.exe': b'x', 'top/readme.txt': b'r'})
        fetch.extract(z, self.d / 'out', strip=1)
        self.assertEqual((self.d / 'out' / 'bin' / 'x.exe').read_bytes(), b'x')
        self.assertEqual((self.d / 'out' / 'readme.txt').read_bytes(), b'r')

    def test_extract_zip_refuses_path_traversal(self):
        z = self.d / 'evil.zip'
        self.make_zip(z, {'ok.txt': b'1', '../escape.txt': b'2'})
        with self.assertRaises(fetch.FetchError):
            fetch.extract(z, self.d / 'out')
        self.assertFalse((self.d / 'escape.txt').exists())

    def test_extract_zip_refuses_drive_path(self):
        z = self.d / 'evil2.zip'
        self.make_zip(z, {'C:/Windows/x.txt': b'2'})
        with self.assertRaises(fetch.FetchError):
            fetch.extract(z, self.d / 'out')

    def test_extract_tar_xz_strip_and_skip_links(self):
        t = self.d / 'a.tar.xz'
        with tarfile.open(t, 'w:xz') as tf:
            for name, data in (('root/bin/clang.exe', b'c'), ('root/lib/a.lib', b'a')):
                ti = tarfile.TarInfo(name)
                ti.size = len(data)
                tf.addfile(ti, io.BytesIO(data))
            ln = tarfile.TarInfo('root/link')
            ln.type = tarfile.SYMTYPE
            ln.linkname = '../../outside'
            tf.addfile(ln)
        n = fetch.extract(t, self.d / 'llvm', strip=1)
        self.assertEqual(n, 2)
        self.assertEqual((self.d / 'llvm' / 'bin' / 'clang.exe').read_bytes(), b'c')
        self.assertFalse((self.d / 'llvm' / 'link').exists())

    def test_extract_tar_refuses_traversal(self):
        t = self.d / 'evil.tar.xz'
        with tarfile.open(t, 'w:xz') as tf:
            ti = tarfile.TarInfo('../escape.txt')
            ti.size = 1
            tf.addfile(ti, io.BytesIO(b'x'))
        with self.assertRaises(fetch.FetchError):
            fetch.extract(t, self.d / 'out')

    def test_fetch_tools_end_to_end(self):
        zbuf = io.BytesIO()
        with zipfile.ZipFile(zbuf, 'w') as z:
            z.writestr('jdk-1/bin/java.exe', b'j')
        data = zbuf.getvalue()
        pin = pin_for('jdk.zip', data, install={'dir': '.', 'strip': 0})
        table = {'tools': {'jdk': pin, 'python_packages': {'wheels': []}}}
        net = FakeNet({pin['url']: data})
        out = fetch.fetch_tools(table, ['jdk'], self.d / 'tools', self.d / 'tools' / 'downloads', lambda q: True, opener=net, say=self.say)
        self.assertTrue((self.d / 'tools' / 'jdk-1' / 'bin' / 'java.exe').exists())
        self.assertEqual(out['jdk'], self.d / 'tools')

    def test_fetch_tools_unknown_name(self):
        with self.assertRaises(fetch.FetchError):
            fetch.fetch_tools({'tools': {}}, ['nope'], self.d, self.d, lambda q: True)

    def test_ask_from_args(self):
        self.assertTrue(fetch.ask_from_args(True)('Download x?'))
        self.assertFalse(fetch.ask_from_args(False, stdin_is_tty=False)('Download x?'))   # no terminal and no --yes: refuse


def fake_wheel(path, dist, module_files, version='1.0'):
    """A tiny pure-Python wheel pip can install."""
    di = f'{dist}-{version}.dist-info'
    files = dict(module_files)
    files[f'{di}/METADATA'] = f'Metadata-Version: 2.1\nName: {dist}\nVersion: {version}\n'
    files[f'{di}/WHEEL'] = 'Wheel-Version: 1.0\nGenerator: test\nRoot-Is-Purelib: true\nTag: py3-none-any\n'
    files[f'{di}/RECORD'] = ''.join(f'{n},,\n' for n in files) + f'{di}/RECORD,,\n'
    with zipfile.ZipFile(path, 'w') as z:
        for n, c in files.items():
            z.writestr(n, c)


class PyenvTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_requirements_text_has_a_hash_per_package_and_skips_the_installer(self):
        w = [{'name': 'pip', 'version': '1', 'sha256': 'A' * 64, 'installer': True},
             {'name': 'numpy', 'version': '2.5.4', 'sha256': 'B' * 64}]
        self.assertEqual(pyenv.requirements_text(w), 'numpy==2.5.4 --hash=sha256:' + 'b' * 64 + '\n')

    def test_real_table_requirements(self):
        table = json.loads((HERE.parent / 'versions.json').read_text(encoding='utf-8'))
        text = pyenv.requirements_text(pyenv.wheels(table))
        for name in ('capstone', 'numpy', 'lz4', 'pyelftools'):
            self.assertRegex(text, rf'(?m)^{name}==[\d.]+ --hash=sha256:[0-9a-f]{{64}}$')
        self.assertNotIn('pip==', text)
        self.assertEqual(pyenv.wanted_python_minor(table), '3.14')

    def build_fake_table(self):
        wheel_dir = self.d / 'wheels'
        wheel_dir.mkdir()
        specs = {'capstone': {'capstone/__init__.py': 'X = 1\n'},
                 'pyelftools': {'elftools/__init__.py': ''},
                 'numpy': {'numpy/__init__.py': ''},
                 'lz4': {'lz4/__init__.py': '', 'lz4/block.py': ''}}
        ws = []
        for name, files in specs.items():
            fn = f'{name}-1.0-py3-none-any.whl'
            fake_wheel(wheel_dir / fn, name, files)
            ws.append({'name': name, 'version': '1.0', 'filename': fn, 'url': 'https://example.invalid/' + fn,
                       'sha256': fetch.sha256_file(wheel_dir / fn)})
        minor = '%d.%d' % sys.version_info[:2]
        return {'tools': {'python_embed': {'version': minor + '.0'}, 'python_packages': {'wheels': ws}}}, wheel_dir

    def test_venv_from_pinned_hashes_offline(self):
        table, wheel_dir = self.build_fake_table()
        py = pyenv.build_with_venv(table, sys.executable, self.d / 'venv', wheel_dir, lambda q: self.fail('must not ask'),
                                   opener=FakeNet(fail=True), say=lambda s: None)
        self.assertTrue(Path(py).exists())
        self.assertTrue(pyenv.import_ok(py))

    def test_pip_rejects_a_wheel_that_does_not_match_its_pinned_hash(self):
        table, wheel_dir = self.build_fake_table()
        table['tools']['python_packages']['wheels'][0]['sha256'] = '0' * 64   # the pin no longer matches the file
        with self.assertRaises(fetch.FetchError):
            pyenv.build_with_venv(table, sys.executable, self.d / 'venv', wheel_dir, lambda q: False, opener=FakeNet(fail=True), say=lambda s: None)

    def test_pip_require_hashes_refuses_a_swapped_wheel(self):
        """Even if a wheel were swapped after the builder verified it, pip itself checks the hash again."""
        table, wheel_dir = self.build_fake_table()
        files = pyenv.fetch_wheels(table, wheel_dir, lambda q: False, opener=FakeNet(fail=True), say=lambda s: None)
        py_dir = self.d / 'venv'
        import subprocess
        subprocess.check_call([sys.executable, '-I', '-m', 'venv', str(py_dir)])
        fake_wheel(files[0], 'capstone', {'capstone/__init__.py': 'X = 2  # swapped\n'})   # same name, different bytes
        req = py_dir / 'req.txt'
        req.write_text(pyenv.requirements_text(pyenv.wheels(table)), encoding='ascii')
        py = py_dir / 'Scripts' / 'python.exe'
        with self.assertRaises(fetch.FetchError) as cm:
            pyenv.install_wheels(py, files, req, [py, '-m', 'pip'], lambda s: None)
        self.assertIn('hash', str(cm.exception).lower())

    def test_wrong_python_version_is_refused_before_any_download(self):
        table, wheel_dir = self.build_fake_table()
        table['tools']['python_embed']['version'] = '3.1.0'
        net = FakeNet(fail=True)
        with self.assertRaises(fetch.FetchError):
            pyenv.build_with_venv(table, sys.executable, self.d / 'venv', wheel_dir, lambda q: True, opener=net, say=lambda s: None)
        self.assertEqual(net.calls, [])


if __name__ == '__main__':
    unittest.main()
