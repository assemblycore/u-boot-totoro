#!/usr/bin/env python3
"""Cold-boot the prepared test flash through SBL and exercise U-Boot CLI."""
import json
import os
from pathlib import Path
import socket
import subprocess
import struct
import tempfile
import time
import zlib

root = Path(__file__).resolve().parents[3]
build = root / 'build-totoro'
qemu = root.parent / 'qemu-bcm21553/build/qemu-system-arm'
sbl = root.parent / 'reference-firmware/sbl.bin'
# Positive/negative native-image fixtures live only in a disposable copy.
# main.img, its boot backup and real calibration partitions remain untouched.
scratch = tempfile.TemporaryDirectory(prefix='phone-check-', dir=build)
test_flash = Path(scratch.name) / 'main.img'
test_oob = Path(scratch.name) / 'oob.img'
subprocess.run(['cp', '--reflink=auto', '--sparse=always',
                str(build / 'main.img'), str(test_flash)], check=True)
subprocess.run(['cp', '--reflink=auto', '--sparse=always',
                str(build / 'oob.img'), str(test_oob)], check=True)
payload = Path(scratch.name) / 'payload.bin'
payload.write_bytes(struct.pack('<II', 0xe3a00000, 0xe12fff1e))  # return 0
uimage = Path(scratch.name) / 'payload.uimg'
subprocess.run([str(build / 'tools/mkimage'), '-A', 'arm', '-O', 'u-boot',
                '-T', 'standalone', '-C', 'none', '-a', '0x82000000',
                '-e', '0x82000000', '-n', 'phone-check', '-d', str(payload),
                str(uimage)], check=True, stdout=subprocess.DEVNULL)
good = uimage.read_bytes()
script_text = Path(scratch.name) / 'boot.cmd'
script_text.write_text('echo TOTORO_SCRIPT_OK ${bootdev} ${bootpart}\n')
script_image = Path(scratch.name) / 'boot.scr'
subprocess.run([str(build / 'tools/mkimage'), '-A', 'arm', '-T', 'script',
                '-C', 'none', '-n', 'script-test', '-d', str(script_text),
                str(script_image)], check=True, stdout=subprocess.DEVNULL)
# FAT12 filesystem fixture for the disposable 2560-sector param_lfs volume.
fat_image = Path(scratch.name) / 'files.img'
with fat_image.open('wb') as out:
    out.truncate(2560 * 512)
subprocess.run(['mkfs.vfat', '-F', '12', str(fat_image)], check=True,
               stdout=subprocess.DEVNULL)
fat = bytearray(fat_image.read_bytes())
reserved = struct.unpack_from('<H', fat, 14)[0]
fats, root_entries, fat_sectors = fat[16], struct.unpack_from('<H', fat, 17)[0], struct.unpack_from('<H', fat, 22)[0]
root_offset = (reserved + fats * fat_sectors) * 512
data_offset = root_offset + ((root_entries * 32 + 511) // 512) * 512
for i in range(fats):
    pos = (reserved + i * fat_sectors) * 512 + 3
    fat[pos] = 0xff
    fat[pos + 1] = (fat[pos + 1] & 0xf0) | 0x0f  # cluster 2 = EOC
scr = script_image.read_bytes()
assert len(scr) <= fat[13] * 512
fat[root_offset:root_offset + 11] = b'BOOT    SCR'
fat[root_offset + 11] = 0x20
struct.pack_into('<HI', fat, root_offset + 26, 2, len(scr))
fat[data_offset:data_offset + len(scr)] = scr
fat_image.write_bytes(fat)
bad_header, bad_data = bytearray(good), bytearray(good)
bad_header[4] ^= 1
bad_data[-1] ^= 1
with test_flash.open('r+b') as flash:
    for block, data in ((108, good), (1997, bad_header), (1998, bad_data)):
        flash.seek(block * 262144)
        flash.write(data)
a, b = socket.socketpair()
a.settimeout(30)
env = dict(os.environ, TOTORO_ONENAND_PROBE='1', TOTORO_ONENAND_RW='1',
           TOTORO_ONENAND_IMAGE=str(test_flash),
           TOTORO_ONENAND_OOB=str(test_oob))
test_sd = Path(scratch.name) / 'sd.img'
if (build / 'sd.img').exists():
    subprocess.run(['cp', '--reflink=auto', str(build / 'sd.img'), str(test_sd)], check=True)
sd_args = ['-drive', f'file={test_sd},if=sd,format=raw'] if test_sd.exists() else []
with (build / 'uart.log').open('wb') as log:
    p = subprocess.Popen([str(qemu), '-M', 'totoro', '-display', 'none',
        '-serial', 'stdio', '-monitor', 'none', '-chardev',
        f'socket,id=qmp,fd={b.fileno()}', '-mon', 'chardev=qmp,mode=control',
        '-device', f'loader,file={sbl},addr=0x96800000,cpu-num=0,force-raw=on',
        '-device', f'loader,file={fat_image},addr=0x82500000,force-raw=on',
        '-device', f'loader,file={script_image},addr=0x82800000,force-raw=on'] + sd_args,
        env=env, pass_fds=[b.fileno()], stdin=subprocess.PIPE, stdout=log)
    b.close()
    f = a.makefile('rwb', buffering=0)
    def command(name, arguments=None):
        f.write((json.dumps({'execute': name, **({'arguments': arguments} if arguments else {})}) + '\n').encode())
        while True:
            reply = json.loads(f.readline())
            if 'return' in reply or 'error' in reply:
                return reply
    try:
        f.readline()
        command('qmp_capabilities')
        for _ in range(60):
            if b'*** U-Boot Boot Menu ***' in (build / 'uart.log').read_bytes():
                break
            if p.poll() is not None:
                raise RuntimeError('QEMU exited')
            time.sleep(0.5)
        else:
            raise RuntimeError('SBL did not reach the U-Boot menu')
        def key(qcode):
            for down in (True, False):
                command('input-send-event', {'events': [{'type': 'key', 'data': {
                    'down': down, 'key': {'type': 'qcode', 'data': qcode}}}]})
                time.sleep(0.2)
        time.sleep(1)
        key('pgdn')
        key('pgup')
        command('screendump', {'filename': str(build / 'menu.png'), 'format': 'png'})
        if sd_args:
            key('pgdn')
            key('pgdn')
            key('home')
            # Choose partition 1, then List files in the script menu.
            key('pgdn')
            key('home')
            key('pgdn')
            key('pgdn')
            key('pgdn')
            key('home')
            for _ in range(100):
                if b'Home/Enter: return to menu' in (build / 'uart.log').read_bytes(): break
                time.sleep(0.1)
            else: raise RuntimeError('Home selection failed')
            key('home')
            time.sleep(1)
            start = (build / 'uart.log').stat().st_size
            for _ in range(4):
                key('pgdn')
            key('home')
            for _ in range(100):
                tail = (build / 'uart.log').read_bytes()[start:]
                if b'Home/Enter: return to menu' in tail:
                    break
                time.sleep(0.1)
            else:
                raise RuntimeError('Main-menu SD Mass Storage did not return')
            if b'UMS: LUN 0, dev mmc 0' not in tail:
                raise RuntimeError('Main-menu SD Mass Storage did not start')
            key('home')
            time.sleep(1)
        def uart(cmd):
            start = (build / 'uart.log').stat().st_size
            p.stdin.write(cmd)
            p.stdin.flush()
            for _ in range(200):
                tail = (build / 'uart.log').read_bytes()[start:]
                if tail.endswith(b'totoro => '):
                    return tail
                time.sleep(0.1)
            raise RuntimeError(f'Command timed out: {cmd!r}')
        uart(b'q')
        uart(b'version\n')
        uart(b'echo TOTORO_SBL_OK\n')
        # Old QEMU binaries expose the whole page size in F003. This model
        # allows register writes: reproduce the phone's 2KiB BufferRAM value.
        # Emulator-only fixture, never a command to run on a physical phone.
        uart(b'mw.w 0x0041e006 0x0800 1\n')
        uart(b'phone part\n')
        if b'Read 1 logical sectors' not in uart(b'phone read param_lfs 0x82000000 0 1\n'):
            raise RuntimeError('STL logical sector read failed')
        if b'21 43 65 87' not in uart(b'md.b 0x82000000 0x20\n'):
            raise RuntimeError('STL returned wrong param.lfs signature')
        # Simulate many PC reads: the session must not be reopened per request.
        start = (build / 'uart.log').stat().st_size
        for _ in range(100):
            if b'Read 1 logical sectors' not in uart(b'phone read param_lfs 0x82000000 0 1\n'):
                raise RuntimeError('Repeated STL reads exhausted SBL memory')
        if b'STL session:' in (build / 'uart.log').read_bytes()[start:]:
            raise RuntimeError('STL session reopened during repeated reads')
        # Only the disposable main/OOB pair is writable. Exercise two pages
        # beyond the stock param.lfs file, then reboot to discard all FSR RAM.
        uart(b'mw.b 0x82000000 0x5a 0x2000\n')
        if b'Wrote 16 logical sectors' not in uart(b'phone write param_lfs 0x82000000 0x800 0x10\n'):
            raise RuntimeError('STL write failed')
        uart(b'mw.b 0x82000000 0xa5 0x200\n')
        if b'Wrote 1 logical sectors' not in uart(b'phone write param_lfs 0x82000000 0x801 1\n'):
            raise RuntimeError('STL partial-page write failed')
        start = (build / 'uart.log').stat().st_size
        command('system_reset')
        for _ in range(200):
            tail = (build / 'uart.log').read_bytes()[start:]
            if b'ENTER to select' in tail:
                break
            time.sleep(0.1)
        else:
            raise RuntimeError('SBL reboot after STL write failed')
        uart(b'q')
        if b'Read 16 logical sectors' not in uart(b'phone read param_lfs 0x82000000 0x800 0x10\n'):
            raise RuntimeError('STL read after reboot failed')
        expected = bytes([0x5a]) * 512 + bytes([0xa5]) * 512 + bytes([0x5a]) * 7168
        expected_crc = f'{zlib.crc32(expected):08x}'.encode()
        if expected_crc not in uart(b'crc32 0x82000000 0x2000\n'):
            raise RuntimeError('STL data did not persist across reboot')
        if b'Phone error:' not in uart(b'phone write param_lfs 0x82000000 0x100000 1\n'):
            raise RuntimeError('Out-of-range STL write accepted')
        if b'Phone error: -22' not in uart(b'phone write boot 0x82000000 0 1\n'):
            raise RuntimeError('Non-STL write accepted')
        if b'Read 1 logical sectors' not in uart(b'phone read param_lfs 0x82000000 0 1\n'):
            raise RuntimeError('STL header read after write failed')
        if b'21 43 65 87' not in uart(b'md.b 0x82000000 0x20\n'):
            raise RuntimeError('STL write corrupted a different sector')
        if b'Read/write USB disk' not in uart(b'phone ums param_lfs\n'):
            raise RuntimeError('Logical USB export did not initialize')
        print('ALL-LUN PROBE:', uart(b'phone ums-all\n').decode(errors='replace'))
        if b'Wrote 2560 logical sectors' not in uart(b'phone write param_lfs 0x82500000 0 0xa00\n'):
            raise RuntimeError('Disposable internal filesystem write failed')
        if b'TOTORO_SCRIPT_OK mtd 15:0' not in uart(b'phone script param_lfs\n'):
            raise RuntimeError('Internal filesystem script execution failed')
        if sd_args:
            uart(b'sdinit\n')
            if b'bytes written' not in uart(f'fatwrite mmc 0:1 0x82800000 BOOT.SCR {len(scr):x}\n'.encode()):
                raise RuntimeError('SD script write failed')
            if b'TOTORO_SCRIPT_OK mmc 0:1' not in uart(b'sdboot 1\n'):
                raise RuntimeError('SD script execution failed')
            if b'Script not found:' not in uart(b'sdboot 1 /missing.scr\n'):
                raise RuntimeError('Missing script accepted')
            uart(b'mw.b 0x82800000 0 1\n')
            uart(f'fatwrite mmc 0:1 0x82800000 BAD.SCR {len(scr):x}\n'.encode())
            if b'Invalid boot.scr' not in uart(b'sdboot 1 /BAD.SCR\n'):
                raise RuntimeError('Corrupt script accepted')
        uart(b'phone read-raw boot 0x82000000 0 0x10\n')
        uart(b'md.b 0x82000000 0x10\n')
        uart(b'phone boot boot\n')  # Stock Android container must not execute.
        uart(b'phone read-raw boot 0x82000000 0x4fffff 2\n')  # Range overflow.
        if b'Application terminated, rc = 0x0' not in uart(b'phone boot boot_backup\n'):
            raise RuntimeError('Internal standalone uImage execution failed')
        if b'Phone error: -22' not in uart(b'phone boot efs\n'):
            raise RuntimeError('Corrupt uImage header accepted')
        if b'Phone error: -74' not in uart(b'phone boot sysparm_dep\n'):
            raise RuntimeError('Corrupt uImage data accepted')
        uart(b'fastboot usb 0\n')
        uart(b'phone ums-raw boot\n')
        if sd_args:
            for line in (b'mmc rescan', b'fatls mmc 0:1 /',
                         b'mw.b 0x83000000 0x41 0x10',
                         b'fatwrite mmc 0:1 0x83000000 TEST.TXT 0x10',
                         b'load mmc 0:1 0x83000100 TEST.TXT',
                         b'md.b 0x83000100 0x10',
                         b'mw.l 0x83000000 0xe3a00000',
                         b'mw.l 0x83000004 0xe12fff1e',
                         b'fatwrite mmc 0:1 0x83000000 PAYLOAD.BIN 8',
                         b'load mmc 0:1 0x82000000 PAYLOAD.BIN',
                         b'go 0x82000000'):
                uart(line + b'\n')
            uart(b'ums 0 mmc 0:1\n')
            uart(b'setenv stdout serial,usbacm\n')
        print(command('screendump', {'filename': str(build / 'uboot.png'), 'format': 'png'}))
        print(command('human-monitor-command', {'command-line': 'info registers'})['return'][:320])
        output = (build / 'uart.log').read_bytes()
        print(output.decode(errors='replace'))
        if b'\r\nTOTORO_SBL_OK\r\n' not in output:
            raise RuntimeError('U-Boot command response missing')
        if b'> MMC Boot' not in output:
            raise RuntimeError('Volume-key menu navigation failed')
        if b'PIT: block 17 page 0' not in output or b'Phone PIT: ID name' not in output:
            raise RuntimeError('Internal PIT was not read from OneNAND')
        if b'buffer=2048' not in output or b'Page=4096 block=256 KiB' not in output:
            raise RuntimeError('OneNAND BufferRAM/page geometry regression')
        if b'Read 16 raw bytes' not in output or b'ANDROID!' not in output:
            raise RuntimeError('Internal boot range read failed')
        if b'Expected ARM standalone uImage' not in output or b'Phone error: -22' not in output:
            raise RuntimeError('Internal image/range validation failed')
        if sd_args and (b'16 bytes written' not in output or b'16 bytes read' not in output):
            raise RuntimeError('SD read/write test failed')
        if sd_args and b'no mmc device' in output:
            raise RuntimeError('SD menu did not select the deferred MMC device')
        if sd_args and (b'SD power 3.0V:' not in output or
                        b'CD=1' not in output or b'SD ready:' not in output):
            raise RuntimeError('SD power/readback, GPIO card detect or initialization failed')
        if sd_args and b'Application terminated, rc = 0x0' not in output:
            raise RuntimeError('SD binary execution failed')
        if sd_args and b'HSOTG core unavailable' not in output:
            raise RuntimeError('Expected USB-unavailable diagnostic in this QEMU model')
        print('PASS: cold SBL boot and U-Boot command execution')
    finally:
        p.terminate()
        p.wait(timeout=5)
        f.close()
        a.close()
        scratch.cleanup()
