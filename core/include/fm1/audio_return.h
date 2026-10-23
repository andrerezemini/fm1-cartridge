/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef FM1_AUDIO_RETURN_H
#define FM1_AUDIO_RETURN_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FM1_RETURN_CAPACITY 1024u
#define FM1_RETURN_TARGET 256u
#define FM1_PCM_MAX_PACKET 196u

typedef struct {
    uint64_t received_frames, rendered_frames, silent_frames;
    uint32_t underruns, overruns, malformed_packets, resets;
    uint32_t fill_min, fill_max;
} fm1_return_stats_t;

/* Single-owner reference implementation for the future Sloop receiver.
 * NOT an ISR-safe ring. The FM-1 integration must define ownership/synchronization.
 * Linear ASRC is a proof of clock recovery, not the final audio-quality filter. */
typedef struct {
    int16_t pcm[FM1_RETURN_CAPACITY][2];
    uint32_t read_pos, count;
    double phase, ratio, filtered_fill;
    bool primed, adaptive;
    fm1_return_stats_t stats;
} fm1_audio_return_t;

void fm1_return_init(fm1_audio_return_t *s, bool adaptive);
/* Clears stale samples/phase, retaining cumulative counters. */
void fm1_return_reset(fm1_audio_return_t *s);
/* PCM signed16 LE stereo packet, 0..49 frames (44.1/48k transport).
 * Does NOT convert a nominal 48k stream to a nominal 44.1k stream.
 * Both clocks must have the same nominal rate. */
bool fm1_return_push(fm1_audio_return_t *s, const uint8_t *packet, size_t bytes);
/* Invoke once per simulated consumer quantum (1 ms in the simulator). */
void fm1_return_render(fm1_audio_return_t *s, int16_t *stereo, size_t frames);
/* Rational scheduling; 44,100 -> nine 44-frame + one 45-frame packets/ms. */
size_t fm1_packet_frames(uint32_t sample_rate, uint32_t *remainder);
#endif
