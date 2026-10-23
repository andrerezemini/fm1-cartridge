/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 3dSam (Clipper) */
/* VECTOR (Clipper 0.8): vector synthesis, after the Sequential Prophet VS and the Korg Wavestation's joystick (the
 * idea, not any code or wave of theirs). Four single-cycle waves sit at the corners of a square, A top left, B top
 * right, C bottom left, D bottom right; a point in it (X, Y) mixes them, each by its nearness (bilinear: the centre
 * an equal mix, a corner its wave alone); MOVE sends the point round a path by itself, from each note's start.
 * EDIT 1  A  B  C  D           the corners' waves (16 of Clipper's: vector_tables.h, tools/gen_vector.py)
 * EDIT 2  X  Y  MOVE  CUT
 *   X Y   the point, -64 (A's side) .. 63; SHAPE (LFO DEST SHP, MW / AT SHP, the matrix's SHP) moves X; the matrix
 *         moves X or Y themselves (DEST E5, E6: X, Y)
 *   MOVE  OFF; CIRC (a circle round the point); 8 (a figure eight); SWEEP (from A to the point, over the note: the
 *         VS's mix envelope); LOOP (the square's edge, A B D C, round the point); RND (a smooth random walk). Each slow
 *         and fast (+): 0.25 Hz / 2 Hz, a sweep of 4 s / 0.5 s
 *   CUT   a 12 dB low-pass (keyboard half), its envelope ENV DEST FLT; (0.9) FX > FILTER > MODL another (dsp.c fmod_*)
 * The waves: three band-limited copies each (512 points to the 96th harmonic, 128 to the 24th, 32 to the 6th), the
 * voice reading the copy that stays under ~20 kHz at its pitch.
 * Voice state: ph[0] the oscillator, ph[1] MOVE's phase (from the note's start), ph[2] RND's generator; s[0..3] the
 * filter (the SVF its first two), s[4] RND's point now, s[5] its target (x, y: two int16), s[6..7] the point at the last
 * block's end (Q15; s[6] < 0: none) */
static inline int32_t vec_lo(int32_t w) { return (int32_t)(int16_t)(w & 0xFFFF); }
static inline int32_t vec_hi(int32_t w) { return w >> 16; }
static inline int32_t vec_pk(int32_t x, int32_t y) { return (int32_t)((uint32_t)(uint16_t)x | (uint32_t)y << 16); }
#include "vector_tables.h"
enum { VM_OFF, VM_CIRC, VM_CIRCF, VM_EIGHT, VM_EIGHTF, VM_SWEEP, VM_SWEEPF, VM_LOOP, VM_LOOPF, VM_RND, VM_RNDF, VM_N };
static const char *const N_VMOVE[VM_N] = {"OFF", "CIRC", "CIRC+", "8", "8+", "SWEEP", "SWP+", "LOOP", "LOOP+", "RND",
                                          "RND+"};
static const uint16_t VEC_OFF[3] = {0, 512, 640};
#define VEC_RADIUS 13107                 /* CIRC 8 RND: 0.4 of the square */

static void vector_note_on(track_t *t, voice_t *v)
{
    (void)t;
    v->ph[1] = 0;
    v->s[4] = v->s[5] = 0;
    v->s[6] = -1;
    if (!v->ph[2])
        v->ph[2] = 0x2545F491u ^ ((uint32_t)v->note * 2654435761u);
    if (!v->env && !v->env_out)
        v->s[0] = v->s[1] = v->s[2] = v->s[3] = 0;
}

/* the point (Q15, 0..32767 each) at this block's end: X Y, SHAPE, MOVE */
static void vector_point(const track_t *t, voice_t *v, const vmod_t *m, uint32_t n, int32_t *px, int32_t *py)
{
    const int16_t *p = t->p;
    uint32_t mv = (uint32_t)clamp(p[P_E6], 0, VM_N - 1), fast = mv && !(mv & 1u);
    uint32_t inc = ((fast ? 6233054u : 779132u) >> CTL_LOG2) * n, ph = v->ph[1];
    int32_t x = (clamp(p[P_E4], -64, 63) + 64) * 258 + (m->shape - (64 << 8)), y = (clamp(p[P_E5], -64, 63) + 64) * 258;
    int32_t dx = 0, dy = 0;
    switch (mv) {
    case VM_CIRC: case VM_CIRCF:
        dx = mulq15(osc_sine(ph + 0x40000000u), VEC_RADIUS), dy = mulq15(osc_sine(ph), VEC_RADIUS);
        break;
    case VM_EIGHT: case VM_EIGHTF:
        dx = mulq15(osc_sine(ph), VEC_RADIUS), dy = mulq15(osc_sine(ph << 1), VEC_RADIUS * 3 / 4);
        break;
    case VM_SWEEP: case VM_SWEEPF: {                 /* from A (0, 0) to the point, eased (1 - (1 - q)^2) */
        int32_t q = (int32_t)(ph >> 17), e = 32767 - ((int32_t)(32767 - q) * (32767 - q) >> 15);
        x = mulq15(clamp(x, 0, 32767), e), y = mulq15(clamp(y, 0, 32767), e);
        if (ph + inc < ph)
            inc = 0xFFFFFFFFu - ph;                  /* (held at its end) */
        break;
    }
    case VM_LOOP: case VM_LOOPF: {                   /* the square's edge round the point, A B D C, half-side 0.5 */
        uint32_t side = ph >> 30;
        int32_t f = (int32_t)((ph >> 15) & 0x7FFFu) - 16384;   /* -0.5 .. 0.5 along the side */
        dx = side == 0 ? f : side == 1 ? 16384 : side == 2 ? -f : -16384;
        dy = side == 0 ? -16384 : side == 1 ? f : side == 2 ? 16384 : -f;
        break;
    }
    case VM_RND: case VM_RNDF:
        if (ph + inc < ph || v->s[6] < 0) {          /* a new target each period */
            uint32_t r = v->ph[2] = v->ph[2] * 1664525u + 1013904223u;
            v->s[5] = vec_pk(mulq15((int32_t)(r >> 16) - 32768, VEC_RADIUS), mulq15((int32_t)(r & 0xFFFFu) - 32768, VEC_RADIUS));
        }
        dx = vec_lo(v->s[4]), dy = vec_hi(v->s[4]);
        dx += (vec_lo(v->s[5]) - dx) >> (fast ? 2 : 5);
        dy += (vec_hi(v->s[5]) - dy) >> (fast ? 2 : 5);
        v->s[4] = vec_pk(dx, dy);
        break;
    default:
        break;
    }
    v->ph[1] = ph + inc;
    *px = clamp(x + dx, 0, 32767);
    *py = clamp(y + dy, 0, 32767);
}

static void vector_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t i, ph = v->ph[0], inc = m->inc, lv = inc < 20270000u ? 0u : inc < 81100000u ? 1u : 2u;
    uint32_t sh = 23u + 2u * lv, mask = (512u >> (2u * lv)) - 1u, fsh = sh - 15u;
    const int16_t *wa = VEC_WAVE[(uint32_t)clamp(p[P_E0], 0, VEC_NW - 1)] + VEC_OFF[lv];
    const int16_t *wb = VEC_WAVE[(uint32_t)clamp(p[P_E1], 0, VEC_NW - 1)] + VEC_OFF[lv];
    const int16_t *wc = VEC_WAVE[(uint32_t)clamp(p[P_E2], 0, VEC_NW - 1)] + VEC_OFF[lv];
    const int16_t *wd = VEC_WAVE[(uint32_t)clamp(p[P_E3], 0, VEC_NW - 1)] + VEC_OFF[lv];
    int32_t x1, y1, x0, y0, st[4] = {v->s[0], v->s[1], v->s[2], v->s[3]}, model = fm_model(t);
    int32_t a0, b0, c0, d0, a1, b1, c1, d1, da, db, dc, dd;
    fmod_t fm;
    vector_point(t, v, m, n, &x1, &y1);
    x0 = v->s[6] < 0 ? x1 : v->s[6], y0 = v->s[6] < 0 ? y1 : v->s[7];
    /* the corners' weights at the block's start and end, Q14 (their sum 16384): ramped over the block */
    a0 = ((32767 - x0) * (32767 - y0)) >> 16, b0 = (x0 * (32767 - y0)) >> 16, c0 = ((32767 - x0) * y0) >> 16;
    d0 = 16383 - a0 - b0 - c0;
    a1 = ((32767 - x1) * (32767 - y1)) >> 16, b1 = (x1 * (32767 - y1)) >> 16, c1 = ((32767 - x1) * y1) >> 16;
    d1 = 16383 - a1 - b1 - c1;
    da = (a1 - a0) / (int32_t)n, db = (b1 - b0) / (int32_t)n, dc = (c1 - c0) / (int32_t)n, dd = (d1 - d0) / (int32_t)n;
    fmod_coef(&fm, model, (p[P_E7] << 8) + m->cutoff + ((v->pitch16 - 60 * 16) << 3), model == FM_SVF ? 24 : 64);
    for (i = 0; i < n; i++, ph += inc) {
        uint32_t k = ph >> sh, k1 = (k + 1u) & mask;
        int32_t fr = (int32_t)((ph >> fsh) & 0x7FFFu), s, y;
        s = (wa[k] + (((wa[k1] - wa[k]) * fr) >> 15)) * a0;
        s += (wb[k] + (((wb[k1] - wb[k]) * fr) >> 15)) * b0;
        s += (wc[k] + (((wc[k1] - wc[k]) * fr) >> 15)) * c0;
        s += (wd[k] + (((wd[k1] - wd[k]) * fr) >> 15)) * d0;
        a0 += da, b0 += db, c0 += dc, d0 += dd;
        y = (s >> 14) * 3 >> 2;
        y = model == FM_SVF ? tsvf_lp(&fm.sv, y, &st[0], &st[1]) : fmod_tick(&fm, y << 1, st, st);
        if (y > 16000 || y < -16000)
            y = soft_knee(y, 16000);
        out[i] += mulq15(mulq15(y << 1, amp_at(m, i)), VOICE_FS) << 1;
    }
    v->ph[0] = ph;
    v->s[0] = st[0], v->s[1] = st[1], v->s[2] = st[2], v->s[3] = st[3];
    v->s[6] = x1;
    v->s[7] = y1;
}

/* waves: 0 SINE 1 TRI 2 SAW 3 SQR 4 PLS25 5 PLS10 6 ORGAN 7 VOX A 8 VOX O 9 BELL 10 HOLLW 11 BRASS 12 RESO 13 DIGI
 * 14 STRNG 15 METAL */
static const preset_t VECTOR_PRESETS[] = {
    /* {A, B, C, D, X, Y, MOVE, CUT}, {A D S R}, ENV -> FILTER, mono */
    {"VS PAD", {2, 7, 14, 9, 0, 0, VM_CIRC, 100}, {70, 100, 115, 95}, 0, 0, FX(0, 30, 20, 60)},
    {"VECTOR SWEEP", {0, 11, 13, 12, 30, 30, VM_SWEEP, 110}, {40, 100, 115, 90}, 0, 0, FX(0, 20, 30, 50)},
    {"VS BRASS", {11, 2, 3, 12, -40, -20, VM_OFF, 84}, {16, 84, 100, 50}, 30, 0, FX(0, 0, 10, 30)},
    {"GLASS VS", {9, 15, 0, 6, -20, 10, VM_EIGHT, 118}, {10, 100, 90, 80}, 0, 0, FX(0, 40, 30, 60)},
    {"VOX MORPH", {7, 8, 10, 1, 0, -10, VM_LOOP, 104}, {50, 100, 115, 90}, 0, 0, FX(0, 30, 20, 60)},
    {"DIGI BELLS", {13, 9, 15, 0, 20, 20, VM_RND, 120}, {0, 96, 50, 80}, 0, 0, FX(0, 30, 40, 60)},
    {"VS BASS", {2, 3, 4, 0, -50, -50, VM_SWEEPF, 70}, {0, 76, 100, 24}, 30, 1, FX(10, 0, 0, 10)},
    {"WAVE LOOP", {3, 13, 12, 5, 0, 0, VM_LOOPF, 108}, {10, 90, 110, 60}, 0, 0, FX(0, 20, 40, 40)},
    {"ORGAN DRIFT", {6, 10, 1, 0, -30, 20, VM_RND, 112}, {6, 60, 127, 40}, 0, 0, FX(0, 20, 0, 40)},
    {"STRING VS", {14, 2, 7, 10, 10, -10, VM_CIRC, 96}, {80, 100, 120, 100}, 0, 0, FX(0, 40, 20, 70)},
    {"METAL HIT", {15, 9, 13, 12, 40, -30, VM_SWEEPF, 120}, {0, 80, 0, 70}, 0, 0, FX(0, 20, 30, 60)},
    {"HOLLOW KEYS", {10, 1, 6, 4, -20, -20, VM_OFF, 98}, {0, 88, 60, 60}, 20, 0, FX(0, 20, 20, 40)},
    {"RESO VECTOR", {12, 2, 5, 11, 0, 0, VM_EIGHTF, 100}, {0, 90, 110, 50}, 10, 0, FX(0, 0, 30, 40)},
    {"SPACE DRONE", {9, 13, 7, 15, 0, 0, VM_RND, 110}, {110, 100, 127, 120}, 0, 0, FX(0, 50, 30, 90)},
    {"VS LEAD", {2, 11, 4, 12, -10, -40, VM_CIRCF, 104}, {0, 80, 110, 40}, 10, 1, FX(10, 0, 30, 30),
     XP(P_GLIDE + 1, 16)},
    {"CHOIR VS", {7, 8, 0, 10, 0, 20, VM_SWEEP, 100}, {90, 100, 120, 100}, 0, 0, FX(0, 40, 20, 80)},
    /* 0.9: the filter MODELs (FX > FILTER > MODL), VINTAGE */
    {"MS20 VECTOR", {2, 11, 3, 12, 20, -20, VM_EIGHTF, 66}, {0, 80, 100, 40}, 30, 1, FX(10, 0, 26, 24),
     XP(P_FMODEL + 1, FM_MS20, P_GLIDE + 1, 18, P_VINT + 1, 30)},
    {"DIODE VS BASS", {2, 3, 4, 0, -50, -50, VM_SWEEPF, 52}, {0, 70, 90, 24}, 40, 1, FX(6, 0, 0, 8),
     XP(P_FMODEL + 1, FM_DIODE, P_VINT + 1, 40)},
};

static const engine_t ENG_VECTOR = {
    "VECTOR", {"WAVES", "JOYSTICK"},
    {
        {"A", F_ENUM, 0, VEC_NW - 1, 2, N_VEC_WAVE, 0},
        {"B", F_ENUM, 0, VEC_NW - 1, 7, N_VEC_WAVE, 0},
        {"C", F_ENUM, 0, VEC_NW - 1, 14, N_VEC_WAVE, 0},
        {"D", F_ENUM, 0, VEC_NW - 1, 9, N_VEC_WAVE, 0},
        {"X", F_BIPCT, -64, 63, 0, 0, 0},
        {"Y", F_BIPCT, -64, 63, 0, 0, 0},
        {"MOVE", F_ENUM, 0, VM_N - 1, VM_CIRC, N_VMOVE, 0},
        {"CUT", F_CUTOFF, 0, 127, 100, 0, 0},
    },
    VECTOR_PRESETS, sizeof(VECTOR_PRESETS) / sizeof(VECTOR_PRESETS[0]), 1, vector_note_on, vector_render,
    0x6B3F, {P_E4, P_E5, P_E7, P_E6}, 0, 0, 0, 0,
};
