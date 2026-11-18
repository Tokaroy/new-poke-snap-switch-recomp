# SPDX-License-Identifier: GPL-3.0-or-later
"""Portable by default: where an install keeps its files, and what happens to stored paths when its folder is moved."""
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from snapbuild import cache, fetch, gamesrc, paths, pipeline, setup_engine as se   # noqa: E402


class PathsTests(unittest.TestCase):
    def setUp(self):
        self.t = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.t, True)
        self.env = {'LOCALAPPDATA': str(self.t / 'local'), 'APPDATA': str(self.t / 'appdata')}

    def test_root_is_found_from_the_builders_own_place(self):
        b = self.t / 'Snap' / 'setup' / 'app' / '1.3.0' / 'builder'
        self.assertEqual(paths.install_root(self.env, b), self.t / 'Snap')
        self.assertEqual(paths.setup_dir(self.env, b), self.t / 'Snap' / 'setup')
        self.assertEqual(paths.tools_dir(self.env, b), self.t / 'Snap' / 'tools')
        self.assertEqual(paths.cache_dir(self.env, b), self.t / 'Snap' / 'cache')
        self.assertEqual(paths.base_dir(self.env, b), self.t / 'Snap')

    def test_source_checkout_keeps_the_per_user_folder(self):
        b = self.t / 'repo' / 'builder'
        self.assertIsNone(paths.install_root(self.env, b))
        self.assertEqual(paths.base_dir(self.env, b), self.t / 'local' / 'SnapRecomp')
        self.assertEqual(paths.setup_dir(self.env, b), self.t / 'local' / 'SnapRecomp' / 'setup')

    def test_a_portable_game_starting_the_builder_does_not_hide_the_real_folder(self):
        env = dict(self.env, LOCALAPPDATA=str(self.t / 'game' / 'portable_data' / 'Local'), SNAP_REAL_LOCALAPPDATA=str(self.t / 'local'))
        self.assertEqual(paths.base_dir(env, self.t / 'repo' / 'builder'), self.t / 'local' / 'SnapRecomp')

    def test_overrides(self):
        env = dict(self.env, SNAP_INSTALL_ROOT=str(self.t / 'R'), SNAP_TOOLS_DIR=str(self.t / 'T'), SNAP_SETUP_DIR=str(self.t / 'S'))
        self.assertEqual(paths.install_root(env, self.t / 'x'), self.t / 'R')
        self.assertEqual(paths.tools_dir(env), self.t / 'T')
        self.assertEqual(paths.setup_dir(env), self.t / 'S')
        self.assertEqual(paths.cache_dir(env), self.t / 'R' / 'cache')

    def test_tools_from_an_older_version_are_reused_not_downloaded_again(self):
        b = self.t / 'Snap' / 'setup' / 'app' / '1.3.0' / 'builder'
        old = self.t / 'local' / 'SnapRecomp' / 'tools'
        (old / 'llvm').mkdir(parents=True)
        self.assertEqual(paths.tools_dir(self.env, b), old)
        (self.t / 'Snap' / 'tools').mkdir(parents=True)   # once the install has its own tools folder, that one is used
        self.assertEqual(paths.tools_dir(self.env, b), self.t / 'Snap' / 'tools')

    def test_the_builder_modules_use_these_folders(self):
        with mock.patch.dict(os.environ, dict(self.env, SNAP_INSTALL_ROOT=str(self.t / 'R'))):
            for k in ('SNAP_SETUP_DIR', 'SNAP_TOOLS_DIR'):
                os.environ.pop(k, None)
            self.assertEqual(Path(se.default_root()), self.t / 'R')
            self.assertEqual(Path(se.setup_dir()), self.t / 'R' / 'setup')
            self.assertEqual(Path(fetch.default_tools_dir()), self.t / 'R' / 'tools')
            self.assertEqual(Path(cache.default_root()), self.t / 'R' / 'cache')
            self.assertEqual(Path(gamesrc.settings_path()), self.t / 'R' / 'setup' / 'settings.json')

    def test_rebase_moves_only_paths_under_the_old_root(self):
        old, new = 'D:\\Games\\Snap', 'E:\\Moved Here\\Snap'
        s = {'exe': 'D:\\Games\\Snap\\work\\out\\SnapRecomp.exe', 'root': 'd:/games/snap', 'other': 'D:\\Games\\Snapshots\\x', 'n': 3,
             'game': {'folder': 'C:\\dumps\\snap', 'extract_dir': 'D:\\Games\\Snap\\game'}, 'shortcuts': ['C:\\Users\\p\\Desktop\\Snap Recomp.lnk']}
        self.assertTrue(paths.rebase(s, old, new))
        self.assertEqual(s['exe'], 'E:\\Moved Here\\Snap\\work\\out\\SnapRecomp.exe')
        self.assertEqual(s['root'], 'E:\\Moved Here\\Snap')
        self.assertEqual(s['other'], 'D:\\Games\\Snapshots\\x')   # a longer name that only starts the same is another folder
        self.assertEqual(s['game'], {'folder': 'C:\\dumps\\snap', 'extract_dir': 'E:\\Moved Here\\Snap\\game'})
        self.assertEqual(s['shortcuts'], ['C:\\Users\\p\\Desktop\\Snap Recomp.lnk'])
        self.assertFalse(paths.rebase(s, old, new))
        self.assertFalse(paths.rebase({'a': 'D:\\Games\\Snap\\x'}, old, 'd:\\games\\snap\\'))

    def test_settings_follow_a_moved_folder(self):
        a, b = self.t / 'A', self.t / 'B'
        with mock.patch.dict(os.environ, dict(self.env, SNAP_INSTALL_ROOT=str(a))):
            os.environ.pop('SNAP_SETUP_DIR', None)
            se.save_settings({'root': str(a), 'exe': str(a / 'work' / 'out' / 'SnapRecomp.exe'), 'tools_dir': str(a / 'tools'),
                              'game': {'folder': str(self.t / 'dump')}, 'completed': True})
            self.assertEqual(se.load_settings()['install_root'], str(a))
        shutil.move(str(a), str(b))
        with mock.patch.dict(os.environ, dict(self.env, SNAP_INSTALL_ROOT=str(b))):
            os.environ.pop('SNAP_SETUP_DIR', None)
            s = se.load_settings()
            self.assertEqual((s['root'], s['exe'], s['tools_dir']), (str(b), str(b / 'work' / 'out' / 'SnapRecomp.exe'), str(b / 'tools')))
            self.assertEqual(s['game']['folder'], str(self.t / 'dump'))
            self.assertEqual(s['install_root'], str(b))
            self.assertIn(str(b / 'work'), (b / 'setup' / 'settings.json').read_text(encoding='utf-8').replace('\\\\', '\\'))   # written back

    def test_config_keys_for_a_moved_install(self):
        r = self.t / 'Snap'
        self.assertEqual(pipeline.move_keys(r / 'work', r), {'install_root': str(r.resolve()), 'exe_rel': 'work\\out'})
        # a work folder somewhere else: its own parent is the root, so the work folder can still be moved
        self.assertEqual(pipeline.move_keys(self.t / 'elsewhere' / 'w', r), {'install_root': str((self.t / 'elsewhere').resolve()), 'exe_rel': 'w\\out'})
        self.assertEqual(pipeline.move_keys(Path(Path.cwd().anchor) / 'w', r), {})


if __name__ == '__main__':
    unittest.main()
