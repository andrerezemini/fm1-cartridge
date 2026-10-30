/* SPDX-License-Identifier: GPL-3.0-only */
#include "fm1/battery.h"
#include "fm1/cartridge_wire.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    fm1_battery_t b;
    fm1_battery_init(&b, 3700, 3550);
    assert(b.state == CART_POWER_NONE && !b.mv);
    fm1_battery_reading(&b, 0);                       /* a failed read is no reading */
    assert(b.state == CART_POWER_NONE && fm1_battery_percent(b.mv) == CART_BATTERY_NONE);
    fm1_battery_reading(&b, 4100);
    assert(b.mv == 4100 && b.state == CART_POWER_OK);
    /* the level follows a quarter of each reading: one low reading does not make the cell LOW */
    fm1_battery_reading(&b, 3500);
    assert(b.mv == 3950 && b.state == CART_POWER_OK);
    for (unsigned i = 0; i < 8; ++i) fm1_battery_reading(&b, 3650);
    assert(b.mv < 3700 && b.mv > 3550 && b.state == CART_POWER_LOW);
    /* LOW turns back to OK only past the threshold plus the margin */
    for (unsigned i = 0; i < 3 && b.mv < 3700; ++i) fm1_battery_reading(&b, 3720);
    assert(b.mv >= 3700 && b.mv < 3700 + FM1_BATTERY_HYSTERESIS_MV && b.state == CART_POWER_LOW);
    for (unsigned i = 0; i < 20; ++i) fm1_battery_reading(&b, 3900);
    assert(b.state == CART_POWER_OK);
    /* the cutoff needs several readings in a row under it, and then stays */
    fm1_battery_init(&b, 3700, 3550);
    fm1_battery_reading(&b, 3500);
    for (unsigned i = 1; i < FM1_BATTERY_CUTOFF_READINGS; ++i) {
        assert(b.state == CART_POWER_LOW);
        fm1_battery_reading(&b, 3500);
    }
    assert(b.state == CART_POWER_CUTOFF);
    for (unsigned i = 0; i < 20; ++i) fm1_battery_reading(&b, 4000);
    assert(b.state == CART_POWER_CUTOFF && b.mv > 3900);
    fm1_battery_init(&b, 3700, 3550);
    fm1_battery_reading(&b, 3540);
    fm1_battery_reading(&b, 3540);
    fm1_battery_reading(&b, 3800);                    /* back over it: the count starts again */
    assert(b.state == CART_POWER_LOW && !b.below);

    /* the percentage: the ends, monotonic in between */
    assert(fm1_battery_percent(4300) == 100 && fm1_battery_percent(4200) == 100);
    assert(fm1_battery_percent(3300) == 0 && fm1_battery_percent(3000) == 0);
    assert(fm1_battery_percent(3800) == 50);
    for (uint32_t mv = 3000, last = 0; mv <= 4300; mv += 5) {
        uint32_t p = fm1_battery_percent(mv);
        assert(p <= 100 && p >= last);
        last = p;
    }

    /* the fade: full level at the start, silence at the end, and silence stays */
    static int16_t pcm[2 * 4800];
    for (unsigned i = 0; i < 2 * 4800; ++i) pcm[i] = (int16_t)(i & 1 ? -20000 : 20000);
    uint32_t gain = FM1_FADE_UNITY, step = FM1_FADE_UNITY / 4410u;
    fm1_fade_out(pcm, 4800, &gain, step);
    assert(gain == 0 && pcm[0] == 20000 && pcm[1] == -20000);
    for (unsigned i = 1; i < 4800; ++i) {
        assert(pcm[2 * i] <= pcm[2 * i - 2] && pcm[2 * i] >= 0);
        assert(pcm[2 * i + 1] <= -pcm[2 * i] && pcm[2 * i + 1] >= -pcm[2 * i] - 1);   /* (the shift rounds down) */
    }
    assert(pcm[2 * 4799] == 0 && pcm[2 * 4700] == 0);
    for (unsigned i = 0; i < 2 * 64; ++i) pcm[i] = 30000;
    fm1_fade_out(pcm, 64, &gain, step);
    for (unsigned i = 0; i < 2 * 64; ++i) assert(pcm[i] == 0);
    printf("battery: filter, LOW with its margin, the latched cutoff, the percentage and the fade passed\n");
    return 0;
}
