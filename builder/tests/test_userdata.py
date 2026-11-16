"""Saves-and-settings backup and portable-mode tests. Everything runs in a temp folder; nothing reads the real %APPDATA%."""
import json
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from snapbuild import safedel, userdata  # noqa: E402


class Quiet:
    def __init__(self):
        self.lines = []

    def write(self, s):
        self.lines.append(s)


def make_root(t):
    root = Path(t) / 'data'
    files = {
        'save/0001/CURRENT': b'SNAPSAVE1\n5\n', 'save/0001/gens/5/f0': b'five', 'save/0001/gens/5/f1': b'five-b', 'save/0001/gens/5.ok': b'ok',
        'save/0001/gens/4/f0': b'four', 'save/0001/.snap.lock': b'',
        'profiles/profiles.json': b'{"p":1}', 'profiles/alt/settings.cfg': b'x=1',
        'profiles/alt/save/0002/CURRENT': b'SNAPSAVE1\n2\n', 'profiles/alt/save/0002/gens/2/f0': b'alt-save',
        'profiles/_trash/settings.cfg': b'ignored', 'profiles/alt/photos/p.png': b'photo',
        'audio.json': b'{"a":1}', 'input.json': b'{"i":1}', 'notes.txt': b'n', 'big.json': None,
        'config/x.cfg': b'cfg', 'mods/mods.json': b'{}', 'mods/pack/a.bin': b'mod', 'shadercache/a.spv': b'spv', 'photos/a.png': b'png',
    }
    for rel, data in files.items():
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        if data is None:
            with open(p, 'wb') as f:
                f.truncate(userdata.MAX_SETTINGS_FILE + 1)          # sparse: too big to be a settings file
        else:
            p.write_bytes(data)
    return root


def rel_files(root):
    return sorted(p.relative_to(root).as_posix() for p in Path(root).rglob('*') if p.is_file())


class Plan(unittest.TestCase):
    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        self.root = make_root(self.td.name)

    def tearDown(self):
        self.td.cleanup()

    def test_what_is_copied(self):
        names = [i[0] for i in userdata.plan(self.root)]
        for want in ('save/0001/CURRENT', 'save/0001/gens/5', 'save/0001/gens/5.ok', 'profiles/alt/save/0002/CURRENT',
                     'profiles/alt/save/0002/gens/2', 'profiles/profiles.json', 'profiles/alt/settings.cfg', 'audio.json', 'input.json',
                     'notes.txt', 'config', 'mods/mods.json'):
            self.assertIn(want, names)
        for no in ('save/0001/gens/4', 'big.json', 'mods/pack/a.bin', 'shadercache', 'photos', 'profiles/_trash/settings.cfg'):
            self.assertNotIn(no, names)

    def test_empty_or_missing_root(self):
        self.assertEqual(userdata.plan(Path(self.td.name) / 'nope'), [])
        self.assertIsNone(userdata.backup(Path(self.td.name) / 'nope', '1.0', '1.1'))

    def test_a_save_without_current_is_skipped(self):
        (self.root / 'save' / '0001' / 'CURRENT').write_text('garbage')
        self.assertNotIn('save/0001/CURRENT', [i[0] for i in userdata.plan(self.root)])

    def test_plan_bytes(self):
        items = userdata.plan(self.root)
        self.assertGreater(userdata.plan_bytes(items), len(b'five') + len(b'five-b'))


class Backup(unittest.TestCase):
    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        self.root = make_root(self.td.name)
        self.before = {p: (self.root / p).read_bytes() for p in rel_files(self.root) if p != 'big.json'}

    def tearDown(self):
        self.td.cleanup()

    def test_copy_is_complete_and_live_data_is_untouched(self):
        r = userdata.backup(self.root, '1.0.0', '1.1.0', now=1_800_000_000, log=Quiet())
        b = Path(r['path'])
        self.assertTrue(r['name'].endswith('-1.0.0-to-1.1.0'))
        self.assertTrue(userdata.NAME_RE.match(r['name']))
        self.assertEqual((b / 'save/0001/gens/5/f1').read_bytes(), b'five-b')
        self.assertEqual((b / 'profiles/alt/save/0002/gens/2/f0').read_bytes(), b'alt-save')
        self.assertEqual((b / 'config/x.cfg').read_bytes(), b'cfg')
        self.assertFalse((b / 'save/0001/.snap.lock').exists())
        meta = json.loads((b / 'backup.json').read_text())
        self.assertEqual((meta['from'], meta['to']), ('1.0.0', '1.1.0'))
        for p, data in self.before.items():
            self.assertEqual((self.root / p).read_bytes(), data)         # nothing live was moved or changed

    def test_backup_is_a_valid_save_root(self):
        r = userdata.backup(self.root, '1', '2', log=Quiet())
        roots = dict(userdata.save_roots_in_backup(self.root, r['name']))
        self.assertEqual(set(roots), {None, 'alt'})
        self.assertEqual((roots[None] / '0001' / 'CURRENT').read_text(), 'SNAPSAVE1\n5\n')

    def test_keeps_the_newest_three(self):
        names = []
        for i in range(6):
            names.append(userdata.backup(self.root, f'1.0.{i}', f'1.0.{i + 1}', now=1_800_000_000 + i * 60, log=Quiet())['name'])
        left = [e['name'] for e in userdata.list_backups(self.root)]
        self.assertEqual(left, list(reversed(names[-3:])))               # newest first
        self.assertEqual(len(left), userdata.KEEP_BACKUPS)

    def test_same_second_never_overwrites(self):
        a = userdata.backup(self.root, '1', '2', now=1_800_000_000)['name']
        b = userdata.backup(self.root, '1', '2', now=1_800_000_000)['name']
        self.assertNotEqual(a, b)
        self.assertEqual(len(userdata.list_backups(self.root)), 2)

    def test_foreign_folders_are_never_listed_or_pruned(self):
        base = self.root / userdata.BACKUP_DIR
        (base / 'my-own-notes').mkdir(parents=True)
        (base / 'my-own-notes' / 'x.txt').write_text('keep me')
        for i in range(5):
            userdata.backup(self.root, '1', '2', now=1_800_000_000 + i)
        self.assertTrue((base / 'my-own-notes' / 'x.txt').exists())

    def test_stale_partial_is_removed(self):
        base = self.root / userdata.BACKUP_DIR
        (base / 'old.partial').mkdir(parents=True)
        (base / 'old.partial' / 'x').write_text('x')
        userdata.backup(self.root, '1', '2')
        self.assertFalse((base / 'old.partial').exists())

    def test_copy_failure_leaves_no_folder_and_raises(self):
        orig = userdata._copy_file

        def boom(src, dst):
            if Path(src).name == 'audio.json':
                raise OSError('disk full')
            return orig(src, dst)
        userdata._copy_file = boom
        try:
            with self.assertRaises(userdata.BackupError) as cm:
                userdata.backup(self.root, '1', '2')
        finally:
            userdata._copy_file = orig
        self.assertIn('Nothing was changed', str(cm.exception))
        base = self.root / userdata.BACKUP_DIR
        self.assertEqual([p.name for p in base.iterdir()], [])           # no .partial and no half-made backup
        self.assertEqual((self.root / 'audio.json').read_bytes(), b'{"a":1}')

    def test_low_disk_space_raises_before_copying(self):
        orig = userdata.shutil.disk_usage
        userdata.shutil.disk_usage = lambda p: shutil._ntuple_diskusage(1 << 40, 1 << 40, 10)
        try:
            with self.assertRaises(userdata.BackupError) as cm:
                userdata.backup(self.root, '1', '2')
        finally:
            userdata.shutil.disk_usage = orig
        self.assertIn('free disk space', str(cm.exception))
        self.assertEqual(userdata.list_backups(self.root), [])

    @unittest.skipUnless(os.name == 'nt', 'junctions are a Windows thing')
    def test_junction_inside_a_save_is_not_followed(self):
        outside = Path(self.td.name) / 'outside'
        outside.mkdir()
        (outside / 'secret.bin').write_bytes(b'must not be copied')
        link = self.root / 'save' / '0001' / 'gens' / '5' / 'jlink'
        import subprocess
        subprocess.run(['cmd', '/c', 'mklink', '/J', str(link), str(outside)], check=True, capture_output=True)
        try:
            r = userdata.backup(self.root, '1', '2')
            self.assertFalse((Path(r['path']) / 'save/0001/gens/5/jlink').exists())
        finally:
            os.rmdir(link)
        self.assertTrue((outside / 'secret.bin').exists())


class RestoreSettings(unittest.TestCase):
    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        self.root = make_root(self.td.name)

    def tearDown(self):
        self.td.cleanup()

    def test_settings_come_back_and_the_old_ones_are_kept(self):
        r = userdata.backup(self.root, '1', '2')
        (self.root / 'audio.json').write_text('{"a":99}')
        (self.root / 'save' / '0001' / 'CURRENT').write_text('SNAPSAVE1\n9\n')   # a save must not be touched by a settings restore
        done = userdata.restore_settings(self.root, r['name'], Quiet())
        self.assertIn('audio.json', done)
        self.assertEqual((self.root / 'audio.json').read_text(), '{"a":1}')
        self.assertEqual((self.root / 'audio.json.before-restore').read_text(), '{"a":99}')
        self.assertEqual((self.root / 'save' / '0001' / 'CURRENT').read_text(), 'SNAPSAVE1\n9\n')
        self.assertFalse([d for d in done if d.startswith('save/') or '/save/' in d])

    def test_unknown_backup(self):
        with self.assertRaises(userdata.BackupError):
            userdata.restore_settings(self.root, '20200101-000000-1-to-2')


class Portable(unittest.TestCase):
    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        self.t = Path(self.td.name)
        self.a, self.b = self.t / 'a', self.t / 'b'
        self.a.mkdir()
        self.b.mkdir()

    def tearDown(self):
        self.td.cleanup()

    def test_not_portable_moves_nothing(self):
        (self.a / 'portable_data').mkdir()
        self.assertEqual(userdata.carry_portable(self.a, self.b), [])
        self.assertTrue((self.a / 'portable_data').exists())

    def test_carry_and_back(self):
        (self.a / 'portable.txt').write_text('')
        (self.a / 'portable_data' / 'Roaming').mkdir(parents=True)
        (self.a / 'portable_data' / 'Roaming' / 'f').write_text('1')
        moved = userdata.carry_portable(self.a, self.b)
        self.assertEqual(set(moved), {'portable.txt', 'portable_data'})
        self.assertTrue((self.b / 'portable_data' / 'Roaming' / 'f').exists())
        self.assertFalse((self.a / 'portable.txt').exists())
        userdata.carry_back(self.a, self.b, moved)
        self.assertTrue((self.a / 'portable_data' / 'Roaming' / 'f').exists())
        self.assertTrue(userdata.is_portable(self.a))
        self.assertFalse(userdata.is_portable(self.b))

    def test_stale_copy_in_the_target_does_not_hide_the_data(self):
        (self.a / 'portable.txt').write_text('')
        (self.a / 'portable_data').mkdir()
        (self.a / 'portable_data' / 'real').write_text('real')
        (self.b / 'portable_data').mkdir()
        (self.b / 'portable_data' / 'stale').write_text('stale')
        userdata.carry_portable(self.a, self.b)
        self.assertEqual(rel_files(self.b / 'portable_data'), ['real'])

    def test_data_root_not_portable_uses_env(self):
        env = {'APPDATA': str(self.t / 'ro'), 'LOCALAPPDATA': str(self.t / 'lo')}
        self.assertEqual(userdata.data_root(self.t, env), self.t / 'ro' / 'SnapRecomp')
        self.assertEqual(userdata.update_state_dir(self.t, env), self.t / 'lo' / 'SnapRecomp' / 'updates')
        env['SNAP_REAL_APPDATA'] = str(self.t / 'real')
        env['SNAP_REAL_LOCALAPPDATA'] = str(self.t / 'reallocal')
        self.assertEqual(userdata.data_root(self.t, env), self.t / 'real' / 'SnapRecomp')
        self.assertEqual(userdata.update_state_dir(self.t, env), self.t / 'reallocal' / 'SnapRecomp' / 'updates')


class PromoteCarriesPortable(unittest.TestCase):
    """pipeline.promote switches out/ for a staged build; portable.txt and portable_data must end up in the new out/."""

    def test_promote(self):
        from snapbuild import pipeline
        with tempfile.TemporaryDirectory() as td:
            work = Path(td) / 'work'
            out, stage = work / 'out', work / 'stage'
            for d in (out, stage):
                d.mkdir(parents=True)
                (d / 'SnapRecomp.exe').write_bytes(b'MZ-' + d.name.encode())
                (d / 'config.json').write_text(json.dumps({'builder_version': '1.0.0'}))
            (out / 'portable.txt').write_text('')
            (out / 'portable_data' / 'Roaming').mkdir(parents=True)
            (out / 'portable_data' / 'Roaming' / 'save').write_text('keep')
            (work / 'builds').mkdir()
            pipeline.promote(work, stage, 'b2', Quiet())
            self.assertEqual((out / 'SnapRecomp.exe').read_bytes(), b'MZ-stage')
            self.assertTrue((out / 'portable.txt').is_file())
            self.assertEqual((out / 'portable_data' / 'Roaming' / 'save').read_text(), 'keep')
            self.assertFalse((work / 'builds' / 'previous' / 'portable_data').exists())


if __name__ == '__main__':
    unittest.main()
