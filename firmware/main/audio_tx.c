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
#include "cartridge_state.h"
#include "persist.h"
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
#define AUDIO_FRAMES 64u                 /* a render pass: 1.45 ms, the step at which MIDI reaches the synth */
#define NOTIFY_WAKE 1u
#define NOTIFY_PANIC 2u
#define UAC_RESTARTS 3u                  /* after a transfer error: starts the stream again, 0.5, 1 and 2 s later */
#define QUIET_SAVE_US 5000000            /* the sound is kept once nothing changed for this long and nothing plays */
#define NOTE_QUIET_US 2000000            /* ... and no note started or ended for this long (release tails) */
#define HELD_EXPIRY_US 60000000          /* a note held this long without any other note event: a lost note-off */

static const char *TAG = "audio_tx";
static TaskHandle_t audio_task_handle;
static QueueHandle_t midi_queue;
static QueueHandle_t control_queue;
static portMUX_TYPE stats_lock = portMUX_INITIALIZER_UNLOCKED;
static fm1_audio_stats_t stats;
#if CONFIG_FM1_AUDIO_SYNTH
typedef struct { uint8_t frame[CART_REQUEST_SIZE]; uint32_t session; } remote_request_t;
static QueueHandle_t remote_queue;
static cartridge_remote_t remote;
static cartridge_metrics_t metrics;      /* the page's AUDIO RENDER MAX / AUDIO UNDERRUNS, since it opened */
static const fm1_synth_api_t *synth;
static volatile bool answered;
/* the record handed to the main loop (persist_ready set), and the last one kept; persist_retry: the
 * main loop could not write it */
static cartridge_state_t pending_state, kept_state;
static size_t pending_size, kept_size;
static bool persist_ready, persist_retry;
#define SAVE_TRIES 3u
#endif

typedef enum { AUDIO_CONNECT, AUDIO_GONE, AUDIO_ERROR } control_kind_t;
typedef struct {
    control_kind_t kind;
    uint8_t address;
    uint8_t interface;
    uac_host_device_handle_t handle;
} control_event_t;

/* What the FM-1 plays, from the MIDI it sends: the notes held per channel and the transport. FIRMWARE
 * does not switch slots while either is on, and the sound is kept only when both are off. A synth
 * panic or a MIDI reopen forgets the notes (the voices are gone with them); the transport is the
 * FM-1's and changes only with START / CONTINUE / STOP, or when the FM-1 itself goes. */
static uint32_t held[16][4];
static bool transport_running;
static int64_t last_note_us;
static void follow_midi(const fm1_midi_event_t *m, int64_t now)
{
    uint32_t kind = m->status & 0xF0u, ch = m->status & 0x0Fu, bit = 1u << (m->data1 & 31u);
    if (m->status == 0xFA || m->status == 0xFB) {
        transport_running = true;
    } else if (m->status == 0xFC) {
        transport_running = false;
    } else if (kind == 0x90 && m->data2) {
        held[ch][(m->data1 >> 5) & 3u] |= bit;
        last_note_us = now;
    } else if (kind == 0x80 || kind == 0x90) {
        held[ch][(m->data1 >> 5) & 3u] &= ~bit;
        last_note_us = now;
    } else if (kind == 0xB0 && (m->data1 == 120 || m->data1 >= 123)) {
        memset(held[ch], 0, sizeof held[ch]);
    }
}
static void forget_notes(void)
{
    memset(held, 0, sizeof held);
}
#if CONFIG_FM1_AUDIO_SYNTH
/* channel 0: any channel (OMNI) */
static bool fm1_playing(uint32_t channel, int64_t now)
{
    if (transport_running) return true;
    if (now - last_note_us >= HELD_EXPIRY_US) return false;
    for (uint32_t c = 0; c < 16u; ++c)
        if (!channel || c + 1u == channel)
            if (held[c][0] | held[c][1] | held[c][2] | held[c][3]) return true;
    return false;
}
#endif

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

static const uac_host_stream_config_t stream_config = {
    .channels = 2, .bit_resolution = 16, .sample_freq = AUDIO_RATE,
};

static esp_err_t open_stream(const control_event_t *event, uac_host_device_handle_t *device)
{
    /* The ring the render fills: the writer blocks once it is full, so it holds about its size in
     * audio ahead of the URBs (CONFIG_FM1_UAC_BUFFER_BYTES / 4 frames), and that is the cartridge's
     * own latency together with the URBs (UAC_NUM_ISOC_URBS x UAC_NUM_PACKETS_PER_URB ms). */
    const uac_host_device_config_t config = {
        .addr = event->address, .iface_num = event->interface,
        .buffer_size = CONFIG_FM1_UAC_BUFFER_BYTES, .buffer_threshold = CONFIG_FM1_UAC_BUFFER_BYTES / 4,
        .callback = device_event,
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
    return uac_host_device_start(*device, &stream_config);
}

static void panic_synth(void)
{
#if CONFIG_FM1_AUDIO_SYNTH
    synth->panic();
#endif
    forget_notes();
}

static void stats_update(void (*fn)(fm1_audio_stats_t *, uint32_t), uint32_t value)
{
    taskENTER_CRITICAL(&stats_lock);
    fn(&stats, value);
    taskEXIT_CRITICAL(&stats_lock);
}
static void stat_render(fm1_audio_stats_t *s, uint32_t us) { if (us > s->render_max_us) s->render_max_us = us; }
static void stat_pass(fm1_audio_stats_t *s, uint32_t us) { if (us > s->pass_max_us) s->pass_max_us = us; }
static void stat_frames(fm1_audio_stats_t *s, uint32_t n) { s->frames += n; }
static void stat_underrun(fm1_audio_stats_t *s, uint32_t n) { s->underruns += n; }
static void stat_retry(fm1_audio_stats_t *s, uint32_t n) { s->write_retries += n; }
static void stat_restart(fm1_audio_stats_t *s, uint32_t n) { s->restarts += n; }
static void stat_stream(fm1_audio_stats_t *s, uint32_t on)
{
    s->streaming = on != 0;
    if (on) s->frames = s->underruns = s->write_retries = 0;
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
    /* the sound this firmware had when it was last left: preset, MIDI channel and edits */
    kept_size = fm1_persist_load(synth->name, &kept_state, sizeof kept_state);
    if (kept_size && cartridge_state_apply(synth, &kept_state, kept_size) != 0) kept_size = 0;
    cartridge_remote_init(&remote, synth, fm1_slots(), (uint16_t)esp_random());
    remote.metrics = &metrics;
    metrics.budget_us = AUDIO_FRAMES * 1000000u / AUDIO_RATE;
    ESP_LOGI(TAG, "%s on core 1: engine=%s preset=%s MIDI channel=%" PRIu32 " (0 omni)%s; waiting for USB audio output",
             synth->name, synth->engine_name(synth->current_engine()),
             synth->preset_name(synth->current_engine(), synth->current_preset()), synth->midi_channel(),
             kept_size ? ", the sound kept from the last session" : "");
    int64_t slot_deadline = 0;              /* FIRMWARE: when the knob's choice becomes the boot partition */
    bool dirty = false;                     /* the sound changed since it was last kept */
    int64_t dirty_at = 0;
    uint32_t save_tries = 0;                /* writes of this sound that failed */
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
    unsigned restarts_left = 0;             /* UAC_RESTARTS per connection */
    int64_t restart_at = 0, restart_wait = 0;
    int64_t stream_t0 = 0;                  /* the underrun estimate: frames written against time since t0 */
    uint64_t stream_frames = 0;
    for (;;) {
        uint32_t notification = 0;
        bool have_work = stream_running || stop_pending || closing || connect_pending;
        TickType_t wait = have_work ? 0 : portMAX_DELAY;
        if (!have_work && restart_at) wait = pdMS_TO_TICKS(50);
#if CONFIG_FM1_AUDIO_SYNTH
        if (!have_work && slot_deadline) wait = pdMS_TO_TICKS(50);
        else if (!have_work && dirty) wait = pdMS_TO_TICKS(1000);
#endif
        xTaskNotifyWait(0, UINT32_MAX, &notification, wait);
        int64_t pass_begin = esp_timer_get_time();
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
                stats_update(stat_stream, 0);
                if (event.kind == AUDIO_GONE) {
                    closing = true;
                    restart_at = 0;
                } else {
                    stop_pending = true;
                }
            }
        }

        if (closing && device) {
            panic_synth();
            transport_running = false;              /* the FM-1 is gone */
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
             * handle. A transfer error on a device still attached is retried a
             * few times (restart_at); a gone device sends DISCONNECTED first. */
            if (restarts_left) {
                restart_wait = restart_wait ? restart_wait * 2 : 500000;
                restart_at = esp_timer_get_time() + restart_wait;
                ESP_LOGW(TAG, "UAC stopped (%s); starting it again in %" PRId64 " ms",
                         esp_err_to_name(error), restart_wait / 1000);
            } else {
                restart_at = 0;
                ESP_LOGW(TAG, "UAC stopped (%s); retaining handle until disconnect, reconnect to retry",
                         esp_err_to_name(error));
            }
            continue;
        }

        if (device && !stream_running && !closing && restart_at && esp_timer_get_time() >= restart_at) {
            restart_at = 0;
            restarts_left--;
            esp_err_t error = uac_host_device_start(device, &stream_config);
            if (error == ESP_OK) {
                stream_running = true;
                pending_pcm = false;
                stream_t0 = 0; stream_frames = 0;
                stats_update(stat_restart, 1);
                stats_update(stat_stream, 1);
                ESP_LOGI(TAG, "UAC TX started again");
            } else if (restarts_left) {
                restart_wait *= 2;
                restart_at = esp_timer_get_time() + restart_wait;
                ESP_LOGW(TAG, "UAC start failed: %s; again in %" PRId64 " ms", esp_err_to_name(error), restart_wait / 1000);
            } else {
                ESP_LOGW(TAG, "UAC start failed: %s; reconnect to retry", esp_err_to_name(error));
            }
            continue;
        }

        if (!device && connect_pending) {
            connect_pending = false;
            esp_err_t error = open_stream(&next_connection, &device);
            if (error != ESP_OK) {
                ESP_LOGW(TAG, "UAC open/start failed: %s", esp_err_to_name(error));
                stop_pending = device != NULL;
                restarts_left = 0;
                continue;
            }
            stream_running = true;
            panic_synth();
            transport_running = false;              /* a new FM-1: its START comes with its song */
            xQueueReset(midi_queue);
            pending_pcm = false;
            restarts_left = UAC_RESTARTS;
            restart_at = restart_wait = 0;
            stream_t0 = 0; stream_frames = 0;
            stats_update(stat_stream, 1);
            ESP_LOGI(TAG, "UAC TX started: address=%u interface=%u PCM16 stereo 44100 Hz, ring %d B",
                     next_connection.address, next_connection.interface, CONFIG_FM1_UAC_BUFFER_BYTES);
            /* Dispatch any disconnect raised during synchronous open/start
             * before rendering or writing into the newly obtained handle. */
            continue;
        }

        fm1_midi_event_t midi;
        int64_t now = esp_timer_get_time();
        while (xQueueReceive(midi_queue, &midi, 0) == pdTRUE) {
            follow_midi(&midi, now);
#if CONFIG_FM1_AUDIO_SYNTH
            if (stream_running && synth->midi(midi.status, midi.data1, midi.data2) == -2) {
                ESP_LOGW(TAG, "Synth MIDI queue overflow: all voices released");
                forget_notes();
            }
#endif
        }
#if CONFIG_FM1_AUDIO_SYNTH
        /* One bounded control operation per audio pass, on the synth owner.
         * USB callbacks only copy bytes; replies travel through the MIDI task. */
        remote_request_t request;
        if (xQueueReceive(remote_queue, &request, 0) == pdTRUE) {
            uint8_t reply[CART_REPLY_SIZE];
            remote.busy = fm1_playing(synth->midi_channel(), now);
            if (cartridge_remote_handle(&remote, request.frame, sizeof request.frame, reply) &&
                fm1_usb_midi_reply(reply, request.session))
                answered = true;
            if (remote.slot_dirty) {
                /* FIRMWARE turned: the slot it rests on boots after CONFIG_FM1_SLOT_SWITCH_MS;
                 * back on the running slot, or off the FIRMWARE entry, nothing happens */
                remote.slot_dirty = 0;
                slot_deadline = remote.slots && remote.slot_cursor != remote.slots->current()
                                    ? now + (int64_t)CONFIG_FM1_SLOT_SWITCH_MS * 1000 : 0;
            }
        }
        if (slot_deadline && now >= slot_deadline) {
            slot_deadline = 0;
            if (remote.slots && remote.slot_cursor != remote.slots->current()) {
                if (fm1_playing(synth->midi_channel(), now)) {
                    ESP_LOGW(TAG, "FIRMWARE: the FM-1 plays; staying on slot %u", remote.slots->current());
                    remote.slot_cursor = remote.slots->current();
                    remote.cached = 0;
                } else {
                    panic_synth();
                    pending_pcm = false;
                    if (fm1_slots_boot(remote.slot_cursor) != 0) {
                        ESP_LOGW(TAG, "FIRMWARE slot %u cannot boot; staying", remote.slot_cursor);
                        remote.slot_cursor = remote.slots->current();
                        remote.cached = 0;
                    }
                }
            }
        }
        /* keep the sound: a few quiet seconds after the last change, and only when it differs */
        if (remote.changed) {
            remote.changed = 0;
            dirty = true;
            dirty_at = now;
            save_tries = 0;
        }
        if (__atomic_exchange_n(&persist_retry, false, __ATOMIC_ACQ_REL) && ++save_tries < SAVE_TRIES) {
            dirty = true;                       /* not written: again after the next quiet seconds */
            dirty_at = now;
        }
        if (dirty && now - dirty_at >= QUIET_SAVE_US && now - last_note_us >= NOTE_QUIET_US && !fm1_playing(0, now) &&
            !__atomic_load_n(&persist_ready, __ATOMIC_ACQUIRE)) {
            dirty = false;
            pending_size = cartridge_state_capture(synth, &pending_state);
            if (pending_size && !cartridge_state_equal(&pending_state, pending_size, &kept_state, kept_size))
                __atomic_store_n(&persist_ready, true, __ATOMIC_RELEASE);
        }
#endif
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
            int64_t end = esp_timer_get_time();
            stats_update(stat_render, (uint32_t)(end - begin));
#if CONFIG_FM1_AUDIO_SYNTH
            if ((uint32_t)(end - begin) > metrics.render_us) metrics.render_us = (uint32_t)(end - begin);
#endif
            stats_update(stat_pass, (uint32_t)(end - pass_begin));
            pending_pcm = true;
        }
        /* USB consumption supplies backpressure. No vTaskDelay-based sample
         * clock: retain this exact PCM block if the ring buffer is full. */
        esp_err_t error = uac_host_device_write(device, (uint8_t *)pcm, sizeof(pcm), pdMS_TO_TICKS(20));
        if (error == ESP_OK) {
            int64_t t = esp_timer_get_time();
            pending_pcm = false;
            stats_update(stat_frames, AUDIO_FRAMES);
            /* The host's SOFs and this timer run from the same crystal: the stream has played
             * (t - t0) x 44.1 kHz frames. More than were written means the URBs ran dry. */
            if (!stream_t0) stream_t0 = t;
            stream_frames += AUDIO_FRAMES;
            if ((uint64_t)((t - stream_t0) * AUDIO_RATE / 1000000) > stream_frames) {
                stats_update(stat_underrun, 1);
#if CONFIG_FM1_AUDIO_SYNTH
                metrics.underruns++;
#endif
                stream_t0 = t - (int64_t)(stream_frames * 1000000 / AUDIO_RATE);
            }
        } else {
            stats_update(stat_retry, 1);
            if (error != ESP_FAIL && error != ESP_ERR_TIMEOUT) {
                ESP_LOGW(TAG, "UAC write failed: %s", esp_err_to_name(error));
                stream_running = false;
                stats_update(stat_stream, 0);
                stop_pending = true;
            }
        }
    }
}

void fm1_audio_stats(fm1_audio_stats_t *out)
{
    taskENTER_CRITICAL(&stats_lock);
    *out = stats;
    stats.render_max_us = stats.pass_max_us = 0;
    taskEXIT_CRITICAL(&stats_lock);
}

bool fm1_audio_answered(void)
{
#if CONFIG_FM1_AUDIO_SYNTH
    return answered;
#else
    return false;
#endif
}

bool fm1_audio_persist_pending(const char **name, const void **record, size_t *size)
{
#if CONFIG_FM1_AUDIO_SYNTH
    if (!__atomic_load_n(&persist_ready, __ATOMIC_ACQUIRE)) return false;
    *name = synth->name; *record = &pending_state; *size = pending_size;
    return true;
#else
    (void)name; (void)record; (void)size;
    return false;
#endif
}

void fm1_audio_persist_done(bool saved)
{
#if CONFIG_FM1_AUDIO_SYNTH
    if (saved) {
        memcpy(&kept_state, &pending_state, pending_size);
        kept_size = pending_size;
    } else {
        __atomic_store_n(&persist_retry, true, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&persist_ready, false, __ATOMIC_RELEASE);
#else
    (void)saved;
#endif
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
