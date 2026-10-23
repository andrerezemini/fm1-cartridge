/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "fm1/cartridge_protocol.h"
#include "fm1/usb_descriptors.h"
#include "usb/usb_host.h"
#include "usb_midi_host.h"
#include "usb_host_fifo.h"
#include "sdkconfig.h"

static const char *TAG = "usb_midi";

typedef struct {
    usb_host_client_handle_t client;
    usb_device_handle_t device;
    usb_transfer_t *transfer, *tx;
    bool tx_in_flight;
    uint32_t session;
    fm1_sysex_t sysex;
    fm1_usb_endpoints_t endpoints;
    fm1_usb_midi_callbacks_t callbacks;
    uint8_t pending_address;
    bool claimed;
    bool in_flight;
    bool closing;
    bool cancel_started;
} midi_host_t;

/* Single owner: after start, every field except callbacks is accessed only
 * by client_task and callbacks dispatched by its handle_events call. */
static midi_host_t host;
static bool started;
typedef struct { uint32_t session; uint8_t frame[CART_REPLY_SIZE]; } reply_t;
static QueueHandle_t reply_queue;
bool fm1_usb_midi_reply(const uint8_t frame[CART_REPLY_SIZE], uint32_t session)
{
    if (!reply_queue || !cart_valid(frame, CART_REPLY_SIZE)) return false;
    reply_t item = {.session = session};
    memcpy(item.frame, frame, sizeof item.frame);
    return xQueueSend(reply_queue, &item, 0) == pdTRUE;
}
static void tx_complete(usb_transfer_t *transfer);


static void request_close(midi_host_t *state)
{
    if (!state->closing) {
        state->closing = true;
        if (state->callbacks.disconnect) {
            state->callbacks.disconnect(state->callbacks.context);
        }
    }
}

static void client_event(const usb_host_client_event_msg_t *message, void *arg)
{
    midi_host_t *state = arg;
    switch (message->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        if (!state->device || state->closing) {
            state->pending_address = message->new_dev.address;
        } else {
            ESP_LOGW(TAG, "Ignoring additional USB device; bench supports one device directly");
        }
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        if (message->dev_gone.dev_hdl == state->device) {
            ESP_LOGI(TAG, "FM-1 disconnected; draining transfers before releasing device");
            request_close(state);
        }
        break;
    default:
        break;
    }
}

static void transfer_complete(usb_transfer_t *transfer)
{
    midi_host_t *state = transfer->context;
    state->in_flight = false;
    if (state->closing) {
        return;
    }
    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGW(TAG, "MIDI transfer status=%d; reconnect device to retry", transfer->status);
        request_close(state);
        return;
    }
    /* A USB-MIDI event packet is atomic and always four bytes. Do not join a
     * truncated tail to the next transfer, which would corrupt framing. */
    int length = transfer->actual_num_bytes;
    if (length < 0 || length > transfer->num_bytes || length % 4 != 0) {
        ESP_LOGW(TAG, "Invalid USB-MIDI transfer length=%d; closing transport to prevent lost Note Off", length);
        request_close(state);
        return;
    }
    for (int offset = 0; offset < length; offset += 4) {
        const uint8_t *packet = transfer->data_buffer + offset;
        if (fm1_sysex_packet(&state->sysex, packet) && state->callbacks.request &&
            state->sysex.length == CART_REQUEST_SIZE && cart_valid(state->sysex.bytes, CART_REQUEST_SIZE))
            state->callbacks.request(state->sysex.bytes, state->session, state->callbacks.context);
        fm1_midi_event_t event;
        if (fm1_midi_decode(transfer->data_buffer + offset, &event) && state->callbacks.event) {
            state->callbacks.event(&event, state->callbacks.context);
        }
    }
    /* Resubmission is done by client_task, after all disconnect events have
     * also been dispatched. No transfer is resubmitted from its callback. */
}

static void tx_complete(usb_transfer_t *transfer)
{
    midi_host_t *state = transfer->context;
    state->tx_in_flight = false;
    if (!state->closing && transfer->status != USB_TRANSFER_STATUS_COMPLETED) request_close(state);
}

static void close_device(midi_host_t *state)
{
    if (!state->cancel_started) {
        state->cancel_started = true;
        if (state->claimed) {
            /* On physical removal the host may have already halted/flushed
             * the pipe. These operations can then return INVALID_STATE. In
             * either case wait for the outstanding callback before freeing. */
            esp_err_t halted = usb_host_endpoint_halt(state->device, state->endpoints.midi_ep);
            esp_err_t flushed = usb_host_endpoint_flush(state->device, state->endpoints.midi_ep);
            if (state->tx) {
                usb_host_endpoint_halt(state->device, state->endpoints.midi_out_ep);
                usb_host_endpoint_flush(state->device, state->endpoints.midi_out_ep);
            }
            ESP_LOGD(TAG, "Cancel halt=%s flush=%s", esp_err_to_name(halted), esp_err_to_name(flushed));
        }
    }
    if (state->in_flight || state->tx_in_flight) {
        return;
    }
    if (state->claimed) {
        esp_err_t error = usb_host_interface_release(state->client, state->device,
                                                    state->endpoints.midi_interface);
        if (error != ESP_OK) {
            /* Preserve state and retry; freeing a live pipe would be unsafe. */
            ESP_LOGD(TAG, "Interface release pending: %s", esp_err_to_name(error));
            return;
        }
        state->claimed = false;
    }
    if (state->transfer) {
        ESP_ERROR_CHECK(usb_host_transfer_free(state->transfer));
        state->transfer = NULL;
    }
    if (state->tx) {
        ESP_ERROR_CHECK(usb_host_transfer_free(state->tx));
        state->tx = NULL;
    }
    memset(&state->sysex, 0, sizeof state->sysex);
    if (state->device) {
        esp_err_t error = usb_host_device_close(state->client, state->device);
        if (error != ESP_OK) {
            ESP_LOGD(TAG, "Device close pending: %s", esp_err_to_name(error));
            return;
        }
        state->device = NULL;
    }
    memset(&state->endpoints, 0, sizeof(state->endpoints));
    state->closing = false;
    state->cancel_started = false;
    ESP_LOGI(TAG, "Waiting for USB MIDI device");
}

static void open_device(midi_host_t *state)
{
    uint8_t address = state->pending_address;
    state->pending_address = 0;
    ++state->session;
    esp_err_t error = usb_host_device_open(state->client, address, &state->device);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Open address %u failed: %s", address, esp_err_to_name(error));
        return;
    }
    const usb_device_desc_t *device_desc;
    const usb_config_desc_t *config_desc;
    error = usb_host_get_device_descriptor(state->device, &device_desc);
    if (error != ESP_OK) {
        goto failed;
    }
    ESP_LOGI(TAG, "USB address=%u VID=%04x PID=%04x", address,
             device_desc->idVendor, device_desc->idProduct);
    error = usb_host_get_active_config_descriptor(state->device, &config_desc);
    if (error != ESP_OK) {
        goto failed;
    }
    if (!fm1_usb_find_endpoints((const uint8_t *)config_desc, config_desc->wTotalLength,
                               &state->endpoints)) {
        ESP_LOGW(TAG, "Malformed configuration descriptor; rejected");
        request_close(state);
        return;
    }
    if (state->endpoints.audio_out_found) {
        ESP_LOGI(TAG, "AudioStreaming ISO OUT advertised: interface=%u alt=%u ep=0x%02x mps=%u",
                 state->endpoints.audio_interface, state->endpoints.audio_alt,
                 state->endpoints.audio_ep, state->endpoints.audio_max_packet);
#if CONFIG_FM1_ENABLE_UAC_TX
        ESP_LOGI(TAG, "UAC client will separately validate the audio format/rate before TX");
#else
        ESP_LOGI(TAG, "Audio format/rate not validated; audio TX is disabled in this build");
#endif
    } else {
        ESP_LOGW(TAG, "No AudioStreaming ISO OUT: expected on current SLOOP; audio return needs SLOOP changes");
    }
    if (!state->endpoints.midi_in_found || state->endpoints.midi_alt != 0 ||
        state->endpoints.midi_max_packet == 0 || state->endpoints.midi_max_packet > 64 ||
        state->endpoints.midi_max_packet % 4 != 0) {
        ESP_LOGW(TAG, "No supported USB-MIDI 1.0 bulk IN at alternate setting 0");
        request_close(state);
        return;
    }
    error = usb_host_interface_claim(state->client, state->device,
                                    state->endpoints.midi_interface, state->endpoints.midi_alt);
    if (error != ESP_OK) {
        goto failed;
    }
    state->claimed = true;
    error = usb_host_transfer_alloc(state->endpoints.midi_max_packet, 0, &state->transfer);
    if (error != ESP_OK) {
        goto failed;
    }
    state->transfer->device_handle = state->device;
    state->transfer->bEndpointAddress = state->endpoints.midi_ep;
    state->transfer->num_bytes = state->endpoints.midi_max_packet;
    state->transfer->callback = transfer_complete;
    state->transfer->context = state;
    if (state->endpoints.midi_out_found) {
        error = usb_host_transfer_alloc(CART_USB_SIZE, 0, &state->tx);
        if (error != ESP_OK) goto failed;
        state->tx->device_handle = state->device;
        state->tx->bEndpointAddress = state->endpoints.midi_out_ep;
        state->tx->callback = tx_complete;
        state->tx->context = state;
    }
    ESP_LOGI(TAG, "MIDI ready: interface=%u alt=%u bulk IN=0x%02x mps=%u",
             state->endpoints.midi_interface, state->endpoints.midi_alt,
             state->endpoints.midi_ep, state->endpoints.midi_max_packet);
    return;

failed:
    ESP_LOGW(TAG, "USB setup failed: %s", esp_err_to_name(error));
    request_close(state);
}

static void client_task(void *arg)
{
    midi_host_t *state = arg;
    for (;;) {
        esp_err_t error = usb_host_client_handle_events(state->client, pdMS_TO_TICKS(10));
        if (error != ESP_OK && error != ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "Client events: %s", esp_err_to_name(error));
        }
        if (state->closing) {
            close_device(state);
        }
        if (!state->device && state->pending_address) {
            open_device(state);
        }
        if (!state->tx_in_flight) {
            reply_t reply;
            while (xQueueReceive(reply_queue, &reply, 0) == pdTRUE) {
                if (!state->tx || state->closing || reply.session != state->session) continue;
                state->tx->num_bytes = fm1_sysex_encode(reply.frame, sizeof reply.frame,
                                                       state->tx->data_buffer, CART_USB_SIZE);
                error = usb_host_transfer_submit(state->tx);
                if (error == ESP_OK) state->tx_in_flight = true;
                else request_close(state);
                break;
            }
        }
        if (state->transfer && !state->in_flight && !state->closing) {
            error = usb_host_transfer_submit(state->transfer);
            if (error == ESP_OK) {
                state->in_flight = true;
            } else {
                ESP_LOGW(TAG, "Submit failed: %s; reconnect device to retry", esp_err_to_name(error));
                request_close(state);
            }
        }
    }
}

static void daemon_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t flags;
        esp_err_t error = usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (error != ESP_OK && error != ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "Host events: %s", esp_err_to_name(error));
        }
    }
}

esp_err_t fm1_usb_midi_start(const fm1_usb_midi_callbacks_t *callbacks)
{
    if (started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!callbacks) {
        return ESP_ERR_INVALID_ARG;
    }
    const usb_host_config_t config = {
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
#if CONFIG_FM1_ENABLE_UAC_TX
        /* IDF 5.3.6's bias calculation gives the S3 only 32 periodic words
         * (128 bytes), despite Kconfig's older 192-byte description. Sloop
         * needs MPS=180. Use explicit sizes so cached Kconfig cannot silently
         * prevent usb_host_interface_claim from opening the UAC endpoint. */
        .fifo_settings_custom = {
            .rx_fifo_lines = FM1_USB_RX_FIFO_LINES,
            .nptx_fifo_lines = FM1_USB_NPTX_FIFO_LINES,
            .ptx_fifo_lines = FM1_USB_PTX_FIFO_LINES,
        },
#endif
    };
#if CONFIG_FM1_ENABLE_UAC_TX
    ESP_LOGI(TAG, "USB FIFO limits: IN=%u, control/bulk OUT=%u, audio OUT=%u bytes (Sloop requires 180)",
             (unsigned)FM1_USB_IN_MAX_PACKET, (unsigned)FM1_USB_NON_PERIODIC_OUT_MAX_PACKET,
             (unsigned)FM1_USB_PERIODIC_OUT_MAX_PACKET);
#endif
    esp_err_t error = usb_host_install(&config);
    if (error != ESP_OK) {
        return error;
    }
    reply_queue = xQueueCreate(8, sizeof(reply_t));
    if (!reply_queue) { usb_host_uninstall(); return ESP_ERR_NO_MEM; }
    host.callbacks = *callbacks;
    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 8,
        .async = { .client_event_callback = client_event, .callback_arg = &host },
    };
    error = usb_host_client_register(&client_config, &host.client);
    if (error != ESP_OK) {
        /* No daemon has started, so no device can have been enumerated yet. */
        usb_host_uninstall();
        return error;
    }
    if (callbacks->host_ready) {
        /* A startup error is propagated to app_main's fatal error check. */
        error = callbacks->host_ready(callbacks->context);
        if (error != ESP_OK) {
            return error;
        }
    }
    started = true;
    /* Task allocation failure is fatal at boot. Do not partially uninstall a
     * live host or leave an orphaned daemon operating a freed host object. */
    ESP_ERROR_CHECK(xTaskCreatePinnedToCore(daemon_task, "usb_daemon", 4096, NULL, 20,
                                          NULL, 0) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreatePinnedToCore(client_task, "usb_midi", 4096, &host, 10,
                                          NULL, 0) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_LOGI(TAG, "USB host running; board must be powered independently");
    return ESP_OK;
}
