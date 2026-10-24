#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Focused native tests for USB playback. No device is accessed or flashed.
# Run after sh build-cartridge.sh. SANITIZE=0 skips the sanitizer repetitions.
set -eu
cd "$(dirname "$0")/.."
OUT=build/host/cartridge
mkdir -p "$OUT"
CC="${CC:-cc}"
SANITIZE="${SANITIZE:-1}"
[ -f build/gen/felucca_tables.h ] || {
    echo "run sh build-cartridge.sh first (generated DSP headers required)" >&2
    exit 1
}

run_logged() {
    name="$1"
    shift
    printf '\n== %s\n' "$name"
    if "$@" >"$OUT/$name.log" 2>&1; then
        cat "$OUT/$name.log"
    else
        cat "$OUT/$name.log"
        return 1
    fi
}

compile_run() {
    name="$1"
    source="$2"
    shift 2
    $CC -std=c11 -Wall -Wextra -Wno-unused-function "$source" "$@" -o "$OUT/$name"
    run_logged "$name" "$OUT/$name"
}

# audio.c marks each DAC burst for the return's rate loop (the receiver tests call it as its interrupt does)
grep -q 'uac_rx_burst();' firmware/src/audio.c || {
    echo "FAIL: audio.c no longer marks the DAC burst (uac_rx_burst)" >&2
    exit 1
}

for mode in native sanitizer; do
    if [ "$mode" = sanitizer ]; then
        [ "$SANITIZE" = 1 ] || continue
        opts="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all"
    else
        opts="-O2"
    fi
    # The receiver gets the complete sanitizer set, without DSP exclusions.
    compile_run "receiver-$mode" tests/uac_rx_test.c $opts
    compile_run "receiver-ramp-$mode" tests/uac_rx_ramp_test.c $opts

    # Every combination of playback/capture/CDC is a separate translation unit.
    # T_CDC=2 means compiled CDC with the runtime USB SERIAL option off.
    for rx in 0 1; do
        for capture in 0 1; do
            rates=0
            [ "$capture" = 0 ] || rates="0 1 2"
            for cdc in 0 1 2; do
                for rate in $rates; do
                    compile_run "usb-rx$rx-capture$capture-cdc$cdc-rate$rate-$mode" \
                        tests/uac_rx_usb_test.c $opts \
                        -DT_RX="$rx" -DT_CAPTURE="$capture" -DT_CDC="$cdc" -DT_RATE="$rate"
                done
            done
        done
    done
    for rx in 0 1; do
        for capture in 0 1; do
            rates=0
            [ "$capture" = 0 ] || rates="0 1 2"
            for rate in $rates; do
                without=$(UAC_DUMP=1 "$OUT/usb-rx$rx-capture$capture-cdc0-rate$rate-$mode")
                hidden=$(UAC_DUMP=1 "$OUT/usb-rx$rx-capture$capture-cdc2-rate$rate-$mode")
                [ "$without" = "$hidden" ] || {
                    echo "FAIL: runtime USB SERIAL OFF differs from compiled CDC off" >&2
                    exit 1
                }
            done
        done
    done

    # Existing synth code uses two's-complement signed wrap/left shifts. Match
    # the established upstream sanitizer exclusions only for this integration.
    dsp_opts=""
    [ "$mode" = native ] || dsp_opts="-fno-sanitize=shift-base,signed-integer-overflow"
    for rx in 0 1; do
        compile_run "mixer-rx$rx-$mode" tests/uac_rx_mix_test.c $opts $dsp_opts -DT_RX="$rx" \
            -Ibuild/gen -Ifirmware/src -Ifirmware/hal -lm -pthread
    done
    before=$(sed -n 's/^BASELINE //p' "$OUT/mixer-rx0-$mode.log")
    after=$(sed -n 's/^BASELINE //p' "$OUT/mixer-rx1-$mode.log")
    [ -n "$before" ] && [ "$before" = "$after" ] || {
        echo "FAIL: enabling the receiver changed the inactive mixer output" >&2
        exit 1
    }
    run_logged "mixer-threaded-$mode" env CORES=2 CHAOS=3 "$OUT/mixer-rx1-$mode"

    compile_run "cartridge-ui-$mode" tests/cartridge_ui_test.c $opts $dsp_opts \
        -Wno-int-to-pointer-cast -Ibuild/gen -Ifirmware/src -Ifirmware/hal -lm -pthread
    compile_run "cartridge-diag-$mode" tests/cartridge_diag_test.c $opts $dsp_opts \
        -Wno-int-to-pointer-cast -Ibuild/gen -Ifirmware/src -Ifirmware/hal -lm -pthread
    for capture in 0 1; do
        for cdc in 0 1; do
            compile_run "ui-capture$capture-cdc$cdc-$mode" tests/uac_rx_ui_test.c $opts $dsp_opts \
                -DFELUCCA_UAC="$capture" -DFELUCCA_CDC="$cdc" \
                -Ibuild/gen -Ifirmware/src -Ifirmware/hal -lm -pthread
        done
    done
done
printf '\nALL CARTRIDGE NATIVE TESTS PASSED (hardware untested)\n'
