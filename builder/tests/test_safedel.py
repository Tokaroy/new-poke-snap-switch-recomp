"""Guard tests for junction-safe deletion (builder/snapbuild/safedel.py).

2026-11-16: a recursive delete of a scratch tree followed junctions into the repository and emptied tools\\. These tests make
sure the builder can never do that again: links are unlinked, their targets are never touched, and no bare shutil.rmtree is left.
"""
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

BUILDER = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BUILDER))
from snapbuild import safedel  # noqa: E402


def make_dir_link(link, target):
    """A junction on Windows (no privilege needed), a directory symlink elsewhere."""
    if os.name == 'nt':
        r = subprocess.run(['cmd', '/c', 'mklink', '/J', str(link), str(target)], capture_output=True, text=True)
        if r.returncode:
            raise unittest.SkipTest('cannot create a junction here: ' + r.stdout + r.stderr)
    else:
        os.symlink(target, link, target_is_directory=True)


class SafeDelete(unittest.TestCase):
    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        self.base = Path(self.td.name)
        self.precious = self.base / 'precious'          # stands in for tools\ in the repository
        (self.precious / 'sub').mkdir(parents=True)
        (self.precious / 'sub' / 'keep.txt').write_text('keep')
        (self.precious / 'top.txt').write_text('keep')
        self.scratch = self.base / 'scratch'            # stands in for build\w\ct_tree
        (self.scratch / 'a' / 'b').mkdir(parents=True)
        (self.scratch / 'a' / 'b' / 'x.txt').write_text('x')

    def tearDown(self):
        # never use a following delete to clean up after a test that may have failed
        safedel.unlink_links(self.base)
        self.td.cleanup()

    def assert_precious_intact(self):
        self.assertTrue((self.precious / 'sub' / 'keep.txt').exists(), 'the link target lost a file')
        self.assertTrue((self.precious / 'top.txt').exists(), 'the link target lost a file')

    def test_nested_junction_target_survives(self):
        make_dir_link(self.scratch / 'a' / 'tools', self.precious)
        self.assertTrue(safedel.is_link(self.scratch / 'a' / 'tools'))
        safedel.rmtree(self.scratch)
        self.assertFalse(self.scratch.exists())
        self.assert_precious_intact()

    def test_root_is_a_link(self):
        make_dir_link(self.base / 'rootlink', self.precious)
        safedel.rmtree(self.base / 'rootlink')
        self.assertFalse((self.base / 'rootlink').exists())
        self.assert_precious_intact()

    def test_two_levels_of_links(self):
        make_dir_link(self.scratch / 'l1', self.precious)
        make_dir_link(self.precious / 'sub' / 'back', self.scratch)   # a cycle back into the tree being deleted
        safedel.rmtree(self.scratch)
        self.assertFalse(self.scratch.exists())
        self.assertTrue((self.precious / 'sub' / 'keep.txt').exists())
        self.assertTrue((self.precious / 'top.txt').exists())

    def test_ignore_errors_and_missing(self):
        safedel.rmtree(self.base / 'nope', ignore_errors=True)
        with self.assertRaises(FileNotFoundError):
            safedel.rmtree(self.base / 'nope')

    def test_readonly_file_is_removed(self):
        f = self.scratch / 'ro.txt'
        f.write_text('r')
        os.chmod(f, 0o444)
        safedel.rmtree(self.scratch)
        self.assertFalse(self.scratch.exists())

    def test_unlink_links_reports_and_keeps_targets(self):
        make_dir_link(self.scratch / 'a' / 'tools', self.precious)
        found = safedel.unlink_links(self.scratch)
        self.assertEqual([Path(f).name for f in found], ['tools'])
        self.assertTrue(self.scratch.exists())
        self.assert_precious_intact()


class NoBareRmtree(unittest.TestCase):
    def test_builder_never_calls_shutil_rmtree(self):
        bad = []
        for p in sorted(list(BUILDER.glob('*.py')) + list((BUILDER / 'snapbuild').glob('*.py'))):
            if p.name == 'safedel.py':
                continue
            for n, line in enumerate(p.read_text(encoding='utf-8', errors='replace').splitlines(), 1):
                code = line.split('#', 1)[0]
                if re.search(r'\bshutil\.rmtree\b|\bfrom shutil import[^\n]*\brmtree\b', code):
                    bad.append(f'{p.relative_to(BUILDER)}:{n}')
        self.assertEqual(bad, [], 'use snapbuild.safedel.rmtree: shutil.rmtree follows junctions')


if __name__ == '__main__':
    unittest.main()
