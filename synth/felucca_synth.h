/* SPDX-License-Identifier: GPL-3.0-only */
/* Portable adapter to the actual Felucca 1.6.1 DSP (hugelton/Felucca): one part of the four-part
 * instrument, played over USB-MIDI by the FM-1, rendered on the cartridge. Same contract as
 * clipper_synth.h, through fm1_synth.h. */
#ifndef FM1_CARTRIDGE_FELUCCA_SYNTH_H
#define FM1_CARTRIDGE_FELUCCA_SYNTH_H
#include "fm1_synth.h"
#ifdef __cplusplus
extern "C" {
#endif
const fm1_synth_api_t *felucca_synth_api(void);
#ifdef __cplusplus
}
#endif
#endif
