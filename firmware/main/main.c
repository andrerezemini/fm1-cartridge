/* SPDX-License-Identifier: GPL-3.0-only */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "driver/uart.h"
#include "driver/uart_vfs.h"
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
#if CONFIG_FM1_AUDIO_SYNTH
#include "persist.h"
#endif

static const char *TAG = "cartridge";

typedef struct {
    bool panic;
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
         * one only feeds the UART counters. A slow console must not cut voices. */
        return;
#else
        /* Never silently lose a Note Off; a full queue triggers panic. */
        enqueue_disconnect(context);
#endif
    }
}

static void enqueue_request(const uint8_t frame[CART_REQUEST_SIZE], uint32_t session, void *context)
{
    (void)context;
#if CONFIG_FM1_ENABLE_UAC_TX
    fm1_audio_request(frame, session);
#else
    (void)frame; (void)session;
#endif
}

#if !CONFIG_FM1_ENABLE_UAC_TX
/* The MIDI bench profile (no audio) logs every event; the audio profiles count them (app_main). */
static void log_midi(const fm1_midi_event_t *event)
{
    unsigned channel = (event->status & 0x0f) + 1;
    if ((event->status & 0xf0) == 0x80 || (event->status & 0xf0) == 0x90)
        ESP_LOGI(TAG, "MIDI cable=%u ch=%u %s note=%u velocity=%u", event->cable,
                 channel, (event->status & 0xf0) == 0x90 && event->data2 != 0 ? "ON" : "OFF",
                 event->data1, event->data2);
    else if (event->status < 0xf0)
        ESP_LOGI(TAG, "MIDI cable=%u status=0x%02x data=%u,%u", event->cable,
                 event->status, event->data1, event->data2);
}
#endif

/* The UART console as a buffered driver: a log line goes into a ring and the task goes on, instead of
 * spinning on the FIFO at 115200 baud (one line is several ms on the wire). A restart (a slot switch)
 * waits for the ring to drain; a panic or a watchdog prints straight to the FIFO, and the lines still
 * in the ring at that moment are lost. */
static void console_drain(void)
{
    uart_wait_tx_done(CONFIG_ESP_CONSOLE_UART_NUM, pdMS_TO_TICKS(400));
}
static void console_buffered(void)
{
    const int port = CONFIG_ESP_CONSOLE_UART_NUM;
    if (uart_driver_install(port, 256, 4096, 0, NULL, 0) == ESP_OK) {
        uart_vfs_dev_use_driver(port);
        esp_register_shutdown_handler(console_drain);
    }
}

void app_main(void)
{
    console_buffered();
    ESP_LOGI(TAG, "FM1 Cartridge: MIDI host bench / ESP-IDF %s", esp_get_idf_version());
    ESP_LOGI(TAG, "UART0 TX=GPIO43 RX=GPIO44, 115200 baud; USB OTG GPIO19/20 reserved for FM-1");
    esp_reset_reason_t reset = esp_reset_reason();
    if (reset == ESP_RST_BROWNOUT || reset == ESP_RST_PANIC || reset == ESP_RST_INT_WDT || reset == ESP_RST_TASK_WDT)
        ESP_LOGW(TAG, "Last reset: %s", reset == ESP_RST_BROWNOUT ? "brownout (supply too low)" :
                 reset == ESP_RST_PANIC ? "panic" : "watchdog");
#if CONFIG_FM1_ENABLE_UAC_TX
    ESP_LOGW(TAG, "Experimental UAC audio TX enabled; awaiting compatible PCM16 stereo 44.1 kHz device");
#else
    ESP_LOGI(TAG, "Audio TX and Clipper synthesis are not enabled in this bench firmware");
#endif

    fm1_slots_init();
#if CONFIG_FM1_AUDIO_SYNTH
    fm1_persist_init();
#endif
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

    uint32_t clock_total = 0, clock_interval = 0, notes_on = 0, notes_off = 0, others = 0;
    int64_t report_at = esp_timer_get_time() + 1000000;
#if CONFIG_FM1_ENABLE_UAC_TX
    int64_t audio_report_at = esp_timer_get_time() + 5000000;
#endif
    for (;;) {
        app_event_t event;
        if (xQueueReceive(queue, &event, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (event.panic) {
                clock_total = 0;
                clock_interval = 0;
                ESP_LOGW(TAG, "PANIC: transport stopped or event queue full; release all voices");
            } else if (event.midi.status == 0xf8) {
                ++clock_total;
                ++clock_interval;
#if !CONFIG_FM1_ENABLE_UAC_TX
            } else if (event.midi.status < 0xf0) {
                log_midi(&event.midi);
#endif
            } else if ((event.midi.status & 0xf0) == 0x90 && event.midi.data2) {
                ++notes_on;
            } else if ((event.midi.status & 0xf0) == 0x80 || (event.midi.status & 0xf0) == 0x90) {
                ++notes_off;
            } else if (event.midi.status == 0xfa || event.midi.status == 0xfb || event.midi.status == 0xfc) {
                ESP_LOGI(TAG, "MIDI transport %s", event.midi.status == 0xfa ? "START" :
                         event.midi.status == 0xfb ? "CONTINUE" : "STOP");
            } else {
                ++others;
            }
        }
#if CONFIG_FM1_AUDIO_SYNTH
        /* The FM-1 has been answered by this firmware: it stays past the next reset. */
        if (fm1_audio_answered())
            fm1_slots_confirm();
        const char *name;
        const void *record;
        size_t size;
        if (fm1_audio_persist_pending(&name, &record, &size)) {
            bool saved = fm1_persist_save(name, record, size);
            if (saved)
                ESP_LOGI(TAG, "%s: the sound is kept for the next start (%u B)", name, (unsigned)size);
            fm1_audio_persist_done(saved);
        }
#endif
        int64_t now = esp_timer_get_time();
        if (now >= report_at) {
            if (notes_on || notes_off || others)
                ESP_LOGI(TAG, "MIDI notes on=%" PRIu32 " off=%" PRIu32 " other=%" PRIu32, notes_on, notes_off, others);
            if (clock_interval != 0) {
                ESP_LOGI(TAG, "MIDI CLOCK ticks=%" PRIu32 " total=%" PRIu32,
                         clock_interval, clock_total);
            }
            notes_on = notes_off = others = clock_interval = 0;
            report_at = now + 1000000;
        }
#if CONFIG_FM1_ENABLE_UAC_TX
        if (now >= audio_report_at) {
            fm1_audio_stats_t s;
            fm1_audio_stats(&s);
            if (s.streaming)
                ESP_LOGI(TAG, "audio frames=%" PRIu32 " underruns=%" PRIu32 " write_retries=%" PRIu32
                         " restarts=%" PRIu32 " render_max_us=%" PRIu32 " pass_max_us=%" PRIu32
                         " (64-frame budget=1451 us)", s.frames, s.underruns, s.write_retries, s.restarts,
                         s.render_max_us, s.pass_max_us);
            audio_report_at = now + 5000000;
        }
#endif
    }
}
