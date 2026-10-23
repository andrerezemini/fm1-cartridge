/* SPDX-License-Identifier: GPL-3.0-only */
#include "fm1/audio_return.h"
#include <string.h>

void fm1_return_init(fm1_audio_return_t *s, bool adaptive)
{
    memset(s, 0, sizeof(*s));
    s->adaptive = adaptive;
    s->ratio = 1.0;
    s->filtered_fill = FM1_RETURN_TARGET;
    s->stats.fill_min = FM1_RETURN_CAPACITY;
}

void fm1_return_reset(fm1_audio_return_t *s)
{
    s->read_pos = s->count = 0;
    s->phase = 0.0;
    s->ratio = 1.0;
    s->filtered_fill = FM1_RETURN_TARGET;
    s->primed = false;
    s->stats.resets++;
}

bool fm1_return_push(fm1_audio_return_t *s, const uint8_t *p, size_t bytes)
{
    if (bytes > FM1_PCM_MAX_PACKET || bytes % 4 || (!p && bytes)) {
        s->stats.malformed_packets++;
        return false;
    }
    size_t frames = bytes / 4;
    if (frames > FM1_RETURN_CAPACITY - s->count) {
        s->stats.overruns++;
        return false; /* Drop whole new packet; never overwrite unread frames. */
    }
    for (size_t i = 0; i < frames; i++) {
        uint32_t at = (s->read_pos + s->count++) % FM1_RETURN_CAPACITY;
        for (size_t c = 0; c < 2; c++) {
            uint16_t bits = p[4 * i + 2 * c] | (uint16_t)p[4 * i + 2 * c + 1] << 8;
            s->pcm[at][c] = (int16_t)(bits <= 32767 ? bits : (int32_t)bits - 65536);
        }
    }
    s->stats.received_frames += frames;
    if (s->count > s->stats.fill_max) s->stats.fill_max = s->count;
    return true;
}

void fm1_return_render(fm1_audio_return_t *s, int16_t *out, size_t frames)
{
    if (!out || !frames) return;
    if (!s->primed && s->count >= FM1_RETURN_TARGET) s->primed = true;
    if (s->primed) {
        /* Low-pass the 44/45-frame sawtooth; proportional control converges
         * with a small bounded offset for crystal error. Clamp at +/-3000ppm. */
        s->filtered_fill += 0.02 * ((double)s->count - s->filtered_fill);
        double correction = (s->filtered_fill - FM1_RETURN_TARGET) * 0.00002;
        if (correction > 0.003) correction = 0.003;
        if (correction < -0.003) correction = -0.003;
        s->ratio = s->adaptive ? 1.0 + correction : 1.0;
        if (s->count < s->stats.fill_min) s->stats.fill_min = s->count;
    }
    for (size_t i = 0; i < frames; i++) {
        unsigned consume = (unsigned)(s->phase + s->ratio);
        if (s->primed && (s->count < 2 || s->count < consume)) {
            s->stats.underruns++;
            s->primed = false;
            s->phase = 0.0;
        }
        if (!s->primed) {
            out[2 * i] = out[2 * i + 1] = 0;
            s->stats.silent_frames++;
            continue;
        }
        uint32_t next = (s->read_pos + 1) % FM1_RETURN_CAPACITY;
        for (size_t c = 0; c < 2; c++) {
            double a = s->pcm[s->read_pos][c], b = s->pcm[next][c];
            out[2 * i + c] = (int16_t)(a + (b - a) * s->phase);
        }
        s->phase += s->ratio;
        s->phase -= consume;
        s->read_pos = (s->read_pos + consume) % FM1_RETURN_CAPACITY;
        s->count -= consume;
        s->stats.rendered_frames++;
    }
}

size_t fm1_packet_frames(uint32_t sample_rate, uint32_t *remainder)
{
    /* Valid for rates <= 48 kHz; caller keeps remainder from previous ms. */
    uint32_t accum = sample_rate + *remainder;
    *remainder = accum % 1000u;
    return accum / 1000u;
}
