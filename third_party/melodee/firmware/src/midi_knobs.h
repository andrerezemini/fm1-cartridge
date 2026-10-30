/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
#ifndef MELODEE_MIDI_KNOBS_H
#define MELODEE_MIDI_KNOBS_H
enum { MK_OFF, MK_GENERAL, MK_MINILAB, MK_PROFILES };
enum { MK_ABSOLUTE, MK_REL1, MK_REL2, MK_REL3, MK_MODES };
/* Device preferences, independent of projects. Opt in to avoid stealing synth CCs. */
static volatile uint8_t settings_midi_knobs, settings_midi_knob_mode;
#endif
