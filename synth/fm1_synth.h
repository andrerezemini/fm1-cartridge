/* SPDX-License-Identifier: GPL-3.0-only */
/* One synthesizer behind the cartridge: the same calls for every firmware (Clipper, Felucca,
 * Melodee), so the audio task, the cartridge server, the simulator and the tests do not care
 * which DSP is linked. Every call from the same audio task; USB callbacks only enqueue. */
#ifndef FM1_SYNTH_H
#define FM1_SYNTH_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define FM1_SYNTH_SAMPLE_RATE 44100u

typedef struct {
    int16_t value, min, max;
    char name[20], text[16], unit[8];
} fm1_param_t;

typedef struct {
    const char *name;                     /* "CLIPPER": shown by the FM-1 as "<name> OK" (12 chars at most) */
    const char *revision;                 /* upstream commit the port was taken from */
    int (*init)(uint32_t sample_rate);    /* 0, or -1 for an unsupported rate (state kept) */
    int (*midi)(uint8_t status, uint8_t data1, uint8_t data2);   /* 0 queued, -1 unsupported, -2 overflow (panics) */
    void (*render)(int16_t *stereo, size_t frames);               /* interleaved PCM16, any frame count */
    void (*panic)(void);                                          /* all voices and tails off; sound kept */
    int (*set_preset)(uint32_t engine, uint32_t preset);          /* -1 if invalid; panics first */
    uint32_t (*engine_count)(void);
    uint32_t (*preset_count)(uint32_t engine);
    const char *(*engine_name)(uint32_t engine);                  /* NULL if invalid */
    const char *(*preset_name)(uint32_t engine, uint32_t preset);
    uint32_t (*current_engine)(void);
    uint32_t (*current_preset)(void);
    /* the remote catalogue: entry 0 is always "MIDI CH" (0 OMNI, 1..16) */
    uint32_t (*param_count)(void);
    int (*param_get)(uint32_t index, fm1_param_t *out);           /* -1 if out of range */
    int (*param_set)(uint32_t index, int32_t value);              /* -1 if out of range or outside min..max */
    int (*set_midi_channel)(uint32_t channel);                    /* 0 OMNI, 1..16; -1 above 16 */
    uint32_t (*midi_channel)(void);
} fm1_synth_api_t;

#ifdef __cplusplus
}
#endif
#endif
