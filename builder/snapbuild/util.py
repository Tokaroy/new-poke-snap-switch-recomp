"""Logging, child processes and resume stamps."""
import hashlib
import json
import os
import subprocess
import sys
import threading
import time
from pathlib import Path

BELOW_NORMAL = 0x4000
CREATE_NO_WINDOW = 0x08000000


class Log:
    """One log file per work folder (appended across runs); everything is echoed to the console too."""

    def __init__(self, path, quiet=False):
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.f = open(self.path, 'a', encoding='utf-8', buffering=1)
        self.quiet = quiet
        self.lock = threading.Lock()

    def write(self, text, echo=True):
        with self.lock:
            stamp = time.strftime('%H:%M:%S')
            for line in str(text).splitlines() or ['']:
                self.f.write(f'[{stamp}] {line}\n')
                if echo and not self.quiet:
                    print(line, flush=True)

    def raw(self, text):
        """Child output: log file only, no echo."""
        with self.lock:
            self.f.write(text if text.endswith('\n') else text + '\n')

    def close(self):
        self.f.close()


class StepError(Exception):
    """A step failed; the message is what the user sees (details are in the log)."""


def run(cmd, log, env=None, cwd=None, timeout=3600, what=None, check=True, echo_tail=0):
    """Run a child at BelowNormal priority with output to the log. Kills only its own process tree on timeout."""
    log.write(f'run: {what or Path(str(cmd[0])).name} ({len(cmd) - 1} args)')
    log.raw('  cmd: ' + ' '.join(str(c) for c in cmd))
    t0 = time.time()
    p = subprocess.Popen([str(c) for c in cmd], env=env, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         creationflags=BELOW_NORMAL | CREATE_NO_WINDOW)
    tail = []

    def pump():
        for line in iter(p.stdout.readline, b''):
            s = line.decode('utf-8', 'replace').rstrip()
            log.raw('  | ' + s)
            tail.append(s)
            del tail[:-40]
    th = threading.Thread(target=pump, daemon=True)
    th.start()
    try:
        p.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        subprocess.run(['taskkill', '/PID', str(p.pid), '/T', '/F'], capture_output=True)
        th.join(5)
        raise StepError(f'{what or cmd[0]} did not finish within {timeout // 60} minutes and was stopped (see the log)')
    th.join(10)
    log.raw(f'  exit {p.returncode} after {time.time() - t0:.1f}s')
    if check and p.returncode != 0:
        last = '\n'.join(tail[-echo_tail or -12:])
        raise StepError(f'{what or cmd[0]} failed with exit code {p.returncode}. Last output:\n{last}')
    return p.returncode, tail


def sha256_file(path, chunk=1 << 20):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(chunk), b''):
            h.update(b)
    return h.hexdigest()


def sha256_text(*parts):
    h = hashlib.sha256()
    for p in parts:
        h.update(str(p).encode('utf-8'))
        h.update(b'\0')
    return h.hexdigest()


def files_digest(paths):
    """Digest of the contents of several files (small ones: sources, scripts)."""
    h = hashlib.sha256()
    for p in sorted(Path(x) for x in paths):
        h.update(p.name.encode())
        h.update(sha256_file(p).encode())
    return h.hexdigest()


class Stamps:
    """Per-step resume stamps under <work>/stamps: a step is skipped when its key matches and its outputs still exist."""

    def __init__(self, work):
        self.dir = Path(work) / 'stamps'
        self.dir.mkdir(parents=True, exist_ok=True)

    def done(self, step, key, outputs=()):
        f = self.dir / f'{step}.json'
        if not f.exists():
            return False
        try:
            d = json.loads(f.read_text(encoding='utf-8'))
        except ValueError:
            return False
        return d.get('key') == key and all(Path(o).exists() for o in outputs)

    def write(self, step, key, seconds):
        (self.dir / f'{step}.json').write_text(json.dumps({'key': key, 'seconds': round(seconds, 1),
                                                           'finished': time.strftime('%Y-%m-%d %H:%M:%S')}), encoding='utf-8')

    def clear(self, step):
        (self.dir / f'{step}.json').unlink(missing_ok=True)


def cpu_count():
    return os.cpu_count() or 4


def total_ram_gb():
    import ctypes

    class MEM(ctypes.Structure):
        _fields_ = [('l', ctypes.c_ulong), ('load', ctypes.c_ulong), ('tp', ctypes.c_ulonglong), ('ap', ctypes.c_ulonglong),
                    ('tv', ctypes.c_ulonglong), ('av', ctypes.c_ulonglong), ('tvv', ctypes.c_ulonglong), ('avv', ctypes.c_ulonglong),
                    ('ae', ctypes.c_ulonglong)]
    m = MEM()
    m.l = ctypes.sizeof(m)
    ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
    return m.tp / 2**30, m.ap / 2**30
