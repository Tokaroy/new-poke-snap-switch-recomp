"""Studio exporter: writes glTF 2.0 (+ PNG maps) of the original and of candidate variants for the local review page.

Everything written is game-derived and goes to build/studio/ (git-ignored). Nothing is uploaded.

Reuses the existing decoders: scripts/hd/build_cache.py (SSBH -> glTF through ssbh_to_gltf, nutexb -> PNG cache, matinstb
material slots) and scripts/hd/mesh_refine (`strip` picks one triangle set out of a refined numshb).

Run (from the repo root, needs Pillow; use the project Python):
  python -E -P scripts/studio/export_studio.py all                       # every entry in scripts/studio/candidates.json
  python -E -P scripts/studio/export_studio.py all --only bouffalant dodrio
  python -E -P scripts/studio/export_studio.py add --species p097 --source-chat hd-textures \
        --tex-suffix solr --tex-label "Sol repaint" --notes "text"      # texture candidate (cache variant suffix)
  python -E -P scripts/studio/export_studio.py add --species p007_00 --source-chat models \
        --mesh-pack build/w/models/pack/mdl-proto --notes "text"       # mesh candidate (refined numshb from a pack)
Giving both --tex-suffix and --mesh-pack on one species also writes the combined variant. `add` stores the entry in
candidates.json (so a rerun of `all` rebuilds it) and exports it right away. Each entry is keyed by (species, variant);
re-adding updates it. Other chats' entries in build/studio/manifest.json are kept.

Variant ids: original, tex (or tex-<tag> if --tag is given), mesh, both. Kinds: original | tex | mesh | both.
Maps written per material: albedo (RGBA PNG), normal, orm (R = occlusion from _par.G, G = roughness from _par.R,
B = 0; the spec mask _par.B is kept in extras), emissive. Skeleton and skin weights are kept (static bind pose).
"""
import argparse, datetime, json, os, re, shutil, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(ROOT / 'scripts' / 'hd'))
import build_cache as bc  # noqa: E402
from PIL import Image  # noqa: E402

Image.MAX_IMAGE_PIXELS = None
STUDIO = ROOT / 'build' / 'studio'
ASSETS = STUDIO / 'assets'
MANIFEST = STUDIO / 'manifest.json'
CAND = HERE / 'candidates.json'
CACHE = bc.CACHE
REFINE = ROOT / 'build' / 'w' / 'models' / 'target' / 'release' / 'mesh_refine.exe'
MODEL_EXT = ('.nusrcmdlb', '.numdlb', '.numshb', '.nusktb', '.numatb', '.matinstb', '.modelb')
MAX_EDGE = 4096


def slug(n):
    return re.sub(r'[^a-z0-9]+', '-', n.lower()).strip('-')


def log(*a):
    print('[studio]', *a, flush=True)


def now():
    return datetime.datetime.now().astimezone().isoformat(timespec='seconds')


def mtime_iso(paths):
    t = max((p.stat().st_mtime for p in paths if p.exists()), default=0)
    return datetime.datetime.fromtimestamp(t).astimezone().isoformat(timespec='seconds') if t else now()


# ---------------------------------------------------------------- mesh sources
def refined_gltf(spec, pack_dir, workdir):
    """Convert the refined LOD0 numshb of a model pack to a glTF (one triangle set) in workdir. Returns the gltf path."""
    packdir, stem = spec['packdir'], spec['stem']
    rel = packdir.relative_to(bc.PACK)
    refined = Path(pack_dir) / 'nx' / 'Pack' / rel / (stem + '.numshb')
    if not refined.exists():
        raise RuntimeError('no refined %s in %s' % (stem + '.numshb', pack_dir))
    if not REFINE.exists():
        raise RuntimeError('mesh_refine.exe not built: ' + str(REFINE))
    tmp = workdir / 'm'
    tmp.mkdir(parents=True, exist_ok=True)
    for f in packdir.iterdir():
        if f.suffix in MODEL_EXT and f.is_file():
            shutil.copy2(f, tmp / f.name)
    stripped = tmp / (stem + '.numshb')
    r = subprocess.run([str(REFINE), 'strip', str(refined), str(stripped)], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError('mesh_refine strip failed: ' + (r.stderr or r.stdout)[-300:])
    bc.ensure_converter()
    gl = workdir / 'refined.gltf'
    r = subprocess.run([str(bc.CONVERTER), str(tmp / (stem + '.nusrcmdlb')), str(gl)], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError('ssbh_to_gltf failed: ' + r.stderr[-300:])
    bc.patch_gltf(gl, packdir, [])
    return gl


def original_gltf(spec):
    g = CACHE / spec['id'] / 'model.gltf'
    if not g.exists():
        log('cache missing for %s, building it with build_cache' % spec['id'])
        bc.build_asset(spec, False)
    return g


# ---------------------------------------------------------------- textures
def tex_src(name, suffix):
    """Path of the PNG for texture `name`: the candidate suffix if it exists, else the decoded original."""
    d = CACHE_TEX
    if suffix:
        p = d / ('%s.%s.png' % (name, suffix))
        if p.exists():
            return p, suffix
    p = d / ('%s.orig.png' % name)
    return (p if p.exists() else None), 'orig'


CACHE_TEX = CACHE  # replaced per species


def fit(im):
    if max(im.size) > MAX_EDGE:
        s = MAX_EDGE / max(im.size)
        im = im.resize((max(1, round(im.width * s)), max(1, round(im.height * s))), Image.LANCZOS)
    return im


def write_map(kind, name, suffix, outdir):
    """Write one material map PNG into outdir; returns (file name, source tag) or None. kind: albedo|normal|orm|emissive."""
    src, tag = tex_src(name, suffix)
    if src is None:
        return None
    fn = '%s.%s.%s.png' % (name, tag, kind)
    out = outdir / fn
    if out.exists() and out.stat().st_mtime >= src.stat().st_mtime:
        return fn, tag
    im = Image.open(src)
    if kind == 'orm':
        im = im.convert('RGBA')
        r, g, b, a = im.split()
        black = Image.new('L', im.size, 0)
        spec = Image.merge('RGB', (b, b, b)).convert('L')
        orm = Image.merge('RGB', (g, r, black))   # occlusion from _par.G, roughness from _par.R, metallic 0
        fit(orm).save(out, optimize=False, compress_level=3)
        fit(spec).save(outdir / (fn[:-4] + '.spec.png'), compress_level=3)
        return fn, tag
    if kind == 'normal':
        im = im.convert('RGB')
    elif kind == 'emissive':
        im = im.convert('RGB')
    elif kind == 'albedo':
        im = im.convert('RGBA')
        if im.getchannel('A').getextrema()[0] == 255:
            im = im.convert('RGB')
    if im.size == Image.open(src).size and max(im.size) <= MAX_EDGE and kind in ('normal', 'albedo') and src.suffix == '.png' and im.mode == Image.open(src).mode:
        shutil.copyfile(src, out)
    else:
        fit(im).save(out, compress_level=3)
    return fn, tag


def material_roles(m):
    roles = {}
    for e in m.get('extras', {}).get('textures', []):
        roles.setdefault(e['role'], e['name'])
    return roles


def build_variant(src_gltf, outdir, texdir, suffix, rel_tex):
    """Rewrite src_gltf into outdir/model.gltf with the maps of `suffix` (None = originals). Returns stats."""
    g = json.loads(src_gltf.read_text())
    bin_src = src_gltf.parent / g['buffers'][0]['uri']
    outdir.mkdir(parents=True, exist_ok=True)
    texdir.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(bin_src, outdir / 'model.bin')
    g['buffers'][0]['uri'] = 'model.bin'
    images, tex_idx = [], {}

    def tex(fn, srgb):
        if fn not in tex_idx:
            images.append({'name': fn[:-4], 'uri': rel_tex + fn})
            tex_idx[fn] = len(images) - 1
        return {'index': tex_idx[fn]}
    used = []
    for m in g.get('materials', []):
        roles = material_roles(m)
        for k in ('normalTexture', 'emissiveTexture', 'occlusionTexture'):
            m.pop(k, None)
        m.pop('emissiveFactor', None)
        pbr = m.setdefault('pbrMetallicRoughness', {})
        pbr.pop('baseColorTexture', None)
        pbr.pop('metallicRoughnessTexture', None)
        pbr['metallicFactor'] = 0.0
        pbr['roughnessFactor'] = 0.7
        ex = m.setdefault('extras', {})
        ex['maps'] = {}
        if 'albedo' in roles:
            w = write_map('albedo', roles['albedo'], suffix, texdir)
            if w:
                pbr['baseColorTexture'] = tex(w[0], True)
                ex['maps']['albedo'] = w[1]
                try:
                    if Image.open(texdir / w[0]).mode == 'RGBA':
                        ex['albedoAlpha'] = True
                except Exception:
                    pass
        if 'normal' in roles:
            w = write_map('normal', roles['normal'], suffix, texdir)
            if w:
                m['normalTexture'] = tex(w[0], False)
                ex['maps']['normal'] = w[1]
        elif suffix:
            # the candidate may add a normal map the original material does not bind (cache-only normal)
            base = roles.get('albedo', '')
            nm = base[:-4] + '_nor' if base.endswith('_dif') else None
            if nm and (CACHE_TEX / ('%s.%s.png' % (nm, suffix))).exists():
                w = write_map('normal', nm, suffix, texdir)
                if w:
                    m['normalTexture'] = tex(w[0], False)
                    ex['maps']['normal'] = w[1] + ' (new slot; original material has none)'
        if 'mask' in roles:
            w = write_map('orm', roles['mask'], suffix, texdir)
            if w:
                t = tex(w[0], False)
                pbr['metallicRoughnessTexture'] = t
                m['occlusionTexture'] = dict(t)
                pbr['roughnessFactor'] = 1.0
                ex['maps']['orm'] = w[1]
                ex['specMask'] = rel_tex + w[0][:-4] + '.spec.png'
        if 'emissive' in roles:
            w = write_map('emissive', roles['emissive'], suffix, texdir)
            if w:
                m['emissiveTexture'] = tex(w[0], True)
                m['emissiveFactor'] = [1, 1, 1]
                ex['maps']['emissive'] = w[1]
        used.append({'material': m['name'], 'maps': ex['maps']})
    g['images'] = images
    g['textures'] = [{'source': i, 'sampler': 0} for i in range(len(images))]
    g['samplers'] = [{'magFilter': 9729, 'minFilter': 9987, 'wrapS': 10497, 'wrapT': 10497}]
    if not images:
        for k in ('images', 'textures', 'samplers'):
            g.pop(k)
    g['asset'] = {'version': '2.0', 'generator': 'snap-recomp studio exporter'}
    (outdir / 'model.gltf').write_text(json.dumps(g, separators=(',', ':')))
    tris = verts = 0
    for me in g['meshes']:
        for p in me['primitives']:
            tris += g['accessors'][p['indices']]['count'] // 3 if 'indices' in p else 0
            verts += g['accessors'][p['attributes']['POSITION']]['count']
    return {'tris': tris, 'verts': verts, 'materials': used, 'bones': len(g['skins'][0]['joints']) if g.get('skins') else 0,
            'has_tangents': all('TANGENT' in p['attributes'] for me in g['meshes'] for p in me['primitives'])}


# ---------------------------------------------------------------- manifest
def load_manifest():
    if MANIFEST.exists():
        try:
            return json.loads(MANIFEST.read_text())
        except Exception:
            pass
    return {'version': 1, 'species': {}, 'candidates': []}


def save_manifest(m):
    m['updated'] = now()
    m['candidates'].sort(key=lambda c: c.get('date', ''), reverse=True)
    tmp = MANIFEST.with_suffix('.tmp')
    tmp.write_text(json.dumps(m, indent=1))
    os.replace(tmp, MANIFEST)


def export_entry(e, manifest):
    global CACHE_TEX
    spec = bc.resolve(e['species'])
    sid = slug(e.get('name') or spec['name'])
    CACHE_TEX = CACHE / spec['id'] / 'textures'
    sdir = ASSETS / sid
    texdir = sdir / 'textures'
    results = []
    suffix = e.get('tex_suffix')
    mesh_pack = e.get('mesh_pack')
    mesh_pack = (ROOT / mesh_pack) if mesh_pack and not os.path.isabs(mesh_pack) else (Path(mesh_pack) if mesh_pack else None)
    tag = e.get('tag')
    plan = [('original', 'original', None, False)]
    if suffix:
        plan.append(('tex' + ('-' + tag if tag else ''), 'tex', suffix, False))
    if mesh_pack:
        plan.append(('mesh' + ('-' + tag if tag else ''), 'mesh', None, True))
    if suffix and mesh_pack:
        plan.append(('both' + ('-' + tag if tag else ''), 'both', suffix, True))
    src_files = []
    if suffix:
        src_files += list(CACHE_TEX.glob('*.%s.png' % suffix))
    if mesh_pack:
        src_files += list(mesh_pack.rglob('%s.numshb' % spec['stem']))
    date = e.get('date') or mtime_iso(src_files)
    base = None
    work = Path(tempfile.mkdtemp(prefix='studio_', dir=str(STUDIO)))
    try:
        refined = None
        for vid, kind, sfx, use_mesh in plan:
            if use_mesh and refined is None:
                refined = refined_gltf(spec, mesh_pack, work)
            src = refined if use_mesh else original_gltf(spec)
            stats = build_variant(src, sdir / vid, texdir, sfx, '../textures/')
            entry = {'species': sid, 'variant': vid, 'kind': kind, 'path': 'assets/%s/%s/model.gltf' % (sid, vid),
                     'tris': stats['tris'], 'verts': stats['verts'], 'bones': stats['bones'], 'tangents': stats['has_tangents'],
                     'materials': stats['materials']}
            if kind == 'original':
                manifest['species'].setdefault(sid, {})
                manifest['species'][sid].update({'name': e.get('name') or spec['name'], 'category': spec['category'], 'original': entry})
            else:
                entry.update({'label': e.get('label') or vid, 'source_chat': e.get('source_chat', 'unknown'), 'date': date,
                              'notes': e.get('notes', ''), 'exported': now()})
                manifest['candidates'] = [c for c in manifest['candidates'] if not (c['species'] == sid and c['variant'] == vid)]
                manifest['candidates'].append(entry)
            results.append((vid, stats['tris']))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return results


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    a = sub.add_parser('all')
    a.add_argument('--only', nargs='*')
    d = sub.add_parser('add')
    d.add_argument('--species', required=True, help='build_cache spec: p097 (Pokemon number), florio_rock, env:LC_001_A/<stem>')
    d.add_argument('--name')
    d.add_argument('--source-chat', required=True)
    d.add_argument('--tex-suffix')
    d.add_argument('--mesh-pack')
    d.add_argument('--label')
    d.add_argument('--tag')
    d.add_argument('--notes', default='')
    d.add_argument('--date')
    args = ap.parse_args()
    STUDIO.mkdir(parents=True, exist_ok=True)
    cands = json.loads(CAND.read_text()) if CAND.exists() else {'entries': []}
    if args.cmd == 'add':
        sp = re.sub(r'^(p\d{3})_\d\d$', lambda m: m.group(1), args.species)
        ent = {k: v for k, v in dict(species=sp, name=args.name, source_chat=args.source_chat, tex_suffix=args.tex_suffix, mesh_pack=args.mesh_pack,
                                     label=args.label, tag=args.tag, notes=args.notes, date=args.date).items() if v}
        cands['entries'] = [x for x in cands['entries'] if not (x['species'] == sp and x.get('tag') == ent.get('tag') and x['source_chat'] == ent['source_chat']
                                                                 and bool(x.get('tex_suffix')) == bool(ent.get('tex_suffix')) and bool(x.get('mesh_pack')) == bool(ent.get('mesh_pack')))]
        cands['entries'].append(ent)
        CAND.write_text(json.dumps(cands, indent=1))
        todo = [ent]
    else:
        todo = [x for x in cands['entries'] if not args.only or x['species'] in args.only or slug(x.get('name') or bc.resolve(x['species'])['name']) in args.only]
    manifest = load_manifest()
    failed = []
    for ent in todo:
        try:
            r = export_entry(ent, manifest)
            log('%s: %s' % (ent['species'], ', '.join('%s=%d tris' % x for x in r)))
        except Exception as ex:  # keep going, report at the end
            failed.append((ent['species'], str(ex)))
            log('FAILED %s: %s' % (ent['species'], ex))
        save_manifest(manifest)
    if failed:
        log('failures: %s' % json.dumps(failed))
        sys.exit(1)


if __name__ == '__main__':
    main()
