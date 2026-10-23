/* SPDX-License-Identifier: GPL-3.0-only */
/* Cartridge server: the FM-1's GLO > CARTRIDGE page edits whichever synth is linked, and picks the
 * firmware slot the ESP32 boots next. Platform slots are optional (NULL on the host). */
#pragma once
#include "fm1/cartridge_wire.h"
#include "fm1_synth.h"

typedef struct {
    uint8_t (*count)(void);              /* app slots in the partition table (1..CART_SLOTS_MAX) */
    uint8_t (*current)(void);            /* the slot running now */
    const char *(*name)(uint8_t slot);   /* the firmware's name in that slot, "EMPTY" when empty/invalid */
    int (*bootable)(uint8_t slot);       /* 1 when a valid image is there */
} fm1_slots_t;
#define CART_SLOTS_MAX 8u

typedef struct {
    const fm1_synth_api_t *synth;
    const fm1_slots_t *slots;
    uint16_t revision, parameter;
    uint8_t slot_cursor;                 /* catalogue entry FIRMWARE: the slot picked (current until turned) */
    uint8_t slot_dirty;                  /* the cursor changed since the platform last looked (its 2 s timer) */
    uint8_t cached, request[CART_REQUEST_SIZE], reply[CART_REPLY_SIZE];
} cartridge_remote_t;

/* All calls from the synth owner. Seed distinguishes ESP32 boot sessions. */
void cartridge_remote_init(cartridge_remote_t *r, const fm1_synth_api_t *synth, const fm1_slots_t *slots, uint16_t seed);
/* 0 rejects unrelated/malformed frames. A valid command always yields a full
 * snapshot, including stale-revision errors. Exact retries are idempotent. */
int cartridge_remote_handle(cartridge_remote_t *r, const uint8_t *request, size_t n, uint8_t reply[CART_REPLY_SIZE]);
/* the catalogue as the FM-1 sees it: entry 0 MIDI CH (the synth's), entry 1 FIRMWARE (when the
 * platform has more than one slot), then the synth's own entries */
uint32_t cartridge_remote_param_count(const cartridge_remote_t *r);
int cartridge_remote_param_get(const cartridge_remote_t *r, uint32_t index, fm1_param_t *out);
int cartridge_remote_param_set(cartridge_remote_t *r, uint32_t index, int32_t value);
