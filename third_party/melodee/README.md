# Vendored Melodee DSP

Origin: [keremimo/melodee](https://github.com/keremimo/melodee/tree/6a608ba122ac42293908d394e26713e31ec52fb0),
revision **6a608ba122ac42293908d394e26713e31ec52fb0** (tag `v1.5.1`, Melodee 1.5.1). Melodee is a
fork of Felucca (Leo Kuroshita / Hügelton Instruments) by Kerem Kilic (Ellic Studio); the header of
each file identifies the authorship.

This directory contains the transitive subset of code that the upstream `tests/hostsim.c` compiles
to render the DSP without hardware, without its USB device and TRS MIDI drivers (`usb.c`,
`midi_uart.c`), plus `build_options.h` (the firmware's build defaults): `core.h` and
`performance_options.h`, the engines (PROPHET, FM6, CZ-1, SID and DRUM 808/909/SID are offered;
MIDI Track, `eng_midi.c`, is compiled but not offered; the retired ones remain in `engines.c` as
silent entries), parameters, modulation matrix, voices, the mixer, the shared effect rack of three
slots (ROOM, CHORUS and the filter effects, the SLOOP reverb and inserts of `fx_sloop_reverb.h` and
`fx_sloop_insert.h`), the track note inserts NOTE TAIL and NOTE ECHO (`midi_insert.c`), the master
punch (`punch.c`), the audio memory arena (`resources.c`), the sequencer with patterns, recording,
chords (AUTO and AUTO2: `chord_auto.c`, `chord_melody.c`), MIDI control, the MIDI controller knob
profiles (`midi_knobs.c`, off by default), the MIDI Track output (`midi_track.c`) and the clock,
and `libc.c`. The public adapter is `../../synth/melodee_synth.c` (interface
`../../synth/fm1_synth.h`; see [PORTS.md](../../docs/PORTS.md)). The port uses the original
**44,100 Hz** tables, blocks of 32 frames, floating-point FM6 as upstream and one part
(`song.ntrk = 1`) of the instrument's four.

## Changes and exclusions

- Only change to an upstream file: `firmware/src/resources.c` accepts the host arena sizes
  by macro (`RESOURCE_HOST_WORDS`, `RESOURCE_HOST_LOW_WORDS`, `RESOURCE_HOST_CACHE_WORDS`;
  the upstream values remain the default). The adapter reserves 64 KB + 8 KB for one part,
  in internal RAM on the ESP32.
- The adapter compiles with `MELODEE_DUAL_CORE=0`: the serial path that upstream uses when the
  AC7911 second core is not available (Prophet with 5 voices, like upstream `P5_POLY` in that
  mode), and with `MELODEE_RAM_AUDIO=0`, the serial admission policy of `tests/hostsim.c` (a
  budget of 16 voice units). `MELODEE_USB_AUDIO`, `MELODEE_USB_HOST`, CDC, flash, OTA, UI, LCD and
  loader are not compiled; the `midi_in_q` queue that `seq.c` consumes is fed by
  `melodee_synth_midi`, without the USB ISR timeline (`midi_render_timed = 0`: each event plays in
  the block in which it arrived). Clock pulses are counted before the queue admits them, as
  upstream `usb.c` does.
- `NMIDI=0`: no MIDI-only tracks (upstream's tracks 5 to 8, which only send MIDI out; upstream's
  capacity tests build the same way). MIDI out does not exist on the cartridge: every packet
  upstream would send is dropped.
- `MIDI IN` is set to `SEL` (every message goes to the selected track, part 1) and the adapter
  filters the channel (entry 0 of the remote catalogue, `MIDI CH`). The clock is `USB`.
- `generated/` holds the five tables that the upstream `tools/build.py` generates (`melodee_tables.h`,
  `melodee_fm6.h`, `melodee_cz1.h`, `melodee_prophet_factory.h`, `x0x_drum_tables.h`), produced by
  the vendored generators below with Python 3 and no extra packages (NumPy is not needed). Their
  inputs are in `assets/`: the reSID waveforms (`resid/`), the 64 CZ-1 factory timbres and the
  200 Prophet-5 v1.03 programs (SysEx), as upstream.
- Fonts, icons, UI scales, web pages, the boot loader/stub, the register HAL
  and the upstream tests are not included.

`UPSTREAM_SHA256.json` records the SHA-256 of the **original** files (including the original
`resources.c`); `tools/check_upstream.py`, at the repository root, audits the three subsets (CTest
`upstream_integrity`). Generated files and the READMEs are not part of the manifest.

## Licences and attribution

The Melodee code and the retained Felucca code are **GPL-3.0-only** (`LICENSE`, `LICENSING.md`). `cz_native.c`
preserves the **BSD-3-Clause** notice (MAME uPD933, `LICENSES/BSD-3-Clause-uPD933.txt`); `fm6_core.c` and
`eng_fm6.c` rework MSFA (**Apache-2.0**, `LICENSES/Apache-2.0-msfa.txt`) and Dexed (GPL-3.0-or-later);
`eng_sid.c`/`drum_sid.c` rework reSID (GPL-2.0-or-later, used under GPL-3.0) and package its
waveform samples as data; `phys_dsp.c` (DaisySP) and `phys_symp.c` (Rings) preserve
their **MIT** notices; `punch.c`, `fx_sloop_reverb.h` and `fx_sloop_insert.h` adapt isod89's SLOOP
punch, reverb and pedal effects (`isod89/sloop-fm1` at `4ec1ac829fd2bdf2c571ea215fb3d62477d01220`,
GPL-3.0-only); the Prophet-5 factory programs and the CZ-1 timbres are factory data
of the original instruments, as distributed by upstream. `LICENSES/` is upstream's complete set,
including the texts for files not vendored here (fonts, the web installer's MPL-2.0 ports, the
m-bootloader). The combination is distributed under GPL-3.0-only.

## Reproduction and limits

To regenerate the tables, with Python 3 and no extra packages:

```sh
python3 third_party/melodee/tools/gen_tables.py third_party/melodee/generated/melodee_tables.h
python3 third_party/melodee/tools/gen_fm6_patches.py third_party/melodee/generated/melodee_fm6.h
python3 third_party/melodee/tools/gen_cz1_factory.py third_party/melodee/generated/melodee_cz1.h
python3 third_party/melodee/tools/gen_prophet_factory.py third_party/melodee/generated/melodee_prophet_factory.h
python3 third_party/melodee/tools/gen_909_tables.py third_party/melodee/generated/x0x_drum_tables.h
```

Compile only `synth/melodee_synth.c`, with private include `third_party/melodee/generated`,
`-fwrapv` and `-ffp-contract=off` (like the upstream tests: the floating-point FM6); do not compile
the upstream `.c` files individually. GCC (ESP-IDF) also needs `-Wno-maybe-uninitialized` and
`-Wno-array-bounds` for two false positives (`firmware/main/CMakeLists.txt`). The arena hands out
4-byte-aligned blocks, the alignment of the 32-bit targets; on a 64-bit host the SLOOP inserts'
pointer in them is misaligned for UBSan, so the sanitized native build leaves out its `alignment`
check for this target.

Native validation (`tests/test_synth.c`, `tests/test_cartridge.c`): the 5 engines with non-null audio
on preset 0 (201 PROPHET presets: INIT and the 200 factory programs; 65 CZ-1: INIT TONE and the 64
factory timbres), note-off, channel filter, remote catalogue,
identical rendering with arbitrary call sizes, panic and recovery; every effect of the shared rack
changing the sound of every engine, the shared REVERB's and NOTE ECHO's tails, the effect lists
passing over their unnamed entries. These tests do not establish equivalence with the FM-1
UI/firmware, real-time performance on the ESP32 (Prophet and FM6 are the most expensive engines)
or USB result on the hardware.
