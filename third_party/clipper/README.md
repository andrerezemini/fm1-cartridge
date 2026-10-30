# Vendored Clipper DSP

Origin: [isod89/clipper-fm1](https://github.com/isod89/clipper-fm1/tree/fbd8434e7b501ec5d772f5c5f656c25735929369), tag `v1.3`, revision **fbd8434e7b501ec5d772f5c5f656c25735929369** (Clipper 1.3).

This directory contains the transitive subset of code required by the real synthesizer, compiled with `CLIPPER_SYNTH=1`. The build of this repository uses only this subset and does not access the network.

The 23 engines and 407 factory sounds, envelopes, voice allocation, MIDI controls, arpeggiator, mixer, limiter and effects are Clipper code. The public adapter is `../../synth/clipper_synth.h`, which also exposes the common interface `../../synth/fm1_synth.h` (see [PORTS.md](../../docs/PORTS.md)). The port uses the original **44,100 Hz** tables, control in blocks of 32 frames and the upstream polyphony of 8 shared voices.

The DSP of this revision includes upstream's SLOOP 2.8.2/2.8.3 fixes and the 1.2/1.3 bug fixes that reach the synthesizer: a SAMPLE note replayed on a sounding voice fades the voice out and starts again from silence with its attack; the FMX PTCH and ALG name lists end with a 0, which the `F_INT` formatter counts up to (ALG runs to 40); the mix rounds each part instead of flooring it to steps of the output; UNISON gives every oscillator of each extra voice its own start phase and scales FMX's unison to POLY's level; the PHYS strings are tuned to their pitch; NOISE (LFSR), SAMPLE and GRAIN follow the keys up to the top of the keyboard, and a GRAIN note started in the previous note's release gets its grains; VECTOR clears its filter states on a model change; FMX starts a stolen voice from rest; the sequencer leaves the notes a key or MIDI holds to their owner.

## Changes and exclusions

- Only change to an upstream file: `firmware/hal/fm1_xip.h` returns an invalid header in normal memory. User banks are absent; no JieLi physical address is dereferenced.
- The adapter replaces the interrupt/second-core services with serial execution in the calling task and omits the AC7911-specific linker sections, preserving the alignment and `noinline` attributes.
- USB device, registers, LCD, UI, boot firmware, persistence, editor and SysEx protocol are not compiled. Program Change and CC20/MORPH depend on the UI and are explicitly rejected. Factory presets are selected through the API.
- MIDI/clock go into the queue consumed by `seq.c`; it is not an inter-task queue. The application must serialize the events and render calls in its audio task. The adapter defines upstream `usb.c`'s `midi_in_lost` flag and leaves it at 0: its queue never drops a note message without running panic.
- `BOARD_SMK37` is not defined, as in upstream's FM-1 build: the inherited drum machine is neither played nor rendered, and its factory kits stay out of the image. `drum_synth.c` still includes `generated/felucca_drumkits.h`.
- `generated/felucca_samples.h` contains exclusively `gen_samples.clipper_block()`: VOX, GLASS, PAD and AIR synthesized by Clipper itself. The original generator of those sounds is included. Photos, icons, fonts, the Hügelton Sample Pack and the CC0 samples of the groovebox mode are not included.
- `gen_samples.py` is kept for provenance, but its groovebox `main()` is not used. It would require materials deliberately not included. Use only `generate.py` below.
- The remaining upstream files are unchanged. Some conditional branches of the shared code remain to reduce divergence; `CLIPPER_SYNTH=0` is not supported in this subset.

`UPSTREAM_SHA256.json` records the SHA-256 of the **original** files. For `firmware/hal/fm1_xip.h`, the original hash deliberately differs from the port. Files created in this repository and generated tables are not part of that manifest. `tools/check_upstream.py`, at the repository root, compares the subset with the manifest (CTest `upstream_integrity`).

## Licences and attribution

The Clipper/Felucca code is **GPL-3.0-only**, with the original notices preserved in the sources. See `LICENSE` and the original `LICENSING.md`, including the distinctions between code and assets. The msfa code in `fm6_core.c` is **Apache-2.0**, with notice and licence in `LICENSES/Apache-2.0-msfa.txt`. `phys_dsp.c` and `phys_symp.c` preserve the **MIT** notices of DaisySP/Rings. The combination is distributed under GPL-3.0-only.

Main upstream authorship: Leo Kuroshita / Hügelton Instruments, 3dSam (Clipper), and the authors identified in each file. The `cpu1.h` header preserves the attribution to Andre Rezemini; the AC7911 execution HAL is not used. The four SAMPLE/GRAIN sounds are upstream's own source material, synthesized by the GPL program `tools/gen_clipper_sets.py`, with the upstream WAVs and metadata (`assets/clipper-sets/`) included for exact reproduction of the tables.

## Reproduction and limits

The required tables are committed in `generated/`. To regenerate them, with Python 3 and no extra packages:

```sh
python3 third_party/clipper/generate.py
```

To recreate the optional source WAVs, the upstream generator `tools/gen_clipper_sets.py` needs NumPy/SciPy. That is not a requirement for building or for regenerating the normal tables.

Compile only `synth/clipper_synth.c`, with private include `third_party/clipper/generated` and `-fwrapv`. Do not compile the upstream `.c` files individually: the original project also uses a single translation unit.

Native validation: 23 engines with non-null audio, note-off, note-on with zero velocity, panic, queue saturation with recovery, equivalent rendering with arbitrary sizes, and FMX's PTCH and ALG names over their whole range. These tests do not establish equivalence of the whole UI/firmware, real-time performance on the ESP32, power consumption or USB result on the hardware.

On the ESP32-S3, the Clipper profiles use `firmware/main/clipper.lf` to place only the large, mostly sequential delay and wavetable buffers (the upstream `.pool` objects) in PSRAM. Voice, engine, track and effect state stays in internal RAM, because random per-sample access to the 40 MHz quad PSRAM through the data cache is too slow for the 44.1 kHz render budget. It is static placement, not dynamic allocation. Clipper notes play cleanly through the FM-1; the render time per block (`render_max_us`) and the PSRAM access cost under load are not yet measured on the board.
