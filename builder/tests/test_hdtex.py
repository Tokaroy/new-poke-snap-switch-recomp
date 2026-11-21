"""HD AI textures (snap_builder.py hdtex): containers, classifier, levels, planning, mods.json, state and a small
end-to-end run with a stand-in engine. All data here is synthetic. The texconv tests run when SNAP_TEXCONV names a
texconv.exe (or one is found in build/w/hdtex/dl); otherwise they are skipped."""
import json
import os
import struct
import sys
import tempfile
import types
import unittest
import zlib
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from snapbuild import hdtex as H, hdtex_classify as hc, hdtex_image as im, hdtex_nutexb as nb  # noqa: E402

REPO = Path(__file__).resolve().parents[2]


def find_texconv():
    for p in (os.environ.get('SNAP_TEXCONV', ''), REPO / 'build/w/hdtex/dl/texconv.exe'):
        if p and Path(p).is_file():
            return Path(p)
    return None


TEXCONV = find_texconv()


def footer(name='T_test', fmt=0x400, w=4, h=4, mips=1):
    f = bytearray(nb.FOOTER)
    f[0x40:0x44] = b' XNT'
    f[0x44:0x44 + len(name)] = name.encode()
    struct.pack_into('<11I', f, nb.FOOTER - 44, w, h, 1, fmt, 2, mips, 0x1000, 1, 0, nb.MAGIC, 2)
    return bytes(f)


TEMPLATE = nb.Tex(b'', footer(), 4, 4, 1, 0x400, 2, 1, 0x1000, 1, 0)


def level_rows(fmt, w, h, mips, seed=0):
    rng = np.random.default_rng(seed)
    bpb, bw, bh = nb.FORMATS[fmt][:3]
    lv, _ = nb.layout(w, h, bpb, bw, bh, mips)
    return [rng.integers(0, 256, (hb, wb * bpb), dtype=np.uint8) for _, wb, hb, _, _ in lv]


def make_nutexb(fmt, w, h, mips, seed=0):
    rows = level_rows(fmt, w, h, mips, seed)
    return nb.build(TEMPLATE, fmt, w, h, rows), rows


class NutexbTests(unittest.TestCase):
    def test_build_parse_roundtrip(self):
        for fmt, w, h, mips in ((0x480, 256, 128, 9), (0x405, 64, 64, 7), (0x4e5, 1024, 512, 1), (0x280, 96, 40, 3)):
            blob, rows = make_nutexb(fmt, w, h, mips)
            t = nb.parse(blob)
            self.assertEqual((t.w, t.h, t.d, t.fmt, t.mips, t.dim, t.layers), (w, h, 1, fmt, mips, 2, 1))
            self.assertEqual(t.data_size, len(blob) - nb.FOOTER)
            self.assertTrue(t.plain_2d)
            self.assertEqual(sum(t.mip_sizes), t.data_size)
            for m in range(mips):
                np.testing.assert_array_equal(np.frombuffer(nb.level_blocks(t, m), np.uint8).reshape(rows[m].shape), rows[m])

    def test_footer_offsets_match_the_game(self):
        # Width at +0x84, mips at +0x98, data size at +0xa4, " XET" at +0xa8 (as in the game's own files).
        blob, _ = make_nutexb(0x485, 1024, 1024, 11)
        f = blob[-nb.FOOTER:]
        self.assertEqual(struct.unpack_from('<4I', f, 0x84), (1024, 1024, 1, 0x485))
        self.assertEqual(struct.unpack_from('<I', f, 0x98)[0], 11)
        self.assertEqual(struct.unpack_from('<I', f, 0xa4)[0], len(blob) - nb.FOOTER)
        self.assertEqual(f[0xa8:0xac], b' XET')
        self.assertEqual(nb.name_of(nb.parse(blob)), 'T_test')

    def test_footer_only_matches_parse(self):
        blob, _ = make_nutexb(0x4a5, 128, 64, 8)
        a, b = nb.parse(blob), H._footer_only(blob[-nb.FOOTER:], len(blob))
        self.assertEqual((a.w, a.h, a.fmt, a.mips, a.tile), (b.w, b.h, b.fmt, b.mips, b.tile))

    def test_bad_input(self):
        with self.assertRaises(nb.NutexbError):
            nb.parse(b'x' * 10)
        with self.assertRaises(nb.NutexbError):
            nb.parse(b'\0' * 300)

    def test_dds_roundtrip(self):
        d = Path(tempfile.mkdtemp()) / 'a.dds'
        for dxgi, w, h in ((28, 32, 16), (71, 64, 64), (10, 8, 8)):
            levels = []
            for m in range(3):
                pitch, n = nb._pitch(dxgi, max(w >> m, 1), max(h >> m, 1))
                levels.append(bytes(np.arange(pitch * n, dtype=np.uint32).astype(np.uint8)))
            nb.write_dds(d, dxgi, w, h, levels)
            g, gw, gh, lv = nb.read_dds(d)
            self.assertEqual((g, gw, gh), (dxgi, w, h))
            self.assertEqual(lv, levels)


def tex(fmt=0x485, w=512, h=512, mips=10):
    return nb.Tex(b'', b'', w, h, 1, fmt, 2, mips, 0x1000, 1, 0)


class ClassifyTests(unittest.TestCase):
    def test_categories(self):
        cases = {
            ('nx/pack/characters/master/ch_626.drp/CH_626_00_0_A00_Body_dif.nutexb', 0x485): 'creature',
            ('nx/pack/characters/master/ch_626.drp/CH_626_00_0_A00_Body_nor.nutexb', 0x280): 'normal',
            ('nx/pack/characters/master/ch_626.drp/CH_626_00_0_A00_Body_par.nutexb', 0x480): 'mask',
            ('nx/pack/characters/master/ch_626.drp/CH_626_00_0_A00_Body_emi.nutexb', 0x485): 'emissive',
            ('nx/pack/levelcomposite/master/lc_001.drp/T_Ground_Grass01_col.nutexb', 0x485): 'scenery',
            ('nx/pack/levelcomposite/master/lc_001.drp/T_Lightmap_000.nutexb', 0x4d7): 'lightmap',
            ('common/ui/menu.drp/icon_camera.nutexb', 0x4a5): 'ui',
            ('common/ui/menu.drp/p_silhouette_A.nutexb', 0x4a5): 'ui',
            ('nx/pack/resident/master/fx.drp/FX_Spark01.nutexb', 0x4a5): 'effect',
            ('nx/pack/resident/master/a.drp/T_Rock_mask.nutexb', 0x180): 'mask',
            ('nx/pack/resident/master/a.drp/T_Sky_cubemap.nutexb', 0x485): 'skip',
            ('nx/pack/levelcomposite/master/lc_999_z.drp/LV_A^SM_Ground1^42^groundShape^0^Shadow.nutexb', 0x4a5): 'shadow',
            ('nx/pack/levelcomposite/master/lc_101.drp/LV_ENV_101_1_A^SM_ENV_Rock^Rock01^3^0^Lightmap.nutexb', 0x4d7): 'lightmap',
            ('nx/pack/levelcomposite/master/lc_101.drp/LV_ENV_101_1_A^SM_ENV_Rock^Rock01^3^2^Lightmap.nutexb', 0x4d7): 'lightdir',
            ('nx/pack/levelcomposite/master/lc_101.drp/LV_ENV_101_1_A^SM_ENV_Rock^Rock01^3^1^LIGHTMAP.nutexb', 0x4d7): 'lightdir',
        }
        for (rel, fmt), want in cases.items():
            self.assertEqual(hc.classify(rel, tex(fmt)), want, rel)

    def test_skips_small_cube_and_float(self):
        rel = 'nx/pack/levelcomposite/master/lc_001.drp/T_A_col.nutexb'
        self.assertEqual(hc.classify(rel, tex(w=8, h=8)), 'skip')
        self.assertEqual(hc.classify(rel, tex(w=256, h=1)), 'skip')
        self.assertEqual(hc.classify(rel, tex(fmt=0x435)), 'skip')
        cube = tex()
        cube.dim = 4
        self.assertEqual(hc.classify(rel, cube), 'skip')

    def test_species(self):
        self.assertEqual(hc.species('nx/pack/characters/master/ch_998.drp/CH_998_00_0_A00_Body_dif.nutexb'), 'CH_998')
        self.assertEqual(hc.species('common/ui/x.nutexb'), '')
        self.assertEqual(H.species_id('25'), 'CH_025')
        self.assertEqual(H.species_id('ch_626'), 'CH_626')
        with self.assertRaises(ValueError):
            H.species_id('pikachu')


class LevelTests(unittest.TestCase):
    def test_levels(self):
        s, hd, u = (H.level_settings(x) for x in ('sharp', 'hd', 'ultra'))
        self.assertEqual((s['scenery']['scale'], s['creature']['scale'], s['ui']['scale']), (2, 0, 2))
        self.assertEqual((hd['scenery']['scale'], hd['creature']['scale'], hd['ui']['scale']), (4, 2, 2))
        self.assertEqual((u['scenery']['scale'], u['creature']['scale'], u['ui']['scale']), (4, 4, 4))
        self.assertLess(hd['creature']['strength'], hd['scenery']['strength'])   # fur: conservative
        self.assertGreater(u['_boost'], 0)
        self.assertEqual(hd['normal']['scale'], 0)
        self.assertEqual([x['lightmap']['scale'] for x in (s, hd, u)], [0, 2, 4])   # the Lighting row
        self.assertEqual([x['lightdir']['scale'] for x in (s, hd, u)], [0, 0, 0])
        self.assertEqual(hd['lightmap']['model'], H.LIGHT_MODEL)

    def test_custom(self):
        c = H.level_settings('custom', ['creature.scale=4', 'scenery.model=texture-dat2', 'ui.denoise=0.5'], base='sharp')
        self.assertEqual(c['creature']['scale'], 4)
        self.assertEqual(c['scenery']['model'], 'texture-dat2')
        self.assertEqual(c['ui']['denoise'], 0.5)
        self.assertEqual(c['scenery']['scale'], 2)
        self.assertEqual(H.level_settings('custom', ['lighting.scale=0'])['lightmap']['scale'], 0)
        self.assertEqual(H.level_settings('custom', ['lightmap.scale=4'])['lightmap']['scale'], 4)
        for bad in ('creature.color=red', 'fish.scale=2', 'nonsense', 'lightdir.scale=2'):
            with self.assertRaises(ValueError):
                H.level_settings('custom', [bad])
        self.assertEqual(H.LEVELS['hd']['creature']['scale'], 2)   # overrides never leak into the table

    def test_sizes_and_formats(self):
        self.assertEqual(H.target_size(512, 256, 4, 4096), (2048, 1024))
        self.assertEqual(H.target_size(2048, 2048, 4, 4096), (4096, 4096))
        self.assertEqual(H.target_size(4096, 64, 2, 4096), (4096, 64))
        self.assertEqual(H.target_size(100, 30, 2, 4096), (200, 60))
        self.assertEqual(H.out_format(0x4a5, 'scenery', True), 0x4e5)
        self.assertEqual(H.out_format(0x4a0, 'creature', True), 0x4e0)
        self.assertEqual(H.out_format(0x485, 'scenery', True), 0x485)
        self.assertEqual(H.out_format(0x485, 'scenery', 'all'), 0x4e5)
        self.assertEqual(H.out_format(0x4a5, 'scenery', False), 0x4a5)
        self.assertEqual(H.out_format(0x280, 'normal', 'all'), 0x280)
        self.assertEqual(H.nutexb_bytes(0x485, 4, 4, 1), 512 + nb.FOOTER)   # one 512-byte GOB


def write_cache(root, files):
    root = Path(root)
    lines = []
    for rel, blob in files.items():
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(blob)
        lines.append(f'{rel}\t{len(blob)}\t{zlib.crc32(blob):08x}\t0')
    (root / 'index.tsv').write_text('\n'.join(lines) + '\n', encoding='utf-8')


class PlanTests(unittest.TestCase):
    def setUp(self):
        self.t = Path(tempfile.mkdtemp())
        self.cache = self.t / 'cache'
        write_cache(self.cache, {
            'nx/pack/characters/master/ch_998.drp/CH_998_00_0_A00_Body_dif.nutexb': make_nutexb(0x485, 64, 64, 7)[0],
            'nx/pack/levelcomposite/master/lc_001.drp/T_Ground_col.nutexb': make_nutexb(0x4a5, 128, 128, 8)[0],
            'nx/pack/levelcomposite/master/lc_001.drp/T_Ground_nor.nutexb': make_nutexb(0x280, 128, 128, 8)[0],
            'common/ui/menu.drp/icon_a.nutexb': make_nutexb(0x405, 64, 32, 1)[0],
        })

    def test_plan_hd(self):
        items, counts = H.plan(self.cache, H.level_settings('hd'), {'nomos2-plksr': 'a', 'texture-dat2': 'c', 'spanimation2': 'b'}, bc7=True)
        self.assertEqual(counts, {'creature': 1, 'scenery': 1, 'normal': 1, 'ui': 1})
        by = {i.cat: i for i in items}
        self.assertEqual(set(by), {'creature', 'scenery', 'ui'})          # HD leaves normal maps alone
        self.assertEqual((by['scenery'].out_w, by['scenery'].out_fmt, by['scenery'].out_mips), (512, 0x4e5, 10))
        self.assertEqual((by['creature'].out_w, by['creature'].out_fmt), (128, 0x485))
        self.assertEqual((by['ui'].out_w, by['ui'].out_h, by['ui'].out_mips), (128, 64, 1))
        est = H.estimate(items)
        self.assertEqual(est['disk'], sum(i.out_bytes for i in items))
        self.assertGreater(est['vram_peak'], est['vram_peak_orig'])

    def test_keys_follow_model_and_level(self):
        a, _ = H.plan(self.cache, H.level_settings('hd'), {'nomos2-plksr': 'a'})
        b, _ = H.plan(self.cache, H.level_settings('hd'), {'nomos2-plksr': 'a'})
        c, _ = H.plan(self.cache, H.level_settings('hd'), {'nomos2-plksr': 'CHANGED'})
        d, _ = H.plan(self.cache, H.level_settings('ultra'), {'nomos2-plksr': 'a'})
        key = lambda xs, cat: next(i.key for i in xs if i.cat == cat)   # noqa: E731
        self.assertEqual(key(a, 'scenery'), key(b, 'scenery'))
        self.assertNotEqual(key(a, 'scenery'), key(c, 'scenery'))
        self.assertNotEqual(key(a, 'scenery'), key(d, 'scenery'))

    def test_exclude_and_only(self):
        items, _ = H.plan(self.cache, H.level_settings('hd'), {}, exclude=['998'])
        self.assertNotIn('creature', {i.cat for i in items})
        items, _ = H.plan(self.cache, H.level_settings('hd'), {}, only='common/ui')
        self.assertEqual([i.cat for i in items], ['ui'])

    def test_sharp_leaves_creatures(self):
        items, _ = H.plan(self.cache, H.level_settings('sharp'), {})
        self.assertNotIn('creature', {i.cat for i in items})


class ModsJsonTests(unittest.TestCase):
    def test_on_off_never_deletes(self):
        d = Path(tempfile.mkdtemp())
        self.assertFalse(H.set_enabled(d, False))
        self.assertFalse((d / 'mods.json').exists())
        (d / 'mods.json').write_text(json.dumps({'packs': [{'id': 'mine', 'enabled': True}]}))
        (d / H.PACK_ID).mkdir()
        (d / H.PACK_ID / 'x.nutexb').write_bytes(b'1')
        self.assertTrue(H.set_enabled(d, True))
        packs = json.loads((d / 'mods.json').read_text())['packs']
        self.assertEqual([p['id'] for p in packs], ['mine', H.PACK_ID])      # a hand-made pack keeps priority
        H.set_enabled(d, False)
        packs = json.loads((d / 'mods.json').read_text())['packs']
        self.assertEqual(packs[1], {'id': H.PACK_ID, 'enabled': False})
        self.assertTrue(packs[0]['enabled'])
        self.assertTrue((d / H.PACK_ID / 'x.nutexb').exists())


class StateTests(unittest.TestCase):
    def test_retire_and_restore(self):
        t = Path(tempfile.mkdtemp())
        pack, work = t / 'pack', t / 'work'
        rel = 'a.drp/x.nutexb'
        (pack / 'a.drp').mkdir(parents=True)
        (pack / rel).write_bytes(b'hd')
        (pack / 'a.drp/hand_made.nutexb').write_bytes(b'not ours')
        st = {'outputs': {rel: {'key': 'k1', 'bytes': 2, 'cat': 'ui'}}}
        self.assertEqual(H.retire_stale(pack, work, st, keep=set(), say=lambda *_: None), 1)
        self.assertFalse((pack / rel).exists())
        self.assertTrue((pack / 'a.drp/hand_made.nutexb').exists())        # files we did not write are never touched
        it = types.SimpleNamespace(rel=rel, key='k2')
        self.assertFalse(H.restore_retired(pack, work, st, it))              # different settings: not reused
        it.key = 'k1'
        self.assertTrue(H.restore_retired(pack, work, st, it))
        self.assertEqual((pack / rel).read_bytes(), b'hd')
        self.assertIn(rel, st['outputs'])

    def test_batches(self):
        its = [types.SimpleNamespace(out_w=1024, out_h=1024) for _ in range(10)]
        got = [len(b) for _, b in H.batches(its, 4, 3.0)]
        self.assertEqual(got, [2, 2, 2, 2, 2])
        big = [types.SimpleNamespace(out_w=8192, out_h=8192)]
        self.assertEqual([len(b) for _, b in H.batches(big, 4, 3.0)], [1])
        self.assertEqual(sum(len(b) for _, b in H.batches(its, 3, 100)), 10)

    def test_locks(self):
        d = Path(tempfile.mkdtemp())
        (d / 'run2.lock').write_text('run_boot pid=1 hifps dlg60')
        self.assertEqual(H.busy_locks(d), [])
        (d / 'run0.lock').write_text('run_boot pid=2 cp_ride florio')
        self.assertEqual(len(H.busy_locks(d)), 1)
        self.assertEqual(H.wait_for_locks('', print), 0.0)
        calls = []

        def sleep(_):
            calls.append(1)
            (d / 'run0.lock').unlink()
        H.wait_for_locks(d, lambda *_: None, sleep=sleep)
        self.assertEqual(len(calls), 1)


class ImageTests(unittest.TestCase):
    def test_srgb_roundtrip(self):
        x = np.linspace(0, 1, 101, dtype=np.float32)
        np.testing.assert_allclose(im.lin_to_srgb(im.srgb_to_lin(x)), x, atol=1e-5)

    def test_resize_keeps_flat_and_tiling(self):
        flat = np.full((16, 16, 3), 0.25, np.float32)
        np.testing.assert_allclose(im.resize(flat, 64, 64), 0.25, atol=1e-5)
        y, x = np.mgrid[0:32, 0:32]
        tile = (0.5 + 0.4 * np.sin(2 * np.pi * x / 32) * np.cos(2 * np.pi * y / 32)).astype(np.float32)[..., None]
        self.assertTrue(im.tiles_seamlessly(tile))
        self.assertFalse(im.tiles_seamlessly((x / 31.0).astype(np.float32)[..., None]))
        big = im.resize(tile, 128, 128, wrap=True)
        self.assertLess(abs(float(big[0].mean() - big[-1].mean())), 0.05)   # edges still meet

    def test_pad(self):
        a = np.arange(16, dtype=np.float32).reshape(4, 4, 1)
        p = im.pad(a, 2, True)
        self.assertEqual(p.shape, (8, 8, 1))
        self.assertEqual(p[0, 2, 0], a[2, 0, 0])                            # wrap: the row two above row 0 is row 2

    def test_mips(self):
        img = np.random.default_rng(0).random((64, 32, 4)).astype(np.float32)
        ch = im.mip_chain(img, im.mip_count(32, 64), 'color')
        self.assertEqual([c.shape[:2] for c in ch], [(64, 32), (32, 16), (16, 8), (8, 4), (4, 2), (2, 1), (1, 1)])

    def test_alpha_coverage_kept(self):
        rng = np.random.default_rng(1)
        img = np.zeros((64, 64, 4), np.float32)
        img[..., 3] = (rng.random((64, 64)) > 0.7).astype(np.float32)        # sparse foliage cut-out
        want = im.coverage(img[..., 3], 0.5)
        for lv in im.mip_chain(img, 5, 'color', alpha_cutoff=0.5)[1:]:
            self.assertAlmostEqual(im.coverage(lv[..., 3], 0.5), want, delta=0.08)

    def test_normals(self):
        rng = np.random.default_rng(2)
        rg = rng.random((8, 8, 2)).astype(np.float32) * 0.8 + 0.1
        n = im.normal_from_rg(rg)
        np.testing.assert_allclose(np.linalg.norm(n, axis=-1), 1, atol=1e-4)
        np.testing.assert_allclose(im.rg_from_normal(n), rg, atol=1e-4)
        r = im.renormalize(n * 0.7)
        np.testing.assert_allclose(np.linalg.norm(r, axis=-1), 1, atol=1e-4)
        for m in im.mip_chain(np.concatenate([n, np.ones((8, 8, 1), np.float32)], -1), 3, 'normal')[1:]:
            np.testing.assert_allclose(np.linalg.norm(m[..., :3], axis=-1), 1, atol=1e-3)

    def test_log_space(self):
        x = np.array([0, 0.5, 4, 60000], np.float32)
        np.testing.assert_allclose(im.from_log(im.to_log(x)), x, rtol=1e-4)

    def test_blend_detail(self):
        base = np.full((4, 4, 3), 0.5, np.float32)
        ai = np.full((4, 4, 3), 0.9, np.float32)
        np.testing.assert_allclose(im.blend_detail(base, ai, 0.0), base)
        np.testing.assert_allclose(im.blend_detail(base, ai, 1.0), ai, atol=1e-6)

    def test_back_project_undoes_colour_drift_and_keeps_lines(self):
        src = np.full((32, 32, 3), 0.4, np.float32)
        src[:, 15] = 0.6                                     # a one-texel line the model smoothed away
        ai = np.full((64, 64, 3), 0.4, np.float32) * np.array([1.08, 1.0, 1.12], np.float32)   # tinted, no line
        out = im.back_project(ai, src, wrap=False)
        down = im.resize(out, 32, 32)
        self.assertLess(np.abs(down - src).mean(), 0.01)
        np.testing.assert_allclose(out[:, :20].mean((0, 1)), [0.4, 0.4, 0.4], atol=0.01)   # tint gone
        self.assertGreater(out[:, 30:32].mean(), out[:, 20:26].mean() + 0.05)              # line back
        same = im.resize(src, 64, 64)
        np.testing.assert_allclose(im.back_project(same, src), same, atol=0.03)            # a faithful upscale barely moves

    def test_fill_cutout_hides_the_colour_under_clear_texels(self):
        px = np.zeros((32, 32, 4), np.uint8)
        px[..., :3] = 230                                    # what the artist left under the clear part: light
        px[8:24, 8:24] = [40, 70, 30, 255]                   # a dark leaf, solid
        out = H.fill_cutout(px)
        np.testing.assert_array_equal(out[..., 3], px[..., 3])          # alpha untouched
        np.testing.assert_array_equal(out[8:24, 8:24], px[8:24, 8:24])  # solid texels untouched
        self.assertLess(np.abs(out[6:8, 8:24, :3].astype(int) - [40, 70, 30]).max(), 3)   # next to the leaf: leaf colour
        soft = px.copy()
        soft[..., 3] = 128                                    # blended alpha, not a cut-out: left alone
        np.testing.assert_array_equal(H.fill_cutout(soft), soft)

    def test_local_contrast(self):
        rng = np.random.default_rng(3)
        x = im.resize(rng.random((16, 16, 3)).astype(np.float32) * 0.6 + 0.2, 64, 64, wrap=True)
        y = im.local_contrast(x, 0.7, wrap=True)
        self.assertEqual(y.shape, x.shape)
        self.assertGreaterEqual(float(y.min()), 0.0)
        self.assertAlmostEqual(float(y.mean() / x.mean()), 1.0, delta=0.02)   # colour and average kept
        fine = lambda z: float((z.mean(-1) - im.gauss(z.mean(-1), 1.2, True)).std())
        self.assertGreater(fine(y), fine(x) * 1.2)                                # grain crisper
        edge = float(np.abs(y[:, 0] - y[:, -1]).mean())                            # still tiles: the wrap edge
        inner = float(np.abs(y[:, 31] - y[:, 32]).mean())                          # is no harsher than inside
        self.assertLess(edge, inner * 1.5 + 1e-3)


class SetupTests(unittest.TestCase):
    def test_offline_files_are_never_moved_or_deleted(self):
        import hashlib
        import zipfile
        from snapbuild import hdtex_setup as S
        t = Path(tempfile.mkdtemp())
        off, tdir = t / 'offline', t / 'tools'
        off.mkdir()
        (off / 'm.onnx').write_bytes(b'model bytes')
        with zipfile.ZipFile(off / 'pack.zip', 'w') as z:
            z.writestr('ONNX/inner.onnx', b'inner model')
        sha = lambda b: hashlib.sha256(b).hexdigest()   # noqa: E731
        lic = {'name': 'MIT', 'url': 'https://example.invalid/license'}
        pins = {'models': {
            'plain': {'title': 'plain', 'engine': 'ort', 'scale': 4, 'filename': 'm.onnx', 'url': 'https://example.invalid/m.onnx',
                      'sha256': sha(b'model bytes'), 'size': 11, 'license': lic},
            'zipped': {'title': 'zipped', 'engine': 'ort', 'scale': 2, 'filename': 'inner.onnx', 'member': 'ONNX/inner.onnx',
                       'member_sha256': sha(b'inner model'), 'license': lic,
                       'archive': {'filename': 'pack.zip', 'url': 'https://example.invalid/pack.zip',
                                   'sha256': sha((off / 'pack.zip').read_bytes()), 'size': (off / 'pack.zip').stat().st_size}}}}
        no = lambda q: False   # noqa: E731  (any download question is refused: everything must come from --offline-dir)
        for mid, want in (('plain', b'model bytes'), ('zipped', b'inner model')):
            got = S.ensure_model(pins, mid, tdir, {'model:' + mid}, no, [off], say=lambda *_: None)
            self.assertEqual(Path(got).read_bytes(), want)
        self.assertTrue((off / 'm.onnx').exists())
        self.assertTrue((off / 'pack.zip').exists())
        self.assertIn('model:plain', json.loads(S.receipt_path(tdir).read_text()))

    def test_license_refused(self):
        from snapbuild import fetch, hdtex_setup as S
        pins = {'models': {'x': {'title': 'x', 'engine': 'ort', 'scale': 4, 'filename': 'x.onnx', 'url': 'https://example.invalid/x',
                                 'sha256': '0' * 64, 'license': {'name': 'CC BY-NC', 'personal_use_only': True}}}}
        with self.assertRaises(fetch.FetchError):
            S.ensure_model(pins, 'x', Path(tempfile.mkdtemp()), set(), lambda q: False, (), say=lambda *_: None)

    def test_yes_never_accepts_a_license(self):
        from snapbuild import hdtex_setup as S, hdtex_cli as C
        tdir = Path(tempfile.mkdtemp())
        self.assertFalse(S.accept('model:x', 'x', {'name': 'CC BY 4.0'}, tdir, set(), C._ask(True), say=lambda *_: None))
        self.assertTrue(S.accept('model:x', 'x', {'name': 'CC BY 4.0'}, tdir, {'model:x'}, C._ask(True), say=lambda *_: None))
        self.assertIn('model:x', S.accepted_before(tdir))   # recorded once accepted

    def test_needed_licenses(self):
        from snapbuild import hdtex as H, hdtex_cli as C
        pins = H.load_pins()
        keys = [x['key'] for x in C.needed_licenses(pins, ['nomos2-plksr', 'x4plus'], Path(tempfile.mkdtemp()))]
        self.assertEqual(keys, ['texconv', 'directml', 'ncnn', 'model:nomos2-plksr', 'model:x4plus'])
        self.assertEqual([x['key'] for x in C.needed_licenses(pins, [], Path(tempfile.mkdtemp()))], ['texconv'])


class LightmapTests(unittest.TestCase):
    """The HDR light map path: energy kept, no NaN, gutters filled (no dark seams), float16 out."""
    def lightmap(self, n=64):
        rng = np.random.default_rng(3)
        y, x = np.mgrid[0:n, 0:n] / n
        light = 0.2 + 6.0 * ((x + 0.3 * y) > 0.5) + 30.0 * np.exp(-((x - 0.3) ** 2 + (y - 0.7) ** 2) / 0.003)
        px = np.ones((n, n, 4), np.float32)
        px[..., :3] = light[..., None] * np.array([1.0, 0.9, 0.7]) * (1 + 0.05 * rng.standard_normal((n, n, 1)))
        px[:, :n // 8, :3] = 0          # an empty gutter left of the island
        px[n // 2:n // 2 + 3, :, :3] = 0   # and one through it
        return px

    def test_process(self):
        px = self.lightmap()
        n = px.shape[0]
        it = H.Item('lm', 'lightmap', 0x4d7, n, n, 1, 0, 0x4d7, 2 * n, 2 * n, 1, 0, 2, H.LIGHT_MODEL, 1.0, 0.0)
        x, rng = H.light_prepare(px)
        self.assertTrue(0 <= x.min() and x.max() <= 1 and x.dtype == np.float32)
        ai = NearestEngine().upscale(im.pad(x, H.PAD, False))
        for a in (None, ai):
            out = H.process_lightmap(it, px, False, a, (x, rng), 2)
            self.assertEqual((out[0].shape, out[0].dtype), ((2 * n, 2 * n, 4), np.float16))
            o = out[0].astype(np.float32)
            self.assertTrue(np.isfinite(o).all())
            self.assertGreaterEqual(o.min(), 0)
            m = px[..., :3].max(2) > 0
            mo = np.repeat(np.repeat(m, 2, 0), 2, 1)
            src, got = px[..., :3][m].mean(), o[..., :3][mo].mean()
            self.assertLess(abs(got / src - 1), 0.02)                       # energy within 2%
            self.assertGreater(o[:, :n // 8 - 4, :3].min(), 0.1)            # gutter holds island light, not black
            L, M = o[..., :3].max(2), mo                                       # the bilinear footprint on island borders:
            q = [(L[i:L.shape[0] - 1 + i, j:L.shape[1] - 1 + j], M[i:M.shape[0] - 1 + i, j:M.shape[1] - 1 + j]) for i in (0, 1) for j in (0, 1)]
            cnt = sum(m.astype(int) for _, m in q)
            edge = (cnt > 0) & (cnt < 4)
            full = sum(v for v, _ in q) / 4
            isl = sum(v * m for v, m in q) / np.maximum(cnt, 1)
            self.assertLess(abs(full[edge].sum() / isl[edge].sum() - 1), 0.05)   # gutters neither darken nor brighten seams

    def test_directional_never_planned(self):
        self.assertEqual(H.level_settings('ultra')['lightdir']['scale'], 0)


class ShadowTests(unittest.TestCase):
    """The HD shadows row: its own category and pack, edge-aware alpha upscale, game files read in place, pause while the game runs."""
    def mask(self, n=64):
        y, x = np.mgrid[0:n, 0:n]
        a = ((x * 0.7 + y * 0.4) % 23 < 9).astype(np.float32) * 255     # hard diagonal bars
        a[10:14, 10:14] = 128                                             # one soft half-tone patch
        px = np.zeros((n, n, 4), np.uint8)
        px[..., 3] = a
        return px

    def item(self, n, scale, strength=1.0):
        return H.Item('s', 'shadow', 0x4a5, n, n, 1, 0, 0x4a5, scale * n, scale * n, im.mip_count(scale * n, scale * n), 0, scale, '', strength)

    def test_process_keeps_the_games_shadow_and_sharpens_edges(self):
        px = self.mask()
        n = px.shape[0]
        a = px[..., 3].astype(np.float32) / 255
        out = H.process_shadow(self.item(n, 2), px, False)
        self.assertEqual(len(out), im.mip_count(2 * n, 2 * n))
        o = out[0]
        self.assertEqual((o.shape, o.dtype), ((2 * n, 2 * n, 4), np.uint8))
        self.assertEqual(int(o[..., :3].max()), 0)                       # colour stays black
        got = o[..., 3].astype(np.float32) / 255
        self.assertLess(abs(got.mean() - a.mean()), 0.02)                # the amount of shadow is the game's
        plain = np.clip(im.resize(a, 2 * n, 2 * n), 0, 1)
        self.assertLess(np.minimum(got, 1 - got).sum(), np.minimum(plain, 1 - plain).sum())   # fewer half-tone texels than plain Lanczos
        lo = np.repeat(np.repeat(a, 2, 0), 2, 1)
        self.assertTrue(((got >= lo.min() - 1e-6) & (got <= 1 + 1e-6)).all())
        off = H.process_shadow(self.item(n, 2, 0.0), px, False)[0][..., 3].astype(np.float32) / 255
        self.assertLess(np.abs(off - plain).max(), 0.15)                 # strength 0: plain enlargement, only de-ringed

    def test_shadow_edge_is_capped(self):
        t = Path(tempfile.mkdtemp())
        rels = {f'nx/Pack/LevelComposite/master/LC_001_A.drp/LV^M^{n}^s^0^Shadow.nutexb': make_nutexb(0x4a5, n, n, 1)[0] for n in (512, 1024, 2048)}
        write_cache(t, rels)
        items, _ = H.plan(t, H.level_settings('hd'), {}, cats=['shadow'])
        self.assertEqual(sorted((i.src_w, i.out_w) for i in items), [(512, 1024), (1024, 2048)])   # 2048 stays as the game has it
        items, _ = H.plan(t, H.level_settings('ultra'), {}, cats=['shadow'])
        self.assertEqual(sorted((i.src_w, i.out_w) for i in items), [(512, 2048), (1024, 2048)])

    def test_planned_only_with_cats_and_keeps_mips(self):
        t = Path(tempfile.mkdtemp())
        rel = 'nx/Pack/LevelComposite/master/LC_001_A.drp/LV^M^1^s^0^Shadow.nutexb'
        write_cache(t, {rel: make_nutexb(0x4a5, 128, 128, 1)[0]})
        items, counts = H.plan(t, H.level_settings('hd'), {})
        self.assertEqual((items, counts), ([], {'shadow': 1}))           # the textures pack never builds shadows
        items, _ = H.plan(t, H.level_settings('hd'), {}, cats=['shadow'])
        self.assertEqual(len(items), 1)
        self.assertEqual((items[0].out_w, items[0].out_fmt, items[0].out_mips), (256, 0x4a5, 1))
        items, _ = H.plan(t, H.level_settings('ultra'), {}, cats=['shadow'])
        self.assertEqual(items[0].out_w, 512)
        items, _ = H.plan(t, H.level_settings('custom', ['shadow.scale=0']), {}, cats=['shadow'])
        self.assertEqual(items, [])

    def test_scan_data_reads_game_files_in_place(self):
        root = Path(tempfile.mkdtemp())
        d = root / 'nx/Pack/LevelComposite/master/LC_002_A.drp'
        d.mkdir(parents=True)
        (d / 'a^Shadow.nutexb').write_bytes(b'x' * 10)
        (d / 'a^0^Lightmap.nutexb').write_bytes(b'y' * 5)
        (d / 'a_col.nutexb').write_bytes(b'z')
        idx = root / 'idx.tsv'
        self.assertEqual(H.scan_data(root, idx), 1)
        got = H.read_index(root, idx)
        self.assertEqual(list(got), ['nx/Pack/LevelComposite/master/LC_002_A.drp/a^Shadow.nutexb'])
        self.assertEqual(got[next(iter(got))][0], 10)

    def test_scan_data_all_categories_walks_every_archive(self):
        root = Path(tempfile.mkdtemp())
        for rel in ('nx/Pack/LevelComposite/master/LC_002_A.drp/a_col.nutexb', 'nx/Pack/Characters/master/CH_025.drp/b_dif.nutexb',
                    'common/UI/master/Scenes/album.drp/album-00001.nutexb', 'resource/nx/resource.drp/dummy_white_texture.nutexb',
                    'nx/Pack/Characters/master/loose.nutexb', 'nx/Pack/Characters/master/CH_025.drp/b.numatb'):
            (root / rel).parent.mkdir(parents=True, exist_ok=True)
            (root / rel).write_bytes(b'x' * 7)
        idx = root / 'idx.tsv'
        self.assertEqual(H.scan_data(root, idx, suffixes=None), 3)   # not resource/, not outside a .drp, not other files
        self.assertEqual(sorted(H.read_index(root, idx)), ['common/UI/master/Scenes/album.drp/album-00001.nutexb',
                                                           'nx/Pack/Characters/master/CH_025.drp/b_dif.nutexb',
                                                           'nx/Pack/LevelComposite/master/LC_002_A.drp/a_col.nutexb'])

    def test_waits_while_the_game_runs(self):
        states = iter([True, True, False])
        real, H.game_running = H.game_running, lambda: next(states)
        try:
            said, slept = [], []
            H.wait_for_game(said.append, poll=0.0, sleep=slept.append)
            self.assertEqual((len(said), len(slept)), (1, 2))
        finally:
            H.game_running = real


class NearestEngine:
    """Stand-in for an AI model: nearest-neighbor 2x."""
    scale = 2
    providers = ['test']

    def upscale(self, rgb):
        return np.repeat(np.repeat(rgb, 2, 0), 2, 1)

    def upscale_many(self, items):
        return {k: self.upscale(v) for k, v in items}


class PackStateTests(unittest.TestCase):
    """Each pack keeps its own state: building the shadow pack must not touch the color pack's records."""
    def run_status(self, *extra):
        import argparse
        from snapbuild import hdtex_cli as C
        t = Path(tempfile.mkdtemp())
        old = os.environ.get('LOCALAPPDATA')
        os.environ['LOCALAPPDATA'] = str(t)
        try:
            p = argparse.ArgumentParser()
            C.add_args(p)
            a = p.parse_args(['--status', '--cache', str(t / 'cache'), '--mods-dir', str(t / 'mods'), *extra])
            lines = []
            C.main(a, say=lines.append)
        finally:
            if old is None:
                os.environ.pop('LOCALAPPDATA', None)
            else:
                os.environ['LOCALAPPDATA'] = old
        return t, '\n'.join(lines)

    def test_status_honors_pack_id(self):
        _, out = self.run_status('--pack-id', 'hd-shadows')
        self.assertIn('hd-shadows', out)
        self.assertNotIn('hd-ai-textures', out)
        _, out = self.run_status()
        self.assertIn('hd-ai-textures', out)

    def test_other_pack_gets_its_own_work_folder(self):
        import argparse
        from snapbuild import hdtex_cli as C
        p = argparse.ArgumentParser()
        C.add_args(p)
        d = C.work_dir(p.parse_args(['--level', 'hd']))
        s = C.work_dir(p.parse_args(['--level', 'hd', '--pack-id', 'hd-shadows']))
        self.assertEqual(s, d / 'packs' / 'hd-shadows')
        self.assertEqual(C.work_dir(p.parse_args(['--level', 'hd', '--pack-id', 'hd-shadows', '--work', 'x'])), Path('x'))


class RebakeTests(unittest.TestCase):
    """HD shadows Stage B (--shadow-source rebaked): the sun table, the per-map Stage A fallback and the pack switch."""
    def parser(self):
        import argparse
        from snapbuild import hdtex_cli as C
        p = argparse.ArgumentParser()
        C.add_args(p)
        return C, p

    def test_shadow_source_flag(self):
        _, p = self.parser()
        self.assertEqual(p.parse_args(['--level', 'hd']).shadow_source, 'upscaled')
        self.assertEqual(p.parse_args(['--level', 'hd', '--shadow-source', 'rebaked']).shadow_source, 'rebaked')
        with self.assertRaises(SystemExit):
            p.parse_args(['--level', 'hd', '--shadow-source', 'other'])

    def test_sun_table(self):
        from snapbuild import hdshadows_rebake as RB
        t = Path(tempfile.mkdtemp()) / 'sun.tsv'
        t.write_text('level\taz\tel\tradius\tbias\tinvert\tflags\tbacc\ttwo_sided\n'
                     'LC_001_A:LV_ENV_001_1_A\t5.25\t47\t0.27\t0.1\t0\t3\t0.84\t\n'
                     'LC_001_A:LV_ENV_001_2_A\t237.5\t79.25\t0.27\t0.1\t0\t3\t0.90\t\n'
                     'LC_002_A:LV_ENV_002_1_A\t10\t60\t0.27\t0.1\t0\t3\t0.88\t\nbroken line\n', encoding='utf-8')
        s = RB.suns(t)
        self.assertEqual(sorted(s), ['LC_001_A', 'LC_002_A'])
        self.assertEqual(len(s['LC_001_A']), 2)
        self.assertEqual(RB.suns(t.with_name('missing.tsv')), {})
        self.assertTrue(RB.SUN_TABLE.is_file(), 'the measured sun table ships with the tool')

    def test_fallback_rule(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location('hdshadows_pack', REPO / 'scripts' / 'hd' / 'hdshadows_pack.py')
        P = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(P)
        self.assertTrue(P.good((0.9, 0.95, 5000), 0.85, 0.88))
        self.assertFalse(P.good((0.8, 0.95, 5000), 0.85, 0.88))        # misses shadows: the upscaled map instead
        self.assertFalse(P.good((0.9, 0.85, 5000), 0.85, 0.88))
        self.assertFalse(P.good(None, 0.85, 0.88))                       # no score: never the re-bake
        self.assertTrue(P.good((0.5, 0.95, 10), 0.85, 0.88))            # few lit texels: agreement decides
        self.assertFalse(P.good((0.99, 0.89, 10), 0.85, 0.88))
        t = Path(tempfile.mkdtemp()) / 'report.tsv'
        t.write_text('LC_001_A\tLV\ta^Shadow.nutexb\t64\t128\t0.9100\t0.9300\t0.1\t4000\t0\n', encoding='utf-8')
        self.assertEqual(P.read_report(t), {'a^Shadow.nutexb': (0.91, 0.93, 4000)})

    def test_off_turns_both_shadow_packs_off(self):
        from snapbuild import hdshadows_rebake as RB
        C, p = self.parser()
        t = Path(tempfile.mkdtemp())
        mods = t / 'mods'
        H.set_enabled(mods, True, RB.STAGE_A_ID)
        H.set_enabled(mods, True, RB.PACK_ID)
        H.set_enabled(mods, True, H.PACK_ID)
        a = p.parse_args(['--level', 'off', '--pack-id', 'hd-shadows', '--cache', str(t / 'cache'), '--mods-dir', str(mods), '--work', str(t / 'w')])
        self.assertEqual(C.main(a, say=lambda *_: None), 0)
        on = {e['id']: e['enabled'] for e in json.loads((mods / 'mods.json').read_text(encoding='utf-8'))['packs']}
        self.assertEqual(on, {RB.STAGE_A_ID: False, RB.PACK_ID: False, H.PACK_ID: True})   # the HD textures pack is not touched


@unittest.skipIf(TEXCONV is None, 'texconv.exe not available (set SNAP_TEXCONV)')
class RunTests(unittest.TestCase):
    def test_end_to_end(self):
        t = Path(tempfile.mkdtemp())
        cache = t / 'cache'
        rels = {'scen': 'nx/pack/levelcomposite/master/lc_001.drp/T_Ground_col.nutexb',
                'cre': 'nx/pack/characters/master/ch_998.drp/CH_998_00_0_A00_Body_dif.nutexb',
                'nor': 'nx/pack/levelcomposite/master/lc_001.drp/T_Ground_nor.nutexb',
                'lm': 'nx/pack/levelcomposite/master/lc_001.drp/T_Lightmap_000.nutexb'}
        # Valid BC blocks: let texconv encode real images, then wrap them in nutexbs.
        src = t / 'src'
        src.mkdir()
        rng = np.random.default_rng(3)
        imgs = {'scen': (0x4a5, 29, 64), 'cre': (0x485, 29, 32), 'nor': (0x280, 28, 32), 'lm': (0x4d7, 10, 32)}
        files = {}
        for k, (fmt, in_dx, n) in imgs.items():
            if in_dx == 10:
                px = (rng.random((n, n, 4)) * 4).astype(np.float16)
            else:
                px = (rng.random((n, n, 4)) * 255).astype(np.uint8)
                px[..., 3] = 255
            nb.write_dds(src / f'{k}.dds', in_dx, n, n, [px.tobytes()])
            H.texconv(TEXCONV, [src / f'{k}.dds'], t / f'enc_{k}', nb.FORMATS[fmt][5], ('-m', '1'))
            g, w, h, lv = nb.read_dds(t / f'enc_{k}' / f'{k}.dds')
            files[rels[k]] = nb.build(TEMPLATE, fmt, w, h, [nb.rows(lv[0], g, w, h)])
        write_cache(cache, files)
        settings = H.level_settings('custom', ['scenery.model=fake', 'creature.model=fake', 'normal.scale=2', 'lightmap.scale=2'],
                                    base='hd')
        items, _ = H.plan(cache, settings, {'fake': '1'}, bc7=True)
        self.assertEqual(len(items), 4)
        mods = t / 'mods'
        o = H.RunOptions(cache=cache, work=t / 'work', pack=mods / H.PACK_ID, texconv=TEXCONV, make_engine=lambda m: NearestEngine(),
                         model_scale=lambda m: 2, ab_orig=True, level='custom', settings=settings)
        rep = H.run(items, o, say=lambda *_: None)
        self.assertEqual((rep['built'], rep['failed']), (4, 0))
        out = nb.parse((mods / H.PACK_ID / rels['scen']).read_bytes())
        self.assertEqual((out.w, out.h, out.fmt, out.mips), (256, 256, 0x4e5, 1))
        out = nb.parse((mods / H.PACK_ID / rels['cre']).read_bytes())
        self.assertEqual((out.w, out.fmt), (64, 0x485))
        self.assertEqual(nb.parse((mods / H.PACK_ID / rels['lm']).read_bytes()).fmt, 0x4d7)
        self.assertEqual(nb.parse((mods / H.PACK_ID / rels['nor']).read_bytes()).fmt, 0x280)
        orig = t / 'work' / 'ab-orig' / rels['scen']
        self.assertTrue(orig.is_file())
        o2 = nb.parse(orig.read_bytes())
        self.assertEqual((o2.w, o2.fmt), (64, 0x4e5))                     # original, in the replacement's format
        rep = H.run(items, o, say=lambda *_: None)
        self.assertEqual((rep['built'], rep['up_to_date']), (0, 4))         # incremental: nothing rebuilt


if __name__ == '__main__':
    unittest.main()
