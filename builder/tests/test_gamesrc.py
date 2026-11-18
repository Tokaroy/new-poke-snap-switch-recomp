"""Game folder detection, container reading and extraction, with fake files only (no game data, no real keys, no NSTool)."""
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from snapbuild import containers, gamesrc  # noqa: E402
from snapbuild.util import StepError  # noqa: E402


def pfs0(members):
    """members: {name: bytes} -> PFS0 bytes."""
    names = b''
    offs = []
    for n in members:
        offs.append(len(names))
        names += n.encode() + b'\0'
    while len(names) % 0x20:
        names += b'\0'
    ents, data, pos = b'', b'', 0
    for (n, b), no in zip(members.items(), offs):
        ents += struct.pack('<QQII', pos, len(b), no, 0)
        data += b
        pos += len(b)
    return struct.pack('<4sIII', b'PFS0', len(members), len(names), 0) + ents + names + data


def hfs0(members):
    names = b''
    offs = []
    for n in members:
        offs.append(len(names))
        names += n.encode() + b'\0'
    while len(names) % 0x20:
        names += b'\0'
    ents, data, pos = b'', b'', 0
    for (n, b), no in zip(members.items(), offs):
        ents += struct.pack('<QQII', pos, len(b), no, 0) + b'\0' * 0x28    # 0x40 per entry: offset, size, name, hashed size, reserved, hash
        data += b
        pos += len(b)
    return struct.pack('<4sIII', b'HFS0', len(members), len(names), 0) + ents + names + data


def xci(secure):
    root_members = {'secure': hfs0(secure), 'normal': hfs0({}), 'update': hfs0({'fw.nca': b'F' * 10})}
    root = hfs0(root_members)
    head = bytearray(0x200)
    head[0x100:0x104] = b'HEAD'
    head[0x130:0x138] = struct.pack('<Q', len(head))
    return bytes(head) + root


CNMT_TEXT = ('Version:   1.0.0 (v{v})\nTitleId:   0x{t}\nType:      {ty} (0)\n    Attributes: 0\n'
             '  ContentInfo:\n    Type: Program (1)\n    Id: {pid}\n    Size: 0x10\n    Hash: {h}\n')


def cnmt_nca(title, version, typ, pid):
    return CNMT_TEXT.format(v=version, t=title, ty=typ, pid=pid, h='ab' * 32).encode()


class FakeTool:
    """Stands in for NSTool: the fake cnmt NCA holds the text the real tool would print."""

    def __init__(self, fail_keys=False):
        self.calls = []
        self.fail_keys = fail_keys

    def run(self, args, timeout=0):
        args = [str(a) for a in args]
        self.calls.append(args)
        if self.fail_keys:
            return 0, ('[WARNING] Ticket "x" will not be imported. Could not decrypt title key.\n  Content Type:    Meta\n'
                       '  Key Generation:  12\n[WARNING] NCA Partition 0 not readable. (AES-CTR Key was not determined)\n')
        if '--part0' in args:
            out = Path(args[args.index('--part0') + 1])
            nca = Path(args[-1])
            out.mkdir(parents=True, exist_ok=True)
            if nca.name.endswith('.cnmt.nca'):
                (out / 'x.cnmt').write_bytes(nca.read_bytes())
            else:
                (out / 'main').write_bytes(b'NSO0main')
                (out / 'sdk').write_bytes(b'NSO0sdk')
            return 0, ''
        if '--part1' in args:
            out = Path(args[args.index('--part1') + 1])
            (out / 'nx').mkdir(parents=True)
            (out / 'nx' / 'f.bin').write_bytes(b'1')
            return 0, ''
        if '-t' in args:
            return 0, Path(args[-1]).read_text()
        return 1, 'unknown'


def make_folder(root, versions=(262144,), with_base=True, with_keys=True):
    root = Path(root)
    if with_base:
        (root / 'base.xci').write_bytes(xci({
            'aaaa.cnmt.nca': cnmt_nca('0100f4300bf2c000', 0, 'Application', 'b' * 32),
            'b' * 32 + '.nca': b'BASEPROGRAM' * 10}))
    for v in versions:
        pid = f'{v:032x}'
        (root / f'upd{v}.nsp').write_bytes(pfs0({
            f'cn{v}.cnmt.nca': cnmt_nca('0100f4300bf2c800', v, 'Patch', pid),
            pid + '.nca': b'PATCHPROGRAM' * 5,
            '0100f4300bf2c800000000000000000b.tik': b'TIK'}))
    if with_keys:
        (root / 'prod.keys').write_text('master_key_00 = 0\n')   # fake; never real material
    return root


class ContainerTests(unittest.TestCase):
    def setUp(self):
        self.t = Path(tempfile.mkdtemp())

    def tearDown(self):
        shutil.rmtree(self.t, ignore_errors=True)

    def test_pfs0_members_and_copy(self):
        p = self.t / 'a.nsp'
        p.write_bytes(pfs0({'one.bin': b'12345', 'two.bin': b'abc'}))
        m = containers.members(p)
        self.assertEqual(set(m), {'one.bin', 'two.bin'})
        off, size, _ = m['two.bin']
        out = containers.copy_member(p, off, size, self.t / 'o' / 'two.bin', check_free=False)
        self.assertEqual(out.read_bytes(), b'abc')
        self.assertFalse((self.t / 'o' / 'two.bin.part').exists())

    def test_xci_secure_and_normal_only(self):
        p = self.t / 'g.xci'
        p.write_bytes(xci({'x.nca': b'XX'}))
        m = containers.members(p)
        self.assertIn('x.nca', m)
        self.assertNotIn('fw.nca', m)       # the update partition (console firmware) is never read
        off, size, part = m['x.nca']
        self.assertEqual(part, 'secure')
        self.assertEqual(p.read_bytes()[off:off + size], b'XX')

    def test_bad_files_give_plain_errors(self):
        (self.t / 'bad.xci').write_bytes(b'\0' * 1000)
        with self.assertRaises(containers.ContainerError):
            containers.members(self.t / 'bad.xci')
        (self.t / 'cut.nsp').write_bytes(pfs0({'a': b'1'})[:10])
        with self.assertRaises(containers.ContainerError):
            containers.members(self.t / 'cut.nsp')
        (self.t / 'z.nsz').write_bytes(b'x')
        with self.assertRaises(containers.ContainerError) as c:
            containers.members(self.t / 'z.nsz')
        self.assertIn('unpacked', str(c.exception))

    def test_copy_cut_short_leaves_nothing(self):
        p = self.t / 'a.nsp'
        p.write_bytes(pfs0({'one.bin': b'12345'}))
        off, size, _ = containers.members(p)['one.bin']
        with self.assertRaises(containers.ContainerError):
            containers.copy_member(p, off, size + 100, self.t / 'o.bin', check_free=False)
        self.assertEqual(list(self.t.glob('o.bin*')), [])


class ScanTests(unittest.TestCase):
    def setUp(self):
        self.t = Path(tempfile.mkdtemp())

    def tearDown(self):
        shutil.rmtree(self.t, ignore_errors=True)

    def test_dump_folder(self):
        make_folder(self.t, versions=(65536, 262144))
        sc = gamesrc.scan(self.t, env={})
        self.assertEqual(sc.kind, 'dump')
        self.assertEqual(len(sc.dumps), 3)
        self.assertEqual(sc.keys, self.t / 'prod.keys')

    def test_extracted_folder(self):
        e = self.t / 'dump'
        (e / 'exefs').mkdir(parents=True)
        (e / 'exefs' / 'main').write_bytes(b'NSO0' + b'\0' * 8)
        (e / 'exefs' / 'sdk').write_bytes(b'NSO0' + b'\0' * 8)
        (e / 'romfs' / 'nx').mkdir(parents=True)
        (e / 'romfs' / 'common').mkdir()
        sc = gamesrc.scan(self.t, env={})
        self.assertEqual(sc.kind, 'extracted')
        self.assertEqual(sc.extracted['main'].name, 'main')
        self.assertEqual(sc.extracted['romfs'].name, 'romfs')

    def test_empty_and_compressed(self):
        sc = gamesrc.scan(self.t, env={})
        self.assertIsNone(sc.kind)
        self.assertTrue(sc.notes)
        (self.t / 'g.nsz').write_bytes(b'x')
        sc = gamesrc.scan(self.t, env={})
        self.assertIn('.nsz', sc.notes[0])

    def test_missing_folder(self):
        with self.assertRaises(StepError):
            gamesrc.scan(self.t / 'nope', env={})

    def test_emulator_keys_offered_not_used(self):
        a = self.t / 'AppData'
        (a / 'Ryujinx' / 'system').mkdir(parents=True)
        (a / 'Ryujinx' / 'system' / 'prod.keys').write_text('x')
        sc = gamesrc.scan(self.t, env={'APPDATA': str(a), 'USERPROFILE': str(self.t / 'nohome')})
        self.assertIsNone(sc.keys)
        self.assertEqual(len(sc.emulator_keys), 1)


class IdentifyPickTests(unittest.TestCase):
    def setUp(self):
        self.t = Path(tempfile.mkdtemp())
        self.scratch = self.t / 'scratch'

    def tearDown(self):
        shutil.rmtree(self.t, ignore_errors=True)

    def identified(self, **kw):
        d = Path(tempfile.mkdtemp(dir=self.t))
        make_folder(d, **kw)
        sc = gamesrc.scan(d, env={})
        gamesrc.identify_all(sc, FakeTool(), self.scratch, say=lambda m: None)
        return sc

    def test_picks_201_and_ignores_older(self):
        sc = self.identified(versions=(65536, 196608, 262144))
        plan = gamesrc.pick(sc)
        self.assertEqual(plan.patch[1]['version'], 262144)
        self.assertEqual(plan.base[1]['version'], 0)
        self.assertFalse((self.scratch / 'ident').exists())      # scratch cleaned up

    def test_updates_up_to_200_say_so(self):
        sc = self.identified(versions=(65536, 196608))
        with self.assertRaises(StepError) as c:
            gamesrc.pick(sc)
        self.assertIn('updates up to 2.0.0', str(c.exception))
        self.assertIn('needs the 2.0.1 update', str(c.exception))
        self.assertNotIn('download', str(c.exception).lower())

    def test_no_update_and_no_base(self):
        with self.assertRaises(StepError) as c:
            gamesrc.pick(self.identified(versions=()))
        self.assertIn('base game only', str(c.exception))
        with self.assertRaises(StepError) as c:
            gamesrc.pick(self.identified(with_base=False))
        self.assertIn('not the base game', str(c.exception))

    def test_key_failure_is_one_plain_message(self):
        d = self.t / 'src2'
        d.mkdir()
        make_folder(d)
        sc = gamesrc.scan(d, env={})
        gamesrc.identify_all(sc, FakeTool(fail_keys=True), self.scratch, say=lambda m: None)
        with self.assertRaises(StepError) as c:
            gamesrc.pick(sc)
        self.assertTrue(c.exception.about_keys)
        self.assertIn('"master_key_0b" (key generation 12)', str(c.exception))     # the key's name, never a value
        self.assertEqual(str(c.exception).count('master_key_0b'), 1)

    def test_scrub_removes_hex(self):
        s = gamesrc.scrub('error 00112233445566778899aabbccddeeff00112233 done')
        self.assertNotIn('0011', s)
        self.assertIn('<hex>', s)


class ExtractTests(unittest.TestCase):
    def setUp(self):
        self.t = Path(tempfile.mkdtemp())
        src = self.t / 'src'
        src.mkdir()
        make_folder(src)
        self.src = src
        self.sc = gamesrc.scan(src, env={})
        self.tool = FakeTool()
        gamesrc.identify_all(self.sc, self.tool, self.t / 'scratch', say=lambda m: None)
        self.plan = gamesrc.pick(self.sc)

    def tearDown(self):
        shutil.rmtree(self.t, ignore_errors=True)

    def test_extract_writes_only_into_game_dir(self):
        before = {p: p.stat().st_mtime_ns for p in self.src.iterdir()}
        g = self.t / 'work' / 'game'
        r = gamesrc.extract(self.plan, self.tool, g, say=lambda m: None)
        self.assertTrue(r['main'].is_file() and r['sdk'].is_file())
        self.assertTrue((r['romfs'] / 'nx' / 'f.bin').is_file())
        self.assertFalse((g / '_scratch').exists())
        self.assertFalse((g / 'romfs.partial').exists())
        self.assertEqual({p: p.stat().st_mtime_ns for p in self.src.iterdir()}, before)     # source folder untouched
        self.assertEqual(sorted(p.name for p in self.src.iterdir()), sorted(before_name.name for before_name in before))
        self.assertEqual(len(r['hashes']['base_program_nca']), 64)
        # the romfs call used the base NCA and the update's ticket
        rom = next(c for c in self.tool.calls if '--part1' in c)
        self.assertIn('--basenca', rom)
        self.assertIn('--tik', rom)
        # resume: a finished extraction is found again
        self.assertEqual(gamesrc.extracted_already(g)['main'], r['main'])
        shutil.rmtree(g / 'romfs')
        self.assertIsNone(gamesrc.extracted_already(g))

    def test_extract_key_failure_message(self):
        class Bad(FakeTool):
            def run(self, args, timeout=0):
                if '--part0' in [str(a) for a in args]:
                    return 0, ('  Content Type:    Program\n  Key Generation:  11\n'
                               '[WARNING] NCA Partition 0 not readable. (AES-CTR Key was not determined)\n')
                return super().run(args, timeout)
        with self.assertRaises(StepError) as c:
            gamesrc.extract(self.plan, Bad(), self.t / 'w' / 'game', say=lambda m: None)
        self.assertEqual(c.exception.code, 'keys_generation')
        self.assertIn('master_key_0a', str(c.exception))
        self.assertFalse((self.t / 'w' / 'game' / '_scratch').exists())

    def test_space_estimate_says_romfs_stays(self):
        e = gamesrc.space_estimate(self.plan)
        self.assertGreater(e['peak'], e['stays'])
        self.assertIn('stays', e['note'])
        self.assertEqual(gamesrc.space_estimate(self.plan, True)['peak'], 0)


class RememberTests(unittest.TestCase):
    def setUp(self):
        self.t = Path(tempfile.mkdtemp())

    def tearDown(self):
        shutil.rmtree(self.t, ignore_errors=True)

    def test_remember_and_moved_files(self):
        src = self.t / 'src'
        src.mkdir()
        make_folder(src)
        sc = gamesrc.scan(src, env={})
        gamesrc.identify_all(sc, FakeTool(), self.t / 's', say=lambda m: None)
        plan = gamesrc.pick(sc)
        st = gamesrc.remember({}, sc, plan, {'x': 'y'}, keys_path=sc.keys)
        sp = self.t / 'cfg' / 'settings.json'
        gamesrc.save_settings(st, sp)
        loaded = gamesrc.load_settings(sp)
        self.assertEqual(loaded['game']['kind'], 'dump')
        self.assertNotIn('master_key', json.dumps(loaded))           # only the keys PATH is stored
        self.assertTrue(gamesrc.still_there(loaded)[0])
        (src / 'upd262144.nsp').unlink()
        ok, why = gamesrc.still_there(loaded)
        self.assertFalse(ok)
        self.assertIn('upd262144.nsp', why)

    def test_load_missing_is_empty(self):
        self.assertEqual(gamesrc.load_settings(self.t / 'none.json'), {})


if __name__ == '__main__':
    unittest.main()
