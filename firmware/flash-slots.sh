#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Write the bootloader, the three-slot partition table and one firmware per slot in a single
# esptool session, then start from ota_0 with a clean otadata. Never part of a build: run it
# deliberately, with the FM-1 disconnected and the ESP32 in its USB bootloader (BOOT held at RESET).
#   ./flash-slots.sh -p /dev/cu.PORT [-b 460800] ota_0=build-clipper-4m2m [ota_1=build-melodee-4m2m] [ota_2=build-felucca-4m2m]
#   ./flash-slots.sh -p /dev/cu.PORT ota_1=build-melodee-4m2m        # one slot only: no table, no otadata erase
# Build directories are named relative to firmware/. Every build named must have the same partition
# table, flash settings and rollback (it is checked); the offsets come from that table. Needs ESP-IDF's
# export.sh (esptool.py, IDF_PATH).
set -eu
port= baud=460800 slots=
while [ "$#" -gt 0 ]; do
    case "$1" in
        -p) port=$2; shift 2 ;;
        -b) baud=$2; shift 2 ;;
        ota_[0-9]=*" "*|ota_[0-9]=*"	"*)
            echo "flash-slots.sh: $1: name the build directory relative to firmware/ (no spaces)" >&2; exit 2 ;;
        ota_[0-9]=*) slots="$slots $1"; shift ;;
        *) echo "unexpected argument: $1" >&2; exit 2 ;;
    esac
done
[ -n "$port" ] && [ -n "$slots" ] || { echo "usage: $0 -p PORT [-b BAUD] ota_N=build-dir ..." >&2; exit 2; }
command -v esptool.py >/dev/null 2>&1 && [ -n "${IDF_PATH:-}" ] ||
    { echo 'Activate ESP-IDF 5.3.6 with its export.sh first (esptool.py, IDF_PATH).' >&2; exit 1; }
cd "$(dirname "$0")"
fail() { echo "flash-slots.sh: $*" >&2; exit 1; }
set -- $slots            # the first slot named: its build carries the bootloader and the table
first_dir=${1#*=}
table=$first_dir/partition_table/partition-table.bin
args_of() {   # build dir -> its esptool flash settings (flash mode / frequency / size)
    python3 -c 'import json,sys; a=json.load(open(sys.argv[1])); print(" ".join(a["write_flash_args"]))' "$1/flasher_args.json"
}
for s in $slots; do
    dir=${s#*=}
    [ -f "$dir/flasher_args.json" ] && [ -f "$dir/fm1_cartridge.bin" ] || fail "$dir: not a finished build"
    cmp -s "$dir/partition_table/partition-table.bin" "$table" || fail "$dir: another partition table than $first_dir"
    [ "$(args_of "$dir")" = "$(args_of "$first_dir")" ] ||
        fail "$dir: flash settings ($(args_of "$dir")) differ from $first_dir ($(args_of "$first_dir")): a -4m2m profile mixed with an 8 MB one?"
    grep -q '^#define CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 1$' "$dir/config/sdkconfig.h" ||
        fail "$dir: built without the bootloader's rollback"
done
flash_args=$(args_of "$first_dir")
csv=$(python3 "$IDF_PATH/components/partition_table/gen_esp32part.py" -q "$table")
field() {   # partition name, column (4 offset, 5 size) -> the value from the build's own table
    printf '%s\n' "$csv" | awk -F, -v n="$1" -v c="$2" '$1 == n { gsub(/ /, "", $c); print $c }'
}
bytes() {   # 8K / 1280K / 1M / 0x2000 -> 0x hex
    case "$1" in
        *K) printf '0x%x' $(( ${1%K} * 1024 )) ;;
        *M) printf '0x%x' $(( ${1%M} * 1048576 )) ;;
        *) printf '%s' "$1" ;;
    esac
}
images=
full=0
for s in $slots; do
    name=${s%%=*}; dir=${s#*=}
    off=$(field "$name" 4); [ -n "$off" ] || fail "$name: not in the partition table of $first_dir"
    [ "$name" = ota_0 ] && full=1
    images="$images $off $dir/fm1_cartridge.bin"
done
if [ "$full" = 1 ]; then
    ota_off=$(field otadata 4); ota_size=$(bytes "$(field otadata 5)")
    [ -n "$ota_off" ] && [ -n "$ota_size" ] || fail "$first_dir: the partition table has no otadata"
    images="0x0 $first_dir/bootloader/bootloader.bin 0x8000 $table$images"
    echo "== erasing otadata ($ota_off, $ota_size): the bootloader starts from ota_0, every slot unconfirmed"
    esptool.py --chip esp32s3 -p "$port" -b "$baud" --before default_reset --after no_reset erase_region "$ota_off" "$ota_size"
fi
echo "== writing:$images"
# shellcheck disable=SC2086
esptool.py --chip esp32s3 -p "$port" -b "$baud" --before default_reset --after no_reset write_flash $flash_args $images
echo "== done; the board stays in its bootloader for the read-back (docs/FLASHING.md), then press RESET."
echo "   GLO > CARTRIDGE > FIRMWARE (knob 3), then knob 4 picks the slot to boot."
