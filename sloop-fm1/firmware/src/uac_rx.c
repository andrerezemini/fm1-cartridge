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
#define UAR_PRIME 512u
#define UAR_TIMEOUT_MS 20u
#define UAR_ONE 16777216u                  /* Q24 input frames per output frame */
#define UAR_CORR_MAX 83886                /* +/- 5000 ppm, rounded down */
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
    uint32_t epoch, phase, go, hist;              /* consumer only; hist: the frame before uar_r */
    int32_t fill_q8, integral, correction;
    uint32_t fill_min, fill_max, limited;         /* consumer diagnostics */
} uar;
static volatile uint8_t uac_rx_gain = 128u;

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
 * this keeps it under 0.5 dB. The 64-bit products never overflow, even on
 * full-scale steps (the master limiter follows the mix; the mix is int32). */
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
    int32_t sl[UAR_BLOCK], sr[UAR_BLOCK], error, corr;
    uint32_t flags, epoch, rd, wr, phase, step, i, gain, hist;
    if (!n || n > UAR_BLOCK)
        return;
    flags = UAC_RX_LOCK();
    epoch = uar.generation;
    if (uar.epoch != epoch) {
        uar_r = uar.floor;               /* only the consumer writes its cursor */
        uar.epoch = epoch;
        uar.go = uar.phase = 0;
        uar.fill_q8 = (int32_t)UAR_TARGET << 8;
        uar.integral = uar.correction = 0;
        uar.reprimes++;
    }
    rd = uar_r;
    wr = uar_w;
    UAC_RX_BARRIER();
    if (!uar.active || (!uar.go && wr - rd < UAR_PRIME)) {
        UAC_RX_UNLOCK(flags);
        return;
    }
    if (!uar.go) {
        uar.go = 1;
        uar.fill_min = wr - rd;
        uar.fill_max = wr - rd;
        uar.hist = uar_ring[rd & UAR_MASK];       /* x[-1] = x[0]: a constant stream starts flat */
    }
    phase = uar.phase;
    hist = uar.hist;
    gain = uac_rx_gain;
    UAC_RX_UNLOCK(flags);
    if (gain > 128u) gain = 128u;
    /* PI occupancy correction, updated once per block. 93 ms occupancy filter
     * suppresses the DAC's 256-frame burst / 1 ms USB packet sawtooth. Q24 phase
     * allows < 0.06 ppm rate steps; a 32-block slew avoids abrupt ratio changes. */
    uar.fill_q8 += (((int32_t)(wr - rd) << 8) - uar.fill_q8) / 128;
    error = (uar.fill_q8 - ((int32_t)UAR_TARGET << 8)) / 256;
    uar.integral = uac_rx_clamp(uar.integral + error * (int32_t)n,
                              -UAR_CORR_MAX * 2048, UAR_CORR_MAX * 2048);
    corr = error * 512 + uar.integral / 2048;
    if (corr > UAR_CORR_MAX || corr < -UAR_CORR_MAX) uar.limited++;
    corr = uac_rx_clamp(corr, -UAR_CORR_MAX, UAR_CORR_MAX);
    uar.correction += (corr - uar.correction) / 32;
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
        sl[i] = uac_rx_hermite((int16_t)(uint16_t)hist, (int16_t)(uint16_t)a,
                               (int16_t)(uint16_t)b, (int16_t)(uint16_t)c, f) * (int32_t)gain / 128;
        sr[i] = uac_rx_hermite((int16_t)(uint16_t)(hist >> 16), (int16_t)(uint16_t)(a >> 16),
                               (int16_t)(uint16_t)(b >> 16), (int16_t)(uint16_t)(c >> 16), f) * (int32_t)gain / 128;
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
        uar_r = uar_w;                            /* drop tail; re-prime cleanly */
        uar.fill_q8 = (int32_t)UAR_TARGET << 8;
        uar.integral = uar.correction = 0;
    } else {
        for (i = 0; i < n; i++) {
            l[i] = uac_rx_add(l[i], sl[i]);
            r[i] = uac_rx_add(r[i], sr[i]);
        }
        uar.phase = phase;
        uar.hist = hist;
        UAC_RX_BARRIER();
        uar_r = rd;
    }
    UAC_RX_UNLOCK(flags);
}
#endif
