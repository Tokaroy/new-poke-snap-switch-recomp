"""Release signing for the builder zip (developer tool; the signing key never leaves this PC and is never printed or committed).

  python -I builder/release_sign.py --genkey                     make the key pair (once). The seed goes to build/release/signing/,
                                                                 the public half to builder/update_pubkey.txt.
  python -I builder/release_sign.py --zip <zip> --version 1.0.1  write <zip>.sig next to the zip
  python -I builder/release_sign.py --verify <zip>               check a zip against its .sig and builder/update_pubkey.txt

The launcher and builder check every update zip against builder/update_pubkey.txt, so a zip that was not signed with this key is refused.
Publishing the release is a separate step that needs the owner's approval; this script only signs."""
import argparse
import json
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'builder'))
from snapbuild import ed25519, update  # noqa: E402

KEY_DIR = ROOT / 'build' / 'release' / 'signing'
SEED_FILE = KEY_DIR / 'update_signing.key'
PUB_FILE = ROOT / 'builder' / 'update_pubkey.txt'


def load_seed(path=None):
    p = Path(path) if path else SEED_FILE
    try:
        s = bytes.fromhex(p.read_text(encoding='ascii').strip())
    except (OSError, ValueError):
        raise SystemExit(f'no usable signing key at {p}; run --genkey once')
    if len(s) != 32:
        raise SystemExit(f'{p} is not a 32-byte key')
    return s


def genkey(seed_file=SEED_FILE, pub_file=PUB_FILE):
    if Path(seed_file).exists():
        raise SystemExit(f'{seed_file} already exists; refusing to replace the release key (every installed builder trusts it)')
    Path(seed_file).parent.mkdir(parents=True, exist_ok=True)
    seed = os.urandom(32)
    fd = os.open(seed_file, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    with os.fdopen(fd, 'w', encoding='ascii') as f:
        f.write(seed.hex() + '\n')
    Path(pub_file).write_text(ed25519.public_key(seed).hex() + '\n', encoding='ascii')
    return Path(pub_file)


def sign_zip(zip_path, version, seed):
    zip_path = Path(zip_path)
    ver = update.norm_version(version)
    if update.ZIP_RE.match(zip_path.name) is None or update.norm_version(update.ZIP_RE.match(zip_path.name).group(1)) != ver:
        raise SystemExit(f'the zip must be named SnapRecomp-Builder-v{ver}-win-x64.zip')
    digest, size = update.sha256_of(zip_path), zip_path.stat().st_size
    sig = ed25519.sign(seed, update.signed_message(ver, digest, size))
    doc = {'format': 1, 'version': ver, 'sha256': digest, 'size': size, 'signature': sig.hex()}
    out = zip_path.with_name(zip_path.name + '.sig')
    out.write_text(json.dumps(doc, indent=1), encoding='ascii')
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--genkey', action='store_true')
    ap.add_argument('--zip')
    ap.add_argument('--version')
    ap.add_argument('--verify')
    ap.add_argument('--key', help='signing key file (default build/release/signing/update_signing.key)')
    a = ap.parse_args(argv)
    if a.genkey:
        print('public key written to', genkey())
        return 0
    if a.zip:
        if not a.version:
            raise SystemExit('--version is required with --zip')
        seed = load_seed(a.key)
        # the version inside the zip must say the same thing the signature does
        import zipfile
        with zipfile.ZipFile(a.zip) as z:
            names = [n for n in z.namelist() if n.replace('\\', '/').endswith('builder/VERSION')]
            if len(names) != 1 or update.norm_version(z.read(names[0]).decode().strip()) != update.norm_version(a.version):
                raise SystemExit('builder/VERSION inside the zip does not say ' + a.version)
        print('wrote', sign_zip(a.zip, a.version, seed))
        return 0
    if a.verify:
        z = Path(a.verify)
        sig = update.parse_sig(Path(str(z) + '.sig').read_text(encoding='ascii'))
        ok = (update.sha256_of(z) == sig['sha256'] and z.stat().st_size == sig['size']
              and ed25519.verify(update.load_pubkey(ROOT / 'builder'), update.signed_message(sig['version'], sig['sha256'], sig['size']),
                                 bytes.fromhex(sig['signature'])))
        print('signature OK for version ' + sig['version'] if ok else 'signature does NOT verify')
        return 0 if ok else 1
    ap.print_help()
    return 2


if __name__ == '__main__':
    sys.exit(main())
