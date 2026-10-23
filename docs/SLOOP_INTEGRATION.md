# Sloop integration

**Status: code, build and native tests in place; part of the hardware validation open.**
The FM-1 runs Sloop 2.7.1 (upstream commit `4ec1ac829fd2bdf2c571ea215fb3d62477d01220`)
with the cartridge profile. The modified tree lives in [`sloop-fm1/`](../sloop-fm1/) and is
edited there.

Provenance is in [integrations/sloop-fm1](../integrations/sloop-fm1/README.md): the whole
difference from upstream as one patch, [`sloop-fm1-cartridge.patch`](../integrations/sloop-fm1/sloop-fm1-cartridge.patch),
and its [manifest](../integrations/sloop-fm1/manifest.json). The tree contains
[`CARTRIDGE_USB_RETURN.md`](../sloop-fm1/docs/CARTRIDGE_USB_RETURN.md) for operation and
[`CARTRIDGE_BUILD.md`](../sloop-fm1/docs/CARTRIDGE_BUILD.md) for build, memory and tests.
Installing the package on the FM-1, with a backup, is in [FLASHING.md](FLASHING.md).

Observed on hardware: a computer recognizes the FM-1 as a stereo 44.1 kHz USB audio output and
its tone plays through the FM-1 speaker; the ESP32's tone and Clipper notes play cleanly; the
CARTRIDGE page shows CLIPPER OK; installing the package with Sloop's updater keeps the FM-1's
projects, settings and presets, as a backup before and after shows. Not yet verified on hardware: USB stereo
L/R, latency, the resampler's cost, long-session stability, capture together with playback, and
the charger control on the SoC.

## USB contract

| Item | Implementation |
| --- | --- |
| Flag | `FELUCCA_UAC_RX=1`, off in the regular build |
| Dedicated profile | `FELUCCA_CARTRIDGE=1`, RX=1, capture=0, CDC=0; preserves MIDI, engines, saving, OTA and limiter |
| MIDI | IF1, bulk OUT 0x01 and IN 0x81, 64 bytes; also carries the cartridge protocol, version 3, and the cartridge track's notes |
| Playback | IF `2 + FELUCCA_UAC`, alt0 no bandwidth, alt1 ISO OUT 0x04 |
| Format | Stereo PCM16LE, 44,100 Hz, up to 180 bytes per 1 ms |
| Synchronization | Adaptive (`bmAttributes=0x09`), no feedback endpoint |
| AudioControl | Input Terminal 3 USB streaming → Output Terminal 4 speaker |
| Rate | EP0 accepts only 44,100; the capture control is independent |
| DMA | Own RX slot on EP4, aligned buffer of 1,028 bytes |

Optional capture stays at 0x84; CDC is shifted after the audio interfaces.
There is no EP5. The tests cover descriptors and EP0 in 24 configurations of RX,
capture, CDC and rate; duplex and CDC on the hardware still need testing.
The dedicated profile's configuration exported over EP0 is 175 bytes and is
in `tests/sloop_cartridge_descriptor.h`, tested by the parser used on the ESP32.
The cartridge frames follow `firmware/src/cartridge_wire.h`, byte-identical to
`core/include/fm1/cartridge_wire.h` ([CARTRIDGE.md](CARTRIDGE.md)).

## Clocks, concurrency and mixer

Sloop's own receiver uses a ring of 1,024 stereo frames (4,096 bytes), an occupancy
target of 384 frames measured per block of 32, and a start at 512. The DAC consumes
256 frames per burst, so the minimum after a burst stays near 5 ms and a late packet
does not become an underrun. TIMER5 is the PCM producer; the core 0 render is the
consumer. The urgent service continues during the nested render. DMA uses a barrier
before reading; separate cursors and reset generations prevent old audio after interruptions.

The ASRC uses 4-point Hermite interpolation, an occupancy-based PI loop and a Q24 phase,
with the smoothed correction limited to ±5,000 ppm. Linear interpolation would let the slowly
sweeping fraction modulate the treble by up to 2.4 dB at 10 kHz; with Hermite the modulation
stays below 0.5 dB. The reference receiver in `core/audio_return.c` is separate: it is not
used inside the FM-1 ISR. Musical quality and the ASRC's cost on the target are not yet measured.

The return enters once on core 0, after summing the cores, before texture,
global effects, master and limiter. It does not feed per-track sends.
**HOME → USB → USB RETURN** sets OFF–100% with knob 1; OCT+ toggles mute/100%.
**CHARGE** (knob 2) holds the FM-1's internal charger off while the cartridge answers (AUTO) or
leaves it as the boot set it (ON).
The gain stays in RAM, without changing the format of saved projects and settings.
Mute keeps draining the ring.

Alt0, reset, deconfiguration, suspend, transport error, invalid packets,
overflow and a 20 ms timeout lead to discard/reprime. Diagnostics include
under/overrun, occupancy, correction and resets; there is no logging in the USB callback.
The diagnostic build shows receiver counters in the CARTRIDGE footer
([diagnostic build](CARTRIDGE.md#diagnostic-build)).

## Memory and validation

Upstream and the cartridge profile build with the same toolchain. Upstream 2.7.1 leaves
3,644 B free in the app image; the cartridge profile disables USB capture and the CDC console
to make room for the receiver. It uses 579,952 of 581,564 bytes of the app image, with **1,612 B
free**. Main RAM: 60,844 B; low RAM: 22,772 B; pool: 333,900 B,
preserving 10,164 B free (minimum of 8,192 B). The package
is built at `build/felucca.fwsc` in `sloop-fm1/`.

`sh tests/run_cartridge_tests.sh` passes natively and with ASan/UBSan, including
±1,000 ppm drift for 120 s, reset during render, the real mix with two cores,
gain/mute, the menu and the CARTRIDGE page. With the return disabled or at zero gain the output
matches the upstream reference bit for bit. `tools/test_sloop_cartridge.sh` runs Sloop's UI
against the real ESP32 server (101 checks, native and sanitized). These results are not
equivalent to physically reproduced audio.

The open bench work follows gates G0 to G6: the power and VBUS path, then **computer → Sloop**
with stereo, gain, effects, reconnection and measurement of the deadlines, then the ESP32 with
its tone and each synthesizer. See the [bench tests](BENCH_TESTS.md) and the [roadmap](ROADMAP.md).
