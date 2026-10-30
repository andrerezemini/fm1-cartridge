/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
/* Track note inserts. CPU0 owns the bounded queue; echoes never re-enter the
 * insert or alter patterns, recording, instrument patches or MIDI output. */
#define MD_QUEUE 64u
#define MD_NONE 255u
static struct {
    uint32_t time, beat, held[NTRK][4], signature[NTRK];
    uint8_t pending, ready;
    struct { uint32_t due; uint8_t track, note, vel, left, slot; } q[MD_QUEUE];
} midi_insert;
static uint32_t division_samples(uint32_t quarter, uint32_t div);
static void midi_insert_init(void)
{
    memset(&midi_insert, 0, sizeof midi_insert);
    memset(midi_verb, 0, sizeof midi_verb);
    for (uint32_t k = 0; k < MD_QUEUE; k++) midi_insert.q[k].track = MD_NONE;
    midi_insert.beat = FS / 2u;
    midi_insert.ready = 1;
}
static void midi_insert_remove(uint32_t k)
{
    if (midi_insert.q[k].track != MD_NONE) { midi_insert.q[k].track = MD_NONE; midi_insert.pending--; }
}
static int midi_insert_echo_held(uint32_t tr, uint32_t note)
{
    if (!midi_insert.pending) return 0;
    for (uint32_t k = 0; k < MD_QUEUE; k++)
        if (midi_insert.q[k].track == tr && midi_insert.q[k].note == note && !midi_insert.q[k].vel) return 1;
    return 0;
}
static void midi_insert_release(uint32_t tr, uint32_t note)
{
    if (!(midi_insert.held[tr][note >> 5] & (1u << (note & 31u))) && !midi_insert_echo_held(tr, note))
        trk_note_off_engine(&trk[tr], note);
}
static void midi_insert_cancel(uint32_t tr)
{
    if (!midi_insert.ready || !midi_insert.pending) return;
    /* Drop onsets first, then release generated notes with original ownership intact. */
    for (uint32_t k = 0; k < MD_QUEUE; k++)
        if (midi_insert.q[k].track == tr && midi_insert.q[k].vel) midi_insert_remove(k);
    for (uint32_t k = 0; k < MD_QUEUE; k++) if (midi_insert.q[k].track == tr) {
        uint32_t note = midi_insert.q[k].note;
        midi_insert_remove(k); midi_insert_release(tr, note);
    }
}
static int midi_insert_queue(uint32_t tr, uint32_t note, uint32_t vel, uint32_t left, uint32_t slot, uint32_t due)
{
    for (uint32_t k = 0; k < MD_QUEUE; k++) if (midi_insert.q[k].track == MD_NONE) {
        midi_insert.q[k].due = due; midi_insert.q[k].track = (uint8_t)tr;
        midi_insert.q[k].note = (uint8_t)note; midi_insert.q[k].vel = (uint8_t)vel;
        midi_insert.q[k].left = (uint8_t)left; midi_insert.q[k].slot = (uint8_t)slot;
        midi_insert.pending++; return 1;
    }
    return 0;
}
static uint32_t midi_insert_gap(const int16_t *p)
{ return division_samples(midi_insert.beat, (uint32_t)clamp(p[2], 0, 9)); }
static uint32_t midi_insert_velocity(uint32_t vel, const int16_t *p)
{
    /* DECAY is the percentage lost at each repeat. Even 0 terminates, with
     * a fixed ceiling of 32 repeats and at least one velocity step per hit. */
    uint32_t next = vel * (127u - (uint32_t)clamp(p[3], 0, 127)) / 127u;
    return next < vel ? next : vel - 1u;
}
static void midi_insert_controls(uint32_t tr)
{
    if (midi_track_only(&trk[tr])) {
        if (midi_insert.signature[tr]) midi_insert_cancel(tr);
        midi_insert.signature[tr] = midi_verb[tr] = 0;
        return;
    }
    uint32_t signature = 0, release = 0;
    for (uint32_t s = 0; s < FX_SLOTS; s++) {
        const int16_t *p = trk[tr].p + P_FXTYPE + s * FX_PARAMS;
        if (p[0] == FX_MIDIVERB && p[1] > (int32_t)release) release = (uint32_t)clamp(p[1], 0, 127);
        if (p[0] == FX_MIDIDELAY)
            for (uint32_t k = 0; k < FX_PARAMS; k++) signature = (signature * 16777619u) ^ ((uint16_t)p[k] + s * 131u);
    }
    midi_verb[tr] = (uint8_t)release;
    if (signature != midi_insert.signature[tr]) {
        midi_insert_cancel(tr); midi_insert.signature[tr] = signature;
    }
}
static void trk_note_on_raw(track_t *t, uint32_t note, uint32_t vel)
{
    if (midi_track_only(t)) { midi_track_note_on(t, note, vel); return; }
    if (!midi_insert.ready) midi_insert_init();
    if (note > 127u) return;
    uint32_t tr = (uint32_t)(t-trk);
    midi_insert_controls(tr);
    if (t->p[P_MUTE] || tr >= trk_n()) return;
    midi_insert.held[tr][note >> 5] |= 1u << (note & 31u);
    trk_note_on_engine(t, note, vel);
    for (uint32_t s = 0; s < FX_SLOTS; s++) {
        const int16_t *p = t->p + P_FXTYPE + s * FX_PARAMS;
        if (p[0] != FX_MIDIDELAY || !p[1]) continue;
        uint32_t v = midi_insert_velocity(vel, p) * (uint32_t)clamp(p[1], 0, 127) / 127u;
        if (v) midi_insert_queue(tr, note, v, 32, s, midi_insert.time + midi_insert_gap(p));
    }
}
static void trk_note_off_raw(track_t *t, uint32_t note)
{
    if (note > 127u) return;
    uint32_t tr = (uint32_t)(t-trk);
    midi_insert.held[tr][note >> 5] &= ~(1u << (note & 31u));
    midi_insert_release(tr, note);
}
static void midi_insert_tick(uint32_t n, uint32_t beat)
{
    if (!midi_insert.ready) midi_insert_init();
    midi_insert.beat = beat ? beat : FS / 2u;
    for (uint32_t tr = 0; tr < NPART; tr++) midi_insert_controls(tr); /* tracks 5..8 cannot have note inserts */
    midi_insert.time += n;
    if (!midi_insert.pending) return;
    /* Snapshot admission: new deadlines are future-only, independent of queue position. */
    for (uint32_t k = 0; k < MD_QUEUE; k++) if (midi_insert.q[k].track < NTRK &&
        (int32_t)(midi_insert.time - midi_insert.q[k].due) >= 0) {
        uint32_t tr = midi_insert.q[k].track, note = midi_insert.q[k].note, vel = midi_insert.q[k].vel;
        uint32_t left = midi_insert.q[k].left, slot = midi_insert.q[k].slot, due = midi_insert.q[k].due;
        midi_insert_remove(k);
        if (!vel) { midi_insert_release(tr, note); continue; }
        if (tr >= trk_n() || trk[tr].p[P_MUTE] || trk[tr].xf_on || trk[tr].eng_req != trk[tr].engine) continue;
        const int16_t *p = trk[tr].p + P_FXTYPE + slot * FX_PARAMS;
        if (p[0] != FX_MIDIDELAY || !p[1]) continue;
        uint32_t gap = midi_insert_gap(p), gate = gap / 2u;
        /* Retrigger replaces its previous release. Reserve the new release
         * before starting the note; full queues always fail silent. */
        for (uint32_t j = 0; j < MD_QUEUE; j++)
            if (midi_insert.q[j].track == tr && midi_insert.q[j].note == note && !midi_insert.q[j].vel) midi_insert_remove(j);
        if (!midi_insert_queue(tr, note, 0, 0, MD_NONE, midi_insert.time + (gate ? gate : 1u))) {
            midi_insert_release(tr, note); continue;
        }
        trk_note_on_engine(&trk[tr], note, vel);
        uint32_t next = midi_insert_velocity(vel, p);
        /* Keep the tempo grid despite control-block rounding; late service
         * skips missed repeats rather than issuing a burst of past onsets. */
        uint32_t next_due = due + gap;
        if ((int32_t)(midi_insert.time - next_due) >= 0)
            next_due += ((midi_insert.time - next_due) / gap + 1u) * gap;
        if (left > 1u && next) midi_insert_queue(tr, note, next, left - 1u, slot, next_due);
    }
}
