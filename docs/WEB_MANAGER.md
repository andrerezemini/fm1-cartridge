# Cartridge manager: firmwares and updates from the browser

`web/` is a page that installs and updates what runs on both devices, without a terminal:

- **Slots:** reads the ESP32-S3's partition table, its `otadata` and the image in every slot (name,
  build date, ESP-IDF version, ELF hash, which slot boots next and whether one was rolled back),
  backs up the whole flash to a file, and writes any of this repository's builds, or an image
  file, into the slots. A tagged release's merged image (or a backup) can be written whole at 0x0.
- **Synths:** the catalogue (`web/catalog.json`) of the ported synthesizers, with the local builds
  and their share of a slot, and of the candidates whose sound could become a slot
  ([PORTING.md](PORTING.md)). A GitHub repository can be checked (licence, latest release, a host
  build of its DSP) and added as a candidate.
- **Updates:** what every pinned upstream (Sloop, Clipper, Felucca with Felucca-Engines and
  felucca-bsp, Melodee, ESP-IDF's 5.3 line, `usb_host_uac`) has released since this repository took
  it, with the commits in between; and, with the cartridge connected, which slots run a build other
  than the local one of the same firmware.
- **FM-1 (Sloop):** installs the Sloop package of this repository
  (`sloop-fm1/build/felucca.fwsc`, or a chosen `.fwsc`) through Sloop's own updater
  (`sloop-fm1/web/fm1ota.js`), the same protocol as Sloop's installer page and `fm1_install.py`.

The page never builds anything. Builds stay the scripts' (`firmware/build.sh`,
`sloop-fm1/build-cartridge.sh`), and every write to a device is a button press with a
confirmation.

## Starting it

```sh
python3 tools/cartridge_web.py --open
```

It serves the page at `http://127.0.0.1:8737/web/` from this checkout, with the local builds under
`firmware/build-<profile>/` and Sloop's package. Open it in **Chrome or Edge** on a computer: the
cartridge is reached through Web Serial and the FM-1 through Web MIDI with SysEx, which other
browsers lack. The catalogue and the update check work in any browser.

The server listens on the loopback interface only, answers only requests whose `Host` is
`127.0.0.1` or `localhost` on its port (no DNS rebinding), serves a fixed list of paths (the page,
the five build outputs of each profile, Sloop's updater modules and package) and accepts one write:
a candidate added to `web/catalog.json` from its own page (same `Origin`, JSON only, never over a
ported synthesizer). Any static server at the repository root also serves the page; the local builds
are then found by probing the catalogue's profiles, and a new candidate is offered as a download of
`catalog.json` instead.

The page loads [esptool-js](https://github.com/espressif/esptool-js) 0.7.0 (Apache-2.0) from
jsDelivr, pinned by version and by a SHA-384 integrity hash in its import map; the first connection
needs the network once. The update check calls GitHub's public API (60 requests an hour without an
account; results are kept for 15 minutes in the tab) and the ESP component registry.

## Writing the slots

1. Unplug the cartridge from the FM-1 and connect it to the computer. Hold **BOOT**, press and
   release **RESET**, release BOOT: the ESP32-S3 starts its ROM loader on the USB Serial/JTAG port.
2. **Connect** and choose that port. The page refuses a chip other than the ESP32-S3 and shows the
   MAC, the flash size and what each slot holds.
3. **Back up the whole flash** (or confirm a backup of this board already exists): writing stays
   locked until then. The file is the whole flash, `esptool.py write_flash 0 <file>` or the release
   image card puts it back.
4. For each slot, pick a local build, an image file or Keep. Every choice is checked before the
   write: an ESP32-S3 application image with an `esp_app_desc_t`, small enough for the slot, built
   for the board's flash (a `-4m2m` profile on the Super Mini, a plain one on an 8 MB board, as
   `flash-slots.sh` checks), every build from the same partition table; an image whose project is
   not `fm1_cartridge` is allowed with a warning, since it never answers the FM-1 and the
   bootloader rolls it back after one boot ([FIRMWARE_SLOTS.md](FIRMWARE_SLOTS.md)).
5. **Write**. As with `flash-slots.sh`, writing slot 1 (`ota_0`), or a board whose partition table
   differs from the builds', is a full install: the bootloader, the table and a clean `otadata` from
   one of the builds go too, so the board starts from slot 1 with every slot unconfirmed; the kept
   sounds in `nvs` stay. Writing only other slots leaves the table and `otadata` alone. esptool-js
   compares the MD5 of every written region with the file before reporting success.
6. Press **RESET** (or power the board off and on) and plug it into the FM-1.

## Installing Sloop on the FM-1

Unplug the cartridge, connect the FM-1 to the computer, back it up
(`python3 tools/fm1_backup.py backup-before.json`), then **Use the local build** or choose a
`.fwsc`, **Find the FM-1** and **Install**. The package's identity must be a SLOOP/Felucca one
(`FM-1_9xx`); an FM-1 left in Sloop's update loader is finished from where it stopped. The
procedure and its checks are the ones of [FLASHING.md](FLASHING.md#fm-1-sloop-with-the-cartridge-profile).

## Tests

CTest runs three groups for the manager: `web_manager` (Node, when installed) parses partition
tables, `otadata` (its CRC checked against values an ESP32-S3 wrote), application headers and the
slot checks, and compares the MD5 with Node's; `catalog` checks that `web/catalog.json` names the
revisions the adapters, `CMakeLists.txt` and the Sloop manifest use, profiles `firmware/build.sh`
knows and the pinned esptool-js; `cartridge_web` checks the server's fixed paths, its Host and
Origin checks and its candidate-only writes. Reading and writing a real board through the page is a
bench step: record it in [BENCH_TESTS.md](BENCH_TESTS.md) like any other flash.
