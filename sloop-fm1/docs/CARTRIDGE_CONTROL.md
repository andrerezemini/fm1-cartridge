# CARTRIDGE page

Profile `sh build-cartridge.sh`: defines `FELUCCA_CARTRIDGE=1`, playback RX=1, capture=0 and
CDC=0. The page uses the SysEx of the OTA transport (OTA must be enabled), in its own namespace
(`F0 7D 43 46`).

Tap GLO and use SELECT to reach CARTRIDGE. Knobs 1/2 choose engine/preset, 3 chooses
a parameter and 4 changes its value on the ESP32. The keyboard keeps sending MIDI notes. The
header shows the page number among the GLO pages and, once the cartridge answers, the name of the
firmware it runs (CLIPPER OK, MELODEE OK, FELUCCA OK), or NO AUDIO after the name while the
playback stream is at alt 0; NO LINK means there is no valid answer. The parameter FIRMWARE
(knob 3) and knob 4 pick the ESP32 firmware slot to boot. The knobs accelerate as Sloop's own. The
other pages keep controlling Sloop.

The track on the cartridge's MIDI channel (from the reply) is the cartridge track: its notes go
out over MIDI, its engine stays silent, it shows the cartridge's names, and PRESETS (HOME, TRACKS,
the PRESETS page) browses the cartridge's presets across engines; its LEVEL and MUTE set the USB
return and its meter shows it; Sloop sends the cartridge START, STOP and the clock while it answers.
See the cartridge project's `docs/CARTRIDGE.md`.

The FM-1 and the ESP32 run the same protocol version, or the ESP32 a newer one, read as far as
this version's fields go. Sound changes are made live, without reflashing. The ESP32 keeps each
firmware's sound in its own flash; it is not part of the Sloop projects. The catalogue
(engines, presets and parameters) comes from the ESP32; Sloop does not port the deep editors of
the synthesizers.

Implementation: `cartridge.c`, `cartridge_wire.h`, `ui_cartridge.c`. Catalogue and formatting on
the ESP32, without duplicating tables on the FM-1. Bounded requests/replies, revision and
sequence, idempotent retries, discarding of stale ACKs and a timeout; with the page hidden, probes
when USB comes up and after every reply, and a liveness query every 5 s once the cartridge has
answered. Opening the page and leaving it each send a query (the second moves the ESP32's cursor
off FIRMWARE, cancelling a switch it armed). The full names appear on four rows, one per knob.

Validation: `tests/run_cartridge_tests.sh` includes `cartridge_ui_test.c`, natively and with
ASan/UBSan. The FM1 Cartridge project also runs this client against the real ESP32 server and DSP
through `tools/test_sloop_cartridge.sh`. These tests simulate the protocols and do not replace
physical validation of the cartridge. Build figures in [CARTRIDGE_BUILD.md](CARTRIDGE_BUILD.md).
