/* SPDX-License-Identifier: GPL-3.0-only
 * Derived from Clipper fm1_xip.h, Copyright (C) 2026 Leo Kuroshita.
 * FM1 Cartridge port: no JieLi memory addresses. User flash banks are absent.
 * Only headers are inspected; invalid zero magic prevents payload accesses. */
#pragma once
#include <stdint.h>
static inline const uint8_t *fm1_xip_ptr(uint32_t off)
{
    static const uint32_t absent_header[128] = {0};
    (void)off;
    return (const uint8_t *)absent_header;
}
