"""The tool pins that go into builder/versions.json ("tools"). Every hash comes from the publisher's own page (see each "source").

Edit here, then run:  python -I builder/dev/tool_pins.py   (rewrites only the "tools" key of versions.json).
Large archives are never downloaded by this script: the hashes below are the digests shown on the release pages.
"""
import json
from pathlib import Path

GH = 'https://github.com'

TOOLS = {
    'llvm': {
        'version_major': 23, 'tested': '23.1.5',
        'filename': 'clang+llvm-23.1.5-x86_64-pc-windows-msvc.tar.xz',
        'url': GH + '/llvm/llvm-project/releases/download/llvmorg-23.1.5/clang+llvm-23.1.5-x86_64-pc-windows-msvc.tar.xz',
        'sha256': '62fdada7f6fa2b73856486dfbbdba3d2c1882991621615bcb55c60207611c6d4', 'size': 901309624,
        'install': {'dir': 'llvm', 'strip': 1},
        'source': GH + '/llvm/llvm-project/releases/tag/llvmorg-23.1.5 (asset digest on the release page; also published as the .jsonl attestation next to the archive)',
    },
    'ghidra': {
        'version': '12.1.2 PUBLIC 20260605',
        'filename': 'ghidra_12.1.2_PUBLIC_20260605.zip',
        'url': GH + '/NationalSecurityAgency/ghidra/releases/download/Ghidra_12.1.2_build/ghidra_12.1.2_PUBLIC_20260605.zip',
        'sha256': 'b62e81a0390618466c019c60d8c2f796ced2509c4c1aea4a37644a77272cf99d', 'size': 572803866,
        'install': {'dir': '.', 'strip': 0},   # the zip holds ghidra_12.1.2_PUBLIC/
        'source': GH + '/NationalSecurityAgency/ghidra/releases/tag/Ghidra_12.1.2_build (asset digest on the release page)',
    },
    'switch_loader': {
        'version': '1.6.1-0f38957 for Ghidra 12.1.2',
        'filename': 'SwitchLoader-1.6.1-0f38957-Ghidra_12.1.2.zip',
        'url': GH + '/Adubbz/Ghidra-Switch-Loader/releases/download/1.6.1-0f38957-Ghidra_12.1.2/SwitchLoader-1.6.1-0f38957-Ghidra_12.1.2.zip',
        'sha256': 'c37b8e8360a5071b47f7a56e2bf467b00e4a6eef884cc3dede17b5ad65298f96', 'size': 9125182,
        'install': {'dir': 'ghidra_12.1.2_PUBLIC/Ghidra/Extensions', 'strip': 0},   # the zip holds SwitchLoader/
        'source': GH + '/Adubbz/Ghidra-Switch-Loader/releases/tag/1.6.1-0f38957-Ghidra_12.1.2 (asset digest on the release page; matches the copy tested)',
    },
    'jdk': {
        'version': 'temurin 21.0.9+10',
        'filename': 'OpenJDK21U-jdk_x64_windows_hotspot_21.0.9_10.zip',
        'url': GH + '/adoptium/temurin21-binaries/releases/download/jdk-21.0.9%2B10/OpenJDK21U-jdk_x64_windows_hotspot_21.0.9_10.zip',
        'sha256': '1c67df516e9795c0b09f5714bfe151da2e3cc988082042f5bbb60d75e4e63fb5', 'size': 204962060,
        'install': {'dir': '.', 'strip': 0},   # the zip holds jdk-21.0.9+10/
        'source': GH + '/adoptium/temurin21-binaries/releases/download/jdk-21.0.9%2B10/OpenJDK21U-jdk_x64_windows_hotspot_21.0.9_10.zip.sha256.txt (Adoptium checksum file)',
    },
    'vulkan_sdk': {
        'note': 'not needed: the prebuilt folder ships a small vulkan-1.lib import library; vulkan-1.dll comes with the graphics driver',
    },
    'python_embed': {
        'version': '3.14.6',
        'filename': 'python-3.14.6-embed-amd64.zip',
        'url': 'https://www.python.org/ftp/python/3.14.6/python-3.14.6-embed-amd64.zip',
        'sha256': 'df901e84a896ff1ee720ad03377e0c8d8c2244fda79808aeeaff6316df1cb75c', 'size': 12570832,
        'install': {'dir': 'python', 'strip': 0},
        'source': 'https://www.python.org/ftp/python/3.14.6/python-3.14.6-embed-amd64.zip.spdx.json (the release SBOM lists this SHA-256; the downloaded zip matched)',
    },
    'python_packages': {
        'note': 'installed with pip --require-hashes --no-deps --only-binary=:all: from wheels fetched and verified by the builder; every wheel is pinned below',
        'wheels': [
            {'name': 'pip', 'version': '26.1.2', 'filename': 'pip-26.1.2-py3-none-any.whl',
             'url': 'https://files.pythonhosted.org/packages/5d/95/6b5cb3461ea5673ba0995989746db58eb18b91b54dbf331e72f569540946/pip-26.1.2-py3-none-any.whl',
             'sha256': '382ff9f685ee3bc25864f820aa50505825f10f5458ffff07e30a6d96e5715cab', 'installer': True},
            {'name': 'capstone', 'version': '5.0.9', 'filename': 'capstone-5.0.9-py3-none-win_amd64.whl',
             'url': 'https://files.pythonhosted.org/packages/50/e6/6f06fdb6a9ed32b2f7cd9c036b92d5324112c3ef7080f2c71efc367d40dd/capstone-5.0.9-py3-none-win_amd64.whl',
             'sha256': '732cedbbb56d42e723f14d7af6387f1454194a820b4b96b56d1e53f865ef85d0'},
            {'name': 'numpy', 'version': '2.5.4', 'filename': 'numpy-2.5.4-cp314-cp314-win_amd64.whl',
             'url': 'https://files.pythonhosted.org/packages/52/9a/e1e512ebc948d5b9dd33b08736760f0ebbed2848fd4eda1f553088a6dcee/numpy-2.5.4-cp314-cp314-win_amd64.whl',
             'sha256': '2ca144f15135b6212a5c47b1e2aeca6e412f102f95a2d5d88d8aec77eb255de3'},
            {'name': 'lz4', 'version': '4.4.5', 'filename': 'lz4-4.4.5-cp314-cp314-win_amd64.whl',
             'url': 'https://files.pythonhosted.org/packages/18/2c/8332080fd293f8337779a440b3a143f85e374311705d243439a3349b81ad/lz4-4.4.5-cp314-cp314-win_amd64.whl',
             'sha256': '92159782a4502858a21e0079d77cdcaade23e8a5d252ddf46b0652604300d7be'},
            {'name': 'pyelftools', 'version': '0.33', 'filename': 'pyelftools-0.33-py3-none-any.whl',
             'url': 'https://files.pythonhosted.org/packages/46/2a/f9697576603dae937727827505a6126a066affb227034e77e6f9068910da/pyelftools-0.33-py3-none-any.whl',
             'sha256': 'f215ad5f47d3f1373a21496a6c9e0707c622840d0622f23ff7ce08678b020036'},
        ],
        'source': 'https://pypi.org/pypi/<name>/json (digests of the files PyPI serves), read 2026-11-14',
    },
}

# Upscaler runtime files: fetched on the user PC after the user accepts the vendor license; never shipped by us.
# SHA-256 values below were computed by us from the official files on 2026-11-14 (AMD publishes none; Intel's release page shows the
# archive digest, which matched; NVIDIA publishes none). The AMD values also equal the independent pins of another project.
AMD_COMMIT = '7539ae8b6568ad29ed78a753404351daf3b5cca7'
AMD_RAW = f'https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/{AMD_COMMIT}/'
UPSCALERS = {
    'fsr': {
        'title': 'AMD FidelityFX Super Resolution',
        'detail': 'FSR 3.1 on any Vulkan GPU; FSR 4 only where the AMD driver offers it (set SNAP_FSR4=1)',
        'vendor': 'AMD', 'default': True,
        'license': {'filename': 'AMD-FidelityFX-LICENSE.md', 'url': AMD_RAW + 'Kits/FidelityFX/docs/license.md',
                    'sha256': '32357259b965d6a8001e9e508ba60eda6034027c5923a7d226ffbaff813c2bfb', 'size': 65770},
        'files': [
            {'url': AMD_RAW + 'Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_vk.dll', 'dest': 'upscalers/amd_fidelityfx_upscaler_vk.dll',
             'sha256': '4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7', 'size': 36135696},
            {'url': AMD_RAW + '3rdpartynotice.md', 'dest': 'licenses/upscalers/AMD-FidelityFX-3rdpartynotice.md',
             'sha256': '3b69b1a1011622557a81b55a9a211dd797d01b8160924a5ec51b4c3432d33a4f', 'size': 2519},
        ],
        'source': 'github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, FSR SDK v2.4.0 = commit ' + AMD_COMMIT + ' (signedbin and docs/license.md); hashes computed by us 2026-11-14, no vendor digest published',
    },
    # FSR 3.1 frame generation (off by default, a separate option): same AMD license, same commit, one more DLL beside the upscaler's
    'fsr_fg': {
        'title': 'AMD FidelityFX Frame Generation',
        'detail': 'FSR 3.1 frame generation on any Vulkan GPU; off by default, needs a steady 50+ fps base frame rate',
        'vendor': 'AMD', 'default': False,
        'license': {'filename': 'AMD-FidelityFX-LICENSE.md', 'url': AMD_RAW + 'Kits/FidelityFX/docs/license.md',
                    'sha256': '32357259b965d6a8001e9e508ba60eda6034027c5923a7d226ffbaff813c2bfb', 'size': 65770},
        'files': [
            {'url': AMD_RAW + 'Kits/FidelityFX/signedbin/amd_fidelityfx_framegeneration_vk.dll', 'dest': 'upscalers/amd_fidelityfx_framegeneration_vk.dll',
             'sha256': 'd002dabff8c5e71eaa715fc99438cd4dc81230262a7897b16bc402efec3498b4', 'size': 36806928},
            {'url': AMD_RAW + '3rdpartynotice.md', 'dest': 'licenses/upscalers/AMD-FidelityFX-3rdpartynotice.md',
             'sha256': '3b69b1a1011622557a81b55a9a211dd797d01b8160924a5ec51b4c3432d33a4f', 'size': 2519},
        ],
        'source': 'github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, FSR SDK v2.4.0 = commit ' + AMD_COMMIT + ' (signedbin/amd_fidelityfx_framegeneration_vk.dll); hash computed by us 2026-11-15, no vendor digest published',
    },
    'dlss': {
        'title': 'NVIDIA DLSS',
        'detail': 'NVIDIA RTX graphics cards only',
        'vendor': 'NVIDIA', 'default': False,
        'license': {'filename': 'NVIDIA-RTX-SDKs-LICENSE.txt', 'url': 'https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.16/LICENSE.txt',
                    'sha256': 'd4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0', 'size': 26620},
        'files': [
            {'url': 'https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.16/lib/Windows_x86_64/rel/nvngx_dlss.dll', 'dest': 'nvngx_dlss.dll',
             'sha256': 'd00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e', 'size': 58956912},
        ],
        'source': 'github.com/NVIDIA/DLSS tag v310.9.16 (lib/Windows_x86_64/rel, LICENSE.txt); hashes computed by us 2026-11-14, no vendor digest published',
    },
    'xess': {
        'title': 'Intel XeSS',
        'detail': 'any Vulkan GPU (best on Intel Arc)',
        'vendor': 'Intel', 'default': False,
        'license': {'filename': 'Intel-XeSS-LICENSE.txt', 'url': 'https://raw.githubusercontent.com/intel/xess/v3.0.2/LICENSE.txt',
                    'sha256': '784be42c39a4a4d03cf85d88e15e59d75828fca773ce45b8dbfffbd7eaf208df', 'size': 4194},
        'archive': {'url': 'https://github.com/intel/xess/releases/download/v3.0.2/XeSS_SDK_3.0.2.zip', 'filename': 'XeSS_SDK_3.0.2.zip',
                    'sha256': '88b8a373f30e33f3558a77a93e634f11b8132fc3047ea1a8edeead32b8471990', 'size': 76839749},
        'files': [
            {'member': 'bin/libxess.dll', 'dest': 'libxess.dll', 'sha256': '251659dd84a3e84de67c886a4186e01f3eca49b00641906fe38bb6b807e5d5b7', 'size': 77795704},
            {'member': 'third-party-programs.txt', 'dest': 'licenses/upscalers/Intel-XeSS-third-party-programs.txt',
             'sha256': 'e8d8899dbfc4de0e8ca25560be9358e03c1405d7c956d20a1838c3c709b9b51c', 'size': 35788},
        ],
        'source': 'github.com/intel/xess release v3.0.2, XeSS_SDK_3.0.2.zip (digest shown on the release page); inner file hashes computed by us 2026-11-14',
    },
}

if __name__ == '__main__':
    p = Path(__file__).resolve().parents[1] / 'versions.json'
    t = json.loads(p.read_text(encoding='utf-8'))
    t['tools'] = TOOLS
    t['upscalers'] = UPSCALERS
    p.write_text(json.dumps(t, indent=1) + '\n', encoding='utf-8')
    print('updated', p)
