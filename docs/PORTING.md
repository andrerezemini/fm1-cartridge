# Porting another synthesizer to a slot

Any slot can hold another synthesizer: the slot list, the names on the FM-1 and the rollback come
from the images themselves ([FIRMWARE_SLOTS.md](FIRMWARE_SLOTS.md)), and the cartridge manager
writes any image into any slot ([WEB_MANAGER.md](WEB_MANAGER.md)). What a new synthesizer needs is
an adapter behind `synth/fm1_synth.h` and a firmware profile. Clipper, Felucca and Melodee were
ported this way; the steps below are theirs.

## What makes a good candidate

- **A GPL-3.0-compatible licence**, since the slot firmware is GPL-3.0-only: GPL-3.0, GPL-2.0-or-later,
  LGPL, MIT, BSD, Apache-2.0, MPL-2.0 (without the "Incompatible With Secondary Licenses" notice),
  zlib, CC0. A repository without a licence cannot be redistributed in a slot.
- **A host build of the DSP.** FM-1 firmwares derived from Felucca ship `tests/hostsim.c`, which
  compiles engines, voices, parameters, effects and the sequencer's MIDI handling without
  hardware. Its includes are the port's file list.
- **Sound, not another sequencer.** The FM-1 runs Sloop; a slot adds engines and presets that the
  cartridge track plays. A firmware whose value is its sequencer or its keyboard UI adds little.
- **One core and the ESP32-S3's memory.** Internal RAM is about 341 KB, with Melodee using 251 KB;
  voices and per-sample state must stay in internal RAM, and only large, sequentially read
  buffers can go to PSRAM. Code written for the FM-1's second core must have a single-core path.

The Synths tab of the manager checks a repository's licence, release and `tests/hostsim.c`, and
keeps the result in `web/catalog.json` as a candidate.

## Steps

1. **Pin the upstream.** Choose a tagged release, note its commit (and those of its submodules).
2. **Vendor the subset** into `third_party/<name>/`, mirroring upstream paths: the transitive
   closure of the files `tests/hostsim.c` includes, the generators of the tables it needs, their
   inputs, `LICENSE`, `LICENSING.md` and every licence of the files taken. Record the SHA-256 of
   each file as taken in `UPSTREAM_SHA256.json`, and add the subset to `tools/check_upstream.py`.
3. **Change upstream only where the hardware is assumed** (the FM-1's XIP flash, an arena size), as
   little as possible; list each changed file in the subset's README and in `PATCHED` of
   `check_upstream.py`. Write the README like the existing three: origin, what is compiled, what is
   not, changes, licences, how to regenerate `generated/`.
4. **Generate the tables** into `third_party/<name>/generated/` with the vendored generators
   (Python 3 without extra packages, as the existing ones).
5. **Write the adapter**, `synth/<name>_synth.c` and `.h`: one translation unit that includes the
   upstream sources in the order of `tests/hostsim.c`, defines the single-core switches, replaces
   the hardware hooks (interrupt control, MIDI out, the keyboard) with no-ops, and feeds the
   upstream MIDI queue from `midi()`. It returns an `fm1_synth_api_t` whose behaviour matches
   the table in [PORTS.md](PORTS.md#common-interface-synthfm1_synthh): 44.1 kHz, any render size,
   `panic`, `release`, factory presets by engine, entry 0 of the parameter catalogue `MIDI CH`, and
   `name` (12 characters at most, shown as "<NAME> OK") and `revision`.
6. **Build and test natively**: a `fm1_synth_library(<name>_synth <name> ...)` and a
   `fm1_port_targets(<name> <engines> <revision>)` line in `CMakeLists.txt` give the simulator and
   the generic `synth` and `cartridge` tests; add the synthesizer to `web/catalog.json` as
   `ported`, which the `catalog` test checks against the adapter and CMake.
7. **Add the firmware profile**: a `FM1_AUDIO_<NAME>` choice in `firmware/main/Kconfig.projbuild`,
   its source and `.lf` fragment in `firmware/main/CMakeLists.txt`, its API in
   `firmware/main/audio_tx.c`, `firmware/sdkconfig.<name>` (with `CONFIG_APP_PROJECT_VER` the name
   the FM-1 shows) and the two profiles in `firmware/build.sh` and the CI matrix. The `.lf` fragment
   moves only the large delay or sample buffers to PSRAM.
8. **Check the budget on the ESP32-S3**: the image must fit a 1.25 MB slot (ESP-IDF refuses it
   otherwise), `idf.py size` gives the internal RAM, and on the bench the CARTRIDGE page's AUDIO
   RENDER MAX shows the worst render time against the 1,451 µs of a 64-frame block. Reduce voices
   or effects if it does not fit; never grow the audio ring to hide underruns.
9. **Flash it into a slot** with the manager or `flash-slots.sh`, and record what is heard in
   [BENCH_TESTS.md](BENCH_TESTS.md).

## Updating a port

The same steps against the new release: diff the two revisions over the subset, recompute the
closure from the new `tests/hostsim.c`, refresh the files and `UPSTREAM_SHA256.json`, re-apply the
documented changes, regenerate the tables, adapt the adapter, and update the revision in the
adapter, `CMakeLists.txt`, `web/catalog.json` and the docs. The manager's Updates tab shows which
upstreams have new releases and the commits since the pinned one.
