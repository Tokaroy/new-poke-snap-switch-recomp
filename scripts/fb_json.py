"""Schemaless FlatBuffers <-> JSON converter with a byte-identical writer.  Generic: no game data inside.

A set of FlatBuffers files of one kind is analysed together with fb_walk (tables, vectors, strings, scalar sizes are
inferred, nothing is assumed). The result is a small structure sidecar (<ext>.structure.json: per table class the slot
kind/size/alignment/format, optional human names and the field placement order) plus one JSON file per input file.
The JSON can be edited (change values, add or remove vector elements and fields) and rebuilt with the sidecar.

  python -I fb_json.py dump  <ext> --root DIR --out OUT [--limit N] [--no-write] [--names]
        analyse every *.<ext> under DIR, write OUT/<ext>.structure.json and OUT/<ext>/<relative path>.json,
        and rebuild every file from its JSON to prove byte identity (prints identical/total).
  python -I fb_json.py build <file.json> --structure <ext>.structure.json -o out.bin

JSON model
  table   = {"<name or sN>": value, ...}  only present slots appear. Optional layout hints: "_vsize", "_tsize", "_fo".
  scalar  = int (unsigned), float for f32 slots, list of numbers for 8/12/16-byte structs, hex string otherwise
  string  = str ; vector = list ; {"_ref": n} = a table/string/vector shared with an earlier object (n = visit index)
  file    = {"_ext", "_ident", "_root", and only when the original layout is not the canonical one:
             "_layout" (creation order as [start, count] runs of canonical indices), "_dead" (unreferenced
             objects kept by the original writer), "_lead", "_minalign", "_vt"}
Canonical creation order = depth-first post-order, slots ascending, first object created lies at the highest address.
"""
import os, sys, json, struct, collections, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fb_walk as fw

try:
    import fb_names as fn
except Exception:
    fn = None


# ------------------------------------------------------------------ value formats
def f32_repr(raw):
    for p in range(6, 10):
        s = '%.*g' % (p, struct.unpack('<f', raw)[0])
        if struct.pack('<f', float(s)) == raw:
            return float(s)
    return struct.unpack('<f', raw)[0]


def plausible_float_raw(vals, allow_zero=True):
    nz = 0
    for v in vals:
        u = int.from_bytes(v, 'little')
        if u == 0:
            continue
        f = struct.unpack('<f', v)[0]
        if not (1e-6 < abs(f) < 1e7) or u < 0x00800000:
            return False
        nz += 1
    return nz > 0


def lane_fmt(vals, sz):
    """vals: list of raw byte strings of size sz (a scalar or struct). Returns the fmt name."""
    if sz in (1, 2):
        return 'u%d' % (8 * sz)
    if sz % 4:
        return 'hex'
    lanes = [v[i:i + 4] for v in vals for i in range(0, sz, 4)]
    isf = plausible_float_raw(lanes)
    if sz == 4:
        return 'f32' if isf else 'u32'
    if sz in (8, 12, 16) and isf:
        return 'lanes_f32'
    if sz == 8:
        return 'u64'
    return 'lanes_u32'


def enc_val(fmt, raw):
    if fmt in ('u8', 'u16', 'u32', 'u64'):
        return int.from_bytes(raw, 'little')
    if fmt == 'f32':
        return f32_repr(raw)
    if fmt == 'lanes_f32':
        return [f32_repr(raw[i:i + 4]) for i in range(0, len(raw), 4)]
    if fmt == 'lanes_u32':
        return [int.from_bytes(raw[i:i + 4], 'little') for i in range(0, len(raw), 4)]
    return raw.hex()


def dec_val(fmt, v, size):
    if fmt in ('u8', 'u16', 'u32', 'u64'):
        return (int(v) & ((1 << (8 * size)) - 1)).to_bytes(size, 'little')
    if fmt == 'f32':
        return struct.pack('<f', float(v))
    if fmt == 'lanes_f32':
        return b''.join(struct.pack('<f', float(x)) for x in v)
    if fmt == 'lanes_u32':
        return b''.join((int(x) & 0xFFFFFFFF).to_bytes(4, 'little') for x in v)
    return bytes.fromhex(v)


# ------------------------------------------------------------------ builder (flatc-style, back to front)
class Builder:
    def __init__(self, lead=0):
        self.parts = []
        self.off = 0
        self.minalign = 1
        self.vtables = []
        if lead:
            self.put(b'\0' * lead)

    def put(self, data):
        self.parts.append(bytearray(data))
        self.off += len(data)

    def prep(self, size, additional=0):
        if size > self.minalign:
            self.minalign = size
        pad = (-(self.off + additional)) & (size - 1)
        if pad:
            self.put(b'\0' * pad)

    def uoffset(self, child):
        self.prep(4)
        self.put(struct.pack('<I', self.off + 4 - child))

    def table(self, fields, vsize, tsize, vt_policy):
        """fields: list of (slot, raw bytes | ('ref', created), align) in placement order (first placed first)."""
        start = self.off
        pos = {}
        for slot, val, align in fields:
            if isinstance(val, tuple):
                self.uoffset(val[1])
            else:
                self.prep(align)
                self.put(val)
            pos[slot] = self.off
        self.prep(4)
        self.put(b'\0\0\0\0')
        tab = self.parts[-1]
        obj = self.off
        if tsize is None:
            tsize = obj - start
        nslots = (vsize - 4) // 2
        vt = struct.pack('<HH', vsize, tsize) + b''.join(
            struct.pack('<H', (obj - pos[s]) if s in pos else 0) for s in range(nslots))
        cands = self.vtables if vt_policy == 'first' else list(reversed(self.vtables))
        found = next((vo for vo, vb in cands if vb == vt), None)
        if found is None:
            self.prep(2)
            self.put(vt)
            found = self.off
            self.vtables.append((found, vt))
        tab[:] = struct.pack('<i', found - obj)
        self.last_tsize = tsize
        return obj

    def string(self, raw):
        self.prep(4, len(raw) + 1)
        self.put(b'\0')
        self.put(raw)
        self.put(struct.pack('<I', len(raw)))
        return self.off

    def vec_scalar(self, data, n, align):
        self.prep(4, len(data))
        self.prep(align, len(data))
        self.put(data)
        self.put(struct.pack('<I', n))
        return self.off

    def vec_offsets(self, children):
        self.prep(4, 4 * len(children))
        for c in reversed(children):
            self.uoffset(c)
        self.put(struct.pack('<I', len(children)))
        return self.off

    def raw(self, data):
        self.put(data)
        return self.off

    def finish(self, root, ident):
        self.prep(self.minalign, 8 if ident else 4)
        if ident:
            self.put(ident)
        self.uoffset(root)
        return b''.join(bytes(p) for p in reversed(self.parts))


# ------------------------------------------------------------------ structure sidecar
def build_structure(ext, w, graphs, use_names=True):
    names = (fn.NAMES.get(ext, {}) if (fn and use_names) else {})
    classes = {}
    for key, cls in w.classes.items():
        tname, slotnames = names.get(key, (None, {}))
        # placement order: pairwise majority of (descending offset) orders over the distinct vtable layouts
        wins = collections.Counter()
        seen = {}
        for fi, t in cls.inst:
            b = w.bufs[fi]
            r = fw.read_table(b, t)
            if r is None:
                continue
            sig = tuple(sorted(r[3].items()))
            seen[sig] = seen.get(sig, 0) + 1
        for sig, cnt in seen.items():
            order = [s for s, o in sorted(sig, key=lambda x: -x[1])]
            for i, a in enumerate(order):
                wins[a] += cnt * (len(order) - 1 - i)
        slots = {}
        for s, info in sorted(cls.slots.items()):
            d = {'kind': info['kind'], 'name': (slotnames.get(s) or ('s%d' % s,))[0]}
            if info['kind'] == 'scalar':
                d['size'] = info['size']
                d['align'] = info.get('align', 4)
                vals = []
                for fi, t in cls.inst[:20000]:
                    b = w.bufs[fi]
                    r = fw.read_table(b, t)
                    if r and s in r[3]:
                        p = t + r[3][s]
                        vals.append(bytes(b[p:p + info['size']]))
                d['fmt'] = lane_fmt(vals, info['size'])
            elif info['kind'] == 'vec':
                ek = info.get('elem')
                if ek == 'table' or ek == 'string':
                    d['elem'] = ek
                else:
                    es = ek['scalar']
                    d['elem'] = es
                    vals = []
                    for fi, t in cls.inst[:3000]:
                        b = w.bufs[fi]
                        r = fw.read_table(b, t)
                        if r and s in r[3]:
                            v = fw.fwd(b, t + r[3][s])
                            n = fw.U32(b, v)[0]
                            vals += [bytes(b[v + 4 + es * i: v + 4 + es * (i + 1)]) for i in range(min(n, 50))]
                    d['fmt'] = lane_fmt(vals, es) if es in (1, 2, 4, 8, 12, 16) else 'hex'
            if any(x['name'] == d['name'] for x in slots.values()):
                d['name'] += '_%d' % s
            slots[str(s)] = d
        order = sorted(cls.slots, key=lambda s: (-wins[s], s))
        classes[key] = {'name': tname, 'slots': slots, 'order': order}
    return {'ext': ext, 'classes': classes}


# ------------------------------------------------------------------ dump (graph -> tree)
class Dumper:
    def __init__(self, st, w, fi, g):
        self.st, self.w, self.fi, self.g = st, w, fi, g
        self.b = w.bufs[fi]
        self.idx = {}          # addr -> canonical index
        self.creation = []     # canonical indices of addresses in canonical order (addr list)
        self.shared = 0
        self.n = 0
        self.strdup = False
        self.shared_slots = set()
        self.straddr = {}

    def key(self, cls, slot):
        return cls['slots'][str(slot)]['name']

    def visit(self, addr, clsname):
        """Return JSON value for the object at addr; assigns canonical indices in post-order."""
        o = self.g.objs[addr]
        k = o['kind']
        b = self.b
        if addr in self.idx:
            if k == 'S':          # equal strings written once (shared string): inline, writer re-dedups by content
                self.strdup = True
                self.shared_slots.add(clsname)
                self.shared_slots.add(self.straddr[addr])
                self.n += 1
                return fw.read_string(b, addr)
            self.shared += 1
            return {'_ref': self.idx[addr]}
        if k == 'T':
            cls = self.st['classes'][o['cls']]
            vt, vsize, tsize, slots = fw.read_table(b, addr)
            d = {}
            for sl in sorted(slots):
                kd, off, extra = o['fields'][sl]
                info = cls['slots'][str(sl)]
                name = info['name']
                if kd == 'scalar':
                    d[name] = enc_val(info['fmt'], bytes(b[addr + off: addr + off + info['size']]))
                elif kd == 'table':
                    d[name] = self.visit(extra, o['cls'] + '.' + str(sl))
                elif kd == 'string':
                    d[name] = self.visit(extra, o['cls'] + '.' + str(sl))
                else:
                    d[name] = self.visit(extra, o['cls'] + '.' + str(sl))
            nsl = max(slots) + 1 if slots else 0
            if vsize != 4 + 2 * nsl:
                d['_vsize'] = vsize
            order = [s for s in cls['order'] if s in slots]
            actual = [s for s, _ in sorted(slots.items(), key=lambda x: -x[1])]
            if actual != order:
                d['_fo'] = actual
            d['_tsize'] = tsize  # removed later if equal to the computed one
            self.idx[addr] = self.n; self.n += 1
            return d
        if k == 'S':
            v = fw.read_string(b, addr)
            self.straddr[addr] = clsname
            self.idx[addr] = self.n; self.n += 1
            return v
        if k == 'VS':
            out = [self.visit(fw.fwd(b, addr + 4 + 4 * i), clsname + '#s') for i in range(o['n'])]
            self.idx[addr] = self.n; self.n += 1
            return out
        if k == 'VT':
            out = [self.visit(fw.fwd(b, addr + 4 + 4 * i), o['cls'] + '[]') for i in range(o['n'])]
            self.idx[addr] = self.n; self.n += 1
            return out
        if k == 'VX':
            n, es = o['n'], o['es']
            vals = [bytes(b[addr + 4 + es * i: addr + 4 + es * (i + 1)]) for i in range(n)]
            parent_fmt = self.vec_fmt(o['cls'])
            self.idx[addr] = self.n; self.n += 1
            return [enc_val(parent_fmt, v) for v in vals]
        raise ValueError(k)

    def vec_fmt(self, clskey):
        parent, slot = clskey.rsplit('.', 1)
        return self.st['classes'][parent]['slots'][slot].get('fmt', 'hex')


def dump_file(st, w, fi, g):
    """Return (json dict, Dumper) for file fi. Layout hints are added by the caller after verification."""
    d = Dumper(st, w, fi, g)
    root = d.visit(g.root, 'R')
    # drop _tsize hints that the builder reproduces on its own (resolved in finalize by trial build)
    doc = {'_ext': st['ext'], '_ident': g.ident.decode() if g.ident else None, '_root': root}
    if d.shared_slots:
        doc['_sharedslots'] = sorted(d.shared_slots)
    return doc, d


def finalize_hints(st, w, fi, g, doc, d):
    """Add layout hints until build(doc) == original bytes. Returns (ok, hints used)."""
    b = w.bufs[fi]
    # canonical index for every live address; dead objects follow in descending address order
    live = dict(d.idx)
    dead_addrs = sorted([a for a in g.objs if a not in live], reverse=True)
    dead = []
    for a in dead_addrs:
        o = g.objs[a]
        if o['kind'] == 'S':
            dead.append({'k': 'S', 'v': fw.read_string(b, a)})
        else:
            sz = o['size']
            dead.append({'k': 'RAW', 'hex': bytes(b[a:a + sz]).hex()} if not o.get('zero') else {'k': 'RAW', 'zero': sz})
        live[a] = d.n + len(dead) - 1
    if dead:
        doc['_dead'] = dead
    creation = sorted(g.objs, reverse=True)
    seq = [live[a] for a in creation]
    # canonical order is 0..n-1 for live then dead; compare
    if seq != list(range(len(seq))):
        runs = []
        for v in seq:
            if runs and runs[-1][0] + runs[-1][1] == v:
                runs[-1][1] += 1
            else:
                runs.append([v, 1])
        doc['_layout'] = runs
    # try builder hint combinations (tsize hints are stripped where identical)
    strip_tsize(doc, st)
    for lead, ma8, pol in [(0, False, 'first'), (0, True, 'first'), (0, False, 'last'), (0, True, 'last')] + \
            [(x, m, 'first') for x in (4, 8, 12, 16) for m in (False, True)]:
        doc['_lead'], doc['_minalign'], doc['_vt'] = lead, (8 if ma8 else 0), pol
        try:
            out = build(doc, st)
        except Exception:
            out = None
        if out == b:
            for k, dv in (('_lead', 0), ('_minalign', 0), ('_vt', 'first')):
                if doc[k] == dv:
                    del doc[k]
            return True
    for k in ('_lead', '_minalign', '_vt'):
        doc.pop(k, None)
    return False


def strip_tsize(doc, st):
    """Remove each table's _tsize when the builder's computed size matches (decided by a full trial build per table).
    The trial is done once for the whole file: build with no hints, collect computed sizes, compare."""
    sizes = {}
    build(doc, st, record_tsize=sizes)
    def walk(v):
        if isinstance(v, dict):
            if '_ref' in v:
                return
            if '_tsize' in v and sizes.get(id(v)) == v['_tsize']:
                del v['_tsize']
            for x in list(v.values()):
                walk(x)
        elif isinstance(v, list):
            for x in v:
                walk(x)
    walk(doc['_root'])


# ------------------------------------------------------------------ build (tree -> bytes)
def build(doc, st, record_tsize=None):
    classes = st['classes']
    nodes = []   # (kind, payload)
    # canonical pass: post-order
    def visit_table(d, key):
        cls = classes[key]
        byname = {s['name']: (int(sl), s) for sl, s in cls['slots'].items()}
        fields = {}
        for name, val in d.items():
            if name.startswith('_'):
                continue
            sl, info = byname[name]
            fields[sl] = (info, val)
        refs = {}
        for sl in sorted(fields):
            info, val = fields[sl]
            kind = info['kind']
            if kind == 'table':
                refs[sl] = node_ref(val, lambda v, k=key + '.' + str(sl): visit_table(v, k))
            elif kind == 'string':
                refs[sl] = node_ref(val, lambda v, lb=key + '.' + str(sl): add(('S', v.encode('utf-8')), lb))
            elif kind == 'vec':
                refs[sl] = node_ref(val, lambda v, i=info, k=key + '.' + str(sl): visit_vec(v, i, k))
        i = len(nodes)
        nodes.append(('T', (d, key, fields, refs)))
        return i

    strmap = {}
    shared = set(doc.get('_sharedslots', []))

    def add(n, label=None):
        if n[0] == 'S' and label in shared:
            if n[1] in strmap:
                nodes.append(('DUP', strmap[n[1]]))
                return len(nodes) - 1
            strmap[n[1]] = len(nodes)
        nodes.append(n)
        return len(nodes) - 1

    def node_ref(val, fn_):
        if isinstance(val, dict) and '_ref' in val:
            return val['_ref']
        return fn_(val)

    def visit_vec(v, info, key):
        e = info['elem']
        if e == 'table':
            ch = [node_ref(x, lambda y, k=key + '[]': visit_table(y, k)) for x in v]
            return add(('VT', ch))
        if e == 'string':
            ch = [node_ref(x, lambda y: add(('S', y.encode('utf-8')), key + '#s')) for x in v]
            return add(('VS', ch))
        fmt = info.get('fmt', 'hex')
        data = b''.join(dec_val(fmt, x, e) for x in v)
        return add(('VX', (data, len(v), e)))

    root = visit_table(doc['_root'], 'R')
    def cr(c):
        while nodes[c][0] == 'DUP':
            c = nodes[c][1]
        return created[c]
    nlive = len(nodes)
    for dd in doc.get('_dead', []):
        if dd['k'] == 'S':
            nodes.append(('S', dd['v'].encode('utf-8')))
        elif 'zero' in dd:
            nodes.append(('RAW', b'\0' * dd['zero']))
        else:
            nodes.append(('RAW', bytes.fromhex(dd['hex'])))
    # fix up string nodes made as str by visit(); normalise
    order = [i for i in range(len(nodes)) if nodes[i][0] != 'DUP']
    lay = doc.get('_layout')
    if lay:
        seq = [x for s, c in lay for x in range(s, s + c)]
        if sorted(seq) == order:
            pos = {x: i for i, x in enumerate(seq)}
            def kids(n):
                if n[0] == 'T':
                    return list(n[1][3].values())
                return n[1] if n[0] in ('VT', 'VS') else []
            def root_of(c):
                while nodes[c][0] == 'DUP':
                    c = nodes[c][1]
                return c
            if all(pos[root_of(c)] < pos[i] for i in seq for c in kids(nodes[i])):
                order = seq      # a stale layout (the JSON was edited structurally) falls back to the canonical order
    bl = Builder(doc.get('_lead', 0))
    if doc.get('_minalign'):
        bl.minalign = doc['_minalign']
    pol = doc.get('_vt', 'first')
    created = {}
    for i in order:
        kind, p = nodes[i]
        if kind == 'DUP':
            continue
        if kind == 'S':
            created[i] = bl.string(p)
        elif kind == 'VT' or kind == 'VS':
            created[i] = bl.vec_offsets([cr(c) for c in p])
        elif kind == 'VX':
            data, n, es = p
            created[i] = bl.vec_scalar(data, n, es if es in (1, 2, 4, 8) else 4)
        elif kind == 'RAW':
            created[i] = bl.raw(p)
        else:
            d, key, fields, refs = p
            cls = classes[key]
            flds = []
            pres = list(fields)
            fo = [s for s in d.get('_fo', []) if s in fields]
            place = fo + [s for s in cls['order'] if s in fields and s not in fo] if fo else [s for s in cls['order'] if s in fields]
            for sl in place:
                info, val = fields[sl]
                if info['kind'] == 'scalar':
                    flds.append((sl, dec_val(info['fmt'], val, info['size']), info.get('align', 4)))
                else:
                    flds.append((sl, ('ref', cr(refs[sl])), 4))
            vsize = d.get('_vsize', 4 + 2 * ((max(pres) + 1) if pres else 0))
            created[i] = bl.table(flds, vsize, None if record_tsize is not None else d.get('_tsize'), pol)
            if record_tsize is not None:
                record_tsize[id(d)] = bl.last_tsize
    ident = doc['_ident'].encode() if doc.get('_ident') else None
    return bl.finish(created[root], ident)


# ------------------------------------------------------------------ driver
def analyse(ext, root, limit=None):
    files = fw.load_files(root, ext)
    if limit and len(files) > limit:
        st_ = len(files) / limit
        files = [files[int(i * st_)] for i in range(limit)]
    bufs = [open(f, 'rb').read() for f in files]
    w, graphs = fw.analyse_all(bufs)
    return files, bufs, w, graphs


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        return
    def opt(n, d=None):
        return a[a.index(n) + 1] if n in a else d
    cmd = a[0]
    if cmd == 'build':
        doc = json.load(open(a[1], encoding='utf-8'))
        st = json.load(open(opt('--structure'), encoding='utf-8'))
        open(opt('-o'), 'wb').write(build(doc, st))
        return
    if cmd == 'dump':
        ext = a[1]
        root = opt('--root', 'extracted/drp-v262144')
        out = opt('--out', 'out')
        limit = int(opt('--limit')) if opt('--limit') else None
        files, bufs, w, graphs = analyse(ext, root, limit)
        st = build_structure(ext, w, graphs, '--names' in a or True)
        if '--no-write' not in a:
            os.makedirs(out, exist_ok=True)
            json.dump(st, open(os.path.join(out, ext + '.structure.json'), 'w', encoding='utf-8'), indent=1)
        ok = 0
        bad = []
        nshared = 0
        for fi, g in enumerate(graphs):
            try:
                doc, d = dump_file(st, w, fi, g)
                good = finalize_hints(st, w, fi, g, doc, d)
            except Exception as ex:   # e.g. one table reached through two class paths (overlapping objects)
                print('  error', files[fi], type(ex).__name__, ex)
                bad.append(files[fi])
                continue
            nshared += d.shared
            if good:
                # the JSON text itself must survive a dump/parse cycle
                text = json.dumps(doc, ensure_ascii=False)
                good = build(json.loads(text), st) == bufs[fi]
            if good:
                ok += 1
                if '--no-write' not in a:
                    rel = os.path.relpath(files[fi], root).replace('\\', '/')
                    p = os.path.join(out, ext, rel + '.json')
                    os.makedirs(os.path.dirname(p), exist_ok=True)
                    open(p, 'w', encoding='utf-8').write(text)
            else:
                bad.append(files[fi])
        print('%s files %d roundtrip-identical %d shared-refs %s' % (ext, len(files), ok, nshared))
        for x in bad[:5]:
            print('  FAIL', x)
        return
    print(__doc__)


if __name__ == '__main__':
    main()
