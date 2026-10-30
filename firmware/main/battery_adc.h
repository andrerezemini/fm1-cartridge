/* SPDX-License-Identifier: GPL-3.0-only */
/* The cell's voltage through a divider on an ADC1 pin (CONFIG_FM1_BATTERY_GPIO, ratio
 * CONFIG_FM1_BATTERY_DIVIDER / 1000), read by the main loop. The Super Mini has no such divider of its
 * own: it is wired by hand (hardware/README.md#battery-reading). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

bool fm1_battery_adc_init(void);
/* The cell in mV, the mean of a burst of calibrated readings; 0 when the ADC failed. */
uint32_t fm1_battery_adc_mv(void);
