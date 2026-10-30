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
| `nvs` | 0x9000 | 16 KB | the sound each firmware keeps (engine, preset, MIDI CH, edits), one record per firmware name |
| `otadata` | 0xd000 | 8 KB | the slot the bootloader boots and the state of each slot |
| `phy_init` | 0xf000 | 4 KB | radio calibration (unused) |
| `ota_0` | 0x10000 | 1.25 MB | slot 1 (the bootloader starts with it after a full flash) |
| `ota_1` | 0x150000 | 1.25 MB | slot 2 |
| `ota_2` | 0x290000 | 1.25 MB | slot 3 |

There is no `factory` partition: the three slots are equal. The current binaries occupy between
44% and 59% of a slot (see [firmware](../firmware/README.md)). Changing the table needs a full
flash, since `otadata` and the slot offsets depend on it.

## What each firmware needs

Each slot holds a complete ESP-IDF firmware from this repository (USB host, UAC TX, MIDI and the
cartridge protocol) built with one of the adapters in `synth/`. Only the DSP differs:
`clipper_synth.c`, `melodee_synth.c` or `felucca_synth.c`, all behind the `synth/fm1_synth.h`
interface. A firmware that does not answer the cartridge protocol (a third-party one, the TONE and
MIDI bench profiles, a build of another protocol version) is never confirmed if booted from a slot
(below): the next reset goes back to the previous slot.

The name the FM-1 shows ("MELODEE OK", the FIRMWARE list) is the profile's
`CONFIG_APP_PROJECT_VER`, read from the header of each image (`esp_app_desc_t.version`): CLIPPER,
MELODEE, FELUCCA, TONE, MIDI. A slot without an image appears as **EMPTY**, and one whose image does
not verify (checked at every start, `esp_image_verify`) as **DAMAGED**; neither can be chosen. The
slot the bootloader last rolled back from carries **FAIL** after its name, for example MELODEE FAIL:
it booted and never answered the FM-1; choosing it tries once more.

## Selection from the FM-1

On the CARTRIDGE page, **FIRMWARE** is the second entry of the parameter list (knob 3; the first is
MIDI CH). The value (knob 4) steps through the slots by name, with the position "2/3" on the right
and the unit **CURRENT** on the running slot. When the knob rests on another slot, the
ESP32 waits `CONFIG_FM1_SLOT_SWITCH_MS` (2 s), sets the chosen slot as the boot partition
(`esp_ota_set_boot_partition`, which verifies the whole image first) and reboots 300 ms later, so
the reply reaches the FM-1 first. Returning to the current slot, moving knob 3 off FIRMWARE or a
new query within the 2 s cancels (Sloop sends one when the page opens and when it is left), and a
query never leaves the cursor on FIRMWARE. While FIRMWARE is selected the footer reads "KNOB 4: SLOT, REBOOTS IN 2 S".

The audio task follows the FM-1's MIDI: notes held on the synthesizer's channel (any channel at
OMNI) and the transport between START and STOP. While either is on, knob 4 cannot leave the running
slot, which reads **PLAYING**, and a switch armed before is dropped when its 2 s run out. A note
held for a minute without any other note event counts as a lost note-off and no longer blocks.

After the reboot the FM-1 shows NO LINK until the new firmware enumerates (the
discovery probes sent on every USB configuration find the cartridge again) and then "<NAME> OK".
The engines, presets and parameters become the new firmware's, with the sound it kept from its own
last session (NVS). The previous firmware's last edits are kept only if it had saved them, a few
quiet seconds after the change.

There is no confirmation button: the cartridge has no screen or buttons, and the FM-1's buttons have
global functions on the page. The choice takes two actions (selecting FIRMWARE with knob 3 and
turning knob 4), only with nothing playing, and can be undone within 2 s. A preset or engine change
never leaves knob 3 on FIRMWARE. The Super Mini's BOOT button cannot serve as "hold to go back":
GPIO0 low at reset enters the ROM download mode before the bootloader runs.

## Bootloader rollback

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`. A slot chosen from the page boots in the *pending*
state; the firmware confirms itself (`esp_ota_mark_app_valid_cancel_rollback`, `slots.c`
`fm1_slots_confirm`) once its cartridge server has answered a request from the FM-1 and the reply
has been queued (`audio_tx.c fm1_audio_answered`, checked by the main loop). A MIDI note or a
request alone confirms nothing: a slot that cannot answer (the TONE and MIDI profiles, another
protocol version) and one that hangs or resets before answering are marked aborted at the next
reset, and the bootloader returns to the previous slot. After a full flash (`otadata` erased) the
bootloader boots `ota_0` without any pending state.

On the bench this means that a firmware booted from the page without the FM-1 connected (UART
console only) goes back to the previous slot on the next reset. Connecting the FM-1 once, with the
same protocol version, confirms it.

## Flashing the slots

`firmware/flash-slots.sh` writes the bootloader, the table and one binary per slot, erasing
`otadata` first so the board starts from `ota_0` with clean states; with `ota_0` not among the
slots named it writes only those slots, without touching the table or `otadata`. It refuses builds
whose partition tables or flash settings differ (a `-4m2m` profile with an 8 MB one) or that lack
the rollback, and takes the offsets from the build's own table:

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
- Internal RAM in use: Melodee 228 KB (Prophet and FM6 in floating point, a 72 KB arena), Felucca
  183 KB, Clipper 150 KB. The render time of Melodee and Felucca on the ESP32 is not measured yet:
  each profile logs `render_max_us` against the 1,451 µs budget of a 64-frame render, and the
  CARTRIDGE page shows AUDIO RENDER MAX.
- A switch interrupts the audio. The chosen slot persists in `otadata` (the last one confirmed
  stays the default across resets), each firmware's sound in `nvs`.
- A newer peer is understood as far as version 3's fields go; an older protocol version shows NO
  LINK.
