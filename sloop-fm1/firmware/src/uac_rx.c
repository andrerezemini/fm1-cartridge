/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Andre Cardoso
 * Cartridge USB playback: 44.1 kHz, signed 16-bit little-endian stereo.
 * Included before fx.c / usb.c in the single firmware compilation unit.
 *
 * TIMER5 is the sole producer, including stream/reset/timeout changes. Core 0's
 * audio render is the sole consumer. Neither side ever changes the other side's
 * cursor. Reset publishes a generation + discard floor, not reused indices;
 * unread slots stay owned by the consumer until its final commit. USB may thus
 * preempt interpolation without overwriting samples being read. Two short IRQ
 * windows snapshot state and commit the sum; interpolation stays outside them.
 * A reset during interpolation cancels that entire return block. The guards
 * preserve ICFG (including nested IRQ state), following WL82 asm/irq.h.
 *
 * No pool, low RAM, floating point, allocation, or resampling in USB callbacks.
 * Gain is volatile RAM-only, 0..128 (128 = unity, 0 = mute; still drains).
 * 4-point Hermite interpolation is a bring-up ASRC, not a measured audio-quality
 * claim. Target hardware load, noise, latency and clock range need bench tests.
 */
#ifndef FELUCCA_UAC_RX
#define FELUCCA_UAC_RX 0
#endif
#if FELUCCA_UAC_RX
#define UAR_N 1024u
#define UAR_MASK (UAR_N - 1u)
#define UAR_BLOCK 32u
/* The DAC renders eight 32-frame blocks in a burst, so the occupancy seen per
 * block swings by 256 around this mean: the floor is one burst below it. 384
 * keeps ~5 ms of packets in hand when the host skips a frame or two (a late
 * ISO submission on the ESP32), where 256 left ~2 ms and every hiccup became a
 * re-prime: 10 ms of silence and a click. Prime above the mean so the first
 * burst leaves interpolation samples and USB jitter room. */
#define UAR_TARGET 384u
/* The rate loop reads the occupancy once a burst, as the burst begins (audio.c calls uac_rx_burst at the
 * interrupt's entry; the first block reads it): there it is what came in over a whole DAC period, whatever
 * the render's own length (TIMER5 pushes packets inside the render, so a reading taken in it follows Sloop's
 * CPU load and wobbles the pitch). Its aim there is the occupancy whose mean over the burst's eight blocks is
 * UAR_TARGET; a start begins at it. */
#define UAR_TARGET_FIRST (UAR_TARGET + 7u * UAR_BLOCK / 2u)
#define UAR_PRIME 512u
#define UAR_TIMEOUT_MS 20u
#define UAR_ONE 16777216u                  /* Q24 input frames per output frame */
#define UAR_CORR_MAX 83886                /* +/- 5000 ppm, rounded down */
/* A start or a re-prime fades in over UAR_RAMP blocks (5.8 ms: the stream comes back without a click; the
 * dropout before a re-prime is still a cut), and the gain glides sample by sample from one block's value to
 * the next (no zipper steps when USB RETURN, the track's LEVEL or a mute changes). 0: the gain applies at
 * once (the exact-value tests). */
#ifndef UAR_RAMP
#define UAR_RAMP 8u
#endif
#ifndef UAC_RX_BARRIER
#include "../hal/fm1_usb.h"                /* existing HAL ssync + memory clobber */
#define UAC_RX_BARRIER() fm1_usb_rx_sync()
#endif
#ifndef UAC_RX_LOCK
static uint32_t uac_rx_lock(void)
{
    uint32_t s = fm1_icfg();
    fm1_icfg_set(s & ~0x100u);
    UAC_RX_BARRIER();
    return s;
}
static void uac_rx_unlock(uint32_t s)
{
    UAC_RX_BARRIER();
    fm1_icfg_set(s);
}
#define UAC_RX_LOCK() uac_rx_lock()
#define UAC_RX_UNLOCK(s) uac_rx_unlock(s)
#endif
/* A host test can inject the TIMER5 producer at this precise preemption point. */
#ifndef UAC_RX_PRECOMMIT
#define UAC_RX_PRECOMMIT() ((void)0)
#endif
static uint32_t uar_ring[UAR_N];
static volatile uint32_t uar_w, uar_r;
static struct {
    volatile uint32_t generation, floor, active;
    uint32_t now, last_packet, seen_packet;       /* producer only */
    uint32_t packets, malformed, errors, overruns, timeouts;
    uint32_t underruns, reprimes, cancelled;      /* consumer only */
    uint32_t ramp, g_last;                        /* consumer only: blocks into the start ramp, the last gain */
    uint32_t epoch, phase, go, hist;              /* consumer only; hist: the frame before uar_r */
    uint32_t bursts;                              /* consumer only: a burst's first block has been seen (uar_first) */
    int32_t fill_q8, integral, correction, aim;  /* aim: the correction the last burst asked for */
    int32_t learned;                              /* a slow mean of aim: the clocks' offset, kept across re-primes */
    uint32_t fill_min, fill_max, limited;         /* consumer diagnostics */
} uar;
static volatile uint8_t uac_rx_gain = 128u;
static volatile uint32_t uar_loud_ms;              /* fm1_ms of the last block above -54 dBFS (project.c audio_quiet) */
static volatile uint32_t uar_track_q12 = 4096u;    /* the cartridge track's LEVEL and MUTE, Q12 (seq.c cart_tracks_block) */
static volatile uint32_t uar_peak;                 /* the largest |sample| since the cartridge track's meter took it */
static volatile uint8_t uar_first;                 /* the next block is a burst's first (audio.c): the rate loop's turn */
static volatile uint32_t uar_mark;                 /* uar_w as that burst began */
static void uac_rx_burst(void)                     /* a DAC burst begins: the audio interrupt's entry, before it renders */
{
    uar_mark = uar_w;
    uar_first = 1;
}

/* Producer only. Published while the audio ISR cannot interrupt TIMER5. Startup
 * / detach callers outside TIMER5 must already have IRQs masked (usb.c contract). */
static void uac_rx_discard(void)
{
    uar.floor = uar_w;
    UAC_RX_BARRIER();
    uar.generation++;
}

static void uac_rx_stream(uint32_t alt)
{
    uint32_t flags = UAC_RX_LOCK();
    uar.active = alt != 0u;
    uar.seen_packet = 0;
    uac_rx_discard();
    UAC_RX_UNLOCK(flags);
}

/* One ISO packet lost (SIE OverRun) or corrupt (DataError / IncompRx): a
 * millisecond of audio is missing and the clock loop absorbs it. Dropping the
 * whole ring here turned each event into a re-prime: ~10 ms of silence and a
 * click, heard as a stuttering note on a marginal link. Count only. */
static void uac_rx_packet_error(void)
{
    uar.errors++;
}

static void uac_rx_tick(uint32_t now_ms)
{
    uar.now = now_ms;
    if (uar.active && uar.seen_packet && (uint32_t)(now_ms - uar.last_packet) >= UAR_TIMEOUT_MS) {
        uar.timeouts++;
        uar.seen_packet = 0;
        uac_rx_discard();
    }
}

/* USB callback: validate one packet, unpack LE safely (including unaligned
 * input), then publish the complete packet. On overflow discard/reprime; never
 * move the consumer's cursor or overwrite its in-flight interpolation inputs. */
static void uac_rx_push(const uint8_t *p, uint32_t bytes)
{
    uint32_t w, n, r, i;
    if (!uar.active || !bytes)
        return;
    if (bytes > 180u || (bytes & 3u) || !p) {
        uar.malformed++;
        uac_rx_discard();
        return;
    }
    w = uar_w;
    r = uar_r;
    UAC_RX_BARRIER();
    n = bytes >> 2;
    if (w - r > UAR_N || n > UAR_N - (w - r)) {
        uar.overruns++;
        uac_rx_discard();
        return;
    }
    for (i = 0; i < n; i++, p += 4)
        uar_ring[(w + i) & UAR_MASK] = (uint32_t)p[0] | (uint32_t)p[1] << 8 |
                                             (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    UAC_RX_BARRIER();
    uar_w = w + n;
    uar.last_packet = uar.now;
    uar.seen_packet = 1;
    uar.packets++;
}

static int32_t uac_rx_clamp(int32_t x, int32_t lo, int32_t hi)
{
    return x < lo ? lo : x > hi ? hi : x;
}

/* 4-point Catmull-Rom (Hermite) interpolation, fraction f in Q15. With linear
 * interpolation the slowly sweeping fraction modulated the treble by up to
 * 2.4 dB at 10 kHz, a tremolo at the beat rate of the two 44.1 kHz clocks;
 * this keeps it to 0.5 dB at 9 kHz, 0.7 dB at 10 kHz. The 64-bit products
 * never overflow, even on full-scale steps (the master limiter follows the
 * mix; the mix is int32). */
static int32_t uac_rx_hermite(int32_t xm1, int32_t x0, int32_t x1, int32_t x2, int32_t f)
{
    int32_t a1 = x1 - xm1, a2 = 2 * xm1 - 5 * x0 + 4 * x1 - x2, a3 = 3 * (x0 - x1) + x2 - xm1;
    int64_t y = ((int64_t)a3 * f) >> 15;
    y = ((y + a2) * f) >> 15;
    y = ((y + a1) * f) >> 16;                     /* the Catmull-Rom 1/2 */
    return x0 + (int32_t)y;
}

static int32_t uac_rx_add(int32_t a, int32_t b)
{
    /* The normal synth mix is far inside these limits. Avoid signed overflow
     * even for adversarial host tests; this is not the master audio limiter. */
    if (b > 0 && a > INT32_MAX - b) return INT32_MAX;
    if (b < 0 && a < INT32_MIN - b) return INT32_MIN;
    return a + b;
}

static void uac_rx_mix(int32_t *l, int32_t *r, uint32_t n)
{
    int32_t sl[UAR_BLOCK], sr[UAR_BLOCK], error, corr, g0, dg;
    uint32_t flags, epoch, rd, wr, occ, phase, step, i, gain, hist, peak = 0, first = uar_first, mark = uar_mark;
    uar_first = 0;
    if (!n || n > UAR_BLOCK)
        return;
    uar.bursts |= first;
    flags = UAC_RX_LOCK();
    epoch = uar.generation;
    if (uar.epoch != epoch) {
        uar_r = uar.floor;               /* only the consumer writes its cursor */
        uar.epoch = epoch;
        uar.go = uar.phase = 0;
        uar.fill_q8 = (int32_t)UAR_TARGET_FIRST << 8;   /* (the rate learned is kept: the host's clock is the same) */
        uar.integral = uar.learned * 2048;
        uar.aim = uar.correction = uar.learned;
        uar.ramp = uar.g_last = 0;
        uar.reprimes++;
    }
    rd = uar_r;
    wr = uar_w;
    UAC_RX_BARRIER();
    occ = mark - rd;                              /* the occupancy as the burst began */
    if (!first || occ > wr - rd)
        occ = wr - rd;                            /* (another block, or the ring reset since) */
    /* a start waits for a burst's first block, its prime counted as the burst began (where the rate loop
     * reads, aiming at UAR_TARGET_FIRST), once the bursts are known: a prime counted later in a burst would
     * put the first reading off the aim */
    if (!uar.active || (!uar.go && (occ < UAR_PRIME || (uar.bursts && !first)))) {
        UAC_RX_UNLOCK(flags);
        return;
    }
    if (!uar.go) {
        uar.go = 1;
        if (occ > UAR_TARGET_FIRST) {             /* start at the aim: no rate excursion to drain the prime */
            uar_r = rd += occ - UAR_TARGET_FIRST;
            occ = UAR_TARGET_FIRST;
        }
        uar.fill_min = wr - rd;
        uar.fill_max = wr - rd;
        uar.hist = uar_ring[rd & UAR_MASK];       /* x[-1] = x[0]: a constant stream starts flat */
    }
    phase = uar.phase;
    hist = uar.hist;
    gain = uac_rx_gain;
    UAC_RX_UNLOCK(flags);
    if (gain > 128u) gain = 128u;
    gain = gain * uar_track_q12 >> 12;            /* (the track's LEVEL above its default: up to +11.5 dB) */
    if (UAR_RAMP && uar.ramp < UAR_RAMP)
        gain = gain * ++uar.ramp / UAR_RAMP;
    g0 = UAR_RAMP ? (int32_t)uar.g_last : (int32_t)gain;
    dg = ((int32_t)gain - g0) * 256 / (int32_t)n;  /* Q8 per frame */
    /* PI occupancy correction, updated once per burst at its first block. A 93 ms occupancy filter
     * suppresses the 1 ms USB packet jitter. Q24 phase allows < 0.06 ppm rate steps; a 32-block slew
     * avoids abrupt ratio changes. */
    if (first) {
        uar.fill_q8 += (((int32_t)occ << 8) - uar.fill_q8) / 16;
        error = (uar.fill_q8 - ((int32_t)UAR_TARGET_FIRST << 8)) / 256;
        uar.integral = uac_rx_clamp(uar.integral + error * (int32_t)(n * 8u),
                                  -UAR_CORR_MAX * 2048, UAR_CORR_MAX * 2048);
        corr = error * 512 + uar.integral / 2048;
        if (corr > UAR_CORR_MAX || corr < -UAR_CORR_MAX) uar.limited++;
        uar.aim = uac_rx_clamp(corr, -UAR_CORR_MAX, UAR_CORR_MAX);
        uar.learned += (uar.aim - uar.learned) / 64;
    }
    uar.correction += (uar.aim - uar.correction) / 32;
    step = UAR_ONE + (uint32_t)uar.correction;
    if (wr - rd < uar.fill_min) uar.fill_min = wr - rd;
    if (wr - rd > uar.fill_max) uar.fill_max = wr - rd;
    for (i = 0; i < n; i++) {
        uint32_t a, b, c, k;
        int32_t f;
        if (wr - rd < 3u) break;
        a = uar_ring[rd & UAR_MASK];
        b = uar_ring[(rd + 1u) & UAR_MASK];
        c = uar_ring[(rd + 2u) & UAR_MASK];
        f = (int32_t)(phase >> 9);                 /* Q15 */
        {
            int32_t g = g0 + ((dg * (int32_t)i) >> 8);
            sl[i] = uac_rx_hermite((int16_t)(uint16_t)hist, (int16_t)(uint16_t)a,
                                   (int16_t)(uint16_t)b, (int16_t)(uint16_t)c, f) * g / 128;
            sr[i] = uac_rx_hermite((int16_t)(uint16_t)(hist >> 16), (int16_t)(uint16_t)(a >> 16),
                                   (int16_t)(uint16_t)(b >> 16), (int16_t)(uint16_t)(c >> 16), f) * g / 128;
        }
        phase += step;
        k = phase >> 24;                           /* 0..2: the frame before the new cursor was read above */
        if (k) {
            hist = uar_ring[(rd + k - 1u) & UAR_MASK];
            rd += k;
        }
        phase &= UAR_ONE - 1u;
    }
    UAC_RX_PRECOMMIT();
    flags = UAC_RX_LOCK();
    if (epoch != uar.generation || !uar.active) {
        uar.cancelled++;                          /* reset preempted interpolation */
    } else if (i != n) {
        uar.underruns++;
        uar.go = uar.phase = 0;
        uar_r = uar_w;                            /* drop tail; re-prime cleanly; back to the rate learned before */
        uar.fill_q8 = (int32_t)UAR_TARGET_FIRST << 8;       /* the gap (whose catch-up it would follow) */
        uar.integral = uar.learned * 2048;
        uar.aim = uar.correction = uar.learned;
        uar.ramp = uar.g_last = 0;
    } else {
        for (i = 0; i < n; i++) {
            uint32_t al = (uint32_t)(sl[i] < 0 ? -sl[i] : sl[i]), ar = (uint32_t)(sr[i] < 0 ? -sr[i] : sr[i]);
            if (al > peak) peak = al;
            if (ar > peak) peak = ar;
            l[i] = uac_rx_add(l[i], sl[i]);
            r[i] = uac_rx_add(r[i], sr[i]);
        }
        if (peak > 64u)
            uar_loud_ms = uar.now;                /* (the producer's fm1_ms, a word read) */
        if (peak > uar_peak)
            uar_peak = peak;
        uar.g_last = gain;
        uar.phase = phase;
        uar.hist = hist;
        UAC_RX_BARRIER();
        uar_r = rd;
    }
    UAC_RX_UNLOCK(flags);
}
#endif
