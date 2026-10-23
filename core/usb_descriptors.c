/* SPDX-License-Identifier: GPL-3.0-only */
#include "fm1/usb_descriptors.h"
#include <string.h>

static uint16_t le16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }

bool fm1_usb_find_endpoints(const uint8_t *d, size_t n, fm1_usb_endpoints_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!d || n < 9 || d[0] != 9 || d[1] != 2) return false;
    size_t total = le16(d + 2);
    if (total < 9 || total > n) return false;
    uint8_t iface = 0, alt = 0, cls = 0, sub = 0, protocol = 0;
    bool have_interface = false;
    fm1_usb_endpoints_t result = {0};
    for (size_t pos = 9; pos < total;) {
        if (total - pos < 2 || d[pos] < 2 || d[pos] > total - pos) return false;
        const uint8_t *p = d + pos;
        if (p[1] == 4) {
            if (p[0] < 9) return false;
            iface = p[2]; alt = p[3]; cls = p[5]; sub = p[6]; protocol = p[7];
            have_interface = true;
        } else if (p[1] == 5) {
            if (p[0] < 7 || !have_interface) return false;
            uint8_t ep = p[2], type = p[3] & 3u;
            uint16_t mps = le16(p + 4);
            if (!(ep & 15u) || (ep & 0x70u) || !mps || mps > 1023) return false;
            if (cls == 1 && sub == 3 && protocol == 0 && type == 2 &&
                (ep & 0x80u) && !alt && mps <= 64 && !result.midi_in_found) {
                result.midi_in_found = true;
                result.midi_interface = iface; result.midi_alt = alt;
                result.midi_ep = ep; result.midi_max_packet = mps;
            }
            if (cls == 1 && sub == 2 && protocol == 0 && type == 1 &&
                !(ep & 0x80u) && alt && !result.audio_out_found) {
                result.audio_out_found = true;
                result.audio_interface = iface; result.audio_alt = alt;
                result.audio_ep = ep; result.audio_max_packet = mps;
            }
        }
        pos += p[0];
    }
    /* A second pass also handles an OUT descriptor placed before IN. Only
     * pair endpoints on the very same MIDI interface/alternate setting. */
    if (result.midi_in_found) {
        have_interface = false;
        for (size_t pos = 9; pos < total; pos += d[pos]) {
            const uint8_t *p = d + pos;
            if (p[1] == 4) {
                have_interface = p[2] == result.midi_interface && p[3] == result.midi_alt;
            } else if (p[1] == 5 && have_interface && !(p[2] & 0x80u) &&
                       (p[3] & 3u) == 2 && le16(p + 4) <= 64 &&
                       le16(p + 4) % 4 == 0 && !result.midi_out_found) {
                result.midi_out_found = true;
                result.midi_out_ep = p[2]; result.midi_out_max_packet = le16(p + 4);
            }
        }
    }
    *out = result;
    return true;
}
