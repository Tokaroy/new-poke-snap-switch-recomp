"""Transparent NTFS compression for HD texture packs (lossless; the game reads the files exactly as before).

Measured on this project's HD pack (docs/hd-pack-compression.md): LZNT1 (compact /C) saves 20 to 57 percent depending on the
BC format and reads as fast as uncompressed on NVMe, so it is the default. XPRESS16K and LZX save more but read 2 to 3 times
slower on one thread. Windows drops the /EXE (XPRESS/LZX) compression when a file is rewritten, and LZNT1 also resets on a
rewrite, so run this after a pack is built, not before.

  python scripts/hd/compress_pack.py <pack folder> [<pack folder> ...]           compress with LZNT1
  python scripts/hd/compress_pack.py <pack folder> --algo lzx                    smallest, slower reads
  python scripts/hd/compress_pack.py <pack folder> --status                      only print the sizes
  python scripts/hd/compress_pack.py <pack folder> --undo                        decompress (compact /U, both kinds)
"""
import argparse
import collections
import ctypes
import os
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

ALGOS = {'lznt1': [], 'xpress4k': ['/EXE:XPRESS4K'], 'xpress8k': ['/EXE:XPRESS8K'], 'xpress16k': ['/EXE:XPRESS16K'], 'lzx': ['/EXE:LZX']}
DEFAULT_ALGO = 'lznt1'
# nutexb image_format byte (footer offset 0x90 of the 0xb0 footer, that is 0x20 bytes before the end of the file)
FORMATS = {0x00: 'R8', 0x01: 'RGBA8', 0x05: 'RGBA8 sRGB', 0x34: 'RGBA32F', 0x80: 'BC1', 0x85: 'BC1 sRGB', 0x90: 'BC2', 0x95: 'BC2 sRGB',
           0xa0: 'BC3', 0xa5: 'BC3 sRGB', 0xb0: 'BC4', 0xb1: 'BC4 snorm', 0xc0: 'BC5', 0xc1: 'BC5 snorm', 0xd7: 'BC6H', 0xd8: 'BC6H signed',
           0xe0: 'BC7', 0xe5: 'BC7 sRGB'}
CHUNK = 100   # file names per compact call (keeps the command line short)


def disk_size(path):
    """Bytes the file takes on disk (the compressed size for NTFS or XPRESS/LZX compressed files)."""
    k = ctypes.WinDLL('kernel32', use_last_error=True)
    k.GetCompressedFileSizeW.restype = ctypes.c_uint32
    k.GetCompressedFileSizeW.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_uint32)]
    hi = ctypes.c_uint32(0)
    ctypes.set_last_error(0)
    lo = k.GetCompressedFileSizeW(path, ctypes.byref(hi))
    if lo == 0xFFFFFFFF and ctypes.get_last_error():
        raise OSError(ctypes.get_last_error(), path)
    return (hi.value << 32) | lo


def format_of(path, size):
    """Texture format name from the nutexb footer, or 'other' for a file that is not one."""
    if size < 0xb0:
        return 'other'
    try:
        with open(path, 'rb') as f:
            f.seek(size - 0x20)
            b = f.read(1)
        return FORMATS.get(b[0], '0x%02x' % b[0]) if b else 'other'
    except OSError:
        return 'other'


def find_files(roots, ext='.nutexb'):
    out = []
    for r in roots:
        for dp, _, fn in os.walk(r):
            out += [os.path.join(dp, x) for x in fn if x.lower().endswith(ext)]
    return sorted(out)


def totals(files):
    """{format: [count, logical bytes, bytes on disk]}, plus the sum under '(all)'."""
    t = collections.defaultdict(lambda: [0, 0, 0])
    for p in files:
        try:
            s = os.path.getsize(p)
            d = disk_size(p)
        except OSError:
            continue
        for key in (format_of(p, s), '(all)'):
            t[key][0] += 1
            t[key][1] += s
            t[key][2] += d
    return dict(t)


def gb(n):
    return '%.2f GB' % (n / 2**30)


def show(t, before=None, say=print):
    say('%-12s %7s %11s %11s %7s' % ('format', 'files', 'size', 'on disk', 'saved'))
    for k in sorted(t, key=lambda x: (x == '(all)', x)):
        n, lg, dk = t[k]
        say('%-12s %7d %11s %11s %6.1f%%' % (k, n, gb(lg), gb(dk), 100 * (1 - dk / lg) if lg else 0))
    if before and '(all)' in t and '(all)' in before:
        b, a = before['(all)'][2], t['(all)'][2]
        say('on disk %s -> %s (%s freed)' % (gb(b), gb(a), gb(b - a)))


def is_ntfs(path):
    k = ctypes.WinDLL('kernel32', use_last_error=True)
    name = ctypes.create_unicode_buffer(32)
    root = os.path.splitdrive(os.path.abspath(path))[0] + os.sep
    ok = k.GetVolumeInformationW(ctypes.c_wchar_p(root), None, 0, None, None, None, name, 32)
    return bool(ok) and name.value.upper() == 'NTFS'


def build_calls(files, algo=DEFAULT_ALGO, undo=False):
    """[(command, folder)] for compact, in batches of CHUNK file names per folder."""
    by_dir = collections.defaultdict(list)
    for p in files:
        by_dir[os.path.dirname(p)].append(os.path.basename(p))
    calls = []
    for d, names in by_dir.items():
        for i in range(0, len(names), CHUNK):
            part = names[i:i + CHUNK]
            if undo:   # plain /U clears NTFS compression, /U /EXE clears XPRESS/LZX; run both so any state ends uncompressed
                calls += [(['compact', '/U', '/I', '/Q'] + part, d), (['compact', '/U', '/EXE', '/I', '/Q'] + part, d)]
            else:
                calls.append((['compact', '/C'] + ALGOS[algo] + ['/I', '/Q'] + part, d))
    return calls


def apply(files, algo=DEFAULT_ALGO, undo=False, jobs=4, say=print):
    """Run compact over the files. Returns how many compact calls failed (a file in use, for example)."""
    calls = build_calls(files, algo, undo)
    done = [0]

    def one(c):
        rc = subprocess.run(c[0], cwd=c[1], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode
        done[0] += 1
        if done[0] % 50 == 0:
            say('  %d/%d batches' % (done[0], len(calls)))
        return rc
    with ThreadPoolExecutor(jobs) as ex:
        return sum(1 for rc in ex.map(one, calls) if rc != 0)


def run(roots, algo=DEFAULT_ALGO, undo=False, status=False, ext='.nutexb', jobs=4, say=print):
    files = find_files(roots, ext)
    if not files:
        say('No %s files under %s' % (ext, ', '.join(roots)))
        return 1
    bad = [r for r in roots if not is_ntfs(r)]
    if bad and not status:
        say('Not an NTFS drive (compact cannot compress here): ' + ', '.join(bad))
        return 1
    before = totals(files)
    say('%s: %d files' % ('Now' if status else 'Before', len(files)))
    show(before, say=say)
    if status:
        return 0
    t = time.time()
    say('%s ...' % ('Decompressing (compact /U)' if undo else 'Compressing with ' + algo))
    failed = apply(files, algo, undo, jobs, say)
    after = totals(files)
    say('After (%.0f s):' % (time.time() - t))
    show(after, before, say)
    if failed:
        say('%d compact batches reported an error (a file in use? run again later).' % failed)
        return 1
    return 0


def add_args(p):
    p.add_argument('roots', nargs='+', help='pack folder(s) to scan for texture files')
    p.add_argument('--algo', choices=sorted(ALGOS), default=DEFAULT_ALGO,
                   help='lznt1 (default: reads as fast as plain), xpress4k/8k/16k, lzx (smallest, slower reads)')
    p.add_argument('--undo', action='store_true', help='decompress (compact /U, NTFS and XPRESS/LZX)')
    p.add_argument('--status', action='store_true', help='only print sizes, change nothing')
    p.add_argument('--ext', default='.nutexb', help='file extension to touch (default .nutexb)')
    p.add_argument('--jobs', type=int, default=4, help='parallel compact calls (default 4)')


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    add_args(p)
    a = p.parse_args(argv)
    return run(a.roots, a.algo, a.undo, a.status, a.ext, a.jobs)


if __name__ == '__main__':
    sys.exit(main())
