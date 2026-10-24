/* SPDX-License-Identifier: GPL-3.0-only
 * The receiver's start ramp and gain glide: native simulation, never a hardware audio claim
 * (tests/uac_rx_test.c checks the exact values with UAR_RAMP 0). */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define FELUCCA_UAC_RX 1
#define UAC_RX_BARRIER() __asm__ volatile("" ::: "memory")
#define UAC_RX_LOCK() 0u
#define UAC_RX_UNLOCK(s) ((void)(s))
#include "../firmware/src/uac_rx.c"

static uint8_t packet[180];
static void samples(uint32_t n, int32_t l, int32_t r)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        packet[4*i] = (uint8_t)l; packet[4*i+1] = (uint8_t)((uint32_t)l >> 8);
        packet[4*i+2] = (uint8_t)r; packet[4*i+3] = (uint8_t)((uint32_t)r >> 8);
    }
    uac_rx_push(packet, n * 4);
}
static void block(int32_t *l, int32_t *r)
{
    memset(l, 0, 32 * sizeof *l); memset(r, 0, 32 * sizeof *r);
    uac_rx_mix(l, r, 32);
}
/* every frame between from and to (inclusive), moving one way, the channels opposite */
static void glides(const int32_t *l, const int32_t *r, int32_t from, int32_t to)
{
    uint32_t i;
    for (i = 0; i < 32; i++) {
        assert(r[i] == -l[i]);
        assert(from <= to ? l[i] >= from && l[i] <= to : l[i] <= from && l[i] >= to);
        if (i) assert(from <= to ? l[i] >= l[i - 1] : l[i] <= l[i - 1]);
    }
}
int main(void)
{
    int32_t l[32], r[32];
    uint32_t i, b;
    memset(&uar, 0, sizeof uar);
    uar_w = uar_r = 0;
    uac_rx_gain = 128;
    uac_rx_stream(1);
    block(l, r);                                     /* the consumer acknowledges the stream */
    for (i = 0; i < 20; i++) samples(44, 16000, -16000);
    /* a start fades in over UAR_RAMP blocks, rising frame by frame from silence */
    block(l, r);
    assert(l[0] == 0 && l[31] > 0 && l[31] < 16000 / (int32_t)UAR_RAMP);
    glides(l, r, 0, 16000 / (int32_t)UAR_RAMP);
    for (b = 1; b < UAR_RAMP; b++) {
        int32_t from = l[31];
        block(l, r);
        glides(l, r, from, 16000 * (int32_t)(b + 1) / (int32_t)UAR_RAMP);
    }
    block(l, r);
    for (i = 0; i < 32; i++) assert(l[i] == 16000 && r[i] == -16000);
    puts("ramp: a start fades in over 5.8 ms, frame by frame, then plays at its gain");
    /* USB RETURN changes glide over one block, then hold */
    uac_rx_gain = 64; block(l, r);
    glides(l, r, 16000, 8000);
    assert(l[0] == 16000 && l[31] > 8000);
    block(l, r);
    for (i = 0; i < 32; i++) assert(l[i] == 8000);
    /* the cartridge track's LEVEL (Q12) on top of it */
    uar_track_q12 = 2048; block(l, r);
    glides(l, r, 8000, 4000);
    block(l, r);
    for (i = 0; i < 32; i++) assert(l[i] == 4000);
    assert(uar_peak >= 4000 && uar_peak <= 16000);
    puts("glide: USB RETURN and the track's LEVEL move over one block, no zipper step");
    /* run dry: an underrun re-primes, and the next start fades in again */
    for (i = 0; i < 40 && !uar.underruns; i++) block(l, r);
    assert(uar.underruns == 1 && !uar.go);
    for (i = 0; i < 20; i++) samples(44, 16000, -16000);
    block(l, r);
    assert(l[0] == 0 && l[31] < 4000 / (int32_t)UAR_RAMP + 1);
    puts("re-prime: the stream fades in again after an underrun");
    return 0;
}
