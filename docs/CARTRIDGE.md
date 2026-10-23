# The CARTRIDGE page: editing the cartridge from the FM-1

The CARTRIDGE page of the modified Sloop edits the synthesizer running on the ESP32
and chooses which firmware slot the ESP32 boots. The FM-1 and the ESP32 must run the same protocol
version; once both are installed, choosing sounds, editing parameters and switching synthesizer
need neither recompiling nor flashing.

## Operation

With the cartridge powered by its battery and connected to the FM-1:

1. Set **USB RETURN = 100%**, in the USB section of the menu opened by holding HOME.
2. Select track 1: on MIDI channel 1 it is the cartridge track (below), which plays the cartridge
   and keeps its own engine silent.
3. **Press and release GLO**, and turn **SELECT** to **CARTRIDGE** (right after GLOBAL).
4. Wait for **CLIPPER OK** (or MELODEE OK, FELUCCA OK: the name of the firmware the cartridge
   runs) at the top of the screen, next to "CARTRIDGE 2/10", the page's position among the GLO
   pages. **NO LINK** means the protocol has not received a reply yet.

| Control | Function |
| --- | --- |
| Knob 1 | Engine: 23 in Clipper, 13 in Felucca, 5 in Melodee |
| Knob 2 | Factory preset of the current engine: 407 in Clipper, 72 in Felucca, 327 in Melodee (201 in its Prophet alone) |
| Knob 3 | Parameter to edit, with its name and position in the list; entry 1 is **MIDI CH**, entry 2 is **FIRMWARE** |
| Knob 4 | Parameter value, within its range, with its unit or name; under FIRMWARE, the slot to boot |
| Keyboard | Keeps playing the cartridge over MIDI |
| SELECT | Moves to the other GLO pages |

Example: choose an engine with knob 1 and a preset with knob 2, look for a filter cutoff among the
`ENG` parameters with knob 3, and sweep knob 4 while holding a note. ENV, LFO, FX and the other
groups follow the engine controls. The screen only shows state confirmed by the ESP32; after
changing engine, preset or parameter, wait for the new name before editing its value.

**Switching firmware:** knob 3 to FIRMWARE, knob 4 to the desired slot (CLIPPER, MELODEE,
FELUCCA; "2/3" on the right, CURRENT marks the running one). Two seconds after the knob
stops on a different slot the cartridge reboots into it; the page shows NO LINK and then the new
name. Returning to the current slot within those two seconds cancels. Details and limits in
[FIRMWARE_SLOTS.md](FIRMWARE_SLOTS.md).

The four knobs control the cartridge **only on this page**; on the other pages they keep
controlling Sloop. ALGORITHM keeps its local function; PRESETS browses the cartridge's presets on
the cartridge track (below).

## The cartridge track

While the cartridge answers, every instrument track (1 to 3) whose MIDI channel under
**GLO → MIDI** equals the cartridge's **MIDI CH** is a cartridge track. With the defaults that is
track 1 on channel 1. With MIDI CH at OMNI, or with no link, no track is a cartridge track. The
drum track never is.

- **Notes.** Everything that would start or end a note on the track's own engine goes out as a
  MIDI note on the track's channel instead: the keys, chords, the arpeggiator, the steps, rolls,
  and takes recorded and played back. This works with **GLO → SYSTEM → OUT** at KEYS; at SEQ a
  note is not sent twice. Every note ends on the channel it started on, also when the channel
  changes while it sounds.
- **Silence.** The track's internal engine renders nothing and is left out of the mix. When a
  track becomes a cartridge track its voices stop at the next audio block; when it stops being
  one (OMNI, another channel, link lost), the notes it had sent are ended and its own engine plays
  again. MUTE and SOLO stop the notes it sends and end the ones sounding.
- **Names.** The footer, the TRACKS list, the studio view and the PRESETS page show the
  cartridge's engine and preset instead of the track's own.
- **PRESETS.** On HOME, TRACKS and the PRESETS page, the PRESETS knob steps through the
  cartridge's presets; past the last preset of an engine it goes on into the next engine's first,
  and before the first it goes back to the previous engine's last. On the PRESETS page, knob 1
  steps presets and knob 2 engines; FAV does not apply.
- **Not carried over.** The track's LEVEL, PAN, sends and its own sound parameters do not shape
  the cartridge: its audio comes back as the single USB RETURN signal. Sloop's knobs send no CC;
  the cartridge's sound is edited on the CARTRIDGE page.

## Scope

- Each synthesizer exposes its engine controls and the common groups it has: envelopes, LFOs,
  modulation matrix, voice/glide/pan, arpeggiator and chords, scale, inserts and effects (Clipper
  adds macros, ENV2, LFO2/3, EQ and compressor; Melodee its three FX slots and sends; Felucca its
  slicer and drum lane levels). The names come from the DSP itself; Clipper exposes up to 128
  parameters per engine.
- One instrument part per synthesizer, answering one MIDI channel (below). These are not four
  independent instruments matching Sloop's tracks.
- Changing engine selects its first preset. Changing preset ends the sounding voices and reloads
  the sound's parameters: play new notes after the change.
- **Edits stay in the ESP32's RAM.** A reboot, or a firmware switch, returns to the build's
  engine and preset. Sloop's projects and presets do not store this state. Global effect controls
  persist across preset changes within a session.
- No deep editors (FM operators, layers, sequence drawing), sample or bank import, or persistence
  of custom presets. The factory patches and main controls of those engines are available.
- On hardware the page shows its handshake with the ESP32; the four knobs end to end, the
  timbres and the ESP32's performance under load are still to be verified.

## Protocol (version 3)

Bidirectional USB-MIDI 1.0, cable 0, MIDI IN `0x81` and OUT `0x01` of the same FM-1 interface.
The PCM return stays on UAC `0x04`. The ESP32 configures its USB host FIFO as RX/NPTX/PTX =
104/48/48 words.

The binary contract is `core/include/fm1/cartridge_wire.h`, copied unchanged to Sloop's
`firmware/src/cartridge_wire.h`. Every message starts with `F0 7D 43 46 03` and ends with `F7`;
the payload contains only 7-bit bytes. This experimental namespace does not collide with Sloop's
OTA/editor. A frame with another version byte is ignored.

Request: 13 bytes. A 14-bit sequence, the operation, the expected 14-bit revision and a signed
14-bit delta with offset 8192. Operations: QUERY=0, ENGINE=1, PRESET=2, PARAM=3, VALUE=4.
Selections and values saturate at the limits, without wrap. Reply: 123 bytes, opcode 64; the full
state: revision, status, engine index and count, preset index and count (14-bit), parameter index
and count, value/min/max, the engine/preset/parameter/value names and unit, the name of the
running firmware (12 bytes), the current slot with the number of slots, and the MIDI channel the
synthesizer answers (0 for OMNI, 1 to 16), from which the FM-1 picks the cartridge track. The positions and
capacities of the fields are in the shared header.

The catalogue the FM-1 scrolls through is built by the server (`synth/cartridge_remote.c`) on top
of the synthesizers' common interface (`synth/fm1_synth.h`): entry 0 is MIDI CH, entry 1 is
FIRMWARE when the partition table has more than one slot, and the rest are the parameters of the
synthesizer the firmware was built with. FIRMWARE only accepts slots with a valid image (an EMPTY
one is skipped in the direction of the turn, and a turn that only finds empty slots
returns BAD_REQUEST); the ESP32 arms the switch when a VALUE changes the slot and executes it
`CONFIG_FM1_SLOT_SWITCH_MS` later if the value still differs from the running slot.

There is one pending request at a time. The UI accumulates turns of the same knob but does not
edit values of a selection that is not yet confirmed. It resends the same request after 250 ms
(three sends at most) and shows NO LINK on timeout. A query is sent on entering the page and
while looking for a disconnected cartridge (500 ms interval). While connected there is no
periodic polling: every reply to a knob carries the full state. Re-entering the page fetches the
real state and discards old turns; the knobs wait for that update before editing. Changes made
through other MIDI inputs are refreshed by leaving and returning to the page. A USB disconnection
clears the status; if only the server stops answering while USB stays up, the next request
detects it.

The server keeps the last request and reply: an exact repetition does not change the sound twice.
Edits with an outdated revision receive the current state without being applied. Each boot starts
with a random revision; the host's USB session also tags the queues so that old replies do not
leak into a new connection.

USB callbacks only validate and enqueue data. The DSP, preset changes, parameters and metadata
are serialized in the audio task. The reply travels through a queue and a bulk OUT transfer of
the MIDI client, separate from the UAC stream. Queues and buffers are bounded; a request overflow
is recovered by retry; notes keep the panic policy.

## MIDI channel

Each synthesizer answers one channel, by default channel 1 (`CONFIG_FM1_MIDI_CHANNEL`). Since the
FM-1 sends each track on its own channel (GLO → MIDI), only the track on channel 1 plays the
cartridge, and that track becomes the cartridge track. The first parameter of the CARTRIDGE page,
**MIDI CH**, changes this live: OMNI (all channels, no cartridge track) or 1..16. Clock and
transport are not filtered. The choice stays in RAM, like the other edits.

## Diagnostic build

`FELUCCA_CARTRIDGE_DIAG=1 sh build-cartridge.sh` (in `sloop-fm1/`) replaces the CARTRIDGE footer with
two lines of local counters. The four knobs stay available. Nothing changes in the DSP, the
receiver, the buffers or the ESP32 firmware, and no USB query is sent to obtain the numbers: the
footer is refreshed once a second from counters copied with interrupts briefly masked.

| Field | Meaning |
| --- | --- |
| D1 | Identifies the diagnostic build |
| P | Valid audio packets received per second; close to 1000 in a continuous stream |
| U | Underruns of the receiver buffer, since boot |
| E | Transport errors, overflows, 20 ms timeouts and malformed packets, since boot |
| N | Note On messages (velocity above zero) accepted into the FM-1's MIDI output queue, since boot; does not prove reception on the ESP32 |
| T | Control requests actually queued for the cartridge, including retries |
| L | Late audio renders on the FM-1 since the diagnostic screen was first opened |

Values saturate at 9999. `P--` means there is no recent window to compute the rate yet; wait one
second. The counters are not cleared when leaving the page; compare their growth during a test, as
a single start or reconnection event is not a continuous failure.

## Reproducible validation

```sh
cmake -S . -B out/native
cmake --build out/native --parallel
ctest --test-dir out/native --output-on-failure
cmake -S . -B out/native-san -DFM1_SANITIZE=ON
cmake --build out/native-san --parallel
ctest --test-dir out/native-san --output-on-failure
# In sloop-fm1/: sh build-cartridge.sh; sh tests/run_cartridge_tests.sh
sh tools/test_sloop_cartridge.sh native        # uses this repository's sloop-fm1/
sh tools/test_sloop_cartridge.sh sanitizer     # SLOOP_ROOT=... for another checkout
```

The integrated test links Sloop's real USB client, UI and SysEx assembler to the real CARTRIDGE
server with Clipper, in separate compilation units. It exercises the four knobs, every engine, a
retry after a lost reply, a stale ACK, the timeout, reconnection, the preservation of the local
parameters, the numbered page, the firmware name, the FIRMWARE parameter, the rejection of a
reply without slots, and the cartridge track: its names, notes over MIDI with the engine silent,
steps played with OUT at KEYS, STOP, PRESETS on HOME and on the PRESETS page, OMNI and the
handover back to the internal engine. The server tests (`test_cartridge.c`, one executable per port) check 407
preset selections and 2,963 parameter ranges across Clipper's 23 engines, 72 presets and 1,272
ranges in Felucca, 327 presets and 526 ranges in Melodee, the FIRMWARE entry with simulated slots
(an empty slot is skipped, returning to the running one disarms), limits, formatting, framing and
the effective change of the PCM by a level control.

Remote formatting treats Clipper's fixed FMX PTCH/ALG tables as enums: the upstream F_INT
formatter expects a NULL-terminated list, absent in those two tables. The workaround lives in the
adapter; the sources and hashes of `third_party/clipper` are unchanged. No native test is an audio
measurement on the hardware.
