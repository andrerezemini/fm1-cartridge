#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
set -eu

profile=${1:-midi}
if [ "$#" -gt 0 ]; then shift; fi
case "$profile" in
    midi) defaults=sdkconfig.defaults ;;
    midi-4m2m) defaults='sdkconfig.defaults;sdkconfig.esp32s3-4m2m' ;;
    tone) defaults='sdkconfig.defaults;sdkconfig.tone' ;;
    tone-4m2m) defaults='sdkconfig.defaults;sdkconfig.tone;sdkconfig.esp32s3-4m2m' ;;
    clipper) defaults='sdkconfig.defaults;sdkconfig.clipper' ;;
    clipper-4m2m) defaults='sdkconfig.defaults;sdkconfig.clipper;sdkconfig.esp32s3-4m2m' ;;
    felucca) defaults='sdkconfig.defaults;sdkconfig.felucca' ;;
    felucca-4m2m) defaults='sdkconfig.defaults;sdkconfig.felucca;sdkconfig.esp32s3-4m2m' ;;
    melodee) defaults='sdkconfig.defaults;sdkconfig.melodee' ;;
    melodee-4m2m) defaults='sdkconfig.defaults;sdkconfig.melodee;sdkconfig.esp32s3-4m2m' ;;
    *) echo "Usage: $0 [midi|midi-4m2m|tone|tone-4m2m|clipper|clipper-4m2m|felucca|felucca-4m2m|melodee|melodee-4m2m] [idf.py actions/options...]" >&2; exit 2 ;;
esac

if ! command -v idf.py >/dev/null 2>&1; then
    echo 'Activate ESP-IDF 5.3.6 with its export.sh before running this script.' >&2
    exit 1
fi
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$script_dir"
if [ "$#" -eq 0 ]; then set -- build; fi

# An existing build-<profile>/sdkconfig wins over the defaults (ESP-IDF loads it after them), so a
# changed sdkconfig.* fragment or Kconfig.projbuild would never reach it. Its inputs are hashed: on a
# change the old file moves to sdkconfig.previous (menuconfig edits stay there) and a fresh one is made.
dir=build-$profile
inputs=$(printf '%s' "$defaults" | tr ';' ' ')
hash=$(python3 -c 'import hashlib,sys; h=hashlib.sha256(); [h.update(open(f,"rb").read()) for f in sys.argv[1:]]; print(h.hexdigest())' \
    $inputs main/Kconfig.projbuild)
if [ -f "$dir/sdkconfig" ] && [ "$(cat "$dir/.defaults.sha256" 2>/dev/null || true)" != "$hash" ]; then
    mv "$dir/sdkconfig" "$dir/sdkconfig.previous"
    echo "build.sh: $defaults or main/Kconfig.projbuild changed: $dir/sdkconfig made again (the old one is $dir/sdkconfig.previous)" >&2
fi
mkdir -p "$dir"
printf '%s\n' "$hash" > "$dir/.defaults.sha256"

idf.py -B "$dir" -D "SDKCONFIG=$script_dir/$dir/sdkconfig" -D "SDKCONFIG_DEFAULTS=$defaults" "$@"

# Every profile shares partitions.csv and the bootloader's rollback (FIRMWARE_SLOTS.md, flash-slots.sh).
header=$dir/config/sdkconfig.h
if [ -f "$header" ]; then
    for symbol in CONFIG_PARTITION_TABLE_CUSTOM CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE; do
        grep -q "^#define $symbol 1$" "$header" || { echo "build.sh: $dir lacks $symbol: the image is not a slot firmware" >&2; exit 1; }
    done
fi
