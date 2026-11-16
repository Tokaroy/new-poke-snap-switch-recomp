"""`snap_builder.py build --prepare-shaders` reaches the pipeline, and start_prepare_shaders launches `<exe> --prepare-shaders` detached,
windowless and at below-normal priority without waiting for it (the sweep takes about half an hour)."""
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import snap_builder  # noqa: E402
from snapbuild import pipeline  # noqa: E402
from snapbuild.util import BELOW_NORMAL, CREATE_NO_WINDOW, Log  # noqa: E402


class FlagParseTests(unittest.TestCase):
    def run_main(self, extra):
        seen = {}

        def fake_build(opt, log):
            seen['opt'] = opt
            return Path('SnapRecomp.exe'), None

        with tempfile.TemporaryDirectory() as d, mock.patch.object(pipeline, 'build', fake_build):
            code = snap_builder.main(['build', '--main', 'm', '--sdk', 's', '--romfs', 'r', '--work', str(Path(d) / 'work'), '--skip-smoke'] + extra)
        return code, seen.get('opt')

    def test_flag_is_off_by_default(self):
        code, opt = self.run_main([])
        self.assertEqual(code, 0)
        self.assertFalse(opt.prepare_shaders)

    def test_flag_reaches_the_pipeline(self):
        code, opt = self.run_main(['--prepare-shaders'])
        self.assertEqual(code, 0)
        self.assertTrue(opt.prepare_shaders)


class StartTests(unittest.TestCase):
    def setUp(self):
        self.d = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.d, True)   # registered first, so it runs after the log is closed
        self.log = Log(self.d / 'builder.log', quiet=True)
        self.addCleanup(self.log.f.close)

    def test_launches_detached_below_normal_and_returns_at_once(self):
        d = self.d
        out = d / 'args.txt'
        stub = d / 'SnapRecomp.bat'
        stub.write_text('@echo off\n' + 'echo %* > "' + str(out) + '"\n' + 'cd >> "' + str(out) + '"\n', encoding='ascii')
        t0 = time.time()
        pipeline.start_prepare_shaders(stub, self.log)
        self.assertLess(time.time() - t0, 5)
        for _ in range(100):   # the stub runs in its own process
            if out.exists() and len(out.read_text().splitlines()) >= 2:
                break
            time.sleep(0.1)
        lines = out.read_text().splitlines()
        self.assertEqual(lines[0].strip(), '--prepare-shaders')
        self.assertEqual(Path(lines[1].strip()).resolve(), d.resolve())   # runs from the exe folder
        self.assertIn('shader preparation started', (d / 'builder.log').read_text(encoding='utf-8'))

    def test_popen_flags(self):
        with mock.patch.object(subprocess, 'Popen') as pop:
            pipeline.start_prepare_shaders(self.d / 'SnapRecomp.exe', self.log)
        args, kw = pop.call_args
        self.assertEqual(args[0][1:], ['--prepare-shaders'])
        flags = kw['creationflags']
        self.assertTrue(flags & BELOW_NORMAL and flags & CREATE_NO_WINDOW and flags & 0x8)
        self.assertIs(kw['stdin'], subprocess.DEVNULL)

    def test_missing_exe_is_not_fatal(self):
        pipeline.start_prepare_shaders(self.d / 'nothere' / 'SnapRecomp.exe', self.log)   # cwd does not exist: OSError, logged
        self.assertIn('could not start', (self.d / 'builder.log').read_text(encoding='utf-8'))


if __name__ == '__main__':
    unittest.main()
