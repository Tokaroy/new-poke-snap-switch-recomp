"""Tests for the install check (builder/snapbuild/installcheck.py) with fake inputs: no game data, no network."""
import hashlib
import json
import re
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE))
from snapbuild import installcheck as ic  # noqa: E402
from snapbuild import update  # noqa: E402
from snapbuild import versioncheck as vc  # noqa: E402
import test_versioncheck as tv  # noqa: E402

OLD_SEGS = [b'T' * 5000 + b'old', b'R' * 700, b'D' * 90]
BUILDER = HERE.parent


def hashes_for_fake():
    return {'program_images': {
        '1.0.0 (v0)': {'main': tv.image(OLD_SEGS), 'sdk': tv.image(tv.SDK_SEGS)},
        '2.0.0 (v196608)': {'main': tv.image(tv.MAIN_SEGS), 'sdk': tv.image(tv.SDK_SEGS)},
        vc.EXPECTED_NAME: {'main': tv.image(tv.MAIN_SEGS), 'sdk': tv.image(tv.SDK_SEGS)}},
        'runtime_files': {'note': 'x', 'SDL3.dll': [hashlib.sha256(b'good sdl').hexdigest()]}}


class RealFilesTests(unittest.TestCase):
    """The shipped hash files themselves."""

    def test_install_hashes_agree_with_versions_json(self):
        table = vc.load_table(BUILDER / 'versions.json')
        h = ic.load_hashes(BUILDER)
        self.assertEqual(set(h['program_images']), {v['name'] for v in table['known_versions']})
        cur = h['program_images'][vc.EXPECTED_NAME]
        self.assertEqual(cur['main'], table['nso']['main']['image_sha256'])
        self.assertEqual(cur['sdk'], table['nso']['sdk']['image_sha256'])

    def test_install_hashes_hold_only_hashes(self):
        text = (BUILDER / 'install_hashes.json').read_text(encoding='utf-8')
        h = json.loads(text)
        for d in h['program_images'].values():
            for v in d.values():
                self.assertRegex(v, r'^[0-9a-f]{64}$')
        for v in h['runtime_files'].values():
            if isinstance(v, list):
                for x in v:
                    self.assertRegex(x, r'^[0-9a-f]{64}$')
        self.assertNotIn('C:\\', text)


class CheckTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name)
        self.table = tv.make_table()
        self.h = hashes_for_fake()
        tv.make_nso(self.d / 'main', tv.ID_GOOD_MAIN, tv.MAIN_SEGS)
        tv.make_nso(self.d / 'sdk', tv.ID_GOOD_SDK, tv.SDK_SEGS)
        tv.fake_romfs(self.d / 'romfs', tv.ROMFS)

    def tearDown(self):
        self.tmp.cleanup()

    def run_check(self, **kw):
        a = dict(main=self.d / 'main', sdk=self.d / 'sdk', romfs=self.d / 'romfs')
        a.update(kw)
        return ic.run(self.table, self.h, **a)

    def test_clean_install_passes(self):
        r = self.run_check()
        self.assertTrue(r.ok, r.lines())
        self.assertIn('everything matches', r.lines()[0])

    def test_old_main_with_good_romfs_is_a_mixed_update(self):
        tv.make_nso(self.d / 'old', tv.ID_OLD_MAIN, OLD_SEGS)
        r = self.run_check(main=self.d / 'old')
        self.assertFalse(r.ok)
        self.assertIn('mixed update', r.lines()[0])
        self.assertIn('1.0.0', r.lines()[0])

    def test_good_main_with_old_romfs_is_a_mixed_update(self):
        for p in list((self.d / 'romfs').rglob('*.bin'))[:6]:
            p.write_bytes(b'y' * (p.stat().st_size + 1))
        r = self.run_check()
        self.assertFalse(r.ok)
        self.assertIn('mixed update', r.lines()[0])

    def test_old_everything_is_wrong_version_not_mixed(self):
        tv.make_nso(self.d / 'old', tv.ID_OLD_MAIN, OLD_SEGS)
        for p in (self.d / 'romfs').rglob('*.bin'):
            p.unlink()
            break
        r = self.run_check(main=self.d / 'old')
        self.assertFalse(r.ok)
        self.assertNotIn('mixed', r.lines()[0])
        self.assertIn('wrong game version', r.lines()[0])

    def test_modified_main_same_build_id(self):
        segs = [tv.MAIN_SEGS[0][:-1] + b'Z'] + tv.MAIN_SEGS[1:]
        tv.make_nso(self.d / 'mod', tv.ID_GOOD_MAIN, segs, check_hashes=False)
        r = self.run_check(main=self.d / 'mod')
        self.assertFalse(r.ok)
        self.assertEqual(r.problems[0].state, ic.MODIFIED)
        self.assertIn('modified or damaged', r.lines()[0])

    def test_unknown_build_id(self):
        tv.make_nso(self.d / 'x', '77' * 20, tv.MAIN_SEGS)
        self.assertEqual(self.run_check(main=self.d / 'x').problems[0].state, ic.UNKNOWN)

    def test_missing_files(self):
        r = self.run_check(main=self.d / 'nope', romfs=self.d / 'nofolder')
        self.assertEqual({f.state for f in r.problems}, {ic.MISSING})
        self.assertEqual(len(r.problems), 2)

    def test_romfs_extra_file_is_modified(self):
        (self.d / 'romfs' / 'extra.bin').write_bytes(b'q')
        r = self.run_check()
        self.assertEqual(r.problems[0].state, ic.MODIFIED)
        self.assertIn('added, removed or resized', r.problems[0].text)

    def test_folder_that_is_not_the_romfs(self):
        (self.d / 'other').mkdir()
        (self.d / 'other' / 'a.txt').write_text('a')
        self.assertEqual(self.run_check(romfs=self.d / 'other').problems[0].state, ic.UNKNOWN)

    def test_nothing_given_is_not_a_pass(self):
        self.assertFalse(ic.run(self.table, self.h).ok)

    def test_output_never_holds_a_hash(self):
        tv.make_nso(self.d / 'old', tv.ID_OLD_MAIN, OLD_SEGS)
        out = '\n'.join(self.run_check(main=self.d / 'old').lines() + self.run_check().lines())
        self.assertIsNone(re.search(r'[0-9a-fA-F]{32,}', out))


class InstallDirTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name)
        self.out = self.d / 'out'
        self.out.mkdir()
        (self.out / 'SnapRecomp.exe').write_bytes(b'MZ' + b'e' * 500)
        (self.out / 'SDL3.dll').write_bytes(b'good sdl')
        (self.d / 'main.elf').write_bytes(b'elf-main')
        self.h = hashes_for_fake()

    def tearDown(self):
        self.tmp.cleanup()

    def record(self):
        return ic.write_record(self.out, {'main_elf': self.d / 'main.elf'}, '1.0', names={'SnapRecomp.exe', 'SDL3.dll'})

    def test_recorded_install_passes(self):
        self.record()
        f, notes = ic.check_install_dir(self.out, self.h)
        self.assertEqual([x.state for x in f], [ic.OK])
        self.assertEqual(notes, [])

    def test_no_record_is_a_note_not_a_problem(self):
        f, notes = ic.check_install_dir(self.out, self.h)
        self.assertTrue(all(x.ok for x in f))
        self.assertEqual(len(notes), 1)
        self.assertIn(ic.RECORD_NAME, notes[0])

    def test_changed_exe_missing_dll_changed_image(self):
        self.record()
        (self.out / 'SnapRecomp.exe').write_bytes(b'MZ' + b'f' * 500)
        (self.out / 'SDL3.dll').unlink()
        (self.d / 'main.elf').write_bytes(b'elf-other')
        f, _ = ic.check_install_dir(self.out, self.h)
        self.assertEqual(sorted(x.state for x in f), [ic.MISSING, ic.MODIFIED, ic.MODIFIED])

    def test_shared_dll_not_in_known_list_is_modified_even_without_record(self):
        (self.out / 'SDL3.dll').write_bytes(b'evil sdl')
        f, _ = ic.check_install_dir(self.out, self.h)
        self.assertEqual(f[0].state, ic.MODIFIED)
        self.assertIn('SDL3.dll', f[0].text)

    def test_missing_exe(self):
        (self.out / 'SnapRecomp.exe').unlink()
        f, _ = ic.check_install_dir(self.out, self.h)
        self.assertEqual(f[0].state, ic.MISSING)

    def test_bad_record_file(self):
        (self.out / ic.RECORD_NAME).write_text('{nope', encoding='utf-8')
        f, _ = ic.check_install_dir(self.out, self.h)
        self.assertTrue(any(x.state == ic.MODIFIED for x in f))


class UpdateRetryTests(unittest.TestCase):
    def run_with(self, exc):
        calls = []

        def once(*a, **k):
            calls.append(1)
            raise exc
        saved = update.download_once
        update.download_once = once
        try:
            with self.assertRaises(update.UpdateError) as cm:
                update.download(None, 'x', sleep=lambda s: None)
        finally:
            update.download_once = saved
        return calls, cm.exception

    def test_short_zip_is_retried_then_fails_in_plain_words(self):
        calls, err = self.run_with(update.ShortDownload('the download stopped early (5 of 10 bytes).'))
        self.assertEqual(len(calls), 3)
        self.assertIn('3 times in a row', str(err))

    def test_other_errors_are_not_retried(self):
        calls, _ = self.run_with(update.UpdateError('the download is larger than the release says'))
        self.assertEqual(len(calls), 1)


if __name__ == '__main__':
    unittest.main()
