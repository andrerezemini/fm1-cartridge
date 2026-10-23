# Vendored Felucca DSP

Origin: [hugelton/Felucca](https://github.com/hugelton/Felucca/tree/63f88ec18b71c6ed4371d03d6f04dd93f4193ad3),
revision **63f88ec18b71c6ed4371d03d6f04dd93f4193ad3** (Felucca 1.6.1), with the submodule
[hugelton/Felucca-Engines](https://github.com/hugelton/Felucca-Engines/tree/c46e0086f69bdc26df1cd144dca9e683cc4531dd)
at revision **c46e0086f69bdc26df1cd144dca9e683cc4531dd** (`firmware/engines/`).

This directory contains the transitive subset of code that the upstream `tests/hostsim.c` compiles
to render the DSP without hardware: `core.h`, the 13 engines (`engines.c` and `eng_*.c`, the five from
Felucca-Engines in `firmware/engines/src`), parameters, modulation matrix, voices, SLICER, FX, the
sequencer with the MIDI control and the clock, and `libc.c`. The public adapter is
`../../synth/felucca_synth.c` (interface `../../synth/fm1_synth.h`; see
[PORTS.md](../../docs/PORTS.md)). The port uses the original **44,100 Hz** tables, blocks of
32 frames and one part (track 1) of the instrument's four.

## Changes and exclusions

- Only change to an upstream file: `firmware/hal/fm1_xip.h` returns an invalid header in
  normal memory (as in Clipper). The user sample slots (USR1..3 of SAMPLE, SLICE sources)
  are absent; no JieLi physical address is dereferenced.
- The adapter compiles with `FELUCCA_CPU2=0` (single core), `FELUCCA_FM4=0` (DIGITAL retired: its
  sounds play as FM6, as upstream) and `FELUCCA_SLICE=1`. The AC7911 second core, the
  USB device driver, UART MIDI, LCD, UI, storage, OTA, editor and console are not compiled; the
  `midi_in_q` queue that `seq.c` consumes is fed by `felucca_synth_midi`.
- `MIDI IN` is set to `SEL` (every message goes to the selected track, part 1) and the adapter
  filters the channel (entry 0 of the remote catalogue, `MIDI CH`). The clock is `USB`.
- `generated/` holds the three tables that the upstream `tools/build.py` generates (`felucca_tables.h`,
  `felucca_fm6.h`, `felucca_samples.h`), produced by the vendored generators below with Python 3
  and no extra packages (NumPy is not needed). `felucca_samples.h` contains the five CC0 sets
  (VSCO-2 CE, `assets/samples-cc0/`, with `ATTRIBUTION.txt`) in ADPCM and the SLICE BREAK, made
  from the Hügelton Sample Pack (`tools/gen_waves.py`, CC0).
- Fonts, icons, palettes, web pages, the loader, the register HAL
  and the upstream tests are not included.

`UPSTREAM_SHA256.json` records the SHA-256 of the **original** files (including the original
`fm1_xip.h`, whose hash deliberately differs from the stub); `tools/check_upstream.py`, at the
repository root, audits the three subsets (CTest `upstream_integrity`). Generated files and this
README are not part of the manifest.

## Licences and attribution

The Felucca code is **GPL-3.0-only** (`LICENSE`, `LICENSING.md`), with the original notices in the sources.
The five engines ANALOG, LOFI, TRIO, WHEEL and NOISE and the shared `dsp.c` are **MIT**
(`firmware/engines/LICENSE`, `LICENSES/MIT-Felucca-Engines.txt`). `fm6_core.c` is msfa,
**Apache-2.0** (`LICENSES/Apache-2.0-msfa.txt`); `phys_dsp.c` (DaisySP) and `phys_symp.c` (Rings)
preserve their **MIT** notices. The samples in `assets/samples-cc0/` are CC0 1.0 (Versilian Studios).
The combination is distributed under GPL-3.0-only. Main upstream authorship: Leo Kuroshita / Hügelton
Instruments and the authors identified in each file (`CREDITS.md`).

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
