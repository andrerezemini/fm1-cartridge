/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef FM1_USB_MIDI_H
#define FM1_USB_MIDI_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t cable, status, data1, data2, length;
} fm1_midi_event_t;

/* Decode one complete USB-MIDI 1.0 event packet. SysEx and reserved CINs
 * are deliberately unsupported. The caller must reject partial packets. */
bool fm1_midi_decode(const uint8_t packet[4], fm1_midi_event_t *event);
#endif
