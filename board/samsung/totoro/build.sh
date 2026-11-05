#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
cd "$root"
cross=${CROSS_COMPILE:-arm-none-eabi-}
make O=build-totoro CROSS_COMPILE="$cross" totoro_defconfig
make O=build-totoro CROSS_COMPILE="$cross" -j"${JOBS:-8}"
"${cross}gcc" -nostdlib -Wl,-Ttext=0x81608000 -Wl,--build-id=none \
    -o build-totoro/sbl-entry.elf board/samsung/totoro/sbl-entry.S
"${cross}objcopy" -O binary build-totoro/sbl-entry.elf build-totoro/sbl-entry.bin
python3 board/samsung/totoro/pack-boot.py build-totoro
