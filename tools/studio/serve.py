"""Studio review server. Python stdlib only; binds 127.0.0.1 only.

Serves tools/studio/ (the page), build/studio/assets/ (exported models and maps) and build/studio/manifest.json,
build/studio/decisions.json. POST /api/decision appends one decision to build/studio/decisions.json.

  python -E -P tools/studio/serve.py [--port 8791]

Decisions file: a JSON list of {species, variant, verdict, comment, timestamp}; the latest entry per (species, variant) wins.
verdict is one of: approve, reject, needs-work.
"""
import argparse, json, mimetypes, os, sys, threading
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlsplit

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
STUDIO = ROOT / 'build' / 'studio'
ASSETS = STUDIO / 'assets'
DECISIONS = STUDIO / 'decisions.json'
VERDICTS = {'approve', 'reject', 'needs-work'}
MAX_BODY = 64 * 1024
LOCK = threading.Lock()
PORT = 8791

mimetypes.add_type('model/gltf+json', '.gltf')
mimetypes.add_type('application/octet-stream', '.bin')
mimetypes.add_type('text/javascript', '.js')
mimetypes.add_type('text/javascript', '.mjs')


def read_decisions():
    try:
        d = json.loads(DECISIONS.read_text(encoding='utf-8'))
        return d if isinstance(d, list) else []
    except FileNotFoundError:
        return []
    except Exception:
        return []


def safe_join(base, rel):
    p = (base / rel).resolve()
    try:
        p.relative_to(base.resolve())
    except ValueError:
        return None
    return p if p.is_file() else None


class Srv(ThreadingHTTPServer):
    allow_reuse_address = False   # on Windows SO_REUSEADDR would let a second process share the port


class H(BaseHTTPRequestHandler):
    server_version = 'studio/1'

    def log_message(self, fmt, *a):
        sys.stderr.write('[studio] ' + (fmt % a) + '\n')

    def host_ok(self):
        host = (self.headers.get('Host') or '').lower()
        if host not in ('127.0.0.1:%d' % PORT, 'localhost:%d' % PORT):
            return False
        org = self.headers.get('Origin')
        if org and org.lower() not in ('http://127.0.0.1:%d' % PORT, 'http://localhost:%d' % PORT):
            return False
        return True

    def send(self, code, body=b'', ctype='text/plain; charset=utf-8', extra=None):
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-cache')
        self.send_header('X-Content-Type-Options', 'nosniff')
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if self.command != 'HEAD':
            self.wfile.write(body)

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        if not self.host_ok():
            return self.send(403, b'forbidden host')
        path = unquote(urlsplit(self.path).path)
        if path in ('/', ''):
            path = '/index.html'
        f = None
        if path == '/manifest.json':
            f = STUDIO / 'manifest.json'
            if not f.is_file():
                return self.send(200, b'{"version":1,"species":{},"candidates":[]}', 'application/json')
        elif path == '/decisions.json':
            return self.send(200, json.dumps(read_decisions()).encode(), 'application/json')
        elif path.startswith('/assets/'):
            f = safe_join(ASSETS, path[len('/assets/'):])
        else:
            f = safe_join(HERE, path.lstrip('/'))
        if f is None or not f.is_file():
            return self.send(404, b'not found')
        ctype = mimetypes.guess_type(str(f))[0] or 'application/octet-stream'
        if ctype.startswith('text/') or ctype.endswith('json'):
            ctype += '; charset=utf-8'
        data = f.read_bytes()
        self.send(200, data, ctype)

    def do_POST(self):
        if not self.host_ok():
            return self.send(403, b'forbidden host')
        if urlsplit(self.path).path != '/api/decision':
            return self.send(404, b'not found')
        try:
            n = int(self.headers.get('Content-Length') or 0)
        except ValueError:
            n = -1
        if n <= 0 or n > MAX_BODY:
            return self.send(413, b'bad length')
        try:
            d = json.loads(self.rfile.read(n).decode('utf-8'))
            species, variant = str(d['species']), str(d['variant'])
            verdict = str(d['verdict'])
            comment = str(d.get('comment', ''))[:4000]
        except Exception:
            return self.send(400, b'bad json')
        if verdict not in VERDICTS or not species or not variant or len(species) > 100 or len(variant) > 100:
            return self.send(400, b'bad fields')
        rec = {'species': species, 'variant': variant, 'verdict': verdict, 'comment': comment,
               'timestamp': datetime.now().astimezone().isoformat(timespec='seconds')}
        with LOCK:
            data = read_decisions()
            data.append(rec)
            STUDIO.mkdir(parents=True, exist_ok=True)
            tmp = DECISIONS.with_suffix('.tmp')
            tmp.write_text(json.dumps(data, indent=1), encoding='utf-8')
            os.replace(tmp, DECISIONS)
        self.send(200, json.dumps({'ok': True, 'decision': rec}).encode(), 'application/json')


def main():
    global PORT
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, default=8791)
    a = ap.parse_args()
    PORT = a.port
    srv = Srv(('127.0.0.1', PORT), H)
    print('[studio] http://127.0.0.1:%d  (assets: %s)' % (PORT, ASSETS), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
