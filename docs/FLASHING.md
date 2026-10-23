# Flashing the cartridge and the FM-1

Flashing is never part of a build. Both procedures below take a full backup first and check the
result afterwards. Connect one device to the computer at a time: the ESP32 and the FM-1 are not
connected to each other while flashing.

## ESP32-S3: the three firmware slots

Requirements: ESP-IDF 5.3.6 with `esp32s3` support activated (`. "$IDF_PATH/export.sh"`, which
provides `esptool.py`), and the profiles built (`firmware/build.sh`, see
[firmware/README.md](../firmware/README.md)). Use the `-4m2m` profiles for a board with 4 MB of
flash and 2 MB of quad PSRAM (the Super Mini prototype) and the plain profiles for the 8 MB XIAO.

1. Disconnect the FM-1. Put the ESP32 in its USB bootloader: hold BOOT, press and release RESET,
   release BOOT. The board appears as a USB Serial/JTAG port (`/dev/cu.usbmodem*` on macOS).
2. Identify the chip and the flash, without resetting it:

   ```sh
   esptool.py --chip esp32s3 -p /dev/cu.usbmodem101 --before no_reset --after no_reset flash_id
   ```

3. Back up the whole flash (4 MB here; 8 MB on the XIAO):

   ```sh
   esptool.py --chip esp32s3 -p /dev/cu.usbmodem101 -b 460800 --before no_reset --after no_reset \
       read_flash 0 0x400000 esp32-backup.bin
   ```

4. Write the bootloader, the partition table and the three slots. The script erases `otadata`, so
   the board starts from slot 1 (`ota_0`) with no pending slot:

   ```sh
   cd firmware
   ./flash-slots.sh -p /dev/cu.usbmodem101 -b 460800 \
       ota_0=build-clipper-4m2m ota_1=build-melodee-4m2m ota_2=build-felucca-4m2m
   ```

   esptool verifies the hash of every image after writing it.
5. Read every region back and compare it with the files, while the board is still in its
   bootloader:

   ```sh
   esptool.py --chip esp32s3 -p /dev/cu.usbmodem101 -b 460800 --before no_reset --after no_reset \
       read_flash 0x10000 $(stat -f %z build-clipper-4m2m/fm1_cartridge.bin) slot1.bin
   cmp slot1.bin build-clipper-4m2m/fm1_cartridge.bin
   ```

   Repeat with `0x150000` for slot 2, `0x290000` for slot 3 and `0x8000` (3,072 bytes) for the
   partition table (`stat -c %s` on Linux).
6. Reset the board with RESET or by power-cycling it. The reset over RTS can leave the chip in
   download mode; once the firmware runs, it takes the USB port as host and the serial port
   disappears. Logs are on the UART console (TX GPIO43, RX GPIO44, 115,200 baud, 3.3 V logic).

To replace a single slot, name only that slot (`./flash-slots.sh -p PORT ota_1=build-melodee-4m2m`):
the table and `otadata` stay untouched. Secure Boot, flash encryption and eFuses are never touched
by these steps. Restoring the backup is `esptool.py ... write_flash 0 esp32-backup.bin`.

## FM-1: Sloop with the cartridge profile

Requirements: Python 3 with `mido` and `python-rtmidi`, the package built in `sloop-fm1/`
(`sh build-cartridge.sh` produces `build/felucca.fwsc`; see
[integrations/sloop-fm1](../integrations/sloop-fm1/README.md)), the cartridge unplugged and the
FM-1 connected to the computer over USB, running its firmware (not the update loader).

1. Check the identity of the connected FM-1:

   ```sh
   python3 sloop-fm1/tools/fm1_install.py --info
   ```

2. Back up the FM-1's exportable objects (projects, settings, user presets) through the Sloop
   editor protocol. Every object's length and CRC are checked; the file can be imported by the
   Sloop web editor:

   ```sh
   python3 tools/fm1_backup.py backup-before.json
   ```

3. Install the package with Sloop's official updater, without `--force`. It checks the package,
   starts the update loader, writes the image, reboots the FM-1 and confirms its identity:

   ```sh
   python3 sloop-fm1/tools/fm1_install.py sloop-fm1/build/felucca.fwsc
   ```

4. Back up again and compare every object with the first backup:

   ```sh
   python3 tools/fm1_backup.py backup-after.json backup-before.json
   ```

   Right after the reboot the computer may still be re-enumerating the FM-1 (on macOS, a CoreMIDI
   error -304 when opening the port); wait a few seconds and run it again.

The identity `FM-1_900` is shared by local Sloop builds: the confirmation is the updater's, on the
verified package, not a full read-back of the FM-1's flash. To leave the cartridge profile, install
any other Sloop or Felucca package the same way, or use the vendor's updater to return to the
official firmware.

## After flashing both

Connect the cartridge to the FM-1 and follow [CARTRIDGE.md](CARTRIDGE.md): the page should show
"CARTRIDGE 2/10" and CLIPPER OK. Both devices must run the same cartridge protocol version.
