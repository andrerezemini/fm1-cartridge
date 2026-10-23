/* SPDX-License-Identifier: GPL-3.0-only */
/* The firmware slots of partitions.csv (ota_0..ota_N): which one runs, what the others hold,
 * switching to another one and confirming this one to the bootloader. */
#pragma once
#include <stdint.h>
#include "cartridge_remote.h"

/* app_main, before USB: enumerate the app partitions and read their descriptors (names). */
void fm1_slots_init(void);
/* the slots as the cartridge server lists them; NULL when the table has a single app partition */
const fm1_slots_t *fm1_slots(void);
/* The FM-1 answered: this image stays. Idempotent; a no-op unless the bootloader marked this
 * boot pending (rollback enabled). Main-loop context: writes otadata. */
void fm1_slots_confirm(void);
/* Set slot as the boot partition (its image is verified first) and restart ~300 ms later, so the
 * reply in flight reaches the FM-1. -1 if the slot is invalid or the image does not verify. */
int fm1_slots_boot(uint8_t slot);
