/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef FM1_TEST_SYNTH_HEADER
#define FM1_TEST_SYNTH_HEADER "clipper_synth.h"
#define FM1_TEST_SYNTH_API clipper_synth_api
#endif
#include FM1_TEST_SYNTH_HEADER
#include "cartridge_remote.h"
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
    q[4] = 2; assert(!cartridge_remote_handle(&server, q, sizeof q, p)); q[4] = 3;
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
    printf("cartridge server: %u presets, %u parameter ranges, idempotence, stale guard, SysEx, PCM and FIRMWARE slots passed\n",
           presets, parameters);
    return 0;
}
