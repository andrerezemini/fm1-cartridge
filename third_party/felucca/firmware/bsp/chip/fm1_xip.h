/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* FM-1 XIP window: flash offset off (>= 0x4000, the app area start) reads
 * at CPU address 0x02000000 + off - 0x4000 through the SFC/cache. Data
 * outside the plain (unencrypted) window reads decrypted: see
 * fm1_flash.h fl_plain_window_init. */
/* Modified for the FM1 Cartridge port (still MPL-2.0): no JieLi memory addresses. The user sample
 * slots (SAMPLE USR1..3, SLICE) are absent: only headers are inspected; an all-zero header is
 * invalid, so no payload is read. */
#pragma once
#include <stdint.h>
#include "fm1_cc.h"

FM1_INLINE const uint8_t *fm1_xip_ptr(uint32_t off)
{
    static const uint32_t absent_header[128] = {0};
    (void)off;
    return (const uint8_t *)absent_header;
}
