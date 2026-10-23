/* SPDX-License-Identifier: GPL-3.0-only */
#include "clipper_synth.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
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
    assert(strcmp(clipper_synth_upstream_revision(), "e69067211c4690f34e494b5b896e4585163dcc2b") == 0);
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
    puts("clipper: 23 upstream engines, release, chunking, panic and recovery passed");
    return 0;
}
