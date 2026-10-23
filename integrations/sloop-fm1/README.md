# Sloop for the ESP32 cartridge

The FM-1 runs [Sloop](https://github.com/isod89/sloop-fm1) 2.7.1 (upstream commit
`4ec1ac829fd2bdf2c571ea215fb3d62477d01220`) with the cartridge profile. The modified tree lives in
[`sloop-fm1/`](../../sloop-fm1/) and is edited there. This folder holds its provenance:

- [`sloop-fm1-cartridge.patch`](sloop-fm1-cartridge.patch): the whole difference between upstream
  and `sloop-fm1/` (15 files added, 14 modified), as a `git diff --binary`.
- [`manifest.json`](manifest.json): the upstream commit and tree, the patch's SHA-256, the git tree
  id the patch produces (equal to `sloop-fm1/` in this repository), the list of files, the build
  flags and the figures of the package built from it.

## What the patch adds

- **USB audio playback into the FM-1:** a UAC1 playback interface on EP 0x04 OUT (PCM16 stereo,
  44,100 Hz, adaptive isochronous), a ring buffer with a Hermite ASRC that follows the host's
  clock, and a single stereo sum into the mixer before the global effects and limiter. Details in
  [CARTRIDGE_USB_RETURN.md](../../sloop-fm1/docs/CARTRIDGE_USB_RETURN.md).
- **HOME → USB menu:** **USB RETURN** (gain and mute of the return) and **CHARGE** (holds the
  FM-1's internal charger off while the cartridge answers, so the cartridge's cell does not charge
  the FM-1's battery).
- **GLO → CARTRIDGE page:** engine, preset, parameter and value of the cartridge's synthesizer on
  the four knobs, the running firmware's name, and the FIRMWARE parameter that picks the ESP32's
  firmware slot; protocol version 3 in `firmware/src/cartridge_wire.h`, byte-identical to
  [`core/include/fm1/cartridge_wire.h`](../../core/include/fm1/cartridge_wire.h). Guide in
  [docs/CARTRIDGE.md](../../docs/CARTRIDGE.md).
- **Cartridge track:** the track on the cartridge's MIDI channel sends every note its engine would
  play (keys, chords, arpeggiator, steps, rolls, takes) as MIDI on its channel, keeps that engine
  silent and out of the mix, shows the cartridge's engine and preset, and turns PRESETS into the
  cartridge's preset browser (`core.h`, `voice.c`, `seq.c`, `fx.c`, `cartridge.c`, `ui_input.c`,
  `ui_draw.c`, `ui_studio.c`).
- **Build profile and tests:** `build-cartridge.sh` (`FELUCCA_CARTRIDGE=1`, `FELUCCA_UAC_RX=1`,
  `FELUCCA_UAC=0`, `FELUCCA_CDC=0`), an optional diagnostic footer (`FELUCCA_CARTRIDGE_DIAG=1`)
  and `tests/run_cartridge_tests.sh`. Details in
  [CARTRIDGE_BUILD.md](../../sloop-fm1/docs/CARTRIDGE_BUILD.md).

## Build and test

Follow upstream's `sloop-fm1/BUILDING.md` for Docker, the JieLi toolchain and the AC79 SDK, then:

```sh
cd sloop-fm1
sh build-cartridge.sh                          # build/felucca.fwsc; never flashes
sh tests/run_cartridge_tests.sh                # native and ASan/UBSan
cd .. && sh tools/test_sloop_cartridge.sh native   # Sloop's UI against the real ESP32 server
```

The package's memory figures and hashes are in [manifest.json](manifest.json) and
[CARTRIDGE_BUILD.md](../../sloop-fm1/docs/CARTRIDGE_BUILD.md): app 579,952 B of 581,564 B
(1,612 B free); RAM, low RAM and pool within the upstream limits. Installing it on the FM-1, with
a backup of its projects and settings, is described in [docs/FLASHING.md](../../docs/FLASHING.md).

## Reproduce `sloop-fm1/` from upstream

```sh
git clone https://github.com/isod89/sloop-fm1.git sloop-upstream
cd sloop-upstream
git checkout --detach 4ec1ac829fd2bdf2c571ea215fb3d62477d01220
git apply /path/to/fm1-cartridge/integrations/sloop-fm1/sloop-fm1-cartridge.patch
git add -A && git write-tree     # prints the result_tree of manifest.json
```

## Known upstream limits

The full upstream suite (`sh tests/run_tests.sh`) reports two static budget failures that the
upstream revision already has (`drums_mix` 6,793 against 6,153 and `fm1_alnk0_irq` 262 against
174, 268 upstream); no golden or budget is changed to hide them, and the 182 reference renders
pass. The capture profile (`FELUCCA_UAC=1`) together
with playback is exercised in simulation only. The physical tests still open on the FM-1 (stereo
L/R, latency, resampler cost, the charger control on the SoC) are listed in
[docs/ROADMAP.md](../../docs/ROADMAP.md).
