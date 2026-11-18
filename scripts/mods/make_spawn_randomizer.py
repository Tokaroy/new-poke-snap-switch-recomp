#!/usr/bin/env python3
"""Spawn randomizer: build the local "spawn-randomizer" mod pack from a seed.

It shuffles which Pokemon appear at the spawn points of every course. The pack holds only changed `LV_MAP_*.levelb`
entries (the course placement files), in the loose-file layout of docs/mods.md. The game's own files are never
touched, and with the pack off the game is the original game. The output is game-derived: it stays in the mods
folder (%APPDATA%\\SnapRecomp\\mods or SNAP_MODS_DIR), never in git.

Modes
  within   each course keeps its own Pokemon; the species are shuffled among that course's spawn points.
  all      every course may show any Pokemon the game has a template for: each course's Pokemon templates are
           re-skinned to Pokemon taken from the other courses, then shuffled among the spawn points too.
  off      removes the pack and switches it off in mods.json.

Same seed + same mode + same game data = same result. The seed is any text (a number works too).

What stays fixed (listed in <pack>/fixed.txt and in docs/spawn-randomizer.md): see FIXED_RULES below. In short: props and
scenery objects, special forms, spawns driven by scripts, species that appear once in a level (secret and event
Pokemon), spawns that carry an extra script parameter, and multi-Pokemon spawns. The camera rail, its event records,
paths and every number other than the species link are never edited.

  python -I scripts/mods/make_spawn_randomizer.py --seed hello --mode within [--out <mods root>] [--data <drp root>]
  python -I scripts/mods/make_spawn_randomizer.py --if-stale        (settings.cfg keys spawn_rand / spawn_seed; used by host/play.ps1)
  python -I scripts/mods/make_spawn_randomizer.py --mode off
  options: --pack NAME  --courses REGEX  --report  (print the fixed list and counts, write nothing)  --selftest
"""
import argparse, glob, collections, hashlib, json, os, re, shutil, struct, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
DATA = os.path.join(ROOT, 'extracted', 'drp-v262144')
SUB = os.path.join('nx', 'Pack', 'LevelComposite', 'master')
VERSION = 3                      # bump when the output for a seed changes (the stale check includes it)
PACK = 'spawn-randomizer'
PLAYABLE = r'LC_0(0[1-9]|1[01])_'  # LC_001..LC_011: the eleven expedition courses and their legs. LC_101, LC_103, LC_201_* are menu and Photodex scenes.

FIXED_RULES = collections.OrderedDict([
    ('prop', 'Not a Pokemon: scenery and event props (flowers, nuts, branches, rocks, the Bidoof dam parts, backgrounds, eggs, apples), '
             'NPC and other special characters. Their scripts and the rail events name them.'),
    ('form', 'Special form or event variant of a Pokemon (names like CH_064_01_0 or CH_034_01). These are the scripted or story versions.'),
    ('multi', 'A spawn that places several Pokemon together (a group with a leader). The group is kept as the game placed it.'),
    ('param', 'Spawn carries an extra script parameter (record slot 10, the timed or counted event spawns). It belongs to its scene.'),
    ('single', 'The only spawn of its species in that level. A species that is placed once is almost always a secret, a boss or an event Pokemon.'),
    ('shared', 'All mode only: its template is shared with a fixed spawn, so it cannot be re-skinned without changing the fixed spawn.'),
])

PKM = re.compile(r'CH_(\d{3})$')          # plain species: CH_016
PKM_FORM0 = re.compile(r'CH_(\d{3})_00$')  # default form: CH_034_00
PKM_ANY = re.compile(r'CH_(\d{3})(_\d\d)?$')


# ---------------------------------------------------------------------------------------------------------------- FlatBuffers
def u32(b, p): return struct.unpack_from('<I', b, p)[0]
def i32(b, p): return struct.unpack_from('<i', b, p)[0]
def u16(b, p): return struct.unpack_from('<H', b, p)[0]


class T:
    """Schemaless FlatBuffers table (same reader as tools/level-editor/src/fb.rs)."""
    __slots__ = ('b', 'pos', 'vt', 'vs')

    def __init__(s, b, pos):
        s.b, s.pos = b, pos
        s.vt = pos - i32(b, pos)
        s.vs = u16(b, s.vt)
        if s.vs < 4 or s.vt < 0 or s.vt + s.vs > len(b):
            raise ValueError('bad vtable')

    def f(s, i):
        e = 4 + 2 * i
        if e + 2 > s.vs: return None
        o = u16(s.b, s.vt + e)
        return None if o == 0 else s.pos + o

    def n(s): return (s.vs - 4) // 2
    def u32(s, i):
        p = s.f(i); return None if p is None else u32(s.b, p)
    def tbl(s, i):
        p = s.f(i)
        return None if p is None else T(s.b, p + u32(s.b, p))
    def vec(s, i):
        p = s.f(i)
        if p is None: return None
        q = p + u32(s.b, p)
        return q + 4, u32(s.b, q)
    def tables(s, i):
        v = s.vec(i)
        if not v: return []
        q, n = v
        return [T(s.b, q + 4 * k + u32(s.b, q + 4 * k)) for k in range(n)]
    def string(s, i):
        p = s.f(i)
        if p is None: return None
        q = p + u32(s.b, p)
        n = u32(s.b, q)
        return bytes(s.b[q + 4:q + 4 + n]).decode('utf8', 'replace')


def root_of(b): return T(b, u32(b, 0))


def body(c):
    for s in range(1, c.n()):
        t = c.tbl(s)
        if t is not None: return s, t
    return None, None


# ---------------------------------------------------------------------------------------------------------------- level model
class Tmpl:
    def __init__(s, h):
        s.hash = h
        s.name = ''
        s.fields = {}      # logical name -> (field position, string)

    def strings(s): return {k: v[1] for k, v in s.fields.items()}


class Rec:
    """One spawn record (a table of a spawn component): where its template hash lives."""
    def __init__(s, ent, pos_hash, h, has_path, multi, param, eligible_kind):
        s.ent, s.pos_hash, s.hash = ent, pos_hash, h
        s.has_path, s.multi, s.param = has_path, multi, param
        s.reason = None   # why it is fixed, or None when it may change


class Level:
    def __init__(s, name, data):
        s.name = name
        s.data = bytearray(data)
        s.orig = bytes(data)
        s.tmpl = {}
        s.recs = []
        s.species_pos = []     # positions of the u32 species numbers of root slot 11
        s.parse()

    def parse(s):
        b = s.orig
        r = root_of(b)
        if r.u32(0) != 6:
            raise ValueError('levelb version %s, expected 6' % r.u32(0))
        for e in r.tables(2):
            h = e.u32(1)
            tm = Tmpl(h)
            is_t = False
            for c in e.tables(0):
                sl, t = body(c)
                if sl == 6:
                    is_t = True
                    for nm, slot in (('skeleton', 1), ('anim', 3)):
                        p = t.f(slot)
                        if p is not None: tm.fields[nm] = (p, t.string(slot))
                elif sl == 9:
                    is_t = True
                    p = t.f(1)
                    if p is not None: tm.fields['name'] = (p, t.string(1)); tm.name = t.string(1)
                elif sl == 11:
                    p = t.f(1)
                    if p is not None: tm.fields['behavior'] = (p, t.string(1))
                elif sl == 10:
                    recs = t.tables(3)
                    for rr in recs:
                        ph = rr.f(0)
                        if ph is None: continue
                        s.recs.append(Rec(h, ph, u32(b, ph), rr.f(2) is not None, len(recs) > 1, rr.f(10) is not None, None))
            if is_t: s.tmpl[h] = tm
        v = r.vec(11)
        if v: s.species_pos = [v[0] + 4 * i for i in range(v[1])]

    def name_of(s, h):
        t = s.tmpl.get(h)
        return t.name if t and t.name else '#%08x' % h

    # ------------------------------------------------------------------------------------------------ classification
    def classify(s):
        count = collections.Counter(r.hash for r in s.recs)
        for r in s.recs:
            nm = s.name_of(r.hash)
            m = PKM_ANY.fullmatch(nm)
            if not m:
                r.reason = 'prop'
            elif m.group(2) and m.group(2) != '_00':
                r.reason = 'form'
            elif r.multi:
                r.reason = 'multi'
            elif r.param:
                r.reason = 'param'
            elif count[r.hash] < 2:
                r.reason = 'single'
            else:
                r.reason = None

    def species_set(s):
        return sorted({int(PKM_ANY.fullmatch(s.name_of(r.hash)).group(1)) for r in s.recs if PKM_ANY.fullmatch(s.name_of(r.hash))})

    # ------------------------------------------------------------------------------------------------ edits
    def set_hash(s, rec, new):
        struct.pack_into('<I', s.data, rec.pos_hash, new)
        rec.hash = new

    def append_string(s, text):
        raw = text.encode('utf8')
        while len(s.data) % 4: s.data.append(0)
        pos = len(s.data)
        s.data += struct.pack('<I', len(raw)) + raw + b'\0'
        while len(s.data) % 4: s.data.append(0)
        return pos

    def set_string(s, field_pos, text):
        new = s.append_string(text)
        struct.pack_into('<I', s.data, field_pos, new - field_pos)


# ---------------------------------------------------------------------------------------------------------------- RNG
M64 = (1 << 64) - 1


class Rng:
    """splitmix64: tiny, portable, identical on every platform."""
    def __init__(s, *parts):
        h = hashlib.sha256('\x1f'.join(str(p) for p in parts).encode('utf8')).digest()
        s.x = struct.unpack('<Q', h[:8])[0]

    def next(s):
        s.x = (s.x + 0x9E3779B97F4A7C15) & M64
        z = s.x
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & M64
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & M64
        return z ^ (z >> 31)

    def below(s, n): return s.next() % n

    def shuffle(s, a):
        for i in range(len(a) - 1, 0, -1):
            j = s.below(i + 1)
            a[i], a[j] = a[j], a[i]


# ---------------------------------------------------------------------------------------------------------------- generation
def level_files(data_root, pattern):
    base = os.path.join(data_root, SUB)
    rx = re.compile(pattern)
    out = []
    for d in sorted(os.listdir(base)):
        if not (d.startswith('LC_') and d.endswith('.drp') and rx.match(d)): continue
        for f in sorted(os.listdir(os.path.join(base, d))):
            if f.startswith('LV_MAP_') and f.endswith('.levelb'):
                out.append((d, f, os.path.join(base, d, f)))
    return out


def donor_pool(levels):
    """Species the game can show anywhere: one template per default-form species, taken from the first course that has it."""
    pool = {}
    for lv in levels:
        for t in lv.tmpl.values():
            m = PKM.fullmatch(t.name) or PKM_FORM0.fullmatch(t.name)
            if not m or t.name in pool: continue
            if not all(k in t.fields for k in ('skeleton', 'anim', 'name', 'behavior')): continue
            pool[t.name] = t.strings()
    return pool


def generate(data_root, seed, mode, pattern=PLAYABLE):
    """Returns (list of (archive, file, bytes) to write, report dict)."""
    files = level_files(data_root, pattern)
    if not files: raise SystemExit('no course levels found under %s' % os.path.join(data_root, SUB))
    levels = []
    for d, f, p in files:
        with open(p, 'rb') as fh: lv = Level(f[:-7], fh.read())
        lv.arch, lv.file = d, f
        lv.classify()
        levels.append(lv)
    pool = donor_pool(levels) if mode == 'all' else {}
    out, rep = [], []
    for lv in levels:
        rng = Rng('snap-spawn-randomizer', seed, mode, lv.arch, lv.file)
        before = [r.hash for r in lv.recs]
        info = {'level': '%s/%s' % (lv.arch[:-4], lv.name), 'spawns': len(lv.recs), 'moved': 0, 'reskinned': {}}
        info['fixed'] = collections.Counter(r.reason for r in lv.recs if r.reason)
        info['fixed_species'] = collections.Counter(lv.name_of(r.hash) for r in lv.recs if r.reason and r.reason != 'prop')
        if mode == 'all' and pool:
            reskin(lv, pool, rng, info)
        shuffle_within(lv, rng)
        info['moved'] = sum(1 for r, h in zip(lv.recs, before) if r.hash != h)
        info['species_after'] = collections.Counter(lv.name_of(r.hash) for r in lv.recs if not r.reason)
        if bytes(lv.data) != lv.orig:
            out.append((lv.arch, lv.file, bytes(lv.data)))
        rep.append(info)
    return out, rep


def eligible(lv):
    return [r for r in lv.recs if not r.reason]


def shuffle_within(lv, rng):
    """Permute the template hashes among the eligible records. Records that follow a route and records with their own
    spot are shuffled apart (a Pokemon that walks a path stays on path spawns)."""
    for has_path in (False, True):
        grp = [r for r in eligible(lv) if r.has_path == has_path]
        hs = [r.hash for r in grp]
        if len(set(hs)) < 2: continue
        orig = list(hs)
        for _ in range(8):
            rng.shuffle(hs)
            if hs != orig: break
        for r, h in zip(grp, hs):
            lv.set_hash(r, h)


def reskin(lv, pool, rng, info):
    """All mode: give every freely movable template of the level a Pokemon from another course (no repeats in the
    level), by rewriting its four strings (skeleton, animation pack, character name, behavior name)."""
    fixed_h = {r.hash for r in lv.recs if r.reason}
    free = []
    for r in eligible(lv):
        if r.hash in fixed_h:
            r.reason = 'shared'
        elif r.hash not in free:
            free.append(r.hash)
    free = [h for h in free if all(k in lv.tmpl[h].fields for k in ('skeleton', 'anim', 'name', 'behavior'))]
    here = {lv.tmpl[h].name for h in fixed_h if h in lv.tmpl}
    names = sorted(n for n in pool if n not in here)
    rng.shuffle(names)
    picks = names[:len(free)]
    for h, nm in zip(free, picks):
        t, src = lv.tmpl[h], pool[nm]
        info['reskinned'][t.name] = nm
        for k in ('skeleton', 'anim', 'name', 'behavior'):
            lv.set_string(t.fields[k][0], src[k])
    if lv.species_pos:
        nums = sorted({int(PKM_ANY.fullmatch(lv.name_of(r.hash)).group(1)) for r in lv.recs if PKM_ANY.fullmatch(lv.name_of(r.hash))} |
                      {int(PKM_ANY.fullmatch(nm).group(1)) for nm in picks})
        have = [u32(lv.data, p) for p in lv.species_pos]
        # keep the fixed species, then the new ones; the vector cannot grow, so a longer set is cut at the end
        fixed_nums = [n for n in have if any(PKM_ANY.fullmatch(lv.name_of(r.hash)) and int(PKM_ANY.fullmatch(lv.name_of(r.hash)).group(1)) == n
                                             for r in lv.recs if r.reason)]
        new = list(dict.fromkeys(fixed_nums + [n for n in nums if n not in fixed_nums]))
        new = (new + [new[-1]] * len(have))[:len(have)] if new else have
        for p, n in zip(lv.species_pos, new): struct.pack_into('<I', lv.data, p, n)
        info['species_slot'] = '%d -> %s' % (len(have), new)
    # templates that were re-skinned now carry the new names; keep the classification meaningful
    for r in lv.recs:
        if r.hash in lv.tmpl: pass


# ---------------------------------------------------------------------------------------------------------------- pack writing
def mods_root(arg=None):
    if arg: return arg
    if os.environ.get('SNAP_MODS_DIR'): return os.environ['SNAP_MODS_DIR']
    return os.path.join(os.environ.get('APPDATA', '.'), 'SnapRecomp', 'mods')


def set_listed(root, pack, enabled):
    """Add or update the pack in mods.json (other entries and their order are kept)."""
    mj = os.path.join(root, 'mods.json')
    doc = {'packs': []}
    if os.path.exists(mj):
        try:
            with open(mj, 'r', encoding='utf-8-sig') as fh: doc = json.load(fh)
        except (ValueError, OSError):
            raise SystemExit('%s is not valid JSON; fix it or move it away' % mj)
    arr = doc.setdefault('packs', [])
    for i, e in enumerate(arr):
        eid = e if isinstance(e, str) else e.get('id', e.get('folder'))
        if eid == pack:
            if isinstance(e, str):
                if not enabled: arr[i] = {'id': pack, 'enabled': False}
            else:
                e['enabled'] = enabled
            break
    else:
        if enabled: arr.append({'id': pack, 'enabled': True})
    os.makedirs(root, exist_ok=True)
    with open(mj, 'w', encoding='utf-8') as fh: json.dump(doc, fh, indent=2); fh.write('\n')


def read_meta(root, pack):
    try:
        with open(os.path.join(root, pack, 'pack.json'), 'r', encoding='utf-8') as fh: return json.load(fh)
    except (OSError, ValueError):
        return {}


def write_pack(root, pack, seed, mode, files, rep):
    pd = os.path.join(root, pack)
    if os.path.isdir(pd):
        shutil.rmtree(pd)       # only a folder this tool made: it is named in pack.json below, and we checked it is ours first
    for arch, f, data in files:
        d = os.path.join(pd, SUB, arch)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, f), 'wb') as fh: fh.write(data)
    meta = {'name': 'Spawn randomizer', 'version': '1.%d' % VERSION, 'author': 'generated',
            'description': 'Shuffled spawns. Mode %s, seed "%s". Built from your own game files by make_spawn_randomizer.py; delete or switch off to play the original courses.' % (mode, seed),
            'spawn_rand': {'mode': mode, 'seed': seed, 'version': VERSION, 'levels': len(files)}}
    with open(os.path.join(pd, 'pack.json'), 'w', encoding='utf-8') as fh: json.dump(meta, fh, indent=2); fh.write('\n')
    with open(os.path.join(pd, 'fixed.txt'), 'w', encoding='utf-8') as fh: fh.write(fixed_text(rep, seed, mode))
    return pd


def fixed_text(rep, seed, mode):
    L = ['Spawn randomizer: mode %s, seed "%s"' % (mode, seed), '', 'Rules that keep a spawn where the game put it:']
    for k, v in FIXED_RULES.items(): L.append('  %-7s %s' % (k, v))
    L.append('')
    for i in rep:
        fx = ', '.join('%s %d' % (k, n) for k, n in sorted(i['fixed'].items())) or 'none'
        L.append('%s: %d spawns, %d changed, fixed: %s' % (i['level'], i['spawns'], i['moved'], fx))
        if i['fixed_species']:
            L.append('    fixed Pokemon: ' + ', '.join('%s x%d' % kv for kv in sorted(i['fixed_species'].items())))
        for a, b in sorted(i['reskinned'].items()): L.append('    template %s -> %s' % (a, b))
    return '\n'.join(L) + '\n'


# ---------------------------------------------------------------------------------------------------------------- settings
def read_settings():
    """settings.cfg key=value lines (runtime/hle/settings.cpp) -> dict. SNAP_SETTINGS names the file; otherwise the newest of
    APPDATA/SnapRecomp/settings.cfg and the per-profile profiles/<id>/settings.cfg (the profile played last)."""
    base = os.path.join(os.environ.get('APPDATA', '.'), 'SnapRecomp')
    if os.environ.get('SNAP_SETTINGS'): cands = [os.environ['SNAP_SETTINGS']]
    else:
        cands = [os.path.join(base, 'settings.cfg')] + glob.glob(os.path.join(base, 'profiles', '*', 'settings.cfg'))
        cands = sorted((c for c in cands if os.path.exists(c)), key=os.path.getmtime, reverse=True)
    for p in cands:
        if p and os.path.exists(p):
            kv = {}
            with open(p, 'r', encoding='utf-8-sig', errors='replace') as fh:
                for line in fh:
                    line = line.rstrip('\r\n')
                    if '=' in line and not line.lstrip().startswith(('#', ';')):
                        k, v = line.split('=', 1); kv[k.strip()] = v.strip()
            return kv
    return {}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--seed'); ap.add_argument('--mode', choices=('within', 'all', 'off'))
    ap.add_argument('--out'); ap.add_argument('--data', default=os.environ.get('SNAP_DRP_ROOT', DATA))
    ap.add_argument('--pack', default=PACK); ap.add_argument('--courses', default=PLAYABLE)
    ap.add_argument('--if-stale', action='store_true', help='read mode and seed from settings.cfg (spawn_rand, spawn_seed); rebuild only when they changed')
    ap.add_argument('--report', action='store_true'); ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args(argv)
    if a.selftest: return selftest(a)
    seed, mode = a.seed, a.mode
    if a.if_stale:
        kv = read_settings()
        m = os.environ.get('SNAP_SPAWN_RAND') or kv.get('spawn_rand', 'off'); m = 'off' if m in ('', 'default', '0') else m   # the env var wins, like every settings key
        mode = mode or (m if m in ('within', 'all') else 'off')
        seed = seed if seed is not None else (os.environ.get('SNAP_SPAWN_SEED') or kv.get('spawn_seed', ''))
    if mode is None: ap.error('give --mode within|all|off (or --if-stale)')
    root = mods_root(a.out)
    if mode == 'off':
        pd = os.path.join(root, a.pack)
        if os.path.isdir(pd) and 'spawn_rand' in read_meta(root, a.pack):
            shutil.rmtree(pd)
            print('spawn randomizer: pack removed')
        if os.path.exists(os.path.join(root, 'mods.json')): set_listed(root, a.pack, False)
        print('spawn randomizer: off')
        return 0
    seed = '' if seed is None else seed
    if not seed:
        if a.if_stale: seed = 'snap'   # an empty seed field gives one fixed layout, so the pack is not rebuilt on every start
        else:
            seed = str(int.from_bytes(os.urandom(4), 'little') % 1000000)
            print('no seed given; using %s' % seed)
    if a.if_stale:
        meta = read_meta(root, a.pack).get('spawn_rand', {})
        if meta.get('mode') == mode and meta.get('seed') == seed and meta.get('version') == VERSION and os.path.isdir(os.path.join(root, a.pack)):
            set_listed(root, a.pack, True)
            print('spawn randomizer: up to date (%s, seed "%s")' % (mode, seed))
            return 0
    if not os.path.isdir(os.path.join(a.data, SUB)):
        print('spawn randomizer: game data not found at %s (unpack the DRP archives with scripts/drp_unpack.py, or pass --data)' % a.data, file=sys.stderr)
        return 2
    files, rep = generate(a.data, seed, mode, a.courses)
    if a.report:
        sys.stdout.write(fixed_text(rep, seed, mode)); return 0
    existing = os.path.join(root, a.pack)
    if os.path.isdir(existing) and 'spawn_rand' not in read_meta(root, a.pack):
        print('spawn randomizer: %s exists and was not made by this tool; use --pack to pick another folder' % existing, file=sys.stderr)
        return 2
    pd = write_pack(root, a.pack, seed, mode, files, rep)
    set_listed(root, a.pack, True)
    moved = sum(i['moved'] for i in rep); total = sum(i['spawns'] for i in rep)
    print('spawn randomizer: %s, seed "%s": %d of %d spawns changed in %d levels -> %s' % (mode, seed, moved, total, len(files), pd))
    return 0


# ---------------------------------------------------------------------------------------------------------------- self test
def selftest(a):
    ok = True
    def check(name, cond, detail=''):
        nonlocal ok
        print(('PASS ' if cond else 'FAIL ') + name + (': ' + str(detail) if detail != '' else ''))
        ok = ok and cond
    check('rng repeats', [Rng('x', 1).below(1000) for _ in range(2)][0] == Rng('x', 1).below(1000))
    data = a.data
    if not os.path.isdir(os.path.join(data, SUB)):
        print('no game data: skipping data checks'); return 0 if ok else 1
    f1, r1 = generate(data, 'seedA', 'within', r'LC_001_A')
    f1b, _ = generate(data, 'seedA', 'within', r'LC_001_A')
    f2, _ = generate(data, 'seedB', 'within', r'LC_001_A')
    check('same seed, same bytes', [x[2] for x in f1] == [x[2] for x in f1b])
    check('other seed, other bytes', [x[2] for x in f1] != [x[2] for x in f2])
    for arch, f, d in f1:
        with open(os.path.join(data, SUB, arch, f), 'rb') as fh: o = fh.read()
        diff = [i for i in range(len(o)) if o[i] != d[i]] if len(o) == len(d) else None
        check('%s same size' % f, diff is not None)
        if diff is not None:
            lv0 = Level(f[:-7], o); lv1 = Level(f[:-7], d)
            pos = {r.pos_hash for r in lv0.recs}
            check('%s only template hash words differ' % f, all((i & ~3) in pos for i in diff), '%d bytes' % len(diff))
            c0 = collections.Counter(r.hash for r in lv0.recs); c1 = collections.Counter(r.hash for r in lv1.recs)
            check('%s species multiset kept' % f, c0 == c1)
    fa, ra = generate(data, 'seedA', 'all', r'LC_001_A')
    for arch, f, d in fa:
        lv = Level(f[:-7], d)
        check('%s all-mode file parses, %d templates' % (f, len(lv.tmpl)), len(lv.tmpl) > 0)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
