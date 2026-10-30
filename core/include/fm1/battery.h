/* SPDX-License-Identifier: GPL-3.0-only */
/* The cartridge's cell, as the FM-1 sees it (CART_BATTERY, CART_POWER). The platform reads the cell
 * through a divider on an ADC pin (CONFIG_FM1_BATTERY) about once a second and feeds each reading
 * here; the filtered level picks the state. LOW comes back to OK only above the threshold plus a
 * margin; CUTOFF needs several readings in a row under its threshold and stays until the next boot:
 * the cartridge has faded its audio out and keeps silent, so the cell's recovery without load does
 * not bring it back. */
#pragma once
#include <stdint.h>

#define FM1_BATTERY_HYSTERESIS_MV 50u    /* LOW turns back to OK above low_mv + this */
#define FM1_BATTERY_CUTOFF_READINGS 5u   /* readings in a row under cutoff_mv (about 5 s) */
#define FM1_FADE_UNITY 65536u            /* fm1_fade_out's gain at full level */

typedef struct {
    uint32_t low_mv, cutoff_mv;
    uint32_t mv;                         /* filtered; 0 before the first reading */
    uint8_t state;                       /* CART_POWER_*: NONE until the first reading */
    uint8_t below;                       /* readings in a row under cutoff_mv */
} fm1_battery_t;

void fm1_battery_init(fm1_battery_t *b, uint32_t low_mv, uint32_t cutoff_mv);
/* One reading of the cell in mV; 0 (a failed read) is ignored. */
void fm1_battery_reading(fm1_battery_t *b, uint32_t mv);
/* 0..100 along a LiPo's discharge curve under a light load; CART_BATTERY_NONE for 0 mV. */
uint8_t fm1_battery_percent(uint32_t mv);
/* A linear fade of interleaved stereo PCM from *gain (FM1_FADE_UNITY is full level) down by step a
 * frame, to silence; *gain is left where the block ended. */
void fm1_fade_out(int16_t *pcm, unsigned frames, uint32_t *gain, uint32_t step);
