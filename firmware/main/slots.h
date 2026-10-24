/* SPDX-License-Identifier: GPL-3.0-only */
/* The firmware slots of partitions.csv (ota_0..ota_N): which one runs, what the others hold,
 * switching to another one and confirming this one to the bootloader. */
#pragma once
#include <stdint.h>
#include "cartridge_remote.h"

/* app_main, before USB: enumerate the app partitions, read their descriptors (names) and verify the
 * images of the slots not running (a damaged one is never offered). */
void fm1_slots_init(void);
/* the slots as the cartridge server lists them; NULL when the table has a single app partition */
const fm1_slots_t *fm1_slots(void);
/* This firmware's cartridge server answered the FM-1 (audio_tx.c fm1_audio_answered): this image stays.
 * Idempotent; a no-op unless the bootloader marked this boot pending (rollback enabled). Main-loop
 * context: writes otadata. The profiles without a synthesizer never call it, so they roll back. */
void fm1_slots_confirm(void);
/* Set slot as the boot partition (its image is verified first) and restart ~300 ms later, so the
 * reply in flight reaches the FM-1. -1 if the slot is invalid or the image does not verify. */
int fm1_slots_boot(uint8_t slot);
