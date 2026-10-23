# Roadmap and gates

The gates go in this order. Native tests move forward in parallel with the physical tests, but
they do not clear a hardware gate. Record the binary hashes, tools, assembly and results in
[BENCH_TESTS.md](BENCH_TESTS.md). `PENDING` does not mean failure; it means there is no evidence
in this repository yet.

The hardware is a bench prototype: an ESP32-S3 Super Mini with 4 MB of flash and 2 MB of quad
PSRAM (circuit revision unconfirmed) on a protected 1S LiPo, plugged into the FM-1 through a USB-C
male-to-male adapter ([hardware](../hardware/README.md)).

| Gate | Deliverable and dependency | Exit criterion | Physical state |
| --- | --- | --- | --- |
| G0 | Computer ↔ Sloop powered by the battery, with VBUS interrupted | Repeatable enumeration; MIDI received; VBUS measured; orientation and connection order recorded | **PENDING** (VBUS and 3V3 measured through the USB-C adapter, see [BENCH_TESTS.md](BENCH_TESTS.md)) |
| G1 | ESP32 ↔ Sloop, MIDI | Descriptors and bulk IN; Note On/Off and clock in the logs; disconnection/reconnection without a stuck voice | **The FM-1 keyboard plays the synthesizer on the ESP32; only the track on the configured channel reaches it. Logs, clock and reconnections PENDING** |
| G2 | ISO OUT, ring, rate adjustment and mixer in Sloop; tested from a computer | The computer recognises the UAC output; tone/stereo on the physical output; memory and load measured; capture/CDC tested separately | **A 440 Hz tone from a computer is heard on the speaker; stereo, load and stability PENDING** |
| G3 | The ESP32 sends a UAC tone to the G2 Sloop | Continuous audio, simultaneous MIDI, losses and latency measured; silent fallback/reconnection | **A tone and Clipper notes are heard; losses, latency and stability PENDING** |
| G4 | Synthesizers on the ESP32-S3 | Same events/presets reproducible on the host and the ESP32; CPU/memory profile; notes and presets without artifacts | **Clipper: clean notes. Melodee and Felucca flashed, not heard. Other presets, `render_max_us`, memory and stability PENDING** |
| G5 | cartridge control over SysEx and the firmware slots | Engine/preset/parameter/value on the four knobs; confirmed state; reconnection; slot switch and rollback | **Handshake (CLIPPER OK) shown. Knobs, slot switching and rollback PENDING** |
| G6 | LiPo, switch, connector and enclosure | Current/peaks/charge/runtime measured; no backfeed; cable/fit firm without stress on the port | **PENDING** |

## Software in place

- The real DSPs of Clipper, Felucca and Melodee behind `synth/fm1_synth.h` ([PORTS.md](PORTS.md)),
  native tests of the protocol, the receiver and each synthesizer, a CLI simulator with WAV/JSON
  per synthesizer ([SIMULATION.md](SIMULATION.md)), and the cartridge server.
- The ESP-IDF firmware with one profile per synthesizer, three firmware slots and rollback
  ([FIRMWARE_SLOTS.md](FIRMWARE_SLOTS.md)). The UAC TX profiles are experimental; the MIDI profile
  sends no audio.
- Sloop with a concurrent ring, ASRC, mixer input, gain control, the CARTRIDGE page, the cartridge
  track and the charger control, following the [SLOOP_INTEGRATION.md](SLOOP_INTEGRATION.md) contract
  ([patch and provenance](../integrations/sloop-fm1/README.md)).
- Test coverage in [VALIDATION.md](VALIDATION.md).

## Software practice

1. Pin source versions, licences and the contract; keep FM-1-dependent code isolated from the
   ESP32 transport.
2. Run native tests of MIDI/descriptor parsing and PCM generation; cover truncated data, USB
   directions and alternate settings.
3. Exercise the ring and the rate adaptation with different clocks, jitter, losses and
   reconnection. WAV and metrics are evidence of logic, not of real latency.
4. Build the affected ESP-IDF profiles and record version and configuration. A build does not
   confirm enumeration or DMA.

## Decisions after each gate

- **G0 passes:** continue with independent power and data/GND, keeping the experimental assembly
  documented. Still test G1; a computer and the ESP32 do not necessarily share the same
  initialization sequence.
- **G0 fails:** check the adapter, firmware, battery, orientation and sequence; repeat the control
  with normal VBUS. The failure does not prove a USB host defect. A powered VBUS needs a proper
  USB-C source design ([future hardware work](../hardware/README.md#future-hardware-work)).
- **G1 passes, G2 pending:** the cartridge proves MIDI but cannot return sound to the FM-1. Do not
  call this state a working product.
- **G2 passes, G3 fails:** compare descriptors, UAC controls, 44.1 kHz packetization and client
  coexistence on the ESP32, using a computer as the reference.
- **G3 stable, G4 heavy:** reduce the measured polyphony, effects or engine; do not mask underruns
  by enlarging the ring indefinitely.
- **G6:** close the enclosure only after electrical and audio evidence. Runtime depends on the
  cell's capacity and the measured consumption.

USB-C compatibility, the voice limit, the latency and the runtime have no promised values until
they are measured.
