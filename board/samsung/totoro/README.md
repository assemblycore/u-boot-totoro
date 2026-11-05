# Samsung Galaxy Y GT-S5360 / BCM21553

Experimental U-Boot loaded by the original Samsung SBL from the phone's
boot partition. The display, volume/Home buttons, USB CDC ACM console and
microSD reading have been confirmed on a real GT-S5360. SBL still initializes
RAM and the panel. This port is not a replacement for SBL.

This revision removes the logo, retains the 8x16 console font and uses all
240x320 pixels (30 columns, 20 rows). It adds the requested boot menu,
Fastboot and an experimental read-only OneNAND/PIT path. SD Mass Storage is
also directly accessible from the main menu (seven entries).

Internal PIT and logical STL reading are confirmed on the phone. Internal
read/write and logical USB storage now use the resident stock SBL's FSR
library, with code CRC checks for the reference and XXMK1 firmware versions.
Single-volume internal USB mounting is confirmed on the phone. Multi-volume
USB export and script boot are new and need hardware testing; QEMU checks
cannot establish physical USB or NAND compatibility. Filesystem access on the
PC requires support for the volume's existing format; param_lfs is Samsung's
logo container, not FAT. Native raw access remains read-only.

## Build and phone image

From this U-Boot checkout, install arm-none-eabi GCC/binutils, make, flex,
bison and the normal U-Boot host dependencies, then run:

```sh
sh board/samsung/totoro/build.sh
```

Outputs in `build-totoro/`:

| File | Purpose |
| --- | --- |
| `boot.img` | Android Boot Image v0 container accepted by stock SBL |
| `totoro-uboot-odin.tar` | USTAR containing only `boot.img` |
| `u-boot.bin`, `u-boot` | Raw U-Boot and ELF for debugging |
| `SHA256SUMS` | SHA-256 of the packaged outputs |

The container has 4096-byte pages, no ramdisk and a kernel load address of
0x81608000. Its first 4096 kernel bytes contain an ARM entry shim with the
zImage magic SBL expects. The shim disables MMU/caches and enters U-Boot at
0x81609000. The header uses the legacy SHA-1 image ID. Keeping this container
format does not enable Android OS loading.

The TAR is for the Odin PDA/AP slot, without repartitioning. It contains no
PIT, SBL or calibration partitions. Keep the previous working boot image
for restoration. The scripts never flash a physical phone.

## Menu and controls

```text
*** U-Boot Boot Menu ***

Fastboot
MMC Boot
SD Card Boot
MMC - Mass Storage
SD - Mass Storage
U-Boot console
USB Console

Press UP/DOWN to move,
ENTER to select
```

Volume Up/Down moves the selection; Home selects. In QEMU use PgUp/PgDn/Home.
UART and USB terminal input accepts arrow keys, w/s and Enter.
`q` exits the menu to the current console. Type `totoro` to reopen it.

The footer wraps across two lines to fit the display without shrinking the
font. Navigation updates only two selection markers on the LCD. UART/ACM
receives a line naming the selected item. Home must be released after choosing
an action before it can cancel that action.

| Item | Behavior |
| --- | --- |
| Fastboot | Receives up to 16 MiB into RAM at 0x82000000; flash/erase disabled |
| MMC Boot | Selects an STL filesystem volume, then boot.scr or another .scr file |
| SD Card Boot | Selects a card partition, then boot.scr or another .scr file |
| MMC - Mass Storage | Exports all PIT entries at once: 4 logical STL volumes R/W, other nominal raw ranges read-only |
| SD - Mass Storage | Exports the entire microSD read/write, including its partition table |
| U-Boot console | Returns to the LCD/UART command prompt and releases ACM |
| USB Console | Starts CDC ACM, retaining LCD/UART output and input |

The two MMC labels follow the requested menu wording. **The internal flash
is OneNAND, not an eMMC device.** `mmc 0` remains the removable microSD;
internal commands use `phone` and never silently fall back to that card.

Selecting another USB function releases the ACM console first. This
disconnects the PC's terminal. Fastboot, mass storage and ACM are alternative
USB modes, not simultaneous interfaces.

## microSD files, payloads and USB storage

SDHC3 is initialized only when needed, so a missing card cannot block the
main menu. For manual access:

```text
sdinit
mmc list
mmc part
ls mmc 0:1 /
```

The phone has already returned a DOS partition table and listed files with
these commands. The boot menu lets you choose a partition or a whole-card
filesystem (`0:0`), then a script in the root, /boot, or the file browser.
FAT and ext4 reading are enabled; FAT writing is enabled.

Place `/payload.bin` or `/payload.elf` on that partition.

```text
load mmc 0:1 0x82000000 /payload.bin
go 0x82000000
```

BIN must contain ARM code linked for 0x82000000. `go` uses U-Boot's standalone
calling convention and stack; it is not a Linux kernel entry protocol.
ELF is staged at 0x82000000 and loaded with `bootelf`; link its segments into
0x83000000..0x83ffffff so they do not overlap its staging buffer, U-Boot,
or the runtime heap/stack near 0x88000000. The ELF entry/load addresses come
from the file and are trusted.

For writable file exchange, select SD Card Boot → SD - Mass Storage, or:

```text
sdinit
ums 0 mmc 0:1
```

The PC receives the entire SD card, including its partition table (`ums 0 mmc 0`).
Its partitions appear as e.g. /dev/sdb1, /dev/sdb2. Eject/unmount
it on the PC before pressing Home on the phone. Do not access the filesystem
from U-Boot while it is exported. This USB storage path still needs hardware
verification; SD file read/write and standalone execution pass QEMU checks.

SD initialization enables MAX8986 DLDO1 at 3.0V through polled BSC1, preserving
ALDO1's bits in the shared register and verifying the writes. GPIO16 supplies
active-low card detect. The driver configures pinmux, pull-ups, 6mA drive,
a 52MHz source clock and the Broadcom SDHC3 wrapper; generic SDHCI uses PIO,
a 4-bit bus and a maximum card clock of 25MHz.

## Internal PIT and raw OneNAND access

### SD appears read-only on Linux

The phone log supplied for this revision reports `Write Protect is off`,
and `lsblk` reports `RO=0`. The gadget already permits writes; that output
also shows no mountpoint. FAT has no Unix ownership on disk, so a mount
owned by root can look unwritable to an ordinary user. After choosing the
actual SD partition from `lsblk`, use your desktop's mount action or:

```sh
udisksctl mount -b /dev/sdX1
findmnt -S /dev/sdX1 -o TARGET,FSTYPE,OPTIONS
```

Replace sdX1 with the card's actual partition. The old single-volume export
used /dev/sdX without a partition suffix. If mount options include `ro`, inspect
`sudo dmesg | tail -40` after mounting; FAT errors can force a read-only mount.
Do not override that or repair a mounted filesystem. For a filesystem mounted
`rw` but owned by root, FAT mount options `uid=<your UID>,gid=<your GID>,umask=022`
give your user write permission. No driver write-protect override is needed.

### Filesystem boot scripts

Both boot menu entries choose a filesystem first, then offer `/boot.scr`,
`/boot/boot.scr`, a browser for directories and `.scr` files, and a file listing.
The browser pages through 12 entries at a time. SD lists up to 13 partition
entries plus whole-card mode; `sdboot <hex partition> [path]` also handles an
explicit partition. Internal boot lists the four STL volumes. A legacy .scr
is a text command script wrapped by mkimage, not a renamed text file:

```sh
build-totoro/tools/mkimage -A arm -T script -C none -n Totoro \
  -d board/samsung/totoro/boot.cmd.example boot.scr
```

Copy `boot.scr` and your `payload.bin` into the filesystem root. The example
loads the payload at 0x82000000 and calls `go`. An ELF script can instead use
`load ${bootdev} ${bootpart} 0x82000000 /payload.elf` followed by `bootelf`.
The payload must fit its RAM region and be linked appropriately.
Scripts run with full U-Boot command privileges, including write commands.

The menu sets `bootdev` (`mmc` or `mtd`), `bootpart` (hex device:partition),
`bootfile`, and `scriptaddr=83000000`. Script images are limited to 1 MiB;
header, payload CRC and embedded script length are checked before execution.
Use payload RAM 0x82000000..0x82ffffff so it does not overwrite the script.
CLI examples:

```text
sdboot 1
sdboot 1 /boot/custom.scr
phone script userdata
phone script userdata /boot/custom.scr
phone bind userdata
ls mtd 17:0 /
load mtd 17:0 0x82000000 /payload.bin
```

`mtd 17:0` means PIT ID 23 (0x17), not SD. The internal IDs are system=14,
param_lfs=15, cache=16, userdata=17 in hexadecimal. `phone bind` must be used
before ordinary filesystem commands; the boot menu binds automatically.
FAT and supported ext filesystems can hold scripts. Stock RFS and Samsung
param_lfs are not supported filesystems for this browser. To repurpose an
existing volume, back up its contents and format that specific writable USB
LUN on the PC as FAT; formatting erases its files but leaves PIT sizes intact.
The service partitions remain read-only. Reformatting param_lfs destroys the
stock logo container used by SBL. No formatting happens automatically.

Logical-sector read/write is available via the resident SBL:

```text
phone read param_lfs 0x82000000 0 1
md.b 0x82000000 0x20
```

Arguments are hexadecimal: partition, destination, logical sector, count.
Only STL IDs 20..23 are accepted. The bridge checks the first 512KiB of SBL
against the reference/XXMK1 code CRC before calling its existing FSR routines.
Unknown firmware is refused. Sessions use the normal read/write access mode.
An already open SBL session is closed and reopened to obtain its exact logical
capacity. The session stays open across reads, writes and USB reconnects;
each of the four STL volumes keeps its own session, including when the PC
alternates requests between LUNs. Reopening it for every
USB request can exhaust the resident SBL heap (`STL open: 80010001`). Reboot
the phone after installing this fix to clear an already exhausted heap.
Writes flush the optional FSR write buffer when present. Partitions without
that buffer write through STL directly. QEMU regression checks repeated
reads and full/partial-page write persistence across a reset on a disposable
flash image. USB transfers on the actual phone still require hardware testing.
No firmware binary is included or patched. This is a firmware-dependent bridge,
not a standalone BML/STL port. FAT/ext filesystem reads use a U-Boot block
device backed by this bridge; the stock Samsung RFS format is not supported
by U-Boot's filesystem layer.

`phone write <name> <RAM address> <sector> <count>` writes exactly the selected
logical sectors from RAM. All numbers are hexadecimal; each sector is 512
bytes. The buffer must fit within 0x82000000..0x82ffffff. Range overflow and
non-STL partitions are rejected. Sector writes overwrite filesystem data;
use the USB filesystem workflow for ordinary file copying:

```text
phone ums userdata
```

This command exports one volume. MMC - Mass Storage / `phone ums-all` exports
all 15 stock PIT entries simultaneously as separate USB LUNs. The host sees
separate disks, identified by their partition names in `lsblk -o NAME,SIZE,RO,MODEL`.
The four STL volumes are read/write; the other 11 entries are raw read-only
diagnostic ranges, without BML remapping. This does not expose a writable PIT
or let a PC repartition OneNAND. All STL volumes must open successfully before
the multi-LUN gadget starts; failures are reported rather than substituting raw data.
Eject/unmount every volume on the PC before Home.
The USB disk contains the selected logical volume, without an added partition
table. The phone's existing filesystem format is preserved. `param_lfs` has a
Samsung-specific format and will not appear as a normal folder on the PC.

SD - Mass Storage exports `mmc 0` read/write. For SD files in the console,
use `load mmc 0:1 0x82000000 /file.bin` and
`fatwrite mmc 0:1 0x82000000 /file.bin <hex-byte-count>` after `sdinit`.
Direct SD sector I/O is available through `mmc read` / `mmc write`.

```text
phone part
phone read-raw boot 0x82000000 0 0x40
md.b 0x82000000 0x40
```

Addresses, offsets and lengths in `read-raw` are hexadecimal. The destination
must lie in 0x82000000..0x82ffffff. Reads reject partition/range overflow and
uncorrectable ECC; each native page wait has a timeout. The raw path has no
program/erase/unlock operation; logical writes go through SBL's FSR only.

The initial hardware path expects Samsung 512MiB OneNAND, 4096-byte pages,
64 pages per eraseblock and a 16-bit DataRAM interface at 0x00400000. It reads
device ID and geometry rather than treating a missing device as valid flash.
On the phone, F003 reports a 2048-byte data buffer. For device 0050 (4Gbit,
non-DDP), the full page is twice that size, as in U-Boot's upstream OneNAND
driver. The diagnostic now distinguishes buffer size from page size.
It searches for PIT in nominal systemdata block 17, then the final 44 blocks.
If no valid PIT is found it reports an error, without inventing a table.

PIT entries provide IDs, names and lengths in 256KiB units. They do **not**
supply a complete physical-to-logical mapping. Sequential nominal starts
below describe the stock layout used to construct this project's QEMU image:

| PIT name | ID | Nominal first block | Blocks |
| --- | ---: | ---: | ---: |
| bcm_boot | 0 | 0 | 1 |
| loke | 1 | 1 | 8 |
| loke_bk | 2 | 9 | 8 |
| systemdata | 3 | 17 | 1 |
| modem | 4 | 18 | 50 |
| param_lfs | 21 | 68 | 20 |
| boot | 5 | 88 | 20 |
| boot_backup | 6 | 108 | 20 |
| system | 20 | 128 | 920 |
| cache | 22 | 1048 | 160 |
| userdata | 23 | 1208 | 789 |
| efs | 7 | 1997 | 1 |
| sysparm_dep | 8 | 1998 | 1 |
| umts_cal | 9 | 1999 | 1 |
| cal | 10 | 2000 | 4 |

BML can replace physical blocks on a real device. STL additionally maps
logical sectors for filesystem partitions. Consequently, reading a nominal
raw range is not equivalent to reading `/dev/bml*` or `/dev/stl*`.
A recovered PIT copy also does not establish which BML mapping is current.
The logical `phone read`, `phone write` and `phone ums` paths use SBL's FSR
to perform those translations. The raw diagnostic path does not.

For a raw diagnostic export:

```text
phone ums-raw boot
```

The device reports write protection to the host and its write callback
rejects writes. The selected nominal range has 512-byte USB sectors, without
an added MBR/GPT or STL translation. A PC may see no filesystem; do not format
it. These raw ranges also appear as read-only LUNs in the all-partitions export.

## Booting your own code from internal memory

The legacy `phone boot <name>` CLI expects an **uncompressed ARM standalone uImage** at offset
zero of the selected nominal range. Both header and payload CRC32 are
checked; the load address must be 0x82000000, the payload must fit within
16MiB and the partition, and its aligned ARM entry must be inside the payload.

Create that format from code linked for 0x82000000:

```sh
build-totoro/tools/mkimage -A arm -O u-boot -T standalone -C none \
  -a 0x82000000 -e 0x82000000 -n totoro-payload \
  -d payload.bin payload.uimg
```

The reader does not install this file into internal flash. This revision
uses filesystem scripts in the menu instead. Do not replace SBL's boot container with this
uImage: SBL still needs the Android-format U-Boot boot.img to enter this menu.
Existing stock Android images are deliberately rejected by `phone boot`.

QEMU checks a valid standalone image in a disposable flash copy, as well as
bad header CRC, bad payload CRC, an Android container and an out-of-range
read. Loading from a real partition remains dependent on its BML mapping.

## USB console and Fastboot

Choose USB Console, connect a cable and open the new serial device:

```sh
picocom -b 115200 /dev/ttyACM0
```

Windows exposes a COM port. ACM is confirmed working on the phone. Home
cancels the connection wait. `usbdiag` prints core state and saved USB
reset/SETUP counters if enumeration fails. `totoro` reopens the menu and
accepts ACM keyboard input.

Fastboot uses the same DWC2 peripheral controller. Choose Fastboot, then:

```sh
fastboot devices
fastboot getvar max-download-size
fastboot stage payload.bin
fastboot continue
```

`stage` sends the exact file to RAM at 0x82000000 (16MiB maximum).
`continue` exits Fastboot. Return to the menu, open USB Console and execute
`go 0x82000000` for a raw standalone ARM payload linked for that address.
Downloads are volatile. `fastboot flash` and `erase` are disabled.

Do not use the host command `fastboot boot payload.bin` for this raw workflow:
the host can wrap a raw kernel in an Android boot image. The board's custom
Fastboot boot hook is a direct `go 0x82000000`, not an Android image parser.
See the [AOSP Fastboot client](https://android.googlesource.com/platform/system/core/+/refs/heads/main/fastboot/fastboot.cpp)
for the distinction between stage and boot.

DWC2 uses 8-bit UTMI, Broadcom clocks/PHY setup, forced device mode and
external B-session validity because VBUS detection belongs to MAX8986.
This QEMU fork has no device-mode HSOTG model, so USB attempts there report
`HSOTG core unavailable`. Fastboot transfer, mass storage and switching
between USB functions require a phone test. USB IDs are development IDs.

## QEMU through unmodified SBL

Prepare the sibling `qemu-bcm21553` flash image as described in its README.
The original SBL must be at `../reference-firmware/sbl.bin`.

Create a separate U-Boot test pair once:

```sh
cp --reflink=auto ../qemu-bcm21553/local/flash/main.img build-totoro/main.img
cp --reflink=auto ../qemu-bcm21553/local/flash/oob.img build-totoro/oob.img
```

With QEMU stopped, update both boot slots after rebuilding U-Boot:

```sh
dd if=build-totoro/boot.img of=build-totoro/main.img bs=262144 seek=88 conv=notrunc status=none
dd if=build-totoro/boot.img of=build-totoro/main.img bs=262144 seek=108 conv=notrunc status=none
sh board/samsung/totoro/run-qemu.sh
```

Those main/OOB files are constructed emulator artifacts, not phone images.
The launcher opens internal flash read-only. Use the terminal for commands;
Ctrl+A then X exits QEMU.

For automated checks, create a test card once if one does not already exist:

```sh
python3 board/samsung/totoro/make-test-sd.py build-totoro/sd.img
python3 board/samsung/totoro/check-qemu.py
```

The check cold-boots SBL without debugger redirection, exercises volume/Home
navigation including the main-menu SD Mass Storage action, sets the emulated
F003 register to the phone's 2048-byte value for older QEMU binaries,
reads PIT and boot data through OneNAND, checks standalone
uImage execution and rejection cases, writes/reads a FAT file and executes an
ARM function loaded from SD. Native-image fixtures modify only a temporary
copy of main.img and oob.img, removed afterwards. The test writes two logical
pages in that disposable flash, resets QEMU and checks the data CRC after SBL
boots again, then checks rejected writes and the unchanged param.lfs header.
SD file tests modify sd.img.
Screenshots and UART output are in `menu.png`, `uboot.png` and `uart.log`.

## Source map and remaining work

| Source | Responsibility |
| --- | --- |
| `totoro.c` | LCD console, buttons, menu, GPT timer and legacy UART |
| `storage.c` | Deferred SDHC3 binding, pinmux/clocks and SD diagnostics |
| `power.c` | BSC1 transactions and MAX8986 SD power |
| `usb.c` | Broadcom DWC2 PHY/controller setup and USB diagnostics |
| `phone.c` | OneNAND/PIT, raw read-only access, SBL STL read/write, USB export and standalone boot |
| `sbl-entry.S` | SBL-to-U-Boot ARM entry shim |
| `pack-boot.py` | SBL-compatible boot.img and Odin TAR packaging |

Environment variables are volatile; `reset` currently halts. Caches are off.
The port declares a conservative 128MiB RAM window, uses embedded DT and
legacy serial registration. Touch, charger management, a standalone BML/STL
implementation, local internal filesystem mounting and hardware reset remain
unimplemented. STL access currently depends on a recognized resident SBL.

Hardware references from the vendor kernel:

- [hardware addresses](https://github.com/CyanogenMod-ARMv6/android_kernel_samsung_bcm21553-common/blob/cm-11.0/arch/arm/mach-bcm215xx/include/mach/hardware.h)
- [GPT timer](https://github.com/CyanogenMod-ARMv6/android_kernel_samsung_bcm21553-common/blob/cm-11.0/arch/arm/plat-bcmap/gpt.c)
- [SDHC3 board setup](https://github.com/CyanogenMod-ARMv6/android_kernel_samsung_bcm21553-common/blob/cm-11.0/arch/arm/mach-bcm215xx/sdio.c)
- [USB clocks](https://github.com/CyanogenMod-ARMv6/android_kernel_samsung_bcm21553-common/blob/cm-11.0/arch/arm/mach-bcm215xx/clock-21553.c)
- [PHY board configuration](https://github.com/CyanogenMod-ARMv6/android_kernel_samsung_bcm21553-common/blob/cm-11.0/arch/arm/mach-bcm215xx/board-bcm21553.c)
- [USB core bring-up](https://github.com/CyanogenMod-ARMv6/android_kernel_samsung_bcm21553-common/blob/cm-11.0/drivers/usb/gadget_brcm/dwc_otg_cil.c)
- [MAX8986 register map](https://github.com/CyanogenMod-ARMv6/android_kernel_samsung_bcm21553-common/blob/cm-11.0/include/linux/mfd/max8986/max8986.h)
- [DLDO1 voltage/mode mapping](https://github.com/CyanogenMod-ARMv6/android_kernel_samsung_bcm21553-common/blob/cm-11.0/drivers/regulator/max8986-regulator.c)
- [BSC I2C transactions](https://github.com/CyanogenMod-ARMv6/android_kernel_samsung_bcm21553-common/blob/cm-11.0/drivers/i2c/busses/i2c-bcm1161.c)
