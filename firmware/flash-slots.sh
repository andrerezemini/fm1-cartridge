#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Write the bootloader, the three-slot partition table and one firmware per slot in a single
# esptool session, then start from ota_0 with a clean otadata. Never part of a build: run it
# deliberately, with the FM-1 disconnected and the ESP32 in its USB bootloader (BOOT held at RESET).
#   ./flash-slots.sh -p /dev/cu.PORT [-b 460800] ota_0=build-clipper-4m2m [ota_1=build-melodee-4m2m] [ota_2=build-felucca-4m2m]
#   ./flash-slots.sh -p /dev/cu.PORT ota_1=build-melodee-4m2m        # one slot only: no table, no otadata erase
# Every build named must share partitions.csv (all profiles do). Needs ESP-IDF's export.sh (esptool.py).
set -eu
port= baud=460800 slots=
while [ "$#" -gt 0 ]; do
    case "$1" in
        -p) port=$2; shift 2 ;;
        -b) baud=$2; shift 2 ;;
        ota_[0-9]=*) slots="$slots $1"; shift ;;
        *) echo "unexpected argument: $1" >&2; exit 2 ;;
    esac
done
[ -n "$port" ] && [ -n "$slots" ] || { echo "usage: $0 -p PORT [-b BAUD] ota_N=build-dir ..." >&2; exit 2; }
command -v esptool.py >/dev/null 2>&1 || { echo 'Activate ESP-IDF 5.3.6 with its export.sh first (esptool.py).' >&2; exit 1; }
cd "$(dirname "$0")"
offset_of() {   # slot name -> flash offset from partitions.csv
    awk -F, -v n="$1" '$1 ~ "^"n"$" { gsub(/ /, "", $4); print $4 }' partitions.csv
}
set -- $slots            # the first slot named: its build carries the bootloader and the table
first_dir=${1#*=}
[ -f "$first_dir/flasher_args.json" ] || { echo "$first_dir: not a finished build (flasher_args.json)" >&2; exit 1; }
# the chip settings every build shares (flash mode / frequency / size)
flash_args=$(python3 -c 'import json,sys; a=json.load(open(sys.argv[1])); print(" ".join(a["write_flash_args"]))' "$first_dir/flasher_args.json")
images=
full=0
for s in $slots; do
    name=${s%%=*}; dir=${s#*=}
    [ -f "$dir/fm1_cartridge.bin" ] || { echo "$dir: fm1_cartridge.bin missing" >&2; exit 1; }
    off=$(offset_of "$name"); [ -n "$off" ] || { echo "$name: not in partitions.csv" >&2; exit 1; }
    [ "$name" = ota_0 ] && full=1
    images="$images $off $dir/fm1_cartridge.bin"
done
if [ "$full" = 1 ]; then
    images="0x0 $first_dir/bootloader/bootloader.bin 0x8000 $first_dir/partition_table/partition-table.bin$images"
    echo "== erasing otadata (0xd000, 8 KB): the bootloader starts from ota_0, every slot unconfirmed"
    esptool.py --chip esp32s3 -p "$port" -b "$baud" --before default_reset --after no_reset erase_region 0xd000 0x2000
fi
echo "== writing:$images"
# shellcheck disable=SC2086
esptool.py --chip esp32s3 -p "$port" -b "$baud" --before default_reset --after hard_reset write_flash $flash_args $images
echo "== done. GLO > CARTRIDGE > FIRMWARE (knob 3), then knob 4 picks the slot to boot."
