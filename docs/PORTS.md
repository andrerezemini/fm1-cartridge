# Synthesizer ports: Clipper, Felucca and Melodee

The cartridge runs one of three synthesizers written for the FM-1: **Clipper**, **Felucca 1.6.1**
and **Melodee 1.4.5**. The originals are firmware for the FM-1's AC7911; their binaries do not run
on the ESP32-S3. Each port compiles the upstream DSP source itself (engines, voices, parameters,
effects and the sequencer's MIDI handling) behind one common interface, `synth/fm1_synth.h`. None
of them is an emulation of the original sound.

Each synthesizer is built into its own firmware and flashed into its own slot; the FM-1 chooses
which slot boots ([FIRMWARE_SLOTS.md](FIRMWARE_SLOTS.md)). The CARTRIDGE page of the
modified Sloop edits whichever synthesizer is running ([CARTRIDGE.md](CARTRIDGE.md)). Provenance,
the vendored file subsets, the one changed upstream file of each port and the licences are in the
`third_party/` READMEs linked below.

## The three ports

| | Clipper | Felucca | Melodee |
| --- | --- | --- | --- |
| Upstream | [isod89/clipper-fm1](https://github.com/isod89/clipper-fm1/tree/e69067211c4690f34e494b5b896e4585163dcc2b) | [hugelton/Felucca](https://github.com/hugelton/Felucca/tree/63f88ec18b71c6ed4371d03d6f04dd93f4193ad3) 1.6.1, with [hugelton/Felucca-Engines](https://github.com/hugelton/Felucca-Engines/tree/c46e0086f69bdc26df1cd144dca9e683cc4531dd) | [keremimo/melodee](https://github.com/keremimo/melodee/tree/398ada1e09d864c12f3b778a43e45c7c2a8031b8) 1.4.5 |
| Pinned revision | `e69067211c4690f34e494b5b896e4585163dcc2b` | `63f88ec18b71c6ed4371d03d6f04dd93f4193ad3`; engines `c46e0086f69bdc26df1cd144dca9e683cc4531dd` | `398ada1e09d864c12f3b778a43e45c7c2a8031b8` |
| Adapter | `synth/clipper_synth.c` | `synth/felucca_synth.c` | `synth/melodee_synth.c` |
| Vendored subset | [third_party/clipper](../third_party/clipper/README.md) | [third_party/felucca](../third_party/felucca/README.md) | [third_party/melodee](../third_party/melodee/README.md) |
| Engines offered | 23 | 13: ANALOG, FM6, PHASE, LOFI, SAMPLE, VOICE, TRIO, WHEEL, GRAIN, PHYS, NOISE, SLICE, DRUM | 5: PROPHET, FM6, CZ-1, SID, DRUM |
| Factory presets | 407 | 72 | 327, of which 201 in PROPHET |
| Voices | 8, shared (upstream `NVOICE`) | 8 per part (upstream `NVOICE`) | 16 voice slots per part: PROPHET 5 (the upstream single-core value), FM6 up to 16 (as Dexed), SID 4, the other engines 8 |
| Parts rendered | One (track 1) | One of the four (part 1) | One of the four (`song.ntrk = 1`) |
| Compile switches | `CLIPPER_SYNTH=1`, `FELUCCA_SLICE=0`, `FELUCCA_ARRANGER=0` | `FELUCCA_CPU2=0`, `FELUCCA_FM4=0`, `FELUCCA_SLICE=1` | `MELODEE_DUAL_CORE=0`; USB audio, CDC, flash, OTA and audio profile at 0 |
| Compiler flags | `-fwrapv` | `-fwrapv` | `-fwrapv -ffp-contract=off` (floating-point FM6, as in the upstream tests) |
| Name on the FM-1 | CLIPPER OK | FELUCCA OK | MELODEE OK |

The three share Felucca's code structure: one translation unit per port includes `core.h`,
`engines.c`, `params.c`, `voice.c`, the effects or mixer and `seq.c`, in the order of the
upstream `tests/hostsim.c`. All three use the original **44,100 Hz** tables and render in control
blocks of 32 frames.

## Common interface: `synth/fm1_synth.h`

Each adapter returns an `fm1_synth_api_t` (`clipper_synth_api()`, `felucca_synth_api()`,
`melodee_synth_api()`). The audio task, the cartridge server (`synth/cartridge_remote.c`), the
simulator and the tests use only this struct, so they do not depend on which DSP is linked. The
Clipper adapter also exports the same calls as named functions (`clipper_synth_init`,
`clipper_synth_render` and so on, in `synth/clipper_synth.h`), which `tests/test_clipper.c` uses.

| Call | Behaviour in the three ports |
| --- | --- |
| `init(rate)` | 44,100 Hz only; any other rate returns `-1` and keeps the current state |
| `midi(status, data1, data2)` | Queues channel messages (notes, pressure, control change, pitch bend) and `F8/FA/FB/FC`; `FF` and CC 120 run panic; CC 123 to 127 on the instrument's channel let go of its notes (`release`; in Clipper, upstream's own handling). Program Change returns `-1`; Clipper also rejects CC 20 (MORPH), which its UI drives. Returns `0` accepted, `-1` invalid or unsupported (or before `init`), `-2` queue full |
| `render(stereo, frames)` | Interleaved stereo PCM16, any frame count including zero; the DSP keeps its 32-frame blocks, so the output does not depend on how the caller splits the calls. Silence before `init` and after panic, until a new Note On |
| `panic()` | Ends voices, queued events, held notes and effect tails; keeps the selected sound |
| `release()` | Every sounding note lets go at the next block, as a note-off would (upstream's per-track release request, `panic_req`, with the arpeggiator, KBD HOLD and the MIDI note owners); a note-on still in the queue is dropped with them (upstream lets go before it drains the queue, so it would start after the release); tails, the rest of the queue and the sound kept. The cartridge server calls it on a MIDI CH change |
| `set_preset(engine, preset)` | Selects an engine and a factory preset; `-1` for an invalid index. What sounds fades out over upstream's engine-switch fade (2.9 ms; 5.8 ms in Clipper, with its own sound fade), on its own values: Felucca and Melodee put the old sound's common values back for the fade's blocks (upstream keeps only the engine's) and load the new FM6, PROPHET and CZ-1 patch after it. The engine follows, the effect tails and the MIDI already queued are kept. In Melodee a DRUM kit of another layout (909, SID, the others) cuts the hits instead, since the kit sizes the engine's state |
| `engine_count/name`, `preset_count/name`, `current_engine/preset` | The catalogue. Clipper indexes engines by their upstream IDs. Felucca and Melodee use the upstream display order (`ENGINE_ORDER`), so retired engines (DIGITAL in Felucca; ANALOG, DIGITAL and the legacy extras in Melodee) are not offered |
| `param_count/get/set` | The remote catalogue that the CARTRIDGE page edits: group, name, value text, unit and range, taken from the upstream parameter descriptors. Entry 0 is always `MIDI CH`. `param_set` returns `-1` outside the range, and in Felucca for SAMPLE SET, GRAIN SRC and SLICE SRC values with no material (the user slots, which the port cannot fill); the range those report ends at their last source |
| `set_midi_channel`, `midi_channel` | 0 answers every channel (OMNI), 1..16 one channel; realtime messages are never filtered |
| `name`, `revision` | `CLIPPER`, `FELUCCA` or `MELODEE`, and the upstream commit of the port |

The firmware sets the channel at boot from `CONFIG_FM1_MIDI_CHANNEL` (default 1), so only FM-1
track 1 plays the cartridge; `MIDI CH` changes it live ([CARTRIDGE.md](CARTRIDGE.md#midi-channel)).

Each synthesizer has a single instance: the DSP keeps its state in globals, as upstream does.
Every call happens on the same audio task. USB callbacks only put events into a task-safe queue
owned by the application; the audio task moves them into `midi()` and calls `render()`. The
adapter's internal queue holds 64 events and is not an inter-task queue. When it overflows, the
adapter runs panic and returns `-2`, so a lost Note Off cannot leave a voice stuck. Synthesis runs
serially on one core: the AC7911's second core, its barriers and its traps are not ported to
FreeRTOS.

Before each 32-frame block the adapters call what the upstream render loop calls: Clipper runs
`fm6_poll()` and its own `port_wt_service()` before `mix_block()`, Felucca runs `fm6_poll()` before
`mix_block()` (held while a fade waits to load the new FM6 patch), and Melodee runs `mix_block()`
alone (after loading a patch that waited for a fade). Upstream's `wt_service` builds four WAVE frames
a main-loop pass on the FM-1, seven inverse FFTs each; called from every block of a real-time render
it would build a whole table in one or two 64-frame calls. `port_wt_service` makes the same copies
bit for bit (upstream's load, transform and rounding, the twiddles from a table) in steps of at most
1,024 butterflies a block, and the WAVE voices stay silent until the table is complete, about 120 ms
of audio, as upstream's do while it builds; `port_wt_hold()` keeps the part's duck holding for that
long (upstream's gives up after 68 blocks), so the voices come back through its fade-in.
`tests/test_clipper.c` compares every factory table with upstream's `wt_build_frame` and checks the
fade-in. A change to an adapter keeps these calls or takes over their work
explicitly. In Clipper's upstream `tests/hostsim.c`, synthesizer mode needs
`CLIPPER_SYNTH=1`; the default legacy mode there does not exercise the Clipper synthesizer.

## What each adapter compiles

Each adapter is one translation unit that includes the vendored sources. The upstream `.c` files
are never compiled individually.

- **Clipper:** `libc.c`, `core.h`, `cpu1.h` (its second-core calls replaced by serial execution in
  the calling task), `engines.c`, `drums.c`, `params.c`, `voice.c`, `slicer.c`, `fx.c` and
  `seq.c`, over the tables in `third_party/clipper/generated`.
- **Felucca:** `libc.c`, `core.h`, `engines.c` (including the five Felucca-Engines engines),
  `params.c`, `mod.c`, `voice.c`, `slicer.c`, `fx.c`, `song_chain.c` and `seq.c`, over the tables
  in `third_party/felucca/generated`.
- **Melodee:** `libc.c`, `core.h`, `engines.c`, `params.c`, `mod.c`, `voice.c`, `mix.c`,
  `song_chain.c` and `seq.c`, over the tables in `third_party/melodee/generated`. The audio arena
  of `resources.c` is sized for one part (64 KB + 8 KB).

The upstream linker sections describe the AC7911 memory map. The adapters drop them and keep the
alignment and `noinline` attributes; on the ESP32 a linker fragment per port places the memory
(below).

## What is not ported

Common to the three ports:

- The UI, LCD, keys and panel; preset and project persistence; the upstream SysEx editor.
- The USB device driver (the cartridge is the USB host), UART MIDI and MIDI out.
- Flash access, OTA, the boot loader and the register HAL.
- The AC7911 second core.
- Program Change, which upstream handles in the UI.
- The other parts of the instrument: each port renders one part, with no per-channel
  multitimbrality.

Per port:

- **Clipper:** user banks (the `fm1_xip.h` stub returns an invalid header); CC 20 (MORPH); photos,
  icons, fonts, the Hügelton Sample Pack and the CC0 samples of the groovebox mode.
  `CLIPPER_SYNTH=0` is not supported by the subset.
- **Felucca:** the user sample slots (USR1..3 of SAMPLE, the SLICE sources); DIGITAL, whose sounds
  play as FM6, as upstream.
- **Melodee:** the USB interrupt timeline (each event plays in the block in which it arrives);
  the dual-core Prophet with 8 voices; the retired engines.

The CARTRIDGE page takes the place of the upstream editing: its own SysEx protocol selects engines
and presets and edits parameters in the ESP32's RAM, and the firmware keeps that state in NVS
(`synth/cartridge_state.c`, through the same interface). A CC accepted by the queue
does not create a new parameter map: the DSP answers only the controls upstream implements.

## Native verification

CTest runs three tests per port:

- `synth_<port>` (`tests/test_synth.c`): silence before `init`, rate rejection, every offered
  engine sounding with its first factory preset, release by Note Off and by velocity zero,
  `release()` with a note-on still queued, All Notes Off and CC 124..127 on the instrument's channel
  only (pedal held or not), preset changes under a chord that fade without a step above 3.5 times the
  sound's own (the first twelve presets of each engine, DRUM's without crashing) and keep the queued
  MIDI, the channel filter and the
  `MIDI CH` entry, identical PCM for any call size, recovery after a queue overflow, and panic by
  CC 120.
- `cartridge_<port>` (`tests/test_cartridge.c`): the cartridge server over that port, with
  simulated slots.
- `simulation_<port>` (`tests/test_simulation.py`): the WAV and the report of
  `cartridge_sim_<port>`, checked beyond the exit code.

`clipper` (`tests/test_clipper.c`) covers Clipper's named functions, and `upstream_integrity`
(`tools/check_upstream.py`) compares the three subsets with their SHA-256 manifests; `protocol`,
`audio_return`, `pipeline` and `wire_identity` cover the rest ([VALIDATION.md](VALIDATION.md)). The 15 tests
pass in Release and with ASan/UBSan. They confirm the behaviour of the adapters; they do not
certify every preset, the sound quality or real time on the ESP32. The
[simulator](SIMULATION.md) renders WAV files and fault scenarios for each port.

## On the ESP32-S3

Each synthesizer has its own ESP-IDF profile (`clipper`, `felucca`, `melodee`, and the `-4m2m`
variants for the Super Mini prototype with 4 MB of flash and 2 MB of quad PSRAM); see
[firmware/README.md](../firmware/README.md). The real-time budget is 1,451 µs per 64 frames. The
profiles build with `-O2`, a 32 KB instruction cache and a 64 KB data cache, and keep the hot
synthesis state (voices, engines, tracks, effect state) in internal RAM. Only buffers that are
large and read sequentially, or not touched per sample, go to PSRAM: random per-sample access to
40 MHz quad PSRAM through the data cache is too slow for the budget.

| ESP-IDF 5.3.6 build | Clipper | Felucca | Melodee |
| --- | --- | --- | --- |
| In PSRAM | The large delay and wavetable buffers (`firmware/main/clipper.lf`) | The large delay and loop buffers (`firmware/main/felucca.lf`) | The sequencer's pattern pool and recording lists (`firmware/main/melodee.lf`) |
| In internal RAM | Voice, engine, track and effect state | Voice, engine (`eng_mem`), track and effect state | The audio arena (engine voice state and FX slots), tracks and mixer |
| Image, `-4m2m` profile | 771,904 B (41% of a 1.25 MB slot free) | 603,424 B (54% free) | 571,168 B (56% free) |
| Internal RAM (DIRAM) used | 152 KB of 341 KB | 183 KB of 341 KB | 235 KB of 341 KB |

Observed on hardware: Clipper notes from the FM-1 keyboard play through the FM-1 with a clean
sound; the MIDI channel filter keeps the FM-1's other tracks on their internal sound; the
CARTRIDGE page shows CLIPPER OK; the three slots (Clipper, Melodee, Felucca) flash and read back
identical.

Not yet verified on hardware: Felucca and Melodee by ear; booting and switching the three slots;
the render cost of each engine on the ESP32 (`render_max_us`); the four CARTRIDGE knobs end to end;
long-session stability; latency.

## Open acceptance criteria on the ESP32

| Area | Required before a port counts as validated |
| --- | --- |
| Data | Reproducible generated tables; revision, flags and generators recorded; missing banks handled explicitly |
| DSP | 44.1 kHz rate and coherent blocks; amplitudes and saturation verified; no AC7911 instruction or HAL in the ESP32 path |
| MIDI | Note On/Off, velocity zero, sustain/pitch/CC only where supported; reset without stuck notes |
| State | Engine/preset changes outside the USB callback; no reads of partially updated buffers |
| Real time | Maximum cost per block under concurrent MIDI/UAC load; no malloc, logging or slow flash access on the critical path |
| Memory | Map and heap per engine; stacks with margin; internal DMA allocations; PSRAM proven under load |
| Multicore | Serial render measured first; the AC7911's `cpu1`, barriers and traps are not carried over to FreeRTOS |
| Storage | The ESP32's own preset and sample policy; no reuse of FM-1 flash addresses |
| Sound | Native reference WAV, ESP32 capture through the FM-1 and a musical regression test with the same events |
| Power | Wi-Fi/BLE off; consumption measured under the real workload |

Physical validation order, per port: one engine and preset → release and retrigger → chords →
preset change → required effects → other engines and banks → voice limit. Polyphony grows only
after the worst case is measured. Equivalence of all engines or presets is not claimed before
that sequence.

## Code, assets and distribution

Each subset keeps the upstream `LICENSE`, `LICENSING.md`, SPDX headers and third-party notices.
The upstream licensing documents distinguish GPL-3.0-only code, components under other notices and
assets with their own conditions; an assets folder is not, as a whole, free for redistribution.
Each `third_party/` README is the inventory of the data actually included and of the material left
out, with the individual provenance of generated tables and samples. When a bank cannot ship with
the project, keep a build that works with test data instead. A distributed revision includes the
corresponding sources and reproducible instructions.

## Risks

- Flash, RAM and render time depend on the engine; the amount of PSRAM does not determine
  performance.
- Melodee is the heaviest of the three: floating-point FM6, and a Prophet with 5 voices as in the
  upstream single-core mode. Its render cost on the ESP32 is not yet measured.
- The native build does not exercise the ESP32's caches, DMA, concurrency or latency.
- Restricted assets and generated dependencies may prevent preset equivalence with the original
  instruments (user banks and user sample slots are absent).
- Every port reaches the FM-1 output only through the modified Sloop's USB audio return
  ([CARTRIDGE_USB_RETURN.md](../sloop-fm1/docs/CARTRIDGE_USB_RETURN.md)); its cost and quality on
  the FM-1 are verified on the bench ([BENCH_TESTS.md](BENCH_TESTS.md)).
- Upstream changes require a provenance comparison and the relevant tests again; a subset is not
  updated without recording its new revision.
