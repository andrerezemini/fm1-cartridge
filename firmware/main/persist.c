/* SPDX-License-Identifier: GPL-3.0-only */
#include "persist.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "persist";
#define NAMESPACE "fm1cart"

esp_err_t fm1_persist_init(void)
{
    esp_err_t error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES || error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition unreadable (%s): erased", esp_err_to_name(error));
        if (nvs_flash_erase() == ESP_OK) error = nvs_flash_init();
    }
    if (error != ESP_OK) ESP_LOGW(TAG, "NVS unavailable: %s; the sound is not kept", esp_err_to_name(error));
    return error;
}

size_t fm1_persist_load(const char *name, void *record, size_t capacity)
{
    nvs_handle_t handle;
    size_t size = capacity;
    if (nvs_open(NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return 0;
    esp_err_t error = nvs_get_blob(handle, name, record, &size);
    nvs_close(handle);
    return error == ESP_OK ? size : 0;
}

bool fm1_persist_save(const char *name, const void *record, size_t size)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (error == ESP_OK) {
        error = nvs_set_blob(handle, name, record, size);
        if (error == ESP_OK) error = nvs_commit(handle);
        nvs_close(handle);
    }
    if (error != ESP_OK) ESP_LOGW(TAG, "%s not kept: %s", name, esp_err_to_name(error));
    return error == ESP_OK;
}
