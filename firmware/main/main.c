/* SPDX-License-Identifier: GPL-3.0-only */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "usb_midi_host.h"
#include "slots.h"
#include "sdkconfig.h"
#if CONFIG_FM1_ENABLE_UAC_TX
#include "audio_tx.h"
#endif

static const char *TAG = "cartridge";

typedef struct {
    bool panic, link;            /* link: a cartridge request arrived (the FM-1 is there) */
    fm1_midi_event_t midi;
} app_event_t;

static void enqueue_disconnect(void *context)
{
#if CONFIG_FM1_ENABLE_UAC_TX
    fm1_audio_panic();
#endif
    QueueHandle_t queue = context;
    const app_event_t event = { .panic = true };
    /* Discard events from the previous connection. The synth turns off all
     * voices on this event rather than relying on MIDI Note Off. */
    xQueueReset(queue);
    xQueueSend(queue, &event, 0);
}

static void enqueue_midi(const fm1_midi_event_t *midi, void *context)
{
#if CONFIG_FM1_ENABLE_UAC_TX
    fm1_audio_midi(midi);
#endif
    QueueHandle_t queue = context;
    const app_event_t event = { .midi = *midi };
    if (xQueueSend(queue, &event, 0) != pdTRUE) {
#if CONFIG_FM1_ENABLE_UAC_TX
        /* The synth already received this event through its own queue; this
         * one only feeds the UART log. A slow console must not cut voices. */
        return;
#else
        /* Never silently lose a Note Off; a full queue triggers panic. */
        enqueue_disconnect(context);
#endif
    }
}

static void enqueue_request(const uint8_t frame[CART_REQUEST_SIZE], uint32_t session, void *context)
{
    static bool linked;
#if CONFIG_FM1_ENABLE_UAC_TX
    fm1_audio_request(frame, session);
#else
    (void)frame; (void)session;
#endif
    if (!linked) {                 /* once: the main loop confirms this slot to the bootloader */
        const app_event_t event = { .link = true };
        linked = xQueueSend((QueueHandle_t)context, &event, 0) == pdTRUE;
    }
}

static void log_midi(const fm1_midi_event_t *event)
{
    unsigned channel = (event->status & 0x0f) + 1;
    switch (event->status & 0xf0) {
    case 0x80:
    case 0x90:
        ESP_LOGI(TAG, "MIDI cable=%u ch=%u %s note=%u velocity=%u", event->cable,
                 channel, (event->status & 0xf0) == 0x90 && event->data2 != 0 ? "ON" : "OFF",
                 event->data1, event->data2);
        break;
    case 0xf0:
        if (event->status == 0xfa || event->status == 0xfb || event->status == 0xfc) {
            ESP_LOGI(TAG, "MIDI transport %s", event->status == 0xfa ? "START" :
                     event->status == 0xfb ? "CONTINUE" : "STOP");
        }
        break;
    default:
        ESP_LOGI(TAG, "MIDI cable=%u status=0x%02x data=%u,%u", event->cable,
                 event->status, event->data1, event->data2);
        break;
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "FM1 Cartridge: MIDI host bench / ESP-IDF %s", esp_get_idf_version());
    ESP_LOGI(TAG, "UART0 TX=GPIO43 RX=GPIO44, 115200 baud; USB OTG GPIO19/20 reserved for FM-1");
#if CONFIG_FM1_ENABLE_UAC_TX
    ESP_LOGW(TAG, "Experimental UAC audio TX enabled; awaiting compatible PCM16 stereo 44.1 kHz device");
#else
    ESP_LOGI(TAG, "Audio TX and Clipper synthesis are not enabled in this bench firmware");
#endif

    fm1_slots_init();
    QueueHandle_t queue = xQueueCreate(128, sizeof(app_event_t));
    ESP_ERROR_CHECK(queue ? ESP_OK : ESP_ERR_NO_MEM);
    const fm1_usb_midi_callbacks_t callbacks = {
#if CONFIG_FM1_ENABLE_UAC_TX
        .host_ready = fm1_audio_install,
#endif
        .event = enqueue_midi,
        .request = enqueue_request,
        .disconnect = enqueue_disconnect,
        .context = queue,
    };
    ESP_ERROR_CHECK(fm1_usb_midi_start(&callbacks));

    uint32_t clock_total = 0;
    uint32_t clock_interval = 0;
    int64_t report_at = esp_timer_get_time() + 1000000;
    for (;;) {
        app_event_t event;
        if (xQueueReceive(queue, &event, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (!event.panic)
                fm1_slots_confirm();    /* the FM-1 talks to this firmware: it stays past the next reset */
            if (event.link) {
                /* nothing to log: cartridge requests are answered by the audio task */
            } else if (event.panic) {
                clock_total = 0;
                clock_interval = 0;
                ESP_LOGW(TAG, "PANIC: transport stopped or event queue full; release all voices");
            } else if (event.midi.status == 0xf8) {
                ++clock_total;
                ++clock_interval;
            } else {
                log_midi(&event.midi);
            }
        }
        int64_t now = esp_timer_get_time();
        if (now >= report_at) {
            if (clock_interval != 0) {
                ESP_LOGI(TAG, "MIDI CLOCK ticks=%" PRIu32 " total=%" PRIu32,
                         clock_interval, clock_total);
            }
            clock_interval = 0;
            report_at = now + 1000000;
        }
    }
}
