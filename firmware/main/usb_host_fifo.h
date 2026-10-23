/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef FM1_USB_HOST_FIFO_H
#define FM1_USB_HOST_FIFO_H

/* ESP32-S3 DWC host FIFO sizing, in 32-bit words. The RX FIFO reserves two
 * words for status. Keep the allocation within the 200-word usable budget. */
enum {
    FM1_USB_RX_FIFO_LINES = 104,
    FM1_USB_NPTX_FIFO_LINES = 48,
    FM1_USB_PTX_FIFO_LINES = 48,
    FM1_USB_IN_MAX_PACKET = (FM1_USB_RX_FIFO_LINES - 2) * 4,
    FM1_USB_NON_PERIODIC_OUT_MAX_PACKET = FM1_USB_NPTX_FIFO_LINES * 4,
    FM1_USB_PERIODIC_OUT_MAX_PACKET = FM1_USB_PTX_FIFO_LINES * 4,
};

_Static_assert(FM1_USB_RX_FIFO_LINES + FM1_USB_NPTX_FIFO_LINES +
               FM1_USB_PTX_FIFO_LINES <= 200, "ESP32-S3 USB FIFO budget exceeded");

#endif
