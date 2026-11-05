#!/usr/bin/env python3
"""Create an empty partitioned 64 MiB FAT16 card for emulator checks only."""
from pathlib import Path
import struct
import subprocess
import sys

path = Path(sys.argv[1])
with path.open('xb') as image:
    image.truncate(64 * 1024 * 1024)
    mbr = bytearray(512)
    struct.pack_into('<B3sB3sII', mbr, 446, 0, b'\xfe\xff\xff', 6,
                     b'\xfe\xff\xff', 2048, 129024)
    mbr[510:] = b'\x55\xaa'
    image.seek(0)
    image.write(mbr)
subprocess.run(['mkfs.vfat', '-F', '16', '--offset', '2048', '-n', 'TOTORO', str(path)], check=True)
