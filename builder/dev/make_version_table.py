"""Dev-only: write builder/versions.json from the local dumps (extracted/v*/exefs and the 2.0.1 romfs). Not shipped.

  python -I builder/dev/make_version_table.py --extracted extracted --romfs extracted/romfs-v262144 --out builder/versions.json
Keeps the "tools" and "prebuilt" sections of an existing versions.json.
"""
import argparse
import hashlib
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from snapbuild import nso as nsomod  # noqa: E402
from snapbuild import versioncheck as vc  # noqa: E402

VERSIONS = [('1.0.0 (v0)', 'v0'), ('1.1.0 (v65536)', 'v65536'), ('1.2.0 (v131072)', 'v131072'),
            ('2.0.0 (v196608)', 'v196608'), (vc.EXPECTED_NAME, 'v262144')]


TODO = 'TODO'
from tool_pins import TOOLS   # the pinned tools, with their sources (builder/dev/tool_pins.py)


def nso_values(path):
    h = nsomod.read_header(path)
    return h.build_id, nsomod.image_digest(path, h)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--extracted', type=Path, required=True)
    ap.add_argument('--romfs', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--sentinels', type=int, default=32)
    ap.add_argument('--maps', type=Path, help='folder with functions_final.tsv and jump_tables.tsv (records their row counts)')
    ap.add_argument('--title-marker', help='base name of the archive the game opens when the title screen loads (only its hash is stored)')
    a = ap.parse_args()
    old = json.loads(a.out.read_text(encoding='utf-8')) if a.out.exists() else {}
    known, nso = [], {}
    for name, d in VERSIONS:
        mb, md = nso_values(a.extracted / d / 'exefs' / 'main')
        sb, sd = nso_values(a.extracted / d / 'exefs' / 'sdk')
        known.append({'name': name, 'main_build_id': mb, 'sdk_build_id': sb})
        if name == vc.EXPECTED_NAME:
            nso = {'main': {'build_id': mb, 'image_sha256': md}, 'sdk': {'build_id': sb, 'image_sha256': sd}}
    listing = vc.romfs_listing(a.romfs)
    step = len(listing) / a.sentinels
    sent = [listing[int(i * step + step / 2)] for i in range(a.sentinels)]
    table = {
        'format': 1,
        'game_version': vc.EXPECTED_NAME,
        'note': 'Hashes of the published game files only. Sentinel paths are stored hashed (lowercase, forward slashes), never as names.',
        'image_hash': 'SHA-256 over the decompressed text, rodata and data segments in order',
        'nso': nso,
        'known_versions': known,
        'romfs': {'file_count': len(listing), 'total_bytes': sum(s for _, s in listing), 'listing_sha256': vc.listing_hash(listing),
                  'sentinels': [{'path_sha256': vc.path_key(r), 'size': s} for r, s in sent]},
        'tools': TOOLS,
        'prebuilt': old.get('prebuilt', {}),
    }
    if a.maps:
        rows = lambda n: sum(1 for l in open(a.maps / n, encoding='utf-8') if l.strip() and not l.startswith('#'))
        table['maps'] = {'functions_final_rows': rows('functions_final.tsv'), 'jump_tables_rows': rows('jump_tables.tsv')}
    elif 'maps' in old:
        table['maps'] = old['maps']
    if a.title_marker:
        table['smoke'] = {'title_marker_sha256': hashlib.sha256(a.title_marker.lower().encode()).hexdigest(), 'min_presents': 60}
    elif 'smoke' in old:
        table['smoke'] = old['smoke']
    table['generated_c_sha256'] = old.get('generated_c_sha256', {'main': None, 'sdk': None})
    a.out.write_text(json.dumps(table, indent=1) + '\n', encoding='utf-8')
    print(f'wrote {a.out}: {len(known)} versions, {len(listing)} romfs files, {len(sent)} sentinels')


if __name__ == '__main__':
    main()
