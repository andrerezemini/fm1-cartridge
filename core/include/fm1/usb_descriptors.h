/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef FM1_USB_DESCRIPTORS_H
#define FM1_USB_DESCRIPTORS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool midi_in_found, midi_out_found, audio_out_found;
    uint8_t midi_interface, midi_alt, midi_ep;
    uint16_t midi_max_packet;
    uint8_t midi_out_ep;
    uint16_t midi_out_max_packet;
    uint8_t audio_interface, audio_alt, audio_ep;
    uint16_t audio_max_packet;
} fm1_usb_endpoints_t;

/* Full-speed endpoint discovery only. audio_out_found is diagnostic: it
 * does not validate UAC format, rates, topology or feedback compatibility.
 * Returns false for truncated or malformed descriptor chains. */
bool fm1_usb_find_endpoints(const uint8_t *data, size_t length,
                          fm1_usb_endpoints_t *out);
#endif
