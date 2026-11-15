"""Hash-pinned patches. A patch file lists the exact image it applies to; every field is checked before generation and a
mismatch stops the build with a plain message. Patch text is data (JSON), never executed here."""
import hashlib
import json
from pathlib import Path


class PatchError(Exception):
    pass


def _sha(b):
    return hashlib.sha256(b).hexdigest()


def load(path):
    raw = Path(path).read_bytes()
    d = json.loads(raw.decode('utf-8'))
    d['_file_sha256'] = _sha(raw)
    return d


def check(patch, image, build_id=None, image_sha256=None):
    """patch: dict with image_sha256, build_id, offset, original_sha256, replacement_hex. image: bytes of the module image.
    Raises PatchError with a plain message on any mismatch; returns True when it applies."""
    name = patch.get('name', '?')
    want = patch.get('image_sha256')
    got = image_sha256 or _sha(image)
    if want != got:
        raise PatchError(f'patch {name}: wrong game image (expected {str(want)[:12]}, have {got[:12]}). Use game version 2.0.1.')
    if build_id is not None and patch.get('build_id') and patch['build_id'].lower() != build_id.lower():
        raise PatchError(f'patch {name}: build id differs from the reviewed one')
    off = int(patch['offset'])
    repl = bytes.fromhex(patch['replacement_hex'])
    if off < 0 or off + len(repl) > len(image):
        raise PatchError(f'patch {name}: span outside the image')
    if _sha(image[off:off + len(repl)]) != patch.get('original_sha256'):
        raise PatchError(f'patch {name}: the bytes at the patch span are not the reviewed ones')
    return True


def check_expected_set(patch_dir, reviewed_names):
    """The staged patch files must equal the reviewed list exactly (no extra, none missing)."""
    have = sorted(p.name for p in Path(patch_dir).glob('*.json'))
    want = sorted(reviewed_names)
    if have != want:
        raise PatchError(f'patch list differs from the reviewed list: extra {sorted(set(have) - set(want))}, missing {sorted(set(want) - set(have))}')
    return True
