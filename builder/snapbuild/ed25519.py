"""Ed25519 signatures (RFC 8032, pure Python, no dependencies). Used to check that an update zip was signed by the release key.

Verification is the part the builder needs at run time. Key generation and signing are here for the release script and the tests.
The code follows the reference implementation in RFC 8032 section 6 (public domain). It is not constant time, which does not matter
for verifying a public signature; keep signing to the release machine."""
import hashlib

P = 2**255 - 19
Q = 2**252 + 27742317777372353535851937790883648493   # group order
D = -121665 * pow(121666, P - 2, P) % P
I = pow(2, (P - 1) // 4, P)


def _inv(x):
    return pow(x, P - 2, P)


def _recover_x(y, sign):
    if y >= P:
        return None
    x2 = (y * y - 1) * _inv(D * y * y + 1) % P
    if x2 == 0:
        return None if sign else 0
    x = pow(x2, (P + 3) // 8, P)
    if (x * x - x2) % P != 0:
        x = x * I % P
    if (x * x - x2) % P != 0:
        return None
    if (x & 1) != sign:
        x = P - x
    return x


_GY = 4 * _inv(5) % P
_GX = _recover_x(_GY, 0)
G = (_GX, _GY, 1, _GX * _GY % P)
ZERO = (0, 1, 1, 0)


def _add(a, b):
    x1, y1, z1, t1 = a
    x2, y2, z2, t2 = b
    A = (y1 - x1) * (y2 - x2) % P
    B = (y1 + x1) * (y2 + x2) % P
    C = 2 * t1 * t2 * D % P
    E = 2 * z1 * z2 % P
    F, H = B - A, B + A
    Gg, Hh = E + C, E - C
    return (F * Hh % P, Gg * H % P, Gg * Hh % P, F * H % P)


def _mul(s, pt):
    q = ZERO
    while s > 0:
        if s & 1:
            q = _add(q, pt)
        pt = _add(pt, pt)
        s >>= 1
    return q


def _equal(a, b):
    return (a[0] * b[2] - b[0] * a[2]) % P == 0 and (a[1] * b[2] - b[1] * a[2]) % P == 0


def _compress(pt):
    zi = _inv(pt[2])
    x, y = pt[0] * zi % P, pt[1] * zi % P
    return int.to_bytes(y | ((x & 1) << 255), 32, 'little')


def _decompress(b):
    if len(b) != 32:
        return None
    y = int.from_bytes(b, 'little')
    sign = y >> 255
    y &= (1 << 255) - 1
    x = _recover_x(y, sign)
    if x is None:
        return None
    return (x, y, 1, x * y % P)


def _h(m):
    return int.from_bytes(hashlib.sha512(m).digest(), 'little')


def _expand(seed):
    if len(seed) != 32:
        raise ValueError('an Ed25519 seed is 32 bytes')
    h = hashlib.sha512(seed).digest()
    a = int.from_bytes(h[:32], 'little')
    a &= (1 << 254) - 8
    a |= 1 << 254
    return a, h[32:]


def public_key(seed):
    a, _ = _expand(seed)
    return _compress(_mul(a, G))


def sign(seed, msg):
    a, prefix = _expand(seed)
    pub = _compress(_mul(a, G))
    r = _h(prefix + msg) % Q
    rs = _compress(_mul(r, G))
    k = _h(rs + pub + msg) % Q
    s = (r + k * a) % Q
    return rs + int.to_bytes(s, 32, 'little')


def verify(pub, msg, sig):
    """True only for a valid signature of msg under the 32-byte public key."""
    if len(pub) != 32 or len(sig) != 64:
        return False
    a = _decompress(pub)
    r = _decompress(sig[:32])
    if a is None or r is None:
        return False
    s = int.from_bytes(sig[32:], 'little')
    if s >= Q:
        return False
    k = _h(sig[:32] + pub + msg) % Q
    return _equal(_mul(s, G), _add(r, _mul(k, a)))
