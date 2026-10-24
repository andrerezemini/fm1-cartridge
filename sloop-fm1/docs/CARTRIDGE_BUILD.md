# Building and testing the cartridge profile

This tree is Sloop 2.7.1 (`https://github.com/isod89/sloop-fm1.git`, revision
`4ec1ac829fd2bdf2c571ea215fb3d62477d01220`) with the cartridge profile: USB playback on the
FM-1 for the audio of the synthesizer running on the ESP32-S3 host, the CARTRIDGE page and the
charger control ([USB return](CARTRIDGE_USB_RETURN.md)). Compiling or simulating this firmware
does not demonstrate audio on the hardware; USB enumeration, DMA, clocks and the physical output
depend on bench tests.

## Building

The upstream prerequisites are in [`../BUILDING.md`](../BUILDING.md) (Docker with the JieLi
toolchain and the AC79 SDK). There is no additional global Python dependency. If needed, create
a virtual environment outside version control and install Pillow in it:

```sh
python3 -m venv build/venv
build/venv/bin/python -m pip install Pillow
PYTHON="$PWD/build/venv/bin/python" sh build-cartridge.sh
```

`build-cartridge.sh` sets `FELUCCA_CARTRIDGE=1`, `FELUCCA_UAC_RX=1`, `FELUCCA_UAC=0` and
`FELUCCA_CDC=0`. The FM-1 receives playback and keeps USB MIDI. USB capture of the master and the
CDC console are disabled to leave flash for the receiver; the local engines, projects, OTA and TRS
MIDI keep the upstream defaults. The normal build `sh build.sh` keeps its defaults.
`tools/build.py` also accepts `FELUCCA_UAC`, `FELUCCA_UAC_48K` and `FELUCCA_UAC_RX`, with values
`0` or `1`. `FELUCCA_CARTRIDGE_DIAG=1 sh build-cartridge.sh` adds the receiver counters to the
CARTRIDGE footer.

The artifacts are `build/felucca.bin`, `build/loader/ota.bin` and `build/felucca.fwsc`. The
`.fwsc` package carries the identity `FM-1_900` and is installed with `tools/fm1_install.py` or
the web installer; it is not an upstream release. The build never flashes a device. Do not remove
the slot, RAM or pool checks to make the firmware fit.

Build environment of the figures below:

- JieLi pi32v2 clang **4.0.1**, in `~/.jieli/toolchain`; SHA-256 of the clang binary
  `c45189ee624de0430deb0db02e6dce171fb5538e7fe323e3964dc8ac6236240f`.
- SDK AC79NN V1.2.1, commit `d179b4484759423312073f5fbb232501aa491047`, in
  `~/fw-AC79_AIoT_SDK`; the three hashes expected by the build are accepted.
- Docker `debian:bookworm-slim`, platform `linux/amd64`.
- Native tests: a C11 compiler and Python 3.

## Memory

All mandatory build checks are enabled (flash, RAM, pool, `.ram_text` without calls into XIP,
no references to the ROM, register access restricted to `hal/`, loader RAM-only execution).

| Resource | Upstream 2.7.1 | Cartridge profile | Diagnostic build | Limit |
| --- | ---: | ---: | ---: | ---: |
| App image | 577,920 B | **581,096 B** | 581,244 B | 581,564 B |
| Free flash | 3,644 B | **468 B** | 320 B | |
| RAM `.data + .bss` | 58,892 B | 60,892 B | 60,924 B | 98,304 B |
| Low RAM | 22,772 B | 22,772 B | 22,772 B | 29,488 B after the RAM code |
| Pool | 333,900 B | 333,900 B | 333,900 B | 344,064 B, at least 8,192 B free |
| Free pool | 10,164 B | 10,164 B | 10,164 B | |
| Packaged loader | 6,879 B | 7,007 B | 7,007 B | |
| `.fwsc` package | 610,035 B | 610,163 B | 610,163 B | |

The loader also includes `usb.c`, so it carries the shared EP0 validations. The receiver's
additional memory is in normal RAM; the pool keeps its reserve.

SHA-256 of the cartridge package built from this tree: `felucca.bin`
`05dc238006da0c931d654b17c6664e9467fe6cd8f99551f3babf5410354aca58`, `felucca.fwsc`
`8a9210c271e8b07c4076ac1bd5a07d6dd67d783694070e585345b595bf1a4e8c`. Sources that use
`__DATE__`/`__TIME__` change the hashes of a rebuild.

## Native tests

```sh
sh tests/run_cartridge_tests.sh            # native and ASan/UBSan
SANITIZE=0 sh tests/run_cartridge_tests.sh # native only
```

They cover:

- Receiver: packet validation, order/channels, slot and index wrap, gain, numeric overflow,
  disconnection, silence, reactivation and reset preemption during consumption; each clock
  scenario of -1,000/0/+1,000 ppm runs 120 simulated seconds without underrun or overrun, with
  the rate loop reading the occupancy as each burst begins. A ±10,000 ppm case exceeds the
  available correction and checks the bounded recovery, with no access outside the buffers; it
  produces losses/reprimes, as expected. A closed-loop model (bursts rendered in 10 % to 80 % of
  their period, the host's packets at any phase) starts every stream on a burst's first block at
  the aim, within 0.95 cent; with the render varying from 0.8 to 3.8 ms a burst, 0.26 cent at the
  start and a steady wobble of 0.15 cent peak to peak. The test runner also checks that `audio.c`
  marks each burst (`uac_rx_burst`). These run with the gain applied at once (`UAR_RAMP 0`);
  `tests/uac_rx_ramp_test.c` checks the 5.8 ms fade-in of a start and of a re-prime and the gain's
  glide over a block, for USB RETURN and for the cartridge track's LEVEL.
- USB: 24 combinations of playback on/off, capture on/off, CDC on/off/hidden and capture rates.
  Descriptors, EP0, controls, altsettings and mocked DMA are tested. The descriptors with CDC
  disabled from the menu match byte for byte those of the build without CDC.
- Real mixer: RX disabled or zero gain preserve the reference hash `d098dd7f4a0fcc65`; an active
  return modifies the audio and gives the same result for splits 0, 15 and automatic, including
  with core 1 simulated by a thread with delays. Each block is consumed once.
- Real menu: four capture/CDC variants, knob limits, mute and unmute back to the level it had,
  visual refresh and tab width. The gain does not change the persisted settings word.
- CARTRIDGE page (`tests/cartridge_ui_test.c`): page number among the GLO pages, firmware name
  and slots from the reply, the FIRMWARE row and footer, request/retry/timeout/reconnection,
  stale replies, a newer cartridge's longer reply, the header following the stream (OK / NO
  AUDIO), no LCD writes when idle, and the charger sequence with a simulated HAL (ON by default,
  a charger found off at start turned back on, AUTO).
- Cartridge track (same test): the track on the reply's MIDI channel, its names, notes over MIDI
  with the engine silent and no double send at OUT = SEQ, steps played with OUT at KEYS, STOP,
  MUTE, PRESETS on HOME and on the PRESETS page (crossing into the next and previous engine, and a
  second turn dropped while it crosses, a crossing dropped whole by a page entry), OMNI and the
  handover back to the internal engine, the All Notes Off at takeover, a panic during a preset fade,
  a note-off lost on a full ring, the autosave guard (`cart_quiet`), the battery level with the
  cartridge, the clock and transport with CLOCK at OFF and START on a bar line for a cartridge that
  comes back mid-song, LEVEL and MUTE on the return and the track's meter, the 5 s liveness query
  (a PRESETS turn during it kept), the query when the page is left, a signed value's knob
  acceleration, and recovery within 4 s after each of six outages in a row.

The receiver and USB use the full ASan/UBSan set. Mixer and UI repeat only the upstream exclusions
of `shift-base` and `signed-integer-overflow`, because of the existing two's-complement DSP
idioms. The cartridge project also runs the cartridge client against its real ESP32 server and DSP
(`tools/test_sloop_cartridge.sh` there).

The full upstream suite (`sh tests/run_tests.sh`) reports two static budget failures that the
upstream revision already has: `drums_mix` costs 6,793 against a budget of 6,153 (+10.4%,
tolerance +10%) and `fm1_alnk0_irq` 262 against 174 (268 upstream). No golden or budget is changed
to hide them; the 182 reference renders, the health, voice and routing checks and the remaining
groups pass (the Chromium PWA test is skipped without Playwright). These static limits are not a
measurement of the audio deadline on the FM-1.

## Limits of the cost evidence

`tests/target_budget.py` estimates the assembly cost without changing its budgets; its counting run
on this profile's listing gives `mix_block`, which inlines the receiver's `uac_rx_mix`, 3,661
instructions (3,432 upstream); `mix_units` 249; `uac_rx_service` 143 instructions, 12 inside loops
and no hardware division in those loops.
These numbers do not measure cycles, worst case or deadlines; inlining and heuristic loop
detection change the estimator's weights. The ASRC cost must be measured on the FM-1 with the
engines and effects active.
