/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "cartridge_wire.h"
#include <stdbool.h>
typedef struct { uint8_t bytes[CART_REPLY_SIZE]; size_t length; bool active; } fm1_sysex_t;
/* Cable 0 only; completed frame remains valid until the next packet. */
bool fm1_sysex_packet(fm1_sysex_t *s, const uint8_t packet[4]);
size_t fm1_sysex_encode(const uint8_t *frame, size_t n, uint8_t *usb, size_t capacity);
