"""Compress or decompress an HD texture pack with NTFS compression; see builder/snapbuild/packcompress.py."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'builder'))
from snapbuild import packcompress  # noqa: E402

if __name__ == '__main__':
    sys.exit(packcompress.main())
