/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 3dSam (Clipper) */
/* Clipper 0.3: the master bus, before the volume knob and the peak limiter (fx.c mix_block). Synth only.
 *   EQ    three bands in parallel on the signal: a low shelf (a TPT one pole at 150 Hz), a peak at 1 kHz (a TPT SVF
 *         band-pass, Q 0.7) and a high shelf (one pole at 5 kHz); each -12 .. +12 dB in 1 dB steps (MID: 10 dB).
 *         The gains glide (~6 ms), so turning a band never clicks.
 *   COMP  a stereo-linked bus compressor (the "glue"): peak detector at the block rate (attack ~10 ms, release
 *         ~150 ms), a 6 dB soft knee in the log domain, then the gain ramped over each block. One knob: COMP 1..127
 *         lowers the threshold (-6 .. -30 dB) and raises the ratio (2:1 .. 4:1) together, with an automatic make-up
 *         of 60 % of what it takes at full scale. 0: off.
 * The values live in the groovebox's drum globals (core.h G_EQLOW ..), centred so a SLOOP project loads flat.
 * Integer only. */
static const uint16_t BUS_LOG2T[65] = {   /* log2(1 + i / 64), Q16 */
    0, 1466, 2909, 4331, 5732, 7112, 8473, 9814, 11136, 12440, 13727, 14996, 16248, 17484, 18704, 19909, 21098,
    22272, 23433, 24579, 25711, 26830, 27936, 29029, 30109, 31178, 32234, 33279, 34312, 35334, 36346, 37346, 38336,
    39316, 40286, 41246, 42196, 43137, 44068, 44990, 45904, 46809, 47705, 48593, 49472, 50344, 51207, 52063, 52911,
    53751, 54584, 55410, 56229, 57040, 57845, 58643, 59434, 60219, 60997, 61769, 62534, 63294, 64047, 64794, 65535};
static const uint16_t BUS_EXP2T[65] = {   /* 2^(i / 64), Q15 (65535: 2.0) */
    32768, 33125, 33486, 33850, 34219, 34591, 34968, 35349, 35734, 36123, 36516, 36914, 37316, 37722, 38133, 38548,
    38968, 39392, 39821, 40255, 40693, 41136, 41584, 42037, 42495, 42958, 43425, 43898, 44376, 44859, 45348, 45842,
    46341, 46846, 47356, 47871, 48393, 48920, 49452, 49991, 50535, 51085, 51642, 52204, 52773, 53347, 53928, 54515,
    55109, 55709, 56316, 56929, 57549, 58176, 58809, 59449, 60097, 60751, 61413, 62081, 62757, 63441, 64132, 64830,
    65535};
static const int16_t BUS_DB[25] = {       /* 10^(dB / 20) - 1, Q12, dB = -12 .. 12 */
    -3067, -2942, -2801, -2643, -2465, -2266, -2043, -1793, -1512, -1196, -842, -445, 0, 500, 1061, 1690, 2396,
    3188, 4077, 5074, 6193, 7448, 8857, 10437, 12210};

/* log2(x / 32768), Q16 (x > 0) */
static int32_t bus_log2(uint32_t x)
{
    int32_t e = 15;
    uint32_t i, f;
    while (x >= 65536u) {                    /* normalise to 32768 .. 65535 */
        x >>= 1;
        e++;
    }
    while (x < 32768u) {
        x <<= 1;
        e--;
    }
    i = (x - 32768u) >> 9;                   /* 0..63 */
    f = (x - 32768u) & 511u;
    return ((e - 15) << 16) + BUS_LOG2T[i] + (int32_t)(((BUS_LOG2T[i + 1] - BUS_LOG2T[i]) * f) >> 9);
}
/* 2^(y / 65536), Q14 (y: -14 .. 3 octaves) */
static int32_t bus_exp2(int32_t y)
{
    int32_t ip = y >> 16, m;
    uint32_t fr = (uint32_t)y & 0xFFFFu, i = fr >> 10, f = fr & 1023u;
    m = BUS_EXP2T[i] + (int32_t)(((BUS_EXP2T[i + 1] - BUS_EXP2T[i]) * f) >> 10);   /* Q15, 1..2 */
    if (ip >= 0)
        return ip > 3 ? (m << 3) >> 1 : (m << ip) >> 1;
    return -ip > 15 ? 0 : (m >> -ip) >> 1;
}

/* the shelves' one-pole corners (TPT: g / (1 + g), g = tan(pi fc / fs), Q16) per dB -12 .. 12: a boost turns at the
 * corner (150 Hz, 5 kHz), a cut at the corner moved by the gain (150 / G, 5 kHz x G), so a cut is the mirror of the
 * same boost and reaches its depth as a boost reaches its height */
static const uint16_t BUS_LO_K[25] = {2676, 2395, 2143, 1917, 1713, 1531, 1368, 1222, 1092, 975, 870, 776, 693, 693, 693,
                                      693, 693, 693, 693, 693, 693, 693, 693, 693, 693};
static const uint16_t BUS_HI_K[25] = {5395, 5997, 6660, 7388, 8187, 9062, 10018, 11060, 12194, 13426, 14762, 16208, 17771,
                                      17771, 17771, 17771, 17771, 17771, 17771, 17771, 17771, 17771, 17771, 17771, 17771};
/* the mid: a TPT state-variable filter at 1 kHz, Q 0.7 (a1 a2 a3, k = 1 / Q; Q16); its band-pass x k peaks at 1 */
#define BUS_A1 59200
#define BUS_A2 4224
#define BUS_A3 301
#define BUS_K 93623

static struct {
    int32_t lo[2], hi[2], m1[2], m2[2];                   /* the states, << 8 */
    int32_t gl, gm, gh;                                   /* the band gains now (10^(dB/20) - 1, Q12 << 4) */
    int32_t kl, kh;                                       /* (0.9) the shelves' corners now (they glide too) */
    int32_t env;                                          /* the compressor's detector (Q15 peak, << 8) */
    int32_t gc;                                           /* its gain now, Q14 */
    uint8_t eq_on;
} bus = {{0, 0}, {0, 0}, {0, 0}, {0, 0}, 0, 0, 0, 0, 0, 0, 16384, 0};

/* a TPT one-pole low-pass (k = g / (1 + g), Q16) of x; the state << 8 */
static inline int32_t bus_lp(int32_t x8, int32_t *s, int32_t k)
{
    int32_t v = (int32_t)(((int64_t)(x8 - *s) * k) >> 16), y = v + *s;
    *s = y + v;
    return y;
}

static int32_t bus_db(int32_t v, int32_t centre) { return clamp(v - centre, -12, 12) + 12; }   /* -> 0..24 */

static void bus_eq(int32_t *l, int32_t *r, uint32_t n)
{
    int32_t dl = bus_db(song.g[G_EQLOW], EQ_LOW_C), dm = bus_db(song.g[G_EQMID], EQ_MID_C),
            dh = bus_db(song.g[G_EQHIGH], EQ_HIGH_C);
    int32_t tl = BUS_DB[dl] << 4, tm = BUS_DB[dm] << 4, th = BUS_DB[dh] << 4, kl = BUS_LO_K[dl], kh = BUS_HI_K[dh];
    int32_t gl0 = bus.gl, gm0 = bus.gm, gh0 = bus.gh, gl1, gm1, gh1, kl0, kh0;
    uint32_t i, c;
    if (!tl && !tm && !th && !bus.gl && !bus.gm && !bus.gh) {
        if (bus.eq_on) {                                   /* flat: off, the states cleared for the next time */
            memset(bus.lo, 0, sizeof bus.lo), memset(bus.hi, 0, sizeof bus.hi);
            memset(bus.m1, 0, sizeof bus.m1), memset(bus.m2, 0, sizeof bus.m2);
            bus.eq_on = 0;
        }
        return;
    }
    if (!bus.eq_on)                                       /* (coming on: the corners from where they go) */
        bus.kl = BUS_LO_K[dl], bus.kh = BUS_HI_K[dh];
    bus.eq_on = 1;
    /* the gains glide: an eighth of the way a block (~6 ms), the last step exact */
    gl1 = bus.gl = gl0 + ((tl - gl0) >> 3) + (tl > gl0 ? 1 : tl < gl0 ? -1 : 0);
    gm1 = bus.gm = gm0 + ((tm - gm0) >> 3) + (tm > gm0 ? 1 : tm < gm0 ? -1 : 0);
    gh1 = bus.gh = gh0 + ((th - gh0) >> 3) + (th > gh0 ? 1 : th < gh0 ? -1 : 0);
    if ((gl0 < 0) != (gl1 < 0) && gl1)                    /* (the corner of the other sign: from the next block) */
        kl = gl1 < 0 ? BUS_LO_K[11] : BUS_LO_K[13];
    if ((gh0 < 0) != (gh1 < 0) && gh1)
        kh = gh1 < 0 ? BUS_HI_K[11] : BUS_HI_K[13];
    /* (0.9) the corners glide as the gains do, sample by sample: a corner that jumped with a 12 dB turn moved the
     * shelf's output at once (a click on a pad) */
    kl0 = bus.kl, kh0 = bus.kh;
    bus.kl = kl0 + ((kl - kl0) >> 3) + (kl > kl0 ? 1 : kl < kl0 ? -1 : 0);
    bus.kh = kh0 + ((kh - kh0) >> 3) + (kh > kh0 ? 1 : kh < kh0 ? -1 : 0);
    for (c = 0; c < 2u; c++) {
        int32_t *x = c ? r : l;
        for (i = 0; i < n; i++) {
            int32_t s = x[i], s8 = s << 8, lo, hp, bp, v1, v2, v3;
            int32_t g_l = gl0 + (((gl1 - gl0) * (int32_t)i) >> CTL_LOG2);
            int32_t g_m = gm0 + (((gm1 - gm0) * (int32_t)i) >> CTL_LOG2);
            int32_t g_h = gh0 + (((gh1 - gh0) * (int32_t)i) >> CTL_LOG2);
            lo = bus_lp(s8, &bus.lo[c], kl0 + (((bus.kl - kl0) * (int32_t)i) >> CTL_LOG2)) >> 8;
            hp = s - (bus_lp(s8, &bus.hi[c], kh0 + (((bus.kh - kh0) * (int32_t)i) >> CTL_LOG2)) >> 8);
            v3 = s8 - bus.m2[c];
            v1 = (int32_t)(((int64_t)BUS_A1 * bus.m1[c] + (int64_t)BUS_A2 * v3) >> 16);
            v2 = bus.m2[c] + (int32_t)(((int64_t)BUS_A2 * bus.m1[c] + (int64_t)BUS_A3 * v3) >> 16);
            bus.m1[c] = 2 * v1 - bus.m1[c];
            bus.m2[c] = 2 * v2 - bus.m2[c];
            bp = (int32_t)(((int64_t)v1 * BUS_K) >> 24);
            x[i] = s + (int32_t)(((int64_t)lo * g_l + (int64_t)bp * g_m + (int64_t)hp * g_h) >> 16);
        }
    }
}

static int32_t bus_gr_db10;                               /* the gain reduction now, 1/10 dB (the page's meter) */
static void bus_comp(int32_t *l, int32_t *r, uint32_t n)
{
    int32_t amt = song.g[G_COMP], pk = 0, g0 = bus.gc, g1, x;
    uint32_t i;
    if (!amt && bus.gc == 16384) {
        bus.env = 0;
        bus_gr_db10 = 0;
        return;
    }
    for (i = 0; i < n; i++) {
        int32_t a = l[i] < 0 ? -l[i] : l[i], b = r[i] < 0 ? -r[i] : r[i];
        if (a > pk)
            pk = a;
        if (b > pk)
            pk = b;
    }
    pk = pk > 8388607 ? 8388607 : pk;                     /* (<< 8 below: no overflow) */
    x = pk << 8;
    if (x > bus.env)                                      /* attack ~10 ms, release ~150 ms (per block) */
        bus.env += (int32_t)(((int64_t)(x - bus.env) * 4587) >> 16);
    else
        bus.env -= (int32_t)(((int64_t)(bus.env - x) * 316) >> 16);
    if (!amt) {
        g1 = g0 + ((16384 - g0) >> 3) + (g0 < 16384 ? 1 : g0 > 16384 ? -1 : 0);   /* switched off: back to unity */
    } else {
        /* in log2 (Q16, 0 = full scale): the threshold -6 .. -30 dB, the ratio 2 .. 4, a 6 dB knee */
        int32_t lv = bus.env > 256 ? bus_log2((uint32_t)bus.env >> 8 ? (uint32_t)bus.env >> 8 : 1u) : -(16 << 16);
        int32_t th = -65536 - amt * 4 * 65536 / 127;      /* -1 .. -5 octaves (6 dB each) */
        int32_t sl = 32768 + amt * 16384 / 127;           /* 1 - 1 / ratio, Q16: 0.5 .. 0.75 */
        int32_t kn = 32768, over = lv - th, gr, mk, gl;   /* half the knee: 3 dB */
        if (over <= -kn)
            gr = 0;
        else if (over >= kn)
            gr = (int32_t)(((int64_t)over * sl) >> 16);
        else {                                            /* the knee: (over + kn)^2 / (4 kn) x slope */
            int64_t o = over + kn;
            gr = (int32_t)((((o * o) >> 17) * sl) >> 16);   /* (4 kn = 2^17) */
        }
        mk = (int32_t)(((int64_t)(-th) * sl * 39322) >> 32);   /* the make-up: 60 % of the gain taken at 0 dB */
        gl = mk - gr;
        bus_gr_db10 = (int32_t)(((int64_t)gr * 602) >> 16);
        g1 = bus_exp2(gl);
    }
    bus.gc = g1;
    for (i = 0; i < n; i++) {
        int32_t g = g0 + (((g1 - g0) * (int32_t)i) >> CTL_LOG2);
        l[i] = (int32_t)(((int64_t)l[i] * g) >> 14);
        r[i] = (int32_t)(((int64_t)r[i] * g) >> 14);
    }
}

static void bus_process(int32_t *l, int32_t *r, uint32_t n)
{
    if (n > CTL)
        return;
    bus_eq(l, r, n);
    bus_comp(l, r, n);
}
