# User guide: playing the cartridge from Sloop

This guide walks through using the cartridge on an FM-1 running the cartridge Sloop: connecting
it, setting the level, playing it from a track, changing sounds, editing them and switching
between Clipper, Melodee and Felucca. Read [Safety and risks](../README.md#safety-and-risks)
first: the cartridge is an experimental prototype with a lithium cell.

## 1. What you need

- An FM-1 with the cartridge build of Sloop installed, and an ESP32-S3 cartridge with the three
  firmware slots flashed. Both come from the same release of this repository, because both sides
  must run the same cartridge protocol version. [FLASHING.md](FLASHING.md) covers both installs,
  with backups.
- The cartridge's cell charged, outside the FM-1 ([hardware](../hardware/README.md#battery-and-switch)).
- The FM-1 charged and running on its own battery. The FM-1 does not power the cartridge, and the
  cartridge does not power the FM-1.

## 2. Connect and switch on

1. Switch the FM-1 on.
2. Turn the cartridge's switch on.
3. Plug the cartridge into the FM-1's USB-C port. Do not connect a computer or a charger to either
   device while they are plugged together.
4. Wait a few seconds. The cartridge starts its firmware, takes the USB port as host and the FM-1
   finds it on its own.

Right after the ESP32 has been flashed it can stay in its download mode. If nothing happens,
press the board's RESET button or switch the cartridge off and on again.

## 3. Set the return level

The cartridge's sound comes back into the FM-1 as a single stereo signal, the USB return, mixed
before the FM-1's global effects, limiter and MASTER volume.

1. Hold **HOME** to open the menu.
2. Turn **SELECT** to the **USB** section.
3. **KNOB 1, USB RETURN** sets the level, from OFF to 100%. **OCT+** switches between mute and
   100%. It starts at 100% every time the FM-1 starts.
4. **KNOB 2, CHARGE** stays at **AUTO**: while the cartridge answers, the FM-1's internal charger
   is held off so the cartridge's cell can never charge the FM-1's battery. **ON** leaves the
   charger as the FM-1 started. This control is not yet measured on the hardware
   ([details](../sloop-fm1/docs/CARTRIDGE_USB_RETURN.md#fm-1-charger-and-the-charging-icon)).
5. Press **OCT−** to close the menu.

Start with the MASTER volume low: a fault or an untested preset can play loud.

## 4. Check the link

1. Tap **GLO** and turn **SELECT** to **CARTRIDGE**, the page right after GLOBAL. Its header reads
   "CARTRIDGE 2/10".
2. Wait for the name of the running firmware at the top right: **CLIPPER OK**, **MELODEE OK** or
   **FELUCCA OK**. **NO LINK** means no reply has arrived yet; see [Troubleshooting](#troubleshooting).

## 5. Play the cartridge

The track on the cartridge's MIDI channel is the **cartridge track**. With the defaults the
cartridge listens on channel 1 and track 1 sends on channel 1, so track 1 is the cartridge track.

1. Turn **ALGORITHM** to track **1**.
2. Play the keys. The notes go to the cartridge over USB and its sound comes back through the FM-1.
   Track 1's own engine stays silent.
3. The footer and the TRACKS screen show the cartridge's engine and preset instead of Sloop's.

Everything that plays track 1 plays the cartridge: the keys, chords, the arpeggiator, the steps,
rolls and recorded takes. Leave **GLO → MIDI 2 → OUT** at **KEYS**; the cartridge track sends its
sequencer notes without OUT = SEQ. Tracks 2 and 3 and the drums keep their own sounds.

Muting or soloing works as on any track and also ends the notes the cartridge is holding.
The track's LEVEL, PAN, effect sends and sound parameters do not change the cartridge's sound;
the USB RETURN level and MASTER do.

## 6. Change sounds with PRESETS

With the cartridge track selected, turn **PRESETS** on the HOME (TRACKS) screen. It steps through
the cartridge's presets. Past the last preset of an engine it moves on to the next engine's first
preset, and turning back past the first one goes to the previous engine's last preset.

On the PRESETS page (one of the SAVE pages), **KNOB 1** steps presets and **KNOB 2** steps engines.

| Firmware | Engines | Factory presets |
| --- | ---: | ---: |
| Clipper | 23 | 407 |
| Felucca | 13 | 72 |
| Melodee | 5 (Prophet, FM6, CZ-1, SID, DRUM) | 327 |

## 7. Edit the sound on the CARTRIDGE page

On **GLO → CARTRIDGE**, the four knobs control the cartridge:

| Knob | What it does |
| --- | --- |
| **KNOB 1** | Engine |
| **KNOB 2** | Factory preset of that engine |
| **KNOB 3** | Parameter to edit: entry 1 is **MIDI CH**, entry 2 is **FIRMWARE**, then the synthesizer's own parameters with their names |
| **KNOB 4** | Value of that parameter, with its unit or name |

1. Choose an engine with KNOB 1 and a preset with KNOB 2.
2. Turn KNOB 3 to a parameter, for example a filter cutoff among the ENG parameters.
3. Hold a note and turn KNOB 4.

The screen only shows what the cartridge has confirmed. After changing the engine, the preset or
the parameter, wait for the new name before turning KNOB 4. On the other pages the four knobs
control Sloop again.

Edits live in the cartridge's memory only. A restart or a firmware switch returns to the
firmware's starting engine and preset, and Sloop's projects do not save the cartridge's sound.

## 8. Switch between Clipper, Melodee and Felucca

The three synthesizers sit in three firmware slots of the cartridge. No computer is needed to
switch.

1. On **GLO → CARTRIDGE**, turn **KNOB 3** to **FIRMWARE**. The footer reads
   "KNOB 4: SLOT, REBOOTS IN 2 S".
2. Turn **KNOB 4** to the slot you want: CLIPPER, MELODEE or FELUCCA, with its position ("2/3")
   on the right. **CURRENT** marks the one running.
3. Leave the knob there. Two seconds later the cartridge restarts into that firmware. Turning
   back to the current slot within those two seconds cancels.
4. The page shows **NO LINK** for a few seconds, then the new name, for example **MELODEE OK**.

The chosen slot stays the default across restarts. A slot that never talks to the FM-1 is undone
by the cartridge's bootloader at its next restart, which returns to the previous slot.
Switching on the hardware is still being tested ([FIRMWARE_SLOTS.md](FIRMWARE_SLOTS.md)).

## 9. Sequence and record

The cartridge track records and plays like any Sloop track.

1. Select the cartridge track with **ALGORITHM**.
2. Press **REC** and play, or write steps on the **SEQ** page.
3. Press **PLAY**: the steps play the cartridge. **STOP** ends every note it was holding.

A take keeps the notes, not the cartridge's sound. When the project plays later, the cartridge
plays them with whatever sound it has at that moment.

## 10. Choose which track plays the cartridge

The first parameter of the CARTRIDGE page, **MIDI CH**, sets the channel the cartridge listens to.

- **1 to 16:** every instrument track (1 to 3) whose channel under **GLO → MIDI** matches becomes a
  cartridge track. For example, MIDI CH 2 makes track 2 the cartridge track with the default
  channels. The drum track never is.
- **OMNI:** no track is the cartridge track. The cartridge plays whatever MIDI reaches it on any
  channel, such as the keys of the selected track, on top of that track's own sound.

When a track stops being the cartridge track, the notes it had sent are ended and its own engine
plays again. The channel choice lives in the cartridge's memory, like the other edits.

## 11. Switch off and charge

1. Press **PLAY** to stop if a song is running.
2. Unplug the cartridge from the FM-1, then turn its switch off.
3. Charge the cartridge's cell outside the FM-1. The FM-1 charges as usual from its own charger.

If CHARGE was at AUTO and the FM-1 was switched off with the cartridge still linked, its charger
can stay off until Sloop starts normally again; switch the FM-1 on once before charging it.

## Troubleshooting

| What you see | What to check |
| --- | --- |
| **NO LINK** on the CARTRIDGE page | The cartridge is switched on and its cell is charged; the adapter is fully seated; the ESP32 is not left in download mode (press RESET or switch it off and on); both devices come from the same release. Opening the CARTRIDGE page asks again. |
| The link is up but there is no sound | USB RETURN is not OFF; MASTER is up; the selected track is the cartridge track (its MIDI channel under GLO → MIDI matches MIDI CH); the preset is not silent at low velocity. |
| Track 1 plays Sloop's own sound | MIDI CH is OMNI or another channel, or the link is down. |
| A note keeps sounding | Press PLAY to stop, or change MIDI CH and back. Unplugging ends the cartridge's voices. |
| Gaps or a stuttering note | The engine or preset may be too heavy for the ESP32. Try a lighter one or fewer notes at once; the render cost of each engine is not measured yet. |
| The FM-1 does not charge | CHARGE at AUTO holds the charger off while the cartridge answers. Unplug the cartridge, or set CHARGE to ON. |
| Edits are gone | Edits stay in the cartridge's memory only and reset at every restart or firmware switch. |

## Quick reference

| Control | With the cartridge |
| --- | --- |
| Hold **HOME** → **USB** | USB RETURN level (KNOB 1) and CHARGE (KNOB 2); OCT− closes |
| **GLO** → **CARTRIDGE** | Engine, preset, parameter and value on KNOB 1 to 4; firmware name or NO LINK |
| **ALGORITHM** | Selects the track; the cartridge track plays the cartridge |
| **PRESETS** | On the cartridge track: the cartridge's presets, across engines |
| **KNOB 3 = FIRMWARE**, **KNOB 4** | Switch firmware slot, 2 s after the knob stops |
| **KNOB 3 = MIDI CH**, **KNOB 4** | Which channel, and so which track, plays the cartridge |
| **REC**, **SEQ**, **PLAY** | Record and play the cartridge track as any track |
