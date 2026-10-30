# Cartridge USB return

This tree is Sloop 2.8.4 (`https://github.com/isod89/sloop-fm1.git`, revision
`91f30182d9095e0ecdb43396554f2ce5a67df179`) with the cartridge profile: USB audio playback into
the FM-1, the CARTRIDGE page that controls the cartridge, and the FM-1 charger control. The code
and the native tests cover the software side; enumeration, physical audio, power, processor load
and latency on the FM-1 are measured on the bench.

## Topology and profile

```text
FM-1 / Sloop (USB device) -- USB-MIDI notes/clock ----> ESP32-S3 / synthesizer (host)
FM-1 / mixer / output     <-- PCM16 stereo 44.1 kHz -- ESP32-S3 / synthesizer
```

`sh build-cartridge.sh` enables `FELUCCA_UAC_RX=1` and turns off capture (`FELUCCA_UAC=0`) and
the console (`FELUCCA_CDC=0`). It keeps the internal instruments, MIDI, saving, recovery/OTA,
master and limiter. The regular build (`sh build.sh`) keeps RX off. See
[build and tests](CARTRIDGE_BUILD.md).

The return endpoint is **0x04 OUT**, adaptive isochronous, at most 180 bytes per 1 ms packet.
The interface is `2 + FELUCCA_UAC`; the host discovers it from the descriptors. As upstream does
for its audio input, the AudioControl header lists the audio streaming interfaces first and MIDI
last (macOS up to 15 drops the MIDI interface from the list while walking it and would miss the
streaming interface listed after it), and bcdDevice reads 4.08 (4.xx: playback; +0.08 that order,
so a host that cached older descriptors reads them again). The format is
PCM signed 16-bit little-endian, two channels, 44,100 Hz. There is no feedback endpoint and no
48 kHz playback rate; the rate controls accept only 44,100. MIDI stays on 0x01/0x81.

The experimental profile with capture uses 0x84 IN and 0x04 OUT at the same time. Its
descriptors and EP0 are exercised in simulation only, and duplex operation on the physical
controller is not established; the cartridge build with capture as well (`FELUCCA_UAC=1`) fits
the application slot with 772 B free. Use the dedicated profile.

## Controls on the FM-1

1. In the menu opened by HOME, use SELECT to reach the **USB** section (SCREEN, LIGHTS, AUDIO,
   SEQ, USB, SYS: with six sections the SYSTEM tab reads SYS so that they fit the screen).
2. **KNOB 1 / USB RETURN** sets the gain from OFF to 100%.
3. **OCT+** mutes, and pressed again brings back the level it had. The state is RAM-only:
   restarting restores 100%.
4. **KNOB 2 / CHARGE** selects **ON** (the default) or **AUTO** for the FM-1's internal charger
   while the cartridge is the host (section below). Also RAM-only.
5. The master, limiter and global effects stay in the audio path.

The return is a single stereo signal, summed once on core 0 after the sum of the cores and
before the global effects and limiter. It does not create a fifth track, does not change the
saved project format and does not feed the per-track chorus/delay/reverb sends. Mute keeps
consuming the packets, so no stale audio builds up while muted. Every start and re-prime fades in
over eight blocks (5.8 ms, `UAR_RAMP`), and the gain glides from one block's value to the next
sample by sample, so USB RETURN, a mute or the cartridge track's LEVEL never steps.

The track on the cartridge's MIDI channel is the cartridge track (`cartridge.c`
`cart_tracks_update`, `core.h` `CART_TRACK`): the notes its engine would play go out over MIDI on
its channel (`voice.c`, `seq.c` `cart_note`), from the keys, the arpeggiator, the steps, rolls and
takes, with OUT at KEYS; its engine renders nothing and is left out of the mix (`fx.c`); the
footer, TRACKS, the studio view and the PRESETS page show the cartridge's engine and preset; and
PRESETS steps through the cartridge's presets (`cart_browse`). The handover happens at the start
of an audio block (`seq.c` `cart_tracks_block`): voices stop and an All Notes Off goes out on the
track's channel when the cartridge takes the track (its keys' plain notes may be sounding there),
and the notes sent are ended when it gives it back or the track is muted. A note-off dropped by a
full MIDI OUT ring (`usb.c` `midi_out_event`) is followed by an All Notes Off on its channel once
there is room. The same block sets the return's level from the cartridge track's LEVEL (its default
is unity), MIDI EXPRESSION (CC 11, as on any track) and MUTE, and feeds the track's meter with the
return's peak; a sample's note its engine had waiting for a voice's fade is dropped at the takeover.
The notes a player holds on the cartridge track behave as on any track: the sequencer neither
restarts nor ends them, and a note-off lost on a full MIDI input ring ends the notes MIDI held,
which then end on the cartridge too. A MIDI Program Change (with its Bank Select) received for the
cartridge track changes the track's own, silent sound, not the cartridge's preset. The cartridge's polyphony
depends on the firmware it runs. Sloop's knobs do not send CC. While the cartridge answers, CLOCK
OUT runs even at OFF (`seq.c` `clkout_block`), unless SYNC follows an external clock.
`cart_quiet` holds the autosave and the section saves, whose flash erase runs with the interrupts
off, while a cartridge track holds a note and for a second after the return last went above
-54 dBFS (`uar_loud_ms`).

### CARTRIDGE page

The GLO → CARTRIDGE page is numbered like the other GLO pages ("CARTRIDGE 2/10")
and shows in its header the name of the firmware the cartridge runs ("CLIPPER OK",
"MELODEE OK", "FELUCCA OK"), taken from the protocol reply (`cartridge_wire.h`,
`F0 7D 43 46 03`: preset index and count in 14 bits, firmware name, current slot and slot
count, MIDI channel). Knobs 1–4 select engine, preset, parameter and value. The second entry of the
parameter list, **FIRMWARE** (knob 3), lists the slots in the ESP32 flash; knob 4 picks one
("2/3" on the right, CURRENT on the running one) and the cartridge reboots into it two
seconds after the knob stops. The footer explains this while FIRMWARE is selected. Without an
answer the header shows NO LINK, and with an answer but the stream at alt 0, "<NAME> NO AUDIO".
The knobs go through Sloop's knob acceleration (`ui_input.c` `accel`); a value given by name never
accelerates. On HOME, the cartridge track's footer reads CARTRIDGE. The protocol is described in
the cartridge project's `docs/CARTRIDGE.md`.

## FM-1 charger and the charging icon

The charging icon in the Sloop header is animation only: `ui_draw.c` fills the bars whenever a
configured, non-suspended USB host is present, and never reads the charger state. With the
cartridge answering it shows the battery's level instead: the cartridge powers nothing.

The FM-1 SoC (AC79/WL82) has an internal Li-ion charger controlled by the P33 register
`P3_CHG_CON0` (bit 0 `CHARGE_EN`, bit 1 `CHGBG_EN`; SDK
`include_lib/driver/cpu/wl82/asm/p33.h` and `charge.h`). If the cartridge powers the FM-1's
VBUS, this charger would drain the cartridge's cell into the FM-1 battery. The cartridge profile
(`FELUCCA_CARTRIDGE=1`) adds an optional control:

- `fm1_charge_en()` and `fm1_charge_set()` in `firmware/hal/fm1_sys.h` read and write only
  bit 0, using P33's own OR/AND operations (no read-modify-write); the bandgap and the
  voltage/current selections stay as the boot left them. Register access stays inside `hal/`.
- At startup, before interrupts are enabled, `cart_charge_boot()` (`cartridge.c`) reads the
  state the boot left the charger in: whatever the ROM, uboot, the hardware default or a
  power-off that kept the P33 domain left, known only on the device.
- Menu **HOME → USB → KNOB 2 / CHARGE**: **ON** (default) keeps the charger on, so the first
  main-loop pass of every start sets `CHARGE_EN` when the boot found it off, with or without a
  cartridge; **AUTO** holds `CHARGE_EN` at zero while `cart.online` (the ESP32 answers the
  cartridge protocol). RAM-only: restarting returns to ON. Below the value, **NOW OFF** means the
  firmware is holding the charger off. Sloop writes the bit in these cases only: ON at start,
  AUTO while linked, and the restores listed below.
- The charger returns to its boot state when the link drops (protocol timeout, suspend,
  deconfiguration or USB reset), when CHARGE changes to ON, and before entering UBOOT, the OTA
  update and the restart after FELUCCA CRASH.
- `P3_CHG_CON0` survives a software reset. The boot state and the "turned off by us" flag live
  in `.noinit` RAM (`cart_keep`): after a watchdog reset or a lockup with the link active, the
  next startup turns the charger back on (`main.c fm1_cstart`, before the boot-loop guard sends it
  to the rescue or to UBOOT) and keeps the true boot state. At power-on the RAM is random and the
  record is ignored (magic).
- So that AUTO acts without a visit to the CARTRIDGE page, every USB configuration sends up to 4
  discovery queries with the page hidden (each with up to 3 attempts; about 5 s in total), and
  every accepted reply gives a later outage 4 more. Without an answer (a computer, for example)
  the page stays quiet until it is opened. Once the cartridge has answered in this USB session,
  a query every 5 s with the page hidden checks the link and finds it again after an outage.

Risks and limits: this control has not been tested on the SoC. The bit map comes only from the
SDK header; the real effect of clearing `CHARGE_EN` (charge current, VBUS detector behaviour,
consumption) must be measured with an ammeter in series with the FM-1 battery or with VBUS.
What powers the FM-1's VBUS with the cartridge attached is still to be confirmed (gate G0 of
the cartridge project): with no VBUS from the cartridge there is nothing to turn off; with
VBUS, the firmware shutoff does not replace an electrical review, because there may be current
paths this bit does not control. If the FM-1 power switch cuts the P33 domain, the state returns
to the hardware default; if it is a soft power-off that also loses `.noinit`, an FM-1 switched off
with the cartridge connected at AUTO keeps the charger off while it stays off, and the next start
turns it back on (CHARGE starts at ON). The native tests (`tests/cartridge_ui_test.c`) exercise
the sequence with a simulated HAL: the ON default turning a charger found off back on, AUTO
online → off, link lost/reset → restored, ON/AUTO menu, restore before reset, and recovery after
an unclean reset.

## Reception and clocks

`uac_rx.c` implements a ring of 1,024 frames, 4,096 bytes in main RAM. TIMER5 produces packets
and the core 0 audio render consumes them. The USB service also runs during the nested render,
without resampling in the callback. The EP4 DMA reserves 1,028 bytes, sized to reject packets
larger than the advertised format without reading past the buffer.

The ASRC uses 4-point Hermite interpolation (Catmull-Rom), a Q24 phase and occupancy-based PI
correction, limited to ±5,000 ppm. Linear interpolation would let the fraction, which sweeps
slowly from 0 to 1 at the beat rate of the two clocks, modulate the treble by up to 2.4 dB at
10 kHz (a slow tremolo on bright sounds); with Hermite it is 0.5 dB at 9 kHz, 0.7 dB at 10 kHz and
3.5 dB at 15 kHz (computed for the interpolator, not measured). The products are 64-bit, with no
overflow on full-scale steps.

The occupancy target is a mean of 384 frames over a burst; since the DAC consumes 256 frames
per burst, the minimum after a burst stays near 5 ms, so a late packet from the host does not
become an underrun. The rate loop reads the occupancy once per burst, as the burst begins
(`audio.c` calls `uac_rx_burst()` at the audio interrupt's entry, and the burst's first block
reads it), where it aims at 496 frames, the start of a sawtooth whose mean is 384: there the
reading is what came in over a whole DAC period, independent of the render's own length, since
TIMER5 pushes packets inside the render. A start waits for 512 frames counted the same way, on a
burst's first block, and then skips to the aim. The rate learned (a slow mean of the correction)
survives a re-prime and an underrun, so a restart does not glide back to it. The closed-loop
model in `tests/uac_rx_test.c` (bursts rendered in 10 % to 80 % of their period, the host's packets
at any phase against them) starts every stream on a burst's first block, at the aim, with a pitch
excursion of at most 0.95 cent; with the render time varying from 0.8 to 3.8 ms burst to burst,
the start's excursion is 0.26 cent and the steady wobble 0.15 cent peak to peak. These values come
from a model: they are neither a latency measurement nor a certification of musical quality, and
the resampler's cost and quality on the device have yet to be measured.

Alt 0, reset, deconfiguration, suspend, an invalid packet, an overflow and the absence of packets
for 20 ms discard the pending audio or restart acquisition. A lost ISO packet (OverRun in the
SIE) or a corrupted one (DataError / IncompRx) is only counted: a still-valid packet in the
buffer is read, a corrupted one is skipped, and the clock loop absorbs the missing millisecond.
A real underrun (empty ring) discards the rest of the block and waits for a new prime. A
generation counter keeps a block interrupted by a reset from returning to the mixer. Memory
barriers and short IRQ windows protect publication; interpolation happens outside those windows.

Diagnostics live in the symbols `uar` and `uac_rx_usb`: packets, format errors, lost/corrupted
packets, overruns, underruns, timeouts, reprimes, cancelled blocks, minimum/maximum occupancy,
Q24 correction and saturation. There is no logging in the ISR. The dedicated profile has no CDC
console; reading the counters on the target needs debug instrumentation, or the diagnostic build
(`FELUCCA_CARTRIDGE_DIAG=1`), which shows them in the CARTRIDGE footer. The native tests print the
simulated counters.

## Physical validation order

1. Resolve the electrical gate G0 of the cartridge project: the cartridge's own power supply,
   VBUS behaviour and enumeration. This firmware does not solve the power circuit nor validate
   cables with interrupted VBUS.
2. Test **computer → Sloop**: 44.1 kHz stereo output, a quiet tone, L/R, gain/mute, master,
   MIDI transport and reconnection.
3. Measure underruns/overruns, ISR duration and CPU margin with the internal engines and effects
   active. Test packet loss and interruption of the source.
4. Test the ESP32 first with a UAC tone, then each synthesizer. Measure continuity, total
   latency, battery and noise; only then move on to the board and the enclosure.
5. If the cartridge powers VBUS, measure the FM-1 charge current with CHARGE at AUTO (link
   active, NOW OFF) and at ON, and check the charger's boot state in the menu before relying on
   the firmware shutoff.
