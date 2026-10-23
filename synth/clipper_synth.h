/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef FM1_CARTRIDGE_CLIPPER_SYNTH_H
#define FM1_CARTRIDGE_CLIPPER_SYNTH_H

#include <stddef.h>
#include <stdint.h>
#include "fm1_synth.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CLIPPER_SYNTH_SAMPLE_RATE 44100u
#define CLIPPER_SYNTH_BLOCK_FRAMES 32u

/* One synthesizer instance. Call every function from the same audio task;
 * enqueue USB events in an application-owned task-safe queue first.
 * The upstream tables and DSP are fixed at 44.1 kHz. Returns 0 on success,
 * -1 for an unsupported rate; a failed init leaves the current state intact. */
int clipper_synth_init(uint32_t sample_rate);

/* status includes the channel nibble (0x90 | channel, channel 0..15).
 * Accepts notes, pressure, CC, pitch bend plus F8/FA/FB/FC/FF. Program
 * change and CC20 (UI-controlled morph) are unsupported. Returns 0 if queued,
 * -1 for invalid/unsupported input or before init, -2 on queue overflow.
 * Overflow panics to avoid a lost note-off leaving a voice stuck. */
int clipper_synth_midi(uint8_t status, uint8_t data1, uint8_t data2);

/* Interleaved signed 16-bit stereo. Any frame count, including zero.
 * Processes MIDI at the next internal 32-frame boundary. NULL is a no-op.
 * Before init and after panic, writes silence until a new note-on. */
void clipper_synth_render(int16_t *stereo, size_t frames);

/* Ends voices, queued events, held notes and effect tails immediately;
 * retains the selected sound. Safe before init. */
void clipper_synth_panic(void);

/* Select an upstream engine/factory preset, without remapping engine IDs.
 * Returns 0 on success, -1 if invalid. Changing a preset panics first. */
int clipper_synth_set_preset(uint32_t engine, uint32_t preset);
uint32_t clipper_synth_engine_count(void);
uint32_t clipper_synth_preset_count(uint32_t engine);
const char *clipper_synth_engine_name(uint32_t engine);
const char *clipper_synth_preset_name(uint32_t engine, uint32_t preset);
const char *clipper_synth_upstream_revision(void);

/* The MIDI channel the instrument answers: 0 OMNI (the library default), 1..16 that channel
 * only; realtime messages are never filtered. Also catalogue entry 0 ("MIDI CH"). Same
 * ownership as render. Returns -1 above 16. */
int clipper_synth_set_midi_channel(uint32_t channel);
uint32_t clipper_synth_midi_channel(void);

typedef fm1_param_t clipper_param_t;
/* Runtime editor: engine controls, common synthesis/modulation and master FX.
 * Indices are catalogue positions, resolved again after an engine change.
 * Same audio-task ownership as render; no UI or USB callback may call these. */
uint32_t clipper_synth_current_engine(void);
uint32_t clipper_synth_current_preset(void);
uint32_t clipper_synth_param_count(void);
int clipper_synth_param_get(uint32_t index, clipper_param_t *out);
int clipper_synth_param_set(uint32_t index, int32_t value);

/* The same calls behind the firmware-independent interface (the audio task, the cartridge server). */
const fm1_synth_api_t *clipper_synth_api(void);

#ifdef __cplusplus
}
#endif
#endif
