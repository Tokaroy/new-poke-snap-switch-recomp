"""Reading the file tables of the user's own dump files: NSP (PFS0) and XCI (a game card image made of nested HFS0 partitions).

Only the tables are parsed here (a few KB). Members are copied out by offset, so a 7.5 GB game card image is never loaded into memory
and nothing is decrypted by this module: decrypting (with the user's own keys) is NSTool's job, see gamesrc.py.
"""
import os
import struct
from pathlib import Path

CHUNK = 8 * 1024 * 1024


class ContainerError(Exception):
    """A dump file that cannot be read as an NSP or XCI. The message is plain text for the user."""


def _read(f, n, what):
    b = f.read(n)
    if len(b) != n:
        raise ContainerError(f'{what} is cut short (the file is damaged or only partly copied)')
    return b


def _cstr(strings, off):
    end = strings.find(b'\0', off)
    if end < 0:
        raise ContainerError('a file name inside the dump is not terminated')
    return strings[off:end].decode('utf-8', 'replace')


def _parse_table(f, base, magic, entry_size, unpack):
    """Shared PFS0/HFS0 table reader. Returns [{name, offset (absolute), size}] and the end of the table."""
    f.seek(base)
    head = _read(f, 16, 'the file table')
    m, count, strsize, _ = struct.unpack('<4sIII', head)
    if m != magic:
        raise ContainerError(f'expected a {magic.decode()} file table, found {m!r}')
    if count > 100000 or strsize > 64 * 1024 * 1024:
        raise ContainerError('the file table of this dump is not plausible (damaged file?)')
    ents = _read(f, count * entry_size, 'the file table')
    strings = _read(f, strsize, 'the file table')
    data0 = base + 16 + count * entry_size + strsize
    out = []
    for i in range(count):
        off, size, name_off = unpack(ents, i * entry_size)
        out.append({'name': _cstr(strings, name_off), 'offset': data0 + off, 'size': size})
    return out


def pfs0_members(path):
    """Members of an NSP (PFS0)."""
    with open(path, 'rb') as f:
        return _parse_table(f, 0, b'PFS0', 0x18, lambda b, o: struct.unpack_from('<QQI', b, o))


def hfs0_members(f, base):
    return _parse_table(f, base, b'HFS0', 0x40, lambda b, o: struct.unpack_from('<QQI', b, o))


def xci_partitions(path):
    """{'secure': [members], 'normal': [...], ...}. Members of the nested HFS0 partitions, offsets absolute in the file."""
    with open(path, 'rb') as f:
        f.seek(0x100)
        if _read(f, 4, 'the game card header') != b'HEAD':
            raise ContainerError('this is not a game card image (XCI): the header is missing')
        f.seek(0x130)   # PartitionFsHeaderAddress (the size follows at 0x138)
        root = struct.unpack('<Q', _read(f, 8, 'the game card header'))[0]
        parts = {}
        for p in hfs0_members(f, root):
            parts[p['name']] = hfs0_members(f, p['offset'])
        return parts


def members(path):
    """All members of the dump file as {name: (offset, size, partition)}. For an XCI only the secure and normal partitions count
    (the update partition of a game card holds the console firmware, which this project never touches)."""
    path = Path(path)
    ext = path.suffix.lower()
    if ext == '.nsp':
        return {m['name']: (m['offset'], m['size'], '') for m in pfs0_members(path)}
    if ext == '.xci':
        out = {}
        parts = xci_partitions(path)
        for pn in ('normal', 'secure'):
            for m in parts.get(pn, []):
                out[m['name']] = (m['offset'], m['size'], pn)
        return out
    raise ContainerError(f'{path.name}: only .nsp and .xci files can be read (compressed .nsz/.xcz files must be unpacked first)')


def copy_member(path, offset, size, dest, progress=None, check_free=True):
    """Copy [offset, offset+size) of a dump into dest (never writes next to the source). Atomic: dest appears only when complete."""
    dest = Path(dest)
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_name(dest.name + '.part')
    if check_free:
        import shutil
        free = shutil.disk_usage(dest.parent).free
        if free < size + (256 << 20):
            raise ContainerError(f'not enough free space in {dest.parent} to copy {size / 2**30:.1f} GB (only {free / 2**30:.1f} GB free)')
    done = 0
    try:
        with open(path, 'rb') as src, open(tmp, 'wb') as out:
            src.seek(offset)
            while done < size:
                b = src.read(min(CHUNK, size - done))
                if not b:
                    raise ContainerError(f'{Path(path).name} is cut short (the copy of this dump is incomplete)')
                out.write(b)
                done += len(b)
                if progress:
                    progress(done, size)
        os.replace(tmp, dest)
    finally:
        try:
            tmp.unlink()
        except OSError:
            pass
    return dest
