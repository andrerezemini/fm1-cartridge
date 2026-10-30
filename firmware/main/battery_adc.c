/* SPDX-License-Identifier: GPL-3.0-only */
#include "battery_adc.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "sdkconfig.h"

#define BURST 16u                        /* readings averaged into one */

static const char *TAG = "battery";
static adc_oneshot_unit_handle_t unit;
static adc_cali_handle_t cali;
static adc_channel_t channel;

bool fm1_battery_adc_init(void)
{
    adc_unit_t unit_id;
    if (adc_oneshot_io_to_channel(CONFIG_FM1_BATTERY_GPIO, &unit_id, &channel) != ESP_OK || unit_id != ADC_UNIT_1) {
        ESP_LOGW(TAG, "GPIO%d is not an ADC1 pin; no battery reading", CONFIG_FM1_BATTERY_GPIO);
        return false;
    }
    const adc_oneshot_unit_init_cfg_t unit_config = { .unit_id = ADC_UNIT_1 };
    const adc_oneshot_chan_cfg_t channel_config = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    const adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1, .chan = channel, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_oneshot_new_unit(&unit_config, &unit) != ESP_OK ||
        adc_oneshot_config_channel(unit, channel, &channel_config) != ESP_OK ||
        adc_cali_create_scheme_curve_fitting(&cali_config, &cali) != ESP_OK) {
        ESP_LOGW(TAG, "ADC1 on GPIO%d could not be set up; no battery reading", CONFIG_FM1_BATTERY_GPIO);
        return false;
    }
    ESP_LOGI(TAG, "cell on GPIO%d, divider %d/1000", CONFIG_FM1_BATTERY_GPIO, CONFIG_FM1_BATTERY_DIVIDER);
    return true;
}

uint32_t fm1_battery_adc_mv(void)
{
    uint32_t sum = 0, n = 0;
    if (!unit || !cali) return 0;
    for (unsigned i = 0; i < BURST; ++i) {
        int raw, mv;
        if (adc_oneshot_read(unit, channel, &raw) == ESP_OK && adc_cali_raw_to_voltage(cali, raw, &mv) == ESP_OK &&
            mv > 0) {
            sum += (uint32_t)mv;
            n++;
        }
    }
    return n ? (uint32_t)((uint64_t)sum * CONFIG_FM1_BATTERY_DIVIDER / (1000u * n)) : 0u;
}
