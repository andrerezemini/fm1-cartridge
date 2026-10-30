# Bench tests: procedures and evidence

**No physical result is pre-filled as passed.** Copy the run log section for each run, attach logs and captures, and update a gate only after running its test. Simulation results are not USB or DAC measurements.

## Preparation

- [ ] Record FM-1/firmware/SHA, ESP32-S3 board/revision, IDF/UAC component, Mac/macOS and measuring instrument.
- [ ] Confirm FM-1 recovery and backup before installing a modified Sloop ([FLASHING.md](FLASHING.md)).
- [ ] Test a known USB data cable in the normal configuration.
- [ ] Check D+/D−/GND continuity and VBUS isolation on the breakout with everything powered off.
- [ ] Check BAT polarity and cell isolation; record the starting battery voltage.
- [ ] Keep UART logs separate from USB OTG. Never tie the UART adapter's 5 V to the battery/USB assembly during the no-VBUS test.

## G0: Sloop on the Mac without VBUS

1. Disconnect the ESP32 and external power. Charge the FM-1 and power it from its own battery, with a known Sloop and USB SERIAL OFF.
2. Run a control with a normal cable: save `system_profiler SPUSBDataType -json` and confirm notes in the MIDI monitor.
3. Power off and disconnect before changing the wiring. Interrupt only VBUS with a verified breakout or jumper; keep data and ground. Do not use tape on USB-C contacts as a build method.
4. Measure VBUS on the FM-1 side; confirm it is not receiving 5 V. Connect to the Mac and save a new USB report and the MIDI events.
5. Repeat with the FM-1 powered on before and after connecting; repeat both cable orientations and at least ten cycles. Note successes, failures and time to enumerate.
6. If it fails, restore the normal cable to check that the control still works. Record the hypothesis and the next test; a single failure does not call for a powered VBUS.

Acceptance: repeatable enumeration and MIDI with VBUS effectively isolated, and a documented assembly. The result applies to that firmware, cable and host; G1 remains necessary.

### G0 measurements on the Super Mini prototype

Assembly: the FM-1 and the ESP32-S3 Super Mini connected **only through a USB-C male-to-male adapter**; the cartridge on its own LiPo; no other wire. Multimeter on the Super Mini pins, cartridge powered from the battery.

Measured on the Super Mini prototype, cell at 3.56 V:

| Measurement | FM-1 disconnected | FM-1 connected and on |
| --- | ---: | ---: |
| 5V (connector VBUS) × GND | 2.77 V | 2.77 V |
| 3V3 × GND | 2.77 V | 2.77 V |
| BAT+ × BAT− | 3.56 V | — |

Measured on the Super Mini prototype, cell charged (FM-1 connection state not recorded):

| Measurement | Cell at 4.2 V |
| --- | ---: |
| 5V (connector VBUS) × GND | 3.3 V |
| 3V3 × GND | 3.3 V |
| BAT+ × BAT− | 4.2 V |

Reading:

- The VBUS that reaches the FM-1 is a floating level that follows the cartridge's 3V3 rail (~2.8 V with the cell at 3.56 V; 3.3 V with the cell at 4.2 V). It is never 5 V and always below the FM-1's own battery (3.7–4.2 V), with or without the FM-1 connected.
- **Charging the FM-1 from the cartridge is physically impossible under this condition.** The charging icon in Sloop's header does not show charging ([CARTRIDGE_USB_RETURN.md](../sloop-fm1/docs/CARTRIDGE_USB_RETURN.md#fm-1-charger-and-the-charging-icon)).
- The FM-1 enumerates, receives MIDI and returns audio with D+, D−, GND and this weak VBUS: the no-VBUS hypothesis holds for this assembly. The G0 procedure above (Mac, VBUS interrupted, repeated cycles) is a separate test.
- What holds VBUS at the 3V3 level (regulator leakage, a pull-up or another path on the board) is not identified. Do not assume that VBUS is isolated from 3V3, nor that this path can carry current.
- The board's regulator regulates with a full cell (3V3 = 3.3 V at 4.2 V) and drops out with a low cell (3V3 = 2.77 V at 3.56 V). At 2.77 V the ESP32-S3 runs below its 3.0 V specification, close to the brownout reset. The usable range of the LiPo is therefore narrowed: 3V3 crosses 3.0 V somewhere between 4.2 V and 3.56 V of cell voltage. G6 measures that point under load and treats it as the cartridge's practical cutoff.

## G1: ESP32 host with MIDI

- [ ] Firmware flashed before connecting to the FM-1; 3.3 V UART console on GPIO43 TX / GPIO44 RX, common GND; Mac USB cable removed.
- [ ] ESP32 and FM-1 each on their own power; VBUS measured on both.
- [ ] Log of the raw descriptor, configuration, MIDI interface and bulk IN address/maxpacket.
- [ ] Keyboard: every note ends; velocity zero is Note Off.
- [ ] Sequencer, arpeggiator and rolls in the corresponding OUT configuration; start/stop/clock as the firmware handles them.
- [ ] Disconnect with notes held; reconnect ten times; no old voice or transfer survives.
- [ ] A transfer error on a device that stays attached (for example a cable touched during notes): the log shows the interface opened again and the CLEAR_FEATURE, and MIDI resumes without a replug; record how often it happens.
- [ ] With upstream Sloop, which has no USB playback, record separately that there is no audio return; this is expected.

## G2: playback on Sloop using the Mac

Run with the modified Sloop ([SLOOP_INTEGRATION.md](SLOOP_INTEGRATION.md)) and the Mac, before the ESP32 transmitter.

- [ ] Build and map of the modified revision and of the baseline attached; relevant tests pass.
- [ ] Audio MIDI Setup shows a two-channel, 44,100 Hz output from the FM-1, with USB SERIAL OFF; on macOS 15 or earlier too (the AudioControl collection lists the streaming interface before MIDI, bcdDevice 4.08).
- [ ] The host identifies ISO **OUT**, alt 1; when stopping, alt 0 or the timeout clears the return.
- [ ] Test tone at low level, then L only and R only; measure gain and check that the channels are not swapped.
- [ ] FM-1 physical output captured on an external interface or recorder; effects, master, limiter and the return mute verified.
- [ ] Thirty minutes of tone, sequencer and UI; record counters, ring minimum/maximum level and rate correction. The duration is a minimum test, not a promise of unlimited stability.
- [ ] Disconnect or reset during audio; the output goes silent and reconnects without old PCM.
- [ ] Repeat with simultaneous USB capture and, separately, with CDC ON; record unsupported variants.

## G3/G4: ESP32 audio and engine

- [ ] The ESP32 tone reaches the same physical destination validated in G2.
- [ ] 44/45 frames per ms and an average of 44,100 frames/s; the effective packet length respects the descriptor.
- [ ] MIDI and audio simultaneously, sequencer active, Sloop UI and effects under load.
- [ ] Thirty continuous minutes per scenario; error logs, ring fill and core load.
- [ ] Restart each side, suspend and reconnect, pull the cable during a note; no stuck note.
- [ ] `render_max_us`, `pass_max_us` and `underruns` on the UART (every 5 s) and AUDIO RENDER MAX / PASS MAX / GAP MAX / UNDERRUNS on the CARTRIDGE page, per engine, with the 2,048 B ring and 64-frame renders; a WAVE table change in Clipper and fast PRESETS browsing during a sequence. If underruns appear, record them before changing `CONFIG_FM1_UAC_BUFFER_BYTES`.
- [ ] MIDI CH changed while a note, a KBD HOLD chord and an arpeggio sound; the link coming up while keys are held; a preset change under a held pad in each synthesizer, PROPHET, FM6 and CZ-1 included (a fade, no click, tails ringing); Melodee DRUM 808 KIT → 909 KIT while a pattern plays (the hits cut, no reset); a Clipper WAVE table changed under a held note (silence, then a fade-in).
- [ ] The cartridge unplugged and plugged back while a song plays: START reaches it on the next bar line, and a synced arpeggio or delay on the cartridge stays on Sloop's beat.
- [ ] The return's start and re-prime (a replug, a slot switch): no click; USB RETURN, the cartridge track's LEVEL, MIDI EXPRESSION (CC 11) and MUTE: no zipper; the track's meter follows the return.
- [ ] The cartridge's sound after a restart and after a slot switch back: engine, preset, MIDI CH and edits as left; an NVS write during silence leaves no audible gap.
- [ ] Cartridge track with OUT at KEYS: the keys, steps, arpeggiator and a recorded take of track 1 play the cartridge while its internal engine stays silent; the footer and TRACKS show the cartridge's preset; PRESETS steps the cartridge's presets across engines; MUTE, STOP and MIDI CH = OMNI leave no stuck note.
- [ ] One reference engine and preset per synthesizer before increasing voices; note program, parameters and seed when there is randomness.
- [ ] Compare the reference WAV and the physical capture with the same notes and level; clock or alignment differences are not automatically a DSP regression.
- [ ] Latency measured with event and capture on a common time base; record median, minimum/maximum, number of repetitions and method. Do not add up nominal times and call them a measurement. `tests/test_pipeline.c` models about 25 ms from render to mixer; the measurement decides.
- [ ] CART DELAY (HOME → USB): with the measured latency set, a cartridge-track step and an internal drum hit on the same step land together in a capture of the FM-1's output, at two tempos; record the value, then set `CART_DELAY_DEFAULT_MS` to it.
- [ ] Program Change and Bank Select from the TRS MIDI input on the cartridge track's channel, with each synthesizer: the preset changes with its fade, the names on the footer and the page follow, the track's own sound is untouched, and a knob turned at the same moment is not applied to the old sound.

### G4 measurements on the Super Mini prototype

Build: the protocol 4 `-4m2m` slots (`clipper-4m2m` SHA-256 `27674c01…`, `melodee-4m2m` `a2cd7a3d…`,
`felucca-4m2m` `0d883c53…`; DIO flash and PSRAM at 40 MHz) with the protocol 4 Sloop
package (`felucca.fwsc` `42405b3a…`), the cartridge plugged into the FM-1 on its battery. Figures
read on the CARTRIDGE page (AUDIO RENDER MAX and AUDIO UNDERRUNS, counted from the page's opening);
the engine and preset are recorded where known, the notes played and the time on the page are not.

| Firmware | Engine, preset | What was played | AUDIO RENDER MAX | AUDIO UNDERRUNS |
| --- | --- | --- | ---: | ---: |
| Clipper | not recorded | not recorded | 673 µs of 1,451 | 0 |
| Melodee | PROPHET, not recorded | not recorded | 390 µs | 49 |
| Felucca | not recorded | not recorded | **1,854 µs** | 78 |
| Felucca | ANALOG, SAW LEAD | one key | 765 µs | 22 |
| Felucca | ANALOG, SAW LEAD | several keys | 765 µs | 59 |
| Melodee | PROPHET, not recorded | one key | **1,583 µs** | 31 |
| Melodee | PROPHET, not recorded | several keys | **1,590 µs** | 36 |
| Melodee | PROPHET, "ITS A PROPHET S…" | not recorded | **2,834 µs** | 120; 128 after turning knob 4 |

With more than one chord on Melodee the figures rise further and the sound breaks up (heard).

With QIO flash, PSRAM at 80 MHz and Melodee's per-sample loops in IRAM (`melodee-4m2m` SHA-256
`b69a4cec…`, same protocol 4 Sloop package):

| Firmware | Engine, preset | What was played | AUDIO RENDER MAX | AUDIO UNDERRUNS |
| --- | --- | --- | ---: | ---: |
| Melodee | PROPHET, not recorded | one key | 2,231 µs | 0 |
| Melodee | PROPHET, not recorded | a chord | 2,379 µs | 0 |

The underruns rise only while chords change faster than about once a second. The longest render
stays above the 1,451 µs budget, but as occasional blocks the 11.6 ms ring absorbs, where the
earlier build lost audio with a single key. On the host, a chord change costs no more than the
blocks around it, so the remaining peaks are specific to the ESP32 (the note-on path still runs
from flash, or the MIDI of a chord change is handled in one pass); AUDIO PASS MAX and AUDIO GAP
MAX are not yet recorded.

Reading:

- The three firmwares boot from their slots on the ESP32-S3 and answer the FM-1 with protocol 4.
- Clipper renders within its budget, with no underrun.
- Melodee's PROPHET exceeds the 64-frame budget with a single key, and the sound breaks up with
  chords. Felucca exceeds it on an engine not recorded. On the host both DSPs cost about what
  Clipper's does (a few µs a block), so the excess points to how they run on the ESP32: in the
  build measured, the code ran from flash in DIO mode with PSRAM at 40 MHz, and the functions
  upstream keeps in RAM on the FM-1 ran from flash.
- Felucca's ANALOG renders within budget (765 µs) yet counts underruns, so something other than
  the render also holds up the audio; it is not yet identified.

## G5: firmware slots

- [ ] Each slot chosen from FIRMWARE boots, enumerates, answers ("<NAME> OK") and stays the default after a power cycle.
- [ ] A turn back to the running slot, knob 3 moved off FIRMWARE and the page left within 2 s each cancel; with notes held or the transport running, FIRMWARE reads PLAYING and does not move.
- [ ] A slot that never answers (the TONE profile in a slot, or the FM-1 unplugged during the boot): the next reset returns to the previous slot, which then lists it as "<NAME> FAIL".
- [ ] A power cycle on a pending slot; a full flash, then `ota_0` boots without a pending state.

### G5 observations on the Super Mini prototype

Full backups of the ESP32's flash, read before two later flashes, show the slot state the board
had reached on its own after a full flash had erased `otadata` (so the board had started from
`ota_0` with every slot unconfirmed):

| Backup before | `otadata` entries | What it shows |
| --- | --- | --- |
| the protocol 4 flash | sequence 1 → `ota_0` CLIPPER, VALID; sequence 3 → `ota_2` FELUCCA, VALID | FIRMWARE moved the boot to Felucca, which started and answered the FM-1 |
| the QIO / 80 MHz flash | sequence 11 → `ota_1` MELODEE, VALID; sequence 12 → `ota_2` FELUCCA, VALID | several switches from the FM-1; Melodee and Felucca each started and answered |

A slot is marked VALID only by `fm1_slots_confirm`, after its cartridge server has answered the
FM-1. Cancelling a switch, rollback from a slot that never answers and the power-cycle checks are
not covered by these records.

## G6: power and enclosure

- [ ] ESP32 average current and peaks in idle, tone and maximum synthesizer load; cell voltage during peaks and resets.
- [ ] Cell voltage at which the 3V3 rail drops below 3.0 V during discharge with the Clipper under load (practical cutoff, above the LiPo protection cutoff). Reference on the Super Mini prototype: 3V3 = 3.3 V with the cell at 4.2 V; 2.77 V with the cell at 3.56 V.
- [ ] Actual charge current compared with the cell limit and with the charger of the board revision.
- [ ] The switch on BAT+ really disconnects the battery; USB may power the board even with the switch open.
- [ ] Controlled discharge down to the protection's normal cutoff; record duration, load, temperatures and conditions. Do not infer hours of use from the nominal capacity.
- [ ] With the optional divider (`CONFIG_FM1_BATTERY`): the level the FM-1 shows against a multimeter on BAT+ at 4.2, 3.9 and 3.7 V; LOW and EMPTY at the thresholds; the fade at the cutoff, with no brownout reset before it. Set `CONFIG_FM1_BATTERY_LOW_MV` and `CONFIG_FM1_BATTERY_CUTOFF_MV` from the 3V3 measurement above.
- [ ] Isolation, cell clearance, switch access, strain relief and stress on the port checked before closing the enclosure.

## Run log template

| Field | Value |
| --- | --- |
| ID / date / owner | PENDING |
| Gate / result (PASSED, FAILED, INCONCLUSIVE) | PENDING |
| FM-1: physical revision / firmware / SHA / settings | PENDING |
| ESP32: board model/revision / firmware SHA / IDF / UAC | PENDING |
| Mac/macOS / host / cable / orientations | PENDING |
| Photo or exact diagram of the assembly | PENDING |
| Battery: model / capacity / protection / charge limit | PENDING |
| VBUS host side / FM-1 side, before and during | PENDING |
| Initial/final BAT voltage / average and peak current | PENDING |
| Enumeration: successes/attempts / times / MIDI | PENDING |
| PCM: format / duration / ISO errors / underrun / overrun | PENDING |
| Ring minimum/target/maximum / ASRC ratio / saturations | PENDING |
| Latency: method / samples / median / range | PENDING |
| CPU / internal heap / PSRAM / temperature | PENDING |
| Logs / WAV / capture / photo / file hashes | PENDING |
| Conclusion limited to what was observed | PENDING |
| Next action | PENDING |
