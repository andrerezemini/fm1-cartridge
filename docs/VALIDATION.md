# Software validation

What the automated tests and builds cover, and what they do not. Physical results and the gates
are in [ROADMAP.md](ROADMAP.md) and [BENCH_TESTS.md](BENCH_TESTS.md).

## Tools

- Native: Apple Clang 21 (`arm64-apple-darwin`) and GCC on Ubuntu (CI), CMake 3.20 or newer,
  Python 3 (no extra packages; the table generators in `third_party/` need none either).
- Firmware: ESP-IDF 5.3.6, target `esp32s3`; `espressif/usb_host_uac` 1.5.0 and its dependencies
  as pinned in `firmware/dependencies.lock`.
- Sloop: Docker with the JieLi pi32v2 toolchain and the AC79 SDK
  ([CARTRIDGE_BUILD.md](../sloop-fm1/docs/CARTRIDGE_BUILD.md)).
- Pinned upstream revisions: Clipper `fbd8434e7b501ec5d772f5c5f656c25735929369` (1.3), Felucca
  `d7b1b7c4099bba28854869ddb256b3d9f634cee8` (1.7.0; Felucca-Engines
  `04ffe903eb87a752305d1e5b10da01b53e4ab004`, felucca-bsp
  `5942e68b89ab9ccd23f2c541d712ca38fe635f11`), Melodee `6a608ba122ac42293908d394e26713e31ec52fb0` (1.5.1),
  Sloop `91f30182d9095e0ecdb43396554f2ce5a67df179` (2.8.4).

## Native tests (CTest)

Eighteen groups (seventeen without Node.js, which `web_manager` needs), run in Release and in Debug with AddressSanitizer + UndefinedBehaviorSanitizer and
`-fno-sanitize-recover=all` (`-DFM1_SANITIZE=ON`). The CI workflows run them on Ubuntu and macOS,
run Sloop's cartridge tests there too, and build every ESP32 profile.

| CTest group | What it checks |
| --- | --- |
| `protocol` | Sloop's real configuration descriptor (175 bytes, SHA-256 `8982eecbb8185e9c04db859e2c290dc224c9bf6b3170d7341613d3f51c1071ee`, exported from Sloop's EP0 with RX=1/capture=0/CDC=0): MIDI IN `0x81`/IF1 and audio OUT `0x04`/IF2/alt1 at 180 bytes are found; every truncation and mutation is rejected; valid and invalid MIDI events; the 180-byte audio packet needs the 192-byte periodic FIFO the firmware configures (it would not fit the 128 bytes of ESP-IDF's balanced default); a newer peer's longer request is read as version 3's, an older version or a broken frame is not |
| `audio_return` | The reference receiver: more than 400,000 frames with order, channels and signed 16-bit samples preserved; 44/45 frames per ms; 120 simulated seconds at −1000, −500, 0, +500 and +1000 ppm without underrun or overrun; the same drift without adaptation fails; recovery from losses and resets |
| `clipper` | Clipper's own adapter API: audio on the 23 engines with preset 0, note-off, velocity zero, panic, queue overflow, identical output for any render call size, the WAVE tables built in bounded steps equal to upstream's for all 16 factory tables (166 blocks, 120 ms of audio), a WAVE note whose table changes coming back through the part's fade-in rather than as a step, and FMX's PTCH and ALG showing their own names over their whole range (ALG past 32 included) |
| `synth_clipper`, `synth_felucca`, `synth_melodee` | The same checks for each port through `fm1_synth.h`: every offered engine sounds with its first factory preset (23 / 13 / 5), note-off and velocity zero, `release()` (a note-on queued before it does not start), All Notes Off and CC 124..127 on the instrument's channel only (sustain pedal held or not), a preset change under a chord (the first twelve presets of every engine) that fades instead of cutting to zero with no step above 3.5 times the sound's own, and DRUM kit changes that do not crash, the MIDI queued before a change kept, the MIDI channel filter and the MIDI CH entry, identical PCM for any render call size, panic and recovery after a queue overflow |
| `cartridge_clipper`, `cartridge_felucca`, `cartridge_melodee` | The cartridge server with each port: every factory preset (407 / 72 / 327) and every exposed parameter range (2,963 / 1,272 / 511), idempotent retries, stale revisions, malformed frames, SysEx framing, a level control changing the real PCM, the FIRMWARE entry with simulated slots (an empty slot is passed over in the turn's direction, returning to the running slot disarms, the busy guard, the cancel on PARAM and on QUERY), the entry kept across a sound change, a MIDI CH change that ends a held note, the read-only audio figures started again by a query before its reply, the state record restored after a fresh start and refused when it comes from elsewhere, in Felucca the sample sources passing over the empty user slots, and in Melodee every effect of the shared rack changing the sound of every engine, the shared REVERB's and NOTE ECHO's tails, the effect lists passing over their unnamed entries, the rack's one REVERB moving to the slot that picks it and MIDI Track not offered |
| `pipeline` | A model of the audio return end to end with the firmware's buffer sizes (read from its sources), the URB refills up to 2 ms late, the FM-1's DAC 300 ppm off the bus and Sloop's real receiver: no gap or underrun at half the core, about 25 ms from render to the FM-1's mixer against 39 ms with a 4 KB ring, 128-frame renders and three-packet URBs, and the longest single render that passes without a gap (at least 15 ms, the figure the docs give) |
| `simulation_clipper`, `simulation_felucca`, `simulation_melodee` | The simulator per port: WAV/JSON of a normal run, a run with faults and a tone; format, frame counts, non-silent audio, silence during the disconnection, resumption and rejected arguments |
| `upstream_integrity` | 236 vendored upstream files of the three subsets checked against their SHA-256 manifests; the one documented change per subset is excluded explicitly |
| `wire_identity` | `core/include/fm1/cartridge_wire.h` and `sloop-fm1/firmware/src/cartridge_wire.h` are byte-identical |
| `catalog` | `web/catalog.json` names the upstream revisions the three adapters, `CMakeLists.txt` and the Sloop manifest use, the `usb_host_uac` version of `firmware/dependencies.lock`, only profiles `firmware/build.sh` knows, and an esptool-js pinned by version and SHA-384 |
| `cartridge_web` | The manager's local server: its fixed paths (traversals, other build files and the rest of the repository refused), the loopback `Host` check, POSTs only from its own `Origin` and as JSON, and candidate-only writes to a copy of the catalogue |
| `web_manager` | `web/espimage.js` (Node.js): the partition table of `firmware/partitions.csv`, `otadata` (its CRC against values an ESP32-S3 wrote, and zlib's), application headers and `esp_app_desc_t`, the names the FM-1 shows, the slot checks (chip, size, project) and MD5 against Node's across the padding boundaries |

In the targets of the upstream DSPs, the UBSan checks `shift-base` and `signed-integer-overflow`
are disabled because of their fixed-point arithmetic, and `-fwrapv` is kept. Bounds, alignment and
the other checks stay active, and the new modules keep the full set. On a 64-bit host Melodee's
target also leaves out `alignment`: its arena hands out 4-byte-aligned blocks, the alignment of the
32-bit targets, and the SLOOP inserts keep a pointer in theirs. This is not a proof that the
original engines are free of undefined behaviour.

Making render output independent of the call size requires each adapter to reset every piece of
DSP state that upstream never re-initialises (the master DC blocker, reverb and delay states,
counters); the `synth_*` groups catch any state that leaks from one render into the next.

## Reference simulation

Clipper, engine 0 / preset 0, 10 simulated seconds, consumer at +400 ppm, adaptation on
(`./build/cartridge_sim --seconds 10` and the same with `--faults`):

| Measure | Normal | With injected faults |
| --- | ---: | ---: |
| WAV frames | 441,176 | 441,176 |
| MIDI events | 520 | 505 |
| Underruns | 0 | 1 |
| Overruns | 0 | 0 |
| Injected losses | 0 | 110 |
| Malformed packets | 0 | 1 |
| Reconnections | 0 | 1 |
| Recorded min/max fill | 235 / 264 | 37 / 267 |

The underrun with faults is expected after the packets stop for 100 ms; the test checks the
resumption instead of hiding the loss. The WAV's playback duration uses the nominal rate and may
differ by the ppm offset from the simulated duration. These numbers are not hardware latency or
loss. Options in [SIMULATION.md](SIMULATION.md).

## Firmware builds

`firmware/build.sh` builds the profiles `midi`, `midi-4m2m`, `tone`, `tone-4m2m`, `clipper`,
`clipper-4m2m`, `felucca`, `felucca-4m2m`, `melodee` and `melodee-4m2m` with the three-slot
partition table and bootloader rollback, and checks both in every build; sizes and hashes are in
[builds.json](validation/builds.json), memory in
[firmware/README.md](../firmware/README.md). The `.lf` linker fragments place only the large delay,
reverb and pattern buffers in PSRAM; the link map confirms the placement, not the speed nor the
presence of the memory on a board.

The disconnection paths follow the code of the UAC component 1.5.0: the application does not treat
an early return of `uac_host_device_close()` during removal as the destruction of the handle, and
waits for the disconnection event before releasing it. Real recovery needs the reconnection tests
on the bench.

## Sloop

- `sloop-fm1/tests/run_cartridge_tests.sh`: the receiver (with its start ramp and gain glide), the
  USB descriptors in 24 profile combinations, the real mixer, the menu, the CARTRIDGE page and the
  cartridge track, natively and with ASan/UBSan; CI runs it after Sloop's Python generators.
- `tools/test_sloop_cartridge.sh native|sanitizer`: Sloop's real UI, SysEx assembler and USB client
  linked against the real cartridge server with Clipper, rebuilt from the current sources (130
  checks).
- `sloop-fm1/tests/run_tests.sh`: the full upstream suite, including 182 reference renders; it
  reports the two static budget failures that upstream already has
  ([CARTRIDGE_BUILD.md](../sloop-fm1/docs/CARTRIDGE_BUILD.md)).
- `sh build-cartridge.sh` keeps all of upstream's flash, RAM, pool, loader and HAL checks.

## What these tests do not establish

USB enumeration, DMA and clocks on the real controllers, the render time of each engine on the
ESP32 (`render_max_us`), latency, the sound through the FM-1, the slot switch and the rollback,
power and battery behaviour. Those are bench tests.
