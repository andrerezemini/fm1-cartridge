/* SPDX-License-Identifier: GPL-3.0-only
 * Cartridge receiver tests: native simulation, never a hardware audio claim.
 * cc -O2 -Wall -Wextra tests/uac_rx_test.c -o build/host/uac_rx_test */
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint32_t test_icfg = 0x12345678u, lock_depth;
static uint32_t test_lock(void) { uint32_t s = test_icfg; test_icfg &= ~0x100u; lock_depth++; return s; }
static void test_unlock(uint32_t s) { assert(lock_depth); lock_depth--; test_icfg = s; }
static void precommit(void);
#define FELUCCA_UAC_RX 1
#ifndef UAR_RAMP
#define UAR_RAMP 0u                       /* the exact-value tests: the gain at once (tests/uac_rx_ramp_test.c: the ramp) */
#endif
#define UAC_RX_BARRIER() __asm__ volatile("" ::: "memory")
#define UAC_RX_LOCK() test_lock()
#define UAC_RX_UNLOCK(s) test_unlock(s)
#define UAC_RX_PRECOMMIT() precommit()
#include "../firmware/src/uac_rx.c"

static uint32_t inject;
static uint8_t packet[180];
static void precommit(void)
{
    uint32_t action = inject;
    inject = 0;
    assert(!lock_depth);                 /* no DSP inside the masked IRQ window */
    if (action == 1) uac_rx_stream(0);
    if (action == 2) { uac_rx_stream(0); uac_rx_stream(1); }
    if (action == 3) uac_rx_packet_error();
    if (action == 4) uac_rx_tick(uar.last_packet + UAR_TIMEOUT_MS);
    if (action == 5) uac_rx_push(packet, sizeof packet);
}
static void fresh(void)
{
    memset(&uar, 0, sizeof uar);
    memset(uar_ring, 0xA5, sizeof uar_ring);
    uar_w = uar_r = 0;
    uac_rx_gain = 128;
    inject = 0;
    uac_rx_stream(1);
}
static void samples(uint32_t n, int32_t l, int32_t r)
{
    uint32_t i;
    assert(n <= 45);
    for (i = 0; i < n; i++) {
        packet[4*i] = (uint8_t)l; packet[4*i+1] = (uint8_t)((uint32_t)l >> 8);
        packet[4*i+2] = (uint8_t)r; packet[4*i+3] = (uint8_t)((uint32_t)r >> 8);
    }
    uac_rx_push(packet, n * 4);
}
#define PRIME_PACKETS ((UAR_PRIME + 43u) / 44u)   /* 44-frame packets until the ring starts */
static void prime(int32_t l, int32_t r)
{
    uint32_t i;
    /* Consumer acknowledges reset before producer fills the ring. */
    int32_t zero[32] = {0}, zr[32] = {0};
    uac_rx_mix(zero, zr, 32);
    for (i = 0; i < PRIME_PACKETS; i++) samples(44, l, r);
}
static void test_packets(void)
{
    uint32_t i, before;
    int32_t l[32] = {0}, r[32] = {0};
    fresh();
    uac_rx_push(NULL, 0); assert(!uar.packets && !uar.malformed);
    uac_rx_push(NULL, 4); uac_rx_push(packet, 1); uac_rx_push(packet, 3);
    uac_rx_push(packet, 181); uac_rx_push(packet, 184);
    assert(uar.malformed == 5 && uar_w == 0);
    /* Accept legal short, zero and full packets; preserve endian/channel order. */
    samples(1, -32768, 32767); uac_rx_push(packet, 0);
    assert(uar.packets == 1 && uar_ring[0] == 0x7fff8000u);
    samples(45, 1234, -2345); assert(uar_w == 46);
    fresh(); prime(-32768, 32767);
    uac_rx_mix(l, r, 32);
    for (i = 0; i < 32; i++) assert(l[i] == -32768 && r[i] == 32767);
    /* Unaligned input is safe; maximum alternating difference times Q15 fraction
     * remains within signed 32 bits even at full-scale opposite-polarity PCM. */
    { uint8_t unaligned[181]; memcpy(unaligned+1, packet, 180); uac_rx_push(unaligned+1,180); }
    before = uar_w;
    while (uar_w - uar_r <= UAR_N - 45) samples(45, 1, -1);
    samples(45, 1, -1);
    assert(uar.overruns == 1 && uar_w - uar_r <= UAR_N && uar_w >= before);
    memset(l, 0, sizeof l); memset(r, 0, sizeof r);
    uac_rx_mix(l, r, 32);
    for (i = 0; i < 32; i++) assert(!l[i] && !r[i]);
    assert(uar_r == uar_w && !uar.go);
    puts("packets: endian, short/zero/malformed/unaligned, overflow and re-prime passed");
}
static void test_wrap_order(void)
{
    uint32_t i, j, seq = 0, last = 0, heard = 0;
    int32_t l[32], r[32];
    fresh();
    /* Monotonic uint32 cursors intentionally wrap too, independently of slots. */
    uar_w = uar_r = UINT32_MAX - 400u;
    uac_rx_stream(1);
    memset(l, 0, sizeof l); memset(r, 0, sizeof r); uac_rx_mix(l, r, 32);
    for (i = 0; i < 400; i++) {
        for (j = 0; j < 44; j++, seq++) {
            uint32_t s = seq + 1u;
            packet[4*j]=(uint8_t)s; packet[4*j+1]=(uint8_t)(s>>8);
            packet[4*j+2]=(uint8_t)s; packet[4*j+3]=(uint8_t)(s>>8);
        }
        uac_rx_push(packet, 176);
        while (uar_w - uar_r >= (uar.go ? 300u : UAR_PRIME)) {
            memset(l, 0, sizeof l); memset(r, 0, sizeof r); uac_rx_mix(l, r, 32);
            for (j = 0; j < 32; j++) {
                if (!heard && !j) last = (uint32_t)l[0];   /* (a start skips to the rate loop's aim: from there) */
                assert(l[j] == r[j] && l[j] >= (int32_t)last && l[j] <= (int32_t)last+3);
                last=(uint32_t)l[j]; heard++;
            }
        }
    }
    assert(heard > 16000 && uar_w < 20000 && !uar.overruns && !uar.underruns);
    puts("ring: frame/channel order, slot wrap and uint32 cursor wrap passed");
}
static void test_lifecycle(void)
{
    uint32_t action, i;
    int32_t l[32], r[32];
    for (action = 1; action <= 4; action++) {
        fresh(); prime(1234,-2345);
        for (i=0;i<32;i++) l[i]=17, r[i]=-23;
        inject = action; uac_rx_mix(l,r,32);
        if (action == 3) {
            /* A lost / corrupt packet is counted, never a reset: the block plays on. */
            for (i=0;i<32;i++) assert(l[i]==17+1234 && r[i]==-23-2345);
            assert(!uar.cancelled && uar.errors == 1 && uar.go);
            continue;
        }
        for (i=0;i<32;i++) assert(l[i]==17 && r[i]==-23);
        assert(uar.cancelled == 1);
        if (action == 1) uac_rx_stream(1);
        prime(-765,876);
        memset(l,0,sizeof l); memset(r,0,sizeof r); uac_rx_mix(l,r,32);
        for (i=0;i<32;i++) assert(l[i]==-765 && r[i]==876);
    }
    fresh(); prime(1234,-2345); inject = 5;
    memset(l,0,sizeof l); memset(r,0,sizeof r); uac_rx_mix(l,r,32);
    for (i=0;i<32;i++) assert(l[i]==1234 && r[i]==-2345);
    assert(!uar.cancelled && uar.packets == PRIME_PACKETS + 1u);
    /* Empty packets cannot keep old audio alive, including millisecond wrap. */
    fresh(); uac_rx_tick(UINT32_MAX-10u); prime(1234,-2345);
    uac_rx_tick(8); assert(uar.timeouts == 0);
    uac_rx_push(NULL,0); uac_rx_tick(9); assert(uar.timeouts == 1);
    memset(l,0,sizeof l); memset(r,0,sizeof r); uac_rx_mix(l,r,32);
    for (i=0;i<32;i++) assert(!l[i] && !r[i]);
    /* Ring exhaustion drops the entire incomplete block, then silence; a new
     * packet sequence must re-prime, never repeat the stale last sample. */
    fresh(); prime(100,-100);
    for(i=0;i<20;i++) { memset(l,0,sizeof l); memset(r,0,sizeof r); uac_rx_mix(l,r,32); }
    assert(uar.underruns == 1 && !uar.go);
    for(i=0;i<32;i++) assert(!l[i] && !r[i]);
    assert(test_icfg == 0x12345678u && !lock_depth);
    puts("lifecycle: nested reset/alt/error/timeout, packet preemption, loss, re-connect and IRQ restore passed");
}
static void test_gain_bounds(void)
{
    uint32_t g,i;
    int32_t l[32],r[32];
    for(g=0;g<=128;g+=64) {
        fresh(); prime(32767,-32768); uac_rx_gain=(uint8_t)g;
        for(i=0;i<32;i++) l[i]=123,r[i]=-321;
        uac_rx_mix(l,r,32);
        for(i=0;i<32;i++) assert(l[i]==123+32767*(int32_t)g/128 && r[i]==-321-32768*(int32_t)g/128);
    }
    fresh(); prime(32767,-32768); uac_rx_gain=255;
    for(i=0;i<32;i++) l[i]=INT32_MAX-4,r[i]=INT32_MIN+4;
    uac_rx_mix(l,r,32);
    for(i=0;i<32;i++) assert(l[i]==INT32_MAX && r[i]==INT32_MIN);
    /* Full-scale transitions exercise the greatest signed product in the ASRC. */
    fresh(); prime(32767,-32768);
    for(i=0;i<UAR_N;i++) uar_ring[i]=(i&1)?0x80007fffu:0x7fff8000u;
    uar.phase=UAR_ONE-1;
    memset(l,0,sizeof l); memset(r,0,sizeof r); uac_rx_mix(l,r,32);
    for(i=0;i<32;i++) assert(l[i]>=-32768 && l[i]<=32767 && r[i]>=-32768 && r[i]<=32767);
    puts("gain: mute/half/unity, invalid gain clamp, interpolation and addition overflow bounds passed");
}
static void test_drift(int ppm, uint32_t seconds)
{
    uint64_t pkt=0, render=0, rem=0, end=(uint64_t)seconds*1000000000ull;
    uint64_t period_num=256ull*1000000000ull*1000000ull;
    uint64_t rate=44100ull*(uint64_t)(1000000+ppm);
    uint32_t packet_frames=0, t, j, blocks=0, steady_under=0, steady_over=0;
    int32_t l[32],r[32];
    fresh();
    while(pkt<end || render<end) {
        if(pkt<=render && pkt<end) {
            uac_rx_tick((uint32_t)(pkt/1000000ull));
            packet_frames += 44100;
            t=packet_frames/1000; packet_frames%=1000;
            samples(t,16000,-8000); pkt+=1000000ull;
        } else if(render<end) {
            for(j=0;j<8;j++) {
                if(!j) uac_rx_burst();              /* the burst begins: the rate loop's turn (audio.c) */
                memset(l,0,sizeof l); memset(r,0,sizeof r); uac_rx_mix(l,r,32);
                if(uar.go) for(t=0;t<32;t++) assert(l[t]==16000 && r[t]==-8000);
            }
            blocks++;
            rem+=period_num; render+=rem/rate; rem%=rate;
            if(render<1000000000ull) steady_under=uar.underruns,steady_over=uar.overruns;
        } else break;
    }
    printf("clock %+d ppm, %u s, %u DAC bursts: fill %u..%u, correction %.2f ppm, under/over %u/%u, limited %u\n",
           ppm,seconds,blocks,uar.fill_min,uar.fill_max,1e6*(double)uar.correction/UAR_ONE,uar.underruns,uar.overruns,uar.limited);
    if(ppm>=-1000 && ppm<=1000) {
        assert(uar.underruns==steady_under && uar.overruns==steady_over);
        assert(uar.fill_min>32 && uar.fill_max<UAR_N-45);
    } else {
        /* A clock outside the declared corrector range must stay bounded and
         * expose saturation, with clean re-prime rather than stale repetition. */
        assert((uar.limited>0 || uar.underruns>0) && uar_w-uar_r<=UAR_N);
        if(ppm<0) assert(uar.limited>0 && uar.overruns>0);
    }
    assert(uar.correction<=UAR_CORR_MAX && uar.correction>=-UAR_CORR_MAX);
    if(ppm>0 && ppm<=1000) assert(uar.correction<0);
    if(ppm<0 && ppm>=-1000) assert(uar.correction>0);
}
/* The FM-1's timing around the receiver, closed loop: a DAC burst of eight 32-frame blocks every 256 frames,
 * rendered in a share of the period that may vary burst to burst (audio.c marks the burst at the interrupt's
 * entry, each block's mix comes at the end of its render), the host's 1 ms packets at any phase against it
 * (TIMER5 pushes them between the blocks). Every start must begin on a burst's first block, at the rate
 * loop's aim. Returns the largest rate
 * correction in the first 2 s (the start's pitch excursion) and, in *pp, its peak-to-peak range from 5 s
 * on (the steady wobble); both in cents. */
#define CENTS(c) ((double)(c) * 1e6 / UAR_ONE / 577.79)    /* (1200 log2(1 + x) for x of a few 1e-4) */
static double loop_run(uint32_t phase_us, uint32_t share_pct, uint32_t jitter_pct, uint32_t seconds, double *pp)
{
    const uint64_t period = 256ull * 1000000000ull / 44100ull, end = (uint64_t)seconds * 1000000000ull;
    uint64_t pkt = (uint64_t)phase_us * 1000ull, burst, started = 0;
    uint32_t acc = 0, j, rng = phase_us * 2654435761u + share_pct;
    int32_t l[32], r[32], cmax = 0, lo = INT32_MAX, hi = INT32_MIN;
    fresh();
    for (burst = 0; burst < end; burst += period) {
        uint64_t render;
        rng = rng * 1103515245u + 12345u;
        render = period * (share_pct + (jitter_pct ? (rng >> 16) % (jitter_pct + 1u) : 0u)) / 100u;
        for (j = 0; j <= 8; j++) {
            uint64_t at = burst + render * j / 8u;    /* (j = 0: the interrupt's entry) */
            uint32_t was = uar.go;
            while (pkt <= at) {
                uac_rx_tick((uint32_t)(pkt / 1000000ull));
                acc += 44100; samples(acc / 1000u, 1000, -1000); acc %= 1000u;
                pkt += 1000000ull;
            }
            if (!j) {
                uac_rx_burst();
                continue;
            }
            memset(l, 0, sizeof l); memset(r, 0, sizeof r); uac_rx_mix(l, r, 32);
            if (!was && uar.go) {
                assert(j == 1 && !started && uar_mark - uar_r == UAR_TARGET_FIRST - 32u);
                started = at;
            }
        }
        if (started && burst - started < 2000000000ull) {
            if (uar.correction > cmax) cmax = uar.correction;
            if (-uar.correction > cmax) cmax = -uar.correction;
        } else if (started && burst - started >= 5000000000ull) {
            if (uar.correction < lo) lo = uar.correction;
            if (uar.correction > hi) hi = uar.correction;
        }
    }
    assert(started && !uar.underruns && !uar.overruns);
    if (pp) *pp = CENTS(hi - lo);
    return CENTS(cmax);
}
static void test_start_phases(void)
{
    static const uint32_t share[] = {10, 30, 50, 80};
    double worst = 0, pp = 0, e;
    uint32_t k, s;
    for (s = 0; s < 4u; s++) {
        double w = 0;
        for (k = 0; k < 24u; k++) {
            e = loop_run(k * 242u, share[s], 0, 3, 0);
            if (e > w) w = e;
        }
        printf("closed loop, render %u%% of the period: start excursion at most %.2f cent\n", share[s], w);
        if (w > worst) worst = w;
    }
    e = loop_run(0, 14, 51, 20, &pp);                 /* the render 0.8 .. 3.8 ms a burst */
    printf("closed loop: start on a burst's first block at every packet phase and render share, pitch excursion "
           "at most %.2f cent; render 0.8-3.8 ms: start %.2f cent, steady wobble %.2f cent peak to peak\n", worst, e, pp);
    assert(worst < 1.2 && e < 0.5 && pp < 0.3);
}
int main(void)
{
    test_packets(); test_wrap_order(); test_lifecycle(); test_gain_bounds();
    test_drift(-1000,120); test_drift(0,120); test_drift(1000,120);
    test_drift(-10000,20); test_drift(10000,20);
    test_start_phases();
    puts("uac_rx: all native receiver tests passed (hardware untested)");
    return 0;
}
