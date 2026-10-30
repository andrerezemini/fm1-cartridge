/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
/* Western AUTO2 harmony: the mapped key is the melody, always on top. Chord
 * selection is a pure function of scale/root/transpose and melody pitch class;
 * accompaniment tracks, earlier presses and UI previews cannot change it.
 * Included by chord.c after chord_kit. Fixed searches, no allocation or history. */
typedef struct { uint8_t pitch[3], cost; const char *name; } chord_melody_shape_t;
static const chord_melody_shape_t CH_MELODY_SHAPE[] = {
    {{0,4,7}, 0, ""}, {{0,3,7}, 0, "m"},
    {{0,3,6}, 24, "dim"}, {{0,4,8}, 16, "aug"},
    {{0,5,7}, 18, "sus4"}, {{0,2,7}, 18, "sus2"},
};
static uint32_t chord_melody_rotate(uint32_t bits, int32_t semitones)
{
    uint32_t n = (uint32_t)((semitones % 12 + 12) % 12);
    bits &= 0xFFFu;
    return ((bits << n) | (bits >> (12u - n))) & 0xFFFu;
}
static int32_t chord_melody_mapped(const track_t *t, uint32_t input)
{
    uint32_t allowed = chord_melody_rotate(scale_mask(t), t->p[P_ROOT] + t->p[P_TRANS]);
    int32_t note = (int32_t)input;
    while (note >= 0 && !(allowed & (1u << (uint32_t)(note % 12)))) note--;
    if (note < 0) { note = 0; while (!(allowed & (1u << (uint32_t)(note % 12)))) note++; }
    return note;
}
static int32_t chord_melody_below(int32_t melody, uint32_t pitch_class)
{
    return melody - (melody - (int32_t)pitch_class + 12) % 12;
}
static uint32_t chord_melody_make(const track_t *t, uint32_t input, uint8_t *out, int32_t *rp, uint16_t *maskp)
{
    int32_t tonic = t->p[P_ROOT] + t->p[P_TRANS], melody = chord_melody_mapped(t, input);
    uint32_t allowed = chord_melody_rotate(scale_mask(t), tonic), pc = (uint32_t)melody % 12u;
    *rp = melody; *maskp = 1;
    if (trk_vmode(t) != V_POLY) { out[0] = (uint8_t)melody; return 1; }
    uint32_t root = pc, chosen = 0xFFFFu, mask = 0;
    int32_t best = 0x7FFFFFFF;
    /* Visit roots relative to the tonic so equal scores transpose consistently. */
    for (uint32_t degree = 0; degree < 12u; degree++) {
        uint32_t r = (uint32_t)((tonic + (int32_t)degree + 120) % 12);
        if (!(allowed & (1u << r))) continue;
        /* Tonic harmony anchors its own tones. The fourth/fifth roots are next;
         * a melody-root triad wins ordinary ties. Colour chords are fallbacks,
         * never selected just to chase passing notes in another track. */
        int32_t root_cost = !degree ? 0 : degree == 7u ? 8 : degree == 5u ? 10 : 14;
        for (uint32_t s = 0; s < NELEM(CH_MELODY_SHAPE); s++) {
            uint32_t shape = 0;
            for (uint32_t j = 0; j < 3u; j++) shape |= 1u << CH_MELODY_SHAPE[s].pitch[j];
            uint32_t pcs = chord_melody_rotate(shape, (int32_t)r);
            if ((pcs & ~allowed) || !(pcs & (1u << pc))) continue;
            int32_t score = root_cost + CH_MELODY_SHAPE[s].cost - (r == pc ? 6 : 0);
            if (score < best) { best = score; root = r; chosen = s; mask = shape; }
        }
    }
    int32_t notes[4]; uint32_t n = 0;
    if (chosen < NELEM(CH_MELODY_SHAPE)) {
        for (uint32_t i = 0; i < 3u; i++)
            notes[n++] = chord_melody_below(melody, (root + CH_MELODY_SHAPE[chosen].pitch[i]) % 12u);
    } else {
        /* Keep the melody and two separated lower degrees if no named triad
         * contains it. The display calls this a set rather than inventing a name. */
        notes[n++] = melody;
        int32_t note = melody; uint32_t degree = 0;
        while (n < 3u) {
            note--;
            if (allowed & (1u << (uint32_t)((note + 120) % 12))) {
                degree++;
                if (!(degree & 1u)) notes[n++] = note;
            }
        }
        root = (uint32_t)((notes[2] + 120) % 12);
        for (uint32_t i = 0; i < n; i++) mask |= 1u << (uint32_t)((notes[i] - (int32_t)root + 120) % 12);
    }
    for (uint32_t i = 1; i < n; i++)
        for (uint32_t j = i; j && notes[j] < notes[j-1u]; j--) {
            int32_t x = notes[j]; notes[j] = notes[j-1u]; notes[j-1u] = x;
        }
    /* Every voicing keeps the melody fixed. Only the lower voices move. */
    uint32_t voicing = (uint32_t)t->p[P_VOIC];
    if (voicing == VC_OPEN) notes[0] -= 12;
    else if (voicing == VC_BASS || voicing == VC_INV1 || voicing == VC_INV2) {
        uint32_t bass = root;
        if (voicing != VC_BASS && chosen < NELEM(CH_MELODY_SHAPE))
            bass = (root + CH_MELODY_SHAPE[chosen].pitch[voicing == VC_INV1 ? 1u : 2u]) % 12u;
        uint32_t at = n;
        if (voicing != VC_BASS)
            for (uint32_t i = 0; i + 1u < n; i++)
                if ((uint32_t)((notes[i] + 120) % 12) == bass) { at = i; break; }
        int32_t low = chord_melody_below(melody, bass);
        if (at == n) do { low -= 12; } while (low >= notes[0]);
        else while (low > notes[0]) low -= 12;
        notes[at] = low;
        if (at == n) n++;
    }
    /* At MIDI's lower endpoint omit unavailable accompaniment, never move the
     * melody up an octave to make room for a full chord. */
    uint32_t count = 0;
    for (uint32_t i = 0; i < n; i++) if (notes[i] >= 0) out[count++] = (uint8_t)notes[i];
    for (uint32_t i = 1; i < count; i++)
        for (uint32_t j = i; j && out[j] < out[j-1u]; j--) {
            uint8_t x = out[j]; out[j] = out[j-1u]; out[j-1u] = x;
        }
    *rp = (int32_t)root; *maskp = (uint16_t)mask;
    if (count == 1u) { *rp = melody; *maskp = 1; }
    return count;
}
static void chord_melody_name(char *b, uint32_t root, uint32_t mask)
{
    const char *name = mask == 1u ? "" : " set";
    for (uint32_t i = 0; i < NELEM(CH_MELODY_SHAPE); i++) {
        uint32_t shape = 0;
        for (uint32_t j = 0; j < 3u; j++) shape |= 1u << CH_MELODY_SHAPE[i].pitch[j];
        if (shape == mask) { name = CH_MELODY_SHAPE[i].name; break; }
    }
    str_cpy(b, N_NOTE[root % 12u], 12);
    str_cpy(b + str_len(b), name, 12 - str_len(b));
}
