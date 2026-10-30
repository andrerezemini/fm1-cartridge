/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "fm1/usb_midi.h"

/* Install after usb_host_install, before starting host event processing. */
esp_err_t fm1_audio_install(void *unused);
/* Nonblocking calls, safe from the MIDI client task. */
void fm1_audio_midi(const fm1_midi_event_t *event);
void fm1_audio_panic(void);

#include "fm1/cartridge_wire.h"
void fm1_audio_request(const uint8_t frame[CART_REQUEST_SIZE], uint32_t session);

/* The audio task's figures, read by the main loop (which prints them on the UART). */
typedef struct {
    bool streaming;
    uint32_t frames;          /* PCM frames handed to the UAC driver since the stream started */
    uint32_t underruns;       /* times the render fell behind the USB frames (estimated from the frames
                               * written against the time elapsed: the host's SOF and the timer share a clock) */
    uint32_t write_retries;   /* writes that timed out on a full ring (normal back-pressure) */
    uint32_t render_max_us;   /* longest render of AUDIO_FRAMES (64: 1451 us of audio) since the last read */
    uint32_t pass_max_us;     /* longest audio-loop pass since the last read (render, MIDI, a request) */
    uint32_t restarts;        /* UAC stream restarts after a transfer error */
} fm1_audio_stats_t;
void fm1_audio_stats(fm1_audio_stats_t *out);
/* The cartridge server has answered the FM-1 at least once: this firmware works with it (the main loop
 * then confirms the slot to the bootloader). Always false in the bench profiles without a synthesizer. */
bool fm1_audio_answered(void);
/* A sound record waiting to be written to NVS (the main loop writes it, then calls done with whether
 * it was written: a record not written is tried again after the next quiet seconds, up to three times). */
bool fm1_audio_persist_pending(const char **name, const void **record, size_t *size);
void fm1_audio_persist_done(bool saved);
/* A reading of the cell in mV (CONFIG_FM1_BATTERY), from the main loop: the audio task filters it,
 * reports it to the FM-1 and fades its audio out at the cutoff. */
void fm1_audio_battery(uint32_t mv);
