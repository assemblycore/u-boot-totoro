#!/usr/bin/env python3
"""Wrap U-Boot and the SBL entry shim in Android Boot Image v0."""
import hashlib
import io
from pathlib import Path
import struct
import sys
import tarfile

build = Path(sys.argv[1])
shim = bytearray((build / 'sbl-entry.bin').read_bytes())
uboot = (build / 'u-boot.bin').read_bytes()
assert len(shim) <= 4096
kernel = shim + bytes(4096 - len(shim)) + uboot
struct.pack_into('<I', kernel, 0x2c, len(kernel))
header = bytearray(4096)
header[:8] = b'ANDROID!'
struct.pack_into('<10I', header, 8, len(kernel), 0x81608000, 0, 0x82600000,
                 0, 0x82500000, 0x81600100, 4096, 0, 0)
header[48:64] = b'totoro-uboot'.ljust(16, b'\0')
h = hashlib.sha1()
for data in (kernel, b'', b''):
    h.update(data)
    h.update(struct.pack('<I', len(data)))
header[576:608] = h.digest() + bytes(12)
image = header + kernel + bytes(-len(kernel) % 4096)
assert len(image) <= 5 * 1024 * 1024
(build / 'boot.img').write_bytes(image)
with tarfile.open(build / 'totoro-uboot-odin.tar', 'w', format=tarfile.USTAR_FORMAT) as t:
    info = tarfile.TarInfo('boot.img')
    info.size = len(image)
    info.mode = 0o644
    t.addfile(info, io.BytesIO(image))
(build / 'SHA256SUMS').write_text(''.join(
    hashlib.sha256((build / n).read_bytes()).hexdigest() + '  ' + n + '\n'
    for n in ('boot.img', 'u-boot.bin', 'totoro-uboot-odin.tar')))
print('boot.img:', len(image), 'bytes')
