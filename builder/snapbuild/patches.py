"""Hash-pinned patches. A patch file lists the exact image it applies to; every field is checked before generation and a
mismatch stops the build with a plain message. Patch text is data (JSON), never executed here."""
import hashlib
import json
from pathlib import Path


REVIEWED = 'reviewed.json'   # the reviewed list: {"patches": ["name.json", ...]}; shipped next to the patch files


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
    have = sorted(p.name for p in Path(patch_dir).glob('*.json') if p.name != REVIEWED)
    want = sorted(reviewed_names)
    if have != want:
        raise PatchError(f'patch list differs from the reviewed list: extra {sorted(set(have) - set(want))}, missing {sorted(set(want) - set(have))}')
    return True


def check_dir(patch_dir, images):
    """Check every staged patch before generation. images: {module: (image bytes, build id, image sha256)}.
    The staged files must equal the reviewed list; each patch names its module and must match that image exactly.
    Returns a digest over the patch files (it goes into the generation key), or '' when there are none."""
    patch_dir = Path(patch_dir)
    rl = patch_dir / REVIEWED
    reviewed = json.loads(rl.read_text(encoding='utf-8')).get('patches', []) if rl.exists() else []
    if not patch_dir.exists() and not reviewed:
        return ''
    check_expected_set(patch_dir, reviewed)
    h = hashlib.sha256()
    for n in sorted(reviewed):
        p = load(patch_dir / n)
        mod = p.get('module', 'main')
        if mod not in images:
            raise PatchError(f'patch {n}: unknown module {mod!r}')
        img, bid, sha = images[mod]
        check(p, img, build_id=bid, image_sha256=sha)
        h.update(n.encode() + b'\0' + p['_file_sha256'].encode())
    return h.hexdigest() if reviewed else ''
