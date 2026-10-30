# Sloop for the ESP32 cartridge

The FM-1 runs [Sloop](https://github.com/isod89/sloop-fm1) 2.8.4 (upstream commit
`91f30182d9095e0ecdb43396554f2ce5a67df179`) with the cartridge profile. The modified tree lives in
[`sloop-fm1/`](../../sloop-fm1/) and is edited there. This folder holds its provenance:

- [`sloop-fm1-cartridge.patch`](sloop-fm1-cartridge.patch): the whole difference between upstream
  and `sloop-fm1/` (16 files added, 24 modified), as a `git diff --binary`.
- [`manifest.json`](manifest.json): the upstream commit and tree, the patch's SHA-256, the git tree
  id the patch produces (equal to `sloop-fm1/` in this repository), the list of files, the build
  flags and the figures of the package built from it.

## What the patch adds

- **USB audio playback into the FM-1:** a UAC1 playback interface on EP 0x04 OUT (PCM16 stereo,
  44,100 Hz, adaptive isochronous), a ring buffer with a Hermite ASRC that follows the host's
  clock (its rate loop reads the occupancy as each DAC burst begins, and a stream starts on a burst's
  first block at its aim), a fade-in at every start, a gain glide, and a
  single stereo sum into the mixer before the global effects and limiter. The AudioControl
  collection lists the streaming interfaces before MIDI, as upstream does for macOS up to 15
  (bcdDevice 4.08). Details in
  [CARTRIDGE_USB_RETURN.md](../../sloop-fm1/docs/CARTRIDGE_USB_RETURN.md).
- **HOME → USB menu:** **USB RETURN** (gain and mute of the return) and **CHARGE** (ON by default;
  AUTO holds the FM-1's internal charger off while the cartridge answers, so the cartridge's cell
  does not charge the FM-1's battery). The menu has six sections (SCREEN, LIGHTS, AUDIO, SEQ, USB,
  SYS); SYSTEM's tab reads SYS so that they fit the screen.
- **GLO → CARTRIDGE page:** engine, preset, parameter and value of the cartridge's synthesizer on
  the four knobs, the running firmware's name, and the FIRMWARE parameter that picks the ESP32's
  firmware slot; protocol version 3 in `firmware/src/cartridge_wire.h`, byte-identical to
  [`core/include/fm1/cartridge_wire.h`](../../core/include/fm1/cartridge_wire.h). Guide in
  [docs/CARTRIDGE.md](../../docs/CARTRIDGE.md).
- **Cartridge track:** the track on the cartridge's MIDI channel sends every note its engine would
  play (keys, chords, arpeggiator, steps, rolls, takes) as MIDI on its channel, keeps that engine
  silent and out of the mix, ends with an All Notes Off what its keys held when it takes the track,
  sets the return's level from its LEVEL, MIDI EXPRESSION and MUTE, sends the cartridge the clock
  while it answers (START on the next bar line when it comes back while the song plays), shows the
  cartridge's engine and preset, and turns PRESETS into the cartridge's preset browser
  (`core.h`, `voice.c`, `seq.c`, `fx.c`, `audio.c`, `usb.c`, `cartridge.c`, `ui_input.c`,
  `ui_draw.c`, `ui_studio.c`). Autosave and section saves wait while the cartridge sounds
  (`project.c`); the editor's sample-slot writes are refused while the song plays (`editor.c`).
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
[CARTRIDGE_BUILD.md](../../sloop-fm1/docs/CARTRIDGE_BUILD.md): app 574,044 B of 581,564 B
(7,520 B free); RAM, low RAM and pool within the upstream limits. Installing it on the FM-1, with
a backup of its projects and settings, is described in [docs/FLASHING.md](../../docs/FLASHING.md).

## Reproduce `sloop-fm1/` from upstream

```sh
git clone https://github.com/isod89/sloop-fm1.git sloop-upstream
cd sloop-upstream
git checkout --detach 91f30182d9095e0ecdb43396554f2ce5a67df179
git apply /path/to/fm1-cartridge/integrations/sloop-fm1/sloop-fm1-cartridge.patch
git add -A && git write-tree     # prints the result_tree of manifest.json
```

## Known upstream limits

The full upstream suite (`sh tests/run_tests.sh`) reports four static budget failures that the
upstream revision has on its own build too (`sample_render` 593 against 126, `grain_render` 2,387
against 1,734, `drums_mix` 6,811 against 6,153 and `fm1_alnk0_irq` 262 against 174, 268
upstream); no golden or budget is changed to hide them, and the 182 reference renders pass. The capture profile (`FELUCCA_UAC=1`) together
with playback is exercised in simulation only. The physical tests still open on the FM-1 (stereo
L/R, latency, resampler cost, the charger control on the SoC) are listed in
[docs/ROADMAP.md](../../docs/ROADMAP.md).
