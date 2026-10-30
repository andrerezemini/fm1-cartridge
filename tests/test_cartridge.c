/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef FM1_TEST_SYNTH_HEADER
#define FM1_TEST_SYNTH_HEADER "clipper_synth.h"
#define FM1_TEST_SYNTH_API clipper_synth_api
#endif
#include FM1_TEST_SYNTH_HEADER
#include "cartridge_remote.h"
#include "cartridge_state.h"
#include "fm1/cartridge_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static cartridge_remote_t server;
static uint8_t q[CART_REQUEST_SIZE], p[CART_REPLY_SIZE], previous[CART_REPLY_SIZE];
static uint16_t seq;
static void command(unsigned op, int delta)
{
    cart_header(q, ++seq, op, server.revision);
    cart_put14(q + CART_ARG, delta + 8192); q[sizeof q - 1] = 0xf7;
    assert(cartridge_remote_handle(&server, q, sizeof q, p));
    assert(cart_valid(p, sizeof p) && p[CART_STATUS] == CART_OK);
}
static int command_status(unsigned op, int delta)
{
    cart_header(q, ++seq, op, server.revision);
    cart_put14(q + CART_ARG, delta + 8192); q[sizeof q - 1] = 0xf7;
    assert(cartridge_remote_handle(&server, q, sizeof q, p));
    assert(cart_valid(p, sizeof p));
    return p[CART_STATUS];
}
static void wire_roundtrip(const uint8_t *frame, size_t n)
{
    uint8_t usb[CART_USB_SIZE]; fm1_sysex_t rx = {0};
    size_t bytes = fm1_sysex_encode(frame, n, usb, sizeof usb);
    assert(bytes && bytes % 4 == 0);
    for (size_t i = 0; i < bytes; i += 4) {
        assert(fm1_sysex_packet(&rx, usb + i) == (i + 4 == bytes));
        if (i + 4 != bytes) { const uint8_t clock[4] = {15, 0xf8, 0, 0}; assert(!fm1_sysex_packet(&rx, clock)); }
    }
    assert(rx.length == n && !memcmp(rx.bytes, frame, n));
    assert(!fm1_sysex_encode(frame, n, usb, bytes - 1));
}
/* a platform with three app slots: this one, a bootable Melodee and an empty one */
static uint8_t fake_current = 0;
static uint8_t fake_count(void) { return 3; }
static uint8_t fake_current_slot(void) { return fake_current; }
static const char *fake_name(uint8_t s) { return s == 0 ? "CLIPPER" : s == 1 ? "MELODEE" : "EMPTY"; }
static int fake_bootable(uint8_t s) { return s < 2; }
static const fm1_slots_t fake_slots = { fake_count, fake_current_slot, fake_name, fake_bootable };
/* the same with the empty slot in the middle: a turn passes over it to the slot after it */
static const char *gap_name(uint8_t s) { return s == 0 ? "CLIPPER" : s == 1 ? "EMPTY" : "FELUCCA"; }
static int gap_bootable(uint8_t s) { return s != 1; }
static const fm1_slots_t gap_slots = { fake_count, fake_current_slot, gap_name, gap_bootable };
/* the catalogue entry with this name, or -1 */
static int entry_named(const char *name)
{
    fm1_param_t d;
    for (unsigned i = 0; !cartridge_remote_param_get(&server, i, &d); ++i)
        if (!strcmp(d.name, name)) return (int)i;
    return -1;
}
int main(void)
{
    const fm1_synth_api_t *synth = FM1_TEST_SYNTH_API();
    assert(synth->init(44100) == 0 && synth->name[0]);
    cartridge_remote_init(&server, synth, NULL, 1234);
    command(CART_QUERY, 0);
    wire_roundtrip(q, sizeof q); wire_roundtrip(p, sizeof p);
    assert(!strcmp((const char *)p + CART_FW_NAME, synth->name) && p[CART_SLOT] == 0 && p[CART_NSLOT] == 1);
    /* the reply carries the MIDI channel the synthesizer answers, and follows the MIDI CH entry (catalogue 0) */
    assert(p[CART_MIDI_CH] == synth->midi_channel());
    assert(!synth->set_midi_channel(1));
    command(CART_PARAM, -8192); assert(cart_u14(p + CART_PAR) == 0 && p[CART_MIDI_CH] == 1);
    command(CART_VALUE, 4); assert(synth->midi_channel() == 5 && p[CART_MIDI_CH] == 5 && cart_s14(p + CART_VAL) == 5);
    command(CART_VALUE, -8192); assert(synth->midi_channel() == 0 && p[CART_MIDI_CH] == 0);
    command(CART_ENGINE, -8192); assert(p[CART_ENG] == 0 && cart_u14(p + CART_PRE) == 0);
    /* Exact retries must neither load twice nor advance relative commands twice. */
    command(CART_PRESET, 1); memcpy(previous, p, sizeof p);
    uint16_t rev = server.revision;
    assert(cartridge_remote_handle(&server, q, sizeof q, p));
    assert(server.revision == rev && !memcmp(p, previous, sizeof p));
    cart_put14(q + CART_SEQ, ++seq); cart_put14(q + CART_REV, rev - 1);
    assert(cartridge_remote_handle(&server, q, sizeof q, p));
    assert(p[CART_STATUS] == CART_STALE && cart_u14(p + CART_PRE) == cart_u14(previous + CART_PRE));
    /* A wrong protocol, bad byte, wrong length or unsupported opcode cannot mutate. */
    q[4] = CART_VERSION - 1; assert(!cartridge_remote_handle(&server, q, sizeof q, p)); q[4] = CART_VERSION;
    q[10] = 128; assert(!cartridge_remote_handle(&server, q, sizeof q, p)); q[10] = 0;
    q[7] = 99; assert(!cartridge_remote_handle(&server, q, sizeof q, p)); q[7] = 0;
    assert(!cartridge_remote_handle(&server, q, sizeof q - 1, p));
    /* Without slots the catalogue is the synth's own: no FIRMWARE entry. */
    assert(cartridge_remote_param_count(&server) == synth->param_count());
    /* Every factory catalogue and every exposed range on every engine. */
    unsigned presets = 0, parameters = 0;
    for (unsigned e = 0; e < synth->engine_count(); ++e) {
        assert(synth->preset_count(e) < 16384);
        for (unsigned pr = 0; pr < synth->preset_count(e); ++pr) {
            assert(synth->set_preset(e, pr) == 0); presets++;
            assert(synth->preset_name(e, pr));
        }
        assert(synth->set_preset(e, 0) == 0);
        unsigned count = synth->param_count(); assert(count > 50 && count < 16384);
        for (unsigned i = 0; i < count; ++i) {
            fm1_param_t d, after;
            assert(!synth->param_get(i, &d)); assert(d.name[0] && d.text[0]);
            assert(d.min >= -8192 && d.max <= 8191 && d.min <= d.value && d.value <= d.max);
            assert(synth->param_set(i, d.min - 1) == -1);
            assert(synth->param_set(i, d.max + 1) == -1);
            assert(!synth->param_set(i, d.min));
            assert(!synth->param_get(i, &after) && after.value == d.min);
            assert(!synth->param_set(i, d.max));
            assert(!synth->param_get(i, &after) && after.value == d.max);
            assert(!synth->param_set(i, d.value)); parameters++;
        }
        fm1_param_t d;
        assert(synth->param_get(count, &d) == -1);
    }
    /* Editor writes affect the real PCM, not just a displayed shadow value. */
    synth->init(44100); synth->set_preset(0, 0);
    int16_t a[8192], b[8192]; unsigned level = 0;
    for (unsigned i = 0; i < synth->param_count(); ++i) {
        fm1_param_t d; synth->param_get(i, &d);
        if (!level && strstr(d.name, "LVL")) level = i;
    }
    assert(level);
    synth->midi(0x90, 60, 100); synth->render(a, 4096);
    synth->init(44100); synth->set_preset(0, 0);
    assert(!synth->param_set(level, 0));
    synth->midi(0x90, 60, 100); synth->render(b, 4096);
    uint64_t loud = 0, muted = 0;
    for (unsigned i = 0; i < 8192; ++i) { loud += (int64_t)a[i] * a[i]; muted += (int64_t)b[i] * b[i]; }
    assert(loud > 100000 && muted < loud / 1000);
    /* Framing failures: overflow, invalid CIN termination, other cable, abort. */
    fm1_sysex_t rx = {0};
    const uint8_t start[] = {4, 0xf0, 0x7d, 'C'}, more[] = {4, 1, 2, 3}, end[] = {5, 0xf7, 0, 0};
    assert(!fm1_sysex_packet(&rx, start));
    for (unsigned i = 0; i < 50; ++i) assert(!fm1_sysex_packet(&rx, more));
    assert(!fm1_sysex_packet(&rx, end));
    assert(!fm1_sysex_packet(&rx, start));
    const uint8_t note[] = {9, 0x90, 60, 100}; assert(!fm1_sysex_packet(&rx, note));
    assert(!fm1_sysex_packet(&rx, end));
    command(CART_QUERY, 0); wire_roundtrip(p, sizeof p);

    /* FIRMWARE: with several slots, catalogue entry 1 picks the slot to boot. MIDI CH stays entry 0,
     * the synth's own entries shift by one, an empty slot is stepped over, the current slot disarms. */
    cartridge_remote_init(&server, synth, &fake_slots, 99);
    unsigned own = synth->param_count();
    assert(cartridge_remote_param_count(&server) == own + 1u);
    fm1_param_t d;
    assert(!cartridge_remote_param_get(&server, 0, &d) && !strcmp(d.name, "MIDI CH"));
    assert(!cartridge_remote_param_get(&server, 1, &d) && !strcmp(d.name, "FIRMWARE") && d.min == 0 && d.max == 2 &&
           d.value == 0 && !strcmp(d.text, "CLIPPER") && !strcmp(d.unit, "CURRENT"));   /* the fake platform's names */
    fm1_param_t s1, r2;
    assert(!synth->param_get(1, &s1) && !cartridge_remote_param_get(&server, 2, &r2) && !strcmp(s1.name, r2.name));
    assert(cartridge_remote_param_get(&server, own + 1u, &d) == -1);
    command(CART_QUERY, 0);
    assert(p[CART_SLOT] == 0 && p[CART_NSLOT] == 3 && cart_u14(p + CART_NPAR) == own + 1u);
    command(CART_PARAM, 1);
    assert(cart_u14(p + CART_PAR) == 1 && !strcmp((const char *)p + CART_PAR_NAME, "FIRMWARE"));
    assert(!server.slot_dirty);
    command(CART_VALUE, 1);
    assert(server.slot_cursor == 1 && server.slot_dirty && !strcmp((const char *)p + CART_VAL_NAME, "MELODEE") &&
           !strcmp((const char *)p + CART_UNIT, "IN 2 S") && cart_s14(p + CART_VAL) == 1);
    server.slot_dirty = 0;
    /* slot 2 is empty: the knob cannot land there, nothing changes, the FM-1 learns it was refused */
    assert(command_status(CART_VALUE, 1) == CART_BAD_REQUEST);
    assert(server.slot_cursor == 1 && !server.slot_dirty);
    /* +5 from slot 1 saturates at slot 2, which is empty; stepping back reaches slot 1 again,
     * where the cursor already is: nothing changes and the request is refused */
    assert(command_status(CART_VALUE, 5) == CART_BAD_REQUEST && server.slot_cursor == 1);
    command(CART_VALUE, -1);
    assert(server.slot_cursor == 0 && server.slot_dirty && !strcmp((const char *)p + CART_UNIT, "CURRENT"));
    /* the first synth entry past FIRMWARE edits the synth, not the cursor */
    command(CART_PARAM, 1); fm1_param_t before;
    assert(!cartridge_remote_param_get(&server, 2, &before));
    if (before.value < before.max) { command(CART_VALUE, 1); assert(!synth->param_get(1, &d) && d.value == before.value + 1); }
    assert(cartridge_remote_param_set(&server, 1, 3) == -1 && cartridge_remote_param_set(&server, 1, -1) == -1);

    /* While the FM-1 plays (busy), FIRMWARE stays on the running slot and says why. */
    server.slot_dirty = 0;
    command(CART_PARAM, -1); assert(cart_u14(p + CART_PAR) == 1);
    server.busy = 1;
    command(CART_QUERY, 0);                       /* (a query moves off FIRMWARE: back to it) */
    assert(cart_u14(p + CART_PAR) == 2);
    command(CART_PARAM, -1);
    assert(!strcmp((const char *)p + CART_UNIT, "PLAYING"));
    assert(command_status(CART_VALUE, 1) == CART_BAD_REQUEST && server.slot_cursor == 0 && !server.slot_dirty);
    server.busy = 0;
    command(CART_VALUE, 1); assert(server.slot_cursor == 1 && server.slot_dirty);
    server.slot_dirty = 0;
    /* leaving FIRMWARE cancels the switch it armed */
    command(CART_PARAM, 1);
    assert(cart_u14(p + CART_PAR) == 2 && server.slot_cursor == 0 && server.slot_dirty);
    server.slot_dirty = 0;
    /* a query (the page opening) never rests on FIRMWARE, nor keeps a switch armed */
    command(CART_PARAM, -1); command(CART_VALUE, 1); assert(server.slot_cursor == 1);
    command(CART_QUERY, 0);
    assert(cart_u14(p + CART_PAR) == 2 && server.slot_cursor == 0 && !strcmp((const char *)p + CART_UNIT, ""));

    /* A sound change keeps the entry when its name stays (an engine's presets share a catalogue) and
     * otherwise goes to the synth's first entry: never MIDI CH or FIRMWARE, which knob 4 would act on. */
    {
        fm1_param_t was, now;
        unsigned k;
        command(CART_PARAM, 4); k = cart_u14(p + CART_PAR); assert(k == 6);
        assert(!cartridge_remote_param_get(&server, k, &was));
        server.changed = 0;
        command(CART_PRESET, 1);
        assert(server.changed);
        assert(!cartridge_remote_param_get(&server, k, &now));
        assert(cart_u14(p + CART_PAR) == (strcmp(was.name, now.name) ? 2u : k));
        command(CART_ENGINE, 1);
        assert(cart_u14(p + CART_PAR) >= 2u);
        command(CART_PARAM, -8192); assert(cart_u14(p + CART_PAR) == 0);
        command(CART_PRESET, 1);                  /* MIDI CH chosen on purpose: it stays */
        assert(cart_u14(p + CART_PAR) == 0);
        command(CART_PARAM, 1);
        command(CART_PRESET, 1);                  /* from FIRMWARE: to the synth's first entry */
        assert(cart_u14(p + CART_PAR) == 2);
    }

    /* MIDI CH turned while a note sounds: the cartridge lets go of it (its note-off comes on the old
     * channel, which the synth now ignores), and the new channel plays. */
    {
        static int16_t pcm[8192 * 2];
        uint64_t power;
        assert(!synth->set_midi_channel(1) && synth->set_preset(0, 0) == 0);
        command(CART_PARAM, -8192); assert(cart_u14(p + CART_PAR) == 0);
        assert(synth->midi(0x90, 60, 100) == 0);
        synth->render(pcm, 8192);
        power = 0; for (unsigned i = 0; i < 16384; ++i) power += (int64_t)pcm[i] * pcm[i];
        assert(power > 100000);
        server.changed = 0;
        command(CART_VALUE, 1); assert(synth->midi_channel() == 2 && p[CART_MIDI_CH] == 2 && server.changed);
        assert(synth->midi(0x80, 60, 0) == 0);    /* the old channel's note-off: filtered out */
        for (unsigned j = 0; j < 16; ++j) synth->render(pcm, 8192);
        power = 0; for (unsigned i = 0; i < 16384; ++i) power += (int64_t)pcm[i] * pcm[i];
        assert(power < 16384 * 16);
        assert(synth->midi(0x91, 64, 100) == 0);
        synth->render(pcm, 8192);
        power = 0; for (unsigned i = 0; i < 16384; ++i) power += (int64_t)pcm[i] * pcm[i];
        assert(power > 100000);
        synth->panic();
    }
    /* The platform's figures: four read-only entries at the end of the catalogue (render and pass against
     * the block, the gap against the ring, the underruns); a query starts them again before it answers. */
    {
        cartridge_metrics_t metrics = { 812, 1451, 3, 990, 4200, 11609 };
        unsigned last;
        server.metrics = &metrics;
        last = cartridge_remote_param_count(&server) - 1u;
        assert(last == own + 4u);
        command(CART_QUERY, 0); assert(!metrics.render_us && !metrics.underruns && !metrics.pass_us && !metrics.gap_us);
        metrics.render_us = 812; metrics.underruns = 3; metrics.pass_us = 990; metrics.gap_us = 4200;
        command(CART_PARAM, 8191);
        assert(cart_u14(p + CART_PAR) == last && !strcmp((const char *)p + CART_PAR_NAME, "AUDIO UNDERRUNS") &&
               !strcmp((const char *)p + CART_VAL_NAME, "3"));
        command(CART_PARAM, -1);
        assert(!strcmp((const char *)p + CART_PAR_NAME, "AUDIO GAP MAX") && !strcmp((const char *)p + CART_VAL_NAME, "4200 US") &&
               !strcmp((const char *)p + CART_UNIT, "/11609"));
        command(CART_PARAM, -1);
        assert(!strcmp((const char *)p + CART_PAR_NAME, "AUDIO PASS MAX") && !strcmp((const char *)p + CART_VAL_NAME, "990 US") &&
               !strcmp((const char *)p + CART_UNIT, "/1451"));
        command(CART_PARAM, 1);
        command(CART_QUERY, 0);                    /* the page opened again on the figures: from 0 */
        assert(cart_u14(p + CART_PAR) == last - 1u && !strcmp((const char *)p + CART_VAL_NAME, "0 US"));
        metrics.render_us = 812; metrics.underruns = 3;
        command(CART_PARAM, -2);
        assert(!strcmp((const char *)p + CART_PAR_NAME, "AUDIO RENDER MAX") && !strcmp((const char *)p + CART_VAL_NAME, "812 US") &&
               !strcmp((const char *)p + CART_UNIT, "/1451"));
        assert(command_status(CART_VALUE, 1) == CART_BAD_REQUEST && cartridge_remote_param_set(&server, last, 0) == -1);
        for (unsigned k = 0; k < 4u; ++k) assert(cartridge_remote_param_set(&server, last - k, 0) == -1);
        server.metrics = NULL;
        command(CART_PARAM, -8192);
    }
    /* A value it refuses is passed over in the knob's direction: an empty slot between two firmwares. */
    cartridge_remote_init(&server, synth, &gap_slots, 7);
    command(CART_QUERY, 0); command(CART_PARAM, -8192); command(CART_PARAM, 1);
    assert(cart_u14(p + CART_PAR) == 1);
    command(CART_VALUE, 1);
    assert(server.slot_cursor == 2 && !strcmp((const char *)p + CART_VAL_NAME, "FELUCCA"));
    command(CART_VALUE, -1);
    assert(server.slot_cursor == 0);
    cartridge_remote_init(&server, synth, &fake_slots, 99);
    /* Felucca's SAMPLE SET and SLICE SRC never land on the user slots, which the port cannot fill */
    if (!strcmp(synth->name, "FELUCCA")) {
        unsigned sample = 0, slice = 0;
        for (unsigned e = 0; e < synth->engine_count(); ++e) {
            if (!strcmp(synth->engine_name(e), "SAMPLE")) sample = e;
            if (!strcmp(synth->engine_name(e), "SLICE")) slice = e;
        }
        assert(sample && slice);
        assert(synth->set_preset(sample, 0) == 0);
        command(CART_QUERY, 0);
        int set = entry_named("ENG SET");
        assert(set > 0);
        command(CART_PARAM, -8192); command(CART_PARAM, set);
        for (unsigned k = 0; k < 20; ++k) {
            command_status(CART_VALUE, 1);
            assert(strncmp((const char *)p + CART_VAL_NAME, "USR", 3));
        }
        assert(synth->set_preset(slice, 0) == 0);
        command(CART_QUERY, 0);
        int src = entry_named("ENG SRC");
        assert(src > 0);
        command(CART_PARAM, -8192); command(CART_PARAM, src);
        command_status(CART_VALUE, -8192);
        assert(!strcmp((const char *)p + CART_VAL_NAME, "BREAK"));
        command(CART_VALUE, 1);
        assert(!strcmp((const char *)p + CART_VAL_NAME, "PIANO"));
        command(CART_VALUE, -1);
        assert(!strcmp((const char *)p + CART_VAL_NAME, "BREAK"));
        printf("felucca: SAMPLE SET and SLICE SRC pass over the empty user slots\n");
    }
    /* Melodee: a track keeps only the note inserts, the shared rack every audio effect. The knob passes over the
     * unnamed entries of their lists (a retired effect, the other rack's types), DRIVE's shapes are named, the
     * rack's one REVERB moves to the slot that picks it, and MIDI Track (no audio) is not offered. */
    if (!strcmp(synth->name, "MELODEE")) {
        static const char *const rack[] = {"OFF", "PHASER", "WIDTH", "FORMANT", "TAPE", "SPIT", "ROOM", "REVERB",
                                           "CHORUS", "DRIVE SOFT", "DRIVE HARD", "DRIVE FOLD", "DRIVE FUZZ", "CRUSH",
                                           "FLANGER", "PEDAL PHASER", "PEDAL CHORUS"};
        static int16_t pcm[2 * 11025];
        unsigned drum = 0;
        for (unsigned e = 0; e < synth->engine_count(); ++e) {
            assert(strcmp(synth->engine_name(e), "MIDI Track"));
            if (!strcmp(synth->engine_name(e), "DRUM")) drum = e;
        }
        assert(drum && synth->set_preset(drum, 0) == 0 && !synth->set_midi_channel(0));
        command(CART_QUERY, 0);
        int kit = entry_named("ENG KIT");                  /* (its names start at value 4) */
        fm1_param_t d;
        assert(kit > 0 && !cartridge_remote_param_set(&server, kit, 5) && !cartridge_remote_param_get(&server, kit, &d) &&
               !strcmp(d.text, "909"));
        double tail[3];                                    /* dry, shared REVERB on the send, NOTE ECHO */
        for (unsigned run = 0; run < 3; ++run) {
            assert(synth->init(44100) == 0 && synth->set_preset(0, 0) == 0);
            command(CART_QUERY, 0);
            int fx1 = entry_named("FX1 TYPE"), fx2 = entry_named("FX2 TYPE"), send = entry_named("SEND SEND");
            int note = entry_named("NOTE1 NOTE FX");
            assert(fx1 > 0 && fx2 > 0 && send > 0 && note > 0);
            if (run == 0) {
                command(CART_PARAM, -8192); command(CART_PARAM, fx1);
                for (unsigned k = 1; k < sizeof rack / sizeof rack[0]; ++k) {
                    command(CART_VALUE, 1);
                    assert(!strcmp((const char *)p + CART_VAL_NAME, rack[k]));
                }
                assert(command_status(CART_VALUE, 1) == CART_BAD_REQUEST);   /* the end of the list */
                assert(cartridge_remote_param_set(&server, fx1, 6) == -1 && cartridge_remote_param_set(&server, fx1, 10) == -1);
                assert(!cartridge_remote_param_set(&server, fx1, 8) && !cartridge_remote_param_set(&server, fx2, 8));
                assert(!cartridge_remote_param_get(&server, fx1, &d) && !strcmp(d.text, "OFF"));
                assert(!cartridge_remote_param_get(&server, fx2, &d) && !strcmp(d.text, "REVERB"));
                command(CART_PARAM, note - fx1);
                assert(!cartridge_remote_param_get(&server, note, &d) && d.min == 0 && !strcmp(d.text, "OFF"));
                command(CART_VALUE, 1); assert(!strcmp((const char *)p + CART_VAL_NAME, "NOTE TAIL"));
                command(CART_VALUE, 1); assert(!strcmp((const char *)p + CART_VAL_NAME, "NOTE ECHO"));
                assert(command_status(CART_VALUE, 1) == CART_BAD_REQUEST && cartridge_remote_param_get(&server, note, &d) == 0 &&
                       d.max == d.value);
                assert(entry_named("NOTE1 LEVEL") == note + 1 && entry_named("NOTE1 TIME") == note + 2);   /* its controls */
                assert(synth->init(44100) == 0 && synth->set_preset(0, 0) == 0);
            } else if (run == 1) {
                assert(!cartridge_remote_param_set(&server, fx1, 8) && !cartridge_remote_param_set(&server, send, 127));
            } else {
                assert(!cartridge_remote_param_set(&server, note, 11));   /* NOTE ECHO, from its starting controls */
            }
            synth->midi(0x90, 60, 110); synth->render(pcm, 4410); synth->midi(0x80, 60, 0);
            synth->render(pcm, 11025);                     /* the release */
            synth->render(pcm, 11025);
            tail[run] = 0;
            for (unsigned i = 0; i < 2u * 11025u; ++i) tail[run] += (double)pcm[i] * pcm[i];
        }
        assert(tail[1] > 100.0 * (tail[0] + 1.0) && tail[2] > 100.0 * (tail[0] + 1.0));
        /* every effect of the rack, on the part's send, changes what each engine plays */
        for (unsigned e = 0; e < synth->engine_count(); ++e) {
            uint32_t dry = 0, changed = 0;
            for (int type = 0; type < 20; ++type) {
                assert(synth->init(44100) == 0 && synth->set_preset(e, 0) == 0);
                command(CART_QUERY, 0);
                int fx1 = entry_named("FX1 TYPE"), send = entry_named("SEND SEND");
                if (cartridge_remote_param_set(&server, fx1, type)) continue;
                assert(!cartridge_remote_param_set(&server, send, 127));
                uint32_t h = 2166136261u;
                synth->midi(0x90, 48, 110); synth->midi(0x90, 55, 100); synth->midi(0x90, 64, 90);
                for (unsigned b = 0; b < 2; ++b) {             /* held, then released */
                    if (b) { synth->midi(0x80, 48, 0); synth->midi(0x80, 55, 0); synth->midi(0x80, 64, 0); }
                    synth->render(pcm, b ? 11025 : 4410);
                    for (unsigned i = 0; i < (b ? 2u * 11025u : 2u * 4410u); ++i) h = (h ^ (uint16_t)pcm[i]) * 16777619u;
                }
                if (!type) dry = h;
                else { assert(h != dry); changed++; }
            }
            assert(changed == sizeof rack / sizeof rack[0] - 1u);
        }
        printf("melodee: the rack and note insert lists pass over their unnamed entries, one REVERB, "
               "every rack effect changes the part, the rack's and NOTE ECHO's tails sound\n");
        cartridge_remote_init(&server, synth, &fake_slots, 99);
    }

    /* Program Change and Bank Select on the synth's channel pick the sound as the page does: CC 0 the engine,
     * CC 32 the page of 128 presets; a Program Change alone stays in the current engine and its page. Each
     * one is a new revision, so a page request made before it is stale. */
    {
        unsigned e = synth->engine_count() - 1u, n = synth->preset_count(e), last = (n < 128u ? n : 128u) - 1u;
        assert(synth->init(44100) == 0 && !synth->set_midi_channel(2) && synth->set_preset(0, 0) == 0);
        command(CART_QUERY, 0);
        uint16_t rev = server.revision;
        server.changed = 0;
        assert(cartridge_remote_midi(&server, 0xC1, 1, 0) == 1);
        assert(synth->current_engine() == 0 && synth->current_preset() == 1 && server.changed && !server.cached &&
               server.revision == ((rev + 1u) & 16383u));
        cart_header(q, ++seq, CART_PRESET, rev); cart_put14(q + CART_ARG, 1 + 8192); q[sizeof q - 1] = 0xf7;
        assert(cartridge_remote_handle(&server, q, sizeof q, p) && p[CART_STATUS] == CART_STALE &&
               cart_u14(p + CART_PRE) == 1 && synth->current_preset() == 1);
        /* another channel, and every other message, go to the synth */
        assert(!cartridge_remote_midi(&server, 0xC0, 2, 0) && !cartridge_remote_midi(&server, 0xB0, 0, 1) &&
               synth->current_preset() == 1);
        assert(!cartridge_remote_midi(&server, 0xB1, 7, 100) && !cartridge_remote_midi(&server, 0x91, 60, 100) &&
               !cartridge_remote_midi(&server, 0xB1, 123, 0));
        synth->panic();
        /* CC 0 only waits for the Program Change */
        assert(cartridge_remote_midi(&server, 0xB1, 0, (uint8_t)e) == 1 && synth->current_engine() == 0);
        assert(cartridge_remote_midi(&server, 0xC1, (uint8_t)last, 0) == 1);
        assert(synth->current_engine() == e && synth->current_preset() == last);
        command(CART_QUERY, 0); assert(p[CART_ENG] == e && cart_u14(p + CART_PRE) == last);
        assert(cartridge_remote_midi(&server, 0xC1, 0, 0) == 1 && synth->current_engine() == e && synth->current_preset() == 0);
        /* an engine or a program that does not exist changes nothing, and the bank is spent */
        rev = server.revision;
        assert(cartridge_remote_midi(&server, 0xB1, 0, 127) == 1 && cartridge_remote_midi(&server, 0xC1, 0, 0) == 1);
        assert(synth->current_engine() == e && synth->current_preset() == 0 && server.revision == rev);
        if (n < 128u) {
            assert(cartridge_remote_midi(&server, 0xC1, (uint8_t)n, 0) == 1);
            assert(synth->current_preset() == 0 && server.revision == rev);
        }
        /* past 128 presets (Melodee's Prophet): CC 32 picks the page, a Program Change alone keeps it */
        for (unsigned k = 0; k < synth->engine_count(); ++k) {
            if (synth->preset_count(k) <= 129u) continue;
            assert(cartridge_remote_midi(&server, 0xB1, 0, (uint8_t)k) && cartridge_remote_midi(&server, 0xB1, 32, 1) &&
                   cartridge_remote_midi(&server, 0xC1, 0, 0));
            assert(synth->current_engine() == k && synth->current_preset() == 128);
            assert(cartridge_remote_midi(&server, 0xC1, 1, 0) && synth->current_preset() == 129);
            assert(cartridge_remote_midi(&server, 0xB1, 0, (uint8_t)k) && cartridge_remote_midi(&server, 0xC1, 2, 0) &&
                   synth->current_preset() == 2);
            printf("%s: CC 32 reaches preset 129 of %s\n", synth->name, synth->engine_name(k));
            break;
        }
        /* OMNI: every channel */
        assert(!synth->set_midi_channel(0));
        assert(cartridge_remote_midi(&server, 0xC9, 1, 0) == 1 && synth->current_preset() == 1);
        synth->panic();
    }
    /* Protocol 4 carries the cell as the platform sets it; out of range reads as no reading. */
    command(CART_QUERY, 0);
    assert(p[CART_BATTERY] == CART_BATTERY_NONE && p[CART_POWER] == CART_POWER_NONE && p[CART_REPLY_SIZE - 1] == 0xf7);
    server.battery = 64; server.power = CART_POWER_LOW;
    command(CART_QUERY, 0); assert(p[CART_BATTERY] == 64 && p[CART_POWER] == CART_POWER_LOW);
    server.battery = 101; server.power = 9;
    command(CART_QUERY, 0); assert(p[CART_BATTERY] == CART_BATTERY_NONE && p[CART_POWER] == CART_POWER_NONE);
    wire_roundtrip(p, sizeof p);

    /* The state record: a sound with edits and a MIDI channel comes back after a fresh start, and a
     * record from elsewhere (another synth or revision, short, malformed) changes nothing. */
    {
        static cartridge_state_t a, b, c;
        size_t na, nb, nc;
        unsigned e = synth->engine_count() > 1 ? 1u : 0u, pr = synth->preset_count(e) > 2 ? 2u : 0u;
        assert(synth->init(44100) == 0 && synth->set_preset(e, pr) == 0 && !synth->set_midi_channel(3));
        for (unsigned i = 1, edited = 0; i < synth->param_count() && edited < 6; ++i) {
            fm1_param_t d;
            assert(!synth->param_get(i, &d));
            if (d.value < d.max && !strstr(d.name, "TYPE")) { assert(!synth->param_set(i, d.value + 1)); edited++; }
        }
        na = cartridge_state_capture(synth, &a);
        assert(na > 16 && a.engine == e && a.preset == pr && a.midi_channel == 3);
        assert(synth->init(44100) == 0 && !synth->set_midi_channel(1));
        nb = cartridge_state_capture(synth, &b);
        assert(!cartridge_state_equal(&a, na, &b, nb));
        assert(cartridge_state_apply(synth, &a, na) == 0);
        nc = cartridge_state_capture(synth, &c);
        assert(cartridge_state_equal(&a, na, &c, nc) && synth->midi_channel() == 3);
        assert(cartridge_state_apply(synth, &a, na - 1) == -1);
        b = a; b.revision ^= 1u; assert(cartridge_state_apply(synth, &b, na) == -1);
        b = a; b.magic = 0; assert(cartridge_state_apply(synth, &b, na) == -1);
        b = a; b.engine = (uint16_t)synth->engine_count(); assert(cartridge_state_apply(synth, &b, na) == -1);
        b = a; b.midi_channel = 17; assert(cartridge_state_apply(synth, &b, na) == -1);
        assert(synth->init(44100) == 0);
        nb = cartridge_state_capture(synth, &b);
        assert(cartridge_state_apply(synth, &a, 3) == -1 && cartridge_state_capture(synth, &c) == nb &&
               cartridge_state_equal(&b, nb, &c, nb));
    }
    printf("cartridge server: %u presets, %u parameter ranges, idempotence, stale guard, SysEx, PCM, FIRMWARE slots, the busy guard, the kept entry, the MIDI CH release, Program Change and Bank Select, the battery fields and the state record passed\n",
           presets, parameters);
    return 0;
}
