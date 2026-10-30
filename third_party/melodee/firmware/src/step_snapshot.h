/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
#ifndef MELODEE_STEP_SNAPSHOT_H
#define MELODEE_STEP_SNAPSHOT_H
/* Compact steps for history and the inactive-pattern pool. Preserve every field, including unused note
 * bytes and n on rests/ties. Three bounded control fields share one byte;
 * all other bytes remain exact. */
typedef struct {
    uint8_t note[4], meta, vel, hit, acc, probability;
} step_snapshot_t;
_Static_assert(sizeof(step_snapshot_t) == 9u, "compact history step");
_Static_assert(ST_REST < 4u && (SF_ACCENT | SF_SLIDE | SF_RECORDED) < 8u,
               "history control fields fit one byte");
static void step_snapshot_put(step_snapshot_t *out, const step_t *in)
{
    for (uint32_t i = 0; i < 4u; i++) out->note[i] = in->note[i];
    out->meta = (uint8_t)(in->n | (in->time << 3) | (in->flags << 5));
    out->vel = in->vel; out->hit = in->hit; out->acc = in->acc; out->probability = in->probability;
}
static step_t step_snapshot_get(const step_snapshot_t *in)
{
    step_t out;
    for (uint32_t i = 0; i < 4u; i++) out.note[i] = in->note[i];
    out.n = in->meta & 7u; out.time = (in->meta >> 3) & 3u; out.flags = in->meta >> 5;
    out.vel = in->vel; out.hit = in->hit; out.acc = in->acc; out.probability = in->probability;
    return out;
}
static void step_snapshot_save(step_snapshot_t *out, const step_t *in)
{
    for (uint32_t i = 0; i < NSTEP; i++) step_snapshot_put(&out[i], &in[i]);
}
static void step_snapshot_load(step_t *out, const step_snapshot_t *in)
{
    for (uint32_t i = 0; i < NSTEP; i++) out[i] = step_snapshot_get(&in[i]);
}
static int step_snapshot_equal(const step_snapshot_t *a, const step_t *b)
{
    for (uint32_t i = 0; i < NSTEP; i++) {
        step_t s = step_snapshot_get(&a[i]);
        if (memcmp(&s, &b[i], sizeof s)) return 0;
    }
    return 1;
}
/* The inactive-pattern pool's step: the 64 bits of a bank record's step (pattern_store.c bank_step_pack), as two
 * little-endian words: four 7-bit notes, n / TIE / REST and the recorded marker (4), ACCENT SLIDE (2), velocity (7),
 * hits and accents (8 each), probability (7). Exact, but for n on a REST or TIE step (0 on the device: step_clear,
 * the ties recording writes), which reads back 0 */
#define STEP_PACKED_BYTES 8u
static void step_packed_put(uint8_t *out, const step_t *in)
{
    uint32_t n = in->n > 4u ? 4u : in->n;
    uint32_t meta = (in->time == ST_NOTE ? n : in->time == ST_TIE ? 5u : 6u) | (in->flags & SF_RECORDED ? 8u : 0u);
    uint32_t w0 = (in->note[0] & 127u) | (in->note[1] & 127u) << 7 | (in->note[2] & 127u) << 14 |
                  (in->note[3] & 127u) << 21 | meta << 28;
    uint32_t w1 = (in->flags & 3u) | (in->vel & 127u) << 2 | (uint32_t)in->hit << 9 | (uint32_t)in->acc << 17 |
                  (in->probability & 127u) << 25;
    for (uint32_t i = 0; i < 4u; i++) { out[i] = (uint8_t)(w0 >> (8u * i)); out[4u + i] = (uint8_t)(w1 >> (8u * i)); }
}
static step_t step_packed_get(const uint8_t *in)
{
    uint32_t w0 = 0, w1 = 0;
    for (uint32_t i = 0; i < 4u; i++) { w0 |= (uint32_t)in[i] << (8u * i); w1 |= (uint32_t)in[4u + i] << (8u * i); }
    step_t out;
    uint32_t code = (w0 >> 28) & 7u;
    for (uint32_t i = 0; i < 4u; i++) out.note[i] = (uint8_t)((w0 >> (7u * i)) & 127u);
    out.n = (uint8_t)(code < 5u ? code : 0u);
    out.time = (uint8_t)(code < 5u ? ST_NOTE : code == 5u ? ST_TIE : ST_REST);
    out.flags = (uint8_t)((w1 & 3u) | (w0 >> 31 ? SF_RECORDED : 0u));
    out.vel = (uint8_t)((w1 >> 2) & 127u); out.hit = (uint8_t)(w1 >> 9); out.acc = (uint8_t)(w1 >> 17);
    out.probability = (uint8_t)(w1 >> 25);
    return out;
}
#endif
