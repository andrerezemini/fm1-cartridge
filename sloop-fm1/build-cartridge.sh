#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Build the FM-1 USB playback prototype. This command never flashes hardware.
# Use PYTHON=/path/to/venv/bin/python if Pillow is installed in a virtualenv.
set -eu
cd "$(dirname "$0")"
# The ESP32 cartridge supplies DSP audio; its MIDI link remains bidirectional.
# Disable the FM-1 capture stream and CDC console to fit the app flash slot.
# Leave flash storage, OTA, the local engines and TRS MIDI at their defaults.
export FELUCCA_CARTRIDGE=1
export FELUCCA_UAC_RX=1
export FELUCCA_UAC=0
export FELUCCA_CDC=0
exec sh ./build.sh "$@"
