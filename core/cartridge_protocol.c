/* SPDX-License-Identifier: GPL-3.0-only */
#include "fm1/cartridge_protocol.h"
bool fm1_sysex_packet(fm1_sysex_t *s, const uint8_t p[4])
{
    if (!s || !p || (p[0] >> 4)) return false;
    unsigned cin = p[0] & 15u;
    if (cin == 15 && p[1] >= 0xf8) return false;
    if (cin < 4 || cin > 7) { s->active = false; return false; }
    unsigned n = cin == 4 || cin == 7 ? 3 : cin - 4;
    bool complete = false;
    for (unsigned i = 1; i <= n; ++i) {
        uint8_t b = p[i];
        if (b >= 0xf8) continue;
        if (b == 0xf0) { s->length = 0; s->active = true; }
        if (!s->active) continue;
        if ((b & 0x80) && b != 0xf0 && b != 0xf7) { s->active = false; continue; }
        if (s->length == sizeof s->bytes) { s->active = false; continue; }
        s->bytes[s->length++] = b;
        if (b == 0xf7) { s->active = false; complete = cin != 4 && i == n; }
    }
    if (cin != 4) s->active = false;
    return complete;
}
size_t fm1_sysex_encode(const uint8_t *p, size_t n, uint8_t *out, size_t cap)
{
    if (!cart_valid(p, n) || !out || ((n + 2) / 3) * 4 > cap) return 0;
    size_t at = 0;
    for (size_t i = 0; i < n;) {
        size_t k = n - i < 3 ? n - i : 3;
        out[at++] = i + k < n ? 4 : (uint8_t)(4 + k);
        for (size_t j = 0; j < 3; ++j) out[at++] = j < k ? p[i + j] : 0;
        i += k;
    }
    return at;
}
