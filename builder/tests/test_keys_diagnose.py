"""Issue 1 ("prod key error"): which failures are a keys problem and which are not.

Fake NSTool output and fake keys files only: the output texts copy the wording NSTool 1.9.2 prints, the key values are zeros.
"""
import json
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from snapbuild import gamesrc, setup_engine  # noqa: E402
from snapbuild.util import StepError  # noqa: E402
import test_gamesrc as tg  # noqa: E402

Z16, Z32 = '00' * 16, '00' * 32
FAKE_USER_DIR = 'C:\\Users\\Some Player\\Downloads\\Snap Recomp\\setup\\scratch_ident\\ident'

# printed by NSTool on every run, good or bad
ALWAYS = (f'[WARNING] "--part0 {FAKE_USER_DIR}\\a.cnmt.nca.d" is deprecated. Consider using "-x \\0 {FAKE_USER_DIR}\\a.cnmt.nca.d" instead.\n'
          '[WARNING] Failed to load "title.keys" keyfile.\n')


def header(content='Meta', generation=11, rights=False):
    return ('[NCA Header]\n  Format Type:     NCA3\n  Dist. Type:      Download\n  Content Type:    %s\n  Key Generation:  %d\n'
            '  Sig. Generation: 0\n  Kaek Index:      Application (0)\n  Size:            0x1400\n  ProgID:          0x0100f4300bf2c000\n'
            '  Content Index:   0\n  SdkAddon Ver.:   13.2.0 (v218234880)\n' % (content, generation)
            + ('  RightsId:        0100f4300bf2c800000000000000000b\n' if rights else ''))


NO_KEY = '[WARNING] NCA Partition 0 not readable. (AES-CTR Key was not determined)\n[ContentArchive/Extract] [WARNING] Failed to extract virtual path: "\\0"\n'
TIK_NOT_IMPORTED = '[WARNING] Ticket "0100f4300bf2c800000000000000000b" will not be imported. Could not decrypt title key.\n'

OUT = {
    'old_keys_meta': ALWAYS + TIK_NOT_IMPORTED + header() + NO_KEY,
    'old_keys_no_ticket': ALWAYS + header() + NO_KEY,
    'old_keys_program': ALWAYS + TIK_NOT_IMPORTED + header('Program', 11, True) + NO_KEY,
    'empty_keys': ALWAYS + '[WARNING] Failed to load NCA Header Key.\n[nstool::SettingsInitializer ERROR] Input file type was undetermined.\n',
    'bad_line': ALWAYS + '[nstool::KeyBagInitializer ERROR] Key: "package2_key_00" has incorrect length\n',
    'no_ticket': ALWAYS + header('Program', 11, True) + NO_KEY,
    'personalized': (ALWAYS + '[WARNING] Ticket "0100f4300bf2c800000000000000000b" will not be imported. Personalised tickets are not supported.\n'
                     + header('Program', 11, True) + NO_KEY),
    'hash': ALWAYS + header() + '[WARNING] NCA Partition 0 not readable. (Hash layer 0 failed hash verification.)\n',
    'cut_short': ALWAYS + header('Program') + '[WARNING] NCA Partition 0 not readable. (SubStream length is greater than base stream length)\n',
    'not_nca': ALWAYS + '[nstool::SettingsInitializer ERROR] Input file type was undetermined.\n',
    'long_path': (ALWAYS + f'[WARNING] Failed to open ticket "{FAKE_USER_DIR}\\t.tik" (The system cannot find the path specified.).\n'
                  '[tc::io::FileStream::open() ERROR] The system cannot find the path specified.\n'),
    'cannot_write': (ALWAYS + header() + '[ContentArchive/Extract] [tc::io::LocalFileSystem::createDirectory() ERROR] Failed to create directory '
                     '(The system cannot find the path specified.)\n'),
    'nothing': '',
}


def keys_text(generations=range(0x16), header_key=True, extra=''):
    t = ''.join(f'master_key_{g:02x} = {Z16}\nkey_area_key_application_{g:02x} = {Z16}\ntitlekek_{g:02x} = {Z16}\n' for g in generations)
    return (f'header_key = {Z32}\n' if header_key else '') + t + extra


class Base(unittest.TestCase):
    def setUp(self):
        self.t = Path(tempfile.mkdtemp())

    def tearDown(self):
        shutil.rmtree(self.t, ignore_errors=True)

    def keys(self, text, name='prod.keys', raw=None):
        p = self.t / name
        p.parent.mkdir(parents=True, exist_ok=True)
        if raw is not None:
            p.write_bytes(raw)
        else:
            p.write_text(text, encoding='utf-8')
        return p


class DiagnoseTests(Base):
    def test_always_printed_warnings_are_not_a_keys_problem(self):
        # the old check called any output with the word "key" a keys problem; NSTool prints `title.keys` every time
        for case in ('hash', 'cut_short', 'not_nca', 'long_path', 'cannot_write', 'no_ticket', 'personalized', 'nothing'):
            self.assertFalse(gamesrc.key_problem(OUT[case]), case)
            self.assertNotIn(gamesrc.diagnose(OUT[case]).code, gamesrc.KEY_CODES, case)
        for case in ('old_keys_meta', 'old_keys_no_ticket', 'old_keys_program', 'empty_keys', 'bad_line'):
            self.assertTrue(gamesrc.key_problem(OUT[case]), case)

    def test_codes(self):
        want = {'old_keys_meta': 'keys_generation', 'old_keys_no_ticket': 'keys_generation', 'old_keys_program': 'keys_generation',
                'empty_keys': 'keys_not_keys', 'bad_line': 'keys_bad_line', 'no_ticket': 'ticket_missing', 'personalized': 'ticket_personalized',
                'hash': 'damaged', 'cut_short': 'cut_short', 'not_nca': 'damaged', 'long_path': 'work_folder', 'cannot_write': 'work_folder',
                'nothing': 'unreadable'}
        for case, code in want.items():
            self.assertEqual(gamesrc.diagnose(OUT[case], 'game.nsp').code, code, case)

    def test_missing_generation_is_named(self):
        old = gamesrc.inspect_keys(self.keys(keys_text(range(0x0a))))
        self.assertIsNone(old.problem)
        pr = gamesrc.diagnose(OUT['old_keys_meta'], 'update.nsp', old, had_ticket=True)
        self.assertEqual(pr.code, 'keys_generation')
        self.assertIn('master_key_0a', pr.text)
        self.assertIn('key generation 11', pr.text)
        self.assertIn('update.nsp', pr.text)
        self.assertIn('"prod.keys"', pr.text)

    def test_key_index(self):
        self.assertEqual([gamesrc.key_index(g) for g in (0, 1, 2, 11, 22)], [0, 0, 1, 10, 21])

    def test_ticket_cases_name_the_ticket_not_the_keys(self):
        new = gamesrc.inspect_keys(self.keys(keys_text()))
        pr = gamesrc.diagnose(OUT['no_ticket'], 'base.nsp', new, had_ticket=False)
        self.assertEqual(pr.code, 'ticket_missing')
        self.assertIn('ticket', pr.text)
        self.assertIn('not a keys problem', pr.text)
        self.assertEqual(gamesrc.diagnose(OUT['no_ticket'], 'base.nsp', new, had_ticket=True).code, 'ticket_mismatch')
        self.assertIn('personalized', gamesrc.diagnose(OUT['personalized'], 'base.nsp', new, had_ticket=True).text)
        # the same NSTool text with keys that lack the generation is a keys problem
        old = gamesrc.inspect_keys(self.keys(keys_text(range(0x0a)), 'old.keys'))
        self.assertEqual(gamesrc.diagnose(OUT['no_ticket'], 'base.nsp', old, had_ticket=True).code, 'keys_generation')

    def test_bad_line_names_the_key(self):
        pr = gamesrc.diagnose(OUT['bad_line'], 'x.nsp')
        self.assertIn('"package2_key_00"', pr.text)
        self.assertIn('wrong length', pr.text)

    def test_messages_name_no_source(self):
        k = gamesrc.inspect_keys(self.keys(keys_text(range(0x0a))))
        for case, out in OUT.items():
            for keys in (None, k):
                text = gamesrc.diagnose(out, 'game.nsp', keys).text.lower()
                for word in ('download', 'http', 'www', 'lockpick', 'google', 'search for', 'website', 'torrent'):
                    self.assertNotIn(word, text.replace('download-type', ''), (case, word))

    def test_reason_is_safe_to_post(self):
        secret = 'ab' * 16
        out = OUT['long_path'] + f'[WARNING] something {secret} happened in C:\\Users\\Some Player\\x\\y.nca\n'
        r = gamesrc.reason(out)
        self.assertNotIn('Some Player', r)
        self.assertNotIn('Users', r)
        self.assertNotIn(secret, r)
        self.assertNotIn('title.keys', r)
        self.assertNotIn('deprecated', r)
        self.assertIn('cannot find the path', r)
        self.assertIn('"t.tik"', r)
        self.assertIn('key generation 11', gamesrc.reason(OUT['old_keys_meta']))
        self.assertEqual(gamesrc.reason(''), 'the reader gave no message')


class InspectKeysTests(Base):
    def test_formats_that_work(self):
        body = keys_text()
        for label, raw in (('crlf', body.replace('\n', '\r\n').encode()), ('bom', b'\xef\xbb\xbf' + body.encode()),
                           ('upper', body.upper().encode()), ('tabs', body.replace(' = ', '\t=\t').encode()),
                           ('tight', body.replace(' = ', '=').encode()), ('trailing', body.replace('\n', '  \n').encode()),
                           ('comments', b'# my keys\n; note\n\n' + body.encode()), ('utf16', body.encode('utf-16'))):
            k = gamesrc.inspect_keys(self.keys(None, f'{label}.keys', raw=raw))
            self.assertIsNone(k.problem, label)
            self.assertTrue(k.has_generation(0x0a), label)
            self.assertEqual(k.generations()[-1], 0x15, label)

    def test_newer_generations_and_unknown_names_are_fine(self):
        extra = ''.join(f'master_key_{g:02x} = {Z16}\n' for g in (0x16, 0x17, 0x18)) + f'mariko_bek = {Z16}\nsome_future_key = {Z16}\n'
        k = gamesrc.inspect_keys(self.keys(keys_text(extra=extra)))
        self.assertIsNone(k.problem)
        self.assertEqual(k.generations()[-1], 0x18)

    def test_derived_keys_without_master_keys(self):
        k = gamesrc.inspect_keys(self.keys(f'header_key = {Z32}\nkey_area_key_application_0a = {Z16}\n'))
        self.assertIsNone(k.problem)
        self.assertTrue(k.has_generation(0x0a))
        self.assertFalse(k.has_generation(0x0a, titlekey=True))
        self.assertFalse(k.has_generation(0x0b))

    def test_header_key_from_sources(self):
        k = gamesrc.inspect_keys(self.keys(f'header_kek_source = {Z16}\nheader_key_source = {Z32}\nmaster_key_00 = {Z16}\n'))
        self.assertIsNone(k.problem)

    def test_files_that_cannot_work(self):
        cases = {
            'empty': (b'', 'keys_not_keys', 'no keys in it'),
            'garbage': (b'hello\nthis is a note\n', 'keys_not_keys', 'no keys in it'),
            'title': (('0100f4300bf2c800000000000000000b = %s\n' % Z16).encode(), 'keys_not_keys', 'title keys'),
            'noheader': (keys_text(header_key=False).replace('master_key_00', 'master_key_zz').encode(), 'keys_not_keys', 'header_key'),
            'emptyvalue': (b'header_key = \nmaster_key_0a = \n', 'keys_not_keys', 'header_key'),
        }
        for label, (raw, code, word) in cases.items():
            k = gamesrc.inspect_keys(self.keys(None, f'{label}.keys', raw=raw))
            self.assertEqual(k.code, code, label)
            self.assertIn(word, k.problem, label)
        self.assertEqual(gamesrc.inspect_keys(self.t / 'gone.keys').code, 'keys_unreadable')
        self.assertEqual(gamesrc.inspect_keys(self.t).code, 'keys_unreadable')

    def test_summary_has_names_and_counts_only(self):
        marker = 'c0ffee' * 6
        k = gamesrc.inspect_keys(self.keys(keys_text().replace(Z32, marker + '00' * 14)))
        s = k.summary()
        self.assertIn('master keys 00 to 15', s)
        self.assertIn('header key present', s)
        self.assertNotIn(marker, s)
        self.assertNotIn(marker, json.dumps(k.names))


class FindKeysTests(Base):
    def test_exact_name_wins(self):
        (self.t / 'sub').mkdir()
        self.keys('x', 'keys.txt')
        self.keys('x', 'prod.keys.txt')
        self.keys('x', 'sub/prod.keys')
        self.assertEqual(gamesrc.find_keys_in(self.t), self.t / 'sub' / 'prod.keys')

    def test_usual_wrong_names_are_found(self):
        for name in ('prod.keys.txt', 'keys.txt', 'PROD.KEYS', 'prod (1).keys', 'prod.keys.keys', 'sub/a/prod.keys.txt'):
            d = self.t / ('case_' + name.replace('/', '_').replace(' ', '_'))
            d.mkdir()
            p = d / name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text('x')
            self.assertEqual(gamesrc.find_keys_in(d), p, name)

    def test_other_files_are_not_taken(self):
        for name in ('title.keys', 'dev.keys', 'notes.txt', 'product.txt', 'console.keys'):
            self.keys('x', name)
        self.assertIsNone(gamesrc.find_keys_in(self.t))

    def test_path_with_spaces_and_other_alphabets(self):
        d = self.t / 'my games é ポケ'
        d.mkdir()
        (d / 'prod.keys').write_text('x')
        self.assertEqual(gamesrc.find_keys_in(self.t), d / 'prod.keys')


class OutputTool(tg.FakeTool):
    """Fails the cnmt step (or a later one) with a given NSTool text."""

    def __init__(self, text, fail=('--part0',), only=None):
        super().__init__()
        self.text, self.fail, self.only = text, fail, only

    def run(self, args, timeout=0):
        a = [str(x) for x in args]
        if any(f in a for f in self.fail) and (self.only is None or self.only(a)):
            self.calls.append(a)
            return 0, self.text
        return super().run(args, timeout)


class PickTests(Base):
    def scanned(self, tool, **kw):
        d = self.t / 'src'
        d.mkdir()
        tg.make_folder(d, **kw)
        (d / 'prod.keys').write_text(keys_text(), encoding='utf-8')
        sc = gamesrc.scan(d, env={})
        self.said = []
        gamesrc.identify_all(sc, tool, self.t / 'scratch', say=self.said.append)
        return sc

    def fail(self, sc):
        with self.assertRaises(gamesrc.GameFilesError) as c:
            gamesrc.pick(sc)
        return c.exception

    def test_old_keys(self):
        d = self.t / 'src'
        d.mkdir()
        tg.make_folder(d)
        (d / 'prod.keys').write_text(keys_text(range(0x0a)), encoding='utf-8')
        sc = gamesrc.scan(d, env={})
        said = []
        gamesrc.identify_all(sc, OutputTool(OUT['old_keys_meta']), self.t / 'scratch', say=said.append)
        e = self.fail(sc)
        self.assertEqual(e.code, 'keys_generation')
        self.assertTrue(e.about_keys)
        self.assertIn('master_key_0a', str(e))
        self.assertEqual(str(e).count('master_key_0a'), 1)        # said once, not once per file
        log = '\n'.join(said)
        self.assertIn('master keys 00 to 09', log)
        self.assertIn('AES-CTR Key was not determined', log)
        self.assertIn('keys_generation', log)

    def test_damaged_files_are_not_a_keys_error(self):
        e = self.fail(self.scanned(OutputTool(OUT['hash'])))
        self.assertFalse(e.about_keys)
        self.assertEqual(e.code, 'damaged')
        self.assertIn('base.xci is damaged', str(e))
        self.assertIn('upd262144.nsp is damaged', str(e))
        self.assertIn('not a keys problem', str(e))
        self.assertIn('failed hash verification', '\n'.join(self.said))

    def test_long_path_is_not_a_keys_error(self):
        e = self.fail(self.scanned(OutputTool(OUT['long_path'])))
        self.assertEqual(e.code, 'work_folder')
        self.assertIn('short folder', str(e))
        self.assertNotIn('Some Player', '\n'.join(self.said))

    def test_update_without_base(self):
        e = self.fail(self.scanned(tg.FakeTool(), with_base=False))
        self.assertEqual(e.code, 'no_base')
        self.assertIn('update', str(e))
        self.assertIn('not the base game', str(e))

    def test_update_read_but_base_damaged(self):
        tool = OutputTool(OUT['cut_short'], only=lambda a: a[-1].endswith('aaaa.cnmt.nca'))
        e = self.fail(self.scanned(tool))
        self.assertIn('not the base game', str(e))
        self.assertIn('base.xci is incomplete', str(e))
        self.assertFalse(e.about_keys)

    def test_base_read_but_update_needs_ticket(self):
        tool = OutputTool(OUT['no_ticket'], only=lambda a: not a[-1].endswith('aaaa.cnmt.nca'))
        e = self.fail(self.scanned(tool))
        self.assertIn('base game only', str(e))
        self.assertIn('ticket', str(e))
        self.assertFalse(e.about_keys)

    def test_container_without_content_list(self):
        d = self.t / 'src'
        d.mkdir()
        (d / 'odd.nsp').write_bytes(tg.pfs0({'readme.txt': b'hi'}))
        (d / 'prod.keys').write_text(keys_text(), encoding='utf-8')
        sc = gamesrc.scan(d, env={})
        gamesrc.identify_all(sc, tg.FakeTool(), self.t / 'scratch', say=lambda m: None)
        e = self.fail(sc)
        self.assertEqual(e.code, 'container')
        self.assertIn('odd.nsp', str(e))


class ExtractTests(Base):
    def plan(self, base_nsp=False):
        d = self.t / 'src'
        d.mkdir()
        tg.make_folder(d, with_base=not base_nsp)
        if base_nsp:
            (d / 'base.nsp').write_bytes(tg.pfs0({
                'aaaa.cnmt.nca': tg.cnmt_nca('0100f4300bf2c000', 0, 'Application', 'b' * 32),
                'b' * 32 + '.nca': b'BASEPROGRAM' * 10,
                '0100f4300bf2c000000000000000000b.tik': b'BASETIK'}))
        (d / 'prod.keys').write_text(keys_text(), encoding='utf-8')
        sc = gamesrc.scan(d, env={})
        gamesrc.identify_all(sc, tg.FakeTool(), self.t / 'scratch', say=lambda m: None)
        return gamesrc.pick(sc)

    def test_download_type_base_game_gets_its_own_ticket(self):
        tool = tg.FakeTool()
        gamesrc.extract(self.plan(base_nsp=True), tool, self.t / 'w' / 'game', say=lambda m: None)
        rom = next(c for c in tool.calls if '--part1' in c)
        tiks = [Path(rom[i + 1]).name for i, a in enumerate(rom) if a == '--tik']
        self.assertEqual(tiks, ['0100f4300bf2c800000000000000000b.tik', 'base_0100f4300bf2c000000000000000000b.tik'])
        exe = next(c for c in tool.calls if '--part0' in c and not c[-1].endswith('.cnmt.nca'))
        self.assertEqual(exe.count('--tik'), 1)

    def test_game_card_base_passes_one_ticket(self):
        tool = tg.FakeTool()
        gamesrc.extract(self.plan(), tool, self.t / 'w' / 'game', say=lambda m: None)
        self.assertEqual(next(c for c in tool.calls if '--part1' in c).count('--tik'), 1)

    def test_extract_failures_say_which_case(self):
        plan = self.plan()
        for case, code in (('cut_short', 'cut_short'), ('hash', 'damaged'), ('cannot_write', 'work_folder'), ('old_keys_program', 'keys_generation')):
            said = []
            tool = OutputTool(OUT[case], only=lambda a: not a[-1].endswith('.cnmt.nca'))
            with self.assertRaises(gamesrc.GameFilesError) as c:
                gamesrc.extract(plan, tool, self.t / 'w' / case, say=said.append)
            self.assertEqual(c.exception.code, code, case)
            self.assertIn('The file reader said:', said[-1], case)
            self.assertFalse((self.t / 'w' / case / '_scratch').exists(), case)
        tool = OutputTool(OUT['hash'], fail=('--part1',))
        with self.assertRaises(gamesrc.GameFilesError) as c:
            gamesrc.extract(plan, tool, self.t / 'w' / 'romfs', say=lambda m: None)
        self.assertIn('base.xci or upd262144.nsp', str(c.exception))
        self.assertIsInstance(c.exception, StepError)


class ScanFolderTests(Base):
    """The wizard's scan: the keys button comes back for a keys problem only, and the cause lands in setup-events.log."""

    def setUp(self):
        super().setUp()
        self.env = mock.patch.dict(os.environ, {'SNAP_SETUP_DIR': str(self.t / 'setup')})
        self.env.start()
        self.src = self.t / 'src'
        self.src.mkdir()
        tg.make_folder(self.src)
        self.exe = self.t / 'nstool.exe'
        self.exe.write_bytes(b'')

    def tearDown(self):
        self.env.stop()
        super().tearDown()

    def scan(self, tool, keys_body=None, keys=None):
        if keys_body is not None:
            (self.src / 'prod.keys').write_text(keys_body, encoding='utf-8')
        with mock.patch.object(gamesrc, 'NsTool', lambda exe, k: tool), mock.patch.object(setup_engine, 'space_report', lambda *a, **k: None):
            return setup_engine._scan_folder(self.src, keys, self.exe, table={})

    def log(self):
        p = self.t / 'setup' / 'setup-events.log'
        return p.read_text(encoding='utf-8') if p.exists() else ''

    def test_good_scan(self):
        out = self.scan(tg.FakeTool(), keys_text())
        self.assertTrue(out['ok'], out.get('error'))
        self.assertIn('master keys 00 to 15', self.log())

    def test_keys_file_that_is_not_keys_never_reaches_nstool(self):
        tool = tg.FakeTool()
        out = self.scan(tool, 'not a keys file\n')
        self.assertFalse(out['ok'])
        self.assertTrue(out['needs_keys'])
        self.assertEqual(out['error_code'], 'keys_not_keys')
        self.assertEqual(tool.calls, [])
        self.assertIn('no keys in it', self.log())

    def test_old_keys_bring_back_the_keys_button(self):
        out = self.scan(OutputTool(OUT['old_keys_meta']), keys_text(range(0x0a)))
        self.assertTrue(out['needs_keys'])
        self.assertEqual(out['error_code'], 'keys_generation')
        self.assertIn('master_key_0a', out['error'])
        self.assertEqual(out['dumps'][0]['error_code'], 'keys_generation')
        self.assertIn('AES-CTR Key was not determined', self.log())

    def test_damaged_dump_does_not_ask_for_keys(self):
        out = self.scan(OutputTool(OUT['hash']), keys_text())
        self.assertFalse(out['needs_keys'])
        self.assertEqual(out['error_code'], 'damaged')
        self.assertIn('failed hash verification', self.log())
        self.assertNotIn(Z16, self.log())

    def test_no_keys_file(self):
        (self.src / 'prod.keys').unlink()
        out = self.scan(tg.FakeTool())
        self.assertTrue(out['needs_keys'])
        self.assertEqual(out['error_code'], 'keys_missing')
        self.assertIn('.txt', out['error'])


if __name__ == '__main__':
    unittest.main()
