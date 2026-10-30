# USB and audio contract between the ESP32 and Sloop

This contract defines the USB link between the ESP32-S3 host and the FM-1 running the modified Sloop. **Sloop's cartridge profile implements the playback below; the physical checks still open are listed in the [roadmap](ROADMAP.md).** Upstream Sloop has no playback interface. [Patch and provenance](../integrations/sloop-fm1/README.md) ([`sloop-fm1-cartridge.patch`](../integrations/sloop-fm1/sloop-fm1-cartridge.patch)). The host discovers the actual interfaces/endpoints from the descriptors; it does not hard-code interface numbers.

## MIDI

| Field | Contract |
| --- | --- |
| Interface | Audio class `0x01`, MIDIStreaming subclass `0x03` |
| Transport | USB-MIDI 1.0, 4-byte events; bulk IN `0x81` (notes, clock, cartridge requests) and bulk OUT `0x01` (cartridge replies), maximum 64 bytes |
| Cable Number | Cable 0; the decoder keeps each event's cable number and the dispatch does not filter on it |
| Channel | 16 channels in the parser; the synthesizer answers one channel (`CONFIG_FM1_MIDI_CHANNEL`, 1 by default, or OMNI), which MIDI CH on the CARTRIDGE page changes live; do not confuse MIDI channel with cable |
| Messages | Note On/Off; Note On with zero velocity = Note Off; CCs, pressure and Pitch Bend are delivered to the synthesizer adapter; Program Change, CC 0 and CC 32 on the synthesizer's channel pick the engine and preset in the cartridge server, before the adapter ([CARTRIDGE.md](CARTRIDGE.md#program-change-and-bank-select)); the adapters themselves return an unsupported-feature error for Program Change, and Clipper's also rejects CC20 |
| Transport | `F8` clock, `FA` start, `FB` continue, `FC` stop; not filtered by channel; do not assume every firmware sends all types |
| SysEx | A frame starting with `F0 7D 43 46` and a version byte of 4 or more, 13 bytes or longer, is a cartridge request; a newer peer's longer one is read as far as version 4's fields go ([CARTRIDGE.md](CARTRIDGE.md)); any other SysEx (Sloop's editor, loader or update) is ignored and is never a cartridge boot command |

Byte 0 contains the Cable Number in the high nibble and the CIN in the low nibble; bytes 1–3 contain MIDI. Validate length, CIN, status and data bytes before dispatching. A USB transfer whose length is not a multiple of four is malformed: the ESP32 never reads past its end, and closes the MIDI transport rather than lose a Note Off; a transfer error closes it too, and the ESP32 opens the device again (up to five times, clearing the endpoints' halts first). Handle System Real-Time messages independently of notes; clock must not create voices. One USB packet can contain up to 16 events on this endpoint.

Sloop's `seq.c` sends keyboard notes and, depending on the OUT setting, sequencer/arp/rolls; start, stop and clock with CLOCK at OUT, and always while the cartridge answers (unless Sloop follows an external clock). When a track becomes the cartridge's, Sloop sends All Notes Off (CC 123) on its channel, and also after a note-off it lost on a full output ring. The FM-1's knobs do not send CC; they edit the cartridge through the CARTRIDGE page. CC OUT is an optional extension in the [roadmap](ROADMAP.md).

Disconnection, reset, invalid configuration and unrecoverable error clear voices, queues and transport state. `STOP` is a musical event; the voice release policy must be explicit in the engine. Do not rely solely on receiving Note Off after the cable is pulled.

Each synthesizer adapter (`synth/fm1_synth.h`) accepts only 44.1 kHz, is a single instance and requires calls serialized by the audio task. SysEx is handled by the cartridge server, not by the adapters; the firmware keeps the sound it sets in NVS. See [PORTS.md](PORTS.md) for the API and limits.

## Audio from the cartridge to the FM-1

| Field | Implementation |
| --- | --- |
| Class | UAC1 AudioStreaming, PCM Type I |
| Direction | ESP32-S3 host → FM-1 device; ISO OUT |
| Format | Signed 16-bit little-endian, two interleaved L/R channels |
| Rate | 44,100 stereo frames/s; rate bytes `44 AC 00` |
| Audio frame | 4 bytes: L low, L high, R low, R high |
| USB FS interval | 1 ms (`bInterval=1`) |
| Nominal packetization | 44 or 45 frames: 176 or 180 bytes; 441 frames every 10 ms |
| Endpoint maximum | 180 bytes for this 44.1 kHz profile |
| Synchronization | Adaptive (`bmAttributes=0x09`), ASRC in the receiver; no feedback endpoint |
| Alt settings | 0 = no endpoints/bandwidth; 1 = streaming |
| Endpoint | `0x04` OUT; the optional capture uses `0x84` IN |

Do not always send 44 frames/ms: that produces 44,000 Hz. The fractional accumulator must generate exactly 44,100 frames every 1,000 nominal USB frames; in this firmware the `usb_host_uac` component performs that 44/45 split. Do not open the capture endpoint `0x84` thinking it is playback.

The receiver accepts an actual length aligned to 4 bytes and up to the advertised maximum, including complete short packets. Zero length is ignored; format/transport errors are counted. It does not pad packets with reads outside the buffer. A lost frame is not retransmitted in ISO. When data is missing, the return goes silent and restarts priming when needed, without repeating old audio indefinitely. Every start and re-prime fades in over 5.8 ms and every gain change glides over a block; a stop cannot fade, since nothing more arrives. How these sound is evaluated on the bench.

## Lifecycle

1. Install the host and clients; wait for a compatible device.
2. Read the full configuration, walk interfaces/alternates with bounds validation and locate MIDI IN.
3. Start MIDI and log the descriptors. Upstream Sloop has no playback interface; against it, the MIDI stage continues without audio.
4. With the modified Sloop: select a compatible playback profile, set the sample rate according to the descriptor, open alt 1 and start with silence.
5. Produce PCM without blocking the USB service; track completion/error callbacks.
6. On unplug, suspend production, clear voices, cancel/drain transfers as the library requires and close interfaces/device; only then free buffers.
7. Re-enumerate on reconnection, without reusing old addresses, handles or PCM.

Minimum metrics: connections, disconnections, valid/invalid/ignored MIDI, clocks, ISO callbacks, bytes/frames sent, transfer failures, low queue, underrun/overrun in the receiver, occupancy range and rate correction. There is no audio timestamp or additional sequence number inside UAC1 PCM.
