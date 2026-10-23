# Project guidelines

- The topology is an ESP32-S3 USB host running a synthesizer DSP, the FM-1/Sloop as the USB device, and the physical audio output on the FM-1. `docs/ARCHITECTURE.md` describes it.
- Upstream Sloop has no USB playback; `sloop-fm1/` adds it (provenance in `integrations/sloop-fm1/`). Do not present a build or a simulation as proof of audio on the hardware.
- Follow the gates in `docs/ROADMAP.md`; record real evidence in `docs/BENCH_TESTS.md` without marking measurements that were not performed as passed.
- Preserve the revision and licences in `third_party/clipper/`, `third_party/felucca/` and `third_party/melodee/`. Changes to the upstream subsets must be minimal, documented (in the README of each one) and reproducible; `tools/check_upstream.py` audits all three.
- The three synthesizers sit behind `synth/fm1_synth.h`; the cartridge server (`synth/cartridge_remote.c`), the audio task, the simulator and the tests use only that interface. Each firmware occupies one slot of `firmware/partitions.csv`; the FM-1 selects the slot through the FIRMWARE parameter (`docs/FIRMWARE_SLOTS.md`).
- Shared code lives in `core/` and `synth/`. USB callbacks must not run heavy DSP; the synth API is serialized by the audio task.
- USB/DMA buffers need appropriate internal memory. PSRAM, USB-C, the power supply and deadlines are verified on the bench.
- Verify changes with the native build and CTest. For firmware, build the affected profiles with ESP-IDF 5.3.6. Flashing is never part of a build (`docs/FLASHING.md`).
- The FM-1 firmware lives in `sloop-fm1/` and is edited there. After changing it, regenerate `integrations/sloop-fm1/sloop-fm1-cartridge.patch` and `manifest.json` against upstream commit `4ec1ac829fd2bdf2c571ea215fb3d62477d01220`. Builds: `cd sloop-fm1 && sh build-cartridge.sh` (Docker and the JieLi toolchain); tests: `sh tests/run_cartridge_tests.sh` and `sh tools/test_sloop_cartridge.sh`.
- `core/include/fm1/cartridge_wire.h` and `sloop-fm1/firmware/src/cartridge_wire.h` must stay byte-identical; a change to the wire format bumps the protocol version byte.
- Documentation describes the project as it is, in English and in the present tense: no dated diaries or development history.
