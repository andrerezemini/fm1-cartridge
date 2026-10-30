# Cartridge firmware: ESP32-S3

ESP-IDF **5.3.6** project for the cartridge's ESP32-S3 at 240 MHz. The ESP32 is the USB host: it enumerates the FM-1 running the modified Sloop, receives its MIDI and, in the audio profiles, sends stereo PCM back to the FM-1 over USB audio. There is no DAC on the cartridge: the sound leaves through the FM-1.

Two board configurations are supported:

- **8 MB boards such as the XIAO ESP32-S3** (plain profiles): 8 MB flash in QIO and 8 MB octal PSRAM at 80 MHz. No such board is built or tested.
- **ESP32-S3 Super Mini** prototype (`-4m2m` profiles): 4 MB flash in DIO at 80 MHz and 2 MB quad PSRAM at 40 MHz, with automatic PSRAM detection. This is the board the prototype uses; its circuit revision is not confirmed ([hardware](../hardware/README.md)).

## Risks

The firmware runs on experimental hardware; the electrical risks are in
[hardware/README.md](../hardware/README.md#risks-of-electrical-damage). These are the risks of
the firmware and of flashing it.

- **Flashing the ESP32.** A wrong address, a partition-table change without a full flash or an
  interrupted write leaves the board unbootable until it is flashed again. The ESP32-S3's download
  mode lives in ROM, so BOOT and RESET always bring it back; the procedures here never touch
  eFuses, Secure Boot or flash encryption, which could take that away. Take the full backup in
  [FLASHING.md](../docs/FLASHING.md) first.
- **Flashing the FM-1.** The cartridge Sloop replaces the FM-1's firmware. Its installer checks the
  package before writing, an interrupted install finishes when it is run again, and Sloop offers a
  USB rescue (OCT− at power-on). An FM-1 that does not start at all needs
  [FM-1-transporter](https://github.com/kurogedelic/FM-1-transporter). The vendor's V15 firmware
  can be put back. Check the vendor's warranty terms before replacing its firmware.
- **The FM-1's charger.** With CHARGE at AUTO (ON is the default at every start) the cartridge
  Sloop clears bit 0 of the SoC's `P3_CHG_CON0` while the cartridge answers. The write touches only
  that bit, through the SoC's own set and clear operations, but the bit map comes only from the SDK
  header and its real effect is not measured. Every start, the rescue and the updater included,
  turns the charger back on when the previous session held it off and its record survived; CHARGE
  at ON turns it back on when a start found it off
  ([CARTRIDGE_USB_RETURN.md](../sloop-fm1/docs/CARTRIDGE_USB_RETURN.md#fm-1-charger-and-the-charging-icon)).
- **Power is outside the firmware's control.** The "root port powered" option enables the USB
  controller and generates no 5 V. The firmware cannot measure or limit the current on VBUS, does
  not read the battery, and cannot prevent the brownout resets of a low cell; it logs a brownout,
  panic or watchdog reset at the next start.
- **Loud output.** A DSP fault, an untested preset or a resampler fault can produce full-scale
  noise through the FM-1. Start every test with the FM-1's volume low and with headphones away
  from the ears.
- **Unmeasured real time.** The render cost of Melodee and Felucca on the ESP32 is not measured.
  An overloaded render produces gaps and stuttering notes, not damage.
- **Slots and rollback.** Booting, switching and rollback are verified in software only. A slot
  confirms itself only after its cartridge server has answered the FM-1; one that does so and then
  misbehaves stays the default across resets: switch away with FIRMWARE, or flash another image
  through the USB bootloader.
- **Sloop's headroom.** The cartridge profile leaves 7,520 B free in the FM-1's application image
  (the diagnostic build 6,736 B).

## Profiles

Each profile has its own `sdkconfig`, built from `sdkconfig.defaults` plus the profile's fragment (`sdkconfig.tone`, `sdkconfig.clipper`, `sdkconfig.felucca`, `sdkconfig.melodee`) and, for `-4m2m`, `sdkconfig.esp32s3-4m2m`.

| Profiles | What it runs | Condition for sound on the FM-1 | Binary size (1.25 MB slot) |
| --- | --- | --- | --- |
| `midi` (default), `midi-4m2m` | Enumeration, bulk IN, MIDI parser and console | Does not transmit audio | 283,536 B (`midi-4m2m`) |
| `tone`, `tone-4m2m` | UAC1 TX of a stereo 440 Hz sine at −20 dBFS, independent of MIDI notes | Modified Sloop with the USB return | 312,320 B (`tone-4m2m`) |
| `clipper`, `clipper-4m2m` | 23 ported Clipper engines, queued MIDI, 64-frame render on core 1, UAC1 TX, cartridge server, the sound kept in NVS | Same, plus the render fitting its budget on the ESP32 | 761,136 B, 42% of the slot free (`clipper-4m2m`) |
| `felucca`, `felucca-4m2m` | 13 engines of Felucca 1.7.0, one part, same path | Same; **not yet heard on hardware** | 604,688 B, 54% free (`felucca-4m2m`) |
| `melodee`, `melodee-4m2m` | Prophet, FM6, CZ-1, SID and DRUM of Melodee 1.5.1, one part, one core | Same; **not yet heard on hardware**; the heaviest in CPU and RAM | 596,528 B, 54% free (`melodee-4m2m`) |

Internal RAM in use (`idf.py size`, DIRAM): Clipper 150 KB, Felucca 183 KB, Melodee 228 KB of 341 KB. The rest of the large state (delay and reverb lines, sequencer pools) sits in PSRAM through the `main/*.lf` fragments ([real-time budget](#real-time-budget)). The 8 MB synthesizer profiles (`clipper`, `felucca`, `melodee`) build with the same partition table.

The `midi` profile enumerates the peripheral, looks for a MIDIStreaming interface (class 1, subclass 3) and receives USB-MIDI 1.0 packets over the bulk IN. Notes and transport appear on the console; the MIDI clock is counted and printed once per second so as not to saturate the UART. The audio profiles count the notes too and print one line a second, so the console never holds up a task. The console is the UART driver with a 4 KB transmit ring, without colour codes: a restart (a slot switch) waits for the ring to drain, while a panic or a watchdog prints its report straight to the UART and the last lines still in the ring before it are lost. On the Super Mini, build `midi-4m2m`: the plain `midi` profile expects 8 MB of flash and octal PSRAM and does not boot there.

The `tone-4m2m` profile isolates the audio transport: it uses the same memory configuration and the same UAC path as the synthesizers but does not depend on a synthesizer or on MIDI notes. After a diagnosis with it, flash the synthesizer profile back.

Observed on hardware: the 440 Hz tone and Clipper notes play through the FM-1 speaker with a clean sound; the three `-4m2m` synthesizer profiles are flashed to the three slots and read back identical. Not yet verified on hardware: booting and switching the slots and the rollback, Melodee and Felucca by ear, the render cost of each engine (`render_max_us`), USB stereo L/R, latency and long-session stability.

## Three firmware slots

`partitions.csv` is the same for every profile, so every profile fits in any slot:

| Partition | Offset | Size | Contents |
| --- | --- | --- | --- |
| `nvs` | 0x9000 | 16 KB | the sound each firmware keeps (`persist.c`, `synth/cartridge_state.c`) |
| `otadata` | 0xd000 | 8 KB | the slot the bootloader boots and the state of each slot |
| `phy_init` | 0xf000 | 4 KB | radio calibration (unused) |
| `ota_0` | 0x10000 | 1.25 MB | slot 1 |
| `ota_1` | 0x150000 | 1.25 MB | slot 2 |
| `ota_2` | 0x290000 | 1.25 MB | slot 3 |

There is no `factory` partition: the three slots are equal. On the XIAO's 8 MB the second half of the flash stays unused. Changing the table needs a full flash, since `otadata` and the slot offsets depend on it.

The FM-1 selects the slot that boots through the FIRMWARE parameter of the CARTRIDGE page. When the knob rests on another slot for `CONFIG_FM1_SLOT_SWITCH_MS` (2,000 ms by default), the firmware sets it as the boot partition and restarts. The name shown on the FM-1 is the profile's `CONFIG_APP_PROJECT_VER` (CLIPPER, MELODEE, FELUCCA, TONE, MIDI).

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`: a slot chosen from the page boots as pending and confirms itself (`slots.c`, `fm1_slots_confirm`) once its cartridge server has answered a request from the FM-1. If it never does, the next reset returns to the previous slot. At every start the images of the other slots are verified; a damaged one is never offered, and the slot last rolled back from is named "<NAME> FAIL". Selection, rollback and limits are in [FIRMWARE_SLOTS.md](../docs/FIRMWARE_SLOTS.md).

Each synthesizer firmware keeps its sound: the engine, the preset, MIDI CH and every catalogue value go into one NVS record per firmware name a few seconds after the last change, once the FM-1 holds no note and its transport is stopped (an NVS write stalls the flash cache, and with it every task, for a few ms). The main loop writes it; the audio task restores it at start, checking the synthesizer, its revision and every entry's name and range.

## Building

Install ESP-IDF 5.3.6 with `esp32s3` support and activate `export.sh`. The Component Manager downloads `espressif/usb_host_uac` **1.5.0** and `cmake_utilities` when a build directory is configured, including in the MIDI profile. `dependencies.lock` pins the resolved versions. There is no dependency on Arduino, TinyUSB device or PlatformIO.

```sh
cd firmware
. "$IDF_PATH/export.sh"
./build.sh midi
```

`export.sh` uses the first `python3` on the `PATH`; if ESP-IDF was installed with another Python, put that one first or run ESP-IDF's `install.sh` again.

Each profile uses a separate build directory and `sdkconfig`, so that one profile's build does not override another's defaults:

```sh
# 440 Hz tone at -20 dBFS.
./build.sh tone

# Real Clipper engine, playing the notes from Sloop.
./build.sh clipper

# Super Mini prototype: 4 MB flash / 2 MB quad PSRAM.
./build.sh clipper-4m2m

# The other two synthesizers, one per slot (felucca / melodee for the XIAO).
./build.sh felucca-4m2m
./build.sh melodee-4m2m

# Same prototype: diagnostic tone, independent of MIDI notes.
./build.sh tone-4m2m
```

The project refers to `core/`, `synth/` and `third_party/` by relative paths, so the repository can live anywhere.

`build.sh` accepts `idf.py` actions after the profile and uses `build-<profile>/sdkconfig`. Use `./build.sh clipper-4m2m menuconfig` → **FM1 Cartridge** to select the engine and preset at boot (or `clipper` for the 8/8 MB XIAO; the same menu exists in the `felucca`/`melodee` profiles, with the catalog indices of each port), the MIDI channel and `FIRMWARE: ms`, the time the knob rests before the slot switch. The engine and preset indices are validated at boot; a non-existent index aborts with an explicit error. The three ports keep their native rate of 44,100 Hz; 48 kHz is not an option.

An existing `build-<profile>/sdkconfig` wins over the defaults in ESP-IDF, so `build.sh` hashes the profile's `sdkconfig.*` fragments and `main/Kconfig.projbuild`: when they change, it moves the old file to `sdkconfig.previous` (menuconfig edits stay there) and makes a fresh one. After a build it checks that the image uses the custom partition table and the bootloader's rollback.

## Programming and console

- USB OTG: GPIO19 = D−, GPIO20 = D+; dedicated to the connection with the FM-1 while the firmware runs.
- Console: UART0, **TX GPIO43**, **RX GPIO44**, **115200 8N1**. On the Super Mini they are the pins marked TX and RX. Reading them needs a **3.3 V** logic UART adapter: board TX → adapter RX; common GND. RX is only needed for programming or input.
- The USB CDC console and the secondary USB Serial/JTAG console are disabled. The same USB port cannot act as host for the FM-1 and as USB console for the computer at the same time.

Flashing is never part of a build. The full procedure, with backups and read-back, is in [FLASHING.md](../docs/FLASHING.md). Flash with the FM-1 disconnected, through the ESP32-S3 USB bootloader (BOOT/RESET) or a UART with the appropriate boot/reset signals:

```sh
./build.sh midi -p /dev/cu.PROGRAMMING_PORT flash
# For another profile, replace midi with tone, clipper, felucca, melodee or a -4m2m profile.
./build.sh midi -p /dev/cu.UART_ADAPTER monitor
# The three synthesizers at once, one per slot (bootloader, table, clean otadata):
./flash-slots.sh -p /dev/cu.PROGRAMMING_PORT ota_0=build-clipper-4m2m ota_1=build-melodee-4m2m ota_2=build-felucca-4m2m
```

`./build.sh <profile> -p PORT flash` writes the image to `ota_0` with the initial `otadata`, so the board boots slot 1. [`flash-slots.sh`](flash-slots.sh) writes the bootloader, the table and one binary per slot and erases `otadata` when `ota_0` is among the slots named; without `ota_0` (for example `ota_1=...` alone) it writes only the slots named, without touching the table, `otadata` or the other slots. It refuses builds whose partition tables or flash settings differ from the first one named, or that lack the rollback, and takes the offsets from that build's table. A tagged release also carries the three synthesizers merged into one image per board, `fm1-cartridge-esp32s3-4m2m.bin` for the Super Mini and `fm1-cartridge-esp32s3-8m.bin` for 8 MB boards, for `esptool.py write_flash 0x0`; that image also clears `nvs`, so every firmware's kept sound starts from its defaults, which `flash-slots.sh` leaves alone ([FLASHING.md](../docs/FLASHING.md)).

To enter the bootloader manually, hold BOOT, press and release RESET and release BOOT. The computer's USB port stops being a console when the firmware takes over the controller as host; runtime logs require the UART above.

After flashing, power the board independently and connect the FM-1 to the USB OTG port. The ESP-IDF "root port powered" software option enables the controller; **it does not generate 5 V on VBUS**. Run the physical VBUS test of the bench plan first ([G0](../docs/BENCH_TESTS.md#g0-sloop-on-the-mac-without-vbus)). Do not connect the UART adapter's 5 V to a LiPo-powered cartridge without defining the power circuit.

## USB audio return

Upstream Sloop has no USB audio input. The modified Sloop in `sloop-fm1/` adds the receiver ([CARTRIDGE_USB_RETURN.md](../sloop-fm1/docs/CARTRIDGE_USB_RETURN.md)): a stereo 44.1 kHz USB audio output with an ISO OUT endpoint, a ring, an ASRC and the mix into the FM-1's output ([patch and validation](../integrations/sloop-fm1/README.md)). The driver reports whether it finds an AudioStreaming ISO OUT, but that diagnostic does not prove format, rate or mixer operation. The tone and synthesizer profiles start only after the UAC driver confirms stereo PCM16 at 44,100 Hz.

The UAC package handles the split into 44/45 frames per millisecond. Compensation for the FM-1's real DAC clock happens in the Sloop receiver ([reception and clocks](../sloop-fm1/docs/CARTRIDGE_USB_RETURN.md#reception-and-clocks)). This firmware does not implement its own UAC feedback negotiation, ASRC or correction of the remote mixer. Descriptor conformance and receiver stability need to be measured.

### USB host FIFO

In the UAC profiles the code sizes the ESP32-S3 USB host hardware FIFO explicitly (`main/usb_host_fifo.h`, applied in `main/usb_midi_host.c`): 104 RX words, 48 control/bulk TX words and 48 periodic TX words, 200 words in total, the controller's usable budget. The periodic TX FIFO then holds audio packets of up to 192 bytes, enough for the 180-byte packets of Sloop's ISO OUT. The balanced FIFO setting of ESP-IDF 5.3.6 reserves only 128 bytes (32 words) for the periodic TX, below those 180 bytes, and with it `usb_host_interface_claim` cannot open the UAC endpoint. The explicit sizes take precedence over the Kconfig choice, so the balanced value can stay in `sdkconfig`. At start the log prints the resulting limits (IN, control/bulk OUT and audio OUT, in bytes). The `midi` profile keeps the IDF setting.

### Tasks and ownership

USB and MIDI events stay on core 0. The audio task, on core 1, is the sole owner of the synthesizer and of the UAC write/close calls. USB callbacks do no DSP: they only validate, enqueue or notify the owner task. PCM production follows USB consumption by blocking on the UAC's internal queue; it does not use `vTaskDelay` to simulate the sample clock. A block that does not fit stays pending for a retry. The USB transfers are allocated by the driver itself in appropriate memory.

### Disconnection and errors

A MIDI disconnection stops new submissions, requests halt/flush and keeps processing callbacks until the pending transfer finishes. Only then does it release the interface, transfer and device. Disconnection and queue overflow generate a voice panic. A transfer error on a device still attached (a STALL, transaction errors, a babble) closes the interface the same way and opens the same address again up to five times, 0.25, 0.5, 1, 2 and 4 s later; before the first transfer it sends CLEAR_FEATURE(ENDPOINT_HALT) on both bulk endpoints, so the FM-1's data toggles start again with the host's. A good transfer resets the count. In the UAC, the component manages cancellation and draining; callbacks only notify the owner task, which ends the session. Support is for **one FM-1 connected directly**, without hubs.

The panic silences the next synthesizer blocks; if the USB stays connected, PCM already queued can still play: up to `CONFIG_FM1_UAC_BUFFER_BYTES` in the ring (2,048 B, 11.6 ms at 44.1 kHz), plus the ISO transfers already submitted and the FM-1's buffer. It is not an instantaneous mute of the physical output. A UAC disconnection ends the session and drains the transfers; a fast reconnection stays pending until the previous close finishes.

A UAC transfer or write error stops the stream and starts it again up to three times per connection, 0.5, 1 and 2 s later; after that, or after a format or start error, a disconnect/reconnect is needed. The application retains the handle until it receives `DISCONNECTED`: in `usb_host_uac` 1.5.0, closing during a disconnection still being prepared can return success without destroying the object, and that separation avoids losing the reference in that window. Descriptors with more discrete rates than `UAC_FREQ_NUM_MAX` are refused before `start`, since that version of the component keeps a limited list.

## Real-time budget

The audio task renders 64 frames a pass, a budget of 1,451 µs at 44.1 kHz; MIDI reaches the synthesizer at that step. Every 5 s the main loop prints the frames handed to the UAC driver, the underruns (estimated from the frames written against the time elapsed: the host's SOFs and the timer share the crystal), the write retries, the stream restarts, and the longest render (`render_max_us`) and audio-loop pass (`pass_max_us`) of those 5 s; the CARTRIDGE page shows the longest render and the underruns since it was opened. **Queued frames are not confirmation of reproduced audio**. Measuring losses, underrun/overflow at the receiver, one-way latency and prolonged stability is part of the bench work ([BENCH_TESTS.md](../docs/BENCH_TESTS.md)).

The writer blocks on a full ring, so the ring stays nearly full: `CONFIG_FM1_UAC_BUFFER_BYTES` (2,048 B, 11.6 ms) plus three URBs of two 1 ms packets (`CONFIG_UAC_NUM_PACKETS_PER_URB=2`) are the cartridge's own latency, and the room it has for a slow pass. `tests/test_pipeline.c` models the whole return with these sizes, URB refills up to 2 ms late, the FM-1's DAC 300 ppm off the bus and Sloop's real receiver: about 25 ms from the render to the FM-1's mixer (39 ms with a 4 KB ring, 128-frame renders and three-packet URBs), and a single render of up to 15 ms rides through without a gap; fewer or shorter URBs, or a smaller ring, fail it. The bench measures the real figures (G3, G4). A preset change does not stall the task (the voices fade out, the effect lines are not cleared), and Clipper's WAVE tables are built in steps of at most 1,024 butterflies a 32-frame block instead of a whole table in one render.

The synthesizer profiles (`sdkconfig.clipper`, `sdkconfig.felucca`, `sdkconfig.melodee`) are built for that budget:

- `-O2` (`CONFIG_COMPILER_OPTIMIZATION_PERF`).
- A 32 KB instruction cache over the flash-resident DSP code and a 64 KB data cache.
- Hot state in internal RAM. The linker fragments `main/clipper.lf`, `main/felucca.lf` and `main/melodee.lf` move to PSRAM, by symbol, only the large buffers that are accessed sequentially or rarely: the Clipper's delay, reverb and chorus lines and WAVE mipmaps; Felucca's delay and loop buffers; Melodee's sequencer pattern pool and recording lists. Voices, the engines' arena, tracks, effects and the mixer stay in internal RAM, because per-sample random access through the data cache to 40 MHz quad PSRAM is far too slow for the budget.
- The task watchdog does not check the idle task of core 1 (`CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=n`), so a saturated audio task on core 1 does not make the watchdog print from an interrupt on core 0 for tens of milliseconds, which would also stall the USB host.

On the receiving side, the Sloop's resampler and buffer target are described in [CARTRIDGE_USB_RETURN.md](../sloop-fm1/docs/CARTRIDGE_USB_RETURN.md#reception-and-clocks).

## MIDI channel

`CONFIG_FM1_MIDI_CHANNEL` (menuconfig → FM1 Cartridge) defines the channel the synthesizer answers on its first start: 1 by default, 0 = omni; the channel kept in NVS takes over afterwards. The FM-1 sends each track on its own channel (GLO → MIDI), so only the track on channel 1 plays the cartridge; Sloop makes it the cartridge track, with its engine silent and the cartridge's names, while the other tracks keep their internal sound. The first parameter of the CARTRIDGE page, **MIDI CH**, changes this live (OMNI or 1..16) and lets go of the notes sounding; clock and transport are not filtered.

## Cartridge control

The synthesizer profiles include the runtime server (`synth/cartridge_remote.c`, on top of the common interface `synth/fm1_synth.h`). The GLO → CARTRIDGE page of the modified Sloop sends commands, receives names and values and changes sounds without reflashing. In protocol version 3 the reply also carries the firmware name ("MELODEE OK" on the FM-1) and the synthesizer's MIDI channel, from which Sloop picks the cartridge track; the FIRMWARE parameter selects the slot that boots. A newer peer is read as far as version 3's fields go; an older one shows NO LINK.

The `midi` and `tone` profiles do not answer the protocol (the FM-1 shows NO LINK with them), so a slot holding one of them is never confirmed and the next reset returns to the previous slot. The engine/preset configuration of the build defines the state of a firmware's first start; after that it starts with the sound it kept. Guide, limits and tests in [CARTRIDGE.md](../docs/CARTRIDGE.md).

## Bench verification

1. MIDI profile (`midi-4m2m` on the Super Mini): connect, press and release notes; check `ON`, `OFF`, `START`, `STOP` and `CLOCK`; disconnect with a sustained note and reconnect at least ten times.
2. With the modified Sloop, the MIDI profile's log announces an AudioStreaming ISO OUT. Validate the Sloop's USB audio output with a computer before the ESP32 audio profiles.
3. Tone profile: check audio on both FM-1 channels, the receiver counters and recovery after disconnecting. Start with a low volume on the FM-1.
4. Synthesizer profiles: check note, velocity, channel, panic and the render time of different engines; measure a long session before closing the enclosure.

Implementation references: [ESP-IDF USB Host Library 5.3.6](https://docs.espressif.com/projects/esp-idf/en/v5.3.6/esp32s3/api-reference/peripherals/usb_host.html), [usb_host_uac 1.5.0](https://components.espressif.com/components/espressif/usb_host_uac/versions/1.5.0/readme) and [API of that version](https://github.com/espressif/esp-usb/blob/6d24137e14a4f6c8138662a7a074be16a899c2c6/host/class/uac/usb_host_uac/include/usb/uac_host.h).

## Future firmware work

On the ESP32:

- Measure `render_max_us` for every engine on the hardware and set per-engine voice limits from
  the measurements.
- User presets on the cartridge, beside the sound it keeps.
- Map MIDI CC and Program Change to the cartridge's parameters and presets (CC20/MORPH are not
  ported).
- Several parts on different channels, so more than one Sloop track can each play the cartridge,
  within the measured CPU budget.
- An asynchronous UAC stream with a feedback endpoint, if the bench shows drift or latency the
  adaptive stream cannot absorb.
- Update a slot over the cartridge link, from the FM-1 or a computer, without the USB bootloader.
- More slots in the unused half of the XIAO's 8 MB flash.
- Battery level in the cartridge protocol, once the hardware can read it.

In Sloop:

- Map the HOME knobs of the cartridge track to cartridge parameters, and its PAN to the return.
- Save the cartridge's engine and preset with a Sloop project, so a project recalls its cartridge
  sound.
- Free flash in the cartridge profile before adding larger features.
- Show the cartridge's battery level once the protocol carries it.
