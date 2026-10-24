/* SPDX-License-Identifier: GPL-3.0-only */
/* The sound each firmware keeps across reboots and slot switches: one NVS blob per firmware name, in
 * the nvs partition the slots share (synth/cartridge_state.h describes the record). */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* app_main, before the audio task starts. A partition from another NVS version is erased. */
esp_err_t fm1_persist_init(void);
/* The record stored under name, or 0 when there is none (or it does not fit). */
size_t fm1_persist_load(const char *name, void *record, size_t capacity);
/* Main-loop context only: the write stalls the flash cache, and with it every task, for a few ms
 * (tens for a page erase), so the audio task asks for one only after several quiet seconds. */
bool fm1_persist_save(const char *name, const void *record, size_t size);
