/* SPDX-License-Identifier: GPL-3.0-only */
#include "fm1/battery.h"
#include "fm1/cartridge_wire.h"
#include <string.h>

void fm1_battery_init(fm1_battery_t *b, uint32_t low_mv, uint32_t cutoff_mv)
{
    memset(b, 0, sizeof *b);
    b->low_mv = low_mv; b->cutoff_mv = cutoff_mv;
    b->state = CART_POWER_NONE;
}

void fm1_battery_reading(fm1_battery_t *b, uint32_t mv)
{
    if (!mv) return;
    /* a quarter of each new reading: the ADC's noise and the load's steps settle over a few seconds */
    b->mv = b->mv ? (b->mv * 3u + mv + 2u) / 4u : mv;
    if (b->state == CART_POWER_CUTOFF) return;
    b->below = b->mv < b->cutoff_mv ? (uint8_t)(b->below + 1u) : 0u;
    if (b->below >= FM1_BATTERY_CUTOFF_READINGS) b->state = CART_POWER_CUTOFF;
    else if (b->mv < b->low_mv) b->state = CART_POWER_LOW;
    else if (b->state != CART_POWER_LOW || b->mv >= b->low_mv + FM1_BATTERY_HYSTERESIS_MV) b->state = CART_POWER_OK;
}

uint8_t fm1_battery_percent(uint32_t mv)
{
    /* a single-cell LiPo's open-circuit curve, mV against %, from full to empty */
    static const uint16_t curve[][2] = {
        {4200, 100}, {4100, 90}, {4000, 78}, {3900, 64}, {3800, 50}, {3750, 40},
        {3700, 30}, {3650, 20}, {3600, 12}, {3500, 5}, {3300, 0},
    };
    if (!mv) return CART_BATTERY_NONE;
    if (mv >= curve[0][0]) return 100;
    for (unsigned i = 1; i < sizeof curve / sizeof curve[0]; ++i)
        if (mv >= curve[i][0]) {
            uint32_t hi = curve[i - 1][0], lo = curve[i][0], phi = curve[i - 1][1], plo = curve[i][1];
            return (uint8_t)(plo + ((mv - lo) * (phi - plo) + (hi - lo) / 2u) / (hi - lo));
        }
    return 0;
}

void fm1_fade_out(int16_t *pcm, unsigned frames, uint32_t *gain, uint32_t step)
{
    for (unsigned i = 0; i < frames; ++i) {
        uint32_t g = *gain;
        pcm[2 * i] = (int16_t)(((int32_t)pcm[2 * i] * (int64_t)g) >> 16);
        pcm[2 * i + 1] = (int16_t)(((int32_t)pcm[2 * i + 1] * (int64_t)g) >> 16);
        *gain = g > step ? g - step : 0u;
    }
}
