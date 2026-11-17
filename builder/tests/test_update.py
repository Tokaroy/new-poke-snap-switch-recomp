"""Updater tests. Everything is local: fake releases are folders with a release JSON, a signed zip and a signature, read through file:// links.
No network, no game files. Run: python -I -m unittest discover -s builder/tests"""
import hashlib
import json
import os
import shutil
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from snapbuild import ed25519, update  # noqa: E402
from snapbuild.update import UpdateError  # noqa: E402

SEED = bytes(range(1, 33))   # a test key, not the release key
PUB = ed25519.public_key(SEED)


class Quiet:
    def __init__(self):
        self.lines = []

    def write(self, s):
        self.lines.append(s)

    def close(self):
        pass


_ENV_KEYS = ('APPDATA', 'LOCALAPPDATA', 'SNAP_REAL_APPDATA', 'SNAP_REAL_LOCALAPPDATA')
_saved_env = {}
_env_tmp = None


def setUpModule():
    """Never let a test reach the real %APPDATA%: an update copies the player's saves first (the real data root was once 3 GB per test)."""
    global _env_tmp
    _env_tmp = tempfile.TemporaryDirectory()
    for k in _ENV_KEYS:
        _saved_env[k] = os.environ.get(k)
    os.environ['APPDATA'] = str(Path(_env_tmp.name) / 'Roaming')
    os.environ['LOCALAPPDATA'] = str(Path(_env_tmp.name) / 'Local')
    os.environ.pop('SNAP_REAL_APPDATA', None)
    os.environ.pop('SNAP_REAL_LOCALAPPDATA', None)


def tearDownModule():
    for k, v in _saved_env.items():
        if v is None:
            os.environ.pop(k, None)
        else:
            os.environ[k] = v
    _env_tmp.cleanup()


def tree_hash(root, skip=()):
    h = hashlib.sha256()
    for p in sorted(Path(root).rglob('*')):
        rel = p.relative_to(root)
        if rel.parts and rel.parts[0] in skip:
            continue
        h.update(str(rel).encode())
        if p.is_file():
            h.update(p.read_bytes())
    return h.hexdigest()


class Rig:
    """A fake installed builder (version 1.0.0), a work folder with a build and user data, and a place to put releases."""

    def __init__(self, tmp):
        self.t = Path(tmp)
        self.builder = self.t / 'cur' / 'builder'
        self.builder.mkdir(parents=True)
        (self.builder / 'VERSION').write_text('1.0.0\n')
        (self.builder / 'update_pubkey.txt').write_text(PUB.hex() + '\n')
        self.work = self.t / 'work'
        (self.work / 'out').mkdir(parents=True)
        (self.work / 'out' / 'SnapRecomp.exe').write_bytes(b'MZ-old-exe')
        (self.work / 'out' / 'config.json').write_text(json.dumps({'builder_version': '1.0.0'}))
        (self.work / 'builds' / 'previous').mkdir(parents=True)
        (self.work / 'builds' / 'previous' / 'SnapRecomp.exe').write_bytes(b'MZ-prev-exe')
        (self.work / 'builds' / 'previous' / 'config.json').write_text(json.dumps({'builder_version': '0.9.0'}))
        (self.work / 'last_build.json').write_text(json.dumps({'version': '1.0.0', 'args': {
            'main': 'm', 'sdk': 's', 'romfs': 'r', 'work': str(self.work), 'jobs': 4},
            'steps': {'compile_main': 120, 'compile_sdk': 60, 'link': 20, 'gen_main': 200, 'gen_sdk': 100}, 'smoke_seconds': 300}))
        self.state = self.t / 'state'
        # user data that an update must never touch
        self.user = self.t / 'user'
        for rel, data in (('save/0001/CURRENT', b'SNAPSAVE1\n3\n'), ('save/0001/gens/3/file0', b'save-bytes'), ('save/0001/gens/3.ok', b'ok'),
                          ('save/0001/gens/2/file0', b'older-gen'), ('settings/hle.json', b'{"a":1}'), ('input.json', b'{"b":2}'),
                          ('audio.json', b'{"c":3}'), ('texcache/x.bin', b'tex'), ('mods/pack/a.nutexb', b'mod'), ('shadercache/k.spv', b'spv')):
            p = self.user / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(data)
        self.rel = self.t / 'rel'
        self.rel.mkdir()

    def user_hash(self):
        """The player's data, not counting the update-backups folder (an update adds to it by design)."""
        return tree_hash(self.user, skip=('update-backups',))

    def make_release(self, version='1.0.1', seed=SEED, notes='Fixes things.', tag=None, tamper=False, wrong_ver_in_zip=None,
                     extra=None, drop=(), sign_version=None, short=False, prerelease=False, doc_name='release.json'):
        name = f'SnapRecomp-Builder-v{version}-win-x64.zip'
        files = {'builder/snap_builder.py': b'print("new")', 'builder/VERSION': (wrong_ver_in_zip or version).encode(),
                 'builder/update_pubkey.txt': PUB.hex().encode(), 'prebuilt/manifest.json': b'{}'}
        files.update(extra or {})
        for d in drop:
            files.pop(d, None)
        zp = self.rel / name
        with zipfile.ZipFile(zp, 'w') as z:
            for n, b in files.items():
                z.writestr(n, b)
        digest, size = update.sha256_of(zp), zp.stat().st_size
        sv = sign_version or version
        sig = ed25519.sign(seed, update.signed_message(sv, digest, size))
        sigp = self.rel / (name + '.sig')
        sigp.write_text(json.dumps({'format': 1, 'version': sv, 'sha256': digest, 'size': size, 'signature': sig.hex()}))
        if tamper:
            zp.write_bytes(zp.read_bytes() + b'x')
        listed = size + (100 if short else 0)
        doc = {'tag_name': tag or 'v' + version, 'body': notes, 'draft': False, 'prerelease': prerelease, 'html_url': 'https://example.invalid/r',
               'assets': [{'name': name, 'size': listed, 'browser_download_url': zp.as_uri()},
                          {'name': name + '.sig', 'size': sigp.stat().st_size, 'browser_download_url': sigp.as_uri()}]}
        j = self.rel / doc_name
        j.write_text(json.dumps(doc))
        return j.as_uri()

    def make_list(self, *releases):
        """One release-list file (what GitHub's /releases returns) from make_release keyword sets, newest first as GitHub orders them."""
        docs = []
        for i, kw in enumerate(releases):
            name = f'doc{i}.json'
            self.make_release(doc_name=name, **kw)
            docs.append(json.loads((self.rel / name).read_text()))
        j = self.rel / 'list.json'
        j.write_text(json.dumps(docs))
        return j.as_uri()


class Ed25519Vectors(unittest.TestCase):
    def test_rfc8032(self):
        seed = bytes.fromhex('9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60')
        self.assertEqual(ed25519.public_key(seed).hex(), 'd75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a')
        sig = ed25519.sign(seed, b'')
        self.assertEqual(sig.hex(), 'e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b')
        self.assertTrue(ed25519.verify(ed25519.public_key(seed), b'', sig))
        self.assertFalse(ed25519.verify(ed25519.public_key(seed), b'x', sig))

    def test_bad_inputs(self):
        sig = ed25519.sign(SEED, b'm')
        self.assertFalse(ed25519.verify(PUB, b'm', sig[:-1]))
        self.assertFalse(ed25519.verify(PUB, b'm', bytes(64)))
        flipped = bytes([sig[0] ^ 1]) + sig[1:]
        self.assertFalse(ed25519.verify(PUB, b'm', flipped))
        self.assertFalse(ed25519.verify(ed25519.public_key(bytes(32)), b'm', sig))


class Versions(unittest.TestCase):
    def test_order(self):
        self.assertTrue(update.newer('1.0.1', '1.0.0'))
        self.assertTrue(update.newer('v1.1', '1.0.9'))
        self.assertFalse(update.newer('1.0', '1.0.0'))
        self.assertFalse(update.newer('1.0.0', '1.0.1'))
        self.assertTrue(update.newer('1.0.10', '1.0.9'))

    def test_rejects_junk(self):
        for bad in ('', 'latest', '1.0.0-rc1', '1', '../1.0'):
            with self.assertRaises(UpdateError):
                update.parse_version(bad)


class ReleaseParsing(unittest.TestCase):
    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        self.rig = Rig(self.td.name)

    def tearDown(self):
        self.td.cleanup()

    def test_good(self):
        r = update.fetch_release(self.rig.make_release())
        self.assertEqual((r.version, r.zip_name), ('1.0.1', 'SnapRecomp-Builder-v1.0.1-win-x64.zip'))

    def test_missing_signature_asset(self):
        url = self.rig.make_release()
        p = Path(self.rig.rel / 'release.json')
        d = json.loads(p.read_text())
        d['assets'] = d['assets'][:1]
        p.write_text(json.dumps(d))
        with self.assertRaises(UpdateError):
            update.fetch_release(url)

    def test_tag_mismatch_and_prerelease(self):
        with self.assertRaises(UpdateError):
            update.fetch_release(self.rig.make_release(tag='v1.0.2'))
        url = self.rig.make_release()
        p = Path(self.rig.rel / 'release.json')
        d = json.loads(p.read_text())
        d['prerelease'] = True
        p.write_text(json.dumps(d))
        with self.assertRaises(UpdateError):
            update.fetch_release(url)

    def test_only_github_https_without_explicit_url(self):
        for bad in ('http://api.github.com/x', 'https://evil.example/x', 'file:///c:/x', 'ftp://github.com/x'):
            with self.assertRaises(UpdateError):
                update.open_url(bad)
        update._check_url('https://api.github.com/repos/a/b', False)
        update._check_url('https://objects.githubusercontent.com/x', False)

    def test_release_url_is_the_renamed_repository(self):
        self.assertEqual(update.RELEASES_URL, 'https://api.github.com/repos/finalpantasy/new-pokemon-snap-recomp/releases/latest')

    def test_redirects_stay_inside_the_allowlist(self):
        import urllib.request
        h = update._HttpsOnly()
        req = urllib.request.Request('https://api.github.com/repos/finalpantasy/old-name/releases/latest')
        # a repository rename: 301 to the numeric repository URL, and a download redirect to the content host
        for ok in ('https://api.github.com/repositories/1454602133/releases/latest',
                   'https://release-assets.githubusercontent.com/a/b?x=1', 'https://github.com/finalpantasy/x/releases/download/v1/x.zip'):
            self.assertIsNotNone(h.redirect_request(req, None, 301, 'Moved', {}, ok))
        for bad in ('http://api.github.com/x', 'https://evil.example/x', 'https://api.github.com.evil.example/x', 'file:///c:/x',
                    'https://githubusercontent.com.evil.example/x'):
            with self.assertRaises(UpdateError):
                h.redirect_request(req, None, 301, 'Moved', {}, bad)

    def test_301_is_followed_end_to_end(self):
        import http.server
        import threading
        import urllib.request
        from unittest import mock

        class H(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                if self.path == '/old':
                    self.send_response(301)
                    self.send_header('Location', 'https://api.github.com/repositories/1/releases/latest')
                    self.end_headers()
                else:
                    self.send_response(404)
                    self.end_headers()

            def log_message(self, *a):
                pass
        srv = http.server.HTTPServer(('127.0.0.1', 0), H)
        t = threading.Thread(target=srv.serve_forever, daemon=True)
        t.start()
        try:
            seen = []
            real_open = urllib.request.OpenerDirector.open

            def spy(self, fullurl, *a, **k):
                url = fullurl.full_url if hasattr(fullurl, 'full_url') else fullurl
                seen.append(url)
                if 'api.github.com' in url:   # no network in a test: answer the redirect target locally
                    raise urllib.error.HTTPError(url, 200, 'stop', {}, None)
                return real_open(self, fullurl, *a, **k)
            req = urllib.request.Request(f'http://127.0.0.1:{srv.server_port}/old')
            opener = urllib.request.build_opener(update._HttpsOnly)
            # the loopback start URL is plain http, which open_url would refuse; here the handler alone decides the redirect target
            with mock.patch.object(urllib.request.OpenerDirector, 'open', spy):
                try:
                    opener.open(req, timeout=5)
                except urllib.error.HTTPError:
                    pass
            self.assertTrue(any('api.github.com/repositories/1/releases/latest' in u for u in seen), seen)
        finally:
            srv.shutdown()
            srv.server_close()


class Verify(unittest.TestCase):
    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        self.rig = Rig(self.td.name)

    def tearDown(self):
        self.td.cleanup()

    def fetch(self, **kw):
        rel = update.fetch_release(self.rig.make_release(**kw))
        return rel, update.download(rel, self.rig.state / 'downloads', True)

    def test_good_signature(self):
        rel, (z, sig) = self.fetch()
        self.assertEqual(update.verify_download(z, sig, rel, PUB, '1.0.0'), '1.0.1')
        self.assertTrue(z.exists())

    def test_wrong_key_is_deleted(self):
        rel, (z, sig) = self.fetch(seed=bytes(range(50, 82)))
        with self.assertRaises(UpdateError):
            update.verify_download(z, sig, rel, PUB, '1.0.0')
        self.assertFalse(z.exists())

    def test_downgrade_and_same_version_refused(self):
        rel, (z, sig) = self.fetch(version='0.9.0')
        with self.assertRaises(UpdateError):
            update.verify_download(z, sig, rel, PUB, '1.0.0')
        self.assertFalse(z.exists())
        rel, (z, sig) = self.fetch(version='1.0.0')
        with self.assertRaises(UpdateError):
            update.verify_download(z, sig, rel, PUB, '1.0.0')

    def test_signed_version_must_match_tag(self):
        rel, (z, sig) = self.fetch(version='1.0.1', sign_version='1.0.9')
        with self.assertRaises(UpdateError):
            update.verify_download(z, sig, rel, PUB, '1.0.0')

    def test_relabelled_signature_fails(self):
        rel, (z, sig) = self.fetch(version='1.0.1')
        d = json.loads(sig)
        d['version'] = '1.0.5'
        rel.version = '1.0.5'
        with self.assertRaises(UpdateError):
            update.verify_download(z, json.dumps(d), rel, PUB, '1.0.0')

    def test_interrupted_download_leaves_nothing(self):
        rel = update.fetch_release(self.rig.make_release(short=True))   # promises more bytes than the file has
        with self.assertRaises(UpdateError):
            update.download(rel, self.rig.state / 'downloads', True)
        left = list((self.rig.state / 'downloads').iterdir()) if (self.rig.state / 'downloads').exists() else []
        self.assertEqual(left, [])

    def test_malformed_signature_file(self):
        for bad in ('not json', '{}', json.dumps({'format': 1, 'version': '1.0.1', 'sha256': 'zz', 'size': 1, 'signature': 'aa'})):
            with self.assertRaises(UpdateError):
                update.parse_sig(bad)


class Unpack(unittest.TestCase):
    GOOD = [('builder/snap_builder.py', b'x'), ('builder/VERSION', b'1.0.1'), ('builder/update_pubkey.txt', b'k'), ('prebuilt/manifest.json', b'{}')]

    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        self.t = Path(self.td.name)

    def tearDown(self):
        self.td.cleanup()

    def zip(self, entries, name='u.zip'):
        p = self.t / name
        with zipfile.ZipFile(p, 'w') as z:
            for n, b in entries:
                z.writestr(n, b)
        return p

    def test_good_and_wrapper_folder_stripped(self):
        d = update.unpack(self.zip([('pkg/' + n, b) for n, b in self.GOOD]), self.t / 'v', '1.0.1')
        self.assertTrue((d / 'builder' / 'snap_builder.py').exists())

    def test_path_tricks_refused(self):
        for i, bad in enumerate(('../evil.txt', 'builder/../../evil.txt', '/abs.txt', 'C:/abs.txt', 'builder/a:b')):
            dest = self.t / f'v{i}'
            with self.assertRaises(UpdateError):
                update.unpack(self.zip(self.GOOD + [(bad, b'x')]), dest, '1.0.1')
            self.assertFalse(dest.exists())
        self.assertFalse((self.t / 'evil.txt').exists())

    def test_symlink_refused(self):
        zi = zipfile.ZipInfo('builder/link')
        zi.external_attr = 0o120777 << 16
        with self.assertRaises(UpdateError):
            update.unpack(self.zip(self.GOOD + [(zi, b'target')]), self.t / 'v3', '1.0.1')

    def test_missing_required_file_and_version_mismatch(self):
        with self.assertRaises(UpdateError):
            update.unpack(self.zip(self.GOOD[:-1]), self.t / 'v4', '1.0.1')
        with self.assertRaises(UpdateError):
            update.unpack(self.zip(self.GOOD), self.t / 'v5', '1.0.2')
        self.assertFalse((self.t / 'v5').exists())


class Apply(unittest.TestCase):
    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        self.rig = Rig(self.td.name)
        self.log = Quiet()

    def tearDown(self):
        self.td.cleanup()

    def apply(self, url, run, **kw):
        return update.apply(self.rig.builder, self.rig.work, self.rig.state, url=url, log=self.log, run=run, **{"data_root": self.rig.user, **kw})

    def test_success_keeps_user_data(self):
        url = self.rig.make_release()
        before_user = self.rig.user_hash()
        calls = []
        r = self.apply(url, lambda cmd, log: calls.append(cmd) or 0)
        self.assertTrue(r['updated'])
        self.assertEqual(r['to'], '1.0.1')
        self.assertEqual(len(calls), 1)
        self.assertIn('build', calls[0])
        self.assertIn(str(self.rig.work), calls[0])
        self.assertNotIn('--prebuilt', calls[0])
        st = update.load_state(self.rig.state)
        self.assertEqual(st['current'], '1.0.1')
        self.assertFalse((self.rig.state / 'downloads').exists())
        self.assertEqual(self.rig.user_hash(), before_user)

    def test_rebuild_failure_leaves_everything_as_it_was(self):
        url = self.rig.make_release()
        before_out, before_user = tree_hash(self.rig.work / 'out'), self.rig.user_hash()
        before_prev = tree_hash(self.rig.work / 'builds' / 'previous')
        with self.assertRaises(UpdateError):
            self.apply(url, lambda cmd, log: 1)
        self.assertEqual(tree_hash(self.rig.work / 'out'), before_out)
        self.assertEqual(tree_hash(self.rig.work / 'builds' / 'previous'), before_prev)
        self.assertEqual(self.rig.user_hash(), before_user)
        self.assertIsNone(update.load_state(self.rig.state).get('current'))
        self.assertFalse((self.rig.state / 'versions' / '1.0.1').exists())
        self.assertFalse((self.rig.state / 'downloads').exists())

    def test_bad_signature_changes_nothing(self):
        url = self.rig.make_release(seed=bytes(range(60, 92)))
        before = tree_hash(self.rig.work)
        ran = []
        with self.assertRaises(UpdateError):
            self.apply(url, lambda c, l: ran.append(c) or 0)
        self.assertEqual(ran, [])
        self.assertEqual(tree_hash(self.rig.work), before)
        dl = self.rig.state / 'downloads'
        self.assertEqual(list(dl.glob('*.zip')) if dl.exists() else [], [])
        self.assertFalse((self.rig.state / 'versions').exists())

    def test_up_to_date_does_nothing(self):
        url = self.rig.make_release(version='1.0.0')
        r = self.apply(url, lambda c, l: self.fail('must not rebuild'))
        self.assertFalse(r['updated'])

    def test_declined(self):
        url = self.rig.make_release()
        r = self.apply(url, lambda c, l: self.fail('no'), confirm=lambda info: False)
        self.assertTrue(r['declined'])
        self.assertFalse((self.rig.state / 'downloads').exists())

    def test_no_build_record_stops_before_downloading(self):
        (self.rig.work / 'last_build.json').unlink()
        url = self.rig.make_release()
        with self.assertRaises(UpdateError):
            self.apply(url, lambda c, l: 0)
        self.assertFalse((self.rig.state / 'downloads').exists())

    def test_lock_blocks_a_second_update_and_dead_lock_is_taken_over(self):
        url = self.rig.make_release()
        with update.Lock(self.rig.state):
            with self.assertRaises(UpdateError):
                self.apply(url, lambda c, l: 0)
        self.rig.state.mkdir(exist_ok=True)
        (self.rig.state / 'update.lock').write_text('999999')
        self.assertTrue(self.apply(url, lambda c, l: 0)['updated'])


class Rollback(unittest.TestCase):
    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        self.rig = Rig(self.td.name)

    def tearDown(self):
        self.td.cleanup()

    def test_swap_and_swap_back(self):
        w, log = self.rig.work, Quiet()
        before_user = self.rig.user_hash()
        update.rollback(w, self.rig.state, log, data_root=self.rig.user)
        self.assertEqual((w / 'out' / 'SnapRecomp.exe').read_bytes(), b'MZ-prev-exe')
        self.assertEqual((w / 'builds' / 'previous' / 'SnapRecomp.exe').read_bytes(), b'MZ-old-exe')
        update.rollback(w, self.rig.state, log, data_root=self.rig.user)
        self.assertEqual((w / 'out' / 'SnapRecomp.exe').read_bytes(), b'MZ-old-exe')
        self.assertEqual(self.rig.user_hash(), before_user)

    def test_no_previous_build(self):
        shutil.rmtree(self.rig.work / 'builds' / 'previous')
        with self.assertRaises(UpdateError):
            update.rollback(self.rig.work, self.rig.state, Quiet(), data_root=self.rig.user)


class StartupCheck(unittest.TestCase):
    """The launcher's start-up check: on by default, at most one request per 24 hours, off with update_check=false, silent when it fails."""

    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        self.rig = Rig(self.td.name)
        self.cfg = self.rig.t / 'config.json'
        self.cfg.write_text('{}')
        self.url = self.rig.make_release()
        self.gone = (self.rig.t / 'nowhere' / 'release.json').as_uri()
        self.t0 = 1_800_000_000.0

    def tearDown(self):
        self.td.cleanup()

    def run_check(self, now, url=None):
        return update.startup_check(self.rig.builder, self.rig.state, self.cfg, work=self.rig.work, url=url or self.url, now=now)

    def test_default_is_on_and_reports_an_update(self):
        r = self.run_check(self.t0)
        self.assertTrue(r['checked'])
        self.assertTrue(r['update_available'])
        self.assertEqual(r['latest'], '1.0.1')

    def test_missing_or_broken_config_means_on(self):
        self.cfg.unlink()
        self.assertTrue(update.startup_check_enabled(self.cfg))
        self.cfg.write_text('not json')
        self.assertTrue(update.startup_check_enabled(self.cfg))

    def test_throttle_24_hours(self):
        self.assertTrue(self.run_check(self.t0)['checked'])
        r = self.run_check(self.t0 + 23 * 3600, url=self.gone)   # a request to a missing release would report an error
        self.assertFalse(r['checked'])
        self.assertNotIn('error', r)
        r = self.run_check(self.t0 + 25 * 3600, url=self.gone)   # past 24 h it asks again
        self.assertIn('error', r)
        self.assertTrue(self.run_check(self.t0 + 25 * 3600)['checked'])

    def test_off_switch_sends_nothing(self):
        self.cfg.write_text(json.dumps({'update_check': False}))
        self.assertFalse(update.startup_check_enabled(self.cfg))
        self.assertIsNone(self.run_check(self.t0, url=self.gone))
        self.assertFalse((self.rig.state / 'state.json').exists())
        self.cfg.write_text(json.dumps({'update_check': True}))
        self.assertTrue(self.run_check(self.t0)['checked'])

    def test_failure_is_silent_and_retried_next_start(self):
        r = self.run_check(self.t0, url=self.gone)
        self.assertFalse(r['checked'])
        self.assertIsNone(update.load_state(self.rig.state).get('last_check'))   # a failed try is not counted
        self.assertTrue(self.run_check(self.t0 + 60)['checked'])

    def test_request_is_plain(self):
        self.assertEqual(update.USER_AGENT, 'SnapRecompUpdater')
        src = Path(update.__file__).read_text(encoding='utf-8')
        self.assertNotIn('Authorization', src)
        self.assertNotIn('Cookie', src)


class RebuildCommand(unittest.TestCase):
    def test_replays_recorded_args_without_prebuilt(self):
        last = {'args': {'main': 'M', 'sdk': 'S', 'romfs': 'R', 'work': 'W', 'maps': 'MP', 'jobs': 6, 'cache': False, 'skip_smoke': True,
                         'smoke_timeout': 900}}
        cmd, work = update.build_command(Path('X') / 'builder', last)
        self.assertEqual(work, 'W')
        for want in ('--main', 'M', '--maps', 'MP', '--jobs', '6'):
            self.assertIn(want, cmd)
        self.assertNotIn('--prebuilt', cmd)
        self.assertNotIn('--force', cmd)


class BackupBeforeUpdate(unittest.TestCase):
    """Saves and settings are copied to update-backups before anything is replaced, and a copy that fails stops the update."""

    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        self.rig = Rig(self.td.name)
        self.log = Quiet()

    def tearDown(self):
        self.td.cleanup()

    def apply(self, url, run, **kw):
        return update.apply(self.rig.builder, self.rig.work, self.rig.state, url=url, log=self.log, run=run, data_root=self.rig.user, **kw)

    def backups(self):
        base = self.rig.user / 'update-backups'
        return sorted(p.name for p in base.iterdir()) if base.exists() else []

    def test_backup_exists_before_the_rebuild_runs(self):
        url = self.rig.make_release()
        seen = {}

        def run(cmd, log):
            seen['backups'] = self.backups()                # the rebuild must find the copy already made
            return 0
        r = self.apply(url, run)
        self.assertEqual(len(seen['backups']), 1)
        self.assertEqual(r['backup'], seen['backups'][0])
        self.assertTrue(r['backup'].endswith('-1.0.0-to-1.0.1'))
        b = self.rig.user / 'update-backups' / r['backup']
        self.assertEqual((b / 'save' / '0001' / 'gens' / '3' / 'file0').read_bytes(), b'save-bytes')
        self.assertEqual((b / 'input.json').read_bytes(), b'{"b":2}')
        self.assertFalse((b / 'texcache').exists())          # caches are not copied
        self.assertFalse((b / 'save' / '0001' / 'gens' / '2').exists())   # only the current generation

    def test_backup_failure_stops_before_anything_changes(self):
        url = self.rig.make_release()
        before_work = tree_hash(self.rig.work)
        orig = update.userdata.backup

        def boom(*a, **k):
            raise update.userdata.BackupError('no space')
        update.userdata.backup = boom
        try:
            with self.assertRaises(UpdateError) as cm:
                self.apply(url, lambda c, l: self.fail('must not rebuild'))
        finally:
            update.userdata.backup = orig
        self.assertIn('no space', str(cm.exception))
        self.assertEqual(tree_hash(self.rig.work), before_work)
        self.assertFalse((self.rig.state / 'versions').exists())
        dl = self.rig.state / 'downloads'
        self.assertEqual(list(dl.glob('*.zip')) if dl.exists() else [], [])
        self.assertIsNone(update.load_state(self.rig.state).get('current'))

    def test_low_disk_stops_the_update(self):
        url = self.rig.make_release()
        orig = update.userdata.shutil.disk_usage
        update.userdata.shutil.disk_usage = lambda p: shutil._ntuple_diskusage(1 << 40, 1 << 40, 1 << 20)   # 1 MB free
        try:
            with self.assertRaises(UpdateError) as cm:
                self.apply(url, lambda c, l: self.fail('must not rebuild'))
        finally:
            update.userdata.shutil.disk_usage = orig
        self.assertIn('free disk space', str(cm.exception))
        self.assertEqual(self.backups(), [])

    def test_only_three_backups_are_kept(self):
        for i in range(5):
            self.rig.make_release(version=f'1.0.{i + 1}')
            self.rig.builder.joinpath('VERSION').write_text(f'1.0.{i}\n')   # as if each update had installed the one before
            url = (self.rig.rel / 'release.json').as_uri()
            r = self.apply(url, lambda c, l: 0)
            self.assertTrue(r['updated'])
        names = self.backups()
        self.assertEqual(len(names), 3)
        self.assertTrue(names[-1].endswith('-1.0.4-to-1.0.5'))

    def test_no_saves_no_backup_and_the_update_goes_on(self):
        shutil.rmtree(self.rig.user)
        url = self.rig.make_release()
        r = self.apply(url, lambda c, l: 0)
        self.assertTrue(r['updated'])
        self.assertIsNone(r['backup'])

    def test_rollback_makes_a_backup_too(self):
        update.rollback(self.rig.work, self.rig.state, Quiet(), data_root=self.rig.user)
        self.assertEqual(len(self.backups()), 1)
        self.assertTrue(self.backups()[0].endswith('-1.0.0-to-0.9.0'))

    def test_tests_never_touch_the_real_appdata(self):
        self.assertTrue(os.environ['APPDATA'].startswith(_env_tmp.name))
        self.assertTrue(os.environ['LOCALAPPDATA'].startswith(_env_tmp.name))
        self.assertNotIn('SNAP_REAL_APPDATA', os.environ)


class Channels(unittest.TestCase):
    """Stable ignores prereleases; Testing takes the newest release of either kind; the answer is remembered per channel."""

    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        self.rig = Rig(self.td.name)
        self.url = self.rig.make_list({'version': '1.2.0', 'prerelease': True},
                                      {'version': '1.1.0'},
                                      {'version': '1.0.5'})

    def tearDown(self):
        self.td.cleanup()

    def test_stable_never_offers_a_prerelease(self):
        r = update.fetch_release(self.url, channel='stable')
        self.assertEqual(r.version, '1.1.0')

    def test_testing_takes_the_newest_prerelease(self):
        r = update.fetch_release(self.url, channel='testing')
        self.assertEqual(r.version, '1.2.0')

    def test_testing_still_takes_a_stable_release_when_it_is_newer(self):
        url = self.rig.make_list({'version': '1.3.0'}, {'version': '1.2.0', 'prerelease': True})
        self.assertEqual(update.fetch_release(url, channel='testing').version, '1.3.0')

    def test_drafts_are_never_taken(self):
        doc = json.loads((self.rig.rel / 'doc0.json').read_text())
        doc['draft'] = True
        (self.rig.rel / 'doc0.json').write_text(json.dumps(doc))
        with self.assertRaises(UpdateError):
            update.parse_release(doc, True)

    def test_single_prerelease_document(self):
        url = self.rig.make_release(version='1.4.0', prerelease=True)
        with self.assertRaises(UpdateError):
            update.fetch_release(url, channel='stable')
        self.assertEqual(update.fetch_release(url, channel='testing').version, '1.4.0')

    def test_unknown_channel(self):
        with self.assertRaises(UpdateError):
            update.check_channel('nightly')
        self.assertEqual(update.check_channel(None), 'stable')
        self.assertEqual(update.check_channel(' Testing '), 'testing')

    def test_check_reports_and_remembers_the_channel(self):
        r = update.check(self.rig.builder, self.rig.state, url=self.url, channel='testing')
        self.assertEqual((r['latest'], r['channel']), ('1.2.0', 'testing'))
        self.assertEqual(update.load_state(self.rig.state)['channel'], 'testing')

    def test_switching_channel_ignores_the_throttled_answer(self):
        t0 = 1_800_000_000.0
        update.check(self.rig.builder, self.rig.state, url=self.url, now=t0, min_age_hours=24, channel='testing')
        r = update.check(self.rig.builder, self.rig.state, url=self.url, now=t0 + 60, min_age_hours=24, channel='stable')
        self.assertTrue(r['checked'])                       # a Testing answer must not stand in for a Stable one
        self.assertEqual(r['latest'], '1.1.0')

    def test_config_channel(self):
        cfg = self.rig.t / 'config.json'
        self.assertEqual(update.config_channel(cfg), 'stable')               # no file
        cfg.write_text('not json')
        self.assertEqual(update.config_channel(cfg), 'stable')
        cfg.write_text('{}')
        self.assertEqual(update.config_channel(cfg), 'stable')               # default
        cfg.write_text(json.dumps({'update_channel': 'Testing'}))
        self.assertEqual(update.config_channel(cfg), 'testing')
        cfg.write_text(json.dumps({'update_channel': 'whatever'}))
        self.assertEqual(update.config_channel(cfg), 'stable')

    def test_apply_on_testing_installs_the_prerelease(self):
        r = update.apply(self.rig.builder, self.rig.work, self.rig.state, url=self.url, log=Quiet(), run=lambda c, l: 0,
                         channel='testing', data_root=self.rig.user)
        self.assertEqual((r['to'], r['channel']), ('1.2.0', 'testing'))

    def test_apply_on_stable_skips_the_prerelease(self):
        r = update.apply(self.rig.builder, self.rig.work, self.rig.state, url=self.url, log=Quiet(), run=lambda c, l: 0,
                         data_root=self.rig.user)
        self.assertEqual(r['to'], '1.1.0')


class PortableUpdate(unittest.TestCase):
    """portable.txt and portable_data live in out\\; an update and a rollback carry them to the active build and keep the state outside."""

    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        self.rig = Rig(self.td.name)
        out = self.rig.work / 'out'
        (out / 'portable.txt').write_text('portable')
        self.root = out / 'portable_data' / 'Roaming' / 'SnapRecomp'
        (self.root / 'save' / '0001' / 'gens' / '1').mkdir(parents=True)
        (self.root / 'save' / '0001' / 'CURRENT').write_text('SNAPSAVE1\n1\n')
        (self.root / 'save' / '0001' / 'gens' / '1' / 'file0').write_bytes(b'portable-save')
        (self.root / 'audio.json').write_text('{"v":1}')

    def tearDown(self):
        self.td.cleanup()

    def test_data_root_and_state_dir_follow_the_flag(self):
        from snapbuild import userdata
        env = {'APPDATA': 'C:/A', 'LOCALAPPDATA': 'C:/L'}
        self.assertEqual(userdata.data_root(self.rig.work, env), self.root)
        self.assertEqual(userdata.update_state_dir(self.rig.work, env), self.rig.work / 'updates')
        (self.rig.work / 'out' / 'portable.txt').unlink()
        self.assertEqual(userdata.data_root(self.rig.work, env), Path('C:/A') / 'SnapRecomp')
        self.assertEqual(userdata.update_state_dir(self.rig.work, env), Path('C:/L') / 'SnapRecomp' / 'updates')

    def test_real_appdata_wins_over_a_redirected_one_when_not_portable(self):
        from snapbuild import userdata
        (self.rig.work / 'out' / 'portable.txt').unlink()
        env = {'APPDATA': 'C:/redirected', 'SNAP_REAL_APPDATA': 'C:/real'}
        self.assertEqual(userdata.data_root(self.rig.work, env), Path('C:/real') / 'SnapRecomp')

    def test_rollback_moves_the_portable_data_to_the_active_build(self):
        update.rollback(self.rig.work, self.rig.state, Quiet())          # data_root found from work: portable
        w = self.rig.work
        self.assertEqual((w / 'out' / 'SnapRecomp.exe').read_bytes(), b'MZ-prev-exe')
        self.assertTrue((w / 'out' / 'portable.txt').is_file())
        self.assertEqual((w / 'out' / 'portable_data' / 'Roaming' / 'SnapRecomp' / 'save' / '0001' / 'gens' / '1' / 'file0').read_bytes(), b'portable-save')
        self.assertFalse((w / 'builds' / 'previous' / 'portable_data').exists())
        self.assertFalse((w / 'builds' / 'previous' / 'portable.txt').exists())
        # the copy taken first is inside the portable data and travelled with it
        self.assertEqual(len(list((w / 'out' / 'portable_data' / 'Roaming' / 'SnapRecomp' / 'update-backups').iterdir())), 1)
        update.rollback(self.rig.work, self.rig.state, Quiet())          # and forward again
        self.assertEqual((w / 'out' / 'SnapRecomp.exe').read_bytes(), b'MZ-old-exe')
        self.assertTrue((w / 'out' / 'portable.txt').is_file())
        self.assertEqual(len(list((w / 'out' / 'portable_data' / 'Roaming' / 'SnapRecomp' / 'update-backups').iterdir())), 2)

    def test_failed_swap_puts_the_portable_data_back(self):
        from snapbuild import pipeline
        w = self.rig.work
        calls = {'n': 0}
        real = pipeline._replace_retry

        def flaky(a, b, *x, **k):
            calls['n'] += 1
            if calls['n'] == 2:                       # prev -> out fails, after out was moved aside
                raise OSError('in use')
            return real(a, b, *x, **k)
        pipeline._replace_retry = flaky
        try:
            with self.assertRaises(UpdateError):
                update.rollback(w, self.rig.state, Quiet())
        finally:
            pipeline._replace_retry = real
        self.assertEqual((w / 'out' / 'SnapRecomp.exe').read_bytes(), b'MZ-old-exe')
        self.assertTrue((w / 'out' / 'portable.txt').is_file())
        self.assertTrue((self.root / 'save' / '0001' / 'gens' / '1' / 'file0').is_file())
        self.assertFalse((w / 'builds' / 'previous' / 'portable_data').exists())

    def test_the_backup_of_a_portable_install_lands_in_its_own_data(self):
        r = update.apply(self.rig.builder, self.rig.work, self.rig.state, url=self.rig.make_release(), log=Quiet(), run=lambda c, l: 0)
        b = self.root / 'update-backups' / r['backup']
        self.assertEqual((b / 'save' / '0001' / 'gens' / '1' / 'file0').read_bytes(), b'portable-save')
        self.assertEqual((b / 'audio.json').read_bytes(), b'{"v":1}')


if __name__ == '__main__':
    unittest.main()
