"""Regression tests from the outside review of the builder (prebuilt hashes, all-or-nothing upscaler installs, download cleanup, MSVC discovery).
Fake inputs only: no network and no game data."""
import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
from snapbuild import fetch, pipeline, toolchain, upscalers as up  # noqa: E402
from snapbuild.util import StepError  # noqa: E402
from test_fetch import FakeNet  # noqa: E402
from test_upscalers import make_table  # noqa: E402


class UpscalerRegressionTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name)
        self.table, self.files = make_table()
        self.inst = self.d / 'out'
        self.dl = self.d / 'dl'

    def tearDown(self):
        self.tmp.cleanup()

    def test_second_direct_file_mismatch_installs_nothing(self):
        files = dict(self.files, **{'https://h/note': b'tampered'})
        with self.assertRaises(fetch.FetchError):
            up.run(self.table, ['one'], self.inst, self.dl, {'one'}, lambda q: True,
                   opener=FakeNet(files), say=lambda s: None)
        self.assertFalse((self.inst / 'upscalers' / 'one.dll').exists())

    def test_second_archive_member_mismatch_installs_nothing(self):
        self.table['upscalers']['two']['files'][1]['sha256'] = '0' * 64
        with self.assertRaises(fetch.FetchError):
            up.run(self.table, ['two'], self.inst, self.dl, {'two'}, lambda q: True,
                   opener=FakeNet(self.files), say=lambda s: None)
        self.assertFalse((self.inst / 'x.dll').exists())

    def test_missing_vendor_license_is_not_installed(self):
        up.run(self.table, ['one'], self.inst, self.dl, {'one'}, lambda q: True,
               opener=FakeNet(self.files), say=lambda s: None)
        (self.inst / 'licenses' / 'upscalers' / 'A-LICENSE.txt').unlink()
        self.assertFalse(up.installed(self.table['upscalers']['one'], self.inst))


class PrebuiltRegressionTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.pre = Path(self.tmp.name)
        for sub in ('include', 'obj', 'bin'):
            (self.pre / sub).mkdir()
        (self.pre / 'include' / 'cpu.h').write_text('#define SNAP_RUNTIME_ABI 7\n', encoding='utf-8')
        (self.pre / 'obj' / 'a.obj').write_bytes(b'object')
        (self.pre / 'bin' / 'a.dll').write_bytes(b'dll')
        self.man = {
            'runtime_abi': 7,
            'objs': [{'name': 'a.obj', 'sha256': hashlib.sha256(b'object').hexdigest()}],
            'dlls': [{'name': 'a.dll', 'path': 'bin/a.dll', 'sha256': hashlib.sha256(b'dll').hexdigest()}],
        }
        (self.pre / 'manifest.json').write_text(json.dumps(self.man), encoding='utf-8')

    def tearDown(self):
        self.tmp.cleanup()

    def test_valid_prebuilt_passes(self):
        self.assertEqual(pipeline.check_prebuilt(self.pre)['runtime_abi'], 7)

    def test_corrupt_object_is_rejected(self):
        (self.pre / 'obj' / 'a.obj').write_bytes(b'tampered')
        with self.assertRaisesRegex(StepError, 'a.obj'):
            pipeline.check_prebuilt(self.pre)

    def test_corrupt_dll_is_rejected(self):
        (self.pre / 'bin' / 'a.dll').write_bytes(b'tampered')
        with self.assertRaisesRegex(StepError, 'a.dll'):
            pipeline.check_prebuilt(self.pre)

    def test_missing_dll_is_rejected(self):
        (self.pre / 'bin' / 'a.dll').unlink()
        with self.assertRaisesRegex(StepError, 'a.dll'):
            pipeline.check_prebuilt(self.pre)


class DownloadRegressionTests(unittest.TestCase):
    def test_replace_failure_removes_part_and_raises_fetch_error(self):
        with tempfile.TemporaryDirectory() as d:
            dest = Path(d) / 'tool.zip'
            net = FakeNet({'https://h/tool.zip': b'ok'})
            with mock.patch('snapbuild.fetch.os.replace', side_effect=OSError('locked')):
                with self.assertRaises(fetch.FetchError):
                    fetch.download('https://h/tool.zip', dest, hashlib.sha256(b'ok').hexdigest(), opener=net)
            self.assertFalse((Path(d) / 'tool.zip.part').exists())


class ToolchainRegressionTests(unittest.TestCase):
    @mock.patch('snapbuild.toolchain.subprocess.run')
    def test_vswhere_output_is_explicit_utf8(self, run):
        run.return_value = mock.Mock(returncode=0, stdout='D:\\Visual Studio\n')
        with mock.patch.object(Path, 'exists', return_value=True):
            toolchain.find_vcvars()
        args, kwargs = run.call_args
        self.assertIn('-utf8', args[0])
        self.assertEqual(kwargs.get('encoding'), 'utf-8')

    @mock.patch('snapbuild.toolchain.subprocess.run')
    def test_vc_env_uses_unicode_cmd_output(self, run):
        run.return_value = mock.Mock(returncode=0, stdout='TEST_PATH=C:\\Users\\José\\SDK\r\n')
        env = toolchain.vc_env(Path('C:/VS/vcvars64.bat'))
        self.assertEqual(env['TEST_PATH'], 'C:\\Users\\José\\SDK')
        args, kwargs = run.call_args
        self.assertIn('/u', args[0].lower())
        self.assertEqual(kwargs.get('encoding'), 'utf-16-le')

    @mock.patch('snapbuild.toolchain.subprocess.run')
    def test_vc_env_rejects_failed_vcvars(self, run):
        run.return_value = mock.Mock(returncode=1, stdout='')
        with self.assertRaises(StepError):
            toolchain.vc_env(Path('C:/VS/vcvars64.bat'))


if __name__ == '__main__':
    unittest.main()
