#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
set -eu

profile=${1:-midi}
if [ "$#" -gt 0 ]; then shift; fi
case "$profile" in
    midi) defaults=sdkconfig.defaults ;;
    tone) defaults='sdkconfig.defaults;sdkconfig.tone' ;;
    tone-4m2m) defaults='sdkconfig.defaults;sdkconfig.tone;sdkconfig.esp32s3-4m2m' ;;
    clipper) defaults='sdkconfig.defaults;sdkconfig.clipper' ;;
    clipper-4m2m) defaults='sdkconfig.defaults;sdkconfig.clipper;sdkconfig.esp32s3-4m2m' ;;
    felucca) defaults='sdkconfig.defaults;sdkconfig.felucca' ;;
    felucca-4m2m) defaults='sdkconfig.defaults;sdkconfig.felucca;sdkconfig.esp32s3-4m2m' ;;
    melodee) defaults='sdkconfig.defaults;sdkconfig.melodee' ;;
    melodee-4m2m) defaults='sdkconfig.defaults;sdkconfig.melodee;sdkconfig.esp32s3-4m2m' ;;
    *) echo "Usage: $0 [midi|tone|tone-4m2m|clipper|clipper-4m2m|felucca|felucca-4m2m|melodee|melodee-4m2m] [idf.py actions/options...]" >&2; exit 2 ;;
esac

if ! command -v idf.py >/dev/null 2>&1; then
    echo 'Activate ESP-IDF 5.3.6 with its export.sh before running this script.' >&2
    exit 1
fi
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$script_dir"
if [ "$#" -eq 0 ]; then set -- build; fi
exec idf.py -B "build-$profile" \
    -D "SDKCONFIG=$script_dir/build-$profile/sdkconfig" \
    -D "SDKCONFIG_DEFAULTS=$defaults" "$@"
