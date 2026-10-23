/* SPDX-License-Identifier: GPL-3.0-only */
#include "slots.h"
#include <ctype.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"

static const char *TAG = "slots";
typedef struct { const esp_partition_t *part; char name[13]; uint8_t valid; } slot_t;
static slot_t slots[CART_SLOTS_MAX];
static uint8_t slot_count, slot_running;

static uint8_t count(void) { return slot_count; }
static uint8_t current(void) { return slot_running; }
static const char *name(uint8_t s) { return s < slot_count ? slots[s].name : "?"; }
static int bootable(uint8_t s) { return s < slot_count && slots[s].valid; }
static const fm1_slots_t api = { count, current, name, bootable };

static void describe(slot_t *s)
{
    esp_app_desc_t desc;
    s->valid = 0;
    strcpy(s->name, "EMPTY");
    if (esp_ota_get_partition_description(s->part, &desc) != ESP_OK) return;
    /* the profile's CONFIG_APP_PROJECT_VER names the firmware (CLIPPER, MELODEE, FELUCCA, TONE, MIDI) */
    const char *src = desc.version[0] ? desc.version : desc.project_name;
    size_t n = 0;
    for (size_t i = 0; src[i] && n + 1 < sizeof s->name; ++i)
        if (src[i] > 32 && src[i] < 127) s->name[n++] = (char)toupper((unsigned char)src[i]);
    s->name[n] = 0;
    if (!n) strcpy(s->name, "APP");
    s->valid = 1;
}

void fm1_slots_init(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    slot_count = 0;
    /* factory first (none in partitions.csv), then ota_0.. in subtype order */
    for (int sub = ESP_PARTITION_SUBTYPE_APP_FACTORY; sub <= ESP_PARTITION_SUBTYPE_APP_OTA_MAX && slot_count < CART_SLOTS_MAX; ++sub) {
        if (sub > ESP_PARTITION_SUBTYPE_APP_FACTORY && sub < ESP_PARTITION_SUBTYPE_APP_OTA_MIN) continue;
        const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_APP, (esp_partition_subtype_t)sub, NULL);
        if (!p) continue;
        slot_t *s = &slots[slot_count];
        s->part = p;
        describe(s);
        if (running && p->address == running->address) slot_running = slot_count;
        slot_count++;
    }
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (running) esp_ota_get_state_partition(running, &state);
    for (uint8_t i = 0; i < slot_count; ++i)
        ESP_LOGI(TAG, "slot %u %-8s @0x%06" PRIx32 " %s%s", i, slots[i].part->label, slots[i].part->address,
                 slots[i].name, i == slot_running ? (state == ESP_OTA_IMG_PENDING_VERIFY ? " (running, pending verify)" : " (running)") : "");
}

const fm1_slots_t *fm1_slots(void) { return slot_count > 1 ? &api : NULL; }

void fm1_slots_confirm(void)
{
    static bool done;
    if (done) return;
    done = true;
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "FM-1 answered: slot %u confirmed to the bootloader (%s)", slot_running, esp_err_to_name(err));
    }
}

static void restart_now(void *arg) { (void)arg; esp_restart(); }

int fm1_slots_boot(uint8_t slot)
{
    if (!bootable(slot) || slot == slot_running) return -1;
    esp_err_t err = esp_ota_set_boot_partition(slots[slot].part);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "slot %u (%s) refused: %s", slot, slots[slot].name, esp_err_to_name(err));
        slots[slot].valid = 0;
        return -1;
    }
    ESP_LOGW(TAG, "rebooting into slot %u (%s) in 300 ms", slot, slots[slot].name);
    static esp_timer_handle_t timer;
    if (!timer) {
        const esp_timer_create_args_t args = { .callback = restart_now, .name = "slot_boot" };
        ESP_ERROR_CHECK(esp_timer_create(&args, &timer));
    }
    esp_timer_start_once(timer, 300000);
    return 0;
}
