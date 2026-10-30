/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
/* Western AUTO harmony. Included by chord.c after chord_kit. All searches have
 * fixed bounds; no allocation, look-ahead, floating point or random choices.
 * chord_make/UI previews are read-only. Only chord_build commits history, and
 * the existing key/MIDI ownership freezes its result through release/strum. */
typedef struct {
    uint8_t n, note[CHORD_MAX], root;
    uint16_t mask, melody;
    int16_t scale, tonic, quant, voicing;
    uint32_t at;
} chord_auto_t;
/* Runtime-only history belongs in pool SRAM; the small ordinary RAM bank is
 * reserved for the interrupt/transport state that already nearly fills it. */
static chord_auto_t chord_auto[NTRK] __attribute__((section(".pool")));
static uint32_t chord_auto_clock;
static uint32_t chord_auto_enabled;
static int32_t chord_auto_abs(int32_t n) { return n < 0 ? -n : n; }
/* Four recent unharmonized onsets per track. Generated chords never feed this
 * history back into themselves. Currently held notes are collected separately. */
static struct { uint8_t n, next, note[4]; uint32_t at[4]; } chord_melody[NTRK] __attribute__((section(".pool")));

typedef struct { uint8_t n, pitch[4], cost; const char *name; } chord_auto_shape_t;
static const chord_auto_shape_t CH_AUTO_SHAPE[] = {
    {3, {0,4,7}, 0, ""}, {3, {0,3,7}, 0, "m"},
    {3, {0,3,6}, 6, "dim"}, {3, {0,4,8}, 6, "aug"},
    {3, {0,5,7}, 12, "sus4"}, {3, {0,2,7}, 12, "sus2"},
    {4, {0,4,7,10}, 16, "7"}, {4, {0,4,7,11}, 16, "maj7"},
    {4, {0,3,7,10}, 16, "m7"}, {4, {0,3,6,10}, 16, "m7b5"},
    {4, {0,3,6,9}, 16, "dim7"}, {4, {0,3,7,11}, 18, "mM7"},
    {4, {0,4,8,10}, 20, "+7"}, {4, {0,4,8,11}, 20, "+M7"},
    {4, {0,4,7,9}, 18, "6"}, {4, {0,3,7,9}, 18, "m6"},
    {4, {0,5,7,10}, 24, "7sus4"}, {4, {0,2,7,10}, 24, "7sus2"},
};

static uint32_t chord_auto_bits(uint32_t bits)
{
    uint32_t n = 0;
    while (bits) { bits &= bits - 1u; n++; }
    return n;
}
static uint32_t chord_auto_rotate(uint32_t bits, int32_t semitones)
{
    bits &= 0xFFFu;
    uint32_t n = (uint32_t)((semitones % 12 + 12) % 12);
    return ((bits << n) | (bits >> (12u - n))) & 0xFFFu;
}
static int chord_auto_config(const track_t *t, const chord_auto_t *h)
{
    return h->n && trk_vmode(t) == V_POLY && h->scale == t->p[P_SCALE] && h->tonic == t->p[P_ROOT] + t->p[P_TRANS] &&
        h->quant == t->p[P_QUANT] && h->voicing == t->p[P_VOIC];
}
static int chord_auto_same(const track_t *t, const chord_auto_t *h)
{
    return chord_auto_config(t, h) && chord_auto_clock - h->at < 2u * beat_samples();
}
static void chord_auto_forget(uint32_t k)
{
    memset(&chord_auto[k], 0, sizeof chord_auto[k]);
    memset(&chord_melody[k], 0, sizeof chord_melody[k]);
}
static void chord_auto_tick(uint32_t samples)
{
    chord_auto_clock += samples;
    uint32_t enabled = 0, previous = chord_auto_enabled;
    for (uint32_t k = 0; k < NTRK; k++)
        if (trk[k].p[P_CHRD] == CH_AUTO && !micro_scale(&trk[k])) enabled |= 1u << k;
    chord_auto_enabled = enabled;
    if (!enabled) {
        if (previous) { memset(chord_auto, 0, sizeof chord_auto); memset(chord_melody, 0, sizeof chord_melody); }
        return;
    }
    for (uint32_t k = 0; k < NTRK; k++) {
        /* A long held chord is still the current harmony. The phrase gap begins
         * at release, not at its original press; slow songs keep their context. */
        if (chord_auto_config(&trk[k], &chord_auto[k]) &&
            (live_held[k][0] || live_held[k][1] || live_held[k][2] || live_held[k][3] || trk[k].nheld))
            chord_auto[k].at = chord_auto_clock;
        if (trk[k].p[P_CHRD] != CH_AUTO || micro_scale(&trk[k]) ||
            (chord_auto[k].n && !chord_auto_same(&trk[k], &chord_auto[k]))) chord_auto[k].n = 0;
        /* Eager expiry also prevents timestamp wrap reviving ancient notes. */
        if (trk[k].p[P_CHRD] != CH_OFF || micro_scale(&trk[k])) chord_melody[k].n = 0;
        else if (chord_melody[k].n) {
            uint32_t newest = (chord_melody[k].next + 3u) % 4u;
            if (chord_auto_clock - chord_melody[k].at[newest] >= 8u * FS) chord_melody[k].n = 0;
        }
    }
}
static void chord_auto_observe(const track_t *t, uint32_t note)
{
    if (!chord_auto_enabled || t->p[P_CHRD] != CH_OFF || micro_scale(t) || chord_kit(t) || note > 127u) return;
    uint32_t k = trk_index(t);
    if (!chord_melody[k].n) chord_melody[k].next = 0;
    uint32_t at = chord_melody[k].next;
    chord_melody[k].note[at] = (uint8_t)(note % 12u);
    chord_melody[k].at[at] = chord_auto_clock;
    chord_melody[k].next = (uint8_t)((at + 1u) % 4u);
    if (chord_melody[k].n < 4u) chord_melody[k].n++;
}
static uint32_t chord_auto_melody(const track_t *t, uint32_t allowed)
{
    uint32_t pcs = 0;
    for (uint32_t k = 0; k < trk_n(); k++) {
        const track_t *other = &trk[k];
        if (other == t || other->p[P_MUTE] || (track_solo && !(track_solo & (1u << k))) ||
            other->p[P_CHRD] != CH_OFF || micro_scale(other) || chord_kit(other)) continue;
        for (uint32_t j = 0; j < chord_melody[k].n; j++)
            if (chord_auto_clock - chord_melody[k].at[j] < 2u * beat_samples())
                pcs |= 1u << chord_melody[k].note[j];
        for (uint32_t word = 0; word < 4u; word++) {
            uint32_t bits = live_held[k][word];
            pcs |= chord_auto_rotate(bits | (bits >> 12) | (bits >> 24), 32 * (int32_t)word);
        }
        for (uint32_t j = 0; j < other->seq_n; j++) pcs |= 1u << (other->seq_notes[j] % 12u);
    }
    return pcs & allowed;
}

/* Return a complete voicing even at MIDI endpoints. AUTO uses nearby inversions
 * with CLOSE; explicit OPEN/INV/+OCT remain deliberate player overrides. */
static void chord_auto_voice(const track_t *t, int32_t root, const uint8_t *pitch, uint32_t n,
                             const chord_auto_t *h, uint8_t *out)
{
    int32_t best = 0x7FFFFFFF;
    uint32_t inversions = t->p[P_VOIC] == VC_CLOSE ? n : 1u;
    for (uint32_t inv = 0; inv < inversions; inv++) {
        int32_t notes[4];
        for (uint32_t i = 0; i < n; i++) {
            uint32_t at = (i + inv) % n;
            notes[i] = root + pitch[at] + (at < inv ? 12 : 0);
        }
        if (t->p[P_VOIC] == VC_OPEN && n >= 3u) {
            int32_t x = notes[1];
            for (uint32_t i = 1; i + 1u < n; i++) notes[i] = notes[i + 1u];
            notes[n - 1u] = x + 12;
        } else if (t->p[P_VOIC] == VC_INV1 || t->p[P_VOIC] == VC_INV2) {
            uint32_t count = t->p[P_VOIC] == VC_INV2 ? 2u : 1u;
            for (uint32_t j = 0; j < count; j++) {
                int32_t x = notes[0] + 12;
                for (uint32_t i = 0; i + 1u < n; i++) notes[i] = notes[i + 1u];
                notes[n - 1u] = x;
            }
        } else if (t->p[P_VOIC] == VC_BASS) {
            /* Keep the selected third/seventh when the four-voice limit drops a fifth. */
            if (n == 4u) { notes[2] = notes[3]; }
            for (uint32_t i = n - 1u; i; i--) notes[i] = notes[i - 1u];
            notes[0] = root - 12;
        }
        for (int32_t oct = -3; oct <= 1; oct++) {
            int32_t low = notes[0] + 12 * oct, high = notes[n - 1u] + 12 * oct;
            if (low < 0 || high > 127) continue;
            int32_t score = chord_auto_abs(oct) * 2 + (int32_t)inv;
            if (h && t->p[P_VOIC] == VC_CLOSE) {
                for (uint32_t i = 0; i < n; i++) {
                    int32_t distance = 128;
                    for (uint32_t j = 0; j < h->n; j++) {
                        int32_t d = chord_auto_abs(notes[i] + 12 * oct - h->note[j]);
                        if (d < distance) distance = d;
                    }
                    score += 4 * distance;
                }
                score += chord_auto_abs(low - h->note[0]);
                /* Follow intentional register changes rather than drifting indefinitely. */
                score += chord_auto_abs(notes[n / 2u] + 12 * oct - root - 4);
            } else score += 12 * chord_auto_abs(oct) + 12 * (int32_t)inv;
            if (score >= best) continue;
            best = score;
            for (uint32_t i = 0; i < n; i++) out[i] = (uint8_t)(notes[i] + 12 * oct);
        }
    }
}

static uint32_t chord_auto_make(const track_t *t, uint32_t input, uint8_t *out, int32_t *rp, uint16_t *maskp)
{
    int32_t tonic = t->p[P_ROOT] + t->p[P_TRANS], root = (int32_t)input;
    uint32_t allowed = chord_auto_rotate(scale_mask(t), tonic);
    /* Use the same downward policy as DIA/SNAP, except the lower endpoint must
     * take the next available note instead of clamping to an out-of-scale C. */
    while (root >= 0 && !(allowed & (1u << (uint32_t)(root % 12)))) root--;
    if (root < 0) { root = 0; while (!(allowed & (1u << (uint32_t)(root % 12)))) root++; }
    *rp = root;
    if (trk_vmode(t) != V_POLY) { out[0] = (uint8_t)root; *maskp = 1; return 1; }
    uint32_t relative = chord_auto_rotate(allowed, -root), melody = chord_auto_melody(t, allowed);
    const chord_auto_t *h = &chord_auto[trk_index(t)];
    if (!chord_auto_same(t, h)) h = 0;
    if (h && h->root == root && h->melody == melody) {
        for (uint32_t i = 0; i < h->n; i++) out[i] = h->note[i];
        *maskp = h->mask;
        return h->n;
    }
    uint8_t degree[12], pitch[4];
    uint32_t count = 0, natural = 0, chosen = 0xFFFFu, n = 3;
    for (uint32_t i = 0; i < 12u; i++) if (relative & (1u << i)) degree[count++] = (uint8_t)i;
    if (count == 7u) natural = 1u | (1u << degree[2]) | (1u << degree[4]);
    int32_t best = 0x7FFFFFFF;
    for (uint32_t i = 0; i < NELEM(CH_AUTO_SHAPE); i++) {
        const chord_auto_shape_t *s = &CH_AUTO_SHAPE[i];
        uint32_t mask = 0;
        for (uint32_t j = 0; j < s->n; j++) mask |= 1u << s->pitch[j];
        if (mask & ~relative) continue;
        uint32_t pcs = chord_auto_rotate(mask, root);
        int32_t score = s->cost + 32 * (int32_t)chord_auto_bits(melody & ~pcs);
        if (natural && (mask & natural) == natural) score -= 12;
        if (h) {
            uint32_t previous = chord_auto_rotate(h->mask, h->root);
            score -= 2 * (int32_t)chord_auto_bits(previous & pcs);
            uint32_t prev = (uint32_t)((h->root - tonic + 120) % 12);
            uint32_t now = (uint32_t)((root - tonic + 120) % 12);
            /* ii/IV -> V invites a dominant seventh when the scale permits it;
             * V -> I prefers a settled triad. No forbidden tones are introduced. */
            if (now == 7u && (prev == 2u || prev == 5u) && mask == 0x491u) score -= 28;
            if (!now && prev == 7u && s->n == 3u) score -= 8;
        }
        if (score < best) { best = score; chosen = i; }
    }
    uint32_t mask = 0;
    if (chosen < NELEM(CH_AUTO_SHAPE)) {
        n = CH_AUTO_SHAPE[chosen].n;
        for (uint32_t i = 0; i < n; i++) pitch[i] = CH_AUTO_SHAPE[chosen].pitch[i];
    } else {
        /* Some blues-scale roots admit no named triad. Keep an honest in-scale
         * set instead of secretly adding a note or claiming a conventional name. */
        for (uint32_t i = 0; i < n; i++) pitch[i] = degree[2u * i % count];
    }
    if (n == 3u && t->p[P_VOIC] == VC_BASS) { pitch[3] = pitch[2]; n = 4; }
    for (uint32_t i = 0; i < n; i++) mask |= 1u << pitch[i];
    chord_auto_voice(t, root, pitch, n, h, out);
    *maskp = (uint16_t)mask;
    return n;
}

static void chord_auto_remember(const track_t *t, int32_t root, uint16_t mask, const uint8_t *notes, uint32_t n)
{
    chord_auto_t *h = &chord_auto[trk_index(t)];
    h->root = (uint8_t)root; h->mask = mask; h->n = (uint8_t)n;
    for (uint32_t i = 0; i < n; i++) h->note[i] = notes[i];
    h->scale = t->p[P_SCALE]; h->tonic = t->p[P_ROOT] + t->p[P_TRANS];
    h->quant = t->p[P_QUANT]; h->voicing = t->p[P_VOIC];
    h->melody = (uint16_t)chord_auto_melody(t, chord_auto_rotate(scale_mask(t), h->tonic));
    h->at = chord_auto_clock;
}

static void chord_auto_name(char *b, uint32_t root, uint32_t mask)
{
    const char *name = mask == 1u ? "" : " set";
    for (uint32_t i = 0; i < NELEM(CH_AUTO_SHAPE); i++) {
        uint32_t shape = 0;
        for (uint32_t j = 0; j < CH_AUTO_SHAPE[i].n; j++) shape |= 1u << CH_AUTO_SHAPE[i].pitch[j];
        if (shape == mask) { name = CH_AUTO_SHAPE[i].name; break; }
    }
    str_cpy(b, N_NOTE[root % 12u], 12);
    str_cpy(b + str_len(b), name, 12 - str_len(b));
}
