/* SPDX-License-Identifier: GPL-3.0-only */
/* The same checks for every port behind fm1_synth.h (tests/test_clipper.c is the Clipper's own):
 * silence before init, every offered engine sounds with its first factory preset, note-off,
 * channel filter, the MIDI CH catalogue entry, identical PCM for any chunking, panic and recovery. */
#include FM1_TEST_SYNTH_HEADER
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
    const fm1_synth_api_t *s = FM1_TEST_SYNTH_API();
    memset(audio, 0x5A, sizeof audio);
    s->render(audio, 8192);
    assert(energy(audio, 16384) == 0);
    assert(s->init(48000) == -1);
    assert(s->init(44100) == 0);
    assert(s->engine_count() == FM1_TEST_ENGINES);
    assert(strcmp(s->revision, FM1_TEST_REVISION) == 0 && strlen(s->name) <= 11);
    assert(s->set_preset(999, 0) == -1);
    assert(s->set_preset(0, 999) == -1);
    assert(s->engine_name(999) == NULL);
    assert(s->preset_name(0, 999) == NULL);
    assert(s->midi(0xF0, 0, 0) == -1);
    assert(s->midi(0x90, 128, 1) == -1);
    assert(s->midi(0xC0, 0, 0) == -1);

    /* Every offered upstream engine must emit nonzero PCM using a factory sound. */
    for (uint32_t e = 0; e < s->engine_count(); ++e) {
        assert(s->preset_count(e) > 0 && s->engine_name(e) && s->engine_name(e)[0]);
        assert(s->set_preset(e, 0) == 0 && s->current_engine() == e && s->current_preset() == 0);
        assert(s->midi(0x95, 60, 110) == 0); /* OMNI after init */
        s->render(audio, 8192);
        uint64_t power = energy(audio, 16384);
        printf("engine %2u %-10s %-12s energy=%llu\n", e, s->engine_name(e), s->preset_name(e, 0),
               (unsigned long long)power);
        assert(power > 100000);
        s->panic();
        s->render(audio, 8192);
        assert(energy(audio, 16384) == 0);
    }

    /* The port's input channel: OMNI by default; 1..16 answers that channel only, live from the
     * catalogue's first entry too. */
    assert(s->init(44100) == 0 && s->set_preset(0, 0) == 0);
    assert(s->set_midi_channel(17) == -1 && s->midi_channel() == 0);
    assert(s->set_midi_channel(1) == 0 && s->midi_channel() == 1);
    assert(s->midi(0x95, 60, 110) == 0);
    s->render(audio, 8192);
    assert(energy(audio, 16384) == 0);
    assert(s->midi(0x90, 60, 110) == 0);
    s->render(audio, 8192);
    assert(energy(audio, 16384) > 100000);
    s->panic();
    {
        fm1_param_t ch;
        assert(!s->param_get(0, &ch) && !strcmp(ch.name, "MIDI CH") && ch.value == 1 && !strcmp(ch.text, "1"));
        assert(!s->param_set(0, 6) && s->midi_channel() == 6);
        assert(s->midi(0x95, 60, 110) == 0);
        s->render(audio, 8192);
        assert(energy(audio, 16384) > 100000);
        s->panic();
        assert(!s->param_set(0, 0) && s->midi_channel() == 0 &&
               !s->param_get(0, &ch) && !strcmp(ch.text, "OMNI"));
    }

    /* Same audio for arbitrary caller chunk sizes: the internal block cadence is preserved. */
    assert(s->init(44100) == 0);
    assert(s->set_preset(0, 0) == 0);
    assert(s->midi(0x90, 60, 100) == 0);
    s->render(reference, 8192);
    assert(s->init(44100) == 0);
    assert(s->set_preset(0, 0) == 0);
    assert(s->midi(0x90, 60, 100) == 0);
    for (size_t offset = 0; offset < 8192;) {
        size_t n = offset % 97 + 1;
        if (n > 8192 - offset) n = 8192 - offset;
        s->render(audio + offset * 2, n);
        offset += n;
    }
    assert(memcmp(audio, reference, sizeof audio) == 0);

    /* MIDI note-off and note-on velocity zero release actual voices. */
    assert(s->midi(0xB0, 72, 0) == 0);
    assert(s->midi(0x80, 60, 0) == 0);
    for (unsigned i = 0; i < 16; ++i) s->render(audio, 8192);
    assert(energy(audio, 16384) < 16384 * 16);
    assert(s->midi(0x90, 64, 100) == 0);
    s->render(audio, 8192);
    assert(energy(audio, 16384) > 100000);
    assert(s->midi(0x90, 64, 0) == 0);
    for (unsigned i = 0; i < 16; ++i) s->render(audio, 8192);
    assert(energy(audio, 16384) < 16384 * 16);

    /* Queue saturation must clear the current voice, even if note-off is lost. */
    assert(s->midi(0x90, 67, 127) == 0);
    s->render(audio, 100);
    for (unsigned i = 0; i < 64; ++i)
        assert(s->midi(0xF8, 0, 0) == 0);
    assert(s->midi(0x80, 67, 0) == -2);
    s->render(audio, 8192);
    assert(energy(audio, 16384) == 0);
    assert(s->midi(0x90, 67, 127) == 0);
    s->render(audio, 8192);
    assert(energy(audio, 16384) > 100000);
    assert(s->midi(0xB0, 120, 0) == 0);
    s->render(audio, 8192);
    assert(energy(audio, 16384) == 0);
    printf("%s: %u upstream engines, release, chunking, panic and recovery passed\n", s->name, (unsigned)s->engine_count());
    return 0;
}
