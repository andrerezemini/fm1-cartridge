/* SPDX-License-Identifier: GPL-3.0-only */
#include "fm1/usb_midi.h"

bool fm1_midi_decode(const uint8_t p[4], fm1_midi_event_t *e)
{
    if (!p || !e) return false;
    uint8_t cin = p[0] & 15u, status = p[1], length = 0;
    if (cin >= 8 && cin <= 14) {
        if ((status >> 4) != cin) return false;
        length = (cin == 12 || cin == 13) ? 2 : 3;
    } else if (cin == 15) {
        if (status != 0xf8 && status != 0xfa && status != 0xfb &&
            status != 0xfc && status != 0xfe && status != 0xff) return false;
        length = 1;
    } else if (cin == 2 && (status == 0xf1 || status == 0xf3)) {
        length = 2;
    } else if (cin == 3 && status == 0xf2) {
        length = 3;
    } else if (cin == 5 && status == 0xf6) {
        length = 1;
    } else {
        return false;
    }
    if ((length >= 2 && p[2] >= 0x80) || (length == 3 && p[3] >= 0x80))
        return false;
    *e = (fm1_midi_event_t){ .cable = p[0] >> 4, .status = status,
        .data1 = length >= 2 ? p[2] : 0, .data2 = length == 3 ? p[3] : 0,
        .length = length };
    return true;
}
