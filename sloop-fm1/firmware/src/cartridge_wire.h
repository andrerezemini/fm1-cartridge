/* SPDX-License-Identifier: GPL-3.0-only */
/* Cartridge protocol, version 3 (byte 4 of every frame). Shared verbatim between the
 * ESP32 firmware (core/include/fm1) and Sloop (firmware/src); all payload bytes are MIDI 7-bit.
 * Preset index and count are 14-bit; the reply also names the running firmware and its slot
 * (CART_FW_NAME, CART_SLOT, CART_NSLOT) and the MIDI channel the synthesizer answers
 * (CART_MIDI_CH: 0 OMNI, 1..16), which tells Sloop which track the cartridge plays.
 * A later version only appends fields before the F7: a frame with a newer version byte, and as
 * long or longer, is read as far as this version's fields go (cart_request_of; Sloop's reply
 * check). A peer with an older version is ignored. */
#ifndef FM1_CARTRIDGE_WIRE_H
#define FM1_CARTRIDGE_WIRE_H
#include <stdint.h>
#include <stddef.h>
#define CART_VERSION 3u
#define CART_REQUEST_SIZE 13u
#define CART_REPLY_SIZE 123u
#define CART_USB_SIZE 164u
enum { CART_QUERY, CART_ENGINE, CART_PRESET, CART_PARAM, CART_VALUE, CART_REPLY = 64 };
enum { CART_OK, CART_STALE, CART_BAD_REQUEST };
enum { CART_SEQ = 5, CART_OP = 7, CART_REV = 8, CART_ARG = 10,
       CART_STATUS = 10, CART_ENG = 11, CART_NENG = 12, CART_PRE = 13, CART_NPRE = 15,
       CART_PAR = 17, CART_NPAR = 19, CART_VAL = 21, CART_MIN = 23, CART_MAX = 25,
       CART_ENG_NAME = 27, CART_PRE_NAME = 39, CART_PAR_NAME = 63,
       CART_VAL_NAME = 83, CART_UNIT = 99, CART_FW_NAME = 107, CART_SLOT = 119, CART_NSLOT = 120,
       CART_MIDI_CH = 121 };
static inline uint16_t cart_u14(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 7; }
static inline int16_t cart_s14(const uint8_t *p) { return (int16_t)(cart_u14(p) - 8192); }
static inline void cart_put14(uint8_t *p, uint16_t v) { p[0] = v & 127u; p[1] = (v >> 7) & 127u; }
static inline void cart_header(uint8_t *p, uint16_t seq, uint8_t op, uint16_t rev)
{
    p[0] = 0xf0; p[1] = 0x7d; p[2] = 'C'; p[3] = 'F'; p[4] = CART_VERSION;
    cart_put14(p + CART_SEQ, seq); p[CART_OP] = op; cart_put14(p + CART_REV, rev);
}
static inline int cart_valid(const uint8_t *p, size_t n)
{
    size_t i;
    if (!p || n < CART_REQUEST_SIZE || p[0] != 0xf0 || p[1] != 0x7d ||
        p[2] != 'C' || p[3] != 'F' || p[4] < CART_VERSION || p[n - 1] != 0xf7) return 0;
    for (i = 1; i + 1 < n; ++i) if (p[i] & 0x80) return 0;
    return 1;
}
/* A request as this version reads it: the first CART_REQUEST_SIZE - 1 bytes of a valid frame (a newer
 * peer's may be longer) and its F7. 0 when frame is not a valid request. */
static inline int cart_request_of(const uint8_t *frame, size_t n, uint8_t q[CART_REQUEST_SIZE])
{
    size_t i;
    if (!cart_valid(frame, n) || frame[CART_OP] >= CART_REPLY) return 0;
    for (i = 0; i + 1 < CART_REQUEST_SIZE; ++i) q[i] = frame[i];
    q[CART_REQUEST_SIZE - 1] = 0xf7;
    return 1;
}
#endif
