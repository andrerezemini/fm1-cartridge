/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 3dSam (Clipper) */
/* ACID: a bass line synthesizer in the manner of the 1981 Roland TB-303 (Clipper 0.2). One voice (mono).
 * A saw or a square (polyBLEP; the square slightly narrow, as the original's shaper makes it) into a four-pole
 * ladder (dsp.c ladder_tick, its resonance held under self-oscillation and the bass kept at high resonance), then a
 * one-pole high-pass and the drive. What makes it acid is the envelope logic:
 *   ENV    the filter envelope's depth: a decay from the note's start (DEC: 70 ms .. 3.8 s to -60 dB)
 *   ACC    accent (a note at velocity >= 120: the sequencer's ACCENT steps are 127): louder, the filter envelope
 *          short and deeper, and the accent SWEEP: successive accents charge a capacitor that pushes the cutoff up
 *          (the squelch of a run of accents)
 *   slide  a sequencer SLIDE (or legato with GLIDE in TIME mode) glides without restarting the envelopes (voice.c
 *          does not call note_on on a legato note)
 * EDIT 1  WAVE CUT RES ENV     EDIT 2  DEC ACC DIST SWP
 * Voice state: ph[0]; s[0..3] the ladder (Q12), s[4] the filter envelope (Q15), s[5] the accent envelope (Q15),
 * s[6] the accent sweep (Q15), s[7] the high-pass. */
static const char *const N_ACID_WAVE[] = {"SAW", "SQR"};

static void acid_note_on(track_t *t, voice_t *v)
{
    int acc = v->vel >= 120u;
    (void)t;
    if (!v->env && !v->env_out) {                    /* from silence: the filter clear */
        v->ph[0] = 0;
        v->s[0] = v->s[1] = v->s[2] = v->s[3] = v->s[7] = 0;
        v->s[6] = 0;
    }
    v->s[4] = 32767;                                 /* the filter envelope restarts (not on a slide) */
    v->s[5] = acc ? 32767 : 0;
}

/* a per-block decay factor (Q16) from a time index (DECAY_K: per sample, to -60 dB in 5 ms .. 4 s) */
static int32_t acid_block_k(uint32_t idx)
{
    uint32_t k = DECAY_K[idx & 127u], i;
    for (i = 0; i < CTL_LOG2; i++)                   /* k^32: five squarings */
        k = (k * k) >> 16;
    return (int32_t)k;
}

static void acid_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t i, ph = v->ph[0], inc = m->inc, sq = p[P_E0] == 1;
    int32_t acc = v->vel >= 120u ? p[P_E5] * 258 : 0;               /* Q15 accent amount */
    int32_t fenv = v->s[4], aenv = v->s[5], swp = v->s[6], hp = v->s[7];
    int32_t ls[4] = {v->s[0], v->s[1], v->s[2], v->s[3]};
    int32_t cut, res, gain, dist = p[P_E6], dg = 32768 + dist * 1024, dw = dist * 258;
    ladder_t lad;
    /* the envelopes, once a block: the filter's decays at DEC (an accent: short), the accent's fast */
    fenv = (int32_t)(((int64_t)fenv * acid_block_k(acc ? 60u : 50u + (uint32_t)p[P_E4] * 3u / 5u)) >> 16);   /* (to -60 dB:
                                                                  * 70 ms .. 3.8 s; an accent ~120 ms) */
    aenv = (int32_t)(((int64_t)aenv * acid_block_k(55u)) >> 16);
    /* the sweep capacitor: charged by the accent envelope, slowly let go (SWP: how far it pushes) */
    swp += ((aenv - swp) >> 4) - (swp >> 9);
    /* cutoff (0..127 << 8, ~14 steps an octave): CUT + ENV x the envelope (up to ~4.5 octaves) + the accent's own
     * (up to ~2.3) + the sweep (up to ~2.3) + the part (ENV / LFO) */
    cut = (p[P_E1] << 8) + ((fenv * p[P_E3]) >> 8) + ((aenv * (acc >> 7)) >> 10) + ((swp * p[P_E7]) >> 9) + m->cutoff;
    res = clamp(p[P_E2] + ((aenv * p[P_E5]) >> 17), 0, 118);           /* (an accent squeezes the resonance a bit) */
    ladder_coef(&lad, cut, res);
    gain = 32767 + (acc ? ((aenv >> 2) + 8192) : 0);                   /* accent: up to ~+5 dB */
    for (i = 0; i < n; i++, ph += inc) {
        int32_t s = sq ? (hy_saw(ph, inc) - hy_saw(ph + 0x73333333u, inc)) >> 1 : hy_saw(ph, inc);   /* (45 %) */
        s = ladder_tick(&lad, s, ls);
        hp += (s - hp) >> 9;                                            /* the high-pass (~14 Hz) */
        s -= hp;
        if (dist)                                                       /* DIST: the acid's overdrive */
            s += mulq15(softclip(((s >> 2) * (dg >> 2)) >> 11) - s, dw);
        out[i] += mulq15(mulq15(mulq15(s, gain), amp_at(m, i)), VOICE_FS) << 1;
    }
    v->ph[0] = ph;
    v->s[0] = ls[0], v->s[1] = ls[1], v->s[2] = ls[2], v->s[3] = ls[3];
    v->s[4] = fenv, v->s[5] = aenv, v->s[6] = clamp(swp, 0, 32767), v->s[7] = hp;
}

static const preset_t ACID_PRESETS[] = {
    /* {WAVE, CUT, RES, ENV, DEC, ACC, DIST, SWP}, {A D S R}, ENV -> FILTER (the part's: 0), mono; glide TIME (slides) */
    {"ACID LINE", {0, 30, 100, 70, 60, 90, 20, 60}, {0, 100, 110, 10}, 0, 1, FX(0, 0, 18, 6),
     XP(P_GLMODE + 1, 1, P_TRANS + 1, -12)},
    {"SQUELCH", {0, 24, 115, 90, 50, 127, 50, 100}, {0, 100, 110, 10}, 0, 1, FX(10, 0, 24, 8),
     XP(P_GLMODE + 1, 1, P_TRANS + 1, -12)},
    {"SQR ACID", {1, 34, 95, 60, 70, 80, 30, 50}, {0, 100, 110, 10}, 0, 1, FX(0, 0, 20, 8),
     XP(P_GLMODE + 1, 1, P_TRANS + 1, -12)},
    {"DEEP 303", {0, 20, 70, 40, 90, 60, 10, 30}, {0, 100, 120, 20}, 0, 1, FX(0, 0, 0, 4),
     XP(P_GLMODE + 1, 1, P_TRANS + 1, -24)},
    {"RUBBER", {0, 40, 80, 50, 40, 70, 0, 40}, {0, 80, 90, 14}, 0, 1, FX(0, 10, 10, 10),
     XP(P_GLMODE + 1, 1, P_TRANS + 1, -12)},
    {"DIST ACID", {0, 28, 108, 80, 55, 110, 110, 90}, {0, 100, 110, 10}, 0, 1, FX(30, 0, 28, 6),
     XP(P_GLMODE + 1, 1, P_TRANS + 1, -12)},
    {"ACID LEAD", {0, 50, 90, 60, 70, 70, 40, 60}, {0, 100, 110, 30}, 0, 1, FX(0, 10, 30, 30),
     XP(P_GLMODE + 1, 1, P_GLIDE + 1, 40)},
    {"BUBBLE", {1, 44, 112, 40, 30, 100, 0, 80}, {0, 70, 60, 10}, 0, 1, FX(0, 0, 32, 20), XP(P_GLMODE + 1, 1)},
    /* Clipper 0.3 */
    {"ACID HOUSE", {0, 36, 104, 64, 56, 100, 30, 70}, {0, 100, 110, 10}, 0, 1, FX(0, 0, 20, 8), XP(P_GLMODE + 1, 1, P_TRANS + 1, -12)},
    {"SQR SQUELCH", {1, 26, 118, 90, 46, 127, 40, 110}, {0, 100, 110, 10}, 0, 1, FX(10, 0, 24, 8), XP(P_GLMODE + 1, 1, P_TRANS + 1, -12)},
    {"SOFT 303", {0, 40, 60, 40, 80, 50, 0, 30}, {0, 100, 120, 20}, 0, 1, FX(0, 10, 10, 10), XP(P_GLMODE + 1, 1, P_TRANS + 1, -12)},
    {"ACID ARP", {0, 40, 100, 70, 50, 90, 20, 60}, {0, 100, 110, 10}, 0, 1, FX(0, 0, 30, 12), ARP(1, 2, 2, 50),
     XP(P_GLMODE + 1, 1, P_TRANS + 1, -12)},
    {"SLIDE BASS", {0, 30, 90, 50, 70, 70, 10, 40}, {0, 100, 120, 20}, 0, 1, FX(0, 0, 0, 6),
     XP(P_GLIDE + 1, 40, P_GLMODE + 1, 1, P_TRANS + 1, -12)},
    {"FUZZ ACID", {0, 32, 110, 80, 50, 110, 127, 90}, {0, 100, 110, 10}, 0, 1, FX(20, 0, 20, 8), XP(P_GLMODE + 1, 1, P_TRANS + 1, -12)},
    {"LOW 303", {0, 18, 80, 50, 80, 60, 20, 40}, {0, 100, 120, 20}, 0, 1, FX(0, 0, 0, 4), XP(P_GLMODE + 1, 1, P_TRANS + 1, -24)},
    {"SQR LEAD", {1, 56, 80, 50, 70, 60, 30, 40}, {0, 100, 110, 30}, 0, 1, FX(0, 10, 28, 28),
     XP(P_GLMODE + 1, 1, P_GLIDE + 1, 30, P_MWDST + 1, MD_VIB)},
};

static const engine_t ENG_ACID = {
    "ACID", {"VCF", "ENV"},
    {
        {"WAVE", F_ENUM, 0, 1, 0, N_ACID_WAVE, 0},
        {"CUT", F_CUTOFF, 0, 127, 30, 0, 0},
        {"RES", F_PCT, 0, 127, 100, 0, 0},
        {"ENV", F_PCT, 0, 127, 70, 0, 0},
        {"DEC", F_PCT, 0, 127, 60, 0, 0},
        {"ACC", F_PCT, 0, 127, 90, 0, 0},
        {"DIST", F_PCT, 0, 127, 20, 0, 0},
        {"SWP", F_PCT, 0, 127, 60, 0, 0},
    },
    ACID_PRESETS, sizeof(ACID_PRESETS) / sizeof(ACID_PRESETS[0]), 0, acid_note_on, acid_render,
    0xFD20, {P_E1, P_E2, P_E3, P_E4}, 1,
};
