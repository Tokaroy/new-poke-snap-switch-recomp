"""A JRE in JAVA_HOME must not be picked for Ghidra, and child processes get no stdin (a question must fail, not wait)."""
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from snapbuild import toolchain, util  # noqa: E402


def make_java(folder, jdk):
    (folder / 'bin').mkdir(parents=True)
    (folder / 'bin' / 'java.exe').write_bytes(b'x')
    if jdk:
        (folder / 'bin' / 'javac.exe').write_bytes(b'x')


class JdkTests(unittest.TestCase):
    def test_jre_in_java_home_is_skipped(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            jre = d / 'jre-21'
            make_java(jre, jdk=False)
            fetched = d / 'managed' / 'jdk-21.0.9+10'
            make_java(fetched, jdk=True)
            with mock.patch.dict(os.environ, {'JAVA_HOME': str(jre)}), mock.patch.object(toolchain, 'managed', lambda: d / 'managed'):
                self.assertEqual(toolchain.find_jdk(None, d / 'repo'), fetched)   # the fetched JDK wins over a JRE
            with mock.patch.dict(os.environ, {'JAVA_HOME': str(jre)}), mock.patch.object(toolchain, 'managed', lambda: d / 'none'):
                self.assertIsNone(toolchain.find_jdk(None, d / 'repo'))           # a JRE alone is not enough

    def test_jdk_in_java_home_is_used_last(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            home = d / 'jdk-home'
            make_java(home, jdk=True)
            with mock.patch.dict(os.environ, {'JAVA_HOME': str(home)}), mock.patch.object(toolchain, 'managed', lambda: d / 'none'):
                self.assertEqual(toolchain.find_jdk(None, d / 'repo'), home)


class StdinTests(unittest.TestCase):
    def test_child_reads_eof(self):
        class L:
            def write(self, *a): pass
            def raw(self, *a): pass
        code, tail = util.run([sys.executable, '-c', 'import sys; print(repr(sys.stdin.read()))'], L(), timeout=60, check=False)
        self.assertEqual(code, 0)
        self.assertIn("''", tail[-1])   # closed stdin: the child reads nothing and does not wait


if __name__ == '__main__':
    unittest.main()
