/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
/* CPU0 owns outgoing notes, separately from audio voices. Failed note-ons are
 * silent; failed releases remain pending until USB can accept them. A channel
 * change finishes the old channel before admitting notes on the new one. */
static int midi_track_emit(uint32_t packet); /* usb.c: bounded, reports admission */
static struct {
    uint32_t held[4], off[4];
    uint8_t channel, pedal, pedal_off;
} midi_output[NTRK];

static uint32_t midi_track_channel(const track_t *t)
{ return (uint32_t)clamp(t->p[P_E0], 1, 16) - 1u; }

static int midi_track_blocked(const track_t *t)
{
    uint32_t tr = (uint32_t)(t - trk);
    return tr >= trk_n() || t->p[P_MUTE] || (track_solo && !(track_solo & (1u << tr)));
}

static int midi_track_pending(uint32_t tr)
{
    return midi_output[tr].pedal_off || midi_output[tr].off[0] || midi_output[tr].off[1] ||
           midi_output[tr].off[2] || midi_output[tr].off[3];
}

static void midi_track_flush(uint32_t tr)
{
    if (!midi_track_pending(tr)) return;
    uint32_t ch = midi_output[tr].channel;
    if (midi_output[tr].pedal_off) {
        if (!midi_track_emit(0x0Bu | (0xB0u | ch) << 8 | 64u << 16)) return;
        midi_output[tr].pedal = midi_output[tr].pedal_off = 0;
    }
    for (uint32_t word = 0; word < 4u; word++) {
        uint32_t bits = midi_output[tr].off[word];
        while (bits) {
            uint32_t bit = (uint32_t)__builtin_ctz(bits), mask = 1u << bit;
            uint32_t note = word * 32u + bit;
            if (!midi_track_emit(0x08u | (0x80u | ch) << 8 | note << 16)) return;
            midi_output[tr].off[word] &= ~mask;
            midi_output[tr].held[word] &= ~mask;
            bits &= ~mask;
        }
    }
}

static void midi_track_all_off(track_t *t)
{
    uint32_t tr = (uint32_t)(t - trk);
    for (uint32_t word = 0; word < 4u; word++) midi_output[tr].off[word] |= midi_output[tr].held[word];
    if (midi_output[tr].pedal) midi_output[tr].pedal_off = 1;
    midi_track_flush(tr);
}

static void midi_track_service(track_t *t)
{
    uint32_t tr = (uint32_t)(t - trk);
    if (!midi_track_only(t) || midi_track_blocked(t) || midi_output[tr].channel != midi_track_channel(t))
        midi_track_all_off(t);
    else midi_track_flush(tr);
}

static void midi_track_note_off(track_t *t, uint32_t note)
{
    if (note > 127u) return;
    uint32_t tr = (uint32_t)(t - trk), word = note >> 5, mask = 1u << (note & 31u);
    if (midi_output[tr].held[word] & mask) midi_output[tr].off[word] |= mask;
    midi_track_flush(tr);
}

static void midi_track_note_on(track_t *t, uint32_t note, uint32_t vel)
{
    if (note > 127u || midi_track_blocked(t)) return;
    uint32_t tr = (uint32_t)(t - trk);
    midi_track_service(t);
    midi_track_note_off(t, note); /* retrigger: finish the previous strike */
    if (midi_track_pending(tr)) return;
    uint32_t ch = midi_track_channel(t);
    if (!midi_track_emit(0x09u | (0x90u | ch) << 8 | note << 16 | (vel & 127u) << 24)) return;
    midi_output[tr].channel = (uint8_t)ch;
    midi_output[tr].held[note >> 5] |= 1u << (note & 31u);
}

static void midi_track_controller(track_t *t, uint32_t st, uint32_t d1, uint32_t d2)
{
    if (midi_track_blocked(t)) return;
    uint32_t tr = (uint32_t)(t - trk);
    midi_track_service(t);
    if (midi_track_pending(tr)) return;
    uint32_t ch = midi_track_channel(t);
    if (!midi_track_emit((st >> 4) | (st | ch) << 8 | d1 << 16 | d2 << 24)) {
        if (st == 0xB0u && ((d1 == 64u && d2 < 64u) || d1 == 121u) && midi_output[tr].pedal)
            midi_output[tr].pedal_off = 1;
        return;
    }
    midi_output[tr].channel = (uint8_t)ch;
    if (st == 0xB0u && d1 == 64u) midi_output[tr].pedal = d2 >= 64u;
    if (st == 0xB0u && d1 == 121u) midi_output[tr].pedal = 0;
}
