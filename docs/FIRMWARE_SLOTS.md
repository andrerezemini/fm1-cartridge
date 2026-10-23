# Three firmwares on the cartridge: slots, selection from the FM-1 and rollback

The cartridge's ESP32-S3 holds **three firmwares at the same time** (Clipper, Melodee and
Felucca, one per slot) and the FM-1 chooses which of them boots from the GLO → CARTRIDGE page.
Switching firmware writes nothing beyond the bootloader's 8 KB `otadata`: the switch takes 1 to
2 s and the FM-1 sees a USB disconnection and reconnection. The three slots are flashed from a
computer once ([FLASHING.md](FLASHING.md)); after that, switching needs no computer.

## Partition table

`firmware/partitions.csv`, the same for every profile (4 MB of flash on the Super Mini; on the
XIAO's 8 MB the second half stays unused):

| Partition | Offset | Size | Contents |
| --- | --- | --- | --- |
| `nvs` | 0x9000 | 16 KB | unused |
| `otadata` | 0xd000 | 8 KB | the slot the bootloader boots and the state of each slot |
| `phy_init` | 0xf000 | 4 KB | radio calibration (unused) |
| `ota_0` | 0x10000 | 1.25 MB | slot 1 (the bootloader starts with it after a full flash) |
| `ota_1` | 0x150000 | 1.25 MB | slot 2 |
| `ota_2` | 0x290000 | 1.25 MB | slot 3 |

There is no `factory` partition: the three slots are equal. The current binaries occupy between
40% and 56% of a slot (see [firmware](../firmware/README.md)). Changing the table needs a full
flash, since `otadata` and the slot offsets depend on it.

## What each firmware needs

Each slot holds a complete ESP-IDF firmware from this repository (USB host, UAC TX, MIDI and the
cartridge protocol) built with one of the adapters in `synth/`. Only the DSP differs:
`clipper_synth.c`, `melodee_synth.c` or `felucca_synth.c`, all behind the `synth/fm1_synth.h`
interface. A third-party firmware that does not implement the USB host and the protocol does not
talk to the FM-1 and, if booted from a slot, is never confirmed (below): the next reset goes back to
the previous slot.

The name the FM-1 shows ("MELODEE OK", the FIRMWARE list) is the profile's
`CONFIG_APP_PROJECT_VER`, read from the header of each image (`esp_app_desc_t.version`): CLIPPER,
MELODEE, FELUCCA, TONE, MIDI. A slot without a valid image appears as **EMPTY** and
cannot be chosen.

## Selection from the FM-1

On the CARTRIDGE page, **FIRMWARE** is the second entry of the parameter list (knob 3; the first is
MIDI CH). The value (knob 4) steps through the slots by name, with the position "2/3" on the right
and the unit **CURRENT** on the running slot. When the knob rests on another slot, the
ESP32 waits `CONFIG_FM1_SLOT_SWITCH_MS` (2 s), sets the chosen slot as the boot partition
(`esp_ota_set_boot_partition`, which verifies the whole image first) and reboots 300 ms later, so
the reply reaches the FM-1 first. Returning to the current slot within the 2 s cancels. While
FIRMWARE is selected the footer reads "KNOB 4: SLOT, REBOOTS IN 2 S".

After the reboot the FM-1 shows NO LINK until the new firmware enumerates (the
discovery probes sent on every USB configuration find the cartridge again) and then "<NAME> OK".
The engines, presets and parameters become the new firmware's. Edits made in the previous firmware
lived in RAM and are lost, as in a reset.

There is no confirmation button: the cartridge has no screen or buttons, and the FM-1's buttons have
global functions on the page. The choice takes two deliberate actions (selecting FIRMWARE with
knob 3 and turning knob 4) and can be undone within 2 s. The Super Mini's BOOT button cannot serve as
"hold to go back": GPIO0 low at reset enters the ROM download mode before the bootloader runs.

## Bootloader rollback

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`. A slot chosen from the page boots in the *pending*
state; the firmware confirms itself (`esp_ota_mark_app_valid_cancel_rollback`, `slots.c`
`fm1_slots_confirm`) when it receives the first MIDI event or the first cartridge request from the
FM-1, in the main loop. If the firmware hangs, reboots by watchdog or never talks to the FM-1, the
next reset marks the slot as aborted and the bootloader returns to the previous slot. After a full
flash (`otadata` erased) the bootloader boots `ota_0` without any pending state.

On the bench this means that a firmware booted from the page without the FM-1 connected (UART
console only) goes back to the previous slot on the next reset. Connecting the FM-1 once confirms
it.

## Flashing the slots

`firmware/flash-slots.sh` writes the bootloader, the table and one binary per slot in a single
`esptool.py` session, erasing `otadata` so the board starts from `ota_0` with clean states; with a
single slot named it writes only that slot, without touching the table or `otadata`:

```sh
cd firmware
./flash-slots.sh -p /dev/cu.PORT ota_0=build-clipper-4m2m ota_1=build-melodee-4m2m ota_2=build-felucca-4m2m
./flash-slots.sh -p /dev/cu.PORT ota_1=build-melodee-4m2m        # slot 2 only
```

Flashing is never part of the build and needs the FM-1 disconnected and the ESP32 in its USB
bootloader. Each profile can also be flashed alone with `./build.sh <profile> -p PORT flash`, which
writes the image to `ota_0` and the initial `otadata` (what the script does for slot 1). The full
procedure, with backups, is in [FLASHING.md](FLASHING.md).

## Limits

- On hardware, writing the three slots works and reads back identical, but booting each slot, switching between
  them and the rollback are verified in software only (native tests of the cartridge server with
  simulated slots and Sloop's real UI).
- Internal RAM in use: Melodee 229 KB (Prophet and FM6 in floating point, a 72 KB arena), Felucca
  177 KB, Clipper 142 KB. The render time of Melodee and Felucca on the ESP32 is not measured yet:
  each profile logs `render_max_us` against the 2,902 µs budget of a 128-frame block.
- A switch interrupts the audio and discards the edits in RAM. The chosen slot persists only in
  `otadata` (the last one chosen stays the default across resets).
- Sloop and the ESP32 must run the same cartridge protocol version; with different versions the page
  shows NO LINK.
