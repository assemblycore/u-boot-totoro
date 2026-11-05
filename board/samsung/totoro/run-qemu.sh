#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
export TOTORO_ONENAND_IMAGE="$root/build-totoro/main.img"
export TOTORO_ONENAND_OOB="$root/build-totoro/oob.img"
export TOTORO_ONENAND_RW=0
if [ -f "$root/build-totoro/sd.img" ]; then
    set -- -drive "file=$root/build-totoro/sd.img,if=sd,format=raw" "$@"
fi
exec sh "$root/../qemu-bcm21553/scripts/run-totoro-sbl.sh" -display gtk,gl=off "$@"
