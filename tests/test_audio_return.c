/* SPDX-License-Identifier: GPL-3.0-only */
#include "fm1/audio_return.h"
#include <assert.h>
#include <stdio.h>

static fm1_audio_return_t receiver;
static uint8_t packet[196];

static int16_t frame_value(uint32_t frame)
{
    uint32_t bits = frame & 65535u;
    return (int16_t)(bits <= 32767u ? (int32_t)bits : (int32_t)bits - 65536);
}

static void pcm_order(void)
{
    /* Distinct L/R ramps include both signed16 limits. Exact-rate bypass must
     * preserve every frame across ring wraps and 44/45-frame packet boundaries. */
    fm1_return_init(&receiver, false);
    uint32_t remainder = 0, sent = 0, consumed = 0;
    int16_t out[100];
    for (unsigned ms = 0; ms < 10000; ++ms) {
        size_t frames = fm1_packet_frames(44100, &remainder);
        for (size_t i = 0; i < frames; ++i) {
            int16_t left = frame_value(sent + (uint32_t)i);
            int16_t right = (int16_t)(-1 - (int32_t)left);
            uint16_t l = (uint16_t)left, r = (uint16_t)right;
            packet[4*i] = (uint8_t)l; packet[4*i+1] = (uint8_t)(l >> 8);
            packet[4*i+2] = (uint8_t)r; packet[4*i+3] = (uint8_t)(r >> 8);
        }
        assert(fm1_return_push(&receiver, packet, frames * 4));
        sent += (uint32_t)frames;
        bool primed = receiver.primed || receiver.count >= FM1_RETURN_TARGET;
        fm1_return_render(&receiver, out, frames);
        for (size_t i = 0; i < frames; ++i) {
            if (!primed) {
                assert(out[2*i] == 0 && out[2*i+1] == 0);
                continue;
            }
            int16_t left = frame_value(consumed++);
            int16_t right = (int16_t)(-1 - (int32_t)left);
            assert(out[2*i] == left && out[2*i+1] == right);
        }
        assert(receiver.count <= FM1_RETURN_CAPACITY);
        assert(receiver.phase >= 0.0 && receiver.phase < 1.0);
    }
    assert(consumed > 400000);
    assert(receiver.stats.underruns == 0 && receiver.stats.overruns == 0);
}

static fm1_return_stats_t run(double ppm, bool adaptive)
{
    fm1_return_init(&receiver,adaptive);
    uint32_t phase=0;
    double consumer=0;
    int16_t out[100];
    for (unsigned ms=0;ms<120000;ms++) {
        size_t n=fm1_packet_frames(44100,&phase);
        (void)fm1_return_push(&receiver,packet,n*4);
        consumer += 44.1 * (1.0 + ppm/1000000.0);
        size_t frames=(size_t)consumer;
        consumer -= frames;
        fm1_return_render(&receiver,out,frames);
    }
    return receiver.stats;
}

int main(void)
{
    pcm_order();
    uint32_t rem=0;
    unsigned total=0, long_packets=0;
    for (unsigned i=0;i<1000;i++) {
        size_t n=fm1_packet_frames(44100,&rem);
        assert(n==44 || n==45); total+=(unsigned)n; long_packets+=n==45;
    }
    assert(total==44100 && long_packets==100 && rem==0);
    for (int ppm=-1000;ppm<=1000;ppm+=500) {
        fm1_return_stats_t s=run(ppm,true);
        assert(s.underruns==0 && s.overruns==0);
        assert(s.fill_min>100 && s.fill_max<500);
    }
    fm1_return_stats_t bad=run(1000,false);
    assert(bad.underruns>0); /* Demonstrates why buffering alone is insufficient. */
    bad=run(-1000,false); assert(bad.overruns>0);
    fm1_return_init(&receiver,true);
    assert(!fm1_return_push(&receiver,packet,195));
    assert(!fm1_return_push(&receiver,packet,200));
    assert(!fm1_return_push(&receiver,NULL,4));
    assert(fm1_return_push(&receiver,NULL,0));
    assert(receiver.stats.malformed_packets==3);
    for (int i=0;i<6;i++) assert(fm1_return_push(&receiver,packet,180));
    int16_t out[2000];
    fm1_return_render(&receiver,out,1000);
    assert(receiver.stats.underruns==1 && !receiver.primed);
    fm1_return_reset(&receiver);
    assert(!receiver.count && !receiver.primed && receiver.stats.resets==1);
    for (int i=0;i<6;i++) assert(fm1_return_push(&receiver,packet,180));
    fm1_return_render(&receiver,out,44);
    assert(receiver.primed && receiver.stats.rendered_frames>44);
    for (int i=0;i<40;i++) (void)fm1_return_push(&receiver,packet,196);
    assert(receiver.count<=FM1_RETURN_CAPACITY && receiver.stats.overruns>0);
    puts("audio return: PCM order/stereo/signed16, drift +/-1000 ppm, loss, overflow, restart ok");
    return 0;
}
