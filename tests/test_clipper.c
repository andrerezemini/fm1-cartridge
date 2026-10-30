/* SPDX-License-Identifier: GPL-3.0-only */
#include "clipper_synth.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int16_t audio[8192 * 2], reference[8192 * 2];
static uint64_t energy(const int16_t *samples, size_t n)
{
    uint64_t value = 0;
    for (size_t i = 0; i < n; ++i)
        value += (int64_t)samples[i] * samples[i];
    return value;
}

int main(void)
{
    memset(audio, 0x5A, sizeof audio);
    clipper_synth_render(audio, 8192);
    assert(energy(audio, 16384) == 0);
    assert(clipper_synth_init(48000) == -1);
    assert(clipper_synth_init(44100) == 0);
    assert(clipper_synth_engine_count() == 23);
    assert(strcmp(clipper_synth_upstream_revision(), "fbd8434e7b501ec5d772f5c5f656c25735929369") == 0);
    assert(clipper_synth_set_preset(999, 0) == -1);
    assert(clipper_synth_set_preset(0, 999) == -1);
    assert(clipper_synth_engine_name(999) == NULL);
    assert(clipper_synth_preset_name(0, 999) == NULL);
    assert(clipper_synth_midi(0xF0, 0, 0) == -1);
    assert(clipper_synth_midi(0x90, 128, 1) == -1);
    assert(clipper_synth_midi(0xC0, 0, 0) == -1);
    assert(clipper_synth_midi(0xB0, 20, 0) == -1);

    /* Every real upstream engine must emit nonzero PCM using a factory sound. */
    for (uint32_t e = 0; e < clipper_synth_engine_count(); ++e) {
        assert(clipper_synth_preset_count(e) > 0);
        assert(clipper_synth_set_preset(e, 0) == 0);
        assert(clipper_synth_midi(0x95, 60, 110) == 0); /* Synth mode is omni. */
        clipper_synth_render(audio, 8192);
        uint64_t power = energy(audio, 16384);
        clipper_synth_render(audio, 8192);       /* (WAVE: silent while its table builds) */
        if (energy(audio, 16384) > power) power = energy(audio, 16384);
        printf("engine %2u %-10s %-12s energy=%llu\n", e,
            clipper_synth_engine_name(e), clipper_synth_preset_name(e, 0),
            (unsigned long long)power);
        assert(power > 100000);
        clipper_synth_panic();
        clipper_synth_render(audio, 8192);
        assert(energy(audio, 16384) == 0);
    }

    /* The port's input channel: OMNI by default; 1..16 answers that channel only, live from the
     * catalogue's first entry too. */
    assert(clipper_synth_init(44100) == 0 && clipper_synth_set_preset(0, 0) == 0);
    assert(clipper_synth_set_midi_channel(17) == -1 && clipper_synth_midi_channel() == 0);
    assert(clipper_synth_set_midi_channel(1) == 0 && clipper_synth_midi_channel() == 1);
    assert(clipper_synth_midi(0x95, 60, 110) == 0);
    clipper_synth_render(audio, 8192);
    assert(energy(audio, 16384) == 0);
    assert(clipper_synth_midi(0x90, 60, 110) == 0);
    clipper_synth_render(audio, 8192);
    assert(energy(audio, 16384) > 100000);
    clipper_synth_panic();
    {
        clipper_param_t ch;
        assert(!clipper_synth_param_get(0, &ch) && !strcmp(ch.name, "MIDI CH") && ch.value == 1 && !strcmp(ch.text, "1"));
        assert(!clipper_synth_param_set(0, 6) && clipper_synth_midi_channel() == 6);
        assert(clipper_synth_midi(0x95, 60, 110) == 0);
        clipper_synth_render(audio, 8192);
        assert(energy(audio, 16384) > 100000);
        clipper_synth_panic();
        assert(!clipper_synth_param_set(0, 0) && clipper_synth_midi_channel() == 0 &&
               !clipper_synth_param_get(0, &ch) && !strcmp(ch.text, "OMNI"));
    }

    /* Same audio for arbitrary caller chunk sizes: preserve CTL=32 cadence. */
    assert(clipper_synth_init(44100) == 0);
    assert(clipper_synth_set_preset(0, 0) == 0);
    assert(clipper_synth_midi(0x90, 60, 100) == 0);
    clipper_synth_render(reference, 8192);
    assert(clipper_synth_init(44100) == 0);
    assert(clipper_synth_set_preset(0, 0) == 0);
    assert(clipper_synth_midi(0x90, 60, 100) == 0);
    for (size_t offset = 0; offset < 8192;) {
        size_t n = offset % 97 + 1;
        if (n > 8192 - offset) n = 8192 - offset;
        clipper_synth_render(audio + offset * 2, n);
        offset += n;
    }
    assert(memcmp(audio, reference, sizeof audio) == 0);

    /* MIDI note-off and note-on velocity zero release actual voices. */
    assert(clipper_synth_midi(0xB0, 72, 0) == 0);
    assert(clipper_synth_midi(0x80, 60, 0) == 0);
    for (unsigned i = 0; i < 16; ++i) clipper_synth_render(audio, 8192);
    assert(energy(audio, 16384) < 16384 * 16);
    assert(clipper_synth_midi(0x90, 64, 100) == 0);
    clipper_synth_render(audio, 8192);
    assert(energy(audio, 16384) > 100000);
    assert(clipper_synth_midi(0x90, 64, 0) == 0);
    for (unsigned i = 0; i < 16; ++i) clipper_synth_render(audio, 8192);
    assert(energy(audio, 16384) < 16384 * 16);

    /* Queue saturation must clear the current voice, even if note-off is lost. */
    assert(clipper_synth_midi(0x90, 67, 127) == 0);
    clipper_synth_render(audio, 100);
    for (unsigned i = 0; i < 64; ++i)
        assert(clipper_synth_midi(0xF8, 0, 0) == 0);
    assert(clipper_synth_midi(0x80, 67, 0) == -2);
    clipper_synth_render(audio, 8192);
    assert(energy(audio, 16384) == 0);
    assert(clipper_synth_midi(0x90, 67, 127) == 0);
    clipper_synth_render(audio, 8192);
    assert(energy(audio, 16384) > 100000);
    assert(clipper_synth_midi(0xB0, 120, 0) == 0);
    clipper_synth_render(audio, 8192);
    assert(energy(audio, 16384) == 0);
    /* WAVE's tables, built in bounded steps per block, equal upstream's copies for every factory table,
     * and are ready within half a second of audio. */
    for (uint32_t k = 0; k < 16; ++k) {
        int blocks = clipper_synth_wavetable_check(k);
        assert(blocks > 0 && blocks * 32 < 44100 / 2);
        if (!k) printf("WAVE table build: %d blocks (%d ms of audio)\n", blocks, blocks * 32 * 1000 / 44100);
    }
    /* A WAVE note whose table changes under it is silent while the new table builds, then comes back
     * through the part's fade-in, not as a step from silence to its level. */
    {
        uint32_t wave = 0, tabl = 0, b, i;
        clipper_param_t d;
        int silent = 0, step = 0, blocks = 0;
        int16_t last;
        for (uint32_t e = 0; e < clipper_synth_engine_count(); ++e)
            if (!strcmp(clipper_synth_engine_name(e), "WAVE")) wave = e;
        assert(wave && clipper_synth_init(44100) == 0 && clipper_synth_set_preset(wave, 0) == 0);
        for (i = 0; i < clipper_synth_param_count(); ++i)
            if (!clipper_synth_param_get(i, &d) && !strcmp(d.name, "ENG TABL")) tabl = i;
        assert(tabl);
        assert(clipper_synth_midi(0x90, 60, 100) == 0);
        for (b = 0; b < 400; ++b) clipper_synth_render(audio, 32);    /* the table built, the note held */
        assert(energy(audio, 64) > 64u * 100000u);
        assert(!clipper_synth_param_get(tabl, &d) && !clipper_synth_param_set(tabl, d.value + 1));
        last = audio[62];
        for (b = 0; b < 400 && blocks < 2; ++b) {          /* (silent: no sample moves by more than 1) */
            int jump = 0;
            clipper_synth_render(audio, 32);
            for (i = 0; i < 64; i += 2) {
                if (abs(audio[i] - last) > jump) jump = abs(audio[i] - last);
                last = audio[i];
            }
            if (!silent)
                silent = jump <= 1;
            else if (jump > 1 && ++blocks && jump > step)
                step = jump;
        }
        printf("WAVE table change: back after %u blocks, largest step %d as it comes in\n", b, step);
        assert(silent && blocks == 2 && step < 400);
    }
    /* FMX's PTCH and ALG name lists end with a 0, which upstream's F_INT formatter counts up to:
     * every value, ALG past 32 too, reads its own name (F1..F16, B1..B27; PAT, 1..40). */
    {
        uint32_t fmx = 0, ptch = 0, alg = 0, i;
        clipper_param_t d;
        char want[8];
        for (uint32_t e = 0; e < clipper_synth_engine_count(); ++e)
            if (!strcmp(clipper_synth_engine_name(e), "FMX")) fmx = e;
        assert(fmx && clipper_synth_init(44100) == 0 && clipper_synth_set_preset(fmx, 0) == 0);
        for (i = 0; i < clipper_synth_param_count(); ++i) {
            if (clipper_synth_param_get(i, &d)) continue;
            if (!strcmp(d.name, "ENG PTCH")) ptch = i;
            if (!strcmp(d.name, "ENG ALG")) alg = i;
        }
        assert(ptch && alg);
        assert(!clipper_synth_param_get(ptch, &d) && d.min == 0 && d.max == 42);
        for (int32_t v = d.min; v <= d.max; ++v) {
            snprintf(want, sizeof want, v < 16 ? "F%d" : "B%d", v < 16 ? v + 1 : v - 15);
            assert(!clipper_synth_param_set(ptch, v) && !clipper_synth_param_get(ptch, &d) && !strcmp(d.text, want));
        }
        assert(!clipper_synth_param_get(alg, &d) && d.min == 0 && d.max == 40);
        for (int32_t v = d.min; v <= d.max; ++v) {
            snprintf(want, sizeof want, "%d", v);
            assert(!clipper_synth_param_set(alg, v) && !clipper_synth_param_get(alg, &d) &&
                   !strcmp(d.text, v ? want : "PAT"));
        }
        assert(clipper_synth_midi(0x90, 48, 110) == 0);   /* ALG 40 plays */
        clipper_synth_render(audio, 8192);
        assert(energy(audio, 16384) > 100000);
        clipper_synth_panic();
    }
    puts("clipper: 23 upstream engines, release, chunking, panic, recovery, the WAVE table build and FMX's name lists passed");
    return 0;
}
