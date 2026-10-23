/* SPDX-License-Identifier: GPL-3.0-only
 * Derived from Felucca fm1_xip.h, Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments.
 * FM1 Cartridge port: no JieLi memory addresses. The user sample slots (SAMPLE USR1..3, SLICE) are
 * absent: only headers are inspected; an all-zero header is invalid, so no payload is read. */
#pragma once
#include <stdint.h>
static inline const uint8_t *fm1_xip_ptr(uint32_t off)
{
    static const uint32_t absent_header[128] = {0};
    (void)off;
    return (const uint8_t *)absent_header;
}
