/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include "esp_err.h"
#include "fm1/usb_midi.h"
#include "fm1/cartridge_wire.h"

/* Callbacks run in the USB client task, never in the ISR. They must not block;
 * copy events to a bounded queue before using them in a synthesis task.
 * disconnect is also delivered on transport failure: release every voice. */
typedef struct {
    /* Optional hook to register additional host clients before enumeration. */
    esp_err_t (*host_ready)(void *context);
    void (*event)(const fm1_midi_event_t *event, void *context);
    void (*request)(const uint8_t frame[CART_REQUEST_SIZE], uint32_t session, void *context);
    void (*disconnect)(void *context);
    void *context;
} fm1_usb_midi_callbacks_t;

/* Nonblocking, task-safe. The session tag prevents an old reply leaking into
 * a new connection. Called only after a request callback provided that tag. */
bool fm1_usb_midi_reply(const uint8_t frame[CART_REPLY_SIZE], uint32_t session);

/* Start once. This bench firmware keeps the host service alive until reset.
 * GPIO19 = D-, GPIO20 = D+; there is no software-controlled 5 V supply. */
esp_err_t fm1_usb_midi_start(const fm1_usb_midi_callbacks_t *callbacks);
