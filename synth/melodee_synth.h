/* SPDX-License-Identifier: GPL-3.0-only */
/* Portable adapter to the actual Melodee 1.4.5 DSP (keremimo/melodee): one part (Prophet, FM6,
 * CZ-1, SID, DRUM), played over USB-MIDI by the FM-1, rendered on the cartridge on one core.
 * Same contract as clipper_synth.h, through fm1_synth.h. */
#ifndef FM1_CARTRIDGE_MELODEE_SYNTH_H
#define FM1_CARTRIDGE_MELODEE_SYNTH_H
#include "fm1_synth.h"
#ifdef __cplusplus
extern "C" {
#endif
const fm1_synth_api_t *melodee_synth_api(void);
#ifdef __cplusplus
}
#endif
#endif
