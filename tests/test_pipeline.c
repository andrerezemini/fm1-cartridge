/* SPDX-License-Identifier: GPL-3.0-only */
/* A model of the whole audio return, ESP32 render to FM-1 mixer: the audio task renders AUDIO_FRAMES and
 * blocks on the UAC ring (CONFIG_FM1_UAC_BUFFER_BYTES), the driver keeps UAC_NUM_ISOC_URBS URBs of
 * UAC_NUM_PACKETS_PER_URB one-millisecond packets (44/45 frames) on the bus, refilling each from the ring
 * up to 2 ms after it completes (the host client task) or parking it when the ring is short
 * (usb_host_uac 1.5.0), and Sloop's own receiver (sloop-fm1/firmware/src/uac_rx.c) takes the packets and
 * feeds eight 32-frame blocks a DAC burst, its clock 300 ppm off the bus. The
 * buffer sizes come from the firmware's sources (CMakeLists.txt reads them). A model of the buffering and
 * its timing, not a measurement: the render cost and its stalls are inputs here, measured on the bench. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define FELUCCA_UAC_RX 1
#define UAC_RX_BARRIER() __asm__ volatile("" ::: "memory")
#define UAC_RX_LOCK() 0u
#define UAC_RX_UNLOCK(s) ((void)(s))
#include "../sloop-fm1/firmware/src/uac_rx.c"

typedef struct { uint32_t frames, ring_bytes, urbs, packets; } config_t;
typedef struct { double latency_mean_ms, latency_max_ms; uint32_t gaps, underruns; } result_t;

#ifndef PIPE_STALL_MS
#define PIPE_STALL_MS 15u                               /* the margin the documentation states */
#endif
#define URB_MAX 8u
#define SCHED_MAX 64u
_Static_assert(PIPE_URBS <= URB_MAX && PIPE_PACKETS <= URB_MAX && PIPE_URBS * PIPE_PACKETS <= SCHED_MAX,
               "the model's arrays are smaller than the firmware's URB settings");
static struct {
    config_t c;
    uint32_t ring, accum, parked, urb_left[URB_MAX];
    uint64_t done_at[URB_MAX];                          /* an URB completed, its refill still to run (0: none) */
    uint32_t sched[SCHED_MAX], head, n;                 /* the packets on the bus, in order: frames | urb << 8 */
} m;

/* stream_tx_xfer_submit: the URB's packets from the ring, else it waits (the accumulator restored) */
static int urb_submit(uint32_t u)
{
    uint32_t a = m.accum, bytes = 0, frames[URB_MAX], k;
    for (k = 0; k < m.c.packets; ++k) {
        a += 100;                                       /* 44.1 frames a ms: one 45-frame packet in ten */
        frames[k] = a >= 1000 ? 45u : 44u;
        if (a >= 1000) a -= 1000;
        bytes += frames[k] * 4u;
    }
    if (m.ring < bytes) return 0;
    m.ring -= bytes;
    m.accum = a;
    m.urb_left[u] = m.c.packets;
    for (k = 0; k < m.c.packets; ++k) m.sched[(m.head + m.n++) % SCHED_MAX] = frames[k] | u << 8;
    return 1;
}

/* stall_ms: one render that long at 10 s; otherwise each render costs cost_us plus up to jitter_us. An URB's
 * refill runs up to 2 ms after its last packet (the USB host client task's latency: stream_tx_xfer_done),
 * and the FM-1's DAC runs ppm off the ESP32's bus (its rate loop follows) */
static result_t run(config_t c, uint32_t stall_ms, uint32_t cost_us, uint32_t jitter_us, double seconds, double ppm)
{
    static uint8_t pkt[180];
    result_t res = {0};
    uint64_t t, end = (uint64_t)(seconds * 1e6), next_frame = 1000, render_done = 0;
    double next_burst = 0, burst_us = 256.0 / 44100.0 * 1e6 * (1.0 + ppm * 1e-6);
    uint32_t rng = 1, lat_n = 0, written = 0, k, u;
    int rendering = 0, pending = 0, stalled = 0;
    double lat_sum = 0;
    int32_t l[32], r[32];
    memset(&m, 0, sizeof m);
    m.c = c;
    m.parked = (1u << c.urbs) - 1u;
    memset(&uar, 0, sizeof uar);
    uar_w = uar_r = 0;
    uac_rx_gain = 128;
    uac_rx_stream(1);
    for (t = 0; t < end; t += 10) {
        /* the audio task: render, then write once the ring has room for the whole block */
        if (!rendering && !pending) {
            rng = rng * 1103515245u + 12345u;
            render_done = t + cost_us + (jitter_us ? (rng >> 16) % (jitter_us + 1u) : 0);
            if (stall_ms && !stalled && t >= 10000000u) render_done = t + stall_ms * 1000u, stalled = 1;
            rendering = 1;
        }
        if (rendering && t >= render_done) rendering = 0, pending = 1;
        if (pending && c.ring_bytes - m.ring >= c.frames * 4u) {
            m.ring += c.frames * 4u;
            pending = 0;
            written = 1;
            for (u = 0; u < c.urbs; ++u)               /* uac_host_device_write: a parked URB goes out */
                if (((m.parked >> u) & 1u) && urb_submit(u)) m.parked &= ~(1u << u);
        }
        for (u = 0; u < c.urbs; ++u)                   /* a completed URB's refill, from the client task */
            if (m.done_at[u] && t >= m.done_at[u]) {
                m.done_at[u] = 0;
                if (!urb_submit(u)) m.parked |= 1u << u;
            }
        /* the bus: a packet each millisecond; an URB done is refilled from the ring a little later, or parked */
        if (t >= next_frame) {
            next_frame += 1000;
            uac_rx_tick((uint32_t)(t / 1000));
            if (m.n) {
                uint32_t e = m.sched[m.head], f = e & 255u;
                u = e >> 8;
                m.head = (m.head + 1) % SCHED_MAX; m.n--;
                memset(pkt, 0x10, f * 4);
                uac_rx_push(pkt, f * 4);
                if (!--m.urb_left[u]) {
                    rng = rng * 1103515245u + 12345u;
                    m.done_at[u] = t + 10u + (rng >> 16) % 2000u;
                }
            } else if (written && t > 50000) {
                res.gaps++;                             /* a frame with no packet: the FM-1 hears a gap */
            }
        }
        /* the FM-1: a DAC burst every 256 frames of its own clock, eight blocks */
        if ((double)t >= next_burst) {
            uint32_t occ;
            next_burst += burst_us;
            uac_rx_burst();
            occ = uar_w - uar_r;                        /* (what waits for the mixer as the burst begins) */
            for (k = 0; k < 8; ++k) {
                memset(l, 0, sizeof l); memset(r, 0, sizeof r);
                uac_rx_mix(l, r, 32);
            }
            if (uar.go && t > 2000000) {                /* what lies between the render and the FM-1's mixer */
                uint32_t inflight = 0;
                for (k = 0; k < m.n; ++k) inflight += m.sched[(m.head + k) % SCHED_MAX] & 255u;
                /* frames rendered and not yet mixed, the burst's mean (the receiver's aim at its start less
                 * half the burst it plays): their age when the mixer takes them */
                double ms = (m.ring / 4.0 + inflight + (pending ? c.frames : 0u) + (double)occ - 112.0) / 44.1;
                lat_sum += ms; lat_n++;
                if (ms > res.latency_max_ms) res.latency_max_ms = ms;
            }
        }
    }
    res.latency_mean_ms = lat_n ? lat_sum / lat_n : 0;
    res.underruns = uar.underruns;
    return res;
}

int main(void)
{
    const config_t now = { PIPE_FRAMES, PIPE_RING_BYTES, PIPE_URBS, PIPE_PACKETS };
    const config_t before = { 128, 4096, 3, 3 };          /* 128-frame renders, a 4 KB ring, 3 URBs of 3 packets */
    /* a render at about half the core: 64 frames are 1451 us; the DAC 300 ppm off the bus */
    result_t a = run(now, 0, now.frames * 6u, now.frames * 4u, 30, 300), b = run(before, 0, 768, 512, 30, 300);
    printf("pipeline %u frames, %u B ring, %u x %u packets: latency %.1f ms (max %.1f), gaps %u, underruns %u\n",
           now.frames, now.ring_bytes, now.urbs, now.packets, a.latency_mean_ms, a.latency_max_ms, a.gaps, a.underruns);
    printf("pipeline %u frames, %u B ring, %u x %u packets: latency %.1f ms (max %.1f), gaps %u, underruns %u\n",
           before.frames, before.ring_bytes, before.urbs, before.packets, b.latency_mean_ms, b.latency_max_ms, b.gaps,
           b.underruns);
    assert(!a.gaps && !a.underruns && !b.gaps && !b.underruns);
    assert(a.latency_mean_ms + 10.0 < b.latency_mean_ms);
    /* the longest single render the buffers ride through without a gap, with the DAC either side of the bus */
    uint32_t ok = 40;
    for (int side = -1; side <= 1; side += 2) {
        uint32_t ms;
        for (ms = 1; ms <= 40; ++ms) {
            result_t s = run(now, ms, now.frames * 6u, now.frames * 4u, 12, 300.0 * side);
            if (s.gaps || s.underruns) break;
        }
        if (ms - 1 < ok) ok = ms - 1;
    }
    printf("pipeline: one render of up to %u ms passes without a gap (the others at half the core)\n", ok);
    assert(ok >= PIPE_STALL_MS);
    return 0;
}
