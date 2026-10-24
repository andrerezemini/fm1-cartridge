#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Compile the real Sloop UI/USB client against this repo's real Clipper server.
# No device access. The Sloop tree must have its generated headers (build-cartridge.sh, or the
# generators alone: CI). The server's libraries are (re)built here from the current sources.
set -eu
cd "$(dirname "$0")/.."
root=$PWD
sloop=${SLOOP_ROOT:-$root/sloop-fm1}            # the Sloop tree imported into this repository
mode=${1:-native}
opts='-O1 -g'
if [ "$mode" = sanitizer ]; then
    opts="$opts -fsanitize=address,undefined -fno-sanitize-recover=all -fno-sanitize=shift-base,signed-integer-overflow"
    build="$root/out/native-san"
    sanitize=ON
else
    build="$root/out/native"
    sanitize=OFF
fi
cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release -DFM1_SANITIZE=$sanitize >/dev/null
cmake --build "$build" --target clipper_synth fm1_protocol >/dev/null
mkdir -p "$root/out/cartridge-tests"
cmp "$root/core/include/fm1/cartridge_wire.h" "$sloop/firmware/src/cartridge_wire.h"
${CC:-cc} -std=c11 $opts -Wno-int-to-pointer-cast -Wno-unused-function -DCART_REAL_SERVER=1 \
    -I"$sloop/build/gen" -I"$sloop/firmware/src" -I"$sloop/firmware/hal" \
    -I"$root/synth" -I"$root/core/include" "$sloop/tests/cartridge_ui_test.c" \
    "$build/libclipper_synth.a" "$build/libfm1_protocol.a" -lm -pthread \
    -o "$root/out/cartridge-tests/sloop-clipper-$mode"
"$root/out/cartridge-tests/sloop-clipper-$mode" "$root/out/cartridge-tests"
