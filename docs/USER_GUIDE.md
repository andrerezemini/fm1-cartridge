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
3. **KNOB 1, USB RETURN** sets the level, from OFF to 100%. **OCT+** mutes it and, pressed again,
   brings back the level it had. It starts at 100% every time the FM-1 starts.
4. **KNOB 2, CHARGE** starts at **ON**: the FM-1's internal charger stays on (a start that finds it
   off turns it on). **AUTO** holds the charger off while the cartridge answers, so the cartridge's
   cell can never charge the FM-1's battery; **NOW OFF** under CHARGE shows it doing so. This control is not yet measured on the hardware
   ([details](../sloop-fm1/docs/CARTRIDGE_USB_RETURN.md#fm-1-charger-and-the-charging-icon)).
5. Press **OCT−** to close the menu.

Start with the MASTER volume low: a fault or an untested preset can play loud. The battery icon
shows the FM-1's own level while the cartridge is plugged in, since the cartridge powers nothing.

## 4. Check the link

1. Tap **GLO** and turn **SELECT** to **CARTRIDGE**, the page right after GLOBAL. Its header reads
   "CARTRIDGE 2/10".
2. Wait for the name of the running firmware at the top right: **CLIPPER OK**, **MELODEE OK** or
   **FELUCCA OK**. **NO LINK** means no reply has arrived yet, and **NO AUDIO** after the name means
   the cartridge answers but its audio stream has stopped; see [Troubleshooting](#troubleshooting).

## 5. Play the cartridge

The track on the cartridge's MIDI channel is the **cartridge track**. With the defaults the
cartridge listens on channel 1 and track 1 sends on channel 1, so track 1 is the cartridge track.

1. Turn **ALGORITHM** to track **1**.
2. Play the keys. The notes go to the cartridge over USB and its sound comes back through the FM-1.
   Track 1's own engine stays silent.
3. The footer and the TRACKS screen show the cartridge's engine and preset instead of Sloop's, and
   on HOME the footer reads **CARTRIDGE** where it reads HOME on the other tracks.

Everything that plays track 1 plays the cartridge: the keys, chords, the arpeggiator, the steps,
rolls and recorded takes. Leave **GLO → MIDI 2 → OUT** at **KEYS**; the cartridge track sends its
sequencer notes without OUT = SEQ. Tracks 2 and 3 and the drums keep their own sounds.

While the cartridge answers, the FM-1 sends it START, STOP and the MIDI clock, so its arpeggiators,
synced LFOs and delays follow Sloop's tempo, whatever **GLO → MIDI → CLOCK** says. When the FM-1
itself follows an external clock (SYNC), it sends none.

The track's **LEVEL** and **MUTE** (TRACKS) set the level of the USB return, with the default
LEVEL at unity, and its meter shows the return. Muting or soloing also ends the notes the
cartridge is holding. PAN, the effect sends and the track's sound parameters on HOME do not change
the cartridge's sound; the CARTRIDGE page does.

## 6. Change sounds with PRESETS

With the cartridge track selected, turn **PRESETS** on the HOME (TRACKS) screen. It steps through
the cartridge's presets. Past the last preset of an engine it moves on to the next engine's first
preset, and turning back past the first one goes to the previous engine's last preset. Turns made
while the cartridge loads the other engine are dropped.

On the PRESETS page (one of the SAVE pages), **KNOB 1** steps presets and **KNOB 2** steps engines.

A new preset fades out what was sounding in a few milliseconds and leaves the delay and reverb
tails ringing; the next notes play the new sound.

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
| **KNOB 3** | Parameter to edit: entry 1 is **MIDI CH**, entry 2 is **FIRMWARE**, then the synthesizer's own parameters with their names, and at the end two figures of the cartridge's audio |
| **KNOB 4** | Value of that parameter, with its unit or name |

1. Choose an engine with KNOB 1 and a preset with KNOB 2.
2. Turn KNOB 3 to a parameter, for example a filter cutoff among the ENG parameters.
3. Hold a note and turn KNOB 4.

The knobs accelerate as Sloop's own do: a slow turn moves one step, a fast one jumps further
through long lists and wide ranges (**MENU → SCREEN → KNOB ACCEL OFF** makes every turn one step). A value
given by name always moves one step.

The screen only shows what the cartridge has confirmed. After changing the engine, the preset or
the parameter, wait for the new name before turning KNOB 4. A new preset keeps KNOB 3 on the same
parameter when the new sound has it, and otherwise moves it to the synthesizer's first parameter,
never to MIDI CH or FIRMWARE. On the other pages the four knobs control Sloop again.

The last two entries only show figures: **AUDIO RENDER MAX**, the longest the cartridge took to
compute 64 frames since the page was opened, against the 1,451 µs those frames last, and **AUDIO
UNDERRUNS**, the times its audio fell behind since then.

The cartridge keeps its sound for each firmware: the engine, the preset, MIDI CH and the edits are
saved a few seconds after the last change, once nothing plays, and come back at the next start or
when that firmware is chosen again. Sloop's projects do not save the cartridge's sound.

## 8. Switch between Clipper, Melodee and Felucca

The three synthesizers sit in three firmware slots of the cartridge. No computer is needed to
switch.

1. Stop the transport and release the keys: while the FM-1 plays, FIRMWARE stays on the running
   slot and reads **PLAYING**.
2. On **GLO → CARTRIDGE**, turn **KNOB 3** to **FIRMWARE**. The footer reads
   "KNOB 4: SLOT, REBOOTS IN 2 S".
3. Turn **KNOB 4** to the slot you want: CLIPPER, MELODEE or FELUCCA, with its position ("2/3")
   on the right. **CURRENT** marks the one running. An empty slot, and one whose image is
   **DAMAGED**, is passed over.
4. Leave the knob there. Two seconds later the cartridge restarts into that firmware. Turning
   back to the current slot, moving KNOB 3 off FIRMWARE or leaving the page within those two
   seconds cancels.
5. The page shows **NO LINK** for a few seconds, then the new name, for example **MELODEE OK**.

The chosen slot stays the default once it has answered the FM-1. A slot that starts and never
answers is undone by the cartridge's bootloader at its next restart, which returns to the previous
slot; the FIRMWARE list then shows it as, for example, **MELODEE FAIL**, and choosing it tries it
once more. Switching on the hardware is still being tested ([FIRMWARE_SLOTS.md](FIRMWARE_SLOTS.md)).

## 9. Sequence and record

The cartridge track records and plays like any Sloop track.

1. Select the cartridge track with **ALGORITHM**.
2. Press **REC** and play, or write steps on the **SEQ** page.
3. Press **PLAY**: the steps play the cartridge. **STOP** ends the notes the steps were playing;
   held keys, KBD HOLD and a running arpeggiator go on, as on any track.

A take keeps the notes, not the cartridge's sound. When the project plays later, the cartridge
plays them with whatever sound it has at that moment.

## 10. Choose which track plays the cartridge

The first parameter of the CARTRIDGE page, **MIDI CH**, sets the channel the cartridge listens to.

- **1 to 16:** every instrument track (1 to 3) whose channel under **GLO → MIDI** matches becomes a
  cartridge track. For example, MIDI CH 2 makes track 2 the cartridge track with the default
  channels. The drum track never is.
- **OMNI:** no track is the cartridge track. The cartridge plays whatever MIDI reaches it on any
  channel, such as the keys of the selected track, on top of that track's own sound.

A new MIDI CH ends the notes the cartridge is playing. When a track stops being the cartridge
track, the notes it had sent are ended and its own engine plays again; when a track becomes the
cartridge track, an All Notes Off on its channel ends what its keys were holding. The channel is
kept with the other edits.

## 11. Switch off and charge

1. Press **PLAY** to stop if a song is running.
2. Unplug the cartridge from the FM-1, then turn its switch off.
3. Charge the cartridge's cell outside the FM-1. The FM-1 charges as usual from its own charger.

With CHARGE at AUTO, an FM-1 switched off while the cartridge was linked can keep its charger off
while it stays off, so it may not charge until it is switched on once: every start turns the
charger back on (CHARGE starts at ON).

## Troubleshooting

| What you see | What to check |
| --- | --- |
| **NO LINK** on the CARTRIDGE page | The cartridge is switched on and its cell is charged; the adapter is fully seated (try it the other way round); the ESP32 is not left in download mode (press RESET or switch it off and on); both devices come from the same release. Opening the CARTRIDGE page asks again; once the cartridge has answered, Sloop also asks every 5 s with the page closed. |
| **NO AUDIO** after the firmware's name | The cartridge answers but its audio stream stopped after a USB error it could not recover from: unplug the cartridge and plug it back. |
| The link is up but there is no sound | USB RETURN is not OFF; MASTER is up; the cartridge track's LEVEL is not 0 and it is not muted; the selected track is the cartridge track (its MIDI channel under GLO → MIDI matches MIDI CH); the preset is not silent at low velocity. |
| Track 1 plays Sloop's own sound | MIDI CH is OMNI or another channel, or the link is down. |
| A note keeps sounding | Turn PRESET to the next preset and back: the cartridge ends every voice (the page's edits go back to the preset's values). Muting and unmuting the track ends the notes Sloop knows it sent. Unplugging ends every voice. |
| FIRMWARE reads **PLAYING** | The FM-1 holds notes or its transport runs: stop it and release the keys. |
| A slot reads **<NAME> FAIL** | That firmware started and never answered the FM-1 last time; its log on the cartridge's UART says why ([firmware](../firmware/README.md)). |
| Gaps or a stuttering note | The engine or preset may be too heavy for the ESP32: **AUDIO RENDER MAX** on the CARTRIDGE page above 1,451 µs, or AUDIO UNDERRUNS counting, says so. Try a lighter one or fewer notes at once. |
| The FM-1 does not charge | CHARGE at AUTO holds the charger off while the cartridge answers. Set CHARGE to ON, or unplug the cartridge. |
| The last edits are gone | The cartridge saves its sound a few seconds after the last change, once nothing plays; a restart or a firmware switch before that keeps the sound saved earlier. |

## Quick reference

| Control | With the cartridge |
| --- | --- |
| Hold **HOME** → **USB** | USB RETURN level (KNOB 1) and CHARGE (KNOB 2); OCT− closes |
| **GLO** → **CARTRIDGE** | Engine, preset, parameter and value on KNOB 1 to 4; firmware name, NO AUDIO or NO LINK |
| **ALGORITHM** | Selects the track; the cartridge track plays the cartridge |
| **PRESETS** | On the cartridge track: the cartridge's presets, across engines |
| **TRACKS** LEVEL, MUTE | The cartridge track's LEVEL and MUTE set the USB return |
| **KNOB 3 = FIRMWARE**, **KNOB 4** | Switch firmware slot, 2 s after the knob stops, with the transport stopped |
| **KNOB 3 = MIDI CH**, **KNOB 4** | Which channel, and so which track, plays the cartridge |
| **REC**, **SEQ**, **PLAY** | Record and play the cartridge track as any track |
