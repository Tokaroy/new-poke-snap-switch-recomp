import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from snapbuild import toolchain, winsdk  # noqa: E402
from snapbuild.util import StepError  # noqa: E402


def fake_splat(out):
    for d in winsdk._INCLUDE + winsdk._LIB:
        (Path(out) / d).mkdir(parents=True, exist_ok=True)
    (Path(out) / 'sdk/lib/um/x86_64/kernel32.lib').write_bytes(b'x')


class WinsdkTests(unittest.TestCase):
    def setUp(self):
        self.t = Path(tempfile.mkdtemp())

    def runner(self, rc=0, out=''):
        calls = []

        def run(cmd, **kw):
            calls.append(cmd)
            if rc == 0:
                fake_splat(cmd[cmd.index('--output') + 1])
            return subprocess.CompletedProcess(cmd, rc, out, '')
        run.calls = calls
        return run

    def test_license_not_accepted_downloads_nothing(self):
        r = self.runner()
        with self.assertRaises(StepError) as e:
            winsdk.install('xwin.exe', self.t, False, runner=r)
        self.assertIn('not accepted', str(e.exception))
        self.assertEqual(r.calls, [])

    def test_install_and_find(self):
        r = self.runner()
        root = winsdk.install('xwin.exe', self.t, True, say=lambda s: None, runner=r)
        self.assertEqual(winsdk.find(self.t), root)
        self.assertIn('--accept-license', r.calls[0])
        self.assertTrue(winsdk.receipt(root)['accepted_license'].startswith('https://'))
        env = winsdk.env_for(root)
        self.assertIn('ucrt', env['INCLUDE'])
        self.assertIn('x86_64', env['LIB'])
        self.assertFalse((self.t / 'winsdk.partial').exists())

    def test_failure_is_plain_and_leaves_nothing(self):
        with self.assertRaises(StepError) as e:
            winsdk.install('xwin.exe', self.t, True, say=lambda s: None, runner=self.runner(1, 'dns error'))
        self.assertIn('internet connection', str(e.exception))
        self.assertIsNone(winsdk.find(self.t))

    def test_native_env_modes(self):
        with self.assertRaises(StepError):
            toolchain.native_env('bogus')
        winsdk.install('xwin.exe', self.t, True, say=lambda s: None, runner=self.runner())
        link, comp, desc = toolchain.native_env('xwin', self.t)
        self.assertIn('INCLUDE', comp)
        self.assertIn('downloaded', desc)
        empty = Path(tempfile.mkdtemp())
        with self.assertRaises(StepError) as e:
            toolchain.native_env('xwin', empty)
        self.assertIn('winsdk --accept-license', str(e.exception))


if __name__ == '__main__':
    unittest.main()
