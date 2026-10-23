/* SPDX-License-Identifier: GPL-3.0-only */
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "usb/uac_host.h"
#include "audio_tx.h"
#if CONFIG_FM1_AUDIO_SYNTH
#include "fm1_synth.h"
#include "cartridge_remote.h"
#include "usb_midi_host.h"
#include "slots.h"
#include "esp_random.h"
#if CONFIG_FM1_AUDIO_CLIPPER
#include "clipper_synth.h"
#define SYNTH_API clipper_synth_api
#elif CONFIG_FM1_AUDIO_FELUCCA
#include "felucca_synth.h"
#define SYNTH_API felucca_synth_api
#elif CONFIG_FM1_AUDIO_MELODEE
#include "melodee_synth.h"
#define SYNTH_API melodee_synth_api
#endif
#endif

#define AUDIO_RATE 44100u
#define AUDIO_FRAMES 128u
#define NOTIFY_WAKE 1u
#define NOTIFY_PANIC 2u

static const char *TAG = "audio_tx";
static TaskHandle_t audio_task_handle;
static QueueHandle_t midi_queue;
static QueueHandle_t control_queue;
#if CONFIG_FM1_AUDIO_SYNTH
typedef struct { uint8_t frame[CART_REQUEST_SIZE]; uint32_t session; } remote_request_t;
static QueueHandle_t remote_queue;
static cartridge_remote_t remote;
static const fm1_synth_api_t *synth;
#endif

typedef enum { AUDIO_CONNECT, AUDIO_GONE, AUDIO_ERROR } control_kind_t;
typedef struct {
    control_kind_t kind;
    uint8_t address;
    uint8_t interface;
    uac_host_device_handle_t handle;
} control_event_t;

static void queue_control(const control_event_t *event)
{
    /* Only connect/disconnect/errors enter this queue, never TX completions.
     * A pathological overflow fails at once rather than losing a disconnect
     * and retaining a handle whose lifetime is no longer known. */
    ESP_ERROR_CHECK(xQueueSend(control_queue, event, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM);
    xTaskNotify(audio_task_handle, NOTIFY_WAKE, eSetBits);
}

static void device_event(uac_host_device_handle_t handle, uac_host_device_event_t event, void *arg)
{
    (void)arg;
    if (event == UAC_HOST_DRIVER_EVENT_DISCONNECTED || event == UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR) {
        control_event_t message = {
            .kind = event == UAC_HOST_DRIVER_EVENT_DISCONNECTED ? AUDIO_GONE : AUDIO_ERROR,
            .handle = handle,
        };
        queue_control(&message);
    } else if (event == UAC_HOST_DEVICE_EVENT_TX_DONE) {
        /* The callback only wakes the owner. No rendering, writes or device
         * destruction occur in the UAC driver's event-processing callback. */
        xTaskNotify(audio_task_handle, NOTIFY_WAKE, eSetBits);
    }
}

static void driver_event(uint8_t address, uint8_t interface, uac_host_driver_event_t event, void *arg)
{
    (void)arg;
    if (event == UAC_HOST_DRIVER_EVENT_TX_CONNECTED) {
        control_event_t message = {
            .kind = AUDIO_CONNECT, .address = address, .interface = interface,
        };
        queue_control(&message);
    }
}

static bool has_rate(const uac_host_dev_alt_param_t *alt)
{
    if (alt->sample_freq_type == 0) {
        return alt->sample_freq_lower <= AUDIO_RATE && alt->sample_freq_upper >= AUDIO_RATE;
    }
    for (unsigned i = 0; i < alt->sample_freq_type && i < UAC_FREQ_NUM_MAX; ++i) {
        if (alt->sample_freq[i] == AUDIO_RATE) {
            return true;
        }
    }
    return false;
}

static bool validate_format(uac_host_device_handle_t handle)
{
    uac_host_dev_info_t info;
    if (uac_host_get_device_info(handle, &info) != ESP_OK || info.type != UAC_STREAM_TX) {
        return false;
    }
    bool found = false;
    for (unsigned i = 1; i <= info.iface_alt_num; ++i) {
        uac_host_dev_alt_param_t alt;
        if (uac_host_get_device_alt_param(handle, i, &alt) != ESP_OK) {
            return false;
        }
        /* UAC 1.5.0 stores only UAC_FREQ_NUM_MAX entries but its start loop
         * uses the advertised count. Reject a longer list on every alternate
         * before calling start, including alternatives without our rate. */
        if (alt.sample_freq_type > UAC_FREQ_NUM_MAX) {
            return false;
        }
        if (alt.channels == 2 && alt.bit_resolution == 16 && has_rate(&alt)) {
            /* The start API selects an alternate setting internally. Refuse
             * ambiguous layouts rather than send packed PCM16 into 24/32-bit
             * containers if another matching alternate would be selected. */
            if (alt.format != 1 || alt.subframe_size != 2) {
                return false;
            }
            found = true;
        }
    }
    return found;
}

static esp_err_t open_stream(const control_event_t *event, uac_host_device_handle_t *device)
{
    const uac_host_device_config_t config = {
        .addr = event->address, .iface_num = event->interface,
        .buffer_size = 4096, .buffer_threshold = 1024, .callback = device_event,
    };
    esp_err_t error = uac_host_device_open(&config, device);
    if (error != ESP_OK) {
        *device = NULL;
        return error;
    }
    if (!validate_format(*device)) {
        ESP_LOGW(TAG, "UAC device lacks unambiguous PCM16 stereo 44100 Hz; rejected");
        return ESP_ERR_NOT_SUPPORTED;
    }
    const uac_host_stream_config_t stream = {
        .channels = 2, .bit_resolution = 16, .sample_freq = AUDIO_RATE,
    };
    return uac_host_device_start(*device, &stream);
}

static void panic_synth(void)
{
#if CONFIG_FM1_AUDIO_SYNTH
    synth->panic();
#endif
}

static void audio_task(void *arg)
{
    (void)arg;
#if CONFIG_FM1_AUDIO_SYNTH
    synth = SYNTH_API();
    ESP_ERROR_CHECK(synth->init(AUDIO_RATE) == 0 ? ESP_OK : ESP_ERR_INVALID_ARG);
    ESP_ERROR_CHECK(synth->set_preset(CONFIG_FM1_SYNTH_ENGINE, CONFIG_FM1_SYNTH_PRESET) == 0
                    ? ESP_OK : ESP_ERR_INVALID_ARG);
    ESP_ERROR_CHECK(synth->set_midi_channel(CONFIG_FM1_MIDI_CHANNEL) == 0 ? ESP_OK : ESP_ERR_INVALID_ARG);
    cartridge_remote_init(&remote, synth, fm1_slots(), (uint16_t)esp_random());
    ESP_LOGI(TAG, "%s on core 1: engine=%s preset=%s MIDI channel=%d (0 omni); waiting for USB audio output",
             synth->name, synth->engine_name(CONFIG_FM1_SYNTH_ENGINE),
             synth->preset_name(CONFIG_FM1_SYNTH_ENGINE, CONFIG_FM1_SYNTH_PRESET), CONFIG_FM1_MIDI_CHANNEL);
    int64_t slot_deadline = 0;              /* FIRMWARE: when the knob's choice becomes the boot partition */
#else
    ESP_LOGI(TAG, "440 Hz tone on core 1 (-20 dBFS); waiting for USB audio output");
    float phase = 0;
#endif
    uac_host_device_handle_t device = NULL;
    bool stream_running = false;
    bool stop_pending = false;
    bool closing = false;
    bool connect_pending = false;
    control_event_t next_connection = {0};
    bool pending_pcm = false;
    int16_t pcm[AUDIO_FRAMES * 2];
    uint64_t frames = 0;
    uint32_t writes_failed = 0;
    int64_t max_render_us = 0;
    int64_t report_at = esp_timer_get_time() + 5000000;
    for (;;) {
        uint32_t notification = 0;
        bool have_work = stream_running || stop_pending || closing || connect_pending;
        TickType_t wait = have_work ? 0 : portMAX_DELAY;
#if CONFIG_FM1_AUDIO_SYNTH
        if (!have_work && slot_deadline) wait = pdMS_TO_TICKS(50);
#endif
        xTaskNotifyWait(0, UINT32_MAX, &notification, wait);
        if (notification & NOTIFY_PANIC) {
            panic_synth();
            pending_pcm = false;
        }

        control_event_t event;
        while (xQueueReceive(control_queue, &event, 0) == pdTRUE) {
            if (event.kind == AUDIO_CONNECT) {
                if (!device || closing) {
                    /* Preserve a rapid reconnect until the preceding session
                     * has finished draining. Its NEW_DEV event is not replayed. */
                    next_connection = event;
                    connect_pending = true;
                }
            } else if (event.handle == device) {
                ESP_LOGW(TAG, "%s; stopping audio", event.kind == AUDIO_GONE ? "UAC disconnected" : "UAC transfer error");
                stream_running = false;
                if (event.kind == AUDIO_GONE) {
                    closing = true;
                } else {
                    stop_pending = true;
                }
            }
        }

        if (closing && device) {
            panic_synth();
            pending_pcm = false;
            xQueueReset(midi_queue);
            /* The UAC driver cancels/drains its own ISO transfers. Only this
             * owner task calls write/close; callbacks never free the handle. */
            esp_err_t error = uac_host_device_close(device);
            if (error == ESP_OK) {
                device = NULL;
                closing = false;
                stop_pending = false;
                ESP_LOGI(TAG, "UAC closed; waiting for reconnect");
            } else {
                ESP_LOGW(TAG, "UAC close pending: %s", esp_err_to_name(error));
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
            }
            continue;
        }

        if (stop_pending && device) {
            panic_synth();
            pending_pcm = false;
            xQueueReset(midi_queue);
            esp_err_t error = uac_host_device_stop(device);
            stop_pending = false;
            /* In UAC 1.5.0, close can return ESP_OK without deleting while
             * DISCONNECTED is being prepared (WAIT_USER_DELETE flag). Keep
             * ownership until that callback arrives; stop never deletes the
             * handle. This also avoids racing class-driver teardown after an
             * INVALID_STATE from write. A connected failed device needs replug. */
            ESP_LOGW(TAG, "UAC stopped (%s); retaining handle until disconnect, reconnect to retry",
                     esp_err_to_name(error));
            continue;
        }

        if (!device && connect_pending) {
            connect_pending = false;
            esp_err_t error = open_stream(&next_connection, &device);
            if (error != ESP_OK) {
                ESP_LOGW(TAG, "UAC open/start failed: %s", esp_err_to_name(error));
                stop_pending = device != NULL;
                continue;
            }
            stream_running = true;
            panic_synth();
            xQueueReset(midi_queue);
            pending_pcm = false;
            frames = 0;
            writes_failed = 0;
            max_render_us = 0;
            ESP_LOGI(TAG, "UAC TX started: address=%u interface=%u PCM16 stereo 44100 Hz",
                     next_connection.address, next_connection.interface);
            /* Dispatch any disconnect raised during synchronous open/start
             * before rendering or writing into the newly obtained handle. */
            continue;
        }

#if CONFIG_FM1_AUDIO_SYNTH
        /* One bounded control operation per audio pass, on the synth owner.
         * USB callbacks only copy bytes; replies travel through the MIDI task. */
        remote_request_t request;
        if (xQueueReceive(remote_queue, &request, 0) == pdTRUE) {
            uint8_t reply[CART_REPLY_SIZE];
            if (cartridge_remote_handle(&remote, request.frame, sizeof request.frame, reply))
                fm1_usb_midi_reply(reply, request.session);
            if (remote.slot_dirty) {
                /* FIRMWARE turned: the slot it rests on boots after CONFIG_FM1_SLOT_SWITCH_MS;
                 * back on the running slot, nothing happens */
                remote.slot_dirty = 0;
                slot_deadline = remote.slots && remote.slot_cursor != remote.slots->current()
                                    ? esp_timer_get_time() + (int64_t)CONFIG_FM1_SLOT_SWITCH_MS * 1000 : 0;
            }
        }
        if (slot_deadline && esp_timer_get_time() >= slot_deadline) {
            slot_deadline = 0;
            if (remote.slots && remote.slot_cursor != remote.slots->current()) {
                panic_synth();
                pending_pcm = false;
                if (fm1_slots_boot(remote.slot_cursor) != 0) {
                    ESP_LOGW(TAG, "FIRMWARE slot %u cannot boot; staying", remote.slot_cursor);
                    remote.slot_cursor = remote.slots->current();
                    remote.cached = 0;
                }
            }
        }
#endif
        fm1_midi_event_t midi;
        while (xQueueReceive(midi_queue, &midi, 0) == pdTRUE) {
#if CONFIG_FM1_AUDIO_SYNTH
            if (stream_running && synth->midi(midi.status, midi.data1, midi.data2) == -2) {
                ESP_LOGW(TAG, "Synth MIDI queue overflow: all voices released");
            }
#endif
        }
        if (!stream_running) {
            continue;
        }
        if (!pending_pcm) {
            int64_t begin = esp_timer_get_time();
#if CONFIG_FM1_AUDIO_SYNTH
            synth->render(pcm, AUDIO_FRAMES);
#else
            for (unsigned i = 0; i < AUDIO_FRAMES; ++i) {
                int16_t sample = (int16_t)(3276.7f * sinf(phase));
                pcm[2 * i] = sample;
                pcm[2 * i + 1] = sample;
                phase += 6.28318530718f * 440.0f / AUDIO_RATE;
                if (phase >= 6.28318530718f) {
                    phase -= 6.28318530718f;
                }
            }
#endif
            int64_t elapsed = esp_timer_get_time() - begin;
            if (elapsed > max_render_us) {
                max_render_us = elapsed;
            }
            pending_pcm = true;
        }
        /* USB consumption supplies backpressure. No vTaskDelay-based sample
         * clock: retain this exact PCM block if the ring buffer is full. */
        esp_err_t error = uac_host_device_write(device, (uint8_t *)pcm, sizeof(pcm), pdMS_TO_TICKS(20));
        if (error == ESP_OK) {
            pending_pcm = false;
            frames += AUDIO_FRAMES;
        } else {
            ++writes_failed;
            if (error != ESP_FAIL && error != ESP_ERR_TIMEOUT) {
                ESP_LOGW(TAG, "UAC write failed: %s", esp_err_to_name(error));
                stream_running = false;
                stop_pending = true;
            }
        }
        int64_t now = esp_timer_get_time();
        if (now >= report_at) {
            ESP_LOGI(TAG, "queued_frames=%" PRIu64 " write_retries=%" PRIu32 " render_max_us=%" PRId64
                     " (128-frame budget=2902 us)", frames, writes_failed, max_render_us);
            report_at = now + 5000000;
        }
    }
}

void fm1_audio_panic(void)
{
    if (audio_task_handle) {
        xQueueReset(midi_queue);
#if CONFIG_FM1_AUDIO_SYNTH
        xQueueReset(remote_queue);
#endif
        /* Notification bits cannot be lost when the MIDI queue is full. */
        xTaskNotify(audio_task_handle, NOTIFY_PANIC, eSetBits);
    }
}

void fm1_audio_midi(const fm1_midi_event_t *event)
{
    if (audio_task_handle) {
        if (xQueueSend(midi_queue, event, 0) != pdTRUE) {
            fm1_audio_panic();
        } else {
            xTaskNotify(audio_task_handle, NOTIFY_WAKE, eSetBits);
        }
    }
}

void fm1_audio_request(const uint8_t frame[CART_REQUEST_SIZE], uint32_t session)
{
#if CONFIG_FM1_AUDIO_SYNTH
    remote_request_t request = {.session = session};
    memcpy(request.frame, frame, sizeof request.frame);
    if (audio_task_handle && xQueueSend(remote_queue, &request, 0) == pdTRUE)
        xTaskNotify(audio_task_handle, NOTIFY_WAKE, eSetBits);
#else
    (void)frame; (void)session;
#endif
}

esp_err_t fm1_audio_install(void *unused)
{
    (void)unused;
#if CONFIG_FM1_AUDIO_SYNTH
    remote_queue = xQueueCreate(8, sizeof(remote_request_t));
    if (!remote_queue) return ESP_ERR_NO_MEM;
#endif
    midi_queue = xQueueCreate(128, sizeof(fm1_midi_event_t));
    control_queue = xQueueCreate(16, sizeof(control_event_t));
    if (!midi_queue || !control_queue) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(audio_task, "audio_tx", 8192, NULL, 8,
                               &audio_task_handle, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    const uac_host_driver_config_t config = {
        .create_background_task = true, .task_priority = 12,
        .stack_size = 6144, .core_id = 0, .callback = driver_event,
    };
    return uac_host_install(&config);
}
