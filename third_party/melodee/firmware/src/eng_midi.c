/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
/* A musical track with a USB MIDI destination and no audio working state.
 * E0 is the saved 1..16 output channel; E1 additionally admits TRS input. */
static const preset_t MIDI_PRESET[] = {{.name = "MIDI Track", .e = {1, 0}}};
static const char *const MIDI_THRU[] = {"OFF", "ON"};
static const engine_t ENG_MIDI = {
    .name = "MIDI Track",
    .page_title = {"MIDI Track", "-"},
    .edit = {{"CH", F_INT, 1, 16, 1, 0, 0},
             {"TRS", F_ENUM, 0, 1, 0, MIDI_THRU, 0},
             {"-", F_INT, 0, 0, 0, 0, 0}, {"-", F_INT, 0, 0, 0, 0, 0},
             {"-", F_INT, 0, 0, 0, 0, 0}, {"-", F_INT, 0, 0, 0, 0, 0},
             {"-", F_INT, 0, 0, 0, 0, 0}, {"-", F_INT, 0, 0, 0, 0, 0}},
    .presets = MIDI_PRESET, .npresets = 1,
    .knob = {P_E0, P_E1, 255, 255},
};
/* a MIDI Track: by its engine, or by its place (tracks 5..8 are nothing else) */
static int midi_track_only(const track_t *t) { return t->eng_req == ENGI_MIDI || (t >= &trk[NPART] && t < &trk[NTRK]); }
static int midi_track_audio_disabled(uint32_t engine, uint32_t id)
{
    return engine == ENGI_MIDI && (id == P_LEVEL || id == P_PAN || id == P_SPRD ||
           param_fx(id) || param_fx_send(id));
}
