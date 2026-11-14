"""The Ghidra wrapper builds the same command lines as scripts/ghidra_analyze.ps1 (checked without running Ghidra)."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from snapbuild import ghidra_maps as gm  # noqa: E402


class GhidraCmdTests(unittest.TestCase):
    def test_commands(self):
        an, ex, rc = gm.build_commands(Path('G'), Path('py.exe'), Path('R'), Path('W'), 'sha', 'BID', 60)
        self.assertTrue(an[0].endswith('analyzeHeadless.bat'))
        self.assertIn('-import', an)
        self.assertIn('-analysisTimeoutPerFile', an)
        self.assertEqual(an[an.index('-analysisTimeoutPerFile') + 1], '3600')
        self.assertIn('SeedFdeStarts.java', an)
        self.assertIn('-process', ex)
        self.assertEqual(ex[-2:], ['sha', 'BID'])
        self.assertIn('ExportMaps.java', ex)
        self.assertIn('-readOnly', ex)
        self.assertEqual(rc[1], '-I')
        self.assertTrue(rc[2].endswith('reconcile_functions.py'))
        self.assertIn('--out', rc)
        # the three stages agree on one project folder and one fde file
        self.assertIn(str(gm.layout(Path('W'))['fde']), an)
        self.assertIn(str(gm.layout(Path('W'))['fde']), ex)

    def test_java_env(self):
        env = gm.java_env(Path('J'))
        self.assertEqual(env['GHIDRA_HEADLESS_MAXMEM'], '12G')
        self.assertEqual(env['JAVA_HOME'], 'J')

    def test_missing_ghidra_message(self):
        from snapbuild.util import Log, StepError
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            log = Log(Path(d) / 'l.log', quiet=True)
            with self.assertRaises(StepError) as cm:
                gm.run_ghidra(None, None, 'py', Path('R'), Path(d), 'x', log)
            log.close()
        self.assertIn('--maps', str(cm.exception))


if __name__ == '__main__':
    unittest.main()
