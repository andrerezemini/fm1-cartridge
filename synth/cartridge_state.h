/* SPDX-License-Identifier: GPL-3.0-only */
/* The cartridge's sound as a small record: engine, preset, MIDI channel and every catalogue value, so
 * the platform can keep it across reboots and slot switches (the ESP32 stores it in NVS, one record
 * per firmware). Only the synth interface is used; all calls from the synth owner. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "fm1_synth.h"

#define CART_STATE_MAGIC 0x31545343u     /* "CST1" */
#define CART_STATE_VALUES 512u

typedef struct {
    uint32_t magic;
    uint32_t revision;                    /* FNV-1a of the synth's name and upstream revision */
    uint16_t engine, preset, midi_channel, count;
    struct { int16_t value; uint16_t name; } entry[CART_STATE_VALUES];   /* the catalogue's values, entry 0
                                          * (MIDI CH) included, each with a hash of its name */
} cartridge_state_t;

/* The record of the sound now; its size in bytes (only the values in use), 0 when the catalogue is
 * larger than CART_STATE_VALUES. */
size_t cartridge_state_capture(const fm1_synth_api_t *synth, cartridge_state_t *out);
/* Loads a record taken from the same synthesizer: the preset, the MIDI channel, then every value
 * whose entry still has the same name and range (twice: a TYPE value reshapes the entries after
 * it). -1 (nothing changed) for another synthesizer or revision, a short or malformed record, or an
 * engine or preset that no longer exists. */
int cartridge_state_apply(const fm1_synth_api_t *synth, const cartridge_state_t *in, size_t size);
/* Two records describe the same sound (the platform writes only when it differs). */
int cartridge_state_equal(const cartridge_state_t *a, size_t a_size, const cartridge_state_t *b, size_t b_size);
