"""NSO0 header parsing and the decompressed-image hash (standard library only; uses lz4 when it is installed)."""
import hashlib
import os
import struct

MAGIC = b'NSO0'
HEADER_SIZE = 0x100
SEGMENT_NAMES = ('text', 'rodata', 'data')


class NsoError(Exception):
    """Carries a machine-readable code and a message for the user."""

    def __init__(self, code, message):
        super().__init__(message)
        self.code = code
        self.message = message


def lz4_block_decompress(src, usize):
    """Plain LZ4 block decoder (fallback when the lz4 module is missing)."""
    out = bytearray()
    i, n = 0, len(src)
    while i < n:
        token = src[i]
        i += 1
        lit = token >> 4
        if lit == 15:
            while True:
                b = src[i]
                i += 1
                lit += b
                if b != 255:
                    break
        out += src[i:i + lit]
        i += lit
        if i >= n:
            break
        off = src[i] | (src[i + 1] << 8)
        i += 2
        if off == 0 or off > len(out):
            raise ValueError('bad LZ4 offset')
        ml = token & 15
        if ml == 15:
            while True:
                b = src[i]
                i += 1
                ml += b
                if b != 255:
                    break
        ml += 4
        start = len(out) - off
        if off >= ml:
            out += out[start:start + ml]
        else:
            for k in range(ml):
                out.append(out[start + k])
    if len(out) != usize:
        raise ValueError('LZ4 size mismatch')
    return bytes(out)


def lz4_decompress(src, usize):
    try:
        import lz4.block
        return lz4.block.decompress(src, uncompressed_size=usize)
    except ImportError:
        return lz4_block_decompress(src, usize)


class Nso:
    """Parsed header of one NSO file."""

    def __init__(self, path, data):
        self.path = path
        self.flags = struct.unpack_from('<I', data, 0xC)[0]
        self.segments = []   # (file_off, mem_off, size, comp_size)
        for i in range(3):
            fo, mo, sz = struct.unpack_from('<3I', data, 0x10 + 0x10 * i)
            self.segments.append((fo, mo, sz, struct.unpack_from('<I', data, 0x60 + 4 * i)[0]))
        self.build_id = data[0x40:0x60][:20].hex().upper()
        self.seg_hashes = [bytes(data[0xA0 + 32 * i:0xA0 + 32 * (i + 1)]) for i in range(3)]


def read_header(path):
    """Open path and parse the NSO0 header. Raises NsoError for a file that is not a usable NSO."""
    if os.path.isdir(path):
        raise NsoError('is_folder', 'this is a folder: pass the NSO file itself, not its folder')
    try:
        with open(path, 'rb') as f:
            head = f.read(0x300)
    except IsADirectoryError:
        raise NsoError('is_folder', 'this is a folder: pass the NSO file itself, not its folder')
    except PermissionError:
        raise NsoError('unreadable', 'cannot read the file (permission denied)')
    if len(head) < HEADER_SIZE:
        raise NsoError('too_small', f'the file is only {len(head)} bytes, too small to be an NSO')
    if head[:4] != MAGIC:
        if head[0x200:0x204] in (b'NCA3', b'NCA2', b'NCA0'):
            raise NsoError('is_nca', 'this is an NCA container, not an NSO: extract the exefs from it and pass the "main" or "sdk" file inside')
        if head[:4] == b'PFS0':
            raise NsoError('is_pfs0', 'this is a PFS0 container (an exefs or NSP), not an NSO: extract it and pass the file inside')
        raise NsoError('no_magic', 'the file has no NSO0 header, so it is probably still encrypted or not an NSO: decrypt and extract it with your own tools first')
    return Nso(path, head)


def image_digest(path, nso):
    """SHA-256 over the three decompressed segments (text, rodata, data) in order.

    Compressed and decompressed NSOs of the same module give the same value, whichever tool extracted them.
    Raises NsoError for a truncated or damaged file (sizes or the header's own segment hashes do not match).
    """
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        f.seek(0, 2)
        fsize = f.tell()
        for i, (fo, _mo, sz, csz) in enumerate(nso.segments):
            compressed = bool(nso.flags >> i & 1)
            want = csz if compressed else sz
            if fo + want > fsize:
                raise NsoError('truncated', f'the file ends inside the {SEGMENT_NAMES[i]} segment: it is truncated or a partial copy')
            f.seek(fo)
            raw = f.read(want)
            if compressed:
                try:
                    raw = lz4_decompress(raw, sz)
                except Exception:
                    raise NsoError('corrupt', f'the {SEGMENT_NAMES[i]} segment does not decompress: the file is damaged')
            if nso.flags >> (3 + i) & 1 and hashlib.sha256(raw).digest() != nso.seg_hashes[i]:
                raise NsoError('corrupt', f'the {SEGMENT_NAMES[i]} segment does not match its own header hash: the file is damaged')
            h.update(raw)
    return h.hexdigest()


def write_uncompressed(path, nso, out_path):
    """Write the decompressed form of an NSO: flags without the compression bits, every segment at file offset 0x100 + its
    memory offset (the layout common dump tools produce). Ghidra and the function reconciler read this file."""
    with open(path, 'rb') as f:
        head = bytearray(f.read(HEADER_SIZE))
        segs = []
        for i, (fo, _mo, sz, csz) in enumerate(nso.segments):
            f.seek(fo)
            raw = f.read(csz if nso.flags >> i & 1 else sz)
            segs.append(lz4_decompress(raw, sz) if nso.flags >> i & 1 else raw)
    struct.pack_into('<I', head, 0xC, nso.flags & ~7)
    head[0x2C:0x30] = bytes(4)   # unknown field in the rodata header slot; NSTool clears it when it decompresses
    end = HEADER_SIZE
    for i, (_fo, mo, sz, _csz) in enumerate(nso.segments):
        struct.pack_into('<3I', head, 0x10 + 0x10 * i, HEADER_SIZE + mo, mo, sz)
        struct.pack_into('<I', head, 0x60 + 4 * i, sz)
        end = max(end, HEADER_SIZE + mo + sz)
    body = bytearray(end)
    body[:HEADER_SIZE] = head
    for (_fo, mo, sz, _csz), raw in zip(nso.segments, segs):
        body[HEADER_SIZE + mo:HEADER_SIZE + mo + sz] = raw
    with open(out_path, 'wb') as f:
        f.write(body)
