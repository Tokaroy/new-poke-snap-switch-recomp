# SPDX-License-Identifier: MIT
r"""species_author_ext2.py plus the mid-range fur treatment `fur_locks` (species_ext_locks.py).

  python -I scripts\hd\species_author_ext3.py author pichu [--debug DIR]
  python -I scripts\hd\species_author_ext3.py pack pichu --root DIR --pack NAME
  python -I scripts\hd\species_author_ext3.py preview pichu [--variant x4_mat] [--only-closeup]
"""
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import species_author_ext2 as ext2  # noqa: E402
import species_ext_locks  # noqa: E402,F401  (registers fur_locks)

if __name__ == '__main__':
    ext2.main()
