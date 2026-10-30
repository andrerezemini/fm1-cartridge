# Native simulation: MIDI, synthesizers and audio return

`cartridge_sim` runs natively the chain simulated USB-MIDI → **real synthesizer DSP** → PCM
packets in memory → reference receiver with clock adaptation → WAV/JSON. `cartridge_sim` uses the
Clipper DSP; `cartridge_sim_clipper`, `cartridge_sim_felucca` and `cartridge_sim_melodee` take the
same options with each port ([PORTS.md](PORTS.md)). The simulator does not open a USB port, does
not control the FM-1 and is not a Sloop emulator. The receiver in `core/audio_return.c` is an
independent reference; the Sloop firmware has its own concurrent receiver, described in
[Sloop integration](SLOOP_INTEGRATION.md) and
[CARTRIDGE_USB_RETURN.md](../sloop-fm1/docs/CARTRIDGE_USB_RETURN.md#reception-and-clocks).

## Build and verify

From the repository root, with CMake 3.20+, a C11 compiler and Python 3 available:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
mkdir -p out
```

The examples below write into `out/`, which Git ignores.

CTest runs 13 tests. They cover descriptor and MIDI parsing, packetization, receiver
drift/overflow/restart, the integrity of the vendored upstream files, and, for each port, the
rendering of every engine (23 in Clipper, 13 in Felucca, 5 in Melodee; `tests/test_synth.c`, one
executable per port), the cartridge server with simulated slots
(`tests/test_cartridge.c`), release/panic and the consistency of the WAV and the report
(`tests/test_simulation.py`). They do not measure ESP32 or FM-1 performance.

Optional sanitizers for native checks:

```bash
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug -DFM1_SANITIZE=ON
cmake --build build-sanitize --parallel
ctest --test-dir build-sanitize --output-on-failure
```

The upstream DSP uses fixed-point operations with wraparound; the build keeps `-fwrapv` and
disables only the `shift-base`/`signed-integer-overflow` checks on the synthesizer targets. This
is an explicit compatibility choice, not a claim of full undefined-behaviour coverage.

## Examples

Render of a sequence on Clipper, with the consumer 400 ppm faster:

```bash
./build/cartridge_sim --seconds 10 --ppm 400 --engine 0 --preset 0 \
  --wav out/clipper.wav --report out/clipper.json
```

The same commands work with `./build/cartridge_sim_felucca` and `./build/cartridge_sim_melodee`;
`--engine` and `--preset` then index that port's catalogue.

440 Hz sine tone, identical on both channels, at about −20 dBFS, to observe the transport without
the synthesizer's timbre:

```bash
./build/cartridge_sim --tone --seconds 10 --ppm 400 \
  --wav out/tone.wav --report out/tone.json
```

In `--tone` mode the MIDI parser validates the events as usual, but they are not queued into the
synthesizer. The tone replaces only the PCM source.

Losses, disconnection and a malformed packet:

```bash
./build/cartridge_sim --faults --seconds 8 \
  --wav out/faults.wav --report out/faults.json
```

Comparison of the drift with and without adaptation, long enough to show draining or
accumulation instead of a merely larger buffer:

```bash
./build/cartridge_sim --tone --seconds 120 --ppm 1000 \
  --wav out/adaptive.wav --report out/adaptive.json
./build/cartridge_sim --tone --seconds 120 --ppm 1000 --no-adaptive \
  --wav out/fixed.wav --report out/fixed.json
```

Repeating with `--ppm -1000` reverses the direction of the drift. Compare `underruns`,
`overruns`, fill and `final_ratio`. The process may exit successfully while the output contains
simulated audio faults, since those faults are the subject of the experiment. To listen to a file
on macOS: `afplay out/clipper.wav`.

## Options

| Option | Default / limit | Meaning |
| --- | --- | --- |
| `--wav path` | `cartridge.wav` | Stereo PCM16 WAV with a nominal 44,100 Hz header; the directory must exist |
| `--report path` | `cartridge.json` | JSON report; the path must differ from the WAV |
| `--seconds number` | 10; 1–600 | Simulated duration, quantized in milliseconds; not real processing time |
| `--ppm number` | 400; −2000 to 2000 | Consumer deviation: positive consumes more frames per ms |
| `--faults` | Off | Injects the fixed scenario described below |
| `--no-adaptive` | Off | Fixes the receiver ratio at 1, removing the drift correction |
| `--tone` | Off | Uses a 440 Hz sine as the PCM source |
| `--engine integer` | 0; 0–22 in Clipper, 0–12 in Felucca, 0–4 in Melodee | Engine index in the port's catalogue (the order the FM-1 shows); names in the adapter API |
| `--preset integer` | 0; 0–255 on the CLI | Factory preset index; must exist in the chosen engine (Melodee's PROPHET has 201; only the first 256 of any catalogue are reachable from the CLI) |
| `--help` | — | Shows the options |

Existing output files are overwritten. The generated sequence plays notes
`60, 64, 67, 72, 67, 64, 62, 55`, one every 500 ms, each released after 400 ms, with a 120 BPM
MIDI clock. It is a deterministic fixture, not a sequencer or a MIDI file reader. Presets are
chosen through the API or the CLI; every adapter rejects Program Change (the cartridge server
handles it before the adapter, [CARTRIDGE.md](CARTRIDGE.md#program-change-and-bank-select)), and
Clipper also rejects CC 20.

With `--faults`, the pair is disconnected between 3.0 and 3.3 s, loses packets between 6.0 and
6.1 s and periodically every 997 ms, and receives an invalid three-byte packet at 7.0 s. Run at
least 8 s to include all phases. The reset discards old PCM and calls the synthesizer's panic; the
scenario counts one reconnection.

## How to read the report

- `simulation_only: true`, the upstream revision, the source/engine/preset and the options
  identify the run.
- `wav_frames = rendered_frames + silent_frames`; the initial priming silence is expected and is
  not by itself a loss.
- `received_frames` counts accepted PCM; injected losses appear separately.
- `underruns`/`overruns` count ring faults, not errors measured on a real USB bus.
- `malformed_packets` counts invalid formats and sizes. `reconnects` is a scenario event, not
  cable detection.
- `fill_min`/`fill_max` are stereo frames; `final_ratio` is the final read ratio of the reference
  ASRC, without a latency unit.

The WAV file keeps 44,100 Hz in its header, while the number of frames produced depends on the
consumer's ppm. Its played-back duration may therefore differ slightly from `--seconds`; this
reflects the simulated clock and is not a timing measurement of the hardware.

## Limits of the reference receiver

The reference ring has 1,024 frames and a target of 256 frames. The correction uses linear
interpolation, a fill filter and a limit of ±3,000 ppm. The code has a single owner and **is not
a ring safe for ISRs or concurrent tasks**. The Sloop receiver uses a concurrent ring, reset
generations, DMA on EP4 OUT and a fixed-point ASRC with 4-point Hermite interpolation, limited to
±5,000 ppm, with an occupancy target of 384 frames. Do not confuse the results of the two
receivers; cost and quality on the FM-1 are measured on the bench.

The PCM parser accepts up to 196 bytes (49 frames) for transport tests at the same nominal rate.
This does not change the playback profile of the USB contract ([PROTOCOL.md](PROTOCOL.md)),
**44.1 kHz with at most 180 bytes per packet**, and the simulator does not convert a nominal
48 kHz stream to 44.1 kHz. The real firmware respects the maximum announced in the descriptor.

The simulator does not include a USB controller, libusb, macOS UAC, FreeRTOS, a DAC, CC/VBUS,
electrical noise, power consumption or interrupt timing. Those points are validated through the
bench gates ([BENCH_TESTS.md](BENCH_TESTS.md), [ROADMAP.md](ROADMAP.md)); for return audio, G2
(computer → Sloop) comes before G3 (ESP32 → Sloop).
