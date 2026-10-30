# Vendored Felucca DSP

Origin: [hugelton/Felucca](https://github.com/hugelton/Felucca/tree/d7b1b7c4099bba28854869ddb256b3d9f634cee8),
revision **d7b1b7c4099bba28854869ddb256b3d9f634cee8** (Felucca 1.7.0, the `v1.7.0` tag), with its two
submodules at the revisions that commit pins:

- [hugelton/Felucca-Engines](https://github.com/hugelton/Felucca-Engines/tree/04ffe903eb87a752305d1e5b10da01b53e4ab004)
  at **04ffe903eb87a752305d1e5b10da01b53e4ab004** (`firmware/engines/`);
- [hugelton/felucca-bsp](https://github.com/hugelton/felucca-bsp/tree/5942e68b89ab9ccd23f2c541d712ca38fe635f11)
  at **5942e68b89ab9ccd23f2c541d712ca38fe635f11** (`firmware/bsp/`, the hardware layer).

This directory contains the transitive subset of code that the upstream `tests/hostsim.c` compiles
to render the DSP without hardware, at the same paths as upstream: `core.h`, the 13 engines (`engines.c`
and `eng_*.c`, the six from Felucca-Engines in `firmware/engines/src`, PHASE among them), parameters,
modulation matrix, voices, SLICER, FX, the sequencer with the MIDI control and the clock, and from
felucca-bsp `chip/libc.c` and `chip/fm1_xip.h` (with the `chip/fm1_cc.h` it includes), which
`eng_sample.c` reads the user sample slots through. The public adapter is
`../../synth/felucca_synth.c` (interface `../../synth/fm1_synth.h`; see
[PORTS.md](../../docs/PORTS.md)). The port uses the original **44,100 Hz** tables, blocks of
32 frames and one part (track 1) of the instrument's four.

## Changes and exclusions

- Only change to an upstream file: `firmware/bsp/chip/fm1_xip.h` returns an invalid header in
  normal memory (as in Clipper). The user sample slots (USR1..3 of SAMPLE, SLICE sources)
  are absent; no JieLi physical address is dereferenced. The modified file stays MPL-2.0, with its
  original notice.
- The adapter compiles with `FELUCCA_CPU2=0` (single core), `FELUCCA_FM4=0` (DIGITAL retired: its
  sounds play as FM6, as upstream) and `FELUCCA_SLICE=1`. The AC7911 second core, the
  USB device driver (`firmware/bsp/chip/usb.c`, which `hostsim.c` also includes), UART MIDI
  (`firmware/src/midi_uart.c`), LCD, UI, storage, OTA, editor and console are not compiled; the
  `midi_in_q` queue that `seq.c` consumes is fed by `felucca_synth_midi`.
- `MIDI IN` is set to `SEL` (every message goes to the selected track, part 1) and the adapter
  filters the channel (entry 0 of the remote catalogue, `MIDI CH`). The clock is `USB`: the
  sequencer and the arpeggiator follow the FM-1's MIDI clock in upstream's fixed musical units, and
  `panic` clears that clock's fractional carries with the rest of its state.
- `generated/` holds the three tables that the upstream `tools/build.py` generates (`felucca_tables.h`,
  `felucca_fm6.h`, `felucca_samples.h`), produced by the vendored generators below with Python 3
  and no extra packages (NumPy is not needed). The engines' own tables come from
  `firmware/engines/tools/gen_engine_tables.py` at its default rate, 44,100 Hz. `felucca_samples.h`
  contains the five CC0 sets (VSCO-2 CE, `assets/samples-cc0/`, with `ATTRIBUTION.txt`) in ADPCM and
  the SLICE BREAK, made from the Hügelton Sample Pack (`tools/gen_waves.py`, CC0).
- Fonts, icons, palettes, web pages, the loader, the rest of felucca-bsp (`chip/`, `boards/fm1/`,
  `loader/`, its tools and tests), `firmware/hal/fm1_uart.h` and the upstream tests are not included.

`UPSTREAM_SHA256.json` records the SHA-256 of the **original** files (including the original
`firmware/bsp/chip/fm1_xip.h`, whose hash deliberately differs from the stub); `tools/check_upstream.py`,
at the repository root, audits the three subsets (CTest `upstream_integrity`). Generated files and this
README are not part of the manifest.

## Licences and attribution

The Felucca code is **GPL-3.0-only** (`LICENSE`, `LICENSING.md`), with the original notices in the sources.
The six engines ANALOG, LOFI, TRIO, WHEEL, NOISE and PHASE, the engine interface and the shared `dsp.c`
are **MIT** (`firmware/engines/LICENSE`, `LICENSES/MIT-Felucca-Engines.txt`). `fm6_core.c` is msfa,
**Apache-2.0** (`LICENSES/Apache-2.0-msfa.txt`); `phys_dsp.c` (DaisySP) and `phys_symp.c` (Rings)
preserve their **MIT** notices. The samples in `assets/samples-cc0/` are CC0 1.0 (Versilian Studios).

The three felucca-bsp files (`firmware/bsp/chip/libc.c`, `fm1_xip.h`, `fm1_cc.h`) are **MPL-2.0**
(`SPDX-License-Identifier: MPL-2.0`; the licence text in `firmware/bsp/LICENSE` and
`LICENSES/MPL-2.0-felucca-bsp.txt`, their authors and origins in `firmware/bsp/NOTICE` and
`firmware/bsp/LICENSING.md`). None of them, nor felucca-bsp as a whole, carries the "Incompatible With
Secondary Licenses" notice of MPL-2.0 Exhibit B, so under MPL-2.0 section 3.3 they can be part of a
larger work distributed under the GPL-3.0 (a Secondary License: a later version of the GPL-2.0).
They stay MPL-2.0 themselves, including the modified `fm1_xip.h`, and their source is in this
directory.

The combination, as upstream's firmware, is distributed under GPL-3.0-only. Main upstream authorship:
Leo Kuroshita / Hügelton Instruments and the authors identified in each file (`CREDITS.md`,
`firmware/bsp/NOTICE`).

## Reproduction and limits

To regenerate the tables, with Python 3 and no extra packages:

```sh
python3 third_party/felucca/tools/gen_tables.py third_party/felucca/generated/felucca_tables.h
python3 third_party/felucca/tools/gen_fm6_patches.py third_party/felucca/generated/felucca_fm6.h
python3 third_party/felucca/tools/gen_samples.py third_party/felucca/generated/felucca_samples.h
```

`gen_samples.py` writes the intermediate Hügelton Sample Pack to `build/genwav` next to
`tools/` (Git ignores it). Compile only `synth/felucca_synth.c`, with private include
`third_party/felucca/generated` and `-fwrapv`; do not compile the upstream `.c` files individually.

Native validation (`tests/test_synth.c`, `tests/test_cartridge.c`): 13 engines with non-null audio
on preset 0, note-off, channel filter, remote catalogue, identical rendering with arbitrary call
sizes, panic and recovery. These tests do not establish equivalence with the FM-1 UI/firmware,
real-time performance on the ESP32 or USB result on the hardware.
