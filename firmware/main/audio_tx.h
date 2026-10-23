/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "esp_err.h"
#include "fm1/usb_midi.h"

/* Install after usb_host_install, before starting host event processing. */
esp_err_t fm1_audio_install(void *unused);
/* Nonblocking calls, safe from the MIDI client task. */
void fm1_audio_midi(const fm1_midi_event_t *event);
void fm1_audio_panic(void);

#include "fm1/cartridge_wire.h"
void fm1_audio_request(const uint8_t frame[CART_REQUEST_SIZE], uint32_t session);
